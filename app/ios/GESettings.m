// GESettings.m — see GESettings.h for the three-store split and the spec.
//
// Copied from GoldenEye-Decomp-ios app/ios/GESettings.m (55a3a9c8), round 9b.
// Midas: the engine rows are Midas's own options table (port/src/port_options.c)
// by midas.ini key through geHostConfigGet/Set; the four game options keep the
// donor's names (hostinput.c maps them); volumes are Midas's music_volume /
// sfx_volume. Round 16 (XBLA step 5) adds the donor's texture section
// ("HD graphics" there, "Textures" here) over GEXbla; its engine keys are the
// [video] Video.* keys of port/apple/xblaconfig.c, which geHostConfigGet/Set
// reach after Midas's own options table (and which save on every set).
#import "GESettings.h"
#import "GEDefaults.h"
#import "GEShell.h"
#import "GEAudio.h"
#import "GEPacing.h"
#import "GEInput.h"
#import "GEPad.h"
#import "GETouchOverlay.h"
#import "GETouchLayout.h"
#import "GEVision.h"
#if TARGET_OS_VISION
#import "GEVision3D.h"   // round 49 (Phase 6 M5): the 3D Settings section
#endif

// GEPacing.m (round 18): the panel maximum, with the dev-only GE_PANEL_HZ.
extern int geIosPanelMaxHz(void);
extern int geIosFastHz(void);   // GEPacing.m, round 34: 60, or the panel max (90 visionOS, 120 ProMotion)
#import "GEXbla.h"
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>

// port/include/hostinput.h
extern int geHostConfigGet(const char *key, char *out, int outLen);
extern int geHostConfigSet(const char *key, const char *value);
extern int geHostGameOptionGet(const char *name);
extern int geHostGameOptionSet(const char *name, int value);
extern int geHostInGameplay(void);
extern void geHostPortActionClear(int src, int pad);   // hostinput.c, round 59
extern void geHostFlushSaves(void);
extern void geHostState(int *stage, float *fps, unsigned int *ticks, unsigned int *frames);
// port/include/audio.h (D-089)
extern float audioGetHostGain(void);

#pragma mark - the three stores

typedef NS_ENUM(NSInteger, GEStore) {
	GEStoreNone = 0,   ///< an action or an info row: nothing to read or write
	GEStoreDefaults,   ///< NSUserDefaults, via GEDefaults
	GEStoreEngine,     ///< midas.ini (Midas's options table), via geHostConfigGet/Set
	GEStoreGameOption, ///< GoldenEye's own options, via geHostGameOption*
};

/// The current value as a string, or nil when the key does not exist in that
/// store. A nil here is what makes a row get dropped rather than written into
/// nothing.
static NSString *geStoreGet(GEStore store, NSString *key)
{
	switch (store) {
	case GEStoreDefaults:
		return [GEDefaults stringForKey:key];
	case GEStoreEngine: {
		char buf[128] = {0};
		if (geHostConfigGet(key.UTF8String, buf, (int)sizeof(buf)) != 0) {
			return nil;
		}
		return @(buf);
	}
	case GEStoreGameOption: {
		int v = geHostGameOptionGet(key.UTF8String);
		return (v < 0) ? nil : [NSString stringWithFormat:@"%d", v];
	}
	default:
		return nil;
	}
}

static void geStoreSet(GEStore store, NSString *key, NSString *value)
{
	switch (store) {
	case GEStoreDefaults:
		[GEDefaults setString:value forKey:key];
		break;
	case GEStoreEngine:
		geHostConfigSet(key.UTF8String, value.UTF8String);
		break;
	case GEStoreGameOption:
		geHostGameOptionSet(key.UTF8String, value.intValue);
		break;
	default:
		break;
	}
}

static double geStoreDouble(GEStore store, NSString *key)
{
	return [geStoreGet(store, key) doubleValue];
}

/// Round 37 (docs/port-ios/50): the 2D layer's defaults. iOS: a 2x xBR
/// picture, point-sampled (crisp). visionOS: 4x, bilinear (a big window).
static NSString *GE2DUpscaleDefault(void)
{
#if TARGET_OS_VISION
	return @"4";
#else
	return @"2";
#endif
}

static NSString *GE2DFilterDefault(void)
{
#if TARGET_OS_VISION
	return @"1";
#else
	return @"0";
#endif
}

/// The engine keys this PAGE owns, and the value "reset" puts back. It is a
/// table here rather than a call into config.c because config.c has no
/// get-default-by-key either — the registered default is the value the static
/// initialiser gave the variable, which the loader has already overwritten by
/// the time anyone could ask. Keeping the list here also keeps "reset" honest
/// about its scope: these keys and no others (Window.*, Tick.*, Debug.* and
/// the harness knobs are nobody's business on a settings page).
static NSDictionary<NSString *, NSString *> *geEngineFactory(void)
{
	// Midas's own defaults (tools/midas_ini_help.txt), for the keys this page
	// shows. The ini spellings are what portOptValueIni() reports.
	return @{
		// Round 23 (R23-3): the panel default -- 120 on a >= 120 Hz (ProMotion)
		// panel, 60 otherwise (video.c's GE_IOS compiled default stays 60;
		// +applyPanelMaxFpsDefault moves a fresh or untouched install).
		// Round 34: the panel's maximum, so 90 on the Vision Pro.
		@"maxfps"           : [NSString stringWithFormat:@"%d", geIosFastHz()],
		@"msaa"             : @"1",
		@"low_res"          : @"0",
		@"filtering"        : @"3point",   // this tree's compiled default (engine.log config line)
		@"anisotropy"       : @"0",
		// Round 37: the 2D layer's xBR upscale and its filter, per platform
		// (video.c's compiled defaults; +apply2DDefaults moves an install once).
		@"upscale_2d"       : GE2DUpscaleDefault(),
		@"filter_2d"        : GE2DFilterDefault(),
		@"hfov_wide"        : @"90",
		@"widescreen"       : @"1",
		@"n64_aa"           : @"0",
		@"show_fps"         : @"0",   // ini-only; hostinput.c special-cases the key
		@"always_crosshair" : @"0",
		@"aim_lock"         : @"0",
		@"mission_timer"    : @"1",
		@"head_roll"        : @"1",
		@"scope_steady"     : @"0",
		@"unlock_all"       : @"0",
		// Round 16: the XBLA switch's two keys (on by default, inert without
		// an import: the sibling's default).
		@"Video.XblaModels"   : @"1",
		@"Video.XblaTextures" : @"1",
	};
}

/// GoldenEye's own options, and the values DEFAULT_OPTIONS gives a fresh save
/// (src/game/file2.h:23 — auto-aim, sight on screen, look-ahead and the ammo
/// counter all on).
static NSDictionary<NSString *, NSString *> *geGameOptionFactory(void)
{
	return @{
		@"LookAhead"   : @"1",
		@"Crosshair"   : @"1",
		@"AutoAim"     : @"1",
		@"AmmoDisplay" : @"1",
	};
}

#pragma mark - the row model

typedef NS_ENUM(NSInteger, GERowKind) {
	GERowSwitch,
	GERowSlider,
	GERowSegmented,
	GERowInfo,      ///< read-only, right-aligned value, refreshed on a timer
	GERowAction,
	GERowChoice,    ///< D-089: title + the current choice as the detail; opens a picker
};

@interface GESettingsRow : NSObject
@property (nonatomic, copy)   NSString *title;
@property (nonatomic)         GERowKind kind;
@property (nonatomic)         GEStore store;
@property (nonatomic, copy, nullable) NSString *key;
/// D-141: more keys in the SAME store that a switch writes along with `key`,
/// and that must all be on for it to read on (one player switch over several
/// engine keys — "GoldenEye XBLA graphics").
@property (nonatomic, copy, nullable) NSArray<NSString *> *alsoKeys;
@property (nonatomic)         double minValue;
@property (nonatomic)         double maxValue;
/// Multiplies the stored value for DISPLAY only (100 turns 0.28 into 28 %).
@property (nonatomic)         double displayScale;
@property (nonatomic, copy, nullable) NSString *format;
@property (nonatomic, copy, nullable) NSArray<NSString *> *labels;
@property (nonatomic, copy, nullable) NSArray<NSString *> *values;
@property (nonatomic, copy, nullable) void (^onChanged)(void);
@property (nonatomic, copy, nullable) NSString *(^infoValue)(void);
@property (nonatomic, copy, nullable) void (^action)(void);
@property (nonatomic)         BOOL destructive;
/// Round 36 (visionOS Max FPS "Auto (N)"): when set, the segment shown as
/// selected comes from here instead of matching the stored value.
@property (nonatomic, copy, nullable) NSInteger (^selectedIndex)(void);
#if TARGET_OS_VISION
/// Round 49 (3D Settings): runs when a slider's thumb is let go (both
/// touch-up events: a gaze-pinch that drifts off the thumb ends Outside).
@property (nonatomic, copy, nullable) void (^onReleased)(void);
/// Whole-number slider (Render Resolution).
@property (nonatomic)         BOOL snap;
/// Writes the store only on release (Render Resolution re-wraps the eye).
@property (nonatomic)         BOOL applyOnRelease;
#endif
@end

@implementation GESettingsRow
- (instancetype)init
{
	if ((self = [super init])) {
		_displayScale = 1.0;
		_maxValue = 1.0;
	}
	return self;
}
@end

@interface GESettingsSection : NSObject
@property (nonatomic, copy, nullable) NSString *title;
@property (nonatomic, copy, nullable) NSString *footer;
@property (nonatomic, strong) NSMutableArray<GESettingsRow *> *rows;
#if TARGET_OS_VISION
/// Round 49: this section's header is "3D Settings | Reset" (the vp3d rows).
@property (nonatomic)         BOOL resetHeader;
#endif
@end

@implementation GESettingsSection
- (instancetype)init
{
	if ((self = [super init])) {
		_rows = [NSMutableArray array];
	}
	return self;
}
@end

#pragma mark - the Other App Audio picker (D-089)

/// The five-mode picker, the family's exactly (PD PDAudioModeViewController,
/// bean settings_uikit.mm RexAudioModeController): one subtitle cell per mode,
/// the name and one sentence saying what it does to the player's music.
/// Picking one writes the default, re-applies the session, shows the checkmark
/// and backs out after a beat so the choice is visibly taken.
@interface GEAudioModeViewController : UITableViewController
@property (nonatomic, copy, nullable) void (^onPick)(void);
@end

@implementation GEAudioModeViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Other App Audio";
	self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
	self.view.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1.0];
	self.tableView.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1.0];
	self.navigationItem.rightBarButtonItem =
		[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
		                                              target:self action:@selector(dismissSelf)];
}

- (void)dismissSelf { [self dismissViewControllerAnimated:YES completion:nil]; }

- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)section
{
	return (NSInteger)GEAudio.modeTitles.count;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
	                                            reuseIdentifier:nil];
	c.textLabel.text = GEAudio.modeTitles[(NSUInteger)ip.row];
	c.detailTextLabel.text = GEAudio.modeDetails[(NSUInteger)ip.row];
	// Wrap rather than truncate: the sentence is the whole point of the sheet.
	c.detailTextLabel.numberOfLines = 0;
	c.detailTextLabel.textColor = [UIColor colorWithWhite:0.68 alpha:1.0];
	c.accessoryType = (ip.row == GEAudio.currentMode)
		? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
	return c;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	[GEDefaults setString:[NSString stringWithFormat:@"%ld", (long)ip.row]
	               forKey:GEDefaultsAudioOtherApps];
	GELog(@"[settings] Other App Audio -> %ld %@", (long)ip.row,
	      GEAudio.modeTitles[(NSUInteger)ip.row]);
	[GEAudio settingsChanged];
	[tv reloadData];
	if (self.onPick) {
		self.onPick();
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)),
		dispatch_get_main_queue(), ^{ [self dismissSelf]; });
}

@end

#pragma mark - scroll that does not abort on iOS 27

@interface UIScrollView (GEScrollBegin)
- (void)_scrollViewWillBeginDragging;
- (BOOL)_scrollToTopIfPossible:(BOOL)animated;
- (void)animator:(id)animator startAnimation:(id)animation;
@end

