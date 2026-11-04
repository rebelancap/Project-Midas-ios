// GESceneDelegate.m — the UIScene lifecycle, and the graft SDL needs.
//
// WHY IT EXISTS AT ALL (PD's D-038, inherited whole). Xcode 27 links against
// the iOS 27 SDK, and iOS refuses to run an app linked on that SDK that has no
// UIApplicationSceneManifest: UIKit trips
// __UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption on the
// FrontBoard scene-creation path and the process dies with EXC_BREAKPOINT
// before a single line of ours runs. It is the LINKED SDK that is checked, not
// the deployment target and not the device — so this applies to the simulator
// too, and "it worked yesterday" stopped being evidence when the toolchain
// moved. TN3187.
//
// WHAT IT IS NOT. It is not a port of the app onto scenes. SDL 2.32.8 has no
// scene support whatsoever, it owns the UIApplicationDelegate, and it creates
// its UIWindow the pre-scene way with -initWithFrame:. So this class does
// exactly two things: it exists and is named by the manifest, so the launch-time
// check passes; and it grafts SDL's sceneless window onto the scene, which is
// what puts the game on screen at all (GEShell.m).
//
// The graft also runs from a timer started in ge_ios_main.m, because the scene
// connects long before SDL's window exists: SDL builds it inside videoInit(),
// which is inside the engine, which starts after UIApplication is already up.
//
// ROUND 45 (Phase 6 M1): iOS ONLY. On visionOS the whole class is compiled out
// (the file stays in the target list). GEVisionApp.swift is that target's
// @main and SwiftUI declares its scenes; UIKit persists a scene session's
// delegate CLASS NAME across installs, and Austin's headset has run Phase-5
// builds whose sessions name GESceneDelegate - with the class absent the
// lookup fails and UIKit falls back to SwiftUI's scenes (PD D-047, q2repro
// NOTES-FROM-VKQUAKE :15-45). Its jobs on visionOS: the graft is GEShell's
// 100 ms watch; the save flush and the audio/overlay reassert are scene
// notification observers in ge_ios_main.m geRegisterLifecycle().
#import "GEVision.h"

#if !TARGET_OS_VISION

#import "GESceneDelegate.h"
#import "GEShell.h"
#import "GEAudio.h"
#import "GETouchOverlay.h"
#import "GEOnboarding.h"

// port/include/hostinput.h
extern void geHostFlushSaves(void);

@implementation GESceneDelegate

- (void)scene:(UIScene *)scene
	willConnectToSession:(UISceneSession *)session
	options:(UISceneConnectionOptions *)options
{
	GELog(@"[scene] connected %@ (role %@, state %ld)",
		session.persistentIdentifier, session.role, (long)scene.activationState);
	geGraftSDLWindows();
	// Round 57: a cold launch BY a file (AirDrop, Files' "Open in") carries
	// it here rather than in scene:openURLContexts:.
	if (options.URLContexts.count) {
		[self scene:scene openURLContexts:options.URLContexts];
	}
}

/// Round 57 (docs/port-ios/77): the ROM as an openable document
/// (CFBundleDocumentTypes in Info.plist). Onboarding's 500 ms poll picks the
/// copied file up and starts the engine.
- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts
{
	(void)scene;
	for (UIOpenURLContext *ctx in URLContexts) {
		GELog(@"[scene] open %@ (from %@, open in place %d)", ctx.URL.lastPathComponent,
		      ctx.options.sourceApplication ?: @"?", ctx.options.openInPlace ? 1 : 0);
		if (ctx.URL.isFileURL) {
			[GEOnboarding importROMAtURL:ctx.URL];
		}
	}
}

- (void)sceneDidBecomeActive:(UIScene *)scene
{
	(void)scene;
	// Cheap, idempotent, and the moment a window that missed the graft at
	// connect time (SDL's, which does not exist yet then) would be visible.
	geGraftSDLWindows();

	// Coming back from Files is exactly when a ROM appears, and coming back
	// from a phone call is exactly when the audio session is gone. Onboarding
	// polls on its own as well (a download can finish while we are already
	// active), but this makes the common case instant.
	[GEAudio reassert];
	[GETouchOverlay applyVisibility];
}

- (void)sceneWillResignActive:(UIScene *)scene
{
	(void)scene;
	// SYNCHRONOUS, on this thread, before returning. iOS gives an app a short
	// window here and then may kill it without another run-loop turn, so a
	// dispatch_async would be a save that sometimes happens.
	//
	// ge007.ini is written here; ge007.eep needs nothing, because
	// port/src/libultra.c rewrites it in full on every osEepromLongWrite — the
	// save is already on disk the moment the game makes it. Both halves are
	// verified by killing the app after a save and relaunching
	// (docs/shell.md §Lifecycle).
	geHostFlushSaves();
	GELog(@"[scene] resigning active — ge007.ini flushed");
}

@end

#endif // !TARGET_OS_VISION
