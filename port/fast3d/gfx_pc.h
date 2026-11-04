#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <list>
#include <cstddef>

#include <PR/gbi.h>

#include "system.h"

#define SCREEN_WIDTH ((int32_t)gfx_current_native_viewport.width)
#define SCREEN_HEIGHT ((int32_t)gfx_current_native_viewport.height)

extern uintptr_t gfxFramebuffer;

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct TextureCacheKey {
    const uint8_t* texture_addr;
    const uint8_t* palette_addrs[2];
    uint8_t fmt, siz;
    uint8_t palette_index;
    /* GEOMETRY IS PART OF THE IDENTITY (triage F2.1, Control mainframe
     * "sometimes corrupted"): keyed on address+format alone, a second
     * texture that lands on the same address - a reused dynAllocate
     * block, an animated screen's next frame buffer, another level of a
     * mip chain addressed by TMEM offset - hit the FIRST one's cached GL
     * texture. The draw then sampled the old image at the new tile's UV
     * scale: the wrong picture, tiled and repeating across the surface,
     * and only when the allocator happened to collide (hence "sometimes"). */
    uint32_t line_size_bytes;
    uint32_t size_bytes;
    /* CI textures: fingerprint of the palette CONTENT, not just its
     * address - GE reloads different TLUTs into the same buffer (see
     * rdp.palette_sum). 0 for non-CI formats. */
    uint32_t palette_sum;
    /* GE: wrap masks are part of the identity - a tile whose mask is
     * NARROWER than its load line is imported cropped to the mask
     * (ge_upload_masked); the same bytes drawn with other masks must not
     * reuse that entry. */
    uint8_t masks, maskt;

    bool operator==(const TextureCacheKey&) const noexcept = default;

    struct Hasher {
        size_t operator()(const TextureCacheKey& key) const noexcept {
            uintptr_t addr = (uintptr_t)key.texture_addr;
            return (size_t)(addr ^ (addr >> 5));
        }
    };
};

typedef std::unordered_map<TextureCacheKey, struct TextureCacheValue, TextureCacheKey::Hasher> TextureCacheMap;
typedef std::pair<const TextureCacheKey, struct TextureCacheValue> TextureCacheNode;

struct TextureCacheValue {
    uint32_t texture_id;
    uint32_t serial;     /* unique per node creation (GE Xbox retained caches validate on it: a
                          * texture_id is recycled by the next import after a delete) */
    uint16_t pins;       /* retained lists holding this node: the LRU evicts unpinned nodes first */
    uint8_t cms, cmt;
    bool linear_filter;
    bool mip_filter; /* GE: minifying through a mip chain (N64 G_TL_LOD draw) */
#if defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) /* PLATFORM_64BIT, spelled out: gfx_opengl.cpp
                                                                    * includes this without platform.h */
    /* Round 11 (XBLA step 2, from GoldenEye-Decomp-ios gfx_pc.h, Tier C
     * round C1, D-110): a Bean picture on a stand-in tile. Its UVs are the
     * release's own (texel centres at +0.5 already), so the N64 bilinear
     * half-texel offset is not applied (gfx_sp_tri1). Only texpack's 'b'
     * source sets it; no Bean picture is registered before step 4. */
    bool exact_uv;
    /* Round 18 (Phase 3a; GoldenEye-Decomp-ios gfx_pc.h round 35a, D-316):
     * the entry draws a resident picture's GL texture (index into
     * gfx_pc.cpp's resident table), which it does not own: an eviction or a
     * clear must not put texture_id on the free list. -1 = an ordinary entry. */
    int32_t resident = -1;
    /* Round 37 (docs/port-ios/50): the GL texture is the xBR picture at this
     * scale (2..4) of the N64 texels, 0 = the texels as imported. UVs need no
     * allowance (they are normalised by the N64 tile and the shader sizes
     * texels with textureSize); kept for the log and for A/Bs. */
    uint8_t upscale = 0;
    /* Round 43 (docs/port-ios/60): the picture is the XBLA release's own glyph
     * (texpack source 'g', xblafont.c), already high resolution: drawn with
     * bilinear filtering whatever the 2D filter says. */
    bool hd_glyph = false;
    /* Round 43: a pack / XBLA / Bean picture went up for this entry (not the
     * N64 texels): the 2D sampler leaves a pack's glyph picture on filter_2d. */
    bool replaced = false;
#endif

    std::list<struct TextureCacheMapIter>::iterator lru_location;
};

struct TextureCacheMapIter {
    TextureCacheMap::iterator it;
};

extern "C" {

#include "gfx_api.h"

}

#endif
