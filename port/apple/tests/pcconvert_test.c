/*
 * pcconvert_test.c - Phase 1 round 7a host test (docs/port-ios/11-round7a-pcconvert.md).
 *
 * usage: pcconvert_test <rom.z64> [donor-data-dir]
 *
 * Converts the user's cart IN MEMORY with port/apple/pcconvert (d43/d69/d88,
 * exactly the bytes pcmodels.bin / pccg.bin would hold) and validates the
 * sidecars against independent references:
 *
 *  1. every model (512): the PC tree (48-byte nodes, 8-byte slots) walked by
 *     SEGMENT token is node-for-node the tree Midas's own 32-bit converter
 *     (port/src/romfile_model.c, run on the same N64 bytes) walks; every
 *     pointer slot is 0 or a seg-5 token inside the image with a zero high
 *     word; every display list is 16-byte {w0,0,w1,0} slots ending in
 *     G_ENDDL; vertex and DL counts against romfile_model.c's.
 *  2. Dam bg: d69's room table, room positions and portal point records equal
 *     port/src/romfile_bg.c's in-place conversion (relocated by portal_delta).
 *  3. Dam stan: d69's tile stream is byte-identical to port/src/romfile_stan.c's
 *     (Midas's StandTile encoding, R5); room slots are offsets + array_delta.
 *  4. Dam setup: header, the eight growing tables and the propDefs stream walk
 *     at the PC strides (sizepropdef's PORT arm), record counts equal the N64
 *     stream's.
 *  5. the menu-model trio (legal page, Nintendo logo, GoldenEye logo): served
 *     from the cart with their embedded image blobs copied (nonzero, equal to
 *     the source bytes).
 *  6. optional: every row against the donor's own converter output
 *     (GoldenEye-Decomp-ios engine/data/pcmodels-ntsc-final, pccg-ntsc-final),
 *     inflated; d69 stans are expected to differ only in the tile header words.
 *
 * Read-only on the ROM. Silent (no audio, no SDL, no window). Exit 0 = pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include "pcconvert.h"
#include "pcconvert_internal.h"

/* ---- Midas's 32-bit converters (port/src/romfile_*.c), linked in --------- */
extern int32_t romfileModelConvert(uint8_t *data, uint32_t size, int32_t numSwitches, int32_t numtextures,
                                   const char *name, const uint8_t *compiled);
extern int32_t romfileStanConvert(uint8_t *data, uint32_t size, const char *name, const uint8_t *compiled);
extern int32_t romfileBgConvert(uint8_t *data, uint32_t size, const char *name);

/* their link surface */
static char s_lastLog[1024];
void portConLogf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_lastLog, sizeof(s_lastLog), fmt, ap);
    va_end(ap);
}
uint8_t list_of_tilesizes[] = { 0x20, 0x20, 0x20, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x00 }; /* stan.c */
void *portStanPackForProbe(void *file) { (void)file; return NULL; }  /* compiled == NULL: never called */

/* ---------------------------------------------------------------- helpers */
static int s_fail;
static uint32_t s_linked0500, s_linkedTotal;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t le32(const uint8_t *b, size_t o) { return pcvGet32(b, o); }
static uint16_t le16(const uint8_t *b, size_t o) { return (uint16_t)(b[o] | (b[o + 1] << 8)); }

static int cmpU32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static const PcvRow *findRow(const PcvImage *im, const char *name)
{
    int i;
    for (i = 0; i < im->n; i++)
        if (!strcmp(im->rows[i].name, name))
            return &im->rows[i];
    return NULL;
}

static uint8_t *rowInflate(const PcvImage *im, const PcvRow *r, size_t *len)
{
    return pcvInflateRZ(im->bin.p + r->offset, r->size, len);
}

/* N64 file straight from the ROM (inflated when RZ) */
static uint8_t *romFile(const uint8_t *rom, size_t romLen, const char *base, size_t *len)
{
    const struct PcvFileRow *row = pcvFindRowExact(base);
    uint8_t *p;
    if (!row || (size_t)row->addr + row->size > romLen)
        return NULL;
    if (rom[row->addr] == 0x11 && rom[row->addr + 1] == 0x72)
        return pcvInflateRZ(rom + row->addr, row->size, len);
    p = (uint8_t *)malloc(row->size);
    memcpy(p, rom + row->addr, row->size);
    *len = row->size;
    return p;
}

/* -------------------------------------------------------- 1. model walk ---*/
struct ModelStats {
    uint32_t nodes, verts, dls, subdls, gdlcmds, slots, badslots, badgdl;
    uint32_t gdlMin;   /* lowest GDL head: d43 places GDLs last, after the image blobs */
    uint32_t dpc;
};

typedef struct { uint32_t *v; int n, cap; } Vec;
static void vpush(Vec *a, uint32_t x)
{
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 256; a->v = realloc(a->v, sizeof(uint32_t) * (size_t)a->cap); }
    a->v[a->n++] = x;
}
static int vhas(const Vec *a, uint32_t x) { int i; for (i = 0; i < a->n; i++) if (a->v[i] == x) return 1; return 0; }

/* an 8-byte pointer slot: 0, or {0x05000000|off, 0} with off inside the image */
static uint32_t slotOff(struct ModelStats *st, const uint8_t *pc, size_t D, size_t o, const char *name, const char *what)
{
    uint32_t lo = le32(pc, o), hi = le32(pc, o + 4);
    st->slots++;
    if (lo == 0 && hi == 0)
        return 0;
    if (hi != 0 || (lo >> 24) != 0x05 || (lo & 0xFFFFFFu) >= D) {
        st->badslots++;
        if (st->badslots <= 3)
            printf("  %s: bad %s slot at %#zx: %08x %08x\n", name, what, o, lo, hi);
        return 0;
    }
    return lo & 0xFFFFFFu;
}

