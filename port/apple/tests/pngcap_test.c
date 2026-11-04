/*
 * Round 38 host test (docs/port-ios/51-round38-hdpack.md): pngReadMaxDim()
 * against pngRead() + a reference box filter written here from the rule in
 * pngread.c, independently of its code:
 *
 *   k   = the smallest integer with ceil(max(w, h) / k) <= cap
 *   out = max(1, w / k) x max(1, h / k); source columns/rows past the last
 *         whole box are dropped, a side shorter than k is one box of its mean
 *   rgb = round(sum(c * a) / sum(a)), or round(mean c) when sum(a) == 0
 *   a   = round(mean a)
 *
 * Part 1 encodes synthetic images (pngwrite.c: RGB and RGBA, odd sizes, a side
 * shorter than k, fully transparent boxes, k = 1..5) and must match exactly.
 * Part 2 (optional: a directory argument) decodes every PNG under it whose
 * larger side exceeds the cap (real files: every filter type, many IDAT
 * chunks) and must match exactly too.
 *
 * usage: port/apple/tests/run_pngcap_test.sh [pack dir] [cap]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "pngread.h"
#include "pngwrite.h"

extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);
extern int memcmp(const void *, const void *, size_t);
extern char *strrchr(const char *, int);
extern int snprintf(char *, size_t, const char *, ...);

void sysLogPrintf(enum LogLevel level, const char *fmt, ...)
{
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int failures;

static u8 *refBox(const u8 *src, s32 w, s32 h, s32 cap, s32 *ow, s32 *oh)
{
    const s32 side = w > h ? w : h;
    s32 k = 1;
    while ((side + k - 1) / k > cap) k++;
    *ow = w / k > 0 ? w / k : 1;
    *oh = h / k > 0 ? h / k : 1;
    u8 *out = malloc((size_t)*ow * *oh * 4);
    for (s32 oy = 0; oy < *oh; oy++) {
        for (s32 ox = 0; ox < *ow; ox++) {
            unsigned long long ca[3] = { 0 }, c[3] = { 0 }, a = 0, n = 0;
            const s32 y1 = (*oh == 1 && h < k) ? h : oy * k + k;
            const s32 x1 = (*ow == 1 && w < k) ? w : ox * k + k;
            for (s32 y = oy * k; y < y1; y++) {
                for (s32 x = ox * k; x < x1; x++) {
                    const u8 *p = src + ((size_t)y * w + x) * 4;
                    for (int i = 0; i < 3; i++) {
                        ca[i] += (unsigned long long)p[i] * p[3];
                        c[i] += p[i];
                    }
                    a += p[3];
                    n++;
                }
            }
            u8 *d = out + ((size_t)oy * *ow + ox) * 4;
            for (int i = 0; i < 3; i++) {
                d[i] = (u8)(a ? (ca[i] + a / 2) / a : (c[i] + n / 2) / n);
            }
            d[3] = (u8)((a + n / 2) / n);
        }
    }
    return out;
}

static void check(const char *what, const char *path, s32 cap)
{
    s32 w, h, ow, oh, cw = 0, ch = 0, sw = 0, sh = 0;
    u8 *full = pngRead(path, &w, &h);
    u8 *capped = pngReadMaxDim(path, cap, &cw, &ch, &sw, &sh);
    if (!full || !capped) {
        printf("FAIL %s: decode failed (%s)\n", what, path);
        failures++;
        free(full);
        free(capped);
        return;
    }
    u8 *ref = refBox(full, w, h, cap, &ow, &oh);
    if (sw != w || sh != h || cw != ow || ch != oh || memcmp(ref, capped, (size_t)ow * oh * 4) != 0) {
        size_t diff = 0;
        if (cw == ow && ch == oh) {
            for (size_t i = 0; i < (size_t)ow * oh * 4; i++) diff += ref[i] != capped[i];
        }
        printf("FAIL %s: %dx%d cap %d -> got %dx%d (src %dx%d), want %dx%d, %zu bytes differ\n",
               what, w, h, cap, cw, ch, sw, sh, ow, oh, diff);
        failures++;
    }
    free(full);
    free(capped);
    free(ref);
}

static u32 rng = 12345;
static u8 rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (u8)(rng >> 24);
}

static void synthetic(const char *dir)
{
    static const s32 sizes[][2] = {
        { 64, 64 }, { 67, 41 }, { 100, 3 }, { 3, 100 }, { 129, 130 }, { 1, 50 }, { 50, 1 }, { 255, 17 },
    };
    static const s32 caps[] = { 64, 40, 33, 20, 16, 7 };
    char path[512];
    int n = 0;
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (int ch = 3; ch <= 4; ch++) {
            const s32 w = sizes[si][0], h = sizes[si][1];
            u8 *px = malloc((size_t)w * h * ch);
            for (s32 i = 0; i < w * h; i++) {
                for (int c = 0; c < ch; c++) px[i * ch + c] = rnd();
                if (ch == 4 && (i / w) % 5 == 0) px[i * ch + 3] = 0;       /* transparent rows */
                if (ch == 4 && (i % w) % 7 == 1) px[i * ch + 3] = 255;     /* opaque columns */
            }
            snprintf(path, sizeof(path), "%s/syn_%dx%d_%d.png", dir, w, h, ch);
            if (!pngWrite(path, px, w, h, ch, 0)) {
                printf("FAIL could not write %s\n", path);
                failures++;
            }
            free(px);
            for (size_t ci = 0; ci < sizeof(caps) / sizeof(caps[0]); ci++) {
                char what[64];
                snprintf(what, sizeof(what), "synthetic %dx%dx%d cap %d", w, h, ch, caps[ci]);
                check(what, path, caps[ci]);
                n++;
            }
        }
    }
    printf("part 1: %d synthetic cases\n", n);
}

static int packFiles, packChecked;

static void walk(const char *dir, s32 cap, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d || depth > 8) {
        if (d) closedir(d);
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char path[1024];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk(path, cap, depth + 1);
            continue;
        }
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".png") != 0) continue;
        packFiles++;
        FILE *f = fopen(path, "rb");
        u8 hdr[24];
        if (!f) continue;
        const size_t got = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        if (got < 24) continue;
        const s32 w = (s32)((hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19]);
        const s32 h = (s32)((hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23]);
        if ((w > h ? w : h) <= cap) continue;
        check(e->d_name, path, cap);
        packChecked++;
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/pngcap_test", tmp);
    mkdir(dir, 0755);
    synthetic(dir);
    if (argc > 1) {
        const s32 cap = argc > 2 ? atoi(argv[2]) : 2048;
        walk(argv[1], cap, 0);
        printf("part 2: %d PNGs under %s, %d above %d checked\n", packFiles, argv[1], packChecked, cap);
    }
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
}
