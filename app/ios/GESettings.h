// GESettings.h — the UIKit settings page, and the gear that opens it.
//
// Austin, on 0.0.0.1: "we need the iOS settings options like we have in BEAN
// GoldenEye. With the customize layout option, other app audio, anything
// relevant to this port."
//
// The spec is the recomp port's page (~/dev/GoldenEye-Recomp-ios,
// `overlay/rexglue/src/ui/settings_uikit.mm` AS PATCHED by rexglue 0039,
// 0051, 0064, 0074, 0078, 0081, 0088, 0091 — the unpatched file is an earlier
// design, the same trap GETouchLayout.h records for the overlay) and, above
// that, the family shape in ~/dev/q2repro-ios/SETTINGS-SPEC-FROM-VKQUAKE.md:
// inset-grouped table, our own header bar with a prominent Done, sliders that
// apply LIVE with a monospaced value on the right, a Reset that touches only
// what the page owns, and read-only diagnostics that say what they are.
//
// THREE STORES, and which row goes where is the decision (DECISIONS D-038):
//
//   * NSUserDefaults (GEDefaults) — anything only the SHELL reads: the touch
//     overlay's geometry, opacity and haptics, the look sensitivities, the
//     target frame rate, the audio session's mix option. None of it reaches
//     the sim, so none of it belongs in a file the determinism gate diffs.
//   * ge007.ini (port/src/config.c, through geHostConfigGet/Set) — anything
//     the RENDERER reads: MSAA, render scale, filtering, FOV, draw distance,
//     the engine's own fps overlay, and the port's Game.* cheats. A netplay
//     peer has to agree with us about these, which is why they are in the
//     engine's file and not ours.
//   * ge007.eep (GoldenEye's own `game_options_entries`, through
//     geHostGameOption*) — the options GE itself has: look-ahead auto-centre,
//     the crosshair, auto-aim, the ammo counter. These already have a home,
//     a UI (the Q Watch) and a save slot; giving them a second one in our ini
//     would let the two disagree in front of the player.
//
// A row whose key does not exist is DROPPED at build time with a log line,
// rather than silently writing into nothing (the recomp does the same). That
// is what stops this file inventing engine keys nothing reads.
#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface GESettings : NSObject

/// Put the gear on the window, above the touch overlay. It is a SIBLING of
/// the overlay and not a child, because the overlay hides itself when a
/// controller connects (`Touch.Enabled` = auto) and that is exactly when a
/// player still needs to reach the settings. Idempotent; main thread.
+ (void)installGear;

/// Show/hide the gear per the rule in §Settings: hidden while the layout
/// editor is open, while the page itself is up, and once the player has been
/// continuously in gameplay for 0.4 s. Called from a timer; main thread.
+ (void)updateGearVisibility;

/// Present the page (idempotent). Main thread.
+ (void)present;
/// Dismiss it. Main thread.
+ (void)dismiss;
/// YES while it is up. The gear and the overlay both ask.
+ (BOOL)visible;

/// Every setting this PAGE owns, back to its factory value: the shell's
/// defaults, the engine keys listed below, and GoldenEye's own player
/// options. Deliberately not "every key in ge007.ini" — that file also holds
/// the window geometry and the harness's Tick.* knobs, which are nobody's
/// business here. Main thread.
+ (void)resetAll;

/// Scroll the page to a section and report what is now on screen, for the dev
/// bridge's `page scroll N`. A simulator cannot be driven by a finger at all
/// (simctl's injected events bypass UIKit), so without this there is no way to
/// photograph the groups below the fold. Main thread; returns a `key=value`
/// line, or an error line when the page is not up.
/// D-047: the settings table's bounds / content size / adjusted insets, so a
/// gate can assert "this page does not scroll horizontally" as a number
/// instead of a screenshot.
+ (NSString *)bridgeMetrics;

+ (NSString *)bridgeScrollToSection:(NSInteger)section;

/// D-089: open the Other App Audio picker over the open page (dev bridge).
+ (NSString *)bridgeAudioPicker;

/// Round 38: open the Texture pack picker sheet over the open page (dev bridge).
+ (NSString *)bridgeTexpackPicker;

/// D-141: flip the switch row titled `title` on the open page the way a
/// finger does — set the UISwitch and send its ValueChanged, so the page's own
/// target writes the keys (dev bridge `page switch "<title>" on|off`).
+ (NSString *)bridgeSetSwitch:(NSString *)title on:(BOOL)on;

/// Round 23 (R23-3): once per install, move an untouched maxfps=60 to 120 on
/// a >= 120 Hz panel. GEPacing calls it (main queue) after the first frame.
+ (void)applyPanelMaxFpsDefault;
/// Round 37: the 2D upscale / 2D filter platform defaults, once per install.
+ (void)apply2DDefaults;
/// Round 58: mp_show_names (the split-screen player label) to 0, once per install.
+ (void)applyMpNamesDefault;
/** Round 36 (visionOS): the pacer adopted a new granted rate; maxfps follows
 *  it unless the player picked one. Main thread. */
+ (void)visionFollowRate:(int)hz;

@end

NS_ASSUME_NONNULL_END
