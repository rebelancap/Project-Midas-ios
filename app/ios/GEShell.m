// GEShell.m — paths and the window graft. See GEShell.h for the two rules.
#import "GEShell.h"
#import "GETouchOverlay.h"
#import "GESettings.h"
#import "GEController.h"
#import "GEVision.h"
#if TARGET_OS_VISION
#import "GEVision3D.h"   // app/vision3d, visionOS target only (round 45)
#endif

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#import <QuartzCore/QuartzCore.h>

// app/gfx/gfx_angle_egl.mm
extern void *geAngleGetHostView(void);
extern void geAngleGetDrawableSize(int *w, int *h);

// port/src/video.c, port/include/hostinput.h (D-048)
extern void videoSetSafeInsetFrac(float l, float t, float r, float b);
extern int  geHostConfigGet(const char *key, char *out, int outLen);

// ---------------------------------------------------------------------------
// Documents/shell.log — the device's only window onto the shell (GEShell.h).
// ---------------------------------------------------------------------------

static const long kShellLogCap = 512 * 1024;

void geShellLogLine(NSString *line)
{
	NSLog(@"projectmidas: %@", line);

	static FILE *f = NULL;
	static long written = 0;
	static NSString *path = nil;
	static NSLock *lock = nil;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		lock = [NSLock new];
		path = [GEShell.shared.documentsPath stringByAppendingPathComponent:@"shell.log"];
		// "w", not "a": this file answers "what did THIS run do". A device
		// round pulls it straight after the launch it is asking about, and a
		// log that carries six previous launches is a log nobody reads.
		f = fopen(path.fileSystemRepresentation, "w");
		if (f) {
			NSString *head = [NSString stringWithFormat:@"projectmidas shell log — %@ — %@\n",
			                  NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"],
			                  [NSDate date]];
			int n = fprintf(f, "%s", head.UTF8String);
			written = (n > 0) ? n : 0;
			fflush(f);
		}
	});
	if (!f) {
		return;
	}
	[lock lock];
	if (written < kShellLogCap) {
		int n = fprintf(f, "%8.3f %s\n", CACurrentMediaTime(), line.UTF8String ?: "");
		if (n > 0) {
			written += n;
		}
		// Flushed every line on purpose: the interesting runs are the ones
		// that end in a crash or a kill, and a buffered tail is the half of
		// the story those runs lose.
		fflush(f);
		if (written >= kShellLogCap) {
			fprintf(f, "--- shell.log capped at %ld bytes ---\n", kShellLogCap);
			fflush(f);
		}
	}
	[lock unlock];
}

// ---------------------------------------------------------------------------
// Documents/engine.log — the ENGINE's stdout/stderr (GEShell.h, D-055).
// ---------------------------------------------------------------------------

static const long kEngineLogCap = 4 * 1024 * 1024;

static FILE *sEngineLog = NULL;
static long  sEngineLogWritten = 0;
static int   sEngineLogPipe[2] = { -1, -1 };
/* The fd stdout pointed at BEFORE the dup2. On the simulator that is
 * `simctl launch --console-pty`'s pty, and scripts/sim-validate.sh greps it
 * for engine lines — so the reader tees every chunk back to it and the gate
 * keeps working unchanged. On a device it is whatever the launchd session
 * handed the process, usually a dead end, and a failed write costs nothing. */
static int   sEngineLogTee = -1;

