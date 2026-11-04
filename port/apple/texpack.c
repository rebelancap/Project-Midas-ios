/* Imported from GoldenEye-Decomp-ios engine/port/src/texpack.c (last changed
 * 47ef757f, at 55a3a9c8), round 11 = XBLA step 2
 * (docs/port-ios/xbla-port-proposal.md, docs/port-ios/20-round11-texpack.md).
 * Midas adaptations, each marked "Midas (round 11)":
 *   - the Rice / texture-means guest scratch (2 x 128 KiB) comes from Midas's
 *     dedicated guest bump (portAllocHighDedicated, pcsched.c), not a blind
 *     guestMemCommit at 0x60000000, which sits inside that bump's range;
 *   - the Rice walk's texLoad() calls are kept out of the tickhz=hw decode
 *     price (portHwLoadSuppress) and are not run while the render thread is
 *     on (texLoad is the game thread's: Rice-named files then fall back to
 *     the texel match);
 *   - the keys register through round 10's shim (port/include/config.h over
 *     midas.ini, port/apple/xblaconfig.c), read by main.c's configLoad();
 *   - host memory only: mema.h / memp.h are refused (here and in CMake).
 * Everything else is the donor's code unchanged. */
/*
 * Texture packs and the XBLA layer — see port/include/texpack.h and
 * docs/texture-packs.md. Round 19c, D-095..D-098.
 *
 * Ported in shape (not line for line) from Perfect Dark's loader,
 * perfect-dark-ios/vendor/dabs-mod/port/src/texpack.c (MIT, Ryan Dwyer and
 * contributors): the registry, the Rice checksum + swizzle, the name parsing
 * and the unplaced-file texel match are the same algorithms, so the same pack
 * files resolve to the same textures. Left out: async decode, the glyph
 * folders, HTC/HTS caches, JPEG, archives, the dumper.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>   /* round 40: the decode worker's utility QoS */
#endif
#include <time.h>
#include <ultra64.h>
#include "platform.h"
#include "system.h"
#include "config.h"
#include "pngread.h"
#include "texpack.h"
#include "xblafont.h"
#include "game/image.h"
#include "game/tex.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "texpack.c must not see the sim's allocators (mema.h/memp.h): host state only"
#endif

/* Midas (round 11): <string.h> above resolves to the decomp's C-side stub
 * (include/string.h), which lacks these two; host libc signatures. The host
 * header cannot be forced in here as it is for the importer TUs: its
 * bcopy/bzero/bcmp clash with PR/os.h, which this file needs. */
extern char *strdup(const char *);
extern char *strrchr(const char *, int);

#define TEXPACK_NUM_TEXTURES 3001        /* NUM_TEXTURES; g_Textures is shorter */
#define TEXPACK_MAXPACKS 32
#define TEXPACK_NAMELEN 128  /* >= hostinput.c geCfgVisit's 128-byte string write */
#define TEXPACK_MAXDEPTH 8
#define TEXPACK_REG_SLOTS 32768          /* power of two */
#define TEXPACK_REG_PROBES 64
#define TEXPACK_SIG_BYTES 64
#define TEXPACK_RICE_SLOTS 8192
#define TEXPACK_RICE_SCRATCH (128 * 1024)
#define TEXPACK_UNPLACED_SLOTS 4096

#define KIND_NATIVE 0
#define KIND_ALL 1
#define KIND_RGB 2
#define KIND_NONE 127

s32 texGetWidthAtLod(struct tex *tex, s32 lod);
s32 texGetHeightAtLod(struct tex *tex, s32 lod);
s32 texGetLineSizeInBytes(struct tex *tex, s32 lod);
s32 texGetSizeInBytes(struct tex *tex, s32 lod);
extern s32 g_NumTexturesPort;           /* image.c, PORT */
extern struct texcacheitem g_TexCacheItems[150];

/* Midas (round 11): the guest scratch both texLoad() walks decompress into.
 * texLoad() hands guest addresses around (osVirtualToPhysical, u32 pool
 * arithmetic), so the pool must sit inside the guest window. The donor
 * committed it at a fixed guest 0x60000000; in Midas that address belongs to
 * the dedicated bump (0x50000000..0x80000000, pcsched.c), which would later
 * commit over it. One 256 KiB piece of that bump instead, taken the first
 * time a walk runs (a Rice-named pack, or Tier C's colour match), never on a
 * run that has neither. Never freed (the bump's contract). */
#define TEXPACK_GUEST_SCRATCH_BYTES (2 * TEXPACK_RICE_SCRATCH)
extern void *portAllocHighDedicated(u32 size);   /* pcsched.c */
extern void portHwLoadSuppress(s32 on);          /* frametiming.c */
extern s32 portRenderAsyncActive(void);          /* video.c (weak 0 at the end: headless) */

static uint8_t *texpackGuestScratch(void)
{
    static uint8_t *scratch;
    static int tried;
    if (!tried) {
        tried = 1;
        scratch = portAllocHighDedicated(TEXPACK_GUEST_SCRATCH_BYTES);
    }
    return scratch;
}

/* ------------------------------------------------------------------ config */

static char cfgPack[TEXPACK_NAMELEN] = "";
static int cfgXbla = 1;
static char cfgXblaPath[512] = "";
static int cfgXblaSame = 0;
static int cfgPicturePrewarm = 1;   /* round 35a (D-316), no settings row */
/* Round 28 (doc 38 item 2, the sibling's D-092c Video.TexCacheMax): fast3d's
 * texture-cache cap. 4096 on iOS (sibling simulator: ~25 evictions/s -> 0);
 * 1024, upstream's, elsewhere. */
#ifdef GE_IOS
static int cfgTexCacheMax = 4096;
#else
static int cfgTexCacheMax = 1024;
#endif
/* Round 28 (doc 38 item 4, the sibling's D-328): after the resident pump
 * uploads a batch, one off-screen draw samples each new picture so ANGLE-Metal
 * does its deferred copy + mip build there, not at the first in-play draw. */
#ifdef GE_IOS
static int cfgPrewarmDraw = 1;
#else
static int cfgPrewarmDraw = 0;
#endif

/* Round 38 (doc 51): the largest side a pack picture is decoded at. A larger
 * one is box-filtered down by an integer factor while it is decoded
 * (pngReadMaxDim: the full-size image never exists in memory). The GoldenEye
 * 007 HD pack ships 98 pictures above 2048 on a side, 46 of them 4096x4096
 * (64 MB each as RGBA, ~200 MB transient through the plain decoder). 2048 on
 * iOS/iPadOS/visionOS; 0 = as authored (the Mac oracle). No settings row. */
#if defined(PLATFORM_IOS)
static int cfgMaxDim = 2048;
#else
static int cfgMaxDim = 0;
#endif

/* Round 40 (doc 53, R38-1): pack PNGs are decoded on a worker thread
 * (ge-texpack), never on the render thread. Video.TexPackAsync 0 = the old
 * synchronous decode inside the texture-cache miss (A/B). Video.TexPack-
 * DecodeAhead 1 = a texture the game thread registers (texLoad) whose number
 * has a pack file is decoded before it is drawn. No settings rows. */
static int cfgAsync = 1;
static int cfgAhead = 1;

void texpackRegisterConfig(void)
{
    configRegisterString("Video.TexturePack", cfgPack, sizeof(cfgPack));
    configRegisterInt("Video.XblaTextures", &cfgXbla, 0, 1);
    configRegisterString("Video.XblaPath", cfgXblaPath, sizeof(cfgXblaPath));
    configRegisterInt("Video.XblaSameSize", &cfgXblaSame, 0, 1);
    configRegisterInt("Video.TexturePrewarm", &cfgPicturePrewarm, 0, 1);
    configRegisterInt("Video.TexCacheMax", &cfgTexCacheMax, 256, 16384);
    configRegisterInt("Video.TexturePrewarmDraw", &cfgPrewarmDraw, 0, 1);
    configRegisterInt("Video.TexPackMaxDim", &cfgMaxDim, 0, 16384);
    configRegisterInt("Video.TexPackAsync", &cfgAsync, 0, 1);
    configRegisterInt("Video.TexPackDecodeAhead", &cfgAhead, 0, 1);
}

const char *texpackXblaPath(void) { return cfgXblaPath; }

/* ---------------------------------------------------------------- registry */

struct regslot {
    const void *data;
    int32_t texnum;
    uint32_t siglen;
    uint32_t sig;
};

static struct regslot *reg;
static int indexing;   /* the Rice index walk loads every texture: not drawn */

/* Round 18 (Phase 3a, R11 review nit): texLoad() registers on EVERY call,
 * packs or not, because the registry also answers texpackTexnumAt() - the
 * bullet-hit surface lookup (image.c, D-102), which is gameplay - so it cannot
 * become a flag load without changing what the game does (doc 27 §R18-2).
 * What it can be is cheap: the signature was a byte-at-a-time FNV-1a over 64
 * bytes (64 dependent multiplies); it is now eight 64-bit words through a
 * multiply-xorshift mix, tail bytes as before. It only ever compares against
 * itself (register vs lookup, same process), so changing the function changes
 * no answer except on a 32-bit collision. */
static uint32_t sigOf(const uint8_t *p, uint32_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * 0x94D049BB133111EBull;
    }
    h ^= h >> 29;
    return (uint32_t)(h ^ (h >> 32));
}

/* GE_TICKPROF (tickprof.c): registrations per second and their cost. */
static uint64_t regCalls, regNs;
void texpackRegStatsTake(uint64_t *calls, uint64_t *ns)
{
    *calls = regCalls;
    *ns = regNs;
    regCalls = regNs = 0;
}

static uint32_t regHash(const void *data)
{
    uintptr_t v = (uintptr_t)data;
    v ^= v >> 17;
    v *= 0x9E3779B1u;
    return (uint32_t)(v ^ (v >> 15)) & (TEXPACK_REG_SLOTS - 1);
}

static void texpackRegisterTextureNow(const void *data, int32_t texnum, uint32_t bytes);

/* ------------------------------------------------- decode ahead (round 40)
 * texLoad() runs on the game thread and is the earliest moment a texture
 * number's pack file is known to be wanted. The game thread only records the
 * number here (a single-producer ring the render thread drains at the top of
 * its frame, texpackFrame); it never touches the index, which belongs to the
 * render thread. aheadOn is the render thread's "a pack with numbered files
 * is selected and Video.TexPackAsync / DecodeAhead are on"; while it is 0
 * (always, in the headless build: nothing calls texpackFrame there) this is
 * one relaxed load. aheadSent keeps a number from being pushed again until
 * the render thread bumps aheadEpoch (a new index, a cleared texture cache =
 * a level change). */
