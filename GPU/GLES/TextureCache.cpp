// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <map>
#include <algorithm>

#include "Core/MemMap.h"
#include "Core/Reporting.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "GPU/GLES/TextureCache.h"
#include "GPU/GLES/Framebuffer.h"
#include "Core/Config.h"

#include "ext/xxhash.h"
#include "native/ext/cityhash/city.h"

#ifdef _M_SSE
#include <xmmintrin.h>
#endif

// If a texture hasn't been seen for this many frames, get rid of it.
#define TEXTURE_KILL_AGE 200
#define TEXTURE_KILL_AGE_LOWMEM 60
// Not used in lowmem mode.
#define TEXTURE_SECOND_KILL_AGE 100

// Try to be prime to other decimation intervals.
#define TEXCACHE_DECIMATION_INTERVAL 13

extern int g_iNumVideos;

u32 RoundUpToPowerOf2(u32 v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}

static inline u32 GetLevelBufw(int level, u32 texaddr) {
	// Special rules for kernel textures (PPGe):
	if (texaddr < PSP_GetUserMemoryBase())
		return gstate.texbufwidth[level] & 0x1FFF;
	return gstate.texbufwidth[level] & 0x7FF;
}

TextureCache::TextureCache() : clearCacheNextFrame_(false), lowMemoryMode_(false), clutBuf_(NULL) {
	lastBoundTexture = -1;
	decimationCounter_ = TEXCACHE_DECIMATION_INTERVAL;
	// This is 5MB of temporary storage. Might be possible to shrink it.
	tmpTexBuf32.resize(1024 * 512);  // 2MB
	tmpTexBuf16.resize(1024 * 512);  // 1MB
	tmpTexBufRearrange.resize(1024 * 512);   // 2MB
	clutBufConverted_ = new u32[4096];  // 16KB
	clutBufRaw_ = new u32[4096];  // 16KB
	glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropyLevel);
}

TextureCache::~TextureCache() {
	delete [] clutBufConverted_;
	delete [] clutBufRaw_;
}

void TextureCache::Clear(bool delete_them) {
	glBindTexture(GL_TEXTURE_2D, 0);
	lastBoundTexture = -1;
	if (delete_them) {
		for (TexCache::iterator iter = cache.begin(); iter != cache.end(); ++iter) {
			DEBUG_LOG(G3D, "Deleting texture %i", iter->second.texture);
			glDeleteTextures(1, &iter->second.texture);
		}
		for (TexCache::iterator iter = secondCache.begin(); iter != secondCache.end(); ++iter) {
			DEBUG_LOG(G3D, "Deleting texture %i", iter->second.texture);
			glDeleteTextures(1, &iter->second.texture);
		}
	}
	if (cache.size() + secondCache.size()) {
		INFO_LOG(G3D, "Texture cached cleared from %i textures", (int)(cache.size() + secondCache.size()));
		cache.clear();
		secondCache.clear();
	}
}

