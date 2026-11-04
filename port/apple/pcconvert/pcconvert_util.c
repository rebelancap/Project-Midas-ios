/* Imported from GoldenEye-Decomp-ios engine/port/src/pcconvert/pcconvert_util.c at 55a3a9c8
 * (donor HEAD), Phase 1 round 7a
 * (docs/port-ios/11-round7a-pcconvert.md). Unchanged apart from the
 * libc includes (host headers; realzlib.h is Midas's generated wrapper). */
/*
 * pcconvert — shared helpers: error list, growable buffer, RZ codec, the
 * concatenated-sidecar image builder, and the embedded-table lookups.
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <stdio.h>   /* vsnprintf: host libc (Midas round 7a) */

#include "realzlib.h"
#include "pcconvert_internal.h"

/* ---------------------------------------------------------------- errors ---*/
#define PCV_MAX_ERRORS 256
#define PCV_ERR_LEN    192
static char s_errs[PCV_MAX_ERRORS][PCV_ERR_LEN];
static int  s_nerr = 0;

void pcvErrReset(void) { s_nerr = 0; }
int  pcvErrCount(void) { return s_nerr; }
const char *pcvErrAt(int i)
{
    return (i >= 0 && i < s_nerr && i < PCV_MAX_ERRORS) ? s_errs[i] : "";
}

void pcvErrf(const char *fmt, ...)
{
    va_list ap;
    if (s_nerr < PCV_MAX_ERRORS) {
        va_start(ap, fmt);
        vsnprintf(s_errs[s_nerr], PCV_ERR_LEN, fmt, ap);
        va_end(ap);
    }
    s_nerr++;
}

/* ---------------------------------------------------------------- buffer ---*/
void pcvBufInit(PcvBuf *b) { b->p = NULL; b->len = 0; b->cap = 0; }
void pcvBufFree(PcvBuf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static int pcvBufReserve(PcvBuf *b, size_t need)
{
    size_t cap;
    uint8_t *np;
    if (need <= b->cap)
        return 1;
    cap = b->cap ? b->cap : 4096;
    while (cap < need)
        cap *= 2;
    np = (uint8_t *)realloc(b->p, (size_t)cap);
    if (!np)
        return 0;
    b->p = np;
    b->cap = cap;
    return 1;
}

int pcvBufResize(PcvBuf *b, size_t n)
{
    if (!pcvBufReserve(b, n ? n : 1))
        return 0;
    if (n > b->len)
        memset(b->p + b->len, 0, n - b->len);
    b->len = n;
    return 1;
}

int pcvBufAppend(PcvBuf *b, const void *d, size_t n)
{
    if (!n)
        return 1;
    if (!pcvBufReserve(b, b->len + n))
        return 0;
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 1;
}

/* -------------------------------------------------------------- RZ codec ---*/
uint8_t *pcvInflateRZ(const uint8_t *comp, size_t clen, size_t *outLen)
{
    z_stream strm;
    size_t cap = 0x40000;
    uint8_t *out;
    int ret;

    if (clen < 2 || comp[0] != 0x11 || comp[1] != 0x72) {
        pcvErrf("pcvInflateRZ: bad RZ magic");
        return NULL;
    }
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -15) != Z_OK) {
        pcvErrf("pcvInflateRZ: inflateInit2 failed");
        return NULL;
    }
    out = (uint8_t *)malloc((size_t)cap);
    if (!out) {
        inflateEnd(&strm);
        return NULL;
    }
    strm.next_in  = (Bytef *)(uintptr_t)(comp + 2);
    strm.avail_in = (uInt)(clen - 2);
    strm.next_out = out;
    strm.avail_out = (uInt)cap;
    for (;;) {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END)
            break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            pcvErrf("pcvInflateRZ: inflate failed (%d)", ret);
            inflateEnd(&strm);
            free(out);
            return NULL;
        }
        if (strm.avail_out == 0) {
            uint8_t *np;
            size_t used = cap - strm.avail_out;
            cap *= 2;
            np = (uint8_t *)realloc(out, (size_t)cap);
            if (!np) {
                inflateEnd(&strm);
                free(out);
                return NULL;
            }
            out = np;
            strm.next_out = out + used;
            strm.avail_out = (uInt)(cap - used);
            continue;
        }
        if (ret == Z_BUF_ERROR) {   /* no progress possible: truncated input */
            pcvErrf("pcvInflateRZ: truncated deflate stream");
            inflateEnd(&strm);
            free(out);
            return NULL;
        }
    }
    *outLen = (size_t)strm.total_out;
    inflateEnd(&strm);
    return out;
}

