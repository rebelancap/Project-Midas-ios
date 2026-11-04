/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/audiotrace.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
/*
 * audiotrace.h — serialized trace writer for the GE_AUDIOTRACE / WIRE probes.
 *
 * D202/M-70 (PORT): the probe sites live in TWO threads — sndPlaySfx runs on
 * the game thread, the sndp event handlers ([EVT]/[VOICE+]/[STOP-EVT]) and
 * load.c's [PASTEND] run on the audio thread. Each site used to open its own
 * unbuffered FILE* on the same path; concurrent fprintf() calls interleaved
 * MID-LINE and corrupted records, which M-69's offline matcher misread as
 * dropped voice allocations. One FILE* per path plus a spinlock around
 * vfprintf makes each line atomic.
 */
#ifndef PORT_AUDIOTRACE_H
#define PORT_AUDIOTRACE_H

void geTracePrintf(const char *path, const char *fmt, ...);

#endif /* PORT_AUDIOTRACE_H */
