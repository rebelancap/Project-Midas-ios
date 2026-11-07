/* Imported from GoldenEye-Decomp-ios engine/port/src/beanstage.c (first 93988236, last changed f54cd9aa; donor HEAD 55a3a9c8),
 * round 13, XBLA step 4.1 (docs/port-ios/22-round13-rooms.md). 64-bit host layer only.
 * Changes against the donor are marked "Midas round 13". */
/*
 * GoldenEye XBLA's HD level geometry in GoldenEye's own rooms — see
 * port/include/beanstage.h and docs/tier-c.md. Tier C, round C1 (D-110).
 *
 * Shape and algorithms from Perfect Dark's gebeanstage.c (perfect-dark-ios
 * vendor/dabs-mod port/src/gebeanstage.c, MIT): the triangle grid
 * (tgridAdd/tgridNearest/pointTriDist), readRoom/fileRoomTrianglesEach,
 * the dealing in build(), and emit/batchFlush/writeLeaf/writeRoom. PD serves
 * its rooms to GoldenEye X's converted level files through a file hook; this
 * engine IS GoldenEye, so the rooms read here are GoldenEye's own, and the
 * built room is swapped in at the one render call site instead.
 *
 * C1 built one room; C2 (D-112..) serves every room Bean's mesh reaches, ports
 * PD's passes (alpha test, twins, weld, backdrop, cut-out clamp, two-faced and
 * back-to-back sheets, decals with lift and corners, shadows, over-range
 * moves), scroll slots for the reservoir, the every-room render
 * (Video.XblaEveryRoom) and the built-level disk cache. docs/tier-c.md.
 *
 * MEMORY RULE (charter rule 3c, plan §(a)): everything here is host malloc.
 * The sim's allocators are not visible to this file (checked below).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>        /* Midas round 22 (R16-3): the cold build's worker */
#include <ultra64.h>
#include <PR/gbi.h>
#include <gbi_extension.h>
#include "platform.h"
#include "system.h"
#include "config.h"
#include "texpack.h"
#include "beanmesh.h"
#include "beanstage.h"
#include "beanmodel.h"      /* Midas round 22 (R16-2): the mid-level rebuild */
#include "beanchr.h"
#include "beanprefetch.h"
#include "beansky.h"         /* Midas round 61 (doc 81): XBLA sky domes */
#include "xblaimport.h"     /* Midas round 22b (R22-9): xblaImportState() */
#include "hdmesh.h"   /* Route B: G_HDMESH_EXT (D-120) */
#include "bondconstants.h"
#include "game/bg.h"
#include "game/bgfog.h"
#include "game/player.h"
#include "game/lv.h"
#include "game/matrixmath.h"
#include "fr.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

/* The N64 libc headers on the include path shadow the host's and lack these. */
double floor(double x);
float fmodf(float x, float y);
float fabsf(float x);
float floorf(float x);
float roundf(float x);
float sqrtf(float x);
int atoi(const char *s);
int rename(const char *a, const char *b);
int remove(const char *p);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
/* Midas round 13: Midas's include/string.h stub lacks strrchr (the donor's has it). */
char *strrchr(const char *s, int c);

/* bg.c / ob.c, not in a header */
extern struct levelentry levelinfotable[];
extern s32 levelentry_index;
extern s32 g_MaxNumRooms;
void obLoadBGFileBytesAtOffset(u8 *bgname, u8 *target, s32 offset, s32 len);
u32 bgDecompress(u8 *source, u8 *target);
void portHwLoadSuppress(s32 on); /* frametiming.c (Midas round 13) */

struct stagerow {
    const char *key;
    const char *bean;
    f32 scale;
    s32 verified;
};

static const struct stagerow stageRows[] = {
#include "beanstagetable.h"
};

#define NUM_STAGEROWS ((s32)(sizeof(stageRows) / sizeof(stageRows[0])))

/* PD gebeanstage.c's grid sizes */
#define ASSIGN_CELL 64.0f
#define ASSIGN_RINGS 2
#define BATCHVERTS 16
#define TILE 32
#define TILE_SCALE (TILE * 32.0f)   /* a UV of one, in S10.5 texels of the tile */
#define FADE_ALPHA 0xf0
#define OPAQUE_ALPHA 0xf0
#define SOFT_PERCENT 1
#define DECOMP_SCRATCH (0x400000 + 0x10000) /* decompressdata()'s avail_out ceiling */
#define DECAL_LIFT 2.0f         /* a lifted decal stands this far off its base (PD) */
#define FIGHT_DEPTH_BIAS 8      /* depth steps a fighting face's back is pushed away (PD) */
#define MTX_RING 8              /* frames of every-room matrices kept (the frame presented may lag) */
#define BACKDROP_RING 8
#define BACKDROP_EXTENT 16000.0f /* the farthest backdrop vertex, in what a Vtx holds */
#define BACKDROP_BATCH 15        /* vertices a load: five whole triangles */
#define BACKDROP_REACH 2000.0f   /* world units the farthest backdrop vertex is drawn at */

/* --------------------------------------------------------------- config */

static int cfgModels = 1;
/* Video.XblaEveryRoom (round C2, D-112): draw every served room by distance
 * instead of the rooms GoldenEye's portals reach. Render only. Default 0 since
 * the round 22 merge (D-136): on the phone every room holds 60 but breaks the
 * AUTO 120 default (presents 112, late 21/s); portals hold 120. */
static int cfgEveryRoom = 1; /* round 26 merge (D-161 flip): every room, stage form - lane 2 within ~11 % of portals (M-082); the phone 120 Hz verdict is still owed */
static int cfgFarCull = 1; /* dev: GE_BEANNOCULL=1 draws every room whatever its distance */
/* Video.XblaMeshPath (Route B, D-120): 1 = each room's lists with their
 * G_VTX/G_TRI4 runs replaced by G_HDMESH_EXT (one indexed draw of the room's
 * static mesh per run); 0 = C2's lists, every vertex through gfx_sp_vertex.
 * Both forms are built; the key picks one per frame (live). */
static int cfgMeshPath = 1;
/* Video.XblaMerge (round 26c, D-160): every room with the mesh path draws one
 * STAGE form - all served rooms' triangles in one mesh, each draw state once,
 * its rooms' index ranges side by side so the rooms in view are one or a few
 * draws - instead of each room's own lists (~8 draws a room). 0 = the per-room
 * every-room pass (the A/B). Live. Portals always draw per room. */
static int cfgMerge = 1;
static int cfgXluRooms = 0; /* dev: GE_BEANXLUROOMS=1 keeps the translucent pass per room, farthest first */

int beanStageModelsEnabled(void) { return cfgModels; }
void beanStageSetModelsEnabled(int on) { cfgModels = on ? 1 : 0; }

PD_CONSTRUCTOR static void beanStageConfigInit(void)
{
    configRegisterInt("Video.XblaModels", &cfgModels, 0, 1);
    configRegisterInt("Video.XblaEveryRoom", &cfgEveryRoom, 0, 1);
    configRegisterInt("Video.XblaMeshPath", &cfgMeshPath, 0, 1);
    configRegisterInt("Video.XblaMerge", &cfgMerge, 0, 1);
    /* Midas round 29 (docs/port-ios/39): each G_HDMESH_EXT on the GPU (fast3d's
     * HD twin program, static VBO/IBO) instead of its CPU fallback; default 1 on
     * iOS, 0 on the Mac oracle (gfx_pc.cpp); GE_HDMESH_GPU=0/1 overrides */
    configRegisterInt("Video.XblaMeshGpu", &gfx_hdmesh_gpu, 0, 1);
    cfgXluRooms = getenv("GE_BEANXLUROOMS") != NULL;
    /* dev: GE_HDMESH_CPU=1 keeps the mesh lists but has fast3d run every
     * G_HDMESH_EXT's fallback (the list's shape on the CPU path: isolates the
     * shader in a pixel A/B) */
    if (getenv("GE_HDMESH_CPU")) gfx_hdmesh_enabled = 0;
    cfgFarCull = getenv("GE_BEANNOCULL") == NULL;
}

/* ---------------------------------------------------------------- state */

struct servedroom {
    Vtx *vtx;
    Gfx *opa;
    Gfx *xlu;
    /* Route B (D-120): the same lists with G_HDMESH_EXT runs, the room's
     * static mesh, the draws the commands point at and their fallbacks */
    Gfx *opa1, *xlu1;
    struct GfxHdMesh *mesh;
    struct GfxHdMeshDraw *draws;
    Gfx *fallback;
    s32 numdraws;
    s32 numvtx, numopa, numxlu;
    s32 tris;
    f32 pos[3];   /* the room's position (its Vtx are relative to it) */
    f32 box[6];   /* its Bean triangles' box, for the every-room order */
};

/*
 * The stage form (round 26c, D-160): every served room's triangles in one
 * list and one Route B mesh, positioned from one anchor. Each draw state
 * (picture, combiner, render mode, cull, depth bias) is written once, its
 * rooms' index ranges side by side (room order), so one G_HDMESH_EXT draws a
 * picture wherever it is; per frame the rooms in view pick their ranges
 * (struct stageseg) and neighbouring ranges merge into a few draws.
 */
struct stageseg {
    s32 room;
    u32 first, count;    /* indices in the stage mesh */
};

struct stagepass {
    Gfx *list;           /* = sr.opa1 / sr.xlu1, `num` words before its G_ENDDL */
    s32 num;
    s32 numgroups;       /* its G_HDMESH_EXT commands, in list order */
    struct GfxHdMeshDraw **full;   /* per group: the static draw (all its rooms; its fallback) */
    s32 *seg0;           /* numgroups + 1: each group's first segment */
    struct stageseg *segs;
    s32 numsegs;
};

struct stageform {
    struct servedroom sr;        /* vtx, opa/xlu (form 0), opa1/xlu1, mesh, draws, fallback */
    struct stagepass pass[2];
    f32 anchor[3];
    struct GfxHdMeshDraw *spans; /* MTX_RING x spanCap per-frame draws */
    s32 spanCap;
    u32 bytes;
};

static struct stageform *stage;
static s32 spanUsed;
static u8 *roomInView;
/* Render frames (beanStageFrame() calls) since the level loaded: the build's
 * log line says which frame it ran in (round 29c, D-226). */
static s32 levelFrames;

static s32 stageDraws, stageSpans; /* this frame (GE_BEANDRAWSTAT) */
static const Gfx emptyList[1] = { { { 0xb8000000u, 0 } } };   /* G_ENDDL */

static s32 tried;
static struct beanstageaudit audit; /* round 25b (D-142): this level's coverage, beanStageAudit() */
static s32 numRooms;
static s32 numServed;
static struct servedroom *served;
/* N64 rooms Bean's mesh leaves out whose surfaces are nowhere near Bean's
 * (GoldenEye's own backdrop cliffs): not drawn while every room is
 * (PD roomHidden, "sky tear") */
static u8 *roomHidden;
static s32 numHidden;
static Mtx *roomMtx;        /* MTX_RING x (numRooms + 2): the every-room pass's matrices (slot 0 the backdrop's,
                             * slot numRooms + 1 the stage form's) */
static s32 mtxFrame;
static s32 *drawOrder;
static f32 *drawDist;
static s32 everyRoomDrawn;
static u32 hostBytes, pictureBytes;
static s32 *backdropOrder;
static f32 *backdropDist;
/* Per-frame commands (round C2): GoldenEye's master display list is a
 * stage-mempool (guest) buffer sized for the N64, with no bound check; the
 * every-room pass and the backdrop write theirs here instead, MTX_RING frames
 * deep, and the guest list carries one gSPDisplayList per pass. */
static Gfx *frameGfx;      /* MTX_RING x frameGfxCap */
static s32 frameGfxCap;
static s32 frameGfxUsed;   /* in the current frame's slot */
static s32 frameGfxOverflow;
/* the stand-in tiles (one per picture used, plus the untextured one): the
 * renderer keys its texture cache on their addresses, so they live until the
 * next level's load */
static u16 **standins;
static s32 numStandins;

/* ----------------------------------------------------------- grid (PD) */

#define GRID_BITS 20
/* tgridAdd(): cells a triangle may span on one axis, and the cell index range
 * a coordinate may fall in (|coord| < 2^20 x 64 units, far past any level) */
#define TGRID_MAXSPAN 64
#define TGRID_MAXCELL (1 << 20)

static u32 gridKey(s32 x, s32 y, s32 z)
{
    return ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & ((1u << GRID_BITS) - 1);
}

struct tgrid {
    f32 cell;
    s32 *head;
    s32 *entnext;
    s32 *enttri;
    s32 nument, capent;
    f32 *tri;
    s32 *room;
    s32 numtri, captri;
    f32 *box;
    u32 *seen;
    u32 ask;
};

static s32 tgridInit(struct tgrid *g, f32 cell)
{
    memset(g, 0, sizeof(*g));
    g->cell = cell;
    g->head = malloc(sizeof(s32) << GRID_BITS);
    if (!g->head) {
        return 0;
    }
    memset(g->head, 0xff, sizeof(s32) << GRID_BITS);
    return 1;
}

static void tgridFree(struct tgrid *g)
{
    free(g->head);
    free(g->entnext);
    free(g->enttri);
    free(g->tri);
    free(g->room);
    free(g->box);
    free(g->seen);
    memset(g, 0, sizeof(*g));
}

static s32 tgridAdd(struct tgrid *g, const f32 v[3][3], s32 room)
{
    s32 lo[3], hi[3];
    s64 cells = 1;

    if (g->numtri >= g->captri) {
        const s32 cap = g->captri ? g->captri * 2 : 16384;
        f32 *t = realloc(g->tri, sizeof(f32) * 9 * cap);
        if (t) g->tri = t;
        s32 *r = realloc(g->room, sizeof(s32) * cap);
        if (r) g->room = r;
        f32 *b = realloc(g->box, sizeof(f32) * 6 * cap);
        if (b) g->box = b;
        u32 *sn = realloc(g->seen, sizeof(u32) * cap);
        if (sn) g->seen = sn;
        if (!t || !r || !b || !sn) return -1;
        g->captri = cap;
    }
    for (s32 k = 0; k < 3; k++) {
        f32 mn = v[0][k], mx = v[0][k];
        for (s32 j = 1; j < 3; j++) {
            if (v[j][k] < mn) mn = v[j][k];
            if (v[j][k] > mx) mx = v[j][k];
        }
        /* Spans in double and checked BEFORE they are multiplied or cast: a
         * degenerate triangle out at +-1e8 would overflow an s32 cell count
         * past the cap and then walk the triple loop for ever. */
        const f64 flo = floor((f64)mn / g->cell), fhi = floor((f64)mx / g->cell);
        if (!(flo >= -(f64)TGRID_MAXCELL) || !(fhi <= (f64)TGRID_MAXCELL) || fhi - flo + 1 > TGRID_MAXSPAN) {
            return -1; /* sky or distant scenery: no room's dealing is helped by filing it */
        }
        lo[k] = (s32)flo;
        hi[k] = (s32)fhi;
        cells *= (s64)(hi[k] - lo[k] + 1);
        g->box[g->numtri * 6 + k] = mn;
        g->box[g->numtri * 6 + 3 + k] = mx;
    }
    if (cells > 4096) {
        return -1;
    }
    for (s32 k = 0; k < 3; k++) {
        memcpy(g->tri + g->numtri * 9 + k * 3, v[k], sizeof(f32) * 3);
    }
    g->room[g->numtri] = room;
    g->seen[g->numtri] = 0;
    for (s32 x = lo[0]; x <= hi[0]; x++) {
        for (s32 y = lo[1]; y <= hi[1]; y++) {
            for (s32 z = lo[2]; z <= hi[2]; z++) {
                const u32 key = gridKey(x, y, z);
                if (g->nument >= g->capent) {
                    const s32 cap = g->capent ? g->capent * 2 : 65536;
                    s32 *n = realloc(g->entnext, sizeof(s32) * cap);
                    if (n) g->entnext = n;
                    s32 *t = realloc(g->enttri, sizeof(s32) * cap);
                    if (t) g->enttri = t;
                    if (!n || !t) return -1;
                    g->capent = cap;
                }
                g->entnext[g->nument] = g->head[key];
                g->enttri[g->nument] = g->numtri;
                g->head[key] = g->nument;
                g->nument++;
            }
        }
    }
    return g->numtri++;
}

static f32 dot3(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Squared distance from p to the triangle abc (Ericson). */
static f32 pointTriDist(const f32 *p, const f32 *a, const f32 *b, const f32 *c)
{
    f32 ab[3], ac[3], ap[3], bp[3], cp[3], q[3], d[3];
    f32 d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denom;

    for (s32 k = 0; k < 3; k++) {
        ab[k] = b[k] - a[k];
        ac[k] = c[k] - a[k];
        ap[k] = p[k] - a[k];
    }
    d1 = dot3(ab, ap);
    d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        memcpy(q, a, sizeof(q));
        goto done;
    }
    for (s32 k = 0; k < 3; k++) bp[k] = p[k] - b[k];
    d3 = dot3(ab, bp);
    d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        memcpy(q, b, sizeof(q));
        goto done;
    }
    vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        v = d1 / (d1 - d3);
        for (s32 k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v;
        goto done;
    }
    for (s32 k = 0; k < 3; k++) cp[k] = p[k] - c[k];
    d5 = dot3(ab, cp);
    d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        memcpy(q, c, sizeof(q));
        goto done;
    }
    vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        w = d2 / (d2 - d6);
        for (s32 k = 0; k < 3; k++) q[k] = a[k] + ac[k] * w;
        goto done;
    }
    va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (s32 k = 0; k < 3; k++) q[k] = b[k] + (c[k] - b[k]) * w;
        goto done;
    }
    denom = 1.0f / (va + vb + vc);
    v = vb * denom;
    w = vc * denom;
    for (s32 k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w;
done:
    for (s32 k = 0; k < 3; k++) d[k] = p[k] - q[k];
    return dot3(d, d);
}

static f32 boxDist(const f32 *p, const f32 *box)
{
    f32 d = 0.0f;
    for (s32 k = 0; k < 3; k++) {
        const f32 out = p[k] < box[k] ? box[k] - p[k] : (p[k] > box[3 + k] ? p[k] - box[3 + k] : 0.0f);
        d += out * out;
    }
    return d;
}

/* The triangle nearest p within `rings` cells, or -1; squared distance in *outd. */
static s32 tgridNearest(struct tgrid *g, const f32 *p, s32 rings, f32 *outd)
{
    const s32 cx = (s32)floorf(p[0] / g->cell);
    const s32 cy = (s32)floorf(p[1] / g->cell);
    const s32 cz = (s32)floorf(p[2] / g->cell);
    s32 best = -1;
    f32 bestd = 0;

    g->ask++;
    if (g->ask == 0) {
        memset(g->seen, 0, sizeof(u32) * g->numtri);
        g->ask = 1;
    }
    const u32 ask = g->ask;
    for (s32 r = 0; r <= rings; r++) {
        for (s32 x = cx - r; x <= cx + r; x++) {
            for (s32 y = cy - r; y <= cy + r; y++) {
                for (s32 z = cz - r; z <= cz + r; z++) {
                    if (x != cx - r && x != cx + r && y != cy - r && y != cy + r && z != cz - r && z != cz + r) {
                        continue;
                    }
                    for (s32 e = g->head[gridKey(x, y, z)]; e >= 0; e = g->entnext[e]) {
                        const s32 t = g->enttri[e];
                        if (g->seen[t] == ask) {
                            continue;
                        }
                        g->seen[t] = ask;
                        if (best >= 0 && boxDist(p, g->box + t * 6) > bestd * 1.001f + 1.0f) {
                            continue;
                        }
                        const f32 d = pointTriDist(p, g->tri + t * 9, g->tri + t * 9 + 3, g->tri + t * 9 + 6);
                        if (best < 0 || d < bestd) {
                            best = t;
                            bestd = d;
                        }
                    }
                }
            }
        }
        if (best >= 0 && bestd <= (r * g->cell) * (r * g->cell)) {
            break;
        }
    }
    *outd = bestd;
    return best;
}

/* ------------------------------------------------------- a point grid */

/* For the overlay probe: nearest point within a radius. */
struct pgrid {
    f32 cell;
    s32 *head;
    s32 *next;
    const f32 *pts;
    s32 num;
};

static s32 pgridInit(struct pgrid *g, f32 cell, const f32 *pts, s32 num)
{
    memset(g, 0, sizeof(*g));
    g->cell = cell;
    g->pts = pts;
    g->num = num;
    g->head = malloc(sizeof(s32) << GRID_BITS);
    g->next = malloc(sizeof(s32) * (size_t)(num ? num : 1));
    if (!g->head || !g->next) {
        free(g->head);
        free(g->next);
        return 0;
    }
    memset(g->head, 0xff, sizeof(s32) << GRID_BITS);
    for (s32 i = 0; i < num; i++) {
        const u32 key = gridKey((s32)floorf(pts[i * 3] / cell), (s32)floorf(pts[i * 3 + 1] / cell),
                                (s32)floorf(pts[i * 3 + 2] / cell));
        g->next[i] = g->head[key];
        g->head[key] = i;
    }
    return 1;
}

static void pgridFree(struct pgrid *g)
{
    free(g->head);
    free(g->next);
}

static s32 pgridNearest(const struct pgrid *g, const f32 *p, s32 rings, f32 *outd)
{
    const s32 cx = (s32)floorf(p[0] / g->cell), cy = (s32)floorf(p[1] / g->cell), cz = (s32)floorf(p[2] / g->cell);
    s32 best = -1;
    f32 bestd = 0;
    for (s32 x = cx - rings; x <= cx + rings; x++) {
        for (s32 y = cy - rings; y <= cy + rings; y++) {
            for (s32 z = cz - rings; z <= cz + rings; z++) {
                for (s32 i = g->head[gridKey(x, y, z)]; i >= 0; i = g->next[i]) {
                    const f32 dx = g->pts[i * 3] - p[0], dy = g->pts[i * 3 + 1] - p[1], dz = g->pts[i * 3 + 2] - p[2];
                    const f32 d = dx * dx + dy * dy + dz * dz;
                    if (best < 0 || d < bestd) {
                        best = i;
                        bestd = d;
                    }
                }
            }
        }
    }
    *outd = bestd;
    return best;
}

/* ------------------------------------------- GoldenEye's rooms, raw */

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static s16 bes16(const u8 *p)
{
    return (s16)(((u16)p[0] << 8) | p[1]);
}

struct rawroom {
    u8 *vtx;      /* big-endian 16-byte Vtx, as the file has them */
    u32 vtxlen;
    u8 *gdl[2];   /* primary, secondary: big-endian 8-byte Gfx */
    u32 gdllen[2];
    f32 pos[3];
    f32 box[6];   /* world box of its vertices */
    s32 hasbox;
    s32 ntri;     /* triangles its lists draw (the grid leaves out the huge ones) */
};

static u8 *decompScratch;

/*
 * One compressed section of the level's bg file (PD readRoom()): loaded with
 * obLoadBGFileBytesAtOffset() into host scratch and inflated with the
 * engine's own bgDecompress(), exactly as bgLoadRoom*() does into mema - but
 * nowhere near mema. `segaddr` is the 0x0f-segment address from the room table.
 */
static u8 *readSection(u32 segaddr, s32 csize, u32 *outLen)
{
    *outLen = 0;
    if (csize <= 0 || csize > 0x400000) {
        return NULL;
    }
    const s32 aligned = (csize + 0xf) & ~0xf;
    u8 *packed = calloc(1, (size_t)aligned + 64);
    if (!packed) {
        return NULL;
    }
    obLoadBGFileBytesAtOffset((u8 *)levelinfotable[levelentry_index].bg_seg_filename, packed,
                              (s32)(segaddr + 0xf1000000u), aligned);
    const u32 n = bgDecompress(packed, decompScratch);
    free(packed);
    if (n == 0 || n > 0x400000) {
        return NULL;
    }
    u8 *out = malloc(n);
    if (out) {
        memcpy(out, decompScratch, n);
        *outLen = n;
    }
    return out;
}

static void readRooms(struct rawroom *rooms, s32 n)
{
    for (s32 r = 1; r < n; r++) {
        const bg_room_data *fp = &ptr_bgdata_room_fileposition_list[r];
        rooms[r].pos[0] = fp->pos.x;
        rooms[r].pos[1] = fp->pos.y;
        rooms[r].pos[2] = fp->pos.z;
        rooms[r].vtx = readSection(fp->pPointTableBin, g_BgRoomInfo[r].csize_point_index_binary, &rooms[r].vtxlen);
        rooms[r].gdl[0] = readSection(fp->pPriMappingBin, g_BgRoomInfo[r].csize_primary_DL_binary, &rooms[r].gdllen[0]);
        rooms[r].gdl[1] = readSection(fp->pSecMappingBin, g_BgRoomInfo[r].csize_secondary_DL_binary, &rooms[r].gdllen[1]);
        for (u32 i = 0; i + 16 <= rooms[r].vtxlen; i += 16) {
            for (s32 k = 0; k < 3; k++) {
                const f32 c = rooms[r].pos[k] + bes16(rooms[r].vtx + i + k * 2);
                if (!rooms[r].hasbox || c < rooms[r].box[k]) rooms[r].box[k] = c;
                if (!rooms[r].hasbox || c > rooms[r].box[3 + k]) rooms[r].box[3 + k] = c;
            }
            rooms[r].hasbox = 1;
        }
    }
}

/*
 * Round 29c (D-223): what each N64 room triangle looks like before the room's
 * light - its texture number (the raw list's G_NOOP texture word, which
 * texLoadFromGdl() turns into the load) and its corners' mean vertex colour -
 * for the brightness match (matchBrightness()). Every triangle of both lists
 * is noted, the grid's index of it where the grid filed it (gridMeas[]).
 */
struct fmeas {
    f32 area;
    s16 room;
    s16 tex;    /* ROM texture number, or -1 */
    u8 rgb[3];
    u8 xlu;     /* from the secondary (translucent) list */
};

static struct fmeas *fmeas;
static s32 numFmeas, capFmeas;
static s32 *gridMeas;   /* filetris index -> fmeas index */
static s32 capGridMeas;

static void measForget(void)
{
    free(fmeas);
    free(gridMeas);
    fmeas = NULL;
    gridMeas = NULL;
    numFmeas = capFmeas = capGridMeas = 0;
}

