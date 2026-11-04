/**
 * romdata.c - the runtime ROM loader.
 *
 * The exe does not ship Rare's data: each user supplies their own cart image
 * and the game data is read from it at runtime, one asset class at a time
 * (docs/port/rom-asset-loading.md is the overview, docs/port/romdata.md the
 * implementation notes and gotchas). Modelled on the Perfect Dark port's
 * romdata.c, and sits underneath ob.c rather than beside it.
 *
 * This file finds, validates and normalises the ROM, inflates its data segment
 * with the game's own 1172 inflater, parses the 727-entry file directory
 * (file_resource_table), and checks the ROM-segment table generated from
 * scripts/filelist.u.csv. Modded-ROM support lives on the mod-loader branch,
 * not here. GE_ROMDATA=0 skips the ROM entirely and uses the compiled-in
 * assets.
 *
 * ROM resolution order (first that loads wins, always logged):
 *   1. argv[1] not starting with '-'   (drag-and-drop onto ge.exe)
 *   2. --rom-file <path> / --rom-file=<path>
 *   3. GE_ROM=<path>                    (env var, e.g. the launcher's env box)
 *   4. midas.ini [data] rom=<path>         (relative = next to the exe)
 *   5. <exe dir>/Data/*                 (alphabetical, first valid file)
 *   6. <exe dir>/baserom.u.z64, <exe dir>/../baserom.u.z64  (dev tree)
 *
 * Formats: .z64 (80 37 12 40), .v64 byte-swapped (37 80 40 12) and .n64
 * word-swapped (40 12 37 80), detected from the first four bytes (never the
 * extension) and normalised to z64 order in memory.
 *
 * Stock cart only: the size must be at least a stock cart's, the header must
 * read GOLDENEYE / NGE (a renamed header only warns when the file was named
 * explicitly), the file table must parse, and the whole-image CRC32 must match
 * the retail (U) cart (B6330846). Modded ROMs, other regions and bad dumps are
 * rejected with a message saying so; the CIC-6102 CRC is only reported.
 *
 * WHERE THE BUFFER LIVES IS LOAD-BEARING: fast3d's seg_addr() treats any
 * word with top byte 1..15 that fails PORT_IS_HOST_ADDR() as a segment
 * reference, so a ROM mapped below 0x10000000 would break display lists that
 * point into it. portAllocHighDedicated (pcsched.c) maps it high.
 *
 * Knobs: [data] rom= (midas.ini), GE_ROM, GE_ROMDATA=0, GE_ROMDBG=1 (per-file and
 * per-segment lines; scripts/port/romtable_lint.py diffs them against
 * scripts/filelist.u.csv).
 */
#ifndef _WIN32
/* 32-bit ELF (linux/deck) on a filesystem with 64-bit inodes (WSL drvfs,
 * btrfs, NTFS mounts): plain stat() fails with EOVERFLOW and Data looked
 * missing AND empty in the first Linux smoke. Must precede every include. */
#define _FILE_OFFSET_BITS 64
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#endif
#include <ultra64.h>
#include "decompress.h" /* decompressdata: the game's own 1172 inflater */
#include "ob.h"         /* fileentry / resource_lookup_data_entry: the compiled file table */
#include "image.h"      /* g_Textures / PORT_TEXTURES_MAX: the texture directory */
#include "assets/obseg/file_resource_id_enums.h" /* OBJ_INDEX_MAX */
#include "romdata.h"
#ifdef PORT
/* round 55: upstream 1.0.2b's romdataTrim calls strspn, which the decomp's
 * include/string.h (first on the include path, shadowing libc's) does not
 * declare; clang makes the implicit declaration a hard error. */
extern size_t strspn(const char *, const char *);
#endif
#include "../generated/romseg_list.h"

/* port_console.c: same output as the fprintf(stderr, ...) this replaced,
 * plus a silent copy in the tilde console ring (spec section 7). */
extern void portConLogf(const char *fmt, ...);

/* include/PR/os.h #undef's errno (an OSMesg field carries that name);
 * this is libc's own expansion, brought back under a port name */
#ifdef _WIN32
#define port_errno (*_errno())
#elif defined(__APPLE__)
#define port_errno (*__error()) /* Darwin's errno expansion */
#else
#define port_errno (*__errno_location())
#endif

extern const char *portRootPath(const char *name); /* main.c: <exe dir>/name */
extern FILE *portRootFopen(const char *name, const char *mode);
extern void *portAllocHighDedicated(u32 size);      /* pcsched.c */
#if defined(PLATFORM_64BIT)
extern void *portAllocRomImage(u32 size);           /* pcsched.c: guest 0x10000000 */
extern void *portAllocHigh(u32 size);               /* pcsched.c: guest 0x30000000 bump arena */
#include "guestmem.h"
#include "pcsidecar.h"  /* round 7b: pcconvert sidecars (port/apple/pcconvert) */
#include "pcmodels.h"
#include "pccg.h"

/*
 * Round 6 (R15, docs/port-ios/09-round6-shim.md): a text bank slot is a u32
 * GUEST address that langGet() rebases with GUEST_PTR. A compiled string
 * literal lives in the exe (above 4 GiB on a 64-bit host), so storing one in
 * a slot truncated it unrecoverably. portGuestString() returns a copy of the
 * string inside the guest window (high arena), interned by the literal's
 * address so repeated calls share one copy; its (u32) is then lossless. A
 * pointer that is already in the window is returned as is. Used here for
 * sTextPatches and offered to the game for langGet()'s own error literals
 * (ripple, language.c is outside this round's files).
 */
#define PORT_GUEST_STRING_MAX 64
const char *portGuestString(const char *s)
{
    static struct {
        const char *key;
        char *copy;
    } sInterned[PORT_GUEST_STRING_MAX];
    static int sInternedCount;
    size_t len;
    char *p;
    int i;

    if (s == NULL || GUEST_IN_WINDOW(s)) {
        return s;
    }
    for (i = 0; i < sInternedCount; i++) {
        if (sInterned[i].key == s) {
            return sInterned[i].copy;
        }
    }
    if (sInternedCount >= PORT_GUEST_STRING_MAX) {
        portConLogf("ge: romdata: portGuestString table full - \"%.24s\" stays outside the guest window\n", s);
        return s;
    }
    len = strlen(s) + 1;
    p = (char *) portAllocHigh((u32) len);
    if (p == NULL) {
        return s;
    }
    memcpy(p, s, len);
    sInterned[sInternedCount].key = s;
    sInterned[sInternedCount].copy = p;
    sInternedCount++;
    return p;
}
#endif
extern void portRomdataSegsApply(void);             /* segments.c: point segment consumers at
                                                     * the ROM */

/* ---- the US cart, first-hand (docs/port/rom-asset-loading.md section 1) */
#define ROMDATA_ROM_SIZE        0xC00000u   /* 12,582,912: a stock cart */
#define ROMDATA_ROM_MAX         (64u << 20) /* sanity cap for a "ROM" */
#define ROMDATA_CDATA_ROMOFS    0x21990u    /* _cdataSegmentRomStart, 1172 blob */
#define ROMDATA_CDATA_SIZE      0x3C550u    /* inflates to 247,120 bytes */
#define ROMDATA_CDATA_ALLOC     (1u << 20)  /* the inflater has no output bound */
#define ROMDATA_CDATA_VADDR     0x80020D90u /* where the N64 loads it */
#define ROMDATA_FILETABLE_OFS   0x252C4u    /* file_resource_table[] in cdata (RAM 0x80046054) */
#define ROMDATA_FILEENTRY_SIZE  12          /* {s32 index; char *filename; u8 *hw_address;} BE */
#define ROMDATA_MAX_FILES       2048        /* 727 + headroom for a larger directory */
#define ROMDATA_STOCK_CRC1      0xDCBC50D1u /* GoldenEye 007 (U) header CRCs */
#define ROMDATA_STOCK_CRC2      0x09FD1AA3u
#define ROMDATA_STOCK_CRC32     0xB6330846u /* zlib crc32 of the whole stock (U) image */

#ifdef _WIN32
#define PATHSEP "\\"
#else
#define PATHSEP "/"
#endif
#define ROMDATA_DATA_DIR "Data"

static u8 *sRom;            /* normalised z64 bytes, high address */
static u32 sRomSize;
static char sRomPath[1200];
static enum romsource sRomSource;
static u8 *sCdata;          /* the inflated data segment: it IS the directory */
static u32 sCdataSize;
static struct romfile sFiles[ROMDATA_MAX_FILES];
static s32 sFileCount;
static s32 sLoaded;
static int sDebug;          /* GE_ROMDBG */

/* midas.ini [data] */
static char sIniRom[1024];
static int sArgLangJp;      /* --lang jp (romdataLanguageJp) */

/* the non-file-table ROM regions, from scripts/filelist.u.csv */
struct romseg {
    const char *name;
    u32 romofs;
    u32 size;
    const char *source;
};
#define ROMSEG(n, ofs, sz, src) { #n, ofs, sz, src },
static const struct romseg sSegs[] = { ROMSEG_LIST() };
#undef ROMSEG

static u32 be32(const u8 *p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | (u32) p[3];
}

static const char *romdataSourceName(enum romsource s)
{
    switch (s) {
    case ROMSRC_ARGV:    return "argv[1] (drag-and-drop)";
    case ROMSRC_ARGFLAG: return "--rom-file";
    case ROMSRC_ENV:     return "GE_ROM";
    case ROMSRC_INI:     return "midas.ini [data] rom=";
    case ROMSRC_DATADIR: return "./" ROMDATA_DATA_DIR " scan";
    case ROMSRC_LEGACY:  return "legacy repo baserom (dev)";
#ifdef PORT
    case ROMSRC_HOST:    return "romdataSetRomPath (shell)";
#endif
    default:             return "none";
    }
}

/* ==== paths ============================================================ */

static int romdataPathIsAbsolute(const char *p)
{
    if (p[0] == '/' || p[0] == '\\') {
        return 1;
    }
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        return 1;
    }
    return 0;
}

/* <exe dir>/rel unless rel is already absolute */
static void romdataResolve(char *out, size_t n, const char *rel)
{
    if (romdataPathIsAbsolute(rel)) {
        snprintf(out, n, "%s", rel);
    } else {
        snprintf(out, n, "%s", portRootPath(rel));
    }
}

/* a stat failure that is not "no such file" is named (rate-limited): the
 * EOVERFLOW above was invisible as "Data is empty" */