static void walkGdl(struct ModelStats *st, const uint8_t *pc, size_t D, uint32_t g, const char *name, Vec *gdls)
{
    uint32_t o = g, guard = 0;
    if (!g || vhas(gdls, g))
        return;
    vpush(gdls, g);
    st->dls++;
    if (!st->gdlMin || g < st->gdlMin) st->gdlMin = g;
    for (;;) {
        uint32_t w0, w1, c;
        if (o + 16 > D || guard++ > 0x8000) {
            st->badgdl++;
            printf("  %s: GDL %#x runs off the image\n", name, g);
            return;
        }
        w0 = le32(pc, o); w1 = le32(pc, o + 8);
        if (le32(pc, o + 4) || le32(pc, o + 12)) {
            st->badgdl++;
            if (st->badgdl <= 3)
                printf("  %s: GDL %#x slot %#x high words nonzero\n", name, g, o);
        }
        c = w0 >> 24;
        st->gdlcmds++;
        if ((c == 0x04 || c == 0xFD || c == 0x06) && (w1 >> 24) == 0x05 && (w1 & 0xFFFFFFu) >= D) {
            st->badgdl++;
            printf("  %s: GDL op %02x w1 %08x outside the image\n", name, c, w1);
        }
        if (c == 0x06 && (w1 >> 24) == 0x05) {
            st->subdls++;   /* d43 does not remap G_DL targets: must never occur */
            walkGdl(st, pc, D, w1 & 0xFFFFFFu, name, gdls);
        }
        if (c == 0xB8)
            break;
        o += 16;
    }
}

static int modelCheck(const uint8_t *rom, size_t romLen, const PcvImage *models, const char *name,
                      struct ModelStats *st, int verbose)
{
    char key[PCV_NAME_LEN];
    const struct PcvFileRow *frow = pcvFindRowVariants(name, key, sizeof(key));
    const struct PcvNsNtRow *hdr = frow ? pcvFindNsNtVariants(key) : NULL;
    const PcvRow *row = findRow(models, name);
    uint8_t *pc, *n64;
    size_t D = 0, n64Len = 0;
    uint32_t NS, NT, R0, i;
    Vec stack = { 0 }, seen = { 0 }, gdls = { 0 }, vtx = { 0 };
    unsigned rn = 0, rv = 0, rd = 0;
    int ok = 1;

    memset(st, 0, sizeof(*st));
    if (!row || !hdr) {
        printf("FAIL: %s: no sidecar row / header\n", name);
        s_fail++;
        return 0;
    }
    NS = hdr->ns; NT = hdr->nt;
    pc = rowInflate(models, row, &D);
    st->dpc = (uint32_t)D;
    R0 = 8 * NS + 12 * NT;
    for (i = 0; i < NS; i++)
        slotOff(st, pc, D, 8u * i, name, "switch");

    vpush(&stack, R0);
    while (stack.n) {
        uint32_t o = stack.v[--stack.n], data, op, nxt, child;
        /* 0 = null slot, except the root itself (NS = NT = 0: PexplosionbitZ) */
        if ((!o && o != R0) || vhas(&seen, o) || (o == R0 && seen.n))
            continue;
        if (o + 48 > D) { st->badslots++; continue; }
        vpush(&seen, o);
        st->nodes++;
        op = le16(pc, o) & 0xFF;
        data   = slotOff(st, pc, D, o + 8, name, "Data");
        (void)   slotOff(st, pc, D, o + 16, name, "Parent");
        nxt    = slotOff(st, pc, D, o + 24, name, "Next");
        (void)   slotOff(st, pc, D, o + 32, name, "Prev");
        child  = slotOff(st, pc, D, o + 40, name, "Child");
        if (data) {
            uint32_t p, s, v;
            switch (op) {
            case 1: slotOff(st, pc, D, data + 8, name, "FirstGroup"); break;
            case 2: case 3: slotOff(st, pc, D, data + 24, name, "ChildGroup"); break;
            case 4:
                p = slotOff(st, pc, D, data + 0, name, "Primary");
                s = slotOff(st, pc, D, data + 8, name, "Secondary");
                v = slotOff(st, pc, D, data + 24, name, "Vertices");
                walkGdl(st, pc, D, p, name, &gdls);
                walkGdl(st, pc, D, s, name, &gdls);
                if (v && le16(pc, data + 32) && !vhas(&vtx, v)) { vpush(&vtx, v); st->verts += le16(pc, data + 32); }
                break;
            case 8: child = slotOff(st, pc, D, data + 8, name, "Affects"); break;
            case 9:
                vpush(&stack, slotOff(st, pc, D, data + 24, name, "leftChild"));
                vpush(&stack, slotOff(st, pc, D, data + 32, name, "rightChild"));
                break;
            case 12: slotOff(st, pc, D, data + 24, name, "Image"); break;
            case 13: slotOff(st, pc, D, data + 16, name, "image"); slotOff(st, pc, D, data + 24, name, "Header"); break;
            case 18: child = slotOff(st, pc, D, data + 0, name, "Controls"); break;
            case 22:
                v = slotOff(st, pc, D, data + 8, name, "Vertices");
                p = slotOff(st, pc, D, data + 16, name, "Primary");
                walkGdl(st, pc, D, p, name, &gdls);
                if (v && le32(pc, data) && !vhas(&vtx, v)) { vpush(&vtx, v); st->verts += 4 * le32(pc, data); }
                break;
            case 24: {
                uint32_t cv;
                p = slotOff(st, pc, D, data + 0, name, "Primary");
                s = slotOff(st, pc, D, data + 8, name, "Secondary");
                v = slotOff(st, pc, D, data + 16, name, "Vertices");
                cv = slotOff(st, pc, D, data + 32, name, "CollisionVertices");
                slotOff(st, pc, D, data + 40, name, "PointUsage");
                walkGdl(st, pc, D, p, name, &gdls);
                walkGdl(st, pc, D, s, name, &gdls);
                if (v && le16(pc, data + 24) && !vhas(&vtx, v)) { vpush(&vtx, v); st->verts += le16(pc, data + 24); }
                if (cv && le16(pc, data + 26) && !vhas(&vtx, cv)) {
                    uint32_t k, n = le16(pc, data + 26);
                    vpush(&vtx, cv);
                    st->verts += n;
                    for (k = 0; k < n; k++) {   /* LinkedTo: a u32 seg-5 token (Vertex stays 16 B) */
                        uint32_t lt = le32(pc, cv + 16 * k + 8);
                        st->slots++;
                        if (lt && ((lt >> 24) != 5 || (lt & 0xFFFFFFu) >= D)) st->badslots++;
                        if (lt && (lt & 0xFFFFu) == 0x0500u) s_linked0500++;   /* model.c raw-unswap trigger */
                        if (lt) s_linkedTotal++;
                    }
                }
                break;
            }
            default: break;
            }
        }
        vpush(&stack, nxt);
        vpush(&stack, child);
    }

