/* Imported from GoldenEye-Decomp-ios engine/port/src/beanmodel.c (first f0454b7c, last changed 95ec1134; donor HEAD 55a3a9c8),
 * round 14, XBLA step 4.2 (docs/port-ios/23-round14-props-guns.md). 64-bit host layer only.
 * Changes against the donor are marked "Midas round 14". */
/*
 * GoldenEye XBLA's HD props on GoldenEye's own prop models - see
 * port/include/beanmodel.h and docs/tier-c.md "What C3 built". Tier C round
 * C3 (DECISIONS D-117..D-119).
 *
 * Shape from Perfect Dark's gebean.c gebeanBuildRigid() (perfect-dark-ios
 * vendor/dabs-mod, MIT): the fitted rigid placement (geproptable.h's
 * columns; ours from scripts/beanfit/propfit.py on our own model space), the
 * opaque-black-vertex rule, glass from the material colour's alpha in the
 * blended pass. Where PD binds Bean's bones to parts (the truck's wheels),
 * this deals each Bean triangle to the display-list node whose own vertices
 * are nearest - the same answer for a rigid model, and every moving part
 * rides its own node's matrix without a bone table. The display list written
 * is the room form of beanstage.c (16-vertex G_VTX, G_TRI4, S10.5 UVs on a
 * 32x32 stand-in tile the texture hook swaps for the decoded picture).
 *
 * MEMORY RULE (charter rule 3c, D-110): everything here is host malloc.
 */

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <ultra64.h>
#include <PR/gbi.h>
#include "platform.h"
#include "system.h"
#include "texpack.h"
#include "beanprefetch.h"
#include "beanmesh.h"
#include "beanstage.h"
#include "beanmodel.h"
#include "beangun.h"
#include "beanchr.h"
#include "bondconstants.h"
#include "bondtypes.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

/* The N64 libc headers on the include path shadow the host's and lack these. */
float floorf(float x);
float roundf(float x);
float sqrtf(float x);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

extern ItemModelFileRecord PitemZ_entries[];
extern PropRecord *g_ActivePropsHead;
s32 modelFindNodeMtxIndex(ModelNode *node, s32 arg1);

#define NUM_PITEMZ 340
#define BATCHVERTS 16
#define TILE 32
#define TILE_SCALE (TILE * 32.0f)
#define OPAQUE_ALPHA 0xf0
#define SOFT_PERCENT 1
#define MAXNODES 64

struct proprow {
    const char *file;
    const char *source;
    s8 perm[3];
    s8 sign[3];
    f32 scale[3];   /* round 35b (D-321): per axis; PROPROW's are all one */
    f32 centre[3];
    u32 flags;      /* round 35b: PROPROW_* */
};

#define PROPROW(file, source, p0, p1, p2, s0, s1, s2, scale, cx, cy, cz) \
    { file, source, { p0, p1, p2 }, { s0, s1, s2 }, { scale, scale, scale }, { cx, cy, cz }, 0 }

/* Round 35b (D-321): a held gun whose release copy is GoldenEye's remodelled
 * (the auto shotgun's squashed cross-section, the rocket launcher's longer
 * tube) - propfit.py's HELD_OVERRIDE row: a scale per axis, and flags. */
#define PROPROW_OVERRIDE 1u
#define PROPROW_FLASH_AHEAD 2u   /* the flash card is a solid star ahead of the muzzle (D-322) */
#define PROPROWX(file, source, p0, p1, p2, s0, s1, s2, sx, sy, sz, cx, cy, cz, fl) \
    { file, source, { p0, p1, p2 }, { s0, s1, s2 }, { sx, sy, sz }, { cx, cy, cz }, PROPROW_OVERRIDE | (fl) }

/* Round 26a (D-144): the first-person guns' rows after the props', the same
 * columns (scripts/beanfit/gunfit.py); a row whose source is gun/... is a gun. */
#define GUNROW PROPROW

static const struct proprow propRows[] = {
#include "beanproptable.h"
#include "beanguntable.h"
};
#define NUM_PROPROWS ((s32)(sizeof(propRows) / sizeof(propRows[0])))

/* ------------------------------------------------------------ buffers */

struct gdlbuf {
    Gfx *g;
    s32 num, cap;
};

