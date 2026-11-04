// GETouchOverlay.m — see GETouchOverlay.h for the design and GETouchLayout.h
// for the table this draws from.
#import "GETouchOverlay.h"
#import "GETouchLayout.h"
#import "GEInput.h"
#import "GEDefaults.h"
#import "GEPad.h"
#import "GEShell.h"
#import "GEVision.h"
#if TARGET_OS_VISION
#import "GEVision3D.h"   // round 48: hidden while 3D is on
#endif

#include <math.h>

// app/gfx/gfx_angle_egl.mm — the handle that finds SDL's window when UIKit
// cannot enumerate it (GEShell.h).
extern void *geAngleGetHostView(void);
// port/src/hostinput.c (D-062)
extern void geHostReloadOnlySet(int on);

static const CGFloat kStickRegionFrac = 0.45f;  // the left share of the screen
static const CGFloat kStickRadius     = 62.0f;  // points of travel to full deflection
static const CGFloat kStickKnobRadius = 26.0f;  // at Touch.ButtonScale = 1
// Stick response, shared with the pad (GEController's geRadialDeadzone) and
// with the recomp port (rex::ui::ApplyStickResponse). The dead zone and the
// cubic share are settings rows now (Touch.StickDeadzone / Touch.StickCurve);
// these are the fallbacks used before the defaults are registered and the
// clamps a bad value is pulled back to.
static const CGFloat kStickDeadzoneDefault = 0.15f;
static const CGFloat kStickCurveDefault    = 0.6f;
// A touch has to move this far before it is a look drag rather than a tap.
// Below it, a stationary finger must not creep the view.
static const CGFloat kLookSlopPoints = 1.0f;

// D-046. Every real UIKit touch the overlay receives, at most one line per
// 150 ms, with the number of lines dropped in between. This is the log a
// device read needs to tell "the finger never arrived" from "the finger
// arrived and the engine ignored it"; the two have identical symptoms and
// opposite causes.
static void geLogTouch(NSString *what, CGPoint p, NSString *name)
{
	static CFAbsoluteTime lastAt = 0;
	static unsigned suppressed = 0;
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (now - lastAt < 0.15) {
		suppressed++;
		return;
	}
	lastAt = now;
	GELog(@"[touch] %@ (%.0f,%.0f) -> %@%@", what, p.x, p.y, name,
	      suppressed ? [NSString stringWithFormat:@" (+%u suppressed)", suppressed] : @"");
	suppressed = 0;
}
// The hit radius is this multiple of the drawn one: the drawn size is what the
// eye aims at, and fingers land short of what they aim at.
static const CGFloat kHitSlop = 1.25f;

// port/include/hostinput.h (D-236)
extern int geHostSplitScreenPlayers(void);

@implementation GETouchOverlay {
	// Touches by identity — a finger keeps its role for its whole life.
	UITouch *_stickTouch;
	CGPoint  _stickOrigin;
	CGPoint  _stickCurrent;

	UITouch *_lookTouch;
	CGPoint  _lookLast;

	NSMapTable<UITouch *, NSString *> *_buttonTouches;   // touch -> name
	NSMutableSet<NSString *> *_held;

	// Bridge-driven presses, which have no UITouch at all.
	NSMutableSet<NSString *> *_bridgeHeld;

	// The finger holding AIM doubles as an aim surface: press-and-drag moves
	// the view without a second finger, which is what makes aim mode usable
	// with two thumbs (recomp 0064).
	UITouch *_aimDragTouch;
	CGPoint  _aimDragLast;

	// Previous dirty rects, so a control that disappears gets repainted.
	CGRect _stickPrevDirty;
	CGRect _leftFirePrevDirty;

#if !TARGET_OS_VISION
	// No haptics on visionOS: UIImpactFeedbackGenerator does not exist there
	// (GEVision.h); the Haptics settings row is compiled out with it.
	UIImpactFeedbackGenerator *_haptics;
#endif
	BOOL _loggedLayout;

	// ---- the layout editor ------------------------------------------------
	BOOL _editing;
	NSString *_editSnapshot;    // Touch.Layout as it was on entry — the undo
	double _editScaleSnapshot;  // Touch.ButtonScale likewise
	NSString *_dragName;        // the control under the finger, or nil
	CGPoint _dragOffset;        // centre - fingertip, so it does not jump
	NSString *_pinchName;       // the control being resized
	CGFloat _pinchStartRadius;
	UIView *_editBar;
	UISlider *_editScale;
	UILabel *_editScaleLabel;
	UIPinchGestureRecognizer *_pinch;
}

static GETouchOverlay *sShared = nil;

+ (GETouchOverlay *)shared { return sShared; }

+ (instancetype)install
{
	// Deliberately NOT "already has a superview, done". Round 11a: the whole
	// class of failure this fixes is an overlay parented to the WRONG window
	// — a window that is then hidden, taking the controls with it while
	// `superview` stays non-nil for ever. The cheap question is asked every
	// time and the early-out below is "already on the right window".
	//
	// The window SDL made and GEShell grafted onto the scene. Adding the
	// overlay to the WINDOW rather than to SDL's view means SDL resizing or
	// replacing its view cannot take the controls away with it.
	//
	// Round 11a: ASK THE RENDERER, do not guess. The old version walked every
	// scene's windows and kept "the key one, else the last one" — which on a
	// launch where onboarding's window (level +100) or the settings window
	// (+90) still held key would put the controls on a window that is about
	// to be hidden, leaving a shown, hit-testable, invisible overlay and an
	// `install` that never retries because superview is non-nil. The host
	// view's own -window is SDL's window whether UIKit can enumerate it or
	// not; that is the same handle geGraftSDLWindows() uses and the same
	// reason (GEShell.h).
	UIWindow *win = ((__bridge UIView *)geAngleGetHostView()).window;
	if (!win) {
		for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
			if (![s isKindOfClass:UIWindowScene.class]) {
				continue;
			}
			for (UIWindow *w in ((UIWindowScene *)s).windows) {
				// Never onboarding's or the settings page's window: those are
				// above UIWindowLevelNormal and both go away again.
				if (w.windowLevel <= UIWindowLevelNormal && (!win || w.isKeyWindow)) {
					win = w;
				}
			}
		}
	}
	if (!win) {
		return sShared;
	}
	if (sShared && sShared.superview == win) {
		return sShared;
	}

	if (!sShared) {
		sShared = [[GETouchOverlay alloc] initWithFrame:win.bounds];
	}
	sShared.frame = win.bounds;
	sShared.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	[win addSubview:sShared];
	[win bringSubviewToFront:sShared];
	[GETouchLayout reload];
	[GETouchOverlay applyVisibility];
	GELog(@"[touch] overlay installed on %@ (%@ level %.0f, key %d)",
	      NSStringFromCGRect(win.bounds), NSStringFromClass(win.class),
	      (double)win.windowLevel, (int)win.isKeyWindow);
	return sShared;
}

