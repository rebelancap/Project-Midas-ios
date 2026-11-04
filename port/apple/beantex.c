/* Imported verbatim from GoldenEye-Decomp-ios engine/port/src/beantex.c (last changed 93988236; donor HEAD 55a3a9c8),
 * round 10, XBLA step 1 (docs/port-ios/19-round10-xbla-import.md). 64-bit host layer only. */
/*
 * GoldenEye XBLA pictures — see port/include/beantex.h. Round 19c, D-096.
 *
 * The CAFF reader and the still-picture decode are Perfect Dark's
 * (perfect-dark-ios vendor/dabs-mod port/src/gebean.c, MIT): the layout is
 * CAFFeinated's BundleV36 checked against the release's own files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "x360.h"
#include "beantex.h"

#define CAFF_MAXSECTS 16
#define BEAN_MAXTEX 4096

struct caffsect {
    char name[16];
    uint64_t offset;
    uint32_t size;
};

struct cafffile {
    uint32_t asset;
    uint32_t start;
    uint32_t size;
    uint8_t sect;
};

struct beanfile {
    const uint8_t *d;
    uint32_t len;
    int be;
    int numsects;
    struct caffsect sects[CAFF_MAXSECTS];
    uint32_t numassets;
    uint32_t nametable;
    uint32_t labels;
    uint32_t numfiles;
    struct cafffile *files;
    int numtex;
    int32_t *texfile;
};

static int fits(uint64_t ofs, uint64_t len, uint64_t size)
{
    return ofs <= size && len <= size - ofs;
}

static uint32_t rbe32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rbe16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t hdr32(const struct beanfile *c, uint64_t o)
{
    if (!fits(o, 4, c->len)) {
        return 0;
    }
    const uint8_t *p = c->d + o;
    return c->be ? rbe32(p) : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static const uint8_t *blob(const struct beanfile *c, int32_t index, uint32_t *outLen)
{
    if (index < 0 || (uint32_t)index >= c->numfiles) {
        return NULL;
    }
    const struct cafffile *f = &c->files[index];
    if (f->sect == 0 || f->sect > c->numsects) {
        return NULL;
    }
    const uint64_t at = c->sects[f->sect - 1].offset + f->start;
    if (!fits(at, f->size, c->len)) {
        return NULL;
    }
    *outLen = f->size;
    return c->d + at;
}

static const char *sectName(const struct beanfile *c, int32_t index)
{
    const uint8_t sect = c->files[index].sect;
    return sect && sect <= c->numsects ? c->sects[sect - 1].name : "";
}

static const char *assetName(const struct beanfile *c, uint32_t asset)
{
    if (asset == 0 || asset > c->numassets) {
        return "";
    }
    const uint32_t off = hdr32(c, c->nametable + 4ull * (asset - 1));
    const uint64_t at = (uint64_t)c->labels + off;
    if (at >= c->len || !memchr(c->d + at, '\0', c->len - at)) {
        return "";
    }
    const char *name = (const char *)c->d + at;
    const char *last = strrchr(name, '\\');
    return last ? last + 1 : name;
}

static int32_t findSect(const struct beanfile *c, uint32_t asset, const char *prefix)
{
    for (uint32_t i = 0; i < c->numfiles; i++) {
        if (c->files[i].asset == asset && strncmp(sectName(c, (int32_t)i), prefix, strlen(prefix)) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

struct beanfile *beanOpen(const uint8_t *d, uint32_t len)
{
    uint32_t stroff[CAFF_MAXSECTS];
    struct beanfile *c;

    if (!d || len < 0x68 || memcmp(d, "CAFF", 4) != 0 || d[0x4a] != 0) {
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->d = d;
    c->len = len;
    c->be = d[0x48] == 1;
    const uint32_t hsize = hdr32(c, 0x14);
    c->numassets = hdr32(c, 0x1c);
    c->numfiles = hdr32(c, 0x20);
    c->numsects = d[0x49];
    const uint32_t namelen = hdr32(c, 0x4c);
    const uint32_t sectsize = hdr32(c, 0x50);
    const uint32_t filesize = hdr32(c, 0x64);

    if (c->numsects > CAFF_MAXSECTS || c->numassets > 0x10000 || c->numfiles > 0x10000) {
        free(c);
        return NULL;
    }
    uint64_t pos = hsize;
    if (!fits(pos, 0x21ull * c->numsects, len)) {
        free(c);
        return NULL;
    }
    for (int i = 0; i < c->numsects; i++) {
        stroff[i] = hdr32(c, pos);
        c->sects[i].size = hdr32(c, pos + 9);
        pos += 0x21;
    }
    for (int i = 0; i < c->numsects; i++) {
        const uint64_t at = pos + stroff[i];
        for (int j = 0; j < (int)sizeof(c->sects[i].name) - 1 && at + j < len && d[at + j]; j++) {
            c->sects[i].name[j] = (char)d[at + j];
        }
    }
    pos += namelen;
    const uint32_t total = hdr32(c, pos);
    c->nametable = (uint32_t)(pos + 4);
    c->labels = (uint32_t)(pos + 4 + 4ull * c->numassets);
    pos = (uint64_t)c->labels + total;
    const uint32_t adb = hdr32(c, pos);
    pos += 4ull + adb;
    if (!fits(pos, 14ull * c->numfiles, len)) {
        free(c);
        return NULL;
    }
    c->files = calloc(c->numfiles ? c->numfiles : 1, sizeof(*c->files));
    c->texfile = calloc(BEAN_MAXTEX, sizeof(*c->texfile));
    if (!c->files || !c->texfile) {
        beanClose(c);
        return NULL;
    }
    for (uint32_t i = 0; i < c->numfiles; i++) {
        c->files[i].asset = hdr32(c, pos);
        c->files[i].start = hdr32(c, pos + 4);
        c->files[i].size = hdr32(c, pos + 8);
        c->files[i].sect = d[pos + 12];
        pos += 14;
    }
    uint64_t data = (uint64_t)hsize + sectsize + filesize;
    for (int i = 0; i < c->numsects; i++) {
        c->sects[i].offset = data;
        data += c->sects[i].size;
    }
    for (uint32_t i = 0; i < c->numfiles && c->numtex < BEAN_MAXTEX; i++) {
        uint32_t blen;
        const uint8_t *b = blob(c, (int32_t)i, &blen);
        /* blen >= 8, as PD's beanLoad() counts them: a material's texture
         * index is a position in this list, so it must be the same list
         * (beanTextureInfo/beanDecodeTexture refuse a header under 0x40). */
        if (b && blen >= 8 && memcmp(b, "texture\0", 8) == 0) {
            c->texfile[c->numtex++] = (int32_t)i;
        }
    }
    return c;
}