static f32 triArea3(const f32 (*v)[3])
{
    const f32 a[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
    const f32 b[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
    const f32 x = a[1] * b[2] - a[2] * b[1];
    const f32 y = a[2] * b[0] - a[0] * b[2];
    const f32 z = a[0] * b[1] - a[1] * b[0];
    return 0.5f * sqrtf(x * x + y * y + z * z);
}

static void measNote(s32 gridIndex, const f32 (*v)[3], const u8 (*rgb)[3], s32 room, s32 tex, s32 xlu)
{
    if (numFmeas >= capFmeas) {
        const s32 cap = capFmeas ? capFmeas * 2 : 16384;
        struct fmeas *m = realloc(fmeas, sizeof(*m) * (size_t)cap);
        if (!m) {
            return;
        }
        fmeas = m;
        capFmeas = cap;
    }
    struct fmeas *m = &fmeas[numFmeas];
    m->area = triArea3(v);
    m->room = (s16)room;
    m->tex = (s16)tex;
    m->xlu = (u8)xlu;
    for (s32 ch = 0; ch < 3; ch++) {
        m->rgb[ch] = (u8)(((u32)rgb[0][ch] + rgb[1][ch] + rgb[2][ch] + 1) / 3);
    }
    if (gridIndex >= 0) {
        if (gridIndex >= capGridMeas) {
            const s32 want = capGridMeas ? capGridMeas * 2 : 16384;
            const s32 cap = gridIndex + 1 > want ? gridIndex + 1 : want;
            s32 *g = realloc(gridMeas, sizeof(s32) * (size_t)cap);
            if (!g) {
                numFmeas++;
                return;
            }
            for (s32 i = capGridMeas; i < cap; i++) g[i] = -1;
            gridMeas = g;
            capGridMeas = cap;
        }
        gridMeas[gridIndex] = numFmeas;
    }
    numFmeas++;
}

/* Midas round 21 (docs/port-ios/30): the light fixtures' N64 triangles, noted
 * as they are filed (the rest of the round's code follows the PD passes) */
static void darkNoteLight(s32 grid, const f32 (*v)[3], s32 room, const s32 *vtx, s32 tex);

/* Every triangle a room's lists draw, world space (PD fileRoomTrianglesEach()). */
static s32 fileRoomTriangles(struct tgrid *g, const struct rawroom *rm, s32 r)
{
    s32 count = 0;
    for (s32 l = 0; l < 2; l++) {
        const u8 *gdl = rm->gdl[l];
        f32 loaded[16][3];
        u8 loadedRgb[16][3];
        s32 loadedVi[16];   /* Midas round 21: each slot's vertex index in the room */
        s32 curTex = -1;
        memset(loaded, 0, sizeof(loaded));
        memset(loadedRgb, 0, sizeof(loadedRgb));
        memset(loadedVi, 0, sizeof(loadedVi));
        for (u32 c = 0; gdl && c + 8 <= rm->gdllen[l]; c += 8) {
            const u8 op = gdl[c];
            const u32 w0 = be32(gdl + c), w1 = be32(gdl + c + 4);
            if (op == 0xc0) { /* G_NOOP (F3D 0xc0): GoldenEye's texture word (texLoadFromGdl()) */
                curTex = (s32)(w1 & 0xfff);
            } else if (op == 0x04) { /* G_VTX */
                const s32 num = (s32)((w0 >> 20) & 0xf) + 1;
                const s32 dest = (s32)((w0 >> 16) & 0xf);
                const u32 at = w1 & 0xffffff;
                for (s32 i = 0; i < num && dest + i < 16; i++) {
                    if (at + (u32)i * 16 + 6 > rm->vtxlen) {
                        break;
                    }
                    loadedVi[dest + i] = (s32)((at + (u32)i * 16) / 16);   /* Midas round 21 */
                    for (s32 k = 0; k < 3; k++) {
                        loaded[dest + i][k] = rm->pos[k] + bes16(rm->vtx + at + (u32)i * 16 + k * 2);
                        loadedRgb[dest + i][k] = at + (u32)i * 16 + 16 <= rm->vtxlen ? rm->vtx[at + (u32)i * 16 + 12 + k] : 0;
                    }
                }
            } else if (op == 0xb1) { /* G_TRI4 */
                for (s32 k = 0; k < 4; k++) {
                    const s32 x = (w1 >> (k * 8)) & 0xf;
                    const s32 y = (w1 >> (k * 8 + 4)) & 0xf;
                    const s32 z = (w0 >> (k * 4)) & 0xf;
                    f32 v[3][3];
                    if (x == 0 && y == 0 && z == 0) {
                        continue;
                    }
                    memcpy(v[0], loaded[x], sizeof(v[0]));
                    memcpy(v[1], loaded[y], sizeof(v[1]));
                    memcpy(v[2], loaded[z], sizeof(v[2]));
                    {
                        const u8 rgb[3][3] = {
                            { loadedRgb[x][0], loadedRgb[x][1], loadedRgb[x][2] },
                            { loadedRgb[y][0], loadedRgb[y][1], loadedRgb[y][2] },
                            { loadedRgb[z][0], loadedRgb[z][1], loadedRgb[z][2] },
                        };
                        const s32 at = tgridAdd(g, (const f32 (*)[3])v, r);
                        measNote(at, (const f32 (*)[3])v, rgb, r, curTex, l);
                        {   /* Midas round 21 */
                            const s32 vi[3] = { loadedVi[x], loadedVi[y], loadedVi[z] };
                            darkNoteLight(at, (const f32 (*)[3])v, r, vi, curTex);
                        }
                    }
                    count++;
                }
            } else if (op == 0xb8) { /* G_ENDDL */
                break;
            }
        }
    }
    return count;
}

/* ---------------------------------------------------- Bean's triangles */

struct stri {
    f32 pos[3][3];
    f32 uv[3][2];
    u32 argb[3];
    s32 decalbase;  /* the triangle a decal was found lying on, or -1 (markDecals()) */
    s16 tex;
    s16 room;
    u8 blend;       /* in the release's blended pass */
    u8 plain;       /* of a draw with no UV and no picture of its own */
    u8 alphatest;   /* its draw's alpha test, against alpharef (alphaTested()) */
    u8 alpharef;
    u8 water;       /* a stride-36 draw: the release's water */
    u8 backed;      /* one face of a two-faced sheet, drawn culled (markBacked()) */
    u8 fights;      /* back to back with another face over part of it (markFights()) */
    u8 decal;       /* lies on another picture's face (markDecals()) */
    u8 lift;        /* a decal lifted off its base before rounding (markDecalLifts()) */
    u8 painted;     /* under a later blended decal that covers it wholly: never seen */
    s8 sink;        /* a translucent base sunk under its decal instead (+1 / -1) */
    u8 unres;       /* round 25b: its draw names a picture the file does not hold */
    u8 shader;      /* round 25b: plain, no material, every corner white - the release's shader colours it */
};

struct collect {
    struct stri *tris;
    s32 num, cap;
    f32 scale;
    f32 keep[6];   /* the N64 level's box grown by KEEP_MARGIN: beyond it, not kept */
    s32 outside;
};

/*
 * Midas round 22 (R16-3): a level's FIRST build (no $C/xbla/levels file yet,
 * or iOS evicted it) spends ~1 s (Frigate) in the passes and the dealing, a
 * frozen frame. Those steps read nothing but host copies, so on a cache miss
 * the sim thread does the parts that read the game (the level file opened,
 * GoldenEye's rooms read and filed, the N64 texture means of the brightness
 * match) and hands the rest to a worker: Bean's triangles, the pictures'
 * alpha, the passes, the dealing, the brightness match and the cache save -
 * build() itself, run on the worker with coldCur set, stopping after the
 * save. Until it is done the level draws GoldenEye's own rooms (served stays
 * NULL, so every reader of the build's globals stays out); then the sim
 * thread runs build() again at the top of a frame, now FROM CACHE, which is
 * the cached build every later visit gets. While the job runs, the build's
 * globals it writes (pics, standins, backdrop, roomHidden, the brightness
 * measurements) are the worker's; beanStageLevelLoaded() aborts and joins it
 * before it frees anything. GE_FIXEDTICK (the capture harness) keeps the
 * build on frame 0, as before (GE_BEANCOLDASYNC=0/1 forces either way).
 */
struct coldjob {
    pthread_t thread;
    const struct stagerow *row;
    struct beanlevel *level;
    struct beanlevelinfo info;
    struct rawroom *rooms;
    s32 n;
    struct tgrid filetris;
    char cachePath[1024];
    char path[1024];       /* the level file (the log line) */
    f32 levelscale;        /* levelinfotable[].levelscale, read on the sim thread */
    s32 *meanNums;         /* matchBrightness()'s texture numbers, in its order ... */
    f32 *means;            /* ... and texpackTextureMeans() of them (game thread only) */
    s32 numMeans;
    s32 frame;             /* the render frame it started at */
    volatile s32 abort;
    volatile s32 done;
    s32 saved;
    f64 ms;
    f64 msTris, msPics, msPasses, msDeal, msBright, msSave;
};
static struct coldjob *coldJob;            /* sim thread: the job in flight */
static __thread struct coldjob *coldCur;   /* the worker's own job; NULL on every other thread */
static s32 coldSyncNext;                   /* the last job kept nothing: the next build stays on the frame */
static s32 coldSwapIn;                     /* Midas round 22b (R22-8): this build is the swap-in from the job's file */
#define COLD_ABORTED() (coldCur && coldCur->abort)

/* Bean triangles further than this beyond GoldenEye's level box are left out
 * (none of Dam's: its far mountains are inside it). Keeps a broken vertex
 * (beanmesh.c already drops |coord| >= 1e8) out of the grids and the s16
 * writer. */
#define KEEP_MARGIN 65536.0f

static void collectTri(void *arg, int tex, const struct beanlevelvtx *v)
{
    struct collect *c = arg;
    if (c->num >= c->cap) {
        const s32 cap = c->cap ? c->cap * 2 : 65536;
        struct stri *n = realloc(c->tris, sizeof(*n) * (size_t)cap);
        if (!n) {
            return;
        }
        c->tris = n;
        c->cap = cap;
    }
    for (s32 k = 0; k < 3; k++) {
        for (s32 j = 0; j < 3; j++) {
            const f32 p = v[k].pos[j] * c->scale;
            if (!(p >= c->keep[j] && p <= c->keep[3 + j])) {
                c->outside++;
                return;
            }
        }
    }
    struct stri *t = &c->tris[c->num++];
    memset(t, 0, sizeof(*t));
    for (s32 k = 0; k < 3; k++) {
        for (s32 j = 0; j < 3; j++) {
            t->pos[k][j] = v[k].pos[j] * c->scale;
        }
        t->uv[k][0] = v[k].uv[0];
        t->uv[k][1] = v[k].uv[1];
        t->argb[k] = v[k].argb;
    }
    /* a draw with no UV and no picture of its own is its vertices' colour
     * alone (PD build(): plainUntextured) */
    t->tex = (s16)(v[0].plain ? -1 : tex);
    t->room = 0;
    t->blend = v[0].blend;
    t->plain = v[0].plain;
    t->alphatest = v[0].alphatest;
    t->alpharef = v[0].alpharef;
    t->water = v[0].water;
    t->decalbase = -1;
    t->unres = v[0].kind == BEANDRAW_BADPIC;
    t->shader = v[0].plain && !v[0].ownmat && v[0].argb == 0xffffffffu && v[1].argb == 0xffffffffu
        && v[2].argb == 0xffffffffu;
    if (t->shader && getenv("GE_BEANSHADERTINT")) {
        /* round 30a dev (D-248, with GE_BEANNOCACHE=1): shader-only room triangles drawn magenta */
        t->argb[0] = t->argb[1] = t->argb[2] = 0xffff00ffu;
    }
}

/* --------------------------------------------- pictures (stand-ins) */

struct pic {
    s32 used;
    s32 standin;   /* index into standins[] */
    u8 alpha;      /* any texel under OPAQUE_ALPHA */
    u8 soft;       /* next to none opaque: drawn blended */
    u8 clampv;     /* a cut-out kept within one repeat in v: drawn clamped (clampCutouts()) */
    u8 water;      /* drawn by a water draw */
    s16 clampshift;
    s16 slot;      /* its scroll slot, or -1 */
    f32 scroll[2]; /* repeats a second the release slides it (beanLevelTextureScroll()) */
    u8 edge[2];    /* mean alpha of its first and last rows, for clampCutouts() */
    u8 failed;     /* round 25b: used, but it did not decode (its stand-in is white) */
    f32 mean[3];   /* round 29c (D-223): r g b over its texels at least half opaque (all when none is); -1 unknown */
};

static struct pic *pics;
static s32 numPics;
static s32 numGlassPanes; /* this level's DXT1 glass panes given back their alpha (D-228) */

static s32 texHasAlpha(s32 tex)
{
    return tex >= 0 && tex < numPics && pics[tex].alpha;
}

static s32 texIsXlu(s32 tex)
{
    return tex >= 0 && tex < numPics && pics[tex].soft;
}

static u16 *newStandin(u16 texel)
{
    u16 **n = realloc(standins, sizeof(*standins) * (size_t)(numStandins + 1));
    if (!n) {
        return NULL;
    }
    standins = n;
    u16 *tile = malloc(TILE * TILE * sizeof(u16));
    if (!tile) {
        return NULL;
    }
    for (s32 i = 0; i < TILE * TILE; i++) {
        tile[i] = texel;
    }
    standins[numStandins++] = tile;
    return tile;
}

/* RGBA5551, big-endian as an N64 texel (only seen if the replacement is not). */
static u16 texel5551(u32 r, u32 g, u32 b)
{
    const u16 t = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
    return (u16)((t >> 8) | (t << 8));
}

/* ------------------------------------------------------- the room DL */

/* a batch's commands [cmd0, cmd1) in the list and its triangles' indices
 * [idx0, idx0 + nidx) in the room's index list (Route B) */
struct gdlrun {
    s32 cmd0, cmd1;
    u32 idx0, nidx;
    s32 room;      /* the stage form's runs: whose triangles (round 26c) */
};

struct gdlbuf {
    Gfx *g;
    s32 num, cap;
    s32 *zupd;     /* render-mode words to give Z_UPD after the fog look-up */
    s32 numzupd, capzupd;
    struct gdlrun *runs;
    s32 numruns, capruns;
};

static void emit(struct gdlbuf *b, uintptr_t w0, uintptr_t w1)
{
    if (b->num >= b->cap) {
        const s32 cap = b->cap ? b->cap * 2 : 1024;
        Gfx *n = realloc(b->g, sizeof(Gfx) * (size_t)cap);
        if (!n) {
            return;
        }
        b->g = n;
        b->cap = cap;
    }
    b->g[b->num].words.w0 = w0;
    b->g[b->num].words.w1 = w1;
    b->num++;
}

static void emitGfx(struct gdlbuf *b, Gfx g)
{
    emit(b, g.words.w0, g.words.w1);
}

static void markZupd(struct gdlbuf *b)
{
    if (b->numzupd >= b->capzupd) {
        const s32 cap = b->capzupd ? b->capzupd * 2 : 64;
        s32 *n = realloc(b->zupd, sizeof(s32) * (size_t)cap);
        if (!n) {
            return;
        }
        b->zupd = n;
        b->capzupd = cap;
    }
    b->zupd[b->numzupd++] = b->num - 1;
}

struct vtxbuf {
    Vtx *v;
    s32 num, cap;
    u32 *idx;      /* Route B: every batch's triangles as room vertex indices */
    u32 numidx, capidx;
};

static void idxAdd(struct vtxbuf *vb, u32 i)
{
    if (vb->numidx >= vb->capidx) {
        const u32 cap = vb->capidx ? vb->capidx * 2 : 4096;
        u32 *n = realloc(vb->idx, sizeof(u32) * (size_t)cap);
        if (!n) {
            return;
        }
        vb->idx = n;
        vb->capidx = cap;
    }
    vb->idx[vb->numidx++] = i;
}

static void runAdd(struct gdlbuf *b, s32 cmd0, u32 idx0, u32 nidx, s32 room)
{
    if (b->numruns >= b->capruns) {
        const s32 cap = b->capruns ? b->capruns * 2 : 256;
        struct gdlrun *n = realloc(b->runs, sizeof(*n) * (size_t)cap);
        if (!n) {
            return;
        }
        b->runs = n;
        b->capruns = cap;
    }
    b->runs[b->numruns].cmd0 = cmd0;
    b->runs[b->numruns].cmd1 = b->num;
    b->runs[b->numruns].idx0 = idx0;
    b->runs[b->numruns].nidx = nidx;
    b->runs[b->numruns].room = room;
    b->numruns++;
}

static void vtxAdd(struct vtxbuf *vb, const Vtx *v)
{
    if (vb->num >= vb->cap) {
        const s32 cap = vb->cap ? vb->cap * 2 : 1024;
        Vtx *n = realloc(vb->v, sizeof(Vtx) * (size_t)cap);
        if (!n) {
            return;
        }
        vb->v = n;
        vb->cap = cap;
    }
    vb->v[vb->num++] = *v;
}

struct batch {
    Vtx v[BATCHVERTS];
    s32 numv;
    u8 tris[64][3];
    s32 numtris;
    f32 shiftu, shiftv;
    s32 shifted;
    s32 room;      /* the stage form: every batch is one room's (round 26c) */
    s32 src[64];   /* Midas round 21: each triangle's c.tris index */
};

static void darkRecordBatch(const struct vtxbuf *vb, const struct batch *b, u32 base); /* Midas round 21 */

static void batchFlush(struct gdlbuf *l, struct vtxbuf *vb, struct batch *b)
{
    if (b->numv == 0) {
        return;
    }
    const u32 offset = (u32)vb->num * (u32)sizeof(Vtx);
    const u32 base = (u32)vb->num;
    const s32 cmd0 = l->num;
    const u32 idx0 = vb->numidx;
    for (s32 i = 0; i < b->numv; i++) {
        vtxAdd(vb, &b->v[i]);
    }
    darkRecordBatch(vb, b, base);   /* Midas round 21: the Vtx of light triangles */
    /* Route B: the same triangles, as indices into the room's Vtx[] */
    for (s32 i = 0; i < b->numtris; i++) {
        for (s32 k = 0; k < 3; k++) {
            idxAdd(vb, base + b->tris[i][k]);
        }
    }
    /* GoldenEye's G_VTX: (n - 1) << 20 | dest << 16 | bytes, segment 0x0e */
    emit(l, (0x04u << 24) | ((u32)(b->numv - 1) << 20) | (u32)(b->numv * (s32)sizeof(Vtx)),
         ((u32)SPSEGMENT_BG_VTX << 24) | offset);
    for (s32 i = 0; i < b->numtris; i += 4) {
        u32 w0 = 0xb1u << 24; /* G_TRI4 */
        u32 w1 = 0;
        for (s32 k = 0; k < 4 && i + k < b->numtris; k++) {
            w1 |= (u32)b->tris[i + k][0] << (k * 8);
            w1 |= (u32)b->tris[i + k][1] << (k * 8 + 4);
            w0 |= (u32)b->tris[i + k][2] << (k * 4);
        }
        emit(l, w0, w1);
    }
    runAdd(l, cmd0, idx0, vb->numidx - idx0, b->room);
    b->numv = 0;
    b->numtris = 0;
    b->shifted = 0;
}

/*
 * Route B (D-120): the room's list with each maximal run of batches (G_VTX +
 * G_TRI4 with nothing else between them) replaced by one G_HDMESH_EXT drawing
 * the same triangles from the room's static mesh. Every other command - state,
 * texture loads, scroll slots, the fog look-up's render modes - is copied as
 * it stands, so the GPU draw sees exactly the state the batches did. Each draw
 * keeps the run itself (+ G_ENDDL) in `fb` as its fallback.
 */
static Gfx *writeMeshList(const struct gdlbuf *l, struct GfxHdMesh *mesh, struct GfxHdMeshDraw *draws, s32 *nd,
                          Gfx *fb, s32 *nfb)
{
    Gfx *out;
    s32 o = 0, r = 0;
    if (l->num == 0) {
        return NULL;
    }
    out = malloc(sizeof(Gfx) * (size_t)l->num);
    if (!out) {
        return NULL;
    }
    for (s32 i = 0; i < l->num;) {
        if (r < l->numruns && l->runs[r].cmd0 == i) {
            s32 r1 = r, end = l->runs[r].cmd1;
            const u32 first = l->runs[r].idx0;
            u32 count = l->runs[r].nidx;
            while (r1 + 1 < l->numruns && l->runs[r1 + 1].cmd0 == end && l->runs[r1 + 1].idx0 == first + count) {
                r1++;
                end = l->runs[r1].cmd1;
                count += l->runs[r1].nidx;
            }
            struct GfxHdMeshDraw *d = &draws[(*nd)++];
            d->mesh = mesh;
            d->first = first;
            d->count = count;
            d->fallback = fb + *nfb;
            memcpy(fb + *nfb, l->g + i, sizeof(Gfx) * (size_t)(end - i));
            *nfb += end - i;
            fb[*nfb].words.w0 = 0xb8000000u; /* G_ENDDL */
            fb[*nfb].words.w1 = 0;
            (*nfb)++;
            out[o].words.w0 = (uintptr_t)G_HDMESH_EXT << 24;
            out[o].words.w1 = (uintptr_t)d;
            o++;
            i = end;
            r = r1 + 1;
        } else {
            out[o++] = l->g[i++];
        }
    }
    return out;
}

/* Route B: the room's mesh, its lists with G_HDMESH_EXT, the draws and fallbacks */
static u32 writeRoomMesh(struct servedroom *sr, struct vtxbuf *vb, const struct gdlbuf *opa, const struct gdlbuf *xlu,
                         s32 queue)
{
    const s32 runs = opa->numruns + xlu->numruns;
    const s32 fbcap = opa->num + xlu->num + runs;
    struct GfxHdMesh *m;
    s32 nd = 0, nfb = 0;
    void *idx;
    if (runs == 0 || vb->numidx == 0 || vb->num == 0) {
        return 0;
    }
    m = calloc(1, sizeof(*m));
    sr->draws = calloc((size_t)runs, sizeof(*sr->draws));
    sr->fallback = malloc(sizeof(Gfx) * (size_t)fbcap);
    if (vb->num <= 65536) {
        u16 *i16 = malloc(sizeof(u16) * vb->numidx);
        if (i16) {
            for (u32 i = 0; i < vb->numidx; i++) {
                i16[i] = (u16)vb->idx[i];
            }
        }
        idx = i16;
    } else {
        idx = vb->idx;
        vb->idx = NULL;
    }
    if (!m || !sr->draws || !sr->fallback || !idx) {
        free(m);
        free(sr->draws);
        free(sr->fallback);
        if (idx != vb->idx) free(idx);
        sr->draws = NULL;
        sr->fallback = NULL;
        return 0;
    }
    m->vtx = sr->vtx;
    m->numvtx = (unsigned int)vb->num;
    m->idx = idx;
    m->numidx = vb->numidx;
    m->idx32 = vb->num > 65536;
    sr->mesh = m;
    if (queue) {
        gfxHdMeshQueueUpload(m);   /* the render thread uploads it at its next frame */
    }
    sr->opa1 = writeMeshList(opa, m, sr->draws, &nd, sr->fallback, &nfb);
    sr->xlu1 = writeMeshList(xlu, m, sr->draws, &nd, sr->fallback, &nfb);
    sr->numdraws = nd;
    return (u32)(m->numidx * (m->idx32 ? 4 : 2)) + (u32)(opa->num + xlu->num) * (u32)sizeof(Gfx)
        + (u32)nd * (u32)sizeof(*sr->draws) + (u32)nfb * (u32)sizeof(Gfx) + (u32)sizeof(*m);
}

static s32 batchFind(const struct batch *b, const Vtx *v)
{
    for (s32 i = 0; i < b->numv; i++) {
        if (memcmp(&b->v[i], v, sizeof(*v)) == 0) {
            return i;
        }
    }
    return -1;
}

static s32 clampS16(f32 f, s16 *out)
{
    const f32 r = roundf(f);
    if (r < -32768.0f || r > 32767.0f) {
        return 0;
    }
    *out = (s16)r;
    return 1;
}

static f32 triUvMiddle(const struct stri *t, s32 k)
{
    f32 lo = t->uv[0][k], hi = lo;
    for (s32 i = 1; i < 3; i++) {
        lo = t->uv[i][k] < lo ? t->uv[i][k] : lo;
        hi = t->uv[i][k] > hi ? t->uv[i][k] : hi;
    }
    return (lo + hi) * 0.5f;
}

/* Whether a triangle's corners are all within a Vtx's reach of a room's position. */
static s32 triFitsAt(const struct stri *tri, const f32 *roompos)
{
    s16 v;
    for (s32 k = 0; k < 3; k++) {
        for (s32 j = 0; j < 3; j++) {
            if (!clampS16(tri->pos[k][j] - roompos[j], &v)) {
                return 0;
            }
        }
    }
    return 1;
}

/* Whether the opaque leaf draws the triangle's front alone at first: one face
 * of a two-faced sheet (markBacked()) or one that fights another over part of
 * its face (markFights(), whose back is drawn after, pushed away). */
static s32 triCulled(const struct stri *t)
{
    return t->backed || t->fights;
}

/* A triangle of a picture with alpha, or of the release's blended pass, that
 * the release fades by its vertices' alpha (PD triFades()). */
static s32 triFades(const struct stri *t)
{
    return (texHasAlpha(t->tex) || t->blend)
        && ((t->argb[0] >> 24) < FADE_ALPHA || (t->argb[1] >> 24) < FADE_ALPHA || (t->argb[2] >> 24) < FADE_ALPHA);
}

static const struct stri *sortTris;
static s32 sortCutoutsLast;
static s32 sortByRoom;   /* the stage form: one room's triangles together within a picture */

static int compareTex(const void *a, const void *b)
{
    const struct stri *ta = &sortTris[*(const s32 *)a];
    const struct stri *tb = &sortTris[*(const s32 *)b];
    /* the opaque leaf's cut-outs after its solid pictures */
    if (sortCutoutsLast && texHasAlpha(ta->tex) != texHasAlpha(tb->tex)) {
        return texHasAlpha(ta->tex) - texHasAlpha(tb->tex);
    }
    /* the faces of two-faced sheets after the rest of their kind (solid
     * pictures: the sheets last; cut-outs: the sheets first) */
    if (triCulled(ta) != triCulled(tb)) {
        return texHasAlpha(ta->tex) ? triCulled(tb) - triCulled(ta) : triCulled(ta) - triCulled(tb);
    }
    /* decals after what they lie on */
    if (ta->decal != tb->decal) {
        return ta->decal - tb->decal;
    }
    if (ta->tex != tb->tex) {
        return ta->tex - tb->tex;
    }
    if (sortByRoom && ta->room != tb->room) {
        return ta->room - tb->room;
    }
    return *(const s32 *)a - *(const s32 *)b;
}

/*
 * The texture load for one stand-in (gDPLoadTextureBlock of a 32x32 RGBA16,
 * wrap, mask 5): the words GoldenEye's own texLoadFromGdl() writes for a
 * room's texture, with the stand-in's host address in G_SETTIMG. `clampv`
 * clamps t (cmt = G_TX_CLAMP) for a cut-out kept inside one repeat.
 */
static void emitTexture(struct gdlbuf *l, const u16 *tile, s32 clampv)
{
    const u32 cmt = clampv ? (2u << 18) : 0;
    emit(l, 0xba000e02, 0x00000000);             /* G_TT_NONE */
    emit(l, 0xfd100000, (uintptr_t)tile);        /* SETTIMG RGBA 16b */
    emit(l, 0xf5100000, 0x07014050 | cmt);       /* SETTILE load tile, wrap, mask 5 */
    emit(l, 0xe6000000, 0x00000000);             /* LOADSYNC */
    emit(l, 0xf3000000, 0x073ff100);             /* LOADBLOCK 1024 texels, dxt 256 */
    emit(l, 0xe7000000, 0x00000000);             /* PIPESYNC */
    emit(l, 0xf5101000, 0x00014050 | cmt);       /* SETTILE render tile, line 8 */
    emit(l, 0xf2000000, 0x0007c07c);             /* SETTILESIZE 32x32 */
}

static const Gfx ccOpaque[] = { gsDPSetCombineMode(G_CC_TRILERP, G_CC_MODULATEI2) };
static const Gfx ccAlpha[] = { gsDPSetCombineMode(G_CC_TRILERP, G_CC_MODULATEIA2) };
static const Gfx rmOpaque[] = { gsDPSetRenderMode(G_RM_PASS, G_RM_AA_ZB_OPA_SURF2) };
static const Gfx rmCutout[] = { gsDPSetRenderMode(G_RM_PASS, G_RM_AA_ZB_TEX_EDGE2) };
static const Gfx rmXlu[] = { gsDPSetRenderMode(G_RM_PASS, G_RM_AA_ZB_XLU_SURF2) };
static const Gfx rmOpaDecal[] = { gsDPSetRenderMode(G_RM_PASS, G_RM_AA_ZB_OPA_DECAL2) };
static const Gfx rmXluDecal[] = { gsDPSetRenderMode(G_RM_PASS, G_RM_AA_ZB_XLU_DECAL2) };

/* ---------------------------------------------------- scroll slots */

/* A scrolling picture (the reservoir) binds through a slot: a two-word list
 * the room's list calls after the texture load, rewritten each frame with the
 * picture's slide at the level's clock (PD gebeanStageScrollSlot()). */
#define SCROLL_SLOTS 8
static Gfx *scrollSlots;          /* SCROLL_SLOTS x 3 words, host */
static s32 scrollTex[SCROLL_SLOTS];
static s32 numScrollSlots;

static s32 scrollSlotFor(s32 tex)
{
    if (tex < 0 || tex >= numPics || (pics[tex].scroll[0] == 0.0f && pics[tex].scroll[1] == 0.0f)) {
        return -1;
    }
    if (pics[tex].slot >= 0) {
        return pics[tex].slot;
    }
    if (!scrollSlots) {
        scrollSlots = calloc(SCROLL_SLOTS * 3, sizeof(Gfx));
    }
    if (!scrollSlots || numScrollSlots >= SCROLL_SLOTS) {
        return -1;
    }
    scrollTex[numScrollSlots] = tex;
    pics[tex].slot = (s16)numScrollSlots;
    return numScrollSlots++;
}

/* G_SETTILEOFFSET_EXT (gbiex.h): tile `t`'s s/t origin moved by s16 fractions
 * of a texel (1/512ths) - the slide, finer than SETTILESIZE's quarter texel. */
static void scrollSlotWrite(s32 i, f32 clock)
{
    Gfx *g = scrollSlots + i * 3;
    const s32 tex = scrollTex[i];
    s32 ofs[2];
    for (s32 a = 0; a < 2; a++) {
        /* the tile's origin moves back by the slide (a texture matrix
         * translating the coordinates by +d reads the picture at uv + d) */
        f32 q = fmodf(-pics[tex].scroll[a] * clock, 1.0f);
        if (q < 0.0f) q += 1.0f;
        ofs[a] = (s32)(q * TILE * 512.0f) & 0xffff;
        if (ofs[a] >= TILE * 512) ofs[a] = 0;
    }
    /* tile 0 and 1: the 2-cycle leaf reads both */
    g[0].words.w0 = ((uintptr_t)G_SETTILEOFFSET_EXT << 24) | 0;
    g[0].words.w1 = (uintptr_t)(((u32)(-ofs[0]) & 0xffff) << 16 | ((u32)(-ofs[1]) & 0xffff));
    g[1].words.w0 = ((uintptr_t)G_SETTILEOFFSET_EXT << 24) | 1;
    g[1].words.w1 = g[0].words.w1;
    g[2].words.w0 = 0xb8000000u; /* G_ENDDL (negative in this gbi.h: never shift it) */
    g[2].words.w1 = 0;
}

/*
 * PD writeLeaf(): one leaf's triangles sorted by picture (cut-outs last in the
 * opaque leaf, sheets and decals after their kind), 16-vertex batches of G_VTX +
 * G_TRI4, positions relative to the room, S10.5 UVs on the 32x32 stand-in,
 * each batch's texture window shifted to its first triangle (re-centred on a
 * triangle that does not fit). Both faces are drawn (Bean's sheets are single
 * planes) except the two-faced sheets (markBacked()/markFights()); the backs
 * of fighting faces are drawn in a second side, culled to their backs and
 * pushed away by G_SETDEPTHBIAS_EXT. Decals take GoldenEye's decal modes and
 * are lifted off their base (markDecalLifts()).
 */
/* writeLeaf() writing the stage form (round 26c, D-160): each triangle is
 * rounded from its OWN room's position exactly as its room's leaf rounds it,
 * then moved by the whole-unit step from that room to the stage anchor; a room
 * change inside a picture starts a new batch as the room's own leaf would. So
 * each room's run of a picture holds the same batches, vertices and UV windows
 * as its room's list - only positioned from the anchor. */
static s32 stageMode;
static const struct rawroom *stageRooms;
static f32 stageAnchor[3];
static s32 stageOverflow;

static void writeLeaf(struct gdlbuf *l, struct vtxbuf *vb, const struct stri *tris, s32 *list, s32 num, s32 xlu,
                      const f32 *roompos, const u16 *untextured, s32 *dropped)
{
    struct batch b;
    s32 numfights = 0;

    if (num == 0) {
        return;
    }
    for (s32 i = 0; i < num && !xlu; i++) {
        numfights += tris[list[i]].fights;
    }
    sortTris = tris;
    sortCutoutsLast = !xlu;
    sortByRoom = stageMode;
    qsort(list, (size_t)num, sizeof(*list), compareTex);
    sortByRoom = 0;
    memset(&b, 0, sizeof(b));

    emit(l, 0xe7000000, 0x00000000);   /* PIPESYNC */
    emit(l, 0xba001402, 0x00100000);   /* 2-cycle */
    emit(l, 0xba001001, 0x00010000);   /* texture LOD (fast3d: tile 0 for both texels) */
    emit(l, 0xba001102, 0x00000000);   /* TEXTDETAIL clamp */
    emit(l, 0xba000c02, 0x00002000);   /* bilinear */
    emit(l, 0xb6000000, 0x00002000);   /* both faces */
    emit(l, 0xfb000000, 0x000000ff);   /* env: opaque */

    s32 curcull = 0;
    for (s32 side = 0; side < (numfights ? 2 : 1); side++) {
        s32 curtex = -2, curalpha = -1, curdecal = -1, curculled = -1;
        if (side == 1) {
            batchFlush(l, vb, &b);
            memset(&b, 0, sizeof(b));
            emit(l, 0xb6000000, 0x00002000);   /* G_CULL_BACK off */
            emit(l, 0xb7000000, 0x00001000);   /* G_CULL_FRONT on: the backs */
            emit(l, (uintptr_t)G_SETDEPTHBIAS_EXT << 24, FIGHT_DEPTH_BIAS);
            curcull = 0;
        }
        for (s32 i = 0; i < num; i++) {
            const struct stri *t = &tris[list[i]];
            Vtx rv[3];
            u8 idx[3];
            f32 lift[3] = { 0.0f, 0.0f, 0.0f };
            s32 ok = 1;
            s32 need = 0;

            if (side == 1 && !t->fights) {
                continue;
            }
            memset(rv, 0, sizeof(rv));
            if (t->tex != curtex || t->decal != curdecal || triCulled(t) != curculled) {
                const s32 alpha = xlu || texHasAlpha(t->tex);
                batchFlush(l, vb, &b);
                if (alpha != curalpha) {
                    emitGfx(l, alpha ? ccAlpha[0] : ccOpaque[0]);
                }
                if (alpha != curalpha || t->decal != curdecal) {
                    /* a cut-out in the opaque leaf is a texture edge
                     * (CVG_X_ALPHA): its clear texels must not write depth.
                     * A decal takes GoldenEye's decal mode (ZMODE_DEC, which
                     * the fog look-up knows); an opaque one writes depth as
                     * well (Z_UPD, added after the look-up) - where it
                     * overhangs its base, whatever came next painted over it */
                    if (xlu) {
                        emitGfx(l, t->decal ? rmXluDecal[0] : rmXlu[0]);
                    } else if (t->decal) {
                        emitGfx(l, alpha ? rmXluDecal[0] : rmOpaDecal[0]);
                        if (!alpha) {
                            markZupd(l);
                        }
                    } else {
                        emitGfx(l, alpha ? rmCutout[0] : rmOpaque[0]);
                    }
                }
                curalpha = alpha;
                curdecal = t->decal;
                /* a two-faced sheet culls, whatever else in the leaf does */
                if (side == 0 && !xlu && triCulled(t) != curcull) {
                    curcull = triCulled(t);
                    emit(l, curcull ? 0xb7000000 : 0xb6000000, 0x00002000);
                }
                curculled = triCulled(t);
                emit(l, alpha ? 0xbb002801 : 0xbb003001, 0xffffffff); /* G_TEXTURE on, scale 1 */
                {
                    const s32 clampv = t->tex >= 0 && pics[t->tex].clampv;
                    emitTexture(l, t->tex >= 0 ? standins[pics[t->tex].standin] : untextured, clampv);
                    const s32 slot = scrollSlotFor(t->tex);
                    if (slot >= 0) {
                        emit(l, 0x06000000, (uintptr_t)(scrollSlots + slot * 3)); /* gSPDisplayList */
                    }
                }
                curtex = t->tex;
            }
            if (stageMode && t->room != b.room) {
                /* the room's own leaf starts this picture's first batch here */
                batchFlush(l, vb, &b);
                b.room = t->room;
                b.shifted = 0;
            }
            if (!b.shifted) {
                b.shiftu = floorf(t->uv[0][0]);
                b.shiftv = t->tex >= 0 && pics[t->tex].clampv ? pics[t->tex].clampshift : floorf(t->uv[0][1]);
                b.shifted = 1;
            }
            /* a decal is lifted off its base before its corners are rounded
             * to whole units (PD: Dam's CTON on its wall) */
            if (t->lift || t->sink) {
                f32 e1[3], e2[3], n[3], len;
                for (s32 k = 0; k < 3; k++) {
                    e1[k] = t->pos[1][k] - t->pos[0][k];
                    e2[k] = t->pos[2][k] - t->pos[0][k];
                }
                n[0] = e1[1] * e2[2] - e1[2] * e2[1];
                n[1] = e1[2] * e2[0] - e1[0] * e2[2];
                n[2] = e1[0] * e2[1] - e1[1] * e2[0];
                len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len > 0.0f) {
                    const f32 by = (t->lift ? DECAL_LIFT : -t->sink * DECAL_LIFT) / len;
                    for (s32 k = 0; k < 3; k++) {
                        lift[k] = n[k] * by;
                    }
                }
            }
            const f32 *rp = stageMode ? stageRooms[t->room].pos : roompos;
            for (s32 pass = 0; pass < 2; pass++) {
                ok = 1;
                for (s32 k = 0; k < 3 && ok; k++) {
                    ok = clampS16(t->pos[k][0] + lift[0] - rp[0], &rv[k].v.ob[0])
                        && clampS16(t->pos[k][1] + lift[1] - rp[1], &rv[k].v.ob[1])
                        && clampS16(t->pos[k][2] + lift[2] - rp[2], &rv[k].v.ob[2])
                        && clampS16((t->uv[k][0] - b.shiftu) * TILE_SCALE, &rv[k].v.tc[0])
                        && clampS16((t->uv[k][1] - b.shiftv) * TILE_SCALE, &rv[k].v.tc[1]);
                    rv[k].v.cn[0] = (u8)(t->argb[k] >> 16);
                    rv[k].v.cn[1] = (u8)(t->argb[k] >> 8);
                    rv[k].v.cn[2] = (u8)t->argb[k];
                    rv[k].v.cn[3] = (u8)(t->argb[k] >> 24);
                }
                if (ok || pass == 1) {
                    break;
                }
                /* outside this batch's texture window: a window centred on
                 * this triangle's UVs (PD: Facility's catwalk decks) */
                batchFlush(l, vb, &b);
                b.shiftu = floorf(triUvMiddle(t, 0));
                b.shiftv = t->tex >= 0 && pics[t->tex].clampv ? pics[t->tex].clampshift : floorf(triUvMiddle(t, 1));
                b.shifted = 1;
            }
            if (!ok) {
                (*dropped)++;
                continue;
            }
            if (stageMode) {
                /* whole units from the room to the anchor (both integral:
                 * checked when the anchor is chosen) */
                for (s32 k = 0; k < 3; k++) {
                    for (s32 j = 0; j < 3; j++) {
                        const s32 w = rv[k].v.ob[j] + (s32)(rp[j] - stageAnchor[j]);
                        if (w < -32768 || w > 32767) {
                            stageOverflow = 1;
                        }
                        rv[k].v.ob[j] = (s16)w;
                    }
                }
            }
            for (s32 k = 0; k < 3; k++) {
                if (batchFind(&b, &rv[k]) < 0) {
                    need++;
                }
            }
            if (b.numv + need > BATCHVERTS || b.numtris >= 64) {
                const f32 su = b.shiftu, sv = b.shiftv;
                batchFlush(l, vb, &b);
                b.shiftu = su;
                b.shiftv = sv;
                b.shifted = 1;
            }
            for (s32 k = 0; k < 3; k++) {
                s32 at = batchFind(&b, &rv[k]);
                if (at < 0) {
                    at = b.numv++;
                    b.v[at] = rv[k];
                }
                idx[k] = (u8)at;
            }
            b.tris[b.numtris][0] = idx[0];
            b.tris[b.numtris][1] = idx[1];
            b.tris[b.numtris][2] = idx[2];
            b.src[b.numtris] = list[i];   /* Midas round 21 */
            b.numtris++;
        }
        batchFlush(l, vb, &b);
    }
    if (numfights) {
        emit(l, 0xb6000000, 0x00001000);                       /* G_CULL_FRONT off */
        emit(l, (uintptr_t)G_SETDEPTHBIAS_EXT << 24, 0);
    }
    emit(l, 0xb7000000, 0x00002000);   /* culling back on, as a GoldenEye room leaves it */
    emit(l, 0xb8000000, 0x00000000);   /* G_ENDDL */
}

/* ------------------------------------------------- PD's marking passes */

/*
 * Ported from Perfect Dark's gebeanstage.c (round C2): alphaTested(),
 * dropTwins(), weldVertices(), takeBackdrop(), clampCutouts(), the normal
 * cache, markBacked(), markFights(), markDecals(), markDecalLifts(),
 * markShadows(), markDecalCorners(). The comments carry PD's evidence (their
 * F3 reports name PD's levels); the thresholds are theirs.
 */

#define DECAL_DIST 1.0f
#define DECAL_COS 0.999f
#define SHADE_APART 48.0f
#define WELD_DIST 0.25f
#define SHADOW_ALPHA 0x80
#define BEAN_CELL 64.0f

#define MAXV(a, b) ((a) > (b) ? (a) : (b))
#define MINV(a, b) ((a) < (b) ? (a) : (b))

/*
 * Midas round 21 (docs/port-ios/30, R19b-4): shot-out lights in the HD rooms.
 *
 * GoldenEye darkens a broken fixture by shifting its N64 room vertices'
 * colours right by 2, in place (src/game/lightfixture.c), and repeats that
 * from darkened_light_table whenever the room's vertices are read again. A
 * served room draws the release's vertices instead, so the same darkening is
 * carried over here:
 *
 *  - The N64 side reports each darkened vertex (room, index into the room's
 *    Vtx), each vertex reload of a room and each table reset through three
 *    PORT-only calls (beanStageLight*()); `darkMirror` is the set of N64
 *    vertices darkened right now, exactly as the N64 room has them.
 *  - Every build (cold or from the level cache: nothing here is cached) notes
 *    the N64 triangles drawn with a light texture (check_if_imageID_is_light,
 *    the same test tex.c opens a fixture entry with) as fileRoomTriangles()
 *    files them, with their three vertex indices. An HD triangle belongs to
 *    one when the dealing's own nearest-triangle query (tgridNearest over
 *    every N64 triangle, ASSIGN_RINGS) from its middle lands on it. Each HD
 *    Vtx written for such a triangle (batchFlush()) keeps the N64 triangle,
 *    its barycentric weights in it and its built colour.
 *  - An HD Vtx's colour is then GoldenEye's: all three N64 corners dark ->
 *    the built RGB >> 2 (the N64's own shift); some -> the built RGB times
 *    sum(w_i x (dark_i ? 1/4 : 1)), the N64 Gouraud ramp at that point; a Vtx
 *    of several such triangles takes the darkest. Alpha is left alone: the
 *    release draws some lamps as cut-outs or translucent, where alpha >> 2
 *    would remove the lamp instead of dimming it.
 *  - Nothing here writes a published Vtx. A change is sent to the renderer as
 *    one G_VTXPATCH_EXT at the start of the frame's first Bean list
 *    (darkPatchEmit()); fast3d applies it in stream order and voids the
 *    retained fallbacks that loaded those Vtx (gfx_pc.cpp gfx_vtx_patch()).
 *    The patch blocks live until the build is retired and freed, a level
 *    after it stops being drawn, as the Vtx themselves do.
 */
#include "game/lightfixture.h"
#define G_VTXPATCH_EXT 0x51 /* port/fast3d/gbiex_compat.h */

struct darklight {   /* an N64 triangle drawn with a light texture */
    s32 grid;        /* its index in the dealing grid (filetris), or -1 */
    s32 room;
    s32 vtx[3];      /* its corners: indices into the room's Vtx */
    s32 ref[3];      /* ... as indices into darkRefKey[] */
    f32 pos[3][3];
};
struct darkrec {     /* an HD Vtx written for an HD triangle dealt to one */
    s32 owner;       /* room (> 0), or 0: the stage form */
    s32 index;       /* into that form's Vtx */
    s32 light;
    f32 w[3];
    u8 orig[4];
};
struct darkvert {    /* one HD Vtx, its records darkRecs[r0 .. r0 + nr) */
    s32 owner, index, r0, nr;
    u8 orig[4], cur[4];
};

static struct darklight *darkLights;
static s32 numDarkLights, capDarkLights;
static u32 *darkRefKey;   /* room << 20 | vertex, sorted, unique */
static u8 *darkRefDark;
static s32 numDarkRefs;
static struct darkrec *darkRecs;
static s32 numDarkRecs, capDarkRecs;
static struct darkvert *darkVerts;
static s32 numDarkVerts;
static const s32 *darkHdLight;          /* during the write: c.tris index -> darkLights index, or -1 */
static const struct stri *darkSrc;      /* ... and c.tris */
static s32 darkOwner = -1;              /* the form being written (struct darkrec owner), -1 none */
static u32 *darkMirror;                 /* the N64 vertices darkened now (room << 20 | vertex) */
static s32 numDarkMirror, capDarkMirror;
static s32 darkDirty;                   /* the mirror moved since the colours were last worked out */
static s32 darkFrameOpen;               /* no Bean list written yet this frame */
static uintptr_t **darkPatches;         /* this build's patch blocks */
static s32 numDarkPatches;
static s32 darkQueried;                 /* HD triangles sent to the nearest-triangle query (the log line) */
static s32 darkTied;                    /* ... and dealt to a light by DARK_TIE */
static s32 darkLightsHit;               /* light triangles with an HD triangle (the log line) */

static u32 darkKey(s32 room, s32 vtx)
{
    return ((u32)room << 20) | ((u32)vtx & 0xfffffu);
}

/* lightfixture.c (sim thread): a vertex of `room` was darkened (>> 2) */
void beanStageLightDarkened(s32 room, s32 vtx)
{
    const u32 k = darkKey(room, vtx);
    for (s32 i = 0; i < numDarkMirror; i++) {
        if (darkMirror[i] == k) {
            return;
        }
    }
    if (numDarkMirror >= capDarkMirror) {
        const s32 cap = capDarkMirror ? capDarkMirror * 2 : 256;
        u32 *m = realloc(darkMirror, sizeof(u32) * (size_t)cap);
        if (!m) {
            return;
        }
        darkMirror = m;
        capDarkMirror = cap;
    }
    darkMirror[numDarkMirror++] = k;
    darkDirty = 1;
}

/* lightfixture.c: the room's vertices were read again (none darkened; the
 * table's are darkened again right after, through beanStageLightDarkened) */
void beanStageLightRoomReloaded(s32 room)
{
    s32 kept = 0;
    for (s32 i = 0; i < numDarkMirror; i++) {
        if ((s32)(darkMirror[i] >> 20) != room) {
            darkMirror[kept++] = darkMirror[i];
        }
    }
    if (kept != numDarkMirror) {
        numDarkMirror = kept;
        darkDirty = 1;
    }
}

/* lightfixture.c lightFixtureInitTables(): a level's start */
void beanStageLightTablesReset(void)
{
    numDarkMirror = 0;
    darkDirty = 1;
}

/* fileRoomTriangles(): a triangle of room `room` filed at grid index `grid` */
static void darkNoteLight(s32 grid, const f32 (*v)[3], s32 room, const s32 *vtx, s32 tex)
{
    if (tex < 0 || !check_if_imageID_is_light(tex)) {
        return;
    }
    if (numDarkLights >= capDarkLights) {
        const s32 cap = capDarkLights ? capDarkLights * 2 : 256;
        struct darklight *n = realloc(darkLights, sizeof(*n) * (size_t)cap);
        if (!n) {
            return;
        }
        darkLights = n;
        capDarkLights = cap;
    }
    struct darklight *l = &darkLights[numDarkLights++];
    l->grid = grid;
    l->room = room;
    memcpy(l->vtx, vtx, sizeof(l->vtx));
    memcpy(l->pos, v, sizeof(l->pos));
}

static void darkForgetBuild(void)
{
    free(darkLights);
    free(darkRefKey);
    free(darkRefDark);
    free(darkRecs);
    free(darkVerts);
    darkLights = NULL;
    darkRefKey = NULL;
    darkRefDark = NULL;
    darkRecs = NULL;
    darkVerts = NULL;
    numDarkLights = capDarkLights = numDarkRefs = numDarkRecs = capDarkRecs = numDarkVerts = 0;
    darkOwner = -1;
    darkHdLight = NULL;
    darkSrc = NULL;
}

static int darkU32Cmp(const void *a, const void *b)
{
    const u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

static s32 darkRefOf(u32 k)
{
    s32 lo = 0, hi = numDarkRefs - 1;
    while (lo <= hi) {
        const s32 m = (lo + hi) / 2;
        if (darkRefKey[m] == k) return m;
        if (darkRefKey[m] < k) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

/* p's barycentric weights in triangle t (projected on its plane), clamped
 * into it; a degenerate triangle gives its nearest corner */
static void darkWeights(const f32 *p, const f32 (*t)[3], f32 *w)
{
    f32 v0[3], v1[3], v2[3];
    for (s32 k = 0; k < 3; k++) {
        v0[k] = t[1][k] - t[0][k];
        v1[k] = t[2][k] - t[0][k];
        v2[k] = p[k] - t[0][k];
    }
    const f32 d00 = dot3(v0, v0), d01 = dot3(v0, v1), d11 = dot3(v1, v1), d20 = dot3(v2, v0), d21 = dot3(v2, v1);
    const f32 den = d00 * d11 - d01 * d01;
    f32 s = 0.0f;
    if (den > 1e-6f * d00 * d11 && den > 0.0f) {
        w[1] = (d11 * d20 - d01 * d21) / den;
        w[2] = (d00 * d21 - d01 * d20) / den;
        w[0] = 1.0f - w[1] - w[2];
        for (s32 k = 0; k < 3; k++) {
            w[k] = MAXV(w[k], 0.0f);
            s += w[k];
        }
    }
    if (s > 0.0f) {
        for (s32 k = 0; k < 3; k++) w[k] /= s;
        return;
    }
    s32 best = 0;
    f32 bestd = 0.0f;
    for (s32 c = 0; c < 3; c++) {
        f32 d = 0.0f;
        for (s32 k = 0; k < 3; k++) d += (p[k] - t[c][k]) * (p[k] - t[c][k]);
        if (c == 0 || d < bestd) { best = c; bestd = d; }
    }
    for (s32 k = 0; k < 3; k++) w[k] = k == best ? 1.0f : 0.0f;
}

/* batchFlush(): the batch's Vtx were just appended at vb->v[base] */
static void darkRecordBatch(const struct vtxbuf *vb, const struct batch *b, u32 base)
{
    if (darkOwner < 0 || !darkHdLight) {
        return;
    }
    for (s32 i = 0; i < b->numtris; i++) {
        const s32 t = b->src[i];
        const s32 li = t >= 0 ? darkHdLight[t] : -1;
        if (li < 0) {
            continue;
        }
        for (s32 k = 0; k < 3; k++) {
            const s32 index = (s32)base + b->tris[i][k];
            if (index >= vb->num) {
                continue;
            }
            if (numDarkRecs >= capDarkRecs) {
                const s32 cap = capDarkRecs ? capDarkRecs * 2 : 1024;
                struct darkrec *n = realloc(darkRecs, sizeof(*n) * (size_t)cap);
                if (!n) {
                    return;
                }
                darkRecs = n;
                capDarkRecs = cap;
            }
            struct darkrec *r = &darkRecs[numDarkRecs++];
            r->owner = darkOwner;
            r->index = index;
            r->light = li;
            darkWeights(darkSrc[t].pos[k], (const f32 (*)[3])darkLights[li].pos, r->w);
            memcpy(r->orig, vb->v[index].v.cn, 4);
        }
    }
}

/* Which HD triangles are a light's: c.tris index -> darkLights index or -1
 * (NULL: none). Before the rooms are written. */
/* Only an HD triangle whose middle lies within DARK_REACH of a light
 * triangle's box is asked (the dealing query itself reaches ~3 cells; a
 * release lamp sits on GoldenEye's, and a wider reach only paid for queries:
 * Dam 30,100 -> see the log line, the same 649 triangles dealt) */
#define DARK_REACH ASSIGN_CELL
#define DARK_COARSE ASSIGN_CELL
/* A light triangle no farther than this beyond the nearest N64 triangle
 * takes the HD triangle: GoldenEye's flush lamps (Facility's squares) lie in
 * their ceiling's plane, so the ceiling ties the lamp at ~0 and the query's
 * first-filed winner was the ceiling for 30 of Facility's 74 */
#define DARK_TIE 2.0f
static s32 *darkDeal(const struct collect *c, struct tgrid *filetris, s32 *outDealt)
{
    *outDealt = 0;
    darkQueried = 0;
    darkTied = 0;
    darkLightsHit = 0;
    if (numDarkLights == 0 || c->num == 0 || filetris->numtri == 0) {
        return NULL;
    }
    /* the lights' corners, as sorted unique keys */
    darkRefKey = malloc(sizeof(u32) * (size_t)numDarkLights * 3);
    s32 *gridLight = malloc(sizeof(s32) * (size_t)filetris->numtri);
    s32 *out = malloc(sizeof(s32) * (size_t)c->num);
    /* coarse cells near a light (open addressing; 0 = empty) */
    u32 cap = 1024;
    while (cap < (u32)numDarkLights * 128u) cap <<= 1;
    u64 *set = calloc(cap, sizeof(u64));
    if (!darkRefKey || !gridLight || !out || !set) {
        free(gridLight);
        free(out);
        free(set);
        return NULL;
    }
    for (s32 i = 0; i < numDarkLights; i++) {
        for (s32 k = 0; k < 3; k++) darkRefKey[i * 3 + k] = darkKey(darkLights[i].room, darkLights[i].vtx[k]);
    }
    qsort(darkRefKey, (size_t)numDarkLights * 3, sizeof(u32), darkU32Cmp);
    numDarkRefs = 0;
    for (s32 i = 0; i < numDarkLights * 3; i++) {
        if (numDarkRefs == 0 || darkRefKey[numDarkRefs - 1] != darkRefKey[i]) darkRefKey[numDarkRefs++] = darkRefKey[i];
    }
    darkRefDark = calloc((size_t)numDarkRefs, 1);
    for (s32 i = 0; i < filetris->numtri; i++) gridLight[i] = -1;
    struct tgrid lights;   /* the light triangles alone, for the tie (DARK_TIE) */
    const s32 haveLights = tgridInit(&lights, ASSIGN_CELL);
    s32 used = 0;
    for (s32 i = 0; i < numDarkLights; i++) {
        struct darklight *l = &darkLights[i];
        for (s32 k = 0; k < 3; k++) l->ref[k] = darkRefOf(darkKey(l->room, l->vtx[k]));
        if (l->grid >= 0 && l->grid < filetris->numtri) gridLight[l->grid] = i;
        if (haveLights && tgridAdd(&lights, (const f32 (*)[3])l->pos, i) < 0) {
            /* not filed (a huge triangle): only the query can find it */
        }
        /* the cells of its box grown by DARK_REACH */
        s32 lo[3], hi[3];
        for (s32 k = 0; k < 3; k++) {
            const f32 g = DARK_REACH;
            const f32 mn = MINV(l->pos[0][k], MINV(l->pos[1][k], l->pos[2][k])) - g;
            const f32 mx = MAXV(l->pos[0][k], MAXV(l->pos[1][k], l->pos[2][k])) + g;
            lo[k] = (s32)floorf(mn / DARK_COARSE);
            hi[k] = (s32)floorf(mx / DARK_COARSE);
        }
        for (s32 x = lo[0]; x <= hi[0]; x++) {
            for (s32 y = lo[1]; y <= hi[1]; y++) {
                for (s32 z = lo[2]; z <= hi[2]; z++) {
                    const u64 key = (((u64)(u32)(x & 0x1fffff)) << 42 | ((u64)(u32)(y & 0x1fffff)) << 21
                                     | (u64)(u32)(z & 0x1fffff)) + 1;
                    u32 h = (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
                    while (set[h] && set[h] != key) h = (h + 1) & (cap - 1);
                    if (!set[h]) {
                        if ((u32)used * 2 >= cap) continue; /* full: never in practice (cap >= 128 cells a light) */
                        set[h] = key;
                        used++;
                    }
                }
            }
        }
    }
    for (s32 t = 0; t < c->num; t++) {
        const struct stri *tri = &c->tris[t];
        f32 mid[3], d;
        out[t] = -1;
        if (tri->room <= 0) {
            continue;
        }
        for (s32 j = 0; j < 3; j++) mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
        const s32 x = (s32)floorf(mid[0] / DARK_COARSE), y = (s32)floorf(mid[1] / DARK_COARSE),
                  z = (s32)floorf(mid[2] / DARK_COARSE);
        const u64 key = (((u64)(u32)(x & 0x1fffff)) << 42 | ((u64)(u32)(y & 0x1fffff)) << 21 | (u64)(u32)(z & 0x1fffff)) + 1;
        u32 h = (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
        while (set[h] && set[h] != key) h = (h + 1) & (cap - 1);
        if (!set[h]) {
            continue;
        }
        const s32 near = tgridNearest(filetris, mid, ASSIGN_RINGS, &d);
        darkQueried++;
        if (near >= 0 && gridLight[near] >= 0) {
            out[t] = gridLight[near];
            (*outDealt)++;
        } else if (near >= 0 && haveLights) {
            f32 dl;
            const s32 ln = tgridNearest(&lights, mid, 1, &dl);
            if (ln >= 0 && sqrtf(dl) <= sqrtf(d) + DARK_TIE) {
                out[t] = lights.room[ln];   /* tgridAdd's `room` is the light's index here */
                (*outDealt)++;
                darkTied++;
            }
        }
    }
    if (getenv("GE_LIGHTDBG")) {
        /* dev: each light triangle that has HD triangles, where it is (world) */
        s32 *cnt = calloc((size_t)numDarkLights, sizeof(s32));
        for (s32 t = 0; cnt && t < c->num; t++) {
            if (out[t] >= 0) cnt[out[t]]++;
        }
        for (s32 i = 0; cnt && i < numDarkLights; i++) {
            if (cnt[i]) {
                const struct darklight *l = &darkLights[i];
                sysLogPrintf(LOG_NOTE, "beanstage: lights: n64 room %d light triangle %d (vertices %d %d %d) at %.0f %.0f %.0f: "
                             "%d HD triangles", l->room, i, l->vtx[0], l->vtx[1], l->vtx[2],
                             (l->pos[0][0] + l->pos[1][0] + l->pos[2][0]) / 3.0f * room_data_float2,
                             (l->pos[0][1] + l->pos[1][1] + l->pos[2][1]) / 3.0f * room_data_float2,
                             (l->pos[0][2] + l->pos[1][2] + l->pos[2][2]) / 3.0f * room_data_float2, cnt[i]);
            }
        }
        free(cnt);
    }
    {
        u8 *hit = calloc((size_t)numDarkLights, 1);
        darkLightsHit = 0;
        for (s32 t = 0; hit && t < c->num; t++) {
            if (out[t] >= 0 && !hit[out[t]]) {
                hit[out[t]] = 1;
                darkLightsHit++;
            }
        }
        free(hit);
    }
    if (haveLights) {
        tgridFree(&lights);
    }
    free(gridLight);
    free(set);
    return out;
}

static int darkRecCmp(const void *a, const void *b)
{
    const struct darkrec *x = a, *y = b;
    if (x->owner != y->owner) return x->owner < y->owner ? -1 : 1;
    if (x->index != y->index) return x->index < y->index ? -1 : 1;
    return x->light < y->light ? -1 : x->light > y->light;
}

static Vtx *darkVtxOf(const struct darkvert *v)
{
    if (v->owner == 0) {
        return stage && stage->sr.vtx ? &stage->sr.vtx[v->index] : NULL;
    }
    return served && v->owner < numRooms && served[v->owner].vtx ? &served[v->owner].vtx[v->index] : NULL;
}

/* the colours the mirror asks for, against what each HD Vtx has now (`cur`):
 * with `emit`, the changed Vtx are collected as patch pairs (pointer, rgba)
 * for the renderer to write; without, they are written here (the build) */
static s32 darkWork(uintptr_t *emit)
{
    s32 n = 0;
    memset(darkRefDark, 0, (size_t)numDarkRefs);
    for (s32 i = 0; i < numDarkMirror; i++) {
        const s32 r = darkRefOf(darkMirror[i]);
        if (r >= 0) darkRefDark[r] = 1;
    }
    for (s32 i = 0; i < numDarkVerts; i++) {
        struct darkvert *v = &darkVerts[i];
        s32 all = 0, some = 0;
        f32 f = 1.0f;
        for (s32 j = v->r0; j < v->r0 + v->nr; j++) {
            const struct darkrec *r = &darkRecs[j];
            const struct darklight *l = &darkLights[r->light];
            const s32 d0 = l->ref[0] >= 0 && darkRefDark[l->ref[0]];
            const s32 d1 = l->ref[1] >= 0 && darkRefDark[l->ref[1]];
            const s32 d2 = l->ref[2] >= 0 && darkRefDark[l->ref[2]];
            if (d0 && d1 && d2) {
                all = 1;
            } else if (d0 || d1 || d2) {
                const f32 g = r->w[0] * (d0 ? 0.25f : 1.0f) + r->w[1] * (d1 ? 0.25f : 1.0f) + r->w[2] * (d2 ? 0.25f : 1.0f);
                f = MINV(f, g);
                some = 1;
            }
        }
        u8 want[4];
        for (s32 ch = 0; ch < 3; ch++) {
            /* all three corners dark: the N64's own shift; some: the ramp at
             * this point, rounded, never above the built colour */
            const f32 x = (f32)v->orig[ch] * f + 0.5f;
            want[ch] = all ? (u8)(v->orig[ch] >> 2) : some ? (u8)MINV((f32)v->orig[ch], x) : v->orig[ch];
        }
        want[3] = v->orig[3];
        if (memcmp(want, v->cur, 4) == 0) {
            continue;
        }
        Vtx *dst = darkVtxOf(v);
        memcpy(v->cur, want, 4);
        if (!dst) {
            continue;
        }
        if (emit) {
            emit[1 + n * 2] = (uintptr_t)dst;
            emit[2 + n * 2] = (uintptr_t)((u32)want[0] | (u32)want[1] << 8 | (u32)want[2] << 16 | (u32)want[3] << 24);
        } else {
            memcpy(dst->v.cn, want, 4); /* the build: nothing has drawn these Vtx yet */
        }
        n++;
    }
    return n;
}

/* the end of build(): this build's HD Vtx of lights, darkened as the mirror
 * says now (a rebuild in a level with broken lights starts dark) */
static f64 msSince(u64 t0);

static void darkFinishBuild(s32 dealt, f64 dealMs)
{
    const u64 t0 = sysGetMicroseconds();
    s32 kept = 0;
    for (s32 i = 0; i < numDarkRecs; i++) {
        if (darkRecs[i].owner != 0 || stage) darkRecs[kept++] = darkRecs[i];
    }
    numDarkRecs = kept;
    if (numDarkRecs) {
        qsort(darkRecs, (size_t)numDarkRecs, sizeof(*darkRecs), darkRecCmp);
        darkVerts = malloc(sizeof(*darkVerts) * (size_t)numDarkRecs);
    }
    numDarkVerts = 0;
    for (s32 i = 0; darkVerts && i < numDarkRecs; i++) {
        const struct darkrec *r = &darkRecs[i];
        if (numDarkVerts && darkVerts[numDarkVerts - 1].owner == r->owner && darkVerts[numDarkVerts - 1].index == r->index) {
            darkVerts[numDarkVerts - 1].nr++;
            continue;
        }
        struct darkvert *v = &darkVerts[numDarkVerts++];
        v->owner = r->owner;
        v->index = r->index;
        v->r0 = i;
        v->nr = 1;
        memcpy(v->orig, r->orig, 4);
        memcpy(v->cur, r->orig, 4);
    }
    const s32 dark = darkVerts && darkRefDark ? darkWork(NULL) : 0;
    darkDirty = 0;
    sysLogPrintf(LOG_NOTE, "beanstage: lights: %d N64 light triangles (%d corners, %d with an HD triangle), %d HD triangles dealt to them, %d HD Vtx "
                 "(%d records) follow their darkening; %d dark at the build (deal %.1f ms, %d queried, %d by a tie; finish %.1f ms)",
                 numDarkLights, numDarkRefs, darkLightsHit, dealt, numDarkVerts, numDarkRecs, dark, dealMs, darkQueried, darkTied, msSince(t0));
}

/* a Bean list of this frame is about to be written at g: the frame's first
 * carries the patch for whatever the mirror changed */
static Gfx *darkPatchEmit(Gfx *g)
{
    if (!darkFrameOpen) {
        return g;
    }
    darkFrameOpen = 0;
    if (!darkDirty) {
        return g;
    }
    darkDirty = 0;
    if (!numDarkVerts || !darkRefDark) {
        return g;
    }
    uintptr_t *blk = malloc(sizeof(uintptr_t) * (1 + 2 * (size_t)numDarkVerts));
    uintptr_t **list = blk ? realloc(darkPatches, sizeof(*darkPatches) * (size_t)(numDarkPatches + 1)) : NULL;
    if (!blk || !list) {
        free(blk);
        darkDirty = 1; /* the next frame tries again */
        return g;
    }
    darkPatches = list;
    const s32 n = darkWork(blk);
    if (n == 0) {
        free(blk);
        return g;
    }
    blk[0] = (uintptr_t)n;
    darkPatches[numDarkPatches++] = blk;
    if (getenv("GE_LIGHTDBG")) {
        /* dev: the mean built and new RGB of the Vtx this patch touches */
        f64 was[3] = { 0, 0, 0 }, now[3] = { 0, 0, 0 };
        s32 m = 0;
        for (s32 i = 0; i < numDarkVerts; i++) {
            const Vtx *dst = darkVtxOf(&darkVerts[i]);
            for (s32 j = 0; dst && j < n; j++) {
                if (blk[1 + j * 2] == (uintptr_t)dst) {
                    for (s32 ch = 0; ch < 3; ch++) {
                        was[ch] += darkVerts[i].orig[ch];
                        now[ch] += darkVerts[i].cur[ch];
                    }
                    m++;
                    break;
                }
            }
        }
        m = m ? m : 1;
        sysLogPrintf(LOG_NOTE, "beanstage: lights: frame %d: %d HD Vtx recoloured (%d N64 vertices dark), mean RGB %.0f %.0f %.0f "
                     "-> %.0f %.0f %.0f", levelFrames, n, numDarkMirror, was[0] / m, was[1] / m, was[2] / m, now[0] / m,
                     now[1] / m, now[2] / m);
    }
    g->words.w0 = (uintptr_t)G_VTXPATCH_EXT << 24;
    g->words.w1 = (uintptr_t)blk;
    return g + 1;
}
/* ---- end Midas round 21 ---- */


/*
 * The release's alpha test on its opaque level draws: greater than the draw's
 * reference, of the picture's alpha times the vertex's. A triangle whose
 * vertex alpha never passes is left out, and one that does is a cut-out of
 * its picture with its vertex alpha whole, as the release draws it. Not a
 * blended draw's, whose vertex alpha blends as well.
 */
static s32 alphaTested(struct collect *c)
{
    s32 kept = 0, dropped = 0;
    for (s32 t = 0; t < c->num; t++) {
        struct stri *tri = &c->tris[t];
        if (tri->alphatest && !tri->blend) {
            u32 amax = 0;
            for (s32 k = 0; k < 3; k++) {
                amax = MAXV(amax, tri->argb[k] >> 24);
            }
            if (amax <= tri->alpharef) {
                dropped++;
                continue;
            }
            for (s32 k = 0; k < 3; k++) {
                tri->argb[k] |= 0xff000000u;
            }
        }
        c->tris[kept++] = *tri;
    }
    c->num = kept;
    return dropped;
}

/*
 * Opaque triangles the release's draws repeat corner for corner in another
 * picture, of which it shows only the first it draws (its opaque pass tests
 * depth LESS): the later twins are left out. A blended twin is kept.
 */
struct twinkey {
    f32 c[9];
    s32 tri;
};

static int twinCmp(const void *a, const void *b)
{
    const struct twinkey *x = a, *y = b;
    const int r = memcmp(x->c, y->c, sizeof(x->c));
    return r ? r : (x->tri > y->tri) - (x->tri < y->tri);
}

static int cornerCmp(const void *a, const void *b)
{
    return memcmp(a, b, 3 * sizeof(f32));
}

static s32 dropTwins(struct collect *c)
{
    struct twinkey *keys = malloc((size_t)(c->num > 0 ? c->num : 1) * sizeof(*keys));
    u8 *drop = calloc((size_t)(c->num > 0 ? c->num : 1), 1);
    s32 dropped = 0, kept = 0;
    if (!keys || !drop) {
        free(keys);
        free(drop);
        return 0;
    }
    for (s32 t = 0; t < c->num; t++) {
        f32 corner[3][3];
        memcpy(corner, c->tris[t].pos, sizeof(corner));
        qsort(corner, 3, sizeof(corner[0]), cornerCmp);
        memcpy(keys[t].c, corner, sizeof(keys[t].c));
        keys[t].tri = t;
    }
    qsort(keys, (size_t)c->num, sizeof(*keys), twinCmp);
    for (s32 a = 0; a < c->num;) {
        s32 b = a + 1, first = -1;
        while (b < c->num && memcmp(keys[b].c, keys[a].c, sizeof(keys[a].c)) == 0) {
            b++;
        }
        for (s32 k = a; k < b; k++) {
            const s32 t = keys[k].tri;
            if (!c->tris[t].blend && (first < 0 || t < first)) {
                first = t;
            }
        }
        for (s32 k = a; k < b && first >= 0; k++) {
            const s32 t = keys[k].tri;
            if (t != first && !c->tris[t].blend
                    && (c->tris[t].tex != c->tris[first].tex
                        || memcmp(c->tris[t].argb, c->tris[first].argb, sizeof(c->tris[t].argb)) != 0)) {
                drop[t] = 1;
                dropped++;
            }
        }
        a = b;
    }
    for (s32 t = 0; t < c->num && dropped; t++) {
        if (!drop[t]) {
            c->tris[kept++] = c->tris[t];
        }
    }
    if (dropped) {
        c->num = kept;
    }
    free(keys);
    free(drop);
    return dropped;
}

/* Corners within WELD_DIST of an earlier corner take its position: Bean's
 * buffers meet at seams a float's rounding apart. */
static s32 weldVertices(struct collect *c)
{
    enum { HASHBITS = 20 };
    const s32 numvtx = c->num * 3;
    s32 *head = malloc(sizeof(s32) << HASHBITS);
    s32 *next = malloc(sizeof(s32) * (size_t)(numvtx + 1));
    s32 welded = 0;
    if (!head || !next) {
        free(head);
        free(next);
        return 0;
    }
    for (s32 i = 0; i < (1 << HASHBITS); i++) {
        head[i] = -1;
    }
    for (s32 v = 0; v < numvtx; v++) {
        f32 *pos = c->tris[v / 3].pos[v % 3];
        const s32 cx = (s32)floorf(pos[0] / WELD_DIST);
        const s32 cy = (s32)floorf(pos[1] / WELD_DIST);
        const s32 cz = (s32)floorf(pos[2] / WELD_DIST);
        s32 found = -1;
        for (s32 dx = -1; dx <= 1 && found < 0; dx++) {
            for (s32 dy = -1; dy <= 1 && found < 0; dy++) {
                for (s32 dz = -1; dz <= 1 && found < 0; dz++) {
                    const u32 h = (((u32)(cx + dx) * 73856093u) ^ ((u32)(cy + dy) * 19349663u)
                                   ^ ((u32)(cz + dz) * 83492791u)) & ((1u << HASHBITS) - 1);
                    for (s32 r = head[h]; r >= 0; r = next[r]) {
                        const f32 *q = c->tris[r / 3].pos[r % 3];
                        const f32 ex = q[0] - pos[0], ey = q[1] - pos[1], ez = q[2] - pos[2];
                        if (ex * ex + ey * ey + ez * ez <= WELD_DIST * WELD_DIST) {
                            found = r;
                            break;
                        }
                    }
                }
            }
        }
        if (found >= 0) {
            const f32 *q = c->tris[found / 3].pos[found % 3];
            if (q[0] != pos[0] || q[1] != pos[1] || q[2] != pos[2]) {
                pos[0] = q[0];
                pos[1] = q[1];
                pos[2] = q[2];
                welded++;
            }
        } else {
            const u32 h = (((u32)cx * 73856093u) ^ ((u32)cy * 19349663u) ^ ((u32)cz * 83492791u))
                          & ((1u << HASHBITS) - 1);
            next[v] = head[h];
            head[h] = v;
        }
    }
    free(head);
    free(next);
    return welded;
}

/*
 * The level's backdrop (PD takeBackdrop()): a picture none of whose
 * triangles lies over the level's rooms in plan, or half of whose triangles
 * are outside and which reaches past the level by more than a third of its
 * size, is a panorama ringing the level (Dam's far mountains). Taken out of
 * the dealing; drawn after the sky and before the rooms
 * (beanStageRenderBackdrop()).
 */
static struct stri *backdrop;
static s32 numBackdrop;
static Vtx *backdropVtx;   /* numBackdrop * 3, rewritten per frame (BACKDROP_RING frames) */

static void takeBackdrop(struct collect *c, const struct rawroom *rooms, s32 n)
{
    f32 mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
    const s32 np = numPics > 0 ? numPics : 1;
    s32 *total = calloc((size_t)np, sizeof(s32));
    s32 *outside = calloc((size_t)np, sizeof(s32));
    f32 (*reach)[4] = calloc((size_t)np, sizeof(*reach));
    u8 *isbackdrop = calloc((size_t)np, 1);
    s32 kept = 0;
    if (!total || !outside || !reach || !isbackdrop) {
        goto out;
    }
    for (s32 t = 0; t < np; t++) {
        reach[t][0] = reach[t][1] = 1e30f;
        reach[t][2] = reach[t][3] = -1e30f;
    }
    for (s32 r = 1; r < n; r++) {
        if (!rooms[r].hasbox) {
            continue;
        }
        mn[0] = MINV(mn[0], rooms[r].box[0]);
        mn[1] = MINV(mn[1], rooms[r].box[2]);
        mx[0] = MAXV(mx[0], rooms[r].box[3]);
        mx[1] = MAXV(mx[1], rooms[r].box[5]);
    }
    for (s32 t = 0; t < c->num; t++) {
        const struct stri *tri = &c->tris[t];
        s32 over = 0;
        if (tri->tex < 0 || tri->tex >= numPics) {
            continue;
        }
        for (s32 k = 0; k < 3; k++) {
            over |= tri->pos[k][0] >= mn[0] && tri->pos[k][0] <= mx[0]
                && tri->pos[k][2] >= mn[1] && tri->pos[k][2] <= mx[1];
        }
        total[tri->tex]++;
        outside[tri->tex] += !over;
        for (s32 k = 0; k < 3; k++) {
            reach[tri->tex][0] = MINV(reach[tri->tex][0], tri->pos[k][0]);
            reach[tri->tex][1] = MINV(reach[tri->tex][1], tri->pos[k][2]);
            reach[tri->tex][2] = MAXV(reach[tri->tex][2], tri->pos[k][0]);
            reach[tri->tex][3] = MAXV(reach[tri->tex][3], tri->pos[k][2]);
        }
    }
    {
        const f32 size = MAXV(mx[0] - mn[0], mx[1] - mn[1]);
        for (s32 t = 0; t < numPics; t++) {
            const f32 past = MAXV(MAXV(mn[0] - reach[t][0], mn[1] - reach[t][1]),
                                  MAXV(reach[t][2] - mx[0], reach[t][3] - mx[1]));
            isbackdrop[t] = total[t] > 0 && (outside[t] == total[t] || (outside[t] * 2 >= total[t] && past > size / 3.0f));
        }
    }
    for (s32 t = 0; t < c->num; t++) {
        const struct stri *tri = &c->tris[t];
        if (tri->tex >= 0 && tri->tex < numPics && isbackdrop[tri->tex]) {
            struct stri *n2 = realloc(backdrop, sizeof(*backdrop) * (size_t)(numBackdrop + 1));
            if (n2) {
                backdrop = n2;
                backdrop[numBackdrop++] = *tri;
                continue;
            }
        }
        c->tris[kept++] = *tri;
    }
    c->num = kept;
out:
    free(total);
    free(outside);
    free(reach);
    free(isbackdrop);
}

/*
 * A cut-out picture whose triangles keep v within one repeat is drawn with t
 * clamped (wrapped, the filter at its clear edge reached round to the far
 * edge: a thin line across the sky above a treeline). So is a fading picture
 * whose v starts at a repeat and runs on past it by less than half, when the
 * picture itself fades from one edge to the other (PD: Dam's server-room
 * lamp cone).
 */
#define CLAMP_SLACK (1.0f / 32.0f)

static s32 clampCutouts(const struct collect *c)
{
    s32 clamped = 0;
    f32 *vmin = malloc(sizeof(f32) * (size_t)(numPics > 0 ? numPics : 1));
    f32 *vmax = malloc(sizeof(f32) * (size_t)(numPics > 0 ? numPics : 1));
    u8 *fades = calloc((size_t)(numPics > 0 ? numPics : 1), 1);
    if (!vmin || !vmax || !fades) {
        free(vmin);
        free(vmax);
        free(fades);
        return 0;
    }
    for (s32 t = 0; t < numPics; t++) {
        vmin[t] = 1e30f;
        vmax[t] = -1e30f;
    }
    for (s32 t = 0; t < c->num; t++) {
        const struct stri *tri = &c->tris[t];
        if (tri->tex < 0 || tri->tex >= numPics) {
            continue;
        }
        for (s32 k = 0; k < 3; k++) {
            vmin[tri->tex] = MINV(vmin[tri->tex], tri->uv[k][1]);
            vmax[tri->tex] = MAXV(vmax[tri->tex], tri->uv[k][1]);
        }
        fades[tri->tex] |= triFades(tri);
    }
    for (s32 t = 0; t < numPics; t++) {
        const f32 k = floorf(vmin[t] + CLAMP_SLACK);
        const s32 edgeFade = pics[t].edge[0] - pics[t].edge[1] >= 0x80 || pics[t].edge[1] - pics[t].edge[0] >= 0x80;
        if (pics[t].alpha && vmin[t] <= vmax[t]
                && (vmax[t] <= k + 1.0f + CLAMP_SLACK
                    || (fades[t] && vmin[t] <= k + 1.0f / 2048.0f && vmax[t] < k + 1.5f && edgeFade))) {
            pics[t].clampv = 1;
            pics[t].clampshift = (s16)k;
            clamped++;
        }
    }
    free(vmin);
    free(vmax);
    free(fades);
    return clamped;
}

/* --- the normal cache (PD triCacheFill()) */

static const struct stri *triCacheBase;
static s32 triCacheNum;
static f32 (*triCacheNormal)[4];
static f32 *triCacheShade;

static f32 triNormalWork(const struct stri *t, f32 *n)
{
    f32 e1[3], e2[3], len;
    for (s32 k = 0; k < 3; k++) {
        e1[k] = t->pos[1][k] - t->pos[0][k];
        e2[k] = t->pos[2][k] - t->pos[0][k];
    }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    len = sqrtf(dot3(n, n));
    if (len > 0) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    }
    return len * 0.5f;
}

static f32 triShadeWork(const struct stri *t)
{
    f32 sum = 0.0f;
    for (s32 k = 0; k < 3; k++) {
        sum += ((t->argb[k] >> 16) & 0xff) + ((t->argb[k] >> 8) & 0xff) + (t->argb[k] & 0xff);
    }
    return sum / 9.0f;
}

static void triCacheFill(const struct stri *tris, s32 num)
{
    triCacheNormal = malloc(sizeof(*triCacheNormal) * (size_t)(num > 0 ? num : 1));
    triCacheShade = malloc(sizeof(*triCacheShade) * (size_t)(num > 0 ? num : 1));
    if (!triCacheNormal || !triCacheShade) {
        free(triCacheNormal);
        free(triCacheShade);
        triCacheNormal = NULL;
        triCacheShade = NULL;
        return;
    }
    for (s32 i = 0; i < num; i++) {
        triCacheNormal[i][3] = triNormalWork(&tris[i], triCacheNormal[i]);
        triCacheShade[i] = triShadeWork(&tris[i]);
    }
    triCacheBase = tris;
    triCacheNum = num;
}

static void triCacheForget(void)
{
    free(triCacheNormal);
    free(triCacheShade);
    triCacheNormal = NULL;
    triCacheShade = NULL;
    triCacheBase = NULL;
    triCacheNum = 0;
}

static inline s32 triCacheIndex(const struct stri *t)
{
    if (triCacheBase && t >= triCacheBase && t < triCacheBase + triCacheNum) {
        return (s32)(t - triCacheBase);
    }
    return -1;
}

static inline f32 triNormal(const struct stri *t, f32 *n)
{
    const s32 i = triCacheIndex(t);
    if (i >= 0) {
        n[0] = triCacheNormal[i][0];
        n[1] = triCacheNormal[i][1];
        n[2] = triCacheNormal[i][2];
        return triCacheNormal[i][3];
    }
    return triNormalWork(t, n);
}

static inline f32 triShade(const struct stri *t)
{
    const s32 i = triCacheIndex(t);
    return i >= 0 ? triCacheShade[i] : triShadeWork(t);
}

/* Whether u is another picture than t for the decals: another texture, or
 * the same one painted much darker or lighter by its vertices. */
static s32 triOther(const struct stri *t, const struct stri *u)
{
    return u->tex != t->tex || fabsf(triShade(t) - triShade(u)) > SHADE_APART;
}

enum { MARKFILTER_BACKED, MARKFILTER_FIGHTS, MARKFILTER_DECAL };

static s32 markCandidate(const struct stri *tris, s32 i, const f32 *ni, s32 o, s32 kind)
{
    const struct stri *t = &tris[i];
    const struct stri *u = &tris[o];
    f32 nu[3], area, cosang;
    s32 flat = 1;
    if (o == i) {
        return 0;
    }
    area = triNormal(u, nu);
    if (area <= 0) {
        return 0;
    }
    cosang = dot3(ni, nu);
    if (kind != MARKFILTER_DECAL) {
        if (cosang > -DECAL_COS || texIsXlu(u->tex)) {
            return 0;
        }
        if (kind == MARKFILTER_BACKED) {
            return texHasAlpha(u->tex) == texHasAlpha(t->tex);
        }
    } else {
        if ((cosang < DECAL_COS && cosang > -DECAL_COS) || !triOther(t, u)
                || (cosang < 0 && triCulled(t) && triCulled(u))) {
            return 0;
        }
    }
    for (s32 k = 0; k < 3 && flat; k++) {
        f32 rel[3] = { t->pos[k][0] - u->pos[0][0], t->pos[k][1] - u->pos[0][1], t->pos[k][2] - u->pos[0][2] };
        flat = fabsf(dot3(rel, nu)) <= DECAL_DIST;
    }
    return flat;
}

/* Whether every point (`all`) or any point is on a triangle of its cell that
 * passes markCandidate(). The grid's "room" field holds Bean triangle numbers. */
static s32 markPointsOn(const struct stri *tris, const struct tgrid *g, s32 i, const f32 *ni,
                        const f32 (*pts)[3], s32 npts, s32 kind, s32 all)
{
    u32 key[7];
    u8 hit[7] = { 0 };
    u8 done[7] = { 0 };
    for (s32 k = 0; k < npts; k++) {
        key[k] = gridKey((s32)floorf(pts[k][0] / g->cell), (s32)floorf(pts[k][1] / g->cell), (s32)floorf(pts[k][2] / g->cell));
    }
    for (s32 k = 0; k < npts; k++) {
        s32 left = 0;
        if (done[k]) {
            continue;
        }
        for (s32 m = k; m < npts; m++) {
            if (key[m] == key[k]) {
                done[m] = 1;
                left++;
            }
        }
        for (s32 e = g->head[key[k]]; e >= 0 && left > 0; e = g->entnext[e]) {
            const s32 o = g->room[g->enttri[e]];
            const struct stri *u = &tris[o];
            if (!markCandidate(tris, i, ni, o, kind)) {
                continue;
            }
            for (s32 m = k; m < npts; m++) {
                if (key[m] == key[k] && !hit[m]
                        && pointTriDist(pts[m], u->pos[0], u->pos[1], u->pos[2]) <= DECAL_DIST * DECAL_DIST) {
                    hit[m] = 1;
                    left--;
                    if (!all) {
                        return 1;
                    }
                }
            }
        }
        if (all && left > 0) {
            return 0;
        }
    }
    return all;
}

static void triInset(const struct stri *t, f32 (*pts)[3], f32 *mid)
{
    for (s32 j = 0; j < 3; j++) {
        mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
    }
    for (s32 k = 0; k < 3; k++) {
        for (s32 j = 0; j < 3; j++) {
            pts[k][j] = t->pos[k][j] + (mid[j] - t->pos[k][j]) * 0.1f;
        }
    }
    memcpy(pts[3], mid, sizeof(f32) * 3);
}

/* The faces of two-faced sheets: a triangle whose whole face is covered by
 * triangles facing the other way in its own plane; drawn culled, so each
 * side shows its own face (PD markBacked()). */
static s32 markBacked(struct stri *tris, s32 num, const struct tgrid *g)
{
    s32 count = 0;
    for (s32 i = 0; i < num; i++) {
        struct stri *t = &tris[i];
        f32 ni[3], mid[3], pts[4][3];
        if (texIsXlu(t->tex) || triNormal(t, ni) <= 0) {
            continue;
        }
        triInset(t, pts, mid);
        if (markPointsOn(tris, g, i, ni, (const f32 (*)[3])pts, 4, MARKFILTER_BACKED, 1)) {
            t->backed = 1;
            count++;
        }
    }
    return count;
}

/* The faces that fight another back to back over part of their face: drawn
 * culled, then their backs pushed away from the eye (PD markFights()). */
static s32 markFights(struct stri *tris, s32 num, const struct tgrid *g)
{
    s32 count = 0;
    for (s32 i = 0; i < num; i++) {
        struct stri *t = &tris[i];
        f32 ni[3], mid[3], pts[4][3];
        if (t->backed || texIsXlu(t->tex) || triNormal(t, ni) <= 0) {
            continue;
        }
        triInset(t, pts, mid);
        if (markPointsOn(tris, g, i, ni, (const f32 (*)[3])pts, 4, MARKFILTER_FIGHTS, 0)) {
            t->fights = 1;
            count++;
        }
    }
    return count;
}

/* Black decals of cut-out or blended draws over their base are cast shadows,
 * drawn see-through (PD markShadows()). */
static s32 markShadows(struct stri *tris, s32 num)
{
    s32 marked = 0;
    for (s32 t = 0; t < num; t++) {
        struct stri *tri = &tris[t];
        if (!tri->decal || (!tri->blend && (!texHasAlpha(tri->tex) || texIsXlu(tri->tex)))
                || (tri->argb[0] & 0xffffff) || (tri->argb[1] & 0xffffff) || (tri->argb[2] & 0xffffff)
                || (tri->argb[0] >> 24) < FADE_ALPHA || (tri->argb[1] >> 24) < FADE_ALPHA || (tri->argb[2] >> 24) < FADE_ALPHA) {
            continue;
        }
        for (s32 k = 0; k < 3; k++) {
            tri->argb[k] = (u32)SHADOW_ALPHA << 24;
        }
        marked++;
    }
    return marked;
}

/* Whether a triangle lies wholly on triangles of other pictures in its plane. */
static s32 decalCovered(const struct stri *tris, const struct tgrid *g, s32 i)
{
    const struct stri *t = &tris[i];
    f32 ni[3], mid[3], pts[7][3];
    s32 n = 0;
    if (triNormal(t, ni) <= 0) {
        return 0;
    }
    for (s32 j = 0; j < 3; j++) {
        mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
    }
    memcpy(pts[n++], mid, sizeof(mid));
    for (s32 k = 0; k < 3; k++) {
        for (s32 j = 0; j < 3; j++) {
            pts[n][j] = t->pos[k][j] + (mid[j] - t->pos[k][j]) * 0.1f;
        }
        n++;
        for (s32 j = 0; j < 3; j++) {
            const f32 edge = (t->pos[k][j] + t->pos[(k + 1) % 3][j]) * 0.5f;
            pts[n][j] = edge + (mid[j] - edge) * 0.1f;
        }
        n++;
    }
    return markPointsOn(tris, g, i, ni, (const f32 (*)[3])pts, n, MARKFILTER_DECAL, 1);
}

/*
 * The triangles that lie flat on another picture's triangle (Bean's decals
 * share the plane of the surface under them exactly): drawn in a decal mode.
 * Of a pair, the one lying wholly on other pictures is the decal; else the
 * cut-out over the solid one; else (both wholly on the other) the one Bean
 * draws later; else the smaller; else the later (PD markDecals()).
 */
static s32 markDecals(struct stri *tris, s32 num, const struct tgrid *g)
{
    s32 count = 0;
    u8 *full = calloc((size_t)(num > 0 ? num : 1), 1);
    for (s32 i = 0; full && i < num; i++) {
        full[i] = (u8)decalCovered(tris, g, i);
    }
    for (s32 i = 0; i < num; i++) {
        struct stri *t = &tris[i];
        f32 ni[3], mid[3];
        const f32 ai = triNormal(t, ni);
        if (ai <= 0) {
            continue;
        }
        for (s32 j = 0; j < 3; j++) {
            mid[j] = (t->pos[0][j] + t->pos[1][j] + t->pos[2][j]) / 3.0f;
        }
        for (s32 e = g->head[gridKey((s32)floorf(mid[0] / g->cell), (s32)floorf(mid[1] / g->cell), (s32)floorf(mid[2] / g->cell))];
                e >= 0 && !t->decal; e = g->entnext[e]) {
            const s32 o = g->room[g->enttri[e]];
            const struct stri *u = &tris[o];
            const s32 alphai = texHasAlpha(t->tex), alphau = texHasAlpha(u->tex);
            f32 nu[3], au, cosang, d;
            s32 flat = 1;
            if (o == i || !triOther(t, u)) {
                continue;
            }
            au = triNormal(u, nu);
            cosang = dot3(ni, nu);
            if (au <= 0 || (cosang < DECAL_COS && cosang > -DECAL_COS)) {
                continue;
            }
            if (cosang < 0 && triCulled(t) && triCulled(u)) {
                continue;
            }
            for (s32 k = 0; k < 3 && flat; k++) {
                f32 rel[3] = { t->pos[k][0] - u->pos[0][0], t->pos[k][1] - u->pos[0][1], t->pos[k][2] - u->pos[0][2] };
                flat = fabsf(dot3(rel, nu)) <= DECAL_DIST;
            }
            if (!flat) {
                continue;
            }
            d = pointTriDist(mid, u->pos[0], u->pos[1], u->pos[2]);
            if (d > DECAL_DIST * DECAL_DIST) {
                continue;
            }
            /* two blended draws: the later paints over the earlier */
            if (t->blend && u->blend && i < o) {
                if (full && full[i] && !texHasAlpha(u->tex) && (u->argb[0] >> 24) == 0xff
                        && (u->argb[1] >> 24) == 0xff && (u->argb[2] >> 24) == 0xff) {
                    t->painted = 1;
                }
                continue;
            }
            if ((t->blend && u->blend) || (full && full[i] != full[o] ? full[i]
                    : alphai != alphau ? alphai > alphau
                    : full && full[i] ? i > o
                    : ai < au * 0.999f ? 1
                    : ai <= au * 1.001f && i > o)) {
                t->decal = 1;
                t->decalbase = o;
                count++;
            }
        }
    }
    free(full);
    return count;
}

/*
 * Which decals are lifted off their base (DECAL_LIFT, writeLeaf()): a piece
 * of decals (joined by shared rounded corners) is lifted only when none of
 * its corners is a corner of a triangle that is not a decal - a decal that is
 * part of a mesh stays in it and the decal mode alone keeps it in front. A
 * translucent base of a kept decal is sunk under it instead (PD
 * markDecalLifts()).
 */
struct liftcorner {
    s32 key[3];
    s32 tri;
};

static int liftCornerCmp(const void *a, const void *b)
{
    const struct liftcorner *ca = a, *cb = b;
    for (s32 j = 0; j < 3; j++) {
        if (ca->key[j] != cb->key[j]) {
            return ca->key[j] < cb->key[j] ? -1 : 1;
        }
    }
    return ca->tri - cb->tri;
}

static s32 liftFind(s32 *parent, s32 i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

static s32 markDecalLifts(struct stri *tris, s32 num, s32 *outKept, s32 *outSunk)
{
    struct liftcorner *corners;
    s32 *parent;
    u8 *pinned;
    s32 numcorners = 0, lifted = 0, kept = 0, sunk = 0, i, j;
    *outKept = *outSunk = 0;
    if (num <= 0) {
        return 0;
    }
    corners = malloc(sizeof(*corners) * (size_t)num * 3);
    parent = malloc(sizeof(*parent) * (size_t)num);
    pinned = calloc((size_t)num, 1);
    if (!corners || !parent || !pinned) {
        free(corners);
        free(parent);
        free(pinned);
        return 0;
    }
    for (i = 0; i < num; i++) {
        parent[i] = i;
        for (s32 k = 0; k < 3; k++) {
            for (j = 0; j < 3; j++) {
                corners[numcorners].key[j] = (s32)roundf(tris[i].pos[k][j]);
            }
            corners[numcorners].tri = i;
            numcorners++;
        }
    }
    qsort(corners, (size_t)numcorners, sizeof(*corners), liftCornerCmp);
    for (i = 0; i < numcorners; i = j) {
        s32 first = -1, plain = 0;
        for (j = i; j < numcorners && memcmp(corners[j].key, corners[i].key, sizeof(corners[i].key)) == 0; j++) {
            const s32 t = corners[j].tri;
            if (!tris[t].decal) {
                plain = 1;
            } else if (first < 0) {
                first = t;
            } else {
                const s32 a = liftFind(parent, first), b = liftFind(parent, t);
                if (a != b) {
                    parent[b] = a;
                    pinned[a] |= pinned[b];
                }
            }
        }
        if (plain && first >= 0) {
            pinned[liftFind(parent, first)] = 1;
        }
    }
    for (i = 0; i < num; i++) {
        if (tris[i].decal) {
            tris[i].lift = !pinned[liftFind(parent, i)];
            if (tris[i].lift) {
                lifted++;
            } else {
                kept++;
            }
        }
    }
    for (i = 0; i < num; i++) {
        const s32 o = tris[i].decalbase;
        f32 ni[3], no[3];
        if (!tris[i].decal || tris[i].lift || o < 0 || o >= num || tris[o].decal || tris[o].sink) {
            continue;
        }
        if (!texIsXlu(tris[o].tex) && !triFades(&tris[o])) {
            continue;
        }
        if (triNormal(&tris[i], ni) <= 0.0f || triNormal(&tris[o], no) <= 0.0f) {
            continue;
        }
        tris[o].sink = dot3(ni, no) >= 0.0f ? 1 : -1;
        sunk++;
    }
    free(corners);
    free(parent);
    free(pinned);
    *outKept = kept;
    *outSunk = sunk;
    return lifted;
}

static f32 cornerNormal(const f32 (*p)[3], f32 *n)
{
    const f32 a[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
    const f32 b[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
    f32 len;
    n[0] = a[1] * b[2] - a[2] * b[1];
    n[1] = a[2] * b[0] - a[0] * b[2];
    n[2] = a[0] * b[1] - a[1] * b[0];
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.0f) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    }
    return len;
}

/* A triangle's corners as writeLeaf() will write them: whole units from its room's position. */
static void cornersRounded(const struct stri *t, const struct rawroom *rooms, f32 (*out)[3])
{
    for (s32 k = 0; k < 3; k++) {
        for (s32 j = 0; j < 3; j++) {
            const f32 rp = rooms[t->room].pos[j];
            out[k][j] = roundf(t->pos[k][j] - rp) + rp;
        }
    }
}

/*
 * The corners of a decal kept in its mesh moved out along the decal's face
 * just far enough that the whole-unit corner writeLeaf() makes of them is not
 * behind its base as that is written (PD markDecalCorners(): Train's riveted
 * plates). Needs the rooms dealt.
 */
static s32 markDecalCorners(struct stri *tris, s32 num, const struct rawroom *rooms)
{
    struct liftcorner *corners;
    s32 numcorners = 0, moved = 0, i, j;
    if (num <= 0) {
        return 0;
    }
    corners = malloc(sizeof(*corners) * (size_t)num * 3);
    if (!corners) {
        return 0;
    }
    for (i = 0; i < num; i++) {
        for (s32 k = 0; k < 3; k++) {
            for (s32 m = 0; m < 3; m++) {
                corners[numcorners].key[m] = (s32)roundf(tris[i].pos[k][m]);
            }
            corners[numcorners].tri = i * 3 + k;
            numcorners++;
        }
    }
    qsort(corners, (size_t)numcorners, sizeof(*corners), liftCornerCmp);
    for (i = 0; i < numcorners; i = j) {
        s32 usable = 1, shared = 0, count = 0;
        f32 dir[3] = { 0.0f, 0.0f, 0.0f }, len, by = -1.0f;
        for (j = i; j < numcorners && memcmp(corners[j].key, corners[i].key, sizeof(corners[i].key)) == 0; j++) {
            const struct stri *t = &tris[corners[j].tri / 3];
            f32 n[3];
            if (!t->decal) {
                shared++;
                continue;
            }
            if (t->lift || t->decalbase < 0 || t->decalbase >= num || t->room <= 0
                    || tris[t->decalbase].room <= 0 || cornerNormal((const f32 (*)[3])t->pos, n) <= 0.0f) {
                usable = 0;
                continue;
            }
            dir[0] += n[0];
            dir[1] += n[1];
            dir[2] += n[2];
            count++;
        }
        if (!usable || count == 0) {
            continue;
        }
        len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len < 0.5f * count) {
            continue;
        }
        dir[0] /= len;
        dir[1] /= len;
        dir[2] /= len;
        for (s32 q = i; q < j && shared; q++) {
            const struct stri *t = &tris[corners[q].tri / 3];
            f32 n[3];
            if (!t->decal && cornerNormal((const f32 (*)[3])t->pos, n) > 0.0f && fabsf(dot3(n, dir)) > 0.05f) {
                usable = 0;
                break;
            }
        }
        if (!usable) {
            continue;
        }
        for (s32 step = 0; step <= 8 && by < 0.0f; step++) {
            const f32 s = step * (DECAL_LIFT / 8.0f);
            s32 ok = 1;
            for (s32 q = i; q < j && ok; q++) {
                const struct stri *t = &tris[corners[q].tri / 3];
                if (!t->decal) {
                    continue;
                }
                const struct stri *b = &tris[t->decalbase];
                const s32 k = corners[q].tri % 3;
                f32 bc[3][3], nb[3], nt[3], w[3], rel[3];
                cornersRounded(b, rooms, bc);
                if (cornerNormal((const f32 (*)[3])bc, nb) <= 0.0f && cornerNormal((const f32 (*)[3])b->pos, nb) <= 0.0f) {
                    continue;
                }
                cornerNormal((const f32 (*)[3])t->pos, nt);
                for (s32 m = 0; m < 3; m++) {
                    const f32 rp = rooms[t->room].pos[m];
                    w[m] = roundf(t->pos[k][m] + dir[m] * s - rp) + rp;
                    rel[m] = w[m] - bc[0][m];
                }
                ok = dot3(rel, nb) * (dot3(nt, nb) >= 0.0f ? 1.0f : -1.0f) >= -0.01f;
            }
            if (ok) {
                by = s;
            }
        }
        if (by <= 0.0f) {
            continue;
        }
        for (s32 q = i; q < j; q++) {
            struct stri *t = &tris[corners[q].tri / 3];
            const s32 k = corners[q].tri % 3;
            if (!t->decal) {
                continue;
            }
            t->pos[k][0] += dir[0] * by;
            t->pos[k][1] += dir[1] * by;
            t->pos[k][2] += dir[2] * by;
        }
        moved++;
    }
    free(corners);
    return moved;
}

/* ----------------------------------------------------------- the build */

static const struct stagerow *levelRow(void)
{
    const char *seg = (const char *)levelinfotable[levelentry_index].bg_seg_filename;
    const char *k = seg ? strstr(seg, "bg_") : NULL;
    char key[32];
    if (!k) {
        return NULL;
    }
    k += 3;
    const char *e = strstr(k, "_all_p");
    const size_t n = e ? (size_t)(e - k) : strlen(k);
    if (n == 0 || n >= sizeof(key)) {
        return NULL;
    }
    memcpy(key, k, n);
    key[n] = 0;
    for (s32 i = 0; i < NUM_STAGEROWS; i++) {
        if (strcmp(stageRows[i].key, key) == 0) {
            return &stageRows[i];
        }
    }
    return NULL;
}

static s32 fileExists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* $C/xbla/files/new/background/<bean>/default.bin (the importer's copy), else
 * the oracle's Video.XblaPath tree read in place. */
static s32 levelPath(const char *bean, char *out, size_t len)
{
    snprintf(out, len, "%s/files/new/background/%s/default.bin", sysResolvePath(TEXPACK_XBLA_DIR), bean);
    if (fileExists(out)) {
        return 1;
    }
    const char *src = texpackXblaPath();
    if (src && *src) {
        char base[512];
        snprintf(base, sizeof(base), "%s", src);
        snprintf(out, len, "%s/files/new/background/%s/default.bin", base, bean);
        if (fileExists(out)) {
            return 1;
        }
        snprintf(out, len, "%s/new/background/%s/default.bin", base, bean);
        if (fileExists(out)) {
            return 1;
        }
    }
    return 0;
}

static f64 msSince(u64 t0)
{
    return (f64)(sysGetMicroseconds() - t0) / 1000.0;
}

/*
 * The overlay probe (plan §(d)): does one scale, offset 0, put Bean's mesh
 * on GoldenEye's? Every N64 vertex should have a Bean vertex on it (PD:
 * "every HD vertex set contains every N64 vertex in the same frame"), so the
 * residual is the mean distance N64 vertex -> nearest Bean vertex, over the
 * N64 vertices with a Bean vertex within 32 units; the share within 1 and 3
 * units, the mean offset of the matched pairs (a translation left over) and
 * the least-squares scale of those pairs about the origin are logged too.
 */
static void probe(const struct rawroom *rooms, s32 n, const struct collect *c, f32 scale)
{
    s32 numn = 0;
    for (s32 r = 1; r < n; r++) {
        numn += (s32)(rooms[r].vtxlen / 16);
    }
    f32 *npts = malloc(sizeof(f32) * 3 * (size_t)(numn ? numn : 1));
    f32 *bpts = malloc(sizeof(f32) * 9 * (size_t)(c->num ? c->num : 1));
    if (!npts || !bpts) {
        free(npts);
        free(bpts);
        return;
    }
    numn = 0;
    for (s32 r = 1; r < n; r++) {
        for (u32 i = 0; i + 16 <= rooms[r].vtxlen; i += 16, numn++) {
            for (s32 k = 0; k < 3; k++) {
                npts[numn * 3 + k] = rooms[r].pos[k] + bes16(rooms[r].vtx + i + k * 2);
            }
        }
    }
    for (s32 t = 0; t < c->num; t++) {
        memcpy(bpts + t * 9, c->tris[t].pos, sizeof(f32) * 9);
    }
    struct pgrid g;
    if (!pgridInit(&g, 16.0f, bpts, c->num * 3)) {
        free(npts);
        free(bpts);
        return;
    }
    f64 sum = 0, off[3] = { 0, 0, 0 }, nb = 0, bb = 0;
    s32 matched = 0, within1 = 0, within3 = 0;
    for (s32 i = 0; i < numn; i++) {
        f32 d;
        const s32 j = pgridNearest(&g, npts + i * 3, 2, &d);
        if (j < 0 || d > 32.0f * 32.0f) {
            continue;
        }
        d = sqrtf(d);
        matched++;
        sum += d;
        within1 += d <= 1.0f;
        within3 += d <= 3.0f;
        if (d <= 3.0f) {
            for (s32 k = 0; k < 3; k++) {
                off[k] += npts[i * 3 + k] - bpts[j * 3 + k];
                nb += (f64)npts[i * 3 + k] * (bpts[j * 3 + k] / scale);
                bb += (f64)(bpts[j * 3 + k] / scale) * (bpts[j * 3 + k] / scale);
            }
        }
    }
    pgridFree(&g);
    sysLogPrintf(LOG_NOTE,
        "beanstage: probe: scale %.7f offset 0: %d N64 vertices, %d with a Bean vertex within 32 "
        "(%.1f%%), residual mean %.3f units, %.1f%% within 1, %.1f%% within 3; matched (<=3) mean "
        "offset N64-Bean %.3f %.3f %.3f, least-squares scale %.7f",
        scale, numn, matched, numn ? 100.0 * matched / numn : 0.0, matched ? sum / matched : 0.0,
        matched ? 100.0 * within1 / matched : 0.0, matched ? 100.0 * within3 / matched : 0.0,
        within3 ? off[0] / within3 : 0.0, within3 ? off[1] / within3 : 0.0, within3 ? off[2] / within3 : 0.0,
        bb > 0 ? nb / bb : 0.0);
    free(npts);
    free(bpts);
}

static s32 nearestRoomBox(const struct rawroom *rooms, s32 n, const f32 *p)
{
    s32 best = 0;
    f32 bestd = 0;
    for (s32 r = 1; r < n; r++) {
        if (!rooms[r].hasbox) {
            continue;
        }
        const f32 d = boxDist(p, rooms[r].box);
        if (best == 0 || d < bestd) {
            best = r;
            bestd = d;
        }
    }
    return best;
}

/* Bond's own room: the build waits for it (he is placed after the intro). */
extern enum CAMERAMODE g_CameraMode;   /* bondview.c */

static s32 bondRoom(void)
{
    /* Bond's own prop: its room list is empty (0xff) through the intro
     * cinema, whose camera is what curRoomIndex follows (Dam: room 111 for
     * the intro, 135 once Bond is placed, measured with GE_BEANLOG). */
    if (g_CurrentPlayer && g_CurrentPlayer->prop && g_CurrentPlayer->prop->rooms[0] != 0xff) {
        return g_CurrentPlayer->prop->rooms[0];
    }
    /* D-141 (round 25a): the prop's list is filled only for a few frames of
     * the intro hand-over (Dam ~600..1100) and is 0xff again for the rest of
     * the level (GE_BEANLOG on lane 2: "curRoom 135 prop room 255" from frame
     * ~1200 on, walking or not). A build that missed that window — the
     * XBLA switch off at the level's load and turned on mid-level — never
     * happened. In first person curRoomIndex IS Bond's room. */
    if (g_CurrentPlayer && (g_CameraMode == CAMERAMODE_FP || g_CameraMode == CAMERAMODE_FP_NOINPUT)) {
        /* a not-yet-placed player has no room: never build against it */
        if (g_CurrentPlayer->curRoomIndex < 0) {
            return -1;
        }
        return g_CurrentPlayer->curRoomIndex;
    }
    return -1;
}

/*
 * A level's built lists and stand-in tiles are RETIRED at the next level's
 * load and freed only at the load after that (docs/tier-c.md "Lifetime").
 * The gfx task that last drew them is done before load_bg_file() runs (boss.c
 * leaves a stage only at pendingGfx == 0, and fast3d runs a task synchronously
 * before posting DONE), but fast3d keeps that frame's list as gfx_last_dl and
 * a D-093 camera-latch rerun can draw it again on the render thread until the
 * new stage presents its first frame - and that list points at these. The
 * pictures go at once: the forget bumps the texpack serial, so the renderer
 * drops its cache, and a rerun then samples the (still live) stand-in tile.
 */
struct retired {
    struct servedroom *served;
    s32 numRooms;
    u16 **standins;
    s32 numStandins;
    Mtx *mtx;
    Gfx *slots;
    Gfx *frameGfx;
    struct stri *backdrop;
    Vtx *backdropVtx;
    struct stageform *stage;
    u8 *inView;
    uintptr_t **patches;   /* Midas round 21: the build's G_VTXPATCH_EXT blocks */
    s32 numPatches;
};
static struct retired retiredSet;

static void freeStage(struct stageform *st)
{
    if (!st) {
        return;
    }
    free(st->sr.vtx);
    free(st->sr.opa);
    free(st->sr.xlu);
    if (st->sr.mesh) {
        gfxHdMeshRelease(st->sr.mesh);
        free((void *)st->sr.mesh->idx);
        free(st->sr.mesh);
    }
    free(st->sr.opa1);
    free(st->sr.xlu1);
    free(st->sr.draws);
    free(st->sr.fallback);
    for (s32 x = 0; x < 2; x++) {
        free(st->pass[x].full);
        free(st->pass[x].seg0);
        free(st->pass[x].segs);
    }
    free(st->spans);
    free(st);
}

static void freeSet(struct retired *s)
{
    for (s32 r = 0; s->served && r < s->numRooms; r++) {
        free(s->served[r].vtx);
        free(s->served[r].opa);
        free(s->served[r].xlu);
        if (s->served[r].mesh) {
            gfxHdMeshRelease(s->served[r].mesh);   /* GL objects: the render thread's next frame */
            free((void *)s->served[r].mesh->idx);
            free(s->served[r].mesh);
        }
        free(s->served[r].opa1);
        free(s->served[r].xlu1);
        free(s->served[r].draws);
        free(s->served[r].fallback);
    }
    free(s->served);
    for (s32 i = 0; i < s->numStandins; i++) {
        free(s->standins[i]);
    }
    free(s->standins);
    free(s->mtx);
    free(s->slots);
    free(s->frameGfx);
    free(s->backdrop);
    free(s->backdropVtx);
    freeStage(s->stage);
    free(s->inView);
    for (s32 i = 0; i < s->numPatches; i++) {   /* Midas round 21 */
        free(s->patches[i]);
    }
    free(s->patches);
    memset(s, 0, sizeof(*s));
}

static void retireServed(void)
{
    freeSet(&retiredSet);
    retiredSet.served = served;
    retiredSet.numRooms = numRooms;
    retiredSet.standins = standins;
    retiredSet.numStandins = numStandins;
    retiredSet.mtx = roomMtx;
    retiredSet.slots = scrollSlots;
    retiredSet.frameGfx = frameGfx;
    frameGfx = NULL;
    frameGfxCap = 0;
    retiredSet.backdrop = backdrop;
    retiredSet.backdropVtx = backdropVtx;
    retiredSet.stage = stage;
    retiredSet.inView = roomInView;
    /* Midas round 21: the patches go with the Vtx they point into; the
     * mirror is the game's and stays */
    retiredSet.patches = darkPatches;
    retiredSet.numPatches = numDarkPatches;
    darkPatches = NULL;
    numDarkPatches = 0;
    darkForgetBuild();
    stage = NULL;
    roomInView = NULL;
    served = NULL;
    numRooms = 0;
    numServed = 0;
    standins = NULL;
    numStandins = 0;
    roomMtx = NULL;
    scrollSlots = NULL;
    numScrollSlots = 0;
    backdrop = NULL;
    backdropVtx = NULL;
    numBackdrop = 0;
    free(pics);
    pics = NULL;
    numPics = 0;
    free(roomHidden);
    roomHidden = NULL;
    numHidden = 0;
    texpackForgetBeanPictures();
}

/* dev: GE_BEANSKIP="decals,backed,fights,cutclamp,twins,alphatest,weld,backdrop,move,lift,corners,scroll,shadows"
 * leaves a pass out (the before/after pairs) */
static s32 skipPass(const char *name)
{
    const char *env = getenv("GE_BEANSKIP");
    if (!env || !*env) {
        return 0;
    }
    const size_t n = strlen(name);
    for (const char *p = env; (p = strstr(p, name)) != NULL; p += n) {
        if ((p == env || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) {
            return 1;
        }
    }
    return 0;
}


/* ------------------------------------------------- the built-level cache */

/*
 * $C/xbla/levels/<key>_<beanfile-hash>_<n64bg-hash>_v<builder>.bin (round C2,
 * D-114; PD's hdcache): the level's triangles after every pass and the
 * dealing, the backdrop's, and each picture's clamp. A load skips reading
 * Bean's triangles, the passes and the dealing; the pictures are decoded and
 * the rooms written as on a cold build. Bump BUILDER_VERSION whenever a pass
 * or the dealing changes what it leaves.
 */
#define BUILDER_VERSION 5   /* 4: round 25b, struct stri unres/shader (D-142); 5: round 29c, brightness match (D-223) */
#define CACHE_MAGIC 0x4c564c42u /* "BLVL" */

struct cachedpic {
    u8 clampv;
    s16 clampshift;
};

static u64 hashBytes(u64 h, const void *data, size_t len)
{
    const u8 *p = data;
    while (len >= 8) {
        u64 w;
        memcpy(&w, p, 8);
        h ^= w * 0x9e3779b97f4a7c15ull;
        h = (h << 27 | h >> 37) * 0xbf58476d1ce4e5b9ull;
        p += 8;
        len -= 8;
    }
    while (len--) {
        h = (h ^ *p++) * 0x100000001b3ull;
    }
    return h ^ (h >> 31);
}

static s32 cacheAllowed(void)
{
    return !getenv("GE_BEANNOCACHE") && !getenv("GE_BEANSKIP") && !getenv("GE_BEANHIDETEX");
}

static void cacheName(const char *bean, const struct beanlevel *level, const struct rawroom *rooms, s32 n,
                      char *out, size_t len)
{
    u32 flen;
    const u8 *file = beanLevelFileBytes(level, &flen);
    const u64 hb = hashBytes(0x1234567ull ^ flen, file, flen);
    u64 hn = 0x7654321ull ^ (u64)n;
    for (s32 r = 1; r < n; r++) {
        hn = hashBytes(hn ^ rooms[r].vtxlen, rooms[r].vtx, rooms[r].vtxlen);
        hn = hashBytes(hn ^ rooms[r].gdllen[0], rooms[r].gdl[0], rooms[r].gdllen[0]);
        hn = hashBytes(hn ^ rooms[r].gdllen[1], rooms[r].gdl[1], rooms[r].gdllen[1]);
        hn = hashBytes(hn, rooms[r].pos, sizeof(rooms[r].pos));
    }
    char dir[768];
    snprintf(dir, sizeof(dir), "%s/levels", sysResolvePath(TEXPACK_XBLA_DIR));
    mkdir(sysResolvePath(TEXPACK_XBLA_DIR), 0755);
    mkdir(dir, 0755);
    snprintf(out, len, "%s/%s_%016llx_%016llx_v%d.bin", dir, bean, (unsigned long long)hb, (unsigned long long)hn,
             BUILDER_VERSION);
}

/* D-275 (round 31b): our own cache file name, exactly as cacheName() writes it:
 * <key>_<16 hex>_<16 hex>_v<digits>.bin, key = [a-z0-9]+. Returns the version,
 * -1 if the name is not ours; *keyLen gets the key's length. A trailing
 * ".part" (an interrupted cacheSave) is reported through *part. */
static s32 cacheParseName(const char *name, size_t *keyLen, s32 *part)
{
    size_t n = strlen(name), k = 0;
    *part = 0;
    if (n > 5 && !strcmp(name + n - 5, ".part")) {
        *part = 1;
        n -= 5;
    }
    if (n < 4 || strncmp(name + n - 4, ".bin", 4)) {
        return -1;
    }
    while (k < n && ((name[k] >= 'a' && name[k] <= 'z') || (name[k] >= '0' && name[k] <= '9'))) {
        k++;
    }
    if (k == 0 || k + 1 + 16 + 1 + 16 + 2 + 1 + 4 > n || name[k] != '_') {
        return -1;
    }
    const char *p = name + k + 1;
    for (s32 seg = 0; seg < 2; seg++) {
        for (s32 i = 0; i < 16; i++, p++) {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
                return -1;
            }
        }
        if (*p++ != '_') {
            return -1;
        }
    }
    if (*p++ != 'v') {
        return -1;
    }
    s32 v = 0, digits = 0;
    while (p < name + n - 4 && *p >= '0' && *p <= '9' && digits < 6) {
        v = v * 10 + (*p++ - '0');
        digits++;
    }
    if (digits == 0 || p != name + n - 4) {
        return -1;
    }
    *keyLen = k;
    return v;
}

/* D-275: every BUILDER_VERSION bump used to leave the previous generation of
 * level caches behind for ever (Austin's phone: Dam _v3 + _v4, 19.8 MB). At
 * startup, delete from $C/xbla/levels ONLY files whose name is exactly ours
 * and whose version is not BUILDER_VERSION, and our own .part leftovers older
 * than ten minutes (a live cacheSave in another process on the oracle's shared
 * data dir is never that old). Nothing else in the directory, and no other
 * directory, is touched. GE_BEANNOPRUNE=1 keeps everything (A/B). */
void beanStageCachePrune(void)
{
    if (getenv("GE_BEANNOPRUNE")) {
        return;
    }
    char dir[768], path[1100];
    snprintf(dir, sizeof(dir), "%s/levels", sysResolvePath(TEXPACK_XBLA_DIR));
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    s32 removed = 0;
    long long bytes = 0;
    const time_t now = time(NULL);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t keyLen = 0;
        s32 part = 0;
        const s32 v = cacheParseName(e->d_name, &keyLen, &part);
        if (v < 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (part ? (now - st.st_mtime < 600) : (v == BUILDER_VERSION)) {
            continue;
        }
        if (remove(path) == 0) {
            removed++;
            bytes += (long long)st.st_size;
            sysLogPrintf(LOG_NOTE, "beanstage: pruned %s (%s, %lld bytes)", e->d_name,
                         part ? "unfinished write" : "older builder version", (long long)st.st_size);
        }
    }
    closedir(d);
    if (removed) {
        sysLogPrintf(LOG_NOTE, "D-275: pruned %d stale level cache file(s), %.1f MB (builder v%d is current)", removed,
                     (double)bytes / (1024.0 * 1024.0), BUILDER_VERSION);
    }
}

/* D-275: a fresh save for a level supersedes every other cache of the same key
 * (an older layer's or ROM's hash, or another version): only the file just
 * written can ever be read again for that key. */
static void cachePruneSiblings(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || getenv("GE_BEANNOPRUNE")) {
        return;
    }
    const char *mine = slash + 1;
    size_t myKey = 0;
    s32 part = 0;
    if (cacheParseName(mine, &myKey, &part) < 0 || part) {
        return;
    }
    char dir[768], other[1100];
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t k = 0;
        if (cacheParseName(e->d_name, &k, &part) < 0 || part || k != myKey || strncmp(e->d_name, mine, k)
                || !strcmp(e->d_name, mine)) {
            continue;
        }
        snprintf(other, sizeof(other), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(other, &st) == 0 && S_ISREG(st.st_mode) && remove(other) == 0) {
            sysLogPrintf(LOG_NOTE, "D-275: pruned %s (superseded by %s, %lld bytes)", e->d_name, mine,
                         (long long)st.st_size);
        }
    }
    closedir(d);
}

static s32 cacheLoad(const char *path, struct collect *c, struct cachedpic **outPics, s32 numtex)
{
    FILE *f = fopen(path, "rb");
    u32 hdr[7];
    s32 ok = 0;
    u8 *const hiddenBefore = roomHidden;
    if (!f) {
        return 0;
    }
    if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == CACHE_MAGIC && hdr[1] == BUILDER_VERSION
            && hdr[2] == sizeof(struct stri) && (s32)hdr[5] == numtex && hdr[3] < 4000000u && hdr[4] < 1000000u
            && hdr[6] < 100000u) {
        struct stri *tris = malloc(sizeof(struct stri) * (size_t)(hdr[3] ? hdr[3] : 1));
        struct stri *bd = hdr[4] ? malloc(sizeof(struct stri) * (size_t)hdr[4]) : NULL;
        struct cachedpic *cp = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*cp));
        if (tris && (bd || !hdr[4]) && cp
                && fread(tris, sizeof(struct stri), hdr[3], f) == hdr[3]
                && (!hdr[4] || fread(bd, sizeof(struct stri), hdr[4], f) == hdr[4])
                && (!hdr[6] || ((roomHidden = calloc(hdr[6], 1)) != NULL && fread(roomHidden, 1, hdr[6], f) == hdr[6]))
                && (numtex <= 0 || fread(cp, sizeof(*cp), (size_t)numtex, f) == (size_t)numtex)) {
            c->tris = tris;
            c->num = (s32)hdr[3];
            c->cap = c->num;
            backdrop = bd;
            numBackdrop = (s32)hdr[4];
            *outPics = cp;
            ok = 1;
        } else {
            free(tris);
            free(bd);
            free(cp);
            if (roomHidden != hiddenBefore) {
                /* Midas round 22b: a short file's hidden-room block, read in part */
                free(roomHidden);
                roomHidden = hiddenBefore;
            }
        }
    }
    fclose(f);
    if (!ok) {
        sysLogPrintf(LOG_WARNING, "beanstage: %s is not a cache this build reads; building", path);
    }
    return ok;
}

/* 1 once the file is in place under its own name (Midas round 22b: the cold
 * worker's "kept" is this, not whether some file of that name exists) */
static s32 cacheSave(const char *path, const struct collect *c, s32 hiddenLen)
{
    char tmp[1100];
    s32 kept = 0;
    for (s32 t = 0; t < c->num; t++) {
        kept += c->tris[t].room > 0;
    }
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return 0;
    }
    const u32 hdr[7] = { CACHE_MAGIC, BUILDER_VERSION, (u32)sizeof(struct stri), (u32)kept, (u32)numBackdrop, (u32)numPics,
                         (u32)(roomHidden ? hiddenLen : 0) };
    s32 ok = fwrite(hdr, sizeof(hdr), 1, f) == 1;
    for (s32 t = 0; ok && t < c->num; t++) {
        if (c->tris[t].room > 0) {
            ok = fwrite(&c->tris[t], sizeof(struct stri), 1, f) == 1;
        }
    }
    if (ok && numBackdrop) {
        ok = fwrite(backdrop, sizeof(struct stri), (size_t)numBackdrop, f) == (size_t)numBackdrop;
    }
    if (ok) {
        ok = fwrite(roomHidden ? roomHidden : (const u8 *)"", 1, roomHidden ? (size_t)hiddenLen : 0, f) == (roomHidden ? (size_t)hiddenLen : 0);
    }
    for (s32 t = 0; ok && t < numPics; t++) {
        struct cachedpic cp;
        memset(&cp, 0, sizeof(cp));
        cp.clampv = pics[t].clampv;
        cp.clampshift = pics[t].clampshift;
        ok = fwrite(&cp, sizeof(cp), 1, f) == 1;
    }
    ok = fclose(f) == 0 && ok;
    if (ok && rename(tmp, path) == 0) {
        struct stat st;
        stat(path, &st);
        sysLogPrintf(LOG_NOTE, "beanstage: built level kept: %s (%lld bytes)", path, (long long)st.st_size);
        cachePruneSiblings(path);
        return 1;
    }
    remove(tmp);
    return 0;
}

