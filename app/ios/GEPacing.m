// GEPacing.m — see GEPacing.h and docs/pacing.md.
#import "GEPacing.h"
#import "GESettings.h"
#import "GEVision.h"
#import "GEDefaults.h"
#import "GEController.h"
#import "GETouchOverlay.h"

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <stdatomic.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/workgroup.h>
#import <AudioToolbox/AudioWorkInterval.h>
#define SDL_MAIN_HANDLED 1
#include <SDL.h>

// app/gfx/gfx_angle_egl.mm
extern int geAngleSetSwapInterval(int interval);
// engine/port/src/video.c — repeat-present, turned on from the render thread
// once the pacer knows what the panel offers (docs/pacing.md §repeat-present R5).
extern void videoSetPresentHz(int hz);
extern int videoGetPresentHz(void);
// D-072: 1 = the repeat-present pump's deadline comes off the retrace grid.
extern void videoSetRepeatMode(int mode);
// engine/port/src/gesched.c — the sim-tick histogram since the last call
// (round 10c; docs/pacing.md §round 10c "what number to read").
extern void geTickTakeStats(unsigned *t0, unsigned *t1, unsigned *t2);
// engine/port/include/system.h — D-065, the VI retrace pacemaker. The engine's
// own portTickThread() posts the retrace the scheduler blocks on with a
// chunked relative nanosleep() on a default-QoS pthread; on this phone that
// delivers 45-54 retraces a second against an intended 60, and since the
// scheduler cannot build a frame until one arrives, THAT is the frame cap
// (D-063). The link below is the panel's own clock and cannot jitter, so it
// posts them instead.
extern void portSetExternalVIPacer(int on);
extern void portExternalVIPulse(void);

// Starts at 1: the first frames are drawn before the app has had a chance to
// tell anyone it is in the foreground, and a 0 here would eat them.
static atomic_int s_foreground = 1;

static dispatch_semaphore_t s_sem = nil;
// Guards "never bank more than one credit". A semaphore has no way to ask its
// value, so the count is tracked alongside it (docs/pacing.md §3.2).
static atomic_int s_credits = 0;
static atomic_int s_running = 0;

static atomic_uint s_linkCallbacks = 0;
static atomic_uint s_presents = 0;      // builds + repeats (every swap)
static atomic_uint s_builds = 0;        // presents of a NEWLY BUILT frame
static atomic_uint s_repeats = 0;       // presents of the last frame again
static atomic_uint s_timeouts = 0;

static double s_lastPresent = 0.0;
static double s_lastDelta = 0.0;

// Inter-present intervals, for the cadence assertion. A ring, because the
// gate samples ten seconds of them and allocating per frame is exactly the
// kind of thing a pacer must not do.
#define GE_PACE_RING 1024
static double s_ring[GE_PACE_RING];
static atomic_uint s_ringCount = 0;
static atomic_int  s_distResetPending = 0;   // bridge `pacing reset` -> render thread (D-054 review)

static int s_targetHz = 60;
static int s_swapIntervalDone = 0;
static int geMidasLinkHz(void);   // Midas round 9c, with geResolveTargetHz below

// Midas round 18 (Phase 3a, docs/port-ios/27-round18-120hz.md). The panel's
// maximum refresh, as every decision in this file sees it. GE_PANEL_HZ is a
// DEV-ONLY override (compiled out of GE_PUBLIC builds): the simulator reports
// a 60 Hz panel for every device, so the 120 Hz path - a 120 Hz sim, a present
// per tick, a 120 Hz link - can only be exercised, and its CPU budget
// measured, by pretending. ANGLE's swap does not block on iOS (doc 18 §2), so
// the simulator then really presents 120/s whatever the Mac screen shows.
static int s_panelOverride = 0;       // GE_PANEL_HZ in effect (dev builds only)
// GE_PANEL_HZ's second half: the simulator's display link is pinned at the
// Mac's 60 Hz however hard it is asked (`granted 16.67 ms` at target 120), so
// with the credit gate on, a 120 Hz sim would still present - and tick - 60
// times a second, and nothing about the 120 Hz budget could be measured. In
// override mode a timer of the pacer's own releases the credits at the target
// rate instead of the link. Dev builds only; never on a real panel.
static dispatch_source_t s_synthTimer = nil;
static void geReleaseCredit(void);
// Phase 6 M3 (round 47, docs/port-ios/67; perfect-dark-ios PDPacing.m D-050/
// D-056): in visionOS 3D the COMPOSITOR is the clock. While the immersive loop
// runs, the display link is PAUSED (by the loop, never by the mode
// transition - a Crown dismissal only ever reaches the loop) and the loop calls
// geIosPacingSignalExternal() once per compositor frame, right after
// cp_time_wait_until(optimal_input_time). That releases the SAME credit
// geIosPacingWaitForPresent blocks on - docs/pacing.md's one-wait rule with a
// different source, so ge-render is phase-locked to the compositor - and runs
// the link's per-frame chores (VI pulse, pad poll, overlay visibility). The
// compositor's period feeds round 36's grant path, so Max FPS: Auto and the
// sim tick follow the compositor's cadence; the pre-3D rate is put back on
// exit. Foreground must not un-pause the link while external is on (two
// clocks is the one thing docs/pacing.md forbids); a present wait under the
// external clock times out after 50 ms (a stalled compositor costs frames,
// never the session).
static atomic_int s_external = 0;
static atomic_uint s_extSignals = 0;
static atomic_uint s_extTimeouts = 0;
static double s_extHz = 0.0, s_extWin0 = 0.0;
static unsigned s_extWinN = 0;
static void geBeatPre(double dt);
static void geBeatPost(double dt);
static double s_synthNext = 0.0;

#if TARGET_OS_VISION
// Round 36 (docs/port-ios/48-round36-decal-flicker-adaptive-hz.md §2): the
// visionOS pacer adopts the rate visionOS GRANTS. Austin's 0.0.0.11 headset log
// (R35-2) shows the compositor handing a 90 Hz request 10 ms frames (100 Hz)
// for ~100 s; a 90 Hz sim on a 100 Hz panel holds some frames for two
// refreshes. The link's granted period (targetTimestamp - timestamp) is binned
// per callback to the nearest visionOS rate (60, 90, 96, 100, 120; within 3 %),
// and a 1 s window whose bins agree on one rate (>= 80 % of its callbacks) is a
// verdict. Two verdicts in a row for a rate other than the adopted one, at
// least 3 s after the last change, re-resolve the pacer to it: panel max (and
// so geIosFastHz: AUTO, the link range, the work-interval deadline, the Max FPS
// row), the engine's panel cap (videoSetPresentHz) and, unless the player picked
// a Max FPS, maxfps = the sim tick (GESettings +visionFollowRate:, the settings
// row's own live path). Only measured while the link asks for the adopted rate
// (a player's 60 is not a 60 Hz panel). 0 = not yet: GEVisionMaxFPS() (90).
static _Atomic int s_visionHz = 0;
static _Atomic int s_visionFakeGrantUs = 0;   // dev: bridge `pacing grant <ms>|off`
static double s_vwStart = 0.0;               // the window's first callback
static unsigned s_vwN = 0, s_vwBin[5];
static int s_vwCand = 0, s_vwRuns = 0;
static double s_vLastChange = 0.0;
static const int kGeVisionRates[5] = { 60, 90, 96, 100, 120 };
static void geVisionNoteGrant(double now, double grantMs);
#endif
int geIosPanelMaxHz(void)
{
#if !GE_PUBLIC
	static int env = -1;
	if (env < 0) {
		const char *e = getenv("GE_PANEL_HZ");
		int v = e ? atoi(e) : 0;
		env = (v >= 30 && v <= 240) ? v : 0;
	}
	if (env > 0) {
		s_panelOverride = env;
#if TARGET_OS_VISION
		// round 36: the override is the starting rate; a dev `pacing grant`
		// can still move it (geVisionNoteGrant), so the re-resolve is testable
		int v = atomic_load(&s_visionHz);
		return v > 0 ? v : env;
#else
		return env;
#endif
	}
#endif
#if TARGET_OS_VISION
	{
		int v = atomic_load(&s_visionHz);
		return v > 0 ? v : (int)GEVisionMaxFPS();
	}
#else
	return (int)GEVisionMaxFPS();
#endif
}

// Round 34 (M-V3, docs/port-ios/44-round34-visionos-90hz.md). The pacer's
// FAST arm: the rate it asks for when it does not ask for 60. Rounds 9c-33
// wrote this as a literal 120 behind a `panel >= 120` test, which was right on
// every iPhone (60 or 120) and wrong on the Vision Pro: a 90 Hz panel failed
// the test, the pacer asked for 60, and visionOS honours a 60 Hz request on its
// 90 Hz compositor as every second refresh = 45 presents/s (Austin's headset on
// 0.0.0.10). Now it is the panel's own maximum, clamped to 60..120: 60 and 120
// on iOS exactly as before, 90 on visionOS (GEVision.h), and whatever
// GE_PANEL_HZ says in a dev build.
int geIosFastHz(void)
{
	int panel = geIosPanelMaxHz();
	if (panel <= 60) {
		return 60;
	}
	return panel >= 120 ? 120 : panel;
}

// port/apple/tickprof.c (round 18): per-tick / per-frame cost distributions.
#include "../../port/apple/tickprof.h"
// engine: Midas's own option table (video.c) and the render-thread switch.
extern int portVideoOptionGet(const char *key, int *out);
extern int portRenderAsyncActive(void);
static int geEngineMaxFps(void)
{
	int v = 0;
	return portVideoOptionGet("maxfps", &v) ? v : 0;
}

// Round 10c. Austin's device report on 0.0.0.1, item 1: "the game stutters a
// lot" — on a 120 Hz ProMotion iPhone Air. The simulator is 60 Hz so it cannot
// reproduce it and cannot measure it, so the shell logs the numbers that tell
// the story instead, once a second, to the console AND to
// Documents/pacing.log (a device has no console the player can reach, but it
// has Files). docs/pacing.md §round 10c says how to read them.
static atomic_uint s_late = 0;        // an inter-present gap over 1.5 periods
static double s_maxGapMs = 0.0;       // the worst gap in the current second
static double s_reportStart = 0.0;
static unsigned s_reportBuilds = 0, s_reportPresents = 0, s_reportRepeats = 0;
static unsigned s_reportLinks = 0;
// D-065: retraces this pacer posted into the engine. `vi 60.0/s` with
// `builds 60.0/s` is the whole proof; `vi 60` with `builds 45` would mean the
// cap moved somewhere else.
static atomic_uint s_viPulses = 0;
static unsigned s_reportVi = 0;
static int s_creditGate = 1;          // Midas: Pacing.CreditGate (9c default 1; 0 = lifecycle gate only)
static int s_viPacer = 0;             // Pacing.ViPacer: 0 off, 1 at 60, 2 at the link rate
static int s_repeatMode = 1;          // Pacing.RepeatMode (D-072): 1 = retrace-grid deadline
static double s_viAccum = 0.0;        // seconds of link time owed to the next pulse

