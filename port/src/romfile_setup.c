/**
 * romfile_setup.c - the setup-file preprocess pass.
 *
 * A setup file (Usetup<lvl>Z / Ump_setup<lvl>Z) arrives from the ROM as the N64
 * memory image: big-endian, with every internal reference a file offset. The
 * game's loader (prop.c) rebases the offsets itself, so this pass only fixes
 * byte order, producing the same host layout the compiled-in setups
 * (assets/obseg/setup/*.c) have so the rest of the game is unchanged.
 *
 * That layout is not "swap every word": the setup generator wrapped some words
 * in _mkword()/_mkshort() (byte order kept) and wrote others as plain u32s. So
 * each word of each record type has a kind:
 *   K32  native u32/f32/offset     K16  two u16
 *   K8   four bytes                KSK  a u16 then two bytes
 *   KKS  two bytes then a u16
 * The per-type tables are derived from data, not hand-written:
 * scripts/port/gen_setup_kinds.py compares the compiled objects with the ROM
 * files for all 34 retail setups and writes port/generated/setup_kinds.h.
 * The fixed-layout sections (pads, paths, name tables...) are hard-coded below
 * and checked by the same script.
 *
 * Special cases handled here:
 *  - propdef word 0 is always {u16 scale; u8 hidden; u8 type} (KSK),
 *    whatever the table says.
 *  - the CREDITS intro record's word 1 is a file offset to a CreditsEntry
 *    table; bondview_r.c needs a real pointer, so it is resolved here (the
 *    only pointer this pass resolves) and the table is swapped up to its
 *    {0,0} terminator.
 *  - UsetupsevbunkerZ.c's OBJECTIVE_DEPOSIT_OBJECT record (index 13) has a
 *    wrong header in the compiled-in data (reads as type 0); the ROM is
 *    correct, so nothing is needed.
 *
 * Probe (GE_ROMDATA_FALLBACK builds): while the compiled-in setup is still
 * linked, every section of the converted image is compared against it,
 * following pointers on both sides. "identical" means the conversion is
 * correct; a modded ROM is expected to report DIFFERS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "bondtypes.h"
#include "../generated/setup_kinds.h"

/* port_console.c: prints to stderr like the fprintf it replaced, and also
 * keeps a silent copy in the tilde console's log. */
extern void portConLogf(const char *fmt, ...);

extern s32 sizepropdef(PropDefHeaderRecord *pdef); /* loadobjectmodel.c: size in words by type */

#define SETUP_HDR_WORDS 10 /* struct stagesetup: 10 offsets */
enum { H_WP = 0, H_WG, H_INTRO, H_PD, H_PATHS, H_AI, H_PADS, H_BPADS, H_PN, H_BPN };

static u32 be32(const u8 *p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | (u32) p[3];
}

static void kindApply(u8 *w, int kind)
{
    u8 t;
    switch (kind) {
    case SETUP_KIND_K32:
        t = w[0]; w[0] = w[3]; w[3] = t;
        t = w[1]; w[1] = w[2]; w[2] = t;
        break;
    case SETUP_KIND_K16:
        t = w[0]; w[0] = w[1]; w[1] = t;
        t = w[2]; w[2] = w[3]; w[3] = t;
        break;
    case SETUP_KIND_KSK:
        t = w[0]; w[0] = w[1]; w[1] = t;
        break;
    case SETUP_KIND_KKS:
        t = w[2]; w[2] = w[3]; w[3] = t;
        break;
    default:
        break; /* K8 */
    }
}

static void k32Run(u8 *p, u32 words)
{
    while (words--) {
        kindApply(p, SETUP_KIND_K32);
        p += 4;
    }
}

struct conv {
    u8 *d;
    u32 size;
    const char *name;
    u32 hdr[SETUP_HDR_WORDS]; /* native offsets after step 1 */
    u32 pdWords, introWords;  /* walked lengths, for the probe */
    u32 npads, nbpads, nwp, nwg, npaths, nai;
    int bad;
};

