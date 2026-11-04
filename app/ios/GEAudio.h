// GEAudio.h — the AVAudioSession policy ("Other App Audio"), and keeping it.
//
// D-089 (round 19a). Austin: "Mix with other audio setting does NOT follow my
// instructions. You're supposed to do what my other ports do. Give options for
// other app audio." This is the program's five-mode picker, exactly as the
// siblings ship it: the spec is ~/dev/vkQuake-ios/docs/IOS-AUDIO-SESSION-GUIDE.md
// and the reference implementation perfect-dark-ios app/ios/PDAudio.{h,m}. The
// names and the one-sentence explanations are the family's, verbatim, with this
// game's name.
//
//   mode  title               category options              game gain while another app plays
//   0     Stop Other Audio    Playback  0 (non-mixable)     1.00 (nothing else can play)
//   1     Play Both           Playback  MixWithOthers       1.00
//   2     Lower Other Audio   Playback  Mix | DuckOthers    1.00   <- default (= SDL's own)
//   3     Lower Game Audio    Playback  MixWithOthers       0.22   (kDuckGain, the family's number)
//   4     Mute Game Audio     Playback  MixWithOthers       0.00
//
// Modes 3 and 4 have no session equivalent — iOS ducks OTHER apps for you and
// never ducks you — so they are a gain the engine's mixer multiplies into each
// block just before SDL_QueueAudio (port/src/audio.c audioSetHostGain, ramped
// over ~0.2 s). "Another app is playing" = isOtherAudioPlaying ||
// secondaryAudioShouldBeSilencedHint, polled.
//
// TRAP 1 (the guide's): SDL's CoreAudio backend sets its OWN category whenever
// it opens or closes a device, and for Playback it always adds DuckOthers. So
// the policy is RE-ASSERTED: on route change, interruption, the silence-
// secondary hint, scene activation, and a 4 Hz poll that compares the live
// options with ours. All of it on GEAudio's own serial queue — the main thread
// is the engine's host loop, and an AVAudioSession round trip inside a frame is
// a frame that can miss its display link (PD M-029).
//
// TRAP 2: the FIRST activation is the one that interrupts other apps, and
// setActive:YES on an already-active session is a no-op. +start therefore runs
// before the engine opens its SDL device (ge_ios_main.m) and also steers SDL
// via the SDL_AUDIO_CATEGORY env var, so a mode picked last session is right on
// this launch; a runtime switch INTO Stop Other Audio bounces the session once.
//
// Every category set logs one `[audio] session category … options …` line; a
// normal boot logs the boot set and, in a mode that disagrees with SDL's
// default, ONE re-assert after SDL's device open — not a fight.
#pragma once

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

NS_ASSUME_NONNULL_BEGIN

@interface GEAudio : NSObject

/// Steer SDL, set the category, activate, subscribe to the notifications and
/// start the poll. Main thread, once, BEFORE the engine opens its audio device.
+ (void)start;

/// The mode changed (settings page, bridge, reset): re-apply the session and
/// recompute the game gain. Any thread.
+ (void)settingsChanged;

/// Re-apply now if it drifted. Cheap and idempotent; any thread.
+ (void)reassert;

/// The stored mode, clamped to 0…4 (2 when out of range).
+ (NSInteger)currentMode;

/// The five titles, index == mode. The family's words.
+ (NSArray<NSString *> *)modeTitles;

/// One sentence per mode, for the picker. The family's words, this game's name.
+ (NSArray<NSString *> *)modeDetails;

/// One line for the bridge's `audio=` and the settings page's info row.
+ (NSString *)describe;

/// `audio_*` lines for the bridge's `state`.
+ (NSString *)stateLines;

/// Dev bridge only: pretend another app is (1) or is not (0) playing, or
/// follow the real session (-1). The simulator has no other app that can play
/// audio, so this is how a sim round drives modes 3 and 4 through the real
/// gain path. Nothing in a public build sets it.
+ (void)setOtherAudioOverride:(int)override;

#if TARGET_OS_VISION
/// Phase 6 M4 (round 48): the immersive space opened (YES) or closed (NO).
/// headTracked(.medium, .front) while open, re-applied every ~3 s;
/// headTracked(.automatic, .automatic) after. Any thread.
+ (void)setImmersive:(BOOL)immersive;
/// `applied` (immersive intent live), `refused` (the session said no; the
/// error is logged), or `off` (2D). For `3d state`'s spatial row.
+ (NSString *)spatialState;
#endif

@end

NS_ASSUME_NONNULL_END