// Round 22 (D-131): the clock-keeper levers. Read once in +start.
static int s_spinUs = 0;              // Pacing.SpinUs: late spin before the awaited link callback
static int s_rtPolicy = 0;            // Pacing.RtPolicy: time-constraint policy on the scheduler
static int s_pumpMode = 0;            // Pacing.PumpMode: the host main thread's idle wait
static _Atomic double s_nextLinkTs = 0.0;   // link.targetTimestamp of the last callback
static dispatch_semaphore_t s_pumpSem = nil; // PumpMode 1: signalled by every link callback
static double s_spinSec = 0.0;        // render thread: seconds spent spinning this report window
static unsigned s_spinWaits = 0, s_spinHits = 0;   // waits that spun / that got their credit in the spin
static int s_rtApplied = -1;          // kern_return_t of the policy set, -1 = not tried
// Round 22: Pacing.WorkInterval -- the frame as an os_workgroup interval with a
// deadline (AudioWorkIntervalCreate, the public way to get one), so the
// performance controller is TOLD when the build must finish.
static int s_workInterval = 0;
static int s_wgDeadlineMs = 3;        // round 28: Pacing.WorkIntervalDeadlineMs (0 = next link callback)
static os_workgroup_interval_t s_wg = NULL;
static int s_wgState = 0;             // 0 untried, 1 joined, -1 failed, -2 skipped (main thread, R23-1)
static int s_wgOpen = 0;              // an interval is started and not yet finished
static unsigned s_wgErrors = 0;
static void geWgBeginBuild(double start);
// Round 26 (the sim thread section at the end of this file).
static atomic_int s_simWgState = 0;   // ge-sim's own join (t_wgJoined there)
static int geSimLine(char *buf, size_t n, double dt);
static int geRenderStatsLine(char *buf, size_t n, double dt);
// Round 22: the CPU and thermal columns, so a device arm measures its own cost
// without an Instruments trace alongside it.
static mach_port_t s_mainThreadPort = MACH_PORT_NULL;
static double s_cpuLastProc = 0.0, s_cpuLastMain = 0.0, s_cpuLastSched = 0.0;
static FILE *s_paceFile = NULL;
static unsigned s_paceLines = 0;
// One hour of once-a-second lines, then the file stops growing. The HEADER is
// what a device read needs most, so the cap keeps the beginning and drops the
// tail rather than the other way round.
#define GE_PACE_LINE_CAP 3600

// Round 11b. 0.0.0.2's log could not tell "the link is running at 60" from
// "the link is at 120 and repeat-present is off", because it reported neither
// the link's rate nor what iOS actually granted. Both now go in the line.
// `duration` is the display's nominal refresh period; targetTimestamp minus
// timestamp is the interval the system is actually handing this link, which is
// the number that says whether the ProMotion request was honoured.
static double s_linkDurMs = 0.0;
static double s_linkGrantMs = 0.0;
static float s_rangeMin = 0.0f, s_rangeMax = 0.0f, s_rangePref = 0.0f;

// Round 13c (D-054). `builds 50/s` with `sim_ticks 2+` says a frame missed its
// deadline; it does not say WHERE the time went. Three per-second
// distributions do, and they partition the render thread's second:
//
//   period_ms= from the moment the pacer released frame N-1 (i.e. the instant
//              before its swap) to the moment frame N is ready to present.
//              This is the FRAME PERIOD -- wall clock, sleep included. Round
//              16b (D-066) renamed it: it shipped as `build_ms`, and round 15a
//              read a p95 of 30 ms here as "the CPU is in the display-list
//              build". It is not, and it never was. The phone's frame period
//              was 17-21 ms while the game thread used 1.8 ms of CPU in it
//              (D-063) -- the rest was the thread ASLEEP waiting for a VI
//              retrace. A number that includes the wait can never say where
//              the work is.
//   build_ms = from videoStartFrame() -- the first thing the scheduler thread
//              does after the retrace wakes it -- to the moment the frame is
//              ready to present. No pacer wait and no retrace wait is inside
//              it, so this IS (near enough) the frame's CPU cost: sim tick,
//              display-list build, fast3d execute. This is the number to read
//              when asking "is the phone CPU-bound?".
//   swap_ms  = the time inside eglSwapBuffers alone, timed in
//              engine/port/fast3d/gfx_sdl2.cpp. eglSwapInterval is 0 here
//              (the display link is the clock), so this call only blocks when
//              the Metal queue is full -- i.e. it is the GPU back-pressure
//              term, and it is a SUBSET of build_ms.
//   wait_ms  = time blocked on the display-link semaphore. Headroom: a big
//              wait means the frame finished early and the panel is the
//              limiter.
//
// So: build p95 low + wait high = fine. build p95 high with swap p95 low =
// CPU (the display list / the sim). build p95 high and swap p95 high = the
// GPU or the driver. Samples are a fixed ring, never allocated, and the
// percentiles are computed once a second off the hot path.
#define GE_PACE_SAMPLES 256
typedef struct {
	double v[GE_PACE_SAMPLES];
	unsigned n;      // total noted this window (may exceed GE_PACE_SAMPLES)
} GEPaceDist;

static GEPaceDist s_periodDist, s_buildDist, s_swapDist, s_waitDist;
// Round 28 (doc 38 item 3): GL draw calls and triangles per frame, noted by
// fast3d's gfx_start_frame for the frame before it (render thread).
static GEPaceDist s_drawsDist, s_trisDist;
// Round 22: the clock probe. A fixed dependent-add chain timed at every build
// start, in us; its duration is inversely the core's clock at the instant the
// build begins (an E-core and a clocked-down P-core both read slow).
static GEPaceDist s_probeDist;
static void gePaceWriteLine(const char *line);
static double s_periodStart = 0.0;    // when the pacer last released a frame
static double s_frameStart = 0.0;     // D-066: when the engine began THIS frame
static unsigned s_slowBuilds = 0;     // build_ms over one sim tick (round 18: 8.33 ms at 120)

static inline void gePaceDistNote(GEPaceDist *d, double ms)
{
	// A bridge `pacing reset` lands here, on the owning thread, so the gate's
	// first reported second does not carry samples from before the reset.
	if (atomic_exchange(&s_distResetPending, 0)) {
		s_periodDist.n = s_buildDist.n = s_swapDist.n = s_waitDist.n = s_probeDist.n = 0;
		s_drawsDist.n = s_trisDist.n = 0;
	}
	// Past the ring, keep the FIRST 256 of the second rather than the last:
	// the percentile of a whole second is what is wanted, and a tail-biased
	// sample would follow whatever happened to be last.
	if (d->n < GE_PACE_SAMPLES) {
		d->v[d->n] = ms;
	}
	++d->n;
}

static int geCompareDouble(const void *a, const void *b);

// p in [0,100]. Returns -1 when the window had no samples.
static double gePaceDistPct(const GEPaceDist *d, int p, double *scratch)
{
	unsigned n = d->n < GE_PACE_SAMPLES ? d->n : GE_PACE_SAMPLES;
	if (n == 0) {
		return -1.0;
	}
	memcpy(scratch, d->v, n * sizeof(double));
	qsort(scratch, n, sizeof(double), geCompareDouble);
	unsigned i = (unsigned)((double)(n - 1) * (double)p / 100.0 + 0.5);
	return scratch[i];
}

// D-066. engine/port/src/video.c videoStartFrame(), i.e. the first thing the
// SCHEDULER thread does once the VI retrace has woken it. Everything between
// here and geIosPacingWaitForPresent() is the frame's own work, with no sleep
// in it -- which is what `build_ms` should have meant all along. Render
// (scheduler) thread only.
static inline uint64_t geSecToAbs(double sec)
{
	static double k = 0.0;
	if (k == 0.0) {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		k = 1e9 * (double)tb.denom / (double)tb.numer;
	}
	return (uint64_t)(sec * k);
}

// Round 23 (R23-3): the ini is loaded by the first frame (sync or render
// thread); the panel's Max FPS default is applied from the main run loop, the
// context a settings-row change runs in, never from inside the frame.
static void geMaxFpsDefaultOnce(void)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		dispatch_async(dispatch_get_main_queue(), ^{
			[GESettings applyPanelMaxFpsDefault];
			[GESettings apply2DDefaults];   // round 37
			[GESettings applyMpNamesDefault]; // round 58
#if TARGET_OS_VISION
			// Round 36: an Auto maxfps a previous session left at 100/120
			// starts again from the rate in effect now (90 until adopted).
			[GESettings visionFollowRate:geIosFastHz()];
#endif
		});
	});
}

void geIosPacingNoteFrameStart(void)
{
	// Round 22: the clock probe, before anything else touches the core.
	{
		uint64_t t0 = mach_absolute_time();
		uint64_t x = t0;
		for (int i = 0; i < 16384; i++) {
			__asm__ volatile("add %0, %0, #1" : "+r"(x));
		}
		uint64_t t1 = mach_absolute_time();
		static double usPerTick = 0.0;
		if (usPerTick == 0.0) {
			mach_timebase_info_data_t tb;
			mach_timebase_info(&tb);
			usPerTick = (double)tb.numer / (double)tb.denom / 1000.0;
		}
		gePaceDistNote(&s_probeDist, (double)(t1 - t0) * usPerTick);
	}
	const double start = CACurrentMediaTime();
	s_frameStart = start;

	geMaxFpsDefaultOnce();
	geWgBeginBuild(start);
}

// Round 22, Pacing.WorkInterval: each build is one interval whose deadline is
// the next link callback -- the one that presents it.
//
// Round 23 (R23-1, docs/port-ios/32 §1): NEVER on the main thread. In Midas the
// sim thread IS the main thread (bossMainloop runs under SDL's
// postFinishLaunch), and at 60 Hz it builds the frame itself, so round 22
// joined the MAIN thread to `ge.frame`. UIKit creates its own CA_CLIENT work
// interval on the main thread lazily, the first time a scroll view animates or
// a drag begins (_UIQOSManagedCommitsBegin -> _beginInterval ->
// work_interval_create); the kernel refuses a thread that is already in an
// interval, and UIKit throws NSInternalInconsistencyException "Failed to
// create work interval": Austin's 0.0.0.1 (drag) and 0.0.0.6 (status-bar
// scroll-to-top after Done) aborts. UIKit's own C callers (the update
// scheduler, sheet drags, scene geometry changes) cannot be caught, so the
// only sound fix is to leave the main thread's work interval to UIKit.
//
// Round 23 (item 5, R23-5, doc 34 §6 item 1): the interval now lives on the
// RENDER thread (ge-render, joined at its entry by geIosPacingRenderThreadEnter
// and started from geIosPacingNoteRenderStart), the thread that builds every
// frame at 120. Round 22's interval only ever started from videoStartFrame,
// which stops running once the render thread is on, so at 120 no deadline was
// ever given.
static __thread int t_wgJoined = 0;   // this thread: 0 untried, 1 joined, -1 failed, -2 main (skipped)
static __thread int t_isRenderThread = 0;   // ge-render (round 26: the interval's owner when it runs)
static __thread os_workgroup_join_token_s t_wgToken;
static unsigned s_wgStarts = 0;       // intervals started (pacing.log `wg state/errors/starts`)

static void geWgJoinThisThread(const char *who)
{
	if (!s_workInterval || t_wgJoined != 0) {
		return;
	}
	char line[160];
	if (pthread_main_np()) {
		t_wgJoined = -2;
		s_wgState = -2;
		snprintf(line, sizeof(line), "[pacing] frame work interval skipped on %s: main thread "
		         "(UIKit owns its work interval, R23-1)", who);
	} else {
		static dispatch_once_t once;
		dispatch_once(&once, ^{
			s_wg = AudioWorkIntervalCreate("ge.frame", OS_CLOCK_MACH_ABSOLUTE_TIME, NULL);
		});
		int rc = s_wg ? os_workgroup_join((os_workgroup_t)s_wg, &t_wgToken) : -1;
		t_wgJoined = (rc == 0) ? 1 : -1;
		s_wgState = t_wgJoined;
		snprintf(line, sizeof(line), "[pacing] frame work interval %s on %s (join rc=%d)",
		         rc == 0 ? "joined" : "FAILED", who, rc);
	}
	NSLog(@"projectmidas: %s", line);
	gePaceWriteLine(line);
}

// Round 28 (doc 38 item 1): `start` is the caller's own clock read, not
// s_frameStart, which ge-sim and ge-render both write (round 26).
static void geWgBeginBuild(double start)
{
	geWgJoinThisThread("the frame-start thread");
	if (t_wgJoined != 1) {
		return;
	}
	if (s_wgOpen) {
		(void)os_workgroup_interval_finish(s_wg, NULL);
		s_wgOpen = 0;
	}
	// Round 28: Pacing.WorkIntervalDeadlineMs N >= 1 = N ms after the build
	// starts (default 3, the sibling's D-131: a tight deadline asks the
	// performance controller for clock); 0 = the next link callback (the one
	// that presents this build), round 22's WorkInterval=1.
	double next = atomic_load_explicit(&s_nextLinkTs, memory_order_relaxed);
	if (s_wgDeadlineMs >= 1) {
		next = start + s_wgDeadlineMs * 1e-3;
	} else if (next < start + 0.002) {
		// Round 34: one frame of the fast arm (1/90 s on visionOS). A 60 Hz
		// panel keeps round 22's 1/120 s, so iOS does not move.
		const int fast = geIosFastHz();
		next = start + 1.0 / (double)(fast > 60 ? fast : 120);
	}
	if (os_workgroup_interval_start(s_wg, geSecToAbs(start), geSecToAbs(next), NULL) == 0) {
		s_wgOpen = 1;
		++s_wgStarts;
	} else {
		++s_wgErrors;
	}
}

