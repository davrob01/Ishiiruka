// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/Vulkan/ObjectCache.h"

#include <algorithm>
#include <sstream>
#include <type_traits>
#include <xxhash.h>

#include "Common/CommonFuncs.h"
#include "Common/LinearDiskCache.h"
#include "Core/ConfigManager.h"
#include "Core/Host.h"

#include "VideoBackends/Vulkan/ShaderCompiler.h"
#include "VideoBackends/Vulkan/StreamBuffer.h"
#include "VideoBackends/Vulkan/Util.h"
#include "VideoBackends/Vulkan/VertexFormat.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Statistics.h"

namespace Vulkan
{
	// The is_trivially_copyable check fails on MSVC due to BitField.
	// TODO: Can we work around this any way?
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 5 && !defined(_MSC_VER)
	static_assert(std::has_trivial_copy_constructor<PipelineInfo>::value,
		"PipelineInfo is trivially copyable");
#elif !defined(_MSC_VER)
	static_assert(std::is_trivially_copyable<PipelineInfo>::value,
		"PipelineInfo is trivially copyable");
#endif

std::unique_ptr<ObjectCache> g_object_cache;

ObjectCache::ObjectCache()
{
}

ObjectCache::~ObjectCache()
{
	DestroyPipelineCache();
	DestroyShaderCaches();
	DestroySharedShaders();
	DestroySamplers();
	DestroyPipelineLayouts();
	DestroyDescriptorSetLayouts();
}

bool ObjectCache::Initialize()
{
	if (!CreateDescriptorSetLayouts())
		return false;

	if (!CreatePipelineLayouts())
		return false;

	LoadShaderCaches();
	if (!CreatePipelineCache(true))
		return false;

	if (!CreateUtilityShaderVertexFormat())
		return false;

	if (!CreateStaticSamplers())
		return false;

	if (!CompileSharedShaders())
		return false;

	m_utility_shader_vertex_buffer =
		StreamBuffer::Create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1024 * 1024, 4 * 1024 * 1024);
	m_utility_shader_uniform_buffer =
		StreamBuffer::Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1024, 4 * 1024 * 1024);
	if (!m_utility_shader_vertex_buffer || !m_utility_shader_uniform_buffer)
		return false;

	return true;
}

static VkPipelineRasterizationStateCreateInfo
GetVulkanRasterizationState(const RasterizationState& state)
{
	return{
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,  // VkStructureType sType
		nullptr,                  // const void*                               pNext
		0,                        // VkPipelineRasterizationStateCreateFlags   flags
		state.depth_clamp,        // VkBool32                                  depthClampEnable
		VK_FALSE,                 // VkBool32                                  rasterizerDiscardEnable
		VK_POLYGON_MODE_FILL,     // VkPolygonMode                             polygonMode
		state.cull_mode,          // VkCullModeFlags                           cullMode
		VK_FRONT_FACE_CLOCKWISE,  // VkFrontFace                               frontFace
		VK_FALSE,                 // VkBool32                                  depthBiasEnable
		0.0f,                     // float                                     depthBiasConstantFactor
		0.0f,                     // float                                     depthBiasClamp
		0.0f,                     // float                                     depthBiasSlopeFactor
		1.0f                      // float                                     lineWidth
	};
}

static VkPipelineMultisampleStateCreateInfo
GetVulkanMultisampleState(const RasterizationState& rs_state)
{
	return{
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,  // VkStructureType sType
		nullptr,                      // const void*                              pNext
		0,                            // VkPipelineMultisampleStateCreateFlags    flags
		rs_state.samples,             // VkSampleCountFlagBits                    rasterizationSamples
		rs_state.per_sample_shading,  // VkBool32                                 sampleShadingEnable
		1.0f,                         // float                                    minSampleShading
		nullptr,                      // const VkSampleMask*                      pSampleMask;
		VK_FALSE,  // VkBool32                                 alphaToCoverageEnable
		VK_FALSE   // VkBool32                                 alphaToOneEnable
	};
}

static VkPipelineDepthStencilStateCreateInfo
GetVulkanDepthStencilState(const DepthStencilState& state)
{
	return{
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,  // VkStructureType sType
		nullptr,             // const void*                               pNext
		0,                   // VkPipelineDepthStencilStateCreateFlags    flags
		state.test_enable,   // VkBool32                                  depthTestEnable
		state.write_enable,  // VkBool32                                  depthWriteEnable
		state.compare_op,    // VkCompareOp                               depthCompareOp
		VK_FALSE,            // VkBool32                                  depthBoundsTestEnable
		VK_FALSE,            // VkBool32                                  stencilTestEnable
		{},                  // VkStencilOpState                          front
		{},                  // VkStencilOpState                          back
		0.0f,                // float                                     minDepthBounds
		1.0f                 // float                                     maxDepthBounds
	};
}

