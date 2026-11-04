// GEShell.h — the iOS shell's shared state, and the one seam into the engine.
//
// Two rules hold this shell together and every other file assumes them:
//
//   1. The MAIN thread is the HOST thread, not the render thread. SDL's
//      UIKitRunApp brings up UIApplication and calls geEngineMain() on the main
//      thread; that function creates the window, starts the game's mainThread
//      and then sits in port/src/main.c's `for (;;) { videoPumpEvents();
//      sysSleep(8); }` loop for the life of the app. Rendering happens on the
//      game's scheduler thread. This is where GoldenEye differs from Perfect
//      Dark, whose main thread IS its game thread, and it is why the ANGLE glue
//      has an explicit per-thread MakeCurrent (app/gfx/gfx_angle_egl.mm).
//   2. Because videoPumpEvents() calls SDL_PumpEvents(), which on iOS runs
//      CFRunLoopRunInMode, the main run loop keeps turning — so a UIView the
//      shell adds is composited, a main-queue block does run, and a scheduled
//      NSTimer does fire. Everything UIKit-side in this shell rides on that.
//
// Round 7b is the FIRST iOS build and this file is deliberately small: paths,
// the window graft, and nothing else. Onboarding, the touch overlay, the
// bridge, the settings page and the CADisplayLink pacer are later rounds.
#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
extern "C" {
#endif

// The engine, from libge007.a (port/src/main.c, under PLATFORM_IOS).
int geEngineMain(int argc, char **argv);

/**
 * Put SDL's sceneless UIWindow onto the foreground scene. Returns how many
 * windows it moved.
 *
 * This is load-bearing, not insurance, and PD measured it (its D-038): once an
 * app declares a UIApplicationSceneManifest — which is MANDATORY on the iOS 27
 * SDK, or UIKit SIGTRAPs at launch before any of our code runs — a UIWindow
 * that SDL built the pre-scene way (-initWithFrame:, no windowScene) appears in
 * NEITHER UIApplication.windows NOR any scene's windows array. It exists, SDL
 * holds it, the renderer draws into its layer, and nothing in UIKit can find it
 * to composite it. The symptom is a perfectly healthy engine and a black
 * screen.
 *
 * The handle that always works is the renderer's: ANGLE built its surface from
 * SDL's SDL_MetalView, and that view's -window is SDL's UIWindow whether UIKit
 * can enumerate it or not (geAngleGetHostView).
 *
 * Main thread only.
 */
int geGraftSDLWindows(void);

/**
 * The shell's log line: NSLog AND a mirror into Documents/shell.log.
 *
 * Round 11a. NSLog is unreachable on a device that cannot be launched from a
 * Mac (devicectl launch is blocked by policy here), so every device read of a
 * shell defect was previously a guess. `Documents/shell.log` is Files-visible
 * exactly like `pacing.log`, is TRUNCATED at every launch (so it is always
 * this run) and is capped at 512 KiB (so a runaway logger cannot fill a phone).
 *
 * Use it for the shell's own narration — [touch], [pad], [scene], [settings],
 * onboarding. Any thread.
 */
void geShellLogLine(NSString *line);

/**
 * Route the ENGINE's stdout and stderr into Documents/engine.log. D-055.
 *
 * Round 14a. The shell has had `shell.log` since round 11a, but the engine
 * itself writes through `fprintf(stderr, ...)` (port/src/system.c's
 * sysLogPrintf, osSyncPrintf, fast3d's warnings) and on a device those file
 * descriptors go nowhere at all -- so the one line that would have named the
 * bug, bg.c's "[WARN ] D69: room vertex batch out of range ... room geometry
 * WILL be missing (silently)", has never once been readable from the phone.
 * That is the whole reason OTA 0.0.0.5's device-only missing world geometry
 * is still a guess.
 *
 * Mechanism: a pipe, with the write end dup2()'d onto fds 1 and 2, and a
 * reader thread that copies it into the file. A pipe rather than dup2 of the
 * file itself because a file descriptor cannot be capped, and an engine that
 * finds a hot warning path would otherwise fill the phone; the reader stops
 * writing at kEngineLogCap and goes on draining for ever, so the engine can
 * never block on a full pipe either.
 *
 * Truncated per launch and headed with the version, exactly like shell.log:
 * a device read pulls it straight after the launch it is asking about.
 *
 * Call ONCE, as early in main() as possible and before anything can print.
 */
void geEngineLogStart(NSString *docs);

/**
 * `view.safeAreaInsets`, corrected for SDL's window.
 *
 * Round 10 open item, measured in round 11a: SDL's UIWindow is created the
 * pre-scene way and grafted onto the scene afterwards, and UIKit never
 * recomputes its safe area for the orientation it is actually composited at —
 * an iPhone Air in landscape reports `points=912x420 safe=68,0,34,0`, i.e. a
 * PORTRAIT inset set (the island at the top, the home indicator at the
 * bottom) on a landscape view. Anything anchored to the safe-area layout
 * guide therefore sits ~68 pt too far down.
 *
 * The correction is deliberately orientation-AGNOSTIC: on a landscape view
 * with a portrait-shaped inset set, the island inset is applied to BOTH sides
 * and the top goes to zero. Which side the island is actually on depends on
 * which way the phone is held, and a control that clears it on both sides is
 * correct either way — where guessing one side is wrong half the time.
 */
UIEdgeInsets geShellSafeInsets(UIView *view);

#define GELog(fmt, ...) geShellLogLine([NSString stringWithFormat:(fmt), ##__VA_ARGS__])

#ifdef __cplusplus
}
#endif