- (instancetype)initWithFrame:(CGRect)frame
{
	if ((self = [super initWithFrame:frame])) {
		self.backgroundColor = UIColor.clearColor;
		self.opaque = NO;
		self.multipleTouchEnabled = YES;
		self.userInteractionEnabled = YES;
		self.contentMode = UIViewContentModeRedraw;
		_buttonTouches = [NSMapTable weakToStrongObjectsMapTable];
		_held = [NSMutableSet set];
		_bridgeHeld = [NSMutableSet set];
#if !TARGET_OS_VISION
		_haptics = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
#endif
		_stickPrevDirty = CGRectNull;
		_leftFirePrevDirty = CGRectNull;
	}
	return self;
}

+ (void)applyVisibility
{
	GETouchOverlay *v = sShared;
	if (!v) {
		return;
	}
	NSInteger mode = [GEDefaults intFor:GEDefaultsTouchEnabled];
	// D-041. Mode 2 is "auto", and auto used to mean `!padAttached` — a pad
	// being ATTACHED was taken as proof that the player had a way to play. It
	// is not. The simulator reports a virtual "Gamepad (MFi)" at launch; a
	// real phone reports a controller that is paired and asleep. Either way
	// the overlay went away ~90 ms after it appeared and the player was left
	// with a game and no input at all — which is exactly what Austin found on
	// 0.0.0.2 ("no touch controls, so I can't advance past the intro demo").
	//
	// So auto now needs EVIDENCE: a pad hides the controls once it has moved
	// something (GEController sets padSeenInput), and never before. The cost
	// of being wrong in this direction is a few seconds of overlay on top of
	// a pad game; the cost of being wrong in the other direction is an app
	// nobody can start.
	BOOL padUsable = GEInput.padAttached && GEInput.padSeenInput;
	BOOL show = (mode == 1) || (mode == 2 && !padUsable);
	// D-236 (round 29d): a split-screen match covers the whole panel with
	// other players' viewports, and our buttons would sit on top of them.
	// While one runs AND controller 1 is connected (player 1 has a pad), the
	// overlay goes, whatever Touch.Enabled says. If controller 1 drops out
	// mid-match it comes back, so player 1 is never left with nothing.
	const int splitPlayers = geHostSplitScreenPlayers();
	if (splitPlayers >= 2 && GEInput.padAttached) {
		show = NO;
	}
	// The editor overrides the setting: a player with a controller attached
	// (or with the overlay switched off entirely) still has to be able to
	// arrange the buttons, and an editor that hides what it is editing is not
	// an editor.
	if (v->_editing) {
		show = YES;
	}
#if TARGET_OS_VISION
	// Phase 6 M4 (round 48, plan section 2.8): hidden while the game is on the
	// immersive panel, whatever the setting says - the buttons would sit on a
	// parked card that shows the curtain, not the game. The curtain swallows a
	// pinch on the card (display-only); the pad stays primary. Restored on the
	// exit (GEHostViewController re-asks when the curtain comes down).
	const BOOL in3d = geVision3dActive() || geVision3dCurtainUp();
	if (in3d) {
		show = NO;
	}
#endif
	if (v.hidden == !show) {
		return;
	}
	v.hidden = !show;
	if (v.hidden) {
		// Everything the overlay was holding goes with it.
		[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourceTouch];
	}
#if TARGET_OS_VISION
	GELog(@"[touch] overlay %@ (Touch.Enabled=%ld, pad %@, pad-used %@, split-screen %d, 3d %d)",
	      show ? @"shown" : @"hidden", (long)mode,
	      GEInput.padAttached ? @"attached" : @"none",
	      GEInput.padSeenInput ? @"yes" : @"no", splitPlayers, (int)in3d);
#else
	GELog(@"[touch] overlay %@ (Touch.Enabled=%ld, pad %@, pad-used %@, split-screen %d)",
	      show ? @"shown" : @"hidden", (long)mode,
	      GEInput.padAttached ? @"attached" : @"none",
	      GEInput.padSeenInput ? @"yes" : @"no", splitPlayers);
#endif
}

#pragma mark - geometry

- (CGFloat)buttonScale
{
	CGFloat s = (CGFloat)[GEDefaults doubleFor:GEDefaultsButtonScale];
	return (s < 0.5 || s > 2.0) ? 1.0 : s;
}

- (CGFloat)stickRadius { return kStickRadius * [self buttonScale]; }

/// Unit coordinates of the FULL view — the representation the layout editor
/// persists, so a baked layout and a saved one land in the same place. It is
/// also the recomp port's shipped choice; its earlier safe-area-anchored
/// version was replaced when the defaults became one player's real exported
/// layout, which already carries his device's insets in the numbers.
- (CGPoint)centreOf:(const GETouchButton *)b
{
	return CGPointMake(b->unitX * self.bounds.size.width,
	                   b->unitY * self.bounds.size.height);
}

- (CGFloat)radiusOf:(const GETouchButton *)b
{
	return b->radius * [self buttonScale];
}

/// The aim-mode left FIRE, mirrored across the vertical centre line from
/// wherever FIRE is, so a custom FIRE placement carries across for free.
- (BOOL)leftFireVisible
{
	return [_held containsObject:@"AIM"] || [_bridgeHeld containsObject:@"AIM"];
}

- (CGPoint)leftFireCentre
{
	if (GETouchLayout.leftFireIsPlaced) {
		GETouchButton lf = GETouchLayout.leftFire;
		return [self centreOf:&lf];
	}
	CGPoint fire = [self centreOf:&GETouchLayout.buttons[0]];   // [0] is FIRE
	return CGPointMake(self.bounds.size.width - fire.x, fire.y);
}

/// Which button a point is on: an index into the table, or -2 for the
/// aim-mode left FIRE, or -1 for nothing.
- (NSInteger)buttonIndexAt:(CGPoint)p
{
	// Checked first: while it is up it supersedes the floating stick, whose
	// half of the screen it shares. There is no moving in aim mode anyway.
	if ([self leftFireVisible]) {
		GETouchButton lf = GETouchLayout.leftFire;
		CGPoint c = [self leftFireCentre];
		CGFloat r = [self radiusOf:&lf] * kHitSlop;
		CGFloat dx = p.x - c.x, dy = p.y - c.y;
		if (dx * dx + dy * dy <= r * r) {
			return -2;
		}
	}
	const GETouchButton *bs = GETouchLayout.buttons;
	NSInteger best = -1;
	CGFloat bestD = CGFLOAT_MAX;
	for (NSInteger i = 0; i < GETouchLayout.count; i++) {
		CGPoint c = [self centreOf:&bs[i]];
		CGFloat r = [self radiusOf:&bs[i]] * kHitSlop;
		CGFloat dx = p.x - c.x, dy = p.y - c.y;
		CGFloat d = sqrt(dx * dx + dy * dy);
		if (d <= r && d < bestD) {
			best = i;
			bestD = d;
		}
	}
	return best;
}