/// What actually threw (round 23, R23-1, docs/port-ios/32 §1): every UIKit
/// path into _UIQOSManagedCommitsBegin creates UIKit's CA_CLIENT work interval
/// on the main thread the first time, and UIKit raises
/// NSInternalInconsistencyException "Failed to create work interval" when the
/// kernel refuses it. The kernel refused because GEPacing had already joined
/// the MAIN thread (Midas's sim thread) to its own `ge.frame` os_workgroup;
/// GEPacing no longer does that (the root fix). These catches are the
/// backstop for any other way the main thread could end up in an interval:
///   - _scrollViewWillBeginDragging: a drag begins (Austin's 0.0.0.1 log,
///     round 9d's catch);
///   - _scrollToTopIfPossible: the window-top / status-bar tap's
///     scroll-to-top (Austin's 0.0.0.6 log, tap on Done after an import);
///   - animator:startAnimation: every animated content-offset change (the
///     scroll-to-top and any setContentOffset:animated: go through it). Caught
///     innermost, so the animation itself still runs without the interval.
/// And no scroll view in this process scrolls to top on a status-bar tap at
/// all (scrollsToTop = NO from didMoveToWindow): nothing in the shell wants
/// it, and in landscape the tap region sits right on the sheet's Done button.
static void (*geOrigWillBeginDragging)(id, SEL);
static BOOL (*geOrigScrollToTop)(id, SEL, BOOL);
static void (*geOrigAnimatorStart)(id, SEL, id, id);
static void (*geOrigDidMoveToWindow)(id, SEL);
static unsigned geScrollCatches;

static void geScrollCaught(const char *where, NSException *e)
{
	++geScrollCatches;
	NSLog(@"projectmidas: [settings] caught %s exception #%u: %@ (%@)", where, geScrollCatches, e.name, e.reason);
	GELog(@"[settings] caught %s exception #%u: %@ (%@)", where, geScrollCatches, e.name, e.reason);
}

static void geWillBeginDragging(id self, SEL _cmd)
{
	@try {
		if (geOrigWillBeginDragging) {
			geOrigWillBeginDragging(self, _cmd);
		}
	} @catch (NSException *e) {
		geScrollCaught("scroll-begin", e);
	}
}

static BOOL geScrollToTop(id self, SEL _cmd, BOOL animated)
{
	@try {
		return geOrigScrollToTop ? geOrigScrollToTop(self, _cmd, animated) : NO;
	} @catch (NSException *e) {
		geScrollCaught("scroll-to-top", e);
		return NO;
	}
}

static void geAnimatorStart(id self, SEL _cmd, id animator, id animation)
{
	@try {
		if (geOrigAnimatorStart) {
			geOrigAnimatorStart(self, _cmd, animator, animation);
		}
	} @catch (NSException *e) {
		geScrollCaught("scroll-animation", e);
	}
}

static void geScrollDidMoveToWindow(id self, SEL _cmd)
{
	if (geOrigDidMoveToWindow) {
		geOrigDidMoveToWindow(self, _cmd);
	}
	UIScrollView *sv = self;
	if (sv.window && sv.scrollsToTop) {
		sv.scrollsToTop = NO;
	}
}

@interface GEScrollBeginGuard : NSObject
@end

@implementation GEScrollBeginGuard
+ (void)load
{
	Method m = class_getInstanceMethod(UIScrollView.class, @selector(_scrollViewWillBeginDragging));
	if (m) {
		geOrigWillBeginDragging = (void (*)(id, SEL))method_getImplementation(m);
		method_setImplementation(m, (IMP)geWillBeginDragging);
	}
	m = class_getInstanceMethod(UIScrollView.class, @selector(_scrollToTopIfPossible:));
	if (m) {
		geOrigScrollToTop = (BOOL (*)(id, SEL, BOOL))method_getImplementation(m);
		method_setImplementation(m, (IMP)geScrollToTop);
	}
	m = class_getInstanceMethod(UIScrollView.class, @selector(animator:startAnimation:));
	if (m) {
		geOrigAnimatorStart = (void (*)(id, SEL, id, id))method_getImplementation(m);
		method_setImplementation(m, (IMP)geAnimatorStart);
	}
	// didMoveToWindow is public and UIScrollView may not define its own: add
	// one that calls up to UIView's when it does not, so UIView is not changed.
	m = class_getInstanceMethod(UIScrollView.class, @selector(didMoveToWindow));
	if (m) {
		geOrigDidMoveToWindow = (void (*)(id, SEL))method_getImplementation(m);
		if (!class_addMethod(UIScrollView.class, @selector(didMoveToWindow),
		                     (IMP)geScrollDidMoveToWindow, method_getTypeEncoding(m))) {
			m = class_getInstanceMethod(UIScrollView.class, @selector(didMoveToWindow));
			geOrigDidMoveToWindow = (void (*)(id, SEL))method_getImplementation(m);
			method_setImplementation(m, (IMP)geScrollDidMoveToWindow);
		}
	}
	NSLog(@"projectmidas: [settings] scroll guards: drag %d, scroll-to-top %d, animation %d, scrollsToTop off",
	      geOrigWillBeginDragging != NULL, geOrigScrollToTop != NULL, geOrigAnimatorStart != NULL);
}
@end

/// The settings page's table. Round 9d scrolled it from a pan of its own
/// (content offset only, no deceleration of UIKit's) because the system pan's
/// drag-begin aborted on iOS 27. Round 23 found why (R23-1: GEPacing had put
/// the main thread in a work interval, so UIKit could not create its own) and
/// removed the cause, so the table is a plain UITableView again, with UIKit's
/// own tracking, deceleration and bounce (R23-4, Austin: "quite slow, just
/// scrolling through"). What is left here is the measurement: how often the
/// offset actually moves while it is moving, logged per burst (a run of
/// changes with no gap over 250 ms) to shell.log.
@interface GESettingsTable : UITableView
- (void)ge_noteScroll;
- (void)ge_flushBurst;
@end

@implementation GESettingsTable {
	double _burstT0, _burstLast, _burstMaxGap;
	unsigned _burstN;
	CGPoint _burstOff;
}

- (void)ge_flushBurst
{
	if (_burstN >= 8 && _burstLast > _burstT0) {
		const double ms = (_burstLast - _burstT0) * 1000.0;
		GELog(@"[settings] scroll burst: %u offsets in %.0f ms = %.1f/s, max gap %.1f ms",
		      _burstN, ms, (double)(_burstN - 1) * 1000.0 / ms, _burstMaxGap * 1000.0);
	}
	_burstN = 0;
	_burstMaxGap = 0.0;
}

// Sampled from the page's -scrollViewDidScroll: (UIKit's own tracking and
// deceleration do not go through -setContentOffset:).
- (void)ge_noteScroll
{
	const CGPoint off = self.contentOffset;
	const BOOL moved = !CGPointEqualToPoint(off, _burstOff);
	_burstOff = off;
	if (!moved) {
		return;
	}
	const double now = CACurrentMediaTime();
	if (_burstN && now - _burstLast > 0.25) {
		[self ge_flushBurst];
	}
	if (_burstN == 0) {
		_burstT0 = now;
	} else if (now - _burstLast > _burstMaxGap) {
		_burstMaxGap = now - _burstLast;
	}
	_burstLast = now;
	++_burstN;
}

- (void)willMoveToWindow:(UIWindow *)newWindow
{
	[super willMoveToWindow:newWindow];
	if (!newWindow) {
		[self ge_flushBurst];
	}
}

@end

#pragma mark - the view controller

// tag = kTagBase + section*1000 + row, so a control can find its own row
// without a per-cell object graph.
static const NSInteger kTagBase = 100000;
static const NSInteger kValueLabelTag = 7;

@interface GESettingsVC : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) NSArray<GESettingsSection *> *sections;
/// Declared here so +dismiss can call it: hiding a window is not guaranteed
/// to run -viewDidDisappear: on its root view controller.
- (void)stopInfoTimer;
/// R23-4: log the last scroll burst (shell.log) as the page goes.
- (void)flushScrollStats;
/// The table's own scroll numbers, for the gate (D-047).
- (NSString *)metricsLine;
/// D-089: the Other App Audio sheet, from the row or from the bridge.
- (void)presentAudioModePicker;
/// Round 36: re-run from +visionFollowRate: for the live "Auto (N)" label.
- (void)buildModel;
#if TARGET_OS_VISION
/// Round 49: hosted by the ornament gear's SwiftUI sheet (Done closes the
/// sheet) rather than by its own UIWindow.
@property (nonatomic) BOOL inSheet;
- (void)reload3D;
#endif
@end

@implementation GESettingsVC {
	NSTimer *_infoTimer;
#if TARGET_OS_VISION
	UIView *_pin3d;            // round 52: the pinned "3D Settings | Reset" bar
	BOOL _didInitialScroll;    // round 52: the 3D-mode open jump ran
	CGFloat _pinX, _pinW;      // round 52: the real header's last seen x/width
	__weak UIView *_real3dHeader;   // round 52: the section's own header view
#endif
}

#pragma mark building the model

/// Build a row only if its key actually resolves. The recomp port does the
/// same and logs the drop; it is the mechanism that stops a settings page
/// quietly writing into a key nothing reads (charter: "do not invent keys").
- (void)add:(GESettingsRow *)row to:(GESettingsSection *)section
{
	if (row.store != GEStoreNone && row.key && geStoreGet(row.store, row.key) == nil) {
		GELog(@"[settings] dropping row '%@' — no key named %@", row.title, row.key);
		return;
	}
	[section.rows addObject:row];
}

- (GESettingsRow *)switchRow:(NSString *)title store:(GEStore)store key:(NSString *)key
{
	GESettingsRow *r = [GESettingsRow new];
	r.title = title; r.kind = GERowSwitch; r.store = store; r.key = key;
	return r;
}

- (GESettingsRow *)sliderRow:(NSString *)title store:(GEStore)store key:(NSString *)key
                         min:(double)lo max:(double)hi scale:(double)scale format:(NSString *)fmt
{
	GESettingsRow *r = [GESettingsRow new];
	r.title = title; r.kind = GERowSlider; r.store = store; r.key = key;
	r.minValue = lo; r.maxValue = hi; r.displayScale = scale; r.format = fmt;
	return r;
}

- (GESettingsRow *)segRow:(NSString *)title store:(GEStore)store key:(NSString *)key
                   labels:(NSArray<NSString *> *)labels values:(NSArray<NSString *> *)values
{
	GESettingsRow *r = [GESettingsRow new];
	r.title = title; r.kind = GERowSegmented; r.store = store; r.key = key;
	r.labels = labels; r.values = values;
	return r;
}

- (GESettingsRow *)infoRow:(NSString *)title value:(NSString *(^)(void))value
{
	GESettingsRow *r = [GESettingsRow new];
	r.title = title; r.kind = GERowInfo; r.infoValue = value;
	return r;
}

/// A one-of-N row that shows the current choice and pushes a picker (the
/// family's ROW_CHOICE: vkQuake ios_settings.m, PD pdChoiceRow).
- (GESettingsRow *)choiceRow:(NSString *)title value:(NSString *(^)(void))value
                      action:(void (^)(void))action
{
	GESettingsRow *r = [GESettingsRow new];
	r.title = title; r.kind = GERowChoice; r.infoValue = value; r.action = action;
	return r;
}

- (GESettingsRow *)actionRow:(NSString *)title action:(void (^)(void))action
{
	GESettingsRow *r = [GESettingsRow new];
	r.title = title; r.kind = GERowAction; r.action = action;
	return r;
}

