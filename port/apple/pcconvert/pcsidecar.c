/*
 * pcsidecar.c - serve the pcconvert sidecars through Midas's file table
 * (64-bit hosts only; Phase 1 round 7a, docs/port-ios/11-round7a-pcconvert.md).
 *
 * Implements port/include/pcmodels.h and port/include/pccg.h (imported in
 * round 2). Derived from GoldenEye-Decomp-ios engine/port/src/pcmodels.c and
 * engine/port/src/pccg.c at 55a3a9c8 (donor HEAD): same pipeline shape --
 *
 *   1. pcconvertEnsureImage() (first run, or ROM/converter change) writes
 *      <data>/pcmodels-<region>/{pcmodels.bin,manifest.csv} and
 *      <data>/pccg-<region>/{pccg.bin,manifest.csv} from the user's ROM;
 *   2. pcmodelsReserveSize()/pccgReserveSize() find them;
 *   3. pcmodelsLoadSidecars()/pccgLoadSidecars() copy both images into the
 *      guest cart region right after the ROM (so hw_address stays inside the
 *      guest window and CART_PTR/romCopy treat it like ROM bytes);
 *   4. pcmodelsPatchTable()/pccgPatchTable() -- after obInit() -- point every
 *      manifest entry's hw_address at its sidecar and set rom_size, so the
 *      vanilla load path (load_resource -> decompressdata) serves PC bytes.
 *
 * Midas differences: the two donor TUs are merged (they differed only in
 * names); paths come from pcsidecarSetDataDir() instead of the donor's
 * $C/$S/$E fs.h prefixes; logging is portConLogf; the cart extension is
 * committed through portAllocGuestFixed (pcsched.c) at a page-rounded guest
 * address after the ROM instead of being part of one big reservation.
 *
 * ROUND 7a: compiled (object library `pcsidecar`) but NOT linked into the
 * game: port/apple/donor_stubs.c still defines pcmodelsPatchTable and
 * pccgPatchTable as no-ops. Round 7b deletes those two stubs and links this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "guestmem.h"
#include "game/ob.h"
#include "pcmodels.h"
#include "pccg.h"
#include "pcsidecar.h"

extern void portConLogf(const char *fmt, ...);                    /* port_console.c */
extern void *portAllocGuestFixed(u32 guest, u32 size, const char *what); /* pcsched.c */

extern fileentry file_resource_table[];                           /* ob.c */
extern resource_lookup_data_entry resource_lookup_data_array[];   /* ob.c */
extern s32 file_entry_max;

#define PCS_CART_BASE 0x10000000u   /* pcsched.c PORT_GUEST_ROM_BASE */
#define PCS_MAX_FILES 1024          /* pcmodels 512 rows, pccg 52 + 34 */
#define PCS_NAME_LEN  64

struct pcsRow {
    char     name[PCS_NAME_LEN];
    uint32_t offset;
    uint32_t size;
};

struct pcsSet {
    const char   *tag;      /* "pcmodels" / "pccg" (log prefix and dir stem) */
    const char   *bin;      /* "pcmodels.bin" / "pccg.bin" */
    uint32_t      total;
    int           loaded, patched;
    uintptr_t     base;     /* host address of the image inside the guest window */
    struct pcsRow rows[PCS_MAX_FILES];
    int           nrows;
    char          binPath[1024];
    char          manPath[1024];
};

static struct pcsSet s_models = { "pcmodels", "pcmodels.bin" };
static struct pcsSet s_cg     = { "pccg",     "pccg.bin" };
static char s_dataDir[1024] = "Data";

void pcsidecarSetDataDir(const char *dataDir)
{
    snprintf(s_dataDir, sizeof(s_dataDir), "%s", dataDir ? dataDir : "Data");
}

static void pcsLogSink(const char *line)
{
    portConLogf("ge: %s\n", line);
}

int pcsidecarPrepare(const u8 *rom, u32 romSize)
{
    pcconvertSetLog(pcsLogSink);
    return pcconvertEnsureImage(rom, romSize, s_dataDir);
}

static long pcsFileSize(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    n = ftell(f);
    fclose(f);
    return n;
}

static uint32_t pcsReserve(struct pcsSet *s, const uint8_t *romImg)
{
    const char *region = pcconvertRegionForCountry(romImg[0x3E]);
    long n;

    if (region == NULL)
        return 0;
    snprintf(s->binPath, sizeof(s->binPath), "%s/%s-%s/%s", s_dataDir, s->tag, region, s->bin);
    snprintf(s->manPath, sizeof(s->manPath), "%s/%s-%s/manifest.csv", s_dataDir, s->tag, region);
    n = pcsFileSize(s->binPath);
    if (n <= 0) {
        portConLogf("ge: %s: %s not found - 64-bit asset loads will fail (pcconvert did not run?)\n",
                    s->tag, s->binPath);
        return 0;
    }
    if (pcsFileSize(s->manPath) <= 0) {
        portConLogf("ge: %s: %s missing\n", s->tag, s->manPath);
        return 0;
    }
    s->total = (uint32_t)n;
    return s->total;
}

