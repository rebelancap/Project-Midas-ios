// GEAudio.m — see GEAudio.h (D-089).
#import "GEAudio.h"
#import "GEShell.h"
#import "GEDefaults.h"
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <math.h>

// port/include/audio.h — the engine's host gain, multiplied into each mixed
// block just before SDL_QueueAudio and ramped there.
extern void  audioSetHostGain(float gain);
extern float audioGetHostGain(void);

// How far the game drops in "Lower Game Audio" while another app plays. The
// family's number (bean audio_session_uikit.mm, PD PDAudio.m, vkQuake guide);
// there is no reason for ports in one family to disagree about it.
static const float kDuckGain = 0.22f;

// Written and read on sQueue only, except where noted.
static int sSets = 0;                   // category/options actually set
static int sDrifts = 0;                 // poll found someone else's options
static NSString *sLastError = nil;
static BOOL sArmed = NO;
static _Atomic(float) sDuck = 1.0f;     // the other-app factor; read by describe
static _Atomic(int) sOtherOverride = -1; // dev bridge: -1 real, 0/1 forced
static _Atomic(int) sOtherSeen = 0;      // last geOtherAudioPlaying answer
static _Atomic(int) sSdlPlayback = 0;    // 1 = SDL_AUDIO_CATEGORY=playback is set

static dispatch_queue_t geAudioQueue(void)
{
	static dispatch_queue_t q;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		q = dispatch_queue_create("com.rebelancap.projectmidas.audio-session",
			dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
	});
	return q;
}

// describe / stateLines run on the bridge thread; sSets, sDrifts and
// sLastError belong to the audio queue. Snapshot them there. Never call this
// FROM the audio queue (dispatch_sync onto itself would deadlock).
static void geAudioCounters(int *sets, int *drifts, NSString **lastError)
{
	__block int a = 0, b = 0;
	__block NSString *e = nil;
	dispatch_sync(geAudioQueue(), ^{
		a = sSets;
		b = sDrifts;
		e = [sLastError copy];
	});
	*sets = a;
	*drifts = b;
	if (lastError) {
		*lastError = e;
	}
}

static NSInteger geMode(void)
{
	NSInteger m = [GEDefaults intFor:GEDefaultsAudioOtherApps];
	return (m < 0 || m > 4) ? 2 : m;
}

/// Playback in all five (the game keeps sounding with the silent switch on);
/// only the mixability bits differ. Modes 3/4 attenuate US, in the mixer.
static AVAudioSessionCategoryOptions geOptionsForMode(NSInteger mode)
{
	switch (mode) {
	case 0:  // Stop Other Audio: non-mixable; activation interrupts the other app
		return 0;
	case 2:  // Lower Other Audio: iOS ducks them
		return AVAudioSessionCategoryOptionMixWithOthers | AVAudioSessionCategoryOptionDuckOthers;
	default: // Play Both / Lower Game / Mute Game
		return AVAudioSessionCategoryOptionMixWithOthers;
	}
}

static NSString *geOptionsName(AVAudioSessionCategoryOptions o)
{
	if (o == 0) {
		return @"none";
	}
	NSMutableArray *a = [NSMutableArray array];
	if (o & AVAudioSessionCategoryOptionMixWithOthers) [a addObject:@"MixWithOthers"];
	if (o & AVAudioSessionCategoryOptionDuckOthers)    [a addObject:@"DuckOthers"];
	AVAudioSessionCategoryOptions rest = o & ~(AVAudioSessionCategoryOptionMixWithOthers |
	                                           AVAudioSessionCategoryOptionDuckOthers);
	if (rest) [a addObject:[NSString stringWithFormat:@"%#lx", (unsigned long)rest]];
	return [a componentsJoinedByString:@"|"];
}

static NSString *geShort(NSString *s)
{
	s = [s stringByReplacingOccurrencesOfString:@"AVAudioSessionCategory" withString:@""];
	return [s stringByReplacingOccurrencesOfString:@"AVAudioSessionMode" withString:@""];
}