/// Name for an index from -buttonIndexAt:, or nil.
- (NSString *)nameForIndex:(NSInteger)i
{
	if (i == -2) return @(GETouchLayout.leftFire.name);
	if (i < 0)   return nil;
	return @(GETouchLayout.buttons[i].name);
}

/// Bits for a held control by name, including the one that is not in the
/// table. Returns 0 for an unknown name.
- (uint16_t)bitsForName:(NSString *)name
{
	if ([name isEqualToString:@(GETouchLayout.leftFire.name)]) {
		// An invisible button must not fire: the left FIRE counts only while
		// AIM is still down (recomp 0090).
		return [self leftFireVisible] ? GETouchLayout.leftFire.bits : 0;
	}
	NSInteger i = [GETouchLayout indexOfName:name];
	return (i < 0) ? 0 : GETouchLayout.buttons[i].bits;
}

/// Mark only the regions the controls occupy. The view has to BE full-screen
/// to receive touches, but it draws in two corners — and a full-screen
/// invalidation re-rasterises the whole backing store on the CPU, which during
/// a drag happened every single frame (the recomp port's M-059).
- (void)invalidateControls
{
	// The stick's rect is unioned with the PREVIOUS one, or the ring left
	// behind when a thumb lifts is never repainted.
	CGRect stickNow = CGRectNull;
	if (_stickTouch) {
		CGFloat r = [self stickRadius] + 8.0;
		stickNow = CGRectMake(_stickOrigin.x - r, _stickOrigin.y - r, r * 2, r * 2);
		stickNow = CGRectUnion(stickNow, CGRectMake(_stickCurrent.x - r, _stickCurrent.y - r,
		                                            r * 2, r * 2));
	}
	CGRect stickDirty = CGRectUnion(stickNow, _stickPrevDirty);
	if (!CGRectIsNull(stickDirty)) {
		[self setNeedsDisplayInRect:stickDirty];
	}
	_stickPrevDirty = stickNow;

	// The left FIRE is on the far side of the screen from the cluster, so it
	// gets its own rect: a union with the cluster's would span the whole width
	// and the partial repaint would be gone.
	CGRect lfNow = CGRectNull;
	if ([self leftFireVisible]) {
		GETouchButton lf = GETouchLayout.leftFire;
		CGPoint c = [self leftFireCentre];
		CGFloat r = [self radiusOf:&lf] + 6.0;
		lfNow = CGRectMake(c.x - r, c.y - r, r * 2, r * 2);
	}
	CGRect lfDirty = CGRectUnion(lfNow, _leftFirePrevDirty);
	if (!CGRectIsNull(lfDirty)) {
		[self setNeedsDisplayInRect:lfDirty];
	}
	_leftFirePrevDirty = lfNow;

	const GETouchButton *bs = GETouchLayout.buttons;
	CGRect cluster = CGRectNull;
	for (NSInteger i = 0; i < GETouchLayout.count; i++) {
		CGPoint c = [self centreOf:&bs[i]];
		CGFloat br = [self radiusOf:&bs[i]] + 6.0;
		cluster = CGRectUnion(cluster, CGRectMake(c.x - br, c.y - br, br * 2, br * 2));
	}
	if (!CGRectIsNull(cluster)) {
		[self setNeedsDisplayInRect:cluster];
	}
}

#pragma mark - stick response

/// The dead zone and the cubic share, read from the settings each time and
/// clamped. Read per call rather than cached because a settings slider has to
/// take effect while the player's thumb is still on the stick — that is how
/// they tell whether they like it.
static CGFloat geStickDeadzone(void)
{
	CGFloat v = (CGFloat)[GEDefaults doubleFor:GEDefaultsStickDeadzone];
	return (v < 0.0 || v > 0.45) ? kStickDeadzoneDefault : v;
}

static CGFloat geStickCurve(void)
{
	CGFloat v = (CGFloat)[GEDefaults doubleFor:GEDefaultsStickCurve];
	return (v < 0.0 || v > 1.0) ? kStickCurveDefault : v;
}

/// The family curve, identical to rex::ui::ApplyStickResponse and to
/// GEController's pad path: radial dead zone, rescale, `(1-c)·n + c·n³`.
/// Direction is taken BEFORE the clamp so pushing past the ring keeps the
/// angle the thumb is actually holding rather than the angle at the edge.
static void geStickResponse(CGFloat dx, CGFloat dy, CGFloat radius,
                            CGFloat *outX, CGFloat *outY)
{
	*outX = 0;
	*outY = 0;
	CGFloat len = sqrt(dx * dx + dy * dy);
	if (radius <= 0 || len <= 0) {
		return;
	}
	const CGFloat dz = geStickDeadzone();
	const CGFloat c  = geStickCurve();
	CGFloat mag = len / radius;
	if (mag <= dz) {
		return;
	}
	CGFloat ux = dx / len, uy = dy / len;
	if (mag > 1.0) mag = 1.0;
	mag = (mag - dz) / (1.0 - dz);
	CGFloat curved = (1.0 - c) * mag + c * mag * mag * mag;
	*outX = ux * curved;
	*outY = uy * curved;
}

/// One place that turns a stick deflection into pad bits, so the finger path
/// and the bridge's synthetic drag cannot disagree. `uy` is UIKit-signed
/// (down-positive); GoldenEye's stick is up-positive.
static void gePadFromStick(CGFloat ux, CGFloat uy, GEPad *pad)
{
	pad->stickY = (int8_t)lrint(-uy * GE_STICK_MAX);
	// Publish the thumb's true X. hostinput.c folds it into C-left/C-right
	// only while the pad is N64-styled, and only in gameplay. Port-driven
	// play (the phone) strafes from this axis, so zeroing it leaves the
	// stick unable to sidestep. Menus read the same axis for the crosshair.
	pad->stickX = (int8_t)lrint(ux * GE_STICK_MAX);
}

#pragma mark - publishing

/// Recompute the whole touch contribution and hand it to GEInput. Called on
/// every touch event; cheap, and being total rather than incremental is what
/// makes a lost touchesCancelled impossible to notice.
- (void)publish
{
	GEPad pad = (GEPad){0, 0, 0};

	for (NSString *name in _held) {
		pad.button |= [self bitsForName:name];
	}
	for (NSString *name in _bridgeHeld) {
		pad.button |= [self bitsForName:name];
	}

	// D-062: RELOAD publishes B like USE, plus a flag that makes the press
	// skip the activate test in src/game/lv.c. Holding USE at the same time
	// wins, because USE is the console-faithful bit and the player who is
	// holding both asked for the door.
	BOOL reloadOnly = ([_held containsObject:@"RELOAD"] || [_bridgeHeld containsObject:@"RELOAD"])
	               && !([_held containsObject:@"USE"] || [_bridgeHeld containsObject:@"USE"]);
	geHostReloadOnlySet(reloadOnly ? 1 : 0);

	if (_stickTouch) {
		CGFloat ux = 0, uy = 0;
		geStickResponse(_stickCurrent.x - _stickOrigin.x,
		                _stickCurrent.y - _stickOrigin.y,
		                [self stickRadius], &ux, &uy);
		gePadFromStick(ux, uy, &pad);
	}

	// While AIM is held the C buttons are lean and crouch, not sidestep.
	// Nothing here sets C-left or C-right. hostinput.c synthesises that pair
	// only for an N64-styled pad. A port-driven stick keeps its analog X,
	// which aim mode already reads as lean.

	[GEInput setPad:pad forSource:GEInputSourceTouch];
	[self invalidateControls];
}