static int inb(struct conv *c, u32 off, u32 len, const char *what)
{
    if (off > c->size || len > c->size - off) {
        if (!c->bad) {
            portConLogf("ge: romdata: setup %s: %s at 0x%X (+%u) is outside the %u-byte file\n", c->name, what, off, len, c->size);
        }
        c->bad++;
        return 0;
    }
    return 1;
}

/* a -1-terminated s32 list (BE) */
static void listConvert(struct conv *c, u32 off, const char *what)
{
    u32 o = off;
    for (;;) {
        s32 v;
        if (!inb(c, o, 4, what)) return;
        v = (s32) be32(c->d + o);
        kindApply(c->d + o, SETUP_KIND_K32);
        o += 4;
        if (v < 0) return;
    }
}

static void convertPads(struct conv *c)
{
    u32 o = c->hdr[H_PADS];
    for (;;) {
        u32 plink;
        if (!inb(c, o, 44, "pad")) return;
        plink = be32(c->d + o + 36);
        k32Run(c->d + o, 11);
        o += 44;
        if (plink == 0) return;
        c->npads++;
    }
}

static void convertBoundPads(struct conv *c)
{
    u32 o = c->hdr[H_BPADS];
    for (;;) {
        u32 plink;
        if (!inb(c, o, 68, "boundpad")) return;
        plink = be32(c->d + o + 36);
        k32Run(c->d + o, 17);
        o += 68;
        if (plink == 0) return;
        c->nbpads++;
    }
}

static void convertWaypoints(struct conv *c)
{
    u32 o = c->hdr[H_WP];
    for (;;) {
        s32 padid;
        u32 nb;
        if (!inb(c, o, 16, "waypoint")) return;
        padid = (s32) be32(c->d + o);
        nb = be32(c->d + o + 4);
        k32Run(c->d + o, 4);
        o += 16;
        if (padid < 0) return;
        listConvert(c, nb, "waypoint neighbours");
        c->nwp++;
    }
}

static void convertWaygroups(struct conv *c)
{
    u32 o = c->hdr[H_WG];
    for (;;) {
        u32 nb, wp;
        if (!inb(c, o, 12, "waygroup")) return;
        nb = be32(c->d + o);
        wp = be32(c->d + o + 4);
        k32Run(c->d + o, 3);
        o += 12;
        if (nb == 0) return;
        listConvert(c, nb, "waygroup neighbours");
        listConvert(c, wp, "waygroup waypoints");
        c->nwg++;
    }
}

static void convertPaths(struct conv *c)
{
    u32 o = c->hdr[H_PATHS];
    for (;;) {
        u32 wp;
        if (!inb(c, o, 8, "path")) return;
        wp = be32(c->d + o);
        kindApply(c->d + o, SETUP_KIND_K32);
        kindApply(c->d + o + 4, SETUP_KIND_KKS); /* u8 ID, u8 isLoop, u16 len */
        o += 8;
        if (wp == 0) return;
        listConvert(c, wp, "path waypoints");
        c->npaths++;
    }
}

static void convertAilists(struct conv *c)
{
    u32 o = c->hdr[H_AI];
    for (;;) {
        u32 al;
        if (!inb(c, o, 8, "ailist")) return;
        al = be32(c->d + o);
        k32Run(c->d + o, 2); /* the streams themselves are bytes */
        o += 8;
        if (al == 0) return;
        c->nai++;
    }
}

static void convertNames(struct conv *c, u32 off, const char *what)
{
    u32 o = off;
    if (off == 0) return;
    for (;;) {
        u32 p;
        if (!inb(c, o, 4, what)) return;
        p = be32(c->d + o);
        kindApply(c->d + o, SETUP_KIND_K32);
        o += 4;
        if (p == 0) return;
    }
}