// Removes old textures.
void TextureCache::Decimate() {
	if (--decimationCounter_ <= 0) {
		decimationCounter_ = TEXCACHE_DECIMATION_INTERVAL;
	} else {
		return;
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	lastBoundTexture = -1;
	int killAge = lowMemoryMode_ ? TEXTURE_KILL_AGE_LOWMEM : TEXTURE_KILL_AGE;
	for (TexCache::iterator iter = cache.begin(); iter != cache.end(); ) {
		if (iter->second.lastFrame + TEXTURE_KILL_AGE < gpuStats.numFlips) {
			glDeleteTextures(1, &iter->second.texture);
			cache.erase(iter++);
		}
		else
			++iter;
	}
	for (TexCache::iterator iter = secondCache.begin(); iter != secondCache.end(); ) {
		if (lowMemoryMode_ || iter->second.lastFrame + TEXTURE_KILL_AGE < gpuStats.numFlips) {
			glDeleteTextures(1, &iter->second.texture);
			secondCache.erase(iter++);
		}
		else
			++iter;
	}
}

void TextureCache::Invalidate(u32 addr, int size, GPUInvalidationType type) {
	addr &= 0x0FFFFFFF;
	u32 addr_end = addr + size;

	// They could invalidate inside the texture, let's just give a bit of leeway.
	const int LARGEST_TEXTURE_SIZE = 512 * 512 * 4;
	u64 startKey = addr - LARGEST_TEXTURE_SIZE;
	u64 endKey = addr + size + LARGEST_TEXTURE_SIZE;
	for (TexCache::iterator iter = cache.lower_bound(startKey), end = cache.upper_bound(endKey); iter != end; ++iter) {
		u32 texAddr = iter->second.addr;
		u32 texEnd = iter->second.addr + iter->second.sizeInRAM;

		if (texAddr < addr_end && addr < texEnd) {
			if ((iter->second.status & TexCacheEntry::STATUS_MASK) == TexCacheEntry::STATUS_RELIABLE) {
				// Clear status -> STATUS_HASHING.
				iter->second.status &= ~TexCacheEntry::STATUS_MASK;
			}
			if (type != GPU_INVALIDATE_ALL) {
				gpuStats.numTextureInvalidations++;
				// Start it over from 0 (unless it's safe.)
				iter->second.numFrames = type == GPU_INVALIDATE_SAFE ? 256 : 0;
				iter->second.framesUntilNextFullHash = 0;
			} else {
				iter->second.invalidHint++;
			}
		}
	}
}

void TextureCache::InvalidateAll(GPUInvalidationType /*unused*/) {
	for (TexCache::iterator iter = cache.begin(), end = cache.end(); iter != end; ++iter) {
		if ((iter->second.status & TexCacheEntry::STATUS_MASK) == TexCacheEntry::STATUS_RELIABLE) {
			// Clear status -> STATUS_HASHING.
			iter->second.status &= ~TexCacheEntry::STATUS_MASK;
		}
		iter->second.invalidHint++;
	}
}

void TextureCache::ClearNextFrame() {
	clearCacheNextFrame_ = true;
}


template <typename T>
inline void AttachFramebufferValid(T &entry, VirtualFramebuffer *framebuffer) {
	const bool hasInvalidFramebuffer = entry->framebuffer == 0 || entry->invalidHint == -1;
	const bool hasOlderFramebuffer = entry->framebuffer != 0 && entry->framebuffer->last_frame_render < framebuffer->last_frame_render;
	if (hasInvalidFramebuffer || hasOlderFramebuffer) {
		entry->framebuffer = framebuffer;
		entry->invalidHint = 0;
	}
}

template <typename T>
inline void AttachFramebufferInvalid(T &entry, VirtualFramebuffer *framebuffer) {
	if (entry->framebuffer == 0 || entry->framebuffer == framebuffer) {
		entry->framebuffer = framebuffer;
		entry->invalidHint = -1;
	}
}

inline void TextureCache::AttachFramebuffer(TexCacheEntry *entry, u32 address, VirtualFramebuffer *framebuffer, bool exactMatch) {
	// If they match exactly, it's non-CLUT and from the top left.
	if (exactMatch) {
		DEBUG_LOG(HLE, "Render to texture detected at %08x!", address);
		if (!entry->framebuffer) {
			if (entry->format != framebuffer->format) {
				WARN_LOG_REPORT_ONCE(diffFormat1, HLE, "Render to texture with different formats %d != %d", entry->format, framebuffer->format);
				// If it already has one, let's hope that one is correct.
				// Affected game list : Kurohyou 2, Evangelion Jo
				AttachFramebufferInvalid(entry, framebuffer);
			} else {
				AttachFramebufferValid(entry, framebuffer);
			}
			// TODO: Delete the original non-fbo texture too.
		}
	} else if (g_Config.iRenderingMode == FB_NON_BUFFERED_MODE || g_Config.iRenderingMode == FB_BUFFERED_MODE) {
		// 3rd Birthday (and possibly other games) render to a 16 bit clut texture.
		const bool compatFormat = framebuffer->format == entry->format
			|| (framebuffer->format == GE_FORMAT_8888 && entry->format == GE_TFMT_CLUT32)
			|| (framebuffer->format != GE_FORMAT_8888 && entry->format == GE_TFMT_CLUT16);

		// Is it at least the right stride?
		if (framebuffer->fb_stride == entry->bufw && compatFormat) {
			if (framebuffer->format != entry->format) {
				WARN_LOG_REPORT_ONCE(diffFormat2, HLE, "Render to texture with different formats %d != %d at %08x", entry->format, framebuffer->format, address);
				// TODO: Use an FBO to translate the palette?
				// Affected game List : DBZ VS Tag , 3rd birthday 
				AttachFramebufferValid(entry, framebuffer);
			} else if ((entry->addr - address) / entry->bufw < framebuffer->height) {
				WARN_LOG_REPORT_ONCE(subarea, HLE, "Render to area containing texture at %08x", address);
				// TODO: Keep track of the y offset.
				// Affected game List : God of War Ghost of Sparta , Tactic Orge , Sword Art Online
				AttachFramebufferValid(entry, framebuffer);
			}
		}
	}
}

inline void TextureCache::DetachFramebuffer(TexCacheEntry *entry, u32 address, VirtualFramebuffer *framebuffer) {
	if (entry->framebuffer == framebuffer) {
		entry->framebuffer = 0;
	}
}

void TextureCache::NotifyFramebuffer(u32 address, VirtualFramebuffer *framebuffer, FramebufferNotification msg) {
	// This is a rough heuristic, because sometimes our framebuffers are too tall.
	static const u32 MAX_SUBAREA_Y_OFFSET = 32;

	// Must be in VRAM so | 0x04000000 it is.
	const u64 cacheKey = (u64)(address | 0x04000000) << 32;
	// If it has a clut, those are the low 32 bits, so it'll be inside this range.
	// Also, if it's a subsample of the buffer, it'll also be within the FBO.
	const u64 cacheKeyEnd = cacheKey + ((u64)(framebuffer->fb_stride * MAX_SUBAREA_Y_OFFSET) << 32);

	switch (msg) {
	case NOTIFY_FB_CREATED:
	case NOTIFY_FB_UPDATED:
		// Ensure it's in the framebuffer cache.
		if (std::find(fbCache_.begin(), fbCache_.end(), framebuffer) == fbCache_.end()) {
			fbCache_.push_back(framebuffer);
		}
		for (auto it = cache.lower_bound(cacheKey), end = cache.upper_bound(cacheKeyEnd); it != end; ++it) {
			AttachFramebuffer(&it->second, address | 0x04000000, framebuffer, it->first == cacheKey);
		}
		break;

	case NOTIFY_FB_DESTROYED:
		fbCache_.erase(std::remove(fbCache_.begin(), fbCache_.end(),  framebuffer), fbCache_.end());
		for (auto it = cache.lower_bound(cacheKey), end = cache.upper_bound(cacheKeyEnd); it != end; ++it) {
			DetachFramebuffer(&it->second, address | 0x04000000, framebuffer);
		}
		break;
	}
}

void *TextureCache::UnswizzleFromMem(u32 texaddr, u32 bufw, u32 bytesPerPixel, u32 level) {
	const u32 rowWidth = (bytesPerPixel > 0) ? (bufw * bytesPerPixel) : (bufw / 2);
	const u32 pitch = rowWidth / 4;
	const int bxc = rowWidth / 16;
	int byc = (gstate.getTextureHeight(level) + 7) / 8;
	if (byc == 0)
		byc = 1;

	u32 ydest = 0;
	if (rowWidth >= 16) {
		const u32 *src = (u32 *) Memory::GetPointer(texaddr);
		u32 *ydestp = tmpTexBuf32.data();
		for (int by = 0; by < byc; by++) {
			u32 *xdest = ydestp;
			for (int bx = 0; bx < bxc; bx++) {
				u32 *dest = xdest;
				for (int n = 0; n < 8; n++) {
					memcpy(dest, src, 16);
					dest += pitch;
					src += 4;
				}
				xdest += 4;
			}
			ydestp += (rowWidth * 8) / 4;
		}
	} else if (rowWidth == 8) {
		const u32 *src = (u32 *) Memory::GetPointer(texaddr);
		for (int by = 0; by < byc; by++) {
			for (int n = 0; n < 8; n++, ydest += 2) {
				tmpTexBuf32[ydest + 0] = *src++;
				tmpTexBuf32[ydest + 1] = *src++;
				src += 2; // skip two u32
			}
		}
	} else if (rowWidth == 4) {
		const u32 *src = (u32 *) Memory::GetPointer(texaddr);
		for (int by = 0; by < byc; by++) {
			for (int n = 0; n < 8; n++, ydest++) {
				tmpTexBuf32[ydest] = *src++;
				src += 3;
			}
		}
	} else if (rowWidth == 2) {
		const u16 *src = (u16 *) Memory::GetPointer(texaddr);
		for (int by = 0; by < byc; by++) {
			for (int n = 0; n < 4; n++, ydest++) {
				u16 n1 = src[0];
				u16 n2 = src[8];
				tmpTexBuf32[ydest] = (u32)n1 | ((u32)n2 << 16);
				src += 16;
			}
		}
	} else if (rowWidth == 1) {
		const u8 *src = (u8 *) Memory::GetPointer(texaddr);
		for (int by = 0; by < byc; by++) {
			for (int n = 0; n < 2; n++, ydest++) {
				u8 n1 = src[ 0];
				u8 n2 = src[16];
				u8 n3 = src[32];
				u8 n4 = src[48];
				tmpTexBuf32[ydest] = (u32)n1 | ((u32)n2 << 8) | ((u32)n3 << 16) | ((u32)n4 << 24);
				src += 64;
			}
		}
	}
	return tmpTexBuf32.data();
}

template <typename IndexT, typename ClutT>
inline void DeIndexTexture(ClutT *dest, const IndexT *indexed, int length, const ClutT *clut) {
	// Usually, there is no special offset, mask, or shift.
	const bool nakedIndex = gstate.isClutIndexSimple();

	if (nakedIndex) {
		if (sizeof(IndexT) == 1) {
			for (int i = 0; i < length; ++i) {
				*dest++ = clut[*indexed++];
			}
		} else {
			for (int i = 0; i < length; ++i) {
				*dest++ = clut[(*indexed++) & 0xFF];
			}
		}
	} else {
		for (int i = 0; i < length; ++i) {
			*dest++ = clut[gstate.transformClutIndex(*indexed++)];
		}
	}
}

template <typename IndexT, typename ClutT>
inline void DeIndexTexture(ClutT *dest, const u32 texaddr, int length, const ClutT *clut) {
	const IndexT *indexed = (const IndexT *) Memory::GetPointer(texaddr);
	DeIndexTexture(dest, indexed, length, clut);
}

template <typename ClutT>
inline void DeIndexTexture4(ClutT *dest, const u8 *indexed, int length, const ClutT *clut) {
	// Usually, there is no special offset, mask, or shift.
	const bool nakedIndex = gstate.isClutIndexSimple();

	if (nakedIndex) {
		for (int i = 0; i < length; i += 2) {
			u8 index = *indexed++;
			dest[i + 0] = clut[(index >> 0) & 0xf];
			dest[i + 1] = clut[(index >> 4) & 0xf];
		}
	} else {
		for (int i = 0; i < length; i += 2) {
			u8 index = *indexed++;
			dest[i + 0] = clut[gstate.transformClutIndex((index >> 0) & 0xf)];
			dest[i + 1] = clut[gstate.transformClutIndex((index >> 4) & 0xf)];
		}
	}
}

template <typename ClutT>
inline void DeIndexTexture4Optimal(ClutT *dest, const u8 *indexed, int length, ClutT color) {
	for (int i = 0; i < length; i += 2) {
		u8 index = *indexed++;
		dest[i + 0] = color | ((index >> 0) & 0xf);
		dest[i + 1] = color | ((index >> 4) & 0xf);
	}
}

template <>
inline void DeIndexTexture4Optimal<u16>(u16 *dest, const u8 *indexed, int length, u16 color) {
	const u16 *indexed16 = (const u16 *)indexed;
	const u32 color32 = (color << 16) | color;
	u32 *dest32 = (u32 *)dest;
	for (int i = 0; i < length / 2; i += 2) {
		u16 index = *indexed16++;
		dest32[i + 0] = color32 | ((index & 0x00f0) << 12) | ((index & 0x000f) >> 0);
		dest32[i + 1] = color32 | ((index & 0xf000) <<  4) | ((index & 0x0f00) >> 8);
	}
}

template <typename ClutT>
inline void DeIndexTexture4(ClutT *dest, const u32 texaddr, int length, const ClutT *clut) {
	const u8 *indexed = (const u8 *) Memory::GetPointer(texaddr);
	DeIndexTexture4(dest, indexed, length, clut);
}

template <typename ClutT>
inline void DeIndexTexture4Optimal(ClutT *dest, const u32 texaddr, int length, ClutT color) {
	const u8 *indexed = (const u8 *) Memory::GetPointer(texaddr);
	DeIndexTexture4Optimal(dest, indexed, length, color);
}

void *TextureCache::readIndexedTex(int level, u32 texaddr, int bytesPerIndex, GLuint dstFmt) {
	int bufw = GetLevelBufw(level, texaddr);
	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);
	int length = bufw * h;
	void *buf = NULL;
	switch (gstate.getClutPaletteFormat()) {
	case GE_CMODE_16BIT_BGR5650:
	case GE_CMODE_16BIT_ABGR5551:
	case GE_CMODE_16BIT_ABGR4444:
		{
		tmpTexBuf16.resize(std::max(bufw, w) * h);
		tmpTexBufRearrange.resize(std::max(bufw, w) * h);
		const u16 *clut = GetCurrentClut<u16>();
		if (!gstate.isTextureSwizzled()) {
			switch (bytesPerIndex) {
			case 1:
				DeIndexTexture<u8>(tmpTexBuf16.data(), texaddr, length, clut);
				break;

			case 2:
				DeIndexTexture<u16>(tmpTexBuf16.data(), texaddr, length, clut);
				break;

			case 4:
				DeIndexTexture<u32>(tmpTexBuf16.data(), texaddr, length, clut);
				break;
			}
		} else {
			tmpTexBuf32.resize(std::max(bufw, w) * h);
			UnswizzleFromMem(texaddr, bufw, bytesPerIndex, level);
			switch (bytesPerIndex) {
			case 1:
				DeIndexTexture(tmpTexBuf16.data(), (u8 *) tmpTexBuf32.data(), length, clut);
				break;

			case 2:
				DeIndexTexture(tmpTexBuf16.data(), (u16 *) tmpTexBuf32.data(), length, clut);
				break;

			case 4:
				DeIndexTexture(tmpTexBuf16.data(), (u32 *) tmpTexBuf32.data(), length, clut);
				break;
			}
		}
		buf = tmpTexBuf16.data();
		}
		break;

	case GE_CMODE_32BIT_ABGR8888:
		{
		tmpTexBuf32.resize(std::max(bufw, w) * h);
		tmpTexBufRearrange.resize(std::max(bufw, w) * h);
		const u32 *clut = GetCurrentClut<u32>();
		if (!gstate.isTextureSwizzled()) {
			switch (bytesPerIndex) {
			case 1:
				DeIndexTexture<u8>(tmpTexBuf32.data(), texaddr, length, clut);
				break;

			case 2:
				DeIndexTexture<u16>(tmpTexBuf32.data(), texaddr, length, clut);
				break;

			case 4:
				DeIndexTexture<u32>(tmpTexBuf32.data(), texaddr, length, clut);
				break;
			}
			buf = tmpTexBuf32.data();
		} else {
			UnswizzleFromMem(texaddr, bufw, bytesPerIndex, level);
			// Since we had to unswizzle to tmpTexBuf32, let's output to tmpTexBuf16.
			tmpTexBuf16.resize(std::max(bufw, w) * h * 2);
			u32 *dest32 = (u32 *) tmpTexBuf16.data();
			switch (bytesPerIndex) {
			case 1:
				DeIndexTexture(dest32, (u8 *) tmpTexBuf32.data(), length, clut);
				buf = dest32;
				break;

			case 2:
				DeIndexTexture(dest32, (u16 *) tmpTexBuf32.data(), length, clut);
				buf = dest32;
				break;

			case 4:
				// TODO: If a game actually uses this mode, check if using dest32 or tmpTexBuf32 is faster.
				DeIndexTexture(tmpTexBuf32.data(), tmpTexBuf32.data(), length, clut);
				buf = tmpTexBuf32.data();
				break;
			}
		}
		}
		break;

	default:
		ERROR_LOG(G3D, "Unhandled clut texture mode %d!!!", (gstate.clutformat & 3));
		break;
	}

	return buf;
}

