// ge_ios_main.m — the iOS process entry point for Project Midas.
//
// Copied from GoldenEye-Decomp-ios app/ios/ge_ios_main.m (55a3a9c8), round 9b.
// Midas changes: GEXbla since round 16 (XBLA step 5), no ge007.ini container
// migrations (Midas keeps
// midas.ini and no container predates it), and the R9A-5 seeding of
// gamecontrollerdefault.txt + Data/shaders_base.txt into Documents.
//
// Order of business, and why each thing is where it is:
//
//   1. publish Documents / Caches / the bundle in the environment, which is
//      where port/src/system.c's sysResolvePath() picks the three data roots
//      up ($S / $C / $E, D-016). Before anything else, because the crash log,
//      the launch beacon and the engine all write through them.
//   2. write a launch beacon, so a run that dies before the first engine log
//      line is still distinguishable from an app that never started;
//   3. assemble argv: the process arguments (xcrun simctl launch --args ...)
//      plus Documents/ge.args, one flag per line, because a device has no
//      command line;
//   4. register the lifecycle hooks — the present gate (GEPacing) and the
//      renderer's suspend/resume, which are what stop an eglSwapBuffers into a
//      CAMetalLayer drawable the system has taken back;
//   5. hand off to SDL_UIKitRunApp, which brings up UIApplication and calls
//      back into geSDLMain on the same thread;
//   6. in geSDLMain, start the graft watch (GEShell.h explains why the graft is
//      load-bearing and why it cannot be done once at scene-connect time), then
//      geEngineMain(), which never returns.
//
// Round 8a added, between 5 and 6: the shell's own services (defaults, audio
// session, controller watch, dev bridge), then ONBOARDING — which blocks in
// geSDLMain, running the main run loop, until Documents holds a usable NTSC-U
// ROM. The engine is only ever entered with a ROM it can open, which is why
// there is no "the app vanished a second after launch" path any more.
//
// Round 45 (Phase 6 M1): steps 1-4 are geShellPrepare(), shared by both entry
// points. iOS keeps main() -> SDL_UIKitRunApp; visionOS has no main() here at
// all - app/vision3d/GEVisionApp.swift is its @main and GEHostViewController
// calls geShellPrepare() + geSDLMain() (docs/port-ios/64 §2.1, PD D-047).
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

// SDL_main.h does `#define main SDL_main` on iOS, which would quietly rename
// the function below and leave the executable with no entry point at all
// ("Undefined symbols: _main", referenced from <initial-undefines>). We provide
// main() ourselves and call SDL_UIKitRunApp() by hand, which is the whole of
// what SDL2main's iOS main() does. (PD docs/build.md §Traps.)
#define SDL_MAIN_HANDLED 1
#include <SDL.h>
#include <SDL_main.h>
#include <SDL_hints.h>

#import "GEShell.h"
#import "GEPacing.h"
#import "GEOnboarding.h"
#import "GEDefaults.h"
#import "GEController.h"
#import "GETouchOverlay.h"
#import "GEAudio.h"
#import "GEBridge.h"
#import "GEInput.h"
#import "GEXbla.h"
#import "GEVision.h"
#if TARGET_OS_VISION
#import "GEVision3D.h"   // round 48: geVision3dReconcileForeground
#endif

// src/game/image.c — D-067.
extern void geSetStageTexPoolBytes(unsigned int n);

// port/include/hostinput.h
extern void geHostFlushSaves(void);

// app/gfx/gfx_angle_egl.mm
extern void geAngleSuspend(void);
extern void geAngleResume(void);

static NSString *geDocs(void) { return GEShell.shared.documentsPath; }

/**
 * Written before anything else can fail. If this file is present and the engine
 * produced no output, the process died between UIApplication coming up and the
 * first log line — which is a different bug from never launching at all.
 */
