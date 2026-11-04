// GEVision3D.h — the visionOS 3D mode's seam, in one header.
//
// Phase 6 (docs/port-ios/64-phase6-stereo3d-plan.md). Copied from
// perfect-dark-ios app/vision3d/PDVision3D.h at its M1 shape (PD commit
// 3db8d80, D-047), pd -> ge, with the Midas deltas written in below.
//
// Everything in this file is visionOS-only: the whole header is gated on
// TARGET_OS_VISION and nothing in the iOS target includes it. The Swift side
// reaches all of it through app/vision3d/ge-vision-bridging.h.
//
// WHY THERE IS A SWIFT ENTRY POINT AT ALL (PD D-047). An ImmersiveSpace can
// only be declared by a SwiftUI `App`. So on visionOS the process entry is
// GEVisionApp.swift, and the engine is booted by GEHostViewController from a
// RUN-LOOP TIMER (performSelector:afterDelay:0), never dispatch_async(main).
//
// THE MIDAS DELTA. PD's pdSDLMain never returns (its game loop owns the main
// thread). Here geSDLMain RETURNS: with the sim thread on (round 26, default
// everywhere) port/src/main.c starts the game loop on `ge-sim`, rendering is
// on `ge-render`, and the main thread goes straight back to UIKit. So post-boot
// work does not need an engine frame hook: the 3D poll rides GEShell's 100 ms
// graft-watch timer on the main thread (geVision3dPoll).
//
// MILESTONE 1 built the entry and a solid-colour panel. MILESTONE 2 (round 46,
// docs/port-ios/66) puts the GAME on the panel: on `3d on` ge-render goes
// surfaceless and draws into an app-owned MTLTexture ring (GEEyeTargets.mm),
// framebuffer slot 0 of the GL backend is that eye (gfx_opengl.cpp), the swap
// becomes a publish, and the compositor loop samples a mipmapped copy. One eye
// (L) was drawn per frame in M2. MILESTONE 3 (round 47, docs/port-ios/67) draws
// BOTH eyes per host frame with the per-eye projection fold and the class tags
// (gfx_pc.cpp), publishes once per pair, and makes the compositor the clock
// (GEPacing external source).
#pragma once

#include <TargetConditionals.h>

#ifndef TARGET_OS_VISION
#define TARGET_OS_VISION 0
#endif

#if TARGET_OS_VISION

#import <UIKit/UIKit.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// The engine entry points the host view controller calls (app/ios/ge_ios_main.m)
// ---------------------------------------------------------------------------

/** Everything main() does before SDL. See ge_ios_main.m. */
char **geShellPrepare(int argc, char *argv[], int *outArgc);
/** Shell services, onboarding, the graft watch, the pacer, then the engine.
 *  RETURNS once `ge-sim` is up (the Midas delta above). */
int geSDLMain(int argc, char *argv[]);

// ---------------------------------------------------------------------------
// Mode switching. Main thread only.
// ---------------------------------------------------------------------------

/**
 * Enter or leave 3D. Idempotent, and a no-op before the engine has booted.
 *
 * ON:  flips the SwiftUI flag, which opens the ImmersiveSpace; the compositor
 *      loop starts on its own thread and draws the panel.
 * OFF: asks the loop to stop and WAITS for it (<= 2 s) before the space is
 *      dismissed — the loop must never touch a layer renderer SwiftUI is
 *      tearing down — then flips the flag back.
 */
void geVision3dSetMode(bool on);

/** 1 while 3D is on (the flag, not the loop: see geVision3dImmRunning). */
int geVision3dActive(void);

/**
 * Called every 100 ms from GEShell's graft watch (main thread).
 *
 * M1's job: honour GE_VP3D_AUTOENTER — the ornament needs a gaze-pinch the
 * simulator cannot inject, so a scripted gate enters 3D once the engine has
 * drawn ~300 frames (g_GlobalTimer) instead. The graft itself is already
 * re-asked by the same timer, so there is no separate retry here.
 */
void geVision3dPoll(void);

/** The `3d state` rows, newline-terminated; also appended to `state`. */
NSString *geVision3dStateLines(void);

