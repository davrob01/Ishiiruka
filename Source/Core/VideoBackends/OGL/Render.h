// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>
#include "VideoCommon/RenderBase.h"

namespace OGL
{
void ClearEFBCache();

enum GLSL_VERSION
{
	GLSL_130,
	GLSL_140,
	GLSL_150,
	GLSL_330,
	GLSL_400,    // and above
	GLSLES_300,  // GLES 3.0
	GLSLES_310,  // GLES 3.1
	GLSLES_320,  // GLES 3.2
};
enum class ES_TEXBUF_TYPE
{
	TEXBUF_NONE,
	TEXBUF_CORE,
	TEXBUF_OES,
	TEXBUF_EXT
};

// ogl-only config, so not in VideoConfig.h
struct VideoConfig
{
	bool bSupportsGLSLCache;
	bool bSupportsGLPinnedMemory;
	bool bSupportsGLSync;
	bool bSupportsGLBaseVertex;
	bool bSupportsGLBufferStorage;
	bool bSupportsMSAA;
	GLSL_VERSION eSupportedGLSLVersion;
	bool bSupportViewportFloat;
	bool bSupportsAEP;
	bool bSupportsDebug;
	bool bSupportsCopySubImage;
	u8 SupportedESPointSize;
	ES_TEXBUF_TYPE SupportedESTextureBuffer;
	bool bSupports2DTextureStorage;
	bool bSupports3DTextureStorage;
	bool bSupportsEarlyFragmentTests;
	bool bSupportsConservativeDepth;
	bool bSupportsAniso;

	const char* gl_vendor;
	const char* gl_renderer;
	const char* gl_version;

	s32 max_samples;
};
extern VideoConfig g_ogl_config;

class Renderer : public ::Renderer
{
public:
	Renderer();
	~Renderer();

	static void Init();
	static void Shutdown();

	void SetColorMask() override;
	void SetBlendMode(bool forceUpdate) override;
	void SetScissorRect(const TargetRectangle& rc) override;
	void SetGenerationMode() override;
	void SetDepthMode() override;
	void SetLogicOpMode() override;
	void SetDitherMode() override;
	void SetSamplerState(int stage, int texindex, bool custom_tex) override;
	void SetInterlacingMode() override;
	void SetViewport() override;

	// TODO: Implement and use these
	void ApplyState(bool bUseDstAlpha) override;
	void RestoreState() override {}

	void RenderText(const std::string& text, int left, int top, u32 color) override;

	u32 AccessEFB(EFBAccessType type, u32 x, u32 y, u32 poke_data) override;
	void PokeEFB(EFBAccessType type, const EfbPokeData* points, size_t num_points) override;

	u16 BBoxRead(int index) override;
	void BBoxWrite(int index, u16 value) override;

	void ResetAPIState() override;
	void RestoreAPIState() override;

	TargetRectangle ConvertEFBRectangle(const EFBRectangle& rc) override;

	void SwapImpl(u32 xfbAddr, u32 fbWidth, u32 fbStride, u32 fbHeight, const EFBRectangle& rc, u64 ticks,
		float Gamma) override;

	void ClearScreen(const EFBRectangle& rc, bool colorEnable, bool alphaEnable, bool zEnable,
		u32 color, u32 z) override;

	void ReinterpretPixelData(unsigned int convtype) override;

	u32 GetMaxTextureSize() override;

	void ChangeSurface(void* new_surface_handle) override;

private:
	struct ViewPort {
		float       X;
		float       Y;
		float       Width;
		float       Height;
		float       NearZ;
		float       FarZ;
	};
	bool m_bColorMaskChanged;
	bool m_bBlendModeChanged;
	bool m_bBlendModeForce;
	bool m_bScissorRectChanged;
	bool m_bViewPortChanged;
	TargetRectangle m_ScissorRect;
	ViewPort m_viewport;
	bool m_bGenerationModeChanged;
	bool m_bDepthModeChanged;
	bool m_bLogicOpModeChanged;
	bool m_bViewPortChangedRequested;

	void _SetColorMask();
	void _SetBlendMode(bool forceUpdate);
	void _SetScissorRect();
	void _SetGenerationMode();
	void _SetDepthMode();
	void _SetLogicOpMode();
	void _SetViewport();
	void UpdateEFBCache(EFBAccessType type, u32 cacheRectIdx, const EFBRectangle& efbPixelRc, const TargetRectangle& targetPixelRc, const void* data);
	void BlitScreen(const TargetRectangle& dst_rect, const TargetRectangle& src_rect, const  TargetSize& src_size, GLuint src_texture, GLuint src_depth_texture, float gamma);

	void FlushFrameDump();
	void DumpFrame(const TargetRectangle& flipped_trc, u64 ticks);

	// avi dumping state to delay one frame
	std::array<u32, 2> m_frame_dumping_pbo = {};
	std::array<bool, 2> m_frame_pbo_is_mapped = {};
	std::array<int, 2> m_last_frame_width = {};
	std::array<int, 2> m_last_frame_height = {};
	bool m_last_frame_exported = false;
	AVIDump::Frame m_last_frame_state;
};
}