GLenum getClutDestFormat(GEPaletteFormat format) {
	switch (format) {
	case GE_CMODE_16BIT_ABGR4444:
		return GL_UNSIGNED_SHORT_4_4_4_4;
	case GE_CMODE_16BIT_ABGR5551:
		return GL_UNSIGNED_SHORT_5_5_5_1;
	case GE_CMODE_16BIT_BGR5650:
		return GL_UNSIGNED_SHORT_5_6_5;
	case GE_CMODE_32BIT_ABGR8888:
		return GL_UNSIGNED_BYTE;
	}
	return 0;
}

static const u8 texByteAlignMap[] = {2, 2, 2, 4};

static const GLuint MinFiltGL[8] = {
	GL_NEAREST,
	GL_LINEAR,
	GL_NEAREST,
	GL_LINEAR,
	GL_NEAREST_MIPMAP_NEAREST,
	GL_LINEAR_MIPMAP_NEAREST,
	GL_NEAREST_MIPMAP_LINEAR,
	GL_LINEAR_MIPMAP_LINEAR,
};

static const GLuint MagFiltGL[2] = {
	GL_NEAREST,
	GL_LINEAR
};

// This should not have to be done per texture! OpenGL is silly yo
// TODO: Dirty-check this against the current texture.
void TextureCache::UpdateSamplingParams(TexCacheEntry &entry, bool force) {
	int minFilt = gstate.texfilter & 0x7;
	int magFilt = (gstate.texfilter>>8) & 1;
	bool sClamp = gstate.isTexCoordClampedS();
	bool tClamp = gstate.isTexCoordClampedT();

	bool noMip = (gstate.texlevel & 0xFFFFFF) == 0x000001 || (gstate.texlevel & 0xFFFFFF) == 0x100001 ;  // Fix texlevel at 0

	if (entry.maxLevel == 0) {
		// Enforce no mip filtering, for safety.
		minFilt &= 1; // no mipmaps yet
	} else {
		// TODO: Is this a signed value? Which direction?
		float lodBias = 0.0; // -(float)((gstate.texlevel >> 16) & 0xFF) / 16.0f;
		if (force || entry.lodBias != lodBias) {
#ifndef USING_GLES2
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, lodBias);
#endif
			entry.lodBias = lodBias;
		}
	}

	if ((g_Config.iTexFiltering == LINEAR || (g_Config.iTexFiltering == LINEARFMV && g_iNumVideos)) && !gstate.isColorTestEnabled()) {
		magFilt |= 1;
		minFilt |= 1;
	}

	if (g_Config.iTexFiltering == NEAREST) {
		magFilt &= ~1;
		minFilt &= ~1;
	}

	if (!g_Config.bMipMap || noMip) {
		magFilt &= 1;
		minFilt &= 1;
	}

	if (force || entry.minFilt != minFilt) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, MinFiltGL[minFilt]);
		entry.minFilt = minFilt;
	}
	if (force || entry.magFilt != magFilt) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, MagFiltGL[magFilt]);
		entry.magFilt = magFilt;
	}
	if (force || entry.sClamp != sClamp) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sClamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		entry.sClamp = sClamp;
	}
	if (force || entry.tClamp != tClamp) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, tClamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		entry.tClamp = tClamp;
	}
}

// All these DXT structs are in the reverse order, as compared to PC.
// On PC, alpha comes before color, and interpolants are before the tile data.

struct DXT1Block {
	u8 lines[4];
	u16 color1;
	u16 color2;
};

struct DXT3Block {
	DXT1Block color;
	u16 alphaLines[4];
};

struct DXT5Block {
	DXT1Block color;
	u32 alphadata2;
	u16 alphadata1;
	u8 alpha1; u8 alpha2;
};

static inline u32 makecol(int r, int g, int b, int a) {
	return (a << 24) | (r << 16) | (g << 8) | b;
}

// This could probably be done faster by decoding two or four blocks at a time with SSE/NEON.
static void decodeDXT1Block(u32 *dst, const DXT1Block *src, int pitch, bool ignore1bitAlpha = false) {
	// S3TC Decoder
	// Needs more speed and debugging.
	u16 c1 = (src->color1);
	u16 c2 = (src->color2);
	int red1 = Convert5To8(c1 & 0x1F);
	int red2 = Convert5To8(c2 & 0x1F);
	int green1 = Convert6To8((c1 >> 5) & 0x3F);
	int green2 = Convert6To8((c2 >> 5) & 0x3F);
	int blue1 = Convert5To8((c1 >> 11) & 0x1F);
	int blue2 = Convert5To8((c2 >> 11) & 0x1F);

	u32 colors[4];
	colors[0] = makecol(red1, green1, blue1, 255);
	colors[1] = makecol(red2, green2, blue2, 255);
	if (c1 > c2 || ignore1bitAlpha) {
		int blue3 = ((blue2 - blue1) >> 1) - ((blue2 - blue1) >> 3);
		int green3 = ((green2 - green1) >> 1) - ((green2 - green1) >> 3);
		int red3 = ((red2 - red1) >> 1) - ((red2 - red1) >> 3);				
		colors[2] = makecol(red1 + red3, green1 + green3, blue1 + blue3, 255);
		colors[3] = makecol(red2 - red3, green2 - green3, blue2 - blue3, 255);
	} else {
		colors[2] = makecol((red1 + red2 + 1) / 2, // Average
			(green1 + green2 + 1) / 2,
			(blue1 + blue2 + 1) / 2, 255);
		colors[3] = makecol(red2, green2, blue2, 0);	// Color2 but transparent
	}

	for (int y = 0; y < 4; y++) {
		int val = src->lines[y];
		for (int x = 0; x < 4; x++) {
			dst[x] = colors[val & 3];
			val >>= 2;
		}
		dst += pitch;
	}
}