// port/src/video.c portRenderThreadProc, GE_IOS (round 23, R23-5): first and
// last thing the render thread does. The sibling runs its frame threads at
// QOS_CLASS_USER_INTERACTIVE on iOS (engine/port/src/libultra.c Tick.ThreadQos,
// D-130: at default QoS they lost the P-cores, late 31-33/s at 120); Midas's
// ge-render is an SDL thread at default QoS, and stays so by default (below).
void geIosPacingRenderThreadEnter(void)
{
	// R23-5b: OFF by default. On the iPhone Air simulator, Dam with XBLA on
	// at 120, USER_INTERACTIVE on ge-render DOUBLED its frame (render p50
	// 4.7 -> 9.0 ms, presents 120 -> 111; A/B in one container, doc 32 §5),
	// and the phone's measured bottleneck is the sim tick on the MAIN thread,
	// which is already user-interactive. Pacing.RenderQos=1 turns it on for a
	// device A/B.
	const int want = (int)[GEDefaults intFor:GEDefaultsRenderQos];
	int qrc = -1;
	if (want) {
		qrc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	}
	char line[128];
	snprintf(line, sizeof(line), "[pacing] ge-render QoS %s rc=%d (Pacing.RenderQos %d)",
	         want ? "user-interactive" : "default (SDL)", qrc, want);
	NSLog(@"projectmidas: %s", line);
	gePaceWriteLine(line);
	t_isRenderThread = 1;
	geWgJoinThisThread("ge-render");
}

void geIosPacingRenderThreadExit(void)
{
	if (t_wgJoined == 1) {
		if (s_wgOpen) {
			(void)os_workgroup_interval_finish(s_wg, NULL);
			s_wgOpen = 0;
		}
		os_workgroup_leave((os_workgroup_t)s_wg, &t_wgToken);
		gePaceWriteLine("[pacing] frame work interval left (ge-render exits)");
	}
	t_wgJoined = 0;
	t_isRenderThread = 0;
	// Round 26: ge-sim stays joined; the column then reports its join.
	s_wgState = atomic_load_explicit(&s_simWgState, memory_order_relaxed);
}

// engine/port/fast3d/gfx_sdl2.cpp, around the iOS present. Render thread only.
// Midas round 18: port/fast3d/gfx_pc.cpp gfx_start_frame, on whichever thread
// renders. videoStartFrame() (geIosPacingNoteFrameStart) only runs when the
// sim thread renders itself; under Midas's render thread (auto-on whenever
// tickhz != 60, i.e. at 120) it never runs, `build` kept the last synchronous
// start and grew without bound. A few microseconds after videoStartFrame in
// the synchronous case, so the 60 Hz numbers do not move.
void geIosPacingNoteRenderStart(void)
{
	const double start = CACurrentMediaTime();
	s_frameStart = start;
	geMaxFpsDefaultOnce();
	// Round 23 (R23-5): the render thread's build is the interval. In the
	// synchronous case this is the thread that ran videoStartFrame, whose
	// geIosPacingNoteFrameStart already began it (round 26: that is ge-sim,
	// which may join, or the main thread, which geWgJoinThisThread refuses).
	if (t_isRenderThread) {
		geWgBeginBuild(start);
	}
}

void geIosPacingNoteDraws(uint32_t draws, uint32_t tris)
{
	gePaceDistNote(&s_drawsDist, (double)draws);
	gePaceDistNote(&s_trisDist, (double)tris);
}

void geIosPacingNoteSwap(double seconds)
{
	gePaceDistNote(&s_swapDist, seconds * 1000.0);
}

static void gePaceNote(double gapSeconds)
{
	double ms = gapSeconds * 1000.0;
	if (ms > s_maxGapMs) {
		s_maxGapMs = ms;
	}
	if (gapSeconds > 1.5 / (double)s_targetHz) {
		atomic_fetch_add(&s_late, 1u);
	}
}

// Documents/pacing.log: a device has no console the player can reach, but it
// has Files. Bounded (GE_PACE_LINE_CAP) so a long session cannot fill the
// container.
static void gePaceWriteLine(const char *line)
{
	if (!s_paceFile) {
		NSString *path = [NSSearchPathForDirectoriesInDomains(
			NSDocumentDirectory, NSUserDomainMask, YES).firstObject
			stringByAppendingPathComponent:@"pacing.log"];
		s_paceFile = fopen(path.fileSystemRepresentation, "w");
	}
	if (!s_paceFile || s_paceLines > GE_PACE_LINE_CAP) {
		return;
	}
	if (s_paceLines == GE_PACE_LINE_CAP) {
		fprintf(s_paceFile, "[pacing] log capped at %d lines\n", GE_PACE_LINE_CAP);
		++s_paceLines;
		fflush(s_paceFile);
		return;
	}
	++s_paceLines;
	fprintf(s_paceFile, "%s\n", line);
	fflush(s_paceFile);
}

// Round 11b: the FIRST line says what was asked for and what the device is,
// so a log Austin sends back is self-diagnosing without a console alongside it.
static void gePaceWriteHeader(void)
{
	char line[448];
	snprintf(line, sizeof(line),
		"[pacing] start target %d Hz  range %.0f/%.0f/%.0f (min/max/pref)  "
		"panel_max %d Hz%s  maxfps %d  tick_hz %d  low_power %d  present_hz %d  build %s  "
		"spin_us %d rt %d pump_mode %d work_interval %d wi_deadline_ms %d sim_thread %d sim_policy %d",
		s_targetHz, s_rangeMin, s_rangeMax, s_rangePref,
		geIosPanelMaxHz(), s_panelOverride ? " (GE_PANEL_HZ)" : "",
		geEngineMaxFps(), [GEPacing simHz],
		NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0,
		videoGetPresentHz(),
		[NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"] UTF8String] ?: "?",
		s_spinUs, s_rtPolicy, s_pumpMode, s_workInterval, s_wgDeadlineMs,
		geIosSimThreadWanted(), (int)[GEDefaults intFor:GEDefaultsSimPolicy]);
	NSLog(@"projectmidas: %s", line);
	gePaceWriteLine(line);
}

static double geThreadCpuSec(mach_port_t th)
{
	if (th == MACH_PORT_NULL) {
		return 0.0;
	}
	thread_basic_info_data_t info;
	mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
	if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) {
		return 0.0;
	}
	return info.user_time.seconds + info.user_time.microseconds * 1e-6
	     + info.system_time.seconds + info.system_time.microseconds * 1e-6;
}

// Called from the render thread at the end of every BUILT frame. Everything
// here is once a second and off the hot path.
static void gePaceReport(void)
{
	double now = CACurrentMediaTime();
	if (s_reportStart == 0.0) {
		s_reportStart = now;
		s_reportBuilds = atomic_load(&s_builds);
		s_reportPresents = atomic_load(&s_presents);
		s_reportRepeats = atomic_load(&s_repeats);
		s_reportLinks = atomic_load(&s_linkCallbacks);
		s_reportVi = atomic_load(&s_viPulses);
		return;
	}
	double dt = now - s_reportStart;
	if (dt < 1.0) {
		return;
	}

	unsigned b = atomic_load(&s_builds), p = atomic_load(&s_presents), r = atomic_load(&s_repeats);
	unsigned lk = atomic_load(&s_linkCallbacks);
	// +resetStats (the dev bridge's `pacing reset`) zeroes the counters under
	// us; without this the next window's rates are an unsigned wraparound and
	// the log reads "builds 4293728355.5/s". Re-baseline and skip one line.
	if (b < s_reportBuilds || p < s_reportPresents || r < s_reportRepeats || lk < s_reportLinks) {
		s_reportStart = now;
		s_reportBuilds = b; s_reportPresents = p; s_reportRepeats = r; s_reportLinks = lk;
		s_reportVi = atomic_load(&s_viPulses);
		atomic_store(&s_late, 0u);
		s_maxGapMs = 0.0;
		return;
	}
	unsigned late = atomic_exchange(&s_late, 0u);
	unsigned t0 = 0, t1 = 0, t2 = 0;
	geTickTakeStats(&t0, &t1, &t2);

	// D-054: where the second went. One scratch buffer, on the stack, sorted
	// three times a second at most.
	static double scratch[GE_PACE_SAMPLES];
	double pp50 = gePaceDistPct(&s_periodDist, 50, scratch);
	double pp95 = gePaceDistPct(&s_periodDist, 95, scratch);
	double bp50 = gePaceDistPct(&s_buildDist, 50, scratch);
	double bp95 = gePaceDistPct(&s_buildDist, 95, scratch);
	double sp50 = gePaceDistPct(&s_swapDist, 50, scratch);
	double sp95 = gePaceDistPct(&s_swapDist, 95, scratch);
	double wp50 = gePaceDistPct(&s_waitDist, 50, scratch);
	unsigned slow = s_slowBuilds;
	double probe50 = gePaceDistPct(&s_probeDist, 50, scratch);
	double probe95 = gePaceDistPct(&s_probeDist, 95, scratch);
	s_probeDist.n = 0;
	// Round 22: process / main-thread / scheduler-thread CPU over the window,
	// in % of one core, and the thermal state (0 nominal .. 3 critical).
	double cpuProc = 0.0, cpuMain = 0.0, cpuSched = 0.0;
	{
		struct rusage ru;
		getrusage(RUSAGE_SELF, &ru);
		double t = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
		cpuProc = (t - s_cpuLastProc) / dt * 100.0;
		s_cpuLastProc = t;
		double tm = geThreadCpuSec(s_mainThreadPort);
		cpuMain = (tm - s_cpuLastMain) / dt * 100.0;
		s_cpuLastMain = tm;
		mach_port_t self = mach_thread_self();
		double ts = geThreadCpuSec(self);
		mach_port_deallocate(mach_task_self(), self);
		cpuSched = (ts - s_cpuLastSched) / dt * 100.0;
		s_cpuLastSched = ts;
	}
	int thermal = (int)NSProcessInfo.processInfo.thermalState;
	double spinMs = s_spinSec * 1000.0 / dt;
	unsigned spinWaits = s_spinWaits, spinHits = s_spinHits;
	s_spinSec = 0.0; s_spinWaits = s_spinHits = 0;
	unsigned vi = atomic_load(&s_viPulses);
	unsigned viRate = vi - s_reportVi;
	s_reportVi = vi;
	s_periodDist.n = s_buildDist.n = s_swapDist.n = s_waitDist.n = 0;
	s_slowBuilds = 0;
	atomic_store(&s_distResetPending, 0);

	GeTickProf tp;
	geTickProfTake(&tp);
	char line[1024];
	snprintf(line, sizeof(line),
		"[pacing] panel_max %d maxfps %d tick_hz %d rt %d sim %.1f/s  "
		"tick_cpu %.2f/%.2f ms frame_cpu %.2f/%.2f ms render %.2f/%.2f ms  "
		"builds %.1f/s presents %.1f/s repeats %.1f/s link %.1f/s vi %.1f/s late %u "
		"max_gap %.1f ms  sim_ticks 0/1/2+ %u/%u/%u  target %d "
		"granted %.2f ms dur %.2f ms present_hz %d lpm %d  "
		"period %.2f/%.2f ms build %.2f/%.2f ms swap %.2f/%.2f ms wait %.2f ms slow %u  "
		"cpu %.0f%% main %.0f%% sched %.0f%% thermal %d spin %.1f ms/s %u/%u probe %.1f/%.1f us wg %d/%u/%u",
		geIosPanelMaxHz(), geEngineMaxFps(), [GEPacing simHz], portRenderAsyncActive() ? 1 : 0,
		(double)tp.ticks / dt,
		tp.tickP50, tp.tickP95, tp.frameP50, tp.frameP95, tp.renderP50, tp.renderP95,
		(b - s_reportBuilds) / dt, (p - s_reportPresents) / dt,
		(r - s_reportRepeats) / dt, (lk - s_reportLinks) / dt,
		(double)viRate / dt,
		late, s_maxGapMs, t0, t1, t2, s_targetHz,
		s_linkGrantMs, s_linkDurMs, videoGetPresentHz(),
		NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0,
		pp50, pp95, bp50, bp95, sp50, sp95, wp50, slow,
		cpuProc, cpuMain, cpuSched, thermal, spinMs, spinHits, spinWaits,
		probe50, probe95, s_wgState, s_wgErrors, s_wgStarts);
	{
		size_t used = strlen(line);
		(void)geSimLine(line + used, sizeof(line) - used, dt);
		used = strlen(line);
		(void)geRenderStatsLine(line + used, sizeof(line) - used, dt);
	}

	NSLog(@"projectmidas: %s", line);
	gePaceWriteLine(line);

	// Midas round 9c: the settings page's Max FPS moves the engine's tick;
	// follow it with the link (applyTargetFPS hops to the link's thread).
	if (geMidasLinkHz() != s_targetHz) {
		[GEPacing applyTargetFPS];
	}

	s_reportStart = now;
	s_reportBuilds = b;
	s_reportPresents = p;
	s_reportRepeats = r;
	s_reportLinks = lk;
	s_maxGapMs = 0.0;
}

