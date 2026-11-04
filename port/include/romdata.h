/**
 * romdata.h - the runtime ROM loader (port/src/romdata.c): locates, validates
 * and normalises the user's GoldenEye ROM and parses its file directory so
 * assets can be served from it instead of the exe (see
 * docs/port/rom-asset-loading.md; GE_ROMDATA=0 skips it all).
 */
#ifndef PORT_ROMDATA_H
#define PORT_ROMDATA_H

#include <ultra64.h>

/* one entry of the ROM's own asset directory (file_resource_table, ob.h
 * fileentry) as parsed from the inflated cdata segment */
struct romfile {
    const char *name;    /* -> string pool inside the inflated cdata */
    u32 romofs;          /* raw ROM byte offset (hw_address) */
    u32 size;            /* next.romofs - this.romofs; 0 = alias entry */
    u8  compressed;      /* rom[romofs] == 0x11 && rom[romofs+1] == 0x72 */
};

/* where the ROM came from, for the always-on log line */
enum romsource {
    ROMSRC_NONE = 0,
    ROMSRC_ARGV,      /* argv[1]: drag-and-drop onto the exe */
    ROMSRC_ARGFLAG,   /* --rom-file <path> */
    ROMSRC_ENV,       /* GE_ROM=<path> */
    ROMSRC_INI,       /* midas.ini [data] rom= */
    ROMSRC_DATADIR,   /* first valid file in ./Data */
    ROMSRC_LEGACY     /* repo-root baserom.u.z64 (dev convenience) */
#ifdef PORT
    , ROMSRC_HOST     /* round 9a: romdataSetRomPath (the iOS shell's onboarding) */
#endif
};

#ifdef PORT
/* Round 9a (docs/port-ios/16): the path the iOS shell resolved (onboarding /
 * Documents), tried FIRST by romdataInit. Call before geEngineMain. A copy
 * is kept; NULL or "" clears it. */
void romdataSetRomPath(const char *path);
#endif

/* main(): find + load the ROM, parse its tables.
 * Returns 1 when a ROM is loaded and its file table parsed; 0 when the
 * game must run on the compiled-in assets (no ROM, GE_ROMDATA=0, or a
 * ROM that failed validation - every case is logged). */
s32 romdataInit(int argc, char **argv);

/* 1 when romdataInit succeeded */
s32 romdataLoaded(void);

/* the whole normalised (z64 byte order) ROM; NULL when not loaded */
const u8 *romdataRom(u32 *size);

/* the file directory: count includes index 0 (NULL sentinel) and the
 * ob/ob_end.seg bound entry, like file_entry_max */
s32 romdataFileCount(void);
const struct romfile *romdataFile(s32 index);
s32 romdataFileIndex(const char *name); /* -1 on a miss */

/* a named ROM region from port/generated/romseg_list.h (images, music,
 * ramrom, animation, fonts ...): pointer into the ROM buffer, or NULL */
const u8 *romdataSeg(const char *name, u32 *size);

/* FILE-TABLE CLASSES: ob.c returns compiled-in data for every file except
 * those romdataFilesApply() repointed at the ROM, which take the vanilla load
 * path (romCopy + inflate) and are then converted for the host by
 * romdataFilePreprocess() according to the file's class. */
void romdataFilesApply(void);                       /* obInit's PC arm, after the size loop */
s32 romdataFileServed(s32 index);                   /* 1 = the ROM copy, vanilla path */
const u8 *romdataFileCompiled(s32 index);             /* the pristine compiled blob a served file replaced, or NULL */
void romdataFilePreprocess(s32 index, u8 *data, u32 size); /* after load_resource */

/* video.c's ini reader: 1 when `key` belongs to [data] (so it is not
 * reported as an unknown control) */
s32 romdataIsIniKey(const char *key);

#ifdef PORT
/* Round 2: the donor's cart-data fixups, called from merged game code under
 * PORT. Inert stubs in port/apple/donor_stubs.c until round 7 (pcconvert). */
u32  romdataAudioBankPcSize(const u8 *src, u32 srcSize);
u32  romdataFontPcSize(const u8 *src, u32 n64Size);
void romdataFixupAnimationData(u8 *blob, u32 blobSize, const s32 *tableA, const s32 *tableB);
void romdataFixupAudioBank(u8 *blob, u32 srcSize, u32 allocSize);
void romdataFixupBriefing(u8 *blob);
void romdataFixupCseq(u8 *blob);
void romdataFixupFont(u8 *blob, u32 n64Size);
void romdataFixupLangBank(u8 *blob, u32 blobSize);
void romdataFixupMusicSeqTable(u8 *blob, u32 blobSize);
#endif

#if defined(PLATFORM_64BIT)
/* Round 6 (R15): a window-resident, interned copy of a string, so its (u32)
 * is a lossless guest address (text bank slots, langGet's error literals).
 * Already-in-window pointers come back unchanged. romdata.c. */
const char *portGuestString(const char *s);
#endif

#endif
