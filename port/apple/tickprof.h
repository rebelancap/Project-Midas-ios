/* tickprof.h - round 18 per-tick / per-frame cost distributions (tickprof.c). */
#ifndef PORT_APPLE_TICKPROF_H
#define PORT_APPLE_TICKPROF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned ticks, frames, renders;          /* samples since the last take */
    double tickP50, tickP95, tickMean;        /* sim-thread CPU per tick, ms */
    double frameP50, frameP95, frameMean;     /* presenting-thread CPU per frame, ms */
    double renderP50, renderP95, renderMean;  /* gfx_start_frame -> present wait, wall ms */
} GeTickProf;

void geTickProfEnable(int on);
void geTickProfTick(void);        /* sim thread, once per sim tick */
void geTickProfFrameBegin(void);  /* presenting thread, gfx_start_frame */
void geTickProfFrameEnd(void);    /* presenting thread, before the present wait */
void geTickProfTake(GeTickProf *out);

#ifdef __cplusplus
}
#endif

#endif
