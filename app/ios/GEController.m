// GEController.m — see GEController.h for the scheme and why.
#import "GEController.h"
#import "GEInput.h"
#import "GEDefaults.h"
#import "GEPad.h"
#import "GETouchOverlay.h"
#import "GEShell.h"

#import <GameController/GameController.h>
#include <math.h>
#include <os/lock.h>

// port/include/hostinput.h
extern void geHostPadStats(unsigned int *sets, unsigned int *edges,
                           unsigned int *lastButton);
// port/src/input_pc.c (GE_IOS): player slots Midas's own SDL pad path holds.
extern int geHostSdlPadsOpen(void);
// Round 57 (docs/port-ios/77): port/apple/hostinput.c, read by bondview2.c.
extern void geHostSetPadLook(float scaleX, float scaleY, int invert);

@implementation GEController

// ---- round 29d (D-233..D-235): up to four controllers ------------------------
//
// Controller N holds controller PORT N-1 for as long as it is connected, in
// connection order: the first controller to connect is port 0 (player 1,
// merged with touch and the bridge exactly as the one adopted pad always was),
// the next is port 1 (player 2), and so on. A disconnect frees THAT port only —
// nobody shifts down, because shifting would hand player 3's split-screen
// quadrant to player 4 in the middle of a match. A controller that comes back
// reclaims its old port (same GCController object, else the most recent port
// freed by a controller with the same vendor name, else the lowest free port).
// The game is told highest-held-port + 1 controllers, never fewer than 1, so a
// phone with no controller still has player 1 on touch.
//
// sSlots is touched on the main queue (connect/disconnect, the dev bridge's
// virtual pads) and read on the pacer's thread (pollWithDelta:), hence sLock.
#define GE_SLOTS 4
static GCController *sSlots[GE_SLOTS];
static __weak GCController *sSlotLastObj[GE_SLOTS];
static NSString *sSlotLastVendor[GE_SLOTS];
static unsigned sSlotFreedSerial[GE_SLOTS];
static unsigned sFreeSerial = 0;
static os_unfair_lock sLock = OS_UNFAIR_LOCK_INIT;
// Dev bridge only (D-235): while virtual mode is on, real controllers are
// released and ignored, and the bridge's snapshot controllers go through the
// same adopt/drop path a connect notification takes.
static BOOL sVirtualMode = NO;
static NSMutableArray<GCController *> *sVirtual = nil;

/// A snapshot controller has no vendor name (read-only, nil); the virtual
/// ones are called "Virtual <k>" in logs and `state`.
static NSString *geName(GCController *c)
{
	if (c.vendorName) {
		return c.vendorName;
	}
	NSUInteger k = [sVirtual indexOfObjectIdenticalTo:c];
	return k != NSNotFound ? [NSString stringWithFormat:@"Virtual %lu", (unsigned long)(k + 1)] : @"controller";
}

static int geSlotOf(GCController *c)
{
	for (int i = 0; i < GE_SLOTS; i++) {
		if (sSlots[i] == c) {
			return i;
		}
	}
	return -1;
}

/// Port count the game sees, the overlay flags and the pad lights.
/// Caller holds sLock.
static NSInteger gePublishSlotsLocked(void)
{
	NSInteger count = 1;
	for (int i = 0; i < GE_SLOTS; i++) {
		if (sSlots[i]) {
			count = i + 1;
		}
	}
	return count;
}

// visionOS (round 37a, D-344; perfect-dark-ios PDController.m :88-115). On
// xrOS a connected controller's buttons are routed into the system's
// pointer/selection machinery — A behaves like a pinch on whatever the user
// is looking at — unless a view in the app claims GCUIEventTypeGamepad with a
// GCEventInteraction. Attached to SDL's UIWindow (the window the overlay and
// the gear live on). Remembered per window, weakly: if SDL's window is ever
// replaced, the next graft-watch tick claims the new one.
+ (void)claimGamepadEventsInWindow:(UIWindow *)window
{
#if TARGET_OS_VISION
	static __weak UIWindow *claimed = nil;
	if (!window || window == claimed) {
		return;
	}
	claimed = window;
	GCEventInteraction *gamepad = [[GCEventInteraction alloc] init];
	gamepad.handledEventTypes = GCUIEventTypeGamepad;
	[window addInteraction:gamepad];
	GELog(@"[pad] GCEventInteraction installed (GCUIEventTypeGamepad claimed) on %@",
	      NSStringFromCGRect(window.bounds));
#else
	(void)window;
#endif
}