/*
 * Midas round 54 (docs/port-ios/74, R52-1 item 1): a cut-out picture that the
 * release also lays down as a cast shadow (a blended triangle with black
 * corners under full vertex alpha: Dam's pit and bungee gratings, picture 56,
 * shadows at 0x8b on the floor below) carries the shadow's mask in its alpha
 * channel: the grating's X truss is opaque there while its colour is pure
 * black (5763 of 36836 opaque texels have every channel under 12; the colour
 * shows only the truss's stubs at each rung). Drawn as a cut-out of its own,
 * that truss was solid black over the holes: Austin's black patches (bungee
 * grating box 12.97 % black, 3.68 % with N64 textures). Those texels are made
 * clear; the colour, the rungs and the stubs are untouched, and the shadow
 * keeps the rungs. GE_BEANKEYBLACK=0 restores the old picture.
 */
#define KEY_BLACK_MAX 12
static s32 triCastsShadow(const struct stri *tri)
{
    return tri->blend && !(tri->argb[0] & 0xffffff) && !(tri->argb[1] & 0xffffff) && !(tri->argb[2] & 0xffffff)
        && (tri->argb[0] >> 24) < 0xff;
}

static s32 keyBlackCaster(s32 caster, uint8_t *rgba, int32_t w, int32_t h)
{
    static s32 cfg = -1;
    if (cfg < 0) {
        const char *e = getenv("GE_BEANKEYBLACK");
        cfg = !(e && e[0] == '0');
    }
    if (!cfg || !rgba || w <= 0 || h <= 0) {
        return 0;
    }
    if (!caster) {
        return 0;
    }
    s32 keyed = 0, clear = 0;
    const u32 total = (u32)w * (u32)h;
    for (u32 i = 0; i < total; i++) {
        clear += rgba[i * 4 + 3] < 0x80;
    }
    if (clear == 0) {
        return 0;   /* not a cut-out: a shadow of a solid picture is not ours to touch */
    }
    for (u32 i = 0; i < total; i++) {
        uint8_t *px = &rgba[i * 4];
        if (px[3] && px[0] < KEY_BLACK_MAX && px[1] < KEY_BLACK_MAX && px[2] < KEY_BLACK_MAX) {
            px[3] = 0;
            keyed++;
        }
    }
    /* The clear texels are black underneath, so the filtered edge of every
     * rung and stub darkened to a black rim: bleed the opaque neighbours'
     * colour three texels out into the clear ones (alpha untouched) */
    uint8_t *src = keyed ? malloc((size_t)total * 4) : NULL;
    for (s32 pass = 0; src && pass < 3; pass++) {
        memcpy(src, rgba, (size_t)total * 4);
        for (s32 y = 0; y < h; y++) {
            for (s32 x = 0; x < w; x++) {
                uint8_t *px = &rgba[((size_t)y * (size_t)w + (size_t)x) * 4];
                u32 sum[3] = { 0, 0, 0 }, n = 0;
                if (src[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] >= 0x80 || px[3] == 1) {
                    continue;
                }
                for (s32 dy = -1; dy <= 1; dy++) {
                    for (s32 dx = -1; dx <= 1; dx++) {
                        const s32 xx = (x + dx + w) % w, yy = (y + dy + h) % h;
                        const uint8_t *q = &src[((size_t)yy * (size_t)w + (size_t)xx) * 4];
                        if ((dx || dy) && (q[3] >= 0x80 || q[3] == 1)) {
                            sum[0] += q[0];
                            sum[1] += q[1];
                            sum[2] += q[2];
                            n++;
                        }
                    }
                }
                if (n) {
                    px[0] = (uint8_t)(sum[0] / n);
                    px[1] = (uint8_t)(sum[1] / n);
                    px[2] = (uint8_t)(sum[2] / n);
                    if (px[3] == 0) {
                        px[3] = 1;   /* bled: a source for the next pass, still clear (alpha 1 of 255) */
                    }
                }
            }
        }
    }
    free(src);
    return keyed;
}