static void *geEngineLogReader(void *unused)
{
	(void)unused;
	pthread_setname_np("ge.enginelog");
	char buf[4096];
	for (;;) {
		ssize_t n = read(sEngineLogPipe[0], buf, sizeof buf);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (n == 0) {
			break;   // every writer closed; the process is going away
		}
		// Past the cap we keep READING and stop WRITING. Dropping the reader
		// instead would fill the 64 KiB pipe buffer and then block the engine
		// inside fprintf() — a logger that deadlocks the game it instruments.
		if (sEngineLogTee >= 0) {
			ssize_t off = 0;
			while (off < n) {
				ssize_t w = write(sEngineLogTee, buf + off, (size_t)(n - off));
				if (w <= 0) {
					if (w < 0 && errno == EINTR) {
						continue;
					}
					close(sEngineLogTee);
					sEngineLogTee = -1;   // gone; stop trying
					break;
				}
				off += w;
			}
		}
		if (sEngineLogWritten < kEngineLogCap) {
			fwrite(buf, 1, (size_t)n, sEngineLog);
			sEngineLogWritten += n;
			// Flushed per chunk on purpose: the runs worth reading are the
			// ones that end in a crash or a kill.
			fflush(sEngineLog);
			if (sEngineLogWritten >= kEngineLogCap) {
				fprintf(sEngineLog, "\n--- engine.log capped at %ld bytes ---\n", kEngineLogCap);
				fflush(sEngineLog);
			}
		}
	}
	return NULL;
}

void geEngineLogStart(NSString *docs)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSString *path = [docs stringByAppendingPathComponent:@"engine.log"];
		sEngineLog = fopen(path.fileSystemRepresentation, "w");
		if (!sEngineLog) {
			return;
		}
		NSString *head = [NSString stringWithFormat:
			@"projectmidas engine log — %@ (build %@) — %@\n"
			@"stdout+stderr of the engine: sysLogPrintf, osSyncPrintf, fast3d.\n"
			@"------------------------------------------------------------\n",
			NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"],
			NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"],
			[NSDate date]];
		sEngineLogWritten = fprintf(sEngineLog, "%s", head.UTF8String);
		fflush(sEngineLog);

		if (pipe(sEngineLogPipe) != 0) {
			fprintf(sEngineLog, "!! pipe() failed (%d) — engine output stays lost\n", errno);
			fflush(sEngineLog);
			return;
		}

		// The engine's fds 1 and 2 become the pipe. Nothing else in the app
		// writes to them (the shell logs through NSLog + shell.log), so this
		// captures the engine and only the engine.
		sEngineLogTee = dup(STDOUT_FILENO);
		dup2(sEngineLogPipe[1], STDOUT_FILENO);
		dup2(sEngineLogPipe[1], STDERR_FILENO);
		close(sEngineLogPipe[1]);
		sEngineLogPipe[1] = -1;

		// Line buffering for stdout (a pipe is not a tty, so libc would
		// otherwise hold 4 KiB — and the per-second [render] line is only
		// useful if it is in the file when the phone is picked up). stderr is
		// unbuffered already; say so rather than assume it.
		setvbuf(stdout, NULL, _IOLBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		pthread_t t;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_create(&t, &attr, geEngineLogReader, NULL);
		pthread_attr_destroy(&attr);

		GELog(@"[enginelog] engine stdout/stderr -> %@ (cap %ld bytes)", path, kEngineLogCap);
	});
}

UIEdgeInsets geShellSafeInsets(UIView *view)
{
	UIEdgeInsets s = view.safeAreaInsets;
	CGSize b = view.bounds.size;
	if (b.width > b.height && s.left == 0.0 && s.right == 0.0 && s.top > 20.0) {
		UIEdgeInsets fixed = UIEdgeInsetsMake(0.0, s.top, s.bottom, s.top);
		static BOOL said = NO;
		if (!said) {
			said = YES;
			GELog(@"[scene] portrait-shaped safe area on a %.0fx%.0f view "
			      @"(%.0f,%.0f,%.0f,%.0f) — corrected to %.0f,%.0f,%.0f,%.0f",
			      b.width, b.height, s.top, s.left, s.bottom, s.right,
			      fixed.top, fixed.left, fixed.bottom, fixed.right);
		}
		return fixed;
	}
	return s;
}

@implementation GEShell {
	NSString *_documentsPath;
	NSString *_cachesPath;
}

+ (GEShell *)shared
{
	static GEShell *s;
	static dispatch_once_t once;
	dispatch_once(&once, ^{ s = [GEShell new]; });
	return s;
}

