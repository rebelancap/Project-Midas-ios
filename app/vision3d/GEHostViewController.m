// GEHostViewController.m — boots the engine under the SwiftUI entry, and owns
// the 2D <-> 3D mode switch (visionOS only).
//
// Copied from perfect-dark-ios app/vision3d/PDHostViewController.m at its M1
// shape (PD commit 3db8d80, D-047), pd -> ge. Read GEVision3D.h first: it says
// why the entry point is SwiftUI and why the boot goes through a run-loop
// timer rather than the main dispatch queue.
//
// THE MIDAS DELTA. PD's pdSDLMain never returns, so PD needed its engine
// frame hook for every post-boot job. Here geSDLMain RETURNS once `ge-sim` is
// up (round 26) and the main thread is UIKit's again, so post-boot work rides
// GEShell's 100 ms graft-watch timer (geVision3dPoll) and nothing in the
// engine is hooked. The one thing to respect instead is the ENGINE LOCK
// (GEPacing.h, round 26): the main thread holds it whenever its run loop is
// awake, so anything here that blocks the main thread (the <= 2 s exit wait)
// also holds the sim for that long. Acceptable for a mode switch; never do it
// per frame.
#import "GEVision3D.h"

#if TARGET_OS_VISION

#import "GEShell.h"
#import "GEPacing.h"   // M3: geIosPacingModeName
#import "GEAudio.h"    // M4: spatial intent
#import "GETouchOverlay.h"   // M4: hidden while immersive
#import "GESettings.h"       // M5: the in-window page closes on entry
#include <SDL.h>       // M4: the synthetic size event after the exit
#include <sys/stat.h>
#include <signal.h>
#include <math.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// SDL2 is statically linked; the declaration is the whole of what is needed.
// SDL_SetMainReady() tells SDL that main() has already run, which on visionOS
// is true in spirit and false in fact: there is no main() in this build at all
// (ge_ios_main.m's is #if !TARGET_OS_VISION).
extern void SDL_SetMainReady(void);

// port/apple/hostinput.c — frames = g_GlobalTimer.
extern void geHostState(int *stage, float *fps, unsigned int *ticks, unsigned int *frames);
// port/apple/hostinput.c — midas.ini rewritten now (portOptSaveHold(0) + portOptSave).
extern void geHostFlushSaves(void);
// app/gfx/gfx_angle_egl.mm — SDL's Metal view (the renderer's host).
extern void *geAngleGetHostView(void);
extern void geAngleGetDrawableSize(int *w, int *h);
// port/fast3d/gfx_sdl2.cpp — the engine's SDL window.
extern SDL_Window *gfx_sdl_get_window(void);

/** -viewDidAppear has scheduled the boot (once per process). */
static BOOL geVisionBootScheduled = NO;
/** geSDLMain has returned: the engine is up on `ge-sim`. */
static BOOL geVisionBooted = NO;
static int geVision3dOn = 0;

// ---------------------------------------------------------------------------
// M4 (round 48): the parked window and the curtain. Copied from
// perfect-dark-ios app/vision3d/PDHostViewController.m (pdVisionCurtain,
// pdVisionParkWindow, pdVisionParkFire, pdVision3dSpaceOpened; D-051), pd ->
// ge. Main thread only, all of it.
//
// THE MIDAS DELTA: PD waited for the un-parked scene by pumping the run loop
// inside its exit. Here the exit is usually reached from a bridge block on the
// main dispatch queue (geOnMain) - a nested run loop inside a main-queue block
// does not drain the main queue, and UIKit's geometry answer may be queued
// there - and the main thread holds the ENGINE LOCK while awake (GEPacing.h).
// So the exit is FINALIZED by a 50 ms run-loop timer instead: it watches the
// scene come back to the captured size (<= 3 s), then pushes the synthetic SDL
// size event, waits a few frames for the window surface to follow, and only
// then drops the curtain. `3d state` says `finalizing=1` until it has.
// ---------------------------------------------------------------------------
static CGSize gePreSize = { 0.0, 0.0 };   // the pre-3D scene size, points
static int geParked = 0;                  // the scene is (being) shrunk to the card
static int geUnparking = 0;               // an un-park is in flight (floor stays down)
static int geCurtain = 0;
static int geParkCycles = 0;
static int geCycleParked = 0;             // this 3D session parked at least once
static unsigned geParkGen = 0;            // bumped by every mode change: stale timers die
static int geParkArmed = 0;
static int geFinalizing = 0;
static int geSpaceOpened = 0;             // .opened seen for the current session
static int geImmSessionsAtEnter = 0;      // M4 review: geVision3dImmSessions at the last entry
static int geExitPending = 0;             // M5: an exit is waiting for the settings sheet to close
static int geExitSheetDone = 0;           // M5: ... and the sheet wait is over (gone or timed out)
static int geGeomRefusals = 0;
static UIView *geCurtainView = nil;
static NSTimer *geFinalizeTimer = nil;

static UIWindow *geVisionSDLWindow(void)
{
	UIView *host = (__bridge UIView *)geAngleGetHostView();
	return host.window;
}