/* Decodes every picture a kept triangle uses, binds each to a stand-in, and
 * notes what the passes ask of it (alpha, soft, its edge rows, its slide). */
static void decodePictures(struct beanlevel *level, const struct collect *c, s32 *outPics, u32 *outBytes)
{
    *outPics = 0;
    *outBytes = 0;
    u8 *casters = calloc((size_t)(numPics > 0 ? numPics : 1), 1);   /* Midas round 54: keyBlackCaster() */
    for (s32 t = 0; t < c->num; t++) {
        if (c->tris[t].tex >= 0 && c->tris[t].tex < numPics) {
            pics[c->tris[t].tex].used = 1;
            if (casters && triCastsShadow(&c->tris[t])) {
                casters[c->tris[t].tex] = 1;
            }
        }
    }
    for (s32 t = 0; t < numBackdrop; t++) { /* a cached load has them apart already */
        if (backdrop[t].tex >= 0 && backdrop[t].tex < numPics) {
            pics[backdrop[t].tex].used = 1;
        }
    }
    for (s32 t = 0; t < numPics; t++) {
        int32_t w, h;
        pics[t].slot = -1;
        if (!pics[t].used) {
            continue;
        }
        uint8_t *rgba = beanLevelDecodeTexture(level, t, &w, &h);
        {
            const s32 keyed = keyBlackCaster(casters && casters[t], rgba, w, h);
            if (keyed && !coldCur) {
                sysLogPrintf(LOG_NOTE, "beanstage: picture %d %dx%d casts a shadow: %d black texels made clear", t, (int)w, (int)h,
                             keyed);
            }
        }
        if (rgba && beanLevelTextureIsGlassPane(level, t)) {
            /* D-228 (PD gebean.c): every glass pane of the release that kept
             * its alpha has it at a flat 119; one that went to DXT1 lost it
             * and, in the blended pass with opaque vertices and no material
             * colour, drew as a solid sheet - Frigate's speedboat windscreen
             * (a grey slab where GoldenEye has dark glass) */
            s32 allOpaque = 1;
            for (u32 i = 0; allOpaque && i < (u32)w * (u32)h; i++) {
                allOpaque = rgba[i * 4 + 3] == 0xff;
            }
            for (u32 i = 0; allOpaque && i < (u32)w * (u32)h; i++) {
                rgba[i * 4 + 3] = 119;
            }
            if (allOpaque) {
                numGlassPanes++;
            }
        }
        if (rgba && getenv("GE_BEANDUMPPIC")) {
            /* dev (round 54): GE_BEANDUMPPIC="56,57" writes those pictures' RGBA to $S/beanpic-<n>.png */
            char key[16], path[64];
            snprintf(key, sizeof(key), ",%d,", t);
            char list[256];
            snprintf(list, sizeof(list), ",%s,", getenv("GE_BEANDUMPPIC"));
            if (strstr(list, key)) {
                extern s32 pngWrite(const char *path, const u8 *pixels, s32 width, s32 height, s32 channels, s32 bottomRowFirst);
                snprintf(path, sizeof(path), "$S/beanpic-%d.png", t);
                pngWrite(sysResolvePath(path), rgba, w, h, 4, 0);
            }
        }
        u32 opaque = 0, rs = 0, gs = 0, bs = 0, half = 0;
        u64 hs[3] = { 0, 0, 0 };
        u64 e0 = 0, e1 = 0;
        const u32 total = rgba ? (u32)w * (u32)h : 0;
        for (u32 i = 0; i < total; i++) {
            opaque += rgba[i * 4 + 3] >= OPAQUE_ALPHA;
            rs += rgba[i * 4];
            gs += rgba[i * 4 + 1];
            bs += rgba[i * 4 + 2];
            if (rgba[i * 4 + 3] >= 0x80) {
                half++;
                hs[0] += rgba[i * 4];
                hs[1] += rgba[i * 4 + 1];
                hs[2] += rgba[i * 4 + 2];
            }
        }
        pics[t].mean[0] = half ? (f32)hs[0] / half : total ? (f32)rs / total : -1.0f;
        pics[t].mean[1] = half ? (f32)hs[1] / half : total ? (f32)gs / total : -1.0f;
        pics[t].mean[2] = half ? (f32)hs[2] / half : total ? (f32)bs / total : -1.0f;
        for (s32 x = 0; total && x < w; x++) {
            e0 += rgba[x * 4 + 3];
            e1 += rgba[((size_t)(h - 1) * (size_t)w + (size_t)x) * 4 + 3];
        }
        u16 *tile = newStandin(total ? texel5551(rs / total, gs / total, bs / total) : 0xffff);
        if (!rgba) {
            /* round 25b (D-142): an unresolved picture - the rooms that use
             * it are left GoldenEye's (build()) and the audit names it */
            int32_t sw = 0, sh = 0;
            beanLevelTextureSize(level, t, &sw, &sh);
            pics[t].failed = 1;
            sysLogPrintf(LOG_WARNING, "[xbla] unresolved: level %s picture %d (%dx%d) does not decode", audit.bean, t,
                         (int)sw, (int)sh);
        }
        if (!tile) {
            free(rgba);
            pics[t].used = 0;
            continue;
        }
        pics[t].standin = numStandins - 1;
        pics[t].alpha = total && opaque < total;
        pics[t].soft = total && (u64)opaque * 100 < (u64)total * SOFT_PERCENT;
        pics[t].edge[0] = total ? (u8)(e0 / (u64)w) : 0xff;
        pics[t].edge[1] = total ? (u8)(e1 / (u64)w) : 0xff;
        if (!skipPass("scroll")) {
            beanLevelTextureScroll(level, t, &pics[t].scroll[0], &pics[t].scroll[1]);
        }
        if (rgba) {
            (*outPics)++;
            *outBytes += total * 4;
            if (coldCur) {
                texpackBeanPictureRelease(rgba);   /* Midas round 22: the worker only measures */
            } else {
                texpackRegisterBeanPicture(tile, rgba, w, h);
            }
            if (getenv("GE_BEANDRAWSTAT")) {
                sysLogPrintf(LOG_NOTE, "beanstage: picture %d %dx%d alpha %d clampv %d scroll %d", t, (int)w, (int)h,
                             pics[t].alpha, pics[t].clampv, pics[t].scroll[0] != 0.0f || pics[t].scroll[1] != 0.0f);
            }
        }
    }
    free(casters);
}