- (NSString *)documentsPath
{
	if (!_documentsPath) {
		_documentsPath = NSSearchPathForDirectoriesInDomains(
			NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
	}
	return _documentsPath;
}

- (NSString *)cachesPath
{
	if (!_cachesPath) {
		_cachesPath = NSSearchPathForDirectoriesInDomains(
			NSCachesDirectory, NSUserDomainMask, YES).firstObject;
	}
	return _cachesPath;
}

- (NSString *)bundlePath
{
	return NSBundle.mainBundle.bundlePath;
}

/**
 * Round 12b / D-048: the safe-area inset, as a FRACTION of the view, handed to
 * the engine (port/src/video.c videoSetSafeInsetFrac) so that anything drawn
 * in a screen corner clears the Dynamic Island and the home indicator.
 *
 * A fraction because the engine draws in the game's own 2D space (320x240 in
 * gameplay, 440x330 on a front-end screen) and has no notion of a point; the
 * corrected insets because SDL's window reports a PORTRAIT inset set in
 * landscape (geShellSafeInsets, round 11a), which on the FPS readout would
 * have moved the number down by a sixth of the screen for nothing.
 */
+ (void)publishSafeInsets:(UIView *)view
{
	CGSize b = view.bounds.size;
	if (b.width <= 1.0 || b.height <= 1.0) {
		return;
	}
	UIEdgeInsets ins = geShellSafeInsets(view);
#if TARGET_OS_VISION
	// Round 35: the window now wears the system's rounded corners
	// (geVisionWearWindowCorners), which clip whatever sits in a corner -- the
	// FPS strip in particular. A point d in from both edges clears a corner of
	// radius r when d >= r (1 - 1/sqrt 2) ~ 0.3 r: 14 pt at the 46-pt radius.
	// The engine's view only; the overlay and the gear keep their own layout.
	{
		UIWindow *w = [view isKindOfClass:UIWindow.class] ? (UIWindow *)view : view.window;
		const CGFloat c = ceil(w.layer.cornerRadius * 0.3);
		ins.left = MAX(ins.left, c);
		ins.top = MAX(ins.top, c);
		ins.right = MAX(ins.right, c);
		ins.bottom = MAX(ins.bottom, c);
	}
#endif
	float l = (float)(ins.left / b.width);
	float t = (float)(ins.top / b.height);
	float r = (float)(ins.right / b.width);
	float bo = (float)(ins.bottom / b.height);

	static float was[4] = { -1.f, -1.f, -1.f, -1.f };
	if (l != was[0] || t != was[1] || r != was[2] || bo != was[3]) {
		was[0] = l; was[1] = t; was[2] = r; was[3] = bo;
		GELog(@"[scene] safe inset %.0f,%.0f,%.0f,%.0f pt of %.0fx%.0f "
		      @"-> frac %.4f,%.4f,%.4f,%.4f",
		      ins.left, ins.top, ins.right, ins.bottom, b.width, b.height,
		      l, t, r, bo);
	}
	videoSetSafeInsetFrac(l, t, r, bo);
}

/**
 * What the renderer is ACTUALLY drawing into, said once, in the one log a
 * device read can reach.
 *
 * Austin, on 0.0.0.3: "does 100 % render scale mean the iPhone's native
 * resolution, or the game's original 320x240?" D-048 makes the answer "native
 * pixels" true by construction; this line is how the next device read
 * CONFIRMS it, rather than taking the code's word for it. `scale` is the
 * drawable divided by the points it covers — 3.00 on an iPhone Air at 100 %.
 */
+ (void)logDrawableSize
{
	int dw = 0, dh = 0;
	geAngleGetDrawableSize(&dw, &dh);
	UIView *host = (__bridge UIView *)geAngleGetHostView();
	CGSize pts = host ? host.bounds.size : CGSizeZero;
	// Midas has no render-scale key; its 3D resolution is low_res (0 = native).
	char rs[32] = "?";
	geHostConfigGet("low_res", rs, (int)sizeof(rs));
	GELog(@"[video] drawable %dx%d, points %.0fx%.0f, scale %.2f, "
	      @"low_res %s (0 = native panel pixels)",
	      dw, dh, pts.width, pts.height,
	      pts.width > 0 ? dw / pts.width : 0.0, rs);
}

#if TARGET_OS_VISION
/**
 * Round 35 (Austin, first headset run of 0.0.0.11: "the window does NOT have
 * rounded corners like a visionOS window should").
 *
 * visionOS rounds the windows it manages. SDL2's UIWindow is born sceneless
 * and is adopted into the scene by geGraftSDLWindows() as a SECOND window,
 * which the system does not round; it is opaque and Metal-backed across the
 * whole scene, so it paints square corners over the rounded ones. The recipe
 * is the sibling family's (perfect-dark-ios PDHostViewController.m "WEAR THE
 * SYSTEM'S ROUNDED CORNERS", HarbourMasters VISION-PRO-LUS-PLAYBOOK): read the
 * radius off another window of the scene when the system gave it one, else
 * 46 pt continuous, and clip. masksToBounds is the half that does the work: a
 * radius alone rounds the layer's own background and clips nothing, so the
 * CAMetalLayer (and the touch overlay) would keep painting the corners.
 *
 * Re-asked on the graft-watch tick with everything else; it assigns only when
 * a value differs, so it costs a compare.
 */
#define GE_VISION_CORNER_FALLBACK 46.0

static void geVisionWearWindowCorners(UIWindow *win, UIWindowScene *scene)
{
	UIWindow *primary = nil;
	for (UIWindow *w in scene.windows) {
		if (w == win) {
			continue;
		}
		if (w.layer.cornerRadius > 0.0) {
			primary = w;
			break;
		}
	}
	CGFloat r = primary ? primary.layer.cornerRadius : GE_VISION_CORNER_FALLBACK;
	CALayerCornerCurve curve = primary ? primary.layer.cornerCurve : kCACornerCurveContinuous;
	if (win.layer.cornerRadius == r && win.layer.masksToBounds &&
	    [win.layer.cornerCurve isEqualToString:curve]) {
		return;
	}
	win.layer.cornerRadius = r;
	win.layer.cornerCurve = curve;
	win.layer.masksToBounds = YES;
	GELog(@"[scene] window corners radius %.1f curve %@ (%@) on %@",
	      (double)r, curve, primary ? @"read from the scene's own window" : @"fallback",
	      NSStringFromCGRect(win.bounds));
}

/**
 * Round 35 (Austin: "when I drag to resize the window, I can squeeze the
 * window to make it skinny ... It should fill a rounded corner window and
 * should not distort the picture at all ... If not, then it should stay fixed
 * to the aspect ratio it has").
 *
 * The engine is Hor+ from 4:3 up (video.c portWideFactor: the projection
 * widens to the window, the 2D stays in a centred 4:3 band; checked to 32:9),
 * so any window at least as wide as 4:3 fills without bars or stretch in
 * gameplay. Below 4:3 it has nothing to widen: portWideFactor floors at 1 and
 * the 4:3 frame is stretched taller. Vert+ (a taller FOV with the 2D in a
 * vertical band) is the mirror image of the whole Hor+ pin and is not in this
 * round (doc 46, R35-1).
 *
 * So the window's aspect has a FLOOR at 4:3 and no ceiling: free to go as
 * wide as visionOS allows, and a drag that leaves it taller than 4:3 is
 * answered, once the drag is over, with a geometry request back to 4:3. The
 * dimension the player grew is the one handed back (taller -> the height
 * returns to width x 3/4; narrower -> the width returns to height x 4/3). A
 * scene-level aspect lock (UIWindowSceneResizingRestrictionsUniform) would
 * also remove the wide windows, which work.
 */
static void geVisionAspectFloor(UIWindowScene *scene)
{
	static CGSize lastGood = { 0.0, 0.0 };
	static CFAbsoluteTime lastAsk = 0.0;
	static CGSize lastAsked = { 0.0, 0.0 };

	UIWindowSceneGeometry *g = scene.effectiveGeometry;
	const CGSize b = g ? g.coordinateSpace.bounds.size : scene.coordinateSpace.bounds.size;
	if (b.width <= 1.0 || b.height <= 1.0) {
		return;
	}
	const CGFloat a = b.width / b.height;
	if (a >= (4.0 / 3.0) * 0.995) {
		lastGood = b;
		return;
	}
	if (g.isInteractivelyResizing) {
		return;   // let the drag finish; it is answered on release
	}
	CGSize want;
	if (lastGood.height > 1.0 && b.height > lastGood.height + 0.5) {
		want = CGSizeMake(b.width, round(b.width * 3.0 / 4.0));
	} else {
		want = CGSizeMake(round(b.height * 4.0 / 3.0), b.height);
	}
	const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (CGSizeEqualToSize(want, lastAsked) && now - lastAsk < 1.0) {
		return;   // asked already; give the system a moment to answer
	}
	lastAsk = now;
	lastAsked = want;
	UIWindowSceneGeometryPreferencesVision *prefs =
		[[UIWindowSceneGeometryPreferencesVision alloc] initWithSize:want];
	[scene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError *e) {
		GELog(@"[video] aspect floor: the request for %.0fx%.0f was refused: %@",
		      want.width, want.height, e);
	}];
	GELog(@"[video] aspect floor: window %.0fx%.0f (%.3f) is taller than 4:3 -> asked for %.0fx%.0f",
	      b.width, b.height, a, want.width, want.height);
}
#endif

