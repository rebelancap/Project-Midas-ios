/*
 * The GoldenEye XBLA release's own fonts, fitted into the ROM's glyph cells.
 * Midas round 43 (docs/port-ios/60-round43-xbla-fonts-hud.md); see
 * port/include/xblafont.h for what this is.
 *
 * The method and most of the measuring code are Perfect Dark's
 * (perfect-dark-ios vendor/dabs-mod port/src/xblafont.c, MIT): xblaFontSpan,
 * the ROM ink box, the line fit by agreement (xblaFontBuildLine), the font's
 * condensation (xblaFontBuildCond), the band fill across (xblaFontFillAcross),
 * the body picture (xblaFontBuildBody) and the area sampler (xblaFontArea).
 * What differs here is GoldenEye's side: its glyphs are I8 bitmaps cropped to
 * the ink, (width + 7) & ~7 texels a row, drawn from the tile's own top-left
 * texel (no border row or column), and the release's two fonts come from the
 * Bean release's misc/doc0 and misc/alps3 "bfont" files (gebean.c's
 * gebeanFontOpen layout) rather than PD's Textures.raw atlases.
 *
 * ## Fitting it to the line (PD's words, GoldenEye's numbers)
 *
 * The ROM's glyph box is its ink, with the baseline held separately; the
 * release's is its own box on its own baseline. The two are matched on ink,
 * and on the ink of the *font*: one scale and one offset per font, a line
 * through the tops and bottoms of every character's ink at once, taken by
 * agreement so that the characters the ROM drew somewhere of its own (an
 * overbar '_', a high '=') do not drag the font off its row. A glyph is placed
 * on that line inside the rows the game draws of it; one that does not fit is
 * shrunk, not squashed. Across, the release's ink fills the columns the ROM's
 * ink filled, widened only as far as the font's own condensation allows and
 * never past the character's width. Everything the game measures is the ROM's:
 * nothing reflows.
 *
 * Render thread, except xblaFontRegisterGlyph (game thread, under `lock`).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <pthread.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "texpack.h"
#include "pngread.h"
#include "xblafont.h"

/* the decomp's include/math.h stub: declared here, as beanmodel.c does */
float floorf(float x);
float ceilf(float x);

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "xblafont.c must not see the sim's allocators (mema.h/memp.h): host memory only"
#endif

extern const char *sysResolvePath(const char *path);   /* hostpaths.c */

#define XF_NUM_CHARS 94
#define XF_FIRST_CHAR 0x21
#define XF_MAX_ROWS 64
#define XF_MAX_STRIDE 32

/* Picture scale, in pixels per ROM texel: chosen per glyph so the release's
 * ink is never shrunk, floored so the GPU never magnifies the picture much on
 * a phone or a headset (about 4.5 to 5 pixels a texel), capped so a stray
 * one-texel ROM glyph cannot ask for a megabyte (PD's 2 and 12). */
#define XF_MIN_SCALE 4
#define XF_MAX_SCALE 12

/* PD's tolerances, in ROM texels (xblafont.c): a third of a texel decides
 * "on the line"; a round letter's overshoot is nudged by at most that much. */
#define XF_FIT_TOL 0.34f
#define XF_FIT_NUDGE 0.34f
#define XF_FIT_MIN_ANCHORS 16
#define XF_FIT_MIN_SPREAD 0.25f
#define XF_COND_MIN_CHARS 8

struct xfbox {
    float x1, y1, x2, y2;
};

struct romglyph {
    const void *addr;
    int32_t w, h, baseline, stride;
    uint8_t *pix;   /* stride * h, the ROM's texels */
};

struct relglyph {
    int32_t ok;
    int32_t x0, y0, w, h;   /* the box in the atlas */
    int32_t left, top, adv;
};

struct relfont {
    int32_t tried;          /* 1 read, -1 not there */
    uint8_t *alpha;         /* one byte a pixel */
    int32_t W, H;
    struct relglyph g[XF_NUM_CHARS];
};

struct xfline {
    int32_t tried;          /* 1 fitted, -1 gave up */
    float scale, offset, cond;
    int32_t anchors, total;
};

struct xfpic {
    int32_t tried;
    uint8_t *rgba;
    int32_t w, h;
};

