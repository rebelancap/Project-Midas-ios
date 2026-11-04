// GEController.h — GCController -> OSContPad, controller-first.
//
// THE CONTROL SCHEME (DECISIONS D-024). GoldenEye's own N64 default is style
// 1.1 "Honey": the analog stick turns and walks, the C buttons sidestep and
// look, R aims, Z fires. That is a fine 1997 scheme and it is not what anyone
// picks up a modern pad expecting, so this port ships the twin-stick layout
// the GoldenEye XBLA remaster and our own recomp port use:
//
//   left stick  X  sidestep       -> C-left / C-right   (GE's own strafe)
//               Y  forward/back   -> analog stick Y
//   right stick    look           -> ABSOLUTE view degrees, continuous
//                                    (geHostLookAdd; C-up/C-down only as the
//                                    fallback when absolute look is off)
//   R2             fire           -> Z trigger
//   L2 / L1        aim            -> R trigger (GE's L and R are one button)
//   A              use / action   -> B    (GE's "action" bit is also reload)
//   B              crouch         -> R + C-down held (GE has no crouch bit)
//   Y / R1         next weapon    -> A    (a tap; hold A + tap Z = previous)
//   D-pad          D-pad          -> D-pad, which GE aliases to the C buttons:
//                                    sidestep/look in hip fire, lean/crouch/
//                                    zoom while aiming
//   Menu / Options pause          -> Start
//
// The face buttons do NOT map straight through, and getting that wrong is
// what shipped in 0.0.0.1: GoldenEye's A is the INVENTORY button (tap = next
// weapon) and its B is "action", which opens doors and reloads
// (src/game/bondview2.c:5231-5243, 5400-5410; the in-game options table says
// the same at src/game/options.c:148). Mapping the pad's A to GE's A made
// "USE" cycle the weapon and "RELOAD" open the door. GE's L is not a separate
// action at all — aimButtons is `L_TRIG | R_TRIG` and the two are
// indistinguishable (bondview2.c:5241).
//
// X/square is deliberately UNBOUND. bean puts RELOAD there and GoldenEye has
// no separate reload bit; the remaining in-game action, "previous weapon", is
// the combo hold-A-and-tap-Z, which cannot be one button without fighting the
// fire interlock at bondview2.c:5482-5484.
//
// Why the right stick does not simply become the N64 stick: in hip fire GE
// reads pitch ONLY from the digital C-up/C-down buttons, so a stick mapped
// there gives one speed of vertical look and nothing in between — the
// complaint that made port/src/input.c grow its D194 absolute-aim path for the
// mouse. Degrees straight to the camera is the same fix for a thumbstick, and
// it is the only mapping under which look speed is a number a settings row can
// mean something about (degrees per second) rather than a stick count.
//
// LOCAL MULTIPLAYER (round 29d, D-233/D-234): up to four controllers, one
// per player, in connection order. Controller 1 and the touch overlay BOTH
// drive player 1; controllers 2-4 drive players 2-4 through ports of their
// own, with this same mapping (one function, geMapGamepad) and their own
// absolute-look accumulator applied to their own player on the sim thread.
//
// Why the LEFT stick's X is digital and this is not a regression: GE has no
// analog sidestep. The sidestep axis is C-left/C-right on the console and
// through every emulator, so there is no continuous value to send. Full
// deflection past the dead zone is what the hardware could express.
#pragma once

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface GEController : NSObject

/// Start watching for controllers. Main thread, once, at launch.
+ (void)start;

/// visionOS only (a no-op on iOS): claim gamepad events for SDL's window with a
/// GCEventInteraction, or the pad's buttons become gaze-pinch events. Main
/// thread; idempotent per window, so the 100 ms graft watch can re-ask it.
+ (void)claimGamepadEventsInWindow:(nullable UIWindow *)window;

/// Sample the attached controller and publish to GEInput. Called once per
/// pacer callback (≈ once per presented frame) — GCController's state is a
/// snapshot, so polling is both correct and cheaper than a value handler per
/// element.
/// @param dt seconds since the previous poll; the right stick's look is in
///           degrees per second, so it has to be integrated over real time or
///           it turns twice as fast on a 120 Hz panel.
+ (void)pollWithDelta:(double)dt;

/// Human-readable, for the bridge's `state`: "1:<vendor>,2:<vendor>" by
/// controller number (= player), or "none".
+ (NSString *)describe;

/// Controllers holding a port right now (0..4). Round 29d (D-233): up to four
/// are adopted, controller N = player N, in connection order; a disconnect
/// frees that port only and a reconnect reclaims it.
+ (NSInteger)connectedCount;

#if !GE_PUBLIC
/// D-235, dev bridge `vpad …`: virtual controllers through the real
/// adopt/drop/poll path (the simulator has no physical pads).
+ (NSString *)bridgeVpad:(NSArray<NSString *> *)w;
#endif

@end

NS_ASSUME_NONNULL_END
