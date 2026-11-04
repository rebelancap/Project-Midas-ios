/* Imported from GoldenEye-Decomp-ios engine/port/src/pcconvert/d69.c at 55a3a9c8
 * (donor HEAD), Phase 1 round 7a (docs/port-ios/11-round7a-pcconvert.md).
 * CHANGED for Midas: convertStan emits Midas's StandTile encoding (R5) --
 * tile word 0 rotated, mid/tail nibble-reversed -- so its tile bytes equal
 * port/src/romfile_stan.c's output; the 8-byte room-slot array and the
 * bg/ *.seg conversion are the donor's, unchanged. */
/*
 * D69: C port of tools_pc/d69_emit.py — convert every stage bg/ *.seg and
 * Tbg_*_stanZ file to PC layout and append it to the concatenated pccg
 * sidecar image. Byte-for-byte equivalent to the Python (scripts/verify-
 * sidecars.sh proves it); the Python's own docstring is the format spec and
 * is not repeated here.
 */

#include <stdlib.h>
#include <string.h>


#include "pcconvert_internal.h"

#define ENVIRONMENTDATA_ALT 100

static const int TILESIZES[12] = {
    0x20, 0x20, 0x20, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0
};

static int cmpU32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

struct blobRegion { uint32_t start; uint32_t size; int isRecord; };