- (void)pressName:(NSString *)name
{
	BOOL wasHeld = [_held containsObject:name];
	[_held addObject:name];
	// Only on the transition to held: a second finger landing on an already
	// held button is not a new press, and buzzing for it would make held fire
	// feel like a fault.
#if !TARGET_OS_VISION
	if (!wasHeld && [GEDefaults boolFor:GEDefaultsTouchHaptics]) {
		[_haptics impactOccurred];
		[_haptics prepare];
	}
#else
	(void)wasHeld;
#endif
}

#pragma mark - touches

/// AIM always. FIRE and the aim-mode left FIRE when Drag fire to look is on.
/// Read on each touch so a settings change applies to the next move.
static BOOL geButtonDragsLook(NSString *name)
{
	if ([name isEqualToString:@"AIM"]) {
		return YES;
	}
	if (![GEDefaults boolFor:GEDefaultsFireDragLooks]) {
		return NO;
	}
	return [name isEqualToString:@"FIRE"] || [name isEqualToString:@"FIRE_L"];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	if (_editing) {
		[self editTouchesBegan:touches withEvent:event];
		return;
	}
	for (UITouch *t in touches) {
		CGPoint p = [t locationInView:self];
		NSString *name = [self nameForIndex:[self buttonIndexAt:p]];
		// D-046. A REAL UIKit touch reaching this method is the thing no gate
		// had ever asserted and no device log had ever shown: `bridge tap`
		// calls -bridgeTapAt: and never comes through here, so a hit-testing
		// or window-order fault is invisible to a green gate. Rate-limited in
		// geLogTouch so a look drag cannot bury the line.
		geLogTouch(@"down", p, name ?: (p.x < self.bounds.size.width * kStickRegionFrac
		                                ? @"<stick>" : @"<look>"));
		if (name) {
			[_buttonTouches setObject:name forKey:t];
			[self pressName:name];
			// AIM always, and FIRE / FIRE_L when Drag fire to look is on:
			// the finger stays on the button and its drag turns the view.
			// The first such finger owns the look; a second does not add.
			if (geButtonDragsLook(name) && !_aimDragTouch) {
				_aimDragTouch = t;
				_aimDragLast = p;
			}
			continue;
		}
		if (!_stickTouch && p.x < self.bounds.size.width * kStickRegionFrac) {
			// The stick appears wherever the thumb lands, and its origin then
			// stays put for the life of the touch.
			_stickTouch = t;
			_stickOrigin = p;
			_stickCurrent = p;
			continue;
		}
		if (!_lookTouch) {
			_lookTouch = t;
			_lookLast = p;
		}
	}
	[self publish];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	if (_editing) {
		[self editTouchesMoved:touches];
		return;
	}
	// Per axis (D-024): a phone's usable vertical drag is a fraction of its
	// horizontal one, so one number for both is a compromise neither axis
	// wants. Two rows, two numbers, both in degrees per point.
	const double degPerPtX = [GEDefaults doubleFor:GEDefaultsLookDegPerPoint];
	const double degPerPtY = [GEDefaults doubleFor:GEDefaultsLookDegPerPointY];
	const double invert = [GEDefaults boolFor:GEDefaultsLookInvert] ? -1.0 : 1.0;
	GELook look = (GELook){0, 0};

	for (UITouch *t in touches) {
		CGPoint p = [t locationInView:self];
		if (t == _stickTouch) {
			_stickCurrent = p;
		} else if (t == _lookTouch) {
			CGFloat dx = p.x - _lookLast.x;
			CGFloat dy = p.y - _lookLast.y;
			if (fabs(dx) >= kLookSlopPoints || fabs(dy) >= kLookSlopPoints) {
				look.yawDeg   += (float)(dx * degPerPtX);
				look.pitchDeg += (float)(dy * degPerPtY * invert);
				_lookLast = p;
			}
		} else if (t == _aimDragTouch) {
			// The button stays held through _held regardless of where this
			// finger wanders. If the setting is turned off mid-drag, stop
			// adding look and leave the finger on the button.
			NSString *held = [_buttonTouches objectForKey:t];
			if (held && geButtonDragsLook(held)) {
				CGFloat dx = p.x - _aimDragLast.x;
				CGFloat dy = p.y - _aimDragLast.y;
				if (fabs(dx) >= kLookSlopPoints || fabs(dy) >= kLookSlopPoints) {
					look.yawDeg   += (float)(dx * degPerPtX);
					look.pitchDeg += (float)(dy * degPerPtY * invert);
					_aimDragLast = p;
				}
			}
		}
		// A finger that started on a button and slid off keeps holding it:
		// that is what a physical button does, and letting it drop means a
		// thumb that shifts during a firefight stops firing.
	}

	[GEInput addLook:look];
	[self publish];
}

- (void)endTouches:(NSSet<UITouch *> *)touches
{
	if (_editing) {
		_dragName = nil;
		return;
	}
	for (UITouch *t in touches) {
		NSString *name = [_buttonTouches objectForKey:t];
		geLogTouch(@"up", [t locationInView:self], name ?: @"-");
		if (name) {
			[_buttonTouches removeObjectForKey:t];
			// Only release if no OTHER finger is on the same button.
			BOOL stillHeld = NO;
			for (UITouch *o in _buttonTouches) {
				if ([[_buttonTouches objectForKey:o] isEqualToString:name]) {
					stillHeld = YES;
					break;
				}
			}
			if (!stillHeld) {
				[_held removeObject:name];
			}
		}
		if (t == _stickTouch)   _stickTouch = nil;
		if (t == _lookTouch)    _lookTouch = nil;
		if (t == _aimDragTouch) _aimDragTouch = nil;
	}
	// The left FIRE lives only in aim mode: letting go of AIM releases it too,
	// finger still on the glass or not — an invisible button must not fire.
	if (![self leftFireVisible]) {
		NSString *lf = @(GETouchLayout.leftFire.name);
		[_held removeObject:lf];
		[_bridgeHeld removeObject:lf];
		for (UITouch *o in [[_buttonTouches keyEnumerator] allObjects]) {
			if ([[_buttonTouches objectForKey:o] isEqualToString:lf]) {
				[_buttonTouches removeObjectForKey:o];
			}
		}
	}
	[self publish];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self endTouches:touches]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self endTouches:touches]; }