static int pcsLoad(struct pcsSet *s, uint32_t guest)
{
    FILE *f;
    char *buf, *line;
    long msize;
    size_t nread;
    void *dst;

    if (s->total == 0)
        return 0;
    dst = portAllocGuestFixed(guest, s->total, s->tag);
    if (dst == NULL)
        return 0;
    f = fopen(s->binPath, "rb");
    if (f == NULL)
        return 0;
    s->base = (uintptr_t)dst;
    nread = fread(dst, 1, s->total, f);
    fclose(f);
    if (nread != s->total) {
        portConLogf("ge: %s: short read of %s\n", s->tag, s->binPath);
        return 0;
    }

    msize = pcsFileSize(s->manPath);
    f = fopen(s->manPath, "rb");
    if (f == NULL || msize <= 0) {
        if (f != NULL)
            fclose(f);
        portConLogf("ge: %s: cannot read %s\n", s->tag, s->manPath);
        return 0;
    }
    buf = (char *)malloc((size_t)msize + 1);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    nread = fread(buf, 1, (size_t)msize, f);
    fclose(f);
    buf[nread] = 0;

    /* name,offset,size lines ("\r\n", Python csv style). No strtok/strchr:
     * the game's include/string.h stub (C TUs) declares neither usefully. */
    s->nrows = 0;
    for (line = buf; *line; ) {
        char *end = line, nm[PCS_NAME_LEN];
        unsigned off, sz;
        struct pcsRow *r;
        while (*end && *end != '\r' && *end != '\n')
            end++;
        if (*end)
            *end++ = 0;
        while (*end == '\r' || *end == '\n')
            end++;
        if (sscanf(line, "%63[^,],%u,%u", nm, &off, &sz) == 3) {
            if (s->nrows >= PCS_MAX_FILES) {
                portConLogf("ge: %s: manifest exceeds %d rows\n", s->tag, PCS_MAX_FILES);
                break;
            }
            if ((uint64_t)off + sz > s->total) {
                portConLogf("ge: %s: manifest row %s runs past the image\n", s->tag, nm);
            } else {
                r = &s->rows[s->nrows++];
                snprintf(r->name, sizeof(r->name), "%s", nm);
                r->offset = off;
                r->size = sz;
            }
        }
        line = end;
    }
    free(buf);
    if (s->nrows == 0) {
        portConLogf("ge: %s: no rows parsed from %s\n", s->tag, s->manPath);
        return 0;
    }
    s->loaded = 1;
    portConLogf("ge: %s: %d sidecars (%u bytes) at guest %08X\n", s->tag, s->nrows, s->total,
                GUEST_ADDR(dst));
    return 1;
}

static void pcsPatch(struct pcsSet *s)
{
    int i, t, hit = 0;

    if (s->patched || !s->loaded)
        return;
    s->patched = 1;
    for (i = 0; i < s->nrows; i++) {
        const struct pcsRow *r = &s->rows[i];
        for (t = 1; t < file_entry_max; t++) {
            if (file_resource_table[t].filename != NULL &&
                strcmp(file_resource_table[t].filename, r->name) == 0) {
                file_resource_table[t].hw_address = (u8 *)(s->base + r->offset);
                resource_lookup_data_array[t].rom_size = r->size;
                hit++;
                break;
            }
        }
    }
    portConLogf("ge: %s: table patched (%d/%d manifest rows)%s\n", s->tag, hit, s->nrows,
                hit == s->nrows ? "" : " - TABLE MISMATCH");
}

/* Guest address of the cart extension: the first 1 MB boundary after the
 * ROM, then pccg right after pcmodels (16 KB pages, guestmem.h). */
static uint32_t s_romSize;

static uint32_t pcsExtBase(void)
{
    return PCS_CART_BASE + ((s_romSize + 0xFFFFFu) & ~0xFFFFFu);
}

/* ---- port/include/pcmodels.h ----------------------------------------- */
uint32_t pcmodelsReserveSize(const uint8_t *romImg) { return pcsReserve(&s_models, romImg); }

int pcmodelsLoadSidecars(uintptr_t cartBase, uint32_t romSize)
{
    (void)cartBase; /* Midas: the ROM sits at guest PCS_CART_BASE (pcsched.c) */
    s_romSize = romSize;
    return pcsLoad(&s_models, pcsExtBase());
}

void pcmodelsPatchTable(void) { pcsPatch(&s_models); }
uint32_t pcmodelsTotalSize(void) { return s_models.loaded ? s_models.total : 0; }
uintptr_t pcmodelsSidecarBase(void) { return s_models.loaded ? s_models.base : 0; }

/* ---- port/include/pccg.h --------------------------------------------- */
uint32_t pccgReserveSize(const uint8_t *romImg) { return pcsReserve(&s_cg, romImg); }

int pccgLoadSidecars(uintptr_t base)
{
    (void)base; /* placed after pcmodels at a page boundary */
    return pcsLoad(&s_cg, pcsExtBase() + (uint32_t)GUEST_PAGE_ROUND((size_t)pcmodelsTotalSize()));
}

void pccgPatchTable(void) { pcsPatch(&s_cg); }
uint32_t pccgTotalSize(void) { return s_cg.loaded ? s_cg.total : 0; }
uintptr_t pccgSidecarBase(void) { return s_cg.loaded ? s_cg.base : 0; }

/* romdata.c's cart-range check (romCopy/osPiStartDma) must accept the
 * extension: true when [p, p+len) lies inside one loaded image. */
int pcsidecarContains(const void *p, uint32_t len)
{
    uintptr_t a = (uintptr_t)p;
    const struct pcsSet *sets[2] = { &s_models, &s_cg };
    int i;
    for (i = 0; i < 2; i++) {
        const struct pcsSet *s = sets[i];
        if (s->loaded && a >= s->base && a + len <= s->base + s->total)
            return 1;
    }
    return 0;
}