static void convertPropDefs(struct conv *c)
{
    u32 o = c->hdr[H_PD];
    u32 n = 0;
    for (;;) {
        u32 type, words, w;
        if (!inb(c, o, 4, "propdef")) return;
        type = c->d[o + 3];
        if (type >= SETUP_PROPDEF_TYPES) {
            portConLogf("ge: romdata: setup %s: propdef %u has type %u (max %d) - stopping the walk\n", c->name, n, type, SETUP_PROPDEF_TYPES - 1);
            c->bad++;
            return;
        }
        words = kSetupPropdefKinds[type].words;
        if (!inb(c, o, words * 4, "propdef record")) return;
        kindApply(c->d + o, SETUP_KIND_KSK); /* {u16 scale; u8 hidden; u8 type} on every type */
        for (w = 1; w < words; w++) {
            kindApply(c->d + o + w * 4, kSetupPropdefKinds[type].kind[w]);
        }
        o += words * 4;
        n++;
        if (type == SETUP_PROPDEF_TYPES - 1) { /* PROPDEF_END */
            c->pdWords = (o - c->hdr[H_PD]) / 4;
            return;
        }
    }
}

static void convertCredits(struct conv *c, u32 off)
{
    u32 o = off;
    for (;;) {
        u32 t1, t2;
        if (!inb(c, o, 12, "credits entry")) return;
        t1 = ((u32) c->d[o] << 8) | c->d[o + 1];
        t2 = ((u32) c->d[o + 2] << 8) | c->d[o + 3];
        kindApply(c->d + o, SETUP_KIND_K16);
        kindApply(c->d + o + 4, SETUP_KIND_K16);
        kindApply(c->d + o + 8, SETUP_KIND_K16);
        o += 12;
        if (t1 == 0 && t2 == 0) return;
    }
}

static void convertIntro(struct conv *c)
{
    u32 o = c->hdr[H_INTRO];
    u32 n = 0;
    for (;;) {
        u32 type, words, w;
        if (!inb(c, o, 4, "intro")) return;
        type = c->d[o + 3];
        if (type >= SETUP_INTRO_TYPES) {
            portConLogf("ge: romdata: setup %s: intro %u has type %u (max %d) - stopping the walk\n", c->name, n, type, SETUP_INTRO_TYPES - 1);
            c->bad++;
            return;
        }
        words = kSetupIntroKinds[type].words;
        if (!inb(c, o, words * 4, "intro record")) return;
        if (type == 8) { /* INTROTYPE_CREDITS: word 1 = offset of the CreditsEntry table */
            u32 tbl = be32(c->d + o + 4);
            u32 ptr;
            convertCredits(c, tbl);
            ptr = (u32) (uintptr_t) (c->d + tbl);
            memcpy(c->d + o + 4, &ptr, 4); /* promoted here: bondview_r.c's PC arm reads a pointer */
            kindApply(c->d + o, kSetupIntroKinds[type].kind[0]);
        } else {
            for (w = 0; w < words; w++) {
                kindApply(c->d + o + w * 4, kSetupIntroKinds[type].kind[w]);
            }
        }
        o += words * 4;
        n++;
        if (type == SETUP_INTRO_TYPES - 1) { /* INTROTYPE_END */
            c->introWords = (o - c->hdr[H_INTRO]) / 4;
            return;
        }
    }
}

/* ==== the probe ======================================================== */

static u32 probeMismatch(const u8 *a, const u8 *b, u32 n)
{
    u32 i, m = 0;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) m++;
    }
    return m;
}

/* -1-terminated s32 lists behind two pointers */
static u32 probeList(const s32 *a, const s32 *b)
{
    u32 m = 0;
    for (;;) {
        if (*a != *b) m++;
        if (*a < 0 || *b < 0) return m;
        a++;
        b++;
    }
}

