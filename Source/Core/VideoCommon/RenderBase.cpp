// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// ---------------------------------------------------------------------------------------------
// GC graphics pipeline
// ---------------------------------------------------------------------------------------------
// 3d commands are issued through the fifo. The gpu draws to the 2MB EFB.
// The efb can be copied back into ram in two forms: as textures or as XFB.
// The XFB is the region in RAM that the VI chip scans out to the television.
// So, after all rendering to EFB is done, the image is copied into one of two XFBs in RAM.
// Next frame, that one is scanned out and the other one gets the copy. = double buffering.
// ---------------------------------------------------------------------------------------------

#include <cinttypes>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/FileUtil.h"
#include "Common/Flag.h"
#include "Common/Profiler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Host.h"
#include "Core/Movie.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/VideoInterface.h"

#include "VideoCommon/AVIDump.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/Debugger.h"
#include "VideoCommon/FPSCounter.h"
#include "VideoCommon/FramebufferManagerBase.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/XFMemory.h"

// TODO: Move these out of here.
int frameCount;
int OSDChoice;
static int OSDTime;

std::unique_ptr<Renderer> g_renderer;

std::mutex Renderer::s_criticalScreenshot;
std::string Renderer::s_sScreenshotName;

Common::Event Renderer::s_screenshotCompleted;

Common::Flag Renderer::s_screenshot;

// The framebuffer size
int Renderer::s_target_width;
int Renderer::s_target_height;

// TODO: Add functionality to reinit all the render targets when the window is resized.
int Renderer::s_backbuffer_width;
int Renderer::s_backbuffer_height;

std::unique_ptr<PostProcessor> Renderer::m_post_processor;

// Final surface changing
Common::Flag Renderer::s_surface_needs_change;
Common::Event Renderer::s_surface_changed;
void* Renderer::s_new_surface_handle;

TargetRectangle Renderer::target_rc;
TargetRectangle Renderer::window_rc;

int Renderer::s_last_efb_scale;

bool Renderer::XFBWrited;

PEControl::PixelFormat Renderer::prev_efb_format = PEControl::INVALID_FMT;
unsigned int Renderer::efb_scale_numeratorX = 1;
unsigned int Renderer::efb_scale_numeratorY = 1;
unsigned int Renderer::efb_scale_denominatorX = 1;
unsigned int Renderer::efb_scale_denominatorY = 1;
unsigned int Renderer::ssaa_multiplier = 1;

// The maximum depth that is written to the depth buffer should never exceed this value.
// This is necessary because we use a 2^24 divisor for all our depth values to prevent
// floating-point round-trip errors. However the console GPU doesn't ever write a value
// to the depth buffer that exceeds 2^24 - 1.
const float Renderer::GX_MAX_DEPTH = 16777215.0f / 16777216.0f;

static float AspectToWidescreen(float aspect)
{
	return aspect * ((16.0f / 9.0f) / (4.0f / 3.0f));
}

Renderer::Renderer()
{
	UpdateActiveConfig();
	TextureCacheBase::OnConfigChanged(g_ActiveConfig);
	OSDChoice = 0;
	OSDTime = 0;
}

Renderer::~Renderer()
{
	// invalidate previous efb format
	prev_efb_format = PEControl::INVALID_FMT;

	efb_scale_numeratorX = efb_scale_numeratorY = efb_scale_denominatorX = efb_scale_denominatorY = 1;
	
	ShutdownFrameDumping();
	if (m_frame_dump_thread.joinable())
		m_frame_dump_thread.join();
}

void Renderer::RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc, u32 fbStride, u32 fbHeight, float Gamma)
{
	CheckFifoRecording();

	if (!fbStride || !fbHeight)
		return;

	XFBWrited = true;

	if (g_ActiveConfig.bUseXFB)
	{
		FramebufferManagerBase::CopyToXFB(xfbAddr, fbStride, fbHeight, sourceRc, Gamma);
	}
	else
	{
		// The timing is not predictable here. So try to use the XFB path to dump frames.
		u64 ticks = CoreTiming::GetTicks();
		// below div two to convert from bytes to pixels - it expects width, not stride
		Swap(xfbAddr, fbStride / 2, fbStride / 2, fbHeight, sourceRc, ticks, Gamma);
	}
}

