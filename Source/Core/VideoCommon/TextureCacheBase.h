// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "Common/CommonTypes.h"
#include "Common/Thread.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VideoCommon.h"

struct VideoConfig;

enum TextureCacheParams
{
	// ugly
	TEXHASH_INVALID = 0,
	FRAMECOUNT_INVALID = 0,
	TEXTURE_KILL_MULTIPLIER = 2,
	TEXTURE_KILL_THRESHOLD = 120,
	TEXTURE_POOL_KILL_THRESHOLD = 3,
	TEXTURE_POOL_MEMORY_LIMIT = 64 * 1024 * 1024
};

class TextureCacheBase
{
public:
	struct TCacheEntryConfig
	{
		constexpr TCacheEntryConfig() = default;

		u32 GetSizeInBytes() const
		{
			u32 result = 0;
			switch (pcformat)
			{
			case PC_TEX_FMT_BGRA32:
			case PC_TEX_FMT_RGBA32:
				result = ((width + 3) & (~3)) * ((height + 3) & (~3)) * 4;
				break;
			case PC_TEX_FMT_IA4_AS_IA8:
			case PC_TEX_FMT_IA8:
			case PC_TEX_FMT_RGB565:
				result = ((width + 3) & (~3)) * ((height + 3) & (~3)) * 2;
				break;
			case PC_TEX_FMT_DXT1:
				result = ((width + 3) >> 2)*((height + 3) >> 2) * 8;
				break;
			case PC_TEX_FMT_DXT3:
			case PC_TEX_FMT_DXT5:
				result = ((width + 3) >> 2)*((height + 3) >> 2) * 16;
				break;
			default:
				break;
			}
			if (levels > 1 || rendertarget)
			{
				result += result * 2;
			}
			if (materialmap)
			{
				result *= 2;
			}
			result = std::max(result, 4096u);
			return result;
		}

		bool operator == (const TCacheEntryConfig& o) const
		{
			return std::tie(width, height, levels, layers, rendertarget, pcformat, materialmap) ==
				std::tie(o.width, o.height, o.levels, o.layers, o.rendertarget, o.pcformat, o.materialmap);
		}

		struct Hasher
		{
			size_t operator()(const TCacheEntryConfig& c) const
			{
				return (u64)c.materialmap << 57	// 1 bit
					| (u64)c.rendertarget << 56	// 1 bit
					| (u64)c.pcformat << 48		// 8 bits
					| (u64)c.layers << 40		// 8 bits 
					| (u64)c.levels << 32		// 8 bits 
					| (u64)c.height << 16		// 16 bits 
					| (u64)c.width;				// 16 bits
			}
		};

		u32 width = 0, height = 0, levels = 1, layers = 1;
		bool rendertarget = false;
		bool materialmap = false;
		PC_TexFormat pcformat = PC_TEX_FMT_NONE;
	};

	struct TCacheEntryBase
	{
		TCacheEntryConfig config;

		// common members
		bool is_efb_copy = false;
		bool is_custom_tex = false;
		bool is_scaled = false;
		bool emissive_in_alpha = false;
		u32 addr = {};
		u32 size_in_bytes = {};
		u32 native_size_in_bytes = {};
		u32 format = {}; // bits 0-3 will contain the in-memory format.
		u32 memory_stride = {};
		u32 native_width = {}, native_height = {}; // Texture dimensions from the GameCube's point of view
		u32 native_levels = {};
		// used to delete textures which haven't been used for TEXTURE_KILL_THRESHOLD frames
		s32 frameCount = {};
		u64 hash = {};
		u64 base_hash = {};

		// Keep an iterator to the entry in textures_by_hash, so it does not need to be searched when removing the cache entry
		std::multimap<u64, TCacheEntryBase*>::iterator textures_by_hash_iter;

		// This is used to keep track of both:
		//   * efb copies used by this partially updated texture
		//   * partially updated textures which refer to this efb copy
		std::unordered_set<TCacheEntryBase*> references;

		std::string basename;

		void SetGeneralParameters(u32 _addr, u32 _size, u32 _format)
		{
			addr = _addr;
			size_in_bytes = _size;
			format = _format;
		}

		void SetDimensions(u32 _native_width, u32 _native_height, u32 _native_levels)
		{
			native_width = _native_width;
			native_height = _native_height;
			native_levels = _native_levels;
			memory_stride = _native_width;
		}

		void SetHiresParams(bool _is_custom_tex, const std::string & _basename, bool _is_scaled, bool _emissive_in_alpha)
		{
			is_custom_tex = _is_custom_tex;
			basename = _basename;
			is_scaled = _is_scaled;
			emissive_in_alpha = _emissive_in_alpha;
		}

		void SetHashes(u64 _hash, u64 _base_hash)
		{
			hash = _hash;
			base_hash = _base_hash;
		}

		// This texture entry is used by the other entry as a sub-texture
		void CreateReference(TCacheEntryBase* other_entry)
		{
			// References are two-way, so they can easily be destroyed later
			this->references.emplace(other_entry);
			other_entry->references.emplace(this);
		}

		void DestroyAllReferences()
		{
			for (auto& reference : references)
				reference->references.erase(this);

			references.clear();
		}

		void SetEfbCopy(u32 stride);

		TCacheEntryBase(const TCacheEntryConfig& c) : config(c)
		{
			native_size_in_bytes = config.GetSizeInBytes();
		}