#define TEXPACK_AHEAD_RING 2048          /* power of two */
static int aheadOn;                      /* render thread writes, game thread reads */
static uint32_t aheadEpoch = 1;          /* render thread bumps */
static int16_t aheadRing[TEXPACK_AHEAD_RING];
static uint32_t aheadHead;               /* game thread (producer) */
static uint32_t aheadTail;               /* render thread (consumer) */
static uint32_t aheadSentEpoch;          /* game thread */
static uint8_t aheadSent[TEXPACK_NUM_TEXTURES]; /* game thread */
static uint32_t statAheadDropped;        /* ring full (game thread) */

static void aheadNote(int32_t texnum)
{
    if (!__atomic_load_n(&aheadOn, __ATOMIC_RELAXED) || texnum < 0 || texnum >= TEXPACK_NUM_TEXTURES) {
        return;
    }
    const uint32_t ep = __atomic_load_n(&aheadEpoch, __ATOMIC_ACQUIRE);
    if (ep != aheadSentEpoch) {
        memset(aheadSent, 0, sizeof(aheadSent));
        aheadSentEpoch = ep;
    }
    if (aheadSent[texnum]) {
        return;
    }
    const uint32_t head = aheadHead;
    if (head - __atomic_load_n(&aheadTail, __ATOMIC_ACQUIRE) >= TEXPACK_AHEAD_RING) {
        statAheadDropped++; /* the draw-time path still finds it */
        return;
    }
    aheadRing[head & (TEXPACK_AHEAD_RING - 1)] = (int16_t)texnum;
    __atomic_store_n(&aheadHead, head + 1, __ATOMIC_RELEASE);
    aheadSent[texnum] = 1;
}

void texpackRegisterTexture(const void *data, int32_t texnum, uint32_t bytes)
{
    static int prof = -1;
    uint64_t t0 = 0;
    if (indexing || !data || texnum < 0) {
        return;
    }
    if (prof < 0) {
        const char *e = getenv("GE_TICKPROF");
        prof = (e != NULL && e[0] != '\0' && e[0] != '0');
    }
    if (prof) {
        t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    }
    texpackRegisterTextureNow(data, texnum, bytes);
    aheadNote(texnum); /* round 40: one relaxed load unless a pack decodes ahead */
    if (prof) {
        regCalls++;
        regNs += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    }
}

static void texpackRegisterTextureNow(const void *data, int32_t texnum, uint32_t bytes)
{
    if (!reg) {
        reg = calloc(TEXPACK_REG_SLOTS, sizeof(*reg));
        if (!reg) {
            return;
        }
    }
    uint32_t slot = regHash(data);
    struct regslot *victim = &reg[slot];
    for (int i = 0; i < TEXPACK_REG_PROBES; i++, slot = (slot + 1) & (TEXPACK_REG_SLOTS - 1)) {
        if (reg[slot].data == data || reg[slot].data == NULL) {
            victim = &reg[slot];
            break;
        }
    }
    /* A full probe run overwrites its first slot: an entry lost is a texture
     * drawn as itself, never a wrong one (the signature guards those). */
    const uint32_t n = bytes < TEXPACK_SIG_BYTES ? bytes : TEXPACK_SIG_BYTES;
    victim->data = data;
    victim->texnum = texnum;
    victim->siglen = n;
    victim->sig = sigOf((const uint8_t *)data, n);
}

static int32_t regLookup(const void *data, uint32_t bytes)
{
    if (!reg) {
        return -1;
    }
    uint32_t slot = regHash(data);
    for (int i = 0; i < TEXPACK_REG_PROBES; i++, slot = (slot + 1) & (TEXPACK_REG_SLOTS - 1)) {
        if (reg[slot].data == NULL) {
            return -1;
        }
        if (reg[slot].data == data) {
            /* The pool is reused stage to stage; an address texLoad() has not
             * written since may hold anything. The first bytes must still be
             * the ones it registered. */
            const uint32_t n = reg[slot].siglen;
            if (n > bytes || sigOf((const uint8_t *)data, n) != reg[slot].sig) {
                return -1;
            }
            return reg[slot].texnum;
        }
    }
    return -1;
}

/* D-102 (round 20a): the bullet-hit paths (bg.c, propobj.c) name the surface
 * they hit by the texture its G_SETTIMG points at. Same registry, same
 * signature guard: a stale pool address answers -1, never a wrong number. */
int32_t texpackTexnumAt(const void *data)
{
    if (!data) {
        return -1;
    }
    return regLookup(data, UINT32_MAX);
}

/* ------------------------------------------------------------ Rice checksum */

/*
 * The N64's every-other-row word swap (texSwapAltRowBytes, stubbed on the
 * port: D159), applied to a copy. A Rice checksum was taken over the swizzled
 * form an emulator sees in RDRAM.
 */