    /* Midas's 32-bit converter on the same N64 bytes */
    n64 = romFile(rom, romLen, key, &n64Len);
    if (n64) {
        s_lastLog[0] = 0;
        romfileModelConvert(n64, (uint32_t)n64Len, (int32_t)NS, (int32_t)NT, name, NULL);
        if (sscanf(strstr(s_lastLog, "converted (") ? strstr(s_lastLog, "converted (") : "",
                   "converted (%u nodes, %u verts, %u DLs)", &rn, &rv, &rd) != 3)
            printf("  %s: romfile_model.c said: %s", name, s_lastLog);
        free(n64);
    }
    if (st->nodes != rn) { ok = 0; printf("FAIL: %s: nodes pc %u != romfile_model %u\n", name, st->nodes, rn); }
    if (st->dls + 0 != rd) { ok = 0; printf("FAIL: %s: DLs pc %u != romfile_model %u\n", name, st->dls, rd); }
    if (st->verts != rv) { ok = 0; printf("FAIL: %s: verts pc %u != romfile_model %u\n", name, st->verts, rv); }
    if (st->badslots || st->badgdl || st->subdls) { ok = 0; printf("FAIL: %s: %u bad slots, %u bad GDL slots, %u G_DL sub-lists\n", name, st->badslots, st->badgdl, st->subdls); }
    if (!ok) s_fail++;
    if (verbose)
        printf("  %-16s NS=%-3u NT=%-3u N64 %6zu B -> PC %6u B (RZ %6u): %4u nodes, %5u verts, %3u DLs "
               "(%5u Gfx x16 B), %5u pointer slots ok | romfile_model.c: %u nodes, %u verts, %u DLs  %s\n",
               name, NS, NT, n64Len, st->dpc, row->size, st->nodes, st->verts, st->dls, st->gdlcmds,
               st->slots, rn, rv, rd, ok ? "PASS" : "FAIL");
    free(pc); free(stack.v); free(seen.v); free(gdls.v); free(vtx.v);
    return ok;
}

/* ----------------------------------------------------------- 2. bg (Dam) ---*/
static uint32_t s_bgOrphans;
static void bgCheck(const uint8_t *rom, size_t romLen, const PcvImage *cg, const char *seg)
{
    const PcvRow *row = findRow(cg, seg);
    const char *base = seg + strcspn(seg, "/") + 1;   /* "bg/<base>.seg" */
    char key[64];
    uint8_t *n64, *mid;
    size_t n64Len = 0;
    const uint8_t *pc;
    uint32_t h1, h2, h3, nRooms, nPortals = 0, delta, o, roomEnd, k, recs = 0, roomsEq = 0;
    uint32_t mo, po, orphans = 0, orphansRaw = 0, refd = 0;
    unsigned mrooms = 0, mvis = 0, mportals = 0;
    Vec refs = { 0 };

    snprintf(key, sizeof(key), "%.*s", (int)(strlen(base) - 4), base);
    CHECK(row != NULL, "%s: no pccg row", seg);
    if (!row) return;
    pc = cg->bin.p + row->offset;
    h1 = le32(pc, 4); h2 = le32(pc, 8); h3 = le32(pc, 12);
    CHECK(le32(pc, 0) == 0 && le32(pc, 16) == 0, "%s: header words 0/4 not 0", seg);
    CHECK((h1 >> 24) == 0x0F && (h2 >> 24) == 0x0F && (!h3 || (h3 >> 24) == 0x0F), "%s: header not seg-0x0F tokens", seg);
    h1 &= 0xFFFFFF; h2 &= 0xFFFFFF; h3 &= 0xFFFFFF;
    roomEnd = h3 ? h3 : h2;
    nRooms = (roomEnd - h1) / 24;
    for (o = h2; le32(pc, o); o += 16) {
        uint32_t t = le32(pc, o);
        CHECK((t >> 24) == 0x0F && le32(pc, o + 4) == 0 && le32(pc, o + 12) == 0, "%s: portal %u slot", seg, nPortals);
        t &= 0xFFFFFF;
        CHECK(t < row->size && pc[t] >= 1 && pc[t] <= 20, "%s: portal %u -> %#x numPoints %u", seg, nPortals, t, t < row->size ? pc[t] : 0);
        nPortals++;
    }
    delta = 8u * (nPortals + 1u);
    for (k = 0; k < nRooms; k++)
        for (o = 0; o < 3; o++) {
            uint32_t t = le32(pc, h1 + 24 * k + 4 * o);
            CHECK(!t || ((t >> 24) == 0x0F && (t & 0xFFFFFF) < row->size), "%s: room %u offset %u = %08x", seg, k, o, t);
        }

    /* Midas's romfile_bg.c on the same N64 bytes (front matter in place) */
    n64 = romFile(rom, romLen, key, &n64Len);
    mid = (uint8_t *)malloc(n64Len);
    memcpy(mid, n64, n64Len);
    s_lastLog[0] = 0;
    romfileBgConvert(mid, (uint32_t)n64Len, seg);
    sscanf(strstr(s_lastLog, "converted (") ? strstr(s_lastLog, "converted (") : "",
           "converted (%u rooms, %u vis cmds, %u portals)", &mrooms, &mvis, &mportals);
    CHECK(row->size == n64Len + delta, "%s: size %u != N64 %zu + portal_delta %u", seg, row->size, n64Len, delta);
    /* rooms: positions (f32, both now LE) equal; offsets equal + delta */
    for (k = 0; k < nRooms; k++) {
        int eq = !memcmp(pc + h1 + 24 * k + 12, mid + h1 + 24 * k + 12, 12);
        for (o = 0; o < 3; o++) {
            uint32_t a = le32(pc, h1 + 24 * k + 4 * o), b = le32(mid, h1 + 24 * k + 4 * o);
            if (!((a == 0 && (b & 0xFFFFFF) == 0) || (a & 0xFFFFFF) == (b & 0xFFFFFF) + delta)) eq = 0;
        }
        roomsEq += eq;
    }
    CHECK(roomsEq == nRooms, "%s: %u/%u rooms equal romfile_bg.c", seg, roomsEq, nRooms);
    /* portal point records: walk romfile_bg.c's linear region. Records a
     * portal entry or an ENVIRONMENTDATA_ALT entry references must equal
     * Midas's (relocated by portal_delta); d69 copies unreferenced ("orphan")
     * records verbatim -- still big-endian -- where romfile_bg.c swaps them. */
    for (o = h2; pcvBE32(n64, o); o += 8)
        vpush(&refs, pcvBE32(n64, o) & 0xFFFFFF);
    if (h3 && h3 > h1 && h3 < h2)
        for (o = h3; o < h2; o += 8) {
            uint8_t t = n64[o];
            int32_t dat = (int32_t)pcvBE32(n64, o + 4);
            if (t == 100 && dat) vpush(&refs, (uint32_t)dat & 0xFFFFFF);
            if (t == 0) break;
        }
    for (mo = h2; le32(mid, mo); mo += 8) { }
    mo += 8;           /* first byte after the N64 NULL entry */
    po = mo + delta;
    while (mo + 4 <= n64Len && recs < mportals) {
        uint32_t n = mid[mo], span;
        if (n == 0) { mo += 4; po += 4; continue; }
        span = 4 + 12 * n;
        if (vhas(&refs, mo)) {
            refd++;
            if (memcmp(mid + mo, pc + po, span)) {
                CHECK(0, "%s: referenced portal record at N64 %#x differs from romfile_bg.c", seg, mo);
                break;
            }
        } else {
            orphans++;
            orphansRaw += !memcmp(n64 + mo, pc + po, span);
        }
        recs++;
        mo += span; po += span;
    }
    CHECK(recs == mportals && orphansRaw == orphans, "%s: %u/%u portal point records walked, %u orphans (%u raw)",
          seg, recs, mportals, orphans, orphansRaw);
    printf("  %-22s N64 %u B -> PC %u B (+portal_delta %u): %u rooms, %u portal entries | romfile_bg.c: %u room rows, "
           "%u vis cmds, %u portal records; rooms equal %u/%u, referenced portal records equal %u/%u%s\n",
           seg, (unsigned)n64Len, row->size, delta, nRooms, nPortals, mrooms, mvis, mportals,
           roomsEq, nRooms, refd, refd, "");
    if (orphans)
        printf("      %u unreferenced portal records copied verbatim (big-endian; romfile_bg.c swaps them)\n", orphans);
    s_bgOrphans += orphans;
    free(refs.v);
    free(n64);
    /* mid stays allocated: romfileBgConvert remembers converted buffers by
     * address and would skip a new file malloc() put at a freed one */
}