/// Round 57 (docs/port-ios/77). Settings -> Controller's sensitivity sliders
/// and invert switch, pushed to the engine for every pad SDL owns. Since round
/// 9c that is every real controller on a phone, and Midas's own Look-stick
/// code (bondview2.c) turns the view, so the shell's own mapping (geMapGamepad)
/// never sees them. The slider value is a multiple of the factory 160 deg/s:
/// 1.0 = Midas's own rate. Idempotent; the engine logs only a change.
+ (void)applyPadLookToEngine
{
	const double ref = GEPadLookFactoryDegPerSec;
	const double x = [GEDefaults doubleFor:GEDefaultsPadLookDegPerSec] / ref;
	const double y = [GEDefaults doubleFor:GEDefaultsPadLookDegPerSecY] / ref;
	geHostSetPadLook((float)x, (float)y, [GEDefaults boolFor:GEDefaultsPadLookInvert] ? 1 : 0);
}

+ (void)start
{
	// Round 57: the sliders follow every write (Settings, the bridge's
	// `settings set|unset`, Reset), and the stored values apply at launch.
	[self applyPadLookToEngine];
	[NSNotificationCenter.defaultCenter addObserverForName:NSUserDefaultsDidChangeNotification object:nil
	                                                queue:NSOperationQueue.mainQueue
	                                           usingBlock:^(NSNotification *n) {
		(void)n;
		[self applyPadLookToEngine];
	}];

	// Value handlers are deliberately NOT installed: we poll in pollWithDelta:
	// from the pacer, so the pad is sampled on the same cadence as everything
	// else and a burst of handler callbacks cannot publish four pads between
	// two sim ticks.
	NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
	[nc addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
	            usingBlock:^(NSNotification *n) {
		if (!sVirtualMode) {
			[self adopt:(GCController *)n.object];
		}
	}];
	[nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
	            usingBlock:^(NSNotification *n) {
		[self drop:(GCController *)n.object];
		// A pad that is still connected but held no port (a fifth one, or
		// one that arrived while all four were taken) gets the freed port.
		if (!sVirtualMode) {
			for (GCController *c in GCController.controllers) {
				[self adopt:c];
			}
		}
	}];

	for (GCController *c in GCController.controllers) {
		[self adopt:c];
	}
	[GCController startWirelessControllerDiscoveryWithCompletionHandler:^{}];
}

/// A controller has gone (or the bridge pulled a virtual one). Main queue.
+ (void)drop:(GCController *)c
{
	if (!c) {
		return;
	}
	os_unfair_lock_lock(&sLock);
	const int slot = geSlotOf(c);
	NSInteger count = 1;
	if (slot >= 0) {
		sSlots[slot] = nil;
		sSlotLastObj[slot] = c;
		sSlotLastVendor[slot] = c.vendorName ?: @"";
		sSlotFreedSerial[slot] = ++sFreeSerial;
		count = gePublishSlotsLocked();
	}
	os_unfair_lock_unlock(&sLock);
	if (slot < 0) {
		return;
	}
	// Release everything the pad was holding. A controller that disconnects
	// mid-sprint otherwise leaves Bond running into a wall for ever, because
	// the last pad we published is the last pad the sim sees.
	if (slot == 0) {
		GEInput.padAttached = NO;
		// A pad that has gone has proved nothing about the next one.
		GEInput.padSeenInput = NO;
		[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourcePad];
	} else {
		[GEInput setPad:(GEPad){0, 0, 0} forPort:slot];
	}
	[GEInput setPortCount:count];
	GELog(@"[pad] controller %d (%@) disconnected — port %d free, the game sees %ld",
	      slot + 1, geName(c), slot, (long)count);
	// Round 11a: the overlay's auto mode has to be re-asked HERE. It used
	// to learn about a pad only from the pacer's 30-frame heartbeat, so
	// the window between "the pad went away" and "the controls came back"
	// was half a second of a game with no input at all.
	[GETouchOverlay applyVisibility];
}