static void riceSwizzle(uint8_t *data, int32_t width, int32_t height, int32_t siz, uint32_t len)
{
    const int32_t wordsPerRow = siz == G_IM_SIZ_32b ? ((width + 3) & 0xffc)
            : siz == G_IM_SIZ_16b ? (((width + 3) & 0xffc) >> 1)
            : siz == G_IM_SIZ_8b ? (((width + 7) & 0xff8) >> 2)
            : (((width + 0xf) & 0xff0) >> 3);
    const int32_t step = siz == G_IM_SIZ_32b ? 4 : 2;

    for (int32_t y = 1; y < height; y += 2) {
        uint32_t *row = (uint32_t *)data + (uint32_t)y * wordsPerRow;
        for (int32_t x = 0; x + step <= wordsPerRow; x += step) {
            for (int32_t k = 0; k < step / 2; k++) {
                uint32_t *a = row + x + k;
                uint32_t *b = row + x + k + step / 2;
                if ((uint8_t *)(b + 1) > data + len) {
                    break;
                }
                const uint32_t tmp = *a;
                *a = *b;
                *b = tmp;
            }
        }
    }
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int texpackRiceCrc(const uint8_t *data, uint32_t len, int32_t width, int32_t height,
                   int32_t siz, int32_t stride, uint32_t *outCrc)
{
    const int32_t bytesPerWidth = ((width << siz) + 1) >> 1;
    uint32_t crc = 0;
    uint32_t wordHash = 0;
    int32_t row = 0;

    for (int32_t counter = height - 1; counter >= 0; counter--) {
        for (int32_t pos = bytesPerWidth - 4; pos >= 0; pos -= 4) {
            if ((uint32_t)(row + pos + 4) > len) {
                return 0;
            }
            wordHash = (uint32_t)pos ^ be32(data + row + pos);
            crc = wordHash + ((crc << 4) | (crc >> 28));
        }
        crc += (uint32_t)counter ^ wordHash;
        row += stride;
    }
    *outCrc = crc;
    return 1;
}

struct ricecrc {
    uint32_t crc;
    int32_t texnum; /* -1 = empty */
};

static struct ricecrc *riceIndex;
/* 0 not built, 1 built, -1 failed. Round 38: written by the game thread
 * (release), read by the render thread (acquire) - riceIndex is complete
 * before a reader can see 1. */
static int riceIndexState;
/* Round 38: the render thread met a Rice name before the index existed and
 * asks the game thread to build it (texpackGameThreadPoint). */
static volatile int riceWanted;
static volatile int reloadWanted = 1;   /* moved up from the index section (round 38) */

static void riceInsert(uint32_t crc, int32_t texnum)
{
    uint32_t slot = crc & (TEXPACK_RICE_SLOTS - 1);
    for (int i = 0; i < TEXPACK_RICE_SLOTS; i++, slot = (slot + 1) & (TEXPACK_RICE_SLOTS - 1)) {
        if (riceIndex[slot].texnum < 0) {
            riceIndex[slot].crc = crc;
            riceIndex[slot].texnum = texnum;
            return;
        }
        if (riceIndex[slot].crc == crc) {
            return; /* identical bytes: either draws the same */
        }
    }
}

static void riceBuildIndex(void)
{
    struct texcacheitem saved[150];
    const s32 savedCount = g_TexCacheCount;
    struct texpool pool;
    uint8_t *buffer;
    uint8_t *scratch;
    int count = 0;
    const uint64_t t0 = sysGetMicroseconds();

    /* Midas (round 11, reworked round 38): texLoad() is the game thread's
     * (its bitstream and the LOD ring are plain globals). This runs on the
     * game thread only: from rebuild() when the drawing thread IS the game
     * thread (render thread off), else from texpackGameThreadPoint(), which
     * the sim calls between frames (pcsched.c schedSubmitTask). Until it has
     * run, the render thread's index matches Rice-named files by the texels
     * drawn (findUnplaced). The table is filled before it is published, so
     * the render thread never reads a half-built one. */
    struct ricecrc *idx = malloc(TEXPACK_RICE_SLOTS * sizeof(*idx));
    /* the first 128 KB of the dedicated-bump scratch (texpackGuestScratch) */
    buffer = texpackGuestScratch();
    scratch = malloc(TEXPACK_RICE_SCRATCH);
    if (!idx || !buffer || !scratch) {
        free(idx);
        free(scratch);
        __atomic_store_n(&riceIndexState, -1, __ATOMIC_RELEASE);
        sysLogPrintf(LOG_ERROR, "texpack: no scratch for the checksum index");
        return;
    }
    riceIndex = idx;
    memset(riceIndex, 0xff, TEXPACK_RICE_SLOTS * sizeof(*riceIndex));

    /* Loading a texture appends to the LOD size ring the game reads for what
     * is on screen; walking the whole table would flush it, so it goes back. */
    memcpy(saved, g_TexCacheItems, sizeof(saved));
    indexing = 1;
    portHwLoadSuppress(1); /* Midas (round 11): not a console decode: no hw price */

    for (int32_t n = 0; n < g_NumTexturesPort; n++) {
        /* texLoad() copies the compressed bytes into a 4000-byte stack
         * buffer; the game never asks it for an entry that does not fit, the
         * whole-table walk would (the table's tail and unused rows). */
        {
            const s32 thisoff = *((s32 *)&g_Textures[n]) & 0xFFFFFF;
            const s32 nextoff = *((s32 *)&g_Textures[n + 1]) & 0xFFFFFF;
            const s32 copylen = (s32)((((u32)(nextoff - thisoff)) + 0x1f) >> 4 << 4);
            if (nextoff <= thisoff || copylen + 16 > 4000) {
                continue;
            }
        }
        /* 2496 is the ROM's one HUFFMANPERCHANNEL texture (compression 8,
         * docs/spike-texmap.md §2.1) and texInflateNonZlib() crashes on it
         * (round 19c, open in STATUS): the game never draws it in any stage
         * walked so far. Not indexed. */
        if (n == 2496) {
            continue;
        }
        texInitPool(&pool, buffer, TEXPACK_RICE_SCRATCH);
        texLoadFromTextureNum(n, &pool);
        struct tex *tex = texFindInPool(n, &pool);
        if (!tex || !tex->data) {
            continue;
        }
        int32_t stride = texGetLineSizeInBytes(tex, 0) * 8;
        int32_t size = texGetSizeInBytes(tex, 0) * 8;
        const int32_t width = texGetWidthAtLod(tex, 0);
        const int32_t height = texGetHeightAtLod(tex, 0);
        if (tex->depth == G_IM_SIZ_32b) {
            stride *= 2;
            size *= 2;
        }
        if (size <= 0 || size > TEXPACK_RICE_SCRATCH || width <= 0 || height <= 0 || stride <= 0) {
            continue;
        }
        memcpy(scratch, tex->data, (size_t)size);
        riceSwizzle(scratch, width, height, tex->depth, (uint32_t)size);
        uint32_t crc;
        if (texpackRiceCrc(scratch, (uint32_t)size, width, height, tex->depth, stride, &crc)) {
            riceInsert(crc, n);
            count++;
        }
    }

    portHwLoadSuppress(0);
    indexing = 0;
    memcpy(g_TexCacheItems, saved, sizeof(saved));
    g_TexCacheCount = savedCount;
    free(scratch);
    __atomic_store_n(&riceIndexState, 1, __ATOMIC_RELEASE);
    sysLogPrintf(LOG_NOTE, "texpack: checksummed %d ROM textures for a Rice-named pack in %.0f ms (%s)",
                 count, (double)(sysGetMicroseconds() - t0) / 1000.0,
                 portRenderAsyncActive() ? "game thread, render thread on" : "drawing thread");
}

/* Round 38 (R11-1): 1 while one of the two walks in this file is inside
 * texLoad(). image.c's persistent LOD table and frametiming.c's decode-cost
 * record test it: a walk loads ~2,700 textures the stage never draws, which
 * would fill the 512-entry LOD table (real textures then go unremembered)
 * and record decode prices for textures the stage has not loaded. */
int texpackIndexing(void)
{
    return indexing;
}

/* Round 38: the sim, between frames (pcsched.c schedSubmitTask, video build).
 * With the render thread on, the Rice checksum walk happens here, on the
 * game thread, the first time the render thread's index met a Rice-named
 * file; the render thread's next frame then re-indexes with the numbers.
 * One flag load otherwise. */
void texpackGameThreadPoint(void)
{
    if (!riceWanted) {
        return;
    }
    riceWanted = 0;
    if (__atomic_load_n(&riceIndexState, __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    riceBuildIndex();
    reloadWanted = 1;
}

/* ------------------------------------------- texture colours (D-223) */

void texGetDepthAndSize(struct tex *tex, s32 *deptharg, s32 *lenarg);

#define MEAN_5_8(v) ((uint8_t)(((v) << 3) | ((v) >> 2)))
#define MEAN_4_8(v) ((uint8_t)((v) * 17))
#define MEAN_3_8(v) ((uint8_t)(((v) << 5) | ((v) << 2) | ((v) >> 1)))

/* One N64 texel of `tex` (LOD 0) as RGBA8; 0 for a format not handled.
 * Texels and palette are big-endian, as texLoad() leaves them (fast3d reads
 * them so). A row's dword swizzle and the padding past the width only move
 * texels around within a row, which a mean does not see. */
static int texelRgba(const struct tex *tex, const uint8_t *row, int32_t x, const uint8_t *pal, uint8_t *dst)
{
    const int fmt = tex->gbiformat, siz = tex->depth;
    if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b) {
        const uint16_t c = (uint16_t)((row[x * 2] << 8) | row[x * 2 + 1]);
        dst[0] = MEAN_5_8(c >> 11);
        dst[1] = MEAN_5_8((c >> 6) & 0x1f);
        dst[2] = MEAN_5_8((c >> 1) & 0x1f);
        dst[3] = (c & 1) ? 255 : 0;
    } else if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_32b) {
        dst[0] = row[x * 4];
        dst[1] = row[x * 4 + 1];
        dst[2] = row[x * 4 + 2];
        dst[3] = row[x * 4 + 3];
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_16b) {
        dst[0] = dst[1] = dst[2] = row[x * 2];
        dst[3] = row[x * 2 + 1];
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_8b) {
        dst[0] = dst[1] = dst[2] = MEAN_4_8(row[x] >> 4);
        dst[3] = MEAN_4_8(row[x] & 0xf);
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_4b) {
        const uint8_t part = (uint8_t)((row[x / 2] >> (4 - (x % 2) * 4)) & 0xf);
        dst[0] = dst[1] = dst[2] = MEAN_3_8(part >> 1);
        dst[3] = (part & 1) ? 255 : 0;
    } else if (fmt == G_IM_FMT_I && siz == G_IM_SIZ_8b) {
        dst[0] = dst[1] = dst[2] = dst[3] = row[x];
    } else if (fmt == G_IM_FMT_I && siz == G_IM_SIZ_4b) {
        dst[0] = dst[1] = dst[2] = dst[3] = MEAN_4_8((row[x / 2] >> (4 - (x % 2) * 4)) & 0xf);
    } else if (fmt == G_IM_FMT_CI && pal) {
        const uint8_t idx = siz == G_IM_SIZ_4b ? (uint8_t)((row[x / 2] >> (4 - (x % 2) * 4)) & 0xf) : row[x];
        const uint16_t e = (uint16_t)((pal[idx * 2] << 8) | pal[idx * 2 + 1]);
        if (tex->lutmodeindex == 3) { /* IA16 palette */
            dst[0] = dst[1] = dst[2] = (uint8_t)(e >> 8);
            dst[3] = (uint8_t)e;
        } else {
            dst[0] = MEAN_5_8(e >> 11);
            dst[1] = MEAN_5_8((e >> 6) & 0x1f);
            dst[2] = MEAN_5_8((e >> 1) & 0x1f);
            dst[3] = (e & 1) ? 255 : 0;
        }
    } else {
        return 0;
    }
    return 1;
}

/*
 * The colour of each ROM texture in nums[] as drawn: mean r, g, b (0-255,
 * three a texture in out[]) over its texels at least half opaque (every texel
 * when none is), -1 for one that does not load. An intensity texture is taken
 * over every texel (its alpha IS its intensity; PD found Silo's I4 grate
 * measured twice as bright otherwise). PD texpackTextureMeans(), for
 * beanstage.c's brightness match (D-223). Game thread only: texLoad() and the
 * LOD size ring, which goes back as the Rice index walk puts it back. Its own
 * 128 KB of guest scratch, right above the Rice walk's (Midas: the second
 * half of texpackGuestScratch).
 */
void texpackTextureMeans(const int32_t *nums, int32_t count, float *out)
{
    struct texcacheitem saved[150];
    const s32 savedCount = g_TexCacheCount;
    struct texpool pool;
    static uint8_t *meanScratch;

    for (int32_t i = 0; i < count * 3; i++) {
        out[i] = -1.0f;
    }
    if (count <= 0) {
        return;
    }
    if (!meanScratch && texpackGuestScratch()) {
        meanScratch = texpackGuestScratch() + TEXPACK_RICE_SCRATCH;
    }
    if (!meanScratch) {
        return;
    }
    memcpy(saved, g_TexCacheItems, sizeof(saved));
    indexing = 1;
    portHwLoadSuppress(1); /* Midas (round 11): not a console decode: no hw price */
    for (int32_t i = 0; i < count; i++) {
        const int32_t n = nums[i];
        if (n < 0 || n >= g_NumTexturesPort || n == 2496) { /* 2496: see riceBuildIndex() */
            continue;
        }
        {
            const s32 thisoff = *((s32 *)&g_Textures[n]) & 0xFFFFFF;
            const s32 nextoff = *((s32 *)&g_Textures[n + 1]) & 0xFFFFFF;
            const s32 copylen = (s32)((((u32)(nextoff - thisoff)) + 0x1f) >> 4 << 4);
            if (nextoff <= thisoff || copylen + 16 > 4000) {
                continue;
            }
        }
        texInitPool(&pool, meanScratch, TEXPACK_RICE_SCRATCH);
        texLoadFromTextureNum(n, &pool);
        struct tex *tex = texFindInPool(n, &pool);
        if (!tex || !tex->data) {
            continue;
        }
        const int32_t wide = tex->depth == G_IM_SIZ_32b ? 2 : 1;
        const int32_t stride = texGetLineSizeInBytes(tex, 0) * 8 * wide;
        const int32_t size = texGetSizeInBytes(tex, 0) * 8 * wide;
        const int32_t width = texGetWidthAtLod(tex, 0);
        const int32_t height = stride > 0 ? size / stride : 0;
        const uint8_t *pal = NULL;
        if (width <= 0 || height <= 0 || size <= 0 || size > TEXPACK_RICE_SCRATCH) {
            continue;
        }
        if (tex->lutmodeindex) {
            s32 depth, len;
            texGetDepthAndSize(tex, &depth, &len);
            pal = tex->data + (size_t)len * 2; /* after every LOD, in 16-bit units (texWriteLoadToTmemAddr) */
        }
        double sum[3] = { 0 }, sumall[3] = { 0 };
        uint32_t num = 0, total = 0;
        int ok = 1;
        for (int32_t y = 0; y < height && ok; y++) {
            const uint8_t *row = tex->data + (size_t)stride * (size_t)y;
            for (int32_t x = 0; x < width; x++) {
                uint8_t px[4];
                if (!texelRgba(tex, row, x, pal, px)) {
                    ok = 0;
                    break;
                }
                const int opaque = tex->gbiformat == G_IM_FMT_I || px[3] >= 0x80;
                for (int c = 0; c < 3; c++) {
                    sumall[c] += px[c];
                    if (opaque) sum[c] += px[c];
                }
                num += (uint32_t)opaque;
                total++;
            }
        }
        for (int c = 0; ok && total && c < 3; c++) {
            out[i * 3 + c] = num ? (float)(sum[c] / num) : (float)(sumall[c] / total);
        }
    }
    portHwLoadSuppress(0);
    indexing = 0;
    memcpy(g_TexCacheItems, saved, sizeof(saved));
    g_TexCacheCount = savedCount;
}

static int32_t riceLookup(uint32_t crc)
{
    int state = __atomic_load_n(&riceIndexState, __ATOMIC_ACQUIRE);
    if (state == 0) {
        if (portRenderAsyncActive()) {
            /* Round 38: this is the render thread, and texLoad() is the game
             * thread's. Ask it (texpackGameThreadPoint); match by the texels
             * drawn until then. */
            riceWanted = 1;
            return -1;
        }
        riceBuildIndex();
        state = __atomic_load_n(&riceIndexState, __ATOMIC_ACQUIRE);
    }
    if (state < 0) {
        return -1;
    }
    uint32_t slot = crc & (TEXPACK_RICE_SLOTS - 1);
    for (int i = 0; i < TEXPACK_RICE_SLOTS; i++, slot = (slot + 1) & (TEXPACK_RICE_SLOTS - 1)) {
        if (riceIndex[slot].texnum < 0) {
            return -1;
        }
        if (riceIndex[slot].crc == crc) {
            return riceIndex[slot].texnum;
        }
    }
    return -1;
}

/* ------------------------------------------------------------- name parsing */

static int hexN(const char *s, int n, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        const char c = s[i];
        if (c >= '0' && c <= '9') v = (v << 4) | (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* "<ROM>#1135D097#3#1[#<palcrc>|#$]_all.png" -> crc, kind. */
static int parseRiceName(const char *name, uint32_t *crc, int *kind, int *isAlpha)
{
    for (const char *p = strchr(name, '#'); p; p = strchr(p + 1, '#')) {
        uint32_t fmt, siz, pal;
        if (!hexN(p + 1, 8, crc) || p[9] != '#' || !hexN(p + 10, 1, &fmt) || p[11] != '#'
                || !hexN(p + 12, 1, &siz)) {
            continue;
        }
        const char *rest = p + 13;
        if (*rest == '#' && hexN(rest + 1, 8, &pal)) {
            rest += 9;
        } else if (rest[0] == '#' && rest[1] == '$') {
            rest += 2; /* GLideN64: any palette */
        }
        if (*rest != '_') {
            continue;
        }
        rest++;
        *isAlpha = 0;
        if (!strcasecmp(rest, "all.png") || !strcasecmp(rest, "allciByRGBA.png")
                || !strcasecmp(rest, "ciByRGBA.png") || !strcasecmp(rest, "ci.png")) {
            *kind = KIND_ALL;
            return 1;
        }
        if (!strcasecmp(rest, "rgb.png")) {
            *kind = KIND_RGB;
            return 1;
        }
        if (!strcasecmp(rest, "a.png")) {
            *kind = KIND_RGB;
            *isAlpha = 1;
            return 1;
        }
    }
    return 0;
}

/* "0a9a.png" or "0a9a_anything.png" -> 0x0a9a, else -1. */
static int32_t parseNativeName(const char *name)
{
    uint32_t n;
    const size_t len = strlen(name);
    if (len < 8 || !hexN(name, 4, &n) || n >= TEXPACK_NUM_TEXTURES) {
        return -1;
    }
    const char *rest = name + 4;
    if (*rest == '_') {
        rest = strrchr(rest, '.');
        if (!rest) {
            return -1;
        }
    }
    return strcasecmp(rest, ".png") == 0 ? (int32_t)n : -1;
}

/* ------------------------------------------------------------------ the index */

struct unplaced {
    uint32_t crc;
    char *path;
};

static char *packPaths[TEXPACK_NUM_TEXTURES];
static uint8_t packKinds[TEXPACK_NUM_TEXTURES];
static char *xblaPaths[TEXPACK_NUM_TEXTURES];
static struct unplaced *unplacedTab;
static int numUnplaced;
static int numPackFiles;
static int numReplacements;
static int numXbla;
static int numXblaFonts;           /* round 43: the release's fonts in $C/xbla/font (0..2) */
static int packFlip;               /* bottomup.txt at the pack root */

static volatile uint32_t serial = 1;
static int haveAny;

static char packNames[TEXPACK_MAXPACKS][TEXPACK_NAMELEN];
static int numPacks;

static void keptClear(void);
static void asyncIndexChanged(void);

static void freeIndex(void)
{
    asyncIndexChanged(); /* round 40: before the strings the jobs are keyed by go */
    keptClear();
    for (int i = 0; i < TEXPACK_NUM_TEXTURES; i++) {
        free(packPaths[i]);
        free(xblaPaths[i]);
        packPaths[i] = NULL;
        xblaPaths[i] = NULL;
        packKinds[i] = KIND_NONE;
    }
    if (unplacedTab) {
        for (int i = 0; i < TEXPACK_UNPLACED_SLOTS; i++) {
            free(unplacedTab[i].path);
        }
        free(unplacedTab);
        unplacedTab = NULL;
    }
    numUnplaced = numPackFiles = numReplacements = numXbla = numXblaFonts = 0;
}

static void addUnplaced(uint32_t crc, const char *path)
{
    if (!unplacedTab) {
        unplacedTab = calloc(TEXPACK_UNPLACED_SLOTS, sizeof(*unplacedTab));
        if (!unplacedTab) {
            return;
        }
    }
    uint32_t slot = crc & (TEXPACK_UNPLACED_SLOTS - 1);
    for (int i = 0; i < TEXPACK_UNPLACED_SLOTS; i++, slot = (slot + 1) & (TEXPACK_UNPLACED_SLOTS - 1)) {
        if (!unplacedTab[slot].path) {
            unplacedTab[slot].crc = crc;
            unplacedTab[slot].path = strdup(path);
            numUnplaced++;
            return;
        }
        if (unplacedTab[slot].crc == crc) {
            return;
        }
    }
}

static const char *findUnplaced(uint32_t crc)
{
    if (!unplacedTab) {
        return NULL;
    }
    uint32_t slot = crc & (TEXPACK_UNPLACED_SLOTS - 1);
    for (int i = 0; i < TEXPACK_UNPLACED_SLOTS; i++, slot = (slot + 1) & (TEXPACK_UNPLACED_SLOTS - 1)) {
        if (!unplacedTab[slot].path) {
            return NULL;
        }
        if (unplacedTab[slot].crc == crc) {
            return unplacedTab[slot].path;
        }
    }
    return NULL;
}

static void indexFile(const char *dir, const char *name, int xbla)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    const int32_t native = parseNativeName(name);
    if (native >= 0) {
        if (xbla) {
            if (!xblaPaths[native]) {
                xblaPaths[native] = strdup(path);
                numXbla++;
            }
            return;
        }
        numPackFiles++;
        if (packKinds[native] > KIND_NATIVE) {
            if (!packPaths[native]) numReplacements++;
            free(packPaths[native]);
            packPaths[native] = strdup(path);
            packKinds[native] = KIND_NATIVE;
        }
        return;
    }
    if (xbla) {
        return;
    }
    uint32_t crc;
    int kind, isAlpha;
    if (!parseRiceName(name, &crc, &kind, &isAlpha) || isAlpha) {
        return;
    }
    numPackFiles++;
    const int32_t texnum = riceLookup(crc);
    if (texnum < 0) {
        addUnplaced(crc, path);
        return;
    }
    if (packKinds[texnum] > kind) {
        if (!packPaths[texnum]) numReplacements++;
        free(packPaths[texnum]);
        packPaths[texnum] = strdup(path);
        packKinds[texnum] = (uint8_t)kind;
    }
}

static void scanDir(const char *dir, int depth, int xbla)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth < TEXPACK_MAXDEPTH) {
                scanDir(path, depth + 1, xbla);
            }
        } else if (S_ISREG(st.st_mode)) {
            indexFile(dir, e->d_name, xbla);
        }
    }
    closedir(d);
}