- (void)buildModel
{
	NSMutableArray<GESettingsSection *> *out = [NSMutableArray array];
	GESettingsSection *s;

	// ---- Touch controls ----------------------------------------------------
	s = [GESettingsSection new];
	s.title = @"Touch controls";
	s.footer = @"Drag fire to look keeps firing while that finger turns the view. "
	           @"Auto hides the on-screen controls while a controller is connected.";
	[self add:[self segRow:@"On-screen controls" store:GEStoreDefaults key:GEDefaultsTouchEnabled
	                labels:@[@"Auto", @"On", @"Off"] values:@[@"2", @"1", @"0"]] to:s];
	[self add:[self switchRow:@"Drag fire to look" store:GEStoreDefaults key:GEDefaultsFireDragLooks] to:s];
	[self add:[self sliderRow:@"Button opacity" store:GEStoreDefaults key:GEDefaultsButtonOpacity
	                      min:0.05 max:0.90 scale:100.0 format:@"%.0f%%"] to:s];
#if !TARGET_OS_VISION
	// No haptics on visionOS (GEVision.h), so no row for them.
	[self add:[self switchRow:@"Haptics" store:GEStoreDefaults key:GEDefaultsTouchHaptics] to:s];
#endif
	[self add:[self sliderRow:@"Stick dead zone" store:GEStoreDefaults key:GEDefaultsStickDeadzone
	                      min:0.05 max:0.40 scale:100.0 format:@"%.0f%%"] to:s];
	[self add:[self sliderRow:@"Stick curve" store:GEStoreDefaults key:GEDefaultsStickCurve
	                      min:0.0 max:1.0 scale:100.0 format:@"%.0f%%"] to:s];
	// Button SIZE is deliberately not here: it is the slider inside the
	// editor, where a player can see what they are resizing (recomp 0064
	// moved it out of the page for that reason).
	GESettingsRow *edit = [self actionRow:@"Customize layout…" action:^{
		// The page has to be gone first: the editor works on the live game
		// screen underneath it.
		[GESettings dismiss];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)),
		               dispatch_get_main_queue(), ^{
			[GETouchOverlay beginEditing];
		});
	}];
	[self add:edit to:s];
	[out addObject:s];

	// ---- Look --------------------------------------------------------------
	s = [GESettingsSection new];
	s.title = @"Look (touch)";
	s.footer = @"How far the view turns for each point you drag. Look ahead lets "
	           @"the view drift back to level while you walk, as on the N64.";
	[self add:[self sliderRow:@"Sensitivity — horizontal" store:GEStoreDefaults key:GEDefaultsLookDegPerPoint
	                      min:0.05 max:1.00 scale:1.0 format:@"%.2f°/pt"] to:s];
	[self add:[self sliderRow:@"Sensitivity — vertical" store:GEStoreDefaults key:GEDefaultsLookDegPerPointY
	                      min:0.05 max:1.00 scale:1.0 format:@"%.2f°/pt"] to:s];
	[self add:[self switchRow:@"Invert look (vertical)" store:GEStoreDefaults key:GEDefaultsLookInvert] to:s];
	[self add:[self switchRow:@"Look ahead (auto-centre)" store:GEStoreGameOption key:@"LookAhead"] to:s];
	[out addObject:s];

	// ---- Controller --------------------------------------------------------
	s = [GESettingsSection new];
	s.title = @"Controller";
	// D-237 (round 29d): the one place a player looks for "how do I play
	// multiplayer" — the game itself only greys the menu entry out.
	// Round 57 (docs/port-ios/77): every row here now reaches REAL
	// controllers. SDL owns those (round 9c) and Midas's own Look-stick code
	// turns the view, so the sensitivity/invert rows are pushed to the engine
	// (+[GEController applyPadLookToEngine]) and the dead zones ARE Midas's
	// own deadzone / deadzone_look options. The sensitivity keys still store
	// deg/s (factory 160); the row shows them as a multiple of the game's own
	// stick speed, 1.00× = Midas's rate (about 210°/s at full tilt).
	s.footer = @"For every controller, including each player's in split-screen. "
	           @"Sensitivity is how fast the right stick turns the view; 1.00× is the "
	           @"game's own speed. Dead zones are how far a stick moves before it counts. "
	           @"Multiplayer needs one controller per player.";
	[self add:[self sliderRow:@"Sensitivity — horizontal" store:GEStoreDefaults key:GEDefaultsPadLookDegPerSec
	                      min:40.0 max:400.0 scale:1.0 / GEPadLookFactoryDegPerSec format:@"%.2f×"] to:s];
	[self add:[self sliderRow:@"Sensitivity — vertical" store:GEStoreDefaults key:GEDefaultsPadLookDegPerSecY
	                      min:40.0 max:400.0 scale:1.0 / GEPadLookFactoryDegPerSec format:@"%.2f×"] to:s];
	[self add:[self switchRow:@"Invert look (vertical)" store:GEStoreDefaults key:GEDefaultsPadLookInvert] to:s];
	// Midas's raw units: of 32768, its list tops out at 50 % (port_options.c).
	[self add:[self sliderRow:@"Move stick dead zone" store:GEStoreEngine key:@"deadzone"
	                      min:0.0 max:16384.0 scale:100.0 / 32768.0 format:@"%.0f%%"] to:s];
	[self add:[self sliderRow:@"Look stick dead zone" store:GEStoreEngine key:@"deadzone_look"
	                      min:0.0 max:16384.0 scale:100.0 / 32768.0 format:@"%.0f%%"] to:s];
	[out addObject:s];

	// ---- Video -------------------------------------------------------------
	s = [GESettingsSection new];
	s.title = @"Video";
	// Round 34: the screen sentence follows the platform (no ProMotion on
	// the headset; its panel is 90 Hz and a 60 Hz pick shows at 45, R34-2).
#if TARGET_OS_VISION
	// Round 36: Auto follows the rate visionOS gives the app (90, 100, 120).
	NSString *screenNote = @"Auto follows the rate the headset gives the game (90 Hz, "
	                       @"sometimes 100 or 120); 90 holds 90, and 60 is shown at 45. "
	                       @"The game never runs faster than the display. ";
#else
	NSString *screenNote = @"120 needs a 120 Hz (ProMotion) screen, and the game never "
	                       @"runs faster than the screen. ";