/**
 * Round 15a / D-059 — see GEShell.h for why this exists at all.
 *
 * The rule, and it is one rule rather than a list of transients to recognise:
 * SDL's window must always have the SCENE's own bounds, and on a
 * landscape-only iPhone a portrait-shaped answer is not an answer. Asked
 * every 100 ms, never remembered.
 */
+ (BOOL)enforceWindowGeometry
{
	UIView *host = (__bridge UIView *)geAngleGetHostView();
	UIWindow *win = host.window;
	UIWindowScene *scene = win.windowScene;
	if (!host || !win || !scene) {
		return NO;
	}

	CGRect want = scene.coordinateSpace.bounds;
	if (want.size.width <= 1.0 || want.size.height <= 1.0) {
		return NO;
	}

	// The scene itself can be reported portrait during the transient. The app
	// is landscape-only on an iPhone (Info.plist), so that is never a size we
	// may render at: fall back to the last landscape rect we DID see, and if
	// there has not been one yet, to the screen's own landscape shape.
	static CGSize lastGood = { 0.0, 0.0 };
	const BOOL phone = (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone);
	if (phone && want.size.height > want.size.width) {
		CGSize fallback = lastGood;
		if (fallback.width <= 1.0) {
			fallback = CGSizeMake(MAX(want.size.width, want.size.height),
			                      MIN(want.size.width, want.size.height));
		}
		want = CGRectMake(0.0, 0.0, fallback.width, fallback.height);
	} else {
		lastGood = want.size;
	}

	BOOL fixed = NO;
	if (!CGSizeEqualToSize(win.bounds.size, want.size)) {
		CGSize was = win.bounds.size;
		win.frame = want;
		[win layoutIfNeeded];
		GELog(@"[video] window %.0fx%.0f -> %.0fx%.0f (scene %.0fx%.0f) — "
		      @"UIKit left SDL's window the wrong shape; put back",
		      was.width, was.height, want.size.width, want.size.height,
		      scene.coordinateSpace.bounds.size.width,
		      scene.coordinateSpace.bounds.size.height);
		fixed = YES;
	}

	// SDL's Metal view is a subview of the view controller's view and does not
	// always carry an autoresizing mask that survives a frame set from the
	// outside, so the renderer's own view is re-asked too. This is what the
	// CAMetalLayer's bounds -- and therefore ANGLE's drawable -- follow.
	if (!CGSizeEqualToSize(host.bounds.size, win.bounds.size)) {
		CGSize was = host.bounds.size;
		host.frame = win.bounds;
		[host layoutIfNeeded];
		GELog(@"[video] host view %.0fx%.0f -> %.0fx%.0f (window)",
		      was.width, was.height, win.bounds.size.width, win.bounds.size.height);
		fixed = YES;
	}

#if TARGET_OS_VISION
	// Round 45 (Phase 6 M1): under the SwiftUI entry the scene carries TWO
	// windows - SwiftUI's WindowGroup window (GEHostViewController + the
	// ornament) and SDL's, grafted on. At the same level their order is
	// whatever UIKit last did with them, and SwiftUI's enumerates first, so
	// a pinch (and the gate's hittest) could land in SwiftUI's empty black
	// host instead of the touch overlay. SDL's window is therefore one level
	// ABOVE normal on visionOS - still below the settings page (+90) and
	// onboarding (+100). Re-asked every tick like everything else here.
	if (win.windowLevel < UIWindowLevelNormal + 1.0) {
		GELog(@"[scene] SDL's window level %.0f -> %.0f (above SwiftUI's host window)",
		      (double)win.windowLevel, (double)(UIWindowLevelNormal + 1.0));
		win.windowLevel = UIWindowLevelNormal + 1.0;
	}
	geVisionWearWindowCorners(win, scene);
	// Round 48 (Phase 6 M4): the floor stands down while the scene is parked
	// as the 480-pt card (or a park/un-park is in flight) - the card's shape is
	// the panel's, not the player's, and a floor request racing the park would
	// re-grow it. Re-applied on the first tick after the un-park has landed.
	if (!geVision3dWindowParked()) {
		geVisionAspectFloor(scene);
	}
#endif

	if (fixed) {
		[GEShell noteDrawableSize:@"window geometry put back"];
	}
	return fixed;
}