static void decodeDXT3Block(u32 *dst, const DXT3Block *src, int pitch)
{
	decodeDXT1Block(dst, &src->color, pitch, true);

	for (int y = 0; y < 4; y++) {
		u32 line = src->alphaLines[y];
		for (int x = 0; x < 4; x++) {
			const u8 a4 = line & 0xF;
			dst[x] = (dst[x] & 0xFFFFFF) | (a4 << 24) | (a4 << 28);
			line >>= 4;
		}
		dst += pitch;
	}
}

static inline u8 lerp8(const DXT5Block *src, int n) {
	float d = n / 7.0f;
	return (u8)(src->alpha1 + (src->alpha2 - src->alpha1) * d);
}

static inline u8 lerp6(const DXT5Block *src, int n) {
	float d = n / 5.0f;
	return (u8)(src->alpha1 + (src->alpha2 - src->alpha1) * d);
}

// The alpha channel is not 100% correct 
static void decodeDXT5Block(u32 *dst, const DXT5Block *src, int pitch) {
	decodeDXT1Block(dst, &src->color, pitch, true);
	u8 alpha[8];

	alpha[0] = src->alpha1;
	alpha[1] = src->alpha2;
	if (alpha[0] > alpha[1]) {
		alpha[2] = lerp8(src, 1);
		alpha[3] = lerp8(src, 2);
		alpha[4] = lerp8(src, 3);
		alpha[5] = lerp8(src, 4);
		alpha[6] = lerp8(src, 5);
		alpha[7] = lerp8(src, 6);
	} else {
		alpha[2] = lerp6(src, 1);
		alpha[3] = lerp6(src, 2);
		alpha[4] = lerp6(src, 3);
		alpha[5] = lerp6(src, 4);
		alpha[6] = 0;
		alpha[7] = 255;
	}

	u64 data = ((u64)src->alphadata1 << 32) | src->alphadata2;

	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			dst[x] = (dst[x] & 0xFFFFFF) | (alpha[data & 7] << 24);
			data >>= 3;
		}
		dst += pitch;
	}
}

static void ConvertColors(void *dstBuf, const void *srcBuf, GLuint dstFmt, int numPixels) {
	const u32 *src = (const u32 *)srcBuf;
	u32 *dst = (u32 *)dstBuf;
	// TODO: All these can be further sped up with SSE or NEON.
	switch (dstFmt) {
	case GL_UNSIGNED_SHORT_4_4_4_4:
		{
			for (int i = 0; i < (numPixels + 1) / 2; i++) {
				u32 c = src[i];
				dst[i] = ((c >> 12) & 0x000F000F) |
				       ((c >> 4)  & 0x00F000F0) |
				       ((c << 4)  & 0x0F000F00) |
				       ((c << 12) & 0xF000F000);
			}
		}
		break;
	case GL_UNSIGNED_SHORT_5_5_5_1:
		{
			for (int i = 0; i < (numPixels + 1) / 2; i++) {
				u32 c = src[i];
				dst[i] = ((c >> 15) & 0x00010001) |
				       ((c >> 9)  & 0x003E003E) |
				       ((c << 1)  & 0x07C007C0) |
				       ((c << 11) & 0xF800F800);
			}
		}
		break;
	case GL_UNSIGNED_SHORT_5_6_5:
		{
			for (int i = 0; i < (numPixels + 1) / 2; i++) {
				u32 c = src[i];
				dst[i] = ((c >> 11) & 0x001F001F) |
				       ((c >> 0)  & 0x07E007E0) |
				       ((c << 11) & 0xF800F800);
			}
		}
		break;
	default:
		{
			// No need to convert RGBA8888, right order already
			if (dst != src)
				memcpy(dst, src, numPixels * sizeof(u32));
		}
		break;
	}
}

void TextureCache::StartFrame() {
	lastBoundTexture = -1;
	if(clearCacheNextFrame_) {
		Clear(true);
		clearCacheNextFrame_ = false;
	} else {
		Decimate();
	}
}

static const u8 bitsPerPixel[16] = {
	16,  //GE_TFMT_5650,
	16,  //GE_TFMT_5551,
	16,  //GE_TFMT_4444,
	32,  //GE_TFMT_8888,
	4,   //GE_TFMT_CLUT4,
	8,   //GE_TFMT_CLUT8,
	16,  //GE_TFMT_CLUT16,
	32,  //GE_TFMT_CLUT32,
	4,   //GE_TFMT_DXT1,
	8,   //GE_TFMT_DXT3,
	8,   //GE_TFMT_DXT5,
	0,   // INVALID,
	0,   // INVALID,
	0,   // INVALID,
	0,   // INVALID,
	0,   // INVALID,
};

static inline u32 MiniHash(const u32 *ptr) {
	return ptr[0];
}

