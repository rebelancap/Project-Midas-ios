/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/pngread.h (last changed 5e2c63ad; donor HEAD 55a3a9c8),
 * round 10, XBLA step 1 (docs/port-ios/19-round10-xbla-import.md). 64-bit host layer only. */
#ifndef _IN_PNGREAD_H
#define _IN_PNGREAD_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decodes a PNG into a freshly malloc'd RGBA32 buffer, top row first, which the
 * caller frees. Returns NULL and logs why on anything it cannot read.
 *
 * Handles greyscale, greyscale+alpha, truecolour, truecolour+alpha and palette
 * (with tRNS) at 8 bits and below, plain or Adam7 interlaced - which covers
 * what every image editor writes and what the texture packs in the wild
 * contain. 16 bit channels are refused rather than guessed at.
 */
u8 *pngRead(const char *path, s32 *outWidth, s32 *outHeight);

/**
 * The same, over a PNG already in memory - a picture built into the binary
 * rather than installed beside it. name is only used in the log.
 */
u8 *pngReadMem(const void *data, u32 size, const char *name, s32 *outWidth, s32 *outHeight);

/**
 * Project Midas round 38: pngRead() at most maxDim on a side. A larger image is
 * reduced by the smallest integer box k (alpha-weighted) WHILE it is decoded,
 * from a mapping of the file, so the full-size image is never in memory.
 * maxDim <= 0 is pngRead(). srcWidth/srcHeight (may be NULL) get the authored
 * size.
 */
u8 *pngReadMaxDim(const char *path, s32 maxDim, s32 *outWidth, s32 *outHeight, s32 *srcWidth, s32 *srcHeight);

#ifdef __cplusplus
}
#endif

#endif
