// GEDefaults.m — see GEDefaults.h.
#import "GEDefaults.h"
#import "GEShell.h"

NSString *const GEDefaultsTouchEnabled      = @"Touch.Enabled";
NSString *const GEDefaultsButtonScale       = @"Touch.ButtonScale";
NSString *const GEDefaultsButtonOpacity     = @"Touch.ButtonOpacity";
NSString *const GEDefaultsLookDegPerPoint   = @"Touch.LookDegPerPoint";
NSString *const GEDefaultsLookDegPerPointY  = @"Touch.LookDegPerPointY";
NSString *const GEDefaultsLookInvert        = @"Touch.LookInvert";
NSString *const GEDefaultsFireDragLooks     = @"Touch.FireDragLooks";
NSString *const GEDefaultsTouchHaptics      = @"Touch.Haptics";
NSString *const GEDefaultsTouchLayout       = @"Touch.Layout";
NSString *const GEDefaultsStickDeadzone     = @"Touch.StickDeadzone";
NSString *const GEDefaultsStickCurve        = @"Touch.StickCurve";
NSString *const GEDefaultsPadDeadzone       = @"Pad.Deadzone";
NSString *const GEDefaultsPadLookDegPerSec  = @"Pad.LookDegPerSec";
NSString *const GEDefaultsPadLookDegPerSecY = @"Pad.LookDegPerSecY";
NSString *const GEDefaultsPadLookInvert     = @"Pad.LookInvert";
NSString *const GEDefaultsTargetFPS         = @"Pacing.TargetFPS";
NSString *const GEDefaultsViPacer           = @"Pacing.ViPacer";
NSString *const GEDefaultsCreditGate        = @"Pacing.CreditGate";
NSString *const GEDefaultsRepeatMode        = @"Pacing.RepeatMode";
NSString *const GEDefaultsSpinUs            = @"Pacing.SpinUs";
NSString *const GEDefaultsRtPolicy          = @"Pacing.RtPolicy";
NSString *const GEDefaultsPumpMode          = @"Pacing.PumpMode";
NSString *const GEDefaultsWorkInterval      = @"Pacing.WorkInterval";
NSString *const GEDefaultsWorkIntervalDeadlineMs = @"Pacing.WorkIntervalDeadlineMs";
NSString *const GEDefaultsRenderQos         = @"Pacing.RenderQos";
NSString *const GEDefaultsSimThread         = @"Pacing.SimThread";
NSString *const GEDefaultsSimPolicy         = @"Pacing.SimPolicy";
NSString *const GEDefaultsMaxFpsUser        = @"Video.MaxFpsUser";
NSString *const GEDefaultsMaxFpsPanelDone   = @"Video.MaxFpsPanelDefault";
NSString *const GEDefaultsUpscale2DDone     = @"Video.Upscale2DDefault";
NSString *const GEDefaultsMpNamesOffDone    = @"Video.MpShowNamesOffDefault";
NSString *const GEDefaultsTexPoolKB         = @"Debug.TexPoolKB";
NSString *const GEDefaultsAudioOtherApps    = @"Audio.OtherAppAudio";
NSString *const GEDefaultsAudioMixOthers    = @"Audio.MixWithOthers";
#if TARGET_OS_VISION
// The key strings are perfect-dark-ios PDDefaults.m's, so the family's notes
// and scripts read the same everywhere.
NSString *const GEDefaults3DDistance        = @"vp3d.dist";
NSString *const GEDefaults3DHalfWidth       = @"vp3d.halfW";
NSString *const GEDefaults3DHalfHeight      = @"vp3d.halfH";
NSString *const GEDefaults3DPosHeight       = @"vp3d.posH";
NSString *const GEDefaults3DDepthPct        = @"vp3d.depthPct";
NSString *const GEDefaults3DConvergence     = @"vp3d.conv";
NSString *const GEDefaults3DDimming         = @"vp3d.dim";
NSString *const GEDefaults3DRenderPct       = @"vp3d.renderPct";
NSString *const GEDefaults3DUnitsFeet       = @"vp3d.unitsFt";
#endif