static void romfileSetupProbe(struct conv *c, const u8 *compiled)
{
    const struct stagesetup *cs = (const struct stagesetup *) compiled;
    const u8 *d = c->d;
    u32 i, m;
    u32 total = 0;
    char report[512];
    int rl = 0;

#define REP(what, mm) do { total += (mm); if ((mm) != 0 && rl < (int) sizeof(report) - 40) rl += snprintf(report + rl, sizeof(report) - rl, " %s=%u", what, (unsigned) (mm)); } while (0)

    /* propDefs + intro: whole regions, no pointers in them (credits aside).
     * A mod's file can be longer than the compiled one, so every loop below
     * must also stop at the compiled side's terminator or it reads out of
     * bounds. */
    {
        const u8 *cp = (const u8 *) cs->propDefs;
        u32 cw = 0;
        for (;;) { /* the compiled walk, by the same type table */
            u32 type = cp[cw * 4 + 3];
            u32 words = kSetupPropdefKinds[type < SETUP_PROPDEF_TYPES ? type : 0].words;
            cw += words;
            if (type >= SETUP_PROPDEF_TYPES - 1 || cw >= c->pdWords + 4096) {
                break;
            }
        }
        m = probeMismatch(cp, d + c->hdr[H_PD], (cw < c->pdWords ? cw : c->pdWords) * 4);
        m += (cw > c->pdWords ? cw - c->pdWords : c->pdWords - cw) * 4;
        REP("propDefs", m);
    }
    {
        const u8 *ci = (const u8 *) cs->intro;
        const u8 *di = d + c->hdr[H_INTRO];
        u32 o = 0;
        m = 0;
        while (o < c->introWords * 4) {
            if (ci[o + 3] != di[o + 3]) { /* the compiled record stream diverged: stop, count the rest */
                m += c->introWords * 4 - o;
                break;
            }
            u32 type = di[o + 3];
            u32 words = kSetupIntroKinds[type < SETUP_INTRO_TYPES ? type : 0].words;
            if (type == 8) {
                const CreditsEntry *ca = *(CreditsEntry *const *) (ci + o + 4);
                const CreditsEntry *da = *(CreditsEntry *const *) (di + o + 4);
                m += probeMismatch(ci + o, di + o, 4);
                for (;;) {
                    m += probeMismatch((const u8 *) ca, (const u8 *) da, sizeof(CreditsEntry));
                    if ((ca->TextId1 == 0 && ca->TextId2 == 0) || (da->TextId1 == 0 && da->TextId2 == 0)) break;
                    ca++;
                    da++;
                }
            } else {
                m += probeMismatch(ci + o, di + o, words * 4);
            }
            o += words * 4;
        }
        REP("intro", m);
    }
    /* pads / boundpads: everything but plink (compared as strings) and stan */
    {
        const PadRecord *cp = cs->pads;
        const PadRecord *dp = (const PadRecord *) (d + c->hdr[H_PADS]);
        m = 0;
        for (i = 0; i < c->npads; i++) {
            if (cp[i].plink == NULL) {
                m += c->npads - i; /* the compiled list is shorter */
                break;
            }
            m += probeMismatch((const u8 *) &cp[i], (const u8 *) &dp[i], 36);
            m += strcmp(cp[i].plink, (const char *) d + (u32) (uintptr_t) dp[i].plink) != 0;
        }
        REP("pads", m);
        m = 0;
        {
            const BoundPadRecord *cb = cs->boundpads;
            const BoundPadRecord *db = (const BoundPadRecord *) (d + c->hdr[H_BPADS]);
            for (i = 0; i < c->nbpads; i++) {
                if (cb[i].plink == NULL) {
                    m += c->nbpads - i;
                    break;
                }
                m += probeMismatch((const u8 *) &cb[i], (const u8 *) &db[i], 36);
                m += probeMismatch((const u8 *) &cb[i].bbox, (const u8 *) &db[i].bbox, 24);
                m += strcmp(cb[i].plink, (const char *) d + (u32) (uintptr_t) db[i].plink) != 0;
            }
        }
        REP("boundpads", m);
    }
    /* waypoints / waygroups / paths: values + the lists behind the pointers */
    {
        const waypoint *cw = cs->pathwaypoints;
        const waypoint *dw = (const waypoint *) (d + c->hdr[H_WP]);
        m = 0;
        for (i = 0; i < c->nwp; i++) {
            if (cw[i].padID < 0) {
                m += c->nwp - i;
                break;
            }
            m += (cw[i].padID != dw[i].padID) + (cw[i].groupNum != dw[i].groupNum) + (cw[i].dist != dw[i].dist);
            m += probeList(cw[i].neighbours, (const s32 *) (d + (u32) (uintptr_t) dw[i].neighbours));
        }
        REP("waypoints", m);
        {
            const waygroup *cg = cs->waypointgroups;
            const waygroup *dg = (const waygroup *) (d + c->hdr[H_WG]);
            m = 0;
            for (i = 0; i < c->nwg; i++) {
                if (cg[i].neighbours == NULL) {
                    m += c->nwg - i;
                    break;
                }
                m += (cg[i].dist != dg[i].dist);
                m += probeList(cg[i].neighbours, (const s32 *) (d + (u32) (uintptr_t) dg[i].neighbours));
                m += probeList(cg[i].waypoints, (const s32 *) (d + (u32) (uintptr_t) dg[i].waypoints));
            }
            REP("waygroups", m);
        }
        {
            const PathRecord *cp = cs->patrolpaths;
            const PathRecord *dp = (const PathRecord *) (d + c->hdr[H_PATHS]);
            m = 0;
            for (i = 0; i < c->npaths; i++) {
                if (cp[i].waypoints == NULL) {
                    m += c->npaths - i;
                    break;
                }
                m += (cp[i].ID != dp[i].ID) + (cp[i].isLoop != dp[i].isLoop);
                m += probeList(cp[i].waypoints, (const s32 *) (d + (u32) (uintptr_t) dp[i].waypoints));
            }
            REP("paths", m);
        }
        {
            const AIListRecord *ca = cs->ailists;
            const AIListRecord *da = (const AIListRecord *) (d + c->hdr[H_AI]);
            m = 0;
            for (i = 0; i < c->nai; i++) {
                if (ca[i].ailist == NULL) {
                    m += c->nai - i;
                    break;
                }
                m += (ca[i].ID != da[i].ID);
            }
            REP("ailist-ids", m);
        }
    }
#undef REP
    if (total == 0) {
        portConLogf("ge: romdata: setup %-16s identical to the compiled-in file (%u propdef words, %u intro words, %u pads, %u boundpads, %u waypoints, %u paths, %u ailists)\n",
                c->name, c->pdWords, c->introWords, c->npads, c->nbpads, c->nwp, c->npaths, c->nai);
    } else {
        portConLogf("ge: romdata: setup %-16s DIFFERS from the compiled-in file:%s (a mod - expected - or a converter bug: scripts/port/gen_setup_kinds.py)\n",
                c->name, report);
    }
}