// ---------------------------------------------------------------------------
// Phase 6 M4 (round 48, docs/port-ios/68): the parked window and the curtain.
// Copied from perfect-dark-ios app/vision3d/PDVision3D.h "M4: the parked
// window and the curtain" (D-051), pd -> ge, with the Midas deltas in
// GEHostViewController.m (the exit is finalized by a run-loop timer, not by
// pumping the run loop; the aspect floor is suspended while parked).
//
// While 3D is on the 2D window must be neither in front of the panel nor
// showing the game. Two mechanisms, because they fail separately:
//   THE CURTAIN is ours: a black view with a line of text, added to SDL's
//   UIWindow the moment the engine goes offscreen, down only after the exit
//   has re-bound the window surface at the restored size.
//   PARKING is the system's: UIWindowSceneGeometryPreferencesVision shrinks
//   the scene to a 480-pt card at the panel's aspect, ~1.5 s AFTER the space
//   finished opening, and is UNDONE BEFORE the dismissal, never after.
// ---------------------------------------------------------------------------

/** Swift, after `openImmersiveSpace` returned .opened: arms the +1.5 s park. */
void geVision3dSpaceOpened(void);
/** The bridge's `3d park` / `3d unpark`: the same request, without the space. Main. */
void geVision3dParkRequest(int park);
/** 1 while the scene is shrunk to the card (or a park is in flight). The
 *  round-35 aspect floor (GEShell.m geVisionAspectFloor) stands down while
 *  this is non-zero. */
int geVision3dWindowParked(void);
/** 1 while the curtain is over SDL's window. */
int geVision3dCurtainUp(void);
/** Completed park -> 3D -> exit cycles this session. */
int geVision3dParkCycles(void);
/** The bridge's `3d crown`: dismiss the space from the SwiftUI side WITHOUT
 *  telling the shell, so the loop sees the layer invalidated and the Crown
 *  reconcile runs exactly as for a system dismissal. Main. */
void geVision3dSimulateSystemDismiss(void);
/** UISceneDidActivate (ge_ios_main.m): a space lost while the app was in the
 *  background, or an exit whose finalize never landed, is reconciled here. */
void geVision3dReconcileForeground(void);

// ---------------------------------------------------------------------------
// The compositor loop (app/vision3d/GEImmersive.m)
// ---------------------------------------------------------------------------

/**
 * The immersive render loop. Runs on its OWN thread, spawned by the
 * CompositorLayer closure in GEVisionApp.swift; `lr` is the layer renderer,
 * passed unretained (SwiftUI owns it).
 */
void geVision3dImmersiveRun(void *lr);

/** Set to ask the loop to finish at the top of its next iteration. */
extern volatile int geVision3dImmStop;
/** 1 between the loop's first statement and its last. */
extern volatile int geVision3dImmRunning;
/** M5: compositor loop sessions started (bumped where ImmRunning goes to 1). */
extern volatile int geVision3dImmSessions;
/** Compositor frames this session. */
extern volatile int geVision3dImmFrames;
/** The compositor's measured cadence, Hz (0 until ~20 frames have landed). */
extern volatile float geVision3dImmHz;

/** The layer went invalid under us: a Crown/system dismissal. Reconciles. */
void geVision3dImmersiveEnded(void);

// ---------------------------------------------------------------------------
// Eye render targets (app/vision3d/GEEyeTargets.mm, Phase 6 M2, round 46)
//
// Everything marked ge-render runs ONLY on the render thread with the EGL
// context current, reached through app/gfx/gfx_angle_egl.mm (which declares
// these itself: the engine's glue has no search path into the shell).
// ---------------------------------------------------------------------------

#define GE_EYE_LEFT  0
#define GE_EYE_RIGHT 1

/**
 * THE ENGINE HOOK (port/fast3d/gfx_opengl.cpp ge_screen_fbo, PD patch 0031's
 * shape). Non-zero while 3D is on: *fbo is the GL name framebuffer slot 0
 * must resolve to this frame, *w x *h the eye's size. 0 (and *fbo = 0) in 2D.
 * ge-render.
 */
int geVisionEyeFBO(unsigned *fbo, int *w, int *h);
/** ge-render: wrap the ring (3 slots x 2 eyes) on ANGLE's MTLDevice. */
int geVisionEyeActivate(void);
/** ge-render: glFinish, then free the ring. The window surface is re-bound first. */
void geVisionEyeDeactivate(void);
/** The eye size: GE_VP3D_EYE=WxH, else the panel-derived size (M5: the
 *  panel's aspect, a 2560x1536 AREA budget, x Render Resolution^2, multiples
 *  of 8). The live ring's size while 3D is on. Any thread. */