/* --------------------------------------------------------- 3. stan (Dam) ---*/
static void stanCheck(const uint8_t *rom, size_t romLen, const PcvImage *cg, const char *name)
{
    const PcvRow *row = findRow(cg, name);
    uint8_t *pc, *n64;
    size_t D = 0, n64Len = 0;
    uint32_t nslots = 0, first, arrayDelta, newFirst, o, ntiles = 0, tileBytes, padDiff = 0, dataDiff = 0;
    int whole;
    unsigned mt = 0, mr = 0, mb = 0;
    int i;

    CHECK(row != NULL, "%s: no pccg row", name);
    if (!row) return;
    pc = rowInflate(cg, row, &D);
    n64 = romFile(rom, romLen, name, &n64Len);
    first = pcvBE32(n64, 4);
    for (o = 8; le32(pc, o); o += 8) {
        CHECK(le32(pc, o + 4) == 0, "%s: slot %u high word", name, nslots);
        nslots++;
    }
    /* N64: [stanfile][first][rooms...][0]; PC: [stanfile][pad][8B slots: first, rooms..., 0] */
    /* d69: N = the N64 offset words before the terminator (first tile +
     * rooms) = nslots; array_delta = 4*(N+2) (4-byte pad + 4 per widened slot) */
    arrayDelta = 4u * (nslots + 2u);
    newFirst = le32(pc, 8);
    CHECK(newFirst == first + arrayDelta, "%s: first tile slot %u != %u + %u", name, newFirst, first, arrayDelta);
    for (i = 0; i < (int)nslots; i++) {
        uint32_t a = le32(pc, 8 + 8 * (uint32_t)i), b = pcvBE32(n64, 4 + 4 * (uint32_t)i);
        CHECK(a == b + arrayDelta, "%s: slot %d %u != N64 %u + delta", name, i, a, b);
    }
    /* Midas's romfile_stan.c on the same N64 bytes */
    s_lastLog[0] = 0;
    romfileStanConvert(n64, (uint32_t)n64Len, name, NULL);
    sscanf(strstr(s_lastLog, "converted (") ? strstr(s_lastLog, "converted (") : "",
           "converted (%u tiles, %u rooms, %u bytes)", &mt, &mr, &mb);
    tileBytes = mb - first;
    CHECK(mb > first && first + tileBytes <= n64Len && newFirst + tileBytes <= D, "%s: tile extents", name);
    whole = !memcmp(pc + newFirst, n64 + first, tileBytes);
    /* per tile: header + pointCount points must match; the slack up to
     * list_of_tilesizes[n] (n < 3) is never read -- d69 zero-fills it,
     * romfile_stan.c leaves the cart bytes */
    for (o = 0; o < tileBytes; ) {
        uint32_t n = le16(n64, first + o + 6) & 0xF, sz = n <= 10 ? list_of_tilesizes[n] : 0, used = 8 + 8 * n;
        if (!sz) break;
        if (memcmp(pc + newFirst + o, n64 + first + o, used)) dataDiff++;
        if (memcmp(pc + newFirst + o + used, n64 + first + o + used, sz - used)) padDiff++;
        o += sz;
    }
    CHECK(dataDiff == 0, "%s: %u tiles differ from romfile_stan.c", name, dataDiff);
    /* walk the PC tiles with Midas's stan.h (low-nibble pointCount) */
    for (o = newFirst; o + 8 <= D && le32(pc, o); ) {
        uint32_t n = le16(pc, o + 6) & 0xF, sz = n <= 10 ? list_of_tilesizes[n] : 0;
        if (!sz) { CHECK(0, "%s: tile %u bad pointCount", name, ntiles); break; }
        o += sz;
        ntiles++;
    }
    CHECK(ntiles == mt && nslots - 1 == mr, "%s: tiles %u/%u rooms %u/%u", name, ntiles, mt, nslots - 1, mr);
    printf("  %-22s N64 %zu B -> PC %zu B (RZ %u, +array_delta %u): %u rooms, %u tiles; tile stream (%u B) "
           "vs romfile_stan.c (%u tiles, %u rooms): %s\n",
           name, n64Len, D, row->size, arrayDelta, nslots - 1, ntiles, tileBytes, mt, mr,
           whole ? "byte-identical" : dataDiff ? "TILES DIFFER" : "headers+points identical, unread tile slack differs");
    if (padDiff)
        printf("      (%u tiles with nonzero cart slack past their points: d69 zero-fills it)\n", padDiff);
    free(pc); free(n64);
}

