/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/texpack.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_TEXPACK_H
#define PORT_TEXPACK_H

/*
 * Texture packs (Tier A) and the GoldenEye XBLA layer (Tier B) — round 19c,
 * DECISIONS D-095..D-098, docs/texture-packs.md.
 *
 * The scheme is Perfect Dark's (perfect-dark-ios/vendor/dabs-mod
 * port/src/texpack.c), cut down to what GoldenEye needs, so a pack written for
 * PD's loader — or for an emulator — loads here unchanged:
 *
 *   $S/texturepacks/<pack>/...   any depth, one pack selected by Video.TexturePack
 *     <texnum>.png               four hex digits, optionally _anything.png: ours
 *     <ROM>#<crc>#<f>#<s>[#<pal>|#$]_{all,ciByRGBA,allciByRGBA,ci,rgb}.png
 *                                Rice/GLideN64: crc = Rice CRC32 of the texels
 *                                as an emulator sees them in RDRAM
 *
 *   $C/xbla/tex/<texnum>.png     the XBLA layer, written by the importer
 *                                (xblaimport.c) from the user's own copy;
 *                                drawn only where the pack has no file.
 *
 * The identity of a drawn texture is its texture number, found by the address
 * texLoad() decompressed it to (texpackRegisterTexture). A Rice name is turned
 * into a number by checksumming every ROM texture once, and a Rice file whose
 * checksum names no ROM texture is matched against the texels of whatever is
 * drawn instead (texels inside model files the number index cannot see).
 *
 * OFF (no pack selected, no XBLA layer) nothing here is consulted by the
 * renderer and nothing in the command stream changes; ON only the pixels of
 * an upload change (D-095: the M-001 replay is identical either way).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEXPACK_DIR "$S/texturepacks"
#define TEXPACK_XBLA_DIR "$C/xbla"

/* Registers the config keys. Call once, before configLoad(). */
void texpackRegisterConfig(void);

/* Round 38 (Midas doc 51). The sim, between frames: runs the Rice checksum
 * walk on the game thread when the render thread's index asked for it. */
void texpackGameThreadPoint(void);
/* 1 while a texpack walk is inside texLoad() (not a game load: R11-1). */
int texpackIndexing(void);
/* Pictures decoded below their authored size, the Video.TexPackMaxDim in
 * force, and the largest decode so far (KiB of RGBA). */
void texpackGetCapStats(uint32_t *capped, int32_t *maxDim, uint32_t *largestKb);

/* texLoad(): the texels at `data` (host address) are texture `texnum`. */
void texpackRegisterTexture(const void *data, int32_t texnum, uint32_t bytes);

/* The texture number texLoad() registered at `data`, or -1 (unknown address,
 * or the bytes there are no longer the ones registered). D-102: the
 * bullet-hit surface lookup. */
int32_t texpackTexnumAt(const void *data);

/* Renderer side (fast3d import_texture on a cache miss). */
int texpackActive(void);

/*
 * Tier C (round C1, D-110): GoldenEye XBLA's own pictures for the meshes
 * beanstage.c builds. A built display list binds a 32x32 RGBA16 stand-in
 * tile (host memory) whose ADDRESS names the picture; the renderer asks
 * texpackLoadReplacement() for those texels as for any other and gets the
 * decoded Bean picture back (*outSource 'b'), uploaded at its own size and
 * drawn with exact UVs (no N64 half-texel offset). Perfect Dark's xblatex.c
 * trick. The pictures are pinned here, outside the PNG LRU's budget.
 *
 * texpackRegisterBeanPicture() takes ownership of `rgba` (malloc'd RGBA8,
 * rows in the order the mesh's UVs address). texpackForgetBeanPictures()
 * frees them all and drops the renderer's texture cache (a later stand-in
 * may be allocated at a freed address).
 */
void texpackRegisterBeanPicture(const void *addr, uint8_t *rgba, int32_t w, int32_t h);
void texpackForgetBeanPictures(void);
void texpackBeanPictureStats(int32_t *count, uint32_t *bytes);

/*
 * Round 35a (D-316): the render thread's own view of the pinned pictures, so
 * it can upload each one ONCE at the level's first frame and keep it as a GL
 * texture (gfx_pc.cpp "resident pictures") instead of copying and uploading
 * it at every texture-cache miss. Both run `fn` under the registry's lock,
 * with the registry's own pixels (no copy; do not keep the pointer).
 * texpackBeanVisit() calls fn for the pictures registered since *next and
 * advances it; when the registry was forgotten since *gen (a new level), it
 * visits nothing, sets *gen and *next = 0 and returns -1: the caller drops
 * what it holds and calls again. texpackBeanWith() calls fn for the picture
 * pinned at `addr` and returns 1, or 0 when there is none.
 * texpackPicturePrewarm() is the key Video.TexturePrewarm (0 off: every miss
 * copies and uploads as before; 1, the default: resident). No settings row.
 */
typedef void (*texpackbeanfn)(void *ctx, const void *addr, const uint8_t *rgba, int32_t w, int32_t h);
uint32_t texpackBeanGeneration(void);
int texpackBeanVisit(uint32_t *gen, int32_t *next, texpackbeanfn fn, void *ctx);
int texpackBeanWith(const void *addr, texpackbeanfn fn, void *ctx);
int texpackPicturePrewarm(void);
/* Round 28 (doc 38): Video.TexCacheMax, fast3d's texture-cache cap (4096 on
 * iOS, 1024 elsewhere), and Video.TexturePrewarmDraw (1 on iOS: the resident
 * pump samples each new picture once off-screen, D-328). No settings rows. */
