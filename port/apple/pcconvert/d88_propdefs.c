/* Imported from GoldenEye-Decomp-ios engine/port/src/pcconvert/d88_propdefs.c at 55a3a9c8
 * (donor HEAD), Phase 1 round 7a
 * (docs/port-ios/11-round7a-pcconvert.md). Unchanged (PD_PCB pinned
 * against Midas's record structs by pcconvert_layout.c). */
/*
 * D88.4: C port of tools_pc/d88_propdefs.py — convert the `propDefs`
 * polymorphic record stream of a Usetup*Z stage file from N64 layout to the
 * native PC struct layout. See the Python docstring (and internals.md D88.4 /
 * D122 / D123 / D126 / D132) for the ground truth behind every table here.
 */

#include <stdlib.h>
#include <string.h>


#include "pcconvert_internal.h"

#define PD_MAXTYPE 48

/* serialized N64 word count per type byte; -1 = unknown type */
static const short PD_N64W[PD_MAXTYPE + 1] = {
    /*0*/ -1, 64, 2, 32, 33, 32, 59, 33, 34, 7,
    /*10*/ 64, 149, 32, 54, 3, -1, -1, 32, 3, 4,
    /*20*/ 45, 34, 4, 4, 1, 2, 2, 2, 2, 2,
    /*30*/ 4, 1, 4, 5, 3, 4, 32, 10, 4, 44,
    /*40*/ 45, -1, 32, 32, 5, 56, 7, 37, 1
};

/* native PC struct size per type byte; -1 = unknown */
static const short PD_PCB[PD_MAXTYPE + 1] = {
    /*0*/ -1, 296, 8, 144, 152, 144, 272, 152, 160, 32,
    /*10*/ 288, 664, 144, 248, 32, -1, -1, 144, 12, 32,
    /*20*/ 200, 152, 24, 16, 4, 8, 8, 8, 8, 8,
    /*30*/ 24, 4, 24, 24, 12, 24, 144, 48, 32, 208,
    /*40*/ 208, -1, 144, 144, 40, 248, 28, 168, 4
};

#define OBJ_N64_WORDS 32
#define OBJ_PC_BYTES  144

static int objPtrWord(int i) { return i == 4 || i == 5 || i == 26 || i == 27; }

static void pdBswap32(uint8_t *o, const uint8_t *w)
{
    o[0] = w[3]; o[1] = w[2]; o[2] = w[1]; o[3] = w[0];
}
/* [u16 extrascale][u8 state][u8 type] -> swap the u16, keep the two bytes */
static void pdHdrWord(uint8_t *o, const uint8_t *w)
{
    o[0] = w[1]; o[1] = w[0]; o[2] = w[2]; o[3] = w[3];
}
/* [hi16][lo16] -> byte-swap each half independently */
static void pdHhWord(uint8_t *o, const uint8_t *w)
{
    o[0] = w[1]; o[1] = w[0]; o[2] = w[3]; o[3] = w[2];
}

static void pdEmitObjectPrefix(uint8_t *out, const uint8_t *src, size_t so)
{
    size_t pc = 0;
    int i;
    for (i = 0; i < OBJ_N64_WORDS; i++) {
        const uint8_t *w = src + so + 4 * (size_t)i;
        if (objPtrWord(i)) {
            pc = (pc + 7) & ~(size_t)7;
            pc += 8;                       /* already zero */
        } else if (i == 0) {
            pdHdrWord(out + pc, w); pc += 4;
        } else if (i == 1) {
            pdHhWord(out + pc, w); pc += 4;
        } else if (i == 30 || i == 31) {
            memcpy(out + pc, w, 4); pc += 4;   /* shadecol/nextcol rgba */
        } else {
            pdBswap32(out + pc, w); pc += 4;
        }
    }
}