static VkPipelineColorBlendAttachmentState GetVulkanAttachmentBlendState(const BlendState& state)
{
	VkPipelineColorBlendAttachmentState vk_state = {
		state.blend_enable,     // VkBool32                                  blendEnable
		state.src_blend,        // VkBlendFactor                             srcColorBlendFactor
		state.dst_blend,        // VkBlendFactor                             dstColorBlendFactor
		state.blend_op,         // VkBlendOp                                 colorBlendOp
		state.src_alpha_blend,  // VkBlendFactor                             srcAlphaBlendFactor
		state.dst_alpha_blend,  // VkBlendFactor                             dstAlphaBlendFactor
		state.alpha_blend_op,   // VkBlendOp                                 alphaBlendOp
		state.write_mask        // VkColorComponentFlags                     colorWriteMask
	};

	return vk_state;
}

static VkPipelineColorBlendStateCreateInfo
GetVulkanColorBlendState(const BlendState& state,
	const VkPipelineColorBlendAttachmentState* attachments,
	uint32_t num_attachments)
{
	VkPipelineColorBlendStateCreateInfo vk_state = {
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,  // VkStructureType sType
		nullptr,                  // const void*                                   pNext
		0,                        // VkPipelineColorBlendStateCreateFlags          flags
		state.logic_op_enable,    // VkBool32                                      logicOpEnable
		state.logic_op,           // VkLogicOp                                     logicOp
		num_attachments,          // uint32_t                                      attachmentCount
		attachments,              // const VkPipelineColorBlendAttachmentState*    pAttachments
		{ 1.0f, 1.0f, 1.0f, 1.0f }  // float                                         blendConstants[4]
	};

	return vk_state;
}

VkPipeline ObjectCache::GetPipeline(const PipelineInfo& info)
{
	auto iter = m_pipeline_objects.find(info);
	if (iter != m_pipeline_objects.end())
		return iter->second;

	// Declare descriptors for empty vertex buffers/attributes
	static const VkPipelineVertexInputStateCreateInfo empty_vertex_input_state = {
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,  // VkStructureType sType
		nullptr,  // const void*                                pNext
		0,        // VkPipelineVertexInputStateCreateFlags       flags
		0,        // uint32_t                                    vertexBindingDescriptionCount
		nullptr,  // const VkVertexInputBindingDescription*      pVertexBindingDescriptions
		0,        // uint32_t                                    vertexAttributeDescriptionCount
		nullptr   // const VkVertexInputAttributeDescription*    pVertexAttributeDescriptions
	};

	// Vertex inputs
	const VkPipelineVertexInputStateCreateInfo& vertex_input_state =
		info.vertex_format ? info.vertex_format->GetVertexInputStateInfo() : empty_vertex_input_state;

	// Input assembly
	VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,  // VkStructureType sType
		nullptr,                  // const void*                                pNext
		0,                        // VkPipelineInputAssemblyStateCreateFlags    flags
		info.primitive_topology,  // VkPrimitiveTopology                        topology
		VK_TRUE                   // VkBool32                                   primitiveRestartEnable
	};

	// Shaders to stages
	VkPipelineShaderStageCreateInfo shader_stages[3];
	uint32_t num_shader_stages = 0;
	if (info.vs != VK_NULL_HANDLE)
	{
		shader_stages[num_shader_stages++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			nullptr,
			0,
			VK_SHADER_STAGE_VERTEX_BIT,
			info.vs,
			"main" };
	}
	if (info.gs != VK_NULL_HANDLE)
	{
		shader_stages[num_shader_stages++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			nullptr,
			0,
			VK_SHADER_STAGE_GEOMETRY_BIT,
			info.gs,
			"main" };
	}
	if (info.ps != VK_NULL_HANDLE)
	{
		shader_stages[num_shader_stages++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			nullptr,
			0,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			info.ps,
			"main" };
	}

	// Fill in Vulkan descriptor structs from our state structures.
	VkPipelineRasterizationStateCreateInfo rasterization_state =
		GetVulkanRasterizationState(info.rasterization_state);
	VkPipelineMultisampleStateCreateInfo multisample_state =
		GetVulkanMultisampleState(info.rasterization_state);
	VkPipelineDepthStencilStateCreateInfo depth_stencil_state =
		GetVulkanDepthStencilState(info.depth_stencil_state);
	VkPipelineColorBlendAttachmentState blend_attachment_state =
		GetVulkanAttachmentBlendState(info.blend_state);
	VkPipelineColorBlendStateCreateInfo blend_state =
		GetVulkanColorBlendState(info.blend_state, &blend_attachment_state, 1);

	// This viewport isn't used, but needs to be specified anyway.
	static const VkViewport viewport = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	static const VkRect2D scissor = { { 0, 0 },{ 1, 1 } };
	static const VkPipelineViewportStateCreateInfo viewport_state = {
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		nullptr,
		0,          // VkPipelineViewportStateCreateFlags    flags;
		1,          // uint32_t                              viewportCount
		&viewport,  // const VkViewport*                     pViewports
		1,          // uint32_t                              scissorCount
		&scissor    // const VkRect2D*                       pScissors
	};

	// Set viewport and scissor dynamic state so we can change it elsewhere.
	static const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR };
	static const VkPipelineDynamicStateCreateInfo dynamic_state = {
		VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr,
		0,                                            // VkPipelineDynamicStateCreateFlags    flags
		static_cast<u32>(ArraySize(dynamic_states)),  // uint32_t dynamicStateCount
		dynamic_states  // const VkDynamicState*                pDynamicStates
	};

	// Combine to full pipeline info structure.
	VkGraphicsPipelineCreateInfo pipeline_info = {
		VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		nullptr,                // VkStructureType sType
		0,                      // VkPipelineCreateFlags                            flags
		num_shader_stages,      // uint32_t                                         stageCount
		shader_stages,          // const VkPipelineShaderStageCreateInfo*           pStages
		&vertex_input_state,    // const VkPipelineVertexInputStateCreateInfo*      pVertexInputState
		&input_assembly_state,  // const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState
		nullptr,                // const VkPipelineTessellationStateCreateInfo*     pTessellationState
		&viewport_state,        // const VkPipelineViewportStateCreateInfo*         pViewportState
		&rasterization_state,  // const VkPipelineRasterizationStateCreateInfo*    pRasterizationState
		&multisample_state,    // const VkPipelineMultisampleStateCreateInfo*      pMultisampleState
		&depth_stencil_state,  // const VkPipelineDepthStencilStateCreateInfo*     pDepthStencilState
		&blend_state,          // const VkPipelineColorBlendStateCreateInfo*       pColorBlendState
		&dynamic_state,        // const VkPipelineDynamicStateCreateInfo*          pDynamicState
		info.pipeline_layout,  // VkPipelineLayout                                 layout
		info.render_pass,      // VkRenderPass                                     renderPass
		0,                     // uint32_t                                         subpass
		VK_NULL_HANDLE,        // VkPipeline                                       basePipelineHandle
		-1                     // int32_t                                          basePipelineIndex
	};

	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult res = vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(), m_pipeline_cache, 1,
		&pipeline_info, nullptr, &pipeline);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkCreateGraphicsPipelines failed: ");

	m_pipeline_objects.emplace(info, pipeline);
	return pipeline;
}