static const char *const relNames[XBLAFONT_NUM] = { "doc0", "alps3" };
static const char *const romNames[XBLAFONT_NUM] = { "Bank Gothic", "Zurich" };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct romglyph rom[XBLAFONT_NUM][XF_NUM_CHARS];
static uint32_t romGen = 1;         /* bumped when a glyph's texels change (never, in practice) */

static struct relfont rel[XBLAFONT_NUM];
static struct xfline lines[XBLAFONT_NUM];
static struct xfpic pics[XBLAFONT_NUM][XF_NUM_CHARS];
static uint32_t builtGen;           /* the romGen the lines and pictures were made against */
static int32_t numBuilt, numOffLine, numMissing;

/* ---------------------------------------------------------------- the ROM's */

void xblaFontRegisterGlyph(int32_t font, int32_t index, const void *pixels, int32_t width, int32_t height,
                           int32_t baseline)
{
    if (font < 0 || font >= XBLAFONT_NUM || index < 0 || index >= XF_NUM_CHARS) {
        return;
    }
    const int32_t stride = (width + 7) & ~7;
    struct romglyph *g = &rom[font][index];
    pthread_mutex_lock(&lock);
    if (!pixels || width <= 0 || height <= 0 || stride > XF_MAX_STRIDE || height > XF_MAX_ROWS) {
        g->addr = NULL;
        pthread_mutex_unlock(&lock);
        return;
    }
    const size_t n = (size_t)stride * (size_t)height;
    if (!g->pix || g->w != width || g->h != height || g->baseline != baseline || memcmp(g->pix, pixels, n) != 0) {
        uint8_t *copy = malloc(n);
        if (!copy) {
            g->addr = NULL;
            pthread_mutex_unlock(&lock);
            return;
        }
        memcpy(copy, pixels, n);
        free(g->pix);
        g->pix = copy;
        g->w = width;
        g->h = height;
        g->baseline = baseline;
        g->stride = stride;
        romGen++;
    }
    g->addr = pixels;
    pthread_mutex_unlock(&lock);
}