@interface GEShell : NSObject

@property (class, readonly) GEShell *shared;

/** ~/Documents — the ROM, ge007.ini, ge007.eep. Files-visible. */
@property (nonatomic, readonly) NSString *documentsPath;
/** ~/Library/Caches — the regenerable ROM-derived sidecars. */
@property (nonatomic, readonly) NSString *cachesPath;
/** The app bundle. Read-only. */
@property (nonatomic, readonly) NSString *bundlePath;

/** Set once geEngineMain() has been entered. */
@property (nonatomic) BOOL engineRunning;

/**
 * Keep trying the graft until it takes. SDL does not create its window until
 * inside videoInit(), which is inside the engine, which starts after the scene
 * has already connected — so the graft cannot simply be done at connect time.
 */
+ (void)startGraftWatch;

/**
 * Round 12b / D-048. Hand the host's (corrected) safe-area insets to the
 * engine as fractions of the view, and say once what the renderer is really
 * drawing into. Both are called from the graft watch; main thread only.
 */
+ (void)publishSafeInsets:(UIView *)view;
+ (void)logDrawableSize;

/**
 * Round 15a / D-059. A landscape-only iPhone app must never be left with a
 * PORTRAIT-shaped window, and SDL's window is the one window on this phone
 * that nothing else will ever put back.
 *
 * SDL builds its UIWindow the pre-scene way and GEShell grafts it onto the
 * scene afterwards (§5c), so it has no root view controller UIKit will
 * consult, no supported-orientation mask, and no autoresizing relationship
 * with the scene. When the system presents something of its own OVER the app
 * -- the screenshot thumbnail and its Markup editor is the one Austin hits --
 * that transient lays the window out at the screen's PORTRAIT shape, and when
 * it goes away nothing lays it back: the CAMetalLayer keeps the portrait
 * bounds, ANGLE recomputes a 1260x2736 drawable from them every frame, and
 * the game renders forever into the left 46 % of the panel. `engine.log` on
 * `0.0.0.6` caught it exactly (`dims=1260x2736 win=1260x2736`, and a scissor
 * that had gone NEGATIVE at `-1606`); `shell.log` said nothing at all,
 * because nothing was watching.
 *
 * So the window's geometry is re-asked on the same 100 ms tick as the graft
 * and the overlay, and for the same reason (§5b): the answer is re-derived,
 * never remembered. The authority is the SCENE's coordinate space, which is
 * the real composited size of the app, and on a landscape-only iPhone that is
 * landscape by construction -- so a portrait answer is rejected rather than
 * applied, and the last known-good landscape rect is used instead.
 *
 * Main thread only. Returns YES if it had to put something back.
 */
+ (BOOL)enforceWindowGeometry;

/**
 * Every drawable size change, with the window it was derived from, in the one
 * log a device read can reach. Called from the same tick; says nothing when
 * nothing changed.
 */
+ (void)noteDrawableSize:(NSString *)reason;

/**
 * The touch overlay has the same problem for the same reason: it has to sit on
 * SDL's window, and SDL's window does not exist until the engine has started.
 * Folded into the same 100 ms timer as the graft, which is why that timer now
 * stops on BOTH having succeeded.
 */

@end

NS_ASSUME_NONNULL_END