#endif
	s.footer = [NSString stringWithFormat:@"%@%@%@",
	           @"Show FPS draws the rate in the corner. "
	           @"Max FPS also sets the game's tick rate, as on the PC; ",
	           screenNote,
	           @"2D upscale redraws the HUD, menus and text larger and smoother; "
	           @"2D smoothing blends their pixels. "
	           @"Classic Mode is the N64 look: low resolution, N64 edge "
	           @"antialiasing and 4:3 pillars."];
	[self add:[self switchRow:@"Show FPS" store:GEStoreEngine key:@"show_fps"] to:s];
	// Midas's maxfps also sets the SIM tick rate (one tick per frame,
	// videoCoupleTickToMaxFps), so this is Midas's own row, not the donor's
	// present-rate picker (doc 16 §2.3; the present-rate split is round 9c).
	// Round 18 (Phase 3a, docs/port-ios/27): 120 is offered only when the
	// panel can show it (ProMotion; GE_PANEL_HZ in dev builds). On a 60 Hz
	// panel the engine caps the tick at 60 anyway (video.c), so offering 120
	// there would be a choice that does nothing. And a stored value none of
	// the offers match (a hand-edited 0/40/90, or a 120 carried over to a
	// 60 Hz device) gets a segment of its own, selected, instead of a row with
	// nothing selected (the 9b review nit): the row always says what is set.
	{
		NSMutableArray<NSString *> *labels = [@[@"N64", @"30", @"60"] mutableCopy];
		NSMutableArray<NSString *> *values = [@[@"-1", @"30", @"60"] mutableCopy];
		// Round 34: the panel's maximum (90 on visionOS, 120 on ProMotion).
		const int fast = geIosFastHz();
		if (fast > 60) {
			NSString *f = [NSString stringWithFormat:@"%d", fast];
			[labels addObject:f];
			[values addObject:f];
		}
		NSString *cur = [geStoreGet(GEStoreEngine, @"maxfps")
			stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		NSScanner *sc = cur.length ? [NSScanner scannerWithString:cur] : nil;
		int curV = 0;
		if (sc && [sc scanInt:&curV] && sc.isAtEnd) {
			NSString *norm = [NSString stringWithFormat:@"%d", curV];
			if (![values containsObject:norm]) {
				[labels addObject:(curV == 0) ? @"No cap" : norm];
				[values addObject:norm];
			}
		}
		GESettingsRow *mf = [self segRow:@"Max FPS" store:GEStoreEngine key:@"maxfps"
		                          labels:labels values:values];
#if TARGET_OS_VISION
		// Round 36: visionOS adopts the rate it grants (GEPacing.m s_visionHz).
		// "Auto (N)" follows it (N = the live rate); a number is the player's
		// pick and holds. The fixed fast offer stays the 90 Hz floor.
		{
			NSMutableArray<NSString *> *vl = [mf.labels mutableCopy];
			NSMutableArray<NSString *> *vv = [mf.values mutableCopy];
			NSString *f90 = [NSString stringWithFormat:@"%d", (int)GEVisionMaxFPS()];
			const NSUInteger at = [vv indexOfObject:[NSString stringWithFormat:@"%d", fast]];
			if (at != NSNotFound && fast != (int)GEVisionMaxFPS()) {
				vl[at] = f90;
				vv[at] = f90;
			}
			[vl addObject:[NSString stringWithFormat:@"Auto (%d)", fast]];
			[vv addObject:@"auto"];
			mf.labels = vl;
			mf.values = vv;
			const NSInteger autoIdx = (NSInteger)vv.count - 1;
			NSArray<NSString *> *vals = [vv copy];
			mf.selectedIndex = ^NSInteger(void) {
				if (![GEDefaults boolFor:GEDefaultsMaxFpsUser]) {
					return autoIdx;
				}
				NSString *c = [geStoreGet(GEStoreEngine, @"maxfps")
					stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
				const NSUInteger i = c ? [vals indexOfObject:c] : NSNotFound;
				return i == NSNotFound ? UISegmentedControlNoSegment : (NSInteger)i;
			};
		}
#endif
		[self add:mf to:s];
	}
	[self add:[self segRow:@"Antialiasing" store:GEStoreEngine key:@"msaa"
	                labels:@[@"Off", @"2×", @"4×", @"8×"] values:@[@"1", @"2", @"4", @"8"]] to:s];
	[self add:[self segRow:@"3D resolution" store:GEStoreEngine key:@"low_res"
	                labels:@[@"N64", @"480", @"720", @"1080", @"Native"]
	                values:@[@"240", @"480", @"720", @"1080", @"0"]] to:s];
	[self add:[self segRow:@"Texture filtering" store:GEStoreEngine key:@"filtering"
	                labels:@[@"Point", @"Bilinear", @"3-point", @"Trilinear"]
	                values:@[@"point", @"bilinear", @"3point", @"trilinear"]] to:s];
	// Round 37: the HUD, menus and text are the N64's own small pictures;
	// "2D upscale" redraws them larger with the xBR pixel-art filter, and
	// "2D smoothing" filters them (off = crisp pixels).
	[self add:[self segRow:@"2D upscale" store:GEStoreEngine key:@"upscale_2d"
	                labels:@[@"Off", @"2×", @"3×", @"4×"] values:@[@"0", @"2", @"3", @"4"]] to:s];
	[self add:[self switchRow:@"2D smoothing" store:GEStoreEngine key:@"filter_2d"] to:s];
	[self add:[self segRow:@"Anisotropy" store:GEStoreEngine key:@"anisotropy"
	                labels:@[@"Off", @"2×", @"4×", @"8×", @"16×"]
	                values:@[@"0", @"2", @"4", @"8", @"16"]] to:s];
	[self add:[self sliderRow:@"Field of view" store:GEStoreEngine key:@"hfov_wide"
	                      min:70 max:140 scale:1.0 format:@"%.0f°"] to:s];
	[self add:[self segRow:@"Widescreen" store:GEStoreEngine key:@"widescreen"
	                labels:@[@"Stretch", @"Wide", @"4:3"] values:@[@"0", @"1", @"2"]] to:s];
	[self add:[self switchRow:@"Classic Mode" store:GEStoreEngine key:@"n64_aa"] to:s];
	[out addObject:s];

	// ---- Textures (round 16, XBLA step 5) ---------------------------------
	//
	// Ported from the sibling's "HD graphics" section (GoldenEye-Decomp-ios
	// app/ios/GESettings.m, round 19c / D-141, at 55a3a9c8). Two sources of
	// better pictures: "Texture pack" is the player's community pack
	// (Video.TexturePack); the XBLA switch is the whole remaster look at once,
	// Tier B's HD texture swaps (Video.XblaTextures) AND the level, prop, gun
	// and character meshes (Video.XblaModels). Both keys are live mid-level:
	// texpack.c's index rebuilds on the next frame, and beanstage tests its key
	// every draw. The switch writes both and reads on only when both are on.
	s = [GESettingsSection new];
	s.title = @"Textures";
	// Round 38: names the community pack and the unzip step (Austin,
	// 2026-10-25: players without the XBLA release use GoldenEye 007 HD).
	s.footer = @"No XBLA copy? Use the GoldenEye 007 HD pack: download the "
	           @"repository zip from github.com/GhostlyDark/GoldenEye-007-HD, put "
	           @"it in Files → Project Midas → texturepacks, tap it to unzip, then "
	           @"pick the folder under Texture pack. XBLA graphics are read from "
	           @"your own copy of the Xbox 360 release (the folder, the package or "
	           @"a .7z). With both on, a level the remaster redrew uses the "
	           @"remaster's art and the pack covers everything else; turn XBLA off "
	           @"to see the pack everywhere. Changes apply immediately.";
	__weak typeof(self) weakTex = self;
	GESettingsRow *pack = [self infoRow:@"Texture pack" value:^NSString *{
		return [GEXbla packLine];
	}];
	pack.action = ^{
		GESettingsVC *me = weakTex;
		if (!me) return;
		[GEXbla presentPackPickerFrom:me sourceView:me.view done:^{
			[weakTex.table reloadData];
		}];
	};
	[self add:pack to:s];
	GESettingsRow *xsw = [self switchRow:@"GoldenEye XBLA graphics" store:GEStoreEngine
	                                 key:@"Video.XblaModels"];
	xsw.alsoKeys = @[@"Video.XblaTextures"];
	[self add:xsw to:s];
	[self add:[self infoRow:@"GoldenEye XBLA" value:^NSString *{
		return [GEXbla statusLine];
	}] to:s];
	GESettingsRow *imp = [self actionRow:@"Import GoldenEye XBLA…" action:^{
		GESettingsVC *me = weakTex;
		if (!me) return;
		[GEXbla presentImporterFrom:me done:^(NSString *err) {
			if (!err) return;
			UIAlertController *a = [UIAlertController alertControllerWithTitle:@"GoldenEye XBLA"
				message:err preferredStyle:UIAlertControllerStyleAlert];
			[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
			[weakTex presentViewController:a animated:YES completion:nil];
		}];
	}];
	[self add:imp to:s];
	GESettingsRow *forget = [self actionRow:@"Remove imported XBLA" action:^{
		GESettingsVC *me = weakTex;
		if (!me) return;
		[GEXbla confirmForgetFrom:me done:^(NSString *err) {   // round 17: asks first
			if (err) {
				UIAlertController *a = [UIAlertController alertControllerWithTitle:@"GoldenEye XBLA"
					message:err preferredStyle:UIAlertControllerStyleAlert];
				[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
				[weakTex presentViewController:a animated:YES completion:nil];
			}
			[weakTex.table reloadData];
		}];
	}];
	forget.destructive = YES;
	[self add:forget to:s];
	[out addObject:s];

	// ---- Game --------------------------------------------------------------
	s = [GESettingsSection new];
	s.title = @"Game";
	s.footer = @"Crosshair, auto-aim and ammo on screen are GoldenEye's own "
	           @"options; the rest are Project Midas's.";
	[self add:[self switchRow:@"Crosshair" store:GEStoreGameOption key:@"Crosshair"] to:s];
	[self add:[self switchRow:@"Auto-aim" store:GEStoreGameOption key:@"AutoAim"] to:s];
	[self add:[self switchRow:@"Ammo on screen" store:GEStoreGameOption key:@"AmmoDisplay"] to:s];
	[self add:[self switchRow:@"Always crosshair" store:GEStoreEngine key:@"always_crosshair"] to:s];
	[self add:[self switchRow:@"Aim lock" store:GEStoreEngine key:@"aim_lock"] to:s];
	[self add:[self switchRow:@"Steady scope" store:GEStoreEngine key:@"scope_steady"] to:s];
	[self add:[self switchRow:@"Stamina sway" store:GEStoreEngine key:@"head_roll"] to:s];
	[self add:[self switchRow:@"Mission timer" store:GEStoreEngine key:@"mission_timer"] to:s];
	[self add:[self switchRow:@"Unlock all" store:GEStoreEngine key:@"unlock_all"] to:s];
	[out addObject:s];

	// ---- Audio -------------------------------------------------------------
	s = [GESettingsSection new];
	s.title = @"Audio";
	s.footer = @"Other App Audio decides what happens to music already playing "
	           @"when the game starts.";
	[self add:[self sliderRow:@"Music volume" store:GEStoreEngine key:@"music_volume"
	                      min:0 max:100 scale:1.0 format:@"%.0f%%"] to:s];
	[self add:[self sliderRow:@"Sound volume" store:GEStoreEngine key:@"sfx_volume"
	                      min:0 max:100 scale:1.0 format:@"%.0f%%"] to:s];
	{
		// D-089: the family's five-mode picker (GEAudio.h has the table),
		// replacing round 10d's single "Mix with other apps" switch.
		__weak typeof(self) weakSelf = self;
		[self add:[self choiceRow:@"Other App Audio" value:^NSString *{
			return GEAudio.modeTitles[(NSUInteger)GEAudio.currentMode];
		} action:^{
			[weakSelf presentAudioModePicker];
		}] to:s];
	}
	[out addObject:s];

	// ---- Diagnostics -------------------------------------------------------
	s = [GESettingsSection new];
	s.title = @"Diagnostics";
	s.footer = @"A hot phone slows the frame rate down. If the game ever crashes, "
	           @"its crash log is in Files → Project Midas.";
	[self add:[self infoRow:@"Frame rate" value:^NSString *{
		float fps = 0;
		geHostState(NULL, &fps, NULL, NULL);
		return (fps > 0) ? [NSString stringWithFormat:@"%.0f fps", fps] : @"measuring…";
	}] to:s];
	[self add:[self infoRow:@"Thermal state" value:^NSString *{
		switch (NSProcessInfo.processInfo.thermalState) {
		case NSProcessInfoThermalStateNominal:  return @"nominal";
		case NSProcessInfoThermalStateFair:     return @"fair";
		case NSProcessInfoThermalStateSerious:  return @"serious";
		case NSProcessInfoThermalStateCritical: return @"critical";
		default:                                return @"unknown";
		}
	}] to:s];
	[self add:[self infoRow:@"Crash log" value:^NSString *{
		NSString *p = [GEShell.shared.documentsPath stringByAppendingPathComponent:@"crash.txt"];
		return [NSFileManager.defaultManager fileExistsAtPath:p] ? @"in Files" : @"none";
	}] to:s];
	[self add:[self infoRow:@"Version" value:^NSString *{
		NSDictionary *info = NSBundle.mainBundle.infoDictionary;
		return [NSString stringWithFormat:@"%@ (%@)",
		        info[@"CFBundleShortVersionString"] ?: @"?", info[@"CFBundleVersion"] ?: @"?"];
	}] to:s];
#if !GE_PUBLIC
	// D-141: developer readouts, dev builds only — a player has no use for the
	// presenting rate or the audio session's category options.
	[self add:[self infoRow:@"Presenting at" value:^NSString *{
		return [NSString stringWithFormat:@"%d Hz (sim %d Hz)", GEPacing.engineHz, GEPacing.simHz];
	}] to:s];
	[self add:[self infoRow:@"Audio session" value:^NSString *{
		// Short: the bridge's `audio` prints the whole line; a phone row
		// wants the options and the gain the mixer is applying (D-089).
		AVAudioSession *a = AVAudioSession.sharedInstance;
		NSString *o = (a.categoryOptions & AVAudioSessionCategoryOptionDuckOthers) ? @"mix+duck"
		            : (a.categoryOptions & AVAudioSessionCategoryOptionMixWithOthers) ? @"mix"
		            : @"solo";
		NSString *c = [a.category stringByReplacingOccurrencesOfString:@"AVAudioSessionCategory"
		                                                    withString:@""];
		return [NSString stringWithFormat:@"%@ %@, game %.0f%%", c, o, 100.0 * audioGetHostGain()];
	}] to:s];
	// Dev builds only, and it says the port number because the whole point is
	// to be able to reach it (D-026: the bridge is compiled OUT of a public
	// build, so this row does not exist there at all).
	[self add:[self infoRow:@"Dev bridge" value:^NSString *{
		const char *env = getenv("GE_BRIDGE_PORT");
		return (env && *env) ? [NSString stringWithFormat:@"127.0.0.1:%s", env]
		                     : [NSString stringWithFormat:@"127.0.0.1:%d", GEVisionDefaultBridgePort()];
	}] to:s];
#endif
	[out addObject:s];

	// ---- Reset -------------------------------------------------------------
	s = [GESettingsSection new];
	s.footer = @"Restores only the settings on this page, including the touch "
	           @"layout. Your saved games and your ROM are not touched.";
	// __weak, or the cycle is closed: self holds the sections, a section holds
	// the row, the row holds this block and the block would hold self. The
	// page would then leak one view controller — and its 0.5 s info timer —
	// every time it was opened.
	__weak typeof(self) weakSelf = self;
	GESettingsRow *reset = [self actionRow:@"Reset all settings" action:^{
		[weakSelf confirmReset];
	}];
	reset.destructive = YES;
	[self add:reset to:s];
	[out addObject:s];

#if TARGET_OS_VISION
	[out addObject:[self build3DSection]];
#endif
	self.sections = out;
}

#if TARGET_OS_VISION
#pragma mark the 3D Settings section (round 49, Phase 6 M5)

/// The LAST section on visionOS, on every settings surface (PD D-082: "all ios
/// settings should show in the vision pro settings above the 3d settings").
/// Rows, ranges, defaults and wording are PD's (perfect-dark-ios
/// PDVisionSettingsViewController.m buildRows, from q2repro's SETTINGS-SPEC);
/// the storage is GEDefaults `vp3d.*` (metres, half-extents), the apply is
/// geVision3dApplySettings (GEVisionSettings.m). Lengths are SHOWN in the
/// Units row's unit, so a Units change rebuilds this section's formats.
- (GESettingsSection *)build3DSection
{
	GESettingsSection *s = [GESettingsSection new];
	s.title = @"3D Settings";
	s.resetHeader = YES;
	s.footer = @"The screen is placed in front of you when 3D starts and then stays put; "
	           @"Recenter Screen moves it to where you are looking now. Stereo Depth is how far "
	           @"apart the two eyes' cameras sit (0% is flat). Render Resolution is how many pixels the game "
	           @"draws for each eye; lower it if 3D stutters - it applies when you let go.";
	const BOOL ft = [GEDefaults boolFor:GEDefaults3DUnitsFeet];
	const double u = ft ? 3.28084 : 1.0;
	NSString *un = ft ? @"ft" : @"m";
	void (^live)(void) = ^{ geVision3dApplySettingsLive(); };
	void (^commit)(void) = ^{
		geVision3dApplySettings();
		// Panel Width / Height / Aspect read the eye, which is re-wrapped a
		// frame or two from now (PD): refresh them after the boundary.
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
		               dispatch_get_main_queue(), ^{ geVision3dSettingsReloadPages(); });
	};
	GESettingsRow *r;
	r = [self sliderRow:@"Screen Distance" store:GEStoreDefaults key:GEDefaults3DDistance
	                min:1.0 max:8.0 scale:u format:[@"%.1f " stringByAppendingString:un]];
	r.onChanged = live; r.onReleased = commit; [self add:r to:s];
	// Stored as the HALF-extent, shown as the full width/height.
	r = [self sliderRow:@"Screen Width" store:GEStoreDefaults key:GEDefaults3DHalfWidth
	                min:0.6 max:6.096 scale:2.0 * u format:[@"%.1f " stringByAppendingString:un]];
	r.onChanged = live; r.onReleased = commit; [self add:r to:s];
	r = [self sliderRow:@"Screen Height" store:GEStoreDefaults key:GEDefaults3DHalfHeight
	                min:0.5 max:3.0 scale:2.0 * u format:[@"%.1f " stringByAppendingString:un]];
	r.onChanged = live; r.onReleased = commit; [self add:r to:s];
	// Signed: 0 is eye level and the sign is the meaning of the row.
	r = [self sliderRow:@"Position Height" store:GEStoreDefaults key:GEDefaults3DPosHeight
	                min:-1.5 max:10.0 scale:u format:[@"%+.1f " stringByAppendingString:un]];
	r.onChanged = live; r.onReleased = commit; [self add:r to:s];
	r = [self sliderRow:@"Stereo Depth" store:GEStoreDefaults key:GEDefaults3DDepthPct
	                min:0.0 max:300.0 scale:1.0 format:@"%.0f%%"];
	r.onChanged = live; r.onReleased = commit; [self add:r to:s];
	// Round 56 (release prep, Austin's call): no Convergence row. The key
	// (GEDefaults3DConvergence, default 762 units = 25 ft), its apply, the
	// bridge's `3d set conv` / `3d settings get|set conv` and the header's
	// Reset all stay; only the slider is hidden.
	r = [self sliderRow:@"Surroundings Dimming" store:GEStoreDefaults key:GEDefaults3DDimming
	                min:0.0 max:1.0 scale:100.0 format:@"%.0f%%"];
	r.onChanged = live; r.onReleased = commit; [self add:r to:s];
	r = [self sliderRow:@"Render Resolution" store:GEStoreDefaults key:GEDefaults3DRenderPct
	                min:40.0 max:100.0 scale:1.0 format:@"%.0f%%"];
	r.snap = YES; r.applyOnRelease = YES; r.onReleased = commit; [self add:r to:s];
	[self add:[self infoRow:@"Panel Width" value:^NSString *{
		int w = 0, h = 0; geVisionEyeGetSize(&w, &h);
		return [NSString stringWithFormat:@"%d px", w];
	}] to:s];
	[self add:[self infoRow:@"Panel Height" value:^NSString *{
		int w = 0, h = 0; geVisionEyeGetSize(&w, &h);
		return [NSString stringWithFormat:@"%d px", h];
	}] to:s];
	[self add:[self infoRow:@"Aspect Ratio" value:^NSString *{
		int w = 0, h = 0; geVisionEyeGetSize(&w, &h);
		return (h > 0) ? [NSString stringWithFormat:@"%.2f:9", 9.0 * (double)w / (double)h] : @"—";
	}] to:s];
	// The engine's own counter: the Video section's Show FPS, the same key
	// (drawn in the 2D pass, so it lands in both eyes); Reset leaves it alone.
	__weak typeof(self) weakSelf = self;
	r = [self switchRow:@"FPS on Panel" store:GEStoreEngine key:@"show_fps"];
	r.onChanged = ^{ geHostFlushSaves(); [weakSelf.table reloadData]; };
	[self add:r to:s];
	r = [self segRow:@"Units" store:GEStoreDefaults key:GEDefaults3DUnitsFeet
	          labels:@[@"m", @"ft"] values:@[@"0", @"1"]];
	r.onChanged = ^{ geVision3dSettingsReloadPages(); };
	[self add:r to:s];
	[self add:[self actionRow:@"Recenter Screen" action:^{ geVision3dRecenter(); }] to:s];
	return s;
}

/// Rebuild the model (the Units row changes every length format) and redraw.
- (void)reload3D
{
	[self buildModel];
	[UIView performWithoutAnimation:^{ [self.table reloadData]; }];
	[self update3DPin];
}

- (void)reset3DTapped
{
	GELog(@"[3d] settings header Reset pressed");
	geVision3dSettingsResetDefaults();
}

- (UIView *)tableView:(UITableView *)tv viewForHeaderInSection:(NSInteger)section
{
	if (!self.sections[section].resetHeader) {
		return nil;   // the default title header
	}
	UIView *hv = [self make3DHeaderWidth:tv.bounds.size.width title:self.sections[section].title];
	_real3dHeader = hv;
	return hv;
}

/// Round 52: the index of the "3D Settings" section, or -1.
- (NSInteger)section3D
{
	for (NSInteger i = 0; i < (NSInteger)self.sections.count; i++) {
		if (self.sections[i].resetHeader) {
			return i;
		}
	}
	return -1;
}

/// Round 52 (R50-1 item 4): opened while in 3D, the page starts at the 3D
/// Settings section - the top of its header at the top of the table's visible
/// area. In 2D the page opens at the top, as PD's does (PD scrolls only on a
/// bridge request). A bottom inset is added when the section is too short for
/// its header to reach the top, so the jump is exact either way.
- (void)scrollTo3DSection
{
	const NSInteger s3 = [self section3D];
	if (s3 < 0) {
		return;
	}
	// Self-sizing rows: the header's rect is an ESTIMATE until the rows above
	// it have been laid out at their real heights, which scrolling there does.
	// So repeat until it stops moving (first sim run: off by 24 pt after one).
	CGRect hr = CGRectZero;
	for (int pass = 0; pass < 4; pass++) {
		[_table layoutIfNeeded];
		hr = [_table rectForHeaderInSection:s3];
		const UIEdgeInsets a = _table.adjustedContentInset;
		const CGFloat visible = _table.bounds.size.height - a.top - a.bottom;
		const CGFloat below = _table.contentSize.height - hr.origin.y;   // header top .. content end
		if (below < visible) {
			UIEdgeInsets ci = _table.contentInset;
			ci.bottom += visible - below;
			_table.contentInset = ci;
		}
		const CGFloat want = hr.origin.y - _table.adjustedContentInset.top;
		if (pass > 0 && fabs(_table.contentOffset.y - want) < 0.5) {
			break;
		}
		_table.contentOffset = CGPointMake(-_table.adjustedContentInset.left, want);
	}
	[self update3DPin];
	GELog(@"[settings] opened in 3D: scrolled to the 3D section (header y %.0f, offset %.0f)",
	      (double)hr.origin.y, (double)_table.contentOffset.y);
}

/// Round 52 (R50-1 item 5): while the 3D Settings section is on screen and
/// its own header has scrolled off the top, a pinned copy of the header sits
/// at the top of the table (pushed up by the section's end, the way a plain
/// table's sticky header leaves); hidden when the section is entirely above
/// or below the viewport. An overlay over the inset-grouped table, so the
/// table style does not change. Its Reset is the section's Reset.
- (void)update3DPin
{
	const NSInteger s3 = [self section3D];
	if (s3 < 0 || !_table) {
		_pin3d.hidden = YES;
		return;
	}
	const CGFloat pinH = 72.0;
	const CGRect hr = [_table rectForHeaderInSection:s3];
	const CGRect sr = [_table rectForSection:s3];
	const CGFloat top = _table.contentOffset.y + _table.adjustedContentInset.top;   // content y at the visible top
	const BOOL show = hr.origin.y < top - 0.5 && CGRectGetMaxY(sr) > top;
	if (!show) {
		_pin3d.hidden = YES;
		return;
	}
	if (!_pin3d) {
		_pin3d = [self make3DHeaderWidth:_table.bounds.size.width title:self.sections[s3].title];
		_pin3d.translatesAutoresizingMaskIntoConstraints = YES;
		_pin3d.layer.shadowColor = UIColor.blackColor.CGColor;
		_pin3d.layer.shadowOpacity = 0.5f;
		_pin3d.layer.shadowRadius = 6.0f;
		_pin3d.layer.shadowOffset = CGSizeMake(0, 3);
		// Under the page's own top bar (added before it? no - the bar is the
		// last subview; keep the pin just below it in z so the bar wins).
		UIView *bar = self.view.subviews.lastObject;
		[self.view insertSubview:_pin3d belowSubview:bar];
	}
	const CGFloat push = MIN(0.0, CGRectGetMaxY(sr) - top - pinH);
	const CGPoint o = [_table convertPoint:CGPointMake(0, _table.contentOffset.y) toView:self.view];
	// Same x/width as the section's real header view (the inset-grouped
	// table lays it out inside its readable margins, not edge to edge).
	UIView *real = (_real3dHeader.window != nil) ? _real3dHeader : nil;
	CGRect hx = real ? [real convertRect:real.bounds toView:self.view] : CGRectMake(_table.frame.origin.x, 0, _table.bounds.size.width, pinH);
	if (real) _pinX = hx.origin.x, _pinW = hx.size.width;
	if (_pinW > 0) hx.origin.x = _pinX, hx.size.width = _pinW;
	_pin3d.frame = CGRectMake(hx.origin.x, o.y + _table.adjustedContentInset.top + push, hx.size.width, pinH);
	_pin3d.hidden = NO;
}

/// Bridge (`3d settings pin [offset <y>]`): the pin's state, optionally after
/// putting the table at a content offset first.
- (NSString *)pinStateAfterOffset:(NSString *)y
{
	if (y) {
		_table.contentOffset = CGPointMake(_table.contentOffset.x, y.doubleValue);
		[self update3DPin];
	}
	const NSInteger s3 = [self section3D];
	const CGRect hr = s3 >= 0 ? [_table rectForHeaderInSection:s3] : CGRectZero;
	const CGRect sr = s3 >= 0 ? [_table rectForSection:s3] : CGRectZero;
	const CGFloat top = _table.contentOffset.y + _table.adjustedContentInset.top;
	return [NSString stringWithFormat:@"pin_visible=%d\npin_y=%.1f\nvisible_top=%.1f\nheader_y=%.1f\nsection_end=%.1f\n"
	        "offset=%.1f\nadj_top=%.1f\ncontent_h=%.1f\nbounds_h=%.1f\nin3d=%d\n",
	        (_pin3d && !_pin3d.hidden) ? 1 : 0,
	        _pin3d ? (double)_pin3d.frame.origin.y : -1.0, (double)top, (double)hr.origin.y, (double)CGRectGetMaxY(sr),
	        (double)_table.contentOffset.y, (double)_table.adjustedContentInset.top,
	        (double)_table.contentSize.height, (double)_table.bounds.size.height, geVision3dActive()];
}

- (UIView *)make3DHeaderWidth:(CGFloat)width title:(NSString *)title
{
	// "3D Settings | Reset" - vkQuake's header by way of PD/openQ4: a REAL
	// tinted pill (a bare text button is nearly impossible to gaze-pinch),
	// the button anchored and the label hung off its centre, opaque.
	UIView *hv = [[UIView alloc] initWithFrame:CGRectMake(0, 0, width, 72)];
	hv.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1.0];
	UILabel *l = [UILabel new];
	l.text = title;
	l.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
	l.textColor = UIColor.whiteColor;
	l.translatesAutoresizingMaskIntoConstraints = NO;
	[hv addSubview:l];
	UIButtonConfiguration *cfg = [UIButtonConfiguration tintedButtonConfiguration];
	cfg.title = @"Reset";
	cfg.baseForegroundColor = UIColor.systemOrangeColor;
	cfg.baseBackgroundColor = UIColor.systemOrangeColor;
	cfg.contentInsets = NSDirectionalEdgeInsetsMake(10, 22, 10, 22);
	UIButton *reset = [UIButton buttonWithConfiguration:cfg primaryAction:nil];
	reset.translatesAutoresizingMaskIntoConstraints = NO;
	[reset addTarget:self action:@selector(reset3DTapped) forControlEvents:UIControlEventTouchUpInside];
	[hv addSubview:reset];
	[NSLayoutConstraint activateConstraints:@[
		[l.leadingAnchor constraintEqualToAnchor:hv.leadingAnchor constant:20],
		[reset.trailingAnchor constraintEqualToAnchor:hv.trailingAnchor constant:-20],
		[reset.bottomAnchor constraintEqualToAnchor:hv.bottomAnchor constant:-10],
		[reset.topAnchor constraintGreaterThanOrEqualToAnchor:hv.topAnchor constant:8],
		[l.centerYAnchor constraintEqualToAnchor:reset.centerYAnchor],
		[l.trailingAnchor constraintLessThanOrEqualToAnchor:reset.leadingAnchor constant:-12],
	]];
	return hv;
}

- (CGFloat)tableView:(UITableView *)tv heightForHeaderInSection:(NSInteger)section
{
	return self.sections[section].resetHeader ? 72.0 : UITableViewAutomaticDimension;
}

- (void)sliderReleased:(UISlider *)sl
{
	GESettingsRow *row = [self rowForTag:sl.tag];
	if (!row) {
		return;
	}
	if (row.applyOnRelease) {
		[self applyRow:row value:[NSString stringWithFormat:@"%g", (double)sl.value]];
	}
	GELog(@"[3d] %@ released at %g", row.title, (double)sl.value);
	if (row.onReleased) {
		row.onReleased();
	}
}
#endif

- (void)presentAudioModePicker
{
	if (self.presentedViewController) {
		return;
	}
	GEAudioModeViewController *picker =
		[[GEAudioModeViewController alloc] initWithStyle:UITableViewStyleInsetGrouped];
	__weak typeof(self) weakSelf = self;
	picker.onPick = ^{ [weakSelf.table reloadData]; };
	UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:picker];
	nav.modalPresentationStyle = UIModalPresentationFormSheet;
	// The bar belongs to the navigation controller, not the picker: without
	// this its title drew black on the dark sheet (PD, same sheet).
	nav.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
	// Presented from THIS page's own window, never SDL's (the page is its
	// own UIWindow for exactly that reason, §Settings).
	[self presentViewController:nav animated:YES completion:nil];
	GELog(@"[settings] Other App Audio picker presented (current %ld %@)",
	      (long)GEAudio.currentMode, GEAudio.modeTitles[(NSUInteger)GEAudio.currentMode]);
}