/* ----------------------------------------------------------- bg/ *.seg ---*/
static uint8_t *convertSeg(const char *name, const uint8_t *src, size_t D,
                           size_t *outLen)
{
    uint32_t h0, h1, h2, h3, h4, roomEnd, portalEnd, portalDelta;
    uint32_t *targets = NULL, *portalTargets = NULL;
    int nTargets = 0, nPortals = 0, nPortalTargets = 0;
    struct blobRegion *blob = NULL;
    int nBlob = 0, blobCap = 0;
    uint8_t *out = NULL;
    uint32_t o, cursor;
    int i, k;

    if (D < 0x40) {
        pcvErrf("%s: too small (%zu)", name, D);
        return NULL;
    }
    h0 = pcvBE32(src, 0);
    h1 = pcvBE32(src, 4) & PCV_MASK;
    h2 = pcvBE32(src, 8) & PCV_MASK;
    h3 = pcvBE32(src, 12) & PCV_MASK;
    h4 = pcvBE32(src, 16) & PCV_MASK;
    if (h0 != 0)
        pcvErrf("%s: header word0 != 0 (%#x)", name, h0);
    if (h4 != 0) {
        pcvErrf("%s: header word4 != 0 (%#x) -- f32 array path not implemented "
                "(spec D80); needs manual review", name, h4);
        return NULL;
    }
    if (h2 == 0 || h2 <= h1) {
        pcvErrf("%s: portal offset %#x <= room offset %#x", name, h2, h1);
        return NULL;
    }
    roomEnd = (h3 && h3 > h1 && h3 < h2) ? h3 : h2;
    if ((roomEnd - h1) % 24 != 0) {
        pcvErrf("%s: room table extent %#x not a multiple of 24",
                name, roomEnd - h1);
        return NULL;
    }

    /* portal table walk */
    portalTargets = (uint32_t *)malloc((size_t)(sizeof(uint32_t) *
                                       (D / 8 + 2)));
    if (!portalTargets)
        return NULL;
    o = h2;
    {
        int terminated = 0;
        while ((size_t)o + 8 <= D) {
            uint32_t v = pcvBE32(src, o) & PCV_MASK;
            if (v == 0) { terminated = 1; break; }
            portalTargets[nPortalTargets++] = v;
            nPortals++;
            o += 8;
        }
        if (!terminated) {
            pcvErrf("%s: portal table terminator not found before EOF", name);
            free(portalTargets);
            return NULL;
        }
    }
    portalEnd = o + 8;
    portalDelta = 8u * ((uint32_t)nPortals + 1u);

    /* ALT-envdata targets share the portal blob's offset space */
    targets = (uint32_t *)malloc((size_t)(sizeof(uint32_t) *
                                 (size_t)(nPortalTargets + (D / 8) + 2)));
    if (!targets) { free(portalTargets); return NULL; }
    for (i = 0; i < nPortalTargets; i++)
        targets[nTargets++] = portalTargets[i];
    if (h3 && roomEnd == h3) {
        uint32_t o2 = h3;
        while (o2 < h2) {
            uint8_t etype = src[o2];
            int32_t data = pcvBES32(src, o2 + 4);
            if (etype == ENVIRONMENTDATA_ALT && data)
                targets[nTargets++] = (uint32_t)data & PCV_MASK;
            o2 += 8;
            if (etype == 0)
                break;
        }
    }
    free(portalTargets);

    /* sorted(set(...)) */
    qsort(targets, (size_t)nTargets, sizeof(uint32_t), cmpU32);
    {
        int w = 0;
        for (i = 0; i < nTargets; i++)
            if (w == 0 || targets[w - 1] != targets[i])
                targets[w++] = targets[i];
        nTargets = w;
    }

    /* tile [portalEnd, D) with the referenced portal-entry records */
    blobCap = nTargets * 2 + 4;
    blob = (struct blobRegion *)malloc((size_t)(sizeof(*blob) *
                                       (size_t)blobCap));
    if (!blob) { free(targets); return NULL; }
    cursor = portalEnd;
    for (i = 0; i < nTargets; i++) {
        uint32_t t = targets[i];
        uint32_t numPoints, recsize;
        if (t < cursor) {
            pcvErrf("%s: portal target %#x overlaps previous region "
                    "(cursor %#x)", name, t, cursor);
            goto fail;
        }
        if (t > cursor) {
            blob[nBlob].start = cursor; blob[nBlob].size = t - cursor;
            blob[nBlob].isRecord = 0; nBlob++;
        }
        numPoints = src[t];
        if (numPoints == 0 || numPoints > 20) {
            pcvErrf("%s: portal target %#x numPoints=%u implausible",
                    name, t, numPoints);
            goto fail;
        }
        recsize = 4 + 12 * numPoints;
        if ((size_t)t + recsize > D) {
            pcvErrf("%s: portal target %#x size %#x overruns EOF",
                    name, t, recsize);
            goto fail;
        }
        blob[nBlob].start = t; blob[nBlob].size = recsize;
        blob[nBlob].isRecord = 1; nBlob++;
        cursor = t + recsize;
    }
    if ((size_t)cursor < D) {
        blob[nBlob].start = cursor; blob[nBlob].size = (uint32_t)(D - cursor);
        blob[nBlob].isRecord = 0; nBlob++;
    }

    out = (uint8_t *)malloc((size_t)(D + portalDelta));
    if (!out)
        goto fail;
    memset(out, 0, D + portalDelta);

    /* header */
    pcvPut32(out, 0, h0);
    pcvPut32(out, 4, 0x0F000000u | h1);
    pcvPut32(out, 8, 0x0F000000u | h2);
    pcvPut32(out, 12, h3 ? (0x0F000000u | h3) : 0u);
    pcvPut32(out, 16, 0);
    memcpy(out + 20, src + 20, 0x40 - 20);

    /* room table: 3 delta-relocated offsets + 3 plain f32 words */
    for (o = h1; o < roomEnd; o += 24) {
        for (k = 0; k < 3; k++) {
            uint32_t v = pcvBE32(src, o + 4 * (uint32_t)k) & PCV_MASK;
            uint32_t nv = v ? (0x0F000000u | ((v + portalDelta) & PCV_MASK)) : 0u;
            pcvPut32(out, o + 4 * (uint32_t)k, nv);
        }
        for (k = 0; k < 3; k++) {
            uint32_t fo = o + 12 + 4 * (uint32_t)k;
            pcvPut32(out, fo, pcvBE32(src, fo));
        }
    }

    /* envdata */
    if (h3 && roomEnd == h3) {
        uint32_t o2 = h3;
        while (o2 < h2) {
            uint8_t etype = src[o2];
            int32_t data = pcvBES32(src, o2 + 4);
            out[o2] = etype;
            memcpy(out + o2 + 1, src + o2 + 1, 3);
            if (etype == ENVIRONMENTDATA_ALT && data)
                data = (int32_t)((uint32_t)data + portalDelta);
            pcvPut32(out, o2 + 4, (uint32_t)data);
            o2 += 8;
            if (etype == 0)
                break;
        }
    }

    /* portal table: 8B N64 records -> 16B PC records, table start unchanged */
    {
        uint32_t o2 = h2, pcPos = h2;
        int idx;
        for (idx = 0; idx <= nPortals; idx++) {
            uint32_t offVal = pcvBE32(src, o2) & PCV_MASK;
            if (offVal)
                pcvPut32(out, pcPos, 0x0F000000u |
                         ((offVal + portalDelta) & PCV_MASK));
            else
                pcvPut32(out, pcPos, 0);
            pcvPut32(out, pcPos + 4, 0);
            out[pcPos + 8]  = src[o2 + 4];
            out[pcPos + 9]  = src[o2 + 5];
            out[pcPos + 10] = src[o2 + 6];
            out[pcPos + 11] = src[o2 + 7];
            pcvPut32(out, pcPos + 12, 0);
            o2 += 8;
            pcPos += 16;
        }
    }

    /* portal point-data blob, relocated by +portalDelta */
    for (i = 0; i < nBlob; i++) {
        uint32_t start = blob[i].start, size = blob[i].size;
        uint32_t dst = start + portalDelta;
        if (!blob[i].isRecord) {
            memcpy(out + dst, src + start, size);
            continue;
        }
        out[dst] = src[start];
        memcpy(out + dst + 1, src + start + 1, 3);
        for (k = 0; k < (int)((size - 4) / 4); k++)
            pcvPut32(out, dst + 4 + 4 * (uint32_t)k,
                     pcvBE32(src, start + 4 + 4 * (uint32_t)k));
    }

    free(targets);
    free(blob);
    *outLen = D + portalDelta;
    return out;

fail:
    free(targets);
    free(blob);
    free(out);
    return NULL;
}