/* -------------------------------------------------------- 4. setup (Dam) ---*/
static const short PC_PROPDEF[49] = {   /* sizepropdef's PORT arm, bytes */
    -1, 296, 8, 144, 152, 144, 272, 152, 160, 32, 288, 664, 144, 248, 32, -1, -1, 144, 12, 32,
    200, 152, 24, 16, 4, 8, 8, 8, 8, 8, 24, 4, 24, 24, 12, 24, 144, 48, 32, 208,
    208, -1, 144, 144, 40, 248, 28, 168, 4 };
static const short N64_PROPDEF[49] = {  /* words */
    -1, 64, 2, 32, 33, 32, 59, 33, 34, 7, 64, 149, 32, 54, 3, -1, -1, 32, 3, 4,
    45, 34, 4, 4, 1, 2, 2, 2, 2, 2, 4, 1, 4, 5, 3, 4, 32, 10, 4, 44,
    45, -1, 32, 32, 5, 56, 7, 37, 1 };
static const short INTRO_SZ[10] = { 12, 16, 16, 32, 8, 8, 40, 12, 8, 4 };

static uint32_t countN64(const uint8_t *s, uint32_t off, uint32_t stride, uint32_t termOff, int neg)
{
    uint32_t n = 0;
    if (!off) return 0;
    while (neg ? (int32_t)pcvBE32(s, off + stride * n + termOff) >= 0 : pcvBE32(s, off + stride * n + termOff) != 0)
        n++;
    return n;
}
static uint32_t countPC(const uint8_t *p, uint32_t off, uint32_t stride, uint32_t termOff, int neg)
{
    uint32_t n = 0;
    if (!off) return 0;
    while (neg ? (int32_t)le32(p, off + stride * n + termOff) >= 0 : le32(p, off + stride * n + termOff) != 0)
        n++;
    return n;
}

static int s_setupsChecked, s_creditsSeen;
static void setupCheck(const uint8_t *rom, size_t romLen, const PcvImage *cg, const char *name, int verbose)
{
    static const char *const F[10] = { "pathwaypoints", "waypointgroups", "intro", "propDefs", "patrolpaths",
                                       "ailists", "pads", "boundpads", "padnames", "boundpadnames" };
    /* per table: N64 stride/term, PC stride/term (d88 GROW_DEF; pointer slots 8 B) */
    static const struct { int f; uint32_t ns, nt; int neg; uint32_t ps, pt; } T[8] = {
        { 6, 44, 0x24, 0, 56, 0x28 }, { 7, 68, 0x24, 0, 80, 0x28 }, { 1, 12, 0, 0, 24, 0 },
        { 0, 16, 0, 1, 24, 0 },        { 4, 8, 0, 0, 16, 0 },        { 5, 8, 0, 0, 16, 0 },
        { 8, 4, 0, 0, 8, 0 },          { 9, 4, 0, 0, 8, 0 } };
    const PcvRow *row = findRow(cg, name);
    uint8_t *pc, *n64;
    size_t D = 0, n64Len = 0;
    uint32_t H[10], h[10], i, o, nPdN = 0, nPdP = 0, nIntroN = 0, nIntroP = 0;
    int typesOk = 1;

    CHECK(row != NULL, "%s: no pccg row", name);
    if (!row) return;
    pc = rowInflate(cg, row, &D);
    n64 = romFile(rom, romLen, name, &n64Len);
    if (verbose) printf("  %-22s N64 %zu B -> PC %zu B (RZ %u)\n", name, n64Len, D, row->size);
    for (i = 0; i < 10; i++) {
        H[i] = pcvBE32(n64, 4 * i);
        h[i] = le32(pc, 8 * i);
        CHECK(le32(pc, 8 * i + 4) == 0 && h[i] < D && (!H[i] == !h[i]), "%s: header %s %08x", name, F[i], h[i]);
    }
    for (i = 0; i < 8; i++) {
        uint32_t a = countN64(n64, H[T[i].f], T[i].ns, T[i].nt, T[i].neg);
        uint32_t b = countPC(pc, h[T[i].f], T[i].ps, T[i].pt, T[i].neg);
        CHECK(a == b, "%s: %s count PC %u != N64 %u", name, F[T[i].f], b, a);
        if (verbose) printf("      %-14s N64 %2u B x %3u -> PC %2u B x %3u %s\n", F[T[i].f], T[i].ns, a, T[i].ps, b, a == b ? "ok" : "MISMATCH");
    }
    /* pad name pointers -> NUL-terminated strings inside the file */
    for (i = 0; i < countPC(pc, h[6], 56, 0x28, 0); i++) {
        uint32_t p = le32(pc, h[6] + 56 * i + 0x28);
        CHECK(p < D && le32(pc, h[6] + 56 * i + 0x2c) == 0 && memchr(pc + p, 0, D - p), "%s: pad %u plink", name, i);
    }
    /* propDefs: N64 words vs PC bytes, same record sequence */
    for (o = H[3]; ; ) {
        int t = n64[o + 3];
        if (t > 48 || N64_PROPDEF[t] < 0) { typesOk = 0; break; }
        nPdN++;
        o += 4u * (uint32_t)N64_PROPDEF[t];
        if (t == 48) break;
    }
    for (o = h[3]; ; ) {
        int t = pc[o + 3];
        if (t > 48 || PC_PROPDEF[t] < 0 || o >= D) { typesOk = 0; break; }
        nPdP++;
        o += (uint32_t)PC_PROPDEF[t];
        if (t == 48) break;
    }
    CHECK(typesOk && nPdN == nPdP, "%s: propDefs PC %u records != N64 %u", name, nPdP, nPdN);
    for (o = H[2]; ; ) { int32_t t = (int32_t)pcvBE32(n64, o); if (t < 0 || t > 9) break; nIntroN++; o += (uint32_t)INTRO_SZ[t]; if (t == 9) break; }
    for (o = h[2]; ; ) {
        int32_t t = (int32_t)le32(pc, o);
        if (t < 0 || t > 9) break;
        nIntroP++;
        if (t == 8) {
            /* CREDITS: word 1 is a file offset to a 12-byte CreditsEntry
             * table {u16 TextId1, TextId2; s16 Position1; u16 Alignment1;
             * s16 Position2; u16 Alignment2}, {0,0}-terminated (bondview_r.c:
             * g_ptrStageSetupFile + unk04). Midas d88 (R7A-2) relocates the
             * word and halfword-swaps the table; the donor did neither. */
            uint32_t off = le32(pc, o + 4), noff = 0, k, n = 0, okEnt = 0;
            for (k = 0; k < (uint32_t)nIntroN + 1; k++) { }
            {   /* the N64 record at the same intro index */
                uint32_t on = H[2], idx = 0;
                while (idx + 1 < nIntroP) { int32_t tt = (int32_t)pcvBE32(n64, on); on += (uint32_t)INTRO_SZ[tt]; idx++; }
                noff = pcvBE32(n64, on + 4);
            }
            for (k = 0; noff + 12 * k + 12 <= n64Len; k++) {
                uint32_t j, same = 1;
                for (j = 0; j < 6; j++)
                    if (off + 12 * k + 2 * j + 2 > D || le16(pc, off + 12 * k + 2 * j) != pcvBE16(n64, noff + 12 * k + 2 * j)) same = 0;
                okEnt += same;
                n++;
                if (!pcvBE16(n64, noff + 12 * k) && !pcvBE16(n64, noff + 12 * k + 2)) break;
            }
            CHECK(okEnt == n && n > 1, "%s: CREDITS table %u/%u entries native at the relocated offset", name, okEnt, n);
            printf("  %-22s CREDITS intro: table N64 %#x -> PC %#x (relocated), %u/%u CreditsEntry records native u16 "
                   "(R7A-2: the donor left the offset and the table unconverted)\n", name, noff, off, okEnt, n);
            s_creditsSeen++;
        }
        o += (uint32_t)INTRO_SZ[t];
        if (t == 9) break;
    }
    CHECK(nIntroN == nIntroP, "%s: intro PC %u != N64 %u", name, nIntroP, nIntroN);
    if (verbose) {
        printf("      %-14s %u records (N64 %u) at the PC strides (sizepropdef PORT arm)\n", "propDefs", nPdP, nPdN);
        printf("      %-14s %u records (N64 %u), record sizes unchanged\n", "intro", nIntroP, nIntroN);
    }
    s_setupsChecked++;
    free(pc); free(n64);
}

