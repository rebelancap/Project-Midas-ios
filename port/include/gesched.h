/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/gesched.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_GESCHED_H
#define PORT_GESCHED_H

/*
 * gesched — the sim tick, the seeded replay harness and the determinism
 * instruments. Round 7a; design and rationale in docs/tick.md.
 *
 * The one rule this module exists to enforce: the SIM advances in whole
 * 1/60 s ticks (the N64's own unit), and how many ticks a rendered frame
 * carries is decided HERE, not by however long the host took to get here.
 *
 *   - normal mode : a host-clock accumulator with the remainder carried, so
 *                   the long-run tick rate is exactly 60/s at any present
 *                   rate. May yield 0 ticks for a frame (GE guards
 *                   g_ClockTimer == 0 everywhere; that is the pause path).
 *   - --fixed-step: exactly ONE tick per rendered frame, no clock read at
 *                   all. Frame k is tick k on every machine, so a replay is
 *                   defined by (seed, input, stage) and nothing else.
 *
 * Everything here is port-layer. The four game-code touch points are
 * #ifdef PORT one-liners that call into this file:
 *   src/game/frametiming.c  waitForNextFrame()  -> geTickNextDelta()
 *   src/boss.c:525          main-loop gate      -> geTickForceMainGate()
 *   src/boss.c:412          randomSetSeed()     -> geTickRngSeed()
 *   src/joy.c               (untouched: we use joySetPlaybackFunc/RecordFunc)
 */

#include <stdint.h>
#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the CLI / ini. Call once from main() AFTER configLoad() and BEFORE
 * the game thread starts. */
void geTickInit(void);

/* True when --fixed-step (or Tick.FixedStep=1) is in force. */
int  geTickIsFixedStep(void);

/* True under Tick.Legacy / --legacy-tick: geTickNextDelta() returns -1 and
 * waitForNextFrame() runs the pre-round-7a (elapsed + Q/2)/Q spin instead.
 * The A/B reference for M-002. */
int  geTickIsLegacy(void);

/* True when --no-sound was given (main.c skips audioInit/mixerInit). */
int  geTickNoSound(void);

/* How many 1/60 s sim ticks this rendered frame carries. Called from
 * waitForNextFrame() exactly once per frame. May return 0, or -1 meaning
 * "use the legacy path". */
s32  geTickNextDelta(void);

/* How many sim ticks the frame currently being BUILT carries, i.e. the value
 * geTickNextDelta() last returned. 0 means a render-only frame: no sim time
 * elapsed, g_ClockTimer is 0, and per-tick state was not refreshed. The
 * replay hooks read it so that input stays indexed by TICK rather than by
 * rendered frame, and videoEndFrame reads it to decide whether the frame is
 * part of the `gfx:` stream (round 8c, D-022). */
s32  geTickCurrentDelta(void);

/* boss.c:525's `mainTickElapsed >= MAIN_LOOP_TICK_INTERVAL` gate. Always 1:
 * one retrace = one frame build; how much sim happens inside it is
 * geTickNextDelta()'s business alone. Kept as a function so the reason is
 * greppable from the call site. */
int  geTickForceMainGate(void);

/* Replaces boss.c:412's `randomSetSeed(osGetCount())`. Returns the seed to
 * use; also seeds g_chrObjRandomSeed from the same value. `wallCount` is the
 * osGetCount() value the N64 path would have used. */
u32  geTickRngSeed(u32 wallCount);

/* Called from videoEndFrame() once per PRESENTED frame, with the port's
 * running frame number. Emits the `gfx:` instrument line, honours
 * --screenshot-frame and --exit-frame. */
void geTickOnFrameRendered(uint32_t frameNo);

/* D-103 (round 20a): GE_STACKFUZZ=<byte> fills 512 KiB of the game thread's
 * stack below the main loop with <byte> before every frame build, so any
 * read of a stack slot the current call chain did not write sees that byte.
 * Two runs with different bytes and identical gfx:/sim: streams prove the
 * replay does not depend on stale stack (replay-check.sh --stackfuzz).
 * Unset: one cached getenv and a return. */
void geStackFuzz(void);

/* The sim's tick period in microseconds (16667 NTSC). Also the BUILD cadence:
 * the retrace pacemaker posts at this rate and one retrace runs one queued
 * gfx task. The repeat-present pump's deadline is derived from this. */
uint64_t geTickIntervalUs(void);

/* When the sim owes its next tick, in sysGetMicroseconds() units — the D-019
 * accumulator's own state read out, not a second model of it. 0 means "do not
 * repeat-present": --fixed-step (no wall clock in the tick, by design) and
 * --legacy-tick both return it. Read by videoRepeatPresentPump()
 * (docs/pacing.md §repeat-present R3). */
uint64_t geTickNextBuildDueUs(void);

/* GE_TIMEAUDIT: count a wall-clock read that is on a sim path.
 * 0 = osGetCount from a sim site, 1 = osGetTime from a sim site,
 * 2 = the accumulator's own host-clock read (expected non-zero in normal
 * mode, expected ZERO under --fixed-step). */
void geTimeAuditHit(int which);
int  geTimeAuditEnabled(void);

/* -------- the gfx stream instrument (implemented in port/fast3d/gfx_pc.cpp) */
/* Enabled by --log. Reset at the top of each gfx_run(). */
extern int       gfx_stream_enable;
extern uint64_t  gfx_stream_hash;   /* FNV-1a 64 over normalised command words */
extern uint32_t  gfx_stream_cmds;
extern uint32_t  gfx_stream_draws;  /* gfx_flush()es that actually drew */
extern uint32_t  gfx_stream_tris;

#ifdef __cplusplus
}
#endif

#endif /* PORT_GESCHED_H */