/* -------------------------------------------------------- Tbg_*_stanZ ---*/
/* Midas: N64 halfword value -> its four nibbles in reverse order (the host
 * value romfile_stan.c's nibbleReverse() stores). */
static uint16_t stanNibbleReverse(uint16_t v)
{
    return (uint16_t)(((v & 0xFu) << 12) | ((v & 0xF0u) << 4) |
                      ((v >> 4) & 0xF0u) | (v >> 12));
}

static uint8_t *convertStan(const char *name, const uint8_t *src, size_t D,
                            size_t *outLen)
{
    int32_t stanfile;
    uint32_t *offs = NULL;
    int nOffs = 0, N;
    uint32_t o, oldArrayEnd, arrayDelta, newTileStart, p, srcO, dstO;
    uint8_t *out = NULL;
    int terminated = 0;

    if (D < 8) {
        pcvErrf("%s: too small (%zu)", name, D);
        return NULL;
    }
    stanfile = pcvBES32(src, 0);
    offs = (uint32_t *)malloc((size_t)(sizeof(uint32_t) * (D / 4 + 2)));
    if (!offs)
        return NULL;
    o = 4;
    while ((size_t)o + 4 <= D) {
        uint32_t v = pcvBE32(src, o);
        offs[nOffs++] = v;
        o += 4;
        if (v == 0) { terminated = 1; break; }
    }
    if (!terminated) {
        pcvErrf("%s: room-offset array terminator not found", name);
        free(offs);
        return NULL;
    }
    N = nOffs - 1;
    oldArrayEnd = o;
    arrayDelta = 4u * ((uint32_t)N + 2u);
    newTileStart = oldArrayEnd + arrayDelta;

    out = (uint8_t *)malloc((size_t)(D + arrayDelta));
    if (!out) { free(offs); return NULL; }
    memset(out, 0, D + arrayDelta);

    pcvPut32(out, 0, (uint32_t)stanfile);
    pcvPut32(out, 4, 0);
    p = 8;
    {
        int i;
        for (i = 0; i < nOffs; i++) {
            pcvPut32(out, p, offs[i] ? (offs[i] + arrayDelta) : 0u);
            pcvPut32(out, p + 4, 0);
            p += 8;
        }
    }
    free(offs);
    if (p != newTileStart) {
        pcvErrf("%s: stan array layout mismatch (%u != %u)", name, p, newTileStart);
        free(out);
        return NULL;
    }

    srcO = oldArrayEnd;
    dstO = newTileStart;
    terminated = 0;
    while ((size_t)srcO + 4 <= D) {
        int16_t midhalf, tailhalf;
        int pointCount, sz, i;
        if (src[srcO] == 0 && src[srcO + 1] == 0 && src[srcO + 2] == 0 &&
            src[srcO + 3] == 0) {
            pcvPut32(out, dstO, 0);
            terminated = 1;
            break;
        }
        /* Midas (R5, docs/port-ios/04-round1-prereqs.md): StandTile keeps
         * Midas's `u32 id:24; u32 room:8` and LSB-first header bitfields,
         * so this emits exactly what port/src/romfile_stan.c produces (the
         * donor's `u8 id[3]` form swaps only the idHi half, D88.5):
         *  - word 0: the N64 word rotated right by 8 (id low 24, room high 8);
         *  - mid/tail: the N64 halfword with its four nibbles reversed
         *    (stan.h STAN_PTCOUNT / STAN_MID_NIB, !TARGET_N64 arm). */
        {
            uint32_t w0 = pcvBE32(src, srcO);
            pcvPut32(out, dstO, (w0 >> 8) | (w0 << 24));
        }
        midhalf  = pcvBES16(src, srcO + 4);
        tailhalf = pcvBES16(src, srcO + 6);
        pcvPut16(out, dstO + 4, stanNibbleReverse((uint16_t)midhalf));
        pcvPut16(out, dstO + 6, stanNibbleReverse((uint16_t)tailhalf));
        pointCount = ((int32_t)tailhalf >> 12) & 0xF;
        sz = TILESIZES[pointCount];
        if (sz == 0) {
            pcvErrf("%s: bad pointCount %d at src %#x", name, pointCount, srcO);
            free(out);
            return NULL;
        }
        if ((size_t)srcO + (size_t)sz > D) {
            pcvErrf("%s: tile at %#x size %#x overruns EOF %#zx",
                    name, srcO, sz, D);
            free(out);
            return NULL;
        }
        for (i = 0; i < pointCount; i++) {
            uint32_t po = srcO + 8 + 8 * (uint32_t)i;
            uint32_t qo = dstO + 8 + 8 * (uint32_t)i;
            pcvPut16(out, qo,     (uint16_t)pcvBES16(src, po));
            pcvPut16(out, qo + 2, (uint16_t)pcvBES16(src, po + 2));
            pcvPut16(out, qo + 4, (uint16_t)pcvBES16(src, po + 4));
            pcvPut16(out, qo + 6, pcvBE16(src, po + 6));
        }
        srcO += (uint32_t)sz;
        dstO += (uint32_t)sz;
    }
    if (!terminated) {
        pcvErrf("%s: ran off EOF without an all-zero EOF sentinel", name);
        free(out);
        return NULL;
    }

    if ((size_t)srcO + 4 < D)
        memcpy(out + dstO + 4, src + srcO + 4, D - (size_t)(srcO + 4));

    *outLen = D + arrayDelta;
    return out;
}