/** The scene the SDL window is grafted onto, or nil. */
static UIWindowScene *geVisionScene(void)
{
	UIWindow *win = geVisionSDLWindow();
	if (win.windowScene) {
		return win.windowScene;
	}
	for (UIScene *sc in UIApplication.sharedApplication.connectedScenes) {
		if ([sc isKindOfClass:UIWindowScene.class]
				&& sc.activationState != UISceneActivationStateUnattached) {
			return (UIWindowScene *)sc;
		}
	}
	return nil;
}

static CGSize geVisionSceneSize(void)
{
	UIWindowScene *scene = geVisionScene();
	if (!scene) {
		return CGSizeZero;
	}
	UIWindowSceneGeometry *g = scene.effectiveGeometry;
	return g ? g.coordinateSpace.bounds.size : scene.coordinateSpace.bounds.size;
}

/** Pixels per point of the renderer's layer (render scale included). */
static CGFloat geVisionPxScale(void)
{
	UIView *host = (__bridge UIView *)geAngleGetHostView();
	CGFloat sc = host.layer.contentsScale;
	return sc > 0.0 ? sc : 2.0;
}

/**
 * The curtain. Black, with words on it, over SDL's window (PD pdVisionCurtain).
 *
 * Not an alpha fade and not a hidden window: SDL's window keeps its scene (the
 * only regular scene; losing it would cost audio) and keeps laying out, so the
 * frame it last drew is covered rather than removed. It also SWALLOWS touches:
 * a pinch on the parked card lands on the curtain, never on SDL's view, which
 * is what makes the card display-only (plan section 2.8).
 */
static void geVisionCurtain(int up)
{
	UIWindow *win = geVisionSDLWindow();
	if (!win) {
		GELog(@"[3d] curtain %d: no SDL window yet", up);
		return;
	}
	if (up) {
		if (!geCurtainView) {
			UIView *v = [[UIView alloc] initWithFrame:win.bounds];
			v.backgroundColor = UIColor.blackColor;
			v.opaque = YES;
			v.userInteractionEnabled = YES;
			v.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
			UILabel *l = [[UILabel alloc] initWithFrame:CGRectInset(v.bounds, 24.0, 0.0)];
			l.autoresizingMask = v.autoresizingMask;
			l.textAlignment = NSTextAlignmentCenter;
			l.numberOfLines = 2;
			l.adjustsFontSizeToFitWidth = YES;
			l.minimumScaleFactor = 0.5;
			l.textColor = [UIColor colorWithWhite:1.0 alpha:0.75];
			l.font = [UIFont systemFontOfSize:22 weight:UIFontWeightMedium];
			l.text = @"Playing in 3D...";
			[v addSubview:l];
			geCurtainView = v;
		}
		geCurtainView.frame = win.bounds;
		[win addSubview:geCurtainView];
		// The overlay stays above the curtain (it is hidden in 3D anyway, but
		// an editor or a future "touch in 3D" mode must not end up under it).
		GETouchOverlay *ov = GETouchOverlay.shared;
		if (ov.superview == win) {
			[win bringSubviewToFront:ov];
		}
		geCurtain = 1;
		GELog(@"[3d] curtain UP frame=%@ window=%@",
		      NSStringFromCGRect(geCurtainView.frame), NSStringFromCGRect(win.bounds));
	} else {
		[geCurtainView removeFromSuperview];
		geCurtain = 0;
		GELog(@"[3d] curtain DOWN (window %@)", NSStringFromCGRect(win.bounds));
	}
	[GETouchOverlay applyVisibility];
}

/**
 * Park (shrink the scene to the card) or un-park (restore the captured size).
 * PD pdVisionParkWindow. The request is asynchronous and the system may refuse
 * it: the flag is set optimistically and the error handler puts it back, and
 * the log says why.
 */
static void geVisionParkWindow(int park)
{
	UIWindowScene *scene = geVisionScene();
	if (!scene) {
		GELog(@"[3d] park %d: no window scene", park);
		return;
	}
	CGSize want;
	if (park) {
		if (gePreSize.width < 1 || gePreSize.height < 1) {
			GELog(@"[3d] park refused — no pre-3D size was captured");
			return;
		}
		// 480 pt at the PRE-3D aspect, which is the panel's aspect: the panel
		// is aspect-fitted to the eye, and the eye is sized like the window.
		// M5 (round 49): the card takes the PANEL's aspect (the Width/Height
		// rows), not the pre-3D window's: the panel is settings-driven now.
		// Long side 480 pt.
		CGFloat aspect = (CGFloat)geVision3dPanelAspect();
		if (!(aspect > 0.05)) {
			aspect = gePreSize.width / gePreSize.height;
		}
		want = aspect >= 1.0 ? CGSizeMake(480.0, round(480.0 / aspect))
		                     : CGSizeMake(round(480.0 * aspect), 480.0);
	} else {
		if (gePreSize.width < 1) {
			return;
		}
		want = gePreSize;
	}
	UIWindowSceneGeometryPreferencesVision *prefs =
		[[UIWindowSceneGeometryPreferencesVision alloc] initWithSize:want];
	GELog(@"[3d] %s -> requesting %.0fx%.0f pt (was %@)",
	      park ? "PARK" : "UN-PARK", want.width, want.height,
	      NSStringFromCGSize(geVisionSceneSize()));
	[scene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError *err) {
		geGeomRefusals++;
		GELog(@"[3d] geometry request (%s) REFUSED: %@", park ? "park" : "un-park",
		      err.localizedDescription);
		if (park) {
			geParked = 0;
		}
	}];
	if (park) {
		geParked = 1;
		geUnparking = 0;
		geCycleParked = 1;
	} else {
		geUnparking = geParked ? 1 : geUnparking;
		geParked = 0;
	}
	// A kill can follow any transition (a Crown press is often the first half
	// of a swipe-kill): midas.ini is rewritten on each one. It costs one small
	// file write per transition.
	geHostFlushSaves();
}

