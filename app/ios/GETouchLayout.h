// GETouchLayout.h — the ONE table the on-screen controls are drawn from,
// hit-tested against, described to the bridge and (next round) edited with.
//
// Split out of GETouchOverlay for one reason: a layout editor and a settings
// page are the next round's work, and they must read and write exactly the
// geometry the view uses. A table that lives inside the view's .m file is a
// table an editor has to duplicate, and a duplicated layout is one that drifts
// — which is how a gate ends up tapping where FIRE used to be.
//
// SOURCE OF THE NUMBERS. The recomp port of this same game is the control spec
// (charter, required reading 6). Its live overlay is `overlay/rexglue/src/ui/
// touch_overlay_uikit.mm` AS PATCHED by `patches/rexglue/0064-touch-rework-
// editor-settings`, `0090-left-fire-in-aim-mode-and-gadget-button` and
// `0091-left-fire-placeable-in-editor` — the unpatched file in that tree is an
// earlier design (safe-area fractions, text labels, a resting "MOVE" ring) and
// reading it alone gets all three of those wrong. The shipped design is:
//
//   * positions in UNIT COORDINATES OF THE FULL VIEW, not of the safe area.
//     That is the representation the editor persists, so a baked layout and a
//     saved one land in the same place; and the baked values below are not a
//     designer's guess but that port's own defaults, exported from Austin's
//     device after real play (its 0.1.0.56, 2026-08-09).
//   * SF Symbol glyphs, never words: "words need reading mid-firefight and
//     don't localise" (Austin, 2026-08-08, recorded in 0064).
//   * FIRE big; AIM small, because it is a mode toggle and not a held-in-panic
//     button — only FIRE earns the big target.
//   * no resting stick visual at all: the left half being blank is the design,
//     and the stick materialises wherever the thumb lands.
//
// Persistence: a customised layout is a JSON object in NSUserDefaults under
// `Touch.Layout`, {"NAME":{"x":<unitX>,"y":<unitY>,"r":<radius>}, …}; the empty
// string (the factory default) means "the baked table". One key rather than
// the recomp's key-per-button-per-axis, because one key is one atomic write,
// one line over the bridge, and one string a player can copy between devices.
// Only geometry is savable — the action a button performs is not editable,
// because an editor that can unbind FIRE can brick the game on a phone with
// no controller.
#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef struct {
	const char *name;    ///< key, and the persistence label. Never drawn.
	const char *glyph;   ///< SF Symbol drawn in the circle
	uint16_t    bits;    ///< N64 pad bits OR'd into the pad while held
	CGFloat     unitX;   ///< centre, as a fraction of the FULL view's width
	CGFloat     unitY;   ///< centre, as a fraction of the FULL view's height
	CGFloat     radius;  ///< points at Touch.ButtonScale = 1
	const char *action;  ///< what it does in GoldenEye, for the settings page
	/// Round 59: a Midas Port action (port/src/joy_port.c, the midas.ini
	/// [controls] name) held while the button is down, or NULL. CROUCH is
	/// `crouch_toggle` and GADGET `next_inventory`: Midas's own actions,
	/// pressed through port/apple/hostinput.c geHostPortActionSet.
	const char *portAction;
} GETouchButton;

@interface GETouchLayout : NSObject

/// The live table: the baked defaults with any saved overrides applied.
/// Stable for the process; re-read after +reload.
+ (const GETouchButton *)buttons;
+ (NSInteger)count;
/// Index of the button with this name, or -1.
+ (NSInteger)indexOfName:(NSString *)name;

/// The aim-mode left-hand FIRE. Not in the table, because it is not a control
/// the game can be played without and it exists only while AIM is held. By
/// DEFAULT it is drawn mirrored across the vertical centre line from wherever
/// FIRE is, so a custom FIRE placement carries across for free — but the
/// layout editor can place it explicitly (recomp 0091), and then `unitX`/
/// `unitY` in the returned struct are meaningful and +leftFireIsPlaced is YES.
+ (GETouchButton)leftFire;
/// NO = mirror FIRE (the default); YES = the editor placed it, so use its
/// unitX/unitY.
+ (BOOL)leftFireIsPlaced;

/// Re-read `Touch.Layout` from NSUserDefaults into the live table. Called at
/// install and whenever the bridge or the settings page changes the key.
+ (void)reload;

/// Move/resize one button and persist it. Unit coordinates are clamped to the
/// view and the radius to 12…80 pt, so a bad editor cannot produce a control
/// that is unreachable or covers the game. `name` may be "FIRE_L", which
/// switches the aim-mode left FIRE from mirroring to an explicit position.
/// Returns NO if the name is unknown.
///
/// MAIN THREAD ONLY — the overlay reads this table on the main thread and
/// GEBridge already hops for it.
+ (BOOL)setUnitX:(CGFloat)x unitY:(CGFloat)y radius:(CGFloat)radius
         forName:(NSString *)name;

/// Forget every override, including FIRE_L's placement (the editor's reset
/// pill and the settings page's "reset layout"). Main thread only.
+ (void)resetToDefaults;

/// The current overrides as the JSON string stored in `Touch.Layout`, or @""
/// when the layout is the baked one. This is also the editor's UNDO token:
/// take one on entry, hand it back to +restoreSerialized: on Cancel.
+ (NSString *)serialize;

/// Put a previously-serialized layout back, exactly as it was. Main thread
/// only. An invalid string lands on the baked table rather than throwing,
/// which is the same fallback +reload already takes for corrupt JSON.
+ (void)restoreSerialized:(NSString *)json;

@end

NS_ASSUME_NONNULL_END