/* ==== entry ============================================================ */

/* data = the inflated file in its memp bank (the loader rebases offsets
 * after this); compiled = the linked-in copy for the probe, or NULL */
s32 romfileSetupConvert(u8 *data, u32 size, const char *name, const u8 *compiled)
{
    struct conv c;
    u32 i;

    memset(&c, 0, sizeof(c));
    c.d = data;
    c.size = size;
    c.name = name;

    if (size < SETUP_HDR_WORDS * 4) {
        portConLogf("ge: romdata: setup %s: %u bytes is no setup file\n", name, size);
        return 0;
    }
    for (i = 0; i < SETUP_HDR_WORDS; i++) {
        c.hdr[i] = be32(data + i * 4);
        kindApply(data + i * 4, SETUP_KIND_K32);
        if (c.hdr[i] >= size && !(i >= H_PN && c.hdr[i] == 0)) {
            portConLogf("ge: romdata: setup %s: header offset %u = 0x%X is outside the %u-byte file - not a setup?\n", name, i, c.hdr[i], size);
            return 0;
        }
    }
    convertPads(&c);
    convertBoundPads(&c);
    convertWaypoints(&c);
    convertWaygroups(&c);
    convertPaths(&c);
    convertAilists(&c);
    convertNames(&c, c.hdr[H_PN], "padnames");
    convertNames(&c, c.hdr[H_BPN], "boundpadnames");
    convertPropDefs(&c);
    convertIntro(&c);

    if (c.bad) {
        portConLogf("ge: romdata: setup %s: %d bounds problems during conversion - expect trouble\n", name, c.bad);
    }
    if (compiled != NULL) {
        romfileSetupProbe(&c, compiled);
    } else {
        portConLogf("ge: romdata: setup %-16s converted (%u propdef words, %u intro words, %u pads)\n", name, c.pdWords, c.introWords, c.npads);
    }
    return c.bad == 0;
}
