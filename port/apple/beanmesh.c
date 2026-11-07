/* Imported from GoldenEye-Decomp-ios engine/port/src/beanmesh.c (first 93988236, last changed 47ef757f; donor HEAD 55a3a9c8),
 * round 13, XBLA step 4.1 (docs/port-ios/22-round13-rooms.md). 64-bit host layer only.
 * Changes against the donor are marked "Midas round 13". */
/*
 * GoldenEye XBLA rendergraphs as triangles — see port/include/beanmesh.h.
 * Tier C, round C1 (DECISIONS D-110, docs/tier-c.md).
 *
 * Ported from Perfect Dark's gebean.c (perfect-dark-ios vendor/dabs-mod
 * port/src/gebean.c, MIT): the function names in the comments are PD's. Only
 * what a LEVEL needs is here; the skinned-character readers (palettes, bones,
 * stride 28/36 skinning) are C3's.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "beantex.h"
#include "beanmesh.h"
#include "x360.h"

/* the N64 <math.h> on the include path shadows the host's and has no atan2f */
float atan2f(float y, float x);
float fabsf(float x);

#define BEAN_MAXDRAWS 4096
#define BEAN_MAXINSTS 256
#define BEAN_MAXIBS 4096
#define BEAN_MAXTREEVBS 32
#define BEAN_MAXHOPS 65536
#define BEAN_MAXREMAP 64

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct beandraw {
    uint32_t vb;
    uint32_t tex;
    uint32_t prim;
    uint32_t count;
    uint32_t ib;
    int16_t inst;      /* the instance matrix it is drawn under, or -1 */
    uint8_t blend;     /* in the release's blended pass */
    uint8_t alpha;     /* its material colour's alpha, 255 for none */
    uint32_t colour;   /* and that colour, RGB, white for none */
    uint8_t alphatest; /* render state 0x60 */
    uint8_t alpharef;  /* render state 0x64 */
    uint8_t ownmat;    /* a material set since its vertex shader was (0x02) */
    int8_t piece;      /* a 0x30 draw's piece (C3: a model keeps 0 and 1), -1 for 0x01 */
    uint8_t numpal;    /* C3: the bone palette (0x13) its skinned vertices' slots index */
    uint8_t pal[64];   /* round 26b: 64 (PD BEAN_MAXPAL) for the characters */
};

struct beanib {
    uint32_t obj;
    uint32_t off;
    uint32_t size;
    uint32_t width; /* bytes an index */
};

struct beanlevel {
    uint8_t *file;
    uint32_t filelen;
    struct beanfile *bf;
    const uint8_t *data;
    uint32_t datalen;
    const uint8_t *gpu;
    uint32_t gpulen;
    const uint8_t *stream;
    uint32_t streamlen;
    int numdraws;
    struct beandraw *draws;
    int numinsts;
    float (*insts)[16];
    int numibs;
    struct beanib ibs[BEAN_MAXIBS];
    int numtex;
    float uvscale;
    int lit128, lit256;
    float (*texscroll)[2]; /* numtex: repeats a second (texAnimation()) */
    uint8_t *glasspane;    /* numtex: round 29c (D-228) - a pane under a shared glass map, in the blended pass */
    int uv20;              /* round 26a: a model's stride-20 vertices carry a UV, not a colour */
    /* round 26b (characters): the palette remap (0x12) - a palette number
     * to a pose bone (PD bm->remap) */
    int numremap;
    uint16_t remap[BEAN_MAXREMAP];
    /* round 35a (D-317): pictures decoded ahead by beanprefetch.c, numtex
     * long; NULL entries are decoded as usual. Not this handle's: the
     * pixels are pinned in texpack's registry (beanLevelClose frees the
     * arrays only). */
    uint8_t **pre;
    int32_t *prew, *preh;
};

struct beanvb {
    uint32_t stride;
    uint32_t off;
    uint32_t count;
    int col28; /* stride 28 carries a colour rather than a UV */
};

struct beanvtx {
    float pos[3];
    float uv[2];
    uint32_t argb;
    int slot;          /* a skinned model vertex's first palette slot, -1 for none */
    float nrm[3];      /* round 26a: the 10:10:10 normal (models; PD beanUnpackNormal) */
};

