// GEVision.h — the UIKit facts that differ on visionOS, in one place, so the
// shell stays ONE source tree (Phase 5, docs/visionos-plan.md §2; the shape of
// perfect-dark-ios app/ios/PDVision.h, PD D-025).
//
// What is actually missing on xrOS, measured by building this shell against the
// xros SDK (round 37a, D-342), not guessed:
//
//   UIScreen                   unavailable — no screens, only window scenes;
//                              a window's size is the user's drag.
//   UIImpactFeedbackGenerator  unavailable — no haptics on the headset
//                              (GETouchOverlay.m / GESettings.m gate it inline).
//
// Everything else the shell uses (UIWindowScene, UIWindow, the settings table,
// gesture recognisers, GCController, AVAudioSession, the document picker)
// compiles and behaves the same.
#pragma once

#import <UIKit/UIKit.h>
#include <TargetConditionals.h>

#ifndef TARGET_OS_VISION
#define TARGET_OS_VISION 0
#endif

/**
 * The panel's maximum refresh, for the pacer's resolve and its log lines.
 *
 * No UIScreen to ask on visionOS. The Vision Pro panel runs 90 Hz (M5 models
 * up to 120); 90 is the honest floor (PD D-056). Round 34 (M-V3, doc 44): the
 * pacer's fast arm is this number (GEPacing.m geIosFastHz), so visionOS paces
 * at 90 - sim tick, link and presents. Until round 34 the arm was a literal
 * `panel >= 120 ? 120 : 60`, which made visionOS ask for 60 and get 45
 * (every second 90 Hz refresh) on Austin's headset. Adopting 120 on an M5 in
 * a 120 Hz mode is R34-1.
 */
static inline NSInteger GEVisionMaxFPS(void)
{
#if TARGET_OS_VISION
	return 90;
#else
	return UIScreen.mainScreen.maximumFramesPerSecond;
#endif
}

/** The dev bridge's default port: :8777 iOS, :8787 visionOS (charter; the two
 *  simulators share the Mac's loopback, PD D-027). GE_BRIDGE_PORT overrides. */
static inline int GEVisionDefaultBridgePort(void)
{
#if TARGET_OS_VISION
	return 8787;
#else
	return 8777;
#endif
}
