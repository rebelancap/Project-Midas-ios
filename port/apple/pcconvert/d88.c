/* Imported from GoldenEye-Decomp-ios engine/port/src/pcconvert/d88.c at 55a3a9c8
 * (donor HEAD), Phase 1 round 7a
 * (docs/port-ios/11-round7a-pcconvert.md). Layouts pinned by
 * pcconvert_layout.c. CHANGED for Midas: the CREDITS intro's CreditsEntry
 * table is converted and its offset relocated (R7A-2, a donor bug). */
/*
 * D88: C port of tools_pc/d88_emit.py — convert the per-level Usetup*Z stage
 * setup files to PC layout and append them to the same pccg image d69 built.
 * The Python docstring is the format spec; the region-tiling + cumulative
 * delta algorithm is reproduced one-for-one here.
 */

#include <stdlib.h>
#include <string.h>


#include "pcconvert_internal.h"

enum {
    F_PATHWAYPOINTS = 0, F_WAYPOINTGROUPS, F_INTRO, F_PROPDEFS,
    F_PATROLPATHS, F_AILISTS, F_PADS, F_BOUNDPADS, F_PADNAMES,
    F_BOUNDPADNAMES, F_COUNT
};

static const short INTRO_SZ[10] = { 12, 16, 16, 32, 8, 8, 40, 12, 8, 4 };

/* region kinds */
enum {
    K_HEADER = 0, K_GROW, K_INTRO, K_PROPDEFS, K_LEAF_S32, K_LEAF_CSTR,
    K_LEAF_AISTREAM, K_GAP,
    K_LEAF_CREDITS   /* Midas R7A-2: the CREDITS intro's CreditsEntry table */
};

struct D88Region {
    uint32_t start;
    int64_t  end;      /* -1 = resolve to the next region's start */
    int      kind;
    int      grow;     /* index into the GROW_TABLES array, or -1 */
    int      seq;      /* insertion index, for a stable sort */
};

struct GrowTable {
    int       field;
    uint32_t *starts;
    int       nStarts;
    int64_t   term;    /* -1 = absent */
    int       oldsz, newsz;
};

/* GROW_TABLES order, as in the Python */
static const struct { int field; int recsize; int termOff; int termNeg;
                      int oldsz; int newsz; } GROW_DEF[8] = {
    { F_PADS,          44, 0x24, 0, 44, 56 },
    { F_BOUNDPADS,     68, 0x24, 0, 68, 80 },
    { F_WAYPOINTGROUPS,12, 0,    0, 12, 24 },
    { F_PATHWAYPOINTS, 16, 0,    1, 16, 24 },
    { F_PATROLPATHS,    8, 0,    0,  8, 16 },
    { F_AILISTS,        8, 0,    0,  8, 16 },
    { F_PADNAMES,       4, 0,    0,  4,  8 },
    { F_BOUNDPADNAMES,  4, 0,    0,  4,  8 },
};