static void geWriteLaunchBeacon(NSString *docs)
{
	NSString *path = [docs stringByAppendingPathComponent:@"launch-beacon.txt"];
	NSString *text = [NSString stringWithFormat:
		@"projectmidas-ios launch\nversion: %@\nwhen: %@\ndocuments: %@\ncaches: %@\n",
		NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"],
		[NSDate date], docs, GEShell.shared.cachesPath];
	[text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

/**
 * Extra launch flags from Documents/ge.args, one per line, '#' comments
 * ignored.
 *
 * The simulator can pass a command line (`xcrun simctl launch --args`) and a
 * device cannot, so every flag the port accepts (-level_33, --fixed-step and
 * the rest of the seeded-replay set when round 7a lands it) needs a second way
 * in that survives a Files drop.
 */
static NSArray<NSString *> *geArgsFromFile(NSString *docs)
{
	NSString *path = [docs stringByAppendingPathComponent:@"ge.args"];
	NSString *body = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
	if (!body) {
		return @[];
	}
	NSMutableArray *out = [NSMutableArray array];
	for (NSString *rawLine in [body componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
		NSString *line = [rawLine stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		if (line.length == 0 || [line hasPrefix:@"#"]) {
			continue;
		}
		for (NSString *word in [line componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet]) {
			if (word.length) {
				[out addObject:word];
			}
		}
	}
	return out;
}

/**
 * R9A-5 (doc 16 §7). portRootFopen() reads fall back to the bundle, but
 * gamecontrollerdefault.txt and Data/shaders_base.txt are opened through
 * portRootPath() (Documents only, no fallback), so a first run copies them
 * from the bundle. Never overwrites: a player may edit either in Files.
 */
static void geSeedDocuments(NSString *docs)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *bundle = NSBundle.mainBundle.resourcePath;
	NSArray<NSArray<NSString *> *> *pairs = @[
		@[ @"gamecontrollerdefault.txt", @"gamecontrollerdefault.txt" ],
		@[ @"shaders_base.txt",          @"Data/shaders_base.txt" ],
	];
	for (NSArray<NSString *> *p in pairs) {
		NSString *from = [bundle stringByAppendingPathComponent:p[0]];
		NSString *to = [docs stringByAppendingPathComponent:p[1]];
		if ([fm fileExistsAtPath:to] || ![fm fileExistsAtPath:from]) {
			continue;
		}
		[fm createDirectoryAtPath:to.stringByDeletingLastPathComponent
		  withIntermediateDirectories:YES attributes:nil error:NULL];
		NSError *err = nil;
		if ([fm copyItemAtPath:from toPath:to error:&err]) {
			GELog(@"[seed] %@ -> Documents/%@", p[0], p[1]);
		} else {
			GELog(@"[seed] %@ FAILED: %@", p[1], err.localizedDescription);
		}
	}
}

static void geRegisterLifecycle(void)
{
	NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;

	// Both the app-level and the scene-level pair. In a scene-based app the
	// app-level notifications are a compatibility courtesy rather than a
	// contract, and both halves here are idempotent, so observing both costs
	// nothing and losing the gate costs a crash.
	void (^background)(NSNotification *) = ^(NSNotification *note) {
		(void)note;
		GELog(@"background — closing the present gate");
		geIosPacingSetForeground(0);
		geAngleSuspend();
	};
	void (^foreground)(NSNotification *) = ^(NSNotification *note) {
		(void)note;
		GELog(@"foreground — resuming");
		geAngleResume();
		geIosPacingSetForeground(1);
	};

	[nc addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:background];
	[nc addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:nil usingBlock:foreground];
	[nc addObserverForName:UISceneDidEnterBackgroundNotification object:nil queue:nil usingBlock:background];
	[nc addObserverForName:UISceneWillEnterForegroundNotification object:nil queue:nil usingBlock:foreground];

#if TARGET_OS_VISION
	// Round 45 (Phase 6 M1): GESceneDelegate is compiled OUT of the visionOS
	// target (the persisted-session trap, docs/port-ios/64 §2.1), so its two
	// lifecycle jobs move here. visionOS ONLY: on iOS the delegate still does
	// them, byte-for-byte as before, and nothing here runs twice there.
	// The graft needs nothing - GEShell's 100 ms watch re-asks it for ever.
	//
	// The save flush: SYNCHRONOUS, on the posting (main) thread, before
	// returning - the delegate's sceneWillResignActive rule. Any window scene
	// deactivating counts (the SwiftUI window, or the space opening/closing);
	// a flush with nothing dirty is free.
	[nc addObserverForName:UISceneWillDeactivateNotification object:nil queue:nil
	            usingBlock:^(NSNotification *note) {
		if (![note.object isKindOfClass:UIWindowScene.class]) {
			return;
		}
		geHostFlushSaves();
		GELog(@"[scene] resigning active — midas.ini flushed");
	}];
	[nc addObserverForName:UISceneDidActivateNotification object:nil queue:nil
	            usingBlock:^(NSNotification *note) {
		if (![note.object isKindOfClass:UIWindowScene.class]) {
			return;
		}
		[GEAudio reassert];
		[GETouchOverlay applyVisibility];
		geGraftSDLWindows();
		// Round 48 (Phase 6 M4): a space lost while we were in the background,
		// or an exit whose un-park never landed, comes back as sane 2D here.
		geVision3dReconcileForeground();
	}];
#endif
}

int geSDLMain(int argc, char *argv[])
{
	@autoreleasepool {
		UIApplication.sharedApplication.idleTimerDisabled = YES;

		// SDL's UIKitRunApp chdir()s to the app BUNDLE before calling us,
		// undoing the chdir(Documents) main() did. That matters because the
		// port still has CWD-relative output paths — GE_PCDUMP's ppm/ and the
		// bridge's `screenshot` among them — and on the SIMULATOR the bundle
		// is writable, so the frames land inside GoldenEye64.app and the only
		// symptom is a file that is not where anyone looks. On a DEVICE the
		// bundle is read-only and the same code silently writes nothing at
		// all. Put the CWD back, here, after SDL has had its turn.
		chdir(GEShell.shared.documentsPath.fileSystemRepresentation);

		// Upstream's inputInit() turns on every SDL HIDAPI driver hint, Steam
		// included. On iOS SDL's HIDAPI backend creates a CBCentralManager the
		// moment the game-controller subsystem starts, and iOS TERMINATES an app
		// that touches CoreBluetooth without NSBluetoothAlwaysUsageDescription
		// (PD shipped two builds that died exactly there on device; the
		// simulator does not enforce it, so a green gate proves nothing).
		// HIDAPI on iOS only ever meant Steam Controllers over BLE; pads arrive
		// through GCController and SDL's own iOS joystick backend. OVERRIDE
		// priority outranks the engine's later plain SDL_SetHint calls.
		SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI, "0", SDL_HINT_OVERRIDE);
		SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "0", SDL_HINT_OVERRIDE);
		// Midas round 9b: Midas's input_pc.c opens EVERY SDL joystick, and
		// SDL's iOS backend offers the accelerometer as one by default — it
		// took player 1 on the first simulator launch ("raw joystick 'iOS
		// Accelerometer' -> player 1"), which would steer Bond by tilting
		// the phone. The donor never opened raw joysticks, so it never had to.
		SDL_SetHintWithPriority(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0", SDL_HINT_OVERRIDE);

		// Order matters. Defaults first (everything else reads them); the
		// audio session before SDL opens a device, so our category is the one
		// SDL has to argue with rather than the other way round; the bridge
		// before onboarding, so a gate can ask `state` and be told "rom=missing"
		// instead of timing out.
		[GEDefaults registerDefaults];

		// D-067. The STAGE texture pool is sized from this global when a stage
		// loads; 0 leaves the N64's own 0x6DDD0. Pushed before the engine starts
		// so a value set last session is honoured on a cold boot.
		{
			NSInteger kb = [GEDefaults intFor:GEDefaultsTexPoolKB];
			if (kb > 0) {
				geSetStageTexPoolBytes((unsigned)(kb * 1024));
			}
		}
		// Before the engine reads anything from Documents (R9A-5).
		geSeedDocuments(GEShell.shared.documentsPath);
		// Round 16: Documents/texturepacks and Documents/xbla (excluded from
		// iCloud backup), there to drop into from Files. The engine's
		// xblaImportAtStartup rebuilds an evicted Caches/xbla from the latter.
		[GEXbla prepareFolders];
		[GEAudio start];
		[GEController start];
		[GEBridge start];

		// Blocks here — running the main run loop, so UIKit stays alive and a
		// Files drop is noticed — until the ROM is right. Returns at once when
		// it already is.
		[GEOnboarding runUntilRomIsReady];

		// D-034: hand the engine the PATH of whatever file onboarding
		// accepted, whatever it is called. romdata.c tries this before its own
		// fixed-name candidate list, so a player never has to rename anything.
		{
			extern void romdataSetRomPath(const char *path);
			NSString *rom = GEOnboarding.acceptedRomPath;
			if (rom.length) {
				romdataSetRomPath(rom.fileSystemRepresentation);
				GELog(@"engine ROM path -> %@", rom);
			}
		}

		[GEShell startGraftWatch];
		[GEPacing start];
		GEShell.shared.engineRunning = YES;
	}

	return geEngineMain(argc, argv);
}

/**
 * Everything main() did before handing the process to SDL, in one function
 * both entry points call (round 45, Phase 6 M1; PD's pdShellPrepare, D-047).
 *
 * On iOS, main() below runs this and then SDL_UIKitRunApp, which brings up
 * UIApplication and calls back into geSDLMain. On visionOS there is no main()
 * at all: an ImmersiveSpace can only be declared by a SwiftUI `App`, so
 * app/vision3d/GEVisionApp.swift owns the process entry and
 * GEHostViewController calls this and then geSDLMain directly, from a run-loop
 * timer. Nothing in here depends on which of the two called it.
 *
 * Returns the assembled argv (NULL-terminated, strdup'd, never freed - it
 * outlives the process) and writes its count to *outArgc.
 */
char **geShellPrepare(int argc, char *argv[], int *outArgc)
{
	int n = 0;
	char **argvOut = NULL;

	@autoreleasepool {
		NSString *docs = geDocs();
		NSString *caches = GEShell.shared.cachesPath;
		NSString *bundle = GEShell.shared.bundlePath;

		// The three data roots, exactly as port/src/system.c reads them.
		setenv("GE_IOS_DOCUMENTS", docs.fileSystemRepresentation, 1);
		setenv("GE_IOS_CACHES", caches.fileSystemRepresentation, 1);
		setenv("GE_IOS_BUNDLE", bundle.fileSystemRepresentation, 1);
		// port/src/dram.c's dual-DRAM backing store is an open()+unlink()ed
		// file (shm_open is EPERM in the iOS sandbox, measured —
		// docs/spike-cartbase.md §F). GE_CACHE_DIR is a HOME-shaped root that
		// it appends /Library/Caches to, NOT the cache dir itself — so it gets
		// the container root. $HOME is already that on iOS; setting it
		// explicitly means the engine never has to guess the layout.
		setenv("GE_CACHE_DIR", NSHomeDirectory().fileSystemRepresentation, 1);

		[NSFileManager.defaultManager createDirectoryAtPath:docs
		                        withIntermediateDirectories:YES attributes:nil error:NULL];
		[NSFileManager.defaultManager createDirectoryAtPath:caches
		                        withIntermediateDirectories:YES attributes:nil error:NULL];

		// An iOS process starts with its working directory at "/", which is
		// read-only. The port still has a handful of CWD-relative paths — most
		// importantly GE_PCDUMP's `ppm/` output, which is the instrument that
		// answers "is the engine drawing, or is the compositor not showing it"
		// — so the CWD becomes Documents and those all land somewhere the user
		// can reach through Files.
		chdir(docs.fileSystemRepresentation);

		// Nothing in this app should die because a socket peer went away, and
		// the default disposition of SIGPIPE is to kill the process.
		signal(SIGPIPE, SIG_IGN);

		// Before ANY engine code can print: from here on the engine's stdout
		// and stderr land in Documents/engine.log instead of /dev/null
		// (GEShell.h, D-055). Round 14a — bg.c's D69 "room geometry WILL be
		// missing (silently)" warning has never been readable on a device.
		geEngineLogStart(docs);

		geWriteLaunchBeacon(docs);

		NSArray<NSString *> *extra = geArgsFromFile(docs);
		int total = argc + (int)extra.count;
		argvOut = (char **)calloc((size_t)total + 1, sizeof(char *));
		for (int i = 0; i < argc; i++) {
			argvOut[n++] = strdup(argv[i]);
		}
		for (NSString *a in extra) {
			argvOut[n++] = strdup(a.UTF8String);
		}
		argvOut[n] = NULL;

		NSMutableString *shown = [NSMutableString string];
		for (int i = 0; i < n; i++) {
			[shown appendFormat:@"%s ", argvOut[i]];
		}
		GELog(@"documents %@", docs);
		GELog(@"caches    %@", caches);
		GELog(@"argv      %@", shown);

		geRegisterLifecycle();
	}

	if (outArgc) {
		*outArgc = n;
	}
	return argvOut;
}

#if !TARGET_OS_VISION
int main(int argc, char *argv[])
{
	int n = 0;
	char **argvOut = geShellPrepare(argc, argv, &n);
	return SDL_UIKitRunApp(n, argvOut, geSDLMain);
}
#endif