std::string ObjectCache::GetDiskCacheFileName(const char* type)
{
	return StringFromFormat("%sIVK-%s-%s.cache", File::GetUserPath(D_SHADERCACHE_IDX).c_str(),
		SConfig::GetInstance().m_strGameID.c_str(), type);
}

class PipelineCacheReadCallback : public LinearDiskCacheReader<u32, u8>
{
public:
	PipelineCacheReadCallback(std::vector<u8>* data) : m_data(data) {}
	void Read(const u32& key, const u8* value, u32 value_size) override
	{
		m_data->resize(value_size);
		if (value_size > 0)
			memcpy(m_data->data(), value, value_size);
	}

private:
	std::vector<u8>* m_data;
};

class PipelineCacheReadIgnoreCallback : public LinearDiskCacheReader<u32, u8>
{
public:
	void Read(const u32& key, const u8* value, u32 value_size) override {}
};

bool ObjectCache::CreatePipelineCache(bool load_from_disk)
{
	// We have to keep the pipeline cache file name around since when we save it
	// we delete the old one, by which time the game's unique ID is already cleared.
	m_pipeline_cache_filename = GetDiskCacheFileName("pipeline");

	std::vector<u8> disk_data;
	if (load_from_disk)
	{
		LinearDiskCache<u32, u8> disk_cache;
		PipelineCacheReadCallback read_callback(&disk_data);
		if (disk_cache.OpenAndRead(m_pipeline_cache_filename, read_callback) != 1)
			disk_data.clear();
	}

	VkPipelineCacheCreateInfo info = {
		VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,  // VkStructureType            sType
		nullptr,                                       // const void*                pNext
		0,                                             // VkPipelineCacheCreateFlags flags
		disk_data.size(),                              // size_t                     initialDataSize
		!disk_data.empty() ? disk_data.data() : nullptr,  // const void*                pInitialData
	};

	VkResult res =
		vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr, &m_pipeline_cache);
	if (res == VK_SUCCESS)
		return true;

	// Failed to create pipeline cache, try with it empty.
	LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed, trying empty cache: ");
	info.initialDataSize = 0;
	info.pInitialData = nullptr;
	res = vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr, &m_pipeline_cache);
	if (res == VK_SUCCESS)
		return true;

	LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed: ");
	return false;
}

void ObjectCache::DestroyPipelineCache()
{
	for (const auto& it : m_pipeline_objects)
	{
		if (it.second != VK_NULL_HANDLE)
			vkDestroyPipeline(g_vulkan_context->GetDevice(), it.second, nullptr);
	}
	m_pipeline_objects.clear();

	vkDestroyPipelineCache(g_vulkan_context->GetDevice(), m_pipeline_cache, nullptr);
	m_pipeline_cache = VK_NULL_HANDLE;
}

void ObjectCache::ClearPipelineCache()
{
	// Reallocate the pipeline cache object, so it starts fresh and we don't
	// save old pipelines to disk. This is for major changes, e.g. MSAA mode change.
	DestroyPipelineCache();
	if (!CreatePipelineCache(false))
		PanicAlert("Failed to re-create pipeline cache");
}

void ObjectCache::SavePipelineCache()
{
	size_t data_size;
	VkResult res =
		vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
		return;
	}

	std::vector<u8> data(data_size);
	res = vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size,
		data.data());
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
		return;
	}

	// Delete the old cache and re-create.
	File::Delete(m_pipeline_cache_filename);

	// We write a single key of 1, with the entire pipeline cache data.
	// Not ideal, but our disk cache class does not support just writing a single blob
	// of data without specifying a key.
	LinearDiskCache<u32, u8> disk_cache;
	PipelineCacheReadIgnoreCallback callback;
	disk_cache.OpenAndRead(m_pipeline_cache_filename, callback);
	disk_cache.Append(1, data.data(), static_cast<u32>(data.size()));
	disk_cache.Close();
}