/* ------------------------------------------------------- 5. menu models ---*/
static void menuCheck(const uint8_t *rom, size_t romLen, const PcvImage *models, const char *name)
{
    struct ModelStats st;
    const PcvRow *row = findRow(models, name);
    uint8_t *pc, *n64;
    size_t D = 0, n64Len = 0;
    uint32_t g, k, blobs = 0, blobBytes = 0, nonzero = 0, eq = 0;
    Vec srcT = { 0 }, pcT = { 0 };
    int id = -1, i;
    char key[PCV_NAME_LEN];

    for (i = 0; i < pcconvTableNameCount; i++)
        if (!strcmp(pcconvTableNames[i], name)) id = i + 1;   /* entry 0 is NULLFILE "" */
    modelCheck(rom, romLen, models, name, &st, 0);
    if (!row) return;
    pc = rowInflate(models, row, &D);
    pcvFindRowVariants(name, key, sizeof(key));
    n64 = romFile(rom, romLen, key, &n64Len);
    /* G_SETTIMG seg-5 targets: N64 GDL commands are 8-aligned, PC slots
     * {w0,0,w1,0} 4-aligned. d43 lays the blobs out in ascending source
     * order, so the sorted unique target lists pair up one-to-one. */
    for (g = 0; g + 8 <= n64Len; g += 8)
        if ((pcvBE32(n64, g) >> 24) == 0xFD && (pcvBE32(n64, g + 4) >> 24) == 0x05 &&
            !vhas(&srcT, pcvBE32(n64, g + 4) & 0xFFFFFF))
            vpush(&srcT, pcvBE32(n64, g + 4) & 0xFFFFFF);
    for (g = 0; g + 16 <= D; g += 4)
        if ((le32(pc, g) >> 24) == 0xFD && (le32(pc, g + 8) >> 24) == 0x05 && !le32(pc, g + 4) &&
            !le32(pc, g + 12) && !vhas(&pcT, le32(pc, g + 8) & 0xFFFFFF))
            vpush(&pcT, le32(pc, g + 8) & 0xFFFFFF);
    qsort(srcT.v, (size_t)srcT.n, sizeof(uint32_t), cmpU32);
    qsort(pcT.v, (size_t)pcT.n, sizeof(uint32_t), cmpU32);
    blobs = (uint32_t)pcT.n;
    if (srcT.n == pcT.n) {
        for (i = 0; i < pcT.n; i++) {
            uint32_t T = srcT.v[i], t = pcT.v[i];
            uint32_t len = (i + 1 < srcT.n ? srcT.v[i + 1] : (uint32_t)n64Len) - T;
            if (st.gdlMin > t && t + len > st.gdlMin) len = st.gdlMin - t;   /* blob region ends at the GDLs */
            if (t + len > D) len = (uint32_t)(D - t);
            if (T + len > n64Len) len = (uint32_t)(n64Len - T);
            if (!memcmp(n64 + T, pc + t, len)) eq++;
            for (k = 0; k < len; k++) if (pc[t + k]) { nonzero++; break; }
            blobBytes += len;
        }
    }
    CHECK(blobs > 0 && srcT.n == pcT.n && eq == blobs && nonzero > 0,
          "%s: %u PC SETTIMG targets vs %d N64, %u equal the source", name, blobs, srcT.n, eq);
    printf("  %-16s file id %3d: N64 %6zu B -> PC %6zu B (RZ %6u in pcmodels.bin); %u nodes, %u DLs; "
           "%u G_SETTIMG seg-5 image refs -> embedded blobs copied, %u/%u equal the cart bytes (%u B compared, %u non-blank)\n",
           name, id, n64Len, D, row->size, st.nodes, st.dls, blobs, eq, blobs, blobBytes, nonzero);
    free(srcT.v); free(pcT.v);
    free(pc); free(n64);
}

