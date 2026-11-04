/* Imported verbatim from GoldenEye-Decomp-ios engine/port/src/xblaimport.c (last changed 9e9929b7; donor HEAD 55a3a9c8),
 * round 10, XBLA step 1 (docs/port-ios/19-round10-xbla-import.md). 64-bit host layer only. */
/*
 * GoldenEye XBLA import (Tier B) — see port/include/xblaimport.h.
 * Round 19c, D-096..D-098.
 *
 * Three ways in, one way out:
 *   folder   files/<rel> read with stdio;
 *   package  the STFS package read in place with x360.c;
 *   .7z      the package streamed out of its solid block with the LZMA SDK,
 *            the STFS layout followed as the bytes go by (Perfect Dark's
 *            overlay 0046, D-079 there): the head is spooled until the file
 *            table is in, then each wanted file is gathered in memory from
 *            its blocks, decoded and dropped. Nothing of the release is
 *            written to disk but the pictures the map asks for.
 * Then, per file the map names: beantex.c decodes the named pictures and
 * pngwrite.c writes them as <texnum>.png under $C/xbla/{tex,same}/.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "x360.h"
#include "beantex.h"
#include "pngwrite.h"
#include "texpack.h"
#include "beanstage.h"
#include "xblaimport.h"

float floorf(float x);   /* the decomp's include/math.h stub does not declare it */
float fabsf(float x);    /* round 44 (xbrScale), likewise */

#include "external/lzma/7z.h"
#include "external/lzma/7zAlloc.h"
#include "external/lzma/7zCrc.h"
#include "external/lzma/7zFile.h"
#include "external/lzma/Lzma2Dec.h"
#include "external/lzma/LzmaDec.h"

#if !defined(_WIN32)
#include <pthread.h>
#define XBLA_HAVE_THREADS 1
#endif

struct xblamaprow {
    int16_t texnum;
    int16_t file;
    const char *pic;
    uint8_t hd;
    int16_t score; /* NCC x 1000 */
};

#include "xblamap_table.h"

#define NUM_ROWS ((int)(sizeof(xblamapRows) / sizeof(xblamapRows[0])))
#define NUM_FILES ((int)(sizeof(xblamapFiles) / sizeof(xblamapFiles[0])))

/* Bumped when the table or what is written changes: an older import is redone.
 * 3 (Tier C round C1, D-110): the layer also carries Bean's own level files,
 * copied whole to $C/xbla/files/<rel> (rawWanted[]). A version-2 layer has
 * none, so it is redone once at the next launch: iOS re-imports from the
 * copy kept in Documents/xbla (D-096), the oracle from Video.XblaPath. */
/* 4 (Tier C round C3, D-119): and the props' files (beanpropfiles.h,
 * new/prop/<name>/default.bin for every beanproptable.h row) - a version-3
 * layer is redone once the same way. */
/* 5 (Tier C rounds 26a + 26b, never shipped alone): first-person guns
 * (beangunfiles.h, D-144) and characters (beanchrfiles.h, D-153) on two
 * branches, each bumping 4 -> 5.
 * 6 (round 26 merge): both lists - the guns' files (new/gun/<name>/default.bin
 * for every beanguntable.h row, ~28 MB) and the characters' files
 * (new/char/<name> and new/head/<name> for every beanchrtable.h row, 72 files,
 * 24 MB). Any older layer (a branch build's 5 had only one list) is redone
 * once the same way. */
/* 7 (round 28a, C5, D-166): every verified level's file (beanstagefiles.h,
 * new/background/<name>/default.bin, 19 files, ~143 MB with Dam's, +128 MB over v6) - any
 * older layer is redone once the same way. */
/* 8 (round 30a, D-246): the multiplayer held M16 and RC-P90 (new/prop/chrm16, new/prop/chrfnp90)
 * joins the props' list - a version-7 layer is redone once the same way. */
/* 9 (round 35b, D-323): the held auto shotgun and rocket launcher
 * (new/prop/chrautoshot, new/prop/chrrocketlaunch) join the props' list, drawn
 * through propfit.py's HELD_OVERRIDE rows - a version-8 layer is redone once. */
/* 10 (Midas round 24, docs/port-ios/33-round24-greenhats.md): the release's hatbird and hatbirdbrown
 * (GoldenEye's PhattbirdZ/PhattbirdbrownZ, Facility's guards' caps) join the props' list - a
 * version-9 layer is redone once the same way. */
/* 11 (Midas round 43, docs/port-ios/60-round43-xbla-fonts-hud.md): the release's two fonts
 * (font/doc0, font/alps3: atlas + glyph boxes, xblafont.c) and its HD HUD pictures by name
 * (hudRows[]: the ammo icons and the sight into tex/) - a version-10 layer is redone once. */
/* 12 (Midas round 44, docs/port-ios/62-round44-remaining-icons.md): the remaining ammo icons
 * (golden gun, the three mine types, the tank shell) join hudRows[], magnified from the
 * release's same-size copies - a version-11 layer is redone once the same way. */
#define XBLA_LAYER_VERSION 12

/* Tier C: files the importer copies byte for byte (not decoded) for
 * beanmesh.c/beanstage.c to read at a level's load. Paths are the release's
 * own under files/. C1 needed Dam's level; C5 (round 28a) every paired level's. */
static const char *const rawWanted[] = {
#include "beanstagefiles.h"
#include "beanpropfiles.h"
#include "beangunfiles.h"
#include "beanchrfiles.h"
};
#define NUM_RAW ((int)(sizeof(rawWanted) / sizeof(rawWanted[0])))

/* Midas round 43 (doc 60): the release's fonts. Each "bfont" file is decoded
 * here into $C/xbla/font/<name>.png (its atlas) and <name>.txt (the boxes of
 * '!'..'~'); xblafont.c fits them onto the ROM's glyphs at draw time. The
 * layout is Perfect Dark's gebeanFontOpen() (perfect-dark-ios vendor/dabs-mod
 * port/src/gebean.c, MIT). doc0 is Bank Gothic, alps3 Zurich (PD's
 * geFolderFont() pairs them the same way; the atlases show it). */
static const struct {
    const char *file;
    const char *name;
} fontWanted[] = {
    { "misc/doc0/default.dt", "doc0" },
    { "misc/alps3/default.dt", "alps3" },
};
#define NUM_FONTS ((int)(sizeof(fontWanted) / sizeof(fontWanted[0])))

/* Midas round 43 (doc 60, Tier C row 6): the release's HD HUD pictures, by
 * name, for the N64 textures images.def names (the N64 size beside each).
 * Each is fitted (one scale, centred) into XBLA_HUD_SCALE times the N64 size
 * and written as tex/<texnum>.png, so the HUD draws it like any HD texture.
 * Round 44 (doc 62): the rest of the ammo icons. The release has them only
 * at the N64's own size (ammoicongold, ammoiconmine{red,green,yellow},
 * ammoicontankshell; nothing larger anywhere under texture/), so those rows
 * (same = 1) are magnified to the same 8x on the CPU - xBR-lv2 2x (xbrScale),
 * then a smooth cubic 4x (cubicScale) - instead of the area fit, and drawn
 * like the HD ones beside them. Which
 * mine is which colour is the decomp's own (assets/oddtextures.c: remote
 * mine = minered, timed = mineyellow, proximity = minegreen).
 * Not here: sightcorner and circle (no N64 texture is that picture:
 * CROSSHAIR2, 2237, is a bare cross). Turned over like every other picture
 * of the layer (decodeFile): stored as is, the 9mm round stood on its tip in
 * the HUD. */
#define XBLA_HUD_SCALE 8
static const struct {
    int16_t texnum;
    uint8_t w, h;
    uint8_t same;   /* round 44: the release's copy is the N64's size: xBR, not the area fit */
    const char *file;
} hudRows[] = {
    { 2161, 7, 22, 0, "texture/bg/ammoiconrocket/default.rba" },    /* ROCKETAMMO */
    { 2162, 14, 14, 0, "texture/bg/ammoiconmine/default.rba" },     /* PLAINMINEAMMO */
    { 2163, 14, 18, 0, "texture/bg/ammogrenadehand/default.rba" },  /* GRENADEAMMO */
    { 2164, 5, 15, 0, "texture/bg/ammoiconmagnum/default.rba" },    /* MAGAMMO */
    { 2165, 8, 21, 0, "texture/bg/ammoicongrenade/default.rba" },   /* GLAMMO */
    { 2166, 6, 24, 0, "texture/bg/ammoiconknife/default.rba" },     /* KNIFEAMMO */
    { 2167, 6, 20, 0, "texture/bg/ammoiconshell/default.rba" },     /* SHOTAMMO */
    { 2231, 5, 12, 0, "texture/bg/ammoicon9mm/default.rba" },       /* 9MMAMMO */
    { 2232, 5, 28, 0, "texture/bg/ammoiconrifle/default.rba" },     /* RIFLEAMMO */
    { 2236, 32, 32, 0, "texture/bg/sight/default.rba" },            /* CROSSHAIR1 */
    { 2233, 5, 12, 1, "texture/bg/ammoicongold/default.rba" },       /* GGAMMO (round 44) */
    { 2234, 14, 14, 1, "texture/bg/ammoiconminered/default.rba" },   /* MINEAMMO, remote mine */
    { 2235, 14, 14, 1, "texture/bg/ammoiconminegreen/default.rba" }, /* PROXAMMO, proximity mine */
    { 2238, 14, 14, 1, "texture/bg/ammoiconmineyellow/default.rba" }, /* TIMEAMMO, timed mine */
    { 2464, 7, 22, 1, "texture/bg/ammoicontankshell/default.rba" },  /* TANKAMMO */
};
#define NUM_HUD ((int)(sizeof(hudRows) / sizeof(hudRows[0])))