- (void)confirmReset
{
	UIAlertController *a = [UIAlertController
		alertControllerWithTitle:@"Reset all settings?"
		                 message:@"Everything on this page returns to its default, "
		                          "including the touch layout. Saved games are not affected."
		          preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[a addAction:[UIAlertAction actionWithTitle:@"Reset" style:UIAlertActionStyleDestructive
	                                    handler:^(UIAlertAction *x) {
		(void)x;
		[GESettings resetAll];
		[self.table reloadData];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

#pragma mark view

- (void)viewDidLoad
{
	[super viewDidLoad];
	// Opaque, not a blur. A backdrop filter over the live CAMetalLayer is the
	// recomp port's recorded 30-second freeze on a thermally clamped device,
	// and there is nothing to see through here anyway.
	self.view.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1.0];
	self.view.opaque = YES;

	[self buildModel];

	_table = [[GESettingsTable alloc] initWithFrame:self.view.bounds style:UITableViewStyleInsetGrouped];
	_table.backgroundColor = UIColor.clearColor;
	_table.dataSource = self;
	_table.delegate = self;
	_table.translatesAutoresizingMaskIntoConstraints = NO;
	/*
	 * Round 12b / D-047. Austin, on 0.0.0.3: "the settings page can be swiped
	 * horizontally — it's drawn incorrectly or something; it should only
	 * scroll vertically."
	 *
	 * It was this line. `…AdjustmentAlways` applies the safe-area inset on
	 * BOTH axes, and a scroll view's scrollable extent is its content size
	 * PLUS its insets — so on a landscape phone the left and right insets
	 * (59 pt each on an iPhone Air) became 118 pt of horizontal travel on a
	 * view whose content is exactly as wide as its bounds. Nothing was
	 * mislaid out; the page was being handed room to move in an axis it has
	 * nothing to show in. `ScrollableAxes` — the UIKit default, and the
	 * reason it IS the default — adjusts only the axes that actually scroll,
	 * so the vertical inset the header bar needs is unaffected.
	 *
	 * The clamp in -scrollViewDidScroll: below is not belt-and-braces for
	 * this: an inset-grouped table can also be given horizontal travel by a
	 * cell whose accessory view is wider than the row (ours are fixed-width
	 * boxes and a long enough label in an accessibility text size would do
	 * it), and the rule Austin asked for is "vertically only", full stop.
	 */
	_table.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentScrollableAxes;
	_table.alwaysBounceHorizontal = NO;
	_table.showsHorizontalScrollIndicator = NO;
	_table.directionalLockEnabled = YES;
	[self.view addSubview:_table];

	// Our OWN header bar rather than a UINavigationController: the family
	// spec warns that relying on a hosted nav bar's Done is fragile, and this
	// window has no navigation stack to host one in anyway.
	UIView *bar = [UIView new];
	bar.backgroundColor = [UIColor colorWithWhite:0.0 alpha:1.0];
	bar.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:bar];

	UILabel *title = [UILabel new];
	title.text = @"Settings";
	title.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
	title.textColor = UIColor.whiteColor;
	title.translatesAutoresizingMaskIntoConstraints = NO;
	[bar addSubview:title];

	UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
	done.configuration = UIButtonConfiguration.filledButtonConfiguration;
	[done setTitle:@"Done" forState:UIControlStateNormal];
	done.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
	done.translatesAutoresizingMaskIntoConstraints = NO;
	[done addTarget:self action:@selector(doneTapped) forControlEvents:UIControlEventTouchUpInside];
	[bar addSubview:done];

	UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[_table.leadingAnchor  constraintEqualToAnchor:self.view.leadingAnchor],
		[_table.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[_table.topAnchor      constraintEqualToAnchor:self.view.topAnchor],
		[_table.bottomAnchor   constraintEqualToAnchor:self.view.bottomAnchor],

		[bar.leadingAnchor  constraintEqualToAnchor:self.view.leadingAnchor],
		[bar.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[bar.topAnchor      constraintEqualToAnchor:self.view.topAnchor],
		[bar.bottomAnchor   constraintEqualToAnchor:safe.topAnchor constant:52],

		[title.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20],
		[title.bottomAnchor  constraintEqualToAnchor:bar.bottomAnchor constant:-12],

		[done.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20],
		[done.centerYAnchor  constraintEqualToAnchor:title.centerYAnchor],
	]];
	// Pushed down by the bar's height rather than laid out under it, because
	// an inset-grouped table's first section header would otherwise sit
	// behind an opaque bar and simply not exist as far as the player knows.
	_table.contentInset = UIEdgeInsetsMake(52, 0, 0, 0);
	_table.verticalScrollIndicatorInsets = UIEdgeInsetsMake(52, 0, 0, 0);
}

- (void)viewDidAppear:(BOOL)animated
{
	[super viewDidAppear:animated];
	// The read-only rows only. 0.5 s is fast enough to watch a frame rate and
	// slow enough to cost nothing; the timer exists only while the page is up.
	//
	// +timerWithTimeInterval: and NOT +scheduledTimer…: the scheduled variant
	// adds the timer to the current run loop in the DEFAULT mode, and adding
	// it again for common modes then schedules the same timer twice — which
	// is a block firing at 4 Hz where the comment above says 2.
	//
	// Common modes because the shell rides on the engine's own host loop
	// (GEShell.h rule 2), and a plain default-mode timer stops firing while
	// the table is being dragged.
	__weak typeof(self) weakSelf = self;
	_infoTimer = [NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
		(void)t;
		[weakSelf refreshInfoRows];
	}];
	[NSRunLoop.mainRunLoop addTimer:_infoTimer forMode:NSRunLoopCommonModes];
#if TARGET_OS_VISION
	if (self.inSheet) {
		geIosPacingSetUiActive(1);   // R23-4: keep UIKit live while the page is up
		GELog(@"[settings] sheet page on screen frame=%@ sections=%ld (last: %@)",
		      NSStringFromCGRect(self.view.frame), (long)self.sections.count,
		      self.sections.lastObject.title);
	}
#endif
}

/// Hiding a window is not guaranteed to run the disappearance callbacks on
/// its root view controller, and a repeating timer the run loop still holds
/// is a leak that also keeps polling. +dismiss calls this directly.
- (void)stopInfoTimer
{
	[_infoTimer invalidate];
	_infoTimer = nil;
}

- (void)viewDidDisappear:(BOOL)animated
{
	[super viewDidDisappear:animated];
	[self stopInfoTimer];
#if TARGET_OS_VISION
	if (self.inSheet) {
		geIosPacingSetUiActive(0);
		geHostFlushSaves();
		GELog(@"[settings] sheet page gone");
	}
#endif
}

- (void)refreshInfoRows
{
	for (NSInteger si = 0; si < (NSInteger)self.sections.count; si++) {
		GESettingsSection *s = self.sections[si];
		for (NSInteger ri = 0; ri < (NSInteger)s.rows.count; ri++) {
			GESettingsRow *r = s.rows[ri];
			if (r.kind != GERowInfo) {
				continue;
			}
			UITableViewCell *cell =
				[_table cellForRowAtIndexPath:[NSIndexPath indexPathForRow:ri inSection:si]];
			cell.detailTextLabel.text = r.infoValue ? r.infoValue() : @"";
		}
	}
}

- (void)flushScrollStats
{
	[(GESettingsTable *)_table ge_flushBurst];
}

/// Vertical only, whatever a gesture or an inset thinks (D-047). The resting
/// x is -left inset, not 0: that is where a scroll view sits when it has no
/// horizontal travel, and forcing 0 would shift every row by the inset.
- (void)scrollViewDidScroll:(UIScrollView *)sv
{
	const CGFloat rest = -sv.adjustedContentInset.left;
	if (sv.contentOffset.x != rest) {
		sv.contentOffset = CGPointMake(rest, sv.contentOffset.y);
	}
	if ([sv isKindOfClass:GESettingsTable.class]) {
		[(GESettingsTable *)sv ge_noteScroll];
	}
#if TARGET_OS_VISION
	if (sv == _table) {
		[self update3DPin];
	}
#endif
}

#if TARGET_OS_VISION
- (void)viewDidLayoutSubviews
{
	[super viewDidLayoutSubviews];
	if (!_didInitialScroll && _table.window && _table.bounds.size.height > 0) {
		_didInitialScroll = YES;
		if (geVision3dActive()) {
			[self scrollTo3DSection];
		}
	}
	[self update3DPin];
}
#endif

/// What the gate asserts instead of eyeballing a screenshot: the table's own
/// numbers, in points. `h_travel` is the thing Austin's finger found, and it
/// must be 0.
- (NSString *)metricsLine
{
	[_table layoutIfNeeded];
	const CGSize b = _table.bounds.size;
	const CGSize c = _table.contentSize;
	const UIEdgeInsets a = _table.adjustedContentInset;
	const CGFloat hTravel = MAX(0.0, (c.width + a.left + a.right) - b.width);
	return [NSString stringWithFormat:
		@"page=settings bounds=%.0fx%.0f content=%.0fx%.0f "
		@"adj_inset=%.0f,%.0f,%.0f,%.0f offset=%.1f,%.1f h_travel=%.1f "
		@"inset_behavior=%ld\n",
		b.width, b.height, c.width, c.height,
		a.top, a.left, a.bottom, a.right,
		_table.contentOffset.x, _table.contentOffset.y, hTravel,
		(long)_table.contentInsetAdjustmentBehavior];
}

- (void)doneTapped
{
#if TARGET_OS_VISION
	if (self.inSheet) {
		geVision3dSettingsSheetRequest(0);
		return;
	}
#endif
	[GESettings dismiss];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIStatusBarStyle)preferredStatusBarStyle { return UIStatusBarStyleLightContent; }

#pragma mark table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv { return (NSInteger)self.sections.count; }

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)section
{
	return (NSInteger)self.sections[section].rows.count;
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)section
{
	return self.sections[section].title;
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)section
{
	return self.sections[section].footer;
}

- (GESettingsRow *)rowForTag:(NSInteger)tag
{
	NSInteger n = tag - kTagBase;
	NSInteger si = n / 1000, ri = n % 1000;
	if (si < 0 || si >= (NSInteger)self.sections.count) {
		return nil;
	}
	GESettingsSection *s = self.sections[si];
	return (ri >= 0 && ri < (NSInteger)s.rows.count) ? s.rows[ri] : nil;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	GESettingsRow *row = self.sections[ip.section].rows[ip.row];
	const NSInteger tag = kTagBase + ip.section * 1000 + ip.row;

	// Built, never dequeued: the cells carry live controls wired to a tag and
	// there are a few dozen of them in total, so reuse buys nothing and costs
	// a whole class of stale-accessory bugs.
	UITableViewCell *cell = [[UITableViewCell alloc]
		initWithStyle:((row.kind == GERowInfo || row.kind == GERowChoice)
		               ? UITableViewCellStyleValue1 : UITableViewCellStyleDefault)
	  reuseIdentifier:nil];
	cell.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.06];
	cell.textLabel.textColor = UIColor.whiteColor;
	cell.textLabel.text = row.title;
	cell.selectionStyle = UITableViewCellSelectionStyleNone;

	switch (row.kind) {
	case GERowSwitch: {
		UISwitch *sw = [UISwitch new];
		BOOL on = geStoreDouble(row.store, row.key) != 0.0;
		for (NSString *k in row.alsoKeys) {
			on = on && geStoreDouble(row.store, k) != 0.0;
		}
		sw.on = on;
		sw.tag = tag;
		[sw addTarget:self action:@selector(switchChanged:) forControlEvents:UIControlEventValueChanged];
		cell.accessoryView = sw;
		break;
	}
	case GERowSlider: {
#if TARGET_OS_VISION
		// Round 49: "40.0 ft" / "+32.8 ft" need a wider readout.
		UIView *box = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 290, 34)];
		UILabel *val = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 80, 34)];