uint8_t *pcvDeflateRZ(const uint8_t *data, size_t len, size_t *outLen)
{
    z_stream strm;
    uLong bound;
    uint8_t *out;
    int ret;

    memset(&strm, 0, sizeof(strm));
    /* Exactly Python's zlib.compressobj(6, zlib.DEFLATED, -15): level 6, raw
     * deflate, default memLevel (8) and Z_DEFAULT_STRATEGY. Any deviation
     * changes the compressed bytes and breaks byte-identity. */
    if (deflateInit2(&strm, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        pcvErrf("pcvDeflateRZ: deflateInit2 failed");
        return NULL;
    }
    bound = deflateBound(&strm, (uLong)len);
    out = (uint8_t *)malloc((size_t)(bound + 2));
    if (!out) {
        deflateEnd(&strm);
        return NULL;
    }
    out[0] = 0x11;
    out[1] = 0x72;
    strm.next_in   = (Bytef *)(uintptr_t)data;
    strm.avail_in  = (uInt)len;
    strm.next_out  = out + 2;
    strm.avail_out = (uInt)bound;
    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        pcvErrf("pcvDeflateRZ: deflate returned %d", ret);
        deflateEnd(&strm);
        free(out);
        return NULL;
    }
    *outLen = (size_t)strm.total_out + 2;
    deflateEnd(&strm);
    return out;
}

/* ---------------------------------------------------------------- image ---*/
void pcvImageInit(PcvImage *im)
{
    pcvBufInit(&im->bin);
    im->rows = NULL;
    im->n = im->cap = 0;
}

void pcvImageFree(PcvImage *im)
{
    pcvBufFree(&im->bin);
    free(im->rows);
    im->rows = NULL;
    im->n = im->cap = 0;
}

int pcvImageEmit(PcvImage *im, const char *name, const uint8_t *d, size_t n)
{
    size_t start = (im->bin.len + 15u) & ~(size_t)15u;
    PcvRow *r;

    if (start > im->bin.len && !pcvBufResize(&im->bin, start))
        return 0;
    if (im->n >= im->cap) {
        int ncap = im->cap ? im->cap * 2 : 128;
        PcvRow *nr = (PcvRow *)realloc(im->rows,
                                       (size_t)(sizeof(PcvRow) * (size_t)ncap));
        if (!nr)
            return 0;
        im->rows = nr;
        im->cap = ncap;
    }
    r = &im->rows[im->n++];
    memset(r->name, 0, sizeof(r->name));
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->offset = (uint32_t)im->bin.len;
    r->size   = (uint32_t)n;
    return pcvBufAppend(&im->bin, d, n);
}

/* ------------------------------------------------------------- filelist ---*/
const struct PcvFileRow *pcvFindRowExact(const char *base)
{
    int i;
    for (i = 0; i < pcconvFileListCount; i++) {
        if (!strcmp(pcconvFileList[i].base, base))
            return &pcconvFileList[i];
    }
    return NULL;
}

/* Python d43_emit.find_row(): for cand in (name, name[1:], name[:-1],
 * name[1:-1]). Returns the matching row and copies the winning key out. */
const struct PcvFileRow *pcvFindRowVariants(const char *name, char *keyOut,
                                            size_t keyOutSz)
{
    char cand[PCV_NAME_LEN];
    size_t len = strlen(name);
    int v;

    for (v = 0; v < 4; v++) {
        size_t s = (v == 1 || v == 3) ? 1 : 0;
        size_t e = (v == 2 || v == 3) ? (len ? len - 1 : 0) : len;
        const struct PcvFileRow *row;
        if (e < s || (e - s) >= sizeof(cand))
            continue;
        memcpy(cand, name + s, e - s);
        cand[e - s] = 0;
        row = pcvFindRowExact(cand);
        if (row) {
            if (keyOut && keyOutSz)
                strncpy(keyOut, cand, keyOutSz - 1), keyOut[keyOutSz - 1] = 0;
            return row;
        }
    }
    return NULL;
}

const struct PcvNsNtRow *pcvFindNsNtVariants(const char *key)
{
    char cand[PCV_NAME_LEN];
    size_t len = strlen(key);
    int v, i;

    for (v = 0; v < 4; v++) {
        size_t s = (v == 1 || v == 3) ? 1 : 0;
        size_t e = (v == 2 || v == 3) ? (len ? len - 1 : 0) : len;
        if (e < s || (e - s) >= sizeof(cand))
            continue;
        memcpy(cand, key + s, e - s);
        cand[e - s] = 0;
        for (i = 0; i < pcconvNsNtCount; i++) {
            if (!strcmp(pcconvNsNt[i].name, cand))
                return &pcconvNsNt[i];
        }
    }
    return NULL;
}