/* OBJ_TAIL_DESC / OBJ_ID_WORDS as predicates (D122 / D123) */
static int tailHasDesc(int t)
{
    return t == 47 || t == 39 || t == 40 || t == 45 || t == 13 || t == 20;
}
static int tailPtrWord(int t, int i)
{
    switch (t) {
    case 39: return i == 41 || i == 43;
    case 40: return i == 43 || i == 44;
    case 45: return i == 32;
    case 13: return i == 49 || i == 50 || i == 51;
    default: return 0;
    }
}
static int tailHhWord(int t, int i)
{
    switch (t) {
    case 39: case 40: return i == 33;
    case 20: return i >= 32 && i <= 44;
    default: return 0;
    }
}
static int tailIdWord(int t, int i)
{
    return (t == 39 || t == 40) && i == 32;
}
static int doorTailPtrWord(int i)
{
    return i == 50 || i == 51 || i == 61 || i == 62;
}

/* Returns PD_PCB[t] bytes into `out` (already zeroed). */
static void pdConvertRecord(uint8_t *out, const uint8_t *src, size_t so, int t)
{
    int n64w = PD_N64W[t];
    int i;
    size_t pc;

    switch (t) {
    case 3: case 5: case 12: case 17: case 36: case 42: case 43:
        pdEmitObjectPrefix(out, src, so);
        return;

    case 1:   /* DOOR */
        pdEmitObjectPrefix(out, src, so);
        pc = OBJ_PC_BYTES;
        for (i = OBJ_N64_WORDS; i < n64w; i++) {
            const uint8_t *w = src + so + 4 * (size_t)i;
            if (doorTailPtrWord(i)) {
                pc = (pc + 7) & ~(size_t)7;
                pc += 8;
            } else if (i == 38) {
                pdHhWord(out + pc, w); pc += 4;
            } else if (i == 47) {
                out[pc] = w[0]; out[pc + 1] = w[1];
                out[pc + 2] = w[3]; out[pc + 3] = w[2];
                pc += 4;
            } else if (i == 49) {
                out[pc] = w[1]; out[pc + 1] = w[0];
                out[pc + 2] = w[2]; out[pc + 3] = w[3];
                pc += 4;
            } else {
                pdBswap32(out + pc, w); pc += 4;
            }
        }
        return;

    case 4: case 21: case 7:   /* KEY / ARMOUR / MAGAZINE */
        pdEmitObjectPrefix(out, src, so);
        pc = OBJ_PC_BYTES;
        for (i = OBJ_N64_WORDS; i < n64w; i++) {
            pdBswap32(out + pc, src + so + 4 * (size_t)i);
            pc += 4;
        }
        return;

    case 8: {  /* COLLECTABLE */
        const uint8_t *w = src + so + 4 * 32;
        pdEmitObjectPrefix(out, src, so);
        out[144] = w[0]; out[145] = w[1]; out[146] = w[3]; out[147] = w[2];
        return;
    }

    case 10: case 11: {   /* SingleMonitor / MultiMonitor */
        int nmon = (t == 10) ? 1 : 4;
        int tailStart = OBJ_N64_WORDS + 29 * nmon;
        pdEmitObjectPrefix(out, src, so);
        pc = (size_t)OBJ_PC_BYTES + 128u * (size_t)nmon;
        for (i = tailStart; i < n64w; i++) {
            const uint8_t *w = src + so + 4 * (size_t)i;
            if (t == 11)
                memcpy(out + pc, w, 4);       /* ImageNums u8[4] verbatim */
            else
                pdBswap32(out + pc, w);
            pc += 4;
        }
        return;
    }

    case 6:   /* CCTV */
        pdEmitObjectPrefix(out, src, so);
        pdBswap32(out + 144, src + so + 4 * 32);
        pc = 212;
        for (i = 49; i < n64w; i++) {
            pdBswap32(out + pc, src + so + 4 * (size_t)i);
            pc += 4;
        }
        return;

    case 22:  /* TAG */
        pdHdrWord(out, src + so);
        pdHhWord(out + 4, src + so + 4);
        return;

    case 37:  /* RENAME */
        pdHdrWord(out, src + so);
        for (i = 1; i < 9; i++)
            pdBswap32(out + 4 * (size_t)i, src + so + 4 * (size_t)i);
        return;

    case 14: case 19: case 38: case 44: {   /* LINK / SWITCH / LOCK_DOOR / SAFE_ITEM */
        int nidx = (t == 44) ? 3 : 2;
        int kk;
        pdHdrWord(out, src + so);
        for (kk = 0; kk < nidx; kk++)
            pdBswap32(out + 8 + 8 * (size_t)kk, src + so + 4 * (size_t)(kk + 1));
        return;
    }

    case 18: {  /* SetGuardAttribute */
        const uint8_t *w = src + so + 8;
        pdHdrWord(out, src + so);
        pdBswap32(out + 4, src + so + 4);
        out[8] = w[1]; out[9] = w[0]; out[10] = w[2]; out[11] = w[3];
        return;
    }

    case 9:   /* GUARD */
        pdHdrWord(out, src + so);
        for (i = 1; i < 6; i++)
            pdHhWord(out + 4 * (size_t)i, src + so + 4 * (size_t)i);
        return;

    case 30: case 32: case 33: case 35:   /* D126 objective sub-records */
        pdHdrWord(out, src + so);
        for (i = 1; i < n64w - 1; i++)
            pdBswap32(out + 4 * (size_t)i, src + so + 4 * (size_t)i);
        return;

    default:
        break;
    }

    if (tailHasDesc(t)) {   /* D122: ObjectRecord prefix + typed tail */
        pdEmitObjectPrefix(out, src, so);
        pc = OBJ_PC_BYTES;
        for (i = OBJ_N64_WORDS; i < n64w; i++) {
            const uint8_t *w = src + so + 4 * (size_t)i;
            if (tailIdWord(t, i)) {
                pc = (pc + 7) & ~(size_t)7;
                pdBswap32(out + pc, w);
                pc += 8;
            } else if (tailPtrWord(t, i)) {
                pc = (pc + 7) & ~(size_t)7;
                pc += 8;
            } else if (tailHhWord(t, i)) {
                pdHhWord(out + pc, w); pc += 4;
            } else {
                pdBswap32(out + pc, w); pc += 4;
            }
        }
        return;
    }

    /* generic: header-only records */
    pdHdrWord(out, src + so);
    for (i = 1; i < n64w; i++)
        pdBswap32(out + 4 * (size_t)i, src + so + 4 * (size_t)i);
}