#else
		UIView *box = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 260, 34)];
		UILabel *val = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 58, 34)];
#endif
		val.tag = kValueLabelTag;
		val.textAlignment = NSTextAlignmentRight;
		// Monospaced digits so the number does not jitter sideways while the
		// thumb drags — the family spec is explicit about this.
		val.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightRegular];
		val.textColor = UIColor.secondaryLabelColor;
		[box addSubview:val];

#if TARGET_OS_VISION
		UISlider *sl = [[UISlider alloc] initWithFrame:CGRectMake(88, 0, 202, 34)];
#else
		UISlider *sl = [[UISlider alloc] initWithFrame:CGRectMake(66, 0, 194, 34)];
#endif
		sl.minimumValue = (float)row.minValue;
		sl.maximumValue = (float)row.maxValue;
		sl.value = (float)geStoreDouble(row.store, row.key);
		sl.tag = tag;
		[sl addTarget:self action:@selector(sliderChanged:) forControlEvents:UIControlEventValueChanged];
#if TARGET_OS_VISION
		if (row.onReleased || row.applyOnRelease) {
			[sl addTarget:self action:@selector(sliderReleased:)
			     forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside];
		}
#endif
		[box addSubview:sl];

		val.text = [NSString stringWithFormat:row.format, sl.value * row.displayScale];
		cell.accessoryView = box;
		break;
	}
	case GERowSegmented: {
		UISegmentedControl *seg = [[UISegmentedControl alloc] initWithItems:row.labels];
		seg.tag = tag;
		// No selected segment when the stored value matches none of the
		// offered ones. Honest: silently snapping the value to the nearest
		// offer would rewrite a setting nobody touched.
		seg.selectedSegmentIndex = UISegmentedControlNoSegment;
		NSString *cur = geStoreGet(row.store, row.key);
		// Round 9c (9b review): text values first. "filtering" reads back
		// as "point"/"bilinear"/"trilinear", which all parse to 0, so a
		// numeric-only test showed every one as Point. The numeric test
		// (stored "60" vs offered "60.0") applies only when both parse.
		NSString *curTrim = [cur stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		for (NSUInteger i = 0; cur && i < row.values.count; i++) {
			if ([[row.values[i] description] caseInsensitiveCompare:curTrim] == NSOrderedSame) {
				seg.selectedSegmentIndex = (NSInteger)i;
				break;
			}
		}
		if (cur && seg.selectedSegmentIndex == UISegmentedControlNoSegment) {
			double cv = 0;
			NSScanner *sc = [NSScanner scannerWithString:curTrim];
			if ([sc scanDouble:&cv] && sc.isAtEnd) {
				for (NSUInteger i = 0; i < row.values.count; i++) {
					double ov = 0;
					NSScanner *so = [NSScanner scannerWithString:[row.values[i] description]];
					if ([so scanDouble:&ov] && so.isAtEnd && ov == cv) {
						seg.selectedSegmentIndex = (NSInteger)i;
						break;
					}
				}
			}
		}
		if (row.selectedIndex) {
			seg.selectedSegmentIndex = row.selectedIndex();
		}
		[seg addTarget:self action:@selector(segChanged:) forControlEvents:UIControlEventValueChanged];
		cell.accessoryView = seg;
		break;
	}
	case GERowInfo:
		if (row.action) {
			cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
		}
		cell.detailTextLabel.text = row.infoValue ? row.infoValue() : @"";
		cell.detailTextLabel.font =
			[UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular];
		cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
		cell.selectionStyle = row.action ? UITableViewCellSelectionStyleDefault
		                                 : UITableViewCellSelectionStyleNone;
		break;
	case GERowChoice:
		cell.detailTextLabel.text = row.infoValue ? row.infoValue() : @"";
		cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
		cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
		cell.selectionStyle = UITableViewCellSelectionStyleDefault;
		break;
	case GERowAction:
		cell.textLabel.textAlignment = NSTextAlignmentCenter;
		cell.textLabel.textColor = row.destructive ? UIColor.systemRedColor : UIColor.systemBlueColor;
		cell.selectionStyle = UITableViewCellSelectionStyleDefault;
		break;
	}
	return cell;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	GESettingsRow *row = self.sections[ip.section].rows[ip.row];
	// An info row may carry an action too (round 19c: "Texture pack" opens
	// its picker); a choice row opens its sheet (round 19a, D-089).
	if ((row.kind == GERowAction || row.kind == GERowInfo || row.kind == GERowChoice) && row.action) {
		row.action();
	}
}