+ (void)adopt:(GCController *)c
{
	if (!c || !c.extendedGamepad) {
		return;
	}
	os_unfair_lock_lock(&sLock);
	if (geSlotOf(c) >= 0) {
		os_unfair_lock_unlock(&sLock);
		return;
	}
	// Reclaim: the same object first, then the newest port freed by the same
	// vendor, then the lowest free port.
	int slot = -1;
	for (int i = 0; i < GE_SLOTS && slot < 0; i++) {
		if (!sSlots[i] && sSlotLastObj[i] == c) {
			slot = i;
		}
	}
	if (slot < 0 && c.vendorName) {
		unsigned best = 0;
		for (int i = 0; i < GE_SLOTS; i++) {
			if (!sSlots[i] && sSlotFreedSerial[i] > best
			    && [sSlotLastVendor[i] isEqualToString:c.vendorName]) {
				best = sSlotFreedSerial[i];
				slot = i;
			}
		}
	}
	for (int i = 0; i < GE_SLOTS && slot < 0; i++) {
		if (!sSlots[i]) {
			slot = i;
		}
	}
	if (slot < 0) {
		os_unfair_lock_unlock(&sLock);
		GELog(@"[pad] %@ connected but all four ports are taken — ignored", geName(c));
		return;
	}
	sSlots[slot] = c;
	sSlotFreedSerial[slot] = 0;
	const NSInteger count = gePublishSlotsLocked();
	os_unfair_lock_unlock(&sLock);

	// The pad's own player light, where it has one.
	c.playerIndex = (GCControllerPlayerIndex)(GCControllerPlayerIndex1 + slot);
	if (slot == 0) {
		GEInput.padAttached = YES;
		// A NEW pad has not been used yet, whatever the old one did. Note
		// this deliberately does NOT hide the overlay by itself any more
		// (D-041): a connect is not evidence that a human is holding it.
		GEInput.padSeenInput = NO;
	}
	[GEInput setPortCount:count];
	// "Re-apply on connect": the mapping is not stored in the controller, it
	// is applied fresh here from the live defaults, so a pad that connects
	// after the settings were changed gets the current values and not the
	// ones that were live at launch.
	[GEDefaults registerDefaults];
	GELog(@"[pad] %@ (%@) connected as controller %d (player %d) — twin-stick scheme, "
	      @"deadzone %.2f, look %.0f deg/s; the game sees %ld",
	      geName(c), c.productCategory, slot + 1, slot + 1,
	      [GEDefaults doubleFor:GEDefaultsPadDeadzone],
	      [GEDefaults doubleFor:GEDefaultsPadLookDegPerSec], (long)count);
	[GETouchOverlay applyVisibility];
}

/// Radial dead zone, rescaled so the first count past the zone is the
/// smallest motion and not a jump to 15 %. A square dead zone (per-axis) is
/// what makes a resting stick walk a character diagonally.
static void geRadialDeadzone(float *x, float *y, float dz)
{
	float mag = sqrtf((*x) * (*x) + (*y) * (*y));
	if (mag <= dz || mag <= 0.0f) {
		*x = 0.0f;
		*y = 0.0f;
		return;
	}
	float scaled = (mag - dz) / (1.0f - dz);
	if (scaled > 1.0f) {
		scaled = 1.0f;
	}
	*x = (*x / mag) * scaled;
	*y = (*y / mag) * scaled;
}

/// D-046. The other half of the `[input]` line: what the ENGINE saw. Called
/// from the pacer's poll (this is the one shell callback that runs every
/// frame), logged at most every 2 s and only when a number moved. On a device
/// — where there is no bridge to ask — the pair of lines is the whole
/// diagnosis: `[input] touch -> merged 0x2000` with no matching rise in
/// `edges` means the seam dropped it; a rise in `edges` with a menu that does
/// not move means the game did.
+ (void)logEngineSeamIfChanged
{
	static unsigned lastSets = 0, lastEdges = 0;
	static CFAbsoluteTime lastAt = 0;
	unsigned sets = 0, edges = 0, lastButton = 0;
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (now - lastAt < 2.0) {
		return;
	}
	geHostPadStats(&sets, &edges, &lastButton);
	if (sets == lastSets && edges == lastEdges) {
		return;
	}
	lastAt = now;
	lastSets = sets;
	lastEdges = edges;
	GELog(@"[input] engine seam: %u publishes, %u button edges consumed, last 0x%04x",
	      sets, edges, lastButton);
}