/* fileIndexOf()'s numbering: the map's files, then the raw copies, the fonts,
 * the HUD pictures. */
#define FI_RAW0 NUM_FILES
#define FI_FONT0 (NUM_FILES + NUM_RAW)
#define FI_HUD0 (NUM_FILES + NUM_RAW + NUM_FONTS)
#define FI_END (NUM_FILES + NUM_RAW + NUM_FONTS + NUM_HUD)

#define PKG_TITLE 0x584108a9u
#define PKG_ENTRY "584108a9/000d0000/"
#define PKG_NAME "30ba92710985645ef623d4a6ba9e8effaec62617"
#define STFS_DATA_START 0xc000
#define STFS_BLOCK 0x1000
#define ARCHIVE_7Z_LZMA2 0x21
#define ARCHIVE_7Z_LZMA 0x30101
#define ARCHIVE_7Z_CHUNK (1 << 20)
#define MAXNAME 512

/* ------------------------------------------------------------------ state */

static _Atomic int state = XBLA_IMPORT_IDLE;
static _Atomic int progDone, progTotal;
/* Written by the import thread, read by the UI: copied under a lock. */
static char message[256] = "";
static char messageOut[256] = "";
#ifdef XBLA_HAVE_THREADS
static pthread_mutex_t messageLock = PTHREAD_MUTEX_INITIALIZER;
#define MSG_LOCK() pthread_mutex_lock(&messageLock)
#define MSG_UNLOCK() pthread_mutex_unlock(&messageLock)
#else
#define MSG_LOCK()
#define MSG_UNLOCK()
#endif
static int wroteHd, wroteSame, wroteRaw, wroteFonts, wroteHud;
static void rawForget(void);

static void setMessage(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void setMessage(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    MSG_LOCK();
    memcpy(message, tmp, sizeof(message));
    MSG_UNLOCK();
}

int xblaImportState(void) { return state; }
void xblaImportProgress(int *done, int *total) { *done = progDone; *total = progTotal; }
const char *xblaImportMessage(void)
{
    MSG_LOCK();
    memcpy(messageOut, message, sizeof(messageOut));
    MSG_UNLOCK();
    return messageOut;
}

int xblaMapNumHd(void)
{
    int n = 0;
    for (int i = 0; i < NUM_ROWS; i++) n += xblamapRows[i].hd;
    return n;
}

int xblaMapNumSame(void) { return NUM_ROWS - xblaMapNumHd(); }

static uint32_t rbe32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void layerDir(char *out, int len, const char *sub)
{
    snprintf(out, (size_t)len, "%s%s%s", sysResolvePath(TEXPACK_XBLA_DIR), sub ? "/" : "", sub ? sub : "");
}

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void emptyDir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        remove(p);
    }
    closedir(d);
}

/* ------------------------------------------------------------- free space */

/* Round 28d (D-202). A full import writes ~395 MB into the layer (Tier C's
 * raw files, docs/tier-c-plan.md) and the Settings importer first copies the
 * release into Documents. Running out of room mid-import leaves a half layer
 * and a phone that is full, so refuse up front. What the layer already holds
 * is credited: an import overwrites it in place. statvfs (f_bavail, what an
 * unprivileged writer may use) is conservative on iOS, which does not count
 * purgeable space; erring that way costs a "free some space" message, never a
 * full disk. */
#define XBLA_IMPORT_WRITES_MB 395ull
#define XBLA_IMPORT_MARGIN_MB 100ull

static unsigned long long dirBytes(const char *dir, int depth)
{
    unsigned long long n = 0;
    DIR *d = depth < 12 ? opendir(dir) : NULL;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1100];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) n += dirBytes(p, depth + 1);
        else if (S_ISREG(st.st_mode)) n += (unsigned long long)st.st_size;
    }
    closedir(d);
    return n;
}

int xblaImportCheckSpace(long long extraBytes, char *msg, int msglen)
{
    char d[1024];
    struct statvfs v;
    layerDir(d, sizeof(d), NULL);
    mkdirs(d);
    if (statvfs(d, &v) != 0) return 1;   /* cannot tell: do not block on it */
    unsigned long long avail = (unsigned long long)v.f_bavail * (unsigned long long)v.f_frsize;
    const char *fake = getenv("GE_XBLA_FAKEFREE_MB");   /* dev: the refusal without a full disk */
    if (fake && *fake) avail = strtoull(fake, NULL, 10) << 20;
    const unsigned long long have = dirBytes(d, 0);
    const unsigned long long writes = XBLA_IMPORT_WRITES_MB << 20;
    unsigned long long need = (writes > have ? writes - have : 0) + (XBLA_IMPORT_MARGIN_MB << 20);
    if (extraBytes > 0) need += (unsigned long long)extraBytes;
    if (avail >= need) return 1;
    snprintf(msg, (size_t)msglen,
             "Not enough free space: the import needs about %llu MB and this device has %llu MB free. "
             "Free some space and try again.", (need + (1u << 20) - 1) >> 20, avail >> 20);
    sysLogPrintf(LOG_WARNING, "xbla: import refused, %llu MB free < %llu MB needed (layer holds %llu MB, +%lld MB copy)",
                 avail >> 20, need >> 20, have >> 20, extraBytes > 0 ? extraBytes >> 20 : 0);
    return 0;
}

/* ------------------------------------------------------------------ marker */

struct marker {
    char source[256];
    long long size;
    int version, hd, same;
    double seconds;
};

static int readMarker(struct marker *m)
{
    char path[1024];
    layerDir(path, sizeof(path), "imported.txt");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(m, 0, sizeof(*m));
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "source=", 7)) snprintf(m->source, sizeof(m->source), "%s", line + 7);
        else if (!strncmp(line, "size=", 5)) m->size = atoll(line + 5);
        else if (!strncmp(line, "version=", 8)) m->version = atoi(line + 8);
        else if (!strncmp(line, "hd=", 3)) m->hd = atoi(line + 3);
        else if (!strncmp(line, "same=", 5)) m->same = atoi(line + 5);
        else if (!strncmp(line, "seconds=", 8)) m->seconds = atof(line + 8);
    }
    fclose(f);
    return m->version > 0;
}

static struct marker cached;
static int cachedValid = -1;

static const struct marker *marker(void)
{
    if (cachedValid < 0) {
        cachedValid = readMarker(&cached);
    }
    return cachedValid ? &cached : NULL;
}

int xblaImportedHd(void) { const struct marker *m = marker(); return m ? m->hd : 0; }
int xblaImportedSame(void) { const struct marker *m = marker(); return m ? m->same : 0; }
const char *xblaImportedSource(void) { const struct marker *m = marker(); return m ? m->source : ""; }
double xblaImportedSeconds(void) { const struct marker *m = marker(); return m ? m->seconds : 0.0; }

void xblaImportForget(void)
{
    char d[1024];
    layerDir(d, sizeof(d), "tex");
    emptyDir(d);
    layerDir(d, sizeof(d), "same");
    emptyDir(d);
    layerDir(d, sizeof(d), "font");
    emptyDir(d);
    layerDir(d, sizeof(d), "imported.txt");
    remove(d);
    rawForget();
    cachedValid = -1;
    texpackReload();
    beanStageLayerChanged();   /* Midas round 22 (R16-2): the level in play goes back to N64 */
}

/* ------------------------------------------------------------------ detect */