static int cmpRegion88(const void *a, const void *b)
{
    const struct D88Region *x = (const struct D88Region *)a;
    const struct D88Region *y = (const struct D88Region *)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

struct D88Ctx {
    const char    *name;
    const uint8_t *src;
    size_t         D;
    uint32_t       H[F_COUNT];
    int64_t       *deltaAt;    /* file offset -> cumulative delta, -1 unset */
};

static uint32_t d88Reloc(const struct D88Ctx *c, uint32_t off)
{
    if (off == 0)
        return 0;
    if ((size_t)off < c->D && c->deltaAt[off] >= 0)
        return off + (uint32_t)c->deltaAt[off];
    return off;
}

/* Python walk_fixed(): collect record starts up to (excluding) the terminator */
static int d88WalkFixed(struct D88Ctx *c, uint32_t off, int recsize,
                        int termOff, int termNeg,
                        uint32_t **startsOut, int *nOut, int64_t *termOut)
{
    uint32_t *starts;
    int n = 0, guard = 0;
    uint32_t o = off;

    *startsOut = NULL; *nOut = 0; *termOut = -1;
    if (off == 0)
        return 1;
    starts = (uint32_t *)malloc((size_t)(sizeof(uint32_t) *
                               (c->D / (size_t)recsize + 2)));
    if (!starts)
        return 0;
    while (guard < 100000) {
        int term;
        guard++;
        if ((size_t)o + (size_t)termOff + 4 > c->D) {
            pcvErrf("%s: walk_fixed ran past EOF at %#x", c->name, o);
            free(starts);
            return 0;
        }
        if (termNeg)
            term = pcvBES32(c->src, o + (uint32_t)termOff) < 0;
        else
            term = pcvBE32(c->src, o + (uint32_t)termOff) == 0;
        if (term) {
            *startsOut = starts;
            *nOut = n;
            *termOut = (int64_t)o;
            return 1;
        }
        starts[n++] = o;
        o += (uint32_t)recsize;
    }
    pcvErrf("%s: runaway walk_fixed at %#x", c->name, off);
    free(starts);
    return 0;
}

static int64_t d88WalkS32Arr(struct D88Ctx *c, uint32_t off)
{
    uint32_t o = off;
    int guard = 0;
    while (guard < 100000) {
        guard += 2;
        if ((size_t)o + 4 > c->D)
            break;
        if (pcvBES32(c->src, o) < 0)
            return (int64_t)o + 4;
        o += 4;
    }
    pcvErrf("%s: runaway s32 array at %#x", c->name, off);
    return -1;
}

static int64_t d88WalkCStr(struct D88Ctx *c, uint32_t off)
{
    size_t o;
    for (o = off; o < c->D; o++)
        if (c->src[o] == 0)
            return (int64_t)o + 1;
    pcvErrf("%s: unterminated string at %#x", c->name, off);
    return -1;
}

static void d88EmitPad(uint8_t *out, const struct D88Ctx *c,
                       uint32_t dstO, uint32_t srcO, int hasBbox)
{
    int k;
    for (k = 0; k < 9; k++)
        pcvPut32(out, dstO + 4u * (uint32_t)k,
                 pcvBE32(c->src, srcO + 4u * (uint32_t)k));
    pcvPut32(out, dstO + 0x28, d88Reloc(c, pcvBE32(c->src, srcO + 0x24)));
    pcvPut32(out, dstO + 0x2c, 0);
    memset(out + dstO + 0x30, 0, 8);   /* stan: dead-on-load */
    if (hasBbox) {
        for (k = 0; k < 6; k++)
            pcvPut32(out, dstO + 0x38 + 4u * (uint32_t)k,
                     pcvBE32(c->src, srcO + 0x2c + 4u * (uint32_t)k));
    }
}

static uint8_t *d88ConvertUsetup(const char *name, const uint8_t *src, size_t D,
                                 size_t *outLen)
{
    struct D88Ctx c;
    struct GrowTable gt[8];
    struct D88Region *regs = NULL;
    int nRegs = 0, capRegs = 0, seq = 0;
    struct D88Region *tiled = NULL;
    int nTiled = 0;
    uint32_t *leafStart = NULL;
    int      *leafKind = NULL;
    int64_t  *leafEnd = NULL;
    int nLeaf = 0, capLeaf = 0;
    uint32_t *introOff = NULL;
    int      *introType = NULL;
    int nIntro = 0;
    uint32_t introEnd;
    uint8_t *propPc = NULL;
    size_t propPcLen = 0;
    uint8_t *out = NULL;
    int64_t cum;
    size_t totalNew;
    int i, j, g;

    memset(&c, 0, sizeof(c));
    memset(gt, 0, sizeof(gt));
    c.name = name; c.src = src; c.D = D;

    if (D < 0x28) {
        pcvErrf("%s: too small (%zu)", name, D);
        return NULL;
    }
    for (i = 0; i < F_COUNT; i++) {
        c.H[i] = pcvBE32(src, 4u * (uint32_t)i);
        if (c.H[i] != 0 && (size_t)c.H[i] >= D) {
            pcvErrf("%s: header field %d offset %#x >= filesize %#zx",
                    name, i, c.H[i], D);
            return NULL;
        }
    }
    c.deltaAt = (int64_t *)malloc((size_t)(sizeof(int64_t) * (D + 1)));
    if (!c.deltaAt)
        return NULL;
    for (i = 0; i <= (int)D; i++)
        c.deltaAt[i] = -1;

    /* ---- the 8 growing tables ---- */
    for (g = 0; g < 8; g++) {
        gt[g].field = GROW_DEF[g].field;
        gt[g].oldsz = GROW_DEF[g].oldsz;
        gt[g].newsz = GROW_DEF[g].newsz;
        if (!d88WalkFixed(&c, c.H[GROW_DEF[g].field], GROW_DEF[g].recsize,
                          GROW_DEF[g].termOff, GROW_DEF[g].termNeg,
                          &gt[g].starts, &gt[g].nStarts, &gt[g].term))
            goto fail;
    }

    /* ---- intro: type-tag walk ---- */
    introEnd = c.H[F_INTRO];
    if (c.H[F_INTRO]) {
        uint32_t o = c.H[F_INTRO];
        int guard = 0;
        introOff  = (uint32_t *)malloc((size_t)(sizeof(uint32_t) * 5001));
        introType = (int *)malloc((size_t)(sizeof(int) * 5001));
        if (!introOff || !introType) goto fail;
        while (guard < 5000) {
            int32_t t;
            guard++;
            if ((size_t)o + 4 > D) {
                pcvErrf("%s: intro walk past EOF", name);
                goto fail;
            }
            t = pcvBES32(src, o);
            if (t < 0 || t > 9) {
                pcvErrf("%s: unknown intro type %d at %#x", name, t, o);
                goto fail;
            }
            introOff[nIntro] = o;
            introType[nIntro] = t;
            nIntro++;
            o += (uint32_t)INTRO_SZ[t];
            if (t == 9)
                break;
        }
        if (guard >= 5000) {
            pcvErrf("%s: intro walk runaway", name);
            goto fail;
        }
        introEnd = o;
    }

    /* ---- leaf sub-regions ---- */
    capLeaf = 4096;
    leafStart = (uint32_t *)malloc((size_t)(sizeof(uint32_t) * (size_t)capLeaf));
    leafKind  = (int *)malloc((size_t)(sizeof(int) * (size_t)capLeaf));
    leafEnd   = (int64_t *)malloc((size_t)(sizeof(int64_t) * (size_t)capLeaf));
    if (!leafStart || !leafKind || !leafEnd) goto fail;

#define ADD_LEAF(st, kind)                                                    \
    do {                                                                      \
        uint32_t _s = (st);                                                   \
        int _k = (kind), _dup = 0, _q;                                        \
        if (_s) {                                                             \
            for (_q = 0; _q < nLeaf; _q++)                                    \
                if (leafStart[_q] == _s) { _dup = 1; break; }                 \
            if (!_dup) {                                                      \
                int64_t _e = (_k == K_LEAF_S32) ? d88WalkS32Arr(&c, _s)       \
                                                : d88WalkCStr(&c, _s);        \
                if (_e >= 0) {                                                \
                    if (nLeaf >= capLeaf) goto fail;                          \
                    leafStart[nLeaf] = _s; leafKind[nLeaf] = _k;              \
                    leafEnd[nLeaf] = _e; nLeaf++;                             \
                }                                                             \
            }                                                                 \
        }                                                                     \
    } while (0)

    for (i = 0; i < gt[3].nStarts; i++)   /* pathwaypoints */
        ADD_LEAF(pcvBE32(src, gt[3].starts[i] + 4), K_LEAF_S32);
    for (i = 0; i < gt[2].nStarts; i++) { /* waypointgroups */
        ADD_LEAF(pcvBE32(src, gt[2].starts[i] + 0), K_LEAF_S32);
        ADD_LEAF(pcvBE32(src, gt[2].starts[i] + 4), K_LEAF_S32);
    }
    for (i = 0; i < gt[4].nStarts; i++)   /* patrolpaths */
        ADD_LEAF(pcvBE32(src, gt[4].starts[i] + 0), K_LEAF_S32);
    for (i = 0; i < gt[0].nStarts; i++)   /* pads */
        ADD_LEAF(pcvBE32(src, gt[0].starts[i] + 0x24), K_LEAF_CSTR);
    for (i = 0; i < gt[1].nStarts; i++)   /* boundpads */
        ADD_LEAF(pcvBE32(src, gt[1].starts[i] + 0x24), K_LEAF_CSTR);
    for (i = 0; i < gt[6].nStarts; i++)   /* padnames */
        ADD_LEAF(pcvBE32(src, gt[6].starts[i]), K_LEAF_CSTR);
    for (i = 0; i < gt[7].nStarts; i++)   /* boundpadnames */
        ADD_LEAF(pcvBE32(src, gt[7].starts[i]), K_LEAF_CSTR);
#undef ADD_LEAF

    /* Midas round 7a (R7A-2; not in the donor): the CREDITS intro record's
     * word 1 is a file offset to a CreditsEntry table -- six u16/s16 per
     * 12-byte entry, ending with TextId1 == TextId2 == 0 (bondview_r.c,
     * bondview2.c). The donor leaves the offset unrelocated and the table in
     * a gap (copied big-endian), so Cuba's end credits read the wrong bytes.
     * Make it a leaf: halfword-swapped, and the intro word relocated below. */
    for (i = 0; i < nIntro; i++) {
        uint32_t cs, ce;
        int q, dup = 0;
        if (introType[i] != 8)
            continue;
        cs = pcvBE32(src, introOff[i] + 4);
        if (!cs || (size_t)cs >= D)
            continue;
        for (q = 0; q < nLeaf; q++)
            if (leafStart[q] == cs) { dup = 1; break; }
        if (dup)
            continue;
        for (ce = cs; (size_t)ce + 12 <= D; ce += 12)
            if (!pcvBE16(src, ce) && !pcvBE16(src, ce + 2)) { ce += 12; break; }
        if ((size_t)ce > D || nLeaf >= capLeaf) {
            pcvErrf("%s: CREDITS table at %#x unterminated", name, cs);
            goto fail;
        }
        leafStart[nLeaf] = cs; leafKind[nLeaf] = K_LEAF_CREDITS;
        leafEnd[nLeaf] = (int64_t)ce; nLeaf++;
    }

    /* ---- assemble the region list ---- */
    capRegs = nLeaf + gt[5].nStarts + 32;
    regs = (struct D88Region *)malloc((size_t)
            (sizeof(struct D88Region) * (size_t)capRegs));
    if (!regs) goto fail;

#define ADD_REGION(st, en, kd, gi)                                            \
    do {                                                                      \
        if (nRegs >= capRegs) goto fail;                                      \
        regs[nRegs].start = (st); regs[nRegs].end = (en);                     \
        regs[nRegs].kind = (kd); regs[nRegs].grow = (gi);                     \
        regs[nRegs].seq = seq++; nRegs++;                                     \
    } while (0)

    ADD_REGION(0, 0x28, K_HEADER, -1);
    for (g = 0; g < 8; g++) {
        if (gt[g].term < 0)
            continue;
        ADD_REGION(c.H[gt[g].field],
                   gt[g].term + gt[g].oldsz, K_GROW, g);
    }
    if (c.H[F_INTRO])
        ADD_REGION(c.H[F_INTRO], (int64_t)introEnd, K_INTRO, -1);
    if (c.H[F_PROPDEFS])
        ADD_REGION(c.H[F_PROPDEFS], -1, K_PROPDEFS, -1);
    for (i = 0; i < nLeaf; i++)
        ADD_REGION(leafStart[i], leafEnd[i], leafKind[i], -1);
    for (i = 0; i < gt[5].nStarts; i++) {   /* ailists: opcode streams */
        uint32_t s = pcvBE32(src, gt[5].starts[i]);
        int dup = 0;
        if (!s) continue;
        for (j = 0; j < nLeaf; j++)
            if (leafStart[j] == s) { dup = 1; break; }
        if (dup) continue;
        for (j = 0; j < nRegs; j++)
            if (regs[j].kind == K_LEAF_AISTREAM && regs[j].start == s) { dup = 1; break; }
        if (dup) continue;
        ADD_REGION(s, -1, K_LEAF_AISTREAM, -1);
    }
#undef ADD_REGION

    qsort(regs, (size_t)nRegs, sizeof(struct D88Region), cmpRegion88);
    for (i = 0; i < nRegs; i++)
        if (regs[i].end < 0)
            regs[i].end = (i + 1 < nRegs) ? (int64_t)regs[i + 1].start : (int64_t)D;

    /* ---- tile ---- */
    tiled = (struct D88Region *)malloc((size_t)
             (sizeof(struct D88Region) * (size_t)(nRegs * 2 + 4)));
    if (!tiled) goto fail;
    {
        uint32_t cursor = 0;
        for (i = 0; i < nRegs; i++) {
            if (regs[i].start < cursor) {
                pcvErrf("%s: region overlap before kind%d@%#x (cursor was %#x)",
                        name, regs[i].kind, regs[i].start, cursor);
                goto fail;
            }
            if (regs[i].start > cursor) {
                tiled[nTiled].start = cursor;
                tiled[nTiled].end = (int64_t)regs[i].start;
                tiled[nTiled].kind = K_GAP;
                tiled[nTiled].grow = -1;
                nTiled++;
            }
            if (regs[i].end < (int64_t)regs[i].start) {
                pcvErrf("%s: region kind%d@%#x has negative length",
                        name, regs[i].kind, regs[i].start);
                goto fail;
            }
            tiled[nTiled++] = regs[i];
            cursor = (uint32_t)regs[i].end;
        }
        if ((size_t)cursor < D) {
            tiled[nTiled].start = cursor;
            tiled[nTiled].end = (int64_t)D;
            tiled[nTiled].kind = K_GAP;
            tiled[nTiled].grow = -1;
            nTiled++;
        } else if ((size_t)cursor > D) {
            pcvErrf("%s: regions overrun EOF (%#x > %#zx)", name, cursor, D);
            goto fail;
        }
    }

    /* ---- propDefs record stream (D88.4) ---- */
    if (c.H[F_PROPDEFS]) {
        int64_t pdEnd = -1;
        for (i = 0; i < nTiled; i++)
            if (tiled[i].kind == K_PROPDEFS) { pdEnd = tiled[i].end; break; }
        propPc = pcvPropDefsConvert(src, c.H[F_PROPDEFS], (size_t)pdEnd,
                                    &propPcLen, name);
        if (!propPc)
            goto fail;
    }

    /* ---- pass 1: cumulative delta at each region start ---- */
    cum = 0x28;
    for (i = 0; i < nTiled; i++) {
        uint32_t st = tiled[i].start;
        if (tiled[i].kind == K_HEADER) {
            c.deltaAt[st] = 0;
            continue;
        }
        c.deltaAt[st] = cum;
        if (tiled[i].kind == K_GROW) {
            const struct GrowTable *t = &gt[tiled[i].grow];
            cum += (int64_t)(t->nStarts + 1) * (t->newsz - t->oldsz);
        } else if (tiled[i].kind == K_PROPDEFS) {
            cum += (int64_t)propPcLen - (tiled[i].end - (int64_t)st);
        }
    }

    totalNew = D + (size_t)cum;
    out = (uint8_t *)malloc((size_t)totalNew);
    if (!out) goto fail;
    memset(out, 0, totalNew);

    /* ---- header 0x28 -> 0x50 ---- */
    for (i = 0; i < F_COUNT; i++) {
        pcvPut32(out, 8u * (uint32_t)i, d88Reloc(&c, c.H[i]));
        pcvPut32(out, 8u * (uint32_t)i + 4, 0);
    }

    /* ---- growing tables ---- */
    for (g = 0; g < 8; g++) {
        const struct GrowTable *t = &gt[g];
        uint32_t tableBase;
        int idx, nAll;
        if (t->term < 0)
            continue;
        tableBase = d88Reloc(&c, c.H[t->field]);
        nAll = t->nStarts + 1;
        for (idx = 0; idx < nAll; idx++) {
            uint32_t so = (idx < t->nStarts) ? t->starts[idx] : (uint32_t)t->term;
            uint32_t doo = tableBase + (uint32_t)(idx * t->newsz);
            switch (t->field) {
            case F_PADS:
                d88EmitPad(out, &c, doo, so, 0);
                break;
            case F_BOUNDPADS:
                d88EmitPad(out, &c, doo, so, 1);
                break;
            case F_WAYPOINTGROUPS: {
                uint32_t nb = pcvBE32(src, so + 0);
                uint32_t wpv = pcvBE32(src, so + 4);
                uint32_t dist = pcvBE32(src, so + 8);
                pcvPut32(out, doo, nb ? d88Reloc(&c, nb) : 0);
                pcvPut32(out, doo + 4, 0);
                pcvPut32(out, doo + 8, wpv ? d88Reloc(&c, wpv) : 0);
                pcvPut32(out, doo + 12, 0);
                pcvPut32(out, doo + 16, dist);
                pcvPut32(out, doo + 20, 0);
                break;
            }
            case F_PATHWAYPOINTS: {
                uint32_t padID = pcvBE32(src, so + 0);
                uint32_t nb = pcvBE32(src, so + 4);
                uint32_t groupNum = pcvBE32(src, so + 8);
                uint32_t dist = pcvBE32(src, so + 12);
                pcvPut32(out, doo, padID);
                pcvPut32(out, doo + 4, 0);
                pcvPut32(out, doo + 8, nb ? d88Reloc(&c, nb) : 0);
                pcvPut32(out, doo + 12, 0);
                pcvPut32(out, doo + 16, groupNum);
                pcvPut32(out, doo + 20, dist);
                break;
            }
            case F_PATROLPATHS: {
                uint32_t wpv = pcvBE32(src, so + 0);
                pcvPut32(out, doo, wpv ? d88Reloc(&c, wpv) : 0);
                pcvPut32(out, doo + 4, 0);
                out[doo + 8] = src[so + 4];
                out[doo + 9] = src[so + 5];
                pcvPut16(out, doo + 10, pcvBE16(src, so + 6));
                pcvPut32(out, doo + 12, 0);
                break;
            }
            case F_AILISTS: {
                uint32_t al = pcvBE32(src, so + 0);
                uint32_t ID = pcvBE32(src, so + 4);
                pcvPut32(out, doo, al ? d88Reloc(&c, al) : 0);
                pcvPut32(out, doo + 4, 0);
                pcvPut32(out, doo + 8, ID);
                pcvPut32(out, doo + 12, 0);
                break;
            }
            case F_PADNAMES:
            case F_BOUNDPADNAMES: {
                uint32_t p = pcvBE32(src, so);
                pcvPut32(out, doo, p ? d88Reloc(&c, p) : 0);
                pcvPut32(out, doo + 4, 0);
                break;
            }
            default:
                break;
            }
        }
    }

    /* ---- intro (bswap, same record size) ---- */
    if (c.H[F_INTRO]) {
        int64_t baseDelta = c.deltaAt[c.H[F_INTRO]];
        if (baseDelta < 0) baseDelta = 0;
        for (i = 0; i < nIntro; i++) {
            uint32_t o = introOff[i];
            uint32_t doo = o + (uint32_t)baseDelta;
            int t = introType[i];
            int sz = INTRO_SZ[t];
            if (t == 6) {
                for (j = 0; j < 10; j++) {
                    uint32_t dk = doo + 4u * (uint32_t)j;
                    if (j == 7) {
                        pcvPut16(out, dk, pcvBE16(src, o + 4u * (uint32_t)j));
                        pcvPut16(out, dk + 2, pcvBE16(src, o + 4u * (uint32_t)j + 2));
                    } else {
                        pcvPut32(out, dk, pcvBE32(src, o + 4u * (uint32_t)j));
                    }
                }
            } else if (t == 8) {
                /* Midas R7A-2: relocate the CreditsEntry table offset */
                pcvPut32(out, doo, pcvBE32(src, o));
                pcvPut32(out, doo + 4, d88Reloc(&c, pcvBE32(src, o + 4)));
            } else {
                for (j = 0; j < sz / 4; j++)
                    pcvPut32(out, doo + 4u * (uint32_t)j,
                             pcvBE32(src, o + 4u * (uint32_t)j));
            }
        }
    }

    /* ---- leaves ---- */
    for (i = 0; i < nLeaf; i++) {
        uint32_t start = leafStart[i];
        uint32_t end = (uint32_t)leafEnd[i];
        uint32_t doo = d88Reloc(&c, start);
        if (leafKind[i] == K_LEAF_S32) {
            uint32_t o;
            for (o = start; o < end; o += 4)
                pcvPut32(out, doo + (o - start), pcvBE32(src, o));
        } else if (leafKind[i] == K_LEAF_CREDITS) {
            uint32_t o;
            for (o = start; o < end; o += 2)
                pcvPut16(out, doo + (o - start), pcvBE16(src, o));
        } else {
            memcpy(out + doo, src + start, end - start);
        }
    }

    /* ---- propdefs + aistreams + gaps verbatim ---- */
    for (i = 0; i < nTiled; i++) {
        uint32_t start = tiled[i].start;
        uint32_t end = (uint32_t)tiled[i].end;
        uint32_t doo = d88Reloc(&c, start);
        if (tiled[i].kind == K_PROPDEFS) {
            if (propPcLen)
                memcpy(out + doo, propPc, propPcLen);
        } else if (tiled[i].kind == K_LEAF_AISTREAM || tiled[i].kind == K_GAP) {
            if (end > start)
                memcpy(out + doo, src + start, end - start);
        }
    }

    *outLen = totalNew;
    free(c.deltaAt);
    for (g = 0; g < 8; g++) free(gt[g].starts);
    free(regs); free(tiled);
    free(leafStart); free(leafKind); free(leafEnd);
    free(introOff); free(introType);
    free(propPc);
    return out;

fail:
    free(out);
    free(c.deltaAt);
    for (g = 0; g < 8; g++) free(gt[g].starts);
    free(regs); free(tiled);
    free(leafStart); free(leafKind); free(leafEnd);
    free(introOff); free(introType);
    free(propPc);
    return NULL;
}

/* ------------------------------------------------------------------ main ---*/
static int cmpStrPtr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int pcvD88(const uint8_t *rom, size_t romLen, PcvImage *im)
{
    const char **names;
    int t, n = 0, nOk = 0, i;

    names = (const char **)malloc((size_t)
            (sizeof(char *) * (size_t)(pcconvTableNameCount + 1)));
    if (!names)
        return 0;
    for (t = 0; t < pcconvTableNameCount; t++) {
        const char *nm = pcconvTableNames[t];
        size_t l = strlen(nm);
        int dup = 0;
        /* Round 29a (D-205): the multiplayer setups Ump_setup*Z are the
         * same struct stagesetup format; the old "Usetup"-only filter left
         * them N64-layout and every MP match died in proplvreset2 (D-179). */
        if ((strncmp(nm, "Usetup", 6) && strncmp(nm, "Ump_setup", 9)) ||
            l == 0 || nm[l - 1] != 'Z')
            continue;
        for (i = 0; i < n; i++)
            if (!strcmp(names[i], nm)) { dup = 1; break; }
        if (!dup)
            names[n++] = nm;
    }
    qsort(names, (size_t)n, sizeof(char *), cmpStrPtr);

    for (i = 0; i < n; i++) {
        const char *name = names[i];
        const struct PcvFileRow *row = pcvFindRowExact(name);
        uint8_t *dec, *out, *comp;
        size_t decLen = 0, outLen = 0, compLen = 0;

        if (!row || row->size == 0)
            continue;
        if ((size_t)row->addr + row->size > romLen) {
            pcvErrf("%s: filelist row past EOF", name);
            continue;
        }
        if (rom[row->addr] != 0x11 || rom[row->addr + 1] != 0x72) {
            pcvErrf("%s: bad RZ magic %02x%02x", name,
                    rom[row->addr], rom[row->addr + 1]);
            continue;
        }
        dec = pcvInflateRZ(rom + row->addr, row->size, &decLen);
        if (!dec) {
            pcvErrf("%s: decompress failed", name);
            continue;
        }
        out = d88ConvertUsetup(name, dec, decLen, &outLen);
        free(dec);
        if (!out)
            continue;
        comp = pcvDeflateRZ(out, outLen, &compLen);
        free(out);
        if (!comp)
            continue;
        pcvImageEmit(im, name, comp, compLen);
        free(comp);
        nOk++;
    }

    pcvLog("pcconvert: d88 converted %d/%d Usetup*Z + Ump_setup*Z files", nOk, n);
    free(names);
    return pcvErrCount() ? 0 : 1;
}