void geVisionEyeGetSize(int *w, int *h);
/** M5, any thread: commit the panel shape + Render Resolution the eye is sized
 *  from. Only ASKS; the ring is re-wrapped by geVisionEyeResizeIfPending. */
void geVisionEyeSetPanel(float halfW, float halfH, float renderPct);
/** M5, ge-render at the frame boundary (geAngleApply3DPending). */
void geVisionEyeResizeIfPending(void);
float geVisionEyeRenderPct(void);
/** M5, compositor thread: bracket acquire..blit-encode so a resize never frees
 *  a ring texture between the acquire and the command buffer's own retain. */
void geVisionEyeSampleBegin(void);
void geVisionEyeSampleEnd(void);
/** Non-zero when GE_VP3D_EYE pins the eye (the panel then aspect-fits). */
int geVisionEyeIsPinned(void);
/** ge-render, at the swap: shared-event sync + publish the slot, advance the ring. */
void geVisionEyePublish(void);
/** Compositor thread: the published eye texture (unretained) and its event (+1, transfer it). */
void *geVisionEyeAcquire(int eye, void **outEvent, unsigned long long *outValue,
                         unsigned int *outGen);
/** Compositor thread: both eyes of the published pair, one lock (M3). */
void geVisionEyeAcquirePair(void **outL, void **outR, void **outEvent,
                            unsigned long long *outValue, unsigned int *outGen);
/** Compositor thread: a fresh pair was copied (1) or the previous copy reused (0). */
void geVisionEyeCountSample(int fresh);
/** GE_VP3D_SHOWEYE=L|R -> GE_EYE_*, else -1 (each view samples its own eye). */
int geVisionEyeShowEye(void);
/** ge-render, at the swap: serve a pending `3d shot` (eye FBO in 3D, window in 2D). */
void geVisionEyeServiceShot(int in3d);
/** Bridge thread (never ge-render): queue a shot and wait <= 3 s; key=value lines. */
NSString *geVisionEyeRequestShot(int eye, NSString *path);
/** The eye rows of `3d state`. */
NSString *geVisionEyeStateLines(void);

// ---------------------------------------------------------------------------
// Phase 6 M3 (round 47, docs/port-ios/67): both eyes, the fold's parameters,
// the pair, the in-flight bound. The engine half (port/fast3d/gfx_pc.cpp)
// declares the ge-render ones itself; listed here for the shell.
// ---------------------------------------------------------------------------
/** ge-render: which eye the next walk draws (gfx_run sets R between walks). */
void geVisionSetEye(int eye);
int geVisionGetEye(void);
/** 2 while the ring is live, else 1. */
int geVisionEyesPerFrame(void);
/** ge-render, before eye L: freeze the gun plane, bound the producer (in flight < 2 before, 200 ms self-heal). */
void geVisionEyeBeginPair(void);
/** ge-render, at the end of each eye's walk. */
void geVisionEyeNoteRender(void);
/** ge-render, the fold's parameters and its instruments (gfx_pc.cpp). */
int geVisionStereoFold(float *outOffset, float *outConvergence, float *outGunConvergence);
void geVisionStereoNoteClass(int cls);
void geVisionStereoNoteP33(int kind, float p33);
void geVisionStereoNoteGunVertex(float depth);
float geVisionStereoGunNear(void);
void geVisionStereoNearLaw(float *outWorldN, float *outGunN, float *outKnee);
void geVisionStereoSetNearLaw(float worldN, float knee);   // round 52 dev knob; < 0 keeps
/** Pairs published but not yet retired on the GPU. */
int geVisionEyeInFlight(void);
/** Any thread (M5): publish a whole stereo set under one lock; ge-render
 *  latches it once per pair before eye L. A negative member is kept. */
void geVisionStereoPublish(float depthPct, float convUnits, float gunConv);
/** Any thread: the M3 knobs, now wrappers over the publish. */
void geVisionStereoSetDepthPct(float pct);
void geVisionStereoSetConvergence(float units);
void geVisionStereoSetGunConv(float units);   // <= 0: the viewmodel's own nearest vertex
/** Any thread: forget the frozen head; the loop re-anchors the panel after >= 30 tracked frames. */
void geVision3dRecenter(void);