// Cache inserter that is called back when reading from the file
template <typename Uid, typename UidHasher>
struct ShaderCacheReader : public LinearDiskCacheReader<Uid, u32>
{
	ShaderCacheReader(ObjectUsageProfiler<Uid, pKey_t, ObjectCache::vkShaderItem, UidHasher>* shader_map) : m_shader_map(shader_map) {}
	void Read(const Uid& key, const u32* value, u32 value_size) override
	{
		// We don't insert null modules into the shader map since creation could succeed later on.
		// e.g. we're generating bad code, but fix this in a later version, and for some reason
		// the cache is not invalidated.
		VkShaderModule module = Util::CreateShaderModule(value, value_size);
		if (module == VK_NULL_HANDLE)
			return;

		ObjectCache::vkShaderItem& it = m_shader_map->GetOrAdd(key);
		it.initialized.test_and_set();
		it.compiled = true;
		it.module = module;
	}

	ObjectUsageProfiler<Uid, pKey_t, ObjectCache::vkShaderItem, UidHasher>* m_shader_map;
};

void ObjectCache::LoadShaderCaches()
{
	pKey_t gameid = (pKey_t)GetMurmurHash3(reinterpret_cast<const u8*>(SConfig::GetInstance().m_strGameID.data()), (u32)SConfig::GetInstance().m_strGameID.size(), 0);
	m_vs_cache.shader_map.reset(VShaderCache::cache_type::Create(
		gameid,
		VERTEXSHADERGEN_UID_VERSION,
		"Ishiiruka.vs",
		StringFromFormat("%s.vs", SConfig::GetInstance().m_strGameID.c_str())
	));
	m_ps_cache.shader_map.reset(PShaderCache::cache_type::Create(
		gameid,
		PIXELSHADERGEN_UID_VERSION,
		"Ishiiruka.ps",
		StringFromFormat("%s.ps", SConfig::GetInstance().m_strGameID.c_str())
	));
	m_gs_cache.shader_map.reset(GShaderCache::cache_type::Create(
		gameid,
		GEOMETRYSHADERGEN_UID_VERSION,
		"Ishiiruka.gs",
		StringFromFormat("%s.gs", SConfig::GetInstance().m_strGameID.c_str())
	));
	ShaderCacheReader<VertexShaderUid, VertexShaderUid::ShaderUidHasher> vs_reader(m_vs_cache.shader_map.get());
	m_vs_cache.disk_cache.OpenAndRead(GetDiskCacheFileName("vs"), vs_reader);

	ShaderCacheReader<PixelShaderUid, PixelShaderUid::ShaderUidHasher> ps_reader(m_ps_cache.shader_map.get());
	m_ps_cache.disk_cache.OpenAndRead(GetDiskCacheFileName("ps"), ps_reader);

	if (g_vulkan_context->SupportsGeometryShaders())
	{
		ShaderCacheReader<GeometryShaderUid, GeometryShaderUid::ShaderUidHasher> gs_reader(m_gs_cache.shader_map.get());
		m_gs_cache.disk_cache.OpenAndRead(GetDiskCacheFileName("gs"), gs_reader);
	}

	if (g_ActiveConfig.bCompileShaderOnStartup)
	{
		int shader_count = 0;
		m_vs_cache.shader_map->ForEachMostUsedByCategory(gameid,
			[&](const VertexShaderUid& uid, size_t total)
		{
			VertexShaderUid item = uid;
			item.ClearHASH();
			item.CalculateUIDHash();
			vkShaderItem& it = m_vs_cache.shader_map->GetOrAdd(item);
			if (!it.initialized.test_and_set())
			{
				Host_UpdateTitle(StringFromFormat("Compiling Vertex Shaders %i %% (%i/%i)", (shader_count * 100) / total, shader_count, total));
				CompileVertexShaderForUid(item, it);
			}
		},
			[](vkShaderItem& entry)
		{
			return !entry.compiled;
		}
		, true);
		shader_count = 0;
		m_ps_cache.shader_map->ForEachMostUsedByCategory(gameid,
			[&](const PixelShaderUid& uid, size_t total)
		{
			PixelShaderUid item = uid;
			item.ClearHASH();
			item.CalculateUIDHash();
			vkShaderItem& it = m_ps_cache.shader_map->GetOrAdd(item);
			if (!it.initialized.test_and_set())
			{
				Host_UpdateTitle(StringFromFormat("Compiling Pixel Shaders %i %% (%i/%i)", (shader_count * 100) / total, shader_count, total));
				CompilePixelShaderForUid(item, it);
			}
		},
			[](vkShaderItem& entry)
		{
			return !entry.compiled;
		}
		, true);

		if (g_vulkan_context->SupportsGeometryShaders())
		{
			shader_count = 0;
			m_gs_cache.shader_map->ForEachMostUsedByCategory(gameid,
				[&](const GeometryShaderUid& uid, size_t total)
			{
				GeometryShaderUid item = uid;
				item.ClearHASH();
				item.CalculateUIDHash();
				vkShaderItem& it = m_gs_cache.shader_map->GetOrAdd(item);
				if (!it.initialized.test_and_set())
				{
					Host_UpdateTitle(StringFromFormat("Compiling Geometry Shaders %i %% (%i/%i)", (shader_count * 100) / total, shader_count, total));
					CompileGeometryShaderForUid(item, it);
				}
			},
				[](vkShaderItem& entry)
			{
				return !entry.compiled;
			}
			, true);
		}
	}

	SETSTAT(stats.numVertexShadersCreated, static_cast<int>(m_vs_cache.shader_map->size()));
	SETSTAT(stats.numVertexShadersAlive, static_cast<int>(m_vs_cache.shader_map->size()));
	SETSTAT(stats.numPixelShadersCreated, static_cast<int>(m_ps_cache.shader_map->size()));
	SETSTAT(stats.numPixelShadersAlive, static_cast<int>(m_ps_cache.shader_map->size()));
}