// One place that turns Pacing.TargetFPS (0 = AUTO, 60, 120/90) plus the panel
// into the rate we ask for. Round 11b: +applyTargetFPS had its OWN copy of
// this and that copy did not know about AUTO — `want <= 0` fell through to 60,
// so choosing "Auto" on the settings page forced 60 on a 120 Hz phone while a
// cold start at the same setting chose 120.
static int geResolveTargetHz(void)
{
	NSInteger want = [GEDefaults intFor:GEDefaultsTargetFPS];
	if (want <= 0) {
		// D-068 (round 16b). AUTO used to mean "the panel's maximum, capped at
		// 120". Measured on Austin's iPhone Air, in Dam, at one position, with
		// the retrace already fixed (D-065):
		//
		//   target 120 -> builds 40/s, period p50 28 ms, late ~33/s
		//   target  60 -> builds 60/s, period p50 16.7 ms, late 0
		//
		// with build_ms (the frame's own CPU cost) IDENTICAL at ~6-12 ms in
		// both. The difference is the repeat-present pump: it blocks the
		// scheduler -- the only thread that builds frames -- on the next link
		// callback, up to 8.3 ms, and that 8.3 ms comes straight out of the
		// next frame's 16.67 ms. 8.3 + 12 > 16.67, so every other frame misses
		// its retrace and the period settles at 25 ms.
		//
		// At a HARD 60 the link asks ProMotion for 60 and gets it (`granted
		// 16.67 ms`), so D-037's original objection -- that a 60 fps stream on
		// a free-running 120 Hz panel alternates one- and two-refresh holds --
		// does not apply: the panel itself is at 60 and every frame occupies
		// exactly one refresh (`late 0`, max_gap 18-20 ms).
		//
		// 120 remains an explicit choice on the settings page, and it is the
		// right one again the moment the repeat can be made not to block the
		// build (its own thread, or a deadline taken from the retrace grid).
		//
		// Round 22 (D-135): it is. D-072 took the repeat off the build's path;
		// what still broke 120 while walking was the CPU clock falling in the
		// idle wait (M-047/M-048), and Pacing.WorkInterval (D-131) fixes that
		// by telling the performance controller each build's deadline. Walking
		// Dam on the iPhone Air: 119.6-119.8 presents/s, late 0.2-0.3/s, build
		// 0.4-0.8 ms, the process at ~22 % of a core and Nominal thermal for 5
		// minutes (M-060..M-062). So AUTO is the panel's own maximum again,
		// 120 on a ProMotion iPhone and 60 everywhere else; 60 stays a choice.
		// Round 34: "the panel's maximum" is geIosFastHz(), so 90 on the
		// Vision Pro's 90 Hz panel (a hard 60 there is 45 presents/s).
		return geIosFastHz();
	}
	// An explicit fast pick (120, or 90 on visionOS) gets the fast arm when
	// the panel has one; anything else is 60.
	const int fast = geIosFastHz();
	return (fast > 60 && want >= fast) ? fast : 60;
}

// Midas round 9c (R9B-3, docs/port-ios/18). Midas has no repeat-present: one
// present per sim tick, and the sim tick follows midas.ini maxfps (video.c
// videoCoupleTickToMaxFps), capped by the engine at the panel rate this file
// hands it (videoSetPresentHz). So the link runs at the TICK rate, never above
// it: a 120 Hz link under a 60 Hz sim would bank a credit every other callback
// and the credit gate would stop phase-locking the present to the panel. At
// the default maxfps=60 that is a hard 60 on every iPhone, which is the
// donor's D-068 observation: ProMotion then runs the panel at 60 and every
// frame holds exactly one refresh.
extern int portTickHz(void);   // src/game/frametiming.c (s32)
static int geMidasLinkHz(void)
{
	int hz = geResolveTargetHz();
	int tick = portTickHz();
	if (tick >= 60 && tick < hz) {
		hz = tick;
	}
	return hz;
}

static int gePanelHz(void)
{
	return geIosFastHz();   // round 34: 90 on visionOS, 60/120 on iOS as before
}

@implementation GEPacing

static CADisplayLink *sLink = nil;
static NSThread *sThread = nil;

// Round 11b (D-043). The range is HARD at both rates: min == max == preferred.
// 0.0.0.2 asked for (60, 120, 120) — D-029's reasoning was that a thermally
// throttled phone falling back to 60 is a correct outcome rather than a broken
// promise — and Austin's iPhone Air delivered a flat 60. A range is a
// CONSTRAINT, not a preference: with 60 admissible, iOS is free to settle
// there for power and it does. Both working siblings ask for a hard range and
// both get the panel rate: perfect-dark-ios `PDPacing.m` -applyLinkRate
// (CAFrameRateRangeMake(want, want, want), D-056) and the GoldenEye XBLA
// recomp (patches/rexglue/0053-display-linked-pacing.patch, max_fps three
// times). Thermal fallback is not given up: iOS drops a link below its range
// under real thermal pressure anyway, and `Pacing.TargetFPS = 60` is the
// explicit lever. PD's other lesson is kept — the range is only ever set on
// the LINK'S OWN THREAD (PDPacing D-056: retuning from another thread froze
// the link), which is what +retuneLink is for.
static void geApplyRangeToLink(CADisplayLink *link, int hz)
{
	// Round 34: the fast arm is the panel's maximum (90 on visionOS), not a
	// literal 120; any other rate (a tick below it) is 60, as before.
	const int fast = geIosFastHz();
	float f = (fast > 60 && hz >= fast) ? (float)fast : 60.0f;
	s_rangeMin = s_rangeMax = s_rangePref = f;
	link.preferredFrameRateRange = CAFrameRateRangeMake(f, f, f);
}

+ (void)start
{
	if (atomic_load(&s_running)) {
		return;
	}

	s_sem = dispatch_semaphore_create(0);

	// Round 9c: 120 IS offered now. D-020 said the engine could not present
	// faster than it ticks; D-027 corrected the reason and repeat-present is
	// the answer — the extra callbacks present the LAST FINISHED FRAME again
	// (a blit and a swap, no game code), while the sim stays at exactly 60
	// ticks/s. Building an extra frame instead would re-run the tick and fire
	// guns twice (D-027). docs/pacing.md §repeat-present R5.
	// Round 10c / D-037. `Pacing.TargetFPS` is now 0 = AUTO by default, and
	// auto means "whatever the panel offers, up to 120". 0.0.0.1 shipped a
	// hard 60, and a hard 60 on a 120 Hz ProMotion panel is the worst of the
	// three choices available: eglSwapInterval is 0 (the link is the clock),
	// so a built frame reaches the compositor whenever it happens to be
	// finished and is then held for either ONE or TWO of the panel's 8.3 ms
	// refreshes depending on which side of a refresh boundary the build
	// landed. That alternation is a 60 fps counter and visible judder at the
	// same time, which is exactly Austin's "stutters a lot" against round 9a's
	// measured "59.9 fps, cadence even". At 120 every callback presents —
	// the extra ones are D-029 repeats of the last finished frame, running no
	// game code — so each built frame occupies exactly two refreshes and the
	// cadence is deterministic. The sim stays at 60 ticks/s either way.
	// An explicit 60 or 120 in Pacing.TargetFPS still wins; 60 is the thing to
	// try if the phone gets hot.
	geTickProfEnable(1);   // round 18: tick_cpu / frame_cpu / render columns
	s_targetHz = geMidasLinkHz();
	// Midas round 9c: the engine caps its maxfps-coupled tick (= its present
	// rate) at the panel. Before geEngineMain, so the ini load sees it.
	videoSetPresentHz(gePanelHz());

	// D-065. Hand the engine's VI retrace to the link below. Armed before the
	// link starts so the engine's tick thread is already out of the way by the
	// first callback; the engine keeps a 10 Hz watchdog of its own for the
	// window where the app is backgrounded and the link is paused.
	// Midas: Pacing.ViPacer defaults to 0 (GEDefaults.m) and the engine's
	// portSetExternalVIPacer is a logged no-op (doc 16 §2.3): pcsched.c owns
	// the VI clock until round 9c decides otherwise.
	s_viPacer = (int)[GEDefaults intFor:GEDefaultsViPacer];
	if (s_viPacer < 0 || s_viPacer > 2) {
		s_viPacer = 0;
	}
	s_creditGate = (int)[GEDefaults intFor:GEDefaultsCreditGate] ? 1 : 0;
	portSetExternalVIPacer(s_viPacer ? 1 : 0);

	// D-072. Two halves, and the pump needs both: the engine-side deadline
	// (videoSetRepeatMode) and the ORDER below, in which this callback posts
	// the retrace before it releases the present gate. Read once here so the
	// A/B is one key.
	s_repeatMode = (int)[GEDefaults intFor:GEDefaultsRepeatMode] ? 1 : 0;
	videoSetRepeatMode(s_repeatMode);

	// Round 22 (D-131): the clock-keeper levers.
	s_spinUs = (int)[GEDefaults intFor:GEDefaultsSpinUs];
	if (s_spinUs < 0 || s_spinUs > 20000) {
		s_spinUs = 0;
	}
	s_rtPolicy = (int)[GEDefaults intFor:GEDefaultsRtPolicy];
	if (s_rtPolicy < 0 || s_rtPolicy > 2) {
		s_rtPolicy = 0;
	}
	s_pumpMode = (int)[GEDefaults intFor:GEDefaultsPumpMode];
	if (s_pumpMode < 0 || s_pumpMode > 2) {
		s_pumpMode = 0;
	}
	s_workInterval = (int)[GEDefaults intFor:GEDefaultsWorkInterval];
	if (s_workInterval < 0 || s_workInterval > 16) {
		s_workInterval = 0;
	}
	// Round 28: the deadline is its own key; a stored round-22 value 2..16 of
	// Pacing.WorkInterval still means "on, N ms" and wins.
	s_wgDeadlineMs = (int)[GEDefaults intFor:GEDefaultsWorkIntervalDeadlineMs];
	if (s_wgDeadlineMs < 0 || s_wgDeadlineMs > 16) {
		s_wgDeadlineMs = 3;
	}
	if (s_workInterval >= 2) {
		s_wgDeadlineMs = s_workInterval;
	}
	s_pumpSem = dispatch_semaphore_create(0);
	s_mainThreadPort = pthread_mach_thread_np(pthread_self());

	atomic_store(&s_running, 1);

#if !GE_PUBLIC
	if (s_panelOverride && s_creditGate) {
		dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
		s_synthTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
		dispatch_source_set_timer(s_synthTimer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 1000, 0);
		dispatch_source_set_event_handler(s_synthTimer, ^{
			const double now = CACurrentMediaTime();
			const double period = 1.0 / (double)(s_targetHz > 0 ? s_targetHz : 60);
			if (s_synthNext == 0.0 || now - s_synthNext > 0.25) {
				s_synthNext = now;
			}
			if (now >= s_synthNext) {
				s_synthNext += period;
				geReleaseCredit();
			}
		});
		dispatch_resume(s_synthTimer);
		NSLog(@"projectmidas: [pacing] GE_PANEL_HZ=%d: credits from a %d Hz timer, not the display link (dev)",
		      s_panelOverride, s_targetHz);
	}
#endif

	sThread = [[NSThread alloc] initWithTarget:self selector:@selector(linkThread) object:nil];
	sThread.name = @"ge.pacing";
	sThread.qualityOfService = NSQualityOfServiceUserInteractive;
	[sThread start];

	NSLog(@"projectmidas: [pacing] display link starting at %d Hz (panel max %d, engine %d)",
	      s_targetHz, geIosPanelMaxHz(), [GEPacing engineHz]);
}