/// The broad "someone else has sound out" OR the narrower "another app is
/// playing PRIMARY audio" — either means the player is listening to something
/// that is not us. The dev bridge can force it (the simulator has no music app).
static BOOL geOtherAudioPlaying(AVAudioSession *s)
{
	const int o = atomic_load_explicit(&sOtherOverride, memory_order_relaxed);
	const BOOL real = s.isOtherAudioPlaying || s.secondaryAudioShouldBeSilencedHint;
	const BOOL v = (o >= 0) ? (o != 0) : real;
	atomic_store_explicit(&sOtherSeen, v ? 1 : 0, memory_order_relaxed);
	return v;
}

/// The other-app half: only modes 3 and 4 attenuate the game. geAudioQueue.
static void geRecomputeDuck(AVAudioSession *s)
{
	const NSInteger mode = geMode();
	const BOOL other = geOtherAudioPlaying(s);
	float duck = 1.0f;
	if (other) {
		if (mode == 3) {
			duck = kDuckGain;   // Lower Game Audio
		} else if (mode == 4) {
			duck = 0.0f;        // Mute Game Audio
		}
	}
	const float was = atomic_exchange_explicit(&sDuck, duck, memory_order_relaxed);
	audioSetHostGain(duck);
	if (fabsf(was - duck) > 0.0005f) {
		GELog(@"[audio] game gain %.2f -> %.2f (mode %ld %@, other audio playing %d)",
		      was, duck, (long)mode, GEAudio.modeTitles[(NSUInteger)mode], (int)other);
	}
}

/// SDL reads SDL_AUDIO_CATEGORY (the env var outranks a plain SDL_SetHint) each
/// time it opens or closes a device. "playback" = Playback without
/// MixWithOthers; unset = its Playback|Mix|Duck default. In SDL2 2.32 the
/// "playback" branch still adds DuckOthers (which implies Mix), so SDL never
/// produces a truly non-mixable session on its own — our own activation in
/// +start, BEFORE the engine opens the device, is what actually stops the other
/// app at launch; the hint only narrows the gap. Main thread only (setenv is
/// not safe against a concurrent getenv on another thread mid-realloc, and
/// SDL's device open is on the main thread here).
static void geSteerSDL(NSInteger mode)
{
	if (mode == 0) {
		setenv("SDL_AUDIO_CATEGORY", "playback", 1);
	} else {
		unsetenv("SDL_AUDIO_CATEGORY");
	}
	// Mirrored so the bridge thread never calls getenv() against a setenv().
	atomic_store_explicit(&sSdlPlayback, mode == 0 ? 1 : 0, memory_order_relaxed);
}

/// Set the category and options if they are not already ours, activate,
/// recompute the gain. geAudioQueue.
static void geApply(NSString *reason)
{
	AVAudioSession *s = AVAudioSession.sharedInstance;
	const NSInteger mode = geMode();
	const AVAudioSessionCategoryOptions want = geOptionsForMode(mode);
	NSError *err = nil;

	if (![s.category isEqualToString:AVAudioSessionCategoryPlayback] ||
	    ![s.mode isEqualToString:AVAudioSessionModeDefault] ||
	    s.categoryOptions != want) {
		NSString *wasCat = geShort(s.category);
		const AVAudioSessionCategoryOptions wasOpts = s.categoryOptions;
		if ([s setCategory:AVAudioSessionCategoryPlayback
		              mode:AVAudioSessionModeDefault
		           options:want
		             error:&err]) {
			sSets++;
			sLastError = nil;
			GELog(@"[audio] session category Playback options %@ (%#lx) — mode %ld %@, "
			      @"reason %@, set #%d (was %@ %#lx), route %@",
			      geOptionsName(want), (unsigned long)want, (long)mode,
			      GEAudio.modeTitles[(NSUInteger)mode], reason, sSets, wasCat,
			      (unsigned long)wasOpts,
			      s.currentRoute.outputs.firstObject.portType ?: @"?");
		} else {
			if (![sLastError isEqualToString:err.localizedDescription]) {
				GELog(@"[audio] setCategory FAILED (mode %ld, reason %@): %@",
				      (long)mode, reason, err);
			}
			sLastError = err.localizedDescription;
		}
	}

	if (![s setActive:YES error:&err]) {
		// Normal during an interruption (a call): the session is inactive on
		// purpose and the next poll picks it back up. Not worth a line per poll.
		sLastError = err.localizedDescription;
	}

	// Switching TO the non-mixable mode mid-session only interrupts the other
	// app when the session (re)activates, and setActive:YES on an active
	// session is a no-op — so if the other app is still going, bounce it once.
	// Losing this race is survivable: the boot path makes the next launch right.
	if (mode == 0 && sArmed && geOtherAudioPlaying(s)) {
		[s setActive:NO
		 withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
		       error:NULL];
		if (![s setActive:YES error:&err]) {
			sLastError = err.localizedDescription;
		}
		GELog(@"[audio] Stop Other Audio: session bounced to interrupt the other app");
	}

	geRecomputeDuck(s);
	sArmed = YES;
}