int Renderer::EFBToScaledX(int x)
{
	switch (g_ActiveConfig.iEFBScale)
	{
	case SCALE_AUTO: // fractional
		return (int)ssaa_multiplier * FramebufferManagerBase::ScaleToVirtualXfbWidth(x);

	default:
		return x * (int)ssaa_multiplier * (int)efb_scale_numeratorX / (int)efb_scale_denominatorX;
	};
}

int Renderer::EFBToScaledY(int y)
{
	switch (g_ActiveConfig.iEFBScale)
	{
	case SCALE_AUTO: // fractional
		return (int)ssaa_multiplier * FramebufferManagerBase::ScaleToVirtualXfbHeight(y);

	default:
		return y * (int)ssaa_multiplier * (int)efb_scale_numeratorY / (int)efb_scale_denominatorY;
	};
}

void Renderer::CalculateTargetScale(int x, int y, int &scaledX, int &scaledY)
{
	if (g_ActiveConfig.iEFBScale == SCALE_AUTO)
	{
		scaledX = x;
		scaledY = y;
	}
	else
	{
		scaledX = x * (int)efb_scale_numeratorX / (int)efb_scale_denominatorX;
		scaledY = y * (int)efb_scale_numeratorY / (int)efb_scale_denominatorY;
	}
}

// return true if target size changed
bool Renderer::CalculateTargetSize(unsigned int framebuffer_width, unsigned int framebuffer_height, int multiplier)
{
	int newEFBWidth = 0;
	int newEFBHeight = 0;

	// TODO: Ugly. Clean up
	switch (s_last_efb_scale)
	{
	case SCALE_AUTO:
	case SCALE_AUTO_INTEGRAL:
		newEFBWidth = FramebufferManagerBase::ScaleToVirtualXfbWidth(EFB_WIDTH);
		newEFBHeight = FramebufferManagerBase::ScaleToVirtualXfbHeight(EFB_HEIGHT);

		if (s_last_efb_scale == SCALE_AUTO_INTEGRAL)
		{
			efb_scale_numeratorX = efb_scale_numeratorY = std::max((newEFBWidth - 1) / EFB_WIDTH + 1, (newEFBHeight - 1) / EFB_HEIGHT + 1);
			efb_scale_denominatorX = efb_scale_denominatorY = 1;
			newEFBWidth = EFBToScaledX(EFB_WIDTH);
			newEFBHeight = EFBToScaledY(EFB_HEIGHT);
		}
		else
		{
			efb_scale_numeratorX = newEFBWidth;
			efb_scale_denominatorX = EFB_WIDTH;
			efb_scale_numeratorY = newEFBHeight;
			efb_scale_denominatorY = EFB_HEIGHT;
		}
		break;
	case SCALE_1X:
		efb_scale_numeratorX = efb_scale_numeratorY = 1;
		efb_scale_denominatorX = efb_scale_denominatorY = 1;
		break;

	case SCALE_1_5X:
		efb_scale_numeratorX = efb_scale_numeratorY = 3;
		efb_scale_denominatorX = efb_scale_denominatorY = 2;
		break;

	case SCALE_2X:
		efb_scale_numeratorX = efb_scale_numeratorY = 2;
		efb_scale_denominatorX = efb_scale_denominatorY = 1;
		break;

	case SCALE_2_5X: // 2.5x
		efb_scale_numeratorX = efb_scale_numeratorY = 5;
		efb_scale_denominatorX = efb_scale_denominatorY = 2;
		break;
	default:
		efb_scale_numeratorX = efb_scale_numeratorY = s_last_efb_scale - 3;
		efb_scale_denominatorX = efb_scale_denominatorY = 1;
		break;
	}
	const u32 max_size = GetMaxTextureSize();
	if (max_size < EFB_WIDTH * multiplier * efb_scale_numeratorX / efb_scale_denominatorX)
	{
		efb_scale_numeratorX = efb_scale_numeratorY = (max_size / (EFB_WIDTH * multiplier));
		efb_scale_denominatorX = efb_scale_denominatorY = 1;
	}

	if (s_last_efb_scale > SCALE_AUTO_INTEGRAL)
		CalculateTargetScale(EFB_WIDTH, EFB_HEIGHT, newEFBWidth, newEFBHeight);

	newEFBWidth *= multiplier;
	newEFBHeight *= multiplier;
	ssaa_multiplier = multiplier;

	if (newEFBWidth != s_target_width || newEFBHeight != s_target_height)
	{
		s_target_width = newEFBWidth;
		s_target_height = newEFBHeight;
		VertexShaderManager::SetViewportChanged();
		GeometryShaderManager::SetViewportChanged();
		PixelShaderManager::SetViewportChanged();
		return true;
	}
	return false;
}

