/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/beantex.h (last changed 93988236; donor HEAD 55a3a9c8),
 * round 10, XBLA step 1 (docs/port-ios/19-round10-xbla-import.md). 64-bit host layer only. */
#ifndef PORT_BEANTEX_H
#define PORT_BEANTEX_H

/*
 * Reading the pictures out of GoldenEye XBLA's ("Project Bean") files.
 * Round 19c, D-096. Every file of the release is a Rare CAFF bundle; a picture
 * is an asset whose ".data"-section blob starts "texture\0" (a D3D texture
 * header: format at +0x1b, width/height at +0x24, level count at +0x30) and
 * whose texels are the asset's own ".gpu" blob, or the file's shared
 * "texture pairs" .gpu at the base offset in +0x28.
 *
 * Ported from Perfect Dark's gebean.c (perfect-dark-ios vendor/dabs-mod,
 * MIT): caffOpen()/caffBlob()/caffAssetName()/caffFind() and
 * beanDecodeTextureFrame() for a still. The surface decode is x360.c's.
 *
 * Nothing here reads the art from anywhere but the user's own copy; nothing
 * of it is in this repository.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct beanfile;

/* Opens a CAFF bundle held in memory (not copied; keep `data` alive). */
struct beanfile *beanOpen(const uint8_t *data, uint32_t len);
void beanClose(struct beanfile *bf);

/* The pictures in it, in file order. */
int beanNumTextures(const struct beanfile *bf);
/* The picture's asset label, e.g. "_0x02600155.rgb.bin". */
const char *beanTextureName(const struct beanfile *bf, int t);
/* Its size and x360 format (X360_FMT_*), without decoding. 0 if unreadable. */
int beanTextureInfo(const struct beanfile *bf, int t, int32_t *w, int32_t *h, int32_t *format);
/* Index of the picture with this label, or -1. */
int beanFindTexture(const struct beanfile *bf, const char *name);

/*
 * Decodes picture t's level 0 to malloc'd RGBA8, top row first (as stored:
 * the N64's row order, the same order the port uploads). NULL on failure.
 */
uint8_t *beanDecodeTexture(const struct beanfile *bf, int t, int32_t *outW, int32_t *outH);

/*
 * Sections (Tier C, round C1). A CAFF asset's data lives in named sections:
 * asset 1 of a model or level file is its rendergraph, whose ".data" holds
 * the buffer descriptors, ".gpu" the vertex/index buffers, shader microcode
 * and texels, and ".stream" the command stream (beanmesh.c walks it).
 * PD's caffFind()/caffBlob().
 */
/* The blob of the first file of `asset` whose section name starts with
 * `prefix` (".data", ".gpu", ".stream"); NULL (and *len 0) if none. */
const uint8_t *beanSection(const struct beanfile *bf, uint32_t asset, const char *prefix, uint32_t *len);
/* The CAFF's own byte order: 1 when its words are big-endian (every
 * Xbox 360 file). */
int beanIsBigEndian(const struct beanfile *bf);
/* Midas round 43: the first blob (any asset, any section) that starts with
 * `magic` and holds at least `minlen` bytes; NULL (and *len 0) if none. */
const uint8_t *beanFindBlob(const struct beanfile *bf, const char *magic, uint32_t magiclen, uint32_t minlen,
                            uint32_t *len);

#ifdef __cplusplus
}
#endif

#endif
