// GEOnboarding.h — "there is no game in here yet, and this is exactly what to do".
//
// The engine's answer to a missing ROM is a log line and a non-zero return
// from romdataInit(), which on a phone is an app that vanishes a second after
// the icon stops bouncing. Onboarding runs BEFORE geEngineMain() is ever
// called, so the engine only ever starts with a ROM it can use.
//
// Round 10c (D-034) rewrote what "a ROM it can use" means. 0.0.0.1 demanded
// one exact filename in one exact folder, which was Austin's device report,
// item 4: "the ROM instructions on first launch are terrible. You shouldn't
// have to rename the z64 to be an exact name, and you should be able to
// select a ROM rather than move one in." So now:
//
//   * ANY filename. Everything ROM-shaped in Documents is classified by its
//     N64 header, and the winner's PATH is handed to the engine
//     (romdataSetRomPath) rather than the engine being told to look for one
//     magic name.
//   * .n64 and .v64 are ACCEPTED, not refused: the classifier already knew
//     the two swapped byte orders in order to complain about them, so it
//     un-swaps the file into Documents and uses that instead.
//   * A **Choose ROM…** button opens the system document picker, so the file
//     can come from iCloud Drive, a USB stick or another app without ever
//     going near the Files app.
//
// Classification stays verdict-shaped, because the answers a player needs are
// different from each other and "ROM not found" is none of them.
#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, GERomVerdict) {
	GERomVerdictOK = 0,        // NTSC-U, the one the port runs
	GERomVerdictMissing,       // nothing ROM-shaped in Documents at all
	GERomVerdictWrongRegion,   // GoldenEye, PAL or NTSC-J
	GERomVerdictByteSwapped,   // GoldenEye, .n64/.v64 order — converted in place
	GERomVerdictUnknown,       // an N64 ROM, or a file, that is not GoldenEye
	GERomVerdictTooSmall,      // a partial copy — a Files drop still in flight
};

@interface GERomCheck : NSObject
@property (nonatomic) GERomVerdict verdict;
/// The file this verdict is about — an ABSOLUTE path, whatever it is called.
@property (nonatomic, copy, nullable) NSString *path;
@property (nonatomic, copy, nullable) NSString *sha1;      // log only, never shown
@property (nonatomic, copy) NSString *headline;
@property (nonatomic, copy) NSString *detail;
@property (nonatomic) unsigned long long bytes;
@end

@interface GEOnboarding : NSObject

/// The absolute path of the ROM that was accepted, or nil. Set by +check as
/// soon as a usable file appears; ge_ios_main.m hands it to romdataSetRomPath().
@property (class, readonly, nullable) NSString *acceptedRomPath;

/// The SHA-1 of the NTSC-U cartridge. For the LOG, not for the screen: a
/// player who has to be told a hash has already been failed by the screen.
@property (class, readonly) NSString *expectedSHA1;

/// Classify what is in Documents right now — every ROM-shaped file in it, not
/// one fixed name. Cheap to call repeatedly: each file's classification is
/// cached against (path, size, mtime), so the work runs once per actual change
/// rather than once per poll.
+ (GERomCheck *)check;

/// Classify one arbitrary file (the document picker's pick). Does not copy.
+ (GERomCheck *)classifyFileAtPath:(NSString *)path;

/// Show the onboarding window (idempotent), or update it in place with a new
/// verdict. Main thread.
+ (void)presentWithCheck:(GERomCheck *)check;

/// Open the system document picker on the onboarding screen, exactly as the
/// **Choose ROM…** button does. Main thread. Returns NO when onboarding is not
/// on screen. This exists so the GATE can exercise the picker: simctl cannot
/// tap UIKit and `idb ui tap` is dead on iOS 27, so without a seam the one
/// control on the first screen every new player sees would ship untested.
+ (BOOL)presentDocumentPicker;

/// Round 57 (docs/port-ios/77): bring one file in from outside — the
/// document picker, AirDrop, or Files' "Open in" (iOS GESceneDelegate
/// scene:openURLContexts:, visionOS SwiftUI onOpenURL). Classifies it like the
/// Documents poll, copies it into Documents (moves it out of Documents/Inbox),
/// and returns the verdict. A refusal is shown on the onboarding screen until
/// another file arrives. Does nothing but log when a ROM is already accepted.
/// Main thread.
+ (GERomCheck *)importROMAtURL:(NSURL *)url;

/// Tear it down once the ROM is good. Main thread.
+ (void)dismiss;

/// Block here until Documents holds a usable ROM, running the main run loop
/// the whole time so UIKit stays alive, the picker works and Files can drop a
/// file in. Returns immediately when the ROM is already good.
+ (void)runUntilRomIsReady;

@end

NS_ASSUME_NONNULL_END