+ (void)linkThread
{
	@autoreleasepool {
		sLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
		geApplyRangeToLink(sLink, s_targetHz);
		[sLink addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
		gePaceWriteHeader();
	}
	// Its own run loop: the main thread spends most of a frame inside SDL's
	// sysSleep(8) pump, and a link scheduled there is delivered late and
	// irregularly (docs/pacing.md §3.2).
	while (atomic_load(&s_running)) {
		@autoreleasepool {
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
		}
	}
}

static void geReleaseCredit(void)
{
	// Never bank credit. A hitch costs one frame; it must not produce a burst
	// of presents with no wait between them.
	if (atomic_load_explicit(&s_credits, memory_order_relaxed) < 1) {
		atomic_fetch_add(&s_credits, 1);
		dispatch_semaphore_signal(s_sem);
	}
}

+ (void)tick:(CADisplayLink *)link
{
	atomic_fetch_add(&s_linkCallbacks, 1u);

	// What iOS actually GRANTED this link, which is the number 0.0.0.2's log
	// was missing: `duration` is the display's nominal refresh period and
	// targetTimestamp - timestamp is the interval the system is handing us.
	// 8.33 ms means the ProMotion request was honoured; 16.67 ms means it was
	// not, whatever the range says.
	s_linkDurMs = link.duration * 1000.0;
	s_linkGrantMs = (link.targetTimestamp - link.timestamp) * 1000.0;
#if TARGET_OS_VISION
	geVisionNoteGrant(link.timestamp, s_linkGrantMs);
#endif
	// Round 22: when the NEXT callback is due, for the scheduler's late spin.
	atomic_store_explicit(&s_nextLinkTs, link.targetTimestamp, memory_order_relaxed);
	if (s_pumpMode == 1 && s_pumpSem) {
		dispatch_semaphore_signal(s_pumpSem);
	}

	// D-072: the credit is released at the END of this callback when the
	// repeat-present pump is on the retrace grid -- see below.
	if (!s_repeatMode && !s_synthTimer) {
		geReleaseCredit();
	}

	// Poll the pad here rather than on a timer: this is the app's real frame
	// boundary, and integrating the right stick's degrees-per-second against
	// the link's own delta is what keeps look speed identical at 60 and 120.
	static double last = 0.0;
	double now = CACurrentMediaTime();
	double dt = (last > 0.0) ? (now - last) : (1.0 / (double)s_targetHz);
	last = now;
	if (dt > 0.25) {
		dt = 1.0 / (double)s_targetHz;   // came back from a stall; do not lurch
	}

	geBeatPre(dt);

	// D-072 (round 17b). The retrace goes out BEFORE the present gate opens.
	//
	// Both happen in this one callback, and the order decides whether
	// portVIRetracePending() can be believed. Released first, the scheduler
	// thread wakes, finishes its present and asks the repeat-present pump
	// whether a build is already due -- and with the pulse still a few lines
	// away the honest answer "no" is wrong by microseconds. The pump then
	// waits for the NEXT callback, 8.3 ms of the only thread that builds
	// frames, and the build starts a whole callback late: D-068's 40 builds/s
	// at target 120, and the reason D-065b "moved no number on its own".
	// Posting the pulse first costs nothing and makes the flag true whenever
	// it should be.
	if (s_repeatMode && !s_synthTimer) {
		geReleaseCredit();
	}

	geBeatPost(dt);
}

// The link callback's per-frame chores, shared with the compositor clock
// (Phase 6 M3): before the credit, the VI retrace; after it, the pad and the
// overlay. Whichever clock is live calls them; the other is paused.
static void geBeatPre(double dt)
{
	s_lastDelta = dt;
	// D-065: the VI retrace, on the panel's clock.
	//
	// The engine's scheduler thread cannot begin a frame until a retrace
	// arrives, so whoever posts the retrace sets the frame rate. Posting it
	// here rather than from portTickThread()'s nanosleep loop is the whole
	// fix: this callback is delivered by the display server against the
	// panel's own refresh and does not jitter.
	//
	// The accumulator (rather than "every second callback") is what makes it
	// right at both link rates and across a dropped callback: 8.33 ms twice
	// is one pulse, 16.67 ms once is one pulse, and a 25 ms hiccup pays the
	// debt back over the next two callbacks instead of dropping a frame. The
	// clamp is what stops it paying back MORE than one frame at a time, which
	// would be a burst of sim ticks.
	//
	// The rate is the SIM rate, 60, not the link rate: gesched.c counts whole
	// 1/60 s ticks (D-019) and the extra presents on a 120 Hz panel are
	// D-029 repeats of the last finished frame, which run no game code.
	// `Pacing.ViPacer = 2` pulses at the link rate instead — the A/B for
	// "what would a true 120 builds/s cost?", not a shipping mode.
	if (s_viPacer) {
		const double viPeriod = (s_viPacer >= 2 && s_targetHz > 0)
			? 1.0 / (double)s_targetHz
			: 1.0 / 60.0;
		s_viAccum += dt;
		if (s_viAccum >= viPeriod - 0.002) {
			s_viAccum -= viPeriod;
			if (s_viAccum > viPeriod) {
				s_viAccum = viPeriod;   // never owe more than one frame
			} else if (s_viAccum < 0.0) {
				s_viAccum = 0.0;
			}
			atomic_fetch_add(&s_viPulses, 1u);
			portExternalVIPulse();
		}
	}

}

static void geBeatPost(double dt)
{
	[GEController pollWithDelta:dt];

	// Cheap and idempotent; this is the only regular main-ish heartbeat the
	// shell has, and the overlay's `auto` mode has to notice a pad arriving.
	static int visEvery = 0;
	if ((++visEvery % 30) == 0) {
		dispatch_async(dispatch_get_main_queue(), ^{ [GETouchOverlay applyVisibility]; });
	}
}

+ (void)applyTargetFPS
{
	int panel = geIosPanelMaxHz();
	int hz = geMidasLinkHz();
	if (hz == s_targetHz) {
		return;
	}
	s_targetHz = hz;

	// The link object belongs to the thread that made it and runs on that
	// thread's run loop; setting a property from here would be the one race
	// this file has spent a round not having.
	if (sThread && sLink) {
		[self performSelector:@selector(retuneLink) onThread:sThread withObject:nil waitUntilDone:NO];
	}
	// Safe from this thread by construction: videoSetPresentHz() defers the
	// GL work (a framebuffer create) to videoStartFrame() on the render
	// thread precisely because it is called from the host thread at init.
	NSLog(@"projectmidas: [pacing] target now %d Hz (panel max %d)", s_targetHz, panel);
}

+ (void)retuneLink
{
	geApplyRangeToLink(sLink, s_targetHz);
}

#if TARGET_OS_VISION
// Round 36: see s_visionHz. Link thread.
static void geVisionNoteGrant(double now, double grantMs)
{
	double ms = grantMs;
	const int fake = atomic_load(&s_visionFakeGrantUs);
	if (fake > 0) {
		ms = fake / 1000.0;
	} else if (s_panelOverride) {
		return;   // GE_PANEL_HZ (dev): the simulator's own link says nothing
	}
	const int cur = geIosPanelMaxHz();
	if (s_targetHz != cur || ms <= 0.0) {
		s_vwStart = 0.0;   // asking for less than the panel (or settling): no verdict
		return;
	}
	if (s_vwStart == 0.0) {
		s_vwStart = now;
		s_vwN = 0;
		memset(s_vwBin, 0, sizeof(s_vwBin));
	}
	const double hz = 1000.0 / ms;
	for (int i = 0; i < 5; i++) {
		if (fabs(hz - kGeVisionRates[i]) <= 0.03 * kGeVisionRates[i]) {
			s_vwBin[i]++;
			break;
		}
	}
	s_vwN++;
	if (now - s_vwStart < 1.0) {
		return;
	}
	int verdict = 0;
	for (int i = 0; i < 5; i++) {
		if (s_vwN > 0 && s_vwBin[i] * 5u >= s_vwN * 4u) {
			verdict = kGeVisionRates[i];
		}
	}
	const unsigned n = s_vwN;
	s_vwStart = 0.0;
	if (verdict == 0 || verdict == cur) {
		s_vwCand = 0;
		s_vwRuns = 0;
		return;
	}
	s_vwRuns = (verdict == s_vwCand) ? s_vwRuns + 1 : 1;
	s_vwCand = verdict;
	if (s_vwRuns < 2 || (s_vLastChange > 0.0 && now - s_vLastChange < 3.0)) {
		return;
	}
	s_vwCand = 0;
	s_vwRuns = 0;
	s_vLastChange = now;
	atomic_store(&s_visionHz, verdict);
	char line[256];
	snprintf(line, sizeof(line),
	         "[pacing] vision rate: granted %.2f ms (%u callbacks, 2 windows%s) -> panel %d -> %d Hz",
	         ms, n, fake > 0 ? ", dev pacing grant" : "", cur, verdict);
	NSLog(@"projectmidas: %s", line);
	const int to = verdict;
	NSString *msg = @(line);
	dispatch_async(dispatch_get_main_queue(), ^{
		gePaceWriteLine(msg.UTF8String);
		videoSetPresentHz(to);              // the engine's cap on the maxfps tick
		[GESettings visionFollowRate:to];  // maxfps (AUTO) / re-couple (a pick)
		[GEPacing applyTargetFPS];          // link range, on the link's thread
	});
}

+ (void)visionFakeGrantMs:(double)ms
{
	atomic_store(&s_visionFakeGrantUs, ms > 0.0 ? (int)lround(ms * 1000.0) : 0);
}

// Phase 6 M3: the rate adopted before 3D, put back when the link is the clock
// again (a compositor-adopted 60 would otherwise stick: the grant path only
// measures while the link asks for the adopted rate, and a link asked for 60
// is granted 60).
static int s_visionHzBefore3d = -1;
static void geVisionRestoreAfter3d(void)
{
	if (s_visionHzBefore3d < 0) {
		return;
	}
	const int was = atomic_load(&s_visionHz);
	atomic_store(&s_visionHz, s_visionHzBefore3d);
	s_visionHzBefore3d = -1;
	s_vwStart = 0.0;
	s_vwCand = 0;
	s_vwRuns = 0;
	s_vLastChange = CACurrentMediaTime();
	const int to = geIosPanelMaxHz();
	NSLog(@"projectmidas: [pacing] 3D over: vision rate %d -> panel %d Hz (the pre-3D rate)", was, to);
	dispatch_async(dispatch_get_main_queue(), ^{
		videoSetPresentHz(to);
		[GESettings visionFollowRate:to];
		[GEPacing applyTargetFPS];
	});
}
#endif

+ (void)applyLinkPause
{
	// On the link's own thread: the link is paused while the compositor is
	// the clock or the app is in the background, and running otherwise.
	if (sLink) {
		sLink.paused = (atomic_load(&s_external) || !atomic_load(&s_foreground)) ? YES : NO;
	}
}

+ (int)targetHz { return s_targetHz; }

+ (int)engineHz
{
	// What the engine can actually PRESENT. Round 9c changed this from 60 to
	// "whatever the link is asking for", because repeat-present decouples the
	// present rate from the sim rate: an extra callback re-presents the last
	// finished frame. The SIM rate is a separate number and is reported as
	// pacing_sim_hz below — it is always 60 and must stay 60.
	return s_targetHz;
}

+ (int)simHz
{
	// Midas: maxfps drives the tick (videoCoupleTickToMaxFps), so a 120 Hz
	// panel with maxfps=120 really simulates at 120 (round 18).
	extern int portTickHz(void);
	return portTickHz();
}

+ (void)resetStats
{
	atomic_store(&s_linkCallbacks, 0u);
	atomic_store(&s_presents, 0u);
	atomic_store(&s_builds, 0u);
	atomic_store(&s_repeats, 0u);
	atomic_store(&s_timeouts, 0u);
	atomic_store(&s_ringCount, 0u);
	// The distribution rings are owned by the render thread (D-054); ask it
	// to clear them at its next report instead of racing its writes here.
	atomic_store(&s_distResetPending, 1);
	atomic_store(&s_viPulses, 0u);
	s_periodStart = 0.0;
	s_frameStart = 0.0;
}

+ (double)lastFrameDelta { return s_lastDelta; }

static int geCompareDouble(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

+ (NSString *)statsDescription
{
	unsigned n = atomic_load(&s_ringCount);
	unsigned have = (n < GE_PACE_RING) ? n : GE_PACE_RING;
	double p50 = 0, p95 = 0, jitter = 0;

	if (have >= 8) {
		double *copy = malloc(sizeof(double) * have);
		memcpy(copy, s_ring, sizeof(double) * have);
		qsort(copy, have, sizeof(double), geCompareDouble);
		p50 = copy[have / 2];
		p95 = copy[(have * 95) / 100];
		jitter = (p50 > 0.0) ? ((p95 - p50) / p50 * 100.0) : 0.0;
		free(copy);
	}

	// "even" is the assertion the gate makes. 20 % of the median is well
	// inside a dropped frame (which would be 100 %) and well outside the
	// noise a healthy link produces.
	const char *cadence = (have < 8) ? "unknown" : (jitter < 20.0 ? "even" : "uneven");

	return [NSString stringWithFormat:
		@"pacing_target=%d\npacing_engine_hz=%d\npacing_sim_hz=%d\n"
		@"link_callbacks=%u\npresents=%u\nbuilds=%u\nrepeats=%u\n"
		@"timeouts=%u\nframe_ms_p50=%.2f\nframe_ms_p95=%.2f\nframe_ms_jitter_pct=%.1f\n"
		@"frame_cadence=%s\nsamples=%u\npanel_max_hz=%d\n"
		// Round 11b: the same self-diagnosis the log line carries, on demand.
		@"link_granted_ms=%.2f\nlink_duration_ms=%.2f\n"
		@"range_min=%.0f\nrange_max=%.0f\nrange_pref=%.0f\n"
		@"present_hz=%d\nlow_power=%d\nvision_rate=%d\nvision_fake_grant_ms=%.2f\n"
		@"pacing_mode=%s\npacing_external_signals=%u\npacing_ext_timeouts=%u\npacing_ext_hz=%.1f\n",
		s_targetHz, [GEPacing engineHz], [GEPacing simHz],
		atomic_load(&s_linkCallbacks), atomic_load(&s_presents),
		atomic_load(&s_builds), atomic_load(&s_repeats), atomic_load(&s_timeouts),
		p50 * 1000.0, p95 * 1000.0, jitter, cadence, have,
		geIosPanelMaxHz(),
		s_linkGrantMs, s_linkDurMs, s_rangeMin, s_rangeMax, s_rangePref,
		videoGetPresentHz(),
		NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0,
#if TARGET_OS_VISION
		atomic_load(&s_visionHz), atomic_load(&s_visionFakeGrantUs) / 1000.0,
#else
		0, 0.0,
#endif
		geIosPacingModeName(), atomic_load(&s_extSignals), atomic_load(&s_extTimeouts), s_extHz];
}

@end

#pragma mark - the render-thread half

// Round 22 (D-131), Pacing.SpinUs. Take one present credit, waiting at most
// timeoutSec. 0 = got it (dispatch_semaphore_wait's convention).
//
// SpinUs 0 is the plain semaphore wait every build before round 22 did. With
// SpinUs N the thread SLEEPS until N us before the link callback it is
// waiting on (the last callback's targetTimestamp) and then busy-waits on the
// credit count. The point is not latency: round 21b (M-048) showed that any
// idle sleep lets the CPU cluster's clock fall, so the build that starts at
// the callback runs ~2.5x slower than at clock; a hot core at the callback is
// what the 1.35-core main-thread spin was buying. The spin is bounded at 3N
// past its start and by the caller's own deadline, so a link that stops
// (background) or a stale target costs one bounded spin, never a hang.
// Round 23 (item 4, R23-4): a shell page is up (Settings). The main thread is
// Midas's sim thread, and at 60 Hz it waits for its present credit on a
// semaphore -- a wait in which UIKit gets nothing, so the page's touches,
// scroll updates and commits were serviced only in SDL's two 2 us pump runs
// once a frame. While a page is up, the main thread waits in the run loop
// instead, in short slices, polling the same credit: the game keeps its rate,
// UIKit gets the rest of the frame.
static atomic_int s_uiActive = 0;

void geIosPacingSetUiActive(int on)
{
#if !GE_PUBLIC
	const char *e = getenv("GE_UIWAIT");   // dev A/B: GE_UIWAIT=0 keeps the old wait
	if (e && atoi(e) == 0) {
		on = 0;
	}
#endif
	int was = atomic_exchange_explicit(&s_uiActive, on ? 1 : 0, memory_order_relaxed);
	if (was != (on ? 1 : 0)) {
		NSLog(@"projectmidas: [pacing] shell page %s: main-thread waits %s", on ? "up" : "down",
		      on ? "service the run loop" : "block on the credit");
	}
}

static long geAcquireCreditRunLoop(double timeoutSec)
{
	const double deadline = CACurrentMediaTime() + timeoutSec;
	for (;;) {
		if (dispatch_semaphore_wait(s_sem, DISPATCH_TIME_NOW) == 0) {
			return 0;
		}
		const double left = deadline - CACurrentMediaTime();
		if (left <= 0.0) {
			return 1;
		}
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, left < 0.001 ? left : 0.001, true);
	}
}

static long geAcquireCredit(double timeoutSec)
{
	if (atomic_load_explicit(&s_uiActive, memory_order_relaxed) && pthread_main_np()) {
		return geAcquireCreditRunLoop(timeoutSec);
	}
	if (s_spinUs <= 0) {
		return dispatch_semaphore_wait(s_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutSec * 1e9)));
	}
	const double spin = s_spinUs * 1e-6;
	double now = CACurrentMediaTime();
	const double deadline = now + timeoutSec;
	const double next = atomic_load_explicit(&s_nextLinkTs, memory_order_relaxed);
	double spinFrom = next - spin;
	if (spinFrom > deadline) {
		spinFrom = deadline;
	}
	if (spinFrom > now) {
		if (dispatch_semaphore_wait(s_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)((spinFrom - now) * 1e9))) == 0) {
			return 0;
		}
		now = CACurrentMediaTime();
	}
	double spinEnd = (spinFrom > now ? spinFrom : now) + 3.0 * spin;
	if (spinEnd > deadline) {
		spinEnd = deadline;
	}
	if (now < spinEnd) {
		const double t0 = now;
		++s_spinWaits;
		while (now < spinEnd) {
			if (atomic_load_explicit(&s_credits, memory_order_acquire) > 0
			    && dispatch_semaphore_wait(s_sem, DISPATCH_TIME_NOW) == 0) {
				s_spinSec += CACurrentMediaTime() - t0;
				++s_spinHits;
				return 0;
			}
			__asm__ volatile("yield");
			now = CACurrentMediaTime();
		}
		s_spinSec += now - t0;
	}
	now = CACurrentMediaTime();
	if (now >= deadline) {
		return dispatch_semaphore_wait(s_sem, DISPATCH_TIME_NOW);
	}
	return dispatch_semaphore_wait(s_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)((deadline - now) * 1e9)));
}

