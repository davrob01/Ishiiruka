// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/Vulkan/TextureEncoder.h"

#include <algorithm>
#include <cstring>

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/Renderer.h"
#include "VideoBackends/Vulkan/StagingTexture2D.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/Texture2D.h"
#include "VideoBackends/Vulkan/Util.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/TextureConversionShader.h"
#include "VideoCommon/TextureDecoder.h"

namespace Vulkan
{
TextureEncoder::TextureEncoder()
{
}

TextureEncoder::~TextureEncoder()
{
	if (m_encoding_render_pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(g_vulkan_context->GetDevice(), m_encoding_render_pass, nullptr);

	if (m_encoding_texture_framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(g_vulkan_context->GetDevice(), m_encoding_texture_framebuffer, nullptr);

	for (VkShaderModule shader : m_texture_encoding_shaders)
	{
		if (shader != VK_NULL_HANDLE)
			vkDestroyShaderModule(g_vulkan_context->GetDevice(), shader, nullptr);
	}
}

bool TextureEncoder::Initialize()
{
	if (!CompileShaders())
	{
		PanicAlert("Failed to compile shaders");
		return false;
	}

	if (!CreateEncodingRenderPass())
	{
		PanicAlert("Failed to create encode render pass");
		return false;
	}

	if (!CreateEncodingTexture())
	{
		PanicAlert("Failed to create encoding texture");
		return false;
	}

	if (!CreateDownloadTexture())
	{
		PanicAlert("Failed to create download texture");
		return false;
	}

	return true;
}

void TextureEncoder::EncodeTextureToRam(VkImageView src_texture, u8* dest_ptr, u32 format,
	u32 native_width, u32 bytes_per_row, u32 num_blocks_y,
	u32 memory_stride, PEControl::PixelFormat src_format,
	bool is_intensity, int scale_by_half,
	const EFBRectangle& src_rect)
{
	if (m_texture_encoding_shaders[format] == VK_NULL_HANDLE)
	{
		ERROR_LOG(VIDEO, "Missing encoding fragment shader for format %u", format);
		return;
	}

	// Can't do our own draw within a render pass.
	StateTracker::GetInstance()->EndRenderPass();

	UtilityShaderDraw draw(g_command_buffer_mgr->GetCurrentCommandBuffer(),
		g_object_cache->GetPushConstantPipelineLayout(), m_encoding_render_pass,
		g_object_cache->GetScreenQuadVertexShader(), VK_NULL_HANDLE,
		m_texture_encoding_shaders[format]);

	// Uniform - int4 of left,top,native_width,scale
	s32 position_uniform[4] = { src_rect.left, src_rect.top, static_cast<s32>(native_width),
		scale_by_half ? 2 : 1 };
	draw.SetPushConstants(position_uniform, sizeof(position_uniform));

	// Doesn't make sense to linear filter depth values
	draw.SetPSSampler(0, src_texture, (scale_by_half && src_format != PEControl::Z24) ?
		g_object_cache->GetLinearSampler() :
		g_object_cache->GetPointSampler());

	u32 render_width = bytes_per_row / sizeof(u32);
	u32 render_height = num_blocks_y;
	Util::SetViewportAndScissor(g_command_buffer_mgr->GetCurrentCommandBuffer(), 0, 0, render_width,
		render_height);

	// TODO: We could use compute shaders here.
	VkRect2D render_region = { { 0, 0 },{ render_width, render_height } };
	draw.BeginRenderPass(m_encoding_texture_framebuffer, render_region);
	draw.DrawWithoutVertexBuffer(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 4);
	draw.EndRenderPass();

	// Transition the image before copying
	m_encoding_texture->OverrideImageLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	m_download_texture->CopyFromImage(g_command_buffer_mgr->GetCurrentCommandBuffer(),
		m_encoding_texture->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
		render_width, render_height, 0, 0);

	// Block until the GPU has finished copying to the staging texture.
	g_command_buffer_mgr->ExecuteCommandBuffer(false, true);
	StateTracker::GetInstance()->InvalidateDescriptorSets();
	StateTracker::GetInstance()->SetPendingRebind();

	// Copy from staging texture to the final destination, adjusting pitch if necessary.
	m_download_texture->ReadTexels(0, 0, render_width, render_height, dest_ptr, memory_stride);
}

bool TextureEncoder::CompileShaders()
{
	// Texture encoding shaders
	static const u32 texture_encoding_shader_formats[] = {
		GX_TF_I4,   GX_TF_I8,   GX_TF_IA4,  GX_TF_IA8,  GX_TF_RGB565, GX_TF_RGB5A3, GX_TF_RGBA8,
		GX_CTF_R4,  GX_CTF_RA4, GX_CTF_RA8, GX_CTF_A8,  GX_CTF_R8,    GX_CTF_G8,    GX_CTF_B8,
		GX_CTF_RG8, GX_CTF_GB8, GX_CTF_Z8H, GX_TF_Z8,   GX_CTF_Z16R,  GX_TF_Z16,    GX_TF_Z24X8,
		GX_CTF_Z4,  GX_CTF_Z8M, GX_CTF_Z8L, GX_CTF_Z16L };
	for (u32 format : texture_encoding_shader_formats)
	{
		const char* shader_source =
			TextureConversionShader::GenerateEncodingShader(format, API_VULKAN);
		m_texture_encoding_shaders[format] = Util::CompileAndCreateFragmentShader(shader_source);
		if (m_texture_encoding_shaders[format] == VK_NULL_HANDLE)
			return false;
	}

	return true;
}

bool TextureEncoder::CreateEncodingRenderPass()
{
	VkAttachmentDescription attachments[] = {
		{ 0, ENCODING_TEXTURE_FORMAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } };

	VkAttachmentReference color_attachment_references[] = {
		{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };

	VkSubpassDescription subpass_descriptions[] = { { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1,
		color_attachment_references, nullptr, nullptr, 0,
		nullptr } };

	VkSubpassDependency dependancies[] = {
		{ 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT } };

	VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		nullptr,
		0,
		static_cast<u32>(ArraySize(attachments)),
		attachments,
		static_cast<u32>(ArraySize(subpass_descriptions)),
		subpass_descriptions,
		static_cast<u32>(ArraySize(dependancies)),
		dependancies };

	VkResult res = vkCreateRenderPass(g_vulkan_context->GetDevice(), &pass_info, nullptr,
		&m_encoding_render_pass);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateRenderPass (Encode) failed: ");
		return false;
	}

	return true;
}

bool TextureEncoder::CreateEncodingTexture()
{
	// From OGL: Why do we create a 1024 height texture?
	m_encoding_texture = Texture2D::Create(
		ENCODING_TEXTURE_WIDTH, ENCODING_TEXTURE_HEIGHT, 1, 1, ENCODING_TEXTURE_FORMAT,
		VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!m_encoding_texture)
		return false;

	VkImageView framebuffer_attachments[] = { m_encoding_texture->GetView() };
	VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		nullptr,
		0,
		m_encoding_render_pass,
		static_cast<u32>(ArraySize(framebuffer_attachments)),
		framebuffer_attachments,
		m_encoding_texture->GetWidth(),
		m_encoding_texture->GetHeight(),
		m_encoding_texture->GetLayers() };

	VkResult res = vkCreateFramebuffer(g_vulkan_context->GetDevice(), &framebuffer_info, nullptr,
		&m_encoding_texture_framebuffer);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateFramebuffer failed: ");
		return false;
	}

	return true;
}

bool TextureEncoder::CreateDownloadTexture()
{
	m_download_texture =
		StagingTexture2D::Create(STAGING_BUFFER_TYPE_READBACK, ENCODING_TEXTURE_WIDTH,
			ENCODING_TEXTURE_HEIGHT, ENCODING_TEXTURE_FORMAT);

	if (!m_download_texture || !m_download_texture->Map())
		return false;

	return true;
}

}  // namespace Vulkan