static int romdataStat(const char *path, struct stat *st)
{
    if (stat(path, st) == 0) {
        return 1;
    }
    if (port_errno != ENOENT) {
        static int budget = 4;
        if (budget > 0) {
            budget--;
            portConLogf("ge: romdata: stat(%s): %s\n", path, strerror(port_errno));
        }
    }
    return 0;
}

static int romdataIsDir(const char *path)
{
    struct stat st;
    return romdataStat(path, &st) && S_ISDIR(st.st_mode);
}

static int romdataIsFile(const char *path)
{
    struct stat st;
    return romdataStat(path, &st) && S_ISREG(st.st_mode);
}

/* create <exe dir>/name when missing (for Data, a fresh install then shows
 * the user where the ROM goes) */
static void romdataEnsureDir(const char *name)
{
    const char *path = portRootPath(name);
    int rc;

    if (romdataIsDir(path)) {
        return;
    }
#ifdef _WIN32
    rc = _mkdir(path);
#else
    rc = mkdir(path, 0755);
#endif
    if (rc == 0) {
        portConLogf("ge: romdata: created %s\n", path);
    } else {
        portConLogf("ge: romdata: could not create %s (read-only install dir?)\n", path);
    }
}

static int romdataCmpStr(const void *a, const void *b)
{
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* sorted names of a directory's entries (regular files when wantdirs=0,
 * directories otherwise); returns the count, names in a caller buffer */
#define ROMDATA_LIST_MAX 256
static int romdataListDir(const char *dirpath, int wantdirs, char names[][256], int max)
{
    DIR *d = opendir(dirpath);
    struct dirent *de;
    const char *ptrs[ROMDATA_LIST_MAX];
    char tmp[ROMDATA_LIST_MAX][256];
    int n = 0;
    int i;

    if (d == NULL) {
        return 0;
    }
    if (max > ROMDATA_LIST_MAX) {
        max = ROMDATA_LIST_MAX;
    }
    /* Read the whole directory and keep the first `max` names in alphabetical
     * order, not readdir order (which is hash order on some filesystems).
     * Otherwise the ROM could be left out of a crowded Data folder. */
    while ((de = readdir(d)) != NULL) {
        char full[1500];
        int slot = n;
        if (de->d_name[0] == '.') {
            continue; /* ., .., dotfiles */
        }
        if (n >= max) {
            int j;
            if (max <= 0) {
                break;
            }
            slot = 0;
            for (j = 1; j < n; j++) {
                if (strcmp(tmp[j], tmp[slot]) > 0) {
                    slot = j;
                }
            }
            if (strcmp(de->d_name, tmp[slot]) >= 0) {
                continue;
            }
        }
        snprintf(full, sizeof(full), "%s" PATHSEP "%s", dirpath, de->d_name);
        if (wantdirs ? !romdataIsDir(full) : !romdataIsFile(full)) {
            continue;
        }
        snprintf(tmp[slot], sizeof(tmp[slot]), "%s", de->d_name);
        ptrs[slot] = tmp[slot];
        if (slot == n) {
            n++;
        }
    }
    closedir(d);
    /* readdir order is filesystem-dependent; alphabetical is what the
     * doc promises */
    qsort(ptrs, (size_t) n, sizeof(ptrs[0]), romdataCmpStr);
    for (i = 0; i < n; i++) {
        snprintf(names[i], 256, "%s", ptrs[i]);
    }
    return n;
}

/* ==== midas.ini [data] ==================================================== */

static char *romdataTrim(char *s)
{
    size_t len;

    s += strspn(s, " \t");
    len = strlen(s);
    while (len > 0 && strchr(" \t\r\n", s[len - 1]) != NULL) {
        len--;
    }
    s[len] = '\0';
    return s;
}

/* video.c's reader is section-blind and video.c is not in headless
 * builds, so [data] has its own (section-aware) reader here */
static void romdataReadIni(void)
{
    FILE *f = portRootFopen("midas.ini", "r");
    char line[1200];
    int indata = 0;

    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = romdataTrim(line);
        char *eq;
        if (s[0] == '\0' || s[0] == ';' || s[0] == '#') {
            continue;
        }
        if (s[0] == '[') {
            indata = (strncmp(s, "[data]", 6) == 0);
            continue;
        }
        if (!indata) {
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        s = romdataTrim(s);
        if (strcmp(s, "rom") == 0) {
            snprintf(sIniRom, sizeof(sIniRom), "%s", romdataTrim(eq + 1));
        }
    }
    fclose(f);
}

s32 romdataIsIniKey(const char *key)
{
    return strcmp(key, "rom") == 0;
}

/* Which of the ROM's two text bank sets (L*E or L*J) the game loads: the
 * command line's --lang jp (romdataInit), or GE_LANG=en|jp, which wins. Not a
 * midas.ini key yet (1.1 adds a menu row; docs/port/japanese-language.md).
 * boss.c reads it once, before langInit, into j_text_trigger. */
s32 romdataLanguageJp(void)
{
    const char *e = getenv("GE_LANG");

    if (e != NULL && e[0] != '\0') {
        return e[0] == 'j' || e[0] == 'J';
    }
    return sArgLangJp;
}

/* ==== byte order ======================================================= */

enum romorder { ORDER_UNKNOWN = 0, ORDER_Z64, ORDER_V64, ORDER_N64 };

static enum romorder romdataDetectOrder(const u8 *p)
{
    if (p[0] == 0x80 && p[1] == 0x37 && p[2] == 0x12 && p[3] == 0x40) return ORDER_Z64;
    if (p[0] == 0x37 && p[1] == 0x80 && p[2] == 0x40 && p[3] == 0x12) return ORDER_V64;
    if (p[0] == 0x40 && p[1] == 0x12 && p[2] == 0x37 && p[3] == 0x80) return ORDER_N64;
    return ORDER_UNKNOWN;
}

static void romdataNormalise(u8 *rom, u32 size, enum romorder order)
{
    u32 i;
    if (order == ORDER_V64) {
        for (i = 0; i + 1 < size; i += 2) {
            u8 t = rom[i];
            rom[i] = rom[i + 1];
            rom[i + 1] = t;
        }
    } else if (order == ORDER_N64) {
        for (i = 0; i + 3 < size; i += 4) {
            u8 t0 = rom[i], t1 = rom[i + 1];
            rom[i] = rom[i + 3];
            rom[i + 1] = rom[i + 2];
            rom[i + 2] = t1;
            rom[i + 3] = t0;
        }
    }
}

/* ==== CIC-6102 CRC (informational) ===================================== */

#define ROL(i, b) ((b) ? (((i) << (b)) | ((i) >> (32 - (b)))) : (i))

static void romdataCrc6102(const u8 *rom, u32 *crc1, u32 *crc2)
{
    const u32 seed = 0xF8CA4DDCu;
    u32 t1 = seed, t2 = seed, t3 = seed, t4 = seed, t5 = seed, t6 = seed;
    u32 i;

    for (i = 0x1000; i < 0x1000 + 0x100000; i += 4) {
        u32 d = be32(rom + i);
        u32 r;
        if ((t6 + d) < t6) {
            t4++;
        }
        t6 += d;
        t3 ^= d;
        r = ROL(d, (d & 0x1F));
        t5 += r;
        if (t2 > d) {
            t2 ^= r;
        } else {
            t2 ^= t6 ^ d;
        }
        t1 += t5 ^ d;
    }
    *crc1 = t6 ^ t4 ^ t3;
    *crc2 = t5 ^ t2 ^ t1;
}

/* zlib-compatible CRC-32 of the whole image: the "is this the stock
 * cart" answer (the CIC CRC above stops 1 MB in - a setup/model mod
 * leaves it intact). ~12 M table steps; a blink even in a Debug build. */