// Round 22 (D-131), Pacing.RtPolicy: the realtime class Apple provides for
// audio and render threads. Applied once, from the scheduler thread itself
// (the engine creates it; this is the first shell code it runs).
static void geApplyRtPolicy(void)
{
	if (s_rtApplied != -1) {
		return;
	}
	if (!s_rtPolicy) {
		s_rtApplied = 0;
		return;
	}
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	const double nsToAbs = (double)tb.denom / (double)tb.numer;
	const double periodMs = (s_rtPolicy == 2) ? 16.667 : 8.333;
	const double compMs   = (s_rtPolicy == 2) ? 4.0 : 2.0;
	const double consMs   = (s_rtPolicy == 2) ? 8.0 : 4.0;
	thread_time_constraint_policy_data_t pol;
	pol.period      = (uint32_t)(periodMs * 1e6 * nsToAbs);
	pol.computation = (uint32_t)(compMs * 1e6 * nsToAbs);
	pol.constraint  = (uint32_t)(consMs * 1e6 * nsToAbs);
	pol.preemptible = TRUE;
	mach_port_t self = mach_thread_self();
	kern_return_t kr = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY,
		(thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	mach_port_deallocate(mach_task_self(), self);
	s_rtApplied = (int)kr;
	char line[160];
	snprintf(line, sizeof(line), "[pacing] scheduler time-constraint policy %d (%.2f/%.1f/%.1f ms): kr=%d",
	         s_rtPolicy, periodMs, compMs, consMs, (int)kr);
	NSLog(@"projectmidas: %s", line);
	gePaceWriteLine(line);
}

// engine/port/src/video.c videoIdleWait() on iOS: the host main thread's wait
// between two videoPumpEvents() calls (D-090), Pacing.PumpMode (D-131).
void geIosPumpIdleWait(int us)
{
	if (atomic_load_explicit(&s_uiActive, memory_order_relaxed)) {
		// R23-4: a shell page is up; the whole wait goes to the run loop
		// (not one source and out), whatever Pacing.PumpMode says.
		const double end = CACurrentMediaTime() + us * 1e-6;
		double left;
		while ((left = end - CACurrentMediaTime()) > 0.0) {
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, left, true);
		}
		return;
	}
	if (s_pumpMode == 1 && s_pumpSem) {
		// No run loop at all: a kernel semaphore wait arms no CFRunLoop
		// timeout timer, so no dispatch worker is woken to service one. The
		// link signals it every callback; SDL's own pump (two nested 2 us
		// CFRunLoopRunInMode) still runs once per wake and services the main
		// queue and touches.
		dispatch_semaphore_wait(s_pumpSem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)us * NSEC_PER_USEC));
		return;
	}
	if (s_pumpMode == 2) {
		// A spin with none of the run-loop churn: one thread hot, nothing
		// armed, SDL pumped every `us`.
		const double end = CACurrentMediaTime() + us * 1e-6;
		while (CACurrentMediaTime() < end) {
			__asm__ volatile("yield");
		}
		return;
	}
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, (CFTimeInterval)us / 1e6, true);
}