/* ------------------------------------------------------------------ main ---*/
static int endsWith(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcmp(s + ls - lf, suf);
}

int pcvD69(const uint8_t *rom, size_t romLen, PcvImage *im)
{
    int t, nSeg = 0, nStan = 0;

    for (t = 0; t < pcconvTableNameCount; t++) {
        const char *name = pcconvTableNames[t];
        const struct PcvFileRow *row;
        uint8_t *out = NULL;
        size_t outLen = 0;

        if (!strncmp(name, "bg/", 3) && endsWith(name, ".seg")) {
            char base[PCV_NAME_LEN];
            const char *slash = strrchr(name, '/');
            const char *leaf = slash ? slash + 1 : name;
            size_t n = strlen(leaf);
            if (n < 4 || n - 4 >= sizeof(base))
                continue;
            memcpy(base, leaf, n - 4);
            base[n - 4] = 0;
            row = pcvFindRowExact(base);
            if (!row || row->size == 0)
                continue;
            if ((size_t)row->addr + row->size > romLen) {
                pcvErrf("%s: filelist row past EOF", name);
                continue;
            }
            out = convertSeg(name, rom + row->addr, row->size, &outLen);
            if (out) {
                pcvImageEmit(im, name, out, outLen);
                free(out);
                nSeg++;
            }
        } else if ((!strncmp(name, "Tbg_", 4) || !strcmp(name, "TbgxZ")) &&
                   endsWith(name, "Z")) {
            uint8_t *dec;
            size_t decLen = 0;
            row = pcvFindRowExact(name);
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
            out = convertStan(name, dec, decLen, &outLen);
            free(dec);
            if (out) {
                size_t compLen = 0;
                uint8_t *comp = pcvDeflateRZ(out, outLen, &compLen);
                free(out);
                if (!comp)
                    continue;
                pcvImageEmit(im, name, comp, compLen);
                free(comp);
                nStan++;
            }
        }
    }

    pcvLog("pcconvert: d69 converted %d bg/*.seg + %d Tbg_*_stanZ files",
           nSeg, nStan);
    return pcvErrCount() ? 0 : 1;
}