static long long fileSize(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static int isPackage(const char *path)
{
    uint8_t head[0x364];
    if (fileSize(path) < (64ll << 20)) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    const int ok = fread(head, 1, sizeof(head), fp) == sizeof(head)
        && (!memcmp(head, "LIVE", 4) || !memcmp(head, "CON ", 4) || !memcmp(head, "PIRS", 4))
        && rbe32(head + 0x360) == PKG_TITLE;
    fclose(fp);
    return ok;
}

static int nameHas(const char *name, const char *needle)
{
    const size_t n = strlen(needle);
    for (const char *p = name; *p; p++) {
        size_t i;
        for (i = 0; i < n && p[i]; i++) {
            char a = p[i] == '\\' ? '/' : p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

struct archive7z {
    CFileInStream stream;
    CLookToRead look;
    CSzArEx db;
    ISzAlloc alloc;
    ISzAlloc allocTemp;
};

static int a7zOpen(struct archive7z *a, const char *path)
{
    a->alloc.Alloc = SzAlloc;
    a->alloc.Free = SzFree;
    a->allocTemp.Alloc = SzAllocTemp;
    a->allocTemp.Free = SzFreeTemp;
    if (InFile_Open(&a->stream.file, path)) return 0;
    FileInStream_CreateVTable(&a->stream);
    LookToRead_CreateVTable(&a->look, False);
    a->look.realStream = &a->stream.s;
    LookToRead_Init(&a->look);
    CrcGenerateTable();
    SzArEx_Init(&a->db);
    if (SzArEx_Open(&a->db, &a->look.s, &a->alloc, &a->allocTemp) != SZ_OK) {
        SzArEx_Free(&a->db, &a->alloc);
        File_Close(&a->stream.file);
        return 0;
    }
    return 1;
}

static void a7zClose(struct archive7z *a)
{
    SzArEx_Free(&a->db, &a->alloc);
    File_Close(&a->stream.file);
}

static int a7zName(const CSzArEx *db, UInt32 i, char *name)
{
    UInt16 utf16[MAXNAME];
    const size_t len = SzArEx_GetFileNameUtf16(db, i, NULL);
    if (len == 0 || len > MAXNAME) return 0;
    SzArEx_GetFileNameUtf16(db, i, utf16);
    for (size_t j = 0; j + 1 < len; j++) name[j] = utf16[j] < 0x80 ? (char)utf16[j] : '_';
    name[len - 1] = 0;
    return 1;
}

/* The package's entry in a .7z: the content folder, or the package's own name. */
static int a7zFindPackage(struct archive7z *a, UInt32 *out)
{
    UInt32 pick = (UInt32)-1;
    char name[MAXNAME];
    for (UInt32 i = 0; i < a->db.db.NumFiles; i++) {
        if (a->db.db.Files[i].IsDir || a->db.FileIndexToFolderIndexMap[i] == (UInt32)-1) continue;
        if (!a7zName(&a->db, i, name)) continue;
        if ((nameHas(name, PKG_ENTRY) || nameHas(name, PKG_NAME))
                && (pick == (UInt32)-1 || a->db.db.Files[i].Size > a->db.db.Files[pick].Size)) {
            pick = i;
        }
    }
    *out = pick;
    return pick != (UInt32)-1;
}

static int isPackage7z(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".7z")) return 0;
    struct archive7z a;
    if (!a7zOpen(&a, path)) return 0;
    UInt32 i;
    const int ok = a7zFindPackage(&a, &i);
    a7zClose(&a);
    return ok;
}

static int isFolder(const char *path, char *filesDir, int len)
{
    char p[1024];
    struct stat st;
    snprintf(p, sizeof(p), "%s/files/original", path);
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (filesDir) snprintf(filesDir, (size_t)len, "%s/files", path);
        return 1;
    }
    snprintf(p, sizeof(p), "%s/original", path);
    char q[1024];
    snprintf(q, sizeof(q), "%s/new", path);
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode) && stat(q, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (filesDir) snprintf(filesDir, (size_t)len, "%s", path);
        return 1;
    }
    return 0;
}

int xblaImportKindOf(const char *path)
{
    struct stat st;
    if (!path || !*path || stat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return isFolder(path, NULL, 0) ? 3 : 0;
    if (isPackage(path)) return 1;
    if (isPackage7z(path)) return 2;
    return 0;
}

static int findIn(const char *dir, int depth, char *out, int outlen)
{
    if (xblaImportKindOf(dir) == 3) {
        snprintf(out, (size_t)outlen, "%s", dir);
        return 1;
    }
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4 && strcmp(e->d_name, "texturepacks")) found = findIn(p, depth + 1, out, outlen);
        } else if (S_ISREG(st.st_mode) && st.st_size >= (64ll << 20)) {
            const int k = xblaImportKindOf(p);
            if (k == 1 || k == 2) {
                snprintf(out, (size_t)outlen, "%s", p);
                found = 1;
            }
        }
    }
    closedir(d);
    return found;
}

int xblaImportFind(const char *dir, char *out, int outlen)
{
    return findIn(dir, 0, out, outlen);
}

/* ------------------------------------------------------------------ decode */

static int fileIndexOf(const char *rel)
{
    /* The map's paths are "original/...", the release's "files/original/..." */
    if (!strncmp(rel, "files/", 6)) rel += 6;
    for (int i = 0; i < NUM_FILES; i++) {
        if (!strcasecmp(xblamapFiles[i], rel)) return i;
    }
    for (int k = 0; k < NUM_RAW; k++) {
        if (!strcasecmp(rawWanted[k], rel)) return NUM_FILES + k;
    }
    for (int k = 0; k < NUM_FONTS; k++) {
        if (!strcasecmp(fontWanted[k].file, rel)) return FI_FONT0 + k;
    }
    for (int k = 0; k < NUM_HUD; k++) {
        if (!strcasecmp(hudRows[k].file, rel)) return FI_HUD0 + k;
    }
    return -1;
}

/* ------------------------------------------------- fonts, HUD (round 43) */

static uint16_t rbe16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int fitsIn(uint64_t ofs, uint64_t len, uint64_t size)
{
    return ofs <= size && len <= size - ofs;
}

/* A font file: its "bfont" metrics (count at +0x30, the metrics block's
 * offset at +0x50; it points at the line metrics and the glyph records, 20
 * bytes each: s8 left, u8 -, u8 width, u8 height, s16 u0 v0 u1 v1 in 32768ths
 * of the atlas (texel centres), u16 advance, s16 -, s16 top, u16 char) and
 * its one picture, the atlas. */
static void fontDecode(int k, const uint8_t *data, uint32_t len)
{
    struct beanfile *bf = beanOpen(data, len);
    if (!bf) {
        sysLogPrintf(LOG_WARNING, "xbla: %s is not a CAFF this reads", fontWanted[k].file);
        return;
    }
    uint32_t blen = 0;
    const uint8_t *b = beanFindBlob(bf, "bfont\0", 6, 0x54, &blen);
    int32_t W = 0, H = 0;
    uint8_t *rgba = b && beanNumTextures(bf) > 0 ? beanDecodeTexture(bf, 0, &W, &H) : NULL;
    const uint32_t count = b ? rbe32(b + 0x30) : 0;
    uint32_t metrics = b ? rbe32(b + 0x50) : 0;
    uint32_t glyphs = 0;
    if (b && fitsIn(metrics, 16, blen)) {
        glyphs = rbe32(b + metrics + 8);
        metrics = rbe32(b + metrics);
    }
    if (!rgba || count == 0 || count > 4096 || !fitsIn(metrics, 12, blen) || !fitsIn(glyphs, 20ull * count, blen)) {
        sysLogPrintf(LOG_WARNING, "xbla: %s has no font this reads", fontWanted[k].file);
        free(rgba);
        beanClose(bf);
        return;
    }
    char dir[1024], path[1100];
    layerDir(dir, sizeof(dir), "font");
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/%s.png", dir, fontWanted[k].name);
    int ok = pngWrite(path, rgba, W, H, 4, 0);
    snprintf(path, sizeof(path), "%s/%s.txt", dir, fontWanted[k].name);
    FILE *f = ok ? fopen(path, "w") : NULL;
    int n = 0;
    if (f) {
        fprintf(f, "# GoldenEye XBLA %s, written by the importer (xblaimport.c, round 43)\n", fontWanted[k].file);
        fprintf(f, "size %d %d\nline %d %d %d\n", W, H, (int)rbe32(b + metrics), (int)rbe32(b + metrics + 4),
                (int)rbe32(b + metrics + 8));
        fprintf(f, "# g char left top width height x0 y0 advance\n");
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *g = b + glyphs + i * 20;
            const int ch = rbe16(g + 18);
            const int w = g[2], h = g[3];
            const int x0 = (int)((int16_t)rbe16(g + 4) / 32768.0f * W);
            const int y0 = (int)((int16_t)rbe16(g + 6) / 32768.0f * H);
            if (ch < 0x21 || ch > 0x7e || w == 0 || h == 0 || x0 < 0 || y0 < 0 || x0 + w > W || y0 + h > H) {
                continue;
            }
            fprintf(f, "g %02x %d %d %d %d %d %d %d\n", ch, (int8_t)g[0], (int16_t)rbe16(g + 16), w, h, x0, y0,
                    rbe16(g + 12));
            n++;
        }
        ok = fclose(f) == 0 && n > 0;
    } else {
        ok = 0;
    }
    if (ok) {
        wroteFonts++;
    } else {
        sysLogPrintf(LOG_WARNING, "xbla: %s did not write", fontWanted[k].file);
    }
    free(rgba);
    beanClose(bf);
}

/* Round 44 (doc 62): xBR-lv2 on the CPU, the first step for the HUD pictures
 * the release has only at the N64's size. The fragment program of the round 37 2D
 * upscale (port/fast3d/gfx_opengl.cpp up_init: Hyllian's xBR-lv2, MIT,
 * port/apple/external/xbr; corner C, the shipped thresholds) run per output
 * pixel, with that port's changes: premultiplied colour, alpha in the edge
 * metric, a fully transparent result keeping its source texel's colour. The
 * source is clamped at its edges (the HUD tiles are G_TX_CLAMP). Done once,
 * at import (with cubicScale after it), so the HUD then draws an 8x picture
 * like the HD icons'. */
typedef struct { float v[4]; } xv4;

static xv4 xbrFetch(const uint8_t *src, int32_t w, int32_t h, int32_t x, int32_t y)
{
    xv4 r;
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    const uint8_t *p = src + ((size_t)y * w + x) * 4;
    const float a = p[3] / 255.0f;
    r.v[0] = p[0] / 255.0f * a;
    r.v[1] = p[1] / 255.0f * a;
    r.v[2] = p[2] / 255.0f * a;
    r.v[3] = a;
    return r;
}