const uint8_t *beanSection(const struct beanfile *c, uint32_t asset, const char *prefix, uint32_t *len)
{
    *len = 0;
    if (!c) {
        return NULL;
    }
    const int32_t i = findSect(c, asset, prefix);
    return i >= 0 ? blob(c, i, len) : NULL;
}

/* Midas round 43 (docs/port-ios/60-round43-xbla-fonts-hud.md): the first
 * blob of any asset that starts with `magic` and is at least `minlen` long -
 * a font file's "bfont\0" metrics (PD's gebeanFontOpen walks the same way). */
const uint8_t *beanFindBlob(const struct beanfile *c, const char *magic, uint32_t magiclen, uint32_t minlen,
                            uint32_t *len)
{
    *len = 0;
    for (uint32_t i = 0; c && i < c->numfiles; i++) {
        uint32_t blen;
        const uint8_t *b = blob(c, (int32_t)i, &blen);
        if (b && blen >= minlen && blen >= magiclen && memcmp(b, magic, magiclen) == 0) {
            *len = blen;
            return b;
        }
    }
    return NULL;
}

int beanIsBigEndian(const struct beanfile *c)
{
    return c ? c->be : 0;
}

void beanClose(struct beanfile *c)
{
    if (c) {
        free(c->files);
        free(c->texfile);
        free(c);
    }
}

int beanNumTextures(const struct beanfile *c)
{
    return c ? c->numtex : 0;
}

const char *beanTextureName(const struct beanfile *c, int t)
{
    if (!c || t < 0 || t >= c->numtex) {
        return "";
    }
    return assetName(c, c->files[c->texfile[t]].asset);
}