+ (void)noteDrawableSize:(NSString *)reason
{
	static int wasW = -1, wasH = -1;
	int dw = 0, dh = 0;
	geAngleGetDrawableSize(&dw, &dh);
	if (dw <= 0 || dh <= 0 || (dw == wasW && dh == wasH)) {
		return;
	}
	UIView *host = (__bridge UIView *)geAngleGetHostView();
	CGSize pts = host ? host.bounds.size : CGSizeZero;
	GELog(@"[video] drawable %dx%d -> %dx%d, points %.0fx%.0f (%@)",
	      wasW, wasH, dw, dh, pts.width, pts.height, reason ?: @"?");
	wasW = dw; wasH = dh;
}

+ (void)startGraftWatch
{
	// Every 100 ms, for the life of the app. This rides on rule 2 in
	// GEShell.h: the engine's own host loop calls SDL_PumpEvents(), which on
	// iOS runs CFRunLoopRunInMode, so a default-mode timer scheduled here does
	// fire even though the main thread never returns to UIKit's own run loop.
	//
	// ROUND 11a, and this is the class fix, not a tweak. The old version tied
	// the overlay to the GRAFT: it only tried [GETouchOverlay install] after
	// geGraftSDLWindows() had returned > 0, and it invalidated itself the
	// moment that happened once. Three ways that loses the controls outright,
	// all of them invisible in a log:
	//
	//   1. a build/OS where SDL's window is NOT sceneless needs no graft, so
	//      `moved` is 0 for ever and the overlay is never installed at all;
	//   2. the graft can succeed against a window that is not the one the
	//      renderer ends up on, and the overlay went wherever the graft went;
	//   3. once the timer had stopped, an overlay that LOST its superview
	//      (SDL replacing its window, a settings window taking key) never
	//      came back.
	//
	// So: the graft and the install are now two independent idempotent
	// questions asked on the same cadence, for ever, and the answer to "is
	// the overlay installed" is re-asked rather than remembered. The cost is
	// one pointer compare per 100 ms.
	__block int ticks = 0;
	__block BOOL announced = NO;
	NSTimer *t = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
		(void)timer;
		geGraftSDLWindows();

		// D-060 (round 15a). `idleTimerDisabled = YES` is set once in
		// geSDLMain(), which runs before the scene is active -- and a value
		// set then does not always take, so the phone still dims and locks
		// itself in the middle of a mission. It cost this round its first two
		// Instruments traces (a locked phone is OFFLINE to xctrace) before
		// anyone noticed it was happening to Austin too. Re-asked on the same
		// tick as everything else here, for the same reason: the answer is
		// re-derived, never remembered.
		if (!UIApplication.sharedApplication.idleTimerDisabled) {
			UIApplication.sharedApplication.idleTimerDisabled = YES;
			static BOOL saidIdle = NO;
			if (!saidIdle) {
				saidIdle = YES;
				GELog(@"[scene] idle timer was ENABLED — re-disabled (the phone "
				      @"was free to lock itself mid-mission)");
			}
		}

		// D-059: before anything that reads the window's size, put the window
		// back if UIKit left it the wrong shape. Same cadence, same "re-ask,
		// never remember" rule as the graft and the overlay install.
		[GEShell enforceWindowGeometry];
		[GEShell noteDrawableSize:@"graft watch"];

#if TARGET_OS_VISION
		// Round 45 (Phase 6 M1, plan §2.1): the 3D poll (GE_VP3D_AUTOENTER)
		// rides this timer - the main thread is free on this tree, so no
		// engine frame hook is needed (PD's pdVision3dFramePoll).
		geVision3dPoll();
#endif

		// Never gated on the graft. A window that already had a scene is a
		// window the overlay belongs on just the same.
		[GETouchOverlay install];
		if (GETouchOverlay.shared.superview) {
			// D-048: publish the host's safe area to the engine, and say
			// once what the renderer actually got. Both live here because
			// this is the one place that knows SDL's window exists AND runs
			// on the main thread; both are cheap enough to re-ask at 10 Hz
			// (the insets change when the phone is rotated, and nothing
			// posts a notification the engine could hear).
			[GEShell publishSafeInsets:GETouchOverlay.shared.superview];
			// The gear goes on the same window and needs the overlay to
			// exist first (it is placed as the overlay's sibling, above
			// it — GESettings.h). Same timer, same question.
			[GESettings installGear];
			// visionOS: claim the pad's events for this window (a no-op on
			// iOS; GEController.m says why). Idempotent per window.
			[GEController claimGamepadEventsInWindow:GETouchOverlay.shared.window];
			if (!announced) {
				announced = YES;
				GELog(@"[scene] overlay + gear in place after %.1f s", ticks * 0.1);
				[GEShell logDrawableSize];
			}
		} else if (++ticks == 300) {
			// Said once, and the watch keeps running: 30 s with no window is
			// worth a line in shell.log, not a reason to stop looking.
			GELog(@"[scene] STILL no window to host the overlay after 30 s");
		}
	}];
	[NSRunLoop.mainRunLoop addTimer:t forMode:NSRunLoopCommonModes];
}