template <typename T>
static void DestroyShaderCache(T& cache)
{
	cache.disk_cache.Close();
	cache.shader_map->ForEach([](ObjectCache::vkShaderItem& entry)
	{
		if (entry.module != VK_NULL_HANDLE)
			vkDestroyShaderModule(g_vulkan_context->GetDevice(), entry.module, nullptr);
	});
	cache.shader_map.reset();
}

void ObjectCache::DestroyShaderCaches()
{
	DestroyShaderCache(m_vs_cache);
	DestroyShaderCache(m_ps_cache);

	if (g_vulkan_context->SupportsGeometryShaders())
		DestroyShaderCache(m_gs_cache);
}

void ObjectCache::CompileVertexShaderForUid(const VertexShaderUid& uid, ObjectCache::vkShaderItem& it)
{
	// Not in the cache, so compile the shader.
	ShaderCompiler::SPIRVCodeVector spv;
	VkShaderModule module = VK_NULL_HANDLE;
	ShaderCode source_code;
	GenerateVertexShaderCodeVulkan(source_code, uid.GetUidData());
	if (ShaderCompiler::CompileVertexShader(&spv, source_code.GetBuffer(),
		source_code.BufferSize()))
	{
		module = Util::CreateShaderModule(spv.data(), spv.size());

		// Append to shader cache if it created successfully.
		if (module != VK_NULL_HANDLE)
		{
			m_vs_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
			INCSTAT(stats.numVertexShadersCreated);
			INCSTAT(stats.numVertexShadersAlive);
		}
	}
	it.compiled = true;
	// We still insert null entries to prevent further compilation attempts.
	it.module = module;
}
void ObjectCache::CompileGeometryShaderForUid(const GeometryShaderUid& uid, ObjectCache::vkShaderItem& it)
{
	// Not in the cache, so compile the shader.
	ShaderCompiler::SPIRVCodeVector spv;
	VkShaderModule module = VK_NULL_HANDLE;
	ShaderCode source_code;
	GenerateGeometryShaderCode(source_code, uid.GetUidData(), API_VULKAN);
	if (ShaderCompiler::CompileGeometryShader(&spv, source_code.GetBuffer(),
		source_code.BufferSize()))
	{
		module = Util::CreateShaderModule(spv.data(), spv.size());

		// Append to shader cache if it created successfully.
		if (module != VK_NULL_HANDLE)
			m_gs_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
	}
	it.compiled = true;
	// We still insert null entries to prevent further compilation attempts.
	it.module = module;
}

void ObjectCache::CompilePixelShaderForUid(const PixelShaderUid& uid, ObjectCache::vkShaderItem& it)
{
	// Not in the cache, so compile the shader.
	ShaderCompiler::SPIRVCodeVector spv;
	VkShaderModule module = VK_NULL_HANDLE;
	ShaderCode source_code;
	GeneratePixelShaderCodeVulkan(source_code, uid.GetUidData());
	if (ShaderCompiler::CompileFragmentShader(&spv, source_code.GetBuffer(),
		source_code.BufferSize()))
	{
		module = Util::CreateShaderModule(spv.data(), spv.size());

		// Append to shader cache if it created successfully.
		if (module != VK_NULL_HANDLE)
		{
			m_ps_cache.disk_cache.Append(uid, spv.data(), static_cast<u32>(spv.size()));
			INCSTAT(stats.numPixelShadersCreated);
			INCSTAT(stats.numPixelShadersAlive);
		}
	}
	it.compiled = true;
	// We still insert null entries to prevent further compilation attempts.
	it.module = module;
}

VkShaderModule ObjectCache::GetVertexShaderForUid(const VertexShaderUid& uid)
{
	vkShaderItem& it = m_vs_cache.shader_map->GetOrAdd(uid);
	if (it.initialized.test_and_set())
		return it.module;

	CompileVertexShaderForUid(uid, it);
	return it.module;
}

VkShaderModule ObjectCache::GetGeometryShaderForUid(const GeometryShaderUid& uid)
{
	_assert_(g_vulkan_context->SupportsGeometryShaders());
	vkShaderItem& it = m_gs_cache.shader_map->GetOrAdd(uid);
	if (it.initialized.test_and_set())
		return it.module;

	CompileGeometryShaderForUid(uid, it);
	return it.module;
}

