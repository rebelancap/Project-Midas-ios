// GETouchOverlay.h — touch v1, at parity with the recomp port of this game.
//
// The layout — which buttons exist, how big they are and where they sit — is
// NOT here. It is `GETouchLayout`, one table that the view draws from, hit-
// tests against, describes to the bridge and that the layout editor and the
// settings page will read and write. See that header for the schema and for
// the bean-action -> N64-action mapping (and what was dropped).
//
// What IS here, all of it carried from ~/dev/GoldenEye-Recomp-ios
// (overlay/rexglue/src/ui/touch_overlay_uikit.mm AS PATCHED by
// patches/rexglue/0064, 0090 and 0091 — the unpatched file in that tree is an
// earlier design and following it gets the glyphs, the anchoring and the
// resting ring all wrong), which the charter names as this game's control spec:
//
//   * a FLOATING move stick on the left 45 % of the screen: it appears
//     wherever the thumb lands, so the thumb never has to find it, and its
//     origin then stays put for the life of that touch. 62 pt of travel to
//     full deflection. NO resting visual — the blank left half is the design.
//   * stick response: a RADIAL 0.15 dead zone, rescaled so the first
//     perceptible push is a small deflection rather than a jump to 15 %, then
//     0.4 linear + 0.6 cubic of the remainder. Radial and not per-axis because
//     a square dead zone lets a diagonal push register on one axis while the
//     other is still dead, which reads as the stick snapping to the cardinals.
//   * DRAG-LOOK on the right. Any touch that is not on a button and not the
//     stick turns the view, at Touch.LookDegPerPoint ABSOLUTE view degrees per
//     point of travel — not through a mouse-sensitivity curve. A drag of N
//     points always turns the same amount, on any panel, at any frame rate.
//     (This is where the two ports differ in mechanism and agree in feel: the
//     remaster injects mouse counts, we write degrees to the camera — D-024.)
//   * the AIM finger doubles as an aim surface: press-and-drag on AIM moves
//     the view, so aiming costs one thumb rather than two. With
//     Touch.FireDragLooks on (the default), FIRE and the aim-mode left FIRE
//     do the same: that finger keeps firing and the drag turns the view.
//   * a second FIRE under the LEFT thumb WHILE AIM IS HELD, mirrored from
//     wherever FIRE is. In aim mode the stick side of the screen is idle and
//     the right thumb is busy, so without it the player cannot shoot while
//     aiming with one hand.
//   * drawing: stroked rings with an SF Symbol, never words — "words need
//     reading mid-firefight and don't localise" (Austin, 2026-08-08).
//   * haptics on the press edge only, so held fire does not buzz continuously.
//   * geometry in unit coordinates of the FULL view, which is what the layout
//     editor persists (GETouchLayout).
//   * hide when a controller connects (Touch.Enabled = 2, the default).
//
// Touches are tracked BY IDENTITY: a finger keeps the role it started with for
// its whole life, so sliding off a button never silently becomes a look drag
// (AIM and, when Drag fire to look is on, FIRE stay on the button and also
// turn the view) and a stick thumb that crosses the middle of the screen does
// not start turning the view.
#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface GETouchOverlay : UIView

/// Create the overlay and add it to the app's window, above SDL's view.
/// Idempotent; main thread. Returns the live instance.
+ (instancetype)install;
+ (nullable GETouchOverlay *)shared;

/// Show/hide according to Touch.Enabled (0 off, 1 on, 2 auto = hidden while a
/// physical controller is attached). Cheap; called from the pacer.
+ (void)applyVisibility;

/// What is under this point, for the bridge's `tap`: "button:FIRE",
/// "stick", "look" or "MISS". Does not press anything.
- (NSString *)describePoint:(CGPoint)p;

/// Every control's live centre and radius IN POINTS, one per line, for the
/// gate and for the layout editor. Anchored geometry cannot be recomputed
/// from a fraction without knowing the safe area, so the view reports it.
- (NSString *)describeLayout;

/// The bridge's synthetic input, so the gate can press a control without a
/// real finger (simctl's injected events bypass UIKit entirely).
- (NSString *)bridgeTapAt:(CGPoint)p seconds:(double)seconds;
- (NSString *)bridgeDragAt:(CGPoint)p by:(CGVector)d;

#pragma mark - the layout editor

/// The layout editor is a MODE of this view, not a separate screen, for the
/// same reason the table was split out of it: the thing being edited has to be
/// drawn by the code that draws it for real, or the editor shows a second
/// opinion about where the buttons are. It is also how the recomp port does it
/// (rexglue 0064/0091).
///
/// While editing: the overlay is forced visible whatever Touch.Enabled says
/// (a player with a controller plugged in still gets to arrange their buttons),
/// the pad is held at zero so nothing leaks into the game, every control is
/// drawn in the editor's yellow at full presence, the aim-mode left FIRE is
/// always shown so it can be placed, and a dark scrim goes over the game so
/// the rings read against whatever is behind them.
///
/// Drag one control to move it; pinch to resize the one nearest the pinch;
/// the bar carries Cancel, Reset, a size slider and Done. Main thread.
+ (void)beginEditing;
/// Leave the editor. `save` NO puts the layout back exactly as it was on
/// entry — the recomp port has no cancel and this port does, because a layout
/// is easy to wreck and hard to remember.
+ (void)endEditingSave:(BOOL)save;
+ (BOOL)isEditing;

/// Drag one control to a point and LEAVE IT SELECTED, for the dev bridge's
/// `page edit-drag NAME x y`. It runs the editor's own move path rather than
/// writing the table directly, so what it proves is the editor and not a
/// parallel implementation of it — and it leaves the drag live so the next
/// screenshot is genuinely mid-drag. A simulator has no other way to press
/// glass (simctl's injected events bypass UIKit entirely).
- (NSString *)bridgeEditDrag:(NSString *)name to:(CGPoint)p;

@end

NS_ASSUME_NONNULL_END