static int cmpNames(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void listPacks(void)
{
    char root[1024];
    snprintf(root, sizeof(root), "%s", sysResolvePath(TEXPACK_DIR));
    numPacks = 0;
    DIR *d = opendir(root);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && numPacks < TEXPACK_MAXPACKS) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= TEXPACK_NAMELEN) {
            continue;
        }
        char path[1024];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(packNames[numPacks++], TEXPACK_NAMELEN, "%s", e->d_name);
        }
    }
    closedir(d);
    qsort(packNames, (size_t)numPacks, TEXPACK_NAMELEN, cmpNames);
}

static void rebuild(void)
{
    const uint64_t t0 = sysGetMicroseconds();
    freeIndex();
    listPacks();
    packFlip = 0;

    if (cfgPack[0]) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/%s", sysResolvePath(TEXPACK_DIR), cfgPack);
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            char marker[1100];
            snprintf(marker, sizeof(marker), "%s/bottomup.txt", dir);
            packFlip = stat(marker, &st) == 0;
            scanDir(dir, 0, 0);
            sysLogPrintf(LOG_NOTE, "texpack: pack '%s': %d files, %d textures by number, %d by texels only%s",
                         cfgPack, numPackFiles, numReplacements, numUnplaced,
                         packFlip ? " (bottom-up)" : "");
        } else {
            sysLogPrintf(LOG_WARNING, "texpack: pack '%s' is not in %s", cfgPack, sysResolvePath(TEXPACK_DIR));
        }
    }

    if (cfgXbla) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/tex", sysResolvePath(TEXPACK_XBLA_DIR));
        scanDir(dir, 0, 1);
        if (cfgXblaSame) {
            /* The release's same-size copies (D-097): the N64 art through
             * DXT1, off unless asked for. Behind the HD layer. */
            snprintf(dir, sizeof(dir), "%s/same", sysResolvePath(TEXPACK_XBLA_DIR));
            scanDir(dir, 0, 1);
        }
        if (numXbla) {
            sysLogPrintf(LOG_NOTE, "texpack: XBLA layer: %d textures", numXbla);
        }
    }
    /* Round 43 (doc 60): the release's two fonts, drawn over the ROM's glyphs
     * (xblafont.c). Forgotten and rescanned with the rest of the layer. */
    numXblaFonts = xblaFontRescan();
    if (!cfgXbla) {
        numXblaFonts = 0;
    }
    if (numXblaFonts) {
        sysLogPrintf(LOG_NOTE, "texpack: XBLA layer: %d of 2 fonts", numXblaFonts);
    }

    haveAny = numReplacements > 0 || numUnplaced > 0 || numXbla > 0 || numXblaFonts > 0;
    serial++;
    if (cfgPack[0] || numXbla) {
        sysLogPrintf(LOG_NOTE, "texpack: index built in %.1f ms", (double)(sysGetMicroseconds() - t0) / 1000.0);
    }
}