/// One controller's state -> one N64 pad + look degrees, for EVERY port (D-233:
/// there is exactly one copy of the twin-stick scheme). `moved` = a human did
/// something this poll (D-041's evidence).
static void geMapGamepad(GCExtendedGamepad *g, double dt, GEPad *outPad, GELook *outLook, BOOL *moved)
{
	const float dz = (float)[GEDefaults doubleFor:GEDefaultsPadDeadzone];
	GEPad pad = (GEPad){0, 0, 0};
	GELook look = (GELook){0.0f, 0.0f};

	// ---- left stick: move + sidestep --------------------------------------
	float lx = g.leftThumbstick.xAxis.value;
	float ly = g.leftThumbstick.yAxis.value;
	geRadialDeadzone(&lx, &ly, dz);

	pad.stickY = (int8_t)lrintf(ly * GE_STICK_MAX);
	// Publish the stick's true X. hostinput.c folds it into C-left/C-right
	// only for an N64-styled pad, and only in gameplay. Port-driven play
	// strafes from this axis. Menus use it for the front-end crosshair.
	pad.stickX = (int8_t)lrintf(lx * GE_STICK_MAX);

	// ---- right stick: look -------------------------------------------------
	float rx = g.rightThumbstick.xAxis.value;
	float ry = g.rightThumbstick.yAxis.value;
	geRadialDeadzone(&rx, &ry, dz);

	// Per axis, exactly as the touch path is (D-024): the two axes do not want
	// the same number, and a settings page that offers one row for both is
	// asking the player to pick which axis to be wrong about.
	// Round 57: the rows are a multiple of the factory 160 deg/s, and 1.0 is
	// Midas's own Look-stick rate on the SDL path, measured on the iPhone Air
	// (docs/port-ios/77 §1: 207 deg/s yaw, 210 pitch at full tilt). So this
	// path (pads SDL does not hold: the bridge's virtual pads) turns at the
	// same speed a real controller does at the same slider position.
	const double kMidasFullDegPerSec = 208.0;
	const double degPerSecX = [GEDefaults doubleFor:GEDefaultsPadLookDegPerSec]
	                          / GEPadLookFactoryDegPerSec * kMidasFullDegPerSec;
	const double degPerSecY = [GEDefaults doubleFor:GEDefaultsPadLookDegPerSecY]
	                          / GEPadLookFactoryDegPerSec * kMidasFullDegPerSec;
	const double invert = [GEDefaults boolFor:GEDefaultsPadLookInvert] ? -1.0 : 1.0;
	if (rx != 0.0f || ry != 0.0f) {
		// Squared response: precise near centre, fast at the edge. Linear is
		// what makes a thumbstick feel twitchy on small corrections and slow
		// on big ones at the same time.
		float cx = rx * fabsf(rx);
		float cy = ry * fabsf(ry);
		look.yawDeg   = (float)(cx * degPerSecX * dt);
		look.pitchDeg = (float)(-cy * degPerSecY * dt * invert);   // stick up = look up
	}
	// The console's own vertical-look pair, synthesised from the same stick.
	// Redundant with the absolute look above in hip fire and harmless there
	// (GE adds them), but it is what keeps vertical look working in any mode
	// that reads the C buttons rather than the camera — and it is suppressed
	// entirely while AIM is held, where the same bits mean crouch/lean.
	if (ry >  0.6f) pad.button |= GE_CONT_D;   // C-down: look up (GE's pitch is inverted)
	if (ry < -0.6f) pad.button |= GE_CONT_E;   // C-up:   look down

	// ---- buttons -----------------------------------------------------------
	// While AIM is held the C buttons mean crouch / lean / zoom, not sidestep
	// and not look. Drop the synthesised pitch pair from the right stick.
	// The left stick's sidestep is hostinput.c's, and only when the pad is
	// N64-styled; a port-driven pad keeps analog X, which aim mode reads as
	// lean. This happens before the real buttons are OR'd in, so a deliberate
	// CROUCH survives it.
	const float trig = 0.35f;   // an analog trigger is a button past a third
	BOOL aiming = (g.leftTrigger.value > trig) || g.leftShoulder.isPressed
	              || g.buttonB.isPressed;   // crouch implies aim mode; see below
	if (aiming) {
		pad.button &= (uint16_t)~(GE_CONT_E | GE_CONT_D);
	}

	// GoldenEye's own button roles under 1.1/HONEY, from the decomp rather
	// than from the pad's silkscreen (src/game/bondview2.c:5231-5243 and
	// 5400-5410; the in-game options table says the same at options.c:148):
	//
	//   Z       shootButtons — fire
	//   L, R    aimButtons   — the SAME button; L is not a separate action
	//   A       invButtons   — tap = next weapon (hold A + tap Z = previous)
	//   B       "action"     — open / activate AND reload, one bit
	//   Start   pause
	//
	// So the modern pad's face buttons do NOT map straight through: A/cross is
	// use-and-reload, which is GE's B, and the weapon button is GE's A. That
	// was backwards before this round — "USE" cycled the weapon and "RELOAD"
	// opened doors. Note also the interlock at bondview2.c:5482-5484: holding
	// GE's A suppresses fire, because A+Z is "previous weapon".
	if (g.rightTrigger.value > trig)  pad.button |= GE_CONT_G;      // FIRE (Z)
	if (aiming)                       pad.button |= GE_CONT_R;      // AIM (R)
	if (g.buttonA.isPressed)          pad.button |= GE_CONT_B;      // use / action / reload
	if (g.buttonY.isPressed)          pad.button |= GE_CONT_A;      // next weapon
	if (g.rightShoulder.isPressed)    pad.button |= GE_CONT_A;      // next weapon
	// Crouch is not a bit: it is aim mode + C-down held
	// (bondview2.c:5431-5433). B/circle sends both, which is exactly what a
	// player does on a real N64 pad.
	if (g.buttonB.isPressed)          pad.button |= GE_CONT_R | GE_CONT_D;
	if (g.buttonMenu.isPressed)       pad.button |= GE_CONT_START;
	if (g.buttonOptions.isPressed)    pad.button |= GE_CONT_START;

	// The d-pad is an exact alias of the C buttons in GoldenEye — every test
	// is `buttons & (X_JPAD | X_CBUTTONS)` — so this is sidestep/look in hip
	// fire and lean/crouch/zoom while aiming, for free, and it is deliberately
	// NOT suppressed above: a d-pad press while aiming is a lean, which is
	// what the player asked for.
	if (g.dpad.up.isPressed)          pad.button |= GE_CONT_UP;
	if (g.dpad.down.isPressed)        pad.button |= GE_CONT_DOWN;
	if (g.dpad.left.isPressed)        pad.button |= GE_CONT_LEFT;
	if (g.dpad.right.isPressed)       pad.button |= GE_CONT_RIGHT;

	// D-041: anything a human hand does and a pad asleep in a drawer does not.
	*moved = pad.button != 0 || pad.stickX != 0 || pad.stickY != 0
	         || lx != 0.0f || ly != 0.0f || rx != 0.0f || ry != 0.0f
	         || g.leftTrigger.value > trig || g.rightTrigger.value > trig;
	*outPad = pad;
	*outLook = look;
}