static float xbrLuma(xv4 c)
{
    return c.v[0] * 14.352f + c.v[1] * 28.176f + c.v[2] * 5.472f + c.v[3] * 48.0f;
}

static xv4 xmix(xv4 a, xv4 b, float t)
{
    xv4 r;
    for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + (b.v[i] - a.v[i]) * t;
    return r;
}

#define XBR_EQ_THRESHOLD 15.0f
#define XBR_LV2_CF 2.0f
static float xdf(float a, float b) { return fabsf(a - b); }
static float xeq(float a, float b) { return xdf(a, b) <= XBR_EQ_THRESHOLD ? 1.0f : 0.0f; }
static float xneq(float a, float b) { return 1.0f - xeq(a, b); }
static float xdiff(float a, float b) { return a != b ? 1.0f : 0.0f; }
static float xclamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float xstep(float edge, float v) { return v < edge ? 0.0f : 1.0f; }
static float xcdf(xv4 a, xv4 b)
{
    return fabsf(a.v[0] - b.v[0]) + fabsf(a.v[1] - b.v[1]) + fabsf(a.v[2] - b.v[2]) + fabsf(a.v[3] - b.v[3]);
}

/* w x h RGBA (straight alpha) -> (w*n) x (h*n) RGBA, malloc'd; NULL on no memory */
static uint8_t *xbrScale(const uint8_t *src, int32_t w, int32_t h, int32_t n)
{
    const int32_t W = w * n, H = h * n;
    uint8_t *out = malloc((size_t)W * H * 4);
    if (!out) return NULL;
    static const float Ao[4] = { 1.0f, -1.0f, -1.0f, 1.0f }, Bo[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
    static const float Co[4] = { 1.5f, 0.5f, -0.5f, 0.5f };
    static const float Ax[4] = { 1.0f, -1.0f, -1.0f, 1.0f }, Bx[4] = { 0.5f, 2.0f, -0.5f, -2.0f };
    static const float Cx[4] = { 1.0f, 1.0f, -0.5f, 0.0f };
    static const float Ay[4] = { 1.0f, -1.0f, -1.0f, 1.0f }, By[4] = { 2.0f, 0.5f, -2.0f, -0.5f };
    static const float Cy[4] = { 2.0f, 0.0f, -1.0f, 0.5f };
    const float Ci = 0.25f;
    const float sc = (float)n;
    for (int32_t Y = 0; Y < H; Y++) {
        for (int32_t X = 0; X < W; X++) {
            const float spx = (X + 0.5f) / sc, spy = (Y + 0.5f) / sc;
            const int32_t ox = (int32_t)floorf(spx), oy = (int32_t)floorf(spy);
            const float fpx = spx - ox, fpy = spy - oy;
#define XF(dx, dy) xbrFetch(src, w, h, ox + (dx), oy + (dy))
            const xv4 A1 = XF(-1, -2), B1 = XF(0, -2), C1 = XF(1, -2);
            const xv4 A = XF(-1, -1), B = XF(0, -1), C = XF(1, -1);
            const xv4 D = XF(-1, 0), E = XF(0, 0), F = XF(1, 0);
            const xv4 G = XF(-1, 1), Hh = XF(0, 1), I = XF(1, 1);
            const xv4 G5 = XF(-1, 2), H5 = XF(0, 2), I5 = XF(1, 2);
            const xv4 A0 = XF(-2, -1), D0 = XF(-2, 0), G0 = XF(-2, 1);
            const xv4 C4 = XF(2, -1), F4 = XF(2, 0), I4 = XF(2, 1);
#undef XF
            /* the shader's swizzled luma vectors */
            const float b[4] = { xbrLuma(B), xbrLuma(D), xbrLuma(Hh), xbrLuma(F) };
            const float c[4] = { xbrLuma(C), xbrLuma(A), xbrLuma(G), xbrLuma(I) };
            const float d[4] = { b[1], b[2], b[3], b[0] };          /* b.yzwx */
            const float e = xbrLuma(E);
            const float f[4] = { b[3], b[0], b[1], b[2] };          /* b.wxyz */
            const float g[4] = { c[2], c[3], c[0], c[1] };          /* c.zwxy */
            const float hh[4] = { b[2], b[3], b[0], b[1] };         /* b.zwxy */
            const float i[4] = { c[3], c[0], c[1], c[2] };          /* c.wxyz */
            const float i4[4] = { xbrLuma(I4), xbrLuma(C1), xbrLuma(A0), xbrLuma(G5) };
            const float i5[4] = { xbrLuma(I5), xbrLuma(C4), xbrLuma(A1), xbrLuma(G0) };
            const float h5[4] = { xbrLuma(H5), xbrLuma(F4), xbrLuma(B1), xbrLuma(D0) };
            const float f4[4] = { h5[1], h5[2], h5[3], h5[0] };     /* h5.yzwx */
            const float delta = 1.0f / sc;
            const float delta_l[4] = { 0.5f / sc, 1.0f / sc, 0.5f / sc, 1.0f / sc };
            const float delta_u[4] = { delta_l[1], delta_l[0], delta_l[3], delta_l[2] };
            float fx45i[4], fx45[4], fx30[4], fx60[4], px[4], maximos[4];
            for (int k = 0; k < 4; k++) {
                const float fx = Ao[k] * fpy + Bo[k] * fpx;
                const float fx_l = Ax[k] * fpy + Bx[k] * fpx;
                const float fx_u = Ay[k] * fpy + By[k] * fpx;
                const float irlv0 = xdiff(e, f[k]) * xdiff(e, hh[k]);
                const float irlv1 = irlv0 * (xneq(f[k], b[k]) * xneq(f[k], c[k]) + xneq(hh[k], d[k]) * xneq(hh[k], g[k])
                                             + xeq(e, i[k]) * (xneq(f[k], f4[k]) * xneq(f[k], i4[k])
                                                               + xneq(hh[k], h5[k]) * xneq(hh[k], i5[k]))
                                             + xeq(e, g[k]) + xeq(e, c[k]));
                const float irlv2l = xdiff(e, g[k]) * xdiff(d[k], g[k]);
                const float irlv2u = xdiff(e, c[k]) * xdiff(b[k], c[k]);
                fx45i[k] = xclamp01((fx + delta - Co[k] - Ci) / (2.0f * delta));
                fx45[k] = xclamp01((fx + delta - Co[k]) / (2.0f * delta));
                fx30[k] = xclamp01((fx_l + delta_l[k] - Cx[k]) / (2.0f * delta_l[k]));
                fx60[k] = xclamp01((fx_u + delta_u[k] - Cy[k]) / (2.0f * delta_u[k]));
                /* wd(e, c, g, i, h5, f4, h, f) and wd(h, d, i5, f, i4, b, e, i) */
                const float wd1 = xdf(e, c[k]) + xdf(e, g[k]) + xdf(i[k], h5[k]) + xdf(i[k], f4[k]) + 4.0f * xdf(hh[k], f[k]);
                const float wd2 = xdf(hh[k], d[k]) + xdf(hh[k], i5[k]) + xdf(f[k], i4[k]) + xdf(f[k], b[k]) + 4.0f * xdf(e, i[k]);
                const float edri = xstep(wd1, wd2) * irlv0;
                const float edr = xstep(wd1 + 0.1f, wd2) * xstep(0.5f, irlv1);
                const float edr_l = xstep(XBR_LV2_CF * xdf(f[k], g[k]), xdf(hh[k], c[k])) * irlv2l * edr;
                const float edr_u = xstep(XBR_LV2_CF * xdf(hh[k], c[k]), xdf(f[k], g[k])) * irlv2u * edr;
                fx45[k] *= edr;
                fx30[k] *= edr_l;
                fx60[k] *= edr_u;
                fx45i[k] *= edri;
                px[k] = xstep(xdf(e, f[k]), xdf(e, hh[k]));
                float m = fx30[k] > fx60[k] ? fx30[k] : fx60[k];
                m = m > fx45[k] ? m : fx45[k];
                maximos[k] = m > fx45i[k] ? m : fx45i[k];
            }
            xv4 res1 = E, res2 = E;
            res1 = xmix(res1, xmix(Hh, F, px[0]), maximos[0]);
            res1 = xmix(res1, xmix(B, D, px[2]), maximos[2]);
            res2 = xmix(res2, xmix(F, B, px[1]), maximos[1]);
            res2 = xmix(res2, xmix(D, Hh, px[3]), maximos[3]);
            const xv4 res = xcdf(E, res1) <= xcdf(E, res2) ? res2 : res1;   /* mix(res1, res2, step(c_df(E,res1), c_df(E,res2))) */
            uint8_t *q = out + ((size_t)Y * W + X) * 4;
            if (res.v[3] <= 0.5f / 255.0f) {
                int32_t cx = ox < 0 ? 0 : ox >= w ? w - 1 : ox, cy = oy < 0 ? 0 : oy >= h ? h - 1 : oy;
                const uint8_t *raw = src + ((size_t)cy * w + cx) * 4;
                q[0] = raw[0];
                q[1] = raw[1];
                q[2] = raw[2];
                q[3] = 0;
            } else {
                for (int k = 0; k < 3; k++) q[k] = (uint8_t)(xclamp01(res.v[k] / res.v[3]) * 255.0f + 0.5f);
                q[3] = (uint8_t)(xclamp01(res.v[3]) * 255.0f + 0.5f);
            }
        }
    }
    return out;
}

/* Round 44: the second step for the same-size HUD pictures - a smooth
 * magnification of xBR's 2x by n, Keys' cubic (a = -0.5, Catmull-Rom),
 * separable, on premultiplied colour, centre-aligned, edges clamped, the
 * result clamped to [0, alpha] so the cubic's overshoot adds no fringe. */
static float cubicW(float x)
{
    x = fabsf(x);
    if (x < 1.0f) return (1.5f * x - 2.5f) * x * x + 1.0f;
    if (x < 2.0f) return ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f;
    return 0.0f;
}

static uint8_t *cubicScale(const uint8_t *src, int32_t w, int32_t h, int32_t n)
{
    const int32_t W = w * n, H = h * n;
    float *pm = malloc((size_t)w * h * 4 * sizeof(float));
    float *mid = malloc((size_t)W * h * 4 * sizeof(float));
    uint8_t *out = malloc((size_t)W * H * 4);
    if (!pm || !mid || !out) {
        free(pm);
        free(mid);
        free(out);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const float a = src[i * 4 + 3] / 255.0f;
        pm[i * 4 + 0] = src[i * 4 + 0] * a;
        pm[i * 4 + 1] = src[i * 4 + 1] * a;
        pm[i * 4 + 2] = src[i * 4 + 2] * a;
        pm[i * 4 + 3] = (float)src[i * 4 + 3];
    }
    for (int32_t y = 0; y < h; y++) {          /* across */
        for (int32_t X = 0; X < W; X++) {
            const float sx = (X + 0.5f) / n - 0.5f;
            const int32_t x0 = (int32_t)floorf(sx);
            float acc[4] = { 0, 0, 0, 0 };
            for (int32_t t = -1; t <= 2; t++) {
                const float wt = cubicW(sx - (x0 + t));
                const int32_t xi = x0 + t < 0 ? 0 : x0 + t >= w ? w - 1 : x0 + t;
                for (int c = 0; c < 4; c++) acc[c] += wt * pm[((size_t)y * w + xi) * 4 + c];
            }
            for (int c = 0; c < 4; c++) mid[((size_t)y * W + X) * 4 + c] = acc[c];
        }
    }
    for (int32_t Y = 0; Y < H; Y++) {          /* down */
        const float sy = (Y + 0.5f) / n - 0.5f;
        const int32_t y0 = (int32_t)floorf(sy);
        for (int32_t X = 0; X < W; X++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int32_t t = -1; t <= 2; t++) {
                const float wt = cubicW(sy - (y0 + t));
                const int32_t yi = y0 + t < 0 ? 0 : y0 + t >= h ? h - 1 : y0 + t;
                for (int c = 0; c < 4; c++) acc[c] += wt * mid[((size_t)yi * W + X) * 4 + c];
            }
            uint8_t *q = out + ((size_t)Y * W + X) * 4;
            const float a = acc[3] < 0.0f ? 0.0f : acc[3] > 255.0f ? 255.0f : acc[3];
            q[3] = (uint8_t)(a + 0.5f);
            for (int c = 0; c < 3; c++) {
                float v = a > 0.5f ? acc[c] / (a / 255.0f) : 0.0f;
                v = v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v;
                q[c] = (uint8_t)(v + 0.5f);
            }
        }
    }
    free(pm);
    free(mid);
    return out;
}