void Renderer::ConvertStereoRectangle(const TargetRectangle& rc, TargetRectangle& leftRc, TargetRectangle& rightRc)
{
	// Resize target to half its original size
	TargetRectangle drawRc = rc;
	if (g_ActiveConfig.iStereoMode == STEREO_TAB)
	{
		// The height may be negative due to flipped rectangles
		int height = rc.bottom - rc.top;
		drawRc.top += height / 4;
		drawRc.bottom -= height / 4;
	}
	else
	{
		int width = rc.right - rc.left;
		drawRc.left += width / 4;
		drawRc.right -= width / 4;
	}

	// Create two target rectangle offset to the sides of the backbuffer
	leftRc = drawRc, rightRc = drawRc;
	if (g_ActiveConfig.iStereoMode == STEREO_TAB)
	{
		leftRc.top -= s_backbuffer_height / 4;
		leftRc.bottom -= s_backbuffer_height / 4;
		rightRc.top += s_backbuffer_height / 4;
		rightRc.bottom += s_backbuffer_height / 4;
	}
	else
	{
		leftRc.left -= s_backbuffer_width / 4;
		leftRc.right -= s_backbuffer_width / 4;
		rightRc.left += s_backbuffer_width / 4;
		rightRc.right += s_backbuffer_width / 4;
	}
}

void Renderer::SetScreenshot(const std::string& filename)
{
	std::lock_guard<std::mutex> lk(s_criticalScreenshot);
	s_sScreenshotName = filename;
	s_screenshot.Set();
}


// Create On-Screen-Messages
void Renderer::DrawDebugText()
{
	std::string final_yellow, final_cyan;

	if (g_ActiveConfig.bShowFPS || SConfig::GetInstance().m_ShowFrameCount)
	{
		if (g_ActiveConfig.bShowFPS)
			final_cyan += StringFromFormat("FPS: %u", g_renderer->m_fps_counter.GetFPS());

		if (g_ActiveConfig.bShowFPS && SConfig::GetInstance().m_ShowFrameCount)
			final_cyan += " - ";
		if (SConfig::GetInstance().m_ShowFrameCount)
		{
			final_cyan += StringFromFormat("Frame: %llu", (unsigned long long)Movie::GetCurrentFrame());
			if (Movie::IsPlayingInput())
				final_cyan += StringFromFormat("\nInput: %llu / %llu",
				(unsigned long long)Movie::GetCurrentInputCount(),
					(unsigned long long)Movie::GetTotalInputCount());
		}

		final_cyan += "\n";
		final_yellow += "\n";
	}

	if (SConfig::GetInstance().m_ShowLag)
	{
		final_cyan += StringFromFormat("Lag: %" PRIu64 "\n", Movie::GetCurrentLagCount());
		final_yellow += "\n";
	}

	if (SConfig::GetInstance().m_ShowInputDisplay)
	{
		final_cyan += Movie::GetInputDisplay();
		final_yellow += "\n";
	}

	if (SConfig::GetInstance().m_ShowRTC)
	{
		final_cyan += Movie::GetRTCDisplay();
		final_yellow += "\n";
	}

	// OSD Menu messages
	if (OSDChoice > 0)
	{
		OSDTime = Common::Timer::GetTimeMs() + 3000;
		OSDChoice = -OSDChoice;
	}

	if ((u32)OSDTime > Common::Timer::GetTimeMs())
	{
		std::string res_text;
		switch (g_ActiveConfig.iEFBScale)
		{
		case SCALE_AUTO:
			res_text = "Auto (fractional)";
			break;
		case SCALE_AUTO_INTEGRAL:
			res_text = "Auto (integral)";
			break;
		case SCALE_1X:
			res_text = "Native";
			break;
		case SCALE_1_5X:
			res_text = "1.5x";
			break;
		case SCALE_2X:
			res_text = "2x";
			break;
		case SCALE_2_5X:
			res_text = "2.5x";
			break;
		default:
			res_text = StringFromFormat("%dx", g_ActiveConfig.iEFBScale - 3);
			break;
		}
		const char* ar_text = "";
		switch (g_ActiveConfig.iAspectRatio)
		{
		case ASPECT_AUTO:
			ar_text = "Auto";
			break;
		case ASPECT_STRETCH:
			ar_text = "Stretch";
			break;
		case ASPECT_ANALOG:
			ar_text = "Force 4:3";
			break;
		case ASPECT_ANALOG_WIDE:
			ar_text = "Force 16:9";
		}

		const char* const efbcopy_text = g_ActiveConfig.bSkipEFBCopyToRam ? "to Texture" : "to RAM";

		// The rows
		const std::string lines[] = {
			std::string("Internal Resolution: ") + res_text,
			std::string("Aspect Ratio: ") + ar_text + (g_ActiveConfig.bCrop ? " (crop)" : ""),
			std::string("Copy EFB: ") + efbcopy_text,
			std::string("Fog: ") + (g_ActiveConfig.bDisableFog ? "Disabled" : "Enabled"),
			SConfig::GetInstance().m_EmulationSpeed <= 0 ?
			"Speed Limit: Unlimited" :
			StringFromFormat("Speed Limit: %li%%",
				std::lround(SConfig::GetInstance().m_EmulationSpeed * 100.f)),
		};

		enum
		{
			lines_count = sizeof(lines) / sizeof(*lines)
		};

		// The latest changed setting in yellow
		for (int i = 0; i != lines_count; ++i)
		{
			if (OSDChoice == -i - 1)
				final_yellow += lines[i];
			final_yellow += '\n';
		}

		// The other settings in cyan
		for (int i = 0; i != lines_count; ++i)
		{
			if (OSDChoice != -i - 1)
				final_cyan += lines[i];
			final_cyan += '\n';
		}
	}

	final_cyan += Common::Profiler::ToString();

	if (g_ActiveConfig.bOverlayStats)
		final_cyan += Statistics::ToString();

	if (g_ActiveConfig.bOverlayProjStats)
		final_cyan += Statistics::ToStringProj();

	// and then the text
	g_renderer->RenderText(final_cyan, 20, 20, 0xFF00FFFF);
	g_renderer->RenderText(final_yellow, 20, 20, 0xFFFFFF00);
}