+ (void)pollWithDelta:(double)dt
{
	[self logEngineSeamIfChanged];

	GCController *slots[GE_SLOTS];
	os_unfair_lock_lock(&sLock);
	for (int i = 0; i < GE_SLOTS; i++) {
		slots[i] = sSlots[i];
	}
	os_unfair_lock_unlock(&sLock);

	// Round 9c (R9B-1): SDL owns pads. Midas's input_pc.c opens every
	// GCController through SDL's MFi backend as a player, with its own
	// bindings, rumble and per-pad options. Publishing the same pad here
	// too doubled the sticks and applied right-stick look twice. While SDL
	// holds any pad this file only REPORTS (padSeenInput, for the overlay's
	// auto mode); it publishes again once SDL has closed them all. The dev
	// bridge's virtual pads are snapshots SDL never sees, so they still
	// publish.
	static BOOL sSdlOwned = NO;
	const BOOL sdlOwned = !sVirtualMode && geHostSdlPadsOpen() > 0;
	if (sdlOwned != sSdlOwned) {
		sSdlOwned = sdlOwned;
		if (sdlOwned) {
			// Release whatever this file last held, once.
			[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourcePad];
			for (int i = 1; i < GE_SLOTS; i++) {
				[GEInput setPad:(GEPad){0, 0, 0} forPort:i];
			}
		}
		GELog(@"[pad] %@", sdlOwned
		      ? @"SDL has the controller open: GEController stops publishing (report only)"
		      : @"SDL has no controller open: GEController publishes again");
	}

	for (int i = 0; i < GE_SLOTS; i++) {
		GCExtendedGamepad *g = slots[i].extendedGamepad;
		if (!g) {
			continue;
		}
		// The dev bridge is holding GEInputSourcePad (D-046, GEInput.h).
		// Polling port 0 here would overwrite it with the resting physical
		// pad on the very next callback.
		if (i == 0 && GEInput.padSourceHeld) {
			continue;
		}
		GEPad pad;
		GELook look;
		BOOL moved = NO;
		geMapGamepad(g, dt, &pad, &look, &moved);
		if (sdlOwned) {
			if (i == 0 && !GEInput.padSeenInput && moved) {
				GEInput.padSeenInput = YES;
				GELog(@"[pad] first real input seen (SDL-owned) — auto mode may now hide the touch overlay");
				dispatch_async(dispatch_get_main_queue(), ^{ [GETouchOverlay applyVisibility]; });
			}
			continue;
		}

		// D-241: publish only while this controller still owns the slot. The
		// snapshot above is taken under sLock and released; if drop: ran on
		// the main queue since, it has already zeroed the port, and
		// publishing the held state now would leave that port pressing
		// buttons for ever (nothing polls an empty slot). drop: clears the
		// slot under sLock BEFORE it zeroes the port, so checking and
		// publishing under sLock orders the two: either we publish first
		// and drop:'s zero lands after, or we see the slot gone and skip.
		// GEInput's own lock never calls back into this file.
		os_unfair_lock_lock(&sLock);
		if (sSlots[i] != slots[i]) {
			os_unfair_lock_unlock(&sLock);
			continue;
		}
		if (i > 0) {
			// Controllers 2-4: a port each, nothing merged (D-234).
			[GEInput setPad:pad forPort:i];
			[GEInput addLook:look forPort:i];
			os_unfair_lock_unlock(&sLock);
			continue;
		}
		[GEInput addLook:look];
		[GEInput setPad:pad forSource:GEInputSourcePad];
		os_unfair_lock_unlock(&sLock);
		// D-041: the overlay's auto mode hides the controls only once
		// controller 1 has actually MOVED something.
		if (!GEInput.padSeenInput && moved) {
			GEInput.padSeenInput = YES;
			GELog(@"[pad] first real input seen — auto mode may now hide the touch overlay");
			dispatch_async(dispatch_get_main_queue(), ^{ [GETouchOverlay applyVisibility]; });
		}
	}
}