static inline u32 QuickClutHash(const u8 *clut, u32 bytes) {
	// CLUTs always come in multiples of 32 bytes, can't load them any other way.
	_dbg_assert_msg_(G3D, (bytes & 31) == 0, "CLUT should always have a multiple of 32 bytes.");

	const u32 prime = 2246822519U;
	u32 hash = 0;
#ifdef _M_SSE
	if ((((u32)(intptr_t)clut) & 0xf) == 0) {
		__m128i cursor = _mm_set1_epi32(0);
		const __m128i mult = _mm_set1_epi32(prime);
		const __m128i *p = (const __m128i *)clut;
		for (u32 i = 0; i < bytes / 16; ++i) {
			cursor = _mm_add_epi32(cursor, _mm_mul_epu32(_mm_load_si128(&p[i]), mult));
		}
		// Add the four parts into the low i32.
		cursor = _mm_add_epi32(cursor, _mm_srli_si128(cursor, 8));
		cursor = _mm_add_epi32(cursor, _mm_srli_si128(cursor, 4));
		hash = _mm_cvtsi128_si32(cursor);
	} else {
#else
	// TODO: ARM NEON implementation (using CPUDetect to be sure it has NEON.)
	{
#endif
		for (const u32 *p = (u32 *)clut, *end = (u32 *)(clut + bytes); p < end; ) {
			hash += *p++ * prime;
		}
	}

	return hash;
}

static inline u32 QuickTexHash(u32 addr, int bufw, int w, int h, GETextureFormat format) {
	const u32 sizeInRAM = (bitsPerPixel[format] * bufw * h) / 8;
	const u32 *checkp = (const u32 *) Memory::GetPointer(addr);
	u32 check = 0;

#ifdef _M_SSE
	// Make sure both the size and start are aligned, OR will get either.
	if ((((u32)(intptr_t)checkp | sizeInRAM) & 0x1f) == 0) {
		__m128i cursor = _mm_set1_epi32(0);
		const __m128i *p = (const __m128i *)checkp;
		for (u32 i = 0; i < sizeInRAM / 16; i += 2) {
			cursor = _mm_add_epi32(cursor, _mm_load_si128(&p[i]));
			cursor = _mm_xor_si128(cursor, _mm_load_si128(&p[i + 1]));
		}
		// Add the four parts into the low i32.
		cursor = _mm_add_epi32(cursor, _mm_srli_si128(cursor, 8));
		cursor = _mm_add_epi32(cursor, _mm_srli_si128(cursor, 4));
		check = _mm_cvtsi128_si32(cursor);
	} else {
#else
	// TODO: ARM NEON implementation (using CPUDetect to be sure it has NEON.)
	{
#endif
		for (u32 i = 0; i < sizeInRAM / 8; ++i) {
			check += *checkp++;
			check ^= *checkp++;
		}
	}

	return check;
}

inline bool TextureCache::TexCacheEntry::Matches(u16 dim2, u8 format2, int maxLevel2) {
	return dim == dim2 && format == format2 && maxLevel == maxLevel2;
}

void TextureCache::LoadClut() {
	u32 clutAddr = gstate.getClutAddress();
	clutTotalBytes_ = gstate.getClutLoadBytes();
	if (Memory::IsValidAddress(clutAddr)) {
		Memory::MemcpyUnchecked(clutBufRaw_, clutAddr, clutTotalBytes_);
	} else {
		memset(clutBufRaw_, 0xFF, clutTotalBytes_);
	}
	// Reload the clut next time.
	clutLastFormat_ = 0xFFFFFFFF;
}

void TextureCache::UpdateCurrentClut() {
	const GEPaletteFormat clutFormat = gstate.getClutPaletteFormat();
	const u32 clutBase = gstate.getClutIndexStartPos();
	const u32 clutBaseBytes = clutBase * (clutFormat == GE_CMODE_32BIT_ABGR8888 ? sizeof(u32) : sizeof(u16));
	// Technically, these extra bytes weren't loaded, but hopefully it was loaded earlier.
	// If not, we're going to hash random data, which hopefully doesn't cause a performance issue.
	const u32 clutExtendedBytes = clutTotalBytes_ + clutBaseBytes;

	clutHash_ = XXH32((const char *)clutBufRaw_, clutExtendedBytes, 0xC0108888);

	// Avoid a copy when we don't need to convert colors.
	if (clutFormat != GE_CMODE_32BIT_ABGR8888) {
		ConvertColors(clutBufConverted_, clutBufRaw_, getClutDestFormat(clutFormat), clutExtendedBytes / sizeof(u16));
		clutBuf_ = clutBufConverted_;
	} else {
		clutBuf_ = clutBufRaw_;
	}

	// Special optimization: fonts typically draw clut4 with just alpha values in a single color.
	clutAlphaLinear_ = false;
	clutAlphaLinearColor_ = 0;
	if (gstate.getClutPaletteFormat() == GE_CMODE_16BIT_ABGR4444 && gstate.isClutIndexSimple()) {
		const u16 *clut = GetCurrentClut<u16>();
		clutAlphaLinear_ = true;
		clutAlphaLinearColor_ = clut[15] & 0xFFF0;
		for (int i = 0; i < 16; ++i) {
			if ((clut[i] & 0xf) != i) {
				clutAlphaLinear_ = false;
				break;
			}
			// Alpha 0 doesn't matter.
			if (i != 0 && (clut[i] & 0xFFF0) != clutAlphaLinearColor_) {
				clutAlphaLinear_ = false;
				break;
			}
		}
	}

	clutLastFormat_ = gstate.clutformat;
}

template <typename T>
inline const T *TextureCache::GetCurrentClut() {
	return (const T *)clutBuf_;
}

inline u32 TextureCache::GetCurrentClutHash() {
	return clutHash_;
}

// #define DEBUG_TEXTURES

#ifdef DEBUG_TEXTURES
bool SetDebugTexture() {
	static const int highlightFrames = 30;

	static int numTextures = 0;
	static int lastFrames = 0;
	static int mostTextures = 1;

	if (lastFrames != gpuStats.numFlips) {
		mostTextures = std::max(mostTextures, numTextures);
		numTextures = 0;
		lastFrames = gpuStats.numFlips;
	}

	static GLuint solidTexture = 0;

	bool changed = false;
	if (((gpuStats.numFlips / highlightFrames) % mostTextures) == numTextures) {
		if (gpuStats.numFlips % highlightFrames == 0) {
			NOTICE_LOG(HLE, "Highlighting texture # %d / %d", numTextures, mostTextures);
		}
		static const u32 solidTextureData[] = {0x99AA99FF};

		if (solidTexture == 0) {
			glGenTextures(1, &solidTexture);
			glBindTexture(GL_TEXTURE_2D, solidTexture);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, solidTextureData);
		} else {
			glBindTexture(GL_TEXTURE_2D, solidTexture);
		}
		changed = true;
	}

	++numTextures;
	return changed;
}
#endif

void TextureCache::SetTextureFramebuffer(TexCacheEntry *entry)
{
	entry->framebuffer->usageFlags |= FB_USAGE_TEXTURE;
	bool useBufferedRendering = g_Config.iRenderingMode != FB_NON_BUFFERED_MODE;
	if (useBufferedRendering) {
		// For now, let's not bind FBOs that we know are off (invalidHint will be -1.)
		// But let's still not use random memory.
		if (entry->framebuffer->fbo && entry->invalidHint != -1) {
			fbo_bind_color_as_texture(entry->framebuffer->fbo, 0);
			// Keep the framebuffer alive.
			// TODO: Dangerous if it sets a new one?
			entry->framebuffer->last_frame_used = gpuStats.numFlips;
		} else {
			glBindTexture(GL_TEXTURE_2D, 0);
			gstate_c.skipDrawReason |= SKIPDRAW_BAD_FB_TEXTURE;
		}
		UpdateSamplingParams(*entry, false);
		gstate_c.curTextureWidth = entry->framebuffer->width;
		gstate_c.curTextureHeight = entry->framebuffer->height;
		gstate_c.flipTexture = true;
		gstate_c.textureFullAlpha = entry->framebuffer->format == GE_FORMAT_565;
	} else {
		if (entry->framebuffer->fbo)
			entry->framebuffer->fbo = 0;
		glBindTexture(GL_TEXTURE_2D, 0);
	}
}

void TextureCache::SetTexture() {
#ifdef DEBUG_TEXTURES
	if (SetDebugTexture()) {
		// A different texture was bound, let's rebind next time.
		lastBoundTexture = -1;
		return;
	}
#endif

	u32 texaddr = (gstate.texaddr[0] & 0xFFFFF0) | ((gstate.texbufwidth[0]<<8) & 0x0F000000);
	if (!Memory::IsValidAddress(texaddr)) {
		// Bind a null texture and return.
		glBindTexture(GL_TEXTURE_2D, 0);
		lastBoundTexture = -1;
		return;
	}

	GETextureFormat format = gstate.getTextureFormat();
	if (format >= 11) {
		ERROR_LOG_REPORT(G3D, "Unknown texture format %i", format);
		// TODO: Better assumption?
		format = GE_TFMT_5650;
	}
	bool hasClut = gstate.isTextureFormatIndexed();

	u64 cachekey = (u64)texaddr << 32;
	u32 cluthash;
	if (hasClut) {
		if (clutLastFormat_ != gstate.clutformat) {
			// We update here because the clut format can be specified after the load.
			UpdateCurrentClut();
		}
		cluthash = GetCurrentClutHash() ^ gstate.clutformat;
		cachekey |= cluthash;
	} else {
		cluthash = 0;
	}

	int w = gstate.getTextureWidth(0);
	int h = gstate.getTextureHeight(0);
	int bufw = GetLevelBufw(0, texaddr);
	int maxLevel = ((gstate.texmode >> 16) & 0x7);

	u32 texhash = MiniHash((const u32 *)Memory::GetPointer(texaddr));
	u32 fullhash = 0;

	TexCache::iterator iter = cache.find(cachekey);
	TexCacheEntry *entry = NULL;
	gstate_c.flipTexture = false;
	gstate_c.skipDrawReason &= ~SKIPDRAW_BAD_FB_TEXTURE;
	bool useBufferedRendering = g_Config.iRenderingMode != FB_NON_BUFFERED_MODE;
	bool replaceImages = false;
	
	if (iter != cache.end()) {
		entry = &iter->second;
		// Check for FBO - slow!
		if (entry->framebuffer) {
			SetTextureFramebuffer(entry);
			lastBoundTexture = -1;
			entry->lastFrame = gpuStats.numFlips;
			return;
		}

		// Validate the texture here (width, height etc)

		int dim = gstate.texsize[0] & 0xF0F;
		bool match = entry->Matches(dim, format, maxLevel);
		bool rehash = (entry->status & TexCacheEntry::STATUS_MASK) == TexCacheEntry::STATUS_UNRELIABLE;
		bool doDelete = true;

		if (match) {
			if (entry->lastFrame != gpuStats.numFlips) {
				entry->numFrames++;
			}
			if (entry->framesUntilNextFullHash == 0) {
				// Exponential backoff up to 2048 frames.  Textures are often reused.
				entry->framesUntilNextFullHash = std::min(2048, entry->numFrames);
				rehash = true;
			} else {
				--entry->framesUntilNextFullHash;
			}

			// If it's not huge or has been invalidated many times, recheck the whole texture.
			if (entry->invalidHint > 180 || (entry->invalidHint > 15 && dim <= 0x909)) {
				entry->invalidHint = 0;
				rehash = true;
			}

			bool hashFail = false;
			if (texhash != entry->hash) {
				fullhash = QuickTexHash(texaddr, bufw, w, h, format);
				hashFail = true;
				rehash = false;
			}

			if (rehash && (entry->status & TexCacheEntry::STATUS_MASK) != TexCacheEntry::STATUS_RELIABLE) {
				fullhash = QuickTexHash(texaddr, bufw, w, h, format);
				if (fullhash != entry->fullhash) {
					hashFail = true;
				} else if ((entry->status & TexCacheEntry::STATUS_MASK) == TexCacheEntry::STATUS_UNRELIABLE && entry->numFrames > TexCacheEntry::FRAMES_REGAIN_TRUST) {
					// Reset to STATUS_HASHING.
					entry->status &= ~TexCacheEntry::STATUS_MASK;
				}
			}

			if (hashFail) {
				match = false;
				entry->status |= TexCacheEntry::STATUS_UNRELIABLE;
				entry->numFrames = 0;

				// Don't give up just yet.  Let's try the secondary cache if it's been invalidated before.
				// If it's failed a bunch of times, then the second cache is just wasting time and VRAM.
				if (entry->numInvalidated > 2 && entry->numInvalidated < 128 && !lowMemoryMode_) {
					u64 secondKey = fullhash | (u64)cluthash << 32;
					TexCache::iterator secondIter = secondCache.find(secondKey);
					if (secondIter != secondCache.end()) {
						TexCacheEntry *secondEntry = &secondIter->second;
						if (secondEntry->Matches(dim, format, maxLevel)) {
							// Reset the numInvalidated value lower, we got a match.
							if (entry->numInvalidated > 8) {
								--entry->numInvalidated;
							}
							entry = secondEntry;
							match = true;
						}
					} else {
						secondKey = entry->fullhash | (u64)entry->cluthash << 32;
						secondCache[secondKey] = *entry;
						doDelete = false;
					}
				}
			}
		}

		if (match) {
			// TODO: Mark the entry reliable if it's been safe for long enough?
			//got one!
			entry->lastFrame = gpuStats.numFlips;
			if (entry->texture != lastBoundTexture) {
				glBindTexture(GL_TEXTURE_2D, entry->texture);
				lastBoundTexture = entry->texture;
				gstate_c.textureFullAlpha = (entry->status & TexCacheEntry::STATUS_ALPHA_MASK) == TexCacheEntry::STATUS_ALPHA_FULL;
			}
			UpdateSamplingParams(*entry, false);
			DEBUG_LOG(G3D, "Texture at %08x Found in Cache, applying", texaddr);
			return; //Done!
		} else {
			entry->numInvalidated++;
			gpuStats.numTextureInvalidations++;
			INFO_LOG(G3D, "Texture different or overwritten, reloading at %08x", texaddr);
			if (doDelete) {
				if (entry->maxLevel == maxLevel && entry->dim == (gstate.texsize[0] & 0xF0F) && entry->format == format && g_Config.iTexScalingLevel <= 1) {
					// Actually, if size and number of levels match, let's try to avoid deleting and recreating.
					// Instead, let's use glTexSubImage to replace the images.
					replaceImages = true;
				} else {
					if (entry->texture == lastBoundTexture) {
						lastBoundTexture = -1;
					}
					glDeleteTextures(1, &entry->texture);
				}
			}
			if (entry->status == TexCacheEntry::STATUS_RELIABLE) {
				entry->status = TexCacheEntry::STATUS_HASHING;
			}
		}
	} else {
		INFO_LOG(G3D, "No texture in cache, decoding...");
		TexCacheEntry entryNew = {0};
		cache[cachekey] = entryNew;

		entry = &cache[cachekey];
		entry->status = TexCacheEntry::STATUS_HASHING;
	}

	if ((bufw == 0 || (gstate.texbufwidth[0] & 0xf800) != 0) && texaddr >= PSP_GetUserMemoryBase()) {
		ERROR_LOG_REPORT(HLE, "Texture with unexpected bufw (full=%d)", gstate.texbufwidth[0] & 0xffff);
	}

	// We have to decode it, let's setup the cache entry first.
	entry->addr = texaddr;
	entry->hash = texhash;
	entry->format = format;
	entry->lastFrame = gpuStats.numFlips;
	entry->framebuffer = 0;
	entry->maxLevel = maxLevel;
	entry->lodBias = 0.0f;
	
	entry->dim = gstate.texsize[0] & 0xF0F;
	entry->bufw = bufw;

	// This would overestimate the size in many case so we underestimate instead
	// to avoid excessive clearing caused by cache invalidations.
	entry->sizeInRAM = (bitsPerPixel[format] * bufw * h / 2) / 8;

	entry->fullhash = fullhash == 0 ? QuickTexHash(texaddr, bufw, w, h, format) : fullhash;
	entry->cluthash = cluthash;

	entry->status &= ~TexCacheEntry::STATUS_ALPHA_MASK;

	gstate_c.curTextureWidth = w;
	gstate_c.curTextureHeight = h;

	// Before we go reading the texture from memory, let's check for render-to-texture.
	for (size_t i = 0, n = fbCache_.size(); i < n; ++i) {
		auto framebuffer = fbCache_[i];
		// This is a rough heuristic, because sometimes our framebuffers are too tall.
		static const u32 MAX_SUBAREA_Y_OFFSET = 32;

		// Must be in VRAM so | 0x04000000 it is.
		const u64 cacheKeyStart = (u64)(framebuffer->fb_address | 0x04000000) << 32;
		// If it has a clut, those are the low 32 bits, so it'll be inside this range.
		// Also, if it's a subsample of the buffer, it'll also be within the FBO.
		const u64 cacheKeyEnd = cacheKeyStart + ((u64)(framebuffer->fb_stride * MAX_SUBAREA_Y_OFFSET) << 32);

		if (cachekey >= cacheKeyStart && cachekey < cacheKeyEnd) {
			AttachFramebuffer(entry, framebuffer->fb_address, framebuffer, cachekey == cacheKeyStart);
		}
	}

	// If we ended up with a framebuffer, attach it - no texture decoding needed.
	if (entry->framebuffer) {
		SetTextureFramebuffer(entry);
		lastBoundTexture = -1;
		entry->lastFrame = gpuStats.numFlips;
		return;
	}

	if (!replaceImages) {
		glGenTextures(1, &entry->texture);
	}
	glBindTexture(GL_TEXTURE_2D, entry->texture);
	lastBoundTexture = entry->texture;
	
	// Adjust maxLevel to actually present levels..
	for (int i = 0; i <= maxLevel; i++) {
		// If encountering levels pointing to nothing, adjust max level.
		u32 levelTexaddr = (gstate.texaddr[i] & 0xFFFFF0) | ((gstate.texbufwidth[i] << 8) & 0x0F000000);
		if (!Memory::IsValidAddress(levelTexaddr)) {
			maxLevel = i - 1;
			break;
		}
	}

	if (g_Config.bMipMap) { 

		// GLES2 doesn't have support for a "Max lod" which is critical as PSP games often
		// don't specify mips all the way down. As a result, we either need to manually generate
		// the bottom few levels or rely on OpenGL's autogen mipmaps instead, which might not
		// be as good quality as the game's own (might even be better in some cases though).

		// For now, I choose to use autogen mips on GLES2 and the game's own on other platforms.
		// As is usual, GLES3 will solve this problem nicely but wide distribution of that is
		// years away.
		LoadTextureLevel(*entry, 0, replaceImages);
		if (maxLevel > 0)
			glGenerateMipmap(GL_TEXTURE_2D);
		/*
		for (int i = 0; i <= maxLevel; i++) {
			LoadTextureLevel(*entry, i, replaceImages);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, (float)maxLevel);
		*/
	} else {
		LoadTextureLevel(*entry, 0, replaceImages);
#ifndef USING_GLES2
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
#endif
	}

	int aniso = 1 << g_Config.iAnisotropyLevel;
	float anisotropyLevel = (float) aniso > maxAnisotropyLevel ? maxAnisotropyLevel : (float) aniso;
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropyLevel);

	UpdateSamplingParams(*entry, true);

	//glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	//glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);

	gstate_c.textureFullAlpha = (entry->status & TexCacheEntry::STATUS_ALPHA_MASK) == TexCacheEntry::STATUS_ALPHA_FULL;
}

