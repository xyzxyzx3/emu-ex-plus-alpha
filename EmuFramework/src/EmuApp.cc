/*  This file is part of EmuFramework.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with EmuFramework.  If not, see <http://www.gnu.org/licenses/> */

#include <emuframework/EmuApp.hh>
#include <emuframework/MainMenuView.hh>
#include <emuframework/LoadProgressView.hh>
#include <emuframework/SystemActionsView.hh>
#include <emuframework/SystemOptionView.hh>
#include <emuframework/VideoOptionView.hh>
#include <emuframework/AudioOptionView.hh>
#include <emuframework/GUIOptionView.hh>
#include <emuframework/FilePathOptionView.hh>
#include <emuframework/FilePicker.hh>
#include <emuframework/Option.hh>
#include "gui/AutosaveSlotView.hh"
#include "WindowData.hh"
#include "InputDeviceData.hh"
import imagine;
import configFile;
import pathUtils;

namespace EmuEx
{

using namespace IG;

constexpr SystemLogger log{"App"};
static EmuApp *gAppPtr{};
constexpr float pausedVideoBrightnessScale = .75f;

EmuApp::EmuApp(ApplicationInitParams initParams, ApplicationContext &ctx):
	Application{initParams},
	fontManager{ctx},
	renderer{ctx},
	audio{ctx},
	videoLayer{video, defaultVideoAspectRatio()},
	inputManager{ctx},
	assetManager{ctx},
	vibrationManager{ctx},
	gameManager{ctx.gameManager()},
	bluetoothAdapter{ctx},
	pixmapWriter{ctx},
	perfHintManager{ctx.performanceHintManager()},
	layoutBehindSystemUI{ctx.hasTranslucentSysUI()}
{
	if(ctx.registerInstance(initParams))
	{
		ctx.exit();
		return;
	}
	if(AppMeta::needsGlobalInstance)
		gAppPtr = this;
	log.info("SET PTR:{}", (void*)gAppPtr);
	ctx.setAcceptIPC(true);
	onEvent = [this](ApplicationContext, const ApplicationEvent& appEvent)
	{
		appEvent.visit(overloaded
		{
			[&](const DocumentPickerEvent& e)
			{
				log.info("document picked with URI:{}", e.uri);
				system().setInitialLoadPath(e.uri);
			},
			[](auto &) {}
		});
	};
	initOptions(ctx);
}

class ExitConfirmAlertView : public AlertView, public EmuAppHelper
{
public:
	ExitConfirmAlertView(ViewAttachParams attach, bool hasEmuContent):
		AlertView
		{
			attach,
			"Really Exit? (Push Back/Escape again to confirm)",
			hasEmuContent ? 3u : 2u
		}
	{
		item.emplace_back("Yes", attach, [this](){ appContext().exit(); });
		item.emplace_back("No", attach, [](){});
		if(hasEmuContent)
		{
			item.emplace_back("Close Menu", attach, [this](){ app().showEmulation(); });
		}
	}