static char appliedPack[TEXPACK_NAMELEN];
static int appliedXbla = -1, appliedSame = -1;

static void ensureIndex(void)
{
    /* The keys can be written behind our back (Settings, the bridge's
     * `settings set`, an ini reload): a change in any of them is a reload. */
    if (appliedXbla != cfgXbla || appliedSame != cfgXblaSame || strcmp(appliedPack, cfgPack) != 0) {
        appliedXbla = cfgXbla;
        appliedSame = cfgXblaSame;
        snprintf(appliedPack, sizeof(appliedPack), "%s", cfgPack);
        reloadWanted = 1;
    }
    if (reloadWanted) {
        reloadWanted = 0;
        rebuild();
    }
}


/* ---------------------------------------------------------- decoded cache */

/* The renderer's texture cache misses the same texture many times a level
 * (palette changes, LRU), and a PNG decode is the expensive part. Decoded
 * images are kept by path, least recently used out, under a byte budget. */
#if defined(PLATFORM_IOS)
/* Round 38 (doc 51 §4): 48 -> 64 MB. One entry may take a quarter of the
 * budget, and at 48 MB that refused every picture above ~1773x1773, i.e.
 * every capped 2048 picture (16 MB) and the pack's 299 1760-wide gun-barrel
 * frames (12.4 MB): each texture-cache miss on one of them decoded the PNG
 * again. At 64 MB the largest capped picture is exactly a quarter. */
#define TEXPACK_KEEP_BUDGET (64u * 1024 * 1024)
_Static_assert(TEXPACK_KEEP_BUDGET / 4 >= 2048u * 2048u * 4u,
               "the iOS keep budget must hold one picture at the default Video.TexPackMaxDim");
#else
#define TEXPACK_KEEP_BUDGET (128u * 1024 * 1024)
#endif
#define TEXPACK_KEEP_SLOTS 512

static void flipRows(uint8_t *rgba, int32_t w, int32_t h);
static uint32_t statCapped, statLargestDecode;   /* round 38; moved up in round 40 */

struct kept {
    const char *path; /* the index's own string: the identity */
    uint8_t *rgba;
    int32_t w, h;
    uint32_t used;
    uint8_t undrawn;  /* round 40: decoded by the worker, not yet taken by a draw */
};

static struct kept kept[TEXPACK_KEEP_SLOTS];
static uint32_t keptBytes;
static uint32_t keptClock;
static uint32_t statDecodes, statKeptHits;
static uint64_t statDecodeUs;
/* Round 40: bytes of kept pictures the worker decoded that no draw has taken
 * yet (they count against the budget like the rest; the worker reads this to
 * stop decoding ahead when half the budget is undrawn). Render thread writes. */
static uint32_t keptUndrawnBytes;
static uint32_t statAheadWasted;   /* undrawn pictures evicted before any draw took them */

/* Round 40: pictures too big for the keep budget (> a quarter of it: the Mac's
 * uncapped 4096^2 = 64 MB) that the worker decoded for a draw. The sync path
 * never kept these either (it decoded again at every miss); here they are
 * held, at most two, so the waiting cache entry can take one. */
#define TEXPACK_BIG_HOLD 2
static struct kept bigHold[TEXPACK_BIG_HOLD];

static void keptDrop(struct kept *k)
{
    const uint32_t n = (uint32_t)k->w * (uint32_t)k->h * 4;
    if (k->undrawn) {
        keptUndrawnBytes -= n;
        statAheadWasted++;
    }
    free(k->rgba);
    k->rgba = NULL;
    k->path = NULL;
    k->undrawn = 0;
}

static void keptClear(void)
{
    for (int i = 0; i < TEXPACK_KEEP_SLOTS; i++) {
        free(kept[i].rgba);
        kept[i].rgba = NULL;
        kept[i].path = NULL;
        kept[i].undrawn = 0;
    }
    for (int i = 0; i < TEXPACK_BIG_HOLD; i++) {
        free(bigHold[i].rgba);
        bigHold[i].rgba = NULL;
        bigHold[i].path = NULL;
    }
    keptBytes = 0;
    keptUndrawnBytes = 0;
}

static struct kept *keptFind(const char *path)
{
    for (int i = 0; i < TEXPACK_KEEP_SLOTS; i++) {
        if (kept[i].path == path && kept[i].rgba) {
            return &kept[i];
        }
    }
    for (int i = 0; i < TEXPACK_BIG_HOLD; i++) {
        if (bigHold[i].path == path && bigHold[i].rgba) {
            return &bigHold[i];
        }
    }
    return NULL;
}

/* R40-7: every kept picture counts as drawn (a level change; asyncIndexChanged
 * also, though keptClear follows it there) */
static void keptMarkAllDrawn(void)
{
    for (int i = 0; i < TEXPACK_KEEP_SLOTS; i++) {
        kept[i].undrawn = 0;
    }
    keptUndrawnBytes = 0;
}

static uint8_t *keptGet(const char *path, int32_t *w, int32_t *h)
{
    struct kept *k = keptFind(path);
    if (!k) {
        return NULL;
    }
    const size_t n = (size_t)k->w * k->h * 4;
    uint8_t *copy = malloc(n);
    if (!copy) return NULL;
    memcpy(copy, k->rgba, n);
    k->used = ++keptClock;
    if (k->undrawn) {
        keptUndrawnBytes -= (uint32_t)n;
        k->undrawn = 0;
    }
    *w = k->w;
    *h = k->h;
    statKeptHits++;
    if (k >= bigHold && k < bigHold + TEXPACK_BIG_HOLD) {
        /* R40-8: a held picture (outside the budget) is for the one draw
         * that waited for it; once taken it goes, as the sync path never
         * kept it either */
        free(k->rgba);
        k->rgba = NULL;
        k->path = NULL;
    }
    return copy;
}

/* Takes ownership of rgba (freed here when it is not kept). */
static void keptPutOwned(const char *path, uint8_t *rgba, int32_t w, int32_t h, int undrawn)
{
    const uint32_t n = (uint32_t)w * (uint32_t)h * 4;
    if (n > TEXPACK_KEEP_BUDGET / 4) {
        if (!undrawn) {
            free(rgba);
            return;
        }
        /* round 40: a draw is waiting for it - hold it (oldest out) */
        /* R40-8: an empty slot first, else the oldest */
        int slot = -1;
        for (int i = 0; i < TEXPACK_BIG_HOLD && slot < 0; i++) {
            if (!bigHold[i].rgba) slot = i;
        }
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < TEXPACK_BIG_HOLD; i++) {
                if (bigHold[i].used < bigHold[slot].used) slot = i;
            }
        }
        free(bigHold[slot].rgba);
        bigHold[slot].rgba = rgba;
        bigHold[slot].path = path;
        bigHold[slot].w = w;
        bigHold[slot].h = h;
        bigHold[slot].used = ++keptClock;
        bigHold[slot].undrawn = 0; /* not budgeted */
        return;
    }
    while (keptBytes + n > TEXPACK_KEEP_BUDGET) {
        int lru = -1;
        for (int i = 0; i < TEXPACK_KEEP_SLOTS; i++) {
            if (kept[i].rgba && (lru < 0 || kept[i].used < kept[lru].used)) lru = i;
        }
        if (lru < 0) break;
        keptBytes -= (uint32_t)kept[lru].w * (uint32_t)kept[lru].h * 4;
        keptDrop(&kept[lru]);
    }
    int slot = -1;
    for (int i = 0; i < TEXPACK_KEEP_SLOTS && slot < 0; i++) {
        if (!kept[i].rgba) slot = i;
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < TEXPACK_KEEP_SLOTS; i++) {
            if (kept[i].used < kept[slot].used) slot = i;
        }
        keptBytes -= (uint32_t)kept[slot].w * (uint32_t)kept[slot].h * 4;
        keptDrop(&kept[slot]);
    }
    kept[slot].rgba = rgba;
    kept[slot].path = path;
    kept[slot].w = w;
    kept[slot].h = h;
    kept[slot].used = ++keptClock;
    kept[slot].undrawn = undrawn ? 1 : 0;
    keptBytes += n;
    if (undrawn) {
        keptUndrawnBytes += n;
    }
}

static void keptPut(const char *path, const uint8_t *rgba, int32_t w, int32_t h)
{
    const uint32_t n = (uint32_t)w * (uint32_t)h * 4;
    if (n > TEXPACK_KEEP_BUDGET / 4) return;
    uint8_t *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, rgba, n);
    keptPutOwned(path, copy, w, h, 0);
}

/* ------------------------------------------------ decode worker (round 40)
 *
 * One thread, ge-texpack, at utility QoS, decodes pack PNGs; the render thread
 * never does (Video.TexPackAsync=1). The job table is the only state the two
 * share, under jobLock:
 *
 *   QUEUED  the render thread asked for it (a draw missed on it, or the game
 *           thread registered its texture number: decode ahead)
 *   BUSY    the worker is decoding it; nothing but the worker touches the
 *           job's file string, and the render thread frees nothing of a
 *           BUSY job
 *   DONE    decoded (rgba, w, h) or failed (rgba NULL, a tombstone so the
 *           same file is not asked for again until the index changes)
 *
 * A job is keyed by the index's own path string (compared, never
 * dereferenced off the render thread) AND the index generation it was asked
 * under. The worker opens its own copy of the path. Every rebuild of the
 * index (pack selected, deselected, rescanned, the Rice index arriving)
 * calls asyncIndexChanged() before it frees the strings: queued and done jobs
 * are freed, a BUSY job is left to finish and is freed, unseen, when it lands
 * (its generation no longer matches). That is the round 22b class of bug
 * (a result landing on state that changed under it) closed by construction:
 * nothing the worker produces reaches the keep cache or a texture unless its
 * generation is the live one, and the worker owns no pointer into the index.
 *
 * Results land in the keep cache at the top of the render thread's frame
 * (texpackFrame); the renderer's waiting cache entries then take them
 * (texpackTicketPoll) and gfx_pc.cpp swaps the picture into the live entry.
 * The worker never touches GL. */
#define TEXPACK_JOBS 256
enum { JOB_FREE = 0, JOB_QUEUED, JOB_BUSY, JOB_DONE };

struct tpjob {
    const char *key;     /* the index's path string: identity only */
    char *file;          /* the worker's own copy */
    uint32_t gen;
    uint32_t order;      /* higher = asked for more recently */
    int32_t maxDim;
    uint8_t state, draw, flip, failed;
    uint8_t *rgba;
    int32_t w, h, srcW, srcH;
};

