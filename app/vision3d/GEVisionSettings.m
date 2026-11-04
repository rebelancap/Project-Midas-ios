// GEVisionSettings.m — the 3D settings: `vp3d.*` -> 3D state, the sheet's
// open/close state and the exit ordering, and the bridge's `3d settings …`
// (Phase 6 M5, round 49, docs/port-ios/69; plan docs/port-ios/64 §2.10).
//
// Copied from perfect-dark-ios app/vision3d/PDVisionSettingsViewController.m
// (the apply ≈ :111 "THE ONLY PLACE vp3d.* BECOMES 3D STATE", the Reset that
// forgets keys rather than writing numbers back, the sheet state and the
// bridge's set/state rows), pd -> ge, with the Midas deltas:
//
//   * THE ROWS LIVE IN GESettings.m. Midas's settings page is a table of
//     GESettingsSection/GESettingsRow descriptors, so the 3D rows are one more
//     section of it (on visionOS only) rather than PD's separate row provider.
//     This file owns everything that is not UIKit.
//   * THE EXIT DOES NOT PUMP THE RUN LOOP. PD's
//     pdVision3dSettingsSheetCloseAndWait spins CFRunLoopRunInMode on main
//     until the sheet's .onDisappear answers. Here the exit can arrive inside
//     a dispatch_sync onto the main queue (the bridge's `3d off`), where a
//     nested run loop does NOT drain the main queue - and SwiftUI's dismissal
//     and our own GE_SetSettingsSheet hop both live on it. So the wait is a
//     20 ms run-loop TIMER that runs the rest of the exit as a continuation
//     (geVision3dSettingsSheetCloseThen), the same shape as M4's exit
//     finalize. The ordering is the same: the sheet is GONE before the
//     un-park.
//   * THE STEREO CONSTANTS ARE PUBLISHED (geVisionStereoPublish): one lock,
//     one generation, latched by ge-render once per pair (the M3 review's
//     race: a bridge write landing between the two eye walks).
#import "GEVision3D.h"

#if TARGET_OS_VISION

#import "GEDefaults.h"
#import "GEShell.h"

#include <math.h>

extern int geHostConfigGet(const char *key, char *out, int outLen);
extern int geHostConfigSet(const char *key, const char *value);
extern void geHostFlushSaves(void);

#define GE_FT_PER_M 3.28084f

// ---------------------------------------------------------------------------
// The row table: the bridge's names, the stored key, the STORED range, and
// the wire scale (the bridge speaks FULL width/height and per-cent dimming,
// like the page's readouts; the keys hold half-extents and a fraction, like
// the compositor). Ranges are PD's (docs/visionos-3d-plan.md §2.10 table).
// ---------------------------------------------------------------------------
typedef struct {
	const char *name;
	const char *title;
	__unsafe_unretained NSString *const *key;
	float lo, hi;       // stored units
	float wire;         // wire value = stored * wire
	const char *unit;   // "len" (metres, shown in m|ft), "pct", "units"
} GE3DRow;

static const GE3DRow kRows[] = {
	{ "dist",   "Screen Distance",        &GEDefaults3DDistance,    1.0f,   8.0f,    1.0f,   "len" },
	{ "width",  "Screen Width",           &GEDefaults3DHalfWidth,   0.6f,   6.096f,  2.0f,   "len" },
	{ "height", "Screen Height",          &GEDefaults3DHalfHeight,  0.5f,   3.0f,    2.0f,   "len" },
	{ "posh",   "Screen Position Height", &GEDefaults3DPosHeight,  -1.5f,  10.0f,    1.0f,   "len" },
	{ "depth",  "Stereo Depth",           &GEDefaults3DDepthPct,    0.0f, 300.0f,    1.0f,   "pct" },
	{ "conv",   "Convergence",            &GEDefaults3DConvergence, 100.0f, 1500.0f, 1.0f,   "units" },
	{ "dim",    "Surroundings Dimming",   &GEDefaults3DDimming,     0.0f,   1.0f,  100.0f,   "pct" },
	{ "render", "Render Resolution",      &GEDefaults3DRenderPct,  40.0f, 100.0f,    1.0f,   "pct" },
};
#define GE_3D_ROWS ((int)(sizeof(kRows) / sizeof(kRows[0])))

static float geF(NSString *key) { return (float)[GEDefaults doubleFor:key]; }
static float geClampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int geFpsOn(void)
{
	char buf[32] = {0};
	return (geHostConfigGet("show_fps", buf, (int)sizeof(buf)) == 0) ? atoi(buf) != 0 : 0;
}

// ---------------------------------------------------------------------------
// The apply. THE ONLY PLACE `vp3d.*` BECOMES 3D STATE.
// ---------------------------------------------------------------------------