void *TextureCache::DecodeTextureLevel(GETextureFormat format, GEPaletteFormat clutformat, int level, u32 &texByteAlign, GLenum &dstFmt) {
	void *finalBuf = NULL;

	u32 texaddr = (gstate.texaddr[level] & 0xFFFFF0) | ((gstate.texbufwidth[level] << 8) & 0x0F000000);

	int bufw = GetLevelBufw(level, texaddr);

	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);
	const u8 *texptr = Memory::GetPointer(texaddr);

	switch (format) {
	case GE_TFMT_CLUT4:
		{
		dstFmt = getClutDestFormat(clutformat);
		// Don't allow this to be less than 16 bytes (32 * 4 / 8 = 16.)
		if (bufw < 32)
			bufw = 32;

		const bool mipmapShareClut = (gstate.texmode & 0x100) == 0;
		const int clutSharingOffset = mipmapShareClut ? 0 : level * 16;

		switch (clutformat) {
		case GE_CMODE_16BIT_BGR5650:
		case GE_CMODE_16BIT_ABGR5551:
		case GE_CMODE_16BIT_ABGR4444:
			{
			tmpTexBuf16.resize(std::max(bufw, w) * h);
			tmpTexBufRearrange.resize(std::max(bufw, w) * h);
			const u16 *clut = GetCurrentClut<u16>() + clutSharingOffset;
			texByteAlign = 2;
			if (!gstate.isTextureSwizzled()) {
				if (clutAlphaLinear_ && mipmapShareClut) {
					DeIndexTexture4Optimal(tmpTexBuf16.data(), texaddr, bufw * h, clutAlphaLinearColor_);
				} else {
					DeIndexTexture4(tmpTexBuf16.data(), texaddr, bufw * h, clut);
				}
			} else {
				tmpTexBuf32.resize(std::max(bufw, w) * h);
				UnswizzleFromMem(texaddr, bufw, 0, level);
				if (clutAlphaLinear_ && mipmapShareClut) {
					DeIndexTexture4Optimal(tmpTexBuf16.data(), (u8 *)tmpTexBuf32.data(), bufw * h, clutAlphaLinearColor_);
				} else {
					DeIndexTexture4(tmpTexBuf16.data(), (u8 *)tmpTexBuf32.data(), bufw * h, clut);
				}
			}
			finalBuf = tmpTexBuf16.data();
			}
			break;

		case GE_CMODE_32BIT_ABGR8888:
			{
			tmpTexBuf32.resize(std::max(bufw, w) * h);
			tmpTexBufRearrange.resize(std::max(bufw, w) * h);
			const u32 *clut = GetCurrentClut<u32>() + clutSharingOffset;
			if (!gstate.isTextureSwizzled()) {
				DeIndexTexture4(tmpTexBuf32.data(), texaddr, bufw * h, clut);
				finalBuf = tmpTexBuf32.data();
			} else {
				UnswizzleFromMem(texaddr, bufw, 0, level);
				// Let's reuse tmpTexBuf16, just need double the space.
				tmpTexBuf16.resize(std::max(bufw, w) * h * 2);
				DeIndexTexture4((u32 *)tmpTexBuf16.data(), (u8 *)tmpTexBuf32.data(), bufw * h, clut);
				finalBuf = tmpTexBuf16.data();
			}
			}
			break;

		default:
			ERROR_LOG(G3D, "Unknown CLUT4 texture mode %d", gstate.getClutPaletteFormat());
			return NULL;
		}
		}
		break;

	case GE_TFMT_CLUT8:
		if (bufw < 8)
			bufw = 8;
		dstFmt = getClutDestFormat(gstate.getClutPaletteFormat());
		texByteAlign = texByteAlignMap[gstate.getClutPaletteFormat()];
		finalBuf = readIndexedTex(level, texaddr, 1, dstFmt);
		break;

	case GE_TFMT_CLUT16:
		if (bufw < 8)
			bufw = 8;
		dstFmt = getClutDestFormat(gstate.getClutPaletteFormat());
		texByteAlign = texByteAlignMap[gstate.getClutPaletteFormat()];
		finalBuf = readIndexedTex(level, texaddr, 2, dstFmt);
		break;

	case GE_TFMT_CLUT32:
		if (bufw < 4)
			bufw = 4;
		dstFmt = getClutDestFormat(gstate.getClutPaletteFormat());
		texByteAlign = texByteAlignMap[gstate.getClutPaletteFormat()];
		finalBuf = readIndexedTex(level, texaddr, 4, dstFmt);
		break;

	case GE_TFMT_4444:
	case GE_TFMT_5551:
	case GE_TFMT_5650:
		if (bufw < 8)
			bufw = 8;
		if (format == GE_TFMT_4444)
			dstFmt = GL_UNSIGNED_SHORT_4_4_4_4;
		else if (format == GE_TFMT_5551)
			dstFmt = GL_UNSIGNED_SHORT_5_5_5_1;
		else if (format == GE_TFMT_5650)
			dstFmt = GL_UNSIGNED_SHORT_5_6_5;
		texByteAlign = 2;

		if (!gstate.isTextureSwizzled()) {
			int len = std::max(bufw, w) * h;
			tmpTexBuf16.resize(len);
			tmpTexBufRearrange.resize(len);
			finalBuf = tmpTexBuf16.data();
			ConvertColors(finalBuf, Memory::GetPointer(texaddr), dstFmt, bufw * h);
		} else {
			tmpTexBuf32.resize(std::max(bufw, w) * h);
			finalBuf = UnswizzleFromMem(texaddr, bufw, 2, level);
			ConvertColors(finalBuf, finalBuf, dstFmt, bufw * h);
		}
		break;

	case GE_TFMT_8888:
		if (bufw < 4)
			bufw = 4;
		dstFmt = GL_UNSIGNED_BYTE;
		if (!gstate.isTextureSwizzled()) {
			// Special case: if we don't need to deal with packing, we don't need to copy.
			if (w == bufw) {
				finalBuf = Memory::GetPointer(texaddr);
			} else {
				int len = bufw * h;
				tmpTexBuf32.resize(std::max(bufw, w) * h);
				tmpTexBufRearrange.resize(std::max(bufw, w) * h);
				Memory::Memcpy(tmpTexBuf32.data(), texaddr, len * sizeof(u32));
				finalBuf = tmpTexBuf32.data();
			}
		}
		else {
			tmpTexBuf32.resize(std::max(bufw, w) * h);
			finalBuf = UnswizzleFromMem(texaddr, bufw, 4, level);
		}
		ConvertColors(finalBuf, finalBuf, dstFmt, bufw * h);
		break;

	case GE_TFMT_DXT1:
		dstFmt = GL_UNSIGNED_BYTE;
		{
			int minw = std::min(bufw, w);
			tmpTexBuf32.resize(std::max(bufw, w) * h);
			tmpTexBufRearrange.resize(std::max(bufw, w) * h);
			u32 *dst = tmpTexBuf32.data();
			DXT1Block *src = (DXT1Block*)texptr;

			for (int y = 0; y < h; y += 4) {
				u32 blockIndex = (y / 4) * (bufw / 4);
				for (int x = 0; x < minw; x += 4) {
					decodeDXT1Block(dst + bufw * y + x, src + blockIndex, bufw);
					blockIndex++;
				}
			}
			finalBuf = tmpTexBuf32.data();
			w = (w + 3) & ~3;
		}
		break;

	case GE_TFMT_DXT3:
		dstFmt = GL_UNSIGNED_BYTE;
		{
			int minw = std::min(bufw, w);
			tmpTexBuf32.resize(std::max(bufw, w) * h);
			tmpTexBufRearrange.resize(std::max(bufw, w) * h);
			u32 *dst = tmpTexBuf32.data();
			DXT3Block *src = (DXT3Block*)texptr;

			for (int y = 0; y < h; y += 4) {
				u32 blockIndex = (y / 4) * (bufw / 4);
				for (int x = 0; x < minw; x += 4) {
					decodeDXT3Block(dst + bufw * y + x, src + blockIndex, bufw);
					blockIndex++;
				}
			}
			w = (w + 3) & ~3;
			finalBuf = tmpTexBuf32.data();
		}
		break;

	case GE_TFMT_DXT5:
		dstFmt = GL_UNSIGNED_BYTE;
		{
			int minw = std::min(bufw, w);
			tmpTexBuf32.resize(std::max(bufw, w) * h);
			tmpTexBufRearrange.resize(std::max(bufw, w) * h);
			u32 *dst = tmpTexBuf32.data();
			DXT5Block *src = (DXT5Block*)texptr;

			for (int y = 0; y < h; y += 4) {
				u32 blockIndex = (y / 4) * (bufw / 4);
				for (int x = 0; x < minw; x += 4) {
					decodeDXT5Block(dst + bufw * y + x, src + blockIndex, bufw);
					blockIndex++;
				}
			}
			w = (w + 3) & ~3;
			finalBuf = tmpTexBuf32.data();
		}
		break;

	default:
		ERROR_LOG_REPORT(G3D, "Unknown Texture Format %d!!!", format);
		return NULL;
	}

	if (!finalBuf) {
		ERROR_LOG_REPORT(G3D, "NO finalbuf! Will crash!");
	}

	if (w != bufw) {
		int pixelSize;
		switch (dstFmt) {
		case GL_UNSIGNED_SHORT_4_4_4_4:
		case GL_UNSIGNED_SHORT_5_5_5_1:
		case GL_UNSIGNED_SHORT_5_6_5:
			pixelSize = 2;
			break;
		default:
			pixelSize = 4;
			break;
		}
		// Need to rearrange the buffer to simulate GL_UNPACK_ROW_LENGTH etc.
		int inRowBytes = bufw * pixelSize;
		int outRowBytes = w * pixelSize;
		const u8 *read = (const u8 *)finalBuf;
		u8 *write = 0;
		if (w > bufw) {
			write = (u8 *)tmpTexBufRearrange.data();
			finalBuf = tmpTexBufRearrange.data();
		} else {
			write = (u8 *)finalBuf;
		}
		for (int y = 0; y < h; y++) {
			memmove(write, read, outRowBytes);
			read += inRowBytes;
			write += outRowBytes;
		}
	}

	return finalBuf;
}