/* ------------------------------------------------------------ 6. donor ---*/
static int readManifest(const char *path, PcvImage *im)
{
    size_t len = 0;
    char *buf = (char *)pcvReadFile(path, &len), *line;
    if (!buf) return 0;
    buf = realloc(buf, len + 1);
    buf[len] = 0;
    for (line = buf; *line; ) {
        char nm[PCV_NAME_LEN], *end = line;
        unsigned off, sz;
        while (*end && *end != '\r' && *end != '\n') end++;
        if (*end) *end++ = 0;
        while (*end == '\r' || *end == '\n') end++;
        if (sscanf(line, "%63[^,],%u,%u", nm, &off, &sz) == 3) {
            if (im->n == im->cap) { im->cap = im->cap ? im->cap * 2 : 128; im->rows = realloc(im->rows, sizeof(PcvRow) * (size_t)im->cap); }
            memset(&im->rows[im->n], 0, sizeof(PcvRow));
            snprintf(im->rows[im->n].name, PCV_NAME_LEN, "%s", nm);
            im->rows[im->n].offset = off;
            im->rows[im->n].size = sz;
            im->n++;
        }
        line = end;
    }
    free(buf);
    return im->n;
}

/* donor stan tile header (D88.5: id bytes [b1 b0 b2 b3], mid/tail plain LE)
 * -> Midas's (rotr8 word, nibble-reversed halfwords); 1 when the two images
 * are otherwise identical */
static uint16_t nibrev(uint16_t v) { return (uint16_t)(((v & 0xF) << 12) | ((v & 0xF0) << 4) | ((v >> 4) & 0xF0) | (v >> 12)); }
static int stanDonorEquiv(const uint8_t *ours, const uint8_t *donor, size_t len)
{
    uint8_t *t = (uint8_t *)malloc(len);
    uint32_t o, first;
    int eq;
    memcpy(t, donor, len);
    first = le32(t, 8);
    for (o = first; o + 8 <= len && le32(t, o); ) {
        uint32_t w0 = ((uint32_t)t[o + 1] << 24) | ((uint32_t)t[o] << 16) | ((uint32_t)t[o + 2] << 8) | t[o + 3];
        uint16_t mid = le16(t, o + 4), tail = le16(t, o + 6), n = (uint16_t)((tail >> 12) & 0xF);
        uint32_t sz = n <= 10 ? list_of_tilesizes[n] : 0;
        if (!sz) break;
        pcvPut32(t, o, (w0 >> 8) | (w0 << 24));
        pcvPut16(t, o + 4, nibrev(mid));
        pcvPut16(t, o + 6, nibrev(tail));
        o += sz;
    }
    eq = !memcmp(t, ours, len);
    free(t);
    return eq;
}

/* setups: 1 when ours and the donor's differ only in the CREDITS intro word
 * and its CreditsEntry table (Midas R7A-2) */
static int setupCreditsOnlyDiff(const uint8_t *ours, const uint8_t *donor, size_t len)
{
    uint32_t o = le32(ours, 16), word = 0, t0 = 0, t1 = 0, i;
    while (o + 8 <= len) {
        int32_t t = (int32_t)le32(ours, o);
        if (t < 0 || t > 9) return 0;
        if (t == 8) {
            word = o + 4;
            t0 = le32(ours, o + 4);
            for (t1 = t0; t1 + 12 <= len; t1 += 12)
                if (!le16(ours, t1) && !le16(ours, t1 + 2)) { t1 += 12; break; }
        }
        o += (uint32_t)INTRO_SZ[t];
        if (t == 9) break;
    }
    if (!word) return 0;
    for (i = 0; i < len; i++)
        if (ours[i] != donor[i] && !(i >= word && i < word + 4) && !(i >= t0 && i < t1))
            return 0;
    return 1;
}

static void donorCompare(const PcvImage *ours, const char *dir, const char *tree, const char *bin)
{
    char p[1024];
    PcvImage d;
    size_t binLen = 0;
    int i, same = 0, rawSame = 0, diff = 0, stanDiff = 0, missing = 0, creditsDiff = 0;
    pcvImageInit(&d);
    snprintf(p, sizeof(p), "%s/%s/manifest.csv", dir, tree);
    if (!readManifest(p, &d)) { printf("  (no donor %s; skipped)\n", p); return; }
    snprintf(p, sizeof(p), "%s/%s/%s", dir, tree, bin);
    d.bin.p = pcvReadFile(p, &binLen);
    d.bin.len = binLen;
    if (!d.bin.p) { printf("  (no donor %s; skipped)\n", p); return; }
    for (i = 0; i < ours->n; i++) {
        const PcvRow *a = &ours->rows[i], *b = findRow(&d, a->name);
        uint8_t *x, *y;
        size_t xl = 0, yl = 0;
        int isRz;
        if (!b) { missing++; continue; }
        if (a->size == b->size && !memcmp(ours->bin.p + a->offset, d.bin.p + b->offset, a->size)) { rawSame++; same++; continue; }
        isRz = ours->bin.p[a->offset] == 0x11 && ours->bin.p[a->offset + 1] == 0x72;
        if (!isRz) { diff++; printf("  donor DIFF %s (raw)\n", a->name); continue; }
        x = rowInflate(ours, a, &xl);
        y = pcvInflateRZ(d.bin.p + b->offset, b->size, &yl);
        if (x && y && xl == yl && !memcmp(x, y, xl)) same++;
        else if (x && y && xl == yl && !strncmp(a->name, "Usetup", 6) && setupCreditsOnlyDiff(x, y, xl)) {
            creditsDiff++;
            printf("  donor %s differs only in the CREDITS word + table (Midas R7A-2 fix)\n", a->name);
        }
        else if (!strncmp(a->name, "Tbg_", 4) && x && y && xl == yl && stanDonorEquiv(x, y, xl)) {
            /* expected: Midas StandTile header words (R5); slots, points and
             * everything else equal, and each donor header word re-encodes
             * to exactly ours */
            stanDiff++;
        } else { diff++; printf("  donor DIFF %s (%zu vs %zu B)\n", a->name, xl, yl); }
        free(x); free(y);
    }
    printf("  donor %-9s %3d rows: %3d identical (%d byte-for-byte compressed), %d stans differ only in the "
           "tile header encoding (donor headers re-encoded to Midas's StandTile = ours, byte for byte), "
           "%d CREDITS fixes, %d other diffs, %d missing in donor\n",
           tree, ours->n, same, rawSame, stanDiff, creditsDiff, diff, missing);
    CHECK(diff == 0 && missing == 0, "donor compare %s", tree);
    pcvImageFree(&d);
}