// The engine-side 3D switch (app/gfx/gfx_angle_egl.mm, GE_VISION_3D builds).
/** Main thread: latch a request; applied on ge-render in gfx_async_apply_pending(). */
void geAngleRequest3D(int on);
/** 1 once ge-render has actually gone surfaceless onto the eye. */
int geAngle3DActive(void);
/** The angle_* / acquire_* rows of `3d state` into buf; returns the length. */
int geAngle3DStateLines(char *buf, int n);

// ---------------------------------------------------------------------------
// Phase 6 M5 (round 49, docs/port-ios/69): the 3D settings. ONE page (PD
// D-082): GESettings' last section on visionOS, shown by the in-window gear's
// page AND by the ornament gear's SwiftUI sheet (2D and 3D). Persisted as
// GEDefaults `vp3d.*`; app/vision3d/GEVisionSettings.m.
// ---------------------------------------------------------------------------

/** THE ONLY PLACE `vp3d.*` BECOMES 3D STATE: panel geometry (GEImmersive),
 *  the stereo publish, dimming, and the eye commit (panel aspect + Render
 *  Resolution). Boot, every entry, every Reset/bridge set, every slider
 *  release. Any thread. */
void geVision3dApplySettings(void);
/** A slider DRAG sample: the same, minus the eye commit (the ring is not
 *  re-wrapped sixty times a second; the quad stretches meanwhile). */
void geVision3dApplySettingsLive(void);
/** Header Reset: the vp3d rows back to factory, Units and FPS on Panel kept. */
void geVision3dSettingsResetDefaults(void);
/** The compositor's panel (GEImmersive.m), metres; dim 0..1. Any thread. */
void geVisionPanelSet(float dist, float halfW, float halfH, float posH, float dim);
/** The panel rows of `3d state`. */
NSString *geVisionPanelStateLines(void);

/** The sheet: the ornament gear, `3d settings open|close`, the exit. Any thread. */
void geVision3dSettingsSheetRequest(int open);
/** Swift's .onAppear / .onDisappear. Main. */
void geVision3dSettingsSheetNote(int up);
/** 1 while the sheet is up (or asked to open). */
int geVision3dSettingsSheetUp(void);
/** Main: run `then` once the sheet is GONE (immediately when it is not up):
 *  close it, poll on a 20 ms run-loop timer (<= 2 s), never block main. */
void geVision3dSettingsSheetCloseThen(void (^then)(void));
/** The settings page for the sheet (GESettings.m). Main. */
UIViewController *geVision3dMakeSettingsPage(void);
/** Bridge `3d settings scroll`: the sheet's page to its 3D section. Main. */
NSString *geVision3dSettingsScrollSheet(void);
NSString *geVision3dSettingsPinState(NSString *offset);   // round 52; nil = no scroll
/** Redraw every live settings page's 3D section (after a bridge set / reset). */
void geVision3dSettingsReloadPages(void);

/** Bridge `3d settings get [row]|set <row> <v>|reset` and the state rows. */
NSString *geVision3dSettingsGet(NSString *row);
NSString *geVision3dSettingsSet(NSString *row, NSString *value);
NSString *geVision3dSettingsStateLines(void);
/** The card's aspect (M4 parks at it since M5): the panel's halfW/halfH. */
float geVision3dPanelAspect(void);

// ---------------------------------------------------------------------------
// Implemented in Swift (@_cdecl), called from ObjC
// ---------------------------------------------------------------------------

void GE_SetImmersiveMode(bool on);
/** M4: dismiss the space without flipping the shell's flag (`3d crown`). */
void GE_DismissImmersiveRaw(void);
/** M5: present / dismiss the settings sheet (synchronous on main). */
void GE_SetSettingsSheet(bool on);

#ifdef __cplusplus
}
#endif

/**
 * The engine's host. Lives in the SwiftUI WindowGroup as a
 * UIViewControllerRepresentable, and boots the engine from -viewDidAppear via
 * a run-loop timer (see the file comment above).
 */
@interface GEHostViewController : UIViewController
@end

#endif // TARGET_OS_VISION