void TextureCache::CheckAlpha(TexCacheEntry &entry, u32 *pixelData, GLenum dstFmt, int w, int h) {
	// TODO: Could probably be optimized more.
	u32 hitZeroAlpha = 0;
	u32 hitSomeAlpha = 0;

	switch (dstFmt) {
	case GL_UNSIGNED_SHORT_4_4_4_4:
		{
			const u32 *p = pixelData;
			for (int i = 0; i < (w * h + 1) / 2; ++i) {
				u32 a = p[i] & 0x000F000F;
				hitZeroAlpha |= a ^ 0x000F000F;
				if (a != 0x000F000F && a != 0x0000000F && a != 0x000F0000 && a != 0) {
					hitSomeAlpha = 1;
					break;
				}
			}
		}
		break;
	case GL_UNSIGNED_SHORT_5_5_5_1:
		{
			const u32 *p = pixelData;
			for (int i = 0; i < (w * h + 1) / 2; ++i) {
				u32 a = p[i] & 0x00010001;
				hitZeroAlpha |= a ^ 0x00010001;
			}
		}
		break;
	case GL_UNSIGNED_SHORT_5_6_5:
		{
			// Never has any alpha.
		}
		break;
	default:
		{
			const u32 *p = pixelData;
			for (int i = 0; i < w * h; ++i) {
				u32 a = p[i] & 0xFF000000;
				hitZeroAlpha |= a ^ 0xFF000000;
				if (a != 0xFF000000 && a != 0) {
					hitSomeAlpha = 1;
					break;
				}
			}
		}
		break;
	}

	if (hitSomeAlpha != 0)
		entry.status |= TexCacheEntry::STATUS_ALPHA_UNKNOWN;
	else if (hitZeroAlpha != 0)
		entry.status |= TexCacheEntry::STATUS_ALPHA_SIMPLE;
	else
		entry.status |= TexCacheEntry::STATUS_ALPHA_FULL;
}