/// The poll body and every notification: re-apply only on drift, otherwise
/// just follow the other app. geAudioQueue.
static void geCheck(NSString *why)
{
	if (!sArmed) {
		return;
	}
	AVAudioSession *s = AVAudioSession.sharedInstance;
	if (![s.category isEqualToString:AVAudioSessionCategoryPlayback] ||
	    s.categoryOptions != geOptionsForMode(geMode())) {
		sDrifts++;
		GELog(@"[audio] session drifted to %@ options %@ (%#lx) [%@] — re-asserting",
		      geShort(s.category), geOptionsName(s.categoryOptions),
		      (unsigned long)s.categoryOptions, why);
		geApply(why);
		return;
	}
	geRecomputeDuck(s);
}

#if TARGET_OS_VISION
// ---------------------------------------------------------------------------
// Phase 6 M4 (round 48, plan section 2.7): the spatial intent while the picture
// is world-locked. Copied from perfect-dark-ios app/ios/PDAudio.m
// pdApplySpatial (D-051), pd -> ge. Head-tracked either way: "off" is not
// Bypassed (that would make the windowed app sound flat), it is the AUTOMATIC
// sound stage anchored the automatic way, which is what a plain window gets.
//
// Written and read on geAudioQueue, except sImmersive (set from the main
// thread's mode switch; a plain int is enough for a flag whose consumer
// re-reads it every 3 s).
// ---------------------------------------------------------------------------
static volatile int sImmersive = 0;
static int sSpatialApplied = 0;          // 0 never, 1 automatic, 2 immersive
static _Atomic(int) sSpatialState = 0;   // 0 off, 1 applied (immersive), 2 refused
static NSString *sSpatialError = nil;
static int sSpatialTicks = 0;

static void geApplySpatial(NSString *why)
{
	AVAudioSession *s = AVAudioSession.sharedInstance;
	const int imm = sImmersive ? 1 : 0;
	NSDictionary *opts = @{
		AVAudioSessionSpatialExperienceOptionSoundStageSize:
			@(imm ? AVAudioSessionSoundStageSizeMedium : AVAudioSessionSoundStageSizeAutomatic),
		AVAudioSessionSpatialExperienceOptionAnchoringStrategy:
			@(imm ? AVAudioSessionAnchoringStrategyFront : AVAudioSessionAnchoringStrategyAutomatic),
	};
	NSError *err = nil;
	if ([s setIntendedSpatialExperience:AVAudioSessionSpatialExperienceHeadTracked
	                            options:opts error:&err]) {
		sSpatialError = nil;
		if (sSpatialApplied != imm + 1) {
			// Only the TRANSITIONS are logged: this re-applies every 3 s.
			GELog(@"[audio] spatial experience -> headTracked(%s, %s) [%@]",
			      imm ? ".medium" : ".automatic", imm ? ".front" : ".automatic", why);
		}
		sSpatialApplied = imm + 1;
		atomic_store_explicit(&sSpatialState, imm ? 1 : 0, memory_order_relaxed);
	} else {
		if (![sSpatialError isEqualToString:err.localizedDescription]) {
			GELog(@"[audio] spatial experience REFUSED (%s) [%@]: %@",
			      imm ? "immersive" : "automatic", why, err);
		}
		sSpatialError = err.localizedDescription;
		sSpatialApplied = 0;
		atomic_store_explicit(&sSpatialState, imm ? 2 : 0, memory_order_relaxed);
	}
}