int geIosPacingWaitForPresent(void)
{
	if (!atomic_load_explicit(&s_foreground, memory_order_relaxed)) {
		return 0;
	}

	if (!atomic_load(&s_running) || !s_sem) {
		// No pacer (a --fixed-step replay, or before the link is up): present
		// as fast as the engine offers, exactly as the oracle does.
		return 1;
	}

	// Midas round 9b/9c: Pacing.CreditGate = 0 makes this a pure LIFECYCLE
	// gate and Midas paces itself (pcsched + the gfx limiter), as on the
	// Mac; only the period is noted. Round 9c's default is 1 (the credit
	// path below): docs/port-ios/18 has the measurements.
	if (!s_creditGate) {
		double now = CACurrentMediaTime();
		if (s_lastPresent > 0.0) {
			unsigned i = atomic_fetch_add(&s_ringCount, 1u) % GE_PACE_RING;
			s_ring[i] = now - s_lastPresent;
			gePaceNote(now - s_lastPresent);
			gePaceDistNote(&s_periodDist, (now - s_lastPresent) * 1000.0);
		}
		s_lastPresent = now;
		atomic_fetch_add(&s_presents, 1u);
		atomic_fetch_add(&s_builds, 1u);
		gePaceReport();
		return atomic_load_explicit(&s_foreground, memory_order_relaxed);
	}

	// ANGLE must not wait as well. Done here and not at init because an EGL
	// context is bound per THREAD and this is the render thread
	// (docs/build.md §Traps).
	if (!s_swapIntervalDone) {
		s_swapIntervalDone = 1;
		geAngleSetSwapInterval(0);
		NSLog(@"projectmidas: [pacing] eglSwapInterval(0) — the display link is the clock");
		// (Donor round 9c turned repeat-present on here; Midas has none, and
		// videoSetPresentHz is the panel cap, set once in +start.)
	}

	// D-054, corrected by D-066. TWO numbers, because one of them cannot
	// answer the question the other one can:
	//   period = pacer release of frame N-1 -> here. Wall clock, sleep and
	//            all. Says what the frame RATE is.
	//   build  = videoStartFrame() -> here. No sleep inside it. Says what the
	//            frame COSTS.
	double tEnter = CACurrentMediaTime();
	if (s_wgOpen) {
		if (os_workgroup_interval_finish(s_wg, NULL) != 0) {
			++s_wgErrors;
		}
		s_wgOpen = 0;
	}
	if (s_periodStart > 0.0) {
		double periodMs = (tEnter - s_periodStart) * 1000.0;
		gePaceDistNote(&s_periodDist, periodMs);
	}
	if (s_frameStart > 0.0) {
		double buildMs = (tEnter - s_frameStart) * 1000.0;
		gePaceDistNote(&s_buildDist, buildMs);
		// D-066: `slow` counts frames whose own WORK overran the 60 Hz sim
		// budget -- the frames the phone could not have delivered whatever the
		// clock did. It used to count frames whose PERIOD exceeded 16 ms,
		// which at a healthy 16.67 ms period is every single one of them: the
		// number read `slow 60` out of 60 builds on a frame-perfect second.
		// Builds run at the SIM rate whatever the present rate is, so the
		// budget is 16.67 ms even on a 120 Hz panel.
		// Midas round 18: the budget is one SIM tick, which here is one
		// present (no repeats): 16.67 ms at 60, 8.33 ms at 120.
		extern int portTickHz(void);
		const int tick = portTickHz();
		if (buildMs > ((tick >= 60) ? 1000.0 / (double)tick : 16.67)) {
			++s_slowBuilds;
		}
	}

	geApplyRtPolicy();
	// Round 26: on ge-sim (render thread off) the credit wait is a sim wait;
	// the main thread may run engine code meanwhile, as it did in round 23's
	// run-loop wait. A no-op on ge-render and on the main thread.
	const int simTok = geIosSimYieldBegin();
	// Phase 6 M3: under the compositor clock a missing signal costs 50 ms, so
	// a stalled immersive loop degrades to >= 20 Hz instead of 4 Hz.
	const int ext = atomic_load_explicit(&s_external, memory_order_relaxed);
	long rc = geAcquireCredit(ext ? 0.050 : 0.250);
	geIosSimYieldEnd(simTok);
	if (rc != 0 && ext) {
		atomic_fetch_add(&s_extTimeouts, 1u);
	}
	if (rc == 0) {
		atomic_fetch_sub(&s_credits, 1);
	} else {
		// The link stopped (backgrounded mid-frame) or the system is wedged.
		// Fall through rather than hang, and re-check the gate: skipping the
		// present is right, blocking the game thread for ever is not.
		atomic_fetch_add(&s_timeouts, 1u);
		s_periodStart = CACurrentMediaTime();  // D-054: don't bill the timeout
		return atomic_load_explicit(&s_foreground, memory_order_relaxed);
	}

	double now = CACurrentMediaTime();
	if (s_lastPresent > 0.0) {
		unsigned i = atomic_fetch_add(&s_ringCount, 1u) % GE_PACE_RING;
		s_ring[i] = now - s_lastPresent;
		gePaceNote(now - s_lastPresent);
	}
	s_lastPresent = now;
	atomic_fetch_add(&s_presents, 1u);
	atomic_fetch_add(&s_builds, 1u);
	gePaceDistNote(&s_waitDist, (now - tEnter) * 1000.0);   // D-054
	s_periodStart = now;
	gePaceReport();

	return atomic_load_explicit(&s_foreground, memory_order_relaxed);
}

int geIosPacingGateOpen(void)
{
	return atomic_load_explicit(&s_foreground, memory_order_relaxed);
}

int geIosPacingWaitForRepeat(unsigned timeoutUs)
{
	if (!atomic_load_explicit(&s_foreground, memory_order_relaxed)) {
		return 0;
	}
	if (!atomic_load(&s_running) || !s_sem) {
		return 0;   // no pacer (a replay run): never repeat-present
	}
	if (timeoutUs == 0 || timeoutUs > 250000u) {
		timeoutUs = 250000u;   // never outlive the build deadline by much
	}

	long rc = geAcquireCredit((double)timeoutUs * 1e-6);
	if (rc != 0) {
		return 0;   // no callback in the window: the next BUILD owns it
	}
	atomic_fetch_sub(&s_credits, 1);

	// A repeat IS a present as far as cadence goes — the panel sees it — so it
	// belongs in the inter-present ring; it is not a build, so `builds` does
	// not move and the gate's 60-ticks/s assertion stays about the sim.
	double now = CACurrentMediaTime();
	if (s_lastPresent > 0.0) {
		unsigned i = atomic_fetch_add(&s_ringCount, 1u) % GE_PACE_RING;
		s_ring[i] = now - s_lastPresent;
		gePaceNote(now - s_lastPresent);
	}
	s_lastPresent = now;
	atomic_fetch_add(&s_presents, 1u);
	atomic_fetch_add(&s_repeats, 1u);
	return 1;
}

void geIosPacingSetForeground(int on)
{
	int was = atomic_exchange_explicit(&s_foreground, on ? 1 : 0, memory_order_relaxed);
	if (was != (on ? 1 : 0)) {
		NSLog(@"projectmidas: present gate %s", on ? "OPEN (foreground)" : "CLOSED (background)");
	}
	// Pausing the link stops the callbacks; the 250 ms timeout above is what
	// keeps a scheduler thread that was already waiting from hanging.
	if (sLink) {
		// Phase 6 M3: never un-pause the link while the compositor is the
		// clock - a foreground notification during 3D must not start a second
		// clock (PD D-050's `-resume` correction).
		sLink.paused = (on && !atomic_load(&s_external)) ? NO : YES;
	}
	if (on && s_sem) {
		// Release one waiter immediately so the first foreground frame does
		// not have to wait out a timeout.
		if (atomic_load_explicit(&s_credits, memory_order_relaxed) < 1) {
			atomic_fetch_add(&s_credits, 1);
			dispatch_semaphore_signal(s_sem);
		}
	}
}

#pragma mark - round 26: the sim thread (docs/port-ios/35-round26-simthread.md)

// Rounds 9-25 ran Midas's whole game loop on the MAIN thread: geEngineMain ->
// mainproc -> bossMainloop never returns, under SDL's postFinishLaunch, and the
// main run loop only ran inside SDL's pump. Two things followed. The thread
// that ticks the sim may never join the frame work interval (UIKit owns the
// main thread's, R23-1), and on Austin's phone that tick ran ~10x slower in
// the seconds the frame rate dipped (doc 34 §4: 5.8 vs 0.5 ms a tick, at 120,
// even on the title screen): the same work, slower, i.e. a slow core or a
// low clock that nothing in the process was in a position to argue with.
//
// So the game loop gets its own thread, ge-sim, created at the point main.c
// used to call mainproc: it joins `ge.frame` like ge-render and takes the
// sibling's thread policy (Pacing.SimPolicy, the sibling's Tick.ThreadQos=1:
// QOS_CLASS_USER_INTERACTIVE, D-130), and geEngineMain returns to UIKit's own
// run loop.
//
// THE ENGINE LOCK. Everything the main thread did "between ticks" by virtue
// of being the sim thread -- main-queue blocks (the settings page, the dev
// bridge's geOnMain, GEXbla's completions), UIKit callbacks that call geHost*,
// SDL's event watches (input_pc.c inputEventWatch: hotplug slot reassignment,
// the console's text), the lifecycle notifications -- still has to see the
// engine between ticks, never during one. One mutex gives exactly that:
//   - the main thread holds it whenever its run loop is not asleep (taken by
//     the first AfterWaiting / Entry observer, dropped by the last
//     BeforeWaiting observer, after Core Animation's commit);
//   - ge-sim holds it except in its waits: pcsched.c's pacing sleep (and the
//     vsync lock wait inside it) and, when ge-sim renders itself, the present
//     credit (geIosPacingWaitForPresent).
// ge-render, the link thread, the bridge's socket thread and the cold-build
// worker never take it, exactly as they never ran on the main thread before.
//
// The GL context: under the sim thread the render thread is always on
// (video.c portRenderThreadWanted, GE_IOS), at 60 as at 120. GL then lives
// on ge-render only, and every engine path that reaches GL from another
// thread is already latched for it (gfx_pc.cpp gfx_async_*): the 120 Hz
// path every build since round 18 has shipped. Without that, a settings
// change made on the main thread at 60 (filtering, a texture-cache clear)
// would issue GL calls on a thread with no current context.

static dispatch_once_t s_simWantOnce;
static int s_simWant = 0;               // Pacing.SimThread, read once
static int s_simPolicyCfg = 1;          // Pacing.SimPolicy, read once
static atomic_int s_simRunning = 0;
static mach_port_t s_simThreadPort = MACH_PORT_NULL;
static double s_cpuLastSim = 0.0;
static int s_simPolicyRc = 0, s_simQosRc = 0;
static pthread_mutex_t s_engineLock = PTHREAD_MUTEX_INITIALIZER;
static int s_mainHolds = 0;             // main thread only
static __thread int t_isSim = 0;
static __thread int t_simHolds = 0;
// ge-sim's waits for the lock (the main thread was busy): ns and count, the
// report's `lockwait` column. Written by ge-sim, read and zeroed by the report.
static _Atomic uint64_t s_simLockWaitNs = 0;
static atomic_uint s_simLockWaits = 0;
static _Atomic uint64_t s_simLockWaitMaxNs = 0;
static atomic_uint s_mainLocks = 0;     // main-thread acquisitions (wakes)

int geIosSimThreadWanted(void)
{
	dispatch_once(&s_simWantOnce, ^{
		s_simWant = [GEDefaults intFor:GEDefaultsSimThread] ? 1 : 0;
		s_simPolicyCfg = (int)[GEDefaults intFor:GEDefaultsSimPolicy];
		if (s_simPolicyCfg < 0 || s_simPolicyCfg > 2) {
			s_simPolicyCfg = 1;
		}
	});
	return s_simWant;
}

static inline double geAbsToNs(uint64_t abs)
{
	static double k = 0.0;
	if (k == 0.0) {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		k = (double)tb.numer / (double)tb.denom;
	}
	return (double)abs * k;
}

static void geEngineLockTake(CFRunLoopObserverRef o, CFRunLoopActivity a, void *info)
{
	(void)o; (void)a; (void)info;
	if (!s_mainHolds) {
		pthread_mutex_lock(&s_engineLock);
		s_mainHolds = 1;
		atomic_fetch_add_explicit(&s_mainLocks, 1u, memory_order_relaxed);
	}
}

static void geEngineLockDrop(CFRunLoopObserverRef o, CFRunLoopActivity a, void *info)
{
	(void)o; (void)a; (void)info;
	if (s_mainHolds) {
		s_mainHolds = 0;
		pthread_mutex_unlock(&s_engineLock);
	}
}

int geIosSimYieldBegin(void)
{
	if (!t_simHolds) {
		return 0;
	}
	t_simHolds = 0;
	pthread_mutex_unlock(&s_engineLock);
	return 1;
}

void geIosSimYieldEnd(int token)
{
	if (!token) {
		return;
	}
	if (pthread_mutex_trylock(&s_engineLock) != 0) {
		const uint64_t t0 = mach_absolute_time();
		pthread_mutex_lock(&s_engineLock);
		const uint64_t ns = (uint64_t)geAbsToNs(mach_absolute_time() - t0);
		atomic_fetch_add_explicit(&s_simLockWaitNs, ns, memory_order_relaxed);
		atomic_fetch_add_explicit(&s_simLockWaits, 1u, memory_order_relaxed);
		if (ns > atomic_load_explicit(&s_simLockWaitMaxNs, memory_order_relaxed)) {
			atomic_store_explicit(&s_simLockWaitMaxNs, ns, memory_order_relaxed);
		}
	}
	t_simHolds = 1;
}