/* A HUD picture into tex/<texnum>.png, turned over like every other picture
 * of the layer (decodeFile). */
static void hudWrite(int k, uint8_t *out, int32_t W, int32_t H)
{
    const size_t row = (size_t)W * 4;
    uint8_t *tmp = malloc(row);
    for (int32_t y = 0; tmp && y < H / 2; y++) {
        memcpy(tmp, out + (size_t)y * row, row);
        memcpy(out + (size_t)y * row, out + (size_t)(H - 1 - y) * row, row);
        memcpy(out + (size_t)(H - 1 - y) * row, tmp, row);
    }
    free(tmp);
    char dir[1024], path[1100];
    layerDir(dir, sizeof(dir), "tex");
    snprintf(path, sizeof(path), "%s/%04x.png", dir, (unsigned)hudRows[k].texnum);
    if (pngWrite(path, out, W, H, 4, 0)) {
        wroteHud++;
    }
}

/* An HD HUD picture: fitted, one scale both ways and centred, into the N64
 * texture's shape at XBLA_HUD_SCALE times its size, by area (premultiplied,
 * so a transparent texel lends no colour to the edge). */
static void hudDecode(int k, const uint8_t *data, uint32_t len)
{
    struct beanfile *bf = beanOpen(data, len);
    int32_t sw = 0, sh = 0;
    uint8_t *src = bf && beanNumTextures(bf) > 0 ? beanDecodeTexture(bf, 0, &sw, &sh) : NULL;
    beanClose(bf);
    if (!src || sw <= 0 || sh <= 0) {
        sysLogPrintf(LOG_WARNING, "xbla: %s did not decode", hudRows[k].file);
        free(src);
        return;
    }
    const int32_t W = hudRows[k].w * XBLA_HUD_SCALE, H = hudRows[k].h * XBLA_HUD_SCALE;
    if (hudRows[k].same && sw == hudRows[k].w && sh == hudRows[k].h) {
        /* round 44: the release's copy is the N64's own size - xBR 2x (the
         * HUD's own icon upscale, its edges and diagonals), then a smooth
         * cubic 4x to the HD icons' 8x (doc 62 §2: xBR straight to 8x paints
         * swirls into the mines and an X into the shells' bands) */
        uint8_t *x2 = xbrScale(src, sw, sh, 2);
        uint8_t *big = x2 ? cubicScale(x2, sw * 2, sh * 2, XBLA_HUD_SCALE / 2) : NULL;
        free(x2);
        free(src);
        if (big) {
            hudWrite(k, big, W, H);
            free(big);
        }
        return;
    }
    if (hudRows[k].same) {
        sysLogPrintf(LOG_WARNING, "xbla: %s is %dx%d, not the N64's %dx%d: fitted by area", hudRows[k].file, sw, sh,
                     hudRows[k].w, hudRows[k].h);
    }
    const float sc = (float)W / sw < (float)H / sh ? (float)W / sw : (float)H / sh;
    const float dw = sw * sc, dh = sh * sc;
    const float ox = (W - dw) * 0.5f, oy = (H - dh) * 0.5f;
    uint8_t *out = calloc((size_t)W * H, 4);
    if (!out) {
        free(src);
        return;
    }
    for (int32_t y = 0; y < H; y++) {
        for (int32_t x = 0; x < W; x++) {
            /* the source rectangle under this pixel, never under a texel wide */
            float x0 = (x - ox) / sc, x1 = (x + 1 - ox) / sc;
            float y0 = (y - oy) / sc, y1 = (y + 1 - oy) / sc;
            if (x1 - x0 < 1) { const float m = (x0 + x1) * 0.5f; x0 = m - 0.5f; x1 = m + 0.5f; }
            if (y1 - y0 < 1) { const float m = (y0 + y1) * 0.5f; y0 = m - 0.5f; y1 = m + 0.5f; }
            float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
            for (int32_t iy = (int32_t)floorf(y0); iy <= (int32_t)floorf(y1 - 0.0001f); iy++) {
                const float ry = ((iy + 1 < y1) ? iy + 1 : y1) - ((iy > y0) ? iy : y0);
                if (ry <= 0) continue;
                for (int32_t ix = (int32_t)floorf(x0); ix <= (int32_t)floorf(x1 - 0.0001f); ix++) {
                    const float rx = ((ix + 1 < x1) ? ix + 1 : x1) - ((ix > x0) ? ix : x0);
                    if (rx <= 0) continue;
                    wsum += rx * ry;   /* outside the picture counts as transparent */
                    if (ix < 0 || iy < 0 || ix >= sw || iy >= sh) continue;
                    const uint8_t *p = src + ((size_t)iy * sw + ix) * 4;
                    const float a = p[3] * rx * ry;
                    acc[0] += p[0] * a;
                    acc[1] += p[1] * a;
                    acc[2] += p[2] * a;
                    acc[3] += a;
                }
            }
            uint8_t *q = out + ((size_t)y * W + x) * 4;
            if (acc[3] > 0 && wsum > 0) {
                q[0] = (uint8_t)(acc[0] / acc[3] + 0.5f);
                q[1] = (uint8_t)(acc[1] / acc[3] + 0.5f);
                q[2] = (uint8_t)(acc[2] / acc[3] + 0.5f);
                q[3] = (uint8_t)(acc[3] / wsum + 0.5f);
            }
        }
    }
    free(src);
    hudWrite(k, out, W, H);
    free(out);
}

/* ------------------------------------------------------ raw (Tier C) */

/* $C/xbla/files/<rel>: written as <path>.part and renamed when whole, so a
 * reader never sees half a level. */