		virtual ~TCacheEntryBase();
		virtual uintptr_t GetInternalObject() = 0;
		virtual void Bind(u32 stage) = 0;
		virtual bool Save(const std::string& filename, u32 level) = 0;

		virtual void CopyRectangleFromTexture(
			const TCacheEntryBase* source,
			const MathUtil::Rectangle<int> &srcrect,
			const MathUtil::Rectangle<int> &dstrect) = 0;

		virtual void Load(const u8* src, u32 width, u32 height,
			u32 expanded_width, u32 level) = 0;
		virtual void LoadMaterialMap(const u8* src, u32 width, u32 height, u32 level) = 0;
		virtual void Load(const u8* src, u32 width, u32 height, u32 expandedWidth,
			u32 expandedHeight, const s32 texformat, const u32 tlutaddr, const TlutFormat tlutfmt, u32 level) = 0;
		virtual void LoadFromTmem(const u8* ar_src, const u8* gb_src, u32 width, u32 height,
			u32 expanded_width, u32 expanded_Height, u32 level) = 0;
		virtual void FromRenderTarget(u8* dst, PEControl::PixelFormat srcFormat, const EFBRectangle& srcRect,
			bool scaleByHalf, unsigned int cbufid, const float *colmat, u32 width, u32 height) = 0;
		bool OverlapsMemoryRange(u32 range_address, u32 range_size) const;
		virtual bool SupportsMaterialMap() const = 0;

		TextureCacheBase::TCacheEntryBase* ApplyPalette(u32 tlutaddr, u32 tlutfmt, u32 palette_size);

		bool IsEfbCopy() const
		{
			return is_efb_copy;
		}

		u32 NumBlocksY() const;
		u32 BytesPerRow() const;

		u64 CalculateHash() const;
	};

	virtual ~TextureCacheBase(); // needs virtual for DX11 dtor

	static void OnConfigChanged(VideoConfig& config);
	// Removes textures which aren't used for more than TEXTURE_KILL_THRESHOLD frames,
	// frameCount is the current frame number.
	static void Cleanup(int frameCount);
	static void Invalidate();

	virtual PC_TexFormat GetNativeTextureFormat(const s32 texformat,
		const TlutFormat tlutfmt, u32 width, u32 height) = 0;
	virtual TCacheEntryBase* CreateTexture(const TCacheEntryConfig& config) = 0;
	virtual bool Palettize(TCacheEntryBase* entry, const TCacheEntryBase* base_entry) = 0;
	virtual void CopyEFB(u8* dst, u32 format, u32 native_width, u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
		PEControl::PixelFormat srcFormat, const EFBRectangle& srcRect,
		bool isIntensity, bool scaleByHalf) = 0;

	virtual bool CompileShaders() = 0; // currently only implemented by OGL
	virtual void DeleteShaders() = 0; // currently only implemented by OGL

	virtual void LoadLut(u32 lutFmt, void* addr, u32 size) = 0;

	static TCacheEntryBase* Load(const u32 stage);
	static void UnbindTextures();
	virtual void BindTextures();
	static void CopyRenderTargetToTexture(u32 dstAddr, u32 dstFormat, u32 dstStride,
		PEControl::PixelFormat srcFormat, const EFBRectangle& srcRect, bool isIntensity, bool scaleByHalf);

protected:
	alignas(16) static u8 *temp;
	static size_t temp_size;
	static TCacheEntryBase* bound_textures[8];
	TextureCacheBase();
private:
	typedef std::multimap<u64, TCacheEntryBase*> TexCache;
	typedef std::unordered_multimap<TCacheEntryConfig, TCacheEntryBase*, TCacheEntryConfig::Hasher> TexPool;
	typedef std::unordered_map<std::string, TCacheEntryBase*> HiresTexPool;
	static void ScaleTextureCacheEntryTo(TCacheEntryBase** entry, u32 new_width, u32 new_height);
	static void CheckTempSize(size_t required_size);
	static TCacheEntryBase* DoPartialTextureUpdates(TexCache::iterator iter, u32 tlutaddr, u32 tlutfmt, u32 palette_size);
	static void DumpTexture(TCacheEntryBase* entry, std::string basename, u32 level);
	static TCacheEntryBase* AllocateTexture(const TCacheEntryConfig& config);
	static TexPool::iterator FindMatchingTextureFromPool(const TCacheEntryConfig& config);
	static TexCache::iterator GetTexCacheIter(TCacheEntryBase* entry);
	static TexCache::iterator InvalidateTexture(TexCache::iterator t_iter);
	static TCacheEntryBase* ReturnEntry(u32 stage, TCacheEntryBase* entry);



	static TexCache textures_by_address;
	static TexCache textures_by_hash;
	static TexPool texture_pool;
	static size_t texture_pool_memory_usage;
	
	static u32 s_last_texture;

	// Backup configuration values
	static struct BackupConfig
	{
		s32 s_colorsamples;
		bool s_texfmt_overlay;
		bool s_texfmt_overlay_center;
		bool s_hires_textures;
		bool s_cache_hires_textures;
		bool s_stereo_3d;
		bool s_efb_mono_depth;
		s32 s_scaling_mode;
		s32 s_scaling_factor;
		bool s_scaling_deposterize;
	} backup_config;
};

extern std::unique_ptr<TextureCacheBase> g_texture_cache;