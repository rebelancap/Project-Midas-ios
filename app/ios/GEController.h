// GEController.h — GCController -> OSContPad, controller-first.
//
// WHO MAPS A REAL CONTROLLER (round 9c, R9B-1; restated in round 59). Midas
// opens every real controller through SDL's MFi backend (port/src/input_pc.c)
// and maps it with its OWN layout table, tools/gamecontrollerdefault.txt
// (bundled; a new pad seeds "Xbox Standard", or "PlayStation Standard" /
// "Switch Standard"). While SDL holds any pad, this file only REPORTS (the
// overlay's auto mode). So the table a player actually gets on iOS and
// visionOS is Midas's Xbox Standard layout:
//
//   left stick     move / strafe; WHILE AIMING: Y zooms (up = in, down = out,
//                  sniper rifle and camera only; with a gun that cannot zoom,
//                  pulling back ducks, Midas's 2.x rule), X leans
//   right stick    look (Settings -> Controller sliders scale it, round 57)
//   RT / R2        fire                LT / L2   aim
//   A / cross      use / accept        B / circle   next gadget / back
//   X / square     reload              Y / triangle next weapon
//   LB / L1        Classic Mode toggle and watch page left
//   RB / R1        detonate remote mines and watch page right
//   L3             crouch / stand (toggle; no aim mode)
//   D-pad          digital move        Menu / Start  pause
//   (Midas's in-game Controls menu can switch a pad to the "Pro" layout or
//   rebind it.)
//
// THE SHELL'S OWN MAPPING (geMapGamepad) serves only the pads SDL does not
// hold, which in practice is the dev bridge's virtual pads (D-235). It is
// the twin-stick scheme of D-024, kept close to the table above so a gate
// that drives it exercises the same actions:
//
//   left stick  X  sidestep / lean      Y  forward/back; while aiming: zoom
//                                          (Midas zoom_in / zoom_out levels)
//   right stick    look            -> ABSOLUTE view degrees, continuous
//   R2             fire            -> Z trigger
//   L2 / L1        aim             -> R trigger (GE's L and R are one button)
//   A              use / action    -> B    (GE's "action" bit is also reload)
//   B              next gadget     -> Midas next_inventory (round 59; until
//                                     then R + C-down, a crouch that forced
//                                     aim mode)
//   X              reload          -> Midas reload (round 59; was unbound)
//   L3             crouch / stand  -> Midas crouch_toggle (round 59)
//   Y / R1         next weapon     -> A    (a tap; hold A + tap Z = previous)
//   D-pad          D-pad           -> D-pad, which GE aliases to the C buttons:
//                                    sidestep/look in hip fire, lean/crouch/
//                                    zoom while aiming
//   Menu / Options pause           -> Start
//
// The face buttons do NOT map straight through, and getting that wrong is
// what shipped in 0.0.0.1: GoldenEye's A is the INVENTORY button (tap = next
// weapon) and its B is "action", which opens doors and reloads
// (src/game/bondview2.c:5231-5243, 5400-5410; the in-game options table says
// the same at src/game/options.c:148). GE's L is not a separate action at
// all: aimButtons is `L_TRIG | R_TRIG` (bondview2.c:5241).
//
// Why the right stick does not simply become the N64 stick: in hip fire GE
// reads pitch ONLY from the digital C-up/C-down buttons, so a stick mapped
// there gives one speed of vertical look and nothing in between. Degrees
// straight to the camera is the fix, and it is the only mapping under which
// look speed is a number a settings row can mean something about.
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