#pragma mark control targets

- (void)applyRow:(GESettingsRow *)row value:(NSString *)value
{
#if TARGET_OS_VISION
	if (row.store == GEStoreEngine && [row.key isEqualToString:@"maxfps"] && [value isEqualToString:@"auto"]) {
		// Round 36: back to following the granted rate.
		[GEDefaults setString:@"0" forKey:GEDefaultsMaxFpsUser];
		[GEDefaults setString:@"1" forKey:GEDefaultsMaxFpsPanelDone];
		GELog(@"[settings] Max FPS -> Auto (%d)", geIosFastHz());
		[GESettings visionFollowRate:geIosFastHz()];
		[GEPacing applyTargetFPS];
		return;
	}
#endif
	geStoreSet(row.store, row.key, value);
	if (row.store == GEStoreEngine && [row.key isEqualToString:@"maxfps"]) {
		// R23-3: the player's own pick; the panel default never overrides it.
		[GEDefaults setString:@"1" forKey:GEDefaultsMaxFpsUser];
		[GEDefaults setString:@"1" forKey:GEDefaultsMaxFpsPanelDone];
		GELog(@"[settings] Max FPS -> %@ (player's pick)", value);
	}
	for (NSString *k in row.alsoKeys) {
		geStoreSet(row.store, k, value);
	}
	// A shell setting can change the overlay's geometry or its visibility,
	// and the overlay is the thing the player is looking at through this
	// page's shoulder. Cheap; do it for every change rather than maintaining
	// a list of which rows need it.
	[GETouchLayout reload];
	[GETouchOverlay applyVisibility];
	[GETouchOverlay.shared setNeedsDisplay];
	if (row.onChanged) {
		row.onChanged();
	}
}

- (void)switchChanged:(UISwitch *)sw
{
	GESettingsRow *row = [self rowForTag:sw.tag];
	if (row) {
		[self applyRow:row value:sw.on ? @"1" : @"0"];
	}
}

- (void)sliderChanged:(UISlider *)sl
{
	GESettingsRow *row = [self rowForTag:sl.tag];
	if (!row) {
		return;
	}
	// LIVE, on every value change: instant feedback is the whole point of a
	// sensitivity slider, and both stores are a word write.
#if TARGET_OS_VISION
	if (row.snap) {
		sl.value = roundf(sl.value);
	}
	if (row.applyOnRelease) {
		// Only the readout follows the thumb; sliderReleased: writes it.
		UILabel *rv = (UILabel *)[sl.superview viewWithTag:kValueLabelTag];
		rv.text = [NSString stringWithFormat:row.format, sl.value * row.displayScale];
		return;
	}
#endif
	// Midas's engine sliders are INT keys (hfov_wide, volumes): whole numbers.
	NSString *v = (row.store == GEStoreEngine)
		? [NSString stringWithFormat:@"%.0f", (double)sl.value]
		: [NSString stringWithFormat:@"%g", (double)sl.value];
	[self applyRow:row value:v];
	UILabel *val = (UILabel *)[sl.superview viewWithTag:kValueLabelTag];
	val.text = [NSString stringWithFormat:row.format, sl.value * row.displayScale];
}

- (void)segChanged:(UISegmentedControl *)seg
{
	GESettingsRow *row = [self rowForTag:seg.tag];
	if (!row || seg.selectedSegmentIndex < 0 ||
	    seg.selectedSegmentIndex >= (NSInteger)row.values.count) {
		return;
	}
	[self applyRow:row value:row.values[seg.selectedSegmentIndex]];
	if (row.store == GEStoreDefaults && [row.key isEqualToString:GEDefaultsTargetFPS]) {
		[GEPacing applyTargetFPS];
	}
}

@end

#pragma mark - the gear, and presenting

@interface GESettingsGearTarget : NSObject
@end
@implementation GESettingsGearTarget
- (void)tapped { [GESettings present]; }
@end

@implementation GESettings

static UIWindow *sWindow = nil;
static GESettingsVC *sVC = nil;
static UIButton *sGear = nil;
// UIButton does not retain its target, so the target has to outlive the
// stack frame that made it. Process-lifetime, exactly one.
static GESettingsGearTarget *sGearTarget = nil;
static NSTimer *sGearTimer = nil;
static CFTimeInterval sGameplaySince = 0.0;

+ (BOOL)visible { return sWindow != nil; }

+ (UIWindowScene *)scene
{
	UIWindowScene *any = nil;
	for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
		if (![s isKindOfClass:UIWindowScene.class]) {
			continue;
		}
		if (s.activationState == UISceneActivationStateForegroundActive) {
			return (UIWindowScene *)s;
		}
		if (!any) {
			any = (UIWindowScene *)s;
		}
	}
	return any;
}

+ (void)installGear
{
	UIView *host = GETouchOverlay.shared.superview;   // SDL's grafted window
	if (!host) {
		return;
	}
	// Round 11a: "already has a superview" is not the question — the overlay
	// can be re-homed onto a different window (GETouchOverlay +install), and a
	// gear left behind on the old one is a settings page nobody can reach.
	// Removing the button takes its constraints with it, so the block below
	// re-creates them against the new host.
	if (sGear.superview == host) {
		return;
	}
	[sGear removeFromSuperview];

	if (!sGear) {
		sGearTarget = [GESettingsGearTarget new];
		sGear = [UIButton buttonWithType:UIButtonTypeSystem];
		[sGear setImage:[UIImage systemImageNamed:@"gearshape.fill"] forState:UIControlStateNormal];
		sGear.tintColor = [UIColor colorWithWhite:1.0 alpha:0.55];
		sGear.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.30];
		sGear.layer.cornerRadius = 17.0;
		sGear.translatesAutoresizingMaskIntoConstraints = NO;
		[sGear addTarget:sGearTarget action:@selector(tapped)
		     forControlEvents:UIControlEventTouchUpInside];
	}

	// A SIBLING of the overlay, added after it so UIKit hit-tests it first,
	// and not a child — the overlay hides itself the moment a controller
	// connects (Touch.Enabled = auto), and that is precisely when a player
	// still has to be able to reach the settings.
	[host addSubview:sGear];
	[host bringSubviewToFront:sGear];

	// Top LEFT. The recomp port moved it there (0064) because the top right
	// is where START lives; ours is in the same corner for the same reason,
	// and the gate taps FIRE in the opposite corner.
	// Not safeAreaLayoutGuide — SDL's window reports a portrait-shaped inset
	// set in landscape, which is what put the gear ~68 pt too low on the
	// phone (GEShell.h geShellSafeInsets).
	UIEdgeInsets ins = geShellSafeInsets(host);
	[NSLayoutConstraint activateConstraints:@[
		[sGear.leadingAnchor constraintEqualToAnchor:host.leadingAnchor constant:ins.left + 12],
		[sGear.topAnchor     constraintEqualToAnchor:host.topAnchor constant:ins.top + 12],
		[sGear.widthAnchor   constraintEqualToConstant:34],
		[sGear.heightAnchor  constraintEqualToConstant:34],
	]];

	if (!sGearTimer) {
		sGearTimer = [NSTimer timerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *t) {
			(void)t;
			[GESettings updateGearVisibility];
		}];
		[NSRunLoop.mainRunLoop addTimer:sGearTimer forMode:NSRunLoopCommonModes];
	}
	GELog(@"[settings] gear installed (top-left, 34 pt, inset %.0f,%.0f)", ins.top, ins.left);
}

+ (void)updateGearVisibility
{
	if (!sGear) {
		return;
	}
	const BOOL playing = geHostInGameplay() != 0;
	if (!playing) {
		sGameplaySince = 0.0;
	} else if (sGameplaySince == 0.0) {
		sGameplaySince = CACurrentMediaTime();
	}
	// Hysteresis in one direction only: SHOWING is immediate, hiding waits
	// 0.4 s, so a stage transition or a pause cannot make the gear flicker.
	const BOOL settled = playing && (CACurrentMediaTime() - sGameplaySince) >= 0.4;
	const BOOL hide = settled || GETouchOverlay.isEditing || [GESettings visible];
	if (sGear.hidden != hide) {
		sGear.hidden = hide;
	}
}

+ (void)present
{
	if (sWindow) {
		return;
	}
	UIWindowScene *scene = [self scene];
	if (!scene) {
		return;
	}

	// Nothing may stay held across the transition: a finger that was on FIRE
	// when the page appeared has no touchesEnded to look forward to.
	[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourceTouch];
	geHostPortActionClear(0, -1);   // round 59: the touch source's Port actions too

	sVC = [GESettingsVC new];
	sWindow = [[UIWindow alloc] initWithWindowScene:scene];
	sWindow.rootViewController = sVC;
	// Its own window rather than a modal presentation on SDL's: SDL's window
	// has no view controller of ours to present from, and a full-screen modal
	// can take the game's CAMetalLayer out of the render tree (the recomp
	// port's recorded GPU stall). Below onboarding's +100, which must always
	// win — a settings page over "there is no ROM" would be absurd.
	sWindow.windowLevel = UIWindowLevelNormal + 90;
	[sWindow makeKeyAndVisible];
	geIosPacingSetUiActive(1);   // R23-4: keep UIKit live while the page is up
	[GESettings updateGearVisibility];
	GELog(@"[settings] page presented");
}

+ (void)dismiss
{
	if (!sWindow) {
		return;
	}
	[sVC stopInfoTimer];
	[sVC flushScrollStats];
	// The Other App Audio sheet, if it is up, goes with the page.
	[sVC dismissViewControllerAnimated:NO completion:nil];
	sWindow.hidden = YES;
	sWindow = nil;
	sVC = nil;
	geIosPacingSetUiActive(0);
	// The engine half of the page lives in ge007.ini, which the engine only
	// writes on its own schedule; a player who changes MSAA and then kills
	// the app should still find it changed.
	geHostFlushSaves();
	[GESettings updateGearVisibility];
	GELog(@"[settings] page dismissed");
}

+ (NSString *)bridgeMetrics
{
	if (!sVC) {
		return @"error=page-not-open\n";
	}
	return [sVC metricsLine];
}

+ (NSString *)bridgeAudioPicker
{
	if (!sVC) {
		return @"error=page-not-open\n";
	}
	[sVC presentAudioModePicker];
	return [NSString stringWithFormat:@"page=audio-picker mode=%ld\n", (long)GEAudio.currentMode];
}