static u32 romdataCrc32(const u8 *p, u32 n)
{
    static u32 table[256];
    u32 c;
    u32 i;

    if (table[1] == 0) {
        for (i = 0; i < 256; i++) {
            u32 k;
            c = i;
            for (k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    c = 0xFFFFFFFFu;
    for (i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ==== the directory ==================================================== */

/* inflate cdata and walk file_resource_table into sFiles. 0 = not a
 * usable GoldenEye image (reason logged). */
static int romdataParseTables(const u8 *rom, u32 romsize)
{
    const u8 *src = rom + ROMDATA_CDATA_ROMOFS;
    u32 n;
    s32 i;
    s32 ncomp = 0;
    s32 nalias = 0;

    if (ROMDATA_CDATA_ROMOFS + 2 > romsize || src[0] != 0x11 || src[1] != 0x72) {
        portConLogf("ge: romdata: no 1172 stream at 0x%X (found %02X %02X) - not a GoldenEye (U) image, or the code segment was moved\n",
                ROMDATA_CDATA_ROMOFS, src[0], src[1]);
        return 0;
    }
    sCdata = (u8 *) malloc(ROMDATA_CDATA_ALLOC);
    if (sCdata == NULL) {
        portConLogf("ge: romdata: out of memory for cdata\n");
        return 0;
    }
    /* decompressdata's PC arm ignores the huft arg (it owns a static
     * workspace); src is read-only here - the inflater never writes src */
    n = decompressdata((u8 *) src, sCdata, NULL);
    sCdataSize = n;
    if (n != ROMDATA_CDATA_SIZE) {
        portConLogf("ge: romdata: cdata inflated to %u bytes, stock is %u - data segment layout differs (JP/EU cart, or a code mod)\n",
                n, ROMDATA_CDATA_SIZE);
    }
    if (n < ROMDATA_FILETABLE_OFS + 3 * ROMDATA_FILEENTRY_SIZE) {
        portConLogf("ge: romdata: cdata too small to hold the file table\n");
        free(sCdata);
        sCdata = NULL;
        return 0;
    }

    /* walk until the self-index stops counting (the table is followed
     * by zeros: entry "727" reads index 0 and stops the walk) */
    for (i = 0; i < ROMDATA_MAX_FILES; i++) {
        const u8 *e = sCdata + ROMDATA_FILETABLE_OFS + (u32) i * ROMDATA_FILEENTRY_SIZE;
        u32 idx, nameptr, hw;
        if (ROMDATA_FILETABLE_OFS + (u32) (i + 1) * ROMDATA_FILEENTRY_SIZE > n) {
            break;
        }
        idx = be32(e);
        nameptr = be32(e + 4);
        hw = be32(e + 8);
        if (idx != (u32) i) {
            break;
        }
        if (nameptr < ROMDATA_CDATA_VADDR || nameptr >= ROMDATA_CDATA_VADDR + n) {
            if (i == 0) {
                sFiles[0].name = "";
            } else {
                break;
            }
        } else {
            /* the padded pool the table points at (there is a SECOND
             * copy of the strings in cdata - resolving against the wrong
             * one yields truncated names; this one is the pointer's) */
            const char *name = (const char *) sCdata + (nameptr - ROMDATA_CDATA_VADDR);
            if (memchr(name, '\0', n - (nameptr - ROMDATA_CDATA_VADDR)) == NULL) {
                break; /* unterminated: not a string pool */
            }
            sFiles[i].name = name;
        }
        sFiles[i].romofs = hw;
        sFiles[i].size = 0;
        sFiles[i].compressed = 0;
    }
    sFileCount = i;
    if (sFileCount < 3) {
        portConLogf("ge: romdata: file table at cdata+0x%X did not parse (%d entries)\n", ROMDATA_FILETABLE_OFS, sFileCount);
        free(sCdata);
        sCdata = NULL;
        sFileCount = 0;
        return 0;
    }

    /* size = next - this (ob.c obInit), so the table must be
     * address-sorted and inside the image */
    for (i = 1; i < sFileCount; i++) {
        if (sFiles[i].romofs > romsize) {
            portConLogf("ge: romdata: file %d \"%s\" at 0x%X is beyond the %u-byte image\n",
                    i, sFiles[i].name, sFiles[i].romofs, romsize);
            goto bad;
        }
        if (i + 1 < sFileCount) {
            if (sFiles[i + 1].romofs < sFiles[i].romofs) {
                portConLogf("ge: romdata: file table not address-sorted at %d (\"%s\" 0x%X > \"%s\" 0x%X)\n",
                        i, sFiles[i].name, sFiles[i].romofs, sFiles[i + 1].name, sFiles[i + 1].romofs);
                goto bad;
            }
            sFiles[i].size = sFiles[i + 1].romofs - sFiles[i].romofs;
        }
        if (sFiles[i].size == 0) {
            if (i + 1 < sFileCount) {
                nalias++; /* the last entry (ob/ob_end.seg) only bounds the one before */
            }
        } else if (sFiles[i].romofs + 2 <= romsize && rom[sFiles[i].romofs] == 0x11 && rom[sFiles[i].romofs + 1] == 0x72) {
            sFiles[i].compressed = 1;
            ncomp++;
        }
    }
    if (strcmp(sFiles[sFileCount - 1].name, "ob/ob_end.seg") != 0) {
        portConLogf("ge: romdata: last file is \"%s\", expected ob/ob_end.seg (the size bound) - a mod that appended files?\n",
                sFiles[sFileCount - 1].name);
    }
    portConLogf("ge: romdata: cdata inflated %u bytes; file table %d entries (%d compressed, %d zero-length aliases), obseg 0x%X..0x%X\n",
            n, sFileCount, ncomp, nalias, sFiles[1].romofs, sFiles[sFileCount - 1].romofs);
    if (sDebug) {
        for (i = 0; i < sFileCount; i++) {
            portConLogf("ge: romfile %4d %-28s 0x%06X %7u %s\n", i, sFiles[i].name, sFiles[i].romofs, sFiles[i].size,
                    sFiles[i].compressed ? "1172" : (sFiles[i].size == 0 ? "alias" : "raw"));
        }
    }
    return 1;

bad:
    free(sCdata);
    sCdata = NULL;
    sFileCount = 0;
    return 0;
}

/* every ROMSEG must lie inside the image; a mod that shrank the cart
 * below a region would otherwise read past the buffer later */
static int romdataCheckSegs(u32 romsize)
{
    int bad = 0;
    size_t i;
    for (i = 0; i < sizeof(sSegs) / sizeof(sSegs[0]); i++) {
        int ok = sSegs[i].romofs + sSegs[i].size <= romsize;
        if (!ok) {
            bad++;
        }
        if (sDebug || !ok) {
            portConLogf("ge: romseg %-20s 0x%06X 0x%06X %s\n", sSegs[i].name, sSegs[i].romofs, sSegs[i].size,
                    ok ? "ok" : "OUT OF RANGE");
        }
    }
    portConLogf("ge: romdata: %u ROM segments (romseg_list.h)%s\n", (unsigned) i, bad ? " - SOME OUT OF RANGE" : " in range");
    return bad == 0;
}

/* ==== loading one candidate ============================================ */

/* strict: a ./Data scan candidate must pass the header check; an
 * explicitly named ROM only warns (a mod may have renamed the cart). */
static int romdataTryLoad(const char *path, enum romsource source, int strict)
{
    FILE *f;
    long len;
    u8 *buf;
    u8 *high;
    enum romorder order;
    int titleok, idok;
    u32 crc1, crc2, hcrc1, hcrc2, crc32;
    char title[21];
    int tl;

    f = fopen(path, "rb");
    if (f == NULL) {
        portConLogf("ge: romdata: %s: cannot open (%s)\n", path, romdataSourceName(source));
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < (long) ROMDATA_ROM_SIZE) {
        portConLogf("ge: romdata: %s: %ld bytes, a GoldenEye cart is %u - not a ROM (%s)\n", path, len, ROMDATA_ROM_SIZE,
                romdataSourceName(source));
        fclose(f);
        return 0;
    }
    if (len > (long) ROMDATA_ROM_MAX) {
        portConLogf("ge: romdata: %s: %ld bytes is too big for a cart image (%s)\n", path, len, romdataSourceName(source));
        fclose(f);
        return 0;
    }
    buf = (u8 *) malloc((size_t) len);
    if (buf == NULL || fread(buf, 1, (size_t) len, f) != (size_t) len) {
        portConLogf("ge: romdata: %s: read failed\n", path);
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);

    order = romdataDetectOrder(buf);
    if (order == ORDER_UNKNOWN) {
        portConLogf("ge: romdata: %s: first bytes %02X %02X %02X %02X are not an N64 image (.z64 80 37 12 40 / .v64 37 80 40 12 / .n64 40 12 37 80)\n",
                path, buf[0], buf[1], buf[2], buf[3]);
        free(buf);
        return 0;
    }
    romdataNormalise(buf, (u32) len, order);

    memcpy(title, buf + 0x20, 20);
    title[20] = '\0';
    for (tl = 20; tl > 0 && title[tl - 1] == ' '; tl--) {
        title[tl - 1] = '\0'; /* the header pads with spaces */
    }
    titleok = memcmp(buf + 0x20, "GOLDENEYE", 9) == 0;
    idok = memcmp(buf + 0x3B, "NGE", 3) == 0;
    if (!titleok && !idok) {
        portConLogf("ge: romdata: %s: header title \"%s\" id %.4s is not GOLDENEYE/NGEE%s\n", path, title, (const char *) buf + 0x3B,
                strict ? " - skipped" : " - named explicitly, trying it anyway");
        if (strict) {
            free(buf);
            return 0;
        }
    }
    if (!romdataParseTables(buf, (u32) len)) {
        free(buf);
        return 0;
    }

    /* Stock cart only: reject any image whose whole-image CRC32 isn't the
     * retail (U) cart's, before it takes the high mapping, since this build
     * lacks the handling mods need and would fail later in confusing ways. An
     * explicitly named file is rejected too; only the header check above is
     * lenient. */
    crc32 = romdataCrc32(buf, (u32) len);
    if (crc32 != ROMDATA_STOCK_CRC32) {
        portConLogf("ge: romdata: %s: image crc32 %08X is not the stock GoldenEye 007 (U) cart (%08X) - a modded ROM, another region, or a bad dump; this build loads the stock cart only (modded ROMs need the mod-loader build) - %s\n",
                path, crc32, ROMDATA_STOCK_CRC32, strict ? "skipped" : "NOT loaded");
        free(buf);
        return 0;
    }

    /* it is the stock GoldenEye image: move it high (see the file comment) */
#if defined(PLATFORM_64BIT)
    /* 64-bit hosts: guest 0x10000000, the N64 cart base (pcsched.c) */
    high = (u8 *) portAllocRomImage((u32) len);
#else
    high = (u8 *) portAllocHighDedicated((u32) len);
#endif
    if (high == NULL) {
        portConLogf("ge: romdata: no high address for the %ld-byte ROM - keeping the heap copy (segment refs into it would misresolve in fast3d)\n", len);
        high = buf;
    } else {
        memcpy(high, buf, (size_t) len);
        free(buf);
    }
    sRom = high;
    sRomSize = (u32) len;
    sRomSource = source;
    snprintf(sRomPath, sizeof(sRomPath), "%s", path);

    /* always-on: which candidate won and what it was */
    portConLogf("ge: romdata: ROM = %s (%s) %u bytes, %s\n", path, romdataSourceName(source), sRomSize,
            order == ORDER_Z64 ? "z64 order" : order == ORDER_V64 ? ".v64 byte-swapped, normalised" : ".n64 word-swapped, normalised");
    hcrc1 = be32(sRom + 0x10);
    hcrc2 = be32(sRom + 0x14);
    romdataCrc6102(sRom, &crc1, &crc2);
    portConLogf("ge: romdata: header \"%s\" %.4s cic-crc %08X/%08X (computed %08X/%08X, %s), image crc32 %08X = stock GoldenEye 007 (U)\n",
            title, (const char *) sRom + 0x3B, hcrc1, hcrc2, crc1, crc2,
            (crc1 == hcrc1 && crc2 == hcrc2) ? "consistent" : "STALE - emulators may refuse it; the port does not care",
            crc32);
    return 1;
}

/* ==== file-table classes (text banks) =================================== */

extern fileentry file_resource_table[];                       /* ob.c (compiled, name + linked data) */
extern resource_lookup_data_entry resource_lookup_data_array[]; /* ob.c (rom_size etc.) */
extern s32 file_entry_max;

enum romfileclass { CLASS_NONE = 0, CLASS_TEXT, CLASS_BRIEF, CLASS_SETUP, CLASS_STAN, CLASS_MODEL, CLASS_BG };
static const char *const sClassName[] = { "none", "text", "brief", "setup", "stan", "model", "bg" };

#if !defined(PLATFORM_64BIT)
extern s32 romfileSetupConvert(u8 *data, u32 size, const char *name, const u8 *compiled); /* romfile_setup.c */
extern s32 romfileStanConvert(u8 *data, u32 size, const char *name, const u8 *compiled);  /* romfile_stan.c */
#endif

/* GE_ROMTEXT / GE_ROMBRIEF / GE_ROMSETUP / GE_ROMSTAN = 0: that class stays
 * compiled-in with the ROM loaded - the per-class A/B knobs */
static int romdataClassEnabled(enum romfileclass cls)
{
    static int cache[7] = { -1, -1, -1, -1, -1, -1, -1 };
    static const char *const env[7] = { NULL, "GE_ROMTEXT", "GE_ROMBRIEF", "GE_ROMSETUP", "GE_ROMSTAN", "GE_ROMMODEL", "GE_ROMBG" };
    if (cache[cls] < 0) {
        const char *e = env[cls] ? getenv(env[cls]) : NULL;
        cache[cls] = !(e != NULL && e[0] == '0');
        if (!cache[cls]) {
            portConLogf("ge: romdata: %s=0: %s files stay compiled-in (A/B)\n", env[cls], sClassName[cls]);
        }
    }
    return cache[cls];
}

static u8 sServed[OBJ_INDEX_MAX];          /* 1 = this compiled index now points at the ROM */
static u8 *sCompiledData[OBJ_INDEX_MAX];   /* the linked data it replaced (probe), or NULL */

/* the class of a file from its ROM name (the prefix grammar in
 * docs/port/rom-asset-loading.md section 1): only the classes a phase has
 * a preprocess pass for are served */
static enum romfileclass romdataClassOf(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return CLASS_NONE;
    }
    if (name[0] == 'L' && strchr(name, '/') == NULL) {
        return CLASS_TEXT; /* L<lvl>E, L<lvl>J */
    }
    if (strncmp(name, "Ubrief", 6) == 0) {
        return CLASS_BRIEF; /* Ubrief<lvl>Z */
    }
    if (strncmp(name, "Usetup", 6) == 0 || strncmp(name, "Ump_setup", 9) == 0) {
        return CLASS_SETUP; /* Usetup<lvl>Z, Ump_setup<lvl>Z */
    }
    if (strncmp(name, "Tbg_", 4) == 0) {
        return CLASS_STAN; /* Tbg_<lvl>_all_p_stanZ */
    }
    if (strncmp(name, "bg/", 3) == 0) {
        return CLASS_BG; /* bg/bg_<lvl>_all_p.seg - converted in load_bg_file */
    }
    {
        size_t l = strlen(name);
        if (l > 2 && name[l - 1] == 'Z' && (name[0] == 'C' || name[0] == 'G' || name[0] == 'P') && strchr(name, '/') == NULL) {
            /* the three inline-texture menu models embed segment-5 texture
             * blobs (objecthandler_2.c portSwapInlineTextures) that the model
             * converter does not yet handle - keep them compiled for now */
#if !defined(PLATFORM_64BIT)
            if (strcmp(name, "PlegalpageZ") == 0 || strcmp(name, "PnintendologoZ") == 0 || strcmp(name, "PgoldeneyelogoZ") == 0) {
                return CLASS_NONE;
            }
#endif
            /* 64-bit (round 7b): the trio is served like every model - d43
             * carries their segment-5 texture blobs, and their texels are
             * converted in objecthandler_2.c portSwapInlineTextures */
            return CLASS_MODEL; /* C<name>Z chr, G<name>Z gun, P<name>Z prop - converted in load_object_fill_header */
        }
    }
    return CLASS_NONE;
}

/* Called by obInit after its size loop: for each compiled-in entry of a served
 * class that the ROM's directory also has (matched by name), point hw_address
 * at the ROM bytes and set rom_size. The vanilla fileIndexLoadTo* path then
 * loads it from the ROM. */
void romdataFilesApply(void)
{
    s32 i;
    s32 served = 0, missing = 0;
    const char *e;

    if (!sLoaded) {
        return;
    }
    (void) e;
    for (i = 1; i < file_entry_max && i < OBJ_INDEX_MAX; i++) {
        const char *name = file_resource_table[i].filename;
        s32 r;
        enum romfileclass cls = romdataClassOf(name);
        if (cls == CLASS_NONE || !romdataClassEnabled(cls)) {
            continue;
        }
        r = romdataFileIndex(name);
        if (r < 0 || sFiles[r].size == 0) {
            portConLogf("ge: romdata: file %-20s not in this ROM's directory - keeping the compiled-in copy\n", name);
            missing++;
            continue;
        }
        sCompiledData[i] = file_resource_table[i].hw_address;
        file_resource_table[i].hw_address = sRom + sFiles[r].romofs;
        resource_lookup_data_array[i].rom_size = (u32) sFiles[r].size;
        sServed[i] = 1;
        served++;
        if (sDebug) {
            portConLogf("ge: romdata: file %-20s served from ROM 0x%06X (%u bytes%s)\n", name, sFiles[r].romofs,
                    sFiles[r].size, sFiles[r].compressed ? ", 1172" : "");
        }
    }
    portConLogf("ge: romdata: file table: %d files served from the ROM (text, briefings, setups, stans)%s\n", served,
            missing ? " (some missing - compiled copies kept)" : "");

    /* GE_ROMPROBE=1: run every served file through the vanilla load path and
     * its preprocess+probe at boot, comparing all setups, stans, briefings and
     * text banks against their compiled-in copies. This is the regression
     * check, since a stage load only exercises its own files. */
    e = getenv("GE_ROMPROBE");
    if (e != NULL && e[0] != '\0' && e[0] != '0') {
        u32 cap = 2u << 20;
        u8 *scratch = (u8 *) malloc(cap);
        s32 n = 0;
        if (scratch != NULL) {
            for (i = 1; i < file_entry_max && i < OBJ_INDEX_MAX; i++) {
                if (sServed[i] && romdataClassOf(file_resource_table[i].filename) != CLASS_BG) {
                    /* bg files are used in place (obGetBGFileData), never loaded
                     * through a bank, and can exceed the scratch buffer - skip */
                    /* the load path writes poolRemaining / rom_remaining for the
                     * entry - the real stage load must see them untouched */
                    resource_lookup_data_entry saved = resource_lookup_data_array[i];
                    fileIndexLoadToAddr(i, FILELOADMETHOD_DEFAULT, scratch, (s32) cap);
                    resource_lookup_data_array[i] = saved;
                    n++;
                }
            }
            free(scratch);
        }
        portConLogf("ge: romdata: GE_ROMPROBE: %d served files loaded and probed\n", n);
    }
}

s32 romdataFileServed(s32 index)
{
    return (index > 0 && index < OBJ_INDEX_MAX) ? sServed[index] : 0;
}

/* the pristine compiled-in blob a served file replaced (or NULL) - the model
 * converter's runtime probe compares its output against it */
const u8 *romdataFileCompiled(s32 index)
{
    return (index > 0 && index < OBJ_INDEX_MAX) ? sCompiledData[index] : NULL;
}

/* TEXT BANKS: the ROM file is a table of big-endian, bank-relative offsets
 * followed by strings, while langGet's PC code expects absolute pointers like
 * the compiled-in banks, so rebase in place. The table size is the smallest
 * non-zero offset / 4; when the compiled bank is linked, the probe strcmp's
 * every slot. */
/* PORT STRING PATCHES: the port relabelled a few vanilla strings, so they are
 * applied by bank + slot after the rebase (the probe compares after patching,
 * so a stock cart still reads "identical"). Append-only; keep each string's
 * trailing '\n'. */
static const struct {
    const char *bank;
    u32 slot;
    const char *text;
} sTextPatches[] = {
    { "LoptionsE", 24, "hud\n" },           /* was "ratio": Wide pushes the HUD to the window edges (options.h);
                                             * formerly "hud centering", before Health
                                             * got its own option */
    { "LoptionsE", 34, "4:3\n" },           /* was "normal" */
    { "LoptionsE", 35, "wide\n" },          /* was "16:9" */
};

static void romdataTextPreprocess(s32 index, u8 *data, u32 size)
{
    u32 n = 0;
    u32 first = 0xFFFFFFFFu;
    u32 i;
    u32 differ = 0;
    u32 firstdiff = 0;
    u32 patched = 0;
    const char *bank = file_resource_table[index].filename;
    const char *const *compiled = (const char *const *) sCompiledData[index];

    if (size >= 2 && data[0] == 0x11 && data[1] == 0x72) {
        portConLogf("ge: romdata: text %s arrived still 1172-compressed (raw load) - not rebased\n",
                file_resource_table[index].filename);
        return;
    }
    for (i = 0; i + 4 <= size; i += 4) {
        u32 off = be32(data + i);
        if (i >= first) {
            break; /* reached the string area */
        }
        if (off != 0 && off < first) {
            first = off;
        }
        n++;
    }
    if (first == 0xFFFFFFFFu || first > size || (first & 3) != 0) {
        /* the cut stages' banks are 32 zero bytes: an empty table, nothing to rebase */
        for (i = 0; i < size && data[i] == 0; i++) {
        }
        if (i == size) {
            if (sDebug) {
                portConLogf("ge: romdata: text %-8s empty bank (%u zero bytes)\n", bank, size);
            }
            return;
        }
        portConLogf("ge: romdata: text %s: no offset table found (size %u) - left as is\n", bank, size);
        return;
    }
    n = first / 4;
    for (i = 0; i < n; i++) {
        u32 off = be32(data + i * 4);
        u32 v = 0;
        if (off != 0) {
            if (off >= size) {
                portConLogf("ge: romdata: text %s: slot %u offset 0x%X beyond the %u-byte bank - zeroed\n",
                        file_resource_table[index].filename, i, off, size);
            } else {
                v = (u32) (uintptr_t) (data + off);
            }
        }
        {
            size_t p;
            for (p = 0; p < sizeof(sTextPatches) / sizeof(sTextPatches[0]); p++) {
                if (sTextPatches[p].slot == i && strcmp(sTextPatches[p].bank, bank) == 0) {
#if defined(PLATFORM_64BIT)
                    /* R15: the literal is exe data; slot words are guest addresses */
                    v = GUEST_ADDR(portGuestString(sTextPatches[p].text));
#else
                    v = (u32) (uintptr_t) sTextPatches[p].text;
#endif
                    patched++;
                }
            }
        }
        if (compiled != NULL && v != 0) {
            /* a NULL ROM slot is not compared: the generated char *LxE[]
             * arrays drop trailing NULL slots, so compiled[i] may not exist */
            const char *c = compiled[i];
#if defined(PLATFORM_64BIT)
            const char *r = (const char *) GUEST_PTR(v); /* v is a guest address (memp bank or interned patch) */
#else
            const char *r = (const char *) (uintptr_t) v;
#endif
            if (c == NULL || strcmp(c, r) != 0) {
                if (differ == 0) {
                    firstdiff = i;
                }
                differ++;
            }
        }
        memcpy(data + i * 4, &v, 4);
    }
    if (compiled != NULL) {
        if (differ == 0) {
            portConLogf("ge: romdata: text %-8s %3u slots, identical to the compiled-in bank%s\n", bank, n,
                    patched ? " (port relabels applied)" : "");
        } else {
            portConLogf("ge: romdata: text %-8s %3u slots, %u DIFFER from the compiled-in bank (first slot %u) - a mod, or a compiled bank that was patched\n",
                    bank, n, differ, firstdiff);
        }
    } else {
        portConLogf("ge: romdata: text %-8s %3u slots from the ROM%s\n", bank, n, patched ? " (port relabels applied)" : "");
    }
}

void romdataFilePreprocess(s32 index, u8 *data, u32 size)
{
    if (!romdataFileServed(index) || data == NULL) {
        return;
    }
    if (size == 0) {
        portConLogf("ge: romdata: file %s: load produced 0 bytes (buffer too small for the compressed image?)\n",
                file_resource_table[index].filename);
        return;
    }
    switch (romdataClassOf(file_resource_table[index].filename)) {
    case CLASS_TEXT:
        romdataTextPreprocess(index, data, size);
        break;
    case CLASS_BRIEF:
        /* struct BriefStruct: u16 brief[4] + 10 x {u16 text, u16 difficulty} = 24 halfwords */
        if (size >= 48) {
            u32 i;
            for (i = 0; i < 48; i += 2) {
                u8 t = data[i];
                data[i] = data[i + 1];
                data[i + 1] = t;
            }
            if (sCompiledData[index] != NULL) {
                portConLogf("ge: romdata: brief %-16s %s the compiled-in file\n", file_resource_table[index].filename,
                        memcmp(sCompiledData[index], data, 48) == 0 ? "identical to" : "DIFFERS from");
            }
        } else {
            portConLogf("ge: romdata: brief %s: %u bytes, expected 48\n", file_resource_table[index].filename, size);
        }
        break;
#if defined(PLATFORM_64BIT)
    case CLASS_SETUP:
    case CLASS_STAN:
        /* round 7b: served from the pccg sidecar, already in the 64-bit host
         * layout (d88/d69) - romfile_setup.c/romfile_stan.c are 32-bit only */
        break;
#else
    case CLASS_SETUP:
        romfileSetupConvert(data, size, file_resource_table[index].filename, sCompiledData[index]);
        break;
    case CLASS_STAN:
        romfileStanConvert(data, size, file_resource_table[index].filename, sCompiledData[index]);
        break;
#endif
    case CLASS_MODEL:
        /* models are converted in load_object_fill_header (objecthandler_2.c),
         * where the compiled header carries numSwitches/numtextures - the ROM
         * bytes are just served here */
        break;
    case CLASS_BG:
        /* bg files are used in place from hw_address (obGetBGFileData), not
         * loaded through a bank, so the conversion is triggered in load_bg_file
         * (bg.c) via romfileBgConvert - nothing to do on this path */
        break;
    default:
        break;
    }
}

/* ==== the texture directory (g_Textures) and the image bank base ========= */

/* Load the texture table (g_Textures) and texture bank base from this cart:
 * walk the table, copy it over the compiled g_Textures before
 * image_entries_load sums the sizes, and take the bank base from the file
 * table. A table that doesn't reach its sentinel inside the ROM keeps the
 * compiled-in directory. */
#define ROMDATA_IMAGETABLE_OFS    0x28570u
#define ROMDATA_IMAGETABLE_STRIDE 8u
#define ROMDATA_IMAGETABLE_MASK   0x00FFFFFFu

static u32 sImagesBase, sImagesSize; /* the bank, from the file table; 0 = static romseg_list entry */

/* Dry-run walk of the texture table: report whether it looks valid (sentinel
 * found, >= 100 entries, bank fits in the ROM) and return its entry count and
 * summed size. g_Textures is never half-written by a table that fails. */
static int romdataTexTableFits(u32 stride, u32 mask, u32 *outn, u32 *outtot)
{
    u32 base = sFiles[sFileCount - 1].romofs + 16;
    u32 o, n = 0, total = 0;

    for (o = ROMDATA_IMAGETABLE_OFS; o + stride <= sCdataSize && n < PORT_TEXTURES_MAX - 1; o += stride, n++) {
        u32 size = be32(sCdata + o) & mask;
        if (size == 0xFFFFu) {
            *outn = n;
            *outtot = total;
            return n >= 100 && base + total <= sRomSize;
        }
        if (size > sRomSize) {
            return 0; /* a single entry larger than the whole cart */
        }
        total += size;
        if (base + total > sRomSize) {
            return 0; /* the bank ran off the end */
        }
    }
    return 0; /* no sentinel */
}

static void romdataImageTableApply(void)
{
    extern struct image_entry g_Textures[];
    const u32 mask = ROMDATA_IMAGETABLE_MASK, stride = ROMDATA_IMAGETABLE_STRIDE;
    u32 o, n = 0, total = 0, differ = 0, i;
    u32 tn, tt;

    if (sCdataSize < ROMDATA_IMAGETABLE_OFS + 8) {
        return;
    }
    if (!romdataTexTableFits(stride, mask, &tn, &tt)) {
        portConLogf("ge: romdata: texture table at cdata+0x%X does not fit the ROM (stock layout) - compiled-in table kept; textures may be wrong\n",
                ROMDATA_IMAGETABLE_OFS);
        return;
    }

    for (o = ROMDATA_IMAGETABLE_OFS; o + stride <= sCdataSize && n < PORT_TEXTURES_MAX - 1; o += stride, n++) {
        u32 w0 = be32(sCdata + o);
        u32 w1 = be32(sCdata + o + 4);
        u32 size = w0 & mask;
        if (size == 0xFFFFu) {
            break; /* the sentinel */
        }
        if (n < PORT_TEXTURES_MAX && g_Textures[n].dataoffset != size) {
            differ++;
        }
        g_Textures[n].hitSound = (w0 >> 28) & 0xF;
        g_Textures[n].hitTexture = (w0 >> 24) & 0xF;
        g_Textures[n].dataoffset = size;
        g_Textures[n].flag3 = (w1 >> 28) & 0xF;
        g_Textures[n].flag4 = (w1 >> 24) & 0xF;
        g_Textures[n].flag5 = (w1 >> 20) & 0xF;
        g_Textures[n].flag6 = (w1 >> 16) & 0xF;
        total += size;
    }
    if (n < 100 || o + stride > sCdataSize) {
        portConLogf("ge: romdata: texture table at cdata+0x%X did not parse (%u entries) - compiled-in table kept\n",
                ROMDATA_IMAGETABLE_OFS, n);
        return;
    }
    /* the sentinel, and anything past it */
    for (i = n; i < PORT_TEXTURES_MAX; i++) {
        g_Textures[i].hitSound = 0;
        g_Textures[i].hitTexture = 0;
        g_Textures[i].dataoffset = (i == n) ? 0xFFFFu : 0;
        g_Textures[i].flag3 = g_Textures[i].flag4 = g_Textures[i].flag5 = g_Textures[i].flag6 = 0;
    }

    /* the bank starts right after the last file (the 16-byte ob_end) */
    sImagesBase = sFiles[sFileCount - 1].romofs + 16;
    sImagesSize = total;
    if (sImagesBase + total > sRomSize) {
        portConLogf("ge: romdata: image bank 0x%X + %u bytes runs past the %u-byte image - textures will be garbage\n",
                sImagesBase, total, sRomSize);
    }
    portConLogf("ge: romdata: texture table: %u entries, %s the compiled-in table; image bank at 0x%X, %u bytes%s\n",
            n, differ ? "DIFFERS from" : "identical to", sImagesBase, total,
            sImagesBase == 0x8F7DF0u && total == 0x2EEF18u ? " (stock)" : " (non-stock layout)");
}

/* ==== accessors ======================================================== */

s32 romdataLoaded(void)
{
    return sLoaded;
}

/* the inflated data segment ("21990 cdata"): compiled tables the ROM carries
 * but no directory lists (fog/environment, model headers, ...) live here at
 * their N64 RAM offset minus ROMDATA_CDATA_VADDR. NULL until a ROM is loaded. */
const u8 *romdataCdata(u32 *size)
{
    if (size != NULL) {
        *size = sLoaded ? sCdataSize : 0;
    }
    return sLoaded ? sCdata : NULL;
}

u32 romdataCdataVaddr(void)
{
    return ROMDATA_CDATA_VADDR;
}

const u8 *romdataRom(u32 *size)
{
    if (size != NULL) {
        *size = sRomSize;
    }
    return sRom;
}

s32 romdataFileCount(void)
{
    return sFileCount;
}

const struct romfile *romdataFile(s32 index)
{
    return (index >= 0 && index < sFileCount) ? &sFiles[index] : NULL;
}

s32 romdataFileIndex(const char *name)
{
    s32 i;
    for (i = 1; i < sFileCount; i++) {
        if (strcmp(sFiles[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const u8 *romdataSeg(const char *name, u32 *size)
{
    size_t i;
    if (sRom == NULL) {
        return NULL;
    }
    if (sImagesBase != 0 && strcmp(name, "images") == 0) {
        /* the bank as the file table places it, not the stock offset */
        if (size != NULL) {
            *size = sImagesSize;
        }
        return sImagesBase + sImagesSize <= sRomSize ? sRom + sImagesBase : NULL;
    }
    for (i = 0; i < sizeof(sSegs) / sizeof(sSegs[0]); i++) {
        if (strcmp(sSegs[i].name, name) == 0 && sSegs[i].romofs + sSegs[i].size <= sRomSize) {
            if (size != NULL) {
                *size = sSegs[i].size;
            }
            return sRom + sSegs[i].romofs;
        }
    }
    return NULL;
}

/* ==== boot ============================================================= */

/* Why the candidate files were refused, for the missing-ROM message box: wrong
 * cart, ROM still inside a .zip, or no ROM at all. Files like Data's own
 * README.txt, desktop.ini and Thumbs.db don't count as ROM attempts. */
static int sRefusedCarts;    /* N64 images, or cart-sized files, that were refused */
static int sRefusedArchives; /* a .zip/.7z/.rar/.gz: the ROM was never extracted */

static void romdataNoteRefusal(const char *path)
{
    FILE *f = fopen(path, "rb");
    u8 m[4];
    long len;

    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fread(m, 1, 4, f) != 4) {
        fclose(f);
        return;
    }
    fclose(f);
    if ((m[0] == 0x50 && m[1] == 0x4B && m[2] == 0x03 && m[3] == 0x04)     /* zip */
        || (m[0] == 0x37 && m[1] == 0x7A && m[2] == 0xBC && m[3] == 0xAF)  /* 7z */
        || (m[0] == 0x52 && m[1] == 0x61 && m[2] == 0x72 && m[3] == 0x21)  /* rar */
        || (m[0] == 0x1F && m[1] == 0x8B)) {                               /* gzip */
        sRefusedArchives++;
    } else if (romdataDetectOrder(m) != ORDER_UNKNOWN || len >= (long) (ROMDATA_ROM_SIZE / 2)) {
        sRefusedCarts++;
    }
}

#ifdef PORT
static char sHostRom[1200];

void romdataSetRomPath(const char *path)
{
    snprintf(sHostRom, sizeof(sHostRom), "%s", path != NULL ? path : "");
}
#endif

s32 romdataInit(int argc, char **argv)
{
    extern void portShowErrorBox(const char *title, const char *text); /* video.c; weak no-op headless */
    const char *e;
    const char *argvrom = NULL;
    const char *flagrom = NULL;
    int i;

    e = getenv("GE_ROMDBG");
    sDebug = (e != NULL && e[0] != '\0' && e[0] != '0');

    romdataEnsureDir(ROMDATA_DATA_DIR);
    romdataReadIni();

    e = getenv("GE_ROMDATA");
    if (e != NULL && e[0] == '0') {
#ifdef GE_ROMDATA_FALLBACK
        portConLogf("ge: romdata: disabled (GE_ROMDATA=0) - compiled-in assets\n");
        return 0;
#else
        portConLogf("ge: romdata: GE_ROMDATA=0 ignored - this build has no compiled-in copies (GE_ROMDATA_FALLBACK=OFF)\n");
#endif
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom-file") == 0 && i + 1 < argc) {
            flagrom = argv[++i];
        } else if (strncmp(argv[i], "--rom-file=", 11) == 0) {
            flagrom = argv[i] + 11;
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            sArgLangJp = (v[0] == 'j' || v[0] == 'J');
        } else if (strncmp(argv[i], "--lang=", 7) == 0) {
            sArgLangJp = (argv[i][7] == 'j' || argv[i][7] == 'J');
        } else if (i == 1 && argv[i][0] != '-') {
            argvrom = argv[i]; /* Explorer drop: the dropped file's full path */
        }
    }

#ifdef PORT
    if (sHostRom[0] != '\0') {
        if (romdataTryLoad(sHostRom, ROMSRC_HOST, 0)) goto done;
        romdataNoteRefusal(sHostRom);
    }
#endif
    if (argvrom != NULL) {
        if (romdataTryLoad(argvrom, ROMSRC_ARGV, 0)) goto done;
        romdataNoteRefusal(argvrom);
    }
    if (flagrom != NULL) {
        if (romdataTryLoad(flagrom, ROMSRC_ARGFLAG, 0)) goto done;
        romdataNoteRefusal(flagrom);
    }
    e = getenv("GE_ROM");
    if (e != NULL && e[0] != '\0') {
        if (romdataTryLoad(e, ROMSRC_ENV, 0)) goto done;
        romdataNoteRefusal(e);
    }
    if (sIniRom[0] != '\0') {
        char p[1200];
        romdataResolve(p, sizeof(p), sIniRom);
        if (romdataTryLoad(p, ROMSRC_INI, 0)) goto done;
        romdataNoteRefusal(p);
    }
    {
        static char names[ROMDATA_LIST_MAX][256];
        char datadir[1200];
        int n;
        snprintf(datadir, sizeof(datadir), "%s", portRootPath(ROMDATA_DATA_DIR));
        n = romdataListDir(datadir, 0, names, ROMDATA_LIST_MAX);
        for (i = 0; i < n; i++) {
            char p[1500];
            snprintf(p, sizeof(p), "%s" PATHSEP "%s", datadir, names[i]);
            if (romdataTryLoad(p, ROMSRC_DATADIR, 1)) goto done;
            romdataNoteRefusal(p); /* an EMPTY folder, a packed ROM and the
                                    * WRONG rom are different mistakes and get
                                    * different words */
        }
        if (n == 0) {
            portConLogf("ge: romdata: %s is empty\n", datadir);
        }
    }
    {
        char p[1200];
        snprintf(p, sizeof(p), "%s", portRootPath("baserom.u.z64"));
        if (romdataIsFile(p) && romdataTryLoad(p, ROMSRC_LEGACY, 0)) goto done;
        snprintf(p, sizeof(p), "%s", portRootPath(".." PATHSEP "baserom.u.z64"));
        if (romdataIsFile(p) && romdataTryLoad(p, ROMSRC_LEGACY, 0)) goto done;
    }

    portConLogf("ge: romdata: NO ROM - tried argv[1], --rom-file, GE_ROM, midas.ini [data] rom=, %s, baserom.u.z64\n",
            portRootPath(ROMDATA_DATA_DIR));
#ifdef GE_ROMDATA_FALLBACK
    portConLogf("ge: romdata: put your GoldenEye 007 (U) cart image (.z64/.v64/.n64) in %s or drop it onto the exe; running on the compiled-in assets\n",
            portRootPath(ROMDATA_DATA_DIR));
    return 0;
#else
    {
        /* Show a message box, since someone who double-clicked the exe never
         * sees the console; an empty folder and a folder of refused files get
         * different messages. Only real attempts count (N64 images or
         * cart-sized files), and a packed ROM is named as such so a README or
         * .zip is never called "a modded ROM". */
        char msg[1700];
        const char *dir = portRootPath(ROMDATA_DATA_DIR);

        if (sRefusedCarts > 0) {
            snprintf(msg, sizeof(msg),
                     "No usable GoldenEye 007 ROM found in ./%s.\n\n"
                     "%d cart image(s) were found, but none of them is a stock GoldenEye 007 (U) cart: "
                     "a modded ROM, another region, or a bad dump.\n"
                     "Please provide your ROM into this folder and try again.\n\n"
                     "Looked in:\n%s",
                     ROMDATA_DATA_DIR, sRefusedCarts, dir);
        } else if (sRefusedArchives > 0) {
            snprintf(msg, sizeof(msg),
                     "No GoldenEye 007 ROM found in ./%s.\n\n"
                     "The ROM is still packed in an archive (.zip/.7z/...). "
                     "Extract the .z64, .v64 or .n64 file from it into this folder and try again.\n\n"
                     "Looked in:\n%s",
                     ROMDATA_DATA_DIR, dir);
        } else {
            snprintf(msg, sizeof(msg),
                     "No GoldenEye 007 ROM found in ./%s.\n"
                     "Please provide your ROM into this folder and try again.\n\n"
                     "Looked in:\n%s\n\n"
                     "The ROM may be .z64, .v64 or .n64. You can also drag one onto the game.",
                     ROMDATA_DATA_DIR, dir);
        }
        portConLogf("ge: romdata: %s\n", msg);
        fflush(stderr);
        portShowErrorBox("GoldenEye 007", msg);
        exit(3);
    }
#endif

done:
    romdataCheckSegs(sRomSize);
#if defined(PLATFORM_64BIT)
    /* Round 7b (docs/port-ios/12-round7b-boot.md, doc 11 section 3): the
     * 64-bit host layout of every model, bg, stan and setup comes from the
     * pcconvert sidecars, derived from THIS cart on first run (SHA-1 stamp
     * in Data/pcconvert.stamp) and loaded into the guest cart region at the
     * first 1 MB boundary after the ROM. obInit patches the file table. */
    {
        char dataDir[1200];
        snprintf(dataDir, sizeof(dataDir), "%s", portRootPath(ROMDATA_DATA_DIR));
        pcsidecarSetDataDir(dataDir);
        if (pcsidecarPrepare(sRom, sRomSize) != 1) {
            char msg[1600];
            snprintf(msg, sizeof(msg),
                     "Could not convert the GoldenEye 007 ROM's models and stages for this build.\n\n"
                     "The converted files go in:\n%s\n\nCheck that the folder is writable and try again.",
                     dataDir);
            portConLogf("ge: romdata: %s\n", msg);
            fflush(stderr);
            portShowErrorBox("GoldenEye 007", msg);
            exit(3);
        }
        /* Round 8a (round 7b review, soft spot c): a sidecar that is not
         * there, or does not load, after a successful prepare leaves every
         * model or stage unserved, and the game would run on into faults far
         * from here. Fatal, the same way as a failed prepare. */
        {
            const char *failed = NULL;
            if (pcmodelsReserveSize(sRom) == 0 || pcmodelsLoadSidecars(0x10000000u, sRomSize) == 0)
                failed = "models (pcmodels)";
            else if (pccgReserveSize(sRom) == 0 || pccgLoadSidecars(0) == 0)
                failed = "stages (pccg)";
            if (failed != NULL) {
                char msg[1600];
                snprintf(msg, sizeof(msg),
                         "Could not load the converted GoldenEye 007 %s for this build.\n\n"
                         "The converted files are in:\n%s\n\nDelete that folder to convert the ROM again.",
                         failed, dataDir);
                portConLogf("ge: romdata: %s\n", msg);
                fflush(stderr);
                portShowErrorBox("GoldenEye 007", msg);
                exit(3);
            }
        }
    }
#endif
    sLoaded = 1;
    /* the texture directory + bank base from THIS cart, before the images
     * consumer is pointed at the bank */
    romdataImageTableApply();
    /* images, music, ramrom and gunbarrel are read from the cart */
    portRomdataSegsApply();
    /* the fog / environment tables (bgfog.c fog_tables / fog_tables2) are a
     * compiled data table the cart carries in cdata but no directory lists;
     * convert this ROM's copy over the compiled one so a mod's custom fog,
     * sky and water import */
    {
        extern void portFogServeFromRom(void);
        portFogServeFromRom();
    }
    /* animation + fonts are filled from the ROM by their own
     * builders (animdata.c, segments.c portFontBuildSegments) when they
     * run; text banks are served through the file table (romdataFilesApply
     * from obInit, later in boot) */
#ifdef GE_ROMDATA_FALLBACK
    portConLogf("ge: romdata: loaded; all asset classes served from the ROM (Phases A-F); compiled-in copies kept for the GE_ROMDATA=0 fallback\n");
#else
    portConLogf("ge: romdata: loaded; all asset classes served from the ROM (Phases A-F); no compiled-in copies (GE_ROMDATA_FALLBACK=OFF)\n");
#endif
    return 1;
}

#if defined(PLATFORM_64BIT)
/* ==== .ctl bank re-layout (donor D37, ripple round: docs/port-ios/10-ripples.md A4)
 *
 * Ported from the donor (GoldenEye-Decomp-ios engine/port/src/romdata.c,
 * romdataAudioBankPcSize/romdataFixupAudioBank) minus its temporary D202
 * dump probes; it replaces the inert stubs that were in
 * port/apple/donor_stubs.c. music.c (merged donor D37 arm) calls it between
 * romCopy and alBnkfNew.
 *
 * On N64 the ROM layout of a .ctl bank tree (ALBankFile -> ALBank ->
 * ALInstrument -> ALSound -> ALWaveTable / ALEnvelope / ALKeyMap /
 * ALADPCMloop / ALRawLoop / ALADPCMBook) and the C structs coincide. On a
 * 64-bit host every pointer slot is 8 bytes, and the ROM packs the structs
 * back to back, so patching in place (what Midas's 32-bit alBnkfNew does)
 * misreads every field after the first pointer slot and overlaps the next
 * struct. The tree is therefore re-laid out into a fresh compact image: each
 * sub-struct is placed once, 8-byte aligned, multi-byte fields converted to
 * host order, and each pointer slot holds the sub-struct's NEW offset from
 * the image start (zero-extended). Wavetable `base` stays an offset into the
 * .tbl segment. alBnkfNew's 64-bit arm (port/src/audio_stubs.c) rebases them.
 *
 * ROM layout (offsets within each struct; multi-byte fields big-endian):
 *   ALBankFile    +0 s16 revision, +2 s16 bankCount, +4 N x u32 bank offsets
 *   ALBank        +0 s16 instCount, +2 u8 flags, +3 u8 pad,
 *                 +4 s32 sampleRate, +8 u32 percussion, +12 M x u32 insts
 *   ALInstrument  +0..+11 12 x u8, +12 s16 bendRange, +14 s16 soundCount,
 *                 +16 K x u32 sound offsets
 *   ALSound       +0 u32 envelope, +4 u32 keyMap, +8 u32 wavetable,
 *                 +12 u8 pan, +13 u8 volume, +14 u8 flags
 *   ALWaveTable   +0 u32 base, +4 s32 len, +8 u8 type, +9 u8 flags,
 *                 adpcm: +12 u32 loop, +16 u32 book; raw: +12 u32 loop
 *   ALEnvelope    +0/+4/+8 s32 times, +12/+13 u8 volumes
 *   ALKeyMap      6 x u8
 *   ALADPCMloop   +0/+4/+8 u32 start/end/count, +12 s16[16] state
 *   ALRawLoop     +0/+4/+8 u32 start/end/count
 *   ALADPCMBook   +0 s32 order, +4 s32 npredictors, +8 s16 coefficients
 * Host offsets written below are checked against <PR/libaudio.h> by the
 * _Static_asserts in audio_stubs.c's 64-bit alBnkfNew.
 */

#define AF_UNSET 0xFFFFFFFFu

struct afCtx {
    const u8 *src;      /* pristine ROM-layout bytes */
    u32 srcSize;
    u8 *dst;            /* image buffer (write pass) */
    u32 allocSize;
    int write;          /* 0 = measure pass, 1 = write pass */
    u32 pos;            /* cursor, kept 8-aligned by afReserve */
    u8 *visited;        /* [srcSize] */
    u32 *newOff;        /* [srcSize] ROM offset -> image offset */
    s16 revision;
    s16 bankCount;
    int errors;
};

static u16 afRd16(const struct afCtx *c, u32 off)
{
    return (u16)((c->src[off] << 8) | c->src[off + 1]);
}

static u32 afRd32(const struct afCtx *c, u32 off)
{
    return ((u32)c->src[off] << 24) | ((u32)c->src[off + 1] << 16) |
           ((u32)c->src[off + 2] << 8) | (u32)c->src[off + 3];
}

/* host order (little-endian on every 64-bit target this port builds for) */
static void afWr16(struct afCtx *c, u32 off, u16 v)
{
    c->dst[off] = (u8)v;
    c->dst[off + 1] = (u8)(v >> 8);
}

static void afWr32(struct afCtx *c, u32 off, u32 v)
{
    c->dst[off] = (u8)v;
    c->dst[off + 1] = (u8)(v >> 8);
    c->dst[off + 2] = (u8)(v >> 16);
    c->dst[off + 3] = (u8)(v >> 24);
}

static void afWrPtr(struct afCtx *c, u32 off, u32 value)
{
    afWr32(c, off, value);
    c->dst[off + 4] = c->dst[off + 5] = c->dst[off + 6] = c->dst[off + 7] = 0;
}

/* -1: invalid offset, 0: first visit, 1: already placed */
static int afEnter(struct afCtx *c, u32 off)
{
    if (off == 0 || off >= c->srcSize)
        return -1;
    if (c->visited[off])
        return 1;
    c->visited[off] = 1;
    return 0;
}

static u32 afReserve(struct afCtx *c, u32 size)
{
    u32 n = (c->pos + 7u) & ~7u;

    c->pos = n + size;
    return c->write ? n : 0;
}

static u32 afPtr(struct afCtx *c, u32 off)
{
    if (off == 0 || off >= c->srcSize)
        return 0;
    return (c->newOff[off] != AF_UNSET) ? c->newOff[off] : 0;
}

static void afFixupBook(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    s32 order, npred;
    u32 size = 8, n, i;

    if (r == 1)
        return;
    if (r < 0 || o + 8 > c->srcSize) { c->errors++; return; }
    order = (s32)afRd32(c, o);
    npred = (s32)afRd32(c, o + 4);
    if (order > 0 && npred > 0)
        size += 2u * (u32)order * (u32)npred * 8u; /* load.c bookSize */
    else
        portConLogf("ge: romdataFixupAudioBank: bad ADPCM book @0x%X (order %d, npredictors %d)\n", o, order, npred);
    n = afReserve(c, size);
    if (!c->write)
        return;
    c->newOff[o] = n;
    afWr32(c, n, (u32)order);
    afWr32(c, n + 4, (u32)npred);
    if (order > 0 && npred > 0 && o + size <= c->srcSize) {
        u32 ncoeff = 2u * (u32)order * (u32)npred * 4u;
        for (i = 0; i < ncoeff; i++)
            afWr16(c, n + 8 + 2 * i, afRd16(c, o + 8 + 2 * i));
    }
}

static void afFixupAdpcmLoop(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    u32 n, i;

    if (r == 1)
        return;
    if (r < 0 || o + 44 > c->srcSize) { c->errors++; return; }
    n = afReserve(c, 48);
    if (!c->write)
        return;
    c->newOff[o] = n;
    for (i = 0; i < 3; i++)
        afWr32(c, n + 4 * i, afRd32(c, o + 4 * i));
    for (i = 0; i < 16; i++)
        afWr16(c, n + 12 + 2 * i, afRd16(c, o + 12 + 2 * i));
}

static void afFixupRawLoop(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    u32 n, i;

    if (r == 1)
        return;
    if (r < 0 || o + 12 > c->srcSize) { c->errors++; return; }
    n = afReserve(c, 16);
    if (!c->write)
        return;
    c->newOff[o] = n;
    for (i = 0; i < 3; i++)
        afWr32(c, n + 4 * i, afRd32(c, o + 4 * i));
}

static void afFixupEnvelope(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    u32 n, i;

    if (r == 1)
        return;
    if (r < 0 || o + 14 > c->srcSize) { c->errors++; return; }
    n = afReserve(c, 16);
    if (!c->write)
        return;
    c->newOff[o] = n;
    for (i = 0; i < 3; i++)
        afWr32(c, n + 4 * i, afRd32(c, o + 4 * i));
    c->dst[n + 12] = c->src[o + 12];
    c->dst[n + 13] = c->src[o + 13];
}

static void afFixupWaveTable(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    u32 base, loop, book, n;
    s32 len;
    u8 type;

    if (r == 1)
        return;
    if (r < 0 || o + 20 > c->srcSize) { c->errors++; return; }
    base = afRd32(c, o);
    len = (s32)afRd32(c, o + 4);
    type = c->src[o + 8];
    if (type == 0 /* AL_ADPCM_WAVE */) {
        loop = afRd32(c, o + 12);
        book = afRd32(c, o + 16);
        if (loop) afFixupAdpcmLoop(c, loop);
        if (book) afFixupBook(c, book);
    } else if (type == 1 /* AL_RAW16_WAVE */) {
        loop = afRd32(c, o + 12);
        book = 0;
        if (loop) afFixupRawLoop(c, loop);
    } else {
        portConLogf("ge: romdataFixupAudioBank: unknown wavetable type %d @0x%X\n", type, o);
        loop = book = 0;
    }
    n = afReserve(c, 32);
    if (!c->write)
        return;
    c->newOff[o] = n;
    afWrPtr(c, n, base); /* stays a .tbl offset */
    afWr32(c, n + 8, (u32)len);
    c->dst[n + 12] = type;
    c->dst[n + 13] = c->src[o + 9];
    afWrPtr(c, n + 16, afPtr(c, loop));
    if (type == 0)
        afWrPtr(c, n + 24, afPtr(c, book));
}

static void afFixupSound(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    u32 env, km, wt, n;

    if (r == 1)
        return;
    if (r < 0 || o + 15 > c->srcSize) { c->errors++; return; }
    env = afRd32(c, o);
    km = afRd32(c, o + 4);
    wt = afRd32(c, o + 8);
    if (env) afFixupEnvelope(c, env);
    if (km) {
        int kr = afEnter(c, km);
        if (kr == -1 || km + 6 > c->srcSize) {
            c->errors++;
        } else if (kr == 0) {
            u32 kn = afReserve(c, 8);
            if (c->write) {
                c->newOff[km] = kn;
                memcpy(c->dst + kn, c->src + km, 6); /* ALKeyMap: all u8 */
            }
        }
    }
    if (wt) afFixupWaveTable(c, wt);
    n = afReserve(c, 32);
    if (!c->write)
        return;
    c->newOff[o] = n;
    afWrPtr(c, n, afPtr(c, env));
    afWrPtr(c, n + 8, afPtr(c, km));
    afWrPtr(c, n + 16, afPtr(c, wt));
    c->dst[n + 24] = c->src[o + 12]; /* pan/volume/flags */
    c->dst[n + 25] = c->src[o + 13];
    c->dst[n + 26] = c->src[o + 14];
}

static void afFixupInst(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    s16 scount;
    u32 maxSounds, n, i;

    if (r == 1)
        return;
    if (r < 0 || o + 16 > c->srcSize) { c->errors++; return; }
    scount = (s16)afRd16(c, o + 14);
    maxSounds = (c->srcSize - (o + 16)) / 4;
    if (scount < 0 || (u32)scount > maxSounds) {
        portConLogf("ge: romdataFixupAudioBank: soundCount %d @0x%X exceeds blob capacity %u\n", scount, o, maxSounds);
        scount = (s16)(scount < 0 ? 0 : maxSounds);
    }
    for (i = 0; i < (u32)scount; i++) {
        u32 soff = afRd32(c, o + 16 + 4 * i);
        if (soff)
            afFixupSound(c, soff);
    }
    n = afReserve(c, 16 + 8u * (u32)scount);
    if (!c->write)
        return;
    c->newOff[o] = n;
    memcpy(c->dst + n, c->src + o, 12); /* 12 x u8 */
    afWr16(c, n + 12, afRd16(c, o + 12)); /* bendRange */
    afWr16(c, n + 14, (u16)scount);
    for (i = 0; i < (u32)scount; i++)
        afWrPtr(c, n + 16 + 8 * i, afPtr(c, afRd32(c, o + 16 + 4 * i)));
}

static void afFixupBank(struct afCtx *c, u32 o)
{
    int r = afEnter(c, o);
    s16 icount;
    u32 perc, maxInsts, n, i;

    if (r == 1)
        return;
    if (r < 0 || o + 12 > c->srcSize) { c->errors++; return; }
    icount = (s16)afRd16(c, o);
    perc = afRd32(c, o + 8);
    maxInsts = (c->srcSize - (o + 12)) / 4;
    if (icount < 0 || (u32)icount > maxInsts) {
        portConLogf("ge: romdataFixupAudioBank: instCount %d @0x%X exceeds blob capacity %u\n", icount, o, maxInsts);
        icount = (s16)(icount < 0 ? 0 : maxInsts);
    }
    if (perc) afFixupInst(c, perc);
    for (i = 0; i < (u32)icount; i++) {
        u32 ioff = afRd32(c, o + 12 + 4 * i);
        if (ioff)
            afFixupInst(c, ioff);
    }
    n = afReserve(c, 16 + 8u * (u32)icount);
    if (!c->write)
        return;
    c->newOff[o] = n;
    afWr16(c, n, (u16)icount);
    c->dst[n + 2] = c->src[o + 2]; /* flags */
    c->dst[n + 3] = c->src[o + 3]; /* pad */
    afWr32(c, n + 4, afRd32(c, o + 4)); /* sampleRate */
    afWrPtr(c, n + 8, afPtr(c, perc));
    for (i = 0; i < (u32)icount; i++)
        afWrPtr(c, n + 16 + 8 * i, afPtr(c, afRd32(c, o + 12 + 4 * i)));
}

/* One pass over the tree; returns the image size (16-aligned). */
static u32 afWalk(struct afCtx *c)
{
    u32 maxBanks;
    s16 i;

    c->revision = (s16)afRd16(c, 0);
    c->bankCount = (s16)afRd16(c, 2);
    if (c->revision != 0x4231 /* AL_BANK_VERSION 'B1' */)
        portConLogf("ge: romdataFixupAudioBank: bad bank revision 0x%04X\n", (u32)(u16)c->revision);
    maxBanks = (c->srcSize - 4) / 4;
    if (c->bankCount < 0 || (u32)c->bankCount > maxBanks) {
        portConLogf("ge: romdataFixupAudioBank: bankCount %d exceeds blob capacity %u\n", c->bankCount, maxBanks);
        c->bankCount = (s16)(c->bankCount < 0 ? 0 : maxBanks);
    }
    /* the ALBankFile header sits at image offset 0 (alBnkfNew rebases
     * relative to the blob start), so reserve it first */
    afReserve(c, 8 + 8u * (u32)c->bankCount);
    for (i = 0; i < c->bankCount; i++) {
        u32 off = afRd32(c, 4 + 4 * (u32)i);
        if (off)
            afFixupBank(c, off);
    }
    if (c->write) {
        afWr16(c, 0, (u16)c->revision);
        afWr16(c, 2, (u16)c->bankCount);
        for (i = 0; i < c->bankCount; i++)
            afWrPtr(c, 8 + 8 * (u32)i, afPtr(c, afRd32(c, 4 + 4 * (u32)i)));
    }
    return (c->pos + 15u) & ~15u;
}

static void afPass(struct afCtx *c, int write)
{
    u32 i;

    c->write = write;
    c->pos = 0;
    memset(c->visited, 0, c->srcSize);
    if (c->newOff)
        for (i = 0; i < c->srcSize; i++)
            c->newOff[i] = AF_UNSET;
    afWalk(c);
}

u32 romdataAudioBankPcSize(const u8 *src, u32 srcSize)
{
    struct afCtx c;
    u32 n;

    if (src == NULL || srcSize < 4)
        return 0;
    memset(&c, 0, sizeof(c));
    c.src = src;
    c.srcSize = srcSize;
    c.visited = (u8 *)calloc(srcSize, 1);
    if (c.visited == NULL)
        return 0;
    n = afWalk(&c);
    free(c.visited);
    return n;
}

void romdataFixupAudioBank(u8 *blob, u32 srcSize, u32 allocSize)
{
    struct afCtx c;

    if (blob == NULL || srcSize < 4) {
        portConLogf("ge: romdataFixupAudioBank: blob too small (%u)\n", srcSize);
        return;
    }
    memset(&c, 0, sizeof(c));
    c.srcSize = srcSize;
    c.dst = blob;
    c.allocSize = allocSize;
    /* the write pass lays the image over the pristine bytes (dst == blob),
     * so both passes read from an untouched copy */
    c.src = (const u8 *)malloc(srcSize);
    c.visited = (u8 *)malloc(srcSize);
    c.newOff = (u32 *)malloc(4 * (size_t)srcSize);
    if (c.src == NULL || c.visited == NULL || c.newOff == NULL) {
        free((void *)c.src);
        free(c.visited);
        free(c.newOff);
        portConLogf("ge: romdataFixupAudioBank: out of memory\n");
        return;
    }
    memcpy((void *)c.src, blob, srcSize);

    afPass(&c, 0); /* measure, then write over the same arrays */
    if (c.pos > allocSize) {
        portConLogf("ge: romdataFixupAudioBank: image 0x%X exceeds allocation 0x%X\n", c.pos, allocSize);
    } else {
        /* every slot is overwritten field by field; clear the padding */
        memset(blob, 0, allocSize);
        c.errors = 0;
        afPass(&c, 1);
        if (c.errors)
            portConLogf("ge: romdataFixupAudioBank: %d bad offsets skipped\n", c.errors);
    }
    free((void *)c.src);
    free(c.visited);
    free(c.newOff);
}
#endif /* PLATFORM_64BIT: .ctl bank re-layout */

#if defined(PLATFORM_64BIT)
/* ==== font segments: N64 layout -> LP64 struct font (round 7b) ===========
 *
 * Ported from the donor's romdataFontPcSize/romdataFixupFont (D51/D130,
 * engine/port/src/romdata.c). struct fontchar is 5 s32 + a u8 * on every
 * build: 24 bytes on N64/x86, 32 on LP64 (pixeldata 8-aligned at +24), so
 * struct font's chars[] start at 680 instead of 676 and every glyph record
 * moves. load_font_tables allocates romdataFontPcSize() bytes, romCopies the
 * n64Size-byte segment image and calls romdataFixupFont, which re-lays the
 * table out in place and moves the glyph bytes up; its own loop then adds
 * the font base to each pixeldata offset, as on N64.
 *
 * Midas difference: the segment image (segments.c portFontFromRom) already
 * holds the kerning words and the 94x6 fontchar words NATIVE, so nothing is
 * byte-swapped here (the donor reads raw big-endian cart bytes). */
#define FONT_N64_COFF    (169 * 4)
#define FONT_N64_CSTRIDE 24
#define FONT_NCHARS      94

static u32 romdataFontPixStart(const u8 *img, u32 n64Size, u32 *nchars)
{
    u32 i, n, pixStart = 0;

    n = (n64Size - FONT_N64_COFF) / FONT_N64_CSTRIDE;
    if (n > FONT_NCHARS) {
        n = FONT_NCHARS;
    }
    for (i = 0; i < n; i++) {
        u32 o;
        memcpy(&o, img + FONT_N64_COFF + FONT_N64_CSTRIDE * i + 20, 4);
        if (o != 0 && (pixStart == 0 || o < pixStart)) {
            pixStart = o;
        }
    }
    *nchars = n;
    return pixStart;
}

u32 romdataFontPcSize(const u8 *src, u32 n64Size)
{
    u32 n, pixStart;

    if (n64Size <= FONT_N64_COFF) {
        return n64Size;
    }
    pixStart = romdataFontPixStart(src, n64Size, &n);
    if (pixStart == 0 || pixStart > n64Size) {
        pixStart = n64Size;
    }
    return (u32) (__builtin_offsetof(struct font, chars) + FONT_NCHARS * sizeof(struct fontchar)) + n64Size - pixStart;
}

void romdataFixupFont(u8 *blob, u32 n64Size)
{
    u32 i, n, pixStart, pcPixOff;

    if (n64Size <= FONT_N64_COFF) {
        return;
    }
    pixStart = romdataFontPixStart(blob, n64Size, &n);
    pcPixOff = (u32) (__builtin_offsetof(struct font, chars) + FONT_NCHARS * sizeof(struct fontchar));
    if (pixStart != 0 && pixStart < n64Size) {
        memmove(blob + pcPixOff, blob + pixStart, n64Size - pixStart);
    }
    /* descending: the PC record i lies above the N64 record i, so walking
     * down never overwrites a source not yet read; each record's six words
     * are staged first because the two spans overlap (donor D130) */
    for (i = n; i-- > 0;) {
        const u8 *s = blob + FONT_N64_COFF + FONT_N64_CSTRIDE * i;
        struct fontchar *d = (struct fontchar *) (blob + __builtin_offsetof(struct font, chars)) + i;
        u32 f[6];
        memcpy(f, s, sizeof(f));
        d->index = (s32) f[0];
        d->baseline = (s32) f[1];
        d->height = (s32) f[2];
        d->width = (s32) f[3];
        d->kerningindex = (s32) f[4];
        if (f[5] != 0 && pixStart != 0 && f[5] >= pixStart) {
            f[5] = pcPixOff + (f[5] - pixStart);
        }
        d->pixeldata = (u8 *) (uintptr_t) f[5]; /* an offset; load_font_tables adds the base */
    }
}
#endif /* PLATFORM_64BIT: fonts */