static void geVisionParkFire(unsigned gen)
{
	geParkArmed = 0;
	if (gen != geParkGen || !geVision3dOn) {
		GELog(@"[3d] the +1.5 s park fired after the exit — ignoring");
		return;
	}
	geVisionParkWindow(1);
}

void geVision3dSpaceOpened(void)
{
	// ~1.5 s AFTER the space finished opening, never during the transition: a
	// geometry request mid-transition wedges a sibling animation (sm64/q2repro
	// both landed on the same delay). A run-loop timer; a park that fires
	// after an exit is recognised by its generation and ignored.
	if (!geVision3dOn) {
		return;
	}
	geSpaceOpened = 1;
	if (geParkArmed || geParked) {
		return;
	}
	geParkArmed = 1;
	const unsigned gen = geParkGen;
	GELog(@"[3d] space opened — parking the 2D window in 1.5 s");
	[NSTimer scheduledTimerWithTimeInterval:1.5 repeats:NO block:^(NSTimer *t) {
		(void)t;
		geVisionParkFire(gen);
	}];
}

void geVision3dParkRequest(int park)
{
	geVisionParkWindow(park ? 1 : 0);
	if (!park && !geVision3dOn) {
		// A bridge un-park in 2D: let the floor back in once it lands.
		geUnparking = 0;
	}
}

int geVision3dWindowParked(void) { return geParked || geUnparking; }
int geVision3dCurtainUp(void)    { return geCurtain; }
int geVision3dParkCycles(void)   { return geParkCycles; }

/** The synthetic size event: SDL_WINDOWEVENT_SIZE_CHANGED for the engine's
 *  window, at its CURRENT size. The engine's pump (gfx_sdl2.cpp) answers it
 *  with geAngleResized - the drawable re-measured from the layer - which is
 *  the insurance for an SDL view whose own layout event did not fire. */
static void geVisionPushSizeEvent(void)
{
	SDL_Window *w = gfx_sdl_get_window();
	if (!w) {
		return;
	}
	int ww = 0, wh = 0;
	SDL_GetWindowSize(w, &ww, &wh);
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = SDL_WINDOWEVENT;
	ev.window.windowID = SDL_GetWindowID(w);
	ev.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
	ev.window.data1 = ww;
	ev.window.data2 = wh;
	const int rc = SDL_PushEvent(&ev);
	GELog(@"[3d] synthetic SDL size event %dx%d pt (push %d)", ww, wh, rc);
}

/**
 * The exit's second half, on a 50 ms run-loop timer (the Midas delta above).
 * Phase 1: wait for the scene to reach the captured size (<= 3 s; a refusal or
 * a backgrounded app gives up and finalizes at whatever size it has). Phase 2:
 * push the size event, then wait until the window surface has been re-bound
 * and the drawable equals the scene in pixels (<= 2 s more). Then the curtain
 * comes down, LAST: its whole purpose is to cover the frames that are wrong.
 */
