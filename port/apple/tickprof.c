/*
 * tickprof.c - round 18 (Phase 3a, docs/port-ios/27-round18-120hz.md): what a
 * sim tick and a rendered frame COST, as distributions, so the 120 Hz budget
 * (8.33 ms) can be read off a log instead of guessed from averages.
 *
 * Two clocks, both per-thread CPU time (CLOCK_THREAD_CPUTIME_ID: sleeps and
 * condvar waits cost nothing, a busy-wait would), plus one wall interval:
 *
 *   tick_cpu   the sim thread's CPU between consecutive sim ticks
 *              (portSimHashTick, once per tick). With the render thread OFF
 *              (Midas's auto rule: tickhz == 60) the sim thread also renders
 *              and presents, so this is the WHOLE engine per tick. With it ON
 *              (tickhz != 60) it is the game tick + display-list build only.
 *   frame_cpu  the presenting thread's CPU between consecutive frames
 *              (gfx_start_frame). Render thread ON: the renderer per frame.
 *   render     gfx_start_frame -> the present wait (gfx_sdl2.cpp
 *              swap_buffers_begin), wall: the interpreter + GL submission.
 *
 * Off unless asked: GE_TICKPROF=1 (any build: prints `ge: tickprof:` once a
 * second from the render thread) or geTickProfEnable(1) (the iOS shell's
 * pacing log reads it through geTickProfTake). It reads clocks and writes
 * only its own rings: nothing the sim hashes, so the headless digests cannot
 * move (and the headless build never calls the frame half).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* texpack.c (round 18): registration count / cost since the last take */
void texpackRegStatsTake(uint64_t *calls, uint64_t *ns);

#include "tickprof.h"

#define TP_RING 512u

typedef struct {
    double v[TP_RING];
    _Atomic unsigned n;   /* samples written since the last take */
} TpRing;

static TpRing s_tick, s_frame, s_render;
static int s_on = -1;          /* -1 = not resolved yet */
static int s_print;            /* GE_TICKPROF: print a line a second */

static double tpThreadMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static double tpWallMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static void tpNote(TpRing *r, double ms)
{
    unsigned i = atomic_fetch_add_explicit(&r->n, 1u, memory_order_relaxed);
    r->v[i % TP_RING] = ms;
}

static int tpOn(void)
{
    if (s_on < 0)
    {
        const char *e = getenv("GE_TICKPROF");
        s_print = (e != NULL && e[0] != '\0' && e[0] != '0');
        s_on = s_print;
    }
    return s_on;
}

void geTickProfEnable(int on)
{
    (void) tpOn();
    s_on = on ? 1 : s_print;
}

void geTickProfTick(void)
{
    static double last = -1.0;
    double now;

    if (!tpOn())
    {
        last = -1.0;
        return;
    }
    now = tpThreadMs();
    if (last >= 0.0)
    {
        tpNote(&s_tick, now - last);
    }
    last = now;
}

static double s_renderStart = -1.0;

void geTickProfFrameBegin(void)
{
    static double last = -1.0;
    double now;

    if (!tpOn())
    {
        last = -1.0;
        s_renderStart = -1.0;
        return;
    }
    now = tpThreadMs();
    if (last >= 0.0)
    {
        tpNote(&s_frame, now - last);
    }
    last = now;
    s_renderStart = tpWallMs();
}

static int tpCmp(const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

static void tpStats(TpRing *r, unsigned *count, double *p50, double *p95, double *mean)
{
    static double scratch[TP_RING];
    unsigned n = atomic_exchange_explicit(&r->n, 0u, memory_order_relaxed);
    unsigned k = n < TP_RING ? n : TP_RING;
    unsigned i;
    double sum = 0.0;

    *count = n;
    if (k == 0)
    {
        *p50 = *p95 = *mean = 0.0;
        return;
    }
    memcpy(scratch, r->v, k * sizeof(double));
    qsort(scratch, k, sizeof(double), tpCmp);
    for (i = 0; i < k; i++)
    {
        sum += scratch[i];
    }
    *p50 = scratch[(unsigned) ((double) (k - 1) * 0.50 + 0.5)];
    *p95 = scratch[(unsigned) ((double) (k - 1) * 0.95 + 0.5)];
    *mean = sum / (double) k;
}

void geTickProfTake(GeTickProf *out)
{
    memset(out, 0, sizeof(*out));
    tpStats(&s_tick, &out->ticks, &out->tickP50, &out->tickP95, &out->tickMean);
    tpStats(&s_frame, &out->frames, &out->frameP50, &out->frameP95, &out->frameMean);
    tpStats(&s_render, &out->renders, &out->renderP50, &out->renderP95, &out->renderMean);
}

void geTickProfFrameEnd(void)
{
    static double lastPrint = -1.0;
    double now;

    if (!tpOn() || s_renderStart < 0.0)
    {
        return;
    }
    now = tpWallMs();
    tpNote(&s_render, now - s_renderStart);
    s_renderStart = -1.0;
    if (!s_print)
    {
        return;
    }
    if (lastPrint < 0.0)
    {
        lastPrint = now;
        return;
    }
    if (now - lastPrint >= 1000.0)
    {
        GeTickProf p;
        double dt = (now - lastPrint) / 1000.0;

        uint64_t regCalls = 0, regNs = 0;

        geTickProfTake(&p);
        texpackRegStatsTake(&regCalls, &regNs);
        printf("ge: tickprof: ticks %.1f/s tick_cpu %.2f/%.2f (mean %.2f) ms  frames %.1f/s "
               "frame_cpu %.2f/%.2f (mean %.2f) ms  render %.2f/%.2f (mean %.2f) ms  "
               "texreg %.0f/s %.1f us/s\n",
               p.ticks / dt, p.tickP50, p.tickP95, p.tickMean, p.frames / dt,
               p.frameP50, p.frameP95, p.frameMean, p.renderP50, p.renderP95, p.renderMean,
               (double) regCalls / dt, (double) regNs / 1000.0 / dt);
        fflush(stdout);
        lastPrint = now;
    }
}