static struct tpjob jobs[TEXPACK_JOBS];
static pthread_mutex_t jobLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobCond = PTHREAD_COND_INITIALIZER;
static int workerState;          /* 0 not started, 1 running, -1 could not start */
static uint32_t jobOrder;
static uint32_t indexGen = 1;    /* render thread: bumped by every index change */
static uint32_t jobDoneBytes;    /* under jobLock: decoded, not yet landed */
static uint32_t jobUndrawnSnap;  /* under jobLock: keptUndrawnBytes as of the last texpackFrame */
static int jobAheadAllowed;      /* under jobLock: cfgAhead as of the last texpackFrame */
/* stats: requests accepted, worker decodes, results thrown away (stale), and
 * the worker's time; all under jobLock */
static uint32_t statJobQueued, statJobAhead, statJobDecoded, statJobDiscarded, statJobFailed;
static uint64_t statJobUs;

static struct tpjob *jobFindLocked(const char *key, uint32_t gen)
{
    for (int i = 0; i < TEXPACK_JOBS; i++) {
        if (jobs[i].state != JOB_FREE && jobs[i].key == key && jobs[i].gen == gen) {
            return &jobs[i];
        }
    }
    return NULL;
}

static void jobFreeLocked(struct tpjob *j)
{
    free(j->file);
    free(j->rgba);
    memset(j, 0, sizeof(*j));
}

static void *workerMain(void *arg)
{
    (void)arg;
#if defined(__APPLE__)
    pthread_setname_np("ge-texpack");
#endif
    pthread_mutex_lock(&jobLock);
    for (;;) {
        /* draw requests first (newest first), then decode-ahead (newest
         * first: the texture registered last is the one about to be drawn),
         * the latter only while less than half the keep budget is decoded
         * and undrawn */
        struct tpjob *best = NULL;
        const int ahead = jobAheadAllowed && jobUndrawnSnap + jobDoneBytes < TEXPACK_KEEP_BUDGET / 2;
        for (int i = 0; i < TEXPACK_JOBS; i++) {
            struct tpjob *j = &jobs[i];
            if (j->state != JOB_QUEUED || (!j->draw && !ahead)) {
                continue;
            }
            if (!best || j->draw > best->draw || (j->draw == best->draw && j->order > best->order)) {
                best = j;
            }
        }
        if (!best) {
            pthread_cond_wait(&jobCond, &jobLock);
            continue;
        }
        best->state = JOB_BUSY;
        const char *file = best->file;
        const int32_t maxDim = best->maxDim;
        const int flip = best->flip;
        pthread_mutex_unlock(&jobLock);

        const uint64_t t0 = sysGetMicroseconds();
        int32_t w = 0, h = 0, srcW = 0, srcH = 0;
        uint8_t *rgba = pngReadMaxDim(file, maxDim, &w, &h, &srcW, &srcH);
        if (rgba && flip) {
            flipRows(rgba, w, h);
        }
        const uint64_t us = sysGetMicroseconds() - t0;

        pthread_mutex_lock(&jobLock);
        best->rgba = rgba;
        best->w = w;
        best->h = h;
        best->srcW = srcW;
        best->srcH = srcH;
        best->failed = rgba == NULL;
        best->state = JOB_DONE;
        statJobUs += us;
        if (rgba) {
            statJobDecoded++;
            jobDoneBytes += (uint32_t)w * (uint32_t)h * 4;
        } else {
            statJobFailed++;
        }
    }
    return NULL;
}

static int asyncActive(void)
{
    return cfgAsync && workerState >= 0;
}

static void workerStartLocked(void)
{
    if (workerState != 0) {
        return;
    }
    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_UTILITY, 0);
#endif
    if (pthread_create(&th, &attr, workerMain, NULL) == 0) {
        workerState = 1;
        sysLogPrintf(LOG_NOTE, "texpack: decode worker ge-texpack started (Video.TexPackAsync 1, utility QoS)");
    } else {
        workerState = -1;
        sysLogPrintf(LOG_ERROR, "texpack: could not start the decode worker; decoding on the render thread");
    }
    pthread_attr_destroy(&attr);
}

/* Render thread. Ask for `path` (the index's string) to be decoded. 1 when a
 * job for it is queued, decoding or done; 0 when the table is full. A draw
 * request promotes an ahead job; an ahead request never pushes out a draw
 * request, and a draw request pushes out the oldest queued ahead job. */
static int jobRequest(const char *path, int draw)
{
    int ok = 0;
    pthread_mutex_lock(&jobLock);
    workerStartLocked();
    if (workerState < 0) {
        pthread_mutex_unlock(&jobLock);
        return 0;
    }
    struct tpjob *j = jobFindLocked(path, indexGen);
    if (j) {
        if (draw && j->state == JOB_QUEUED && !j->draw) {
            j->draw = 1;
            j->order = ++jobOrder;
            pthread_cond_signal(&jobCond);
        }
        pthread_mutex_unlock(&jobLock);
        return 1;
    }
    struct tpjob *slot = NULL;
    for (int i = 0; i < TEXPACK_JOBS && !slot; i++) {
        if (jobs[i].state == JOB_FREE) slot = &jobs[i];
    }
    if (!slot) {
        /* R40-6: the oldest failed tombstone. A pack with many bad files
         * would otherwise fill the table for good; losing a tombstone only
         * means that file may be tried (and fail) once more. */
        for (int i = 0; i < TEXPACK_JOBS; i++) {
            struct tpjob *c = &jobs[i];
            if (c->state == JOB_DONE && c->failed && (!slot || c->order < slot->order)) slot = c;
        }
        if (slot) {
            jobFreeLocked(slot);
        }
    }
    if (!slot) {
        /* the oldest queued ahead job (or, for an ahead request, nothing) */
        for (int i = 0; i < TEXPACK_JOBS; i++) {
            struct tpjob *c = &jobs[i];
            if (c->state == JOB_QUEUED && !c->draw && (!slot || c->order < slot->order)) slot = c;
        }
        if (slot && (draw || slot->order + TEXPACK_JOBS / 2 < jobOrder)) {
            jobFreeLocked(slot);
        } else {
            slot = NULL;
        }
    }
    if (slot) {
        slot->file = strdup(path);
        if (slot->file) {
            slot->key = path;
            slot->gen = indexGen;
            slot->order = ++jobOrder;
            slot->maxDim = cfgMaxDim;
            slot->flip = (uint8_t)packFlip;
            slot->draw = (uint8_t)(draw ? 1 : 0);
            slot->state = JOB_QUEUED;
            statJobQueued++;
            if (!draw) statJobAhead++;
            pthread_cond_signal(&jobCond);
            ok = 1;
        }
    }
    pthread_mutex_unlock(&jobLock);
    return ok;
}

/* rebuild()/freeIndex(), render thread: see the block comment above. */
static void asyncIndexChanged(void)
{
    keptMarkAllDrawn(); /* R40-7 */
    pthread_mutex_lock(&jobLock);
    for (int i = 0; i < TEXPACK_JOBS; i++) {
        struct tpjob *j = &jobs[i];
        if (j->state == JOB_QUEUED) {
            jobFreeLocked(j);
        } else if (j->state == JOB_DONE) {
            if (j->rgba) {
                jobDoneBytes -= (uint32_t)j->w * (uint32_t)j->h * 4;
                statJobDiscarded++;
            }
            jobFreeLocked(j);
        }
        /* BUSY: the worker's; freed when it lands under a stale generation */
    }
    indexGen++;
    pthread_mutex_unlock(&jobLock);
    __atomic_add_fetch(&aheadEpoch, 1, __ATOMIC_RELEASE);
}

/* Render thread, top of a frame: finished decodes into the keep cache
 * (stale ones freed). */
static void jobCollect(void)
{
    struct landed { const char *key; uint8_t *rgba; int32_t w, h, srcW, srcH; } got[16];
    int n;
    do {
        n = 0;
        pthread_mutex_lock(&jobLock);
        for (int i = 0; i < TEXPACK_JOBS && n < 16; i++) {
            struct tpjob *j = &jobs[i];
            if (j->state != JOB_DONE || j->failed) {
                continue;
            }
            jobDoneBytes -= (uint32_t)j->w * (uint32_t)j->h * 4;
            if (j->gen != indexGen) {
                statJobDiscarded++;
                jobFreeLocked(j);
                continue;
            }
            got[n].key = j->key;
            got[n].rgba = j->rgba;
            got[n].w = j->w;
            got[n].h = j->h;
            got[n].srcW = j->srcW;
            got[n].srcH = j->srcH;
            n++;
            j->rgba = NULL;
            jobFreeLocked(j);
        }
        /* failed jobs under a stale generation go too */
        for (int i = 0; i < TEXPACK_JOBS; i++) {
            if (jobs[i].state == JOB_DONE && jobs[i].failed && jobs[i].gen != indexGen) {
                jobFreeLocked(&jobs[i]);
            }
        }
        pthread_mutex_unlock(&jobLock);
        for (int k = 0; k < n; k++) {
            const uint32_t bytes = (uint32_t)got[k].w * (uint32_t)got[k].h * 4u;
            if (got[k].w != got[k].srcW || got[k].h != got[k].srcH) {
                if (statCapped++ < 16) {
                    const char *base = strrchr(got[k].key, '/');
                    sysLogPrintf(LOG_NOTE, "texpack: %s %dx%d decoded at %dx%d (Video.TexPackMaxDim %d)",
                                 base ? base + 1 : got[k].key, got[k].srcW, got[k].srcH, got[k].w, got[k].h,
                                 cfgMaxDim);
                }
            }
            if (bytes > statLargestDecode) {
                statLargestDecode = bytes;
            }
            statDecodes++;
            keptPutOwned(got[k].key, got[k].rgba, got[k].w, got[k].h, 1);
        }
    } while (n == 16);
}

/* Render thread, top of a frame (gfx_pc.cpp, the pump slot): the index, the
 * decode-ahead numbers the game thread registered, finished decodes. */