/// Do not swallow touches that are not ours: a point that is neither a button
/// nor inside a drag zone belongs to whatever is underneath.
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
	if (self.hidden || self.alpha < 0.01) {
		return nil;
	}
	// Ask UIKit first, so a real control we own — the editor's bar — gets its
	// own touches. Without this the blanket `return self` below swallows
	// every tap on the bar's buttons and slider, which is a whole afternoon
	// if you have not seen it before. With no subviews (the normal case) the
	// super call returns self and nothing changes.
	UIView *sub = [super hitTest:point withEvent:event];
	if (sub && sub != self) {
		return sub;
	}
	return CGRectContainsPoint(self.bounds, point) ? self : nil;
}

#pragma mark - the bridge's synthetic input

- (NSString *)describePoint:(CGPoint)p
{
	NSString *name = [self nameForIndex:[self buttonIndexAt:p]];
	if (name) {
		return [NSString stringWithFormat:@"button:%@", name];
	}
	if (p.x < self.bounds.size.width * kStickRegionFrac) {
		return @"stick";
	}
	if (CGRectContainsPoint(self.bounds, p)) {
		return @"look";
	}
	return @"MISS";
}

- (NSString *)describeLayout
{
	const GETouchButton *bs = GETouchLayout.buttons;
	UIEdgeInsets safe = self.safeAreaInsets;
	NSMutableString *out = [NSMutableString string];
	// visible= is the assertion a gate needs and a log line cannot give: a
	// hidden overlay still answers `tap` correctly through the bridge, so
	// "the tap landed on FIRE" was never evidence that a finger could have
	// done it (round 11a).
	UIWindow *w = self.window;
	[out appendFormat:@"points=%.0fx%.0f safe=%.0f,%.0f,%.0f,%.0f scale=%.2f custom=%@ "
	                  @"visible=%@ installed=%@ window=%@ level=%.0f\n",
	     self.bounds.size.width, self.bounds.size.height,
	     safe.top, safe.left, safe.bottom, safe.right, [self buttonScale],
	     GETouchLayout.serialize.length ? @"yes" : @"no",
	     (self.superview && !self.hidden && self.alpha > 0.01) ? @"yes" : @"no",
	     self.superview ? @"yes" : @"no",
	     w ? NSStringFromClass(w.class) : @"none", (double)(w ? w.windowLevel : 0)];
	for (NSInteger i = 0; i < GETouchLayout.count; i++) {
		CGPoint c = [self centreOf:&bs[i]];
		[out appendFormat:@"button=%s x=%.0f y=%.0f r=%.0f unit=%.4f,%.4f bits=0x%04x glyph=%s action=%s\n",
		     bs[i].name, c.x, c.y, [self radiusOf:&bs[i]], bs[i].unitX, bs[i].unitY,
		     bs[i].bits, bs[i].glyph, bs[i].action];
	}
	{
		GETouchButton lf = GETouchLayout.leftFire;
		CGPoint c = [self leftFireCentre];
		[out appendFormat:@"button=%s x=%.0f y=%.0f r=%.0f bits=0x%04x glyph=%s action=%s visible=%@\n",
		     lf.name, c.x, c.y, [self radiusOf:&lf], lf.bits, lf.glyph, lf.action,
		     [self leftFireVisible] ? @"yes" : @"aim-only"];
	}
	[out appendFormat:@"stick=floating region=%.0f travel=%.0f deadzone=%.2f curve=%.2f\n",
	     self.bounds.size.width * kStickRegionFrac, [self stickRadius],
	     geStickDeadzone(), geStickCurve()];
	return out;
}

- (NSString *)bridgeTapAt:(CGPoint)p seconds:(double)seconds
{
	NSString *name = [self nameForIndex:[self buttonIndexAt:p]];
	if (!name) {
		return [NSString stringWithFormat:@"tap=MISS at=%@", [self describePoint:p]];
	}
	uint16_t bits = [self bitsForName:name];
	[_bridgeHeld addObject:name];
	[self publish];
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
	               dispatch_get_main_queue(), ^{
		[self->_bridgeHeld removeObject:name];
		[self publish];
	});
	return [NSString stringWithFormat:@"tap=OK button:%@ bits=0x%04x hold=%.2fs",
	        name, bits, seconds];
}

- (NSString *)bridgeDragAt:(CGPoint)p by:(CGVector)d
{
	if (p.x < self.bounds.size.width * kStickRegionFrac) {
		// Drive the stick instead: a drag that starts in the stick zone is a
		// walk, and the gate needs both. Same response curve as a finger.
		GEPad pad = (GEPad){0, 0, 0};
		CGFloat ux = 0, uy = 0;
		geStickResponse(d.dx, d.dy, [self stickRadius], &ux, &uy);
		gePadFromStick(ux, uy, &pad);
		[GEInput setPad:pad forSource:GEInputSourceBridge];
		return [NSString stringWithFormat:@"drag=stick stick_x=%d stick_y=%d",
		        (int)pad.stickX, (int)pad.stickY];
	}

	const double degPerPtX = [GEDefaults doubleFor:GEDefaultsLookDegPerPoint];
	const double degPerPtY = [GEDefaults doubleFor:GEDefaultsLookDegPerPointY];
	const double invert = [GEDefaults boolFor:GEDefaultsLookInvert] ? -1.0 : 1.0;
	GELook look = (GELook){
		.yawDeg   = (float)(d.dx * degPerPtX),
		.pitchDeg = (float)(d.dy * degPerPtY * invert),
	};
	[GEInput addLook:look];
	return [NSString stringWithFormat:@"drag=look look_deg=%.2f,%.2f", look.yawDeg, look.pitchDeg];
}

#pragma mark - drawing

/// One ring. `filled` is the held state — a control that is down has to be
/// obviously down without the thumb moving off it.
static void geStroke(CGContextRef ctx, CGPoint c, CGFloat r, CGFloat a, BOOL filled)
{
	CGContextSetLineWidth(ctx, 2.5);
	CGContextSetStrokeColorWithColor(ctx, [UIColor colorWithWhite:1.0 alpha:MIN(a, 1.0)].CGColor);
	CGContextStrokeEllipseInRect(ctx, CGRectMake(c.x - r, c.y - r, r * 2, r * 2));
	if (filled) {
		CGContextSetFillColorWithColor(ctx, [UIColor colorWithWhite:1.0 alpha:MIN(a * 0.4, 1.0)].CGColor);
		CGContextFillEllipseInRect(ctx, CGRectMake(c.x - r, c.y - r, r * 2, r * 2));
	}
}

/// The glyph. SF Symbol images carry their own aspect ratio, so the symbol
/// configuration sizes them proportionally to the circle rather than stretching
/// an image into a box. imageWithTintColor and not a template draw:
/// -drawInRect: ignores the rendering mode and the current fill colour, so a
/// template image drawn that way comes out black on a dark overlay.
static void geGlyph(const char *symbol, CGPoint c, CGFloat r, CGFloat a)
{
	UIImage *img = [UIImage systemImageNamed:@(symbol)];
	if (!img) {
		return;
	}
	img = [img imageByApplyingSymbolConfiguration:
	           [UIImageSymbolConfiguration configurationWithPointSize:MAX(r * 0.62, 10.0)
	                                                          weight:UIImageSymbolWeightSemibold]];
	img = [img imageWithTintColor:[UIColor colorWithWhite:1.0 alpha:MIN(a, 1.0)]
	                renderingMode:UIImageRenderingModeAlwaysOriginal];
	CGSize is = img.size;
	[img drawInRect:CGRectMake(c.x - is.width / 2, c.y - is.height / 2, is.width, is.height)];
}