void TextureCache::LoadTextureLevel(TexCacheEntry &entry, int level, bool replaceImages) {
	// TODO: only do this once
	u32 texByteAlign = 1;

	// TODO: Look into using BGRA for 32-bit textures when the GL_EXT_texture_format_BGRA8888 extension is available, as it's faster than RGBA on some chips.
	GLenum dstFmt = 0;

	GEPaletteFormat clutformat = gstate.getClutPaletteFormat();
	void *finalBuf = DecodeTextureLevel(GETextureFormat(entry.format), clutformat, level, texByteAlign, dstFmt);
	if (finalBuf == NULL) {
		return;
	}

	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);

	gpuStats.numTexturesDecoded++;

	// Can restore these and remove the fixup at the end of DecodeTextureLevel on desktop GL and GLES 3.
	// glPixelStorei(GL_UNPACK_ROW_LENGTH, bufw);
	// glPixelStorei(GL_PACK_ROW_LENGTH, bufw);

	glPixelStorei(GL_UNPACK_ALIGNMENT, texByteAlign);
	glPixelStorei(GL_PACK_ALIGNMENT, texByteAlign);

	int scaleFactor = g_Config.iTexScalingLevel;

	// Don't scale the PPGe texture.
	if (entry.addr > 0x05000000 && entry.addr < 0x08800000)
		scaleFactor = 1;

	u32 *pixelData = (u32 *)finalBuf;
	if (scaleFactor > 1 && entry.numInvalidated == 0) 
		scaler.Scale(pixelData, dstFmt, w, h, scaleFactor);
	// Or always?
	if (entry.numInvalidated == 0)
		CheckAlpha(entry, pixelData, dstFmt, w, h);
	else
		entry.status |= TexCacheEntry::STATUS_ALPHA_UNKNOWN;

	GLuint components = dstFmt == GL_UNSIGNED_SHORT_5_6_5 ? GL_RGB : GL_RGBA;

	if (replaceImages) {
		glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, components, dstFmt, pixelData);
	} else {
		glTexImage2D(GL_TEXTURE_2D, level, components, w, h, 0, components, dstFmt, pixelData);
		GLenum err = glGetError();
		if (err == GL_OUT_OF_MEMORY) {
			lowMemoryMode_ = true;
			Decimate();
			// Try again.
			glTexImage2D(GL_TEXTURE_2D, level, components, w, h, 0, components, dstFmt, pixelData);
		}
	}
}

// Only used by Qt UI?
bool TextureCache::DecodeTexture(u8* output, GPUgstate state)
{
	GPUgstate oldState = gstate;
	gstate = state;

	u32 texaddr = (gstate.texaddr[0] & 0xFFFFF0) | ((gstate.texbufwidth[0]<<8) & 0x0F000000);

	if (!Memory::IsValidAddress(texaddr)) {
		return false;
	}

	u32 texByteAlign = 1;
	GLenum dstFmt = 0;

	GETextureFormat format = gstate.getTextureFormat();
	GEPaletteFormat clutformat = gstate.getClutPaletteFormat();
	u8 level = 0;

	int bufw = GetLevelBufw(level, texaddr);

	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);

	void *finalBuf = DecodeTextureLevel(format, clutformat, level, texByteAlign, dstFmt);
	if (finalBuf == NULL) {
		return false;
	}

	switch (dstFmt)
	{
	case GL_UNSIGNED_SHORT_4_4_4_4:
		for(int y = 0; y < h; y++)
			for(int x = 0; x < bufw; x++)
			{
				u32 val = ((u16*)finalBuf)[y*bufw + x];
				u32 r = ((val>>12) & 0xF) * 17;
				u32 g = ((val>> 8) & 0xF) * 17;
				u32 b = ((val>> 4) & 0xF) * 17;
				u32 a = ((val>> 0) & 0xF) * 17;
				((u32*)output)[y*w + x] = (a << 24) | (r << 16) | (g << 8) | b;
			}
		break;

	case GL_UNSIGNED_SHORT_5_5_5_1:
		for(int y = 0; y < h; y++)
			for(int x = 0; x < bufw; x++)
			{
				u32 val = ((u16*)finalBuf)[y*bufw + x];
				u32 r = Convert5To8((val>>11) & 0x1F);
				u32 g = Convert5To8((val>> 6) & 0x1F);
				u32 b = Convert5To8((val>> 1) & 0x1F);
				u32 a = (val & 0x1) * 255;
				((u32*)output)[y*w + x] = (a << 24) | (r << 16) | (g << 8) | b;
			}
		break;

	case GL_UNSIGNED_SHORT_5_6_5:
		for(int y = 0; y < h; y++)
			for(int x = 0; x < bufw; x++)
			{
				u32 val = ((u16*)finalBuf)[y*bufw + x];
				u32 a = 0xFF;
				u32 r = Convert5To8((val>>11) & 0x1F);
				u32 g = Convert6To8((val>> 5) & 0x3F);
				u32 b = Convert5To8((val    ) & 0x1F);
				((u32*)output)[y*w + x] = (a << 24) | (r << 16) | (g << 8) | b;
			}
		break;

	default:
		for(int y = 0; y < h; y++)
			for(int x = 0; x < bufw; x++)
			{
				u32 val = ((u32*)finalBuf)[y*bufw + x];
				((u32*)output)[y*w + x] = ((val & 0xFF000000)) | ((val & 0x00FF0000)>>16) | ((val & 0x0000FF00)) | ((val & 0x000000FF)<<16);
			}
		break;
	}

	gstate = oldState;
	return true;
}