+ (NSString *)bridgeTexpackPicker
{
	// Round 38: the Textures section's "Texture pack" row action, for the
	// simulator pass (the sheet cannot be reached by a simulated finger).
	if (!sVC) {
		return @"error=page-not-open\n";
	}
	[GEXbla presentPackPickerFrom:sVC sourceView:sVC.view done:nil];
	return [NSString stringWithFormat:@"page=texpack-picker line=%@\n", [GEXbla packLine]];
}

+ (NSString *)bridgeSetSwitch:(NSString *)title on:(BOOL)on
{
	if (!sVC) {
		return @"error=page-not-open\n";
	}
	for (NSInteger si = 0; si < (NSInteger)sVC.sections.count; si++) {
		NSArray<GESettingsRow *> *rows = sVC.sections[si].rows;
		for (NSInteger ri = 0; ri < (NSInteger)rows.count; ri++) {
			if (rows[ri].kind != GERowSwitch || ![rows[ri].title isEqualToString:title]) {
				continue;
			}
			NSIndexPath *ip = [NSIndexPath indexPathForRow:ri inSection:si];
			[sVC.table scrollToRowAtIndexPath:ip atScrollPosition:UITableViewScrollPositionMiddle animated:NO];
			[sVC.table layoutIfNeeded];
			UISwitch *sw = (UISwitch *)[sVC.table cellForRowAtIndexPath:ip].accessoryView;
			if (![sw isKindOfClass:UISwitch.class]) {
				return @"error=no-switch-cell\n";
			}
			if (sw.on != on) {
				[sw setOn:on animated:YES];
				[sw sendActionsForControlEvents:UIControlEventValueChanged];
			}
			NSMutableString *out = [NSMutableString stringWithFormat:@"switch=%@ on=%d", title, sw.on];
			for (NSString *k in [@[rows[ri].key ?: @""] arrayByAddingObjectsFromArray:rows[ri].alsoKeys ?: @[]]) {
				[out appendFormat:@" %@=%@", k, geStoreGet(rows[ri].store, k) ?: @"?"];
			}
			[out appendString:@"\n"];
			return out;
		}
	}
	return [NSString stringWithFormat:@"error=no-such-switch title=%@\n", title];
}

+ (NSString *)bridgeScrollToSection:(NSInteger)section
{
	if (!sVC) {
		return @"error=page-not-open\n";
	}
	NSInteger n = [sVC numberOfSectionsInTableView:sVC.table];
	if (section < 0 || section >= n) {
		return [NSString stringWithFormat:@"error=no-such-section section=%ld sections=%ld\n",
		        (long)section, (long)n];
	}
	[sVC.table scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:0 inSection:section]
	                 atScrollPosition:UITableViewScrollPositionTop animated:NO];
	// Lay out NOW: the caller's next act is a screenshot, and a scroll that
	// has not been committed photographs the previous group.
	[sVC.table layoutIfNeeded];
	return [NSString stringWithFormat:@"page=settings section=%ld/%ld title=%@\n",
	        (long)section, (long)n,
	        [sVC tableView:sVC.table titleForHeaderInSection:section] ?: @"(reset)"];
}

/// Round 23 (R23-3, Austin on 0.0.0.6: "ProMotion should be default if the
/// phone supports it"). Once per install, on the first frame (the ini is
/// loaded, and this runs from the main run loop like a settings-row change):
/// a >= 120 Hz panel whose maxfps is still the old default 60, on an install
/// where the player never picked a Max FPS, moves to 120. A fresh install is
/// the same case (no maxfps line reads back as the compiled 60). Anything else
/// the player has (30, N64, 120, a hand-edited value) is left alone. A 60 Hz
/// panel is never touched, and the engine caps any stored 120 to the panel
/// anyway (video.c videoCoupleTickToMaxFps, videoSetPresentHz).
/// Round 34 (M-V3): "a >= 120 Hz panel ... to 120" is now "a panel above 60
/// ... to its maximum", so an untouched visionOS install moves 60 -> 90 once
/// (0.0.0.10 never set the done flag there: its 90 Hz panel took the early
/// return). iOS is unchanged: 60 -> 120 on ProMotion, nothing on 60 Hz.
+ (void)applyPanelMaxFpsDefault
{
	// Round 34: any panel above 60 (90 on visionOS), to its own maximum.
	const int panel = geIosPanelMaxHz();
	const int fast = geIosFastHz();
	if (fast <= 60) {
		GELog(@"[settings] Max FPS panel default: %d Hz panel, left as is", panel);
		return;
	}
	if ([GEDefaults boolFor:GEDefaultsMaxFpsPanelDone]) {
		return;
	}
	[GEDefaults setString:@"1" forKey:GEDefaultsMaxFpsPanelDone];
	NSString *cur = [geStoreGet(GEStoreEngine, @"maxfps")
		stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
	if ([GEDefaults boolFor:GEDefaultsMaxFpsUser]) {
		GELog(@"[settings] Max FPS panel default: player picked %@, untouched", cur);
		return;
	}
	if (![cur isEqualToString:@"60"]) {
		GELog(@"[settings] Max FPS panel default: maxfps %@ is not the old default, untouched", cur);
		return;
	}
	geStoreSet(GEStoreEngine, @"maxfps", [NSString stringWithFormat:@"%d", fast]);
	GELog(@"[settings] Max FPS panel default: %d Hz panel, maxfps 60 -> %@", panel,
	      geStoreGet(GEStoreEngine, @"maxfps"));
}

/// Round 37 (docs/port-ios/50): once per install, on the first frame (with
/// the Max FPS default, GEPacing.m geMaxFpsDefaultOnce), the 2D layer gets
/// this platform's defaults: upscale_2d 2 on iOS / 4 on visionOS, and on
/// visionOS filter_2d on. An install from before round 37 has filter_2d=0 in
/// its midas.ini (the old compiled default, written back at every launch),
/// so the new visionOS default needs this push; upscale_2d is a new key and
/// already reads as the compiled default, so for it this only logs. A
/// player's later choice is never touched again.
+ (void)apply2DDefaults
{
	if ([GEDefaults boolFor:GEDefaultsUpscale2DDone]) {
		return;
	}
	[GEDefaults setString:@"1" forKey:GEDefaultsUpscale2DDone];
	NSString *up = geStoreGet(GEStoreEngine, @"upscale_2d");
	NSString *flt = geStoreGet(GEStoreEngine, @"filter_2d");
	if (![up isEqualToString:GE2DUpscaleDefault()]) {
		geStoreSet(GEStoreEngine, @"upscale_2d", GE2DUpscaleDefault());
	}
#if TARGET_OS_VISION
	/* Only visionOS moves filter_2d (its default is 1 for the big window). An
	 * iOS install keeps whatever 2D smoothing the player chose (review R37). */
	if (![flt isEqualToString:GE2DFilterDefault()]) {
		geStoreSet(GEStoreEngine, @"filter_2d", GE2DFilterDefault());
	}
#endif
	GELog(@"[settings] 2D defaults: upscale_2d %@ -> %@, filter_2d %@ -> %@", up,
	      geStoreGet(GEStoreEngine, @"upscale_2d"), flt, geStoreGet(GEStoreEngine, @"filter_2d"));
}

/// Round 58 (doc 78): the split-screen "PLAYER n" label (Midas's [display]
/// mp_show_names) now defaults to 0 on iOS/visionOS (video.c). video.c writes
/// every key back to midas.ini, so an install that ran 0.0.0.25 or earlier has
/// mp_show_names=1 saved; this moves it to 0 once per install, with the other
/// first-frame defaults. A player who turns it back on later keeps it.
+ (void)applyMpNamesDefault
{
	if ([GEDefaults boolFor:GEDefaultsMpNamesOffDone]) {
		return;
	}
	[GEDefaults setString:@"1" forKey:GEDefaultsMpNamesOffDone];
	NSString *cur = geStoreGet(GEStoreEngine, @"mp_show_names");
	if (![cur isEqualToString:@"0"]) {
		geStoreSet(GEStoreEngine, @"mp_show_names", @"0");
	}
	GELog(@"[settings] MP player names default: mp_show_names %@ -> %@", cur,
	      geStoreGet(GEStoreEngine, @"mp_show_names"));
}

/// Round 36 (visionOS): the pacer adopted `hz` (GEPacing.m geVisionNoteGrant;
/// the engine's panel cap is already `hz`). Unless the player picked a Max FPS,
/// maxfps follows it, which moves the sim tick through Midas's own live path
/// (video.c videoCoupleTickToMaxFps, as a settings-row change). A picked value
/// is written back unchanged, only so the tick re-couples to the new cap.
+ (void)visionFollowRate:(int)hz
{
	NSString *cur = [geStoreGet(GEStoreEngine, @"maxfps")
		stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
	if ([GEDefaults boolFor:GEDefaultsMaxFpsUser]) {
		if (cur.length) {
			geStoreSet(GEStoreEngine, @"maxfps", cur);
		}
		GELog(@"[settings] vision rate %d Hz: player's Max FPS %@ kept", hz, cur);
		return;
	}
	NSString *want = [NSString stringWithFormat:@"%d", hz];
	if (![cur isEqualToString:want]) {
		geStoreSet(GEStoreEngine, @"maxfps", want);
		GELog(@"[settings] vision rate %d Hz: maxfps %@ -> %@ (Auto)", hz, cur, want);
	}
	if (sVC) {
		[sVC buildModel];   // the "Auto (N)" label
		[sVC.table reloadData];
	}
}

+ (void)resetAll
{
	[GEDefaults resetAll];
	NSDictionary *eng = geEngineFactory();
	for (NSString *k in eng) {
		geHostConfigSet(k.UTF8String, [eng[k] UTF8String]);
	}
	NSDictionary *opt = geGameOptionFactory();
	for (NSString *k in opt) {
		geHostGameOptionSet(k.UTF8String, [opt[k] intValue]);
	}
	// GEDefaults.resetAll already dropped Touch.Layout, but the live table is
	// a memcpy of it and has to be told (main thread; this is called from a
	// UIAlertAction handler).
	[GETouchLayout reload];
	[GETouchOverlay applyVisibility];
	[GETouchOverlay.shared setNeedsDisplay];
	[GEAudio settingsChanged];   // the mode may have just gone back to 2
	[GEPacing applyTargetFPS];
#if TARGET_OS_VISION
	geVision3dApplySettings();   // round 49: the vp3d keys are shell keys too
#endif
	geHostFlushSaves();
	GELog(@"[settings] reset: %lu shell keys, %lu engine keys, %lu game options",
	      (unsigned long)GEDefaults.all.count, (unsigned long)eng.count, (unsigned long)opt.count);
}

@end

#if TARGET_OS_VISION
#pragma mark - the sheet's page and the 3D reload (round 49)

static __weak GESettingsVC *sSheetVC;

UIViewController *geVision3dMakeSettingsPage(void)
{
	GESettingsVC *vc = [GESettingsVC new];
	vc.inSheet = YES;
	sSheetVC = vc;
	GELog(@"[settings] page built for the ornament sheet");
	return vc;
}

void geVision3dSettingsReloadPages(void)
{
	if (!NSThread.isMainThread) {
		dispatch_async(dispatch_get_main_queue(), ^{ geVision3dSettingsReloadPages(); });
		return;
	}
	[sVC reload3D];
	[sSheetVC reload3D];
}

/// Bridge (round 52): `3d settings pin [offset <y>]` - the pinned header's
/// state on the sheet's page.
NSString *geVision3dSettingsPinState(NSString *offset)
{
	GESettingsVC *vc = sSheetVC;
	if (!vc || !vc.table) {
		return @"error=sheet-not-open\n";
	}
	return [vc pinStateAfterOffset:offset];
}

/// Bridge: scroll the SHEET's page to its last (3D) section, for a screenshot.
NSString *geVision3dSettingsScrollSheet(void)
{
	GESettingsVC *vc = sSheetVC;
	if (!vc || !vc.table) {
		return @"error=sheet-not-open\n";
	}
	const NSInteger last = (NSInteger)vc.sections.count - 1;
	[vc.table scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:0 inSection:last]
	                atScrollPosition:UITableViewScrollPositionTop animated:NO];
	[vc.table layoutIfNeeded];
	NSMutableString *rows = [NSMutableString string];
	for (GESettingsRow *r in vc.sections[last].rows) {
		[rows appendFormat:@"%@|", r.title];
	}
	return [NSString stringWithFormat:@"sheet_page section=%ld title=%@ rows=%@\n",
	        (long)last, vc.sections[last].title, rows];
}
#endif