@implementation GEDefaults

+ (NSDictionary *)factory
{
#if TARGET_OS_VISION
	// Phase 6 M5: the 3D rows, visionOS only (Austin's PD D-061 defaults:
	// a 20 x 12 ft panel at 3.6 m, Stereo Depth 150 %, Convergence 25 ft,
	// dimming 80 %, Render Resolution 100 % of the 2560x1536 area budget,
	// readouts in feet - PDDefaults.m :139-152).
	static NSDictionary *fac;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSMutableDictionary *m = [[self factoryCommon] mutableCopy];
		[m addEntriesFromDictionary:@{
			GEDefaults3DDistance   : @3.6,
			GEDefaults3DHalfWidth  : @3.048,
			GEDefaults3DHalfHeight : @1.8288,
			GEDefaults3DPosHeight  : @0.0,
			GEDefaults3DDepthPct   : @150.0,
			GEDefaults3DConvergence: @762.0,
			GEDefaults3DDimming    : @0.80,
			GEDefaults3DRenderPct  : @100.0,
			GEDefaults3DUnitsFeet  : @YES,
		}];
		fac = [m copy];
	});
	return fac;
#else
	return [self factoryCommon];
#endif
}

+ (NSDictionary *)factoryCommon
{
	return @{
		// 2 = auto. A pad attached hides the overlay; unplug it and the
		// overlay comes back. The simulator reports a virtual "Gamepad", so
		// the gate forces this to 1 (`settings set Touch.Enabled 1`).
		GEDefaultsTouchEnabled     : @2,
		GEDefaultsButtonScale      : @1.0,
		// 0.28 is the recomp port's tuned value, and it goes with that port's
		// drawing: a stroked ring rather than a filled disc, so the game
		// underneath stays readable through the controls.
		GEDefaultsButtonOpacity    : @0.28,
		// The family's touch-look seed. The recomp port ships 0.4 deg/pt as a
		// tuned default after playtesting; this port starts at 0.25 because
		// GE's FOV is narrower than the remaster's, and the settings row is
		// what settles it. Fed as ABSOLUTE view degrees (hostinput.c), never
		// through the mouse sensitivity chain — that chain is a velocity
		// curve and it is what makes phone look feel like ice.
		GEDefaultsLookDegPerPoint  : @0.25,
		// Vertical is its own number, not a scale of the horizontal one: a
		// phone's usable vertical drag is about a third of its horizontal one
		// (the thumb arcs), and GE's pitch range is ±85° against 360° of yaw.
		// Two rows a player can reason about beat one row and a hidden ratio.
		GEDefaultsLookDegPerPointY : @0.25,
		GEDefaultsLookInvert       : @NO,
		// Hold Fire and slide that finger to aim. On so a one-thumb fire
		// also turns the view; the settings switch is how to turn it off.
		GEDefaultsFireDragLooks    : @YES,
		// A glass button gives no other confirmation that a press registered.
		GEDefaultsTouchHaptics     : @YES,
		// "" = the baked layout. See GETouchLayout.h for the schema; this is
		// the key the layout editor and the settings page save through.
		GEDefaultsTouchLayout      : @"",
		// The floating stick's own response, previously two constants in
		// GETouchOverlay.m. Same family curve as the pad and as the recomp
		// port's rex::ui::ApplyStickResponse: radial dead zone, rescale, then
		// `(1-curve)·n + curve·n³`. Curve 0 is linear, 1 is pure cubic.
		GEDefaultsStickDeadzone    : @0.15,
		GEDefaultsStickCurve       : @0.6,
		// Radial, not per-axis: a square dead zone lets a stick resting
		// slightly off-centre walk Bond diagonally for ever.
		GEDefaultsPadDeadzone      : @0.15,
		GEDefaultsPadLookDegPerSec : @160.0,
		GEDefaultsPadLookDegPerSecY: @160.0,
		GEDefaultsPadLookInvert    : @NO,
		// Round 10c / D-037: 0 = AUTO — the panel's own maximum, capped at 120
		// (GEPacing has the reasoning). An explicit 60 or 120 overrides it.
		GEDefaultsTargetFPS        : @0,
		// D-065 (round 16b). 1 = the CADisplayLink posts the engine's VI
		// retrace; 0 = the engine's own portTickThread nanosleep does, which
		// is what every build up to 0.0.0.7 did and which capped the phone at
		// 45-54 fps (D-063). Here rather than in ge007.ini on purpose: the
		// determinism gate diffs the ini and this key is host-side pacing,
		// nothing the sim can see. It is the A/B lever for D-065.
		GEDefaultsViPacer          : @0,   // Midas 9b: pcsched owns the VI clock (doc 16 §2.3)
		// Midas round 9c (R9B-3, docs/port-ios/18): ON. The display link is
		// the present clock (the donor's D-025), the gfx limiter is off on iOS
		// (gfx_pc.cpp) and maxfps=60 keeps the sim at 60. 0 = the 9b
		// lifecycle-only gate, the A/B lever.
		GEDefaultsCreditGate       : @1,
		// D-072 (round 17b). Where the repeat-present pump takes its deadline
		// from. 1 = the retrace grid (portVILastRetraceUs() + one tick), which
		// is the instant the next BUILD is released and therefore the instant
		// the pump must give the scheduler thread back. 0 = the round-16
		// estimate (the last build's PRESENT + one tick), which is always later
		// than that by the frame's own cost and is what parked the only
		// frame-building thread on a link callback for 8.3 ms -- D-068, 40
		// builds/s at target 120. The A/B lever for D-072; shell-side pacing, so
		// here and not in the ini the determinism gate diffs.
		GEDefaultsRepeatMode       : @1,
		// Round 22 (D-131). The clock-keeper A/B levers, all read once in
		// +[GEPacing start] (a relaunch applies them). docs/pacing.md §Round 22
		// has the table each value was measured into.
		//   SpinUs   - the scheduler busy-waits the last N us before the link
		//              callback it is waiting on, instead of sleeping through
		//              it, so its core is at clock when the build starts.
		//   RtPolicy - THREAD_TIME_CONSTRAINT_POLICY on the scheduler thread:
		//              1 = period 8.33 / computation 2 / constraint 4 ms,
		//              2 = 16.67 / 4 / 8 ms.
		//   PumpMode - the host main thread's idle wait: 0 = the run loop for
		//              Video.PumpWaitUs (D-090), 1 = a semaphore the link
		//              signals (no run-loop timer armed), 2 = a bare busy loop
		//              that pumps SDL every PumpWaitUs (a spin without the
		//              run-loop churn and its dispatch workers).
		GEDefaultsSpinUs           : @0,
		GEDefaultsRtPolicy         : @0,
		GEDefaultsPumpMode         : @0,
		//   WorkInterval - each build is an os_workgroup interval (the public
		//              AudioWorkIntervalCreate) whose deadline is the link
		//              callback that presents it, so the performance
		//              controller is told the deadline instead of guessing.
		//              DEFAULT 3 (D-131): a 3 ms deadline per build. Walking
		//              Dam at 120 it is the only lever that passes without a
		//              spin -- build 0.4-0.8 ms, late 0.2-0.3/s, ~22 % of a
		//              core, Nominal for 5 min -- and at 60 it HALVES the CPU
		//              (18 % vs 35-38 %): racing to idle at clock is cheaper
		//              than crawling through the build at a low one.
		//              Round 28 (doc 38 item 1): the switch and the deadline
		//              are two keys. WorkInterval 0 = off, 1 = on (default);
		//              a stored 2..16 is the round-22 form and still means
		//              "on, N ms" (it overrides the key below).
		//   WorkIntervalDeadlineMs - the deadline, N ms after the build's
		//              start (the sibling's D-131 shape: start + 3 ms).
		//              DEFAULT 3 = what rounds 22-27 shipped (WorkInterval=3).
		//              0 = the next link callback (round 22's WorkInterval=1).
		GEDefaultsWorkInterval     : @1,
		GEDefaultsWorkIntervalDeadlineMs : @3,
		//   RenderQos - round 23 (R23-5b): 1 = ge-render runs at
		//              QOS_CLASS_USER_INTERACTIVE. Off: on the simulator it
		//              doubled the render thread's frame in Dam with XBLA at
		//              120 (doc 32 §5). A device A/B lever.
		GEDefaultsRenderQos        : @0,
		//   SimThread - round 26 (doc 35): 1 = Midas's game loop (mainproc ->
		//              bossMainloop) runs on its own thread, ge-sim, which can
		//              join the frame work interval (the main thread never may,
		//              R23-1); the main thread keeps UIKit and its own run loop,
		//              and an engine lock keeps the two from running engine code
		//              at once. 0 = rounds 9-25 (the sim IS the main thread).
		//              Read once, at launch.
		//   SimPolicy - ge-sim's scheduling: 0 = default QoS, 1 = QOS_CLASS_
		//              USER_INTERACTIVE (the sibling's shipped Tick.ThreadQos=1
		//              on iOS, D-130), 2 = 1 plus the time-constraint policy of
		//              Pacing.RtPolicy=1 (8.33 / 2 / 4 ms).
		GEDefaultsSimThread        : @1,
		GEDefaultsSimPolicy        : @1,
		// D-089 (round 19a). The family's five-mode Other App Audio choice
		// (GEAudio.h has the table). 2 = Lower Other Audio, which is exactly
		// what SDL's own Playback default already does — Austin: "You're
		// supposed to do what my other ports do." Replaces the round-10d
		// Audio.MixWithOthers switch; +registerDefaults migrates it once.
		GEDefaultsAudioOtherApps   : @2,
		// D-067 (round 16b). 0 = leave the STAGE texture pool at the N64's own
		// 0x6DDD0 (439 KB). texLoad()'s pool-full arm is a silent corruption
		// -- it points the display list at another texture's bytes -- so this
		// is the A/B lever for "is the phone thrashing the pool?" without a
		// rebuild: `settings set Debug.TexPoolKB 878`, then load a stage.
		// Round 23 (R23-3). Max FPS lives in midas.ini (maxfps); these two
		// only say whether the panel default (120 on a >= 120 Hz panel,
		// Austin 2026-10-20) may still be applied: never once the player has
		// picked a value, and only once per install.
		GEDefaultsMaxFpsUser       : @0,
		GEDefaultsMaxFpsPanelDone  : @0,
		GEDefaultsUpscale2DDone    : @0,   // round 37
		GEDefaultsMpNamesOffDone   : @0,   // round 58
		GEDefaultsTexPoolKB        : @0,
	};
}