int texpackTexCacheMax(void);
int texpackPrewarmDraw(void);
/* Round 35a (D-317): pixels registered twice (beanprefetch.c registers a
 * picture it decoded ahead; the build that later gets the same pointer from
 * beanLevelDecodeTexture registers it on its stand-in) are pinned once and
 * shared: the second entry does not own them. The Gen form returns the
 * registry's generation the picture went into (0: not registered, and the
 * pixels were freed). texpackBeanPictureRelease() frees pixels that are
 * not pinned (a build's failure path for a picture it may not own). */
uint32_t texpackRegisterBeanPictureGen(const void *addr, uint8_t *rgba, int32_t w, int32_t h);
void texpackBeanPictureRelease(uint8_t *rgba);

/*
 * The replacement for the texels at `data`, as a malloc'd RGBA8 image, top row
 * first, or NULL. `bytes`/`tileW`/`tileH`/`siz`/`rowBytes` describe the load
 * so a texel checksum can be taken when the address is not registered.
 * *outSource is 'p' (pack), 'x' (XBLA layer) or 'b' (a Bean picture on a
 * Tier C stand-in tile: upload as is, exact UVs).
 */
uint8_t *texpackLoadReplacement(const void *data, uint32_t bytes, int32_t tileW, int32_t tileH,
                                int32_t siz, uint32_t rowBytes, int32_t *outW, int32_t *outH,
                                char *outSource);
void texpackFreeReplacement(uint8_t *rgba);

/*
 * Round 40 (Midas doc 53, R38-1): pack PNGs are decoded on a worker thread
 * (ge-texpack), never on the render thread (Video.TexPackAsync, default 1).
 * texpackLoadReplacementAsync() is texpackLoadReplacement() except that a
 * pack picture not decoded yet returns NULL with ticket->key set: the caller
 * uploads the N64 texels and keeps the ticket; texpackTicketPoll() later
 * gives the picture (1: malloc'd RGBA, free with texpackFreeReplacement),
 * says it is still decoding (0), or that it never will be (-1: the index
 * changed or the file did not decode). ticket->key is NULL for every other
 * answer (a picture, XBLA, Bean, nothing), and always with the key at 0.
 * texpackFrame() runs at the top of the render thread's frame: the index,
 * decode-ahead requests from texLoad() (Video.TexPackDecodeAhead), finished
 * decodes into the keep cache. texpackCacheCleared(): the renderer dropped
 * its texture cache (a level change): ahead requests start over.
 * All render thread.
 */
typedef struct texpackticket {
    const void *key;   /* opaque: the index's identity of the file */
    uint32_t gen;      /* the index generation it belongs to */
} texpackticket;
uint8_t *texpackLoadReplacementAsync(const void *data, uint32_t bytes, int32_t tileW, int32_t tileH,
                                     int32_t siz, uint32_t rowBytes, int32_t *outW, int32_t *outH,
                                     char *outSource, texpackticket *ticket);
int texpackTicketPoll(const texpackticket *ticket, uint8_t **outRgba, int32_t *outW, int32_t *outH);
void texpackFrame(void);
void texpackCacheCleared(void);
int texpackAsyncEnabled(void);
/* requests accepted (all / decode-ahead), worker decodes, results thrown
 * away as stale, files that failed, worker time, jobs queued or decoding
 * now, decode-ahead pictures evicted before a draw took them */
void texpackGetAsyncStats(uint32_t *queued, uint32_t *ahead, uint32_t *decoded, uint32_t *discarded,
                          uint32_t *failed, uint64_t *workerUs, uint32_t *waiting, uint32_t *wasted);

/* Bumped whenever what would be drawn changes; the renderer drops its
 * texture cache when it sees a new value. */
uint32_t texpackSerial(void);

/* Re-read the pack list and the selected pack (settings changed). Any thread;
 * the work happens on the next renderer lookup. */
void texpackReload(void);

/* Pack list, for the settings page. */
int texpackGetNumPacks(void);
const char *texpackGetPackName(int index);
const char *texpackGetSelected(void);           /* "" for none */
void texpackSetSelected(const char *name);      /* "" for none; reloads */
int texpackGetNumReplacements(void);            /* files of the selected pack that found a texture */
int texpackGetNumPackFiles(void);
int texpackGetNumUnplaced(void);

/* XBLA layer switch + status. */
int texpackXblaEnabled(void);
void texpackSetXblaEnabled(int on);
int texpackXblaNumTextures(void);               /* textures in the XBLA layer on disk */
const char *texpackXblaPath(void);              /* Video.XblaPath (dev key) */

/* Decode statistics for the probe/bridge. */
void texpackGetStats(uint32_t *decodes, uint32_t *keptHits, uint64_t *decodeUs, uint32_t *keptMb);

/* Rice CRC32 as GlideHQ computes it, over texels in RDRAM order. Exposed for
 * the host tools and the probe. */
int texpackRiceCrc(const uint8_t *data, uint32_t len, int32_t width, int32_t height,
                   int32_t siz, int32_t stride, uint32_t *outCrc);

/* Mean r, g, b (0-255) of each ROM texture in nums[] as drawn, three floats a
 * texture in out[], -1 where one does not load. Game thread only (texLoad).
 * beanstage.c's brightness match (D-223). */
void texpackTextureMeans(const int32_t *nums, int32_t count, float *out);

#ifdef __cplusplus
}
#endif

#endif