// Round 28 (doc 38): fast3d's own per-second counters, at the very end of the
// line. `texmiss`/`evict` = texture-cache imports and LRU evictions per second
// (item 2, Video.TexCacheMax), `tcache` = entries / cap.
extern uint32_t gfx_texcache_misses, gfx_texcache_evictions, gfx_texcache_size;
extern int texpackTexCacheMax(void);
static int geRenderStatsLine(char *buf, size_t n, double dt)
{
	static uint32_t lastMiss = 0, lastEvict = 0;
	const uint32_t miss = gfx_texcache_misses, ev = gfx_texcache_evictions;
	// Item 3: `draws p50/p95 max` and `tris p50/p95` per frame over the second.
	static double scratch[GE_PACE_SAMPLES];
	const double d50 = gePaceDistPct(&s_drawsDist, 50, scratch);
	const double d95 = gePaceDistPct(&s_drawsDist, 95, scratch);
	const double dmax = gePaceDistPct(&s_drawsDist, 100, scratch);
	const double t50 = gePaceDistPct(&s_trisDist, 50, scratch);
	const double t95 = gePaceDistPct(&s_trisDist, 95, scratch);
	s_drawsDist.n = s_trisDist.n = 0;
	const int w = snprintf(buf, n, " texmiss %.0f/s evict %.0f/s tcache %u/%d draws %.0f/%.0f/%.0f tris %.0f/%.0f",
		(double)(miss - lastMiss) / dt, (double)(ev - lastEvict) / dt,
		(unsigned)gfx_texcache_size, texpackTexCacheMax(), d50, d95, dmax, t50, t95);
	lastMiss = miss;
	lastEvict = ev;
	return w;
}

// The pacing.log tail for the sim thread: its CPU over the window (% of one
// core), the lock waits, the main thread's wakes. "" without the sim thread.
static int geSimLine(char *buf, size_t n, double dt)
{
	if (!atomic_load_explicit(&s_simRunning, memory_order_relaxed) || n < 2 || dt <= 0.0) {
		if (n) {
			buf[0] = '\0';
		}
		return 0;
	}
	const double ts = geThreadCpuSec(s_simThreadPort);
	const double simCpu = (ts - s_cpuLastSim) / dt * 100.0;
	s_cpuLastSim = ts;
	const uint64_t waitNs = atomic_exchange_explicit(&s_simLockWaitNs, 0, memory_order_relaxed);
	const unsigned waits = atomic_exchange_explicit(&s_simLockWaits, 0u, memory_order_relaxed);
	const uint64_t maxNs = atomic_exchange_explicit(&s_simLockWaitMaxNs, 0, memory_order_relaxed);
	const unsigned wakes = atomic_exchange_explicit(&s_mainLocks, 0u, memory_order_relaxed);
	return snprintf(buf, n, " simcpu %.0f%% lockwait %.2f ms/s %u max %.2f ms mainwakes %.0f/s simwg %d",
	                simCpu, waitNs / 1e6 / dt, waits, maxNs / 1e6, wakes / dt,
	                atomic_load_explicit(&s_simWgState, memory_order_relaxed));
}

static void (*s_simEntry)(void *) = NULL;

static void *geSimThreadMain(void *arg)
{
	(void)arg;
	pthread_setname_np("ge-sim");
	t_isSim = 1;
	s_simThreadPort = pthread_mach_thread_np(pthread_self());
	s_cpuLastSim = geThreadCpuSec(s_simThreadPort);

	// SimPolicy 2: the time-constraint class on top of user-interactive QoS,
	// the shape of Pacing.RtPolicy=1 (period one 120 Hz frame, 2 ms of
	// computation inside 4). Preemptible; the kernel demotes a thread that
	// overruns it for long (a level load), which is the safe failure.
	if (s_simPolicyCfg == 2) {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		const double nsToAbs = (double)tb.denom / (double)tb.numer;
		thread_time_constraint_policy_data_t pol;
		pol.period      = (uint32_t)(8.333 * 1e6 * nsToAbs);
		pol.computation = (uint32_t)(2.0 * 1e6 * nsToAbs);
		pol.constraint  = (uint32_t)(4.0 * 1e6 * nsToAbs);
		pol.preemptible = TRUE;
		s_simPolicyRc = (int)thread_policy_set(s_simThreadPort, THREAD_TIME_CONSTRAINT_POLICY,
			(thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	}
	{
		char line[192];
		snprintf(line, sizeof(line), "[pacing] sim thread ge-sim up: policy %d (qos %#x, attr rc=%d, "
		         "time-constraint kr=%d), the main thread keeps UIKit's run loop",
		         s_simPolicyCfg, (unsigned)qos_class_self(), s_simQosRc, s_simPolicyRc);
		NSLog(@"projectmidas: %s", line);
		gePaceWriteLine(line);
	}
	// Round 26: ge-sim is not the main thread, so it may join (R23-1).
	geWgJoinThisThread("ge-sim");
	atomic_store_explicit(&s_simWgState, t_wgJoined, memory_order_relaxed);

	// The main thread holds the lock until its run loop first sleeps, i.e.
	// until geEngineMain has returned through SDL's postFinishLaunch.
	pthread_mutex_lock(&s_engineLock);
	t_simHolds = 1;
	s_simEntry(NULL);   // mainproc: never returns in practice

	gePaceWriteLine("[pacing] sim thread: the game loop returned");
	t_simHolds = 0;
	pthread_mutex_unlock(&s_engineLock);
	return NULL;
}

int geIosSimThreadStart(void (*entry)(void *))
{
	if (!geIosSimThreadWanted() || !pthread_main_np() || entry == NULL) {
		gePaceWriteLine("[pacing] sim thread off (Pacing.SimThread 0): the game loop runs on the main thread");
		return 0;
	}
	s_simEntry = entry;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8u << 20);   // the main thread's is 1 MB on iOS
	s_simQosRc = pthread_attr_set_qos_class_np(&attr,
		s_simPolicyCfg >= 1 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT, 0);

	// The main thread holds the engine lock from here until its run loop first
	// goes to sleep; the observers keep it that way for the life of the app.
	pthread_mutex_lock(&s_engineLock);
	s_mainHolds = 1;
	CFRunLoopObserverRef take = CFRunLoopObserverCreate(kCFAllocatorDefault,
		kCFRunLoopEntry | kCFRunLoopAfterWaiting, true, LONG_MIN, geEngineLockTake, NULL);
	CFRunLoopObserverRef drop = CFRunLoopObserverCreate(kCFAllocatorDefault,
		kCFRunLoopBeforeWaiting, true, LONG_MAX, geEngineLockDrop, NULL);
	CFRunLoopAddObserver(CFRunLoopGetMain(), take, kCFRunLoopCommonModes);
	CFRunLoopAddObserver(CFRunLoopGetMain(), drop, kCFRunLoopCommonModes);

	// SDL's own iOS pump runs the MAIN run loop from inside SDL_PumpEvents
	// (UIKit_PumpEvents). From here the main run loop is UIKit's, and the
	// SDL_PollEvent calls fast3d makes on ge-sim only drain SDL's queue (and
	// update the joysticks), under the engine lock. SDL turns this off itself
	// once SDL_main returns; doing it first closes the gap.
	SDL_iPhoneSetEventPump(SDL_FALSE);

	pthread_t th;
	const int rc = pthread_create(&th, &attr, geSimThreadMain, NULL);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		CFRunLoopRemoveObserver(CFRunLoopGetMain(), take, kCFRunLoopCommonModes);
		CFRunLoopRemoveObserver(CFRunLoopGetMain(), drop, kCFRunLoopCommonModes);
		CFRelease(take);
		CFRelease(drop);
		s_mainHolds = 0;
		pthread_mutex_unlock(&s_engineLock);
		SDL_iPhoneSetEventPump(SDL_TRUE);
		char line[128];
		snprintf(line, sizeof(line), "[pacing] sim thread FAILED (pthread_create rc=%d): the game loop stays on main", rc);
		NSLog(@"projectmidas: %s", line);
		gePaceWriteLine(line);
		return 0;
	}
	pthread_detach(th);
	atomic_store(&s_simRunning, 1);
	return 1;
}

void geIosSdlStartTextInput(void)
{
	if (pthread_main_np()) {
		SDL_StartTextInput();
		return;
	}
	dispatch_async(dispatch_get_main_queue(), ^{ SDL_StartTextInput(); });
}

void geIosSdlStopTextInput(void)
{
	if (pthread_main_np()) {
		SDL_StopTextInput();
		return;
	}
	dispatch_async(dispatch_get_main_queue(), ^{ SDL_StopTextInput(); });
}

int geIosSdlSetRelativeMouseMode(int on)
{
	if (pthread_main_np()) {
		return SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
	}
	// With a GCMouse this ends in UIKit (setNeedsUpdateOfPrefersPointerLocked).
	dispatch_async(dispatch_get_main_queue(), ^{ SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE); });
	return 0;
}

int geIosSdlShowSimpleMessageBox(unsigned int flags, const char *title, const char *message, void *window)
{
	// SDL_uikitmessagebox.m dispatch_syncs to the main queue off the main
	// thread; the main thread needs the engine lock to run that block.
	const int tok = geIosSimYieldBegin();
	const int r = SDL_ShowSimpleMessageBox(flags, title, message, (SDL_Window *)window);
	geIosSimYieldEnd(tok);
	return r;
}

#pragma mark - Phase 6 M3: the compositor as the clock (D-050)

const char *geIosPacingModeName(void)
{
	return atomic_load(&s_external) ? "compositor" : "displaylink";
}

void geIosPacingSetExternal(int on)
{
	on = on ? 1 : 0;
	if (atomic_exchange(&s_external, on) == on) {
		return;
	}
	NSLog(@"projectmidas: [pacing] clock -> %s", on ? "compositor (link paused)" : "displaylink");
#if TARGET_OS_VISION
	if (on) {
		s_visionHzBefore3d = atomic_load(&s_visionHz);
		s_vwStart = 0.0;
		s_vwCand = 0;
		s_vwRuns = 0;
		s_vLastChange = 0.0;   // the compositor's first verdict may adopt at once
	}
#endif
	s_extWin0 = 0.0;
	s_extWinN = 0;
	// The link is PAUSED, never removed, and only from its own thread (PD
	// D-043: reconfiguring a live CADisplayLink off its thread).
	if (sThread) {
		[GEPacing performSelector:@selector(applyLinkPause) onThread:sThread withObject:nil waitUntilDone:NO];
	}
	if (!on) {
#if TARGET_OS_VISION
		geVisionRestoreAfter3d();
#endif
		// whoever waits for a compositor frame that will never come gets out
		// now; the link's next callback takes over
		geReleaseCredit();
	}
}

void geIosPacingSignalExternal(double periodSec)
{
	if (!atomic_load(&s_external)) {
		return;
	}
	atomic_fetch_add(&s_extSignals, 1u);
	const double now = CACurrentMediaTime();
	static double last = 0.0;
	double dt = (periodSec > 0.0005 && periodSec < 0.25) ? periodSec : ((last > 0.0) ? now - last : 1.0 / 90.0);
	last = now;
	if (dt > 0.25) {
		dt = 1.0 / 90.0;
	}
	if (s_extWin0 <= 0.0) {
		s_extWin0 = now;
		s_extWinN = 0;
	}
	s_extWinN++;
	if (now - s_extWin0 >= 1.0) {
		s_extHz = s_extWinN / (now - s_extWin0);
		s_extWin0 = now;
		s_extWinN = 0;
	}
#if TARGET_OS_VISION
	// D-056 via round 36: the compositor's period is the grant, so Max FPS:
	// Auto (N) and the sim tick follow the compositor's cadence.
	geVisionNoteGrant(now, dt * 1000.0);
#endif
	geBeatPre(dt);
	geReleaseCredit();
	geBeatPost(dt);
}