void texpackFrame(void)
{
    ensureIndex();
    const int ahead = asyncActive() && cfgAhead && numReplacements > 0;
    __atomic_store_n(&aheadOn, ahead, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&aheadHead, __ATOMIC_ACQUIRE);
    uint32_t tail = aheadTail;
    for (; tail != head; tail++) {
        const int32_t texnum = aheadRing[tail & (TEXPACK_AHEAD_RING - 1)];
        if (!ahead || texnum < 0 || texnum >= TEXPACK_NUM_TEXTURES || !packPaths[texnum]) {
            continue;
        }
        if (keptFind(packPaths[texnum])) {
            continue;
        }
        (void)jobRequest(packPaths[texnum], 0);
    }
    __atomic_store_n(&aheadTail, tail, __ATOMIC_RELEASE);
    if (workerState > 0) {
        jobCollect();
        pthread_mutex_lock(&jobLock);
        jobUndrawnSnap = keptUndrawnBytes;
        jobAheadAllowed = cfgAhead && cfgAsync;
        /* the line may have moved (draws took undrawn pictures): let the
         * worker look again if anything is queued */
        for (int i = 0; i < TEXPACK_JOBS; i++) {
            if (jobs[i].state == JOB_QUEUED) {
                pthread_cond_signal(&jobCond);
                break;
            }
        }
        pthread_mutex_unlock(&jobLock);
    }
}

/* gfx_pc.cpp gfx_texture_cache_clear (render thread): a level change, a pack
 * change, a filter change. Textures the next level registers are decoded
 * ahead again even if this one already asked for them. */
void texpackCacheCleared(void)
{
    /* R40-7: pictures the last level decoded ahead and never drew stay kept
     * (LRU), but stop counting as undrawn: otherwise they hold the worker's
     * decode-ahead line shut for the next level */
    keptMarkAllDrawn();
    __atomic_add_fetch(&aheadEpoch, 1, __ATOMIC_RELEASE);
}

int texpackTicketPoll(const texpackticket *t, uint8_t **outRgba, int32_t *outW, int32_t *outH)
{
    if (!t || !t->key || t->gen != indexGen) {
        return -1;
    }
    uint8_t *rgba = keptGet((const char *)t->key, outW, outH);
    if (rgba) {
        *outRgba = rgba;
        return 1;
    }
    pthread_mutex_lock(&jobLock);
    struct tpjob *j = jobFindLocked((const char *)t->key, t->gen);
    const int failed = j && j->state == JOB_DONE && j->failed;
    pthread_mutex_unlock(&jobLock);
    if (failed) {
        return -1;
    }
    if (j) {
        return 0;
    }
    /* landed and evicted (or pushed out of the table) before this entry took
     * it: ask again */
    (void)jobRequest((const char *)t->key, 1);
    return 0;
}

void texpackGetAsyncStats(uint32_t *queued, uint32_t *ahead, uint32_t *decoded, uint32_t *discarded,
                          uint32_t *failed, uint64_t *workerUs, uint32_t *waiting, uint32_t *wasted)
{
    uint32_t wait = 0;
    pthread_mutex_lock(&jobLock);
    for (int i = 0; i < TEXPACK_JOBS; i++) {
        wait += jobs[i].state == JOB_QUEUED || jobs[i].state == JOB_BUSY;
    }
    if (queued) *queued = statJobQueued;
    if (ahead) *ahead = statJobAhead;
    if (decoded) *decoded = statJobDecoded;
    if (discarded) *discarded = statJobDiscarded;
    if (failed) *failed = statJobFailed;
    if (workerUs) *workerUs = statJobUs;
    pthread_mutex_unlock(&jobLock);
    if (waiting) *waiting = wait;
    if (wasted) *wasted = statAheadWasted;
}

int texpackAsyncEnabled(void)
{
    return asyncActive();
}

void texpackGetStats(uint32_t *decodes, uint32_t *keptHits, uint64_t *decodeUs, uint32_t *keptMb)
{
    if (decodes) *decodes = statDecodes;
    if (keptHits) *keptHits = statKeptHits;
    if (decodeUs) *decodeUs = statDecodeUs;
    if (keptMb) *keptMb = keptBytes >> 20;
}

/* Round 38: pictures decoded smaller than authored (Video.TexPackMaxDim), the
 * cap in force, and the largest decode so far in RGBA bytes. */
void texpackGetCapStats(uint32_t *capped, int32_t *maxDim, uint32_t *largestKb)
{
    if (capped) *capped = statCapped;
    if (maxDim) *maxDim = cfgMaxDim;
    if (largestKb) *largestKb = statLargestDecode >> 10;
}

/* ------------------------------------------------------- Bean pictures */

/* Tier C (round C1): stand-in tile address -> pinned decoded picture. A
 * flat list, linear search: one level's pictures (Dam 73), looked up only on
 * a texture-cache miss. Registered on the game thread at a level's build,
 * read by the renderer: under a lock. */
#define TEXPACK_BEAN_MAX 1024

struct beanpic {
    const void *addr;
    uint8_t *rgba;
    int32_t w, h;
    int owned;   /* round 35a (D-317): 0 = another entry's pixels (a stand-in for a picture decoded ahead) */
};

static struct beanpic beanPics[TEXPACK_BEAN_MAX];
static volatile int numBeanPics;
static volatile uint32_t beanGen = 1;   /* round 35a: bumped by every forget that had pictures */
static uint32_t beanBytes;
static pthread_mutex_t beanLock = PTHREAD_MUTEX_INITIALIZER;

/* under beanLock: is `rgba` an entry's own pixels? */
static int beanOwnsLocked(const uint8_t *rgba)
{
    for (int i = 0; rgba && i < numBeanPics; i++) {
        if (beanPics[i].owned && beanPics[i].rgba == rgba) {
            return 1;
        }
    }
    return 0;
}

uint32_t texpackRegisterBeanPictureGen(const void *addr, uint8_t *rgba, int32_t w, int32_t h)
{
    uint32_t gen = 0;
    pthread_mutex_lock(&beanLock);
    /* round 35a (D-317): pixels already pinned (decoded ahead by
     * beanprefetch.c and registered then) are shared, not taken */
    const int shared = beanOwnsLocked(rgba);
    if (!addr || !rgba || w <= 0 || h <= 0) {
        pthread_mutex_unlock(&beanLock);
        if (!shared) {
            free(rgba);
        }
        return 0;
    }
    if (numBeanPics < TEXPACK_BEAN_MAX) {
        beanPics[numBeanPics].addr = addr;
        beanPics[numBeanPics].rgba = rgba;
        beanPics[numBeanPics].w = w;
        beanPics[numBeanPics].h = h;
        beanPics[numBeanPics].owned = !shared;
        if (!shared) {
            beanBytes += (uint32_t)w * (uint32_t)h * 4;
        }
        numBeanPics++;
        gen = beanGen;
        rgba = NULL;
    }
    pthread_mutex_unlock(&beanLock);
    if (!shared) {
        free(rgba);
    }
    return gen;
}

void texpackRegisterBeanPicture(const void *addr, uint8_t *rgba, int32_t w, int32_t h)
{
    (void)texpackRegisterBeanPictureGen(addr, rgba, w, h);
}

void texpackBeanPictureRelease(uint8_t *rgba)
{
    if (!rgba) {
        return;
    }
    pthread_mutex_lock(&beanLock);
    const int pinned = beanOwnsLocked(rgba);
    pthread_mutex_unlock(&beanLock);
    if (!pinned) {
        free(rgba);
    }
}

void texpackForgetBeanPictures(void)
{
    pthread_mutex_lock(&beanLock);
    const int had = numBeanPics;
    for (int i = 0; i < numBeanPics; i++) {
        if (beanPics[i].owned) {
            free(beanPics[i].rgba);
        }
        beanPics[i].rgba = NULL;
        beanPics[i].addr = NULL;
    }
    numBeanPics = 0;
    beanBytes = 0;
    if (had) {
        beanGen++;
    }
    pthread_mutex_unlock(&beanLock);
    if (had) {
        serial++; /* the renderer drops its texture cache */
    }
}

void texpackBeanPictureStats(int32_t *count, uint32_t *bytes)
{
    pthread_mutex_lock(&beanLock);
    if (count) *count = numBeanPics;
    if (bytes) *bytes = beanBytes;
    pthread_mutex_unlock(&beanLock);
}

uint32_t texpackBeanGeneration(void)
{
    return beanGen;
}

int texpackPicturePrewarm(void)
{
    return cfgPicturePrewarm;
}

int texpackTexCacheMax(void)
{
    return cfgTexCacheMax < 256 ? 256 : cfgTexCacheMax;
}

int texpackPrewarmDraw(void)
{
    return cfgPrewarmDraw;
}

int texpackBeanVisit(uint32_t *gen, int32_t *next, texpackbeanfn fn, void *ctx)
{
    int n = 0;
    pthread_mutex_lock(&beanLock);
    if (*gen != beanGen) {
        *gen = beanGen;
        *next = 0;
        pthread_mutex_unlock(&beanLock);
        return -1;
    }
    for (; *next < numBeanPics; (*next)++, n++) {
        const struct beanpic *p = &beanPics[*next];
        fn(ctx, p->addr, p->rgba, p->w, p->h);
    }
    pthread_mutex_unlock(&beanLock);
    return n;
}