int main(int argc, char **argv)
{
    const char *romPath = argc > 1 ? argv[1] : NULL;
    const char *donor = argc > 2 ? argv[2] : NULL;
    uint8_t *rom;
    size_t romLen = 0;
    PcvImage models, cg;
    struct ModelStats st;
    clock_t t0;
    int i, nModels = 0, okModels = 0;
    uint64_t sumNodes = 0, sumVerts = 0, sumDls = 0, sumSlots = 0, sumGfx = 0;

    if (!romPath) {
        fprintf(stderr, "usage: %s <ge007.z64> [donor engine/data dir]\n", argv[0]);
        return 2;
    }
    rom = pcvReadFile(romPath, &romLen);
    if (!rom || romLen < 0x1000 || rom[0] != 0x80 || rom[0x3E] != 'E') {
        fprintf(stderr, "not a z64-order NTSC-U GoldenEye cart: %s\n", romPath);
        return 2;
    }
    pcconvertSetLog(NULL);
    pcvImageInit(&models);
    pcvImageInit(&cg);
    t0 = clock();
    if (!pcconvertBuild(rom, romLen, &models, &cg)) {
        printf("FAIL: pcconvert: %d errors, first: %s\n", pcvErrCount(), pcvErrAt(0));
        return 1;
    }
    printf("pcconvert: %d model sidecars (%zu B pcmodels.bin) + %d stage sidecars (%zu B pccg.bin) in %.2f s CPU\n",
           models.n, models.bin.len, cg.n, cg.bin.len, (double)(clock() - t0) / CLOCKS_PER_SEC);

    printf("\n[1] models: PC tree vs port/src/romfile_model.c (Midas 32-bit path), all files\n");
    {
        static const char *const pick[] = { "PdamgatedoorZ", "GwppkZ", "CdjbondZ", "CrusguardZ" };
        for (i = 0; i < (int)(sizeof(pick) / sizeof(pick[0])); i++)
            modelCheck(rom, romLen, &models, pick[i], &st, 1);
    }
    for (i = 0; i < models.n; i++) {
        nModels++;
        if (modelCheck(rom, romLen, &models, models.rows[i].name, &st, 0))
            okModels++;
        sumNodes += st.nodes; sumVerts += st.verts; sumDls += st.dls; sumSlots += st.slots; sumGfx += st.gdlcmds;
    }
    printf("  all models: %d/%d match romfile_model.c node/vertex/DL counts with every pointer slot a seg-5 token "
           "or 0 and every Gfx 16 B (%llu nodes, %llu verts, %llu DLs, %llu Gfx, %llu slots)\n",
           okModels, nModels, (unsigned long long)sumNodes, (unsigned long long)sumVerts,
           (unsigned long long)sumDls, (unsigned long long)sumGfx, (unsigned long long)sumSlots);

    printf("  collision LinkedTo tokens: %u, of which %u have low half 0x0500 (model.c's raw unswap would corrupt them)\n",
           s_linkedTotal, s_linked0500);
    printf("\n[2] bg vs port/src/romfile_bg.c\n");
    bgCheck(rom, romLen, &cg, "bg/bg_dam_all_p.seg");
    printf("\n[3] stan vs port/src/romfile_stan.c\n");
    stanCheck(rom, romLen, &cg, "Tbg_dam_all_p_stanZ");
    for (i = 0; i < cg.n; i++)          /* every stan, quietly */
        if (!strncmp(cg.rows[i].name, "Tbg_", 4) && strcmp(cg.rows[i].name, "Tbg_dam_all_p_stanZ")) {
            stanCheck(rom, romLen, &cg, cg.rows[i].name);
        }
    for (i = 0; i < cg.n; i++)          /* every other bg, one line each */
        if (!strncmp(cg.rows[i].name, "bg/", 3) && strcmp(cg.rows[i].name, "bg/bg_dam_all_p.seg"))
            bgCheck(rom, romLen, &cg, cg.rows[i].name);
    printf("\n[4] setup\n");
    setupCheck(rom, romLen, &cg, "UsetupdamZ", 1);
    {
        int before = s_fail;
        for (i = 0; i < cg.n; i++)
            if ((!strncmp(cg.rows[i].name, "Usetup", 6) || !strncmp(cg.rows[i].name, "Ump_setup", 9)) &&
                strcmp(cg.rows[i].name, "UsetupdamZ"))
                setupCheck(rom, romLen, &cg, cg.rows[i].name, 0);
        printf("  all setups: %d checked (header, 8 tables, propDefs, intro), %s\n", s_setupsChecked,
               s_fail == before ? "all ok" : "FAILURES above");
    }
    printf("\n[5] menu-model trio (compiled-in KEEP set on x86)\n");
    menuCheck(rom, romLen, &models, "PlegalpageZ");
    menuCheck(rom, romLen, &models, "PnintendologoZ");
    menuCheck(rom, romLen, &models, "PgoldeneyelogoZ");
    if (donor) {
        printf("\n[6] against the donor's converter output (%s)\n", donor);
        donorCompare(&models, donor, "pcmodels-ntsc-final", "pcmodels.bin");
        donorCompare(&cg, donor, "pccg-ntsc-final", "pccg.bin");
    }
    printf("\n%s (%d failure%s)\n", s_fail ? "FAIL" : "PASS", s_fail, s_fail == 1 ? "" : "s");
    pcvImageFree(&models);
    pcvImageFree(&cg);
    free(rom);
    return s_fail ? 1 : 0;
}