/* ----------------------------------------------- the stage form (26c) */

/* Each G_HDMESH_EXT of a stage list = the runs writeMeshList() merged into it
 * (the same walk), and its rooms' index ranges in order. */
static s32 stageGroupsOf(struct stagepass *p, const struct gdlbuf *l, struct GfxHdMeshDraw *draws, s32 *nd)
{
    s32 g = 0, ns = 0;
    p->full = malloc(sizeof(*p->full) * (size_t)(l->numruns + 1));
    p->seg0 = malloc(sizeof(s32) * (size_t)(l->numruns + 2));
    p->segs = malloc(sizeof(*p->segs) * (size_t)(l->numruns + 1));
    if (!p->full || !p->seg0 || !p->segs) {
        return 0;
    }
    for (s32 r = 0; r < l->numruns;) {
        s32 r1 = r, end = l->runs[r].cmd1;
        const u32 first = l->runs[r].idx0;
        u32 count = l->runs[r].nidx;
        while (r1 + 1 < l->numruns && l->runs[r1 + 1].cmd0 == end && l->runs[r1 + 1].idx0 == first + count) {
            r1++;
            end = l->runs[r1].cmd1;
            count += l->runs[r1].nidx;
        }
        p->full[g] = &draws[(*nd)++];
        if (p->full[g]->first != first || p->full[g]->count != count) {
            return 0; /* not the walk writeMeshList() made */
        }
        p->seg0[g] = ns;
        for (s32 q = r; q <= r1; q++) {
            struct stageseg *last = ns > p->seg0[g] ? &p->segs[ns - 1] : NULL;
            if (last && last->room == l->runs[q].room && last->first + last->count == l->runs[q].idx0) {
                last->count += l->runs[q].nidx;
            } else {
                p->segs[ns].room = l->runs[q].room;
                p->segs[ns].first = l->runs[q].idx0;
                p->segs[ns].count = l->runs[q].nidx;
                ns++;
            }
        }
        g++;
        r = r1 + 1;
    }
    p->seg0[g] = ns;
    p->numgroups = g;
    p->numsegs = ns;
    return 1;
}

static struct stageform *buildStage(const struct collect *c, const struct rawroom *rooms, s32 n, s32 **lists,
                                    const s32 *listlen, const u16 *untextured)
{
    struct stageform *st;
    f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    s32 total = 0, numopa = 0, numxlu = 0, dropped = 0, ok = 0;
    for (s32 r = 1; r < n; r++) {
        if (!served[r].vtx) {
            continue;
        }
        total += listlen[r];
        for (s32 k = 0; k < 3; k++) {
            if (rooms[r].pos[k] != floorf(rooms[r].pos[k]) || fabsf(rooms[r].pos[k]) > 65536.0f) {
                sysLogPrintf(LOG_NOTE, "beanstage: no stage form (room %d's position is not whole units)", r);
                return NULL;
            }
            lo[k] = MINV(lo[k], rooms[r].pos[k]);
            hi[k] = MAXV(hi[k], rooms[r].pos[k]);
        }
    }
    st = total ? calloc(1, sizeof(*st)) : NULL;
    s32 *opalist = st ? malloc(sizeof(s32) * (size_t)total) : NULL;
    s32 *xlulist = st ? malloc(sizeof(s32) * (size_t)total) : NULL;
    if (!st || !opalist || !xlulist) {
        free(st);
        free(opalist);
        free(xlulist);
        return NULL;
    }
    for (s32 k = 0; k < 3; k++) {
        st->anchor[k] = floorf((lo[k] + hi[k]) * 0.5f);
    }
    /* the rooms' own split (their pictures already resolved by their pass) */
    for (s32 r = 1; r < n; r++) {
        for (s32 i = 0; served[r].vtx && i < listlen[r]; i++) {
            const struct stri *tri = &c->tris[lists[r][i]];
            if (texIsXlu(tri->tex) || triFades(tri)) {
                xlulist[numxlu++] = lists[r][i];
            } else {
                opalist[numopa++] = lists[r][i];
            }
        }
    }
    struct gdlbuf opa, xlu;
    struct vtxbuf vb;
    memset(&opa, 0, sizeof(opa));
    memset(&xlu, 0, sizeof(xlu));
    memset(&vb, 0, sizeof(vb));
    stageMode = 1;
    stageRooms = rooms;
    memcpy(stageAnchor, st->anchor, sizeof(stageAnchor));
    stageOverflow = 0;
    darkOwner = 0;   /* Midas round 21: the stage form's Vtx */
    writeLeaf(&opa, &vb, c->tris, opalist, numopa, 0, st->anchor, untextured, &dropped);
    writeLeaf(&xlu, &vb, c->tris, xlulist, numxlu, 1, st->anchor, untextured, &dropped);
    darkOwner = -1;
    stageMode = 0;
    free(opalist);
    free(xlulist);
    if (opa.num) {
        bgApplyDynamicCCRMLUT(opa.g, opa.g + opa.num, g_FogSkyIsEnabled ? 1 : 6);
        for (s32 i = 0; i < opa.numzupd; i++) {
            opa.g[opa.zupd[i]].words.w1 |= Z_UPD;
        }
    }
    if (xlu.num) {
        bgApplyDynamicCCRMLUT(xlu.g, xlu.g + xlu.num, g_FogSkyIsEnabled ? 5 : 7);
    }
    st->sr.vtx = vb.v;
    if (!stageOverflow && vb.num > 0) {
        st->bytes = writeRoomMesh(&st->sr, &vb, &opa, &xlu, 0);
    }
    if (st->sr.mesh) {
        s32 nd = 0;
        ok = (!opa.num || stageGroupsOf(&st->pass[0], &opa, st->sr.draws, &nd))
            && (!xlu.num || stageGroupsOf(&st->pass[1], &xlu, st->sr.draws, &nd)) && nd == st->sr.numdraws;
        st->pass[0].list = st->sr.opa1;
        st->pass[1].list = st->sr.xlu1;
        for (s32 x = 0; x < 2; x++) {
            for (st->pass[x].num = 0; st->pass[x].list && (u32)(st->pass[x].list[st->pass[x].num].words.w0 >> 24) != 0xb8u;
                 st->pass[x].num++) {
            }
        }
        st->spanCap = st->pass[0].numsegs + st->pass[1].numsegs;
        st->spans = calloc((size_t)MTX_RING * (size_t)(st->spanCap + 1), sizeof(*st->spans));
        ok = ok && st->spans;
    }
    free(opa.runs);
    free(xlu.runs);
    free(opa.zupd);
    free(xlu.zupd);
    free(opa.g);   /* form 0: the stage form draws only through the mesh */
    free(xlu.g);
    free(vb.idx);
    if (!ok) {
        sysLogPrintf(LOG_NOTE, "beanstage: no stage form (%s)", stageOverflow ? "a vertex out of reach of the anchor" : "build failed");
        freeStage(st);
        return NULL;
    }
    st->bytes += (u32)vb.num * (u32)sizeof(Vtx) + (u32)sizeof(*st)
        + (u32)(st->pass[0].numsegs + st->pass[1].numsegs) * (u32)(sizeof(struct stageseg) + MTX_RING * sizeof(*st->spans))
        + (u32)(st->pass[0].numgroups + st->pass[1].numgroups) * (u32)(sizeof(void *) + sizeof(s32));
    if (getenv("GE_BEANDRAWSTAT")) {
        for (s32 x = 0; x < 2; x++) {
            u64 tile = 0, cc = 0, rm = 0, cull = 0, bias = 0;
            s32 gi = 0;
            for (s32 i = 0; i < st->pass[x].num; i++) {
                const Gfx *g = &st->pass[x].list[i];
                const u32 op = (u32)(g->words.w0 >> 24);
                if (op == 0xfd) tile = (u64)g->words.w1;
                else if (op == 0xfc) cc = ((u64)(u32)g->words.w0 << 32) | (u32)g->words.w1;
                else if (op == 0xb9) rm = (u64)(u32)g->words.w1;
                else if (op == 0xb6) cull &= ~(u64)(u32)g->words.w1;
                else if (op == 0xb7) cull |= (u64)(u32)g->words.w1;
                else if (op == G_SETDEPTHBIAS_EXT) bias = (u64)(u32)g->words.w1;
                else if (op == G_HDMESH_EXT) {
                    s32 pic = -1;
                    for (s32 t = 0; t < numPics; t++) {
                        if (pics[t].used && (u64)(uintptr_t)standins[pics[t].standin] == tile) pic = t;
                    }
                    sysLogPrintf(LOG_NOTE, "beanstage: stage %s group %d: picture %d cc %016llx rm %08llx cull %llx bias %llu, %d rooms, %u tris",
                                 x ? "xlu" : "opa", gi, pic, (unsigned long long)cc, (unsigned long long)rm, (unsigned long long)cull,
                                 (unsigned long long)bias, st->pass[x].seg0[gi + 1] - st->pass[x].seg0[gi], st->pass[x].full[gi]->count / 3);
                    gi++;
                }
            }
        }
    }
    sysLogPrintf(LOG_NOTE, "beanstage: stage form: anchor %.0f %.0f %.0f, %d vertices, %u indices (%s); opaque %d draws over %d room "
                 "ranges, translucent %d over %d; %u host bytes", st->anchor[0], st->anchor[1], st->anchor[2], vb.num,
                 st->sr.mesh->numidx, st->sr.mesh->idx32 ? "32-bit" : "16-bit", st->pass[0].numgroups, st->pass[0].numsegs,
                 st->pass[1].numgroups, st->pass[1].numsegs, st->bytes);
    return st;
}

/* ------------------------------------------- brightness match (round 29c) */