static void geVisionFinalizeExit(NSString *why)
{
	[geFinalizeTimer invalidate];
	geFinalizing = 1;
	const unsigned gen = geParkGen;
	const CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
	__block int phase = 1;
	__block CFAbsoluteTime t1 = 0.0;
	GELog(@"[3d] exit finalize started (%@): waiting for the scene to come back to %@",
	      why, NSStringFromCGSize(gePreSize));
	geFinalizeTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
		if (gen != geParkGen || geVision3dOn) {
			// Re-entered 3D before this landed: the new session owns the window.
			[t invalidate];
			geFinalizing = 0;
			GELog(@"[3d] exit finalize abandoned (3D re-entered)");
			return;
		}
		const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
		const CGSize sz = geVisionSceneSize();
		if (phase == 1) {
			const BOOL back = gePreSize.width < 1 ||
				(fabs(sz.width - gePreSize.width) < 1.0 && fabs(sz.height - gePreSize.height) < 1.0);
			if (!back && now - t0 < 3.0) {
				return;
			}
			GELog(@"[3d] the scene is %@ after %.0f ms%s", NSStringFromCGSize(sz),
			      (now - t0) * 1000.0, back ? "" : " (gave up waiting for the captured size)");
			// The un-park has landed (or will not): the floor may answer again.
			geUnparking = 0;
			[GEShell enforceWindowGeometry];
			geVisionPushSizeEvent();
			phase = 2;
			t1 = now;
			return;
		}
		int dw = 0, dh = 0;
		geAngleGetDrawableSize(&dw, &dh);
		const CGFloat sc = geVisionPxScale();
		const BOOL match = !geAngle3DActive() &&
			abs(dw - (int)lround(sz.width * sc)) <= 1 && abs(dh - (int)lround(sz.height * sc)) <= 1;
		if (!match && now - t1 < 2.0) {
			if (now - t1 > 0.5 && fmod((now - t1), 0.5) < 0.05) {
				geVisionPushSizeEvent();   // the insurance, again
			}
			return;
		}
		[t invalidate];
		geFinalizeTimer = nil;
		geVisionCurtain(0);
		if (geCycleParked) {
			geParkCycles++;
			geCycleParked = 0;
		}
		geFinalizing = 0;
		geSpaceOpened = 0;
		[GEShell noteDrawableSize:@"3d exit finalized"];
		GELog(@"[3d] exit finalized after %.0f ms (drawable %dx%d, scene %.0fx%.0f pt x %.1f%s; "
		      @"parked=%d curtain=%d cycles=%d)",
		      (now - t0) * 1000.0, dw, dh, sz.width, sz.height, (double)sc,
		      match ? "" : " MISMATCH", geParked, geCurtain, geParkCycles);
	}];
}

/** The shell half of every exit (settings/bridge, Crown, scene loss). Main.
 *  The loop has already stopped. Un-park FIRST, then the dismissal (when we
 *  are the ones dismissing), then the engine back onto the window. */
static void geVisionExitShell(BOOL dismiss, NSString *why)
{
	geVision3dOn = 0;
	geParkGen++;
	geParkArmed = 0;
	// UN-PARK BEFORE THE DISMISSAL, never after. q2repro issued it after and
	// the request raced the space's transition and was DROPPED on device -
	// the "stuck tiny window" the simulator's instant transitions never show.
	if (geParked || geCycleParked) {
		geVisionParkWindow(0);
	}
	GE_SetImmersiveMode(false);   // a no-op for SwiftUI if the space is already gone
	(void)dismiss;
	// Window surface back FIRST, ring freed second (inside geAngleSet3DActive,
	// on ge-render at its next frame boundary; not waited for - ENGINE LOCK).
	geAngleRequest3D(0);
	[GEAudio setImmersive:NO];
	geVisionFinalizeExit(why);
}

// ---------------------------------------------------------------------------
// Mode switching
// ---------------------------------------------------------------------------

int geVision3dActive(void)
{
	return geVision3dOn;
}

void geVision3dSetMode(bool on)
{
	if (!geVisionBooted) {
		GELog(@"[3d] setMode(%d) ignored — the engine has not booted", (int)on);
		return;
	}
	if (on == (bool)geVision3dOn) {
		return;
	}

	if (on) {
		// M4: the pre-3D size, captured FIRST and as an explicit state (plan
		// section 2.9). If an exit is still finalizing, the scene may still be
		// coming back from the card: keep the size that exit restored to.
		const CGSize pre = geVisionSceneSize();
		if (!geFinalizing && !geParked && pre.width >= 1 && pre.height >= 1) {
			gePreSize = pre;
		}
		GELog(@"[3d] pre-3D scene size captured: %@ (scene now %@)",
		      NSStringFromCGSize(gePreSize), NSStringFromCGSize(pre));
		[geFinalizeTimer invalidate];
		geFinalizeTimer = nil;
		geFinalizing = 0;
		geParkGen++;
		geCycleParked = 0;
		geSpaceOpened = 0;
		geImmSessionsAtEnter = geVision3dImmSessions;
		geExitPending = 0;
		geExitSheetDone = 0;
		// A kill in 3D must find midas.ini current: write it on the way in.
		geHostFlushSaves();
		// M5: the in-window settings page (its own UIWindow) would end up
		// inside the parked card; in 3D the ornament gear's sheet is the route.
		[GESettings dismiss];
		// M5: every entry applies vp3d.* - the eye is sized from the panel
		// before ge-render wraps the ring for this entry.
		geVision3dApplySettings();
		geVision3dOn = 1;
		// M2 (round 46): the engine goes offscreen FIRST. The request is a
		// latch; ge-render applies it at its next frame boundary
		// (gfx_async_apply_pending -> geAngleApply3DPending): surfaceless
		// eglMakeCurrent, the eye ring wrapped, framebuffer slot 0 = the eye.
		// The main thread never touches EGL and does not wait for it: the
		// panel shows M1's solid colour until the first eye is published (one
		// or two engine frames after the space opens). M4 parks the window
		// afterwards (plan §2.9); in M2 the window stays where it is and
		// simply stops updating (nothing presents into it while 3D is on).
		geAngleRequest3D(1);
		// CURTAIN BEFORE THE SPACE (M4): from this instant the window's last
		// drawn frame is stale and must not sit a metre in front of the panel.
		geVisionCurtain(1);
		[GETouchOverlay applyVisibility];
		[GEAudio setImmersive:YES];
		GE_SetImmersiveMode(true);
		GELog(@"[3d] ENTER committed: engine offscreen requested (ge-render, next frame), curtain up, space opening");
	} else {
		// M5 (doc 68 §5): the settings sheet is CLOSED AND WAITED FOR before
		// anything else in the exit - the loop stop, the un-park, the
		// dismissal. A continuation on a run-loop timer, never a run-loop
		// pump (GEVisionSettings.m says why); this call re-enters once the
		// sheet is gone (or after 2 s).
		if (!geExitSheetDone && geVision3dSettingsSheetUp()) {
			if (geExitPending) {
				return;
			}
			geExitPending = 1;
			const unsigned gen = geParkGen;
			geVision3dSettingsSheetCloseThen(^{
				geExitPending = 0;
				if (gen != geParkGen || !geVision3dOn) {
					GELog(@"[3d] exit continuation dropped (session changed)");
					return;
				}
				geExitSheetDone = 1;
				geVision3dSetMode(false);
			});
			return;
		}
		geExitSheetDone = 0;
		// Stop the loop and WAIT for it before the space is torn down: it must
		// never touch a layer renderer SwiftUI is dismantling - and from M2 on
		// it must not be sampling the eye ring when ge-render frees it.
		geVision3dImmStop = 1;
		int waited = 0;
		for (; waited < 200 && geVision3dImmRunning; waited++) {
			usleep(10 * 1000);
		}
		GELog(@"[3d] EXIT: loop stopped=%d after %d ms (frames=%d)",
		      !geVision3dImmRunning, waited * 10, geVision3dImmFrames);
		// Then (M4) un-park BEFORE the dismissal, the dismissal, and the
		// engine back onto the window: ge-render re-binds the window surface
		// FIRST and frees the ring second, at its next frame boundary. NOT
		// waited for here: this thread holds the ENGINE LOCK while awake
		// (GEPacing.h, round 26), and a render frame needs the sim's task,
		// which needs that lock - a wait here would only ever time out. The
		// finalize timer drops the curtain once the drawable is right.
		geVisionExitShell(YES, @"exit");
	}
}