static void rawPath(int k, char *out, int len, int part)
{
    char sub[600];
    snprintf(sub, sizeof(sub), "files/%s%s", rawWanted[k], part ? ".part" : "");
    layerDir(out, len, sub);
}

static FILE *rawOpen(int k)
{
    char p[1100];
    rawPath(k, p, sizeof(p), 1);
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s", p);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    return fopen(p, "wb");
}

static void rawClose(int k, FILE *f, int ok)
{
    char part[1100], fin[1100];
    rawPath(k, part, sizeof(part), 1);
    rawPath(k, fin, sizeof(fin), 0);
    if (f && fclose(f) != 0) ok = 0;
    if (ok && rename(part, fin) == 0) {
        wroteRaw++;
    } else {
        remove(part);
        sysLogPrintf(LOG_WARNING, "xbla: %s did not write", rawWanted[k]);
    }
}

static void rawWrite(int k, const uint8_t *data, uint32_t len)
{
    FILE *f = rawOpen(k);
    rawClose(k, f, f && fwrite(data, 1, len, f) == len);
}

static void rawForget(void)
{
    for (int k = 0; k < NUM_RAW; k++) {
        char p[1100];
        rawPath(k, p, sizeof(p), 0);
        remove(p);
    }
}

static void decodeFile(int fi, const uint8_t *data, uint32_t len)
{
    if (fi >= FI_HUD0) {
        hudDecode(fi - FI_HUD0, data, len);
        return;
    }
    if (fi >= FI_FONT0) {
        fontDecode(fi - FI_FONT0, data, len);
        return;
    }
    if (fi >= NUM_FILES) {
        rawWrite(fi - NUM_FILES, data, len);
        return;
    }
    /* a file the texture map decodes that Tier C also reads whole (two props:
     * the CCTV, the key analyser case): fileIndexOf() names the decode, so the
     * copy is written here too */
    for (int k = 0; k < NUM_RAW; k++) {
        if (!strcasecmp(rawWanted[k], xblamapFiles[fi])) {
            rawWrite(k, data, len);
            break;
        }
    }
    /* round 44: a HUD picture whose file the map also names (the mines' and the
     * tank shell's same-size copies): fileIndexOf() names the map's decode, so
     * the HUD picture is made here too */
    for (int k = 0; k < NUM_HUD; k++) {
        if (!strcasecmp(hudRows[k].file, xblamapFiles[fi])) {
            hudDecode(k, data, len);
        }
    }
    struct beanfile *bf = beanOpen(data, len);
    if (!bf) {
        sysLogPrintf(LOG_WARNING, "xbla: %s is not a CAFF this reads", xblamapFiles[fi]);
        return;
    }
    char tex[1024], same[1024];
    layerDir(tex, sizeof(tex), "tex");
    layerDir(same, sizeof(same), "same");
    for (int r = 0; r < NUM_ROWS; r++) {
        if (xblamapRows[r].file != fi) continue;
        const int t = beanFindTexture(bf, xblamapRows[r].pic);
        int32_t w, h;
        uint8_t *rgba = t >= 0 ? beanDecodeTexture(bf, t, &w, &h) : NULL;
        if (!rgba) {
            sysLogPrintf(LOG_WARNING, "xbla: %s %s did not decode", xblamapFiles[fi], xblamapRows[r].pic);
            continue;
        }
        /* The release stores rows the other way up from the order the port
         * uploads an N64 texture in (measured: Dam frame 900 with the
         * same-size layer, mean |diff| 20.1 as stored, 3.7 turned over;
         * PD's beanDecodeTexture() turns them over too). */
        {
            const size_t row = (size_t)w * 4;
            uint8_t *tmp = malloc(row);
            for (int32_t y = 0; tmp && y < h / 2; y++) {
                memcpy(tmp, rgba + (size_t)y * row, row);
                memcpy(rgba + (size_t)y * row, rgba + (size_t)(h - 1 - y) * row, row);
                memcpy(rgba + (size_t)(h - 1 - y) * row, tmp, row);
            }
            free(tmp);
        }
        char path[1100];
        snprintf(path, sizeof(path), "%s/%04x.png", xblamapRows[r].hd ? tex : same, (unsigned)xblamapRows[r].texnum);
        if (pngWrite(path, rgba, w, h, 4, 0)) {
            if (xblamapRows[r].hd) wroteHd++; else wroteSame++;
        }
        free(rgba);
    }
    beanClose(bf);
}

/* ---------------------------------------------------------- folder / package */

static int importFolder(const char *filesDir)
{
    progTotal = NUM_FILES;
    for (int fi = 0; fi < NUM_FILES; fi++) {
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", filesDir, xblamapFiles[fi]);
        FILE *f = fopen(p, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            rewind(f);
            uint8_t *d = n > 0 && n < (256L << 20) ? malloc((size_t)n) : NULL;
            if (d && fread(d, 1, (size_t)n, f) == (size_t)n) {
                decodeFile(fi, d, (uint32_t)n);
            }
            free(d);
            fclose(f);
        } else {
            sysLogPrintf(LOG_WARNING, "xbla: %s is missing", p);
        }
        progDone = fi + 1;
    }
    /* Tier C: the level and prop files, copied through a buffer (not held whole) */
    progTotal = NUM_FILES + NUM_RAW;
    for (int k = 0; k < NUM_RAW; k++) {
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", filesDir, rawWanted[k]);
        FILE *in = fopen(p, "rb");
        if (!in) {
            sysLogPrintf(LOG_WARNING, "xbla: %s is missing", p);
            continue;
        }
        FILE *out = rawOpen(k);
        int ok = out != NULL;
        uint8_t *buf = malloc(1 << 20);
        size_t n;
        ok = ok && buf;
        while (ok && (n = fread(buf, 1, 1 << 20, in)) > 0) {
            ok = fwrite(buf, 1, n, out) == n;
        }
        free(buf);
        fclose(in);
        rawClose(k, out, ok);
        progDone = NUM_FILES + k + 1;
    }
    /* round 43: the fonts and the HUD pictures, decoded like the map's */
    progTotal = FI_END;
    for (int fi = FI_FONT0; fi < FI_END; fi++) {
        const char *rel = fi < FI_HUD0 ? fontWanted[fi - FI_FONT0].file : hudRows[fi - FI_HUD0].file;
        if (fileIndexOf(rel) != fi) {
            progDone = fi + 1;
            continue;   /* round 44: a map file too - its HUD picture was made with the map's decode */
        }
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", filesDir, rel);
        FILE *f = fopen(p, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            rewind(f);
            uint8_t *d = n > 0 && n < (64L << 20) ? malloc((size_t)n) : NULL;
            if (d && fread(d, 1, (size_t)n, f) == (size_t)n) {
                decodeFile(fi, d, (uint32_t)n);
            }
            free(d);
            fclose(f);
        } else {
            sysLogPrintf(LOG_WARNING, "xbla: %s is missing", p);
        }
        progDone = fi + 1;
    }
    return 1;
}

static int importPackage(const char *path)
{
    struct x360stfs stfs;
    char name[MAXNAME];
    if (!x360StfsOpen(&stfs, path)) {
        setMessage("The package could not be read.");
        return 0;
    }
    progTotal = FI_END;
    for (uint32_t i = 0; i < stfs.numFiles; i++) {
        if (stfs.files[i].isDir) continue;
        const int fi = fileIndexOf(x360StfsPath(&stfs, i, name, sizeof(name)));
        if (fi < 0) continue;
        uint32_t len;
        uint8_t *d = x360StfsRead(&stfs, i, &len);
        if (d) {
            decodeFile(fi, d, len);
            free(d);
        }
        progDone++;
    }
    x360StfsClose(&stfs);
    return 1;
}

/* ------------------------------------------------------------------- .7z */

struct pkgfile {
    int fi;
    uint32_t size;
    uint32_t got;
    uint8_t *buf;
    FILE *out; /* Tier C raw file: written through as its blocks pass */
};

struct pkgblock {
    unsigned long long ofs;
    uint32_t len;
    uint32_t file;
    uint32_t fileOfs;
};

struct pkgsink {
    unsigned long long pos, size, tableEnd;
    uint8_t head[STFS_BLOCK];
    uint32_t headLen, tableShift;
    char spool[1100];
    FILE *spoolFp;
    int spooling;
    struct pkgfile *files;
    uint32_t numFiles;
    struct pkgblock *blocks;
    uint32_t numBlocks, next;
    int failed;
};

static unsigned long long pkgBlockOffset(uint32_t tableShift, uint32_t block)
{
    /* x360.c stfsBackingBlock(): a hash table before every 0xaa blocks, and a
     * second level's before every 0x70e4 */
    uint32_t adjust = 0;
    if (block >= 0xaa) adjust += ((block / 0xaa) + 1) << tableShift;
    if (block >= 0x70e4) adjust += ((block / 0x70e4) + 1) << tableShift;
    return STFS_DATA_START + (unsigned long long)(block + adjust) * STFS_BLOCK;
}

static int cmpBlocks(const void *a, const void *b)
{
    const struct pkgblock *x = a, *y = b;
    return x->ofs < y->ofs ? -1 : x->ofs > y->ofs;
}