static void geApply(int commit)
{
	const float dist  = geClampf(geF(GEDefaults3DDistance),   1.0f,  8.0f);
	const float halfW = geClampf(geF(GEDefaults3DHalfWidth),  0.6f,  6.096f);
	const float halfH = geClampf(geF(GEDefaults3DHalfHeight), 0.5f,  3.0f);
	const float posH  = geClampf(geF(GEDefaults3DPosHeight), -1.5f, 10.0f);
	const float dim   = geClampf(geF(GEDefaults3DDimming),    0.0f,  1.0f);
	geVisionPanelSet(dist, halfW, halfH, posH, dim);
	// PD D-061: a value persisted above the 300 % cap is clamped DOWN and
	// written back, so the slider and the fold agree.
	if (geF(GEDefaults3DDepthPct) > 300.0f) {
		[GEDefaults setString:@"300" forKey:GEDefaults3DDepthPct];
		GELog(@"[3d] stored Stereo Depth was above the 300 %% cap — clamped");
	}
	const float depth = geClampf(geF(GEDefaults3DDepthPct), 0.0f, 300.0f);
	const float conv  = geClampf(geF(GEDefaults3DConvergence), 100.0f, 1500.0f);
	// Only a CHANGE is published: a live drag of a Screen row must not bump the
	// fold generation sixty times a second for nothing.
	static float lastDepth = -1.0f, lastConv = -1.0f;
	if (depth != lastDepth || conv != lastConv) {
		lastDepth = depth;
		lastConv = conv;
		geVisionStereoPublish(depth, conv, -1.0f);
	}
	if (commit) {
		geVisionEyeSetPanel(halfW, halfH, geClampf(geF(GEDefaults3DRenderPct), 40.0f, 100.0f));
	}
}

void geVision3dApplySettings(void)     { geApply(1); }
void geVision3dApplySettingsLive(void) { geApply(0); }

float geVision3dPanelAspect(void)
{
	const float hw = geClampf(geF(GEDefaults3DHalfWidth), 0.6f, 6.096f);
	const float hh = geClampf(geF(GEDefaults3DHalfHeight), 0.5f, 3.0f);
	return hw / hh;
}

void geVision3dSettingsResetDefaults(void)
{
	// FORGET the keys rather than write the numbers back (PD): the registered
	// factory in GEDefaults.m is then the one place the defaults live.
	// Units and FPS on Panel are deliberately NOT in this list (spec).
	NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
	for (int i = 0; i < GE_3D_ROWS; i++) {
		[d removeObjectForKey:*kRows[i].key];
	}
	[d synchronize];
	geVision3dApplySettings();
	GELog(@"[3d] settings RESET to defaults (Units and FPS on Panel kept)");
	geVision3dSettingsReloadPages();
}

// ---------------------------------------------------------------------------
// The sheet's open/close state, and the exit ordering
// ---------------------------------------------------------------------------

static int geSheetUp = 0;
static int geSheetAsked = 0;

void geVision3dSettingsSheetRequest(int open)
{
	if (!NSThread.isMainThread) {
		dispatch_async(dispatch_get_main_queue(), ^{ geVision3dSettingsSheetRequest(open); });
		return;
	}
	geSheetAsked = open ? 1 : 0;
	GELog(@"[3d] settings sheet %s requested", open ? "OPEN" : "CLOSE");
	GE_SetSettingsSheet(open ? true : false);
}

void geVision3dSettingsSheetNote(int up)
{
	geSheetUp = up ? 1 : 0;
	if (!up) {
		geSheetAsked = 0;
	}
	GELog(@"[3d] settings sheet is now %s", up ? "UP" : "GONE");
}

int geVision3dSettingsSheetUp(void) { return geSheetUp || geSheetAsked; }

void geVision3dSettingsSheetCloseThen(void (^then)(void))
{
	if (!geSheetUp && !geSheetAsked) {
		then();
		return;
	}
	GELog(@"[3d] the settings sheet is open at the 3D exit — closing it BEFORE the un-park");
	geVision3dSettingsSheetRequest(0);
	const CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
	void (^cont)(void) = [then copy];
	[NSTimer scheduledTimerWithTimeInterval:0.02 repeats:YES block:^(NSTimer *t) {
		const double ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000.0;
		if (geSheetUp && ms < 2000.0) {
			return;
		}
		[t invalidate];
		GELog(@"[3d] settings sheet gone=%d after %.0f ms — the exit continues", !geSheetUp, ms);
		cont();
	}];
}

// ---------------------------------------------------------------------------
// The bridge
// ---------------------------------------------------------------------------

static NSString *geShown(const GE3DRow *r, float stored)
{
	const BOOL ft = [GEDefaults boolFor:GEDefaults3DUnitsFeet];
	const float wire = stored * r->wire;
	if (!strcmp(r->unit, "len")) {
		const float v = ft ? wire * GE_FT_PER_M : wire;
		return [NSString stringWithFormat:(!strcmp(r->name, "posh") ? @"%+.1f %s" : @"%.1f %s"),
			(double)v, ft ? "ft" : "m"];
	}
	if (!strcmp(r->name, "conv")) {
		// GoldenEye's world unit is ~1 cm, the same convention PD's readout
		// uses: 762 units = 7.62 m = 25 ft.
		const float m = wire / 100.0f;
		return [NSString stringWithFormat:@"%.1f %s", (double)(ft ? m * GE_FT_PER_M : m), ft ? "ft" : "m"];
	}
	return [NSString stringWithFormat:@"%.0f%%", (double)wire];
}