/// One ring in the editor's colours. Deliberately ignores Touch.ButtonOpacity:
/// what is grabbable has to be unmistakable, and a player who has set the
/// opacity to 5 % would otherwise be dragging things they cannot see.
static void geEditStroke(CGContextRef ctx, CGPoint c, CGFloat r, CGFloat alpha, BOOL selected)
{
	UIColor *yellow = [UIColor colorWithRed:1.0 green:0.85 blue:0.4 alpha:alpha];
	CGContextSetLineWidth(ctx, selected ? 4.0 : 2.5);
	CGContextSetStrokeColorWithColor(ctx, yellow.CGColor);
	CGContextStrokeEllipseInRect(ctx, CGRectMake(c.x - r, c.y - r, r * 2, r * 2));
	if (selected) {
		CGContextSetFillColorWithColor(ctx,
			[UIColor colorWithRed:1.0 green:0.85 blue:0.4 alpha:alpha * 0.25].CGColor);
		CGContextFillEllipseInRect(ctx, CGRectMake(c.x - r, c.y - r, r * 2, r * 2));
	}
}

- (void)drawEditing:(CGContextRef)ctx
{
	// A scrim over the live game. The recomp port edits against the raw
	// picture; a phone screen with a bright Dam sky behind it makes a thin
	// yellow ring genuinely hard to see, and the scrim costs one fill.
	CGContextSetFillColorWithColor(ctx, [UIColor colorWithWhite:0.0 alpha:0.55].CGColor);
	CGContextFillRect(ctx, self.bounds);

	const GETouchButton *bs = GETouchLayout.buttons;
	for (NSInteger i = 0; i < GETouchLayout.count; i++) {
		NSString *name = @(bs[i].name);
		CGPoint c = [self centreOf:&bs[i]];
		CGFloat r = [self radiusOf:&bs[i]];
		BOOL sel = [name isEqualToString:_dragName] || [name isEqualToString:_pinchName];
		geEditStroke(ctx, c, r, 0.95, sel);
		geGlyph(bs[i].glyph, c, r, 0.95);
	}

	// Always drawn here, however AIM is: it cannot be placed if it is only
	// visible while a finger that is busy dragging holds AIM down (recomp
	// 0091). Fainter, because it IS conditional in play and the editor should
	// not imply otherwise.
	GETouchButton lf = GETouchLayout.leftFire;
	CGPoint lc = [self leftFireCentre];
	CGFloat lr = [self radiusOf:&lf];
	BOOL lsel = [_dragName isEqualToString:@(lf.name)] || [_pinchName isEqualToString:@(lf.name)];
	geEditStroke(ctx, lc, lr, 0.45, lsel);
	geGlyph(lf.glyph, lc, lr, 0.5);
}

- (void)drawRect:(CGRect)rect
{
	CGContextRef ctx = UIGraphicsGetCurrentContext();
	if (_editing) {
		[self drawEditing:ctx];
		return;
	}
	CGFloat a = (CGFloat)[GEDefaults doubleFor:GEDefaultsButtonOpacity];
	if (a < 0.05) a = 0.05;
	if (a > 1.0)  a = 1.0;

	const GETouchButton *bs = GETouchLayout.buttons;
	for (NSInteger i = 0; i < GETouchLayout.count; i++) {
		NSString *name = @(bs[i].name);
		BOOL held = [_held containsObject:name] || [_bridgeHeld containsObject:name];
		CGPoint c = [self centreOf:&bs[i]];
		CGFloat r = [self radiusOf:&bs[i]];
		geStroke(ctx, c, r, held ? a * 2.0 : a, held);
		geGlyph(bs[i].glyph, c, r, a * 2.2);
	}

	// Aim mode: the mirrored FIRE for the left thumb.
	if ([self leftFireVisible]) {
		GETouchButton lf = GETouchLayout.leftFire;
		CGPoint c = [self leftFireCentre];
		CGFloat r = [self radiusOf:&lf];
		BOOL held = [_held containsObject:@(lf.name)] || [_bridgeHeld containsObject:@(lf.name)];
		geStroke(ctx, c, r, held ? a * 2.0 : a, held);
		geGlyph(lf.glyph, c, r, a * 2.2);
	}

	// No resting visual for the stick. The left half being blank is the
	// design, not an omission (recomp 0064, Austin 2026-08-05 "as originally
	// intended"): the stick materialises wherever the thumb lands, so there is
	// nothing to find and nothing to paint over the game until there is.
	if (_stickTouch) {
		CGFloat r = [self stickRadius];
		geStroke(ctx, _stickOrigin, r, a * 1.6, NO);
		CGFloat dx = _stickCurrent.x - _stickOrigin.x;
		CGFloat dy = _stickCurrent.y - _stickOrigin.y;
		CGFloat len = sqrt(dx * dx + dy * dy);
		if (len > r) {
			dx *= r / len;
			dy *= r / len;
		}
		geStroke(ctx, CGPointMake(_stickOrigin.x + dx, _stickOrigin.y + dy),
		         kStickKnobRadius * [self buttonScale], a * 2.0, YES);
	}
}

#pragma mark - the layout editor

+ (BOOL)isEditing { return sShared ? sShared->_editing : NO; }

+ (void)beginEditing
{
	[sShared beginEditing];
}

+ (void)endEditingSave:(BOOL)save
{
	[sShared endEditingSave:save];
}

- (void)beginEditing
{
	if (_editing) {
		return;
	}
	_editing = YES;
	// The undo token. The recomp port has no cancel at all; this one does,
	// because a layout takes a minute to arrange and a second to wreck, and
	// "put it back" is otherwise only reachable through "reset everything".
	_editSnapshot = [GETouchLayout serialize];
	_editScaleSnapshot = [GEDefaults doubleFor:GEDefaultsButtonScale];

	// Nothing may be held, and nothing may become held: the pad is zeroed
	// here and the editor's touch path never publishes one.
	[_held removeAllObjects];
	[_bridgeHeld removeAllObjects];
	_stickTouch = nil;
	_lookTouch = nil;
	_aimDragTouch = nil;
	[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourceTouch];

	[GETouchOverlay applyVisibility];
	[self buildEditBar];

	if (!_pinch) {
		_pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinch:)];
		// The drag path is raw touchesMoved:, not a recogniser, so the pinch
		// must not steal those touches — it only needs to observe them.
		_pinch.cancelsTouchesInView = NO;
		_pinch.delaysTouchesBegan = NO;
		_pinch.delaysTouchesEnded = NO;
	}
	[self addGestureRecognizer:_pinch];

	// One line per control, so "open the editor" is enough to read a layout
	// back over the bridge or out of the console log.
	GELog(@"[touch] layout editor OPEN\n%@", [self describeLayout]);
	[self setNeedsDisplay];
}