void Renderer::UpdateDrawRectangle(int backbuffer_width, int backbuffer_height)
{
	float FloatGLWidth = (float)backbuffer_width;
	float FloatGLHeight = (float)backbuffer_height;
	float FloatXOffset = 0;
	float FloatYOffset = 0;

	// The rendering window size
	const float WinWidth = FloatGLWidth;
	const float WinHeight = FloatGLHeight;

	// Update aspect ratio hack values
	// Won't take effect until next frame
	// Don't know if there is a better place for this code so there isn't a 1 frame delay
	if (g_ActiveConfig.bWidescreenHack)
	{
		float source_aspect = VideoInterface::GetAspectRatio();
		if (Core::g_aspect_wide)
			source_aspect = AspectToWidescreen(source_aspect);
		float target_aspect;

		switch (g_ActiveConfig.iAspectRatio)
		{
		case ASPECT_STRETCH:
			target_aspect = WinWidth / WinHeight;
			break;
		case ASPECT_ANALOG:
			target_aspect = VideoInterface::GetAspectRatio();
			break;
		case ASPECT_ANALOG_WIDE:
			target_aspect = AspectToWidescreen(VideoInterface::GetAspectRatio());
			break;
		case ASPECT_4_3:
			target_aspect = 4.0f / 3.0f;
			break;
		case ASPECT_16_9:
			target_aspect = 16.0f / 9.0f;
			break;
		case ASPECT_16_10:
			target_aspect = 16.0f / 10.0f;
			break;
		default:
			// ASPECT_AUTO
			target_aspect = source_aspect;
			break;
		}

		float adjust = source_aspect / target_aspect;
		if (adjust > 1)
		{
			// Vert+
			g_Config.fAspectRatioHackW = 1.0f;
			g_Config.fAspectRatioHackH = 1.0f / adjust;
		}
		else
		{
			// Hor+
			g_Config.fAspectRatioHackW = adjust;
			g_Config.fAspectRatioHackH = 1.0f;
		}
	}
	else
	{
		// Hack is disabled
		g_Config.fAspectRatioHackW = 1.0f;
		g_Config.fAspectRatioHackH = 1.0f;
	}

	// Check for force-settings and override.
	// The rendering window aspect ratio as a proportion of the 4:3 or 16:9 ratio
	float Ratio;
	if (g_ActiveConfig.iAspectRatio == ASPECT_ANALOG_WIDE || (g_ActiveConfig.iAspectRatio != ASPECT_ANALOG && g_ActiveConfig.iAspectRatio < ASPECT_ANALOG_WIDE  && Core::g_aspect_wide))
	{
		Ratio = (WinWidth / WinHeight) / AspectToWidescreen(VideoInterface::GetAspectRatio());
	}
	else if (g_ActiveConfig.iAspectRatio == ASPECT_4_3)
	{
		Ratio = (WinWidth / WinHeight) / (4.0f / 3.0f);
	}
	else if (g_ActiveConfig.iAspectRatio == ASPECT_16_9)
	{
		Ratio = (WinWidth / WinHeight) / (16.0f / 9.0f);
	}
	else if (g_ActiveConfig.iAspectRatio == ASPECT_16_10)
	{
		Ratio = (WinWidth / WinHeight) / (16.0f / 10.0f);
	}
	else
	{
		Ratio = (WinWidth / WinHeight) / VideoInterface::GetAspectRatio();
	}

	if (g_ActiveConfig.iAspectRatio != ASPECT_STRETCH)
	{
		// Check if height or width is the limiting factor. If ratio > 1 the picture is too wide and have to limit the width.
		if (Ratio > 1.0f)
		{
			// Scale down and center in the X direction.
			FloatGLWidth /= Ratio;
			FloatXOffset = (WinWidth - FloatGLWidth) / 2.0f;
		}
		// The window is too high, we have to limit the height
		else
		{
			// Scale down and center in the Y direction.
			FloatGLHeight *= Ratio;
			FloatYOffset = FloatYOffset + (WinHeight - FloatGLHeight) / 2.0f;
		}
	}

	// -----------------------------------------------------------------------
	// Crop the picture from Analog to 4:3 or from Analog (Wide) to 16:9.
	//		Output: FloatGLWidth, FloatGLHeight, FloatXOffset, FloatYOffset
	// ------------------
	if (g_ActiveConfig.iAspectRatio != ASPECT_STRETCH && g_ActiveConfig.bCrop)
	{
		Ratio = (4.0f / 3.0f) / VideoInterface::GetAspectRatio();
		if (Ratio <= 1.0f)
		{
			Ratio = 1.0f / Ratio;
		}
		// The width and height we will add (calculate this before FloatGLWidth and FloatGLHeight is adjusted)
		float IncreasedWidth = (Ratio - 1.0f) * FloatGLWidth;
		float IncreasedHeight = (Ratio - 1.0f) * FloatGLHeight;
		// The new width and height
		FloatGLWidth = FloatGLWidth * Ratio;
		FloatGLHeight = FloatGLHeight * Ratio;
		// Adjust the X and Y offset
		FloatXOffset = FloatXOffset - (IncreasedWidth * 0.5f);
		FloatYOffset = FloatYOffset - (IncreasedHeight * 0.5f);
	}

	int XOffset = (int)(FloatXOffset + 0.5f);
	int YOffset = (int)(FloatYOffset + 0.5f);
	int iWhidth = (int)ceil(FloatGLWidth);
	int iHeight = (int)ceil(FloatGLHeight);
	iWhidth -= iWhidth % 4; // ensure divisibility by 4 to make it compatible with all the video encoders
	iHeight -= iHeight % 4;

	target_rc.left = XOffset;
	target_rc.top = YOffset;
	target_rc.right = XOffset + iWhidth;
	target_rc.bottom = YOffset + iHeight;
}