static void pkgFeed(struct pkgsink *s, unsigned long long ofs, const uint8_t *data, uint32_t len)
{
    const unsigned long long end = ofs + len;
    while (!s->failed && s->next < s->numBlocks && s->blocks[s->next].ofs < end) {
        struct pkgblock *b = &s->blocks[s->next];
        struct pkgfile *f = &s->files[b->file];
        const unsigned long long from = b->ofs > ofs ? b->ofs : ofs;
        const unsigned long long to = b->ofs + b->len < end ? b->ofs + b->len : end;
        const int raw = f->fi >= NUM_FILES && f->fi < FI_FONT0;   /* round 43: fonts / HUD are decoded */
        if (raw && !f->out && f->got == 0) {
            /* write-through: a 15 MB level never doubles in RAM */
            f->out = rawOpen(f->fi - NUM_FILES);
            if (!f->out) { s->failed = 1; break; }
        }
        if (!raw && !f->buf) {
            f->buf = malloc(f->size ? f->size : 1);
            if (!f->buf) { s->failed = 1; break; }
        }
        if (from < to) {
            const uint32_t n = (uint32_t)(to - from);
            const uint32_t at = b->fileOfs + (uint32_t)(from - b->ofs);
            if (raw) {
                if (fseek(f->out, (long)at, SEEK_SET) != 0 || fwrite(data + (from - ofs), 1, n, f->out) != n) {
                    s->failed = 1;
                    break;
                }
            } else {
                memcpy(f->buf + at, data + (from - ofs), n);
            }
            f->got += n;
        }
        if (b->ofs + b->len > end) break;
        s->next++;
        if (f->got == f->size) {
            if (raw) {
                rawClose(f->fi - NUM_FILES, f->out, 1);
                f->out = NULL;
            } else {
                decodeFile(f->fi, f->buf, f->size);
                free(f->buf);
                f->buf = NULL;
            }
            progDone++;
        }
    }
}

/* The file table is in: which blocks belong to the files the map names. */
static int pkgPlan(struct pkgsink *s)
{
    struct x360stfs stfs;
    char name[MAXNAME];
    fflush(s->spoolFp);
    if (!x360StfsOpen(&stfs, s->spool)) {
        setMessage("The package's file table did not read.");
        s->failed = 1;
        return 0;
    }
    s->files = calloc(stfs.numFiles ? stfs.numFiles : 1, sizeof(*s->files));
    uint64_t nb = 0, totalBytes = 0;
    for (uint32_t i = 0; s->files && i < stfs.numFiles; i++) {
        const struct x360stfsfile *f = &stfs.files[i];
        if (f->isDir) continue;
        const int fi = fileIndexOf(x360StfsPath(&stfs, i, name, sizeof(name)));
        if (fi < 0) continue;
        /* Bounded by the package (PD's D-080): a table that says otherwise is
         * not trusted with an allocation. */
        /* f->size is the table's own word, so it is first held to things it
         * does not choose: a ceiling per file, the length of the stream being
         * decoded, and (summed) that length again (round 19c review). */
        totalBytes += f->size;
        if (f->size > X360_STFS_MAX_FILE || (uint64_t)f->size > s->size || totalBytes > s->size
                || pkgBlockOffset(s->tableShift, f->startBlock + (f->size ? (f->size - 1) / STFS_BLOCK : 0))
                + STFS_BLOCK > s->size || !f->consecutive) {
            setMessage("The package's file table does not fit the package.");
            s->failed = 1;
            break;
        }
        nb += ((uint64_t)f->size + STFS_BLOCK - 1) / STFS_BLOCK;
        s->files[s->numFiles].fi = fi;
        s->files[s->numFiles].size = f->size;
        s->numFiles++;
    }
    if (!s->files || s->failed || nb > (1u << 24)) {
        x360StfsClose(&stfs);
        s->failed = 1;
        return 0;
    }
    s->blocks = malloc((size_t)(nb ? nb : 1) * sizeof(*s->blocks));
    if (!s->blocks) {
        x360StfsClose(&stfs);
        s->failed = 1;
        return 0;
    }
    for (uint32_t i = 0, w = 0; i < stfs.numFiles && w < s->numFiles; i++) {
        const struct x360stfsfile *f = &stfs.files[i];
        if (f->isDir || fileIndexOf(x360StfsPath(&stfs, i, name, sizeof(name))) != s->files[w].fi) continue;
        for (uint32_t b = 0; (uint64_t)b * STFS_BLOCK < f->size; b++) {
            struct pkgblock *blk = &s->blocks[s->numBlocks++];
            const uint32_t at = b * STFS_BLOCK;
            blk->ofs = pkgBlockOffset(s->tableShift, f->startBlock + b);
            blk->len = f->size - at < STFS_BLOCK ? f->size - at : STFS_BLOCK;
            blk->file = w;
            blk->fileOfs = at;
        }
        w++;
    }
    x360StfsClose(&stfs);
    qsort(s->blocks, s->numBlocks, sizeof(*s->blocks), cmpBlocks);
    progTotal = (int)s->numFiles;

    /* what went by while the table was read in */
    uint8_t *buf = malloc(1 << 20);
    if (!buf) { s->failed = 1; return 0; }
    for (unsigned long long at = 0; at < s->pos && !s->failed;) {
        const uint32_t n = s->pos - at < (1u << 20) ? (uint32_t)(s->pos - at) : (1u << 20);
        if (fseek(s->spoolFp, (long)at, SEEK_SET) != 0 || fread(buf, 1, n, s->spoolFp) != n) {
            s->failed = 1;
            break;
        }
        pkgFeed(s, at, buf, n);
        at += n;
    }
    free(buf);
    fclose(s->spoolFp);
    s->spoolFp = NULL;
    remove(s->spool);
    s->spooling = 0;
    return !s->failed;
}

static int pkgSink(const uint8_t *data, uint32_t len, struct pkgsink *s)
{
    const unsigned long long at = s->pos;
    s->pos += len;
    if (!s->spooling) {
        pkgFeed(s, at, data, len);
        return !s->failed;
    }
    if (fwrite(data, 1, len, s->spoolFp) != len) {
        s->failed = 1;
        return 0;
    }
    if (s->headLen < STFS_BLOCK) {
        const uint32_t n = STFS_BLOCK - s->headLen < len ? STFS_BLOCK - s->headLen : len;
        memcpy(s->head + s->headLen, data, n);
        s->headLen += n;
        if (s->headLen == STFS_BLOCK) {
            const uint8_t *h = s->head;
            const uint32_t headerSize = rbe32(h + 0x340);
            const uint32_t tableBlocks = h[0x37c] | ((uint32_t)h[0x37d] << 8);
            const uint32_t tableBlock = h[0x37e] | ((uint32_t)h[0x37f] << 8) | ((uint32_t)h[0x380] << 16);
            if ((memcmp(h, "LIVE", 4) && memcmp(h, "CON ", 4) && memcmp(h, "PIRS", 4))
                    || rbe32(h + 0x360) != PKG_TITLE || tableBlocks == 0 || tableBlocks > 0x400) {
                setMessage("The archive does not hold GoldenEye XBLA's package.");
                s->failed = 1;
                return 0;
            }
            s->tableShift = (((headerSize + 0xfff) & 0xf000) >> 12) == 0xb ? 0 : 1;
            s->tableEnd = pkgBlockOffset(s->tableShift, tableBlock + tableBlocks - 1) + STFS_BLOCK;
            if (s->tableEnd > s->size) {
                setMessage("The package's file table lies past its end.");
                s->failed = 1;
                return 0;
            }
        }
    }
    if (s->tableEnd && s->pos >= s->tableEnd) {
        return pkgPlan(s);
    }
    return 1;
}