static void emit(struct gdlbuf *b, uintptr_t w0, uintptr_t w1)
{
    if (b->num >= b->cap) {
        const s32 cap = b->cap ? b->cap * 2 : 256;
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

struct vtxbuf {
    Vtx *v;
    s32 num, cap;
};

static void vtxAdd(struct vtxbuf *vb, const Vtx *v)
{
    if (vb->num >= vb->cap) {
        const s32 cap = vb->cap ? vb->cap * 2 : 256;
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
};

/* GoldenEye's G_VTX into segment SPSEGMENT_MODEL_VTX, then G_TRI4s
 * (beanstage.c batchFlush(), the model's segment). */
static void batchFlush(struct gdlbuf *l, struct vtxbuf *vb, struct batch *b)
{
    if (b->numv == 0) {
        return;
    }
    const u32 offset = (u32)vb->num * (u32)sizeof(Vtx);
    for (s32 i = 0; i < b->numv; i++) {
        vtxAdd(vb, &b->v[i]);
    }
    emit(l, (0x04u << 24) | ((u32)(b->numv - 1) << 20) | (u32)(b->numv * (s32)sizeof(Vtx)),
         ((u32)SPSEGMENT_MODEL_VTX << 24) | offset);
    for (s32 i = 0; i < b->numtris; i += 4) {
        u32 w0 = 0xb1u << 24;
        u32 w1 = 0;
        for (s32 k = 0; k < 4 && i + k < b->numtris; k++) {
            w1 |= (u32)b->tris[i + k][0] << (k * 8);
            w1 |= (u32)b->tris[i + k][1] << (k * 8 + 4);
            w0 |= (u32)b->tris[i + k][2] << (k * 4);
        }
        emit(l, w0, w1);
    }
    b->numv = 0;
    b->numtris = 0;
    b->shifted = 0;
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

/* ------------------------------------------------------------ the build */

/* A triangle in the model's space. */
struct mtri {
    f32 pos[3][3];
    f32 uv[3][2];
    u32 argb[3];
    s16 tex;
    s16 node;     /* index into the entry's nodes */
    s16 group;    /* the Bean bone it rides, else 256 + its draw: dealt whole */
    u8 glass;     /* the release's blended pass with a translucent material */
    u8 kind;      /* its draw's BEANDRAW_* (round 25b) */
    u8 shader;    /* plain, no material, every corner white as stored: the release's shader colours it */
    u8 sphere;    /* round 26a (D-146): a sphere-mapped draw, drawn by GoldenEye's texgen on its normals */
    s8 nrm[3][3]; /* round 26a: the corners' normals, model axes, x127 */
};

struct pic {
    u8 used, alpha, soft;
    s16 standin;
};

struct bnode {
    ModelNode *node;
    f32 origin[3];          /* model space of its group */
    const Vertex *verts;
    s32 numverts;
    u8 candidate;           /* takes Bean triangles */
    u8 keep;                /* GoldenEye's own list stays (under a switch), unless it takes HD triangles */
    u8 switched;            /* under a switch, with more than a screen's quad: takes the HD triangles nearest it */
    s32 numtris;
    struct beanmodelnode out;
};

struct entry {
    ModelFileHeader *hdr;
    ModelNode *root;        /* round 26a: a hand's header is refilled per gun (beangun.c) */
    const char *gunfile;    /* round 26a: the G*Z file it held when built, NULL for a prop */
    s16 row;                /* propRows index, -1 none */
    s8 state;               /* 0 unbuilt, 1 built, -1 GoldenEye's own */
    s32 numnodes;
    struct bnode *nodes;
    u16 **standins;
    s32 numstandins;
    u32 meshbytes, picbytes;
};

static struct entry *entries;
static s32 numEntries, capEntries;
static struct entry *retired;
static s32 numRetired;
static s32 builtCount, fellbackCount;
static u32 meshBytes, picBytes;
static s32 prebuilt;
static u16 *whiteTile;       /* untextured triangles; not registered */

static s32 envOff(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANPROPS");
        v = e && *e == '0';
    }
    return v;
}

static s32 logOn(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANPROPLOG");
        v = e && *e ? (*e == '2' ? 2 : 1) : 0;
    }
    return v;
}

static s32 onlyAllows(const char *file)
{
    const char *e = getenv("GE_BEANPROPONLY");
    if (!e || !*e) {
        return 1;
    }
    const size_t n = strlen(file);
    for (const char *p = e; (p = strstr(p, file)) != NULL; p += n) {
        if ((p == e || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) {
            return 1;
        }
    }
    return 0;
}

/* Round 28b (D-177): the third-person item a player holds in the intro
 * swirl / cutscenes (bondview2.c, one player) is a copy of its P*Z header in
 * the left hand's weapon buffer: noted by beanModelAlias() and found by the
 * original's name while the copy still holds what it was filled with (the
 * same buffer is refilled with the first-person gun or the watch arm after,
 * as beanChrAlias's copies are). */
static struct {
    const ModelFileHeader *copy, *original;
    const ModelNode *root;
    s16 numMatrices;
} modelAliases[4];
static s32 numModelAliases;

void beanModelAlias(struct ModelFileHeader *copy, struct ModelFileHeader *original)
{
    s32 at = -1;
    for (s32 i = 0; i < numModelAliases && at < 0; i++) {
        at = modelAliases[i].copy == copy ? i : -1;
    }
    if (at < 0) {
        at = numModelAliases < 4 ? numModelAliases++ : 3;
    }
    modelAliases[at].copy = copy;
    modelAliases[at].original = original;
    modelAliases[at].root = copy->RootNode;
    modelAliases[at].numMatrices = copy->numMatrices;
}

static const char *fileOf(const ModelFileHeader *hdr)
{
    for (s32 i = 0; i < numModelAliases; i++) {
        if (modelAliases[i].copy == hdr && hdr->RootNode == modelAliases[i].root
                && hdr->numMatrices == modelAliases[i].numMatrices) {
            hdr = modelAliases[i].original;
            break;
        }
    }
    for (s32 i = 0; i < NUM_PITEMZ; i++) {
        if (PitemZ_entries[i].header == hdr) {
            return PitemZ_entries[i].filename;
        }
    }
    return beanGunFile(hdr);   /* round 26a: a player's first-person gun */
}

/* D-124 (round 23 merge): the boot and title models stay GoldenEye's. The
 * release's legal page carries its own text and sits ~210 model units above
 * GoldenEye's (drawn over the N64 text it doubles every line), and the
 * Nintendo/GoldenEye logos are sphere-mapped chrome, which C3 does not do
 * (docs/tier-c.md "What C3 left out" 2): the Nintendo logo drew magenta with
 * doubled letters on the phone. propfit.py still fits them; they are simply
 * never swapped. */
static const char *const frontEndModels[] = {
    "PnintendologoZ",
    "PgoldeneyelogoZ",
    "PlegalpageZ",
};

static s32 rowOf(const char *file)
{
    for (u32 i = 0; file && i < sizeof(frontEndModels) / sizeof(frontEndModels[0]); i++) {
        if (strcmp(frontEndModels[i], file) == 0) {
            return -1;
        }
    }
    for (s32 i = 0; file && i < NUM_PROPROWS; i++) {
        if (strcmp(propRows[i].file, file) == 0) {
            return i;
        }
    }
    return -1;
}

static s32 fileExists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* $C/xbla/files/new/<source>/default.bin (the importer's copy), else the
 * oracle's Video.XblaPath tree read in place (beanstage.c levelPath()). */
static s32 sourcePath(const char *source, char *out, size_t len)
{
    snprintf(out, len, "%s/files/new/%s/default.bin", sysResolvePath(TEXPACK_XBLA_DIR), source);
    if (fileExists(out)) {
        return 1;
    }
    const char *src = texpackXblaPath();
    if (src && *src) {
        char base[512];
        snprintf(base, sizeof(base), "%s", src);
        snprintf(out, len, "%s/files/new/%s/default.bin", base, source);
        if (fileExists(out)) {
            return 1;
        }
        snprintf(out, len, "%s/new/%s/default.bin", base, source);
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

/* The model's display-list nodes, in tree order, with the origin of the
 * group each is in (model space) and whether it hangs under a switch or a
 * far level of detail (gemodel.py nodes(), the same walk). */
static s32 collectNodes(ModelFileHeader *hdr, struct bnode *out, s32 max, s32 *underlod)
{
    ModelNode *n = hdr->RootNode;
    s32 count = 0;
    s32 steps = 0;

    *underlod = 0;
    while (n && steps++ < 4096) {
        const u32 type = n->Opcode & 0xff;
        if ((type == MODELNODE_OPCODE_DLCOLLISION || type == MODELNODE_OPCODE_DL) && n->Data && count < max) {
            const union ModelRoData *ro = n->Data;
            struct bnode *b = &out[count];
            memset(b, 0, sizeof(*b));
            b->node = n;
            if (type == MODELNODE_OPCODE_DLCOLLISION) {
                b->verts = ro->DisplayListCollisions.Primary ? ro->DisplayListCollisions.Vertices : NULL;
                b->numverts = ro->DisplayListCollisions.numVertices;
            } else {
                b->verts = ro->DisplayList.Primary ? ro->DisplayList.Vertices : NULL;
                b->numverts = ro->DisplayList.numVertices;
            }
            if (!b->verts || b->numverts <= 0) {
                b->verts = NULL;
                b->numverts = 0;
            }
            s32 sw = 0, lod = 0;
            for (ModelNode *up = n->Parent; up; up = up->Parent) {
                const u32 t = up->Opcode & 0xff;
                if ((t == MODELNODE_OPCODE_GROUP || t == MODELNODE_OPCODE_OP03 || t == MODELNODE_OPCODE_GROUPSIMPLE)
                        && up->Data) {
                    const coord3d *o = t == MODELNODE_OPCODE_GROUPSIMPLE ? &up->Data->GroupSimple.Origin
                                                                          : &up->Data->Group.Origin;
                    b->origin[0] += o->x;
                    b->origin[1] += o->y;
                    b->origin[2] += o->z;
                } else if (t == MODELNODE_OPCODE_SWITCH) {
                    sw = 1;
                } else if (t == MODELNODE_OPCODE_LOD && up->Data && up->Data->LOD.MinDistance > 0.0f) {
                    lod = 1;
                }
            }
            b->keep = (u8)(sw || lod); /* a far level of detail stays GoldenEye's */
            b->candidate = !sw && !lod && b->numverts > 0;
            /* a switched part (the truck's canopy) takes the HD triangles
             * nearest its own vertices and shows and hides with its switch; a
             * screen's programme quad (4 vertices: the TV, the modem box) is
             * never one - GoldenEye's own programme stays (PD) */
            b->switched = sw && !lod && b->numverts > 8;
            *underlod |= lod;
            count++;
        }
        if (n->Child) {
            n = n->Child;
        } else {
            while (n) {
                if (n->Next) {
                    n = n->Next;
                    break;
                }
                n = n->Parent;
            }
        }
    }
    return count;
}

struct collect {
    struct mtri *tris;
    s32 num, cap;
    const struct proprow *row;
    s32 mirror;
    s32 black, glass;
    s32 gun, cards;         /* round 26a: a gun file, its flash cards left out */
    u32 tint;               /* round 26a: beanGunTint(), 0 none */
    struct beanlevel *bm;   /* round 26a: for the tint's glove test */
    const u8 *flashdraw;    /* round 30a (D-247): a held gun's (prop/chr*) flash-card draws, by draw */
    s32 flashdraws;
    s32 held;               /* round 31c (D-283): a held gun (prop/chr*), drawn under the character's combiner */
    s32 heldclear, heldopaque; /* its glass triangles left out (alpha 0) / drawn opaque */
};

/* Round 30a (D-247): a held gun's muzzle-flash cards. The release's held
 * guns (prop/chr*) are drawn by 0x01 draws (no pieces, so the first-person
 * guns' piece-0 rule cannot see them); the M16's carries its flash star as a
 * draw of its own - every corner on one plane across the barrel (file y, the
 * held guns' barrel axis), reaching wider across than the rest of the gun.
 * GoldenEye draws its own flash for the shot (GUNFIRE node), as for the
 * first-person guns: such a draw is left out. */
#define FLASH_MAXDRAWS 256
struct flashscan {
    f32 lo[FLASH_MAXDRAWS][3], hi[FLASH_MAXDRAWS][3];
    u8 seen[FLASH_MAXDRAWS];
};

static void flashScanTri(void *arg, int tex, const struct beanmodelvtx *v)
{
    struct flashscan *f = arg;
    (void)tex;
    const s32 d = v[0].draw;
    if (d >= FLASH_MAXDRAWS) {
        return;
    }
    for (s32 k = 0; k < 3; k++) {
        for (s32 a = 0; a < 3; a++) {
            if (!f->seen[d] || v[k].pos[a] < f->lo[d][a]) {
                f->lo[d][a] = v[k].pos[a];
            }
            if (!f->seen[d] || v[k].pos[a] > f->hi[d][a]) {
                f->hi[d][a] = v[k].pos[a];
            }
        }
        f->seen[d] = 1;
    }
}

/* Round 35b (D-322), PROPROW_FLASH_AHEAD rows only: the auto shotgun's flash
 * star is solid (46 units deep) and split over four draws, one per quadrant,
 * none wider than the gun alone - so not D-247's flat card. It lies wholly in
 * front of the muzzle (file y min; held guns' barrels run along file y):
 * the draws whose y ranges chain from the muzzle end with a gap before the
 * rest, together wider across (x or z) than the rest, are left out. */
static s32 flashAhead(const struct flashscan *f, u8 *out)
{
    u8 in[FLASH_MAXDRAWS];
    f32 reach = 1e30f;
    s32 any = 0, grew = 1, n = 0;
    memset(in, 0, sizeof(in));
    for (s32 d = 0; d < FLASH_MAXDRAWS; d++) {
        if (f->seen[d] && f->lo[d][1] < reach) {
            reach = f->lo[d][1];
        }
    }
    while (grew) {   /* the chain of overlapping y ranges from the muzzle end */
        grew = 0;
        for (s32 d = 0; d < FLASH_MAXDRAWS; d++) {
            if (f->seen[d] && !in[d] && f->lo[d][1] <= reach + 0.5f) {
                in[d] = 1;
                any = 1;
                grew = 1;
                reach = __builtin_fmaxf(reach, f->hi[d][1]);
            }
        }
    }
    f32 lo[2][3], hi[2][3];
    s32 has[2] = { 0, 0 };
    for (s32 d = 0; d < FLASH_MAXDRAWS; d++) {
        if (!f->seen[d]) {
            continue;
        }
        const s32 k = in[d] ? 0 : 1;
        for (s32 a = 0; a < 3; a++) {
            lo[k][a] = has[k] ? __builtin_fminf(lo[k][a], f->lo[d][a]) : f->lo[d][a];
            hi[k][a] = has[k] ? __builtin_fmaxf(hi[k][a], f->hi[d][a]) : f->hi[d][a];
        }
        has[k] = 1;
    }
    if (!any || !has[1] || !(hi[0][0] - lo[0][0] > hi[1][0] - lo[1][0] || hi[0][2] - lo[0][2] > hi[1][2] - lo[1][2])) {
        return 0;
    }
    for (s32 d = 0; d < FLASH_MAXDRAWS; d++) {
        if (in[d]) {
            out[d] = 1;
            n++;
        }
    }
    return n;
}

static s32 flashDraws(struct beanlevel *bm, u8 *out, s32 ahead)
{
    struct flashscan *f = calloc(1, sizeof(*f));
    s32 n = 0;
    if (!f) {
        return 0;
    }
    beanModelTriangles(bm, flashScanTri, f);
    if (ahead) {
        n = flashAhead(f, out);
    }
    for (s32 d = 0; d < FLASH_MAXDRAWS; d++) {
        if (!f->seen[d] || out[d]) {
            continue;
        }
        if (f->hi[d][1] - f->lo[d][1] > 0.5f) {
            continue;
        }
        /* wider across the barrel (x or z) than every other draw together */
        f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        s32 others = 0;
        for (s32 e = 0; e < FLASH_MAXDRAWS; e++) {
            if (e == d || !f->seen[e]) {
                continue;
            }
            others = 1;
            for (s32 a = 0; a < 3; a++) {
                lo[a] = __builtin_fminf(lo[a], f->lo[e][a]);
                hi[a] = __builtin_fmaxf(hi[a], f->hi[e][a]);
            }
        }
        if (others && (f->hi[d][0] - f->lo[d][0] > hi[0] - lo[0] || f->hi[d][2] - f->lo[d][2] > hi[2] - lo[2])) {
            out[d] = 1;
            n++;
        }
    }
    free(f);
    return n;
}

/* Round 25b (D-142): this level's prop coverage, for the [xbla] stage line. */
static struct {
    s32 models;       /* models built HD */
    s32 unresolved;   /* models left GoldenEye's own for an unresolved reference */
    s32 draws, kinds[BEANDRAW_KINDS], unreadable;
    s32 pictures, decoded;
    s32 placeholders; /* shader-only triangles under a list GoldenEye keeps: dropped */
    s32 spheretris;   /* round 26a: sphere-mapped triangles drawn by texgen */
    s32 gunsHD, gunsN64; /* round 26a: first-person guns built HD / left GoldenEye's */
    s32 refs;         /* unresolved references named */
} pa;
static s32 summarised;

static void addTri(void *arg, int tex, const struct beanmodelvtx *v)
{
    struct collect *c = arg;
    struct mtri t;

    if (c->flashdraws && v[0].draw < FLASH_MAXDRAWS && c->flashdraw[v[0].draw]) {
        c->cards++;     /* round 30a (D-247): a held gun's flash cards */
        return;
    }
    if (c->gun && beanGunDrawIsCard(v[0].piece)) {
        c->cards++;     /* GoldenEye's own flash draws for the shot (beangun.h) */
        return;
    }
    memset(&t, 0, sizeof(t));
    for (s32 k = 0; k < 3; k++) {
        const s32 src = c->mirror ? (k == 0 ? 0 : 3 - k) : k;
        for (s32 a = 0; a < 3; a++) {
            t.pos[k][a] = c->row->sign[a] * v[src].pos[c->row->perm[a]] * c->row->scale[a] + c->row->centre[a];
        }
        t.uv[k][0] = v[src].uv[0];
        t.uv[k][1] = v[src].uv[1];
        {
            /* round 26a: the normal under the placement's axes (a uniform
             * scale and the centre do not turn it), for the sphere map; a
             * per-axis scale (round 35b) turns it by the inverse scale */
            f32 n[3], len = 0.0f;
            for (s32 a = 0; a < 3; a++) {
                n[a] = c->row->sign[a] * v[src].nrm[c->row->perm[a]];
                if (c->row->flags & PROPROW_OVERRIDE) {
                    n[a] /= c->row->scale[a];
                }
                len += n[a] * n[a];
            }
            len = len > 1e-12f ? 1.0f / sqrtf(len) : 0.0f;
            for (s32 a = 0; a < 3; a++) {
                t.nrm[k][a] = (s8)roundf(n[a] * len * 127.0f);
            }
        }
        u32 argb = v[src].argb;
        /* PD: opaque black is a part exported with no colour of its own, not
         * paint (the clipboard, the desk lamp, the bike's bars) */
        /* round 26a (PD gebeanBuildFirstPerson): a gun's vertex colours are
         * its gunmetal shading (texel x vertex colour, the PP7 at 0x30) -
         * kept, black included, or every HD gun draws bright silver */
        if (argb == 0xff000000u && !c->gun) {
            argb = 0xffffffffu;
            c->black++;
        }
        /* PD: glass is the release's blended pass with a material colour
         * whose alpha is under one (the window 0.56, the glassware 0.51) -
         * that alpha times the vertex's */
        if (v[src].blend && v[src].matalpha < 0xff) {
            const u32 a = ((argb >> 24) * v[src].matalpha + 127) / 255;
            argb = (argb & 0x00ffffffu) | (a << 24);
            t.glass = 1;
        }
        /* round 26a: the Golden Gun's gold (beanGunTint), not on its glove */
        if (c->tint && tex >= 0) {
            int32_t tw = 0, th = 0;
            if (!(beanLevelTextureSize(c->bm, tex, &tw, &th) && beanGunPictureIsGlove(tw, th))) {
                u32 o = argb & 0xff000000u;
                for (s32 sh = 16; sh >= 0; sh -= 8) {
                    o |= ((((argb >> sh) & 0xff) * ((c->tint >> sh) & 0xff) + 127) / 255) << sh;
                }
                argb = o;
            }
        }
        t.argb[k] = argb;
    }
    /* round 31c (D-283): a held gun is drawn inside a character's list, whose
     * combiner reads the vertex alpha as GoldenEye's blood lerp (environment
     * colour -> texel, D-175), not as cover: the RC-P90's clear magazine
     * (alpha 0) drew solid blood red. Glass a character holds cannot be
     * translucent: fully clear glass is left out (it draws nothing in the
     * first-person gun either), any other glass is drawn opaque. */
    if (c->held && t.glass) {
        if ((t.argb[0] >> 24) == 0 && (t.argb[1] >> 24) == 0 && (t.argb[2] >> 24) == 0) {
            c->heldclear++;
            return;
        }
        for (s32 k = 0; k < 3; k++) {
            t.argb[k] |= 0xff000000u;
        }
        t.glass = 0;
        c->heldopaque++;
    }
    t.kind = v[0].kind;
    t.sphere = v[0].kind == BEANDRAW_SPHEREMAP && tex >= 0;
    t.shader = v[0].kind == BEANDRAW_COLOUR && !v[0].ownmat && v[0].argb == 0xffffffffu && v[1].argb == 0xffffffffu
        && v[2].argb == 0xffffffffu;
    c->glass += t.glass;
    t.tex = (s16)tex;
    t.group = (s16)(v[0].bone >= 0 ? v[0].bone : 256 + (v[0].draw & 0x3fff));
    if (c->num >= c->cap) {
        const s32 cap = c->cap ? c->cap * 2 : 1024;
        struct mtri *n = realloc(c->tris, sizeof(*n) * (size_t)cap);
        if (!n) {
            return;
        }
        c->tris = n;
        c->cap = cap;
    }
    c->tris[c->num++] = t;
}

static u16 texel5551(u32 r, u32 g, u32 b)
{
    const u16 t = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
    return (u16)((t >> 8) | (t << 8));
}

static u16 *newTile(u16 texel)
{
    u16 *tile = malloc(TILE * TILE * sizeof(u16));
    if (tile) {
        for (s32 i = 0; i < TILE * TILE; i++) {
            tile[i] = texel;
        }
    }
    return tile;
}

/* The texture load for one stand-in (beanstage.c emitTexture()): a 32x32
 * RGBA16, wrap, mask 5, its host address in G_SETTIMG. */
static void emitTexture(struct gdlbuf *l, const u16 *tile)
{
    emit(l, 0xba000e02, 0x00000000);             /* G_TT_NONE */
    emit(l, 0xfd100000, (uintptr_t)tile);        /* SETTIMG RGBA 16b */
    emit(l, 0xf5100000, 0x07014050);             /* SETTILE load tile, wrap, mask 5 */
    emit(l, 0xe6000000, 0x00000000);             /* LOADSYNC */
    emit(l, 0xf3000000, 0x073ff100);             /* LOADBLOCK 1024 texels */
    emit(l, 0xe7000000, 0x00000000);             /* PIPESYNC */
    emit(l, 0xf5101000, 0x00014050);             /* SETTILE render tile */
    emit(l, 0xf2000000, 0x0007c07c);             /* SETTILESIZE 32x32 */
}

static const struct mtri *sortTris;

static int compareTex(const void *a, const void *b)
{
    const struct mtri *ta = &sortTris[*(const s32 *)a];
    const struct mtri *tb = &sortTris[*(const s32 *)b];
    if (ta->tex != tb->tex) {
        return ta->tex - tb->tex;
    }
    if (ta->sphere != tb->sphere) {
        return ta->sphere - tb->sphere;
    }
    return *(const s32 *)a - *(const s32 *)b;
}

/* Round 28b (D-176): the watch arm's painter's order. The watch faces +y in
 * Csuit_lf_handZ's model space (the hands sit above the screen) and the arm
 * is held up to the camera there: lowest y first. */
static int compareArmDepth(const void *a, const void *b)
{
    const struct mtri *ta = &sortTris[*(const s32 *)a];
    const struct mtri *tb = &sortTris[*(const s32 *)b];
    const f32 ya = ta->pos[0][1] + ta->pos[1][1] + ta->pos[2][1];
    const f32 yb = tb->pos[0][1] + tb->pos[1][1] + tb->pos[2][1];
    if (ya != yb) {
        return ya < yb ? -1 : 1;
    }
    return *(const s32 *)a - *(const s32 *)b;
}

/* The sleeve is layers round the forearm (the shirt's cuff inside the
 * jacket's): with no depth buffer a far-to-near order interleaves them, so
 * a sleeve list goes picture by picture, the innermost picture (its
 * triangles' mean distance from the sleeve's own long axis) first; back
 * faces culled within each. */
double sqrt(double x);
static f32 *sortLayer;   /* per picture + 1 (index 0: untextured) */

static int compareArmLayer(const void *a, const void *b)
{
    const struct mtri *ta = &sortTris[*(const s32 *)a];
    const struct mtri *tb = &sortTris[*(const s32 *)b];
    const f32 la = sortLayer[ta->tex + 1], lb = sortLayer[tb->tex + 1];
    if (la != lb) {
        return la < lb ? -1 : 1;
    }
    if (ta->tex != tb->tex) {
        return ta->tex - tb->tex;
    }
    return compareArmDepth(a, b);
}

static void armLayers(const struct mtri *tris, const s32 *list, s32 num, s32 numtex)
{
    f64 mean[3] = { 0, 0, 0 }, cov[3][3] = { { 0 } }, axis[3] = { 1, 0, 0 };
    f64 *sum = calloc((size_t)numtex + 1, sizeof(f64));
    s32 *cnt = calloc((size_t)numtex + 1, sizeof(s32));
    free(sortLayer);
    sortLayer = calloc((size_t)numtex + 1, sizeof(f32));
    if (!sum || !cnt || !sortLayer || num <= 0) {
        free(sum);
        free(cnt);
        return;
    }
    for (s32 i = 0; i < num; i++) {
        for (s32 a = 0; a < 3; a++) {
            mean[a] += (tris[list[i]].pos[0][a] + tris[list[i]].pos[1][a] + tris[list[i]].pos[2][a]) / 3.0;
        }
    }
    for (s32 a = 0; a < 3; a++) {
        mean[a] /= num;
    }
    for (s32 i = 0; i < num; i++) {
        f64 d[3];
        for (s32 a = 0; a < 3; a++) {
            d[a] = (tris[list[i]].pos[0][a] + tris[list[i]].pos[1][a] + tris[list[i]].pos[2][a]) / 3.0 - mean[a];
        }
        for (s32 r = 0; r < 3; r++) {
            for (s32 k = 0; k < 3; k++) {
                cov[r][k] += d[r] * d[k];
            }
        }
    }
    for (s32 it = 0; it < 32; it++) {   /* the long axis: power iteration */
        f64 n[3] = { 0, 0, 0 }, len = 0;
        for (s32 r = 0; r < 3; r++) {
            for (s32 k = 0; k < 3; k++) {
                n[r] += cov[r][k] * axis[k];
            }
            len += n[r] * n[r];
        }
        len = len > 0 ? 1.0 / sqrt(len) : 0;
        for (s32 r = 0; r < 3; r++) {
            axis[r] = n[r] * len;
        }
    }
    for (s32 i = 0; i < num; i++) {
        const struct mtri *t = &tris[list[i]];
        f64 d[3], along = 0, r2 = 0;
        for (s32 a = 0; a < 3; a++) {
            d[a] = (t->pos[0][a] + t->pos[1][a] + t->pos[2][a]) / 3.0 - mean[a];
            along += d[a] * axis[a];
        }
        for (s32 a = 0; a < 3; a++) {
            const f64 q = d[a] - along * axis[a];
            r2 += q * q;
        }
        const s32 k = t->tex >= -1 && t->tex < numtex ? t->tex + 1 : 0;
        sum[k] += sqrt(r2);
        cnt[k]++;
    }
    for (s32 k = 0; k <= numtex; k++) {
        sortLayer[k] = cnt[k] ? (f32)(sum[k] / cnt[k]) : 0.0f;
    }
    free(sum);
    free(cnt);
}

/*
 * One list of a node: its triangles by picture, 16-vertex batches, positions
 * relative to the node's group origin, UVs S10.5 on the stand-in, each
 * batch's window shifted to whole repeats at its first triangle (re-centred
 * on one that does not fit). Both faces drawn (the release's winding is its
 * own, and a closed prop shows no difference); G_CULL_BACK put back after,
 * as a GoldenEye list leaves it. The render mode and combiner are the
 * model's (modelApplyRenderModeType*), so GoldenEye's per-object fog/shade
 * colour, its shot darkening and its fades apply unchanged.
 */
static Gfx *writeList(struct vtxbuf *vb, const struct mtri *tris, s32 *list, s32 num, const f32 *origin,
                      const struct pic *pics, u16 *const *standins, s32 alpha, s32 *dropped, u32 *bytes, s32 armview,
                      s32 numtex)
{
    struct gdlbuf l;
    struct batch b;
    s32 curtex = -2;
    s32 cursphere = 0;

    if (num == 0) {
        return NULL;
    }
    memset(&l, 0, sizeof(l));
    memset(&b, 0, sizeof(b));
    sortTris = tris;
    if (armview == 2) {
        armLayers(tris, list, num, numtex);
    }
    qsort(list, (size_t)num, sizeof(*list),
          armview == 2 && sortLayer ? compareArmLayer : armview ? compareArmDepth : compareTex);

    emit(&l, 0xe7000000, 0x00000000);   /* PIPESYNC */
    emit(&l, 0xba001001, 0x00010000);   /* texture LOD (fast3d: tile 0 for both texels) */
    emit(&l, 0xba001102, 0x00000000);   /* TEXTDETAIL clamp */
    emit(&l, 0xba000c02, 0x00002000);   /* bilinear */
    if (armview) {
        /* round 28b (D-176): the watch arm is drawn with no depth buffer
         * (bondview2.c, zbufferenabled 0): back faces culled, as the
         * release's own character lists, and far to near */
        emit(&l, 0xb6000000, 0x00001000);
        emit(&l, 0xb7000000, 0x00002000);
    } else {
        emit(&l, 0xb6000000, 0x00003000);   /* both faces (G_CULL_BOTH off: a mirrored door sets G_CULL_FRONT) */
    }
    for (s32 i = 0; i < num; i++) {
        const struct mtri *t = &tris[list[i]];
        Vtx rv[3];
        u8 idx[3];
        s32 ok = 1, need = 0;

        memset(rv, 0, sizeof(rv));
        if (t->tex != curtex || t->sphere != cursphere) {
            batchFlush(&l, vb, &b);
            /* round 26a (D-146): a sphere-mapped picture is looked up by the
             * normal - GoldenEye's own texgen (G_LIGHTING | G_TEXTURE_GEN,
             * lit by the frame's light and look-at: bg.c's GlobalLight, the
             * gun's g_WeaponEnvmapLight), the map spread over the 32-texel
             * stand-in as an N64 sphere map is (scale 31 x 64) */
            if (t->sphere && !cursphere) {
                emit(&l, 0xb7000000, G_LIGHTING | G_TEXTURE_GEN);
            } else if (!t->sphere && cursphere) {
                emit(&l, 0xb6000000, G_LIGHTING | G_TEXTURE_GEN);
            }
            emit(&l, alpha ? 0xbb002801 : 0xbb003001, t->sphere ? 0x07c007c0 : 0xffffffff); /* G_TEXTURE on */
            emitTexture(&l, t->tex >= 0 && pics[t->tex].used ? standins[pics[t->tex].standin] : whiteTile);
            curtex = t->tex;
            cursphere = t->sphere;
        }
        if (!b.shifted) {
            b.shiftu = floorf(t->uv[0][0]);
            b.shiftv = floorf(t->uv[0][1]);
            b.shifted = 1;
        }
        for (s32 pass = 0; pass < 2; pass++) {
            ok = 1;
            for (s32 k = 0; k < 3 && ok; k++) {
                ok = clampS16(t->pos[k][0] - origin[0], &rv[k].v.ob[0])
                    && clampS16(t->pos[k][1] - origin[1], &rv[k].v.ob[1])
                    && clampS16(t->pos[k][2] - origin[2], &rv[k].v.ob[2])
                    && clampS16((t->uv[k][0] - b.shiftu) * TILE_SCALE, &rv[k].v.tc[0])
                    && clampS16((t->uv[k][1] - b.shiftv) * TILE_SCALE, &rv[k].v.tc[1]);
                rv[k].v.cn[0] = (u8)(t->argb[k] >> 16);
                rv[k].v.cn[1] = (u8)(t->argb[k] >> 8);
                rv[k].v.cn[2] = (u8)t->argb[k];
                rv[k].v.cn[3] = (u8)(t->argb[k] >> 24);
                if (t->sphere) {
                    /* lit vertices carry their normal where the colour was */
                    rv[k].v.tc[0] = rv[k].v.tc[1] = 0;
                    rv[k].v.cn[0] = (u8)t->nrm[k][0];
                    rv[k].v.cn[1] = (u8)t->nrm[k][1];
                    rv[k].v.cn[2] = (u8)t->nrm[k][2];
                }
            }
            if (ok || pass == 1) {
                break;
            }
            batchFlush(&l, vb, &b);
            b.shiftu = floorf((t->uv[0][0] + t->uv[1][0] + t->uv[2][0]) / 3.0f);
            b.shiftv = floorf((t->uv[0][1] + t->uv[1][1] + t->uv[2][1]) / 3.0f);
            b.shifted = 1;
        }
        if (!ok) {
            (*dropped)++;
            continue;
        }
        for (s32 k = 0; k < 3; k++) {
            if (batchFind(&b, &rv[k]) < 0) {
                need++;
            }
        }
        if (b.numv + need > BATCHVERTS || b.numtris >= 64) {
            const f32 su = b.shiftu, sv = b.shiftv;
            batchFlush(&l, vb, &b);
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
        b.numtris++;
    }
    batchFlush(&l, vb, &b);
    if (cursphere) {
        emit(&l, 0xb6000000, G_LIGHTING | G_TEXTURE_GEN);
    }
    emit(&l, 0xba001001, 0x00000000);   /* texture LOD off again */
    emit(&l, 0xb7000000, 0x00002000);   /* culling back on */
    emit(&l, 0xb8000000, 0x00000000);   /* G_ENDDL */
    *bytes += (u32)l.cap * (u32)sizeof(Gfx);
    return l.g;
}

static void freeEntry(struct entry *e)
{
    for (s32 i = 0; i < e->numnodes; i++) {
        free(e->nodes[i].out.vtx);
        free(e->nodes[i].out.solid);
        free(e->nodes[i].out.cut);
        free(e->nodes[i].out.xlu);
    }
    free(e->nodes);
    for (s32 i = 0; i < e->numstandins; i++) {
        free(e->standins[i]);
    }
    free(e->standins);
    memset(e, 0, sizeof(*e));
}

static s32 nodeUnder(const ModelNode *node, const ModelNode *up)
{
    for (const ModelNode *n = node; n && up; n = n->Parent) {
        if (n == up) {
            return 1;
        }
    }
    return 0;
}

#define ARM_SUIT_SLEEVE 7   /* bondviewSelectCuff(..., 4) + 3: CUFF_BLUE, BODY_Formal_Wear (Frigate, Statue, Train) */

/* Round 28b (D-176): the watch arm's deal. The release's clock hands (bones
 * SKEL_HOUR/MINUTE/SECOND) ride GoldenEye's matrices 1/2/3 (bondview2.c
 * turns them by the clock); every other triangle goes to the nearest list
 * on matrix 0 - the hand, the watch, or the sleeve. Only the outfit's own
 * sleeve is linked into the tree (the switch's child): a triangle nearest
 * it is the release's suit sleeve, drawn on the suit's list and dropped on
 * any other outfit's (GoldenEye's own sleeve shows). Returns the number of
 * clock hands placed. */
static s32 dealArm(struct beanlevel *bm, struct collect *c, struct bnode *nodes, s32 numnodes, const s8 *armsw)
{
    static const char *const hands[3] = { "SKEL_HOUR", "SKEL_MINUTE", "SKEL_SECOND" };
    struct beanskel *sk = calloc(1, sizeof(*sk));
    s32 mtx[MAXNODES], handnode[3] = { -1, -1, -1 }, placed = 0;

    for (s32 i = 0; i < numnodes; i++) {
        mtx[i] = modelFindNodeMtxIndex(nodes[i].node, 0);
        for (s32 h = 0; h < 3; h++) {
            if (nodes[i].candidate && mtx[i] == h + 1 && handnode[h] < 0) {
                handnode[h] = i;
            }
        }
    }
    const s32 numbones = sk ? beanModelSkeleton(bm, sk) : 0;
    for (s32 t = 0; t < c->num; t++) {
        struct mtri *tri = &c->tris[t];
        s32 h = -1;
        const s32 bone = tri->group < 256 ? beanModelPoseBone(bm, tri->group) : -1;
        if (bone >= 0 && bone < numbones) {
            for (s32 k = 0; k < 3 && h < 0; k++) {
                h = strncmp(sk->name[bone], hands[k], strlen(hands[k])) == 0 ? k : -1;
            }
        }
        if (h >= 0 && handnode[h] >= 0) {
            tri->node = (s16)handnode[h];
            placed |= 1 << h;
            continue;
        }
        f32 m[3], best = 1e30f;
        s32 bi = -1;
        if ((t & 7) != 0 && t + 1 < c->num) {
            /* the picture vote below needs a sample: every eighth triangle
             * measures, the rest take their picture's winner */
            tri->node = -2;
            continue;
        }
        for (s32 a = 0; a < 3; a++) {
            m[a] = (tri->pos[0][a] + tri->pos[1][a] + tri->pos[2][a]) / 3.0f;
        }
        for (s32 i = 0; i < numnodes; i++) {
            if ((!nodes[i].candidate && armsw[i] < 4) || mtx[i] != 0 || armsw[i] == 3) {
                continue;
            }
            for (s32 v = 0; v < nodes[i].numverts; v++) {
                const f32 dx = nodes[i].verts[v].coord.x + nodes[i].origin[0] - m[0];
                const f32 dy = nodes[i].verts[v].coord.y + nodes[i].origin[1] - m[1];
                const f32 dz = nodes[i].verts[v].coord.z + nodes[i].origin[2] - m[2];
                const f32 d = dx * dx + dy * dy + dz * dz;
                if (d < best) {
                    best = d;
                    bi = i;
                }
            }
        }
        tri->node = (s16)bi;
    }
    /* then each picture goes whole to the list most of its triangles lie
     * nearest (the wrist's skin to the hand, the strap to the watch, the
     * cloth to the sleeve): drawn with no depth buffer, a picture split over
     * two lists draws part of itself over what it should be under */
    {
        s32 maxtex = 0;
        for (s32 t = 0; t < c->num; t++) {
            maxtex = c->tris[t].tex + 2 > maxtex ? c->tris[t].tex + 2 : maxtex;
        }
        s32 *votes = calloc((size_t)maxtex * (size_t)numnodes, sizeof(s32));
        for (s32 t = 0; votes && t < c->num; t++) {
            const struct mtri *tri = &c->tris[t];
            if (tri->node >= 0 && mtx[tri->node] == 0) {
                votes[(size_t)(tri->tex + 1) * (size_t)numnodes + (size_t)tri->node]++;
            }
        }
        for (s32 t = 0; votes && t < c->num; t++) {
            struct mtri *tri = &c->tris[t];
            if (tri->node == -1 || (tri->node >= 0 && mtx[tri->node] != 0)) {
                continue;
            }
            const s32 *v = &votes[(size_t)(tri->tex + 1) * (size_t)numnodes];
            s32 best = -1;
            for (s32 i = 0; i < numnodes; i++) {
                best = v[i] > 0 && (best < 0 || v[i] > v[best]) ? i : best;
            }
            tri->node = (s16)best;
        }
        free(votes);
        for (s32 t = 0; t < c->num; t++) {
            if (c->tris[t].node < -1) {
                c->tris[t].node = -1;   /* a picture no sampled triangle voted for */
            }
            if (c->tris[t].node >= 0 && !nodes[c->tris[t].node].candidate) {
                c->tris[t].node = -1;   /* another outfit's sleeve: GoldenEye's own shows */
            }
        }
    }
    if (logOn()) {
        for (s32 i = 0; i < numnodes; i++) {
            s32 n = 0;
            for (s32 t = 0; t < c->num; t++) {
                n += c->tris[t].node == i;
            }
            sysLogPrintf(LOG_NOTE, "beanmodel: Csuit_lf_handZ list %d: op %d, switch %d, %d vertices, matrix %d, %s, %d HD triangles", i,
                         nodes[i].node->Opcode & 0xff, armsw[i], nodes[i].numverts, mtx[i],
                         nodes[i].candidate ? "HD" : nodes[i].keep ? "GoldenEye's (switched)" : "GoldenEye's", n);
        }
    }
    free(sk);
    return (placed & 1) + ((placed >> 1) & 1) + ((placed >> 2) & 1);
}

/* Round 28b (D-180): shader-only placeholders left out by name, where no
 * GoldenEye list covers them (PD gebean.c beanVertexDrops' role). Silo's
 * satellite (prop/sat1reflect): a flat half-disc, radius ~555 of a 10150-unit
 * model, at its aft end (z -4550) - coloured by the release's shader, a
 * glow GoldenEye's model does not have. Without it the satellite was the
 * last unresolved reference layer-wide and drew GoldenEye's own whole. */
/*
 * Round 36 (doc 48, R36-1): the release's prop decals - stencilled text on a
 * crate's brace, a hazard label, a container's number - are their own
 * triangles lying ON a face of another picture (Xbox draws them over it). Here
 * the base and the decal take the same depth mode (the model's ZMODE_OPA, GL
 * LESS), and the decal's s16-rounded corners put it a fraction of a unit in
 * front of or behind its face at random: they z-fought, and with Bond's idle
 * sway the text flickered from frame to frame (Austin, headset, 0.0.0.12).
 * beanstage.c's markDecalLifts() does the same for the rooms.
 *
 * A triangle lies on a base when the two face the same way or opposite ways
 * (|cos| >= DECAL_COS), its three corners are within tol of the base's plane
 * and its middle lies inside the base. Of such a pair the decal is the one
 * drawn later (translucent over cut-out over solid, i.e. writeList()'s kinds)
 * and, within a kind, the smaller one. It is moved off the base's plane, away
 * from the model's middle (the outside of a closed prop), by lift. Pairs with
 * the plane through the middle are left alone. tol and lift are fractions of
 * the model's size, so they are the same share of every prop whatever its
 * scale: a crate (840 units) gets tol 1.7 and lift 3.4 model units (about a
 * third of a world unit at its 0.1 scale). GE_BEANPROPDECALS=0 turns it off.
 */
#define PROPDECAL_COS 0.999f
#define MIN2F(a, b) ((a) < (b) ? (a) : (b))
#define MAX2F(a, b) ((a) > (b) ? (a) : (b))
#define MIN3F(a, b, c) MIN2F(MIN2F(a, b), c)
#define MAX3F(a, b, c) MAX2F(MAX2F(a, b), c)
static s32 propDecalsOff(void)
{
    static s32 v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANPROPDECALS");
        v = e && e[0] == '0';
    }
    return v;
}

static s32 decalKind(const struct mtri *t, const struct pic *pics, s32 numtex)
{
    const s32 tex = t->tex >= 0 && t->tex < numtex && pics[t->tex].used ? t->tex : -1;
    if (t->glass || (tex >= 0 && pics[tex].soft)) {
        return 2;
    }
    return tex >= 0 && pics[tex].alpha ? 1 : 0;
}

static void triNormal(const f32 p[3][3], f32 n[3], f32 *area)
{
    const f32 a[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
    const f32 b[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
    n[0] = a[1] * b[2] - a[2] * b[1];
    n[1] = a[2] * b[0] - a[0] * b[2];
    n[2] = a[0] * b[1] - a[1] * b[0];
    const f32 len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    *area = len * 0.5f;
    if (len > 0.0f) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    }
}

/* whether q (in the plane of p, normal n) lies inside p, with a margin */
static s32 insideTri(const f32 p[3][3], const f32 n[3], const f32 q[3], f32 margin)
{
    for (s32 k = 0; k < 3; k++) {
        const f32 *a = p[k], *b = p[(k + 1) % 3];
        const f32 e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const f32 d[3] = { q[0] - a[0], q[1] - a[1], q[2] - a[2] };
        const f32 c[3] = { e[1] * d[2] - e[2] * d[1], e[2] * d[0] - e[0] * d[2], e[0] * d[1] - e[1] * d[0] };
        const f32 elen = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (elen > 0.0f && (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]) / elen < -margin) {
            return 0;
        }
    }
    return 1;
}

static s32 liftDecals(struct mtri *tris, s32 num, const struct pic *pics, s32 numtex, const f32 lo[3], const f32 hi[3])
{
    if (propDecalsOff() || num < 2) {
        return 0;
    }
    f32 size = 0.0f, mid[3];
    for (s32 a = 0; a < 3; a++) {
        size = hi[a] - lo[a] > size ? hi[a] - lo[a] : size;
        mid[a] = (lo[a] + hi[a]) * 0.5f;
    }
    const f32 tol = size * 0.002f > 1.0f ? size * 0.002f : 1.0f;
    const f32 lift = 2.0f * tol;
    f32(*nrm)[3] = malloc(sizeof(*nrm) * (size_t)num);
    f32(*cen)[3] = malloc(sizeof(*cen) * (size_t)num);
    f32(*box)[6] = malloc(sizeof(*box) * (size_t)num);
    f32 *area = malloc(sizeof(f32) * (size_t)num);
    s8 *kind = malloc((size_t)num);
    s32 *base = malloc(sizeof(s32) * (size_t)num);
    s32 lifted = 0;
    if (!nrm || !cen || !box || !area || !kind || !base) {
        goto out;
    }
    for (s32 t = 0; t < num; t++) {
        triNormal(tris[t].pos, nrm[t], &area[t]);
        kind[t] = (s8)decalKind(&tris[t], pics, numtex);
        base[t] = -1;
        for (s32 a = 0; a < 3; a++) {
            cen[t][a] = (tris[t].pos[0][a] + tris[t].pos[1][a] + tris[t].pos[2][a]) / 3.0f;
            box[t][a] = MIN3F(tris[t].pos[0][a], tris[t].pos[1][a], tris[t].pos[2][a]) - tol;
            box[t][a + 3] = MAX3F(tris[t].pos[0][a], tris[t].pos[1][a], tris[t].pos[2][a]) + tol;
        }
    }
    /* find every decal's base first, then lift, so a lifted decal is not
     * itself taken for a base */
    for (s32 t = 0; t < num; t++) {
        if (area[t] <= 0.0f) {
            continue;
        }
        for (s32 u = 0; u < num && base[t] < 0; u++) {
            if (u == t || tris[u].tex == tris[t].tex || area[u] <= 0.0f) {
                continue;
            }
            /* t is the later draw: a later kind, or the smaller in one kind */
            if (kind[u] > kind[t] || (kind[u] == kind[t] && (area[u] < area[t] || (area[u] == area[t] && u > t)))) {
                continue;
            }
            if (cen[t][0] < box[u][0] || cen[t][0] > box[u][3] || cen[t][1] < box[u][1] || cen[t][1] > box[u][4]
                || cen[t][2] < box[u][2] || cen[t][2] > box[u][5]) {
                continue;
            }
            const f32 cosang = nrm[t][0] * nrm[u][0] + nrm[t][1] * nrm[u][1] + nrm[t][2] * nrm[u][2];
            if (cosang < PROPDECAL_COS && cosang > -PROPDECAL_COS) {
                continue;
            }
            s32 flat = 1;
            for (s32 k = 0; k < 3 && flat; k++) {
                const f32 d = (tris[t].pos[k][0] - tris[u].pos[0][0]) * nrm[u][0]
                    + (tris[t].pos[k][1] - tris[u].pos[0][1]) * nrm[u][1] + (tris[t].pos[k][2] - tris[u].pos[0][2]) * nrm[u][2];
                flat = fabsf(d) <= tol;
            }
            if (flat && insideTri(tris[u].pos, nrm[u], cen[t], tol)) {
                base[t] = u;
            }
        }
    }
    for (s32 t = 0; t < num; t++) {
        const s32 u = base[t];
        if (u < 0) {
            continue;
        }
        const f32 out = (cen[u][0] - mid[0]) * nrm[u][0] + (cen[u][1] - mid[1]) * nrm[u][1] + (cen[u][2] - mid[2]) * nrm[u][2];
        if (fabsf(out) <= tol) {
            continue;   /* the base's plane runs through the middle: no outside */
        }
        const f32 s = out > 0.0f ? lift : -lift;
        for (s32 k = 0; k < 3; k++) {
            for (s32 a = 0; a < 3; a++) {
                tris[t].pos[k][a] += s * nrm[u][a];
            }
        }
        lifted++;
    }
out:
    free(nrm);
    free(cen);
    free(box);
    free(area);
    free(kind);
    free(base);
    return lifted;
}

static s32 shaderOnlyDropped(const char *file)
{
    return strcmp(file, "Psat1_reflectZ") == 0;
}

static void build(struct entry *e)
{
    const u64 t0 = sysGetMicroseconds();
    const struct proprow *row = &propRows[e->row];
    struct bnode nodes[MAXNODES];
    struct collect c;
    struct pic *pics = NULL;
    char path[1024];
    s32 underlod = 0, numcand = 0, dropped = 0, numpics = 0;
    s32 numdraws, numtex;
    f32 uvscale;
    f64 msOpen, msTris, msPics, msDeal, msWrite;
    u64 mark;

    e->state = -1;
    const s32 numnodes = collectNodes(e->hdr, nodes, MAXNODES, &underlod);
    /* round 26a (D-145): a first-person gun's lists are all the gun's, under
     * whatever switch, but its muzzle flash, which stays GoldenEye's own */
    const s32 gun = strncmp(row->source, "gun/", 4) == 0;
    s8 gcls[MAXNODES];
    for (s32 i = 0; gun && i < numnodes; i++) {
        gcls[i] = (s8)beanGunNodeClass(e->hdr, nodes[i].node);
        nodes[i].keep = gcls[i] == BEANGUN_NODE_FLASH;
        nodes[i].candidate = !nodes[i].keep && nodes[i].numverts > 0;
        nodes[i].switched = 0;
    }
    /* round 28b (D-176): the watch arm (Csuit_lf_handZ, the left hand's item
     * on the watch page). Its lists: the hand and the watch (plain, matrix
     * 0), the three watch hands (matrices 1..3, bondview2.c sets them from
     * the clock), the watch's screen (switch 3: GoldenEye's page stays) and
     * six sleeves (switches 4..9, bondviewSelectCuff: by Bond's outfit). The
     * release has the suit's only (char/suitlfhand: grey suit, white cuff):
     * its sleeve rides switch 7's list (CUFF_BLUE, the formal wear), every
     * other outfit keeps its own. */
    const s32 arm = strcmp(row->file, "Csuit_lf_handZ") == 0;
    s8 armsw[MAXNODES];
    for (s32 i = 0; arm && i < numnodes; i++) {
        s32 sw = -1;
        for (s32 k = 3; k <= 9 && k < e->hdr->numSwitches && sw < 0; k++) {
            if (e->hdr->Switches[k] && nodeUnder(nodes[i].node, e->hdr->Switches[k])) {
                sw = k;
            }
        }
        nodes[i].switched = 0;
        armsw[i] = (s8)sw;
        if (sw >= 0 && sw != ARM_SUIT_SLEEVE) {
            nodes[i].keep = 1;
            nodes[i].candidate = 0;
        } else {
            nodes[i].keep = 0;
            nodes[i].candidate = nodes[i].numverts > 0;
        }
    }
    for (s32 i = 0; i < numnodes; i++) {
        numcand += nodes[i].candidate;
    }
    if (numcand == 0) {
        sysLogPrintf(LOG_NOTE, "beanmodel: %s: no plain display list to draw on - GoldenEye's own", row->file);
        return;
    }
    if (!sourcePath(row->source, path, sizeof(path))) {
        if (logOn()) {
            sysLogPrintf(LOG_NOTE, "beanmodel: %s: %s not in the layer - GoldenEye's own", row->file, row->source);
        }
        return;
    }
    mark = sysGetMicroseconds();
    struct beanlevel *bm = beanModelOpen(path);
    msOpen = msSince(mark);
    if (!bm) {
        sysLogPrintf(LOG_WARNING, "beanmodel: %s: %s is not a rendergraph this reads", row->file, path);
        return;
    }
    beanModelInfo(bm, &numdraws, &numtex, &uvscale);

    mark = sysGetMicroseconds();
    memset(&c, 0, sizeof(c));
    c.row = row;
    /* round 35b (D-321), dev only: GE_BEANHELDUNIFORM=1 draws a per-axis
     * override row at its length scale on every axis, the hand (the model's
     * origin) kept where the per-axis placement puts it - the release's own
     * proportions, for the A/B */
    struct proprow uniform;
    if ((row->flags & PROPROW_OVERRIDE) && getenv("GE_BEANHELDUNIFORM") && *getenv("GE_BEANHELDUNIFORM") == '1') {
        uniform = *row;
        for (s32 a = 0; a < 3; a++) {
            uniform.centre[a] = row->centre[a] * row->scale[0] / row->scale[a];
            uniform.scale[a] = row->scale[0];
        }
        c.row = &uniform;
    }
    c.gun = gun;
    c.tint = beanGunTint(row->source);
    c.bm = bm;
    /* an odd permutation swaps two axes; each negative sign mirrors once:
     * a mirrored placement turns the triangles over */
    c.mirror = (row->perm[0] == 0) + (row->perm[1] == 1) + (row->perm[2] == 2) == 1;
    for (s32 k = 0; k < 3; k++) {
        c.mirror ^= row->sign[k] < 0;
    }
    u8 flash[FLASH_MAXDRAWS];
    memset(flash, 0, sizeof(flash));
    if (!gun && !strncmp(row->source, "prop/chr", 8)) {
        c.flashdraws = flashDraws(bm, flash, (row->flags & PROPROW_FLASH_AHEAD) != 0);
        c.flashdraw = flash;
        c.held = 1;
    }
    beanModelTriangles(bm, addTri, &c);
    msTris = msSince(mark);
    if (c.flashdraws) {
        sysLogPrintf(LOG_NOTE, "beanmodel: %s: %d flash-card draws (%d triangles) left out (D-247)", row->file, c.flashdraws, c.cards);
    }
    if ((row->flags & PROPROW_OVERRIDE) && logOn()) {
        sysLogPrintf(LOG_NOTE, "beanmodel: %s: held-gun override (D-321): scale %.5f %.5f %.5f centre %.2f %.2f %.2f%s", row->file,
                     row->scale[0], row->scale[1], row->scale[2], row->centre[0], row->centre[1], row->centre[2],
                     (row->flags & PROPROW_FLASH_AHEAD) ? ", flash ahead of the muzzle" : "");
    }
    if (c.heldclear || c.heldopaque) {
        sysLogPrintf(LOG_NOTE, "beanmodel: %s: held gun's glass: %d clear triangles left out, %d drawn opaque (D-283)", row->file,
                     c.heldclear, c.heldopaque);
    }
    if (c.num == 0) {
        beanLevelClose(bm);
        free(c.tris);
        sysLogPrintf(LOG_NOTE, "beanmodel: %s: %s has no triangles this reads - GoldenEye's own", row->file, row->source);
        return;
    }

    /* Round 25b (D-142), the coverage audit. A draw with no picture and no
     * colour of its own (plain layout, no material since its vertex shader,
     * every corner white) is coloured by the release's shader at run time:
     * the gate switch's two lamps, the modem box's screen (PD drops the same
     * vertices by hand, gebean.c beanVertexDrops). GoldenEye draws that look
     * itself, from a list C3 keeps (a switched lamp, a screen's programme):
     * such triangles over a kept list are left out and GoldenEye's shows.
     * Anything else unresolved - a picture the file does not hold, a
     * sphere-mapped draw, a placeholder with no GoldenEye list over it, a
     * picture that does not decode - leaves the whole prop GoldenEye's own,
     * never a white face. */
    s32 badpic = 0, sphere = 0, uncovered = 0, placeholders = 0;
    {
        struct beandrawcount dc;
        beanMeshCountDraws(bm, 1, &dc);
        pa.draws += dc.draws;
        pa.unreadable += dc.unreadable;
        for (s32 k = 0; k < BEANDRAW_KINDS; k++) {
            pa.kinds[k] += dc.kinds[k];
        }
        if (dc.unreadable) {
            sysLogPrintf(LOG_WARNING, "[xbla] unresolved: prop %s (%s): %d draw%s with a vertex buffer this reader cannot read "
                         "(missing from the HD mesh)", row->file, row->source, dc.unreadable, dc.unreadable == 1 ? "" : "s");
            pa.refs++;
        }
        f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (s32 i = 0; i < numnodes; i++) {
            for (s32 v = 0; v < nodes[i].numverts; v++) {
                const f32 p[3] = { nodes[i].verts[v].coord.x + nodes[i].origin[0], nodes[i].verts[v].coord.y + nodes[i].origin[1],
                                   nodes[i].verts[v].coord.z + nodes[i].origin[2] };
                for (s32 a = 0; a < 3; a++) {
                    lo[a] = p[a] < lo[a] ? p[a] : lo[a];
                    hi[a] = p[a] > hi[a] ? p[a] : hi[a];
                }
            }
        }
        f32 size = 0.0f;
        for (s32 a = 0; a < 3; a++) {
            size = hi[a] - lo[a] > size ? hi[a] - lo[a] : size;
        }
        const f32 margin = size * 0.05f > 1.0f ? size * 0.05f : 1.0f;
        s32 kept = 0;
        for (s32 t = 0; t < c.num; t++) {
            const struct mtri *tri = &c.tris[t];
            badpic += tri->kind == BEANDRAW_BADPIC;
            sphere += tri->kind == BEANDRAW_SPHEREMAP;
            if (tri->shader) {
                f32 m[3];
                s32 covered = 0;
                for (s32 a = 0; a < 3; a++) {
                    m[a] = (tri->pos[0][a] + tri->pos[1][a] + tri->pos[2][a]) / 3.0f;
                }
                for (s32 i = 0; i < numnodes && !covered; i++) {
                    f32 klo[3] = { 1e30f, 1e30f, 1e30f }, khi[3] = { -1e30f, -1e30f, -1e30f };
                    if (!nodes[i].keep || nodes[i].numverts <= 0) {
                        continue;
                    }
                    for (s32 v = 0; v < nodes[i].numverts; v++) {
                        const f32 p[3] = { nodes[i].verts[v].coord.x + nodes[i].origin[0],
                                           nodes[i].verts[v].coord.y + nodes[i].origin[1],
                                           nodes[i].verts[v].coord.z + nodes[i].origin[2] };
                        for (s32 a = 0; a < 3; a++) {
                            klo[a] = p[a] < klo[a] ? p[a] : klo[a];
                            khi[a] = p[a] > khi[a] ? p[a] : khi[a];
                        }
                    }
                    covered = 1;
                    for (s32 a = 0; a < 3; a++) {
                        covered &= m[a] >= klo[a] - margin && m[a] <= khi[a] + margin;
                    }
                }
                if (covered || shaderOnlyDropped(row->file)) {
                    placeholders++;
                    continue;
                }
                uncovered++;
            }
            c.tris[kept++] = *tri;
        }
        c.num = kept;
        pa.placeholders += placeholders;
        if (c.num == 0) {
            beanLevelClose(bm);
            free(c.tris);
            sysLogPrintf(LOG_NOTE, "beanmodel: %s: %s is placeholders only - GoldenEye's own", row->file, row->source);
            return;
        }
        if (placeholders) {
            sysLogPrintf(LOG_NOTE, "[xbla] prop %s (%s): %d shader-only placeholder triangles %s - left out", row->file,
                         row->source, placeholders,
                         shaderOnlyDropped(row->file) ? "(dropped by name, D-180)"
                                                      : "under GoldenEye's own switched/screen list, GoldenEye's draws there");
        }
        if (badpic) {
            sysLogPrintf(LOG_WARNING, "[xbla] unresolved: prop %s (%s): %d triangles name a picture the file does not hold",
                         row->file, row->source, badpic);
            pa.refs++;
        }
        if (sphere) {
            /* round 26a (D-146): drawn by texgen on their normals */
            sysLogPrintf(LOG_NOTE, "[xbla] prop %s (%s): %d triangles of a sphere-mapped draw - drawn by GoldenEye's "
                         "texgen", row->file, row->source, sphere);
            pa.spheretris += sphere;
        }
        if (uncovered) {
            sysLogPrintf(LOG_WARNING, "[xbla] unresolved: prop %s (%s): %d shader-only triangles (no picture, no colour of "
                         "their own) with no GoldenEye list over them", row->file, row->source, uncovered);
            pa.refs++;
        }
    }

    /* deal: each triangle votes for the candidate node whose own vertex is
     * nearest its middle (model space); a Bean bone's triangles (else a
     * draw's) go together to the node most of them voted for - a wheel's
     * bone to the wheel's node, the mudflap beside it (the body's bone) to
     * the body */
    mark = sysGetMicroseconds();
    s32 first = -1;
    s32 numgroups = 0;
    for (s32 i = 0; i < numnodes; i++) {
        if (nodes[i].candidate) {
            first = i;
            break;
        }
    }
    s32 numswitched = 0;
    for (s32 i = 0; i < numnodes; i++) {
        numswitched += nodes[i].switched;
    }
    s32 bodymtx = -1, offnode = 0, handtris = 0;
    if (arm) {
        numgroups = dealArm(bm, &c, nodes, numnodes, armsw);
    } else if (gun) {
        /* round 26a (D-145, beangun.c): moving parts whole on their own
         * matrix, the body's triangles to the nearest list under the body's */
        struct beangunnode *gn = calloc((size_t)numnodes, sizeof(*gn));
        struct beanguntri *gt = malloc(sizeof(*gt) * (size_t)c.num);
        s16 *dealt = malloc(sizeof(s16) * (size_t)c.num);
        if (gn && gt && dealt) {
            for (s32 i = 0; i < numnodes; i++) {
                gn[i].verts = nodes[i].verts;
                gn[i].numverts = nodes[i].candidate ? nodes[i].numverts : 0;
                memcpy(gn[i].origin, nodes[i].origin, sizeof(gn[i].origin));
                gn[i].mtx = modelFindNodeMtxIndex(nodes[i].node, 0);
                gn[i].cls = gcls[i];
            }
            for (s32 t = 0; t < c.num; t++) {
                for (s32 a = 0; a < 3; a++) {
                    gt[t].mid[a] = (c.tris[t].pos[0][a] + c.tris[t].pos[1][a] + c.tris[t].pos[2][a]) / 3.0f;
                }
                gt[t].group = c.tris[t].group < 256 ? c.tris[t].group : -1;
                int32_t tw = 0, th = 0;
                gt[t].glove = (s8)(c.tris[t].tex >= 0 && beanLevelTextureSize(bm, c.tris[t].tex, &tw, &th)
                                   && beanGunPictureIsGlove(tw, th));
            }
            numgroups = beanGunDeal(gt, c.num, gn, numnodes, dealt, &bodymtx);
            for (s32 t = 0; t < c.num; t++) {
                c.tris[t].node = dealt[t];
                offnode += dealt[t] < 0;
                handtris += dealt[t] >= 0 && gcls[dealt[t]] == BEANGUN_NODE_HAND;
            }
        } else {
            for (s32 t = 0; t < c.num; t++) {
                c.tris[t].node = (s16)first;
            }
        }
        if (logOn()) {
            for (s32 i = 0; i < numnodes; i++) {
                s32 n = 0;
                for (s32 t = 0; t < c.num; t++) {
                    n += c.tris[t].node == i;
                }
                sysLogPrintf(LOG_NOTE, "beanmodel: %s list %d: class %d, %d vertices, matrix %d, %d HD triangles",
                             row->file, i, gcls[i], nodes[i].numverts, gn ? gn[i].mtx : -1, n);
            }
            sysLogPrintf(LOG_NOTE, "beanmodel: %s: %d flash-card triangles left out, body matrix %d, %d triangles on no list",
                         row->file, c.cards, bodymtx, offnode);
            for (s32 p = 0; p < numtex && p < 16; p++) {
                s32 n[3] = { 0, 0, 0 };
                int32_t tw = 0, th = 0;
                beanLevelTextureSize(bm, p, &tw, &th);
                for (s32 t = 0; t < c.num; t++) {
                    if (c.tris[t].tex == p && c.tris[t].node >= 0) {
                        n[gcls[c.tris[t].node] & 1]++;
                    }
                }
                sysLogPrintf(LOG_NOTE, "beanmodel: %s picture %d (%dx%d): %d triangles on gun lists, %d on hand lists",
                             row->file, p, (int)tw, (int)th, n[0], n[1]);
            }
        }
        free(gn);
        free(gt);
        free(dealt);
        /* fail open: a gun whose HD file holds no hand keeps GoldenEye's */
        for (s32 i = 0; handtris == 0 && i < numnodes; i++) {
            if (gcls[i] == BEANGUN_NODE_HAND) {
                nodes[i].candidate = 0;
                nodes[i].keep = 1;
            }
        }
    } else if (numcand > 1 || numswitched > 0) {
        const s32 ng = 256 + 0x4000;
        s32 *votes = calloc((size_t)ng * (size_t)numnodes, sizeof(s32));
        s16 *nearAll = malloc(sizeof(s16) * (size_t)c.num);
        for (s32 t = 0; t < c.num; t++) {
            struct mtri *tri = &c.tris[t];
            f32 m[3];
            for (s32 a = 0; a < 3; a++) {
                m[a] = (tri->pos[0][a] + tri->pos[1][a] + tri->pos[2][a]) / 3.0f;
            }
            f32 best = 1e30f, bestall = 1e30f;
            s32 bi = first, ball = first;
            for (s32 i = 0; i < numnodes; i++) {
                if (!nodes[i].candidate && !nodes[i].switched) {
                    continue;
                }
                for (s32 v = 0; v < nodes[i].numverts; v++) {
                    const f32 dx = nodes[i].verts[v].coord.x + nodes[i].origin[0] - m[0];
                    const f32 dy = nodes[i].verts[v].coord.y + nodes[i].origin[1] - m[1];
                    const f32 dz = nodes[i].verts[v].coord.z + nodes[i].origin[2] - m[2];
                    const f32 d = dx * dx + dy * dy + dz * dz;
                    if (nodes[i].candidate && d < best) {
                        best = d;
                        bi = i;
                    }
                    if (d < bestall) {
                        bestall = d;
                        ball = i;
                    }
                }
            }
            tri->node = (s16)bi;
            if (nearAll) {
                nearAll[t] = (s16)ball;
            }
            if (votes) {
                votes[(size_t)tri->group * (size_t)numnodes + (size_t)bi]++;
            }
        }
        for (s32 g = 0; votes && g < ng; g++) {
            s32 bestn = -1, bestv = 0;
            for (s32 i = 0; i < numnodes; i++) {
                if (votes[(size_t)g * (size_t)numnodes + (size_t)i] > bestv) {
                    bestv = votes[(size_t)g * (size_t)numnodes + (size_t)i];
                    bestn = i;
                }
            }
            if (bestn < 0) {
                continue;
            }
            numgroups++;
            for (s32 t = 0; t < c.num; t++) {
                if (c.tris[t].group == g) {
                    c.tris[t].node = (s16)bestn;
                }
            }
        }
        for (s32 t = 0; nearAll && t < c.num; t++) {
            if (nodes[nearAll[t]].switched) {
                c.tris[t].node = nearAll[t];
            }
        }
        free(nearAll);
        free(votes);
    } else {
        for (s32 t = 0; t < c.num; t++) {
            c.tris[t].node = (s16)first;
        }
    }
    msDeal = msSince(mark);

    /* the overlay check (D-117): the HD mesh's box against GoldenEye's own
     * (the candidate lists' vertices, model space) - a prop that floats or
     * sinks shows as a bottom offset; logged in model units */
    f32 nlo[3] = { 1e30f, 1e30f, 1e30f }, nhi[3] = { -1e30f, -1e30f, -1e30f };
    f32 blo[3] = { 1e30f, 1e30f, 1e30f }, bhi[3] = { -1e30f, -1e30f, -1e30f };
    for (s32 i = 0; i < numnodes; i++) {
        for (s32 v = 0; nodes[i].candidate && v < nodes[i].numverts; v++) {
            const f32 p[3] = { nodes[i].verts[v].coord.x + nodes[i].origin[0], nodes[i].verts[v].coord.y + nodes[i].origin[1],
                               nodes[i].verts[v].coord.z + nodes[i].origin[2] };
            for (s32 a = 0; a < 3; a++) {
                nlo[a] = p[a] < nlo[a] ? p[a] : nlo[a];
                nhi[a] = p[a] > nhi[a] ? p[a] : nhi[a];
            }
        }
    }
    for (s32 t = 0; t < c.num; t++) {
        for (s32 k = 0; k < 3; k++) {
            for (s32 a = 0; a < 3; a++) {
                blo[a] = c.tris[t].pos[k][a] < blo[a] ? c.tris[t].pos[k][a] : blo[a];
                bhi[a] = c.tris[t].pos[k][a] > bhi[a] ? c.tris[t].pos[k][a] : bhi[a];
            }
        }
    }
    {
        f32 size = 0.0f;
        for (s32 a = 0; a < 3; a++) {
            size = nhi[a] - nlo[a] > size ? nhi[a] - nlo[a] : size;
        }
        sysLogPrintf(LOG_NOTE, "beanmodel: %s fit: HD box vs GoldenEye's - bottom %+.1f, top %+.1f, centre %+.1f %+.1f %+.1f "
                     "(model units; size %.0f, %.2f%%)",
                     row->file, blo[1] - nlo[1], bhi[1] - nhi[1], (blo[0] + bhi[0] - nlo[0] - nhi[0]) / 2,
                     (blo[1] + bhi[1] - nlo[1] - nhi[1]) / 2, (blo[2] + bhi[2] - nlo[2] - nhi[2]) / 2, size,
                     size > 0.0f ? 100.0f * (blo[1] - nlo[1]) / size : 0.0f);
    }

    /* pictures: each one a triangle uses, decoded and bound to a stand-in */
    mark = sysGetMicroseconds();
    pics = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*pics));
    e->standins = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*e->standins));
    if (!pics || !e->standins) {
        free(pics);
        free(e->standins);
        e->standins = NULL;
        free(c.tris);
        beanLevelClose(bm);
        return;
    }
    for (s32 t = 0; t < c.num; t++) {
        if (c.tris[t].tex >= 0 && c.tris[t].tex < numtex) {
            pics[c.tris[t].tex].used = 1;
        }
    }
    s32 failedpics = 0;
    for (s32 p = 0; p < numtex; p++) {
        int32_t w, h;
        if (!pics[p].used) {
            continue;
        }
        pa.pictures++;
        uint8_t *rgba = beanLevelDecodeTexture(bm, p, &w, &h);
        const u32 total = rgba ? (u32)w * (u32)h : 0;
        u32 opaque = 0, rs = 0, gs = 0, bs = 0;
        for (u32 i = 0; i < total; i++) {
            opaque += rgba[i * 4 + 3] >= OPAQUE_ALPHA;
            rs += rgba[i * 4];
            gs += rgba[i * 4 + 1];
            bs += rgba[i * 4 + 2];
        }
        u16 *tile = newTile(total ? texel5551(rs / total, gs / total, bs / total) : 0xffff);
        {
            /* dev (GE_BEANHEADDUMP=<dir>, round 31c): the decoded picture, RGB PPM + alpha PGM, and
             * every triangle drawn with it (corner colours, glass flag) */
            const char *dd = getenv("GE_BEANHEADDUMP");
            if (dd && *dd && rgba) {
                char dp[1024];
                snprintf(dp, sizeof(dp), "%s/%s_pic%d_%dx%d.ppm", dd, row->file, (int)p, (int)w, (int)h);
                FILE *f = fopen(dp, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h);
                    for (u32 i = 0; i < total; i++) {
                        fwrite(&rgba[i * 4], 1, 3, f);
                    }
                    fclose(f);
                }
                snprintf(dp, sizeof(dp), "%s/%s_pic%d_alpha.pgm", dd, row->file, (int)p);
                f = fopen(dp, "wb");
                if (f) {
                    fprintf(f, "P5\n%d %d\n255\n", (int)w, (int)h);
                    for (u32 i = 0; i < total; i++) {
                        fwrite(&rgba[i * 4 + 3], 1, 1, f);
                    }
                    fclose(f);
                }
                snprintf(dp, sizeof(dp), "%s/%s_pic%d_tris.txt", dd, row->file, (int)p);
                f = fopen(dp, "w");
                for (s32 t = 0; f && t < c.num; t++) {
                    if (c.tris[t].tex == p) {
                        fprintf(f, "glass %d argb %08x %08x %08x uv %.4f %.4f %.4f %.4f %.4f %.4f\n", c.tris[t].glass,
                                (unsigned)c.tris[t].argb[0], (unsigned)c.tris[t].argb[1], (unsigned)c.tris[t].argb[2],
                                c.tris[t].uv[0][0], c.tris[t].uv[0][1], c.tris[t].uv[1][0], c.tris[t].uv[1][1],
                                c.tris[t].uv[2][0], c.tris[t].uv[2][1]);
                    }
                }
                if (f) {
                    fclose(f);
                }
            }
        }
        if (!rgba) {
            int32_t sw = 0, sh = 0;
            beanLevelTextureSize(bm, p, &sw, &sh);
            sysLogPrintf(LOG_WARNING, "[xbla] unresolved: prop %s (%s): picture %d (%dx%d) does not decode", row->file,
                         row->source, p, (int)sw, (int)sh);
            pa.refs++;
            failedpics++;
        }
        if (!tile || !rgba) {
            free(tile);
            texpackBeanPictureRelease(rgba);   /* D-317: it may be pinned (decoded ahead) */
            pics[p].used = 0;
            continue;
        }
        pics[p].standin = (s16)e->numstandins;
        e->standins[e->numstandins++] = tile;
        pics[p].alpha = opaque < total;
        pics[p].soft = (u64)opaque * 100 < (u64)total * SOFT_PERCENT;
        e->picbytes += total * 4;
        numpics++;
        texpackRegisterBeanPicture(tile, rgba, w, h);
    }
    msPics = msSince(mark);
    pa.decoded += numpics;
    if (badpic || uncovered || failedpics) {
        /* the stand-ins stay with the entry (the registry pins pictures to
         * their addresses until the level's texpackForgetBeanPictures) */
        pa.unresolved++;
        sysLogPrintf(LOG_WARNING, "[xbla] prop %s (%s): unresolved references - drawn GoldenEye's own", row->file,
                     row->source);
        free(pics);
        free(c.tris);
        beanLevelClose(bm);
        return;
    }

    /* Round 36 (doc 48): decals lifted off their base. */
    const s32 numlifted = liftDecals(c.tris, c.num, pics, numtex, blo, bhi);
    if (numlifted > 0 && logOn()) {
        sysLogPrintf(LOG_NOTE, "beanmodel: %s: %d decal triangles lifted off their base", row->file, numlifted);
    }

    /* the lists, node by node: solid, cut-outs, translucent */
    mark = sysGetMicroseconds();
    s32 *list = malloc(sizeof(s32) * (size_t)c.num);
    s32 numsolid = 0, numcut = 0, numxlu = 0, numdrawn = 0;
    e->nodes = calloc((size_t)numnodes, sizeof(*e->nodes));
    if (!list || !e->nodes) {
        free(list);
        free(pics);
        free(c.tris);
        beanLevelClose(bm);
        return;
    }
    memcpy(e->nodes, nodes, sizeof(*nodes) * (size_t)numnodes);
    e->numnodes = numnodes;
    for (s32 i = 0; i < numnodes; i++) {
        struct bnode *b = &e->nodes[i];
        struct vtxbuf vb;
        memset(&vb, 0, sizeof(vb));
        for (s32 kind = 0; kind < 3; kind++) {
            s32 n = 0;
            for (s32 t = 0; t < c.num; t++) {
                const struct mtri *tri = &c.tris[t];
                if (tri->node != i) {
                    continue;
                }
                const s32 tex = tri->tex >= 0 && pics[tri->tex].used ? tri->tex : -1;
                const s32 xlu = tri->glass || (tex >= 0 && pics[tex].soft);
                const s32 cut = !xlu && tex >= 0 && pics[tex].alpha;
                if ((kind == 0 && !xlu && !cut) || (kind == 1 && cut) || (kind == 2 && xlu)) {
                    list[n++] = t;
                }
            }
            if (kind == 0) numsolid += n;
            if (kind == 1) numcut += n;
            if (kind == 2) numxlu += n;
            b->numtris += n;
            for (s32 t = 0; t < n; t++) {
                if (c.tris[list[t]].tex >= 0 && !pics[c.tris[list[t]].tex].used) {
                    c.tris[list[t]].tex = -1;
                }
            }
            /* the watch arm: far to near (1); its sleeve layer by layer (2) */
            const s32 armview = !arm ? 0 : armsw[i] == ARM_SUIT_SLEEVE ? 2 : 1;
            Gfx *g = writeList(&vb, c.tris, list, n, b->origin, pics, e->standins, kind != 0, &dropped, &e->meshbytes,
                               armview, numtex);
            if (kind == 0) b->out.solid = g;
            if (kind == 1) b->out.cut = g;
            if (kind == 2) b->out.xlu = g;
        }
        b->out.vtx = vb.v;
        e->meshbytes += (u32)vb.cap * (u32)sizeof(Vtx);
        numdrawn += b->numtris > 0;
    }
    msWrite = msSince(mark);
    e->meshbytes += (u32)e->numstandins * TILE * TILE * sizeof(u16);

    free(list);
    free(pics);
    free(c.tris);
    beanLevelClose(bm);
    e->state = 1;
    builtCount++;
    pa.models++;
    meshBytes += e->meshbytes;
    picBytes += e->picbytes;
    s32 numkept = 0, numunread = 0;
    for (s32 i = 0; i < numnodes; i++) {
        numunread += !e->nodes[i].candidate && !e->nodes[i].keep;
    }
    if (numunread && logOn()) {
        sysLogPrintf(LOG_NOTE, "beanmodel: %s: %d display list%s with no readable vertices and no switch - drawn GoldenEye's own",
                     row->file, numunread, numunread == 1 ? "" : "s");
    }
    for (s32 i = 0; i < numnodes; i++) {
        numkept += e->nodes[i].numtris == 0 && (e->nodes[i].keep || !e->nodes[i].candidate);
    }
    sysLogPrintf(LOG_NOTE, "beanmodel: %s <- %s: %d triangles (%d solid, %d cut-out, %d translucent; %d glass, %d black vertices made white, "
                 "%d dropped) on %d of %d lists (%d groups dealt; %d kept GoldenEye's%s), %d pictures %u KB, mesh %u KB, uv 1/%.0f; "
                 "%.2f ms (open %.2f, tris %.2f, deal %.2f, pictures %.2f, write %.2f)",
                 row->file, row->source, c.num, numsolid, numcut, numxlu, c.glass, c.black, dropped, numdrawn, numnodes,
                 numgroups, numkept,
                 underlod ? "; far level of detail left GoldenEye's" : "", numpics, e->picbytes / 1024, e->meshbytes / 1024,
                 uvscale, msSince(t0), msOpen, msTris, msDeal, msPics, msWrite);
}

/* round 35a: game-thread HD build time (props, guns, characters), summed;
 * GE_FRAMEPROF's `build_us=` prints it per frame */
volatile u64 g_beanBuildUs;

static struct entry *entryFor(ModelFileHeader *hdr)
{
    static struct entry *last;
    /* round 26a: a player's hand header holds whichever gun is in that hand,
     * refilled in place - an entry is per header, root node and file */
    const char *gunfile = beanGunFile(hdr);
    if (last && last >= entries && last < entries + numEntries && last->hdr == hdr && last->root == hdr->RootNode
            && last->gunfile == gunfile) {
        return last;
    }
    for (s32 i = 0; i < numEntries; i++) {
        if (entries[i].hdr == hdr && entries[i].root == hdr->RootNode && entries[i].gunfile == gunfile) {
            return last = &entries[i];
        }
    }
    if (numEntries >= capEntries) {
        const s32 cap = capEntries ? capEntries * 2 : 64;
        struct entry *n = realloc(entries, sizeof(*n) * (size_t)cap);
        if (!n) {
            return NULL;
        }
        entries = n;
        capEntries = cap;
        last = NULL;
    }
    struct entry *e = &entries[numEntries++];
    memset(e, 0, sizeof(*e));
    e->hdr = hdr;
    e->root = hdr->RootNode;
    e->gunfile = gunfile;
    e->row = (s16)rowOf(gunfile ? gunfile : fileOf(hdr));
    if (e->row < 0 || !onlyAllows(propRows[e->row].file)) {
        /* round 26a (D-147): the coverage line counts guns too */
        if (gunfile && e->row < 0 && beanStageModelsEnabled()) {
            sysLogPrintf(LOG_NOTE, "[xbla] gun %s: no XBLA gun mapped (beanguntable.h) - GoldenEye's own", gunfile);
            pa.gunsN64++;
        }
        e->state = -1;
        e->row = -1;
    }
    return last = e;
}

static void ensureBuilt(struct entry *e)
{
    if (e->state != 0) {
        return;
    }
    if (!whiteTile) {
        whiteTile = newTile(0xffff);
    }
    const u64 tb = sysGetMicroseconds();
    build(e);
    g_beanBuildUs += sysGetMicroseconds() - tb;
    if (e->state < 0) {
        fellbackCount++;
    }
    /* round 26a (D-147): one always-on line per first-person gun built
     * (engine.log on the phone), counted for the coverage report */
    if (e->gunfile && e->row >= 0) {
        if (e->state == 1) {
            pa.gunsHD++;
        } else {
            pa.gunsN64++;
        }
        sysLogPrintf(LOG_NOTE, "[xbla] gun %s (%s): %s; this level %d gun%s HD, %d GoldenEye's own", e->gunfile,
                     propRows[e->row].source, e->state == 1 ? "HD" : "GoldenEye's own (see the beanmodel/[xbla] lines above)",
                     pa.gunsHD, pa.gunsHD == 1 ? "" : "s", pa.gunsN64);
    }
}

/* ------------------------------------------------------------- public */

s32 beanModelNode(struct ModelFileHeader *hdr, struct ModelNode *node, const struct beanmodelnode **out)
{
    if (!hdr || envOff() || !beanStageModelsEnabled()) {
        return BEANMODEL_N64;
    }
    struct entry *e = entryFor(hdr);
    if (!e || e->state < 0) {
        return BEANMODEL_N64;
    }
    ensureBuilt(e);
    if (e->state != 1) {
        return BEANMODEL_N64;
    }
    for (s32 i = 0; i < e->numnodes; i++) {
        const struct bnode *b = &e->nodes[i];
        if (b->node != node) {
            continue;
        }
        if (b->numtris == 0) {
            /* SKIP only a plain list the HD mesh was dealt over; anything
             * else (switched, far LOD, or a list whose vertices this could
             * not read) fails open to GoldenEye's own list */
            return b->candidate && !b->keep ? BEANMODEL_SKIP : BEANMODEL_N64;
        }
        *out = &b->out;
        return BEANMODEL_DRAW;
    }
    return BEANMODEL_N64;
}

/* Round 39 (doc 52, beanmodel.h): the first-person gun pass draws its HD
 * lists with the depth buffer, cleared before the first of them. */
static s32 gunPass, gunPassClear;

void beanModelGunPass(s32 on)
{
    static s32 off = -1;
    if (off < 0) {
        const char *e = getenv("GE_BEANGUNDEPTH");
        off = e && e[0] == '0';
    }
    gunPass = on && !off;
    gunPassClear = gunPass;
}

s32 beanModelGunDepth(void)
{
    if (!gunPass) {
        return 0;
    }
    if (gunPassClear) {
        gunPassClear = 0;
        return 2;
    }
    return 1;
}

void beanModelLevelLoaded(void)
{
    /* The last frame presented may still reference the previous level's lists
     * (fast3d's camera-latch rerun, docs/tier-c.md "Lifetime"): free the set
     * retired one level ago, retire this one. Pictures are forgotten by
     * beanStageLevelLoaded() (texpackForgetBeanPictures), which runs first. */
    for (s32 i = 0; i < numRetired; i++) {
        freeEntry(&retired[i]);
    }
    free(retired);
    retired = entries;
    numRetired = numEntries;
    entries = NULL;
    numEntries = capEntries = 0;
    builtCount = fellbackCount = 0;
    meshBytes = picBytes = 0;
    prebuilt = 0;
    memset(&pa, 0, sizeof(pa));
    summarised = 0;
}

/* ------------------------------------------------- coverage (round 25b) */

static void countTri(void *arg, int tex, const struct beanmodelvtx *v)
{
    s32 *n = arg; /* {tris, badpic, sphere, shader, textured per picture...} */
    (void)tex;
    n[0]++;
    n[1] += v[0].kind == BEANDRAW_BADPIC;
    n[2] += v[0].kind == BEANDRAW_SPHEREMAP;
    n[3] += v[0].kind == BEANDRAW_COLOUR && !v[0].ownmat && v[0].argb == 0xffffffffu && v[1].argb == 0xffffffffu
        && v[2].argb == 0xffffffffu;
}

/* GE_XBLA_AUDIT=1 (scripts/xbla-coverage.sh): every row of the prop table,
 * live in this level or not - its file, its draws, every picture a triangle
 * uses decoded. Nothing is built or drawn. A shader-only placeholder is
 * listed for information: whether GoldenEye keeps a list over it is known
 * only once the prop's own model is loaded (the live build checks it). */
static void auditAllProps(void)
{
    const u64 t0 = sysGetMicroseconds();
    s32 present = 0, models = 0, draws = 0, kinds[BEANDRAW_KINDS] = { 0 }, unreadable = 0;
    s32 pictures = 0, failed = 0, badModels = 0, placeholderModels = 0, sphereModels = 0;
    for (s32 i = 0; i < NUM_PROPROWS; i++) {
        char path[1024];
        s32 dup = 0;
        for (s32 j = 0; j < i && !dup; j++) {
            dup = strcmp(propRows[j].source, propRows[i].source) == 0;
        }
        if (dup) {
            continue;
        }
        models++;
        if (!sourcePath(propRows[i].source, path, sizeof(path))) {
            sysLogPrintf(LOG_WARNING, "[xbla] audit: prop %s (%s): file not in the imported layer", propRows[i].file,
                         propRows[i].source);
            badModels++;
            continue;
        }
        present++;
        struct beanlevel *bm = beanModelOpenRaw(path);   /* never a prefetched handle (D-317) */
        if (!bm) {
            sysLogPrintf(LOG_WARNING, "[xbla] audit: prop %s (%s): not a rendergraph this reads", propRows[i].file,
                         propRows[i].source);
            badModels++;
            continue;
        }
        struct beandrawcount dc;
        s32 n[4] = { 0, 0, 0, 0 };
        s32 nd, nt;
        f32 uvs;
        beanMeshCountDraws(bm, 1, &dc);
        beanModelInfo(bm, &nd, &nt, &uvs);
        draws += dc.draws;
        unreadable += dc.unreadable;
        for (s32 k = 0; k < BEANDRAW_KINDS; k++) {
            kinds[k] += dc.kinds[k];
        }
        beanModelTriangles(bm, countTri, n);
        /* every picture the file holds, decoded once */
        s32 mfailed = 0;
        for (s32 p = 0; p < nt; p++) {
            int32_t w, h;
            uint8_t *rgba = beanLevelDecodeTexture(bm, p, &w, &h);
            pictures++;
            if (!rgba) {
                int32_t sw = 0, sh = 0;
                beanLevelTextureSize(bm, p, &sw, &sh);
                sysLogPrintf(LOG_WARNING, "[xbla] audit: prop %s (%s): picture %d (%dx%d) does not decode",
                             propRows[i].file, propRows[i].source, p, (int)sw, (int)sh);
                mfailed++;
            }
            free(rgba);
        }
        failed += mfailed;
        if (n[1] || dc.unreadable) {
            sysLogPrintf(LOG_WARNING, "[xbla] audit: prop %s (%s): %d triangles name a missing picture, "
                         "%d draws unreadable", propRows[i].file, propRows[i].source, n[1], dc.unreadable);
        }
        if (n[2]) {
            /* round 26a (D-146): drawn by texgen, no longer a fallback */
            sysLogPrintf(LOG_NOTE, "[xbla] audit: prop %s (%s): %d sphere-mapped triangles (drawn by texgen)",
                         propRows[i].file, propRows[i].source, n[2]);
            sphereModels++;
        }
        if (n[3]) {
            sysLogPrintf(LOG_NOTE, "[xbla] audit: prop %s (%s): %d shader-only placeholder triangles (needs a GoldenEye "
                         "list over them, else the prop falls back)", propRows[i].file, propRows[i].source, n[3]);
            placeholderModels++;
        }
        badModels += n[1] || dc.unreadable || mfailed;
        beanLevelClose(bm);
    }
    sysLogPrintf(LOG_NOTE, "[xbla] audit props: %d table rows, %d model files (%d in the layer), %d draws (%d picture, "
                 "%d colour, %d sphere-mapped, %d missing picture, %d unreadable), %d pictures decoded of %d, "
                 "%d models with an unresolved reference, %d with shader-only placeholders, %d with sphere-mapped draws "
                 "(texgen); %.0f ms",
                 NUM_PROPROWS, models, present, draws, kinds[BEANDRAW_PICTURE], kinds[BEANDRAW_COLOUR],
                 kinds[BEANDRAW_SPHEREMAP], kinds[BEANDRAW_BADPIC], unreadable, pictures - failed, pictures, badModels,
                 placeholderModels, sphereModels, msSince(t0));
}

/* The one [xbla] line per stage: once rooms and the level's live props are
 * built (Settings' engine.log carries it from the phone). */
static void maybeSummarise(void)
{
    struct beanstageaudit sa;
    if (summarised || !beanStageModelsEnabled() || !(prebuilt || envOff()) || !beanStageAudit(&sa)) {
        return;
    }
    summarised = 1;
    char rooms[512];
    if (sa.state == 1) {
        snprintf(rooms, sizeof(rooms), "rooms %d of %d served (%d left N64 for an unresolved picture), %d draws "
                 "(%d picture, %d colour, %d missing picture, %d unreadable), pictures %d of %d resolved, "
                 "%d shader-only room triangles drawn, %d dropped as decals",
                 sa.served, sa.rooms, sa.unresolvedRooms, sa.draws, sa.pictureDraws, sa.colourDraws, sa.badpicDraws,
                 sa.unreadableDraws, sa.decoded, sa.pictures, sa.shaderTris, sa.shaderDecals);
    } else {
        snprintf(rooms, sizeof(rooms), "rooms N64 (%s)", sa.why ? sa.why : "not built");
    }
    /* round 26b: the characters' part (beanchr.c, built before this line) */
    char chrs[512];
    s32 chrunres = 0;
    beanChrDescribe(chrs, sizeof(chrs), &chrunres);
    const s32 unresolved = (sa.state == 1 ? sa.unresolvedRooms + sa.badpicDraws + sa.unreadableDraws
                                              + (sa.pictures - sa.decoded) : 0) + pa.refs + chrunres;
    sysLogPrintf(LOG_NOTE, "[xbla] stage %s%s%s: %s; props %s%d HD, %d GoldenEye's for an unresolved reference, %d draws "
                 "(%d picture, %d colour, %d sphere-mapped, %d missing picture, %d unreadable), pictures %d of %d resolved, "
                 "%d shader-only placeholders left to GoldenEye's own list; %s; %d unresolved reference%s",
                 sa.key[0] ? sa.key : "?", sa.bean[0] ? " -> " : "", sa.bean, rooms, envOff() ? "off (GE_BEANPROPS=0) " : "",
                 pa.models, pa.unresolved, pa.draws, pa.kinds[BEANDRAW_PICTURE], pa.kinds[BEANDRAW_COLOUR],
                 pa.kinds[BEANDRAW_SPHEREMAP], pa.kinds[BEANDRAW_BADPIC], pa.unreadable, pa.decoded, pa.pictures,
                 pa.placeholders, chrs, unresolved, unresolved == 1 ? "" : "s");
    if (getenv("GE_XBLA_AUDIT") && *getenv("GE_XBLA_AUDIT") == '1') {
        auditAllProps();
    }
}

void beanModelFrame(void)
{
    beanPrefetchFrame();   /* round 35a (D-317) */
    /* dev: GE_BEANPROPLOG=2 also logs the moving props (vehicles) every 100 frames */
    static u32 frames;
    if (logOn() > 1 && (frames++ % 100) == 0) {
        s32 n = 0;
        for (PropRecord *pr = g_ActivePropsHead; pr && n < 4096; pr = pr->next, n++) {
            if (pr->type == PROP_TYPE_OBJ && pr->obj && pr->obj->model && pr->obj->model->obj
                    && pr->obj->model->obj->numMatrices > 1) {
                const char *f = fileOf(pr->obj->model->obj);
                sysLogPrintf(LOG_NOTE, "beanmodel: frame %u %s at %.0f,%.0f,%.0f", frames - 1, f ? f : "?",
                             pr->pos.x, pr->pos.y, pr->pos.z);
            }
        }
    }
    maybeSummarise();
    if (prebuilt || envOff() || !beanStageModelsEnabled() || !g_ActivePropsHead) {
        return;
    }
    prebuilt = 1;
    const u64 t0 = sysGetMicroseconds();
    s32 n = 0;
    for (PropRecord *pr = g_ActivePropsHead; pr && n < 4096; pr = pr->next, n++) {
        if ((pr->type == PROP_TYPE_OBJ || pr->type == PROP_TYPE_DOOR || pr->type == PROP_TYPE_WEAPON) && pr->obj
                && pr->obj->model && pr->obj->model->obj) {
            struct entry *e = entryFor(pr->obj->model->obj);
            if (e) {
                ensureBuilt(e);
            }
            if (logOn() > 1) {
                const char *f = fileOf(pr->obj->model->obj);
                sysLogPrintf(LOG_NOTE, "beanmodel: prop type %d %s at %.0f,%.0f,%.0f room %d", pr->type, f ? f : "?",
                             pr->pos.x, pr->pos.y, pr->pos.z, pr->rooms[0]);
            }
        }
    }
    sysLogPrintf(LOG_NOTE, "beanmodel: level props: %d models built, %d GoldenEye's own (of %d seen, %d live props); "
                 "%.1f ms; mesh %u KB, pictures %u KB",
                 builtCount, numEntries - builtCount, numEntries, n, msSince(t0), meshBytes / 1024, picBytes / 1024);
    maybeSummarise();
    if (logOn()) {
        for (s32 i = 0; i < numEntries; i++) {
            const char *f = fileOf(entries[i].hdr);
            sysLogPrintf(LOG_NOTE, "beanmodel:   %s %s", f ? f : "?",
                         entries[i].state == 1 ? "HD" : entries[i].row >= 0 ? "mapped, GoldenEye's own" : "not mapped");
        }
    }
}

void beanModelMemory(u32 *meshes, u32 *pictures, s32 *built, s32 *fellback)
{
    if (meshes) *meshes = meshBytes;
    if (pictures) *pictures = picBytes;
    if (built) *built = builtCount;
    if (fellback) *fellback = fellbackCount;
}