/*
 * HD rooms as bright as GoldenEye's own (D-223; after PD gebeanstage.c
 * matchN64Brightness(), the factors our own). The release bakes its light into
 * the level's vertex colours and in places baked it much darker than
 * GoldenEye lights the same room (Train's carriages, Streets, Silo, Statue
 * Park's fields; D-168). Both sides go through the same combiner (TEXEL0 x
 * SHADE, the room's render-mode/combiner LUT, its fog), so what differs is
 * vertex colour x picture colour: a triangle's brightness is the Rec. 601
 * luma of its mean vertex colour times its picture's mean colour, channel by
 * channel - the release's pictures from decodePictures(), GoldenEye's ROM
 * textures from texpackTextureMeans(), each N64 triangle's texture and
 * vertex colours from the raw room lists (fileRoomTriangles()).
 *
 * Each HD triangle is held against the N64 triangle the dealing found under
 * it (target[]). Gains come from a smooth field rather than PD's per-surface
 * and per-room factors (those left hard-edged patches on Statue Park's grass
 * where one surface or room met another): every paired triangle splats its
 * area, brightness and target trilinearly into BRIGHT_CELL-unit cells, once
 * for its picture and once for any picture, and each corner reads both back
 * trilinearly. A corner takes its picture's field where that has enough area,
 * blended into the any-picture field (Train's dark wood walls are matched as
 * wood, not as the average with the room's bright vents, PD) and, where the
 * field itself thins (GoldenEye's huge triangles are not in the dealing grid:
 * Statue Park's far field is six of them), into its room's own ratio.
 * Gain = GoldenEye's / the release's, never below 1 (nothing is made darker,
 * so a place the release lit brighter stays as it is), at most
 * BRIGHT_MAX_GAIN; a channel past 255 is held there. Blended, fading, water,
 * untextured and shader-coloured triangles keep their colours.
 *
 * A cold build only; the colours are what the level cache keeps
 * (BUILDER_VERSION 5). brightOn(): every paired level but Dam (D-224).
 */
#define BRIGHT_REPORT   0.7f
#define BRIGHT_MAX_GAIN 6.0f
#define BRIGHT_MIN_AREA 1000.0f   /* PD's, in its (X file) units: x levelscale^2 here */
#define BRIGHT_TEXNUMS  0x1000
#define BRIGHT_CELL     400.0f    /* the field's cell, world units (x levelscale here) */

static const f32 brightLuma[3] = { 0.299f, 0.587f, 0.114f };

/* D-224: Dam's look is Austin's signed-off reference and the replay
 * references are Dam's; its rooms are measured and logged, not changed.
 * GE_BEANBRIGHT=0 turns the match off everywhere, =all on Dam too (dev;
 * GE_BEANNOCACHE=1 with either, the cache does not key on it). */
static s32 brightOn(const char *key)
{
    const char *env = getenv("GE_BEANBRIGHT");
    if (env && *env) {
        if (strcmp(env, "0") == 0) return 0;
        if (strcmp(env, "all") == 0) return 1;
    }
    return strcmp(key, "dam") != 0;
}

static f32 hdTriBright(const struct stri *t)
{
    const f32 *m = pics[t->tex].mean;
    f32 luma = 0.0f;
    for (s32 ch = 0; ch < 3; ch++) {
        const s32 sh = 16 - ch * 8;
        const f32 v = (f32)(((t->argb[0] >> sh) & 0xff) + ((t->argb[1] >> sh) & 0xff) + ((t->argb[2] >> sh) & 0xff)) / 3.0f;
        luma += brightLuma[ch] * v * m[ch] / 255.0f;
    }
    return luma;
}

static s32 hdTriMeasured(const struct stri *t, s32 n)
{
    return t->room > 0 && t->room < n && t->tex >= 0 && t->tex < numPics && pics[t->tex].used && !pics[t->tex].failed
        && pics[t->tex].mean[0] >= 0.0f && !pics[t->tex].water && !t->blend && !t->plain && !t->water && !t->shader
        && !t->unres && !texIsXlu(t->tex) && !triFades(t);
}

struct brightsum {
    f64 area, hd, go;
};

/* The brightness field's cells (matchBrightness()): open addressing on
 * (cell, picture), picture -1 = any picture; grows at half full. */
struct fieldcell {
    s32 x, y, z, tex;   /* tex -2: empty */
    f32 area, hd, go;
};

struct fieldmap {
    struct fieldcell *c;
    u32 mask;
    u32 used;
};