	bool inputEvent(const Input::Event &e, ViewInputEventParams) final
	{
		if(e.keyEvent() && e.keyEvent()->pushed(Input::DefaultKey::CANCEL))
		{
			if(!e.keyEvent()->repeated())
			{
				appContext().exit();
			}
			return true;
		}
		return AlertView::inputEvent(e);
	}
};

EmuViewController &EmuApp::viewController() { return mainWindowData().viewController; }
const EmuViewController &EmuApp::viewController() const { return mainWindowData().viewController; }
ToastView &EmuApp::toastView() { return viewController().popup; }
const Screen &EmuApp::emuScreen() const { return *viewController().emuWindowScreen(); }
Window &EmuApp::emuWindow() { return viewController().emuWindow(); }
const Window &EmuApp::emuWindow() const { return viewController().emuWindow(); }

void EmuApp::setCPUNeedsLowLatency(ApplicationContext ctx, bool needed)
{
	#ifdef __ANDROID__
	if(useNoopThread)
		ctx.setNoopThreadActive(needed);
	#endif
	if(useSustainedPerformanceMode)
		ctx.setSustainedPerformanceMode(needed);
	applyCPUAffinity(needed);
}

static void suspendEmulation(EmuApp &app)
{
	if(!app.system().hasContent())
		return;
	app.autosaveManager.save();
	app.system().flushBackupMemory(app);
}

void EmuApp::closeSystem()
{
	systemTask.stop();
	showUI();
	system().closeRuntimeSystem(*this);
	autosaveManager.resetSlot();
	rewindManager.clear();
	viewController().onSystemClosed();
}

void EmuApp::closeSystemWithoutSave()
{
	autosaveManager.resetSlot(noAutosaveName);
	closeSystem();
}

void EmuApp::applyOSNavStyle(ApplicationContext ctx, bool inEmu)
{
	SystemUIStyleFlags flags;
	if(lowProfileOSNav > (inEmu ? InEmuTristate::Off : InEmuTristate::InEmu))
		flags.dimNavigation = true;
	if(hidesOSNav > (inEmu ? InEmuTristate::Off : InEmuTristate::InEmu))
		flags.hideNavigation = true;
	if(hidesStatusBar > (inEmu ? InEmuTristate::Off : InEmuTristate::InEmu))
		flags.hideStatus = true;
	ctx.setSysUIStyle(flags);
}

void EmuApp::showSystemActionsViewFromSystem(ViewAttachParams attach, const Input::Event &e)
{
	viewController().showSystemActionsView(attach, e);
}

void EmuApp::showLastViewFromSystem(ViewAttachParams attach, const Input::Event &e)
{
	if(systemActionsIsDefaultMenu)
	{
		showSystemActionsViewFromSystem(attach, e);
	}
	else
	{
		showUI();
	}
}

void EmuApp::showExitAlert(ViewAttachParams, const Input::Event &)
{
    showEmulation();
}

static const char *parseCommandArgs(CommandArgs arg)
{
	if(arg.c < 2)
	{
		return nullptr;
	}
	auto launchPath = arg.v[1];
	log.info("starting content from command line:{}", launchPath);
	return launchPath;
}

bool EmuApp::setWindowDrawableConfig(Gfx::DrawableConfig conf)
{
	windowDrawableConfig = conf;
	auto ctx = appContext();
	for(auto &w : ctx.windows())
	{
		if(!renderer.setDrawableConfig(*w, conf))
			return false;
	}
	applyRenderPixelFormat();
	return true;
}

PixelFormat EmuApp::windowPixelFormat() const
{
	auto fmt = windowDrawableConfig.pixelFormat.value();
	if(fmt)
		return fmt;
	return appContext().defaultWindowPixelFormat();
}

void EmuApp::setRenderPixelFormat(PixelFormat fmt)
{
	renderPixelFormat = fmt;
	applyRenderPixelFormat();
}

void EmuApp::applyRenderPixelFormat()
{
	if(!video.hasRendererTask())
		return;
	auto fmt = renderPixelFormat.value();
	if(!fmt)
		fmt = windowPixelFormat();
	if(!AppMeta::canRenderRGBA8888 && fmt != PixelFmtRGB565)
	{
		log.info("Using RGB565 render format since emulated system can't render RGBA8888");
		fmt = PixelFmtRGB565;
	}
	videoLayer.setFormat(system(), fmt, videoEffectPixelFormat(), windowDrawableConfig.colorSpace);
}

void EmuApp::renderSystemFramebuffer(EmuVideo &video)
{
	if(!system().hasContent())
	{
		video.clear();
		return;
	}
	log.info("updating video with current framebuffer content");
	system().renderFramebuffer(video);
}

void EmuApp::startAudio()
{
	audio.start(system().frameRate().duration());
}

void EmuApp::updateLegacySavePath(ApplicationContext ctx, CStringView path)
{
	auto oldSaveSubDirs = subDirectoryStrings(ctx, path);
	if(oldSaveSubDirs.empty())
	{
		log.info("no legacy save folders in:{}", path);
		return;
	}
	flattenSubDirectories(ctx, oldSaveSubDirs, path);
}

static bool hasExtraWindow(ApplicationContext ctx)
{
	return ctx.windows().size() == 2;
}

static void dismissExtraWindow(ApplicationContext ctx)
{
	if(!hasExtraWindow(ctx))
		return;
	ctx.windows()[1]->dismiss();
}

static bool extraWindowIsFocused(ApplicationContext ctx)
{
	if(!hasExtraWindow(ctx))
		return false;
	return windowData(*ctx.windows()[1]).focused;
}

static Screen *extraWindowScreen(ApplicationContext ctx)
{
	if(!hasExtraWindow(ctx))
		return nullptr;
	return ctx.windows()[1]->screen();
}

void EmuApp::mainInitCommon(ApplicationInitParams initParams, ApplicationContext ctx)
{
	loadConfigFile(ctx);
	system().onOptionsLoaded();
	loadSystemOptions();
	updateLegacySavePathOnStoragePath(ctx, system());
	system().setInitialLoadPath(parseCommandArgs(initParams.commandArgs()));
	audio.manager.setMusicVolumeControlHint();
	if(!renderer.supportsColorSpace())
		windowDrawableConfig.colorSpace = {};
	applyOSNavStyle(ctx, false);

	ctx.addOnResume(
		[this](ApplicationContext, [[maybe_unused]] bool focused)
		{
			audio.manager.startSession();
			audio.open();
			return true;
		});

	ctx.addOnExit(
		[this](ApplicationContext ctx, bool backgrounded)
		{
			if(backgrounded)
			{
				suspendEmulation(*this);
				if(showsNotificationIcon)
				{
					auto title = std::format("{} was suspended", ApplicationMeta::name);
					ctx.addNotification(title, title, system().contentDisplayName());
				}
			}
			audio.close();
			audio.manager.endSession();
			saveConfigFile(ctx);
			saveSystemOptions();
			if(!backgrounded || (backgrounded && !keepBluetoothActive))
				closeBluetoothConnections();
			onEvent(ctx, FreeCachesEvent{false});
			return true;
		});

	WindowConfig winConf{ .title = ApplicationMeta::name };
	winConf.setFormat(windowDrawableConfig.pixelFormat);
	ctx.makeWindow(winConf,
		[this](ApplicationContext ctx, Window &win)
		{
			renderer.initMainTask(&win, windowDrawableConfig);
			textureBufferMode = renderer.validateTextureBufferMode(textureBufferMode);
			viewManager.defaultFace = {renderer, fontManager.makeSystem(), fontSettings(win)};
			viewManager.defaultBoldFace = {renderer, fontManager.makeBoldSystem(), fontSettings(win)};
			ViewAttachParams viewAttach{viewManager, win, renderer.task()};
			auto &vController = inputManager.vController;
			auto &winData = win.makeAppData<MainWindowData>(viewAttach, vController, videoLayer, system());
			winData.updateWindowViewport(win, makeViewport(win), renderer);
			win.setAcceptDnd(true);
			renderer.setWindowValidOrientations(win, menuOrientation);
			inputManager.updateInputDevices(ctx);
			vController.configure(win, renderer, viewManager.defaultFace);
			if(AppMeta::inputHasKeyboard)
			{
				vController.setKeyboardImage(asset(AssetID::keyboardOverlay));
			}
			winData.viewController.placeElements();
			winData.viewController.pushAndShow(makeView(viewAttach, ViewID::MAIN_MENU));
			configureSecondaryScreens();
			video.setRendererTask(renderer.task());
			video.setTextureBufferMode(system(), textureBufferMode);
			videoLayer.setRendererTask(renderer.task());
			applyRenderPixelFormat();
			videoLayer.updateEffect(system(), videoEffectPixelFormat());
			videoLayer.updateOverlay();
			if((frameClockSource == FrameClockSource::Screen   && !emuWindow().supportsFrameClockSource(FrameClockSource::Screen)) ||
				 (frameClockSource == FrameClockSource::Renderer && !emuWindow().supportsFrameClockSource(FrameClockSource::Renderer)))
			{
				frameClockSource = {};
			}
			if(emuScreen().supportedFrameRates().size() == 1)
			{
				overrideScreenFrameRate = {};
			}

			win.onEvent = [this](Window& win, const WindowEvent& winEvent)
			{
				return winEvent.visit(overloaded
				{
					[&](const Input::Event& e) { return viewController().inputEvent(e); },
					[&](const DrawEvent& e)
					{
						return viewController().drawMainWindow(win, e.params, renderer.task());
					},
					[&](const WindowSurfaceChangeEvent& e)
					{
						if(e.change.resized())
						{
							viewController().updateMainWindowViewport(win, makeViewport(win), renderer.task());
						}
						renderer.task().updateDrawableForSurfaceChange(win, e.change);
						return true;
					},
					[&](const DragDropEvent& e)
					{
						log.info("got DnD:{}", e.filename);
						handleOpenFileCommand(e.filename);
						return true;
					},
					[&](const FocusChangeEvent& e)
					{
						windowData(win).focused = e.in;
						onFocusChange(e.in);
						return true;
					},
					[](auto&){ return false; }
				});
			};

			onMainWindowCreated(viewAttach, ctx.defaultInputEvent());

			onEvent = [this](ApplicationContext ctx, const ApplicationEvent& appEvent)
			{
				appEvent.visit(overloaded
				{
					[&](const DocumentPickerEvent& e)
					{
						log.info("document picked with URI:{}", e.uri);
						if(!viewController().isShowingEmulation() && viewController().top().onDocumentPicked(e))
							return;
						handleOpenFileCommand(e.uri);
					},
					[&](const ScreenChangeEvent &e)
					{
						if(e.change == ScreenChange::added)
						{
							log.info("screen added");
							if(showOnSecondScreen && ctx.screens().size() > 1)
								setEmuViewOnExtraWindow(true, e.screen);
						}
						else if(e.change == ScreenChange::removed)
						{
							log.info("screen removed");
							if(hasExtraWindow(appContext()) && *extraWindowScreen(appContext()) == e.screen)
								setEmuViewOnExtraWindow(false, e.screen);
						}
						else if(e.change == ScreenChange::frameRate && e.screen == emuScreen())
						{
							if(viewController().isShowingEmulation())
							{
								if(perfHintSession)
								{
									auto targetDuration = e.screen.targetFrameDuration();
									perfHintSession.updateTargetWorkDuration(targetDuration);
									log.info("updated performance hint session with target duration:{}", targetDuration);
								}
								auto _ = suspendEmulationThread();
								systemTask.updateScreenFrameRate(e.screen.frameRate());
							}
						}
					},
					[&](const Input::DevicesEnumeratedEvent &)
					{
						log.info("input devs enumerated");
						inputManager.updateInputDevices(ctx);
					},
					[&](const Input::DeviceChangeEvent &e)
					{
						log.info("got input dev change");
						inputManager.updateInputDevices(ctx);
						if(notifyOnInputDeviceChange && (e.change == Input::DeviceChange::added || e.change == Input::DeviceChange::removed))
						{
							postMessage(2, 0, std::format("{} {}", inputDevData(e.device).displayName, e.change == Input::DeviceChange::added ? "connected" : "disconnected"));
						}
						else if(e.change == Input::DeviceChange::connectError)
						{
							postMessage(2, 1, std::format("{} had a connection error", e.device.name()));
						}
						viewController().onInputDevicesChanged();
					},
					[&](const FreeCachesEvent &e)
					{
						viewManager.defaultFace.freeCaches();
						viewManager.defaultBoldFace.freeCaches();
						if(e.running)
							viewController().prepareDraw();
					},
					[](auto &) {}
				});
			};

			ctx.addOnExit(
				[this](ApplicationContext ctx, bool backgrounded)
				{
					if(backgrounded)
					{
						showUI();
						if(showOnSecondScreen && ctx.screens().size() > 1)
						{
							setEmuViewOnExtraWindow(false, *ctx.screens()[1]);
						}
						viewController().onHide();
						ctx.addOnResume(
							[this](ApplicationContext, bool focused)
							{
								configureSecondaryScreens();
								viewController().prepareDraw();
								if(viewController().isShowingEmulation() && focused && system().isPaused())
								{
									log.info("resuming emulation due to app resume");
									viewController().inputView.resetInput();
									startEmulation();
								}
								return false;
							}, 10);
					}
					else
					{
						closeSystem();
					}
					return true;
				}, -10);

			if(auto launchPathStr = system().contentLocation();
				launchPathStr.size())
			{
				system().setInitialLoadPath("");
				handleOpenFileCommand(launchPathStr);
			}

			win.show();
		});
}

Viewport EmuApp::makeViewport(const Window &win) const
{
	return win.viewport(layoutBehindSystemUI ? win.bounds() : win.contentBounds());
}

void WindowData::updateWindowViewport(const Window &win, Viewport viewport, const Gfx::Renderer &r)
{
	windowRect = viewport.bounds();
	contentRect = viewport.bounds().intersection(win.contentBounds());
	projM = Gfx::Mat4::makePerspectiveFovRH(std::numbers::pi / 4.0, viewport.realAspectRatio(), .1f, 100.f)
		.projectionPlane(viewport, .5f, r.projectionRollAngle(win));
}

void EmuApp::launchSystem(const Input::Event &e)
{
	if(autosaveManager.autosaveLaunchMode == AutosaveLaunchMode::Ask)
	{
		autosaveManager.resetSlot(noAutosaveName);
		viewController().pushAndShow(EmuApp::makeView(attachParams(), EmuApp::ViewID::SYSTEM_ACTIONS), e);
		viewController().pushAndShow(std::make_unique<AutosaveSlotView>(attachParams()), e);
	}
	else
	{
		auto loadMode = autosaveManager.autosaveLaunchMode == AutosaveLaunchMode::LoadNoState ? LoadAutosaveMode::NoState : LoadAutosaveMode::Normal;
		if(autosaveManager.autosaveLaunchMode == AutosaveLaunchMode::NoSave)
			autosaveManager.resetSlot(noAutosaveName);
		static auto finishLaunch = [](EmuApp &app, LoadAutosaveMode mode)
		{
			app.autosaveManager.load(mode);
			if(!app.system().hasContent())
			{
				log.error("system was closed while trying to load autosave");
				return;
			}
			app.showEmulation();
		};
		auto stateIsOlderThanBackupMemory = [&]
		{
			auto stateTime = autosaveManager.stateTime();
			return hasTime(stateTime) && (autosaveManager.backupMemoryTime() - stateTime) > Seconds{1};
		};
		if(system().usesBackupMemory() && loadMode == LoadAutosaveMode::Normal &&
			!autosaveManager.saveOnlyBackupMemory && stateIsOlderThanBackupMemory())
		{
			viewController().pushAndShowModal(std::make_unique<YesNoAlertView>(attachParams(),
				"Autosave state timestamp is older than the contents of backup memory, really load it even though progress may be lost?",
				YesNoAlertView::Delegates
				{
					.onYes = [this]{ finishLaunch(*this, LoadAutosaveMode::Normal); },
					.onNo = [this]{ finishLaunch(*this, LoadAutosaveMode::NoState); }
				}), e, false);
		}
		else
		{
			finishLaunch(*this, loadMode);
		}
	}
}

void EmuApp::onSelectFileFromPicker(IO io, CStringView path, std::string_view displayName,
	const Input::Event &e, EmuSystemCreateParams params, ViewAttachParams attachParams)
{
	createSystemWithMedia(std::move(io), path, displayName, e, params, attachParams,
		[this](const Input::Event &e)
		{
			recentContent.add(system());
			launchSystem(e);
		});
}

void EmuApp::handleOpenFileCommand(CStringView path)
{
	auto name = appContext().fileUriDisplayName(path);
	if(name.empty())
	{
		postErrorMessage(std::format("Can't access path name for:\n{}", path));
		return;
	}
	if(appContext().fileUriType(path) == FS::file_type::directory)
	{
		log.info("changing to dir {} from external command", path);
		showUI(false);
		viewController().popToRoot();
		contentSearchPath = path;
		viewController().pushAndShow(
			FilePicker::forLoading(attachParams(), appContext().defaultInputEvent()),
			appContext().defaultInputEvent(),
			false);
	}
	else
	{
		log.info("opening file {} from external command", path);
		showUI();
		viewController().popToRoot();
		onSelectFileFromPicker({}, path, name, Input::KeyEvent{}, {}, attachParams());
	}
}

void EmuApp::runBenchmarkOneShot(EmuVideo &video)
{
	log.info("starting benchmark");
	auto time = system().benchmark(video);
	autosaveManager.resetSlot(noAutosaveName);
	closeSystem();
	auto timeSecs = duration_cast<FloatSeconds>(time);
	log.info("done in:{}", timeSecs);
	postMessage(2, 0, std::format("{:.2f} fps", 180. / timeSecs.count()));
}

void EmuApp::showEmulation()
{
	if(viewController().isShowingEmulation() || !system().hasContent())
		return;
	configureAppForEmulation(true);
	resetInput();
	inputManager.vController.applySavedButtonAlpha();
	viewController().emuView.setShowFrameTimingStats(showFrameTimingStats);
	viewController().showEmulationView();
	startEmulation();
}

void EmuApp::startEmulation()
{
	if(!viewController().isShowingEmulation())
		return;
	videoLayer.setBrightnessScale(1.f);
	frameTimingStats = {};
	system().start(*this);
	systemTask.start(emuWindow());
	setCPUNeedsLowLatency(appContext(), true);
	gameManager.setGameState({.mode = GameStateMode::GameplayInterruptible});
}

void EmuApp::showUI(bool updateTopView)
{
	if(!viewController().isShowingEmulation())
		return;
	pauseEmulation();
	configureAppForEmulation(false);
	videoLayer.setBrightnessScale(menuVideoBrightnessScale);
	viewController().showMenuView(updateTopView);
}

void EmuApp::pauseEmulation()
{
	systemTask.stop();
	setCPUNeedsLowLatency(appContext(), false);
	gameManager.setGameState({.mode = GameStateMode::None});
	system().pause(*this);
	setRunSpeed(1.);
	videoLayer.setBrightnessScale(pausedVideoBrightnessScale);
}

bool EmuApp::hasArchiveExtension(std::string_view name)
{
	return FS::hasArchiveExtension(name);
}

void EmuApp::pushAndShowModalView(std::unique_ptr<View> v, const Input::Event &e)
{
	viewController().pushAndShowModal(std::move(v), e, false);
}

void EmuApp::pushAndShowModalView(std::unique_ptr<View> v)
{
	auto e = v->appContext().defaultInputEvent();
	pushAndShowModalView(std::move(v), e);
}

void EmuApp::popModalViews()
{
	viewController().popModalViews();
}

void EmuApp::popMenuToRoot()
{
	viewController().popToRoot();
}

void EmuApp::reloadSystem(EmuSystemCreateParams params)
{
	if(!system().hasContent())
		return;
	pauseEmulation();
	viewController().popToSystemActionsMenu();
	auto ctx = appContext();
	try
	{
		system().createWithMedia({}, system().contentLocation(),
			ctx.fileUriDisplayName(system().contentLocation()), params,
			[](int, int, const char*){ return true; });
		onSystemCreated();
		if(autosaveManager.slotName() != noAutosaveName)
			system().loadBackupMemory(*this);
	}
	catch(...)
	{
		log.error("Error reloading system");
		system().clearGamePaths();
	}
}

void EmuApp::onSystemCreated()
{
	updateVideoContentRotation();
	if(!rewindManager.reset(system().stateSize()))
	{
		postErrorMessage(4, "Not enough memory for rewind states");
	}
	viewController().onSystemCreated();
}

void EmuApp::promptSystemReloadDueToSetOption(ViewAttachParams attach, const Input::Event &e, EmuSystemCreateParams params)
{
	if(!system().hasContent())
		return;
	viewController().pushAndShowModal(std::make_unique<YesNoAlertView>(attach,
		"This option takes effect next time the system starts. Restart it now?",
		YesNoAlertView::Delegates
		{ .onYes = [this, params]
			{
				reloadSystem(params);
				showEmulation();
				return false;
			}
		}), e, false);
}

void EmuApp::unpostMessage()
{
	viewController().popup.clear();
}

void EmuApp::printScreenshotResult(bool success)
{
	postMessage(3, !success, std::format("{}{}",
		success ? "Wrote screenshot at " : "Error writing screenshot at ",
		appContext().formatDateAndTime(WallClock::now())));
}

[[gnu::weak]] bool EmuApp::willCreateSystem(ViewAttachParams, const Input::Event&) { return true; }

void EmuApp::createSystemWithMedia(IO io, CStringView path, std::string_view displayName,
	const Input::Event &e, EmuSystemCreateParams params, ViewAttachParams attachParams,
	CreateSystemCompleteDelegate onComplete)
{
	assume(std::strlen(path));
	if(!EmuApp::hasArchiveExtension(displayName) && !AppMeta::defaultFsFilter(displayName))
	{
		postErrorMessage("File doesn't have a valid extension");
		return;
	}
	if(!EmuApp::willCreateSystem(attachParams, e))
	{
		return;
	}
	closeSystem();
	auto loadProgressView = std::make_unique<LoadProgressView>(attachParams, e, onComplete);
	auto &msgPort = loadProgressView->messagePort();
	pushAndShowModalView(std::move(loadProgressView), e);
	gameManager.setGameState({.mode = GameStateMode::None, .isLoading = true});
	makeDetachedThread(
		[this, io{std::move(io)}, pathStr = FS::PathString{path}, nameStr = FS::FileString{displayName}, &msgPort, params]() mutable
		{
			log.info("starting loader thread");
			try
			{
				system().createWithMedia(std::move(io), pathStr, nameStr, params,
					[&msgPort](int pos, int max, const char *label)
					{
						int len = label ? std::string_view{label}.size() : -1;
						auto msg = EmuSystem::LoadProgressMessage{EmuSystem::LoadProgress::UPDATE, pos, max, len};
						msgPort.sendWithExtraData(msg, std::span{label, len > 0 ? size_t(len) : 0});
						return true;
					});
				msgPort.send({EmuSystem::LoadProgress::OK, 0, 0, 0});
				log.info("loader thread finished");
			}
			catch(std::exception &err)
			{
				system().clearGamePaths();
				std::string_view errStr{err.what()};
				auto len = errStr.size();
				if(len > 1024)
				{
					log.warn("truncating long error size:{}", len);
					len = 1024;
				}
				msgPort.sendWithExtraData({EmuSystem::LoadProgress::FAILED, 0, 0, int(len)}, std::span{errStr.data(), len});
				log.error("loader thread failed");
				return;
			}
		});
}

FS::PathString EmuApp::contentSavePath(std::string_view name) const
{
	auto slotName = autosaveManager.slotName();
	if(slotName.size() && slotName != noAutosaveName)
		return system().contentLocalSaveDirectory(slotName, name);
	else
		return system().contentSavePath(name);
}

FS::PathString EmuApp::contentSaveFilePath(std::string_view ext) const
{
	auto slotName = autosaveManager.slotName();
	if(slotName.size() && slotName != noAutosaveName)
		return system().contentLocalSaveDirectory(slotName, FS::FileString{"auto"}.append(ext));
	else
		return system().contentSaveFilePath(ext);
}

void EmuApp::setupStaticBackupMemoryFile(FileIO &io, std::string_view ext, size_t size, uint8_t initValue) const
{
	if(io)
		return;
	io = system().openStaticBackupMemoryFile(system().contentSaveFilePath(ext), size, initValue);
	if(!io) [[unlikely]]
		throw std::runtime_error(std::format("Error opening {}, please verify save path has write access", system().contentNameExt(ext)));
}

void EmuApp::readState(std::span<uint8_t> buff)
{
	auto suspendCtx = suspendEmulationThread();
	system().readState(*this, buff);
	system().clearInputBuffers();
	autosaveManager.resetTimer();
}

size_t EmuApp::writeState(std::span<uint8_t> buff, SaveStateFlags flags)
{
	auto suspendCtx = suspendEmulationThread();
	return system().writeState(buff, flags);
}

DynArray<uint8_t> EmuApp::saveState()
{
	auto suspendCtx = suspendEmulationThread();
	return system().saveState();
}

bool EmuApp::saveState(CStringView path, bool notify)
{
	if(!system().hasContent())
	{
		postErrorMessage("System not running");
		return false;
	}
	log.info("saving state {}", path);
	auto suspendCtx = suspendEmulationThread();
	try
	{
		system().saveState(path);
		if(notify)
			postMessage("State Saved");
		return true;
	}
	catch(std::exception &err)
	{
		postErrorMessage(4, std::format("Can't save state:\n{}", err.what()));
		return false;
	}
}

bool EmuApp::saveStateWithSlot(int slot, bool notify)
{
	return saveState(system().statePath(slot), notify);
}

bool EmuApp::loadState(CStringView path)
{
	if(!system().hasContent()) [[unlikely]]
	{
		postErrorMessage("System not running");
		return false;
	}
	log.info("loading state {}", path);
	auto suspendCtx = suspendEmulationThread();
	try
	{
		system().loadState(*this, path);
		autosaveManager.resetTimer();
		return true;
	}
	catch(std::exception &err)
	{
		if(system().hasContent() && !hasWriteAccessToDir(system().contentSaveDirectory()))
			postErrorMessage(8, "Save folder inaccessible, please set it in Options➔File Paths➔Saves");
		else
			postErrorMessage(4, std::format("Can't load state:\n{}", err.what()));
		return false;
	}
}

bool EmuApp::loadStateWithSlot(int slot)
{
	assume(slot != -1);
	return loadState(system().statePath(slot));
}

FS::PathString EmuApp::inContentSearchPath(std::string_view name) const
{
	return FS::uriString(contentSearchPath, name);
}

FS::PathString EmuApp::validSearchPath(const FS::PathString &path) const
{
	if(path.empty())
		return contentSearchPath;
	return hasArchiveExtension(path) ? FS::dirnameUri(path) : path;
}

[[gnu::weak]] void EmuApp::onMainWindowCreated(ViewAttachParams, const Input::Event &) {}

[[gnu::weak]] std::unique_ptr<View> EmuApp::makeCustomView(ViewAttachParams, EmuApp::ViewID)
{
	return nullptr;
}

std::unique_ptr<YesNoAlertView> EmuApp::makeCloseContentView()
{
	return std::make_unique<YesNoAlertView>(attachParams(), "Really close current content?",
		YesNoAlertView::Delegates
		{
			.onYes = [this]
			{
				closeSystem(); // pops any System Actions views in the stack
				viewController().popModalViews();
				return false;
			}
		});
}

void EmuApp::resetInput()
{
	inputManager.turboModifierActive = false;
	inputManager.turboActions = {};
	setRunSpeed(1.);
}

void EmuApp::setRunSpeed(double speed)
{
	assume(speed > 0.);
	auto _ = suspendEmulationThread();
	system().frameRateMultiplier = speed;
	audio.setSpeedMultiplier(speed);
	systemTask.updateSystemFrameRate();
}

FS::PathString EmuApp::sessionConfigPath()
{
	return system().contentSaveFilePath(".config");
}

bool EmuApp::hasSavedSessionOptions()
{
	return system().sessionOptionsAreSet() || appContext().fileUriExists(sessionConfigPath());
}

void EmuApp::resetSessionOptions()
{
	inputManager.resetSessionOptions(appContext());
	saveStateSlot.reset();
	system().resetSessionOptions(*this);
}

void EmuApp::deleteSessionOptions()
{
	if(!hasSavedSessionOptions())
	{
		return;
	}
	resetSessionOptions();
	system().resetSessionOptionsSet();
	appContext().removeFileUri(sessionConfigPath());
}

void EmuApp::saveSessionOptions()
{
	if(!system().sessionOptionsAreSet())
		return;
	auto configFilePath = sessionConfigPath();
	try
	{
		auto ctx = appContext();
		auto configFile = ctx.openFileUri(configFilePath, OpenFlags::newFile());
		writeConfigHeader(configFile);
		system().writeConfig(ConfigType::SESSION, configFile);
		writeOptionValueIfNotDefault(configFile, saveStateSlot);
		inputManager.writeSessionConfig(configFile);
		system().resetSessionOptionsSet();
		if(configFile.size() == 1)
		{
			// delete file if only header was written
			configFile = {};
			ctx.removeFileUri(configFilePath);
			log.info("deleted empty session config file:{}", configFilePath);
		}
		else
		{
			log.info("wrote session config file:{}", configFilePath);
		}
	}
	catch(...)
	{
		log.info("error creating session config file:{}", configFilePath);
	}
}

void EmuApp::loadSessionOptions()
{
	resetSessionOptions();
	auto ctx = appContext();
	if(readConfigKeys(FileUtils::bufferFromUri(ctx, sessionConfigPath(), {.test = true}),
		[this, ctx](auto key, auto& io) -> bool
		{
			if(key == CFGKEY_SAVE_STATE_SLOT)
			{
				readOptionValue(io, saveStateSlot);
				return true;
			}
			if(inputManager.readSessionConfig(ctx, io, key))
				return true;
			if(system().readConfig(ConfigType::SESSION, io, key))
				return true;
			log.info("skipping unknown key {}", key);
			return false;
		}))
	{
		system().onSessionOptionsLoaded(*this);
	}
}

void EmuApp::loadSystemOptions()
{
	auto configName = system().configName();
	if(configName.empty())
		return;
	readConfigKeys(FileUtils::bufferFromPath(FS::pathString(appContext().supportPath(), configName), {.test = true}),
		[this](uint16_t key, auto &io)
		{
			if(!system().readConfig(ConfigType::CORE, io, key))
			{
				log.info("skipping unknown system config key:{}", key);
			}
		});
}

void EmuApp::saveSystemOptions()
{
	auto configName = system().configName();
	if(configName.empty())
		return;
	try
	{
		auto configFilePath = FS::pathString(appContext().supportPath(), configName);
		auto configFile = FileIO{configFilePath, OpenFlags::newFile()};
		saveSystemOptions(configFile);
		if(configFile.size() == 1)
		{
			// delete file if only header was written
			configFile = {};
			FS::remove(configFilePath);
			log.info("deleted empty system config file");
		}
	}
	catch(...)
	{
		log.error("error writing system config file");
	}
}

void EmuApp::saveSystemOptions(FileIO &configFile)
{
	writeConfigHeader(configFile);
	system().writeConfig(ConfigType::CORE, configFile);
}

EmuSystemTask::SuspendContext EmuApp::suspendEmulationThread() { return systemTask.suspend(); }

void EmuApp::updateFrameRate() { systemTask.updateSystemFrameRate(); }

bool EmuApp::writeScreenshot(PixmapView pix, CStringView path)
{
	return pixmapWriter.writeToFile(pix, path);
}

FS::PathString EmuApp::makeNextScreenshotFilename()
{
	static constexpr std::string_view subDirName = "screenshots";
	auto &sys = system();
	auto userPath = sys.userPath(userScreenshotPath);
	sys.createContentLocalDirectory(userPath, subDirName);
	return sys.contentLocalDirectory(userPath, subDirName,
		appContext().formatDateAndTimeAsFilename(WallClock::now()).append(".png"));
}

void EmuApp::setMogaManagerActive(bool on, bool notify)
{
	doIfUsed(mogaManagerPtr,
		[&](auto &mogaManagerPtr)
		{
			if(on)
				mogaManagerPtr = std::make_unique<Input::MogaManager>(appContext(), notify);
			else
				mogaManagerPtr.reset();
		});
}

ViewAttachParams EmuApp::attachParams()
{
	return viewController().inputView.attachParams();
}

bool EmuApp::setFontSize(int size)
{
	if(!fontSize.set(size))
		return false;
	applyFontSize(viewController().emuWindow());
	viewController().placeElements();
	return true;
}

void EmuApp::configureAppForEmulation(bool running)
{
	appContext().setIdleDisplayPowerSave(running ? idleDisplayPowerSave.value() : true);
	applyOSNavStyle(appContext(), running);
	appContext().setHintKeyRepeat(!running);
}

void EmuApp::onFocusChange(bool in)
{
	if(viewController().isShowingEmulation())
	{
		if(in && system().isPaused())
		{
			log.info("resuming emulation due to window focus");
			viewController().inputView.resetInput();
			startEmulation();
		}
		else if(pauseUnfocused && !system().isPaused() && !allWindowsAreFocused())
		{
			log.info("pausing emulation with all windows unfocused");
			pauseEmulation();
			viewController().postDrawToEmuWindows();
		}
	}
}

bool EmuApp::allWindowsAreFocused() const
{
	return windowData(appContext().mainWindow()).focused && (!hasExtraWindow(appContext()) || extraWindowIsFocused(appContext()));
}

void EmuApp::setEmuViewOnExtraWindow(bool on, Screen &screen)
{
	auto ctx = appContext();
	if(on && !hasExtraWindow(ctx))
	{
		log.info("setting emu view on extra window");
		WindowConfig winConf{ .title = ApplicationMeta::name };
		winConf.setScreen(screen);
		winConf.setFormat(windowDrawableConfig.pixelFormat);
		auto extraWin = ctx.makeWindow(winConf,
			[this](ApplicationContext, Window &win)
			{
				renderer.attachWindow(win, windowDrawableConfig);
				auto &extraWinData = win.makeAppData<WindowData>();
				extraWinData.hasPopup = false;
				extraWinData.focused = true;
				auto suspendCtx = systemTask.setWindow(win);
				mainWindow().setFrameEventsOnThisThread();
				mainWindow().setDrawEventEnabled(true);
				extraWinData.updateWindowViewport(win, makeViewport(win), renderer);
				viewController().moveEmuViewToWindow(win);

				win.onEvent = [this](Window& win, const WindowEvent& winEvent)
				{
					return winEvent.visit(overloaded
					{
						[&](const Input::Event& e) { return viewController().extraWindowInputEvent(e); },
						[&](const DrawEvent& e)
						{
							return viewController().drawExtraWindow(win, e.params, renderer.task());
						},
						[&](const WindowSurfaceChangeEvent& e)
						{
							if(e.change.resized())
							{
								viewController().updateExtraWindowViewport(win, makeViewport(win), renderer.task());
							}
							renderer.task().updateDrawableForSurfaceChange(win, e.change);
							return true;
						},
						[&](const DragDropEvent& e)
						{
							log.info("got DnD:{}", e.filename);
							handleOpenFileCommand(e.filename);
							return true;
						},
						[&](const FocusChangeEvent& e)
						{
							windowData(win).focused = e.in;
							onFocusChange(e.in);
							return true;
						},
						[&](const DismissRequestEvent&)
						{
							win.dismiss();
							return true;
						},
						[&](const DismissEvent&)
						{
							auto suspendCtx = systemTask.setWindow(mainWindow());
							system().resetFrameTiming();
							log.info("setting emu view on main window");
							viewController().moveEmuViewToWindow(appContext().mainWindow());
							viewController().movePopupToWindow(appContext().mainWindow());
							viewController().placeEmuViews();
							suspendCtx.resume();
							mainWindow().postDraw();
							return true;
						},
						[](auto&){ return false; }
					});
				};

				win.show();
				viewController().placeEmuViews();
				suspendCtx.resume();
				mainWindow().postDraw();
			});
		if(!extraWin)
		{
			log.error("error creating extra window");
			return;
		}
	}
	else if(!on && hasExtraWindow(ctx))
	{
		dismissExtraWindow(ctx);
	}
}

void EmuApp::configureSecondaryScreens()
{
	if(showOnSecondScreen && appContext().screens().size() > 1)
	{
		setEmuViewOnExtraWindow(true, *appContext().screens()[1]);
	}
}

void EmuApp::record(FrameTimingStatEvent event, SteadyClockTimePoint t)
{
	if(!viewController().emuView.showingFrameTimingStats())
			return;
	auto setTime = [](auto& var, SteadyClockTimePoint t)
	{
		if(!used(var))
			return;
		var = hasTime(t) ? t : SteadyClock::now();
	};
	switch(event)
	{
		case FrameTimingStatEvent::startOfFrame: return setTime(frameTimingStats.startOfFrame, t);
		case FrameTimingStatEvent::startOfEmulation: return setTime(frameTimingStats.startOfEmulation, t);
		case FrameTimingStatEvent::waitForPresent: return setTime(frameTimingStats.waitForPresent, t);
		case FrameTimingStatEvent::endOfFrame: return setTime(frameTimingStats.endOfFrame, t);
	}
	std::unreachable();
}

bool EmuApp::setAltSpeed(AltSpeedMode mode, int16_t speed)
{
	if(mode == AltSpeedMode::slow)
		return slowModeSpeed.set(speed);
	else
		return fastModeSpeed.set(speed);
}

void EmuApp::applyCPUAffinity(bool active)
{
	if(cpuAffinityMode.value() == CPUAffinityMode::Any)
		return;
	auto frameThreadGroup = std::vector{systemTask.threadId(), renderer.task().threadId()};
	system().addThreadGroupIds(frameThreadGroup);
	if(cpuAffinityMode.value() == CPUAffinityMode::Auto && perfHintManager)
	{
		if(active)
		{
			auto targetDuration = emuScreen().targetFrameDuration();
			perfHintSession = perfHintManager.session(frameThreadGroup, targetDuration);
			if(perfHintSession)
				log.info("made performance hint session with target duration:{}", targetDuration);
			else
				log.error("error making performance hint session with target duration:{}", targetDuration);
		}
		else
		{
			perfHintSession = {};
			log.info("closed performance hint session");
		}
		return;
	}
	auto mask = active ?
		(cpuAffinityMode.value() == CPUAffinityMode::Auto ? appContext().performanceCPUMask() : cpuAffinityMask.value()) : 0;
	log.info("applying CPU affinity mask {:X}", mask);
	setThreadCPUAffinityMask(frameThreadGroup, mask);
}

void EmuApp::setCPUAffinity(int cpuNumber, bool on)
{
	doIfUsed(cpuAffinityMask, [&](auto &cpuAffinityMask)
	{
		cpuAffinityMask = setOrClearBits(cpuAffinityMask, bit(cpuNumber), on);
	});
}

bool EmuApp::cpuAffinity(int cpuNumber) const
{
	return doIfUsed(cpuAffinityMask, [&](auto &cpuAffinityMask) { return cpuAffinityMask & bit(cpuNumber); }, false);
}

void EmuApp::setLowLatencyVideo(bool on)
{
	lowLatencyVideo = on;
	video.resetImage();
}

std::unique_ptr<View> EmuApp::makeView(ViewAttachParams attach, ViewID id)
{
	auto view = makeCustomView(attach, id);
	if(view)
		return view;
	switch(id)
	{
		case ViewID::MAIN_MENU: return std::make_unique<MainMenuView>(attach);
		case ViewID::SYSTEM_ACTIONS: return std::make_unique<SystemActionsView>(attach);
		case ViewID::VIDEO_OPTIONS: return std::make_unique<VideoOptionView>(attach, videoLayer);
		case ViewID::AUDIO_OPTIONS: return std::make_unique<AudioOptionView>(attach, audio);
		case ViewID::SYSTEM_OPTIONS: return std::make_unique<SystemOptionView>(attach);
		case ViewID::FILE_PATH_OPTIONS: return std::make_unique<FilePathOptionView>(attach);
		case ViewID::GUI_OPTIONS: return std::make_unique<GUIOptionView>(attach);
		default: unreachable();
	}
}

void EmuApp::closeBluetoothConnections()
{
	Bluetooth::closeBT(bluetoothAdapter);
}

void EmuApp::reportFrameWorkDuration(Nanoseconds nSecs)
{
	perfHintSession.reportActualWorkDuration(nSecs);
}

MainWindowData &EmuApp::mainWindowData() const
{
	return EmuEx::mainWindowData(appContext().mainWindow());
}

EmuApp &EmuApp::get(ApplicationContext ctx)
{
	return ctx.applicationAs<EmuApp>();
}

EmuApp &gApp() { return *gAppPtr; }

ApplicationContext gAppContext() { return gApp().appContext(); }

void pushAndShowModalView(std::unique_ptr<View> v, const Input::Event &e)
{
	v->appContext().applicationAs<EmuApp>().viewController().pushAndShowModal(std::move(v), e, false);
}

void pushAndShowNewYesNoAlertView(ViewAttachParams attach, const Input::Event &e, const char *label,
	const char *choice1, const char *choice2, TextMenuItem::SelectDelegate onYes, TextMenuItem::SelectDelegate onNo)
{
	attach.appContext().applicationAs<EmuApp>().pushAndShowModalView(std::make_unique<YesNoAlertView>(attach, label, choice1, choice2, YesNoAlertView::Delegates{onYes, onNo}), e);
}

Gfx::TextureSpan collectTextCloseAsset(ApplicationContext ctx)
{
	return ctx.applicationAs<const EmuApp>().collectTextCloseAsset();
}

void postErrorMessage(ApplicationContext ctx, std::string_view s)
{
	ctx.applicationAs<EmuApp>().postErrorMessage(s);
}

}
