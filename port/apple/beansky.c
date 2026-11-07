/* Midas round 61 (docs/port-ios/81-round61-xbla-sky-domes.md, GitHub issue #1).
 * Ported from Perfect Dark's gebeansky.c (perfect-dark-ios vendor/dabs-mod
 * port/src/gebeansky.c, MIT): the release-dome path only (no Community
 * Edition, no CE fog colours, no CE sea). 64-bit host layer only.
 */
/*
 * GoldenEye XBLA's sky over a level served in HD. See beansky.h for what this
 * is; this file is the how.
 *
 * A sky file is one dome and a cloud cap (files/new/skydome/<name>/default.bin,
 * the same CAFF rendergraph as a level, UVs in 1/16384), in the release's own
 * axes, which are the level's up to a scale (beanstagetable.h) - so the dome
 * needs nothing but the camera's turn. It is drawn round the eye as the
 * level's backdrop is (beanStageRenderBackdrop()): placed at the eye, scaled
 * so its farthest vertex is BEANSKY_REACH world units out (inside the far
 * plane; scaling about the eye moves nothing on the screen), no depth test.
 * The dome's own height is kept (Dam's horizon ring sits 4000 over the
 * placeholder's), as the release tuned it level by level.
 *
 * The triangles are built into one host display list a level (a texture load
 * and the batches a picture); each frame loads the matrix and calls it.
 *
 * MEMORY RULE (charter rule 3c): host malloc only, like beanstage.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <ultra64.h>
#include <PR/gbi.h>
#include <gbi_extension.h>
#include "platform.h"
#include "system.h"
#include "texpack.h"
#include "beanmesh.h"
#include "beanstage.h"
#include "beansky.h"
#include "guestmem.h"
#include "bondconstants.h"
#include "game/bg.h"
#include "game/player.h"
#include "game/matrixmath.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "beansky.c is host malloc only"
#endif

s32 get_BONDdata_field_10E0(void);   /* bondview.c: the player's world projection */
Gfx *bondviewGfxPlayerField5cMatrix(Gfx *gdl);

#define BEANSKY_MAXTEX  4
#define BEANSKY_BATCH   15        /* vertices a load: five whole triangles (G_TRI4 indexes 0..15) */
#define BEANSKY_EXTENT  30000.0f  /* the farthest vertex, in what a Vtx holds */
#define BEANSKY_REACH   2000.0f   /* world units the farthest vertex is drawn at (the backdrop's) */
#define BEANSKY_MTXRING 32        /* 8 frames of matrices kept (the frame presented may lag) x 4 split-screen
                                   * viewports: a slot is taken per beanSkyRender() call, not per frame */
#define BEANSKY_UVSCALE 16384.0f  /* a sky file's UVs: sixteen-thousandths (PD gebeanSkyOpen()) */
#define TILE 32
#define TILE_SCALE (TILE * 32.0f) /* a UV of one, in S10.5 texels of the stand-in */

/*
 * GoldenEye's key for a level -> the release's own sky (PD releaseSkyNames[]).
 * The release's eleven are one picture, Surface's: a blue sky over the peaks
 * and low sun of Surface's panorama (the level's backdrop). Everywhere else
 * the placeholder is wrong and the level keeps GoldenEye's sky. On these three
 * GoldenEye's sky (Surface's lavender dusk, Dam's navy cloud plane, Runway's
 * pink) met the release's daylit panorama in a hard seam of two skies
 * (GitHub issue #1). Surface 2 shares Surface's bg key but not its backdrop
 * (beanstage.c backdropWanted()), so it keeps GoldenEye's night.
 */
static const struct { const char *key, *sky; } releaseSkyNames[] = {
    { "sevx", "surface" },
    { "dam", "dam" },
    { "run", "runway" },
};
#define NUM_RELEASESKIES ((s32)(sizeof(releaseSkyNames) / sizeof(releaseSkyNames[0])))

struct skylist {
    s32 numtris, captris;
    f32 *tris;          /* numtris x 3 x {x, y, z, u, v}, file units, while building */
    u8 alpha;           /* the picture has alpha (a cloud cap of Rare's own) */
};

/* What one build leaves for the frames: kept a level longer when retired, as
 * a frame being presented may still read it (beanstage.c retireServed()). */
struct skyset {
    Vtx *vtx;
    Gfx *gdl;
    u16 *tiles[BEANSKY_MAXTEX];
};