static NSString *geRowLine(const GE3DRow *r)
{
	NSString *key = *r->key;
	const float v = geF(key);
	const float def = (float)[[GEDefaults factoryStringForKey:key] doubleValue];
	return [NSString stringWithFormat:@"row=%s key=%@ value=%.3f min=%.3f max=%.3f default=%.3f shown=\"%@\" title=\"%s\"\n",
		r->name, key, (double)(v * r->wire), (double)(r->lo * r->wire), (double)(r->hi * r->wire),
		(double)(def * r->wire), geShown(r, v), r->title];
}

NSString *geVision3dSettingsGet(NSString *row)
{
	NSMutableString *out = [NSMutableString string];
	NSString *want = row.lowercaseString;
	for (int i = 0; i < GE_3D_ROWS; i++) {
		if (!want.length || [want isEqualToString:@(kRows[i].name)]) {
			[out appendString:geRowLine(&kRows[i])];
		}
	}
	if (!want.length || [want isEqualToString:@"units"]) {
		[out appendFormat:@"row=units key=%@ value=%@ default=%@ choices=m|ft title=\"Units\"\n",
			GEDefaults3DUnitsFeet, [GEDefaults boolFor:GEDefaults3DUnitsFeet] ? @"ft" : @"m",
			[[GEDefaults factoryStringForKey:GEDefaults3DUnitsFeet] intValue] ? @"ft" : @"m"];
	}
	if (!want.length || [want isEqualToString:@"fps"]) {
		[out appendFormat:@"row=fps key=show_fps(midas.ini) value=%d default=0 title=\"FPS on Panel\"\n", geFpsOn()];
	}
	if (!out.length) {
		return [NSString stringWithFormat:@"error=no-such-row row=%@\n", row];
	}
	return out;
}

NSString *geVision3dSettingsSet(NSString *row, NSString *value)
{
	NSString *r = row.lowercaseString;
	if ([r isEqualToString:@"units"]) {
		NSString *lv = value.lowercaseString;
		const BOOL ft = [lv hasPrefix:@"f"] || [lv isEqualToString:@"1"];
		[GEDefaults setString:ft ? @"1" : @"0" forKey:GEDefaults3DUnitsFeet];
		geVision3dSettingsReloadPages();
		GELog(@"[3d] settings set units -> %s", ft ? "ft" : "m");
		return [NSString stringWithFormat:@"set_units=%s\n", ft ? "ft" : "m"];
	}
	if ([r isEqualToString:@"fps"]) {
		NSString *lv = value.lowercaseString;
		const BOOL on = lv.floatValue != 0.0f || [lv isEqualToString:@"on"] || [lv isEqualToString:@"yes"];
		// The engine's own counter (Display show_fps) - the 2D page's "Show
		// FPS", one key. Drawn in the 2D pass, so it lands in both eyes.
		geHostConfigSet("show_fps", on ? "1" : "0");
		geHostFlushSaves();
		geVision3dSettingsReloadPages();
		GELog(@"[3d] settings set fps -> %d", (int)on);
		return [NSString stringWithFormat:@"set_fps=%d\n", geFpsOn()];
	}
	for (int i = 0; i < GE_3D_ROWS; i++) {
		const GE3DRow *k = &kRows[i];
		if (![r isEqualToString:@(k->name)]) {
			continue;
		}
		float stored = geClampf(value.floatValue / k->wire, k->lo, k->hi);
		if (!strcmp(k->name, "render")) {
			stored = roundf(stored);
		}
		[GEDefaults setString:[NSString stringWithFormat:@"%g", (double)stored] forKey:*k->key];
		// A bridge set is atomic - no drag, no release - so it commits.
		geVision3dApplySettings();
		geVision3dSettingsReloadPages();
		GELog(@"[3d] settings set %s -> %.3f (stored %.4f)", k->name, (double)(stored * k->wire), (double)stored);
		return [NSString stringWithFormat:@"set_%s=%.3f\n", k->name, (double)(stored * k->wire)];
	}
	return [NSString stringWithFormat:@"error=no-such-row row=%@ (dist|width|height|posh|depth|conv|dim|render|units|fps)\n", row];
}

NSString *geVision3dSettingsStateLines(void)
{
	return [NSString stringWithFormat:@"sheet=%s\nsheet_up=%d\nsheet_asked=%d\nunits=%s\nfps_on_panel=%d\n%@",
		geSheetUp ? "open" : "closed", geSheetUp, geSheetAsked,
		[GEDefaults boolFor:GEDefaults3DUnitsFeet] ? "ft" : "m", geFpsOn(),
		geVisionPanelStateLines()];
}

#endif // TARGET_OS_VISION