int xblaFontFind(const void *addr, uint32_t bytes, int32_t *font, int32_t *index)
{
    int found = 0;
    if (!addr) {
        return 0;
    }
    pthread_mutex_lock(&lock);
    for (int32_t f = 0; f < XBLAFONT_NUM && !found; f++) {
        for (int32_t i = 0; i < XF_NUM_CHARS; i++) {
            const struct romglyph *g = &rom[f][i];
            if (g->addr == addr && g->pix) {
                const uint32_t n = (uint32_t)(g->stride * g->h);
                if (n <= bytes && memcmp(addr, g->pix, n) == 0) {
                    *font = f;
                    *index = i;
                    found = 1;
                }
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}

/**
 * The outer edges of a run of coverage, to a fraction of a texel (PD's
 * xblaFontSpan). The outermost row of an antialiased glyph is often the spill
 * the rasteriser left when an edge fell near a texel boundary; its ink against
 * the ink of the row inside it says where the edge really was. Down a glyph
 * the reading is each row's total, across it each column's deepest texel.
 */
static void xfSpan(const float *ink, int32_t num, float *outLo, float *outHi)
{
    int32_t first = 0;
    int32_t last = num - 1;

    while (first < num && ink[first] <= 0) {
        first++;
    }
    while (last >= 0 && ink[last] <= 0) {
        last--;
    }
    if (first > last) {
        *outLo = 0;
        *outHi = 0;
        return;
    }
    *outLo = (float)first;
    *outHi = (float)(last + 1);
    if (first < last && ink[first + 1] > ink[first]) {
        *outLo = first + 1.0f - ink[first] / ink[first + 1];
    }
    if (last > first && ink[last - 1] > ink[last]) {
        *outHi = last + ink[last] / ink[last - 1];
    }
}

/* The box the ROM glyph's ink fills, in texels of its tile. Caller holds lock. */
static int xfRomInk(const struct romglyph *g, struct xfbox *body)
{
    float rowink[XF_MAX_ROWS];
    float colink[XF_MAX_STRIDE];
    int any = 0;

    if (!g->pix || g->h <= 0 || g->h > XF_MAX_ROWS || g->stride > XF_MAX_STRIDE) {
        return 0;
    }
    memset(colink, 0, sizeof(colink));
    for (int32_t r = 0; r < g->h; r++) {
        rowink[r] = 0;
        for (int32_t c = 0; c < g->w; c++) {
            const float a = g->pix[r * g->stride + c];
            if (a > 0) {
                rowink[r] += a;
                if (a > colink[c]) {
                    colink[c] = a;
                }
                any = 1;
            }
        }
    }
    if (!any) {
        return 0;
    }
    xfSpan(colink, g->w, &body->x1, &body->x2);
    xfSpan(rowink, g->h, &body->y1, &body->y2);
    return body->x2 > body->x1 && body->y2 > body->y1;
}

/**
 * The character whose release glyph is drawn for this one (PD's
 * xblaFontSourceIndex): where the ROM draws a lowercase letter with the very
 * texels of its capital (a font written in capitals), the release's capital is
 * drawn too, not its lowercase in a capital's cell. Caller holds lock.
 */
static int32_t xfSourceIndex(int32_t font, int32_t index)
{
    const int32_t ch = index + XF_FIRST_CHAR;
    if (ch < 'a' || ch > 'z') {
        return index;
    }
    const int32_t upper = index - ('a' - 'A');
    const struct romglyph *lo = &rom[font][index];
    const struct romglyph *up = &rom[font][upper];
    if (!lo->pix || !up->pix || lo->w != up->w || lo->h != up->h || lo->baseline != up->baseline
            || memcmp(lo->pix, up->pix, (size_t)lo->stride * lo->h) != 0) {
        return index;
    }
    return upper;
}

/* ------------------------------------------------------------ the release's */

static void xfForgetBuilt(void)
{
    for (int32_t f = 0; f < XBLAFONT_NUM; f++) {
        for (int32_t i = 0; i < XF_NUM_CHARS; i++) {
            free(pics[f][i].rgba);
        }
    }
    memset(pics, 0, sizeof(pics));
    memset(lines, 0, sizeof(lines));
    numBuilt = numOffLine = numMissing = 0;
}

int xblaFontRescan(void)
{
    int n = 0;
    pthread_mutex_lock(&lock);
    xfForgetBuilt();
    for (int32_t f = 0; f < XBLAFONT_NUM; f++) {
        free(rel[f].alpha);
        memset(&rel[f], 0, sizeof(rel[f]));
        char p[1024];
        struct stat st;
        snprintf(p, sizeof(p), "%s/font/%s.txt", sysResolvePath(TEXPACK_XBLA_DIR), relNames[f]);
        if (stat(p, &st) == 0) {
            snprintf(p, sizeof(p), "%s/font/%s.png", sysResolvePath(TEXPACK_XBLA_DIR), relNames[f]);
            if (stat(p, &st) == 0) {
                n++;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return n;
}

static struct relfont *xfOpen(int32_t f)
{
    struct relfont *rf = &rel[f];
    char p[1024];
    if (rf->tried) {
        return rf->tried > 0 ? rf : NULL;
    }
    rf->tried = -1;

    snprintf(p, sizeof(p), "%s/font/%s.txt", sysResolvePath(TEXPACK_XBLA_DIR), relNames[f]);
    FILE *fp = fopen(p, "r");
    if (!fp) {
        return NULL;
    }
    int32_t W = 0, H = 0, count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int ch, left, top, w, h, x0, y0, adv;
        if (sscanf(line, "size %d %d", &W, &H) == 2) {
            continue;
        }
        if (sscanf(line, "g %x %d %d %d %d %d %d %d", &ch, &left, &top, &w, &h, &x0, &y0, &adv) == 8
                && ch >= XF_FIRST_CHAR && ch < XF_FIRST_CHAR + XF_NUM_CHARS && w > 0 && h > 0 && x0 >= 0
                && y0 >= 0) {
            struct relglyph *g = &rf->g[ch - XF_FIRST_CHAR];
            g->ok = 1;
            g->x0 = x0;
            g->y0 = y0;
            g->w = w;
            g->h = h;
            g->left = left;
            g->top = top;
            g->adv = adv;
            count++;
        }
    }
    fclose(fp);

    snprintf(p, sizeof(p), "%s/font/%s.png", sysResolvePath(TEXPACK_XBLA_DIR), relNames[f]);
    int32_t w = 0, h = 0;
    uint8_t *rgba = pngRead(p, &w, &h);
    if (!rgba || w != W || h != H || count == 0) {
        free(rgba);
        sysLogPrintf(LOG_WARNING, "xblafont: %s (%s) did not read", relNames[f], romNames[f]);
        return NULL;
    }
    rf->alpha = malloc((size_t)w * h);
    if (!rf->alpha) {
        free(rgba);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rf->alpha[i] = rgba[i * 4 + 3];
    }
    free(rgba);
    rf->W = w;
    rf->H = h;
    for (int32_t i = 0; i < XF_NUM_CHARS; i++) {
        struct relglyph *g = &rf->g[i];
        if (g->ok && (g->x0 + g->w > w || g->y0 + g->h > h)) {
            g->ok = 0;
        }
    }
    rf->tried = 1;
    sysLogPrintf(LOG_NOTE, "xblafont: the release's %s (%s): %dx%d atlas, %d glyphs", romNames[f], relNames[f],
                 w, h, count);
    return rf;
}

/* The ink inside a release glyph's box, in atlas pixels, measured as the
 * ROM's is (PD's xblaFontCellInk). */
static int xfRelInk(const struct relfont *rf, const struct relglyph *g, struct xfbox *ink)
{
    if (!g->ok) {
        return 0;
    }
    float *spans = calloc((size_t)(g->w + g->h), sizeof(float));
    if (!spans) {
        return 0;
    }
    for (int32_t y = 0; y < g->h; y++) {
        const uint8_t *row = &rf->alpha[(size_t)(g->y0 + y) * rf->W + g->x0];
        for (int32_t x = 0; x < g->w; x++) {
            spans[g->w + y] += row[x];
            if (row[x] > spans[x]) {
                spans[x] = row[x];
            }
        }
    }
    xfSpan(spans, g->w, &ink->x1, &ink->x2);
    xfSpan(spans + g->w, g->h, &ink->y1, &ink->y2);
    free(spans);
    if (ink->x2 <= ink->x1 || ink->y2 <= ink->y1) {
        return 0;
    }
    ink->x1 += g->x0;
    ink->x2 += g->x0;
    ink->y1 += g->y0;
    ink->y2 += g->y0;
    return 1;
}

/* ------------------------------------------------------------------ the line */

/* How far across the ROM drew the font: the middle of what its ink boxes
 * allow against what the release's glyphs ask for at the line's scale (PD's
 * xblaFontBuildCond). Caller holds lock. */
static float xfBuildCond(int32_t f, const struct relfont *rf, float scale)
{
    float ratios[XF_NUM_CHARS];
    int32_t num = 0;

    for (int32_t index = 0; index < XF_NUM_CHARS; index++) {
        struct xfbox body, ink;
        if (!xfRomInk(&rom[f][index], &body)) {
            continue;
        }
        const struct relglyph *g = &rf->g[xfSourceIndex(f, index)];
        if (!xfRelInk(rf, g, &ink)) {
            continue;
        }
        const float want = scale * (ink.x2 - ink.x1);
        if (want <= 0) {
            continue;
        }
        const float r = (body.x2 - body.x1) / want;
        int32_t i;
        for (i = num; i > 0 && ratios[i - 1] > r; i--) {
            ratios[i] = ratios[i - 1];
        }
        ratios[i] = r;
        num++;
    }
    if (num < XF_COND_MIN_CHARS) {
        return 1;
    }
    const int32_t j = num / 2;
    return (num & 1) ? ratios[j] : (ratios[j - 1] + ratios[j]) * 0.5f;
}

/**
 * The line through the two fonts' ink, `rom = scale * release + offset`, in
 * the coordinate a ROM glyph's baseline is an offset into (rows down from the
 * line's top) against the release's rows down from its baseline. Every pair of
 * anchors far enough apart to be a cap line against a baseline proposes a
 * line; the one most anchors sit on wins and is re-taken by least squares over
 * those anchors, twice (PD's xblaFontBuildLine). Caller holds lock.
 */
static int xfBuildLine(struct xfline *out, int32_t f, const struct relfont *rf)
{
    float r0[2 * XF_NUM_CHARS], r1[2 * XF_NUM_CHARS];   /* rom, release */
    int32_t num = 0;

    for (int32_t index = 0; index < XF_NUM_CHARS; index++) {
        const struct romglyph *rg = &rom[f][index];
        struct xfbox body, ink;
        if (!xfRomInk(rg, &body)) {
            continue;
        }
        const struct relglyph *g = &rf->g[xfSourceIndex(f, index)];
        if (!xfRelInk(rf, g, &ink)) {
            continue;
        }
        r0[num] = rg->baseline + body.y1;
        r1[num] = (ink.y1 - g->y0) - g->top;
        num++;
        r0[num] = rg->baseline + body.y2;
        r1[num] = (ink.y2 - g->y0) - g->top;
        num++;
    }
    out->total = num;
    if (num < XF_FIT_MIN_ANCHORS) {
        return 0;
    }
    float lo = r1[0], hi = r1[0];
    for (int32_t i = 1; i < num; i++) {
        if (r1[i] < lo) lo = r1[i];
        if (r1[i] > hi) hi = r1[i];
    }
    const float spread = (hi - lo) * XF_FIT_MIN_SPREAD;
    if (spread <= 0) {
        return 0;
    }
    float bestscale = 0, bestoffset = 0;
    int32_t bestcount = 0;
    for (int32_t i = 0; i < num; i++) {
        for (int32_t j = i + 1; j < num; j++) {
            const float d = r1[j] - r1[i];
            if (d < spread && -d < spread) {
                continue;
            }
            const float scale = (r0[j] - r0[i]) / d;
            const float offset = r0[i] - scale * r1[i];
            if (scale <= 0) {
                continue;
            }
            int32_t count = 0;
            for (int32_t k = 0; k < num; k++) {
                const float r = scale * r1[k] + offset - r0[k];
                if (r < XF_FIT_TOL && -r < XF_FIT_TOL) {
                    count++;
                }
            }
            if (count > bestcount) {
                bestcount = count;
                bestscale = scale;
                bestoffset = offset;
            }
        }
    }
    if (bestcount < XF_FIT_MIN_ANCHORS) {
        return 0;
    }
    for (int32_t pass = 0; pass < 2; pass++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int32_t count = 0;
        for (int32_t k = 0; k < num; k++) {
            const float r = bestscale * r1[k] + bestoffset - r0[k];
            if (r >= XF_FIT_TOL || -r >= XF_FIT_TOL) {
                continue;
            }
            sx += r1[k];
            sy += r0[k];
            sxx += (double)r1[k] * r1[k];
            sxy += (double)r1[k] * r0[k];
            count++;
        }
        const double den = count * sxx - sx * sx;
        if (count < XF_FIT_MIN_ANCHORS || den <= 0) {
            break;
        }
        bestscale = (float)((count * sxy - sx * sy) / den);
        bestoffset = (float)((sy - bestscale * sx) / count);
        bestcount = count;
    }
    out->scale = bestscale;
    out->offset = bestoffset;
    out->anchors = bestcount;
    out->cond = xfBuildCond(f, rf, bestscale);
    sysLogPrintf(LOG_NOTE, "xblafont: %s sits on the release's %s at rom = %.4f x + %.3f (%d of %d anchors), "
                 "condensed to %.3f", romNames[f], relNames[f], bestscale, bestoffset, bestcount, num, out->cond);
    return 1;
}

/* --------------------------------------------------------------- the picture */

/* One destination pixel's worth of the atlas: the mean alpha over the source
 * rectangle it covers, never narrower than a source pixel (PD's xblaFontArea). */
static uint8_t xfArea(const struct relfont *rf, float x0, float x1, float y0, float y1)
{
    float sum = 0, weight = 0;
    for (int32_t iy = (int32_t)floorf(y0); iy <= (int32_t)floorf(y1 - 0.0001f); iy++) {
        const float ry = ((iy + 1 < y1) ? iy + 1 : y1) - ((iy > y0) ? iy : y0);
        if (iy < 0 || iy >= rf->H || ry <= 0) {
            continue;
        }
        for (int32_t ix = (int32_t)floorf(x0); ix <= (int32_t)floorf(x1 - 0.0001f); ix++) {
            const float rx = ((ix + 1 < x1) ? ix + 1 : x1) - ((ix > x0) ? ix : x0);
            if (ix < 0 || ix >= rf->W || rx <= 0) {
                continue;
            }
            sum += rf->alpha[(size_t)iy * rf->W + ix] * rx * ry;
            weight += rx * ry;
        }
    }
    return weight > 0 ? (uint8_t)(sum / weight + 0.5f) : 0;
}

/* The columns a glyph fills: `wide` centred on the ROM's own ink and slid back
 * inside the band the game samples (PD's xblaFontFillAcross). */
static void xfFillAcross(struct xfbox *dst, const struct xfbox *body, float wide, float bandleft, float bandright)
{
    const float mid = (body->x1 + body->x2) * 0.5f;
    if (wide > bandright - bandleft) {
        wide = bandright - bandleft;
    }
    dst->x1 = mid - wide * 0.5f;
    if (dst->x1 < bandleft) {
        dst->x1 = bandleft;
    }
    if (dst->x1 + wide > bandright) {
        dst->x1 = bandright - wide;
    }
    dst->x2 = dst->x1 + wide;
}

/* Builds one glyph's picture (PD's xblaFontBuildBody, GoldenEye's tile: the
 * quad samples the tile's columns 0..width and rows 0..height). Caller holds
 * lock. */
static int xfBuild(struct xfpic *out, int32_t f, int32_t index, const struct relfont *rf, const struct xfline *line)
{
    const struct romglyph *rg = &rom[f][index];
    struct xfbox body, ink, dst;
    static int logOn = -1;

    if (logOn < 0) {
        logOn = getenv("GE_XBLAFONTLOG") != NULL;
    }
    if (!xfRomInk(rg, &body)) {
        return 0;
    }
    const struct relglyph *g = &rf->g[xfSourceIndex(f, index)];
    if (!xfRelInk(rf, g, &ink)) {
        numMissing++;
        return 0;
    }
    dst = body;
    int offline = 0;
    {
        const float bandtop = 0, bandbot = (float)rg->h;
        const float bandleft = 0, bandright = (float)rg->w;
        const float room = bandbot - bandtop;
        const float top = line->scale * ((ink.y1 - g->y0) - g->top) + line->offset - rg->baseline;
        const float bot = line->scale * ((ink.y2 - g->y0) - g->top) + line->offset - rg->baseline;
        const float want = bot - top;
        const float over = want > room ? want - room : 0;
        const float high = want - over;
        float y = top;

        if (want <= 0) {
            return 0;
        }
        if (y + high > bandbot) {
            y = bandbot - high;
        }
        if (y < bandtop) {
            y = bandtop;
        }
        if (over <= XF_FIT_NUDGE && y - top <= XF_FIT_NUDGE && top - y <= XF_FIT_NUDGE) {
            const float asks = line->scale * line->cond * (ink.x2 - ink.x1);
            dst.y1 = y;
            dst.y2 = y + high;
            xfFillAcross(&dst, &body, asks > body.x2 - body.x1 ? asks : body.x2 - body.x1, bandleft, bandright);
        } else {
            /* the line cannot place it: shrunk into the tile, not squashed */
            xfFillAcross(&dst, &body, line->scale * (ink.x2 - ink.x1) * (high / want), bandleft, bandright);
            dst.y1 = y;
            dst.y2 = y + high;
            offline = 1;
            numOffLine++;
        }
    }

    int32_t scale = (int32_t)ceilf((ink.x2 - ink.x1) / (dst.x2 - dst.x1) - 0.001f);
    const int32_t sy = (int32_t)ceilf((ink.y2 - ink.y1) / (dst.y2 - dst.y1) - 0.001f);
    if (sy > scale) {
        scale = sy;
    }
    if (scale < XF_MIN_SCALE) {
        scale = XF_MIN_SCALE;
    } else if (scale > XF_MAX_SCALE) {
        scale = XF_MAX_SCALE;
    }

    out->w = rg->stride * scale;
    out->h = rg->h * scale;
    out->rgba = calloc((size_t)out->w * out->h, 4);
    if (!out->rgba) {
        return 0;
    }
    int32_t px0 = (int32_t)(dst.x1 * scale + 0.5f);
    int32_t py0 = (int32_t)(dst.y1 * scale + 0.5f);
    int32_t px1 = (int32_t)(dst.x2 * scale + 0.5f);
    int32_t py1 = (int32_t)(dst.y2 * scale + 0.5f);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > rg->w * scale) px1 = rg->w * scale;
    if (py1 > out->h) py1 = out->h;
    if (px1 <= px0 || py1 <= py0) {
        free(out->rgba);
        out->rgba = NULL;
        return 0;
    }
    const float stepx = (ink.x2 - ink.x1) / (float)(px1 - px0);
    const float stepy = (ink.y2 - ink.y1) / (float)(py1 - py0);
    const float halfx = (stepx > 1 ? stepx : 1) * 0.5f;
    const float halfy = (stepy > 1 ? stepy : 1) * 0.5f;
    for (int32_t y = py0; y < py1; y++) {
        const float sy2 = ink.y1 + (y - py0 + 0.5f) * stepy;
        for (int32_t x = px0; x < px1; x++) {
            const float sx = ink.x1 + (x - px0 + 0.5f) * stepx;
            const uint8_t a = xfArea(rf, sx - halfx, sx + halfx, sy2 - halfy, sy2 + halfy);
            uint8_t *p = out->rgba + ((size_t)y * out->w + x) * 4;
            /* the ROM's I8: colour and alpha are both the coverage */
            p[0] = p[1] = p[2] = p[3] = a;
        }
    }
    numBuilt++;
    if (logOn) {
        sysLogPrintf(LOG_NOTE, "xblafont: %s '%c' rom %dx%d base %d ink x %.2f-%.2f y %.2f-%.2f -> "
                     "x %.2f-%.2f y %.2f-%.2f at %dx%s", romNames[f], index + XF_FIRST_CHAR, rg->w, rg->h,
                     rg->baseline, body.x1, body.x2, body.y1, body.y2, dst.x1, dst.x2, dst.y1, dst.y2, scale,
                     offline ? " (off the line: shrunk)" : "");
    }
    return 1;
}

uint8_t *xblaFontPicture(int32_t font, int32_t index, int32_t *outW, int32_t *outH)
{
    static int disabled = -1;
    uint8_t *copy = NULL;

    if (disabled < 0) {
        const char *e = getenv("GE_XBLAFONT");   /* GE_XBLAFONT=0: the ROM's glyphs (A/B) */
        disabled = e != NULL && e[0] == '0';
    }
    if (disabled || font < 0 || font >= XBLAFONT_NUM || index < 0 || index >= XF_NUM_CHARS) {
        return NULL;
    }
    pthread_mutex_lock(&lock);
    if (builtGen != romGen) {
        xfForgetBuilt();
        builtGen = romGen;
    }
    struct xfpic *pic = &pics[font][index];
    if (!pic->tried) {
        pic->tried = 1;
        const struct relfont *rf = xfOpen(font);
        struct xfline *line = &lines[font];
        if (rf && !line->tried) {
            line->tried = xfBuildLine(line, font, rf) ? 1 : -1;
            if (line->tried < 0) {
                sysLogPrintf(LOG_WARNING, "xblafont: %s did not fit the release's %s (%d anchors): ROM glyphs kept",
                             romNames[font], relNames[font], line->total);
            } else if (getenv("GE_XBLAFONTLOG") != NULL) {
                /* dev: build the whole font now and say which characters
                 * left the line (shrunk) or have no picture */
                char off[XF_NUM_CHARS + 1], none[XF_NUM_CHARS + 1];
                int32_t no = 0, nn = 0, nb = 0;
                const int32_t o0 = numOffLine;
                for (int32_t i = 0; i < XF_NUM_CHARS; i++) {
                    struct xfpic *q = &pics[font][i];
                    const int32_t before = numOffLine;
                    if (!q->tried || q == pic) {
                        q->tried = 1;
                        xfBuild(q, font, i, rf, line);
                    }
                    if (q->rgba) {
                        nb++;
                        if (numOffLine != before) off[no++] = (char)(i + XF_FIRST_CHAR);
                    } else {
                        none[nn++] = (char)(i + XF_FIRST_CHAR);
                    }
                }
                off[no] = none[nn] = 0;
                sysLogPrintf(LOG_NOTE, "xblafont: %s: %d of 94 built; shrunk off the line (%d): %s; no picture (%d): %s",
                             romNames[font], nb, numOffLine - o0, off, nn, none);
            }
        }
        if (rf && line->tried > 0 && !pic->rgba) {
            xfBuild(pic, font, index, rf, line);
            if ((numBuilt + numMissing) % 32 == 0 && numBuilt > 0) {
                sysLogPrintf(LOG_NOTE, "xblafont: %d glyphs built (%d shrunk off the line, %d not in the release)",
                             numBuilt, numOffLine, numMissing);
            }
        }
    }
    if (pic->rgba) {
        const size_t n = (size_t)pic->w * pic->h * 4;
        copy = malloc(n);
        if (copy) {
            memcpy(copy, pic->rgba, n);
            *outW = pic->w;
            *outH = pic->h;
        }
    }
    pthread_mutex_unlock(&lock);
    return copy;
}