VkShaderModule ObjectCache::GetPixelShaderForUid(const PixelShaderUid& uid)
{
	vkShaderItem& it = m_ps_cache.shader_map->GetOrAdd(uid);
	if (it.initialized.test_and_set())
		return it.module;

	CompilePixelShaderForUid(uid, it);
	return it.module;
}

void ObjectCache::ClearSamplerCache()
{
	for (const auto& it : m_sampler_cache)
	{
		if (it.second != VK_NULL_HANDLE)
			vkDestroySampler(g_vulkan_context->GetDevice(), it.second, nullptr);
	}
	m_sampler_cache.clear();
}

void ObjectCache::DestroySamplers()
{
	ClearSamplerCache();

	if (m_point_sampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(g_vulkan_context->GetDevice(), m_point_sampler, nullptr);
		m_point_sampler = VK_NULL_HANDLE;
	}

	if (m_linear_sampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(g_vulkan_context->GetDevice(), m_linear_sampler, nullptr);
		m_linear_sampler = VK_NULL_HANDLE;
	}
}

void ObjectCache::RecompileSharedShaders()
{
	DestroySharedShaders();
	if (!CompileSharedShaders())
		PanicAlert("Failed to recompile shared shaders.");
}

bool ObjectCache::CreateDescriptorSetLayouts()
{
	static const VkDescriptorSetLayoutBinding ubo_set_bindings[] = {
		{ UBO_DESCRIPTOR_SET_BINDING_PS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT },
		{ UBO_DESCRIPTOR_SET_BINDING_VS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
		{ UBO_DESCRIPTOR_SET_BINDING_GS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
		VK_SHADER_STAGE_GEOMETRY_BIT } };

	// Annoying these have to be split, apparently we can't partially update an array without the
	// validation layers throwing a warning.
	static const VkDescriptorSetLayoutBinding sampler_set_bindings[] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
		{ 13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT }, 
		{ 14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT }, 
		{ 15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT } };

	static const VkDescriptorSetLayoutBinding ssbo_set_bindings[] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT } };

	static const VkDescriptorSetLayoutCreateInfo create_infos[NUM_DESCRIPTOR_SETS] = {
		{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
		static_cast<u32>(ArraySize(ubo_set_bindings)), ubo_set_bindings },
		{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
		static_cast<u32>(ArraySize(sampler_set_bindings)), sampler_set_bindings },
		{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
		static_cast<u32>(ArraySize(ssbo_set_bindings)), ssbo_set_bindings } };

	for (size_t i = 0; i < NUM_DESCRIPTOR_SETS; i++)
	{
		VkResult res = vkCreateDescriptorSetLayout(g_vulkan_context->GetDevice(), &create_infos[i],
			nullptr, &m_descriptor_set_layouts[i]);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateDescriptorSetLayout failed: ");
			return false;
		}
	}

	return true;
}

void ObjectCache::DestroyDescriptorSetLayouts()
{
	for (VkDescriptorSetLayout layout : m_descriptor_set_layouts)
	{
		if (layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(g_vulkan_context->GetDevice(), layout, nullptr);
	}
}

bool ObjectCache::CreatePipelineLayouts()
{
	VkResult res;

	// Descriptor sets for each pipeline layout
	VkDescriptorSetLayout standard_sets[] = {
		m_descriptor_set_layouts[DESCRIPTOR_SET_UNIFORM_BUFFERS],
		m_descriptor_set_layouts[DESCRIPTOR_SET_PIXEL_SHADER_SAMPLERS] };
	VkDescriptorSetLayout bbox_sets[] = {
		m_descriptor_set_layouts[DESCRIPTOR_SET_UNIFORM_BUFFERS],
		m_descriptor_set_layouts[DESCRIPTOR_SET_PIXEL_SHADER_SAMPLERS],
		m_descriptor_set_layouts[DESCRIPTOR_SET_SHADER_STORAGE_BUFFERS] };
	VkPushConstantRange push_constant_range = {
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PUSH_CONSTANT_BUFFER_SIZE };

	// Info for each pipeline layout
	VkPipelineLayoutCreateInfo standard_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		nullptr,
		0,
		static_cast<u32>(ArraySize(standard_sets)),
		standard_sets,
		0,
		nullptr };
	VkPipelineLayoutCreateInfo bbox_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		nullptr,
		0,
		static_cast<u32>(ArraySize(bbox_sets)),
		bbox_sets,
		0,
		nullptr };
	VkPipelineLayoutCreateInfo push_constant_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		nullptr,
		0,
		static_cast<u32>(ArraySize(standard_sets)),
		standard_sets,
		1,
		&push_constant_range };

	if ((res = vkCreatePipelineLayout(g_vulkan_context->GetDevice(), &standard_info, nullptr,
		&m_standard_pipeline_layout)) != VK_SUCCESS ||
		(res = vkCreatePipelineLayout(g_vulkan_context->GetDevice(), &bbox_info, nullptr,
			&m_bbox_pipeline_layout)) != VK_SUCCESS ||
			(res = vkCreatePipelineLayout(g_vulkan_context->GetDevice(), &push_constant_info, nullptr,
				&m_push_constant_pipeline_layout)))
	{
		LOG_VULKAN_ERROR(res, "vkCreatePipelineLayout failed: ");
		return false;
	}

	return true;
}