static u32 fieldHash(s32 x, s32 y, s32 z, s32 tex)
{
    return (u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u ^ (u32)(tex + 2) * 2654435761u;
}

static s32 fieldInit(struct fieldmap *m, u32 size)
{
    m->c = malloc(sizeof(*m->c) * size);
    m->mask = size - 1;
    m->used = 0;
    for (u32 i = 0; m->c && i < size; i++) {
        m->c[i].tex = -2;
    }
    return m->c != NULL;
}

static void fieldFree(struct fieldmap *m)
{
    free(m->c);
    m->c = NULL;
}

static struct fieldcell *fieldAt(struct fieldmap *m, s32 x, s32 y, s32 z, s32 tex, s32 add)
{
    for (u32 i = fieldHash(x, y, z, tex) & m->mask, probe = 0; probe <= m->mask; probe++, i = (i + 1) & m->mask) {
        struct fieldcell *e = &m->c[i];
        if (e->tex == -2) {
            if (!add) {
                return NULL;
            }
            e->x = x;
            e->y = y;
            e->z = z;
            e->tex = tex;
            e->area = e->hd = e->go = 0.0f;
            m->used++;
            return e;
        }
        if (e->x == x && e->y == y && e->z == z && e->tex == tex) {
            return e;
        }
    }
    return NULL;
}

static s32 fieldGrow(struct fieldmap *m)
{
    struct fieldmap n;
    if (!fieldInit(&n, (m->mask + 1) * 2)) {
        return 0;
    }
    for (u32 i = 0; i <= m->mask; i++) {
        const struct fieldcell *e = &m->c[i];
        if (e->tex != -2) {
            *fieldAt(&n, e->x, e->y, e->z, e->tex, 1) = *e;
        }
    }
    fieldFree(m);
    *m = n;
    return 1;
}

/* p in cell units, shifted so cell centres are at whole numbers */
static s32 fieldSplat(struct fieldmap *m, const f32 *p, s32 tex, f32 area, f32 hd, f32 go)
{
    const s32 x0 = (s32)floorf(p[0]), y0 = (s32)floorf(p[1]), z0 = (s32)floorf(p[2]);
    const f32 fx = p[0] - (f32)x0, fy = p[1] - (f32)y0, fz = p[2] - (f32)z0;
    for (s32 i = 0; i < 8; i++) {
        const f32 w = ((i & 1) ? fx : 1.0f - fx) * ((i & 2) ? fy : 1.0f - fy) * ((i & 4) ? fz : 1.0f - fz);
        if (w <= 0.0f) {
            continue;
        }
        if (m->used * 2 > m->mask && !fieldGrow(m)) {
            return 0;
        }
        struct fieldcell *e = fieldAt(m, x0 + (i & 1), y0 + ((i >> 1) & 1), z0 + ((i >> 2) & 1), tex, 1);
        if (!e) {
            return 0;
        }
        e->area += w * area;
        e->hd += w * hd;
        e->go += w * go;
    }
    return 1;
}

static void fieldRead(struct fieldmap *m, const f32 *p, s32 tex, f64 *out)
{
    const s32 x0 = (s32)floorf(p[0]), y0 = (s32)floorf(p[1]), z0 = (s32)floorf(p[2]);
    const f32 fx = p[0] - (f32)x0, fy = p[1] - (f32)y0, fz = p[2] - (f32)z0;
    out[0] = out[1] = out[2] = 0.0;
    for (s32 i = 0; i < 8; i++) {
        const f32 w = ((i & 1) ? fx : 1.0f - fx) * ((i & 2) ? fy : 1.0f - fy) * ((i & 4) ? fz : 1.0f - fz);
        const struct fieldcell *e = w > 0.0f ? fieldAt(m, x0 + (i & 1), y0 + ((i >> 1) & 1), z0 + ((i >> 2) & 1), tex, 0) : NULL;
        if (e) {
            out[0] += (f64)w * e->area;
            out[1] += (f64)w * e->hd;
            out[2] += (f64)w * e->go;
        }
    }
}

/* dealNear[t]: the filetris index the dealing found for HD triangle t, or -1 */
static void matchBrightness(struct collect *c, const s32 *dealNear, s32 n, const char *key, const char *bean,
                            s32 *outChanged, f64 *outMs)
{
    const u64 t0 = sysGetMicroseconds();
    const f32 ls = coldCur ? coldCur->levelscale   /* Midas round 22: read on the sim thread */
        : levelinfotable[levelentry_index].levelscale > 0.0f ? levelinfotable[levelentry_index].levelscale : 1.0f;
    const f64 minArea = BRIGHT_MIN_AREA * (f64)ls * (f64)ls;
    const s32 on = brightOn(key);
    f32 (*texMean)[3] = calloc(BRIGHT_TEXNUMS + 1, sizeof(*texMean));
    s32 *nums = malloc(sizeof(s32) * BRIGHT_TEXNUMS);
    f32 *means = malloc(sizeof(f32) * 3 * BRIGHT_TEXNUMS);
    u8 *used = calloc(BRIGHT_TEXNUMS, 1);
    f32 *fileB = numFmeas ? malloc(sizeof(f32) * (size_t)numFmeas) : NULL;
    f32 *target = c->num ? malloc(sizeof(f32) * (size_t)c->num) : NULL;
    struct brightsum *room = calloc((size_t)n + 1, sizeof(*room));   /* hd: all measured; go: N64 own */
    s32 num = 0, changed = 0, clamped = 0, measured = 0, under = 0, paired = 0;
    f64 hdall = 0, hdarea = 0, goall = 0, goarea = 0;
    f64 *hdA = NULL, *hdS = NULL;   /* per room, the release's measured area and brightness before the match */
    char list[1024];
    s32 at = 0;

    *outChanged = 0;
    list[0] = 0;
    if (!texMean || !nums || !means || !used || !fileB || !target || !room) {
        goto done;
    }

    /* GoldenEye's side: its textures' colours, then each triangle's brightness */
    for (s32 i = 0; i < numFmeas; i++) {
        if (fmeas[i].tex >= 0 && fmeas[i].tex < BRIGHT_TEXNUMS && !used[fmeas[i].tex]) {
            used[fmeas[i].tex] = 1;
            nums[num++] = fmeas[i].tex;
        }
    }
    if (coldCur) {
        /* Midas round 22: the worker may not decode GoldenEye's textures;
         * the sim thread took the same numbers' means before it started */
        if (coldCur->numMeans == num && (!num || !memcmp(coldCur->meanNums, nums, sizeof(s32) * (size_t)num))) {
            memcpy(means, coldCur->means, sizeof(f32) * 3 * (size_t)num);
        } else {
            for (s32 i = 0; i < num * 3; i++) means[i] = -1.0f;
            sysLogPrintf(LOG_WARNING, "beanstage: cold worker: texture means do not match (%d vs %d)", num, coldCur->numMeans);
        }
    } else {
        texpackTextureMeans(nums, num, means);
    }
    {
        f64 sum[3] = { 0, 0, 0 };
        s32 known = 0;
        for (s32 t = 0; t <= BRIGHT_TEXNUMS; t++) texMean[t][0] = -1.0f;
        for (s32 i = 0; i < num; i++) {
            memcpy(texMean[nums[i]], &means[i * 3], sizeof(texMean[0]));
            if (means[i * 3] >= 0.0f) {
                for (s32 ch = 0; ch < 3; ch++) sum[ch] += means[i * 3 + ch];
                known++;
            }
        }
        for (s32 ch = 0; ch < 3; ch++) texMean[BRIGHT_TEXNUMS][ch] = known ? (f32)(sum[ch] / known) : 255.0f;
    }
    for (s32 i = 0; i < numFmeas; i++) {
        const struct fmeas *m = &fmeas[i];
        const f32 *tm = m->tex >= 0 && m->tex < BRIGHT_TEXNUMS && texMean[m->tex][0] >= 0.0f ? texMean[m->tex]
                                                                                             : texMean[BRIGHT_TEXNUMS];
        f32 luma = 0.0f;
        for (s32 ch = 0; ch < 3; ch++) luma += brightLuma[ch] * m->rgb[ch] * tm[ch] / 255.0f;
        fileB[i] = luma;
        if (!m->xlu && m->room > 0 && m->room < n) {
            room[m->room].go += (f64)m->area * luma;
            room[m->room].area += m->area; /* N64 area, reused below as the N64 side's */
        }
    }

    /* the release's side, and each HD triangle's target */
    hdA = calloc((size_t)n + 1, sizeof(f64));
    hdS = calloc((size_t)n + 1, sizeof(f64));
    if (!hdA || !hdS) {
        goto done;
    }
    {
        for (s32 t = 0; t < c->num; t++) {
            const struct stri *tri = &c->tris[t];
            s32 mi = -1;
            target[t] = -1.0f;
            if (dealNear[t] >= 0 && dealNear[t] < capGridMeas) {
                mi = gridMeas[dealNear[t]];
            }
            if (mi >= 0 && !fmeas[mi].xlu) {
                target[t] = fileB[mi];
            }
            if (hdTriMeasured(tri, n)) {
                const f32 a = triArea3((const f32 (*)[3])tri->pos);
                hdA[tri->room] += a;
                hdS[tri->room] += (f64)a * hdTriBright(tri);
            }
        }
        for (s32 r = 1; r < n; r++) {
            f32 ratio;
            if (hdA[r] < minArea || room[r].area < minArea || room[r].go <= 0.0) {
                continue;
            }
            measured++;
            hdall += hdS[r];
            hdarea += hdA[r];
            goall += room[r].go;
            goarea += room[r].area;
            ratio = (f32)((hdS[r] / hdA[r]) / (room[r].go / room[r].area));
            if (ratio < BRIGHT_REPORT) {
                under++;
                if (at < (s32)sizeof(list) - 24) {
                    at += snprintf(list + at, sizeof(list) - (size_t)at, " %d:%.2f", r, ratio);
                }
            }
        }
    }
    if (getenv("GE_BEANBRIGHTLOG")) {
        /* dev: per room, the mean vertex colour and picture colour each side */
        for (s32 r = 1; r < n; r++) {
            f64 nv[3] = { 0 }, nt[3] = { 0 }, na = 0, hv[3] = { 0 }, ht[3] = { 0 }, ha = 0;
            s32 nn = 0, nk = 0;
            for (s32 i = 0; i < numFmeas; i++) {
                const struct fmeas *m = &fmeas[i];
                if (m->room != r || m->xlu) continue;
                const f32 *tm = m->tex >= 0 && m->tex < BRIGHT_TEXNUMS && texMean[m->tex][0] >= 0.0f ? texMean[m->tex] : texMean[BRIGHT_TEXNUMS];
                nk += !(m->tex >= 0 && m->tex < BRIGHT_TEXNUMS && texMean[m->tex][0] >= 0.0f);
                for (s32 ch = 0; ch < 3; ch++) { nv[ch] += m->area * m->rgb[ch]; nt[ch] += m->area * tm[ch]; }
                na += m->area;
                nn++;
            }
            for (s32 t = 0; t < c->num; t++) {
                const struct stri *tri = &c->tris[t];
                if (tri->room != r || !hdTriMeasured(tri, n)) continue;
                const f32 a = triArea3((const f32 (*)[3])tri->pos);
                for (s32 ch = 0; ch < 3; ch++) {
                    const s32 sh = 16 - ch * 8;
                    hv[ch] += a * (((tri->argb[0] >> sh) & 0xff) + ((tri->argb[1] >> sh) & 0xff) + ((tri->argb[2] >> sh) & 0xff)) / 3.0;
                    ht[ch] += a * pics[tri->tex].mean[ch];
                }
                ha += a;
            }
            if (na > 0 && ha > 0) {
                sysLogPrintf(LOG_NOTE, "beanstage: bright room %d: N64 %d tris (%d unknown tex) vtx %.0f %.0f %.0f tex %.0f %.0f %.0f | HD vtx %.0f %.0f %.0f pic %.0f %.0f %.0f",
                             r, nn, nk, nv[0] / na, nv[1] / na, nv[2] / na, nt[0] / na, nt[1] / na, nt[2] / na,
                             hv[0] / ha, hv[1] / ha, hv[2] / ha, ht[0] / ha, ht[1] / ha, ht[2] / ha);
            }
        }
        for (s32 i = 0; i < num && i < 400; i++) {
            sysLogPrintf(LOG_NOTE, "beanstage: bright tex %d: %.0f %.0f %.0f", nums[i], means[i * 3], means[i * 3 + 1], means[i * 3 + 2]);
        }
    }
    sysLogPrintf(LOG_NOTE, "beanstage: %s brightness against GoldenEye's rooms: level %.2f (HD %.1f, N64 %.1f), %d of %d "
                 "rooms under %.2f (room:HD/N64)%s%s", bean, hdarea > 0 && goall > 0 ? (hdall / hdarea) / (goall / goarea) : 0.0,
                 hdarea > 0 ? hdall / hdarea : 0.0, goarea > 0 ? goall / goarea : 0.0, under, measured, BRIGHT_REPORT, list,
                 on ? " - matching" : " - left as the release lit it");
    if (!on) {
        goto done;
    }

    /*
     * The field (round 29c's own, not PD's per-surface factors: those left
     * hard-edged patches on Statue Park's grass where one surface met
     * another or a room's bound). Each paired triangle's sums - its area, its
     * brightness and GoldenEye's under it - are splatted trilinearly from its
     * middle into a grid of BRIGHT_CELL world units, once for its picture and
     * once for any picture; each corner reads both back trilinearly, so the
     * gain varies smoothly in space whatever the rooms and meshes. A corner
     * takes its picture's field where that has BRIGHT_MIN_AREA under it,
     * blended into the any-picture field where it has less (Train's dark wood
     * walls matched as wood, not as the room's average with its bright
     * vents, PD). Gain = GoldenEye's / the release's there, never below 1
     * (nothing darker), at most BRIGHT_MAX_GAIN.
     */
    {
        const f32 cell = BRIGHT_CELL * ls;
        struct fieldmap fm;
        if (!fieldInit(&fm, 1 << 16)) {
            goto done;
        }
        for (s32 t = 0; t < c->num; t++) {
            const struct stri *tri = &c->tris[t];
            if (!hdTriMeasured(tri, n) || target[t] < 0.0f) {
                continue;
            }
            f32 mid[3];
            for (s32 k = 0; k < 3; k++) {
                mid[k] = (tri->pos[0][k] + tri->pos[1][k] + tri->pos[2][k]) / 3.0f / cell - 0.5f;
            }
            const f32 a = triArea3((const f32 (*)[3])tri->pos);
            const f32 mine = hdTriBright(tri);
            if (!fieldSplat(&fm, mid, tri->tex, a, a * mine, a * target[t])
                    || !fieldSplat(&fm, mid, -1, a, a * mine, a * target[t])) {
                fieldFree(&fm);
                goto done;
            }
            paired++;
        }
        const s32 dbgRoom = getenv("GE_BEANBRIGHTROOM") ? atoi(getenv("GE_BEANBRIGHTROOM")) : -1;
        s32 dbgN = 0;
        for (s32 t = 0; t < c->num; t++) {
            struct stri *tri = &c->tris[t];
            f32 vgain[3];
            s32 any = 0;
            if (!hdTriMeasured(tri, n)) {
                continue;
            }
            for (s32 k = 0; k < 3; k++) {
                f32 at[3];
                f64 sa[3], sp[3];
                for (s32 j = 0; j < 3; j++) {
                    at[j] = tri->pos[k][j] / cell - 0.5f;
                }
                fieldRead(&fm, at, tri->tex, sp);
                fieldRead(&fm, at, -1, sa);
                const f64 minA = minArea * 0.25;
                const s32 r = tri->room;
                /* where pairs are few (GoldenEye's huge triangles are not in
                 * the dealing grid: Statue Park's far field is 6 of them),
                 * the room's own ratio, blended in as the field thins */
                f64 groom = 1.0;
                if (hdA[r] >= minArea && room[r].area >= minArea && hdS[r] > 0.0) {
                    groom = (room[r].go / room[r].area) / (hdS[r] / hdA[r]);
                }
                f64 gf = groom, wf = 0.0;
                if (sa[0] >= minA && sa[1] > 0.0) {
                    const f64 w = sp[0] >= minA * 4.0 ? 1.0 : sp[0] / (minA * 4.0);
                    const f64 gp = sp[1] > 0.0 ? sp[2] / sp[1] : 1.0;
                    const f64 ga = sa[2] / sa[1];
                    gf = w * gp + (1.0 - w) * ga;
                    wf = sa[0] >= minA * 4.0 ? 1.0 : sa[0] / (minA * 4.0);
                }
                const f32 g = (f32)(wf * gf + (1.0 - wf) * groom);
                if (dbgRoom == r && k == 0 && dbgN < 12) {
                    dbgN++;
                    sysLogPrintf(LOG_NOTE, "beanstage: bright dbg room %d tri %d: groom %.2f field %.2f (any %.0f/%.2f, pic %.0f/%.2f) wf %.2f -> %.2f; target %.1f mine %.1f",
                                 r, t, groom, gf, sa[0], sa[1] > 0 ? sa[2] / sa[1] : 0.0, sp[0], sp[1] > 0 ? sp[2] / sp[1] : 0.0,
                                 wf, g, target[t], hdTriBright(tri));
                }
                vgain[k] = g < 1.0f ? 1.0f : g > BRIGHT_MAX_GAIN ? BRIGHT_MAX_GAIN : g;
                any |= vgain[k] > 1.005f;
            }
            if (!any) {
                continue;
            }
            for (s32 k = 0; k < 3; k++) {
                const u32 v = tri->argb[k];
                u32 o = v & 0xff000000u;
                for (s32 sh = 0; sh <= 16; sh += 8) {
                    const s32 ch = (s32)((f32)((v >> sh) & 0xff) * vgain[k] + 0.5f);
                    o |= (u32)(ch > 255 ? 255 : ch) << sh;
                    clamped += ch > 255 && ((v >> sh) & 0xff) < 255;
                }
                tri->argb[k] = o;
            }
            changed++;
        }
        fieldFree(&fm);
    }
    {
        f64 postS = 0, postA = 0, goS = 0, goA = 0;
        memset(hdA, 0, sizeof(f64) * ((size_t)n + 1));
        memset(hdS, 0, sizeof(f64) * ((size_t)n + 1));
        for (s32 t = 0; hdA && hdS && t < c->num; t++) {
            const struct stri *tri = &c->tris[t];
            if (hdTriMeasured(tri, n)) {
                const f32 a = triArea3((const f32 (*)[3])tri->pos);
                hdA[tri->room] += a;
                hdS[tri->room] += (f64)a * hdTriBright(tri);
            }
        }
        char still[512];
        s32 sat = 0, nstill = 0;
        still[0] = 0;
        for (s32 r = 1; hdA && hdS && r < n; r++) {
            if (hdA[r] >= minArea && room[r].area >= minArea && room[r].go > 0.0) {
                const f64 ratio = (hdS[r] / hdA[r]) / (room[r].go / room[r].area);
                postS += hdS[r];
                postA += hdA[r];
                goS += room[r].go;
                goA += room[r].area;
                if (ratio < BRIGHT_REPORT) {
                    nstill++;
                    if (sat < (s32)sizeof(still) - 24) {
                        sat += snprintf(still + sat, sizeof(still) - (size_t)sat, " %d:%.2f", r, ratio);
                    }
                }
            }
        }
        sysLogPrintf(LOG_NOTE, "beanstage: %s: %d triangles brightened to GoldenEye's rooms (%d pairs, %d colour channels "
                     "held at 255), level now %.2f; %d rooms still under %.2f%s", bean, changed, paired, clamped,
                     postA > 0 && goS > 0 ? (postS / postA) / (goS / goA) : 0.0, nstill, BRIGHT_REPORT, still);
    }
    *outChanged = changed;

done:
    free(texMean);
    free(nums);
    free(means);
    free(used);
    free(fileB);
    free(target);
    free(room);
    free(hdA);
    free(hdS);
    *outMs = msSince(t0);
}

static f32 roomPosOf(const struct rawroom *rooms, s32 r, s32 k)
{
    return rooms[r].pos[k];
}

static s32 coldLaunch(const struct stagerow *row, struct beanlevel **level, const struct beanlevelinfo *info,
                      struct rawroom **rooms, s32 n, struct tgrid *filetris, const char *cachePath, const char *path);
static s32 coldAsyncWanted(void);
static void build(void);

/* Midas round 22 (R16-3): the cold worker (struct coldjob, above) */
static s32 coldAsyncWanted(void)
{
    static s32 want = -1;
    if (want < 0) {
        const char *e = getenv("GE_BEANCOLDASYNC");
        want = e && *e ? *e != '0' : getenv("GE_FIXEDTICK") == NULL;
    }
    return want && !coldSyncNext;
}

/* what the worker leaves in the build's globals: none of it is drawn */
static void coldDiscard(void)
{
    for (s32 i = 0; i < numStandins; i++) {
        free(standins[i]);
    }
    free(standins);
    standins = NULL;
    numStandins = 0;
    free(pics);
    pics = NULL;
    numPics = 0;
    numGlassPanes = 0;
    free(backdrop);
    backdrop = NULL;
    numBackdrop = 0;
    free(roomHidden);
    roomHidden = NULL;
    numHidden = 0;
}

static void *coldMain(void *arg)
{
    struct coldjob *j = arg;
    const u64 t0 = sysGetMicroseconds();
    coldCur = j;
    build();
    coldCur = NULL;
    coldDiscard();
    j->ms = msSince(t0);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static s32 coldLaunch(const struct stagerow *row, struct beanlevel **level, const struct beanlevelinfo *info,
                      struct rawroom **rooms, s32 n, struct tgrid *filetris, const char *cachePath, const char *path)
{
    struct coldjob *j = calloc(1, sizeof(*j));
    if (!j) {
        return 0;
    }
    /* matchBrightness()'s texture numbers, gathered as it gathers them, and
     * their means, which only the game thread may decode */
    j->meanNums = malloc(sizeof(s32) * BRIGHT_TEXNUMS);
    u8 *used = calloc(BRIGHT_TEXNUMS, 1);
    if (!j->meanNums || !used) {
        free(j->meanNums);
        free(used);
        free(j);
        return 0;
    }
    for (s32 i = 0; i < numFmeas; i++) {
        if (fmeas[i].tex >= 0 && fmeas[i].tex < BRIGHT_TEXNUMS && !used[fmeas[i].tex]) {
            used[fmeas[i].tex] = 1;
            j->meanNums[j->numMeans++] = fmeas[i].tex;
        }
    }
    free(used);
    j->means = malloc(sizeof(f32) * 3 * (size_t)(j->numMeans > 0 ? j->numMeans : 1));
    if (!j->means) {
        free(j->meanNums);
        free(j);
        return 0;
    }
    texpackTextureMeans(j->meanNums, j->numMeans, j->means);
    j->levelscale = levelinfotable[levelentry_index].levelscale > 0.0f ? levelinfotable[levelentry_index].levelscale : 1.0f;
    j->row = row;
    j->level = *level;
    j->info = *info;
    j->rooms = *rooms;
    j->n = n;
    j->filetris = *filetris;
    j->frame = levelFrames - 1;
    snprintf(j->cachePath, sizeof(j->cachePath), "%s", cachePath);
    snprintf(j->path, sizeof(j->path), "%s", path);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
#ifdef __APPLE__
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
    const int rc = pthread_create(&j->thread, &attr, coldMain, j);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(j->meanNums);
        free(j->means);
        free(j);
        return 0;   /* built on the frame, as before */
    }
    /* the worker's now */
    *level = NULL;
    *rooms = NULL;
    memset(filetris, 0, sizeof(*filetris));
    coldJob = j;
    sysLogPrintf(LOG_NOTE, "beanstage: %s: no built level kept yet; building it off the frame from render frame %d "
                 "(GoldenEye's rooms until then)", row->key, j->frame);
    return 1;
}

static void coldFree(struct coldjob *j)
{
    pthread_join(j->thread, NULL);
    free(j->meanNums);
    free(j->means);
    free(j);
}

/* beanStageLevelLoaded(): the level is going; nothing kept from the job */
static void coldCancel(void)
{
    if (!coldJob) {
        return;
    }
    coldJob->abort = 1;
    const u64 t0 = sysGetMicroseconds();
    coldFree(coldJob);
    coldJob = NULL;
    sysLogPrintf(LOG_NOTE, "beanstage: cold build abandoned with its level (joined in %.1f ms)", msSince(t0));
}

/* beanStageFrame(): 1 once the job is over (the build may run now) */
static s32 coldFinished(void)
{
    if (!__atomic_load_n(&coldJob->done, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    /* Midas round 22b (R22-7): what the log line and the next build need, read
     * before coldFree() frees the job */
    struct coldjob *const j = coldJob;
    const s32 saved = j->saved, frame = j->frame;
    const f64 ms = j->ms, msTris = j->msTris, msPics = j->msPics, msPasses = j->msPasses, msDeal = j->msDeal,
              msBright = j->msBright, msSave = j->msSave;
    coldJob = NULL;
    coldFree(j);
    coldSyncNext = !saved;
    coldSwapIn = saved;
    sysLogPrintf(LOG_NOTE, "beanstage: cold build off the frame done: %.1f ms on the worker (Bean triangles %.1f, pictures %.1f, "
                 "passes %.1f, deal %.1f, brightness %.1f, cache save %.1f), started at render frame %d, swapped in at %d%s",
                 ms, msTris, msPics, msPasses, msDeal, msBright, msSave, frame, levelFrames - 1,
                 saved ? "" : "; nothing kept, building on this frame");
    return 1;
}

static void build(void)
{
    const u64 t0 = sysGetMicroseconds();
    const struct stagerow *row = coldCur ? coldCur->row : levelRow();   /* Midas round 22: the worker reads no game state */
    const s32 n = coldCur ? coldCur->n : g_MaxNumRooms;
    char path[1024];
    struct beanlevel *level;
    struct beanlevelinfo info;
    struct rawroom *rooms = NULL;
    struct collect c;
    struct tgrid filetris, beantris;
    s32 *listlen = NULL;
    s32 **lists = NULL;
    s32 *dealNear = NULL;
    s32 *hdLight = NULL;   /* Midas round 21: darkDeal() */
    s32 darkDealt = 0;
    f64 darkDealMs = 0;
    s32 brightened = 0;
    f64 msBright = 0;
    f64 msOpen, msRooms, msTris, msPics = 0, msPasses = 0, msDeal = 0, msWrite = 0, msCache = 0, msSave = 0;
    s32 cached = 0;
    u64 mark;
    s32 fileTriCount = 0, byBox = 0, dropped = 0, decodedPics = 0;
    s32 untested = 0, twins = 0, welded = 0, clamped = 0, backed = 0, fights = 0, decals = 0, lifted = 0,
        keptDecals = 0, sunk = 0, shadows = 0, cornersMoved = 0, moved = 0, toBackdrop = 0, plainDecals = 0,
        painted = 0;
    u32 decodedBytes = 0;
    const char *onlyEnv = getenv("GE_BEANROOM");
    const s32 only = onlyEnv && *onlyEnv ? atoi(onlyEnv) : 0;
    s32 launched = 0;   /* Midas round 22: the rest went to the cold worker */

    if (coldCur) {
        /* Midas round 22 (R16-3): the worker's run, from the cache miss on */
        level = coldCur->level;
        info = coldCur->info;
        rooms = coldCur->rooms;
        filetris = coldCur->filetris;
        snprintf(path, sizeof(path), "%s", coldCur->path);
        memset(&c, 0, sizeof(c));
        memset(&beantris, 0, sizeof(beantris));
        msOpen = msRooms = msCache = 0;
        goto coldResume;
    }
    memset(&audit, 0, sizeof(audit));
    audit.state = -1;
    if (!row) {
        audit.why = "no XBLA pairing for this level";
        return;
    }
    snprintf(audit.key, sizeof(audit.key), "%s", row->key);
    snprintf(audit.bean, sizeof(audit.bean), "%s", row->bean);
    if (row->verified != 1 && !getenv("GE_BEANSTAGE_ANY")) {
        sysLogPrintf(LOG_NOTE, "beanstage: %s (%s) has no verified pairing yet; not built", row->key, row->bean);
        audit.why = "pairing not verified yet (C5)";
        return;
    }
    if (!levelPath(row->bean, path, sizeof(path))) {
        audit.why = "level file not in the imported layer";
        return; /* no Bean layer: nothing to do, nothing logged twice */
    }
    level = beanLevelOpen(path, row->bean);
    if (!level) {
        sysLogPrintf(LOG_WARNING, "beanstage: %s is not a rendergraph this reads", path);
        audit.why = "level file is not a rendergraph this reads";
        return;
    }
    beanLevelInfo(level, &info);
    audit.why = "build did not finish (allocation failed)";
    {
        struct beandrawcount dc;
        beanMeshCountDraws(level, 0, &dc);
        audit.draws = dc.draws;
        audit.pictureDraws = dc.kinds[BEANDRAW_PICTURE];
        audit.colourDraws = dc.kinds[BEANDRAW_COLOUR];
        audit.badpicDraws = dc.kinds[BEANDRAW_BADPIC];
        audit.unreadableDraws = dc.unreadable;
        if (dc.kinds[BEANDRAW_BADPIC] || dc.unreadable) {
            sysLogPrintf(LOG_WARNING, "[xbla] unresolved: level %s: %d draws name a picture the file does not hold, "
                         "%d draws have a vertex buffer this reader cannot read", row->bean, dc.kinds[BEANDRAW_BADPIC],
                         dc.unreadable);
        }
    }
    msOpen = msSince(t0);

    mark = sysGetMicroseconds();
    decompScratch = malloc(DECOMP_SCRATCH);
    rooms = calloc((size_t)n + 1, sizeof(*rooms));
    memset(&c, 0, sizeof(c));
    memset(&filetris, 0, sizeof(filetris));
    memset(&beantris, 0, sizeof(beantris));
    if (!decompScratch || !rooms || !tgridInit(&filetris, ASSIGN_CELL)) {
        goto out;
    }
    /* Midas round 13: GoldenEye's rooms are read again into host scratch here;
     * that is not a console load, so it must not be priced into Midas's
     * tickhz=hw load hitch (frametiming.c), as texpack.c's walks are not. */
    portHwLoadSuppress(1);
    readRooms(rooms, n);
    portHwLoadSuppress(0);
    measForget();
    darkForgetBuild();   /* Midas round 21 */
    for (s32 r = 1; r < n; r++) {
        rooms[r].ntri = fileRoomTriangles(&filetris, &rooms[r], r);
        fileTriCount += rooms[r].ntri;
        if (getenv("GE_BEANROOMS") && rooms[r].hasbox) {
            sysLogPrintf(LOG_NOTE, "beanstage: n64 room %d world box %.0f %.0f %.0f .. %.0f %.0f %.0f", r,
                         rooms[r].box[0] * room_data_float2, rooms[r].box[1] * room_data_float2,
                         rooms[r].box[2] * room_data_float2, rooms[r].box[3] * room_data_float2,
                         rooms[r].box[4] * room_data_float2, rooms[r].box[5] * room_data_float2);
        }
    }
    msRooms = msSince(mark);

    /* The built level kept on disk (round C2, PD hdcacheKey/Save/Load): the
     * triangles after every pass and the dealing, keyed on the Bean file, the
     * N64 rooms' raw bytes and the builder's version */
    mark = sysGetMicroseconds();
    s32 cacheable = cacheAllowed();
    char cachePath[1024];
    struct cachedpic *cpics = NULL;
    cachePath[0] = 0;
    if (cacheable) {
        cacheName(row->bean, level, rooms, n, cachePath, sizeof(cachePath));
        cached = cacheLoad(cachePath, &c, &cpics, info.numtextures);
    }
    msCache = msSince(mark);
    if (!cached && coldSwapIn) {
        /* Midas round 22b (R22-8): the file the worker just kept did not load
         * (written over since, or a load this build refuses). Another worker
         * would keep the same file and miss again, once a second, for as long
         * as the level runs: this one build stays on the frame. */
        coldSyncNext = 1;
        sysLogPrintf(LOG_WARNING, "beanstage: %s: the built level the worker kept did not load; building it on this frame",
                     row->key);
    }
    /* Midas round 22 (R16-3): a miss goes to the worker; this frame draws
     * GoldenEye's rooms and a later one builds from the file it keeps */
    if (!cached && cacheable && cachePath[0] && coldAsyncWanted()
            && coldLaunch(row, &level, &info, &rooms, n, &filetris, cachePath, path)) {
        launched = 1;
        audit.why = "first visit: being built off the frame";
        goto out;
    }
coldResume:
    if (coldCur) {
        cacheable = 1;
        cached = 0;
        cpics = NULL;
        snprintf(cachePath, sizeof(cachePath), "%s", coldCur->cachePath);
    }

    mark = sysGetMicroseconds();
    c.scale = row->scale;
    for (s32 k = 0; k < 3; k++) {
        c.keep[k] = 1e30f;
        c.keep[3 + k] = -1e30f;
    }
    for (s32 r = 1; r < n; r++) {
        for (s32 k = 0; rooms[r].hasbox && k < 3; k++) {
            if (rooms[r].box[k] < c.keep[k]) c.keep[k] = rooms[r].box[k];
            if (rooms[r].box[3 + k] > c.keep[3 + k]) c.keep[3 + k] = rooms[r].box[3 + k];
        }
    }
    for (s32 k = 0; k < 3; k++) {
        c.keep[k] -= KEEP_MARGIN;
        c.keep[3 + k] += KEEP_MARGIN;
    }
    if (!cached) {
        beanLevelTriangles(level, collectTri, &c);
    }
    msTris = msSince(mark);

    sysLogPrintf(LOG_NOTE, "beanstage: %s <- %s: %d draws, %d pictures, %d instancing matrices, %d index buffers "
                 "(%d 32-bit), UVs in 1/%.0f (literals 1/128 x%d, 1/256 x%d), %u bytes; %d Bean triangles "
                 "(%d left out beyond the level box + %.0f), %d N64 triangles in %d rooms", row->key, path,
                 info.numdraws, info.numtextures, info.numinsts, info.numibs, info.numibs32, info.uvscale,
                 info.lit128, info.lit256, info.filebytes, c.num, c.outside, KEEP_MARGIN, fileTriCount, n - 1);

    if (!cached && getenv("GE_BEANPROBE")) {
        probe(rooms, n, &c, row->scale);
    }

    /* The pictures first: the passes ask which have alpha */
    mark = sysGetMicroseconds();
    numPics = info.numtextures;
    numGlassPanes = 0;
    pics = calloc((size_t)(numPics > 0 ? numPics : 1), sizeof(*pics));
    if (!pics) {
        goto out;
    }
    decodePictures(level, &c, &decodedPics, &decodedBytes);
    for (s32 t = 0; !coldCur && t < numPics; t++) {   /* Midas round 22: the audit is the sim thread's */
        audit.pictures += pics[t].used != 0;
        audit.decoded += pics[t].used && !pics[t].failed;
    }
    for (s32 t = 0; cached && cpics && t < numPics; t++) {
        pics[t].clampv = cpics[t].clampv;
        pics[t].clampshift = cpics[t].clampshift;
    }
    free(cpics);
    msPics = msSince(mark);

    if (getenv("GE_BEANPICKAT")) {
        /* dev (round 29c): the pictures of the triangles within r of x,y,z
         * (world units): "x,y,z,r" */
        f32 q[4] = { 0, 0, 0, 0 };
        if (sscanf(getenv("GE_BEANPICKAT"), "%f,%f,%f,%f", &q[0], &q[1], &q[2], &q[3]) == 4) {
            s32 *cnt = calloc((size_t)numPics + 1, sizeof(s32));
            s32 *pl = calloc((size_t)numPics + 1, sizeof(s32));
            for (s32 i = 0; cnt && pl && i < c.num; i++) {
                const struct stri *tri = &c.tris[i];
                f32 d2 = 0.0f;
                for (s32 k = 0; k < 3; k++) {
                    const f32 m = (tri->pos[0][k] + tri->pos[1][k] + tri->pos[2][k]) / 3.0f * room_data_float2 - q[k];
                    d2 += m * m;
                }
                if (d2 <= q[3] * q[3]) {
                    const s32 t = tri->tex >= 0 && tri->tex < numPics ? tri->tex : numPics;
                    cnt[t]++;
                    pl[t] += tri->plain;
                }
            }
            for (s32 t = 0; cnt && pl && t <= numPics; t++) {
                if (cnt[t]) {
                    int32_t w = 0, h = 0;
                    if (t < numPics) beanLevelTextureSize(level, t, &w, &h);
                    sysLogPrintf(LOG_NOTE, "beanstage: pick: picture %d %dx%d mean %.0f %.0f %.0f: %d triangles (%d plain)",
                                 t < numPics ? t : -1, (int)w, (int)h, t < numPics ? pics[t].mean[0] : -1.0f,
                                 t < numPics ? pics[t].mean[1] : -1.0f, t < numPics ? pics[t].mean[2] : -1.0f, cnt[t], pl[t]);
                }
            }
            free(cnt);
            free(pl);
        }
    }
    if (!cached && getenv("GE_BEANTEXLOG")) {
        /* dev: each picture's use - triangles, blended, faded, water, vertex alpha */
        for (s32 t = -1; t < numPics; t++) {
            s32 cnt = 0, bl = 0, fa = 0, wa = 0;
            u32 amin = 255, amax = 0;
            int32_t w = 0, h = 0;
            for (s32 i = 0; i < c.num; i++) {
                const struct stri *tri = &c.tris[i];
                if (tri->tex != t) continue;
                cnt++;
                bl += tri->blend;
                fa += triFades(tri);
                wa += tri->water;
                for (s32 k = 0; k < 3; k++) {
                    amin = MINV(amin, tri->argb[k] >> 24);
                    amax = MAXV(amax, tri->argb[k] >> 24);
                }
            }
            if (!cnt) continue;
            if (t >= 0) beanLevelTextureSize(level, t, &w, &h);
            sysLogPrintf(LOG_NOTE, "beanstage: tex %d %dx%d: %d tris, %d blended, %d fade, %d water, vtx alpha %u..%u, alpha %d soft %d scroll %.3f %.3f",
                         t, w, h, cnt, bl, fa, wa, amin, amax, t >= 0 ? pics[t].alpha : 0, t >= 0 ? pics[t].soft : 0,
                         t >= 0 ? pics[t].scroll[0] : 0.0f, t >= 0 ? pics[t].scroll[1] : 0.0f);
        }
    }
    if (!cached && getenv("GE_BEANHIDETEX")) {
        /* dev: leave pictures out ("12,40") */
        s32 kept = 0;
        for (s32 i = 0; i < c.num; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%d", c.tris[i].tex);
            const char *env = getenv("GE_BEANHIDETEX");
            s32 hide = 0;
            for (const char *p = env; (p = strstr(p, key)) != NULL; p++) {
                const size_t n = strlen(key);
                if ((p == env || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) { hide = 1; break; }
            }
            if (!hide) c.tris[kept++] = c.tris[i];
        }
        c.num = kept;
    }

    listlen = calloc((size_t)n + 1, sizeof(*listlen));
    lists = calloc((size_t)n + 1, sizeof(*lists));
    if (!listlen || !lists) {
        goto out;
    }
    if (COLD_ABORTED()) {
        goto out;   /* Midas round 22: the level is going */
    }
    if (!cached) {
    /* PD's passes, in PD build()'s order */
    mark = sysGetMicroseconds();
    if (!skipPass("alphatest")) untested = alphaTested(&c);
    if (!skipPass("twins")) twins = dropTwins(&c);
    if (!skipPass("weld")) welded = weldVertices(&c);
    if (!skipPass("backdrop")) takeBackdrop(&c, rooms, n);
    if (!skipPass("cutclamp")) clamped = clampCutouts(&c);
    if (!tgridInit(&beantris, BEAN_CELL)) {
        goto out;
    }
    for (s32 t = 0; t < c.num; t++) {
        tgridAdd(&beantris, (const f32 (*)[3])c.tris[t].pos, t);
    }
    triCacheFill(c.tris, c.num);
    if (!skipPass("backed")) backed = markBacked(c.tris, c.num, &beantris);
    if (!skipPass("fights")) fights = markFights(c.tris, c.num, &beantris);
    if (!skipPass("decals")) decals = markDecals(c.tris, c.num, &beantris);
    triCacheForget();
    if (!skipPass("lift")) lifted = markDecalLifts(c.tris, c.num, &keptDecals, &sunk);
    if (!skipPass("shadows")) shadows = markShadows(c.tris, c.num);
    if (getenv("GE_BEANTRILOG")) {
        /* dev (round 54): every triangle of one picture after the passes */
        const s32 want = atoi(getenv("GE_BEANTRILOG"));
        for (s32 t = 0; t < c.num; t++) {
            const struct stri *tri = &c.tris[t];
            if (tri->tex != want) continue;
            sysLogPrintf(LOG_NOTE, "beanstage: tri %d tex %d y %.1f %.1f %.1f x %.0f z %.0f a %02x %02x %02x rgb %06x blend %d at %d/%d decal %d base %d fights %d backed %d fade %d uv %.2f,%.2f %.2f,%.2f %.2f,%.2f",
                         t, tri->tex, tri->pos[0][1] * room_data_float2, tri->pos[1][1] * room_data_float2, tri->pos[2][1] * room_data_float2,
                         tri->pos[0][0] * room_data_float2, tri->pos[0][2] * room_data_float2,
                         tri->argb[0] >> 24, tri->argb[1] >> 24, tri->argb[2] >> 24, tri->argb[0] & 0xffffff, tri->blend, tri->alphatest, tri->alpharef,
                         tri->decal, tri->decalbase, tri->fights, tri->backed, triFades(tri),
                         tri->uv[0][0], tri->uv[0][1], tri->uv[1][0], tri->uv[1][1], tri->uv[2][0], tri->uv[2][1]);
            if (tri->decalbase >= 0) {
                const struct stri *u = &c.tris[tri->decalbase];
                f32 nu[3];
                triNormal(u, nu);
                sysLogPrintf(LOG_NOTE, "beanstage:   base %d tex %d n %.2f %.2f %.2f culled %d blend %d a %02x rgb %06x p0 %.0f %.0f %.0f p1 %.0f %.0f %.0f p2 %.0f %.0f %.0f",
                             tri->decalbase, u->tex, nu[0], nu[1], nu[2], triCulled(u), u->blend, u->argb[0] >> 24, u->argb[0] & 0xffffff,
                             u->pos[0][0] * room_data_float2, u->pos[0][1] * room_data_float2, u->pos[0][2] * room_data_float2,
                             u->pos[1][0] * room_data_float2, u->pos[1][1] * room_data_float2, u->pos[1][2] * room_data_float2,
                             u->pos[2][0] * room_data_float2, u->pos[2][1] * room_data_float2, u->pos[2][2] * room_data_float2);
                sysLogPrintf(LOG_NOTE, "beanstage:   self p0 %.0f %.0f %.0f p1 %.0f %.0f %.0f p2 %.0f %.0f %.0f",
                             tri->pos[0][0] * room_data_float2, tri->pos[0][1] * room_data_float2, tri->pos[0][2] * room_data_float2,
                             tri->pos[1][0] * room_data_float2, tri->pos[1][1] * room_data_float2, tri->pos[1][2] * room_data_float2,
                             tri->pos[2][0] * room_data_float2, tri->pos[2][1] * room_data_float2, tri->pos[2][2] * room_data_float2);
            }
        }
    }
    msPasses = msSince(mark);
    if (COLD_ABORTED()) {
        goto out;   /* Midas round 22 */
    }

    /* Deal each triangle to the room whose own triangle its middle lies on
     * (PD build(): the nearest VERTEX named the room next door) */
    mark = sysGetMicroseconds();
    dealNear = malloc(sizeof(s32) * (size_t)(c.num > 0 ? c.num : 1));
    for (s32 t = 0; t < c.num; t++) {
        struct stri *tri = &c.tris[t];
        f32 mid[3], d;
        if (dealNear) dealNear[t] = -1;
        /* a white decal of a draw with no UV or picture: shaded by the
         * release's shader alone, which the port has nothing to draw with */
        if (tri->plain && tri->decal && tri->argb[0] == 0xffffffff && tri->argb[1] == 0xffffffff
                && tri->argb[2] == 0xffffffff) {
            tri->room = 0;
            plainDecals++;
            continue;
        }
        if (tri->painted) {
            tri->room = 0;
            painted++;
            continue;
        }
        for (s32 j = 0; j < 3; j++) {
            mid[j] = (tri->pos[0][j] + tri->pos[1][j] + tri->pos[2][j]) / 3.0f;
        }
        const s32 near = tgridNearest(&filetris, mid, ASSIGN_RINGS, &d);
        if (dealNear) dealNear[t] = near;
        if (near >= 0) {
            tri->room = (s16)filetris.room[near];
        } else {
            tri->room = (s16)nearestRoomBox(rooms, n, mid);
            byBox++;
        }
        if (tri->room > 0) {
            listlen[tri->room]++;
        }
    }
    /* A triangle too far from its room's position for a Vtx to hold goes to
     * the nearest room it fits that has triangles of its own; out of every
     * room's reach, to the backdrop (PD build()) */
    for (s32 t = 0; t < c.num; t++) {
        struct stri *tri = &c.tris[t];
        s32 best = -1;
        f32 bestd = 0.0f;
        if (tri->room <= 0 || triFitsAt(tri, rooms[tri->room].pos)) {
            continue;
        }
        if (!skipPass("move")) {
            for (s32 r = 1; r < n; r++) {
                if (r != tri->room && listlen[r] > 0 && triFitsAt(tri, rooms[r].pos)) {
                    const f32 dx = tri->pos[0][0] - rooms[r].pos[0];
                    const f32 dy = tri->pos[0][1] - rooms[r].pos[1];
                    const f32 dz = tri->pos[0][2] - rooms[r].pos[2];
                    const f32 dd = dx * dx + dy * dy + dz * dz;
                    if (best < 0 || dd < bestd) {
                        best = r;
                        bestd = dd;
                    }
                }
            }
        }
        if (best > 0) {
            listlen[tri->room]--;
            listlen[best]++;
            tri->room = (s16)best;
            moved++;
        } else if (!skipPass("backdrop")) {
            struct stri *n2 = realloc(backdrop, sizeof(*backdrop) * (size_t)(numBackdrop + 1));
            if (n2) {
                backdrop = n2;
                backdrop[numBackdrop++] = *tri;
                listlen[tri->room]--;
                tri->room = 0;
                toBackdrop++;
            }
        }
    }
    if (!skipPass("corners")) {
        cornersMoved = markDecalCorners(c.tris, c.num, rooms);
    }
    msDeal = msSince(mark);
    /* round 29c (D-223): the release's light matched to GoldenEye's rooms */
    if (dealNear && !skipPass("bright")) {
        matchBrightness(&c, dealNear, n, row->key, row->bean, &brightened, &msBright);
    }
    /* the N64 rooms nothing of Bean's was dealt to, and whose triangles lie
     * nowhere near Bean's (fewer than a quarter within a cell): hidden */
    roomHidden = calloc((size_t)n + 1, 1);
    if (roomHidden && !skipPass("hidden")) {
        s32 *total = calloc((size_t)n + 1, sizeof(s32)), *nearCnt = calloc((size_t)n + 1, sizeof(s32));
        for (s32 i = 0; total && nearCnt && i < filetris.numtri; i++) {
            const s32 r = filetris.room[i];
            f32 mid[3], d;
            if (r <= 0 || r >= n || listlen[r] > 0) continue;
            for (s32 j = 0; j < 3; j++) {
                mid[j] = (filetris.tri[i * 9 + j] + filetris.tri[i * 9 + 3 + j] + filetris.tri[i * 9 + 6 + j]) / 3.0f;
            }
            total[r]++;
            if (tgridNearest(&beantris, mid, 1, &d) >= 0 && d < BEAN_CELL * BEAN_CELL) {
                nearCnt[r]++;
            }
        }
        for (s32 r = 1; total && nearCnt && r < n; r++) {
            /* a triangle too big for the grid (GoldenEye's far cliffs are a
             * few huge ones) counts as not near */
            total[r] = listlen[r] > 0 ? 0 : rooms[r].ntri;
            if (total[r] > 0 && nearCnt[r] * 4 < total[r]) {
                roomHidden[r] = 1;
            }
            if (total[r] > 0 && getenv("GE_BEANROOMS")) {
                sysLogPrintf(LOG_NOTE, "beanstage: unserved room %d: %d N64 triangles, %d near Bean's%s", r, total[r],
                             nearCnt[r], roomHidden[r] ? " -> hidden" : "");
            }
        }
        free(total);
        free(nearCnt);
    }
    } /* !cached */
    numHidden = 0;
    for (s32 r = 1; roomHidden && r < n; r++) {
        numHidden += roomHidden[r];
    }
    memset(listlen, 0, sizeof(*listlen) * ((size_t)n + 1));
    for (s32 t = 0; t < c.num; t++) {
        if (c.tris[t].room > 0 && c.tris[t].room < n) {
            listlen[c.tris[t].room]++;
        }
    }
    if (COLD_ABORTED()) {
        goto out;   /* Midas round 22: nothing kept */
    }
    s32 savedOk = 0;
    if (!cached && cacheable && cachePath[0]) {
        const u64 s0 = sysGetMicroseconds();
        savedOk = cacheSave(cachePath, &c, n + 1);
        msSave = msSince(s0);
    }
    if (coldCur) {
        /* Midas round 22: the worker stops here; the sim thread builds from the
         * file. Round 22b (R22-8): only a file this job wrote counts; a stale
         * one of the same name (a save that failed over it) is not kept. */
        coldCur->saved = savedOk;
        coldCur->msTris = msTris;
        coldCur->msPics = msPics;
        coldCur->msPasses = msPasses;
        coldCur->msDeal = msDeal;
        coldCur->msBright = msBright;
        coldCur->msSave = msSave;
        goto out;
    }
    for (s32 r = 1; r < n; r++) {
        lists[r] = listlen[r] ? malloc(sizeof(s32) * (size_t)listlen[r]) : NULL;
        listlen[r] = 0;
    }
    for (s32 t = 0; t < c.num; t++) {
        const s32 r = c.tris[t].room;
        if (r > 0 && lists[r]) {
            lists[r][listlen[r]++] = t;
        }
    }

    /* Midas round 21: the HD triangles of the light fixtures */
    {
        const u64 d0 = sysGetMicroseconds();
        hdLight = darkDeal(&c, &filetris, &darkDealt);
        darkDealMs = msSince(d0);
    }
    darkHdLight = hdLight;
    darkSrc = c.tris;

    /* Every room Bean's mesh reaches, written in GoldenEye's room form */
    mark = sysGetMicroseconds();
    served = calloc((size_t)n + 1, sizeof(*served));
    numRooms = n;
    if (!served) {
        goto out;
    }
    {
        const u16 *untextured = newStandin(0xffff);
        s32 *opalist = NULL, *xlulist = NULL;
        u32 roomBytes = 0, meshBytes = 0;
        s32 tris = 0;
        for (s32 r = 1; r < n; r++) {
            s32 numopa = 0, numxlu = 0;
            if (listlen[r] == 0 || (only > 0 && r != only)) {
                continue;
            }
            {
                /* round 25b (D-142): a room holding a triangle whose picture
                 * is unresolved (named but not in the file, or undecodable)
                 * is left GoldenEye's whole - never a white face */
                s32 unres = 0, shader = 0;
                for (s32 i = 0; i < listlen[r]; i++) {
                    const struct stri *tri = &c.tris[lists[r][i]];
                    unres += tri->unres || (tri->tex >= 0 && tri->tex < numPics && pics[tri->tex].failed);
                    shader += tri->shader;
                }
                audit.shaderTris += shader;
                if (shader && getenv("GE_BEANSHADERLOG")) {
                    /* round 30a dev (D-248): where a room's shader-only triangles are */
                    f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                    s32 blend = 0, decal = 0, at = 0;
                    for (s32 i = 0; i < listlen[r]; i++) {
                        const struct stri *tri = &c.tris[lists[r][i]];
                        if (!tri->shader) {
                            continue;
                        }
                        blend += tri->blend != 0;
                        decal += tri->decalbase >= 0;
                        at += tri->alphatest != 0;
                        for (s32 k = 0; k < 3; k++) {
                            for (s32 a = 0; a < 3; a++) {
                                lo[a] = __builtin_fminf(lo[a], tri->pos[k][a]);
                                hi[a] = __builtin_fmaxf(hi[a], tri->pos[k][a]);
                            }
                        }
                    }
                    sysLogPrintf(LOG_NOTE, "beanstage: room %d: %d shader-only triangles (%d blended, %d decal, %d alpha-tested), "
                                 "box %.0f %.0f %.0f .. %.0f %.0f %.0f (level units)", r, shader, blend, decal, at, lo[0], lo[1],
                                 lo[2], hi[0], hi[1], hi[2]);
                }
                if (unres) {
                    audit.unresolvedRooms++;
                    audit.unresolvedTris += unres;
                    sysLogPrintf(LOG_WARNING, "[xbla] unresolved: level %s room %d: %d of %d triangles with an unresolved "
                                 "picture - room drawn N64", row->bean, r, unres, listlen[r]);
                    continue;
                }
            }
            opalist = realloc(opalist, sizeof(s32) * (size_t)listlen[r]);
            xlulist = realloc(xlulist, sizeof(s32) * (size_t)listlen[r]);
            if (!opalist || !xlulist) {
                break;
            }
            for (s32 i = 0; i < listlen[r]; i++) {
                struct stri *tri = &c.tris[lists[r][i]];
                if (tri->tex >= 0 && (tri->tex >= numPics || !pics[tri->tex].used)) {
                    tri->tex = -1;
                }
                if (texIsXlu(tri->tex) || triFades(tri)) {
                    xlulist[numxlu++] = lists[r][i];
                } else {
                    opalist[numopa++] = lists[r][i];
                }
            }
            struct gdlbuf opa, xlu;
            struct vtxbuf vb;
            memset(&opa, 0, sizeof(opa));
            memset(&xlu, 0, sizeof(xlu));
            memset(&vb, 0, sizeof(vb));
            darkOwner = r;   /* Midas round 21 */
            writeLeaf(&opa, &vb, c.tris, opalist, numopa, 0, rooms[r].pos, untextured, &dropped);
            writeLeaf(&xlu, &vb, c.tris, xlulist, numxlu, 1, rooms[r].pos, untextured, &dropped);
            darkOwner = -1;
            /* GoldenEye's render-mode/combiner look-up, as bgLoadRoomModelData()
             * applies it to the room's own lists: fog or not */
            if (opa.num) {
                bgApplyDynamicCCRMLUT(opa.g, opa.g + opa.num, g_FogSkyIsEnabled ? 1 : 6);
                for (s32 i = 0; i < opa.numzupd; i++) {
                    opa.g[opa.zupd[i]].words.w1 |= Z_UPD;
                }
            }
            if (xlu.num) {
                bgApplyDynamicCCRMLUT(xlu.g, xlu.g + xlu.num, g_FogSkyIsEnabled ? 5 : 7);
            }
            free(opa.zupd);
            free(xlu.zupd);
            served[r].vtx = vb.v;
            meshBytes += writeRoomMesh(&served[r], &vb, &opa, &xlu, 0); /* queued after the stage form, below */
            free(opa.runs);
            free(xlu.runs);
            free(vb.idx);
            served[r].numvtx = vb.num;
            served[r].opa = opa.num ? opa.g : NULL;
            served[r].numopa = opa.num;
            served[r].xlu = xlu.num ? xlu.g : NULL;
            served[r].numxlu = xlu.num;
            if (!opa.num) free(opa.g);
            if (!xlu.num) free(xlu.g);
            for (s32 k = 0; k < 3; k++) {
                served[r].pos[k] = roomPosOf(rooms, r, k);
                served[r].box[k] = 1e30f;
                served[r].box[3 + k] = -1e30f;
            }
            for (s32 i = 0; i < listlen[r]; i++) {
                const struct stri *tri = &c.tris[lists[r][i]];
                for (s32 v = 0; v < 3; v++) {
                    for (s32 k = 0; k < 3; k++) {
                        served[r].box[k] = MINV(served[r].box[k], tri->pos[v][k]);
                        served[r].box[3 + k] = MAXV(served[r].box[3 + k], tri->pos[v][k]);
                    }
                }
            }
            served[r].tris = listlen[r];
            if (getenv("GE_BEANROOMS")) {
                /* dev: each served room's world box (Bond's units) and size */
                sysLogPrintf(LOG_NOTE, "beanstage: room %d: %d tris, world box %.0f %.0f %.0f .. %.0f %.0f %.0f", r,
                             listlen[r], served[r].box[0] * room_data_float2, served[r].box[1] * room_data_float2,
                             served[r].box[2] * room_data_float2, served[r].box[3] * room_data_float2,
                             served[r].box[4] * room_data_float2, served[r].box[5] * room_data_float2);
            }
            tris += listlen[r];
            roomBytes += (u32)vb.num * (u32)sizeof(Vtx) + (u32)(opa.num + xlu.num) * (u32)sizeof(Gfx);
            numServed++;
        }
        if (getenv("GE_BEANDRAWSTAT")) {
            s32 nonint = 0, xtris = 0;
            f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            for (s32 r = 1; r < n; r++) {
                if (!served[r].vtx) continue;
                for (s32 k = 0; k < 3; k++) {
                    nonint += served[r].pos[k] != floorf(served[r].pos[k]);
                    lo[k] = MINV(lo[k], served[r].box[k]);
                    hi[k] = MAXV(hi[k], served[r].box[3 + k]);
                }
            }
            for (s32 t = 0; t < c.num; t++) {
                const struct stri *tri = &c.tris[t];
                if (tri->room > 0 && tri->room < n && served[tri->room].vtx && (texIsXlu(tri->tex) || triFades(tri))) {
                    xtris++;
                }
            }
            sysLogPrintf(LOG_NOTE, "beanstage: drawstat build: %d non-integral room positions; stage box %.0f %.0f %.0f .. %.0f %.0f %.0f; "
                         "%d xlu triangles", nonint, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], xtris);
        }
        free(opalist);
        free(xlulist);
        /* round 26c (D-160): the stage form, and which form's GPU mesh the
         * render thread uploads now (the other uploads at its first draw) */
        stage = buildStage(&c, rooms, n, lists, listlen, untextured);
        roomInView = calloc((size_t)n + 1, 1);
        if (stage && cfgEveryRoom && cfgMerge && cfgMeshPath) {
            gfxHdMeshQueueUpload(stage->sr.mesh);
        } else {
            for (s32 r = 1; r < n; r++) {
                if (served[r].mesh) {
                    gfxHdMeshQueueUpload(served[r].mesh);
                }
            }
        }
        roomMtx = calloc((size_t)MTX_RING * (size_t)(n + 2), sizeof(Mtx));
        frameGfxCap = (n + 1) * 3 * 2 + 16 + numBackdrop * 14 + 32 + 4 /* Midas round 21: G_VTXPATCH_EXT */
            + (stage ? stage->pass[0].num + stage->pass[1].num + stage->spanCap + 8 : 0);
        frameGfx = calloc((size_t)MTX_RING * (size_t)frameGfxCap, sizeof(Gfx));
        drawOrder = realloc(drawOrder, sizeof(s32) * (size_t)(n + 1));
        drawDist = realloc(drawDist, sizeof(f32) * (size_t)(n + 1));
        if (numBackdrop) {
            backdropVtx = calloc((size_t)BACKDROP_RING * (size_t)numBackdrop * 3, sizeof(Vtx));
            backdropOrder = realloc(backdropOrder, sizeof(s32) * (size_t)numBackdrop);
            backdropDist = realloc(backdropDist, sizeof(f32) * (size_t)numBackdrop);
            if (!backdropVtx || !backdropOrder || !backdropDist) {
                numBackdrop = 0;
            }
        }
        darkFinishBuild(darkDealt, darkDealMs);   /* Midas round 21 */
        msWrite = msSince(mark);
        hostBytes = roomBytes + meshBytes + (stage ? stage->bytes : 0) + (u32)numStandins * TILE * TILE * 2u
            + (u32)((size_t)MTX_RING * (size_t)(n + 2) * sizeof(Mtx)) + (u32)numBackdrop * (u32)(sizeof(struct stri) + BACKDROP_RING * 3 * sizeof(Vtx));
        pictureBytes = decodedBytes;

        sysLogPrintf(LOG_NOTE,
"beanstage: %d of %d rooms served (%d left N64, %d of them hidden while every room is drawn), %d Bean triangles in them, %d off a room's s16/UV "
            "range; %d dealt to another room in reach, %d to the backdrop (%d backdrop triangles in all); "
            "%d pictures decoded (%.1f MB RGBA), %d scrolling; host bytes %u rooms + %u pictures",
            numServed, n - 1, n - 1 - numServed, numHidden, tris, dropped, moved, toBackdrop, numBackdrop, decodedPics,
            decodedBytes / 1048576.0, numScrollSlots, hostBytes, decodedBytes);
        audit.state = 1;
        audit.rooms = n - 1;
        audit.served = numServed;
        audit.shaderDecals = plainDecals;
        {
            f32 zr[2];
            viGetZRange(zr);
            sysLogPrintf(LOG_NOTE, "beanstage: z range %.0f..%.0f (matrix units; visibility scale %.3f, room scale %.5f: "
                         "far = %.0f world = %.0f bg units)", zr[0], zr[1], bgGetLevelVisibilityScale(), room_data_float2,
                         zr[1] / bgGetLevelVisibilityScale(), zr[1] / bgGetLevelVisibilityScale() / room_data_float2);
        }
        {
            s32 meshRooms = 0, meshDraws = 0;
            u32 meshVtx = 0, meshIdx = 0;
            for (s32 r = 1; r < n; r++) {
                if (served[r].mesh) {
                    meshRooms++;
                    meshDraws += served[r].numdraws;
                    meshVtx += served[r].mesh->numvtx;
                    meshIdx += served[r].mesh->numidx;
                }
            }
            sysLogPrintf(LOG_NOTE, "beanstage: Route B meshes: %d rooms, %d G_HDMESH_EXT draws, %u vertices, %u indices, "
                         "%u host bytes (lists, indices, fallbacks); Video.XblaMeshPath=%d", meshRooms, meshDraws, meshVtx,
                         meshIdx, meshBytes, cfgMeshPath);
        }
        sysLogPrintf(LOG_NOTE,
            "beanstage: passes: %d alpha-tested out, %d twins out, %d corners welded, %d cut-outs clamped, "
            "%d two-faced, %d back to back in part, %d decals (%d lifted, %d kept in their mesh, %d bases sunk, "
            "%d corners moved), %d shadows, %d plain white decals out, %d painted over out, %d glass panes given their alpha",
            untested, twins, welded, clamped, backed, fights, decals, lifted, keptDecals, sunk, cornersMoved,
            shadows, plainDecals, painted, numGlassPanes);
        sysLogPrintf(LOG_NOTE,
            "beanstage: build %.1f ms %s at render frame %d of the level (open %.1f, N64 rooms %.1f, cache key+load %.1f, Bean triangles %.1f, pictures %.1f, "
            "passes %.1f, deal %.1f [%d by box], brightness %.1f [%d triangles], write %.1f, cache save %.1f)",
            msSince(t0), cached ? "FROM CACHE" : "cold", levelFrames - 1, msOpen, msRooms, msCache, msTris, msPics, msPasses, msDeal, byBox,
            msBright, brightened, msWrite, msSave);
    }

out:
    if (rooms) {
        for (s32 r = 0; r <= n; r++) {
            free(rooms[r].vtx);
            free(rooms[r].gdl[0]);
            free(rooms[r].gdl[1]);
        }
    }
    if (lists) {
        for (s32 r = 0; r <= n; r++) {
            free(lists[r]);
        }
    }
    free(lists);
    free(listlen);
    free(dealNear);
    free(hdLight);   /* Midas round 21 */
    darkHdLight = NULL;
    darkSrc = NULL;
    darkOwner = -1;
    if (!launched) {
        measForget();   /* Midas round 22: a launched job's worker still reads them */
    }
    free(rooms);
    free(c.tris);
    tgridFree(&filetris);
    tgridFree(&beantris);
    free(decompScratch);
    decompScratch = NULL;
    beanLevelClose(level);
}

/* ------------------------------------------------------- every room */

/*
 * Draw every served room by distance (round C2, Video.XblaEveryRoom, D-112):
 * render only. GoldenEye's portal walk still decides which N64 rooms load and
 * which props draw (bgDetermineVisibleRooms, g_RoomLoadBudget, mema - none of
 * it is read or written here); a served room is drawn from its host lists
 * whether or not its N64 room is loaded, with the full view's scissor, its
 * own matrix (a host ring, not GoldenEye's roomMatrices pool), nearest first
 * for the opaque leaves and farthest first for the translucent ones.
 */
static s32 nodrawEnv(void)
{
    static int nodraw = -1;
    if (nodraw < 0) nodraw = getenv("GE_BEANNODRAW") != NULL;
    return nodraw;
}

static f32 eyeWorld[3];
static s32 eyeSet;
static f32 viewProj[4][4];
static s32 viewProjSet;
static s32 frustumCulled;

void beanStageSetViewProj(f32 m[4][4])
{
    memcpy(viewProj, m, sizeof(viewProj));
    viewProjSet = 1;
}

/* Whether a served room's box lies wholly outside one plane of the view
 * frustum (all eight corners past it), in the rooms' own projection. */
static s32 roomOutsideView(s32 r)
{
    const f32 vis = bgGetLevelVisibilityScale();
    u32 outAll = 0x3f;
    for (s32 c = 0; c < 8; c++) {
        f32 p[3], clip[4];
        u32 out = 0;
        for (s32 k = 0; k < 3; k++) {
            /* grown by a lifted decal's reach */
            const f32 b = (c >> k) & 1 ? served[r].box[3 + k] + 2.0f * DECAL_LIFT : served[r].box[k] - 2.0f * DECAL_LIFT;
            p[k] = (b * room_data_float2 - g_CurrentPlayer->current_model_pos.f[k]) * vis;
        }
        for (s32 j = 0; j < 4; j++) {
            clip[j] = p[0] * viewProj[0][j] + p[1] * viewProj[1][j] + p[2] * viewProj[2][j] + viewProj[3][j];
        }
        out |= clip[0] < -clip[3] ? 1 : 0;
        out |= clip[0] > clip[3] ? 2 : 0;
        out |= clip[1] < -clip[3] ? 4 : 0;
        out |= clip[1] > clip[3] ? 8 : 0;
        out |= clip[2] < -clip[3] ? 16 : 0;
        out |= clip[2] > clip[3] ? 32 : 0;
        outAll &= out;
        if (!outAll) {
            return 0;
        }
    }
    return outAll != 0;
}

void beanStageSetEye(f32 x, f32 y, f32 z)
{
    eyeWorld[0] = x;
    eyeWorld[1] = y;
    eyeWorld[2] = z;
    eyeSet = 1;
}

/* Midas round 61 (doc 81, beansky.c): the eye the backdrop is placed round, in
 * world units (the player's model position until bondview sets it), and the
 * level's GoldenEye key ("dam", "sevx", "run"; NULL for an unpaired level). */
int beanStageEyeWorld(f32 out[3])
{
    if (!g_CurrentPlayer) {
        return 0;
    }
    for (s32 k = 0; k < 3; k++) {
        out[k] = eyeSet ? eyeWorld[k] : g_CurrentPlayer->current_model_pos.f[k];
    }
    return 1;
}

const char *beanStageLevelKey(void)
{
    const struct stagerow *row = levelRow();
    return row ? row->key : NULL;
}

int beanStageDrawsEveryRoom(void)
{
    return cfgModels && cfgEveryRoom && served && numServed > 0 && !nodrawEnv();
}

static int compareNear(const void *a, const void *b)
{
    const f32 da = drawDist[*(const s32 *)a], db = drawDist[*(const s32 *)b];
    return da < db ? -1 : da > db ? 1 : *(const s32 *)a - *(const s32 *)b;
}

static int compareFar(const void *a, const void *b)
{
    return -compareNear(a, b);
}

/* The room's matrix as setupRoomTransformationMatrix() builds it: scaled by
 * the level's room scale, placed relative to the player's model position. */
static Mtx *placeMatrix(s32 slot, const f32 *pos);

static Mtx *roomMatrix(s32 r)
{
    return placeMatrix(r, served[r].pos);
}

/* the stage form's matrix: the room matrix for a room at the anchor (slot
 * numRooms + 1; slot 0 is the backdrop's) */
static Mtx *stageMatrix(void)
{
    return placeMatrix(numRooms + 1, stage->anchor);
}

static Mtx *placeMatrix(s32 r, const f32 *pos)
{
    /* host floats (G_MTX_FLOATS_EXT): Dam's room placements reach past
     * S15.16's +-32768 world units */
    f32 (*mf)[4] = (f32 (*)[4])(roomMtx + (size_t)(mtxFrame % MTX_RING) * (size_t)(numRooms + 2) + r);
    /* matrix_4x4_f32_to_s32() scales every matrix it converts by the
     * level's visibility scale (matrix_4x4_7F058C4C(), Dam 0.2) on all but
     * the w column; a float matrix has to carry it itself */
    const f32 vis = bgGetLevelVisibilityScale();
    memset(mf, 0, sizeof(f32) * 16);
    mf[0][0] = mf[1][1] = mf[2][2] = room_data_float2 * vis;
    mf[3][3] = 1.0f;
    for (s32 k = 0; k < 3; k++) {
        mf[3][k] = (pos[k] * room_data_float2 - g_CurrentPlayer->current_model_pos.f[k]) * vis;
    }
    return (Mtx *)mf;
}

/* `need` commands (plus the G_ENDDL) in this frame's host slot, or NULL */
static Gfx *frameBegin(s32 need)
{
    if (!frameGfx || frameGfxUsed + need + 1 > frameGfxCap) {
        frameGfxOverflow++;
        return NULL;
    }
    return frameGfx + (size_t)(mtxFrame % MTX_RING) * (size_t)frameGfxCap + frameGfxUsed;
}

/* closes the list begun at g0 and links it from the guest list */
static Gfx *frameEnd(Gfx *gdl, Gfx *g0, Gfx *g)
{
    gSPEndDisplayList(g++);
    frameGfxUsed += (s32)(g - g0);
    gSPDisplayList(gdl++, (uintptr_t)g0);
    return gdl;
}

/* the list a room draws: Route B's when Video.XblaMeshPath is on and built */
static Gfx *roomList(const struct servedroom *sr, int xlu)
{
    if (cfgMeshPath) {
        Gfx *m = xlu ? sr->xlu1 : sr->opa1;
        if (m) {
            return m;
        }
    }
    return xlu ? sr->xlu : sr->opa;
}

/* dev: GE_BEANSTAGEGAP=<indices> - rooms out of view between two in view of
 * one picture are drawn with them (one draw) up to this many indices */
static u32 stageGap(void)
{
    static long gap = -1;
    if (gap < 0) {
        const char *e = getenv("GE_BEANSTAGEGAP");
        gap = e && *e ? strtol(e, NULL, 10) : 3072;
    }
    return (u32)gap;
}

/* A stage list for this frame: its words as built, each G_HDMESH_EXT replaced
 * by the draws of its rooms in view (neighbouring ranges merged). The first
 * draw of a picture carries the whole picture's fallback, the rest an empty
 * one, so a fallback frame draws each triangle once. */
static Gfx *stageEmit(Gfx *g, const struct stagepass *p)
{
    struct GfxHdMeshDraw *ring = stage->spans + (size_t)(mtxFrame % MTX_RING) * (size_t)(stage->spanCap + 1);
    const u32 gap = stageGap();
    s32 gi = 0;
    for (s32 i = 0; i < p->num; i++) {
        const Gfx *w = &p->list[i];
        if ((u32)(w->words.w0 >> 24) != G_HDMESH_EXT) {
            *g++ = *w;
            continue;
        }
        const struct GfxHdMeshDraw *full = p->full[gi];
        const s32 q0 = p->seg0[gi], q1 = p->seg0[gi + 1];
        gi++;
        if (spanUsed + (q1 - q0) > stage->spanCap) {
            *g++ = *w; /* no room left in this frame's ring: the whole picture */
            stageDraws++;
            continue;
        }
        s32 nsp = 0, open = 0;
        u32 a = 0, e = 0, hole = 0;
        for (s32 q = q0; q <= q1; q++) {
            const s32 last = q == q1;
            if (!last && !roomInView[p->segs[q].room]) {
                hole += open ? p->segs[q].count : 0;
                continue;
            }
            if (!last && open && hole <= gap) {
                e = p->segs[q].first + p->segs[q].count;
                hole = 0;
                continue;
            }
            if (open) {
                struct GfxHdMeshDraw *d = &ring[spanUsed++];
                d->mesh = full->mesh;
                d->first = a;
                d->count = e - a;
                d->fallback = nsp == 0 ? full->fallback : (const void *)emptyList;
                g->words.w0 = (uintptr_t)G_HDMESH_EXT << 24;
                g->words.w1 = (uintptr_t)d;
                g++;
                nsp++;
            }
            if (!last) {
                open = 1;
                a = p->segs[q].first;
                e = a + p->segs[q].count;
                hole = 0;
            }
        }
        stageDraws += nsp;
        stageSpans += q1 - q0;
    }
    return g;
}

/* dev, round 26c: GE_BEANDRAWSTAT=1 - every 100 frames, for the rooms this
 * pass draws: G_HDMESH_EXT commands, and how many distinct draw states
 * (picture, combiner, render mode, cull, depth bias) they span - what a
 * merge across rooms by state could reach. */
static void drawStat(const s32 *order, s32 num, int xlu)
{
    static int on = -1;
    static s32 frames[2];
    if (on < 0) on = getenv("GE_BEANDRAWSTAT") != NULL;
    if (!on || (frames[xlu]++ % 100) != 0) {
        return;
    }
    u64 keys[4096], tiles[1024];
    s32 nkeys = 0, cmds = 0, ntiles = 0;
    for (s32 i = 0; i < num; i++) {
        const Gfx *g = roomList(&served[order[i]], xlu);
        u64 tile = 0, cc = 0, rm = 0, cull = 0, bias = 0;
        for (; g && (u32)(g->words.w0 >> 24) != 0xb8u; g++) {
            const u32 op = (u32)(g->words.w0 >> 24);
            if (op == 0xfd) tile = (u64)g->words.w1;
            else if (op == 0xfc) cc = ((u64)(u32)g->words.w0 << 32) | (u32)g->words.w1;
            else if (op == 0xb9) rm = (u64)(u32)g->words.w1;
            else if (op == 0xb6) cull &= ~(u64)(u32)g->words.w1;
            else if (op == 0xb7) cull |= (u64)(u32)g->words.w1;
            else if (op == G_SETDEPTHBIAS_EXT) bias = (u64)(u32)g->words.w1;
            else if (op == G_HDMESH_EXT) {
                const u64 k = tile * 1000003u ^ cc * 7919u ^ rm * 31u ^ cull * 131u ^ bias;
                s32 j = 0;
                cmds++;
                while (j < nkeys && keys[j] != k) j++;
                if (j == nkeys && nkeys < 4096) keys[nkeys++] = k;
                for (j = 0; j < ntiles && tiles[j] != tile; j++) {}
                if (j == ntiles && ntiles < 1024) tiles[ntiles++] = tile;
            }
        }
    }
    sysLogPrintf(LOG_NOTE, "beanstage: drawstat %s frame %d: %d rooms, %d G_HDMESH_EXT, %d distinct states, %d distinct pictures",
                 xlu ? "xlu" : "opa", frames[xlu] - 1, num, cmds, nkeys, ntiles);
}

Gfx *beanStageRenderEveryRoom(Gfx *gdl, int xlu)
{
    s32 num = 0;
    f32 cam[3];
    if (!beanStageDrawsEveryRoom() || !roomMtx || !drawOrder || !g_CurrentPlayer) {
        return gdl;
    }
    /* round 26c (D-160): the stage form, unless the per-room pass is asked for */
    const s32 merged = stage && roomInView && cfgMeshPath && cfgMerge && !(xlu && cfgXluRooms);
    for (s32 k = 0; k < 3; k++) {
        cam[k] = (eyeSet ? eyeWorld[k] : g_CurrentPlayer->current_model_pos.f[k]) / room_data_float2;
    }
    f32 zr[2];
    viGetZRange(zr);
    /* a room wholly past the far plane draws nothing: left out (the plane is
     * GoldenEye's own, Dam 15000 x Video.DrawDistance; not raised - the fog
     * and the guards' sight read the same figure) */
    const f32 far = zr[1] / bgGetLevelVisibilityScale() / room_data_float2;
    for (s32 r = 1; r < numRooms; r++) {
        if (!served[r].vtx || !(xlu ? served[r].xlu : served[r].opa)) {
            continue;
        }
        static s32 noxlu = -1;   /* Midas round 28 (doc 38 item 7): once, not per room per frame */
        if (noxlu < 0) {
            noxlu = getenv("GE_BEANNOXLU") != NULL;
        }
        if (xlu && noxlu) {
            continue; /* dev */
        }
        drawDist[r] = boxDist(cam, served[r].box);
        if (cfgFarCull && drawDist[r] > far * far) {
            continue;
        }
        if (cfgFarCull && viewProjSet && drawDist[r] > 0.0f && roomOutsideView(r)) {
            frustumCulled++;
            continue;   /* wholly outside the view (the eye is not in its box) */
        }
        drawOrder[num++] = r;
    }
    if (merged) {
        const struct stagepass *p = &stage->pass[xlu];
        if (!p->list) {
            return gdl;
        }
        memset(roomInView, 0, (size_t)numRooms + 1);
        for (s32 i = 0; i < num; i++) {
            roomInView[drawOrder[i]] = 1;
        }
        Gfx *g0 = frameBegin(p->num + p->numsegs + 4), *g = g0;
        if (!g0) {
            return gdl;
        }
        g = darkPatchEmit(g);   /* Midas round 21 */
        const s32 d0 = stageDraws;
        gSPMatrix(g++, stageMatrix(), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH | G_MTX_FLOATS_EXT);
        gSPSegment(g++, SPSEGMENT_BG_VTX, (uintptr_t)stage->sr.vtx);
        g = stageEmit(g, p);
        everyRoomDrawn += num;
        static s32 drawstat = -1;   /* Midas round 28: once, not per frame */
        if (drawstat < 0) {
            drawstat = getenv("GE_BEANDRAWSTAT") != NULL;
        }
        if (drawstat && (mtxFrame % 100) == 0) {
            sysLogPrintf(LOG_NOTE, "beanstage: drawstat stage %s frame %d: %d rooms in view, %d draws (%d pictures)", xlu ? "xlu" : "opa",
                         mtxFrame, num, stageDraws - d0, p->numgroups);
        }
        return frameEnd(gdl, g0, g);
    }
    qsort(drawOrder, (size_t)num, sizeof(s32), xlu ? compareFar : compareNear);
    drawStat(drawOrder, num, xlu);
    Gfx *g0 = frameBegin(num * 3 + 1), *g = g0;
    if (!g0) {
        return gdl;
    }
    g = darkPatchEmit(g);   /* Midas round 21 */
    for (s32 i = 0; i < num; i++) {
        const s32 r = drawOrder[i];
        gSPMatrix(g++, roomMatrix(r), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH | G_MTX_FLOATS_EXT);
        gSPSegment(g++, SPSEGMENT_BG_VTX, (uintptr_t)served[r].vtx);
        gSPDisplayList(g++, (uintptr_t)roomList(&served[r], xlu));
    }
    everyRoomDrawn += num;
    return frameEnd(gdl, g0, g);
}

/* ------------------------------------------------------------- public */

/*
 * Midas round 22 (R16-2): an XBLA import (or Remove) while a level is in play
 * reaches that level. xblaimport.c bumps the generation from its worker or the
 * UI thread; the sim thread notices it here, at the top of its display-list
 * build, and runs exactly what bg.c load_bg_file() runs for a new level
 * (prefetch drain, the three forgets, prefetch again), so this frame's
 * beanStageFrame()/beanChrFrame()/beanModelFrame() build the level again from
 * the layer now on disk - or find it gone and leave GoldenEye's own. Render
 * only: nothing the sim reads is touched. The forgets keep one set retired
 * (the last frame presented may still read it), so a second rebuild waits
 * until MTX_RING frames of the first have gone by.
 */
static volatile u32 layerGen;
static u32 layerSeen;

void beanStageLayerChanged(void)
{
    __atomic_add_fetch(&layerGen, 1, __ATOMIC_RELEASE);
}

static void layerCheck(void)
{
    const u32 g = __atomic_load_n(&layerGen, __ATOMIC_ACQUIRE);
    if (g == layerSeen) {
        return;
    }
    if (!tried) {
        layerSeen = g;   /* nothing built yet: the build reads the new layer anyway */
        return;
    }
    if (levelFrames < MTX_RING) {
        return;          /* the set retired by the last load/rebuild may still be presented */
    }
    if (xblaImportState() == XBLA_IMPORT_RUNNING) {
        /* Midas round 22b (R22-9): never rebuild from a layer an import is
         * still writing; its end (done or failed) bumps the generation again */
        static u32 noted;
        if (noted != g) {
            noted = g;
            sysLogPrintf(LOG_NOTE, "beanstage: the XBLA layer changed mid-level (render frame %d); "
                         "the rebuild waits for the import to finish", levelFrames);
        }
        return;
    }
    layerSeen = g;
    const int levelid = beanPrefetchCurrentLevel();
    sysLogPrintf(LOG_NOTE, "beanstage: the XBLA layer changed mid-level (render frame %d): rebuilding level %d",
                 levelFrames, levelid);
    beanPrefetchDrain();
    beanStageLevelLoaded();
    beanModelLevelLoaded();
    beanChrLevelLoaded();
    if (levelid >= 0) {
        beanPrefetchLevel(levelid);
    }
}

void beanStageLevelLoaded(void)
{
    coldCancel();   /* Midas round 22: before anything the worker uses is freed */
    coldSyncNext = 0;
    coldSwapIn = 0;
    retireServed();
    beanSkyLevelReset();   /* Midas round 61: the last level's dome goes with its pictures */
    tried = 0;
    levelFrames = 0;
    memset(&audit, 0, sizeof(audit));
}

int beanStageAudit(struct beanstageaudit *out)
{
    if (out) {
        *out = audit;
    }
    return tried && !coldJob;   /* Midas round 22: the stage line waits for the cold build */
}

/* Settings -> Textures status text and the bridge's `xbla` lines (round 22
 * merge, D-136). Read from the UI thread without a lock: plain ints and a
 * pointer test, so the worst a race does is show last frame's label. */
void beanStageDescribe(char *buf, int len)
{
    if (!buf || len <= 0) {
        return;
    }
    if (!cfgModels) {
        snprintf(buf, (size_t)len, "off (N64 rooms)");
    } else if (coldJob) {
        snprintf(buf, (size_t)len, "building this level's XBLA rooms (first visit)");   /* Midas round 22 */
    } else if (served && numServed > 0) {
        snprintf(buf, (size_t)len, "%d rooms from XBLA, %s", (int)numServed,
                 cfgEveryRoom ? "every room" : "portals");
    } else if (tried) {
        snprintf(buf, (size_t)len, "this level: N64 rooms");
    } else {
        snprintf(buf, (size_t)len, "on (paired levels, when one loads)");
    }
}

void beanStageFrame(void)
{
    {
        /* dev: GE_BEANLOG=1 logs Bond's rooms and position every 100 frames */
        static int logOn = -1;
        static int frames;
        if (logOn < 0) logOn = getenv("GE_BEANLOG") != NULL;
        if (logOn && (frames++ % 100) == 0 && g_CurrentPlayer && g_CurrentPlayer->prop) {
            sysLogPrintf(LOG_NOTE, "beanstage: frame %d curRoom %d prop room %d pos %.0f %.0f %.0f; every-room drew %d, %d out of view",
                         frames - 1, g_CurrentPlayer->curRoomIndex, g_CurrentPlayer->prop->rooms[0],
                         g_CurrentPlayer->prop->pos.x, g_CurrentPlayer->prop->pos.y, g_CurrentPlayer->prop->pos.z,
                         everyRoomDrawn, frustumCulled);
        }
    }
    {
        /* dev (Midas round 22): GE_BEANREBUILDAT=<n> acts as if the layer
         * changed at the first level's render frame n (the R16-2 rebuild on
         * an unchanged layer: its pixels must equal a run without it) */
        static int at = -2;
        if (at == -2) at = getenv("GE_BEANREBUILDAT") ? atoi(getenv("GE_BEANREBUILDAT")) : -1;
        if (at >= 0 && levelFrames == at) {
            at = -1;
            beanStageLayerChanged();
        }
    }
    layerCheck();   /* Midas round 22 (R16-2) */
    mtxFrame++;
    spanUsed = 0;
    stageDraws = stageSpans = 0;
    everyRoomDrawn = 0;
    frustumCulled = 0;
    frameGfxUsed = 0;
    darkFrameOpen = 1;   /* Midas round 21 */
    if (served) {
        /* the slides at the level's clock (g_GlobalTimer, read only) */
        for (s32 i = 0; i < numScrollSlots; i++) {
            scrollSlotWrite(i, (f32)g_GlobalTimer / 60.0f);
        }
    }
    levelFrames++;
    if (coldJob) {
        /* Midas round 22 (R16-3): GoldenEye's rooms until the worker is done */
        if (!coldFinished()) {
            return;
        }
        tried = 0;   /* build now, from the kept file */
    }
    if (tried || !cfgModels) {
        return;
    }
    /*
     * Round 29c (D-226): the level's first render frame, before anything of
     * it is presented - its intro included. Until now the build waited for
     * Bond's prop to have a room (C1 served Bond's room; D-141), which on
     * every level comes several hundred frames into the intro: the intro
     * drew N64 rooms, then the rooms popped to HD, and a cold build (Frigate
     * 1.2 s) stalled a frame of the intro. The build needs nothing of Bond.
     * GE_BEANBUILDATPLACE=1 (dev) keeps the old trigger (the A/B; the replay
     * references' pre-D-226 digests).
     */
    {
        static int atPlace = -1;
        if (atPlace < 0) atPlace = getenv("GE_BEANBUILDATPLACE") != NULL;
        if (atPlace && bondRoom() <= 0) {
            return; /* Bond is not placed yet */
        }
    }
    tried = 1;
    build();
    coldSyncNext = 0;   /* Midas round 22: a fallback build on the frame is a one-off */
    coldSwapIn = 0;
    /* Midas round 13 (R13-2): the slides above ran before the build, when
     * nothing was served, so this frame's room lists would G_DL into the
     * calloc'd (all-zero) slots: no G_ENDDL, and the walker runs into the
     * next heap block. Write them now, at the same clock. */
    if (served) {
        for (s32 i = 0; i < numScrollSlots; i++) {
            scrollSlotWrite(i, (f32)g_GlobalTimer / 60.0f);
        }
    }
}

int beanStageRoomHidden(s32 room)
{
    static int all = -1;
    if (all < 0) all = getenv("GE_BEANHIDEN64") != NULL; /* dev: hide every unserved room */
    if (all && beanStageDrawsEveryRoom() && room > 0 && room < numRooms && !served[room].vtx) return 1;
    return beanStageDrawsEveryRoom() && roomHidden && room > 0 && room < numRooms && roomHidden[room];
}

int beanStageRoomServed(s32 room)
{
    if (nodrawEnv()) return 0;
    return cfgModels && served && room > 0 && room < numRooms && served[room].vtx != NULL;
}

Gfx *beanStageRenderRoom(Gfx *gdl, s32 room, int xlu)
{
    const struct servedroom *s = &served[room];
    Gfx *list = roomList(s, xlu);
    /* drawn by beanStageRenderEveryRoom() instead: the portal pass still
     * loads the N64 room and moves its state, and draws nothing of it */
    if (!list || beanStageDrawsEveryRoom()) {
        return gdl;
    }
    gdl = darkPatchEmit(gdl);   /* Midas round 21 */
    gSPSegment(gdl++, SPSEGMENT_BG_VTX, (uintptr_t)s->vtx);
    gSPDisplayList(gdl++, (uintptr_t)list);
    return gdl;
}

void beanStageMemory(u32 *rooms, u32 *pictures)
{
    *rooms = served ? hostBytes : 0;
    *pictures = served ? pictureBytes : 0;
}

/* ------------------------------------------------------- the backdrop */

/*
 * The level's backdrop (takeBackdrop(), and triangles no room's position can
 * reach), drawn after the sky and before the rooms as PD's
 * gebeanStageRenderBackdrop() draws it: where it stands relative to the eye,
 * but scaled towards the eye so its farthest vertex is inside the far plane
 * (scaling about the eye moves nothing on the screen), with no depth test or
 * write - the rooms drawn after it cover it - its own triangles farthest
 * first, faded into the sky by their vertex alpha. Written into a ring of
 * per-frame vertex buffers (the frame being presented may still read the
 * last one).
 */
static int compareBackdropFar(const void *a, const void *b)
{
    const f32 da = backdropDist[*(const s32 *)a], db = backdropDist[*(const s32 *)b];
    return da > db ? -1 : da < db ? 1 : *(const s32 *)a - *(const s32 *)b;
}

/* Round 28a (C5, D-167): the backdrop is drawn unfogged in front of
 * GoldenEye's own sky, so it must agree with that sky. Left out where it
 * does not (both seen on the round's sheet, artifacts/oracle/round28-c5):
 *  - Surface 2: the same level file as Surface at night; the release's
 *    backdrop is a daylit photograph of the peaks, which GoldenEye's (and the
 *    release's) dark red fog hides there (PD "Surface's two skies");
 *  - both Bunkers: their 3400 backdrop triangles are a cloudy DAYTIME sky
 *    dome round the level, drawn over GoldenEye's night sky.
 * Dam, Surface, Runway and the Cradle keep theirs (peaks and the canyon). */
static s32 backdropWanted(void)
{
    const s32 id = levelinfotable[levelentry_index].levelID;
    return id != LEVELID_SURFACE2 && id != LEVELID_BUNKER1 && id != LEVELID_BUNKER2;
}

int beanStageHasBackdrop(void)
{
    return cfgModels && served && numBackdrop > 0 && backdropVtx && !nodrawEnv() && backdropWanted();
}

Gfx *beanStageRenderBackdrop(Gfx *guest)
{
    Gfx *gdl, *g0;
    f32 cam[3], far2 = 0.0f;
    s32 curtex = -2;
    if (!cfgModels || !served || numBackdrop == 0 || !backdropVtx || nodrawEnv() || !g_CurrentPlayer
            || !backdropWanted()) {
        return guest;
    }
    for (s32 k = 0; k < 3; k++) {
        cam[k] = (eyeSet ? eyeWorld[k] : g_CurrentPlayer->current_model_pos.f[k]) / room_data_float2;
    }
    for (s32 t = 0; t < numBackdrop; t++) {
        f32 mid[3] = { 0, 0, 0 };
        for (s32 j = 0; j < 3; j++) {
            f32 d2 = 0.0f;
            for (s32 k = 0; k < 3; k++) {
                const f32 x = backdrop[t].pos[j][k] - cam[k];
                d2 += x * x;
                mid[k] += x;
            }
            far2 = MAXV(far2, d2);
        }
        backdropOrder[t] = t;
        backdropDist[t] = dot3(mid, mid);
    }
    if (far2 <= 1.0f) {
        return guest;
    }
    qsort(backdropOrder, (size_t)numBackdrop, sizeof(s32), compareBackdropFar);

    const f32 k = BACKDROP_EXTENT / sqrtf(far2);
    Vtx *vtx = backdropVtx + (size_t)(mtxFrame % BACKDROP_RING) * (size_t)numBackdrop * 3;
    for (s32 t = 0; t < numBackdrop; t++) {
        const struct stri *tri = &backdrop[backdropOrder[t]];
        const f32 su = floorf(tri->uv[0][0]), sv = floorf(tri->uv[0][1]);
        for (s32 j = 0; j < 3; j++) {
            Vtx *v = &vtx[t * 3 + j];
            const u32 argb = tri->argb[j];
            v->v.ob[0] = (s16)((tri->pos[j][0] - cam[0]) * k);
            v->v.ob[1] = (s16)((tri->pos[j][1] - cam[1]) * k);
            v->v.ob[2] = (s16)((tri->pos[j][2] - cam[2]) * k);
            v->v.flag = 0;
            v->v.tc[0] = (s16)MAXV(-32768.0f, MINV(32767.0f, (tri->uv[j][0] - su) * TILE_SCALE));
            v->v.tc[1] = (s16)MAXV(-32768.0f, MINV(32767.0f, (tri->uv[j][1] - sv) * TILE_SCALE));
            v->v.cn[0] = (u8)(argb >> 16);
            v->v.cn[1] = (u8)(argb >> 8);
            v->v.cn[2] = (u8)argb;
            v->v.cn[3] = (u8)(argb >> 24);
        }
    }

    g0 = gdl = frameBegin(numBackdrop * 14 + 33);
    if (!g0) {
        return guest;
    }
    gdl = darkPatchEmit(gdl);   /* Midas round 21 */
    /* the eye's matrix (rotation and projection, as the rooms use) and a
     * model scale that puts the farthest vertex at BACKDROP_REACH world units */
    {
        Mtx *m = roomMtx + (size_t)(mtxFrame % MTX_RING) * (size_t)(numRooms + 2); /* slot 0: no room uses it */
        f32 mf[4][4];
        memset(mf, 0, sizeof(mf));
        mf[0][0] = mf[1][1] = mf[2][2] = BACKDROP_REACH / BACKDROP_EXTENT;
        mf[3][3] = 1.0f;
        /* about the eye: the projection's origin is the room anchor */
        for (s32 k = 0; k < 3; k++) {
            mf[3][k] = cam[k] * room_data_float2 - g_CurrentPlayer->current_model_pos.f[k];
        }
        matrix_4x4_f32_to_s32(mf, (s32 (*)[4])m);
        gSPMatrix(gdl++, m, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    }
    gDPPipeSync(gdl++);
    gDPSetCycleType(gdl++, G_CYC_1CYCLE);
    gDPSetTexturePersp(gdl++, G_TP_PERSP);
    gDPSetTextureLOD(gdl++, G_TL_TILE);
    gDPSetTextureFilter(gdl++, G_TF_BILERP);
    gDPSetAlphaCompare(gdl++, G_AC_NONE);
    gSPClearGeometryMode(gdl++, G_ZBUFFER | G_FOG | G_CULL_BOTH);
    gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);
    gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(gdl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
    gSPSegment(gdl++, SPSEGMENT_BG_VTX, (uintptr_t)vtx);
    for (s32 first = 0; first < numBackdrop * 3; first += BACKDROP_BATCH) {
        const s32 count = MINV(numBackdrop * 3 - first, BACKDROP_BATCH);
        s32 trisInBatch = 0;
        u8 tri[5][3];
        /* a load is five whole triangles; a picture change flushes them */
        for (s32 t = 0; t < count; t += 3) {
            const s32 tex = backdrop[backdropOrder[(first + t) / 3]].tex;
            if (tex != curtex) {
                if (trisInBatch) {
                    for (s32 i = 0; i < trisInBatch; i += 4) {
                        u32 w0 = 0xb1u << 24, w1 = 0;
                        for (s32 q = 0; q < 4 && i + q < trisInBatch; q++) {
                            w1 |= (u32)tri[i + q][0] << (q * 8) | (u32)tri[i + q][1] << (q * 8 + 4);
                            w0 |= (u32)tri[i + q][2] << (q * 4);
                        }
                        gdl->words.w0 = w0;
                        gdl->words.w1 = w1;
                        gdl++;
                    }
                    trisInBatch = 0;
                }
                curtex = tex;
                gDPPipeSync(gdl++);
                {
                    const u16 *tile = tex >= 0 && tex < numPics && pics[tex].used ? standins[pics[tex].standin] : standins[numStandins - 1];
                    gDPSetTextureImage(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, tile);
                    gDPSetTile(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, G_TX_LOADTILE, 0, G_TX_WRAP, 5, G_TX_NOLOD, G_TX_WRAP, 5, G_TX_NOLOD);
                    gDPLoadSync(gdl++);
                    gDPLoadBlock(gdl++, G_TX_LOADTILE, 0, 0, TILE * TILE - 1, CALC_DXT(TILE, G_IM_SIZ_16b_BYTES));
                    gDPPipeSync(gdl++);
                    gDPSetTile(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 0, G_TX_RENDERTILE, 0, G_TX_WRAP, 5, G_TX_NOLOD, G_TX_WRAP, 5, G_TX_NOLOD);
                    gDPSetTileSize(gdl++, G_TX_RENDERTILE, 0, 0, (TILE - 1) << G_TEXTURE_IMAGE_FRAC, (TILE - 1) << G_TEXTURE_IMAGE_FRAC);
                }
            }
            if (trisInBatch == 0 || t == 0) {
                /* (re)load this batch's vertices; triangles index into it */
                gdl->words.w0 = (0x04u << 24) | ((u32)(count - 1) << 20) | (u32)(count * (s32)sizeof(Vtx));
                gdl->words.w1 = ((u32)SPSEGMENT_BG_VTX << 24) | (u32)((size_t)first * sizeof(Vtx));
                gdl++;
            }
            tri[trisInBatch][0] = (u8)t;
            tri[trisInBatch][1] = (u8)(t + 1);
            tri[trisInBatch][2] = (u8)(t + 2);
            trisInBatch++;
        }
        for (s32 i = 0; i < trisInBatch; i += 4) {
            u32 w0 = 0xb1u << 24, w1 = 0;
            for (s32 q = 0; q < 4 && i + q < trisInBatch; q++) {
                w1 |= (u32)tri[i + q][0] << (q * 8) | (u32)tri[i + q][1] << (q * 8 + 4);
                w0 |= (u32)tri[i + q][2] << (q * 4);
            }
            gdl->words.w0 = w0;
            gdl->words.w1 = w1;
            gdl++;
        }
    }
    /* what a GoldenEye room expects back: depth on, back faces culled */
    gDPPipeSync(gdl++);
    gSPSetGeometryMode(gdl++, G_ZBUFFER | G_CULL_BACK);
    return frameEnd(guest, g0, gdl);
}