// A Crown or system dismissal: the loop saw the layer invalidated and has
// already exited. Reconcile the shell exactly like an exit (M4: un-park, the
// engine back on the window, curtain down once the drawable is right) AND
// SAVE AT ONCE: a Crown exit is very often the first half of a swipe-kill,
// and a swipe-kill is SIGKILL - nothing runs after it (PD pdVision3dImmersiveEnded).
void geVision3dImmersiveEnded(void)
{
	GELog(@"[3d] immersive space ended by the system (Crown)");
	// M4 review: the session this loop belonged to. A dismissal that drains
	// after the player re-entered 3D must not tear the NEW session down.
	const unsigned gen = geParkGen;
	dispatch_async(dispatch_get_main_queue(), ^{
		geHostFlushSaves();
		GELog(@"[3d] Crown reconcile: midas.ini flushed");
		if (!geVision3dOn) {
			return;   // an exit we started ourselves raced it; already reconciled
		}
		if (gen != geParkGen) {
			GELog(@"[3d] Crown reconcile for an older session (gen %u, now %u) — ignored", gen, geParkGen);
			return;
		}
		// M5: the sheet goes first here too (the un-park is still ahead).
		geVision3dSettingsSheetCloseThen(^{
			if (gen != geParkGen || !geVision3dOn) {
				return;
			}
			geVisionExitShell(NO, @"Crown/system dismissal");
		});
	});
}

void geVision3dSimulateSystemDismiss(void)
{
	if (!geVision3dOn) {
		GELog(@"[3d] crown: not in 3D");
		return;
	}
	GELog(@"[3d] crown (simulated): dismissing the space from SwiftUI, shell untouched");
	GE_DismissImmersiveRaw();
}

void geVision3dReconcileForeground(void)
{
	if (!geVisionBooted) {
		return;
	}
	if (geVision3dOn) {
		// 3D on, but no loop and the space opened long ago: the space was lost
		// while we were away and the loop never saw an invalidation it could
		// report (or reported it before the main queue ran again).
		// M4 review: only once the loop has actually RUN this session - in
		// the gap between `.opened` and the loop's first statement the scene
		// can activate, and a healthy new session must not be kicked out.
		if (geSpaceOpened && geVision3dImmSessions != geImmSessionsAtEnter && !geVision3dImmRunning) {
			GELog(@"[3d] foreground: 3D on but the compositor loop is gone — reconciling as a system dismissal");
			geHostFlushSaves();
			geVisionExitShell(NO, @"scene loss");
		}
		return;
	}
	if ((geParked || geUnparking || geCurtain) && !geFinalizing) {
		GELog(@"[3d] foreground: 2D with parked=%d unparking=%d curtain=%d — re-running the exit finalize",
		      geParked, geUnparking, geCurtain);
		geParked = 1;   // force the un-park request out again
		geVisionParkWindow(0);
		geVisionFinalizeExit(@"foreground");
	}
}