static int import7z(const char *path)
{
    struct archive7z a;
    struct pkgsink s;
    UInt32 pick;
    int result = 0;

    if (!a7zOpen(&a, path)) {
        setMessage("The .7z could not be read.");
        return 0;
    }
    if (!a7zFindPackage(&a, &pick)) {
        a7zClose(&a);
        setMessage("The .7z does not hold GoldenEye XBLA's package.");
        return 0;
    }
    const CSzArEx *db = &a.db;
    const UInt32 fi = db->FileIndexToFolderIndexMap[pick];
    CSzFolder *folder = db->db.Folders + fi;
    if (folder->NumCoders != 1 || folder->NumPackStreams != 1 || folder->NumBindPairs != 0) {
        a7zClose(&a);
        setMessage("The .7z is packed in a way this cannot stream; repack it as LZMA/LZMA2.");
        return 0;
    }
    const CSzCoderInfo *coder = &folder->Coders[0];
    int islzma2;
    if (coder->MethodID == ARCHIVE_7Z_LZMA2 && coder->Props.size == 1) islzma2 = 1;
    else if (coder->MethodID == ARCHIVE_7Z_LZMA && coder->Props.size == 5) islzma2 = 0;
    else {
        a7zClose(&a);
        setMessage("The .7z uses a compression method this cannot stream; repack it as LZMA/LZMA2.");
        return 0;
    }

    UInt64 skip = 0;
    for (UInt32 i = db->FolderStartFileIndex[fi]; i < db->db.NumFiles && i != pick; i++) {
        if (db->FileIndexToFolderIndexMap[i] == fi) skip += db->db.Files[i].Size;
    }
    memset(&s, 0, sizeof(s));
    s.size = db->db.Files[pick].Size;
    s.spooling = 1;
    layerDir(s.spool, sizeof(s.spool), ".head.part");
    s.spoolFp = fopen(s.spool, "w+b");
    if (!s.spoolFp) {
        a7zClose(&a);
        setMessage("Could not write to the cache.");
        return 0;
    }

    UInt64 left = s.size;
    UInt64 packLeft = db->db.PackSizes[db->FolderStartPackStreamIndex[fi]];
    UInt64 unpackLeft = SzFolder_GetUnpackSize(folder);
    Byte *chunk = malloc(ARCHIVE_7Z_CHUNK);
    CLzma2Dec lzma2;
    CLzmaDec lzma;
    int decOk = chunk && LookInStream_SeekTo(&a.look.s, SzArEx_GetFolderStreamPos(db, fi, 0)) == SZ_OK;
    if (decOk) {
        if (islzma2) {
            Lzma2Dec_Construct(&lzma2);
            decOk = Lzma2Dec_Allocate(&lzma2, coder->Props.data[0], &a.alloc) == SZ_OK;
            if (decOk) Lzma2Dec_Init(&lzma2);
        } else {
            LzmaDec_Construct(&lzma);
            decOk = LzmaDec_Allocate(&lzma, coder->Props.data, 5, &a.alloc) == SZ_OK;
            if (decOk) LzmaDec_Init(&lzma);
        }
    }
    const unsigned long long total = s.size;
    while (decOk && left > 0 && unpackLeft > 0 && !s.failed) {
        const void *inBuf = NULL;
        size_t lookahead = 1 << 18;
        SizeT inProcessed, outLen = ARCHIVE_7Z_CHUNK, pos = 0;
        ELzmaStatus st;
        if (lookahead > packLeft) lookahead = (size_t)packLeft;
        if (outLen > unpackLeft) outLen = (SizeT)unpackLeft;
        if (a.look.s.Look(&a.look.s, &inBuf, &lookahead) != SZ_OK) break;
        inProcessed = lookahead;
        const SRes res = islzma2
            ? Lzma2Dec_DecodeToBuf(&lzma2, chunk, &outLen, inBuf, &inProcessed, LZMA_FINISH_ANY, &st)
            : LzmaDec_DecodeToBuf(&lzma, chunk, &outLen, inBuf, &inProcessed, LZMA_FINISH_ANY, &st);
        if (res != SZ_OK || (inProcessed == 0 && outLen == 0) || a.look.s.Skip(&a.look.s, inProcessed) != SZ_OK) break;
        packLeft -= inProcessed;
        unpackLeft -= outLen;
        if (skip >= outLen) { skip -= outLen; continue; }
        pos = (SizeT)skip;
        skip = 0;
        const SizeT take = (UInt64)(outLen - pos) < left ? outLen - pos : (SizeT)left;
        if (!pkgSink(chunk + pos, (uint32_t)take, &s)) break;
        left -= take;
        if (s.spooling) {
            setMessage("Reading the package's file table...");
        } else {
            setMessage("Reading the package: %llu of %llu MB", (s.pos >> 20), (total >> 20));
        }
    }
    if (chunk) {
        if (islzma2) {
            Lzma2Dec_Free(&lzma2, &a.alloc);
        } else {
            LzmaDec_Free(&lzma, &a.alloc);
        }
    }
    free(chunk);
    a7zClose(&a);
    if (s.spoolFp) fclose(s.spoolFp);
    remove(s.spool);

    if (left == 0 && !s.failed && !s.spooling && s.next == s.numBlocks) {
        result = 1;
    } else if (!s.failed) {
        setMessage("The package ended early (%llu MB to go).", (unsigned long long)(left >> 20));
    }
    for (uint32_t f = 0; f < s.numFiles; f++) {
        free(s.files[f].buf);
        if (s.files[f].out) rawClose(s.files[f].fi - NUM_FILES, s.files[f].out, 0);   /* raw files only have one */
    }
    free(s.files);
    free(s.blocks);
    return result;
}

/* ------------------------------------------------------------------- run */

static char runSrc[1024];

int xblaImportRun(const char *src)
{
    const uint64_t t0 = sysGetMicroseconds();
    const int kind = xblaImportKindOf(src);
    char filesDir[1100];
    char d[1024];

    state = XBLA_IMPORT_RUNNING;
    progDone = progTotal = 0;
    wroteHd = wroteSame = wroteRaw = wroteFonts = wroteHud = 0;
    if (!kind) {
        setMessage("That is not GoldenEye XBLA (a .7z of the package, the package, or the unpacked release).");
        state = XBLA_IMPORT_FAILED;
        return 0;
    }
    {
        char why[256];
        if (!xblaImportCheckSpace(0, why, (int)sizeof(why))) {   /* D-202: before the old layer is touched */
            setMessage("%s", why);
            state = XBLA_IMPORT_FAILED;
            return 0;
        }
    }
    layerDir(d, sizeof(d), "tex");
    mkdirs(d);
    emptyDir(d);
    layerDir(d, sizeof(d), "same");
    mkdirs(d);
    emptyDir(d);
    layerDir(d, sizeof(d), "font");
    mkdirs(d);
    emptyDir(d);
    layerDir(d, sizeof(d), "imported.txt");
    remove(d);  /* written last: an interrupted import is never mistaken for a finished one */
    cachedValid = -1;

    setMessage("Importing...");
    sysLogPrintf(LOG_NOTE, "xbla: importing %s (%s)", src, kind == 1 ? "package" : kind == 2 ? ".7z" : "folder");
    int ok;
    if (kind == 3) {
        isFolder(src, filesDir, sizeof(filesDir));
        ok = importFolder(filesDir);
    } else if (kind == 1) {
        ok = importPackage(src);
    } else {
        ok = import7z(src);
    }
    const double secs = (double)(sysGetMicroseconds() - t0) / 1e6;
    if (!ok || wroteHd + wroteSame == 0) {
        if (ok) setMessage("Nothing in it matched the map.");
        sysLogPrintf(LOG_ERROR, "xbla: import failed: %s", message);
        state = XBLA_IMPORT_FAILED;
        texpackReload();
        beanStageLayerChanged();   /* Midas round 22 (R16-2): the old layer was cleared above */
        return 0;
    }
    FILE *f = fopen(d, "w");
    if (f) {
        const char *base = strrchr(src, '/');
        fprintf(f, "source=%s\nsize=%lld\nversion=%d\nhd=%d\nsame=%d\nfiles=%d\nfonts=%d\nhud=%d\nseconds=%.1f\n",
                base ? base + 1 : src, fileSize(src), XBLA_LAYER_VERSION, wroteHd, wroteSame, wroteRaw, wroteFonts,
                wroteHud, secs);
        fclose(f);
    }
    cachedValid = -1;
    setMessage("Imported: %d HD textures, %d same-size copies, %d level and prop files, %d fonts, %d HUD pictures (%.0f s)",
               wroteHd, wroteSame, wroteRaw, wroteFonts, wroteHud, secs);
    sysLogPrintf(LOG_NOTE, "xbla: %s", message);
    state = XBLA_IMPORT_DONE;
    texpackReload();
    beanStageLayerChanged();   /* Midas round 22 (R16-2): reaches the level in play, not only the next */
    return 1;
}

#ifdef XBLA_HAVE_THREADS
static void *worker(void *arg)
{
    (void)arg;
    xblaImportRun(runSrc);
    return NULL;
}
#endif

int xblaImportStart(const char *src)
{
    if (state == XBLA_IMPORT_RUNNING || !src) return 0;
    snprintf(runSrc, sizeof(runSrc), "%s", src);
    state = XBLA_IMPORT_RUNNING;
#ifdef XBLA_HAVE_THREADS
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) {
        pthread_detach(t);
        return 1;
    }
#endif
    return xblaImportRun(runSrc);
}

void xblaImportAtStartup(void)
{
    beanStageCachePrune();   /* D-275: only $C/xbla/levels, only our own names */
    const char *src = texpackXblaPath();
    if (!src || !*src) {
#if defined(PLATFORM_IOS)
        /* A copy dropped into Documents through Files (or put there by the
         * Settings importer) whose layer is not on disk - never imported, or
         * iOS evicted Library/Caches - is imported in the background.
         * Whatever the XBLA switch says (D-141): the switch is live, and a
         * player who turns it on mid-level must find the layer there rather
         * than wait for the next launch. The import only fills the cache;
         * what is drawn is still the switch's call. */
        const struct marker *m = marker();
        if (!m || m->version != XBLA_LAYER_VERSION) {
            char docs[1024], found[1024];
            snprintf(docs, sizeof(docs), "%s", sysResolvePath("$S/xbla"));
            if (xblaImportFind(docs, found, sizeof(found))) {
                sysLogPrintf(LOG_NOTE, "xbla: %s is not imported yet; importing in the background", found);
                xblaImportStart(found);
            }
        }
#endif
        return;
    }
    if (!texpackXblaEnabled()) return;   /* the oracle's dev key: unchanged */
    const struct marker *m = marker();
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    if (m && m->version == XBLA_LAYER_VERSION && !strcmp(m->source, base) && m->size == fileSize(src)) {
        return;
    }
    xblaImportRun(src);
}
