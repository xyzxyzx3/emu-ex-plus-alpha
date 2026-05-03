/*  This file is part of GBC.emu.

	GBC.emu is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	GBC.emu is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with GBC.emu.  If not, see <http://www.gnu.org/licenses/> */

module;
#include <resample/resampler.h>
#include <resample/resamplerinfo.h>
#include <libgambatte/src/mem/cartridge.h>
#include <libgambatte/src/video/lcddef.h>

module system;

namespace EmuEx
{

constexpr WSize lcdSize{gambatte::lcd_hres, gambatte::lcd_vres};

extern "C++" std::string_view EmuSystem::shortSystemName() const { return "GB"; }
extern "C++" std::string_view EmuSystem::systemName() const { return "Game Boy"; }

uint_least32_t GbcSystem::makeOutputColor(uint_least32_t rgb888) const
{
	unsigned b = rgb888       & 0xFF;
	unsigned g = rgb888 >>  8 & 0xFF;
	unsigned r = rgb888 >> 16 & 0xFF;
	auto desc = useBgrOrder ? PixelDescBGRA8888Native : PixelDescRGBA8888Native;
	return desc.build(r, g, b, 0u);
}

void GbcSystem::applyGBPalette()
{
    GBPalette pal;

    pal.bg  = { 0xE0F8D0, 0x88C070, 0x346856, 0x081820 };
    pal.sp1 = pal.bg;
    pal.sp2 = pal.bg;

    for(auto i: iotaCount(4))
        gbEmu.setDmgPaletteColor(0, i, makeOutputColor(pal.bg[i]));

    for(auto i: iotaCount(4))
        gbEmu.setDmgPaletteColor(1, i, makeOutputColor(pal.sp1[i]));

    for(auto i: iotaCount(4))
        gbEmu.setDmgPaletteColor(2, i, makeOutputColor(pal.sp2[i]));
}

void GbcSystem::reset(EmuApp& app, ResetMode)
{
	flushBackupMemory(app);
	gbEmu.reset();
	loadBackupMemory(app);
}

FS::FileString GbcSystem::stateFilename(int slot, std::string_view name) const
{
	return format<FS::FileString>("{}.0{}.gqs", name, saveSlotCharUpper(slot));
}

void GbcSystem::readState(EmuApp&, std::span<uint8_t> buff)
{
	IStream<MapIO> stream{buff};
	if(!gbEmu.loadState(stream))
		throw std::runtime_error("Invalid state data");
}

size_t GbcSystem::writeState(std::span<uint8_t> buff, SaveStateFlags)
{
	assume(saveStateSize == buff.size());
	OStream<MapIO> stream{buff};
	gbEmu.saveState(frameBuffer, gambatte::lcd_hres, stream);
	return saveStateSize;
}

void GbcSystem::loadBackupMemory(EmuApp &app)
{
	if(auto sram = gbEmu.srambank();
		sram.size())
	{
		log.info("loading sram");
		app.setupStaticBackupMemoryFile(saveFileIO, ".sav", sram.size(), 0xFF);
		saveFileIO.read(sram, 0);
	}
	if(auto timeOpt = gbEmu.rtcTime();
		timeOpt)
	{
		log.info("loading rtc");
		app.setupStaticBackupMemoryFile(rtcFileIO, ".rtc", 4);
		auto rtcData = rtcFileIO.get<std::array<uint8_t, 4>>(0);
		gbEmu.setRtcTime(rtcData[0] << 24 | rtcData[1] << 16 | rtcData[2] << 8 | rtcData[3]);
	}
}

void GbcSystem::onFlushBackupMemory(EmuApp &, BackupMemoryDirtyFlags)
{
	if(auto sram = gbEmu.srambank();
		sram.size())
	{
		log.info("saving sram");
		saveFileIO.write(sram, 0);
	}
	if(auto timeOpt = gbEmu.rtcTime();
		timeOpt)
	{
		log.info("saving rtc");
		rtcFileIO.put(std::array<uint8_t, 4>
			{
				uint8_t(*timeOpt >> 24 & 0xFF),
				uint8_t(*timeOpt >> 16 & 0xFF),
				uint8_t(*timeOpt >>  8 & 0xFF),
				uint8_t(*timeOpt       & 0xFF)
			}, 0);
	}
}

WallClockTimePoint GbcSystem::backupMemoryLastWriteTime(const EmuApp &app) const
{
	return appContext().fileUriLastWriteTime(app.contentSaveFilePath(".sav").c_str());
}

void GbcSystem::closeSystem()
{
	cheatList.clear();
	saveFileIO = {};
	rtcFileIO = {};
	gameBuiltinPalette = nullptr;
	totalFrames = 0;
	totalSamples = 0;
}

void GbcSystem::loadContent(IO &io, EmuSystemCreateParams, OnLoadProgressDelegate)
{
	gbEmu.setSaveDir(std::string{contentSaveDirectory()});
	auto buff = io.buffer();
	if(!buff)
	{
		throwFileReadError();
	}
	if(auto result = gbEmu.load(buff.data(), buff.size(), contentFileName().data(), optionReportAsGba ? gbEmu.GBA_CGB : 0);
		result != gambatte::LOADRES_OK)
	{
		throw std::runtime_error(gambatte::to_string(result));
	}
	if(!gbEmu.isCgb())
	{
		gameBuiltinPalette = findGbcTitlePal(gbEmu.romTitle().c_str());
		if(gameBuiltinPalette)
			log.info("game {} has built-in palette", gbEmu.romTitle());
		applyGBPalette();
	}
	readCheatFile();
	applyCheats();
	saveStateSize = 0;
	OStream<OutSizeTracker> stream{&saveStateSize};
	gbEmu.saveState(frameBuffer, gambatte::lcd_hres, stream);
}

bool GbcSystem::onVideoRenderFormatChange(EmuVideo &video, PixelFormat fmt)
{
	video.setFormat({lcdSize, fmt});
	auto isBgrOrder = fmt == PixelFmtBGRA8888;
	if(isBgrOrder != useBgrOrder)
	{
		useBgrOrder = isBgrOrder;
		MutablePixmapView frameBufferPix{{lcdSize, PixelFmtRGBA8888}, frameBuffer};
		frameBufferPix.transformInPlace(
			[](uint32_t srcPixel) // swap red/blue values
			{
				return (srcPixel & 0xFF000000) | ((srcPixel & 0xFF0000) >> 16) | (srcPixel & 0x00FF00) | ((srcPixel & 0x0000FF) << 16);
			});
	}
	refreshPalettes();
	return true;
}

void GbcSystem::configAudioRate(FrameRate outputFrameRate, int outputRate)
{
	// input/output frame rate parameters swapped to generate the sound input rate
	long inputRate = std::round(audioMixRate(2097152, outputFrameRate, frameRate()));
	if(optionAudioResampler >= ResamplerInfo::num())
		optionAudioResampler = std::min(ResamplerInfo::num(), 1zu);
	if(!resampler || optionAudioResampler != activeResampler
		|| resampler->outRate() != outputRate  || resampler->inRate() != inputRate)
	{
		log.info("setting up resampler {} for input rate {}Hz", optionAudioResampler.value(), inputRate);
		resampler.reset(ResamplerInfo::get(optionAudioResampler).create(inputRate, outputRate, 35112 + 2064));
		activeResampler = optionAudioResampler;
	}
}

size_t GbcSystem::runUntilVideoFrame(uint_least32_t *videoBuf, std::ptrdiff_t pitch,
	EmuAudio *audio, VideoFrameDelegate videoFrameCallback)
{
	size_t samplesEmulated = 0;
	constexpr unsigned samplesPerRun = 2064;
	bool didOutputFrame;
	do
	{
		std::array<uint_least32_t, samplesPerRun+2064> snd;
		size_t samples = samplesPerRun;
		didOutputFrame = gbEmu.runFor(videoBuf, pitch, snd.data(), samples, videoFrameCallback) != -1;
		samplesEmulated += samples;
		if(audio)
		{
			constexpr size_t buffSize = (snd.size() / (2097152./48000.) + 1); // TODO: std::ceil() is constexpr with GCC but not Clang yet
			std::array<uint32_t, buffSize> destBuff;
			unsigned destFrames = resampler->resample((short*)destBuff.data(), (const short*)snd.data(), samples);
			assume(destFrames <= destBuff.size());
			audio->writeFrames(destBuff.data(), destFrames);
		}
	} while(!didOutputFrame);
	return samplesEmulated;
}

void GbcSystem::renderVideo(const EmuSystemTaskContext &taskCtx, EmuVideo &video)
{
	auto fmt = video.renderPixelFormat() == PixelFmtBGRA8888 ? PixelFmtBGRA8888 : PixelFmtRGBA8888;
	PixmapView frameBufferPix{{lcdSize, fmt}, frameBuffer};
	video.startFrameWithAltFormat(taskCtx, frameBufferPix);
}

void GbcSystem::runFrame(EmuSystemTaskContext taskCtx, EmuVideo *video, EmuAudio *audio)
{
	auto incFrameCountOnReturn = scopeGuard([&](){ totalFrames++; });
	auto currentFrame = totalSamples / 35112;
	if(totalFrames < currentFrame)
	{
		log.info("unchanged video frame");
		if(video)
			video->startUnchangedFrame(taskCtx);
		return;
	}
	if(video)
	{
		totalSamples += runUntilVideoFrame(frameBuffer, gambatte::lcd_hres, audio,
			[this, &taskCtx, video]()
			{
				renderVideo(taskCtx, *video);
			});
	}
	else
	{
		totalSamples += runUntilVideoFrame(nullptr, gambatte::lcd_hres, audio, {});
	}
}

void GbcSystem::renderFramebuffer(EmuVideo &video)
{
	renderVideo({}, video);
}

void GbcSystem::updateColorConversionFlags()
{
	unsigned flags{};
	if(optionFullGbcSaturation)
		flags |= COLOR_CONVERSION_SATURATED_BIT;
	if(useBgrOrder)
		flags |= COLOR_CONVERSION_BGR_BIT;
	gbEmu.setColorConversionFlags(flags);
}

void GbcSystem::refreshPalettes()
{
	updateColorConversionFlags();
	if(!hasContent())
		return;
	gbEmu.refreshPalettes();
}

}

extern "C++"
{

EmuEx::uint_least32_t gbcToRgb32(unsigned const bgr15, unsigned flags)
{
	unsigned r = bgr15       & 0x1F;
	unsigned g = bgr15 >>  5 & 0x1F;
	unsigned b = bgr15 >> 10 & 0x1F;
	unsigned outR, outG, outB;
	if(flags & EmuEx::COLOR_CONVERSION_SATURATED_BIT)
	{
		outR = (r * 255 + 15) / 31;
		outG = (g * 255 + 15) / 31;
		outB = (b * 255 + 15) / 31;
	}
	else
	{
		outR = (r * 13 + g * 2 + b) >> 1;
		outG = (g * 3 + b) << 1;
		outB = (r * 3 + g * 2 + b * 11) >> 1;
	}
	auto desc = (flags & EmuEx::COLOR_CONVERSION_BGR_BIT) ? IG::PixelDescBGRA8888Native : IG::PixelDescRGBA8888Native;
	return desc.build(outR, outG, outB, 0u);
}

namespace gambatte
{
	// no-ops, all save data is explicitly loaded/saved
	void Cartridge::loadSavedata() {}
	void Cartridge::saveSavedata() {}
}

}