// ---------------------------------------------------------------------------
// SIGTERM: `simctl terminate` and a polite system termination send it before
// any SIGKILL. A dispatch source turns it into an ordinary main-queue block
// (nothing runs in signal context), which writes midas.ini and exits. The
// SIGKILL half (a swipe-kill) is covered by the flushes at every transition.
// Installed BEFORE geSDLMain: SDL_QuitInit only takes SIGTERM over when its
// disposition is still SIG_DFL (it would turn it into SDL_QUIT -> exit(0) on
// ge-sim with no save), so SIG_IGN here keeps the signal ours.
// ---------------------------------------------------------------------------
static void geVisionInstallTermFlush(void)
{
	static dispatch_source_t src;
	if (src) {
		return;
	}
	signal(SIGTERM, SIG_IGN);
	src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(src, ^{
		geHostFlushSaves();
		GELog(@"[3d] SIGTERM — midas.ini flushed (3d=%d), exiting", geVision3dOn);
		// _exit, not exit: atexit/static destructors with ge-sim and ge-render
		// still running are a crash.txt waiting to happen, and everything that
		// must survive is on disk now.
		_exit(0);
	});
	dispatch_resume(src);
}

// ---------------------------------------------------------------------------
// The graft watch's visionOS 3D half (main thread, every 100 ms)
// ---------------------------------------------------------------------------

// M6 (round 50): the device-round log (Documents/logs/vision3d-*.log).
static void geVision3dLogTick(void);

void geVision3dPoll(void)
{
	if (!geVisionBooted) {
		return;
	}
	geVision3dLogTick();
	// GE_VP3D_AUTOENTER=1 — enter 3D once the engine has drawn 300 frames.
	// The ornament's "3D" button needs a gaze-pinch, and simctl cannot inject
	// one, so a scripted gate has no other way in. Absent, this does nothing
	// at all, so it cannot change a shipped build's behaviour.
	static int autoEnter = -1;
	if (autoEnter < 0) {
		const char *e = getenv("GE_VP3D_AUTOENTER");
		autoEnter = (e && *e && *e != '0') ? 1 : 0;
		if (autoEnter) {
			GELog(@"[3d] GE_VP3D_AUTOENTER=1 — will enter 3D at frame 300");
		}
	}
	if (autoEnter == 1) {
		unsigned int frames = 0;
		geHostState(NULL, NULL, NULL, &frames);
		if (frames >= 300) {
			autoEnter = 2;   // once per process
			GELog(@"[3d] autoenter at frame %u", frames);
			geVision3dSetMode(true);
		}
	}
}

NSString *geVision3dStateLines(void)
{
	char angle[512];
	angle[0] = 0;
	geAngle3DStateLines(angle, (int)sizeof(angle));
	// pacing_mode: the live clock (M3, plan §2.6): `compositor` while the
	// immersive loop runs, `displaylink` otherwise. presents_hz: the engine's
	// own present rate over the last second (GEPacing), beside imm_hz.
	// M4 rows: parked/curtain/cycles, the scene and the drawable in pixels,
	// the overlay, the spatial intent, and how old midas.ini is.
	const CGSize sz = geVisionSceneSize();
	const CGFloat sc = geVisionPxScale();
	int dw = 0, dh = 0;
	geAngleGetDrawableSize(&dw, &dh);
	GETouchOverlay *ov = GETouchOverlay.shared;
	const char *overlay = !ov ? "none" : ((ov.superview && !ov.hidden && ov.alpha > 0.01) ? "shown" : "hidden");
	double iniAge = -1.0;
	struct stat st;
	NSString *ini = [GEShell.shared.documentsPath stringByAppendingPathComponent:@"midas.ini"];
	if (ini && stat(ini.fileSystemRepresentation, &st) == 0) {
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		iniAge = (double)(now.tv_sec - st.st_mtimespec.tv_sec)
		       + (double)(now.tv_nsec - st.st_mtimespec.tv_nsec) / 1e9;
	}
	return [NSString stringWithFormat:
		@"imm_running=%d\nimm_frames=%d\nimm_hz=%.1f\nactive=%d\n%@%spacing_mode=%s\n"
		@"parked=%d\nunparking=%d\ncurtain=%d\ncycles=%d\nfinalizing=%d\nspace_opened=%d\n"
		@"pre3d_pt=%.0fx%.0f\nscene_pt=%.0fx%.0f\nscene_px=%ldx%ld\ndrawable_px=%dx%d\n"
		@"geom_refusals=%d\noverlay=%s\nspatial=%@\nini_age_s=%.2f\nexit_pending=%d\n%@",
		(int)geVision3dImmRunning, (int)geVision3dImmFrames,
		(double)geVision3dImmHz, geVision3dOn, geVisionEyeStateLines(), angle,
		geIosPacingModeName(),
		geParked, geUnparking, geCurtain, geParkCycles, geFinalizing, geSpaceOpened,
		gePreSize.width, gePreSize.height, sz.width, sz.height,
		(long)lround(sz.width * sc), (long)lround(sz.height * sc), dw, dh,
		geGeomRefusals, overlay, GEAudio.spatialState, iniAge, geExitPending,
		geVision3dSettingsStateLines()];
}