int texpackBeanWith(const void *addr, texpackbeanfn fn, void *ctx)
{
    int found = 0;
    if (!addr || !numBeanPics) {
        return 0;
    }
    pthread_mutex_lock(&beanLock);
    for (int i = 0; i < numBeanPics; i++) {
        if (beanPics[i].addr == addr) {
            fn(ctx, addr, beanPics[i].rgba, beanPics[i].w, beanPics[i].h);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&beanLock);
    return found;
}

static uint8_t *beanLookup(const void *data, int32_t *outW, int32_t *outH)
{
    uint8_t *copy = NULL;
    if (!numBeanPics) {
        return NULL;
    }
    pthread_mutex_lock(&beanLock);
    for (int i = 0; i < numBeanPics; i++) {
        if (beanPics[i].addr == data) {
            const size_t n = (size_t)beanPics[i].w * beanPics[i].h * 4;
            copy = malloc(n);
            if (copy) {
                memcpy(copy, beanPics[i].rgba, n);
                *outW = beanPics[i].w;
                *outH = beanPics[i].h;
            }
            break;
        }
    }
    pthread_mutex_unlock(&beanLock);
    return copy;
}

/* ------------------------------------------------------------------ public */

int texpackActive(void)
{
    ensureIndex();
    return haveAny || numBeanPics > 0;
}

uint32_t texpackSerial(void)
{
    ensureIndex();
    return serial;
}

void texpackReload(void)
{
    reloadWanted = 1;
}

static void flipRows(uint8_t *rgba, int32_t w, int32_t h)
{
    const size_t row = (size_t)w * 4;
    uint8_t *tmp = malloc(row);
    if (!tmp) return;
    for (int32_t y = 0; y < h / 2; y++) {
        uint8_t *a = rgba + (size_t)y * row;
        uint8_t *b = rgba + (size_t)(h - 1 - y) * row;
        memcpy(tmp, a, row);
        memcpy(a, b, row);
        memcpy(b, tmp, row);
    }
    free(tmp);
}

static uint8_t *loadReplacement(const void *data, uint32_t bytes, int32_t tileW, int32_t tileH,
                                int32_t siz, uint32_t rowBytes, int32_t *outW, int32_t *outH,
                                char *outSource, texpackticket *ticket);

uint8_t *texpackLoadReplacement(const void *data, uint32_t bytes, int32_t tileW, int32_t tileH,
                                int32_t siz, uint32_t rowBytes, int32_t *outW, int32_t *outH,
                                char *outSource)
{
    return loadReplacement(data, bytes, tileW, tileH, siz, rowBytes, outW, outH, outSource, NULL);
}

uint8_t *texpackLoadReplacementAsync(const void *data, uint32_t bytes, int32_t tileW, int32_t tileH,
                                     int32_t siz, uint32_t rowBytes, int32_t *outW, int32_t *outH,
                                     char *outSource, texpackticket *ticket)
{
    if (ticket) {
        ticket->key = NULL;
        ticket->gen = 0;
    }
    return loadReplacement(data, bytes, tileW, tileH, siz, rowBytes, outW, outH, outSource,
                           asyncActive() ? ticket : NULL);
}

static uint8_t *loadReplacement(const void *data, uint32_t bytes, int32_t tileW, int32_t tileH,
                                int32_t siz, uint32_t rowBytes, int32_t *outW, int32_t *outH,
                                char *outSource, texpackticket *ticket)
{
    ensureIndex();
    if (data && numBeanPics > 0) {
        uint8_t *pic = beanLookup(data, outW, outH);
        if (pic) {
            if (outSource) {
                *outSource = 'b';
            }
            return pic;
        }
    }
    if (!haveAny || !data) {
        return NULL;
    }
    const char *path = NULL;
    int fromPack = 0;
    const int32_t texnum = regLookup(data, bytes);

    if (texnum >= 0 && texnum < TEXPACK_NUM_TEXTURES) {
        if (packPaths[texnum]) {
            path = packPaths[texnum];
            fromPack = 1;
        } else if (xblaPaths[texnum]) {
            path = xblaPaths[texnum];
        }
    }

    if (!path && numUnplaced > 0 && bytes > 0 && bytes <= TEXPACK_RICE_SCRATCH && tileW > 0 && tileH > 0) {
        /* A pack file whose checksum named no ROM texture: match the texels
         * being drawn, the way the emulator the pack was made for did. */
        uint8_t *copy = malloc(bytes);
        if (copy) {
            uint32_t crc;
            memcpy(copy, data, bytes);
            riceSwizzle(copy, tileW, tileH, siz, bytes);
            if (texpackRiceCrc(copy, bytes, tileW, tileH, siz, (int32_t)rowBytes, &crc)) {
                path = findUnplaced(crc);
                fromPack = path != NULL;
            }
            free(copy);
        }
        /* Round 38 (doc 51 §2.4): texels that never went through texLoad()
         * - the font glyphs above all - were never swizzled in the N64's
         * RDRAM either, so the emulator's checksum is over them as they are.
         * Tried second: the ROM-texture match above stays first. */
        if (!path) {
            uint32_t crc;
            if (texpackRiceCrc((const uint8_t *)data, bytes, tileW, tileH, siz, (int32_t)rowBytes, &crc)) {
                path = findUnplaced(crc);
                fromPack = path != NULL;
            }
        }
        /* Round 38 (doc 51 §2.4): font glyphs. Rare pads each glyph row to a
         * multiple of 8 by repeating its last column, and Midas clears that
         * padding at load (textrelated.c portFontClearPadding: half-texel
         * filtering sampled it). An emulator's checksum covers the padding as
         * the cart has it, so put it back on a copy: for each width the zero
         * columns at the right of an 8-bit tile allow, repeat that width's
         * last column. 8-bit tiles of at most 32x64 only (glyphs are 8 or 16
         * wide); a texture-cache miss, at most 7 small checksums. */
        if (!path && siz == G_IM_SIZ_8b && tileW >= 8 && tileW <= 32 && (tileW & 7) == 0 && tileH > 0
                && tileH <= 64 && rowBytes == (uint32_t)tileW && bytes >= (uint32_t)(tileW * tileH)) {
            const uint8_t *src = (const uint8_t *)data;
            uint8_t padded[32 * 64];
            for (int32_t w = tileW - 1; w >= tileW - 7 && w >= 1 && !path; w--) {
                int32_t y, x;
                int clear = 1;
                for (y = 0; y < tileH && clear; y++) {
                    for (x = w; x < tileW; x++) {
                        if (src[y * tileW + x] != 0) {
                            clear = 0;
                            break;
                        }
                    }
                }
                if (!clear) {
                    break; /* a narrower width has ink in its padding: none can be it */
                }
                for (y = 0; y < tileH; y++) {
                    memcpy(padded + y * tileW, src + y * tileW, (size_t)w);
                    memset(padded + y * tileW + w, src[y * tileW + w - 1], (size_t)(tileW - w));
                }
                uint32_t crc;
                if (texpackRiceCrc(padded, (uint32_t)(tileW * tileH), tileW, tileH, siz, tileW, &crc)) {
                    path = findUnplaced(crc);
                    fromPack = path != NULL;
                }
            }
        }
    }

    /* Round 43 (doc 60): a font glyph the pack has no picture for is the
     * release's, when the XBLA layer holds its font (a pack outranks it, glyph
     * by glyph, as it outranks the numbered layer). */
    int32_t gfont = -1, gindex = -1;
    if (!path && numXblaFonts > 0 && siz == G_IM_SIZ_8b && !xblaFontFind(data, bytes, &gfont, &gindex)) {
        gfont = gindex = -1;
    }
    {
        static int logOn = -1;
        static int logged;
        if (logOn < 0) logOn = getenv("GE_TEXPACKLOG") != NULL;
        if (logOn && logged < 4000) {
            logged++;
            if (gfont >= 0) {
                sysLogPrintf(LOG_NOTE, "texpack: lookup %p bytes=%u tile=%dx%d siz=%d -> XBLA font %d glyph '%c'",
                             data, bytes, tileW, tileH, siz, gfont, gindex + 0x21);
            } else {
                sysLogPrintf(LOG_NOTE, "texpack: lookup %p bytes=%u tile=%dx%d siz=%d texnum=%d -> %s",
                             data, bytes, tileW, tileH, siz, texnum, path ? path : "-");
            }
        }
    }
    if (!path) {
        if (gfont >= 0) {
            uint8_t *pic = xblaFontPicture(gfont, gindex, outW, outH);
            if (pic) {
                if (outSource) {
                    *outSource = 'g';
                }
                return pic;
            }
        }
        return NULL;
    }
    int32_t w, h;
    uint8_t *rgba = keptGet(path, &w, &h);
    if (!rgba && ticket && fromPack) {
        /* Round 40: not decoded yet - the worker does it; the caller draws
         * the N64 texels meanwhile and swaps the picture in when it lands
         * (texpackTicketPoll). A full job table (rare) is asked again by the
         * poll. */
        if (jobRequest(path, 1) || workerState >= 0) {
            ticket->key = path;
            ticket->gen = indexGen;
            return NULL;
        }
        /* no worker could be started: decode here, as before */
    }
    if (!rgba) {
        const uint64_t t0 = sysGetMicroseconds();
        int32_t srcW = 0, srcH = 0;
        /* Round 38: decoded at most Video.TexPackMaxDim on a side (0 = as
         * authored); UVs stay normalised by the N64 tile, so a smaller
         * picture needs nothing else. */
        rgba = pngReadMaxDim(path, cfgMaxDim, &w, &h, &srcW, &srcH);
        if (!rgba) {
            return NULL;
        }
        if (w != srcW || h != srcH) {
            if (statCapped++ < 16) {
                const char *base = strrchr(path, '/');
                sysLogPrintf(LOG_NOTE, "texpack: %s %dx%d decoded at %dx%d (Video.TexPackMaxDim %d)",
                             base ? base + 1 : path, srcW, srcH, w, h, cfgMaxDim);
            }
        }
        if ((uint32_t)w * (uint32_t)h * 4u > statLargestDecode) {
            statLargestDecode = (uint32_t)w * (uint32_t)h * 4u;
        }
        if (fromPack && packFlip) {
            flipRows(rgba, w, h);
        }
        statDecodes++;
        statDecodeUs += sysGetMicroseconds() - t0;
        keptPut(path, rgba, w, h);
    }
    *outW = w;
    *outH = h;
    if (outSource) {
        *outSource = fromPack ? 'p' : 'x';
    }
    return rgba;
}

void texpackFreeReplacement(uint8_t *rgba)
{
    free(rgba);
}

/* Midas round 16 (XBLA step 5): the iOS settings page and the dev bridge call
 * the getters below on the UI thread. With Midas's render thread on, that
 * thread draws and owns the index (rebuild() frees what a draw may be
 * reading), so a getter there must not rebuild: it reads the counts the
 * render thread's next frame refreshes. With the render thread off the UI
 * thread is the drawing thread (SDL's game loop runs on it) and may. */
static void ensureIndexFromUi(void)
{
    if (!portRenderAsyncActive()) {
        ensureIndex();
    }
}

int texpackGetNumPacks(void)
{
    ensureIndexFromUi();
    return numPacks;
}

const char *texpackGetPackName(int index)
{
    return index >= 0 && index < numPacks ? packNames[index] : "";
}

const char *texpackGetSelected(void) { return cfgPack; }

void texpackSetSelected(const char *name)
{
    snprintf(cfgPack, sizeof(cfgPack), "%s", name ? name : "");
    texpackReload();
}

int texpackGetNumReplacements(void) { ensureIndexFromUi(); return numReplacements; }
int texpackGetNumPackFiles(void) { ensureIndexFromUi(); return numPackFiles; }
int texpackGetNumUnplaced(void) { ensureIndexFromUi(); return numUnplaced; }

int texpackXblaEnabled(void) { return cfgXbla; }

void texpackSetXblaEnabled(int on)
{
    cfgXbla = on ? 1 : 0;
    texpackReload();
}

int texpackXblaNumTextures(void) { ensureIndexFromUi(); return numXbla; }

PD_CONSTRUCTOR static void texpackConfigInit(void)
{
    texpackRegisterConfig();
}

/* Midas (round 11): the headless build has no video.c and so no render
 * thread; the video build's strong definition wins over this one. */
__attribute__((weak)) s32 portRenderAsyncActive(void)
{
    return 0;
}
