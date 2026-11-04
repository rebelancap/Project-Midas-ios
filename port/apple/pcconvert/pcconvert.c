/* Imported from GoldenEye-Decomp-ios engine/port/src/pcconvert/pcconvert.c at
 * 55a3a9c8 (donor HEAD), Phase 1 round 7a (docs/port-ios/11-round7a-pcconvert.md).
 * CHANGED for Midas: host stdio instead of the donor's fs.h/system.h; a log
 * sink (pcconvertSetLog) instead of sysLogPrintf; pcconvertBuild() split out
 * for the host test; pcconvertEnsureImage() hashes romdata.c's in-memory,
 * z64-normalised image; stamp tag "midas-<n>" (d69 stan output differs). */
/*
 * pcconvert — first-run sidecar conversion, natively, in C.
 *
 * Replaces port/src/romconvert.c (PR #84's PyInstaller `ge007-convert`
 * subprocess). The engine needs two ROM-derived sidecar trees that are never
 * shipped with a release because they are derived from the user's own ROM:
 *
 *     <data>/pcmodels-<region>/{pcmodels.bin,manifest.csv}   (d43)
 *     <data>/pccg-<region>/{pccg.bin,manifest.csv}           (d69 + d88)
 *
 * pcconvertEnsure() computes the ROM's SHA-1, compares it against
 * <data>/pcconvert.stamp, and — on a miss, or when any output is absent —
 * regenerates all of them from the ROM plus the committed metadata embedded
 * at build time (pcconvert_tables.c). The output is byte-identical to the
 * three Python scripts in tools_pc/ (see scripts/verify-sidecars.sh).
 *
 * Returns 1 when the sidecars are present (already, or after converting),
 * -1 when they could not be produced (the caller aborts boot rather than
 * crashing later in modelPromoteNodeOffsetsToPointers, D179).
 *
 * There is no Python, no subprocess and no second binary at runtime on any
 * platform — iOS blocker #2 (charter rule 4).
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>          /* Midas: host stdio replaces the donor's fs.h */

#if defined(_WIN32)
  #include <direct.h>
  #define PCV_MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #define PCV_MKDIR(p) mkdir((p), 0755)
#endif

#include "pcconvert.h"
#include "pcconvert_internal.h"

/* Bump whenever a converter's output changes; the stamp carries it so an
 * engine upgrade regenerates rather than serving stale sidecars. */
/* Midas: "midas-<n>". The donor's history (its "4" = d43 blob copy, D303
 * star-flash x4, D-205 Ump_setup) is all in this code; the tag differs because
 * d69's stan output is Midas's StandTile encoding, so a donor stamp must
 * never validate a Midas data dir or vice versa. */
#define PCCONVERT_VERSION "midas-1"

/* Midas: the donor logs through sysLogPrintf; here a sink the caller sets
 * (the game passes portConLogf-alike, the host test its own). Default: stderr. */
static void (*s_logSink)(const char *line);

void pcconvertSetLog(void (*sink)(const char *line))
{
    s_logSink = sink;
}

void pcvLog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_logSink)
        s_logSink(buf);
    else
        fprintf(stderr, "%s\n", buf);
}

/* ----------------------------------------------------------------- SHA-1 ---
 * RFC 3174, ~80 lines. There was no SHA-1 anywhere in the engine (grep
 * `sha1` finds only the ge007.*.sha1 text files and docs), so this is new.
 */
struct Sha1Ctx {
    uint32_t h[5];
    uint64_t len;
    uint8_t  blk[64];
    size_t   n;
};

static uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1Block(struct Sha1Ctx *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, tmp;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t fn;
        if (i < 20)      { fn = (b & d) | (~b & e);             k = 0x5A827999u; }
        else if (i < 40) { fn = b ^ d ^ e;                      k = 0x6ED9EBA1u; }
        else if (i < 60) { fn = (b & d) | (b & e) | (d & e);    k = 0x8F1BBCDCu; }
        else             { fn = b ^ d ^ e;                      k = 0xCA62C1D6u; }
        tmp = rol32(a, 5) + fn + f + k + w[i];
        f = e; e = d; d = rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void sha1Init(struct Sha1Ctx *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
}

static void sha1Update(struct Sha1Ctx *c, const uint8_t *p, size_t n)
{
    c->len += (uint64_t)n;
    while (n) {
        size_t take = 64 - c->n;
        if (take > n) take = n;
        memcpy(c->blk + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha1Block(c, c->blk); c->n = 0; }
    }
}

static void sha1Hex(struct Sha1Ctx *c, char out[41])
{
    static const char hexd[] = "0123456789abcdef";
    uint64_t bits = c->len * 8u;
    uint8_t pad[72];
    size_t padLen;
    int i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    padLen = (c->n < 56) ? (56 - c->n) : (120 - c->n);
    for (i = 0; i < 8; i++)
        pad[padLen + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1Update(c, pad, padLen + 8);   /* c->len is now meaningless; unused */
    for (i = 0; i < 20; i++) {
        uint8_t byte = (uint8_t)(c->h[i / 4] >> (8 * (3 - (i % 4))));
        out[2 * i]     = hexd[byte >> 4];
        out[2 * i + 1] = hexd[byte & 0xF];
    }
    out[40] = 0;
}

/* ------------------------------------------------------------- file I/O ---*/
/* Midas: plain stdio (the donor's fs.h layer resolves $S/$C path prefixes;
 * here the caller passes resolved paths). */
uint8_t *pcvReadFile(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *buf;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static int pcvExists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int pcvWriteFile(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) {
        pcvLog("pcconvert: cannot write %s", path);
        return 0;
    }
    ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0)
        ok = 0;
    if (!ok)
        pcvLog("pcconvert: short write to %s", path);
    return ok;
}

/* Write "name,offset,size" exactly as Python's csv.writer does: minimal
 * quoting (no field here ever contains a comma, quote or newline), decimal
 * integers, "\r\n" line terminator. */
static int pcvWriteManifest(const char *path, const PcvImage *im)
{
    PcvBuf b;
    char line[128];
    int i, ok;

    pcvBufInit(&b);
    if (!pcvBufAppend(&b, "name,offset,size\r\n", 18)) { pcvBufFree(&b); return 0; }
    for (i = 0; i < im->n; i++) {
        int n = snprintf(line, sizeof(line), "%s,%u,%u\r\n", im->rows[i].name,
                         (unsigned)im->rows[i].offset, (unsigned)im->rows[i].size);
        if (n <= 0 || !pcvBufAppend(&b, line, (size_t)n)) { pcvBufFree(&b); return 0; }
    }
    ok = pcvWriteFile(path, b.p, b.len);
    pcvBufFree(&b);
    return ok;
}

const char *pcconvertRegionForCountry(unsigned char country)
{
    switch (country) {
    case 'E': return "ntsc-final";
    case 'P': return "pal-final";
    case 'J': return "jpn-final";
    default:  return NULL;
    }
}

/* --------------------------------------------------------------- driver ---*/
/* Midas: the three converters as one call (the host test and the driver
 * below share it). The images are the exact bytes pcmodels.bin / pccg.bin
 * hold. Returns 1, or 0 with the pcvErr list filled. */
int pcconvertBuild(const uint8_t *rom, size_t romLen, PcvImage *models, PcvImage *cg)
{
    pcvErrReset();
    pcvD43(rom, romLen, models);
    pcvD69(rom, romLen, cg);
    pcvD88(rom, romLen, cg);
    return pcvErrCount() == 0;
}

int pcconvertEnsure(const char *romPath, const char *dataDir)
{
    uint8_t *rom;
    size_t romLen = 0;
    int rc;

    rom = pcvReadFile(romPath, &romLen);
    if (!rom || romLen < 0x40) {
        pcvLog("pcconvert: cannot read ROM %s", romPath);
        free(rom);
        return -1;
    }
    rc = pcconvertEnsureImage(rom, romLen, dataDir);
    free(rom);
    return rc;
}

/* Midas: romdata.c already holds the validated, z64-normalised cart image
 * (it accepts .v64/.n64 too), so the stamp hashes that image, not the file. */
int pcconvertEnsureImage(const uint8_t *rom, size_t romLen, const char *dataDir)
{
    const char *region;
    char stampPath[1024], dirPath[1024], filePath[1024];
    char sha[41], want[256];
    struct Sha1Ctx sc;
    PcvImage models, cg;
    size_t stampLen = 0;
    uint8_t *stamp;
    int i, rc = -1;
    int needRun = 1;

    pcvImageInit(&models);
    pcvImageInit(&cg);

    if (!rom || romLen < 0x40)
        return -1;
    region = pcconvertRegionForCountry(rom[0x3E]);
    if (!region)
        return -1;  /* Midas: romdata only accepts the stock NTSC-U cart */

    sha1Init(&sc);
    sha1Update(&sc, rom, romLen);
    sha1Hex(&sc, sha);
    snprintf(want, sizeof(want), "pcconvert %s %s %s\n",
             PCCONVERT_VERSION, region, sha);

    snprintf(stampPath, sizeof(stampPath), "%s/pcconvert.stamp", dataDir);
    stamp = pcvReadFile(stampPath, &stampLen);
    if (stamp) {
        if (stampLen == strlen(want) && !memcmp(stamp, want, stampLen)) {
            /* Stamp matches — but only trust it if all four outputs exist. */
            static const char *rel[4] = {
                "pcmodels-%s/pcmodels.bin", "pcmodels-%s/manifest.csv",
                "pccg-%s/pccg.bin",         "pccg-%s/manifest.csv"
            };
            needRun = 0;
            for (i = 0; i < 4; i++) {
                char sub[256];
                snprintf(sub, sizeof(sub), rel[i], region);
                snprintf(filePath, sizeof(filePath), "%s/%s", dataDir, sub);
                if (!pcvExists(filePath)) { needRun = 1; break; }
            }
        }
        free(stamp);
    }

    if (!needRun)
        return 1;

    pcvLog("pcconvert: preparing game data from your ROM (one-time, a "
           "few seconds) - region %s, sha1 %s", region, sha);

    if (!pcconvertBuild(rom, romLen, &models, &cg)) {
        int n = pcvErrCount();
        pcvLog("pcconvert: %d error(s) converting the ROM:", n);
        for (i = 0; i < n && i < 20; i++)
            pcvLog("pcconvert:   %s", pcvErrAt(i));
        goto out;
    }

    /* ---- write the four outputs (+ their directories) ---- */
    PCV_MKDIR(dataDir);   /* Midas: the data dir may not exist yet (EEXIST is fine) */
    snprintf(dirPath, sizeof(dirPath), "%s/pcmodels-%s", dataDir, region);
    PCV_MKDIR(dirPath);
    snprintf(filePath, sizeof(filePath), "%s/pcmodels.bin", dirPath);
    if (!pcvWriteFile(filePath, models.bin.p, models.bin.len))
        goto out;
    snprintf(filePath, sizeof(filePath), "%s/manifest.csv", dirPath);
    if (!pcvWriteManifest(filePath, &models))
        goto out;

    snprintf(dirPath, sizeof(dirPath), "%s/pccg-%s", dataDir, region);
    PCV_MKDIR(dirPath);
    snprintf(filePath, sizeof(filePath), "%s/pccg.bin", dirPath);
    if (!pcvWriteFile(filePath, cg.bin.p, cg.bin.len))
        goto out;
    snprintf(filePath, sizeof(filePath), "%s/manifest.csv", dirPath);
    if (!pcvWriteManifest(filePath, &cg))
        goto out;

    if (!pcvWriteFile(stampPath, want, strlen(want)))
        goto out;

    pcvLog("pcconvert: wrote %d model + %d stage sidecars (%u + %u bytes)",
           models.n, cg.n, (unsigned)models.bin.len, (unsigned)cg.bin.len);
    rc = 1;

out:
    pcvImageFree(&models);
    pcvImageFree(&cg);
    if (rc != 1)
        pcvLog("pcconvert: sidecar generation FAILED - the ROM may be a bad "
               "dump or a hack");
    return rc;
}

/* --------------------------------------------------------------- CLI hook ---
 * `ge007 --pcconvert <romPath> <outDir>` runs the converter and exits. Used by
 * scripts/verify-sidecars.sh to diff the C output against the Python output
 * without launching a window; exercised code is exactly the boot path's.
 */
/* Round 10 (docs/port-ios/19-round10-xbla-import.md): the XBLA importer's
 * `--xbla-import <src> [<cachedir>]` / GE_XBLA_IMPORT entry shares this
 * pre-boot hook (port/apple/xblacli.c, in the game target). The weak
 * definition keeps the host test, which links this library alone, linking.
 * -1 = not an importer invocation. */
__attribute__((weak)) int xblaCliMain(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return -1;
}

int pcconvertCliMain(int argc, char **argv)
{
    int i;
    {
        const int rc = xblaCliMain(argc, argv);
        if (rc >= 0)
            return rc;
    }
    for (i = 1; i + 2 < argc; i++) {
        if (!strcmp(argv[i], "--pcconvert")) {
            char stamp[1024];
            snprintf(stamp, sizeof(stamp), "%s/pcconvert.stamp", argv[i + 2]);
            /* Always regenerate for the CLI (the stamp is for boot). */
            PCV_MKDIR(argv[i + 2]);
            remove(stamp);
            return pcconvertEnsure(argv[i + 1], argv[i + 2]) == 1 ? 0 : 1;
        }
    }
    return -1;   /* not a converter invocation */
}