// ---------------------------------------------------------------------------
// M6 (round 50): the device-round log. A headset has no console, so 3D writes
// what the verdict needs to Documents/logs/vision3d-<stamp>.log (one file per
// launch, opened on the first 3D entry; the newest 5 are kept):
//   - `[3d state] <why>` + the whole `3d state` contract block on every entry
//     (once the space has settled, +3 s, after the park) and on every exit;
//   - while immersive, one `xr3stat` line every 5 s: the rates over that
//     window plus the levels the verdict reads (eye size, render %, fold
//     generation, spatial intent, parked, thermal state).
// Main thread (the 100 ms graft-watch tick); a string build + one fputs per
// 5 s. Silent: file only, no NSLog.
// ---------------------------------------------------------------------------
#define GE_V3DLOG_KEEP 5
#define GE_V3DLOG_LINE_CAP 20000

static FILE *geV3dLog = NULL;
static int geV3dLogLines = 0;

static void geV3dLogOpen(void)
{
	if (geV3dLog) {
		return;
	}
	NSString *docs = GEShell.shared.documentsPath;
	if (!docs) {
		return;
	}
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *dir = [docs stringByAppendingPathComponent:@"logs"];
	[fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
	// Rotate: keep the newest KEEP-1 old files, so with this one there are KEEP.
	NSMutableArray<NSString *> *old = [NSMutableArray array];
	for (NSString *f in [fm contentsOfDirectoryAtPath:dir error:NULL] ?: @[]) {
		if ([f hasPrefix:@"vision3d-"] && [f hasSuffix:@".log"]) {
			[old addObject:f];
		}
	}
	[old sortUsingSelector:@selector(compare:)];   // the stamp sorts by time
	while ((int)old.count > GE_V3DLOG_KEEP - 1) {
		[fm removeItemAtPath:[dir stringByAppendingPathComponent:old.firstObject] error:NULL];
		[old removeObjectAtIndex:0];
	}
	NSDateFormatter *df = [NSDateFormatter new];
	df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
	df.dateFormat = @"yyyyMMdd-HHmmss";
	NSString *name = [NSString stringWithFormat:@"vision3d-%@.log", [df stringFromDate:NSDate.date]];
	geV3dLog = fopen([dir stringByAppendingPathComponent:name].fileSystemRepresentation, "w");
	if (!geV3dLog) {
		return;
	}
	NSDictionary *info = NSBundle.mainBundle.infoDictionary;
	fprintf(geV3dLog, "projectmidas vision3d log — %s (build %s) — %s\n",
		[info[@"CFBundleShortVersionString"] UTF8String] ?: "?",
		[info[@"CFBundleVersion"] UTF8String] ?: "?",
		NSDate.date.description.UTF8String);
	fflush(geV3dLog);
	GELog(@"[3d] device-round log: Documents/logs/%@", name);
}

static void geV3dLogWrite(NSString *text)
{
	geV3dLogOpen();
	if (!geV3dLog || geV3dLogLines > GE_V3DLOG_LINE_CAP) {
		return;
	}
	if (geV3dLogLines == GE_V3DLOG_LINE_CAP) {
		fputs("[log capped]\n", geV3dLog);
		++geV3dLogLines;
		fflush(geV3dLog);
		return;
	}
	geV3dLogLines += (int)[text componentsSeparatedByString:@"\n"].count;
	fputs(text.UTF8String, geV3dLog);
	if (![text hasSuffix:@"\n"]) {
		fputc('\n', geV3dLog);
	}
	fflush(geV3dLog);
}

static NSDictionary<NSString *, NSString *> *geV3dKV(NSString *block)
{
	NSMutableDictionary *d = [NSMutableDictionary dictionary];
	for (NSString *ln in [block componentsSeparatedByString:@"\n"]) {
		NSRange r = [ln rangeOfString:@"="];
		if (r.location != NSNotFound && r.location > 0) {
			d[[ln substringToIndex:r.location]] = [ln substringFromIndex:r.location + 1];
		}
	}
	return d;
}

static double geV3dNow(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void geV3dLogState(const char *why)
{
	geV3dLogWrite([NSString stringWithFormat:@"[3d state] %s t=%.1f thermal=%ld\n%@[3d state end]",
		why, geV3dNow(), (long)NSProcessInfo.processInfo.thermalState, geVision3dStateLines()]);
}

static void geVision3dLogTick(void)
{
	static int lastOn = 0;
	static double entryAt = 0.0, lastStat = 0.0;
	static int entryLogged = 0;
	static unsigned long long p0 = 0, pairs0 = 0, ticks0 = 0, imm0 = 0;
	const double now = geV3dNow();

	if (geVision3dOn != lastOn) {
		lastOn = geVision3dOn;
		if (geVision3dOn) {
			entryAt = now;
			entryLogged = 0;
			lastStat = 0.0;
		} else {
			geV3dLogState("exit");
		}
	}
	if (!geVision3dOn) {
		return;
	}
	if (!entryLogged && now - entryAt >= 3.0) {
		entryLogged = 1;
		geV3dLogState("entry");
	}
	if (!geVision3dImmRunning) {
		lastStat = 0.0;
		return;
	}
	if (lastStat != 0.0 && now - lastStat < 5.0) {
		return;
	}
	NSDictionary *s = geV3dKV(geVision3dStateLines());
	NSDictionary *pc = geV3dKV([GEPacing statsDescription]);
	unsigned int ticks = 0;
	geHostState(NULL, NULL, &ticks, NULL);
	const unsigned long long p = strtoull([pc[@"presents"] UTF8String] ?: "0", NULL, 10);
	const unsigned long long pairs = strtoull([s[@"eye_pairs"] UTF8String] ?: "0", NULL, 10);
	const unsigned long long imm = strtoull([s[@"imm_frames"] UTF8String] ?: "0", NULL, 10);
	if (lastStat != 0.0) {
		const double dt = now - lastStat;
		// Counters can be reset under us (bridge `resetStats`, a new 3D
		// session's pair count): a backwards step reads as 0, not garbage.
#define GE_RATE(a, b) ((a) >= (b) ? (double)((a) - (b)) / dt : 0.0)
		geV3dLogWrite([NSString stringWithFormat:
			@"xr3stat t=%.1f imm_hz=%@ imm_fps=%.1f presents=%.1f/s pairs=%.1f/s ticks=%.1f/s "
			@"eye_px=%@ render_pct=%@ inflight_peak=%@ eye_stalls=%@ pacing=%@ fold_gen=%@ "
			@"spatial=%@ parked=%@ dim=%@ panel=%@ thermal=%ld lowpower=%d",
			now, s[@"imm_hz"] ?: @"?", GE_RATE(imm, imm0), GE_RATE(p, p0), GE_RATE(pairs, pairs0),
			GE_RATE((unsigned long long)ticks, ticks0),
			s[@"eye_px"] ?: @"?", s[@"render_pct"] ?: @"?", s[@"inflight_peak"] ?: @"?",
			s[@"eye_stalls"] ?: @"?", s[@"pacing_mode"] ?: @"?", s[@"fold_gen"] ?: @"?",
			s[@"spatial"] ?: @"?", s[@"parked"] ?: @"?", s[@"dim"] ?: @"?", s[@"panel_m"] ?: @"?",
			(long)NSProcessInfo.processInfo.thermalState,
			NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0]);
#undef GE_RATE
	}
	lastStat = now;
	p0 = p;
	pairs0 = pairs;
	ticks0 = ticks;
	imm0 = imm;
}

// ---------------------------------------------------------------------------
// The engine bootstrap
// ---------------------------------------------------------------------------

@implementation GEHostViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.blackColor;
}

- (void)viewDidAppear:(BOOL)animated
{
	[super viewDidAppear:animated];
	if (geVisionBootScheduled) {
		return;
	}
	geVisionBootScheduled = YES;
	// One run-loop hop, so the window scene is fully active before SDL goes
	// looking for one — but via a RUN LOOP TIMER, not dispatch_async(main).
	// performSelector:afterDelay: schedules a CFRunLoopTimer: same one hop,
	// same thread, and not a block on the serial main QUEUE. On PD that
	// difference was everything (its engine never returns; a main-queue block
	// that starts it holds the queue for ever and every SwiftUI effect dies,
	// sm64coopdx M-38). Here geSDLMain returns, but onboarding still runs a
	// nested run loop inside it for as long as the ROM is missing, and that
	// must not happen inside a main-queue block either.
	[self performSelector:@selector(geBootEngine) withObject:nil afterDelay:0.0];
}

- (void)geBootEngine
{
	// The process arguments, which on visionOS are all we get: there is no
	// main() to be handed argv, and `xcrun simctl launch --args` lands in
	// NSProcessInfo exactly the same way. geShellPrepare() then appends
	// Documents/ge.args, which is the device's only way to pass a flag.
	NSArray<NSString *> *args = NSProcessInfo.processInfo.arguments;
	int argc = (int)args.count;
	char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
	for (int i = 0; i < argc; i++) {
		argv[i] = strdup(args[i].UTF8String ?: "");
	}
	argv[argc] = NULL;

	int n = 0;
	char **assembled = geShellPrepare(argc, argv, &n);

	// GESceneDelegate's connect line, which the visionOS gate reads; the scene
	// is SwiftUI's WindowGroup now, and it connected before any of our code.
	UIWindowScene *scene = self.view.window.windowScene;
	GELog(@"[scene] connected %@ (role %@, state %ld) — SwiftUI WindowGroup host",
	      scene.session.persistentIdentifier, scene.session.role,
	      (long)scene.activationState);
	GELog(@"[3d] SwiftUI shell — booting the engine");

	geVisionInstallTermFlush();
	SDL_SetMainReady();
	int rc = geSDLMain(n, assembled);
	// EXPECTED on this tree (unlike PD): the sim thread owns the game loop.
	geVisionBooted = YES;
	// M5: vp3d.* -> 3D state once at boot (panel, stereo, eye size).
	geVision3dApplySettings();
	GELog(@"[3d] geSDLMain returned %d — engine on ge-sim, main thread back to UIKit", rc);
}

@end

#endif // TARGET_OS_VISION
