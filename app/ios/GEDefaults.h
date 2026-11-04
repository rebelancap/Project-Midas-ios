// GEDefaults.h — the shell's own settings, in NSUserDefaults.
//
// Two stores exist and the split is deliberate:
//
//   * ge007.ini (engine, port/src/config.c) owns anything the SIM or the
//     renderer reads. It is written by the engine, on the engine's schedule,
//     and a netplay peer has to agree with us about it.
//   * NSUserDefaults (this file) owns anything only the SHELL reads — the
//     touch overlay's geometry and opacity, the look sensitivity, the target
//     frame rate. None of it reaches the sim, so none of it belongs in a file
//     the determinism gate diffs.
//
// The bridge's `settings get/set` reaches BOTH: a dotted key with a known
// engine prefix goes to the ini, everything else here.
#pragma once

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

NS_ASSUME_NONNULL_BEGIN

// Touch overlay
extern NSString *const GEDefaultsTouchEnabled;     // 0 = off, 1 = on, 2 = auto (hide when a pad is attached)
extern NSString *const GEDefaultsButtonScale;      // 0.6 … 1.6, multiplies every button radius
extern NSString *const GEDefaultsButtonOpacity;    // 0.1 … 1.0
extern NSString *const GEDefaultsLookDegPerPoint;  // view degrees per point of HORIZONTAL drag
extern NSString *const GEDefaultsLookDegPerPointY; // view degrees per point of VERTICAL drag
extern NSString *const GEDefaultsLookInvert;       // bool
extern NSString *const GEDefaultsFireDragLooks;    // bool — hold Fire and drag that finger to look
extern NSString *const GEDefaultsTouchHaptics;     // bool — tap the Taptic Engine on a button press
extern NSString *const GEDefaultsTouchLayout;      // JSON overrides for GETouchLayout; "" = the baked table
extern NSString *const GEDefaultsStickDeadzone;    // 0.05 … 0.40, radial, on the floating stick
extern NSString *const GEDefaultsStickCurve;       // 0.0 … 1.0 — the CUBIC share of the response
// Controller
extern NSString *const GEDefaultsPadDeadzone;      // 0.05 … 0.40, radial
extern NSString *const GEDefaultsPadLookDegPerSec;  // right stick X, degrees/second at full deflection
extern NSString *const GEDefaultsPadLookDegPerSecY; // right stick Y, degrees/second at full deflection
extern NSString *const GEDefaultsPadLookInvert;    // bool
// Round 57: the factory value of both pad look rows. A row's value / this is
// the multiple of Midas's own Look-stick rate a real (SDL) controller gets.
static const double GEPadLookFactoryDegPerSec = 160.0;
// Pacing
extern NSString *const GEDefaultsTargetFPS;        // 0 auto, 60, or the panel max (120 ProMotion, 90 visionOS)
extern NSString *const GEDefaultsCreditGate;       // Midas 9b: 1 = the display link credit gates every present (donor); 0 = lifecycle only
extern NSString *const GEDefaultsViPacer;          // D-065: 1 = the display link posts the VI retrace
extern NSString *const GEDefaultsRepeatMode;       // D-072: 1 = repeat-present deadline off the retrace grid
extern NSString *const GEDefaultsSpinUs;           // round 22: scheduler late-spin before a link callback, us (0 = off)
extern NSString *const GEDefaultsRtPolicy;         // round 22: 1/2 = THREAD_TIME_CONSTRAINT_POLICY on the scheduler (0 = off)
extern NSString *const GEDefaultsWorkInterval;     // round 22: 1 = each build is an os_workgroup interval with a deadline
extern NSString *const GEDefaultsWorkIntervalDeadlineMs; // round 28: that deadline, ms after the build starts (0 = the next link callback)
extern NSString *const GEDefaultsPumpMode;
extern NSString *const GEDefaultsRenderQos;        // round 23: 1 = ge-render at QOS_CLASS_USER_INTERACTIVE (0 = SDL default)
extern NSString *const GEDefaultsSimThread;        // round 26: 1 = the game loop runs on its own thread (ge-sim), not the main thread
extern NSString *const GEDefaultsSimPolicy;        // round 26: ge-sim's policy: 0 default QoS, 1 user-interactive, 2 = 1 + time-constraint
// Video (round 23, R23-3)
extern NSString *const GEDefaultsMaxFpsUser;       // 1 = the player has picked a Max FPS on the settings page
extern NSString *const GEDefaultsUpscale2DDone;    // round 37: 1 = the 2D upscale platform default was applied once
extern NSString *const GEDefaultsMpNamesOffDone;   // round 58: 1 = mp_show_names was moved to the new default 0 once
extern NSString *const GEDefaultsMaxFpsPanelDone;  // 1 = the panel default (120 on ProMotion) was considered once         // round 22: main-thread idle wait (0 run loop, 1 link semaphore, 2 bare spin)
// Debug
extern NSString *const GEDefaultsTexPoolKB;        // D-067: STAGE texture pool size, KB; 0 = the N64's own 439
// Audio
extern NSString *const GEDefaultsAudioOtherApps;   // D-089: 0..4, the Other App Audio mode (GEAudio.h)
extern NSString *const GEDefaultsAudioMixOthers;   // RETIRED bool, read once by the migration and removed
#if TARGET_OS_VISION
// Phase 6 M5 (round 49, docs/port-ios/69): the 3D settings, visionOS ONLY.
// perfect-dark-ios app/ios/PDDefaults.m `vp3d.*` (D-054/D-061): lengths in
// METRES, widths/heights as HALF-extents (what the compositor scales a unit
// quad by). geVision3dApplySettings() (app/vision3d/GEVisionSettings.m) is the
// one place they become 3D state.
extern NSString *const GEDefaults3DDistance;       // 1.0 … 8.0 m, default 3.6
extern NSString *const GEDefaults3DHalfWidth;      // 0.6 … 6.096 m (full 1.2 … 40 ft), default 3.048 (20 ft)
extern NSString *const GEDefaults3DHalfHeight;     // 0.5 … 3.0 m, default 1.8288 (12 ft)
extern NSString *const GEDefaults3DPosHeight;      // -1.5 … 10.0 m, default 0
extern NSString *const GEDefaults3DDepthPct;       // 0 … 300 %, default 150
extern NSString *const GEDefaults3DConvergence;    // 100 … 1500 units, default 762 (25 ft)
extern NSString *const GEDefaults3DDimming;        // 0 … 1, default 0.80
extern NSString *const GEDefaults3DRenderPct;      // 40 … 100 % of the panel-derived eye, default 100
extern NSString *const GEDefaults3DUnitsFeet;      // bool, default YES (ft)
#endif

@interface GEDefaults : NSObject

/// Registers every default above. Called once, before anything reads one.
+ (void)registerDefaults;

+ (double)doubleFor:(NSString *)key;
+ (NSInteger)intFor:(NSString *)key;
+ (BOOL)boolFor:(NSString *)key;

/// Returns NO if the key is not one of ours (so the bridge can fall through
/// to the engine's ini).
+ (BOOL)setString:(NSString *)value forKey:(NSString *)key;
+ (nullable NSString *)stringForKey:(NSString *)key;
/// Every key/value we own, for `settings get` with no argument.
+ (NSDictionary<NSString *, NSString *> *)all;

/// The factory value of one key as a string, for the settings page's
/// "Reset all settings" row and for a per-row revert.
+ (nullable NSString *)factoryStringForKey:(NSString *)key;

/// Forget every shell setting, including the saved touch layout, so the next
/// read of each returns the registered factory value. Does NOT touch the
/// engine's ini or the game's own saved options — the settings page resets
/// those itself, because they live in different files with different owners
/// (docs/shell.md §Settings).
+ (void)resetAll;

@end

NS_ASSUME_NONNULL_END