void ObjectCache::DestroyPipelineLayouts()
{
	if (m_standard_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), m_standard_pipeline_layout, nullptr);
	if (m_bbox_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), m_bbox_pipeline_layout, nullptr);
	if (m_push_constant_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), m_push_constant_pipeline_layout,
			nullptr);
}

bool ObjectCache::CreateUtilityShaderVertexFormat()
{
	PortableVertexDeclaration vtx_decl = {};
	vtx_decl.position.enable = true;
	vtx_decl.position.type = EVTXComponentFormat::FORMAT_FLOAT;
	vtx_decl.position.components = 4;
	vtx_decl.position.offset = offsetof(UtilityShaderVertex, Position);
	vtx_decl.texcoords[0].enable = true;
	vtx_decl.texcoords[0].type = EVTXComponentFormat::FORMAT_FLOAT;
	vtx_decl.texcoords[0].components = 4;
	vtx_decl.texcoords[0].offset = offsetof(UtilityShaderVertex, TexCoord);
	vtx_decl.colors[0].enable = true;
	vtx_decl.colors[0].type = EVTXComponentFormat::FORMAT_BYTE;
	vtx_decl.colors[0].components = 4;
	vtx_decl.colors[0].offset = offsetof(UtilityShaderVertex, Color);
	vtx_decl.stride = sizeof(UtilityShaderVertex);

	m_utility_shader_vertex_format = std::make_unique<VertexFormat>(vtx_decl);
	return true;
}

bool ObjectCache::CreateStaticSamplers()
{
	VkSamplerCreateInfo create_info = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,    // VkStructureType         sType
		nullptr,                                  // const void*             pNext
		0,                                        // VkSamplerCreateFlags    flags
		VK_FILTER_NEAREST,                        // VkFilter                magFilter
		VK_FILTER_NEAREST,                        // VkFilter                minFilter
		VK_SAMPLER_MIPMAP_MODE_NEAREST,           // VkSamplerMipmapMode     mipmapMode
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeU
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeV
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,    // VkSamplerAddressMode    addressModeW
		0.0f,                                     // float                   mipLodBias
		VK_FALSE,                                 // VkBool32                anisotropyEnable
		1.0f,                                     // float                   maxAnisotropy
		VK_FALSE,                                 // VkBool32                compareEnable
		VK_COMPARE_OP_ALWAYS,                     // VkCompareOp             compareOp
		std::numeric_limits<float>::min(),        // float                   minLod
		std::numeric_limits<float>::max(),        // float                   maxLod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,  // VkBorderColor           borderColor
		VK_FALSE                                  // VkBool32                unnormalizedCoordinates
	};

	VkResult res =
		vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &m_point_sampler);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
		return false;
	}

	// Most fields are shared across point<->linear samplers, so only change those necessary.
	create_info.minFilter = VK_FILTER_LINEAR;
	create_info.magFilter = VK_FILTER_LINEAR;
	create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	res = vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &m_linear_sampler);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
		return false;
	}

	return true;
}

VkSampler ObjectCache::GetSampler(const SamplerState& info)
{
	auto iter = m_sampler_cache.find(info);
	if (iter != m_sampler_cache.end())
		return iter->second;

	VkSamplerCreateInfo create_info = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,      // VkStructureType         sType
		nullptr,                                    // const void*             pNext
		0,                                          // VkSamplerCreateFlags    flags
		info.mag_filter,                            // VkFilter                magFilter
		info.min_filter,                            // VkFilter                minFilter
		info.mipmap_mode,                           // VkSamplerMipmapMode     mipmapMode
		info.wrap_u,                                // VkSamplerAddressMode    addressModeU
		info.wrap_v,                                // VkSamplerAddressMode    addressModeV
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,      // VkSamplerAddressMode    addressModeW
		static_cast<float>(info.lod_bias / 32.0f),  // float                   mipLodBias
		VK_FALSE,                                   // VkBool32                anisotropyEnable
		0.0f,                                       // float                   maxAnisotropy
		VK_FALSE,                                   // VkBool32                compareEnable
		VK_COMPARE_OP_ALWAYS,                       // VkCompareOp             compareOp
		static_cast<float>(info.min_lod / 16.0f),   // float                   minLod
		static_cast<float>(info.max_lod / 16.0f),   // float                   maxLod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,    // VkBorderColor           borderColor
		VK_FALSE                                    // VkBool32                unnormalizedCoordinates
	};

	// Can we use anisotropic filtering with this sampler?
	if (info.enable_anisotropic_filtering && g_vulkan_context->SupportsAnisotropicFiltering())
	{
		// Cap anisotropy to device limits.
		create_info.anisotropyEnable = VK_TRUE;
		create_info.maxAnisotropy = std::min(static_cast<float>(1 << g_ActiveConfig.iMaxAnisotropy),
			g_vulkan_context->GetMaxSamplerAnisotropy());
	}

	VkSampler sampler = VK_NULL_HANDLE;
	VkResult res = vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &sampler);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");

	// Store it even if it failed
	m_sampler_cache.emplace(info, sampler);
	return sampler;
}