int beanFindTexture(const struct beanfile *c, const char *name)
{
    for (int t = 0; c && t < c->numtex; t++) {
        if (strcmp(beanTextureName(c, t), name) == 0) {
            return t;
        }
    }
    return -1;
}

int beanTextureInfo(const struct beanfile *c, int t, int32_t *w, int32_t *h, int32_t *format)
{
    uint32_t blen;
    if (!c || t < 0 || t >= c->numtex) {
        return 0;
    }
    const uint8_t *b = blob(c, c->texfile[t], &blen);
    if (!b || blen < 0x40) {
        return 0;
    }
    *w = rbe16(b + 0x24);
    *h = rbe16(b + 0x26);
    *format = b[0x1b] & 0x3f;
    return 1;
}

uint8_t *beanDecodeTexture(const struct beanfile *c, int t, int32_t *outW, int32_t *outH)
{
    struct x360fetch fetch;
    uint32_t blen, glen = 0;
    if (!c || t < 0 || t >= c->numtex) {
        return NULL;
    }
    const uint8_t *b = blob(c, c->texfile[t], &blen);
    if (!b || blen < 0x40) {
        return NULL;
    }
    const uint32_t w = rbe16(b + 0x24);
    const uint32_t h = rbe16(b + 0x26);
    uint32_t base = rbe32(b + 0x28);
    const uint32_t levels = b[0x30];

    int32_t gi = findSect(c, c->files[c->texfile[t]].asset, ".gpu");
    if (gi >= 0) {
        base = 0;
    } else {
        for (uint32_t i = 0; i < c->numfiles && gi < 0; i++) {
            if (strcmp(assetName(c, c->files[i].asset), "texture pairs") == 0
                    && strncmp(sectName(c, (int32_t)i), ".gpu", 4) == 0) {
                gi = (int32_t)i;
            }
        }
    }
    const uint8_t *g = gi >= 0 ? blob(c, gi, &glen) : NULL;
    if (!g || w == 0 || h == 0 || w > 4096 || h > 4096 || base > glen) {
        return NULL;
    }

    memset(&fetch, 0, sizeof(fetch));
    fetch.width = w;
    fetch.height = h;
    fetch.format = b[0x1b] & 0x3f;
    fetch.tiled = 1;
    fetch.endian = b[0x1a] ? (fetch.format == X360_FMT_8888 ? 2 : 1) : 0;
    /* One level is not packed: it stands at the tile's origin. Every N64-size
     * picture is small enough for this to decide whether it decodes at all. */
    fetch.packed = levels > 1;
    if (!x360FetchSupported(&fetch)) {
        return NULL;
    }

    uint32_t bpe, ew, eh;
    if (fetch.format == X360_FMT_8888) {
        bpe = 4;
        fetch.pitch = w;
        ew = (w + 31) & ~31u;
        eh = (h + 31) & ~31u;
    } else {
        bpe = fetch.format == X360_FMT_DXT1 ? 8 : 16;
        fetch.pitch = (w + 3) & ~3u;
        ew = (((w + 3) / 4) + 31) & ~31u;
        eh = (((h + 3) / 4) + 31) & ~31u;
    }
    const uint64_t need = (uint64_t)ew * eh * bpe;
    const uint64_t have = glen - base;
    uint8_t *copy = calloc((size_t)need, 1);
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!copy || !rgba) {
        free(copy);
        free(rgba);
        return NULL;
    }
    memcpy(copy, g + base, (size_t)(have < need ? have : need));
    if (!x360DecodeTexture(copy, (uint32_t)need, &fetch, rgba)) {
        free(copy);
        free(rgba);
        return NULL;
    }
    free(copy);
    /* x360DecodeTexture() reads 8888 as PD's BGRA; the release holds RGBA. */
    if (fetch.format == X360_FMT_8888) {
        for (uint32_t i = 0; i < w * h; i++) {
            const uint8_t r = rgba[i * 4];
            rgba[i * 4] = rgba[i * 4 + 2];
            rgba[i * 4 + 2] = r;
        }
    }
    *outW = (int32_t)w;
    *outH = (int32_t)h;
    return rgba;
}
