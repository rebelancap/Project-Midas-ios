/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/xblaimport.h (last changed 36ecd33e; donor HEAD 55a3a9c8),
 * round 10, XBLA step 1 (docs/port-ios/19-round10-xbla-import.md). 64-bit host layer only. */
#ifndef PORT_XBLAIMPORT_H
#define PORT_XBLAIMPORT_H

/*
 * Importing GoldenEye XBLA's pictures from the user's own copy (Tier B).
 * Round 19c, D-096..D-098, docs/texture-packs.md.
 *
 * The release is accepted in every form Perfect Dark's port accepts it:
 *   - the Xbox 360 package (STFS "LIVE", title 584108A9) — the console's own
 *     download, 739 MB, bare or anywhere in a folder;
 *   - a .7z holding that package (the form it is usually kept in), read
 *     streaming, no 739 MB spool (PD's D-079 approach);
 *   - the unpacked release: a folder holding files/original, files/new, ...
 * Only the files the shipped map names are read (xblamap_table.h). Their
 * pictures are decoded and written as a texture layer the pack loader reads:
 * $C/xbla/tex/<texnum>.png (HD upscales) and $C/xbla/same/<texnum>.png (the
 * release's same-size copies, drawn only with Video.XblaSameSize=1). On iOS
 * $C is Library/Caches: never backed up, never Files-visible, never shipped.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XBLA_IMPORT_IDLE = 0,
    XBLA_IMPORT_RUNNING,
    XBLA_IMPORT_DONE,
    XBLA_IMPORT_FAILED,
};

/* What a path is: 0 not the release, 1 package, 2 .7z, 3 folder. */
int xblaImportKindOf(const char *path);

/* The first copy of the release under `dir` (depth 4), into out. 0 if none. */
int xblaImportFind(const char *dir, char *out, int outlen);

/* Start importing `src` on a worker thread. 0 if one is already running or
 * `src` is not the release. */
int xblaImportStart(const char *src);

/* Run the import on this thread. Returns 1 on success. */
int xblaImportRun(const char *src);
/* D-202: 1 if the layer's volume has room for an import plus extraBytes (the
 * shell's copy of the release); else 0 and a player-facing reason in msg. */
int xblaImportCheckSpace(long long extraBytes, char *msg, int msglen);

/* State, progress (files of the map read so far / to read) and a status line. */
int xblaImportState(void);
void xblaImportProgress(int *done, int *total);
const char *xblaImportMessage(void);

/* What the last finished import wrote (from the marker on disk): 0 = none. */
int xblaImportedHd(void);
int xblaImportedSame(void);
const char *xblaImportedSource(void);   /* "" when not imported */
double xblaImportedSeconds(void);

/* Remove the imported layer. */
void xblaImportForget(void);

/* Startup: Video.XblaPath (the macOS oracle's dev key) imports once when the
 * layer on disk came from somewhere else. Synchronous; call before the
 * first frame. */
void xblaImportAtStartup(void);

/* The shipped map's size, for status text. */
int xblaMapNumHd(void);
int xblaMapNumSame(void);

#ifdef __cplusplus
}
#endif

#endif