std::string ObjectCache::GetUtilityShaderHeader() const
{
	std::stringstream ss;
	if (g_ActiveConfig.iMultisamples > 1)
	{
		ss << "#define MSAA_ENABLED 1" << std::endl;
		ss << "#define MSAA_SAMPLES " << g_ActiveConfig.iMultisamples << std::endl;
		if (g_ActiveConfig.bSSAA)
			ss << "#define SSAA_ENABLED 1" << std::endl;
	}

	u32 efb_layers = (g_ActiveConfig.iStereoMode != STEREO_OFF) ? 2 : 1;
	ss << "#define EFB_LAYERS " << efb_layers << std::endl;

	return ss.str();
}



bool ObjectCache::CompileSharedShaders()
{
	static const char PASSTHROUGH_VERTEX_SHADER_SOURCE[] = R"(
    layout(location = 0) in vec4 ipos;
    layout(location = 5) in vec4 icol0;
    layout(location = 8) in vec3 itex0;

    layout(location = 0) out vec3 uv0;
    layout(location = 1) out vec4 col0;

    void main()
    {
      gl_Position = ipos;
      uv0 = itex0;
      col0 = icol0;
    }
  )";

	static const char PASSTHROUGH_GEOMETRY_SHADER_SOURCE[] = R"(
    layout(triangles) in;
    layout(triangle_strip, max_vertices = EFB_LAYERS * 3) out;

    layout(location = 0) in vec3 in_uv0[];
    layout(location = 1) in vec4 in_col0[];

    layout(location = 0) out vec3 out_uv0;
    layout(location = 1) out vec4 out_col0;

    void main()
    {
      for (int j = 0; j < EFB_LAYERS; j++)
      {
        for (int i = 0; i < 3; i++)
        {
          gl_Layer = j;
          gl_Position = gl_in[i].gl_Position;
          out_uv0 = vec3(in_uv0[i].xy, float(j));
          out_col0 = in_col0[i];
          EmitVertex();
        }
        EndPrimitive();
      }
    }
  )";

	static const char SCREEN_QUAD_VERTEX_SHADER_SOURCE[] = R"(
    layout(location = 0) out vec3 uv0;

    void main()
    {
        /*
         * id   &1    &2   clamp(*2-1)
         * 0    0,0   0,0  -1,-1      TL
         * 1    1,0   1,0  1,-1       TR
         * 2    0,2   0,1  -1,1       BL
         * 3    1,2   1,1  1,1        BR
         */
        vec2 rawpos = vec2(float(gl_VertexID & 1), clamp(float(gl_VertexID & 2), 0.0f, 1.0f));
        gl_Position = vec4(rawpos * 2.0f - 1.0f, 0.0f, 1.0f);
        uv0 = vec3(rawpos, 0.0f);
    }
  )";

	static const char SCREEN_QUAD_GEOMETRY_SHADER_SOURCE[] = R"(
    layout(triangles) in;
    layout(triangle_strip, max_vertices = EFB_LAYERS * 3) out;

    layout(location = 0) in vec3 in_uv0[];

    layout(location = 0) out vec3 out_uv0;

    void main()
    {
      for (int j = 0; j < EFB_LAYERS; j++)
      {
        for (int i = 0; i < 3; i++)
        {
          gl_Layer = j;
          gl_Position = gl_in[i].gl_Position;
          out_uv0 = vec3(in_uv0[i].xy, float(j));
          EmitVertex();
        }
        EndPrimitive();
      }
    }
  )";

	std::string header = GetUtilityShaderHeader();

	m_screen_quad_vertex_shader =
		Util::CompileAndCreateVertexShader(header + SCREEN_QUAD_VERTEX_SHADER_SOURCE);
	m_passthrough_vertex_shader =
		Util::CompileAndCreateVertexShader(header + PASSTHROUGH_VERTEX_SHADER_SOURCE);
	if (m_screen_quad_vertex_shader == VK_NULL_HANDLE ||
		m_passthrough_vertex_shader == VK_NULL_HANDLE)
	{
		return false;
	}

	if (g_ActiveConfig.iStereoMode != STEREO_OFF && g_vulkan_context->SupportsGeometryShaders())
	{
		m_screen_quad_geometry_shader =
			Util::CompileAndCreateGeometryShader(header + SCREEN_QUAD_GEOMETRY_SHADER_SOURCE);
		m_passthrough_geometry_shader =
			Util::CompileAndCreateGeometryShader(header + PASSTHROUGH_GEOMETRY_SHADER_SOURCE);
		if (m_screen_quad_geometry_shader == VK_NULL_HANDLE ||
			m_passthrough_geometry_shader == VK_NULL_HANDLE)
		{
			return false;
		}
	}

	return true;
}

void ObjectCache::DestroySharedShaders()
{
	auto DestroyShader = [this](VkShaderModule& shader) {
		if (shader != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(g_vulkan_context->GetDevice(), shader, nullptr);
			shader = VK_NULL_HANDLE;
		}
	};

	DestroyShader(m_screen_quad_vertex_shader);
	DestroyShader(m_passthrough_vertex_shader);
	DestroyShader(m_screen_quad_geometry_shader);
	DestroyShader(m_passthrough_geometry_shader);
}
}
