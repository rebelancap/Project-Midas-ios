/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/dxt.h (last changed 5e2c63ad; donor HEAD 55a3a9c8),
 * round 10, XBLA step 1 (docs/port-ios/19-round10-xbla-import.md). 64-bit host layer only. */
#ifndef _IN_DXT_H
#define _IN_DXT_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * S3TC block decoding, shared by everything that ships DXT at us.
 *
 * Two unrelated paths need it and neither should carry its own copy: the Glide
 * high resolution texture caches an emulator pack comes as (texpack.c), and
 * the Xbox 360 surfaces in the XBLA release (x360.c). The formats they name it
 * by differ - Glide numbers, GPU fetch constants - so the kind below is
 * neither of theirs, and each side maps to it.
 *
 * The alpha ramps are the part worth having once rather than twice: DXT5 has
 * two of them, chosen by whether the endpoints are in order, and the second
 * one ends in a fixed 0 and 255 rather than continuing to interpolate.
 */

#define DXT_KIND_1 1 // 8 byte blocks, colour only, c0 <= c1 punches through
#define DXT_KIND_3 3 // 16 byte blocks, four bit alpha
#define DXT_KIND_5 5 // 16 byte blocks, interpolated alpha

/** Bytes one block of this kind occupies. */
u32 dxtBlockSize(s32 kind);

/** One block into a 4x4 of RGBA8 at dst, rows stride bytes apart. */
void dxtBlock(const u8 *block, s32 kind, u8 *dst, u32 stride, s32 w, s32 h);

#ifdef __cplusplus
}
#endif

#endif