uint8_t *pcvPropDefsConvert(const uint8_t *src, size_t start, size_t end,
                            size_t *outLen, const char *name)
{
    PcvBuf out;
    size_t o = start;
    int n = 0;

    pcvBufInit(&out);
    while (o < end) {
        int t, n64w, pcb;
        uint8_t *rec;
        if (o + 4 > end) {
            pcvErrf("%s: propDefs: truncated header at %#zx", name, o);
            goto fail;
        }
        t = src[o + 3];
        if (t < 0 || t > PD_MAXTYPE || PD_N64W[t] < 0) {
            pcvErrf("%s: propDefs: unknown record type %d at %#zx (rec #%d)",
                    name, t, o, n);
            goto fail;
        }
        n64w = PD_N64W[t];
        pcb  = PD_PCB[t];
        if (o + 4 * (size_t)n64w > end) {
            pcvErrf("%s: propDefs: record type %d at %#zx overruns region end",
                    name, t, o);
            goto fail;
        }
        rec = (uint8_t *)malloc((size_t)pcb);
        if (!rec)
            goto fail;
        memset(rec, 0, (size_t)pcb);
        pdConvertRecord(rec, src, o, t);
        if (!pcvBufAppend(&out, rec, (size_t)pcb)) { free(rec); goto fail; }
        free(rec);
        o += 4 * (size_t)n64w;
        n++;
        if (t == 48)
            break;
    }
    if (o != end) {
        size_t pad = end - o, i;
        int nonzero = 0;
        for (i = o; i < end; i++)
            if (src[i]) { nonzero = 1; break; }
        if (pad >= 16 || nonzero) {
            pcvErrf("%s: propDefs: walk ended at %#zx, region end %#zx (pad=%zu)",
                    name, o, end, pad);
            goto fail;
        }
        if (!pcvBufAppend(&out, src + o, pad))
            goto fail;
    }
    *outLen = out.len;
    if (!out.p && !pcvBufResize(&out, 0))
        goto fail;
    return out.p;   /* ownership transfers to the caller */

fail:
    pcvBufFree(&out);
    return NULL;
}