static int fits(uint64_t ofs, uint64_t len, uint64_t size)
{
    return ofs <= size && len <= size - ofs;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static float bef32(const uint8_t *p)
{
    const uint32_t w = be32(p);
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

/* ------------------------------------------------------------- pictures */

static int texArea(const struct beanlevel *bm, uint32_t t)
{
    int32_t w, h, fmt;
    return t < (uint32_t)bm->numtex && beanTextureInfo(bm->bf, (int)t, &w, &h, &fmt) ? w * h : 0;
}

/* PD beanTexIsGlassOverlay(): the release's shared maps laid over glass. */
static int texIsGlassOverlay(const struct beanlevel *bm, uint32_t t)
{
    static const char *const names[] = {
        "_0x059B9F65.tga.bin", "_0x00B5FD45.bin", "_0x00B5FD45.bmp.bin",
        "_0x03B30B95.tga.bin", "_0x018952E5.tga.bin", "_0x0F0BCEB5.tga.bin",
    };
    if (t >= (uint32_t)bm->numtex) {
        return 0;
    }
    const char *name = beanTextureName(bm->bf, (int)t);
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* PD beanTexSphereMap(): the reflecting props' spot map (1) / landscape (2). */
static int texSphereMap(const struct beanlevel *bm, uint32_t t)
{
    static const struct { const char *name; int kind; } maps[] = {
        { "_0x0E1C2BF5.tga.bin", 1 }, { "_0x081346B5.tga.bin", 1 }, { "_0x0B9D6195.tga.bin", 1 },
        { "_0x0C2BCE25.tga.bin", 2 }, { "_0x018248E5.tga.bin", 2 },
    };
    if (t >= (uint32_t)bm->numtex) {
        return 0;
    }
    const char *name = beanTextureName(bm->bf, (int)t);
    for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        if (strcmp(name, maps[i].name) == 0) {
            return maps[i].kind;
        }
    }
    return 0;
}

/*
 * PD beanMaterialTexture(): which of a material's inputs is its own picture.
 * Eight bytes an input after a 12-byte header {value, slot << 16 | sampler};
 * the biggest picture wins (last of equals), overlays and sphere maps only
 * when alone, and GoldenEye's own four-input shape takes its fourth.
 */
static uint32_t materialTexture(const struct beanlevel *bm, const uint8_t *st, uint32_t pc, uint32_t size, uint32_t len)
{
    static const uint16_t gesamplers[4] = { 0, 0, 1, 2 };
    const uint32_t count = size >= 20 ? (size - 12) / 8 : 0;
    uint32_t value[4] = { 0, 0, 0, 0 };
    uint32_t best = 0;
    int bestarea = 0;
    uint32_t read = 0;
    int found = 0;
    int geshape = count == 4;

    for (uint32_t k = 0; k < count && fits(pc + 12 + 8 * k, 8, len) && 12 + 8 * k + 8 <= size; k++) {
        const uint32_t t = be32(st + pc + 12 + 8 * k);
        const uint32_t where = be32(st + pc + 16 + 8 * k);
        const int area = texArea(bm, t);

        read = k + 1;
        if (k < 4) {
            value[k] = t;
            if ((where >> 16) != k || (k < 3 ? (where & 0xffff) != gesamplers[k]
                    : (where & 0xffff) < gesamplers[k])) {
                geshape = 0;
            }
        }
        if (found && texIsGlassOverlay(bm, t)) {
            continue;
        }
        if (found && texIsGlassOverlay(bm, best)) {
            best = t;
            bestarea = area;
            continue;
        }
        if (found && texSphereMap(bm, t)) {
            continue;
        }
        if (found && texSphereMap(bm, best)) {
            best = t;
            bestarea = area;
            continue;
        }
        if (!found || area >= bestarea) {
            found = 1;
            best = t;
            bestarea = area;
        }
    }
    if (geshape && read == 4 && value[3] < (uint32_t)bm->numtex) {
        return value[3];
    }
    return found ? best : 0;
}

/* ------------------------------------------------------------ the stream */

/*
 * PD beanWalkStream(), the level subset: first alternative at every switch
 * (0x16), jumps (0x19), end (0x1d); vertex buffer 0x2e, vertex shader 0x02,
 * pass 0x1a / section 0x15, render state 0x0c, material 0x2d, material
 * colour 0x06, instancing 0x21 / 0x25, draws 0x01 / 0x30 (every piece of a
 * level is the level's).
 */
static void walkStream(struct beanlevel *bm)
{
    const uint8_t *st = bm->stream;
    const uint32_t len = bm->streamlen;
    uint32_t pc = 0x24;
    uint32_t vb = 0, tex = 0;
    int16_t inst = -1;
    uint8_t passblend = 0, blend = 0;
    uint8_t alpha = 0xff;
    uint32_t colour = 0xffffff;
    uint8_t alphatest = 0, alpharef = 0;
    uint32_t secend = 0;
    uint8_t secblend = 0;
    uint8_t ownmat = 0;
    uint8_t pal[64];
    uint8_t numpal = 0;

    int hops = 0;

    if (len < 0x28) {
        return;
    }
    uint32_t end = be32(st + 4);
    if (end > len) {
        end = len;
    }

    for (int steps = 0; pc + 4 <= end && steps < 200000; steps++) {
        const uint32_t tag = be32(st + pc);
        const uint32_t size = tag >> 16;
        const uint32_t type = (tag >> 8) & 0xff;

        if (size < 4 || !fits(pc, size, len)) {
            break;
        }
        blend = secend && pc < secend ? secblend : passblend;

        /* Jumps: the record must hold its target word, the target must land
         * on a record inside the walked stream, and a file whose jumps loop
         * is stopped after BEAN_MAXHOPS of them (steps bounds the rest). */
        if (type == 0x16 || type == 0x19) {
            const uint32_t at = type == 0x16 ? 8 : 4;
            if (!fits(pc, at + 4, len) || size < at + 4 || ++hops > BEAN_MAXHOPS) {
                break;
            }
            const uint32_t to = be32(st + pc + at);
            if (to < 0x24 || (to & 3) || to + 4 > end) {
                break;
            }
            pc = to;
            continue;
        }
        if (type == 0x1d) {
            break;
        }

        if (type == 0x2e && size >= 12) {
            vb = be32(st + pc + 8);
        } else if (type == 0x02) {
            ownmat = 0;
        } else if (type == 0x1a && size >= 8) {
            /* 1 opaque, 2 blended, 3 both with 0x15 sections */
            passblend = (be32(st + pc + 4) & 0xff) == 2;
            secend = 0;
            blend = passblend;
        } else if (type == 0x15 && size >= 12) {
            secend = be32(st + pc + 4);
            secblend = (be32(st + pc + 8) & 0xff) == 2;
        } else if (type == 0x0c && size >= 12 && be32(st + pc + 4) == 0x64) {
            const uint32_t r = be32(st + pc + 8);
            alpharef = (uint8_t)(r > 0xff ? 0xff : r);
        } else if (type == 0x0c && size >= 12 && be32(st + pc + 4) == 0x60) {
            alphatest = be32(st + pc + 8) != 0;
        } else if (type == 0x2d && size >= 20) {
            tex = materialTexture(bm, st, pc, size, len);
            /* PD gebean.c: glass is a pane under one of the shared maps, in
             * the blended pass (Frigate's speedboat windscreen, D-228) */
            if (blend && bm->glasspane && tex < (uint32_t)bm->numtex && !texIsGlassOverlay(bm, tex)) {
                for (uint32_t k = 0; 12 + 8 * k + 8 <= size && fits(pc + 12 + 8 * k, 4, len); k++) {
                    if (texIsGlassOverlay(bm, be32(st + pc + 12 + 8 * k))) {
                        bm->glasspane[tex] = 1;
                    }
                }
            }
            ownmat = 1;
            alpha = 0xff;
            colour = 0xffffff;
        } else if (type == 0x06 && size >= 28 && be32(st + pc + 4) == 0 && (be32(st + pc + 8) >> 16) == 0x0c) {
            /* pixel shader constant 12: the material colour */
            const float a = bef32(st + pc + 24);
            alpha = a >= 1.0f ? 0xff : a <= 0.0f ? 0 : (uint8_t)(a * 255.0f + 0.5f);
            colour = 0;
            for (int k = 0; k < 3; k++) {
                const float c = bef32(st + pc + 12 + k * 4);
                colour = colour << 8 | (c >= 1.0f ? 0xff : c <= 0.0f ? 0 : (uint32_t)(c * 255.0f + 0.5f));
            }
        } else if (type == 0x21 && size >= 80) {
            /* a world matrix for the draws that follow, until 0x25:
             * {constant block, 0, 0, then 16 floats}, row vectors */
            if (!bm->insts) {
                bm->insts = calloc(BEAN_MAXINSTS, sizeof(*bm->insts));
            }
            if (bm->insts && bm->numinsts < BEAN_MAXINSTS) {
                for (int k = 0; k < 16; k++) {
                    bm->insts[bm->numinsts][k] = bef32(st + pc + 16 + k * 4);
                }
                inst = (int16_t)bm->numinsts++;
            }
        } else if (type == 0x25) {
            inst = -1;
        } else if (type == 0x12 && size >= 12) {
            /* round 26b, PD beanWalkStream(): the palette remap,
             * {_, count << 16, count x {u16 bone, ffff, 4}} */
            uint32_t count = be16(st + pc + 8);
            if (count > BEAN_MAXREMAP) {
                count = BEAN_MAXREMAP;
            }
            bm->numremap = 0;
            for (uint32_t k = 0; k < count && fits(pc + 12 + 8 * k, 2, len) && 12 + 8 * k + 2 <= size; k++) {
                bm->remap[bm->numremap++] = be16(st + pc + 12 + 8 * k);
            }
        } else if (type == 0x13 && size >= 12) {
            /* PD beanWalkStream(): the bone palette, {_, count << 16, bytes} */
            uint32_t count = be16(st + pc + 8);
            if (count > sizeof(pal) || !fits(pc + 12, count, len) || 12 + count > size) {
                count = 0;
            }
            memcpy(pal, st + pc + 12, count);
            numpal = (uint8_t)count;
        } else if ((type == 0x01 || type == 0x30) && size >= 16 && bm->numdraws < BEAN_MAXDRAWS) {
            struct beandraw *d = &bm->draws[bm->numdraws++];
            d->vb = vb;
            d->tex = tex;
            d->prim = be32(st + pc + 4);
            d->count = be32(st + pc + 8);
            d->ib = be32(st + pc + 12);
            d->inst = inst;
            d->blend = blend;
            d->alpha = alpha;
            d->colour = colour;
            d->alphatest = alphatest;
            d->alpharef = alpharef;
            d->ownmat = ownmat;
            d->piece = type == 0x30 && size >= 20 ? (int8_t)(be32(st + pc + 16) > 127 ? 127 : be32(st + pc + 16)) : -1;
            d->numpal = numpal;
            memcpy(d->pal, pal, numpal);
        }
        pc += size;
    }
}

/* PD beanFindIndexBuffers(): {obj, off, size, kind} rows in .data, kind 1 =
 * 16-bit indices, 6 = 32-bit (Dam's terrain, Frigate's hull). */
static void findIndexBuffers(struct beanlevel *bm)
{
    const uint8_t *d = bm->data;
    for (uint32_t o = 0; o + 16 <= bm->datalen && bm->numibs < BEAN_MAXIBS; o += 4) {
        const uint32_t obj = be32(d + o);
        const uint32_t off = be32(d + o + 4);
        const uint32_t size = be32(d + o + 8);
        const uint32_t kind = be32(d + o + 12);
        const uint32_t width = kind == 6 ? 4 : 2;
        int known = 0;

        if ((kind != 1 && kind != 6) || size == 0 || (size % width) || !fits(off, size, bm->gpulen)
                || obj >= bm->datalen) {
            continue;
        }
        for (int i = 0; i < bm->numibs; i++) {
            if (bm->ibs[i].obj == obj) {
                known = 1;
                break;
            }
        }
        if (!known) {
            bm->ibs[bm->numibs].obj = obj;
            bm->ibs[bm->numibs].off = off;
            bm->ibs[bm->numibs].size = size;
            bm->ibs[bm->numibs].width = width;
            bm->numibs++;
        }
    }
}

/* PD beanTriangles32(): a draw's triangles as index triples, list (4), quad
 * list (13) or fan (5 on Xenos). The caller frees *out. */
static int drawTriangles(const struct beanlevel *bm, const struct beandraw *d, uint32_t **out)
{
    const struct beanib *ib = NULL;
    int n = 0;

    *out = NULL;
    for (int i = 0; i < bm->numibs; i++) {
        if (bm->ibs[i].obj == d->ib) {
            ib = &bm->ibs[i];
            break;
        }
    }
    if (!ib || d->count < 3 || (uint64_t)d->count * ib->width > ib->size) {
        return 0;
    }
    const uint8_t *idx = bm->gpu + ib->off;
    uint32_t *tris = malloc(sizeof(uint32_t) * 3 * ((size_t)d->count * 2));
    if (!tris) {
        return 0;
    }
#define IDX(i) (ib->width == 4 ? be32(idx + (size_t)(i) * 4) : be16(idx + (size_t)(i) * 2))
    if (d->prim == 4) {
        for (uint32_t i = 0; i + 2 < d->count; i += 3, n++) {
            tris[n * 3] = IDX(i);
            tris[n * 3 + 1] = IDX(i + 1);
            tris[n * 3 + 2] = IDX(i + 2);
        }
    } else if (d->prim == 13) {
        for (uint32_t i = 0; i + 3 < d->count; i += 4) {
            const uint32_t a = IDX(i), b = IDX(i + 1), c = IDX(i + 2), e = IDX(i + 3);
            tris[n * 3] = a; tris[n * 3 + 1] = b; tris[n * 3 + 2] = c; n++;
            tris[n * 3] = a; tris[n * 3 + 1] = c; tris[n * 3 + 2] = e; n++;
        }
    } else if (d->prim == 5) {
        for (uint32_t i = 0; i + 2 < d->count; i++, n++) {
            tris[n * 3] = IDX(0);
            tris[n * 3 + 1] = IDX(i + 1);
            tris[n * 3 + 2] = IDX(i + 2);
        }
    }
#undef IDX
    *out = tris;
    return n;
}

/* ------------------------------------------------------------- vertices */

/* PD beanReadVb() + beanReadVbPositions(): a {stride, _, off, size} row. */
static int readVb(const struct beanlevel *bm, uint32_t desc, struct beanvb *vb)
{
    memset(vb, 0, sizeof(*vb));
    if (!fits(desc, 16, bm->datalen)) {
        return 0;
    }
    vb->stride = be32(bm->data + desc);
    vb->off = be32(bm->data + desc + 8);
    const uint32_t size = be32(bm->data + desc + 12);
    if ((vb->stride < 16 && vb->stride != 12) || vb->stride > 256 || !fits(vb->off, size, bm->gpulen)) {
        return 0;
    }
    vb->count = size / vb->stride;
    if (vb->stride == 28) {
        /* a colour's alpha byte is 0xff on every vertex, a UV's high byte is not */
        vb->col28 = 1;
        for (uint32_t i = 0; i < vb->count; i++) {
            if (bm->gpu[vb->off + i * 28 + 24] != 0xff) {
                vb->col28 = 0;
                break;
            }
        }
    }
    return 1;
}

/* ABGR as Bean stores it -> ARGB (PD beanColour()). */
static uint32_t beanColour(uint32_t abgr)
{
    return (abgr & 0xff00ff00) | ((abgr >> 16) & 0xff) | ((abgr & 0xff) << 16);
}

/*
 * One vertex of a level buffer (PD beanVertex() + the level arms of
 * gebeanLevelTriangles()):
 *   12  position only (shaded by the material colour)
 *   16  position, normal
 *   20  position, normal, colour (tree cards: UVs made)
 *   24  position, normal, UV, colour
 *   28  position, palette slots / tangent ..., colour or UV
 *   32  LEVEL: position, normal +12, UV +16, UV2 +20, blend +24, colour +28
 *   36  LEVEL water: word, position +4, normal +16, UV +20, ..., colour +32
 */
static int levelVertex(const struct beanlevel *bm, const struct beanvb *vb, const struct beandraw *d,
                       uint32_t i, struct beanvtx *v)
{
    if (i >= vb->count) {
        return 0;
    }
    const uint8_t *p = bm->gpu + vb->off + (size_t)i * vb->stride;
    const float s = bm->uvscale;
    memset(v, 0, sizeof(*v));
    v->argb = 0xffffffff;

    if (vb->stride == 36) {
        for (int j = 0; j < 3; j++) v->pos[j] = bef32(p + 4 + j * 4);
        v->uv[0] = (int16_t)be16(p + 20) / s;
        v->uv[1] = (int16_t)be16(p + 22) / s;
        v->argb = beanColour(be32(p + 32));
        return 1;
    }
    for (int j = 0; j < 3; j++) v->pos[j] = bef32(p + j * 4);
    switch (vb->stride) {
    case 12:
        v->argb = (uint32_t)d->alpha << 24 | (d->colour & 0xffffff);
        break;
    case 16:
        break;
    case 20:
        v->argb = beanColour(be32(p + 16));
        break;
    case 24:
        v->uv[0] = (int16_t)be16(p + 16) / s;
        v->uv[1] = (int16_t)be16(p + 18) / s;
        v->argb = beanColour(be32(p + 20));
        break;
    case 28:
        if (vb->col28) {
            v->argb = beanColour(be32(p + 24));
        } else {
            v->uv[0] = (int16_t)be16(p + 24) / s;
            v->uv[1] = (int16_t)be16(p + 26) / s;
        }
        break;
    case 32:
        v->uv[0] = (int16_t)be16(p + 16) / s;
        v->uv[1] = (int16_t)be16(p + 18) / s;
        v->argb = beanColour(be32(p + 28));
        break;
    default:
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- trees */

/* PD beanTreeMeasure()/beanTreeUvs(): a tree's buffers carry no UVs; the
 * release's shader makes them, so they are made here from the geometry. */
struct beantree {
    float axis[2];
    float radius;
    float ymin;
};

static void treeMeasure(const struct beanlevel *bm, const struct beanvb *vb, struct beantree *tree)
{
    double sum[2] = { 0, 0 }, rsum = 0;
    int rn = 0;
    tree->ymin = 1e30f;
    for (uint32_t i = 0; i < vb->count; i++) {
        const uint8_t *p = bm->gpu + vb->off + (size_t)i * vb->stride;
        sum[0] += bef32(p);
        sum[1] += bef32(p + 8);
        if (bef32(p + 4) < tree->ymin) tree->ymin = bef32(p + 4);
    }
    tree->axis[0] = vb->count ? (float)(sum[0] / vb->count) : 0;
    tree->axis[1] = vb->count ? (float)(sum[1] / vb->count) : 0;
    for (uint32_t i = 0; i < vb->count; i++) {
        const uint8_t *p = bm->gpu + vb->off + (size_t)i * vb->stride;
        if (bef32(p + 4) < tree->ymin + 1) {
            const float dx = bef32(p) - tree->axis[0];
            const float dz = bef32(p + 8) - tree->axis[1];
            rsum += sqrtf(dx * dx + dz * dz);
            rn++;
        }
    }
    tree->radius = rn && rsum > 0 ? (float)(rsum / rn) : 50;
}

static float treeDistance(const struct beanlevel *bm, const struct beanvb *vb, const struct beantree *tree, uint32_t i)
{
    const uint8_t *p = bm->gpu + vb->off + (size_t)i * vb->stride;
    const float dx = bef32(p) - tree->axis[0];
    const float dz = bef32(p + 8) - tree->axis[1];
    return sqrtf(dx * dx + dz * dz);
}

static void treeUvs(const struct beanlevel *bm, const struct beanvb *vb, const struct beantree *tree,
                    const uint32_t *idx, struct beanlevelvtx *v)
{
    if (vb->stride == 20) {
        for (int k = 0; k < 3; k++) {
            const uint32_t card = idx[k] & ~3u;
            if (card + 3 >= vb->count) {
                v[k].uv[0] = v[k].uv[1] = 0;
                continue;
            }
            const float near0 = treeDistance(bm, vb, tree, card) + treeDistance(bm, vb, tree, card + 1);
            const float near1 = treeDistance(bm, vb, tree, card + 2) + treeDistance(bm, vb, tree, card + 3);
            v[k].uv[0] = ((idx[k] & 2) != 0) == (near1 < near0) ? 1 : 0;
            v[k].uv[1] = (idx[k] & 1) ? 0 : 1;
        }
        return;
    }
    const float tile = (float)(2 * 2 * M_PI * tree->radius);
    int onaxis[3];
    float lo = 1, hi = 0, sum = 0;
    int n = 0;
    for (int k = 0; k < 3; k++) {
        const float dx = v[k].pos[0] - tree->axis[0];
        const float dz = v[k].pos[2] - tree->axis[1];
        onaxis[k] = dx * dx + dz * dz < tree->radius * tree->radius * 0.0625f;
        v[k].uv[0] = onaxis[k] ? 0 : (float)(atan2f(dz, dx) / (2 * M_PI) + 0.5f);
        v[k].uv[1] = (tree->ymin - v[k].pos[1]) / tile;
        if (!onaxis[k]) {
            lo = v[k].uv[0] < lo ? v[k].uv[0] : lo;
            hi = v[k].uv[0] > hi ? v[k].uv[0] : hi;
        }
    }
    for (int k = 0; k < 3; k++) {
        if (!onaxis[k]) {
            if (hi - lo > 0.5f && v[k].uv[0] < 0.5f) {
                v[k].uv[0] += 1;
            }
            sum += v[k].uv[0];
            n++;
        }
    }
    for (int k = 0; k < 3; k++) {
        if (onaxis[k]) {
            v[k].uv[0] = n ? sum / n : 0;
        }
    }
}

/* -------------------------------------------------------------- public */

/*
 * PD beanTexAnimation() (gebean.c), the slide alone: .data's texture-animation
 * block (pointer at 0xdc) holds n entries of 0x64 bytes {attr name, list of the
 * stream records it applies to, slide u/v (+0x2c/+0x30), ...}; a 0x05 record
 * in the stream names the entry's target, applied to the picture of the
 * material bound before it - its one picture, or of several the one with
 * alpha for a "transparency" attribute. Frames and rotation are not read
 * (Dam has neither on its level pictures).
 */
static int texHasAlphaFormat(const struct beanlevel *bm, int t)
{
    int32_t w, h, fmt = 0;
    if (!beanTextureInfo(bm->bf, t, &w, &h, &fmt)) {
        return 0;
    }
    return fmt == X360_FMT_DXT23 || fmt == X360_FMT_DXT45 || fmt == X360_FMT_8888;
}

static void texAnimation(struct beanlevel *bm)
{
    const uint8_t *d = bm->data;
    const uint8_t *st = bm->stream;
    uint32_t mat[8];
    int nummat = 0;

    if (bm->numtex <= 0 || !d || bm->datalen < 0xe0 || memcmp(d, "rendergraph", 12) != 0 || bm->streamlen < 0x28) {
        return;
    }
    const uint32_t blk = be32(d + 0xdc);
    if (blk == 0 || !fits(blk, 0x10, bm->datalen)) {
        return;
    }
    const uint32_t n = be32(d + blk + 8);
    const uint32_t entries = be32(d + blk + 12);
    if (n == 0 || n > 256 || !fits(entries, (uint64_t)n * 0x64, bm->datalen)) {
        return;
    }
    bm->texscroll = calloc((size_t)bm->numtex, sizeof(*bm->texscroll));
    if (!bm->texscroll) {
        return;
    }
    uint8_t *seen = calloc((size_t)bm->numtex, 1);
    if (!seen) {
        return;
    }
    uint32_t end = be32(st + 4);
    if (end > bm->streamlen) {
        end = bm->streamlen;
    }
    /* a straight walk of the records (no jumps): every 0x2d/0x05 pair */
    for (uint32_t pc = 0x24; pc + 4 <= end;) {
        const uint32_t tag = be32(st + pc);
        const uint32_t size = tag >> 16;
        const uint32_t type = (tag >> 8) & 0xff;
        if (size < 4 || !fits(pc, size, bm->streamlen)) {
            break;
        }
        if (type == 0x2d && size >= 20) {
            const uint32_t extra = be16(st + pc + 10);
            nummat = 0;
            for (uint32_t k = extra; 12 + 8 * k + 8 <= size && nummat < 8; k++) {
                mat[nummat++] = be32(st + pc + 12 + 8 * k);
            }
        } else if (type == 0x05 && size >= 12) {
            const uint32_t ptr = be32(st + pc + 4);
            for (uint32_t e = 0; e < n; e++) {
                const uint8_t *p = d + entries + e * 0x64;
                const uint32_t attr = be32(p + 8);
                const uint32_t list = be32(p + 0x18);
                const uint32_t count = be32(p + 0x20);
                const float su = bef32(p + 0x2c);
                const float sv = bef32(p + 0x30);
                const int transparency = fits(attr, 13, bm->datalen) && memcmp(d + attr, "transparency", 13) == 0;
                int listed = 0, t = -1;
                if (count > 64 || !fits(list, (uint64_t)count * 4, bm->datalen)) {
                    continue;
                }
                for (uint32_t k = 0; k < count && !listed; k++) {
                    listed = be32(d + list + k * 4) == ptr;
                }
                if (!listed) {
                    continue;
                }
                if (nummat == 1) {
                    t = (int)mat[0];
                } else {
                    for (int m = 0; m < nummat; m++) {
                        if (transparency && (int)mat[m] < bm->numtex && texHasAlphaFormat(bm, (int)mat[m])) {
                            t = t < 0 ? (int)mat[m] : -2;
                        }
                    }
                }
                if (t < 0 || t >= bm->numtex || seen[t]) {
                    continue;
                }
                seen[t] = 1;
                bm->texscroll[t][0] = su == su && fabsf(su) < 100.0f ? su : 0.0f;
                bm->texscroll[t][1] = sv == sv && fabsf(sv) < 100.0f ? sv : 0.0f;
            }
        }
        pc += size;
    }
    free(seen);
}

int beanLevelTextureIsGlassPane(const struct beanlevel *bm, int tex)
{
    return bm && bm->glasspane && tex >= 0 && tex < bm->numtex && bm->glasspane[tex];
}

int beanLevelTextureScroll(const struct beanlevel *bm, int tex, float *su, float *sv)
{
    *su = *sv = 0.0f;
    if (!bm || !bm->texscroll || tex < 0 || tex >= bm->numtex) {
        return 0;
    }
    *su = bm->texscroll[tex][0];
    *sv = bm->texscroll[tex][1];
    return *su != 0.0f || *sv != 0.0f;
}

struct beanlevel *beanLevelOpen(const char *path, const char *name)
{
    FILE *fp = fopen(path, "rb");
    long size;
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) <= 0 || size > 64L * 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    struct beanlevel *bm = calloc(1, sizeof(*bm));
    if (!bm) {
        fclose(fp);
        return NULL;
    }
    bm->file = malloc((size_t)size);
    bm->filelen = (uint32_t)size;
    bm->draws = calloc(BEAN_MAXDRAWS, sizeof(*bm->draws));
    if (!bm->file || !bm->draws || fread(bm->file, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        beanLevelClose(bm);
        return NULL;
    }
    fclose(fp);

    bm->bf = beanOpen(bm->file, bm->filelen);
    if (bm->bf) {
        bm->data = beanSection(bm->bf, 1, ".data", &bm->datalen);
        bm->gpu = beanSection(bm->bf, 1, ".gpu", &bm->gpulen);
        bm->stream = beanSection(bm->bf, 1, ".stream", &bm->streamlen);
    }
    if (!bm->bf || !bm->data || !bm->gpu || !bm->stream) {
        beanLevelClose(bm);
        return NULL;
    }
    bm->numtex = beanNumTextures(bm->bf);
    bm->glasspane = bm->numtex > 0 ? calloc((size_t)bm->numtex, 1) : NULL;
    walkStream(bm);
    findIndexBuffers(bm);
    texAnimation(bm);

    /* gebeanLevelOpenSource(): a level's UV scale is written only in its
     * shaders, as the big-endian float literal 1/128 (Dam, Train) or 1/256
     * (most); levels with neither are 1/1024, Silo 1/512. */
    {
        static const uint8_t lits[2][4] = { { 0x3c, 0x00, 0x00, 0x00 }, { 0x3b, 0x80, 0x00, 0x00 } };
        float scale = 1024.0f;
        for (uint32_t o = 0; o + 4 <= bm->gpulen; o += 4) {
            if (memcmp(bm->gpu + o, lits[0], 4) == 0) bm->lit128++;
            else if (memcmp(bm->gpu + o, lits[1], 4) == 0) bm->lit256++;
        }
        if (bm->lit128 >= 3 && bm->lit128 > bm->lit256) {
            scale = 128.0f;
        } else if (bm->lit256 >= 3) {
            scale = 256.0f;
        } else if (name && strcmp(name, "silo") == 0) {
            scale = 512.0f;
        }
        bm->uvscale = scale;
    }
    return bm;
}

void beanLevelClose(struct beanlevel *bm)
{
    if (!bm) {
        return;
    }
    beanClose(bm->bf);
    free(bm->draws);
    free(bm->insts);
    free(bm->texscroll);
    free(bm->glasspane);
    free(bm->pre);
    free(bm->prew);
    free(bm->preh);
    free(bm->file);
    free(bm);
}

const uint8_t *beanLevelFileBytes(const struct beanlevel *bm, uint32_t *len)
{
    *len = bm ? bm->filelen : 0;
    return bm ? bm->file : NULL;
}

void beanLevelInfo(const struct beanlevel *bm, struct beanlevelinfo *out)
{
    memset(out, 0, sizeof(*out));
    if (!bm) {
        return;
    }
    out->numdraws = bm->numdraws;
    out->numtextures = bm->numtex;
    out->numinsts = bm->numinsts;
    out->numibs = bm->numibs;
    for (int i = 0; i < bm->numibs; i++) {
        out->numibs32 += bm->ibs[i].width == 4;
    }
    out->uvscale = bm->uvscale;
    out->lit128 = bm->lit128;
    out->lit256 = bm->lit256;
    out->filebytes = bm->filelen;
}

/* Midas round 61 (doc 81): a sky file's UVs are sixteen-thousandths, not a
 * level's shader scale (PD gebean.c gebeanSkyOpen() sets 16384 after the load) */
void beanLevelSetUvScale(struct beanlevel *bm, float uvscale)
{
    if (bm && uvscale > 0.0f) {
        bm->uvscale = uvscale;
    }
}

/* the buffers any instancing record draws (its first copy is drawn
 * without one, where the tree was modelled) */
static int levelTreeVbs(const struct beanlevel *bm, uint32_t *treevbs)
{
    int numtreevbs = 0;
    for (int d = 0; d < bm->numdraws; d++) {
        const struct beandraw *draw = &bm->draws[d];
        int known = 0;
        if (draw->inst < 0) {
            continue;
        }
        for (int i = 0; i < numtreevbs; i++) {
            known |= treevbs[i] == draw->vb;
        }
        if (!known && numtreevbs < BEAN_MAXTREEVBS) {
            treevbs[numtreevbs++] = draw->vb;
        }
    }
    return numtreevbs;
}

static int levelIsTree(const uint32_t *treevbs, int numtreevbs, const struct beandraw *draw, const struct beanvb *vb)
{
    int istree = 0;
    for (int i = 0; i < numtreevbs; i++) {
        istree |= treevbs[i] == draw->vb && (vb->stride == 20 || vb->stride == 28);
    }
    return istree;
}

/* a level draw with no UV and no picture since its vertex shader was set:
 * shaded by its colour alone (gebeanLevelTriangles' `plain`) */
static int levelPlain(const struct beandraw *draw, const struct beanvb *vb, int istree)
{
    return (!draw->ownmat && !istree && (vb->stride == 16 || vb->stride == 20)) || vb->stride == 12;
}

/* the model layouts with no UV: drawn by their colour (tex -1) */
static int modelPlain(const struct beanlevel *bm, const struct beanvb *vb)
{
    return vb->stride == 16 || (vb->stride == 20 && !bm->uv20) || (vb->stride == 28 && vb->col28);
}

/* Round 25b (D-142): what one draw's material comes to, for the coverage
 * audit and the callers' fallbacks. */
static int classifyDraw(const struct beanlevel *bm, const struct beandraw *draw, int plain, int ownpic_nouv)
{
    if (ownpic_nouv) {
        return BEANDRAW_SPHEREMAP;
    }
    if (plain) {
        return BEANDRAW_COLOUR;
    }
    return draw->tex < (uint32_t)bm->numtex ? BEANDRAW_PICTURE : BEANDRAW_BADPIC;
}

void beanMeshCountDraws(const struct beanlevel *bm, int model, struct beandrawcount *out)
{
    uint32_t treevbs[BEAN_MAXTREEVBS];
    const int numtreevbs = model ? 0 : levelTreeVbs(bm, treevbs);

    memset(out, 0, sizeof(*out));
    for (int d = 0; bm && d < bm->numdraws; d++) {
        const struct beandraw *draw = &bm->draws[d];
        struct beanvb vb;
        out->draws++;
        if (model && draw->piece > 1) {
            out->skipped++;
            continue;
        }
        if (!readVb(bm, draw->vb, &vb)) {
            out->unreadable++;
            continue;
        }
        if (model && vb.stride == 12) {
            out->skipped++;
            continue;
        }
        int kind;
        if (model) {
            const int plain = modelPlain(bm, &vb);
            kind = classifyDraw(bm, draw, plain, plain && draw->ownmat && draw->tex < (uint32_t)bm->numtex);
        } else {
            kind = classifyDraw(bm, draw, levelPlain(draw, &vb, levelIsTree(treevbs, numtreevbs, draw, &vb)), 0);
        }
        out->kinds[kind]++;
    }
}

int beanLevelTriangles(struct beanlevel *bm,
                       void (*fn)(void *arg, int tex, const struct beanlevelvtx *v), void *arg)
{
    uint32_t treevbs[BEAN_MAXTREEVBS];
    const int numtreevbs = levelTreeVbs(bm, treevbs);
    int count = 0;

    for (int d = 0; d < bm->numdraws; d++) {
        const struct beandraw *draw = &bm->draws[d];
        const float *m = draw->inst >= 0 ? bm->insts[draw->inst] : NULL;
        struct beanvb vb;
        struct beantree tree;
        uint32_t *tris = NULL;
        int istree = 0;
        int tex = draw->tex < (uint32_t)bm->numtex ? (int)draw->tex : -1;

        if (!readVb(bm, draw->vb, &vb)) {
            continue;
        }
        if (vb.stride == 12) {
            /* positions alone: no picture, the material colour */
            tex = -1;
        }
        istree = levelIsTree(treevbs, numtreevbs, draw, &vb);
        if (istree) {
            treeMeasure(bm, &vb, &tree);
        }
        const int numtris = drawTriangles(bm, draw, &tris);
        const int plain = levelPlain(draw, &vb, istree);
        const uint8_t kind = (uint8_t)classifyDraw(bm, draw, plain, 0);

        for (int t = 0; t < numtris; t++) {
            struct beanlevelvtx v[3];
            int ok = 1;
            for (int k = 0; k < 3 && ok; k++) {
                struct beanvtx bv;
                ok = levelVertex(bm, &vb, draw, tris[t * 3 + k], &bv);
                /* Statue Park runs out to 3.1 million units; Dam's broken
                 * vertices are past 1e30 */
                for (int j = 0; j < 3 && ok; j++) {
                    ok = bv.pos[j] == bv.pos[j] && bv.pos[j] > -1e8f && bv.pos[j] < 1e8f;
                }
                if (ok) {
                    memcpy(v[k].pos, bv.pos, sizeof(v[k].pos));
                    v[k].uv[0] = bv.uv[0];
                    v[k].uv[1] = bv.uv[1];
                    v[k].argb = bv.argb;
                    v[k].blend = draw->blend;
                    v[k].alphatest = draw->alphatest;
                    v[k].alpharef = draw->alpharef;
                    v[k].plain = (uint8_t)plain;
                    v[k].water = vb.stride == 36;
                    v[k].kind = kind;
                    v[k].ownmat = draw->ownmat;
                }
            }
            if (!ok) {
                continue;
            }
            if (istree) {
                treeUvs(bm, &vb, &tree, &tris[t * 3], v);
            }
            if (m) {
                for (int k = 0; k < 3; k++) {
                    const float x = v[k].pos[0], y = v[k].pos[1], z = v[k].pos[2];
                    for (int j = 0; j < 3; j++) {
                        v[k].pos[j] = x * m[j] + y * m[4 + j] + z * m[8 + j] + m[12 + j];
                    }
                }
            }
            fn(arg, tex, v);
            count++;
        }
        free(tris);
    }
    return count;
}

uint8_t *beanLevelDecodeTexture(const struct beanlevel *bm, int tex, int32_t *w, int32_t *h)
{
    if (bm && bm->pre && tex >= 0 && tex < bm->numtex && bm->pre[tex]) {
        *w = bm->prew[tex];
        *h = bm->preh[tex];
        return bm->pre[tex];   /* D-317: decoded ahead, pinned in the registry */
    }
    return bm ? beanDecodeTexture(bm->bf, tex, w, h) : NULL;
}

void beanLevelAttachPictures(struct beanlevel *bm, uint8_t *const *pics, const int32_t *w, const int32_t *h)
{
    if (!bm || bm->numtex <= 0 || bm->pre) {
        return;
    }
    bm->pre = calloc((size_t)bm->numtex, sizeof(*bm->pre));
    bm->prew = calloc((size_t)bm->numtex, sizeof(*bm->prew));
    bm->preh = calloc((size_t)bm->numtex, sizeof(*bm->preh));
    if (!bm->pre || !bm->prew || !bm->preh) {
        free(bm->pre);
        free(bm->prew);
        free(bm->preh);
        bm->pre = NULL;
        bm->prew = bm->preh = NULL;
        return;
    }
    for (int t = 0; t < bm->numtex; t++) {
        bm->pre[t] = pics[t];
        bm->prew[t] = w[t];
        bm->preh[t] = h[t];
    }
}

void beanLevelUsedTextures(const struct beanlevel *bm, uint8_t *used)
{
    for (int d = 0; bm && d < bm->numdraws; d++) {
        if (bm->draws[d].tex < (uint32_t)bm->numtex) {
            used[bm->draws[d].tex] = 1;
        }
    }
}

int beanLevelTextureSize(const struct beanlevel *bm, int tex, int32_t *w, int32_t *h)
{
    int32_t fmt;
    return bm ? beanTextureInfo(bm->bf, tex, w, h, &fmt) : 0;
}


/* =================================================================
 * Models (round C3): a prop's rendergraph, rigid.
 * ================================================================= */

/* PD beanUnpackNormal() is not needed: props are drawn with their baked
 * colours (as the release's own vertex colour), so only position, UV and
 * colour are read. */

/*
 * PD beanVertex(), the model layouts: every layout starts with a float
 * position; 28/32/36 carry four u16 palette slots (and, on 36, four weight
 * bytes) - on a PROP they are one bone, the model's own, so they are read
 * past and the prop is drawn rigid; then a 10:10:10 normal, an s16 UV and an
 * ABGR colour, whichever the stride has room for. Stride 28 holds one of a
 * UV and a colour (col28). 20 = normal + colour, or normal + UV in a file
 * whose shaders fetch one (bm->uv20, round 26a: the saloon car, Escort, ZIL,
 * landmine), 16 = normal alone, 24 = normal, UV, colour.
 */
static int modelVertex(const struct beanlevel *bm, const struct beanvb *vb, uint32_t i, struct beanvtx *v)
{
    if (i >= vb->count) {
        return 0;
    }
    const uint8_t *p = bm->gpu + vb->off + (size_t)i * vb->stride;
    uint32_t k;
    int hasuv = 1;

    memset(v, 0, sizeof(*v));
    for (int j = 0; j < 3; j++) v->pos[j] = bef32(p + j * 4);
    v->argb = 0xffffffff;
    v->slot = -1;
    switch (vb->stride) {
    case 28:
    case 32:
    case 36: {
        /* four u16 palette slots, three times the slot (0xf000 none): the
         * first is the bone the vertex rides (PD beanVertex) */
        const uint16_t sl = be16(p + 12);
        v->slot = sl == 0xf000 ? -1 : sl / 3;
        k = vb->stride == 36 ? 24 : 20;
        hasuv = !(vb->stride == 28 && vb->col28);
        if (vb->stride != 28 || vb->col28) {
            v->argb = beanColour(be32(p + vb->stride - 4));
        }
        break;
    }
    case 20:
        k = 12;
        hasuv = bm->uv20;
        if (!bm->uv20) {
            v->argb = beanColour(be32(p + 16));
        }
        break;
    case 16:
        k = 12;
        hasuv = 0;
        break;
    case 24:
        k = 12;
        v->argb = beanColour(be32(p + 20));
        break;
    default:
        return 0;
    }
    /* round 26a (D-146): the normal, 10:10:10 signed at k (PD
     * beanUnpackNormal) - the sphere map's lookup */
    {
        const uint32_t w = be32(p + k);
        for (int j = 0; j < 3; j++) {
            int c = (int)((w >> (10 * j)) & 1023);
            if (c & 512) {
                c -= 1024;
            }
            v->nrm[j] = (float)c / 511.0f;
        }
    }
    if (hasuv) {
        v->uv[0] = (int16_t)be16(p + k + 4) / bm->uvscale;
        v->uv[1] = (int16_t)be16(p + k + 6) / bm->uvscale;
    }
    return 1;
}

/* PD beanMeasureUvScale(): the smallest power of two from 1024 within a
 * quarter of the median picture's largest |UV|. */
static float modelMeasureUvScale(const struct beanlevel *bm)
{
    uint32_t *biggest = calloc((size_t)(bm->numtex > 0 ? bm->numtex : 1), sizeof(uint32_t));
    uint8_t *drawn = calloc((size_t)(bm->numtex > 0 ? bm->numtex : 1), 1);
    uint32_t *sorted = calloc((size_t)(bm->numtex > 0 ? bm->numtex : 1), sizeof(uint32_t));
    int n = 0;
    uint32_t scale;

    if (!biggest || !drawn || !sorted) {
        free(biggest);
        free(drawn);
        free(sorted);
        return 32768.0f;
    }
    for (int di = 0; di < bm->numdraws; di++) {
        const struct beandraw *d = &bm->draws[di];
        struct beanvb vb;
        uint32_t uvo, *tris = NULL;
        int32_t w, h, fmt;

        if (d->tex >= (uint32_t)bm->numtex || !readVb(bm, d->vb, &vb)) {
            continue;
        }
        if (beanTextureInfo(bm->bf, (int)d->tex, &w, &h, &fmt) && w * h <= 4) {
            continue;
        }
        switch (vb.stride) {
        case 36: uvo = 28; break;
        case 32: uvo = 24; break;
        case 28: uvo = 24; break;
        case 24: uvo = 16; break;
        case 20: if (!bm->uv20) continue; uvo = 16; break;   /* round 26a */
        default: continue;
        }
        if (vb.stride == 28 && vb.col28) {
            continue;
        }
        const int numtris = drawTriangles(bm, d, &tris);
        for (int i = 0; i < numtris * 3; i++) {
            if (tris[i] >= vb.count) {
                continue;
            }
            const uint8_t *p = bm->gpu + vb.off + (size_t)tris[i] * vb.stride + uvo;
            int u = (int16_t)be16(p), t = (int16_t)be16(p + 2);
            u = u < 0 ? -u : u;
            t = t < 0 ? -t : t;
            if ((uint32_t)u > biggest[d->tex]) biggest[d->tex] = (uint32_t)u;
            if ((uint32_t)t > biggest[d->tex]) biggest[d->tex] = (uint32_t)t;
        }
        if (numtris > 0) {
            drawn[d->tex] = 1;
        }
        free(tris);
    }
    for (int t = 0; t < bm->numtex; t++) {
        if (drawn[t]) {
            int at = n++;
            while (at > 0 && sorted[at - 1] > biggest[t]) {
                sorted[at] = sorted[at - 1];
                at--;
            }
            sorted[at] = biggest[t];
        }
    }
    if (n == 0) {
        scale = 32768;
    } else {
        for (scale = 1024; scale < 32768 && scale * 1.25f < sorted[(n - 1) / 2]; scale *= 2) {
        }
    }
    free(biggest);
    free(drawn);
    free(sorted);
    return (float)scale;
}

/* PD beanShaderUvScale(): an HD model's vertex shaders multiply the s16 UV by
 * a literal power of two (1/4096.01 on the truck); any float past the vertex
 * and index buffers within a thousandth of 2^-20..2^-5, all agreeing. 0 when
 * there is none (or two disagree). */
static uint32_t modelBuffersEnd(const struct beanlevel *bm)
{
    uint32_t end = 0;
    for (int di = 0; di < bm->numdraws; di++) {
        const struct beandraw *d = &bm->draws[di];
        struct beanvb vb;
        if (readVb(bm, d->vb, &vb) && vb.off + vb.count * vb.stride > end) {
            end = vb.off + vb.count * vb.stride;
        }
        for (int i = 0; i < bm->numibs; i++) {
            if (bm->ibs[i].obj == d->ib && bm->ibs[i].off + bm->ibs[i].size > end) {
                end = bm->ibs[i].off + bm->ibs[i].size;
            }
        }
    }
    return end;
}

/* Round 26a (D-146), PD beanShaderUv20(): whether a model's stride-20
 * vertices carry a UV rather than a colour - its vertex shaders' fetch
 * constants past the buffers (a fetch of format 25 is the UV's, 6 the
 * colour's; the saloon car, Escort, ZIL and landmine take a UV). Read
 * as a colour, their textured draws had no UV and passed for sphere maps. */
static int modelUv20(const struct beanlevel *bm)
{
    const uint32_t end = modelBuffersEnd(bm);
    int uv = 0;
    if (end == 0) {
        return 0;
    }
    for (uint32_t at = (end + 3) & ~3u; at + 12 <= bm->gpulen; at += 4) {
        const uint32_t w0 = be32(bm->gpu + at);
        const uint32_t fmt = (be32(bm->gpu + at + 4) >> 16) & 63;
        if ((w0 & 31) != 0 || !((w0 >> 19) & 1) || (be32(bm->gpu + at + 8) & 0x7fffffffu) != (4u << 8 | 5u)) {
            continue;
        }
        if (fmt == 6) {
            return 0;
        }
        uv |= fmt == 25;
    }
    return uv;
}

static float modelShaderUvScale(const struct beanlevel *bm)
{
    uint32_t end = modelBuffersEnd(bm);
    float found = 0.0f;

    if (end == 0) {
        return 0.0f;
    }
    for (uint32_t at = (end + 3) & ~3u; at + 4 <= bm->gpulen; at += 4) {
        const float f = bef32(bm->gpu + at);
        if (!(f > 1.0f / 1048576.0f && f < 1.0f / 32.0f)) {
            continue;
        }
        const float r = 1.0f / f;
        float p = 1.0f;
        while (p * 2.0f <= r * 1.4142135f) {
            p *= 2.0f;
        }
        if (fabsf(r / p - 1.0f) > 0.001f) {
            continue;
        }
        if (found > 0.0f && p != found) {
            return 0.0f;
        }
        found = p;
    }
    return found;
}

struct beanlevel *beanModelOpenRaw(const char *path)
{
    struct beanlevel *bm = beanLevelOpen(path, NULL);
    if (!bm) {
        return NULL;
    }
    /* round 26a: stride 20 is normal + UV in some files (PD beanShaderUv20) */
    bm->uv20 = modelUv20(bm);
    /* a model's UV range is its own (beanLevelOpen sniffed a level's) */
    bm->uvscale = modelMeasureUvScale(bm);
    const float shader = modelShaderUvScale(bm);
    if (shader > 0.0f) {
        bm->uvscale = shader;
    }
    return bm;
}

struct beanlevel *beanPrefetchTake(const char *path);   /* beanprefetch.c (D-317) */

struct beanlevel *beanModelOpen(const char *path)
{
    struct beanlevel *bm = beanPrefetchTake(path);
    return bm ? bm : beanModelOpenRaw(path);
}

int beanModelTriangles(struct beanlevel *bm,
                       void (*fn)(void *arg, int tex, const struct beanmodelvtx *v), void *arg)
{
    int count = 0;

    for (int d = 0; d < bm->numdraws; d++) {
        const struct beandraw *draw = &bm->draws[d];
        struct beanvb vb;
        uint32_t *tris = NULL;
        int tex = draw->tex < (uint32_t)bm->numtex ? (int)draw->tex : -1;

        /* PD beanWalkStream(): a 0x30 draw's piece past 1 is not the model
         * (a head's sunglasses' arms; a gun's pieces are kept by PD for guns
         * only) */
        if (draw->piece > 1) {
            continue;
        }
        if (!readVb(bm, draw->vb, &vb)) {
            continue;
        }
        if (vb.stride == 12) {
            continue; /* positions alone: no prop draws one (levels only) */
        }
        /* no UV in the layout and no picture of its own: its colour alone;
         * with a picture of its own the release sphere-maps it (PD
         * beanDrawIsSphereMapped), which this reader does not do */
        const int plain = modelPlain(bm, &vb);
        const uint8_t kind = (uint8_t)classifyDraw(bm, draw, plain,
                                                   plain && draw->ownmat && draw->tex < (uint32_t)bm->numtex);
        const int numtris = drawTriangles(bm, draw, &tris);
        for (int t = 0; t < numtris; t++) {
            struct beanmodelvtx v[3];
            int ok = 1;
            for (int k = 0; k < 3 && ok; k++) {
                struct beanvtx bv;
                ok = modelVertex(bm, &vb, tris[t * 3 + k], &bv);
                for (int j = 0; j < 3 && ok; j++) {
                    ok = bv.pos[j] == bv.pos[j] && bv.pos[j] > -1e8f && bv.pos[j] < 1e8f;
                }
                if (ok) {
                    memcpy(v[k].pos, bv.pos, sizeof(v[k].pos));
                    v[k].uv[0] = bv.uv[0];
                    v[k].uv[1] = bv.uv[1];
                    v[k].argb = bv.argb;
                    v[k].blend = draw->blend;
                    v[k].matalpha = draw->alpha;
                    v[k].alphatest = draw->alphatest;
                    v[k].alpharef = draw->alpharef;
                    v[k].plain = (uint8_t)plain;
                    v[k].draw = (uint16_t)d;
                    v[k].bone = bv.slot >= 0 && bv.slot < draw->numpal ? draw->pal[bv.slot] : -1;
                    v[k].kind = kind;
                    v[k].ownmat = draw->ownmat;
                    v[k].piece = draw->piece;
                    memcpy(v[k].nrm, bv.nrm, sizeof(v[k].nrm));
                }
            }
            if (!ok) {
                continue;
            }
            /* round 26a (D-146): a sphere-mapped draw hands over its own
             * picture (its reflection map); the caller draws it by texgen */
            fn(arg, plain && kind != BEANDRAW_SPHEREMAP ? -1 : tex, v);
            count++;
        }
        free(tris);
    }
    return count;
}

void beanModelInfo(const struct beanlevel *bm, int *numdraws, int *numtex, float *uvscale)
{
    *numdraws = bm ? bm->numdraws : 0;
    *numtex = bm ? bm->numtex : 0;
    *uvscale = bm ? bm->uvscale : 0.0f;
}

/* =================================================================
 * Characters (round 26b, Tier C C4 part 2): the skeleton and the skin.
 * ================================================================= */

/*
 * PD beanLoad()'s name pool + beanReadPose(): asset 2's .data names the bones
 * in pose order among its other strings (SKEL_* and BEAN_* entries); the
 * 'pose' record in asset 1's .data holds a count at +0x18 and its entries at
 * the offset in +0x34, 52 bytes each - a local translation, the absolute bind
 * (+12), a spare, 1.0, then parent (+40) / child, sibling / self.
 */
int beanModelSkeleton(const struct beanlevel *bm, struct beanskel *out)
{
    memset(out, 0, sizeof(*out));
    if (!bm) {
        return 0;
    }
    uint32_t poollen = 0;
    const uint8_t *pool = beanSection(bm->bf, 2, ".data", &poollen);
    int numnames = 0;
    const char *names[BEAN_MAXBONES];
    for (uint32_t at = 0; pool && at < poollen && numnames < BEAN_MAXBONES;) {
        const uint8_t *nul = memchr(pool + at, '\0', poollen - at);
        const uint32_t end = nul ? (uint32_t)(nul - pool) : poollen;
        if (nul && end - at > 5 && (memcmp(pool + at, "SKEL_", 5) == 0 || memcmp(pool + at, "BEAN_", 5) == 0)) {
            names[numnames++] = (const char *)pool + at;
        }
        at = end + 1;
    }
    const uint8_t *d = bm->data;
    uint32_t at;
    for (at = 0; at + 0x38 <= bm->datalen; at += 4) {
        if (memcmp(d + at, "pose\0\0\0\0", 8) == 0) {
            break;
        }
    }
    if (at + 0x38 > bm->datalen) {
        return 0;
    }
    uint32_t count = be32(d + at + 0x18);
    const uint32_t entries = be32(d + at + 0x34);
    if (count > BEAN_MAXBONES) {
        count = BEAN_MAXBONES;
    }
    for (uint32_t i = 0; i < count && fits(entries + 52ull * i, 52, bm->datalen); i++) {
        const uint8_t *e = d + entries + 52 * i;
        const uint16_t up = be16(e + 40);
        out->parent[i] = up == 0xffff || up >= count ? -1 : (int16_t)up;
        for (int k = 0; k < 3; k++) {
            out->bind[i][k] = bef32(e + 12 + k * 4);
        }
        if ((int)i < numnames) {
            snprintf(out->name[i], sizeof(out->name[i]), "%s", names[i]);
        }
        out->numbones = (int)i + 1;
    }
    return out->numbones;
}

/*
 * Every triangle of a character or head file with each corner's pose bones
 * and weights (PD beanVertex() for strides 28/32/36: four u16 palette slots,
 * slot / 3 into the draw's 0x13 palette, through the 0x12 remap to a pose
 * bone; weights on stride 36 as bytes 23..20, a single bone otherwise). A
 * draw's piece past 1 is left out as for props. Round 32c (D-300): each
 * corner's normal is passed too (lit characters).
 */
int beanModelSkinTriangles(struct beanlevel *bm,
                           void (*fn)(void *arg, int tex, const struct beanskinvtx *v), void *arg)
{
    int count = 0;

    for (int d = 0; d < bm->numdraws; d++) {
        const struct beandraw *draw = &bm->draws[d];
        struct beanvb vb;
        uint32_t *tris = NULL;
        const int tex = draw->tex < (uint32_t)bm->numtex ? (int)draw->tex : -1;

        if (draw->piece > 1 || !readVb(bm, draw->vb, &vb) || vb.stride == 12) {
            continue;
        }
        const int plain = modelPlain(bm, &vb);
        const uint8_t kind = (uint8_t)classifyDraw(bm, draw, plain,
                                                   plain && draw->ownmat && draw->tex < (uint32_t)bm->numtex);
        const int numtris = drawTriangles(bm, draw, &tris);
        for (int t = 0; t < numtris; t++) {
            struct beanskinvtx v[3];
            int ok = 1;
            memset(v, 0, sizeof(v));
            for (int k = 0; k < 3 && ok; k++) {
                struct beanvtx bv;
                const uint32_t vi = tris[t * 3 + k];
                ok = modelVertex(bm, &vb, vi, &bv);
                for (int j = 0; j < 3 && ok; j++) {
                    ok = bv.pos[j] == bv.pos[j] && bv.pos[j] > -1e8f && bv.pos[j] < 1e8f;
                }
                if (!ok) {
                    break;
                }
                memcpy(v[k].pos, bv.pos, sizeof(v[k].pos));
                v[k].uv[0] = bv.uv[0];
                v[k].uv[1] = bv.uv[1];
                v[k].argb = bv.argb;
                v[k].blend = draw->blend;
                v[k].matalpha = draw->alpha;
                v[k].plain = (uint8_t)plain;
                v[k].kind = kind;
                v[k].ownmat = draw->ownmat;
                v[k].draw = (uint16_t)d;
                v[k].vindex = vi;
                memcpy(v[k].nrm, bv.nrm, sizeof(v[k].nrm));   /* round 32c (D-300) */
                for (int s = 0; s < 4; s++) {
                    v[k].bone[s] = -1;
                    v[k].weight[s] = 0;
                }
                if (vb.stride == 28 || vb.stride == 32 || vb.stride == 36) {
                    const uint8_t *p = bm->gpu + vb.off + (size_t)vi * vb.stride;
                    for (int s = 0; s < 4; s++) {
                        const uint16_t sl = be16(p + 12 + s * 2);
                        const int slot = sl == 0xf000 ? -1 : sl / 3;
                        const uint8_t w = vb.stride == 36 ? p[23 - s] : (s == 0 ? 255 : 0);
                        if (slot < 0 || slot >= draw->numpal || w == 0) {
                            continue;
                        }
                        int bone = draw->pal[slot];
                        if (bm->numremap && bone < bm->numremap) {
                            bone = bm->remap[bone];
                        }
                        v[k].bone[s] = (int16_t)bone;
                        v[k].weight[s] = w;
                    }
                }
            }
            if (!ok) {
                continue;
            }
            fn(arg, plain ? -1 : tex, v);
            count++;
        }
        free(tris);
    }
    return count;
}

int beanModelStrides(const struct beanlevel *bm, int *hist, int max)
{
    int n = 0;
    for (int d = 0; d < bm->numdraws; d++) {
        struct beanvb vb;
        if (readVb(bm, bm->draws[d].vb, &vb) && vb.stride < (uint32_t)max) {
            hist[vb.stride]++;
            n++;
        }
    }
    return n;
}

int beanModelPoseBone(const struct beanlevel *bm, int bone)
{
    if (bone >= 0 && bm->numremap && bone < bm->numremap) {
        return bm->remap[bone];
    }
    return bone;
}