@end

static UIWindowScene *geForegroundScene(void)
{
	UIWindowScene *any = nil;
	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
		if (![scene isKindOfClass:UIWindowScene.class]) {
			continue;
		}
		if (scene.activationState == UISceneActivationStateForegroundActive) {
			return (UIWindowScene *)scene;
		}
		if (!any) {
			any = (UIWindowScene *)scene;
		}
	}
	return any;
}

int geGraftSDLWindows(void)
{
	UIWindowScene *scene = geForegroundScene();
	int moved = 0;

	if (!scene) {
		return 0;
	}

	// UIApplication.windows and scene.windows BOTH omit a window that has no
	// scene, so neither can be used to FIND the one window that needs this.
	// The renderer's own view is the handle that always works.
	UIView *host = (__bridge UIView *)geAngleGetHostView();
	NSMutableArray<UIWindow *> *candidates = [NSMutableArray array];
	if (host.window) {
		[candidates addObject:host.window];
	}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	for (UIWindow *w in UIApplication.sharedApplication.windows) {
		if (![candidates containsObject:w]) {
			[candidates addObject:w];
		}
	}
#pragma clang diagnostic pop

	for (UIWindow *w in candidates) {
		if (w.windowScene) {
			continue;
		}
		w.windowScene = scene;
		[w makeKeyAndVisible];
		moved++;
		GELog(@"[scene] grafted a sceneless window %@ (root %@) onto %@",
			NSStringFromCGRect(w.bounds), NSStringFromClass(w.rootViewController.class),
			scene.session.persistentIdentifier);
	}
	return moved;
}