+ (NSString *)describe
{
	NSMutableArray *parts = [NSMutableArray array];
	os_unfair_lock_lock(&sLock);
	for (int i = 0; i < GE_SLOTS; i++) {
		GCController *c = sSlots[i];
		if (c) {
			[parts addObject:[NSString stringWithFormat:@"%d:%@", i + 1,
			                  [geName(c) stringByReplacingOccurrencesOfString:@" " withString:@"_"]]];
		}
	}
	os_unfair_lock_unlock(&sLock);
	return parts.count ? [parts componentsJoinedByString:@","] : @"none";
}

+ (NSInteger)connectedCount
{
	NSInteger n = 0;
	os_unfair_lock_lock(&sLock);
	for (int i = 0; i < GE_SLOTS; i++) {
		n += sSlots[i] != nil;
	}
	os_unfair_lock_unlock(&sLock);
	return n;
}

#if !GE_PUBLIC
// ---- D-235: virtual controllers for the simulator (dev bridge only) --------
//
// GCController's snapshot controllers (+controllerWithExtendedGamepad) are
// real GCController objects whose elements can be SET (GCControllerButtonInput
// -setValue:, GCControllerDirectionPad -setValueForXAxis:yAxis:). They are
// adopted and dropped through the very adopt:/drop: a connect notification
// calls, and pollWithDelta: reads them like any pad — so the port assignment,
// the twin-stick mapping and the per-port publish under test are the shipped
// code. Only the HAND is synthetic.
static void geVpadApply(GCExtendedGamepad *g, NSString *buttons, float lx, float ly, float rx, float ry)
{
	NSSet *b = [NSSet setWithArray:[buttons.lowercaseString componentsSeparatedByString:@","]];
	[g.buttonA setValue:[b containsObject:@"a"] ? 1.0f : 0.0f];
	[g.buttonB setValue:[b containsObject:@"b"] ? 1.0f : 0.0f];
	[g.buttonX setValue:[b containsObject:@"x"] ? 1.0f : 0.0f];
	[g.buttonY setValue:[b containsObject:@"y"] ? 1.0f : 0.0f];
	[g.leftShoulder setValue:[b containsObject:@"l1"] ? 1.0f : 0.0f];
	[g.rightShoulder setValue:[b containsObject:@"r1"] ? 1.0f : 0.0f];
	[g.leftTrigger setValue:[b containsObject:@"l2"] ? 1.0f : 0.0f];
	[g.rightTrigger setValue:[b containsObject:@"r2"] ? 1.0f : 0.0f];
	[g.buttonMenu setValue:[b containsObject:@"menu"] ? 1.0f : 0.0f];
	float dx = ([b containsObject:@"right"] ? 1.0f : 0.0f) - ([b containsObject:@"left"] ? 1.0f : 0.0f);
	float dy = ([b containsObject:@"up"] ? 1.0f : 0.0f) - ([b containsObject:@"down"] ? 1.0f : 0.0f);
	[g.dpad setValueForXAxis:dx yAxis:dy];
	[g.leftThumbstick setValueForXAxis:lx yAxis:ly];
	[g.rightThumbstick setValueForXAxis:rx yAxis:ry];
}