- (void)endEditingSave:(BOOL)save
{
	if (!_editing) {
		return;
	}
	if (!save) {
		[GETouchLayout restoreSerialized:_editSnapshot];
		[GEDefaults setString:[NSString stringWithFormat:@"%g", _editScaleSnapshot]
		               forKey:GEDefaultsButtonScale];
	}
	_editing = NO;
	_dragName = nil;
	_pinchName = nil;
	_editSnapshot = nil;
	[_pinch.view removeGestureRecognizer:_pinch];
	[_editBar removeFromSuperview];
	_editBar = nil;
	_editScale = nil;
	_editScaleLabel = nil;
	[GETouchOverlay applyVisibility];
	[self setNeedsDisplay];
	GELog(@"[touch] layout editor CLOSED (%@)\n%@",
	      save ? @"saved" : @"cancelled", [self describeLayout]);
}

/// Bottom-left, the family's shape (reset pill, live size slider with a
/// percentage, done pill). Cancel is ours. No instruction text: dragging a
/// circle is self-evident, and text in the way of the thing being arranged is
/// worse than no text.
- (void)buildEditBar
{
	UIView *bar = [UIView new];
	bar.translatesAutoresizingMaskIntoConstraints = NO;
	[self addSubview:bar];
	_editBar = bar;

	UIButton *cancel = [self pillWithSymbol:@"xmark"
	                                  colour:[UIColor colorWithRed:0.45 green:0.45 blue:0.48 alpha:0.95]
	                                  action:@selector(editCancel)];
	UIButton *reset  = [self pillWithSymbol:@"arrow.uturn.backward"
	                                  colour:[UIColor colorWithRed:0.85 green:0.20 blue:0.22 alpha:0.95]
	                                  action:@selector(editReset)];
	UIButton *done   = [self pillWithSymbol:@"checkmark"
	                                  colour:[UIColor colorWithRed:0.18 green:0.78 blue:0.34 alpha:0.95]
	                                  action:@selector(editDone)];
	[bar addSubview:cancel];
	[bar addSubview:reset];
	[bar addSubview:done];

	UISlider *sl = [UISlider new];
	sl.minimumValue = 0.6f;
	sl.maximumValue = 1.8f;
	sl.value = (float)MIN(MAX([GEDefaults doubleFor:GEDefaultsButtonScale], 0.6), 1.8);
	sl.minimumTrackTintColor = [UIColor colorWithWhite:0.9 alpha:1.0];
	sl.translatesAutoresizingMaskIntoConstraints = NO;
	[sl addTarget:self action:@selector(editScaleChanged:) forControlEvents:UIControlEventValueChanged];
	[bar addSubview:sl];
	_editScale = sl;

	UILabel *pct = [UILabel new];
	pct.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightSemibold];
	pct.textColor = [UIColor colorWithWhite:0.9 alpha:1.0];
	pct.textAlignment = NSTextAlignmentCenter;
	pct.translatesAutoresizingMaskIntoConstraints = NO;
	[bar addSubview:pct];
	_editScaleLabel = pct;
	[self updateEditScaleLabel];

	// Not safeAreaLayoutGuide: SDL's window reports a PORTRAIT inset set in
	// landscape, which put this bar 34 pt off the bottom of where it belongs
	// (GEShell.h geShellSafeInsets, round 10 open item / round 11a).
	UIEdgeInsets ins = geShellSafeInsets(self);
	[NSLayoutConstraint activateConstraints:@[
		[bar.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:ins.left + 14],
		[bar.bottomAnchor  constraintEqualToAnchor:self.bottomAnchor constant:-(ins.bottom + 14)],
		[bar.heightAnchor  constraintEqualToConstant:42],
		[bar.trailingAnchor constraintEqualToAnchor:done.trailingAnchor],

		[cancel.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor],
		[cancel.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
		[reset.leadingAnchor  constraintEqualToAnchor:cancel.trailingAnchor constant:10],
		[reset.centerYAnchor  constraintEqualToAnchor:bar.centerYAnchor],

		[sl.leadingAnchor constraintEqualToAnchor:reset.trailingAnchor constant:16],
		[sl.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
		[sl.widthAnchor   constraintEqualToConstant:200],

		[pct.centerXAnchor constraintEqualToAnchor:sl.centerXAnchor],
		[pct.bottomAnchor  constraintEqualToAnchor:sl.topAnchor constant:-2],

		[done.leadingAnchor constraintEqualToAnchor:sl.trailingAnchor constant:16],
		[done.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
	]];
}

- (UIButton *)pillWithSymbol:(NSString *)symbol colour:(UIColor *)colour action:(SEL)action
{
	UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
	[b setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
	b.tintColor = UIColor.whiteColor;
	b.backgroundColor = colour;
	b.layer.cornerRadius = 21.0;
	b.translatesAutoresizingMaskIntoConstraints = NO;
	[b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
	[NSLayoutConstraint activateConstraints:@[
		[b.widthAnchor constraintEqualToConstant:42],
		[b.heightAnchor constraintEqualToConstant:42],
	]];
	return b;
}

- (void)updateEditScaleLabel
{
	_editScaleLabel.text = [NSString stringWithFormat:@"%.0f%%", _editScale.value * 100.0];
}

- (void)editScaleChanged:(UISlider *)sl
{
	[GEDefaults setString:[NSString stringWithFormat:@"%.3f", (double)sl.value]
	               forKey:GEDefaultsButtonScale];
	[self updateEditScaleLabel];
	[self setNeedsDisplay];
}

- (void)editCancel { [self endEditingSave:NO]; }
- (void)editDone   { [self endEditingSave:YES]; }

- (void)editReset
{
	[GETouchLayout resetToDefaults];
	[GEDefaults setString:@"1" forKey:GEDefaultsButtonScale];
	_editScale.value = 1.0f;
	[self updateEditScaleLabel];
	[self setNeedsDisplay];
	GELog(@"[touch] layout reset to defaults from the editor");
}

/// The control nearest a point, INCLUDING the left FIRE, with the same 1.25×
/// hit slop play uses. Returns nil if nothing is near enough.
- (NSString *)editControlNear:(CGPoint)p
{
	NSString *best = nil;
	CGFloat bestD = CGFLOAT_MAX;

	GETouchButton lf = GETouchLayout.leftFire;
	CGPoint lc = [self leftFireCentre];
	CGFloat lr = [self radiusOf:&lf] * kHitSlop;
	CGFloat ld = hypot(p.x - lc.x, p.y - lc.y);
	if (ld <= lr) {
		best = @(lf.name);
		bestD = ld;
	}

	const GETouchButton *bs = GETouchLayout.buttons;
	for (NSInteger i = 0; i < GETouchLayout.count; i++) {
		CGPoint c = [self centreOf:&bs[i]];
		CGFloat r = [self radiusOf:&bs[i]] * kHitSlop;
		CGFloat d = hypot(p.x - c.x, p.y - c.y);
		if (d <= r && d < bestD) {
			best = @(bs[i].name);
			bestD = d;
		}
	}
	return best;
}

/// The live centre and radius of a control by name, whichever table it is in.
- (BOOL)editCentre:(CGPoint *)outC radius:(CGFloat *)outR forName:(NSString *)name
{
	if ([name isEqualToString:@(GETouchLayout.leftFire.name)]) {
		GETouchButton lf = GETouchLayout.leftFire;
		if (outC) *outC = [self leftFireCentre];
		if (outR) *outR = [self radiusOf:&lf];
		return YES;
	}
	NSInteger i = [GETouchLayout indexOfName:name];
	if (i < 0) {
		return NO;
	}
	if (outC) *outC = [self centreOf:&GETouchLayout.buttons[i]];
	if (outR) *outR = [self radiusOf:&GETouchLayout.buttons[i]];
	return YES;
}

/// Write a control's geometry back. Unit coordinates are of the FULL view;
/// the stored radius is the UNSCALED one, because Touch.ButtonScale is a
/// separate multiplier applied at draw time and folding it in here would make
/// the slider compound with itself on the next drag.
- (void)editCommitName:(NSString *)name centre:(CGPoint)c radius:(CGFloat)drawnRadius
{
	CGFloat scale = [self buttonScale];
	if (scale <= 0.0) {
		scale = 1.0;
	}
	[GETouchLayout setUnitX:c.x / MAX(self.bounds.size.width, 1.0)
	                  unitY:c.y / MAX(self.bounds.size.height, 1.0)
	                 radius:drawnRadius / scale
	                forName:name];
}

- (void)editTouchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	// A second finger means a pinch, not a second drag: drop the drag so the
	// two gestures cannot fight over the same control.
	if (event.allTouches.count > 1) {
		_dragName = nil;
		return;
	}
	CGPoint p = [[touches anyObject] locationInView:self];
	NSString *name = [self editControlNear:p];
	if (!name) {
		return;
	}
	CGPoint c = CGPointZero;
	if (![self editCentre:&c radius:NULL forName:name]) {
		return;
	}
	_dragName = name;
	// Grab where it was grabbed, not by the middle: a button that jumps to
	// the fingertip is impossible to place accurately.
	_dragOffset = CGPointMake(c.x - p.x, c.y - p.y);
	[self setNeedsDisplay];
}

- (void)editTouchesMoved:(NSSet<UITouch *> *)touches
{
	[self editTouchesMovedToPoint:[[touches anyObject] locationInView:self]];
}

- (void)editTouchesMovedToPoint:(CGPoint)p
{
	if (!_dragName) {
		return;
	}
	CGPoint c = CGPointMake(p.x + _dragOffset.x, p.y + _dragOffset.y);
	CGFloat r = 0;
	[self editCentre:NULL radius:&r forName:_dragName];
	// Snap-clamp so the whole control stays on screen. GETouchLayout clamps
	// the unit coordinates as well (2…98 %), but that is a backstop measured
	// in fractions of the view; this one is measured in the button's own
	// radius, which is what "you cannot push it off the edge" means to a
	// player dragging a big FIRE ring.
	c.x = MIN(MAX(c.x, r), self.bounds.size.width - r);
	c.y = MIN(MAX(c.y, r), self.bounds.size.height - r);
	[self editCommitName:_dragName centre:c radius:r];
	[self setNeedsDisplay];
}

- (void)handlePinch:(UIPinchGestureRecognizer *)g
{
	if (!_editing) {
		return;
	}
	CGPoint c = [g locationInView:self];
	if (g.state == UIGestureRecognizerStateBegan) {
		_dragName = nil;
		_pinchName = [self editControlNear:c];
		if (!_pinchName) {
			// Nothing under the pinch: take whatever is nearest, so a player
			// pinching next to a small button still resizes it.
			_pinchName = [self editNearestControlTo:c];
		}
		CGFloat r = 0;
		[self editCentre:NULL radius:&r forName:_pinchName];
		_pinchStartRadius = r;
		[self setNeedsDisplay];
		return;
	}
	if (!_pinchName) {
		return;
	}
	if (g.state == UIGestureRecognizerStateChanged) {
		CGPoint centre = CGPointZero;
		[self editCentre:&centre radius:NULL forName:_pinchName];
		CGFloat drawn = MIN(MAX(_pinchStartRadius * g.scale, 12.0), 80.0);
		[self editCommitName:_pinchName centre:centre radius:drawn];
		[self setNeedsDisplay];
		return;
	}
	if (g.state == UIGestureRecognizerStateEnded ||
	    g.state == UIGestureRecognizerStateCancelled ||
	    g.state == UIGestureRecognizerStateFailed) {
		_pinchName = nil;
		[self setNeedsDisplay];
	}
}

- (NSString *)bridgeEditDrag:(NSString *)name to:(CGPoint)p
{
	if (!_editing) {
		return @"error=not-editing\n";
	}
	CGPoint c = CGPointZero;
	CGFloat r = 0;
	if (![self editCentre:&c radius:&r forName:name]) {
		return [NSString stringWithFormat:@"error=unknown-control name=%@\n", name];
	}
	// Exactly what a finger does: grab the centre, then move. The offset is
	// zero because there is no fingertip to preserve an offset from.
	_dragName = name;
	_dragOffset = CGPointZero;
	[self editTouchesMovedToPoint:p];
	CGPoint now = CGPointZero;
	[self editCentre:&now radius:&r forName:name];
	return [NSString stringWithFormat:@"edit-drag=%@ from=%.0f,%.0f to=%.0f,%.0f r=%.0f\n",
	        name, c.x, c.y, now.x, now.y, r];
}

- (NSString *)editNearestControlTo:(CGPoint)p
{
	NSString *best = @(GETouchLayout.leftFire.name);
	CGPoint lc = [self leftFireCentre];
	CGFloat bestD = hypot(p.x - lc.x, p.y - lc.y);
	const GETouchButton *bs = GETouchLayout.buttons;
	for (NSInteger i = 0; i < GETouchLayout.count; i++) {
		CGPoint c = [self centreOf:&bs[i]];
		CGFloat d = hypot(p.x - c.x, p.y - c.y);
		if (d < bestD) {
			bestD = d;
			best = @(bs[i].name);
		}
	}
	return best;
}

- (void)layoutSubviews
{
	[super layoutSubviews];
	[self setNeedsDisplay];
	if (!_loggedLayout && self.bounds.size.width > 1) {
		_loggedLayout = YES;
		// Charter rule 6: a UIKit placement is believed when the view logs it.
		GELog(@"[touch] %@", [self describeLayout]);
	}
}

@end