void Renderer::SetWindowSize(int width, int height)
{
	if (width < 16)
		width = 16;
	if (height < 16)
		height = 16;

	// Scale the window size by the EFB scale.
	CalculateTargetScale(width, height, width, height);

	Host_RequestRenderWindowSize(width, height);
}

void Renderer::CheckFifoRecording()
{
	bool wasRecording = g_bRecordFifoData;
	g_bRecordFifoData = FifoRecorder::GetInstance().IsRecording();

	if (g_bRecordFifoData)
	{
		if (!wasRecording)
		{
			RecordVideoMemory();
		}

		FifoRecorder::GetInstance().EndFrame(CommandProcessor::fifo.CPBase, CommandProcessor::fifo.CPEnd);
	}
}

void Renderer::RecordVideoMemory()
{
	u32 *bpmem_ptr = (u32*)&bpmem;
	u32 cpmem[256];
	// The FIFO recording format splits XF memory into xfmem and xfmem; follow
	// that split here.
	u32 *xfmem_ptr = (u32*)&xfmem;
	u32 *xfregs_ptr = (u32*)&xfmem + FifoDataFile::XF_MEM_SIZE;
	u32 xfregs_size = sizeof(XFMemory) / 4 - FifoDataFile::XF_MEM_SIZE;

	memset(cpmem, 0, 256 * 4);
	FillCPMemoryArray(cpmem);

	FifoRecorder::GetInstance().SetVideoMemory(bpmem_ptr, cpmem, xfmem_ptr, xfregs_ptr, xfregs_size);
}