+ (NSString *)bridgeVpad:(NSArray<NSString *> *)w
{
	// vpad <n>                 virtual mode with n (0..4) controllers; 0 = back to real ones
	// vpad drop <k> | add <k>  disconnect / reconnect virtual controller k (1-based)
	// vpad set <k> <buttons|none> <lx> <ly> <rx> <ry> [secs]
	//                           buttons: a,b,x,y,l1,r1,l2,r2,menu,up,down,left,right
	if (w.count >= 2 && [w[1] isEqualToString:@"set"] && w.count >= 8) {
		NSInteger k = w[2].integerValue;
		if (k < 1 || k > (NSInteger)sVirtual.count) {
			return @"error=vpad-index\n";
		}
		GCController *c = sVirtual[k - 1];
		NSString *buttons = [w[3] isEqualToString:@"none"] ? @"" : w[3];
		geVpadApply(c.extendedGamepad, buttons, w[4].floatValue, w[5].floatValue,
		            w[6].floatValue, w[7].floatValue);
		double secs = w.count >= 9 ? w[8].doubleValue : 0.0;
		// A later `set` supersedes an earlier hold's release (main queue).
		static unsigned serial[GE_SLOTS + 1];
		const unsigned mine = ++serial[k];
		if (secs > 0.0) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)),
			               dispatch_get_main_queue(), ^{
				if (serial[k] == mine) {
					geVpadApply(c.extendedGamepad, @"", 0, 0, 0, 0);
				}
			});
		}
		return [NSString stringWithFormat:@"vpad=set k=%ld buttons=%@ l=%@,%@ r=%@,%@ hold=%.2f\n",
		        (long)k, buttons.length ? buttons : @"none", w[4], w[5], w[6], w[7], secs];
	}
	if (w.count >= 3 && ([w[1] isEqualToString:@"drop"] || [w[1] isEqualToString:@"add"])) {
		NSInteger k = w[2].integerValue;
		if (k < 1 || k > (NSInteger)sVirtual.count) {
			return @"error=vpad-index\n";
		}
		GCController *c = sVirtual[k - 1];
		if ([w[1] isEqualToString:@"drop"]) {
			geVpadApply(c.extendedGamepad, @"", 0, 0, 0, 0);
			[self drop:c];
		} else {
			[self adopt:c];
		}
		return [NSString stringWithFormat:@"vpad=%@ k=%ld pads=%@ ports=%ld\n", w[1], (long)k,
		        [self describe], (long)GEInput.portCount];
	}
	if (w.count >= 2) {
		NSInteger n = w[1].integerValue;
		if (n < 0 || n > GE_SLOTS) {
			return @"error=vpad-count\n";
		}
		for (GCController *c in sVirtual) {
			[self drop:c];
		}
		[sVirtual removeAllObjects];
		if (n == 0) {
			sVirtualMode = NO;
			for (GCController *c in GCController.controllers) {
				[self adopt:c];
			}
		} else {
			if (!sVirtual) {
				sVirtual = [NSMutableArray array];
			}
			sVirtualMode = YES;
			for (GCController *c in GCController.controllers) {
				[self drop:c];
			}
			for (NSInteger i = 0; i < n; i++) {
				GCController *c = [GCController controllerWithExtendedGamepad];
				[sVirtual addObject:c];
				[self adopt:c];
			}
		}
		return [NSString stringWithFormat:@"vpad=%ld pads=%@ ports=%ld\n", (long)n,
		        [self describe], (long)GEInput.portCount];
	}
	return @"error=usage cmd=vpad\n";
}
#endif

@end