static struct {
    const char *key;    /* what was built (or tried and found nothing) */
    s32 tried;
    s32 ok;
    u32 gen;            /* texpackBeanGeneration() its pictures went into */
    struct skyset set;
} sky;

static struct skyset retired;
static Mtx mtxRing[BEANSKY_MTXRING];
static u32 mtxFrame;

static void setFree(struct skyset *s)
{
    free(s->vtx);
    free(s->gdl);
    for (s32 i = 0; i < BEANSKY_MAXTEX; i++) {
        free(s->tiles[i]);
    }
    memset(s, 0, sizeof(*s));
}

/* the current build into `retired` (the one before it freed) */
static void skyRetire(void)
{
    setFree(&retired);
    retired = sky.set;
    memset(&sky, 0, sizeof(sky));
}

void beanSkyLevelReset(void)
{
    skyRetire();
}

static s32 fileExists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* $C/xbla/files/new/skydome/<name>/default.bin (the importer's copy), else the
 * oracle's Video.XblaPath tree read in place (beanstage.c levelPath()). */
static s32 skyPath(const char *name, char *out, size_t len)
{
    snprintf(out, len, "%s/files/new/skydome/%s/default.bin", sysResolvePath(TEXPACK_XBLA_DIR), name);
    if (fileExists(out)) {
        return 1;
    }
    const char *src = texpackXblaPath();
    if (src && *src) {
        char base[512];
        snprintf(base, sizeof(base), "%s", src);
        snprintf(out, len, "%s/files/new/skydome/%s/default.bin", base, name);
        if (fileExists(out)) {
            return 1;
        }
        snprintf(out, len, "%s/new/skydome/%s/default.bin", base, name);
        if (fileExists(out)) {
            return 1;
        }
    }
    return 0;
}

struct collect {
    struct skylist lists[BEANSKY_MAXTEX];
    s32 numlists;
    f32 extent;         /* the farthest vertex from the dome's middle (the eye), file units */
};

static void skyTakeTriangle(void *arg, int tex, const struct beanlevelvtx *v)
{
    struct collect *c = arg;
    if (tex < 0 || tex >= BEANSKY_MAXTEX) {
        return;
    }
    if (tex >= c->numlists) {
        c->numlists = tex + 1;
    }
    struct skylist *l = &c->lists[tex];
    if (l->numtris == l->captris) {
        const s32 cap = l->captris ? l->captris * 2 : 1024;
        f32 *t = realloc(l->tris, sizeof(f32) * 15 * (size_t)cap);
        if (!t) {
            return;
        }
        l->tris = t;
        l->captris = cap;
    }
    for (s32 k = 0; k < 3; k++) {
        f32 *out = &l->tris[(l->numtris * 3 + k) * 5];
        const f32 r = sqrtf(v[k].pos[0] * v[k].pos[0] + v[k].pos[1] * v[k].pos[1] + v[k].pos[2] * v[k].pos[2]);
        out[0] = v[k].pos[0];
        out[1] = v[k].pos[1];
        out[2] = v[k].pos[2];
        out[3] = v[k].uv[0];
        out[4] = v[k].uv[1];
        if (r > c->extent) {
            c->extent = r;
        }
    }
    l->numtris++;
}

/*
 * How much of the cloud cap shows at a UV's distance from its middle, for a
 * cap whose picture has no alpha (PD skyCapFade(): Rare's own cap fades in
 * its alpha - whole to 0.33 of the way out, gone by 0.475). The release's
 * caps carry their alpha, so this is the fallback only.
 */
static u8 skyCapFade(f32 u, f32 v)
{
    const f32 r = sqrtf((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
    if (r <= 0.33f) {
        return 255;
    }
    if (r >= 0.475f) {
        return 0;
    }
    const f32 t = (0.475f - r) / (0.475f - 0.33f);
    return (u8)(255.0f * t * t * (3.0f - 2.0f * t));
}

static s16 clampS16(f32 f)
{
    return (s16)(f < -32768.0f ? -32768.0f : f > 32767.0f ? 32767.0f : f);
}

/* a 32x32 RGBA16 stand-in of one colour, whose address names the picture */
static u16 *newTile(u32 r, u32 g, u32 b)
{
    u16 *tile = malloc(TILE * TILE * sizeof(u16));
    if (!tile) {
        return NULL;
    }
    const u16 t = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
    const u16 be = (u16)((t >> 8) | (t << 8));
    for (s32 i = 0; i < TILE * TILE; i++) {
        tile[i] = be;
    }
    return tile;
}

/*
 * The dome's list: per picture its state (the dome opaque DECALRGB, u wrapped
 * round the horizon; the cap blended over it with the picture's alpha times
 * the vertex's), its texture load and its batches. Each frame calls it under
 * the eye's matrix (beanSkyRender()).
 */
static s32 skyWrite(struct collect *c, const s32 *usable)
{
    s32 numvtx = 0, numcmds = 2;
    const f32 k = BEANSKY_EXTENT / c->extent;
    for (s32 i = 0; i < c->numlists; i++) {
        if (!usable[i]) {
            continue;
        }
        const s32 n = c->lists[i].numtris * 3;
        const s32 loads = (n + BEANSKY_BATCH - 1) / BEANSKY_BATCH;
        numvtx += n;
        numcmds += 16 + loads * 3;   /* state + load; a G_VTX and two G_TRI4 a batch */
    }
    if (numvtx == 0) {
        return 0;
    }
    Vtx *vtx = calloc((size_t)numvtx, sizeof(Vtx));
    Gfx *gdl = calloc((size_t)numcmds, sizeof(Gfx));
    if (!vtx || !gdl) {
        free(vtx);
        free(gdl);
        return 0;
    }
    Gfx *g = gdl;
    s32 first = 0;
    gSPSegment(g++, SPSEGMENT_BG_VTX, (uintptr_t)vtx);
    for (s32 i = 0; i < c->numlists; i++) {
        const struct skylist *l = &c->lists[i];
        if (!usable[i]) {
            continue;
        }
        /* past the first (the dome) is the cloud cap, faded where its picture does not fade it */
        const s32 cap = i > 0;
        const s32 fade = cap && !l->alpha;
        const s32 n = l->numtris * 3;
        for (s32 j = 0; j < n; j++) {
            const f32 *in = &l->tris[j * 5];
            Vtx *v = &vtx[first + j];
            v->v.ob[0] = (s16)(in[0] * k);
            v->v.ob[1] = (s16)(in[1] * k);
            v->v.ob[2] = (s16)(in[2] * k);
            v->v.flag = 0;
            /* the dome's u runs one repeat past 1 (0.48 to 1.48) and is wrapped;
             * Midas's pictures decode first row first, as the UVs address them
             * (beanLevelDecodeTexture()), so v is not turned over as PD's is */
            v->v.tc[0] = clampS16(in[3] * TILE_SCALE);
            v->v.tc[1] = clampS16(in[4] * TILE_SCALE);
            v->v.cn[0] = 0xff;
            v->v.cn[1] = 0xff;
            v->v.cn[2] = 0xff;
            v->v.cn[3] = fade ? skyCapFade(in[3], in[4]) : 0xff;
        }
        gDPPipeSync(g++);
        if (cap) {
            gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
            gDPSetCombineLERP(g++, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0);
        } else {
            gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
            gDPSetCombineMode(g++, G_CC_DECALRGB, G_CC_DECALRGB);
        }
        {
            /* the dome's u wraps round the horizon; nothing else repeats */
            const u32 cms = cap ? G_TX_CLAMP : G_TX_WRAP;
            const u16 *tile = sky.set.tiles[i];
            gDPSetTextureImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, tile);
            gDPSetTile(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, G_TX_LOADTILE, 0, G_TX_CLAMP, 5, G_TX_NOLOD, cms, 5, G_TX_NOLOD);
            gDPLoadSync(g++);
            gDPLoadBlock(g++, G_TX_LOADTILE, 0, 0, TILE * TILE - 1, CALC_DXT(TILE, G_IM_SIZ_16b_BYTES));
            gDPPipeSync(g++);
            gDPSetTile(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 0, G_TX_RENDERTILE, 0, G_TX_CLAMP, 5, G_TX_NOLOD, cms, 5, G_TX_NOLOD);
            gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, (TILE - 1) << G_TEXTURE_IMAGE_FRAC, (TILE - 1) << G_TEXTURE_IMAGE_FRAC);
        }
        for (s32 b = 0; b < n; b += BEANSKY_BATCH) {
            const s32 count = n - b < BEANSKY_BATCH ? n - b : BEANSKY_BATCH;
            const u32 offset = (u32)(first + b) * (u32)sizeof(Vtx);
            /* GoldenEye's G_VTX: (n - 1) << 20 | bytes, segment 0x0e (beanstage.c batchFlush()) */
            g->words.w0 = (0x04u << 24) | ((u32)(count - 1) << 20) | (u32)(count * (s32)sizeof(Vtx));
            g->words.w1 = ((u32)SPSEGMENT_BG_VTX << 24) | offset;
            g++;
            const s32 numtris = count / 3;
            for (s32 t = 0; t < numtris; t += 4) {
                u32 w0 = 0xb1u << 24, w1 = 0;   /* G_TRI4 */
                for (s32 q = 0; q < 4 && t + q < numtris; q++) {
                    const u32 v0 = (u32)((t + q) * 3);
                    w1 |= v0 << (q * 8) | (v0 + 1) << (q * 8 + 4);
                    w0 |= (v0 + 2) << (q * 4);
                }
                g->words.w0 = w0;
                g->words.w1 = w1;
                g++;
            }
        }
        first += n;
    }
    gSPEndDisplayList(g++);
    sky.set.vtx = vtx;
    sky.set.gdl = gdl;
    return 1;
}

static void skyBuild(const char *key, const char *name)
{
    char path[1024];
    struct collect c;
    s32 usable[BEANSKY_MAXTEX] = { 0 };
    s32 pictures = 0;
    struct beanlevel *level;
    const u64 t0 = sysGetMicroseconds();

    sky.key = key;
    sky.tried = 1;
    sky.ok = 0;
    memset(&c, 0, sizeof(c));

    if (!skyPath(name, path, sizeof(path))) {
        sysLogPrintf(LOG_NOTE, "beansky: %s (GoldenEye's %s): no files/new/skydome/%s in the XBLA layer, GoldenEye's sky",
                     name, key, name);
        return;
    }
    level = beanLevelOpen(path, name);
    if (!level) {
        sysLogPrintf(LOG_WARNING, "beansky: %s does not open, GoldenEye's sky", path);
        return;
    }
    beanLevelSetUvScale(level, BEANSKY_UVSCALE);
    beanLevelTriangles(level, skyTakeTriangle, &c);

    for (s32 i = 0; i < c.numlists && c.extent > 0.0f; i++) {
        int32_t w = 0, h = 0;
        if (c.lists[i].numtris == 0) {
            continue;
        }
        uint8_t *rgba = beanLevelDecodeTexture(level, i, &w, &h);
        if (!rgba) {
            continue;
        }
        u64 rs = 0, gs = 0, bs = 0;
        const u32 total = (u32)w * (u32)h;
        u32 opaque = 0;
        for (u32 p = 0; p < total; p++) {
            rs += rgba[p * 4];
            gs += rgba[p * 4 + 1];
            bs += rgba[p * 4 + 2];
            opaque += rgba[p * 4 + 3] == 0xff;
        }
        c.lists[i].alpha = opaque < total;
        if (getenv("GE_BEANSKYDUMP")) {
            /* dev: the decoded pictures, $S/beansky-<name>-<n>.png (rows as the UVs address them) */
            extern s32 pngWrite(const char *path, const u8 *pixels, s32 width, s32 height, s32 channels, s32 bottomRowFirst);
            char dump[96];
            snprintf(dump, sizeof(dump), "$S/beansky-%s-%d.png", name, i);
            pngWrite(sysResolvePath(dump), rgba, w, h, 4, 0);
        }
        sky.set.tiles[i] = total ? newTile((u32)(rs / total), (u32)(gs / total), (u32)(bs / total)) : NULL;
        if (!sky.set.tiles[i]) {
            free(rgba);
            continue;
        }
        const u32 gen = texpackRegisterBeanPictureGen(sky.set.tiles[i], rgba, w, h);   /* takes rgba */
        if (gen == 0 && texpackBeanGeneration() != 0) {
            sysLogPrintf(LOG_WARNING, "beansky: picture %d of %s not registered (the registry is full)", i, name);
            continue;
        }
        sky.gen = gen;
        usable[i] = 1;
        pictures++;
    }
    if (pictures > 0 && usable[0]) {
        sky.ok = skyWrite(&c, usable);
    }
    sysLogPrintf(LOG_NOTE,
                 "beansky: %s (GoldenEye's %s): %d pictures (cap alpha %d), %d + %d triangles, extent %.0f, %s in %.1f ms",
                 name, key, pictures, c.numlists > 1 ? c.lists[1].alpha : -1, c.lists[0].numtris,
                 c.numlists > 1 ? c.lists[1].numtris : 0, (double)c.extent, sky.ok ? "drawn" : "NOT drawn",
                 (double)(sysGetMicroseconds() - t0) / 1000.0);
    for (s32 i = 0; i < BEANSKY_MAXTEX; i++) {
        free(c.lists[i].tris);
    }
    beanLevelClose(level);
}

/* The release's sky for this level, or NULL for GoldenEye's own. */
static const char *skyNameFor(const char *key)
{
    for (s32 i = 0; i < NUM_RELEASESKIES; i++) {
        if (strcmp(releaseSkyNames[i].key, key) == 0) {
            return releaseSkyNames[i].sky;
        }
    }
    return NULL;
}

int beanSkyWanted(void)
{
    static int off = -1;
    if (off < 0) off = getenv("GE_BEANNOSKY") != NULL;   /* dev: GoldenEye's sky over the backdrop (the A/B) */
    /* the backdrop's own gate: Video.XblaModels on, the level built and
     * served, its backdrop drawn (not Surface 2 or the Bunkers) */
    if (off || !beanStageHasBackdrop()) {
        return 0;
    }
    const char *key = beanStageLevelKey();
    const char *name = key ? skyNameFor(key) : NULL;
    if (!name) {
        return 0;
    }
    if (sky.tried && (strcmp(sky.key, key) != 0 || (sky.ok && sky.gen != texpackBeanGeneration()))) {
        /* another level, or its pictures were forgotten under it (a rebuild) */
        skyRetire();
    }
    if (!sky.tried) {
        skyBuild(key, name);
    }
    return sky.ok;
}

Gfx *beanSkyRender(Gfx *gdl)
{
    f32 eye[3];
    if (!sky.ok || !sky.set.gdl || !beanStageEyeWorld(eye)) {
        return gdl;
    }
    /* the eye's projection (rotation and perspective, as the rooms and the
     * backdrop use) and a model matrix that puts the farthest vertex at
     * BEANSKY_REACH world units, about the eye: the projection's origin is
     * the player's model position */
    Mtx *m = &mtxRing[mtxFrame++ % BEANSKY_MTXRING];
    {
        f32 mf[4][4];
        memset(mf, 0, sizeof(mf));
        mf[0][0] = mf[1][1] = mf[2][2] = BEANSKY_REACH / BEANSKY_EXTENT;
        mf[3][3] = 1.0f;
        for (s32 k = 0; k < 3; k++) {
            mf[3][k] = eye[k] - g_CurrentPlayer->current_model_pos.f[k];
        }
        matrix_4x4_f32_to_s32(mf, (s32 (*)[4])m);
    }
    /* Phase 6: the dome is SKY for the stereo eyes (the far asymptote, no
     * translation), as GoldenEye's own sky is; the tag names this projection
     * load only, the rooms' later load is WORLD again (gbiex_compat.h). Without
     * it the headset put the dome at BEANSKY_REACH, the backdrop's depth
     * (R63-1, doc 83). */
    gDPGeDlTag(gdl++, GE_DLCLASS_SKY);
    gSPMatrix(gdl++, osVirtualToPhysical(GUEST_PTR(get_BONDdata_field_10E0())), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gdl++, m, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gDPPipeSync(gdl++);
    gDPSetCycleType(gdl++, G_CYC_1CYCLE);
    gDPSetTexturePersp(gdl++, G_TP_PERSP);
    gDPSetTextureLOD(gdl++, G_TL_TILE);
    gDPSetTextureFilter(gdl++, G_TF_BILERP);
    gDPSetAlphaCompare(gdl++, G_AC_NONE);
    gSPClearGeometryMode(gdl++, G_ZBUFFER | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_FOG | G_CULL_BOTH);
    gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
    gSPDisplayList(gdl++, (uintptr_t)sky.set.gdl);
    /* what GoldenEye's frame expects back: depth on, back faces culled, the
     * modelview as bondview left it */
    gDPPipeSync(gdl++);
    gSPSetGeometryMode(gdl++, G_ZBUFFER | G_CULL_BACK);
    gdl = bondviewGfxPlayerField5cMatrix(gdl);
    return gdl;
}