/// The 4 Hz poll's spatial half: re-apply every 12th tick (~3 s) while
/// immersive - SDL and the system both reset the intent behind our back.
static void geSpatialTick(void)
{
	if (!sImmersive) {
		return;
	}
	if (++sSpatialTicks >= 12) {
		sSpatialTicks = 0;
		geApplySpatial(@"re-apply");
	}
}
#endif

@implementation GEAudio

+ (NSArray<NSString *> *)modeTitles
{
	// Index == the stored mode. They say what the ROW does to the player's
	// music, not what AVAudioSession calls it.
	return @[ @"Stop Other Audio", @"Play Both", @"Lower Other Audio",
	          @"Lower Game Audio", @"Mute Game Audio" ];
}

+ (NSArray<NSString *> *)modeDetails
{
	// One sentence each, because "duck" and "mix" mean nothing to a player.
	// The family's sentences (PD PDAudio.m, bean), this game's name.
	return @[
		@"Music and podcasts stop when GoldenEye starts.",
		@"Both play together, neither one quieter.",
		@"Music and podcasts drop to the background; game audio stays full.",
		@"Game audio drops to the background while another app is playing.",
		@"Game audio goes silent while another app is playing.",
	];
}

+ (NSInteger)currentMode
{
	return geMode();
}

+ (void)start
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		const NSInteger mode = geMode();
		// TRAP 2. Main thread, before geEngineMain opens the SDL device.
		geSteerSDL(mode);
		// SYNCHRONOUS for the boot set: the whole point is that our
		// category is live (and, in Stop Other Audio, has interrupted the
		// other app) before SDL activates anything.
		dispatch_sync(geAudioQueue(), ^{ geApply(@"boot"); });

		// The poll, 4 Hz, on our queue (never the engine's main thread).
		// SDL sets its own options when it opens the device — after this —
		// so in a mode that disagrees with SDL's default the first tick after
		// the open logs one re-assert, and then nothing.
		static dispatch_source_t timer;
		timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, geAudioQueue());
		dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4),
		                          NSEC_PER_SEC / 4, NSEC_PER_SEC / 20);
		dispatch_source_set_event_handler(timer, ^{
			geCheck(@"poll");
#if TARGET_OS_VISION
			geSpatialTick();
#endif
		});
		dispatch_resume(timer);

		NSOperationQueue *q = [NSOperationQueue new];
		q.maxConcurrentOperationCount = 1;
		q.qualityOfService = NSQualityOfServiceUtility;
		NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;

		[nc addObserverForName:AVAudioSessionInterruptionNotification
		                object:nil queue:q usingBlock:^(NSNotification *n) {
			NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
			const BOOL ended = (type == AVAudioSessionInterruptionTypeEnded);
			GELog(@"[audio] interruption %@", ended ? @"ended" : @"began");
			if (ended) {
				// A phone call, Siri, an alarm: the session was deactivated
				// under us and nothing re-activates it unless we do.
				dispatch_async(geAudioQueue(), ^{ geApply(@"interruption-ended"); });
			}
		}];
		[nc addObserverForName:AVAudioSessionRouteChangeNotification
		                object:nil queue:q usingBlock:^(NSNotification *n) {
			(void)n;
			dispatch_async(geAudioQueue(), ^{ geCheck(@"route-change"); });
		}];
		[nc addObserverForName:AVAudioSessionSilenceSecondaryAudioHintNotification
		                object:nil queue:q usingBlock:^(NSNotification *n) {
			(void)n;
			dispatch_async(geAudioQueue(), ^{ geCheck(@"secondary-hint"); });
		}];
		[nc addObserverForName:UIApplicationDidBecomeActiveNotification
		                object:nil queue:q usingBlock:^(NSNotification *n) {
			(void)n;
			dispatch_async(geAudioQueue(), ^{ geApply(@"did-become-active"); });
		}];

		GELog(@"[audio] session armed — Playback, mode %ld %@, SDL_AUDIO_CATEGORY=%s, "
		      @"re-asserted on drift (4 Hz poll + route/interruption/hint/active)",
		      (long)mode, GEAudio.modeTitles[(NSUInteger)mode],
		      getenv("SDL_AUDIO_CATEGORY") ?: "(unset)");
	});
}

