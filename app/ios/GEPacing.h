// GEPacing.h — the present gate AND the pacer. Design: docs/pacing.md.
//
// Two jobs, both living at the same call site (gfx_sdl_swap_buffers_begin, on
// the game's SCHEDULER thread, once per frame):
//
//   1. the LIFECYCLE gate (round 7b): while the app is backgrounded the
//      CAMetalLayer's drawables belong to the system and an eglSwapBuffers
//      into one is the family's classic "looks like a port bug, is a
//      lifecycle bug" crash;
//   2. the PACER (round 8a): block until the CADisplayLink says the next
//      frame is due, so there is exactly one present per link callback and
//      the app declares its rate instead of discovering it by spinning.
//
// Blocking the scheduler thread here is safe in THIS port and would not be in
// Perfect Dark: GoldenEye's host main thread only pumps SDL events, so UIKit
// keeps dispatching touches for the whole of the wait. docs/pacing.md §3.1.
#pragma once

#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 1 = present now, 0 = skip this present. Blocks until the link fires (or
 *  250 ms, whichever comes first). RENDER THREAD ONLY. */
int geIosPacingWaitForPresent(void);

/** Round 9c repeat-present (D-029, docs/pacing.md §repeat-present). Wait for
 *  the NEXT link callback, at most timeoutUs. 1 = one arrived, so the caller
 *  may present the last finished frame again. Unlike WaitForPresent this does
 *  not touch the cadence ring (a repeat is not a frame) and it never blocks
 *  longer than the caller's own build deadline. RENDER THREAD ONLY. */
int geIosPacingWaitForRepeat(unsigned timeoutUs);

/** The LIFECYCLE half of the gate on its own: 1 = the app is in the
 *  foreground and a swap is safe. A repeat-present has already waited for its
 *  display refresh and must not consume a pacer credit. */
int geIosPacingGateOpen(void);

/** D-066. engine/port/src/video.c videoStartFrame(): the instant the
 *  scheduler thread began building this frame, i.e. the moment the VI retrace
 *  released it. `build` in pacing.log is measured from here, so it reports the
 *  frame's CPU cost; `period` is the wall-clock frame period. RENDER THREAD
 *  ONLY. */
void geIosPacingNoteFrameStart(void);

/** Round 28 (doc 38 item 3): port/fast3d/gfx_pc.cpp gfx_start_frame, for the
 *  frame before it: GL draw calls and triangles (pacing.log `draws`/`tris`). */
void geIosPacingNoteDraws(uint32_t draws, uint32_t tris);

/** Main thread, from the UIApplication/UIScene background notifications. */
void geIosPacingSetForeground(int on);

// Round 23 (R23-4): a shell page (Settings) is up; the main thread waits in
// the run loop rather than on the bare credit semaphore so UIKit stays live.
void geIosPacingSetUiActive(int on);

// Round 23 (R23-5): port/src/video.c portRenderThreadProc, GE_IOS. Enter sets
// the render thread's QoS and joins it to the frame work interval; Exit
// leaves it.
/** Phase 6 M3 (visionOS 3D, PD D-050): the compositor as the clock. The
 *  immersive loop turns it on at its start and off at its end; while on, the
 *  display link is paused and SignalExternal (compositor thread, after
 *  cp_time_wait_until) releases the present credit. periodSec = the
 *  compositor's frame period (0 = unknown). */
void geIosPacingSetExternal(int on);
void geIosPacingSignalExternal(double periodSec);
/** "compositor" while the external clock is on, else "displaylink". */
const char *geIosPacingModeName(void);

void geIosPacingRenderThreadEnter(void);
void geIosPacingRenderThreadExit(void);

// Round 26 (docs/port-ios/35-round26-simthread.md): the game loop on its own
// thread. port/src/main.c calls Start in place of mainproc(NULL) (1 = the
// game now runs on ge-sim and geEngineMain returns to UIKit's run loop);
// video.c asks Wanted (the render thread is always on under it). The ENGINE
// LOCK serialises engine code on ge-sim with everything the main thread does
// (UIKit callbacks, main-queue blocks, SDL event watches): the main thread
// holds it whenever its run loop is not asleep, ge-sim whenever it is not in
// one of its waits (YieldBegin/End, pcsched.c's pacing sleep and the present
// credit). Begin returns a token for End; both are no-ops on any other thread.
int  geIosSimThreadWanted(void);
int  geIosSimThreadStart(void (*entry)(void *));
int  geIosSimYieldBegin(void);
void geIosSimYieldEnd(int token);
// UIKit-bound SDL calls the engine makes from ge-sim (video.c, input_pc.c,
// GE_IOS macros): hopped to the main queue (async), or, for the message box
// (SDL dispatch_syncs it to main itself), made with the engine lock released.
void geIosSdlStartTextInput(void);
void geIosSdlStopTextInput(void);
int  geIosSdlSetRelativeMouseMode(int on);
int  geIosSdlShowSimpleMessageBox(unsigned int flags, const char *title, const char *message, void *window);

#ifdef __cplusplus
}
#endif

@interface GEPacing : NSObject

/// Start the display link on its own thread. Main thread, once, after the
/// engine is up (the link's rate depends on nothing the engine owns, but
/// starting it before there is anything to present just burns callbacks).
+ (void)start;

/// Re-read `Pacing.TargetFPS` and retune the live display link. The settings
/// page calls this; nothing else should. Main thread.
///
/// The link's own `preferredFrameRateRange` is changed on the link's thread,
/// which is safe. Turning repeat-present ON is NOT done here: it creates a GL
/// target and an EGL context is bound per thread, so it is handed to the
/// render thread through the same one-shot that already owns
/// eglSwapInterval(0) (D-029). A player who moves this row therefore sees the
/// link rate change at once and the repeats begin on the next present.
+ (void)applyTargetFPS;
#if TARGET_OS_VISION
/** Round 36, dev bridge `pacing grant <ms>|off`: pretend visionOS grants this
 *  link period (0 = the link's own again), to exercise the rate adoption. */
+ (void)visionFakeGrantMs:(double)ms;
#endif

/// Hz the link is asked for, and Hz the engine can actually honour. They are
/// asserted equal by scripts/sim-validate.sh (docs/pacing.md §3.4).
+ (int)targetHz;
+ (int)engineHz;
/// The SIM rate, always 60. Round 9c: engineHz became the PRESENT rate the
/// engine can honour (repeat-present), so the tick rate needs its own name.
+ (int)simHz;

/// Reset the cadence statistics (the bridge's `pacing reset`).
+ (void)resetStats;
/// `key=value` lines for the bridge's `pacing`.
+ (NSString *)statsDescription;

/// Seconds since the previous link callback, for anything that integrates
/// over real time (GEController's look).
+ (double)lastFrameDelta;

@end
