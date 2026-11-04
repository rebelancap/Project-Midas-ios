/* Imported from GoldenEye-Decomp-ios engine/port/src/pcconvert/pcconvert_internal.h at 55a3a9c8
 * (donor HEAD), Phase 1 round 7a
 * (docs/port-ios/11-round7a-pcconvert.md). Midas: built as a host-libc library
 * (real <stdlib.h>, not the game's include/ stubs), so the malloc decls are
 * gone; pcvLog goes through a settable sink (pcconvert.h). */
/*
 * pcconvert — shared internals for the in-engine C port of the three Python
 * sidecar converters (tools_pc/d43_emit.py, d69_emit.py, d88_emit.py).
 *
 * See docs/sidecar-converters.md for the design and the byte-identity proof.
 * Nothing here talks to the game; the module is a pure function of the ROM
 * plus the build-time-embedded committed metadata (pcconvert_tables.c).
 */
#ifndef PORT_PCCONVERT_INTERNAL_H
#define PORT_PCCONVERT_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

/* Midas round 7a: the pcconvert library is compiled against the host libc
 * (CMake gives it no game include path), so the donor's malloc/calloc/free
 * redeclarations for the decomp's stub <stdlib.h> are not needed. */
#include <stdlib.h>

/* ------------------------------------------------------------- embedded ---
 * Generated at configure time by cmake/gen_pcconvert_tables.cmake from the
 * committed files the Python scripts read:
 *   scripts/filelist.u.csv                    -> pcconvFileList
 *   assets/obseg/file_resource_table.inc.c    -> pcconvTableNames
 *   assets/{...}/modelFileHeader.inc.c        -> pcconvNsNt
 */
struct PcvFileRow  { const char *base; uint32_t addr; uint32_t size; };
struct PcvNsNtRow  { const char *name; uint16_t ns; uint16_t nt; };

extern const struct PcvFileRow pcconvFileList[];
extern const int               pcconvFileListCount;
extern const char *const       pcconvTableNames[];
extern const int               pcconvTableNameCount;
extern const struct PcvNsNtRow pcconvNsNt[];
extern const int               pcconvNsNtCount;

/* --------------------------------------------------------------- errors ---
 * Mirrors the Python `errors` list: converters accumulate, the orchestrator
 * aborts if any fired. pcvErr() never allocates.
 */
void        pcvErrReset(void);
void        pcvErrf(const char *fmt, ...);
int         pcvErrCount(void);
const char *pcvErrAt(int i);

/* --------------------------------------------------------------- buffer ---*/
typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
} PcvBuf;

void pcvBufInit(PcvBuf *b);
void pcvBufFree(PcvBuf *b);
int  pcvBufResize(PcvBuf *b, size_t n);     /* zero-fills growth; 1 on success */
int  pcvBufAppend(PcvBuf *b, const void *d, size_t n);

/* ---------------------------------------------------- big-endian readers ---*/
static inline uint32_t pcvBE32(const uint8_t *b, size_t o)
{
    return ((uint32_t)b[o] << 24) | ((uint32_t)b[o + 1] << 16) |
           ((uint32_t)b[o + 2] << 8) | (uint32_t)b[o + 3];
}
static inline int32_t pcvBES32(const uint8_t *b, size_t o)
{
    return (int32_t)pcvBE32(b, o);
}
static inline uint16_t pcvBE16(const uint8_t *b, size_t o)
{
    return (uint16_t)(((uint32_t)b[o] << 8) | (uint32_t)b[o + 1]);
}
static inline int16_t pcvBES16(const uint8_t *b, size_t o)
{
    return (int16_t)pcvBE16(b, o);
}
/* Little-endian stores (the PC sidecar layout). */
static inline void pcvPut32(uint8_t *b, size_t o, uint32_t v)
{
    b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8);
    b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24);
}
static inline void pcvPut16(uint8_t *b, size_t o, uint16_t v)
{
    b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8);
}
static inline uint32_t pcvGet32(const uint8_t *b, size_t o)
{
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
           ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

#define PCV_MASK 0xFFFFFFu

/* ------------------------------------------------------------- RZ codec ---
 * RZ = 0x11 0x72 + a raw deflate stream. Inflate reuses the same real-zlib
 * path port/src/rzdecomp.c uses for decompressdata(); deflate must match
 * Python's zlib.compressobj(6, DEFLATED, -15) exactly (level 6, memLevel 8,
 * Z_DEFAULT_STRATEGY) or the sidecars stop being byte-identical.
 */
uint8_t *pcvInflateRZ(const uint8_t *comp, size_t clen, size_t *outLen);
uint8_t *pcvDeflateRZ(const uint8_t *data, size_t len, size_t *outLen);

/* ---------------------------------------------------------- output image ---*/
#define PCV_NAME_LEN 64
typedef struct { char name[PCV_NAME_LEN]; uint32_t offset; uint32_t size; } PcvRow;
typedef struct {
    PcvBuf  bin;
    PcvRow *rows;
    int     n;
    int     cap;
} PcvImage;

void pcvImageInit(PcvImage *im);
void pcvImageFree(PcvImage *im);
/* 16-align the cursor, record (name, offset, size), append the bytes. */
int  pcvImageEmit(PcvImage *im, const char *name, const uint8_t *d, size_t n);

/* ------------------------------------------------------------- filelist ---*/
const struct PcvFileRow *pcvFindRowExact(const char *base);
/* d43's find_row(): tries name, name+1, name-minus-last, both. */
const struct PcvFileRow *pcvFindRowVariants(const char *name, char *keyOut,
                                            size_t keyOutSz);
const struct PcvNsNtRow *pcvFindNsNtVariants(const char *key);

/* ------------------------------------------------------- the converters ---
 * Each appends to `im` in exactly the order its Python counterpart does.
 */
int pcvD43(const uint8_t *rom, size_t romLen, PcvImage *im);   /* pcmodels.bin */
/* Midas: d69's Tbg_*_stanZ output is Midas's StandTile encoding (R5), not
 * the donor's -- see d69.c convertStan. */
int pcvD69(const uint8_t *rom, size_t romLen, PcvImage *im);   /* pccg.bin     */
int pcvD88(const uint8_t *rom, size_t romLen, PcvImage *im);   /* += Usetup*Z  */

/* d88's propDefs record-stream converter (tools_pc/d88_propdefs.py). Returns a
 * malloc'd buffer of *outLen bytes, or NULL with pcvErrf() called. */
uint8_t *pcvPropDefsConvert(const uint8_t *src, size_t start, size_t end,
                            size_t *outLen, const char *name);

/* Logging shim so the module works both inside the engine and from the CLI
 * hook. Implemented in pcconvert.c. */
void pcvLog(const char *fmt, ...);

/* Midas round 7a: the three converters in one call (driver + host test);
 * returns 1, or 0 with the pcvErr list filled. Images are appended to. */
int pcconvertBuild(const uint8_t *rom, size_t romLen, PcvImage *models, PcvImage *cg);

/* Whole-file read (malloc'd), NULL on failure. */
uint8_t *pcvReadFile(const char *path, size_t *len);

#endif /* PORT_PCCONVERT_INTERNAL_H */