+ (void)settingsChanged
{
	const NSInteger mode = geMode();
	dispatch_block_t steer = ^{ geSteerSDL(mode); };
	if (NSThread.isMainThread) {
		steer();
	} else {
		dispatch_async(dispatch_get_main_queue(), steer);
	}
	dispatch_async(geAudioQueue(), ^{ geApply(@"settings"); });
}

+ (void)reassert
{
	dispatch_async(geAudioQueue(), ^{ geCheck(@"reassert"); });
}

#if TARGET_OS_VISION
+ (void)setImmersive:(BOOL)immersive
{
	sImmersive = immersive ? 1 : 0;
	dispatch_async(geAudioQueue(), ^{
		sSpatialTicks = 0;
		geApplySpatial(immersive ? @"space open" : @"space closed");
	});
}

+ (NSString *)spatialState
{
	switch (atomic_load_explicit(&sSpatialState, memory_order_relaxed)) {
	case 1:  return @"applied";
	case 2:  return @"refused";
	default: return @"off";
	}
}
#endif

+ (void)setOtherAudioOverride:(int)override
{
	atomic_store_explicit(&sOtherOverride, override < 0 ? -1 : (override ? 1 : 0),
	                      memory_order_relaxed);
	GELog(@"[audio] other-audio override -> %d (dev bridge)", override < 0 ? -1 : (override ? 1 : 0));
	dispatch_async(geAudioQueue(), ^{ geCheck(@"override"); });
}

+ (NSString *)describe
{
	AVAudioSession *s = AVAudioSession.sharedInstance;
	const NSInteger mode = geMode();
	int sets, drifts;
	NSString *lastError;
	geAudioCounters(&sets, &drifts, &lastError);
	return [NSString stringWithFormat:@"%@/%@ %@ mode=%ld(%@) other=%d gain=%.2f sets=%d drifts=%d%@",
	        s.category, s.mode, geOptionsName(s.categoryOptions), (long)mode,
	        GEAudio.modeTitles[(NSUInteger)mode],
	        atomic_load_explicit(&sOtherSeen, memory_order_relaxed),
	        audioGetHostGain(), sets, drifts,
	        lastError ? [NSString stringWithFormat:@" err=%@", lastError] : @""];
}

+ (NSString *)stateLines
{
	AVAudioSession *s = AVAudioSession.sharedInstance;
	const NSInteger mode = geMode();
	int sets, drifts;
	geAudioCounters(&sets, &drifts, NULL);
	return [NSString stringWithFormat:
		@"audio_mode=%ld\naudio_mode_name=%@\naudio_category=%@\naudio_options=%#lx\n"
		@"audio_options_want=%#lx\naudio_other_playing=%d\naudio_other_override=%d\n"
		@"audio_duck=%.2f\naudio_gain=%.2f\naudio_sets=%d\naudio_drifts=%d\n"
		@"audio_sdl_category=%s\n",
		(long)mode, GEAudio.modeTitles[(NSUInteger)mode], geShort(s.category),
		(unsigned long)s.categoryOptions, (unsigned long)geOptionsForMode(mode),
		atomic_load_explicit(&sOtherSeen, memory_order_relaxed),
		atomic_load_explicit(&sOtherOverride, memory_order_relaxed),
		atomic_load_explicit(&sDuck, memory_order_relaxed), audioGetHostGain(),
		sets, drifts,
		atomic_load_explicit(&sSdlPlayback, memory_order_relaxed) ? "playback" : "-"];
}

@end