+ (void)registerDefaults
{
	NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
	// D-089 migration, BEFORE registering: the retired switch is no longer in
	// the factory, so objectForKey: here sees only a value a player actually
	// stored. ON meant "keep my music going" = Play Both; OFF meant the old
	// non-mixable default = Stop Other Audio (what that player has been
	// getting). Never set = the new default. Once, then the old key is gone.
	id old = [d objectForKey:GEDefaultsAudioMixOthers];
	if (old) {
		if ([d objectForKey:GEDefaultsAudioOtherApps] == nil) {
			const NSInteger mode = [old boolValue] ? 1 : 0;
			[d setInteger:mode forKey:GEDefaultsAudioOtherApps];
			GELog(@"[audio] migrated Audio.MixWithOthers=%d -> Audio.OtherAppAudio=%ld",
			      (int)[old boolValue], (long)mode);
		}
		[d removeObjectForKey:GEDefaultsAudioMixOthers];
	}
	[d registerDefaults:[self factory]];
	[self registerAbOverrides];
}

// Round 28 (doc 38): Documents/shell-ab.txt, one `Key=Value` per line, puts
// shell keys into the REGISTRATION domain at launch -- a phone A/B lever that
// needs only a file copy into Documents (the dev bridge is not always
// available on the phone, R27-1). Nothing is written to NSUserDefaults, so
// deleting the file and relaunching restores the factory value. A value the
// player (or `settings set`) STORED still wins over it. Unknown keys and `#`
// lines are ignored; what was applied is logged.
+ (void)registerAbOverrides
{
	NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
	NSString *path = [docs stringByAppendingPathComponent:@"shell-ab.txt"];
	NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
	if (text.length == 0) {
		return;
	}
	NSDictionary *fac = [self factory];
	NSMutableDictionary *reg = [NSMutableDictionary dictionary];
	for (NSString *raw in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
		NSString *line = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		NSRange eq = [line rangeOfString:@"="];
		if (line.length == 0 || [line hasPrefix:@"#"] || eq.location == NSNotFound) {
			continue;
		}
		NSString *k = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		NSString *v = [[line substringFromIndex:eq.location + 1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		id f = fac[k];
		if (!f) {
			GELog(@"[settings] shell-ab.txt: unknown key %@ ignored", k);
			continue;
		}
		if ([f isKindOfClass:NSString.class]) {
			reg[k] = v;
		} else if (strcmp([f objCType], @encode(double)) == 0) {
			reg[k] = @(v.doubleValue);
		} else {
			reg[k] = @(v.integerValue);
		}
		GELog(@"[settings] shell-ab.txt: %@=%@ (registration domain%@)", k, v,
		      [NSUserDefaults.standardUserDefaults persistentDomainForName:NSBundle.mainBundle.bundleIdentifier][k]
		          ? @"; a STORED value overrides it" : @"");
	}
	if (reg.count) {
		[NSUserDefaults.standardUserDefaults registerDefaults:reg];
	}
}

+ (double)doubleFor:(NSString *)key   { return [NSUserDefaults.standardUserDefaults doubleForKey:key]; }
+ (NSInteger)intFor:(NSString *)key   { return [NSUserDefaults.standardUserDefaults integerForKey:key]; }
+ (BOOL)boolFor:(NSString *)key       { return [NSUserDefaults.standardUserDefaults boolForKey:key]; }

+ (BOOL)setString:(NSString *)value forKey:(NSString *)key
{
	if (![self factory][key]) {
		return NO;
	}
	id fac = [self factory][key];
	NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
	if ([fac isKindOfClass:NSString.class]) {
		[d setObject:value forKey:key];
	} else if (strcmp([fac objCType], @encode(double)) == 0 || [fac isKindOfClass:NSDecimalNumber.class]) {
		[d setDouble:value.doubleValue forKey:key];
	} else {
		[d setInteger:value.integerValue forKey:key];
	}
	[d synchronize];
	return YES;
}

+ (NSString *)stringForKey:(NSString *)key
{
	id fac = [self factory][key];
	if (!fac) {
		return nil;
	}
	if ([fac isKindOfClass:NSString.class]) {
		return [NSUserDefaults.standardUserDefaults stringForKey:key] ?: @"";
	}
	return [NSString stringWithFormat:@"%g", [self doubleFor:key]];
}

+ (NSString *)factoryStringForKey:(NSString *)key
{
	id fac = [self factory][key];
	if (!fac) {
		return nil;
	}
	if ([fac isKindOfClass:NSString.class]) {
		return fac;
	}
	return [NSString stringWithFormat:@"%g", [fac doubleValue]];
}

+ (void)resetAll
{
	NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
	// Remove rather than re-write: the registration domain still holds the
	// factory value, so every read falls through to it and there is exactly
	// one definition of "default" in the process. Writing the factory value
	// back would make a later change to +factory invisible to anyone who had
	// ever pressed this button.
	for (NSString *k in [self factory]) {
		[d removeObjectForKey:k];
	}
	[d synchronize];
	NSLog(@"projectmidas: [settings] every shell setting reset to factory");
}

+ (NSDictionary<NSString *, NSString *> *)all
{
	NSMutableDictionary *out = [NSMutableDictionary dictionary];
	for (NSString *k in [self factory]) {
		out[k] = [self stringForKey:k];
	}
	return out;
}

@end