void Renderer::Swap(u32 xfbAddr, u32 fbWidth, u32 fbStride, u32 fbHeight, const EFBRectangle& rc, u64 ticks, float Gamma)
{
	// TODO: merge more generic parts into VideoCommon
	g_renderer->SwapImpl(xfbAddr, fbWidth, fbStride, fbHeight, rc, ticks, Gamma);

	if (XFBWrited)
		g_renderer->m_fps_counter.Update();

	frameCount++;
	GFX_DEBUGGER_PAUSE_AT(NEXT_FRAME, true);

	// Begin new frame
	// Set default viewport and scissor, for the clear to work correctly
	// New frame
	stats.ResetFrame();

	Core::Callback_VideoCopiedToXFB(XFBWrited || (g_ActiveConfig.bUseXFB && g_ActiveConfig.bUseRealXFB));
	XFBWrited = false;
}

bool Renderer::IsFrameDumping()
{
	if (s_screenshot.IsSet())
		return true;

#if defined(HAVE_LIBAV) || defined(_WIN32)
	if (SConfig::GetInstance().m_DumpFrames)
		return true;
#endif

	ShutdownFrameDumping();
	return false;
}

void Renderer::ShutdownFrameDumping()
{
	if (!m_frame_dump_thread_running.IsSet())
		return;

	FinishFrameData();
	m_frame_dump_thread_running.Clear();
	m_frame_dump_start.Set();
}

void Renderer::DumpFrameData(const u8* data, int w, int h, int stride, const AVIDump::Frame& state, bool swap_upside_down, bool bgra)
{
	FinishFrameData();

	m_frame_dump_config = FrameDumpConfig{ data, w, h, stride, swap_upside_down, bgra, state };

	if (!m_frame_dump_thread_running.IsSet())
	{
		if (m_frame_dump_thread.joinable())
			m_frame_dump_thread.join();
		m_frame_dump_thread_running.Set();
		m_frame_dump_thread = std::thread(&Renderer::RunFrameDumps, this);
	}

	m_frame_dump_start.Set();
	m_frame_dump_frame_running = true;
}

void Renderer::FinishFrameData()
{
	if (!m_frame_dump_frame_running)
		return;

	m_frame_dump_done.Wait();
	m_frame_dump_frame_running = false;
}

void Renderer::RunFrameDumps()
{
	Common::SetCurrentThreadName("FrameDumping");
	bool avi_dump_started = false;

	while (true)
	{
		m_frame_dump_start.Wait();
		if (!m_frame_dump_thread_running.IsSet())
			break;

		auto config = m_frame_dump_config;
		if (config.upside_down)
		{
			config.data = config.data + (config.height - 1) * config.stride;
			config.stride = -config.stride;
		}

		// Save screenshot
		if (s_screenshot.TestAndClear())
		{
			std::lock_guard<std::mutex> lk(s_criticalScreenshot);

			if (TextureToPng(config.data, config.stride, s_sScreenshotName, config.width, config.height,
				false, config.bgra))
				OSD::AddMessage("Screenshot saved to " + s_sScreenshotName);

			// Reset settings
			s_sScreenshotName.clear();
			s_screenshotCompleted.Set();
		}

#if defined(HAVE_LIBAV) || defined(_WIN32)
		if (SConfig::GetInstance().m_DumpFrames)
		{
			if (!avi_dump_started)
			{
				if (AVIDump::Start(config.width, config.height, config.bgra))
				{
					avi_dump_started = true;
				}
				else
				{
					SConfig::GetInstance().m_DumpFrames = false;
				}
			}

			AVIDump::AddFrame(config.data, config.width, config.height, config.stride, config.state);
		}
#endif

		m_frame_dump_done.Set();
	}

#if defined(HAVE_LIBAV) || defined(_WIN32)
	if (avi_dump_started)
	{
		avi_dump_started = false;
		AVIDump::Stop();
	}
#endif
}
