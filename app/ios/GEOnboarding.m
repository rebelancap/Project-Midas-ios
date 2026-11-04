// GEOnboarding.m — see GEOnboarding.h.
#import "GEOnboarding.h"
#import "GEShell.h"

#import <CommonCrypto/CommonDigest.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// engine/README.md §Requirements. Kept for the LOG line and for the "this is
// the exact reference dump" confirmation; the screen never shows a hash.
static NSString *const kSHA1_NTSCU = @"abe01e4aeb033b6c0836819f549c791b26cfde83";

// The cartridge is 12 MiB (12,582,912 bytes — work/gamedata-PROVENANCE.md;
// GoldenEye is a 96 Mbit cart, not a 128 Mbit one). Anything smaller is a
// truncated copy, which is what a Files transfer looks like while it is still
// running, and it is worth saying so rather than calling it "not GoldenEye".
static const unsigned long long kRomBytes = 12u * 1024u * 1024u;

// Where a converted .n64/.v64 lands. The player's own file is never touched:
// it is theirs, it may be on iCloud, and silently rewriting someone's library
// is not this app's business.
static NSString *const kConvertedName = @"goldeneye-converted.z64";

@implementation GERomCheck
@end

#pragma mark - header classification

typedef NS_ENUM(NSInteger, GEByteOrder) {
	GEOrderUnknown = 0,
	GEOrderZ64,      // 80 37 12 40 — big endian, what the engine reads
	GEOrderV64,      // 37 80 40 12 — half-word swapped
	GEOrderN64,      // 40 12 37 80 — word swapped (little endian)
};

static GEByteOrder geOrderOfMagic(const unsigned char *b)
{
	if (b[0] == 0x80 && b[1] == 0x37 && b[2] == 0x12 && b[3] == 0x40) return GEOrderZ64;
	if (b[0] == 0x37 && b[1] == 0x80 && b[2] == 0x40 && b[3] == 0x12) return GEOrderV64;
	if (b[0] == 0x40 && b[1] == 0x12 && b[2] == 0x37 && b[3] == 0x80) return GEOrderN64;
	return GEOrderUnknown;
}

/// Rewrite `buf` (a whole number of 4-byte words) into z64 order.
static void geToZ64(unsigned char *buf, size_t len, GEByteOrder order)
{
	if (order == GEOrderV64) {
		for (size_t i = 0; i + 1 < len; i += 2) {
			unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
		}
	} else if (order == GEOrderN64) {
		for (size_t i = 0; i + 3 < len; i += 4) {
			unsigned char a = buf[i], b = buf[i + 1];
			buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2];
			buf[i + 2] = b;      buf[i + 3] = a;
		}
	}
}

/// SHA-1 of a file, streamed in 1 MiB chunks. 12 MiB would fit in memory, but
/// a mapped read of a file the user is still copying in over Files is exactly
/// how you get a SIGBUS in a handler you did not write.
static NSString *geSHA1OfFile(NSString *path)
{
	NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
	if (!fh) {
		return nil;
	}
	CC_SHA1_CTX ctx;
	CC_SHA1_Init(&ctx);
	while (1) {
		@autoreleasepool {
			NSData *chunk = [fh readDataOfLength:1024 * 1024];
			if (chunk.length == 0) {
				break;
			}
			CC_SHA1_Update(&ctx, chunk.bytes, (CC_LONG)chunk.length);
		}
	}
	[fh closeFile];
	unsigned char out[CC_SHA1_DIGEST_LENGTH];
	CC_SHA1_Final(out, &ctx);
	NSMutableString *hex = [NSMutableString stringWithCapacity:40];
	for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) {
		[hex appendFormat:@"%02x", out[i]];
	}
	return hex;
}

#pragma mark -

@interface GEOnboardingVC : UIViewController <UIDocumentPickerDelegate>
@property (nonatomic, strong) UILabel *headline;
@property (nonatomic, strong) UILabel *detail;
@property (nonatomic, strong) UILabel *ways;
@property (nonatomic, strong) UIButton *choose;
- (void)applyCheck:(GERomCheck *)check;
- (void)pickFile;
@end

static NSString *sAcceptedPath = nil;
/// Round 57: the verdict of the last file handed to us (picker, AirDrop, Files
/// "Open in") that was refused. Shown while Documents itself has nothing, so a
/// refusal is not overwritten by "Add your cartridge" on the next 500 ms poll.
static GERomCheck *sLastImportRejection = nil;

@implementation GEOnboarding

+ (NSString *)acceptedRomPath { return sAcceptedPath; }
+ (NSString *)expectedSHA1    { return kSHA1_NTSCU; }

#pragma mark - classification

/// The whole verdict for ONE file. No copying, no conversion — that is the
/// caller's decision, because the picker classifies a file it must not touch.
+ (GERomCheck *)classifyFileAtPath:(NSString *)path
{
	GERomCheck *r = [GERomCheck new];
	r.path = path;

	NSDictionary *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL];
	if (!attrs) {
		r.verdict  = GERomVerdictMissing;
		r.headline = @"Add your GoldenEye 007 cartridge";
		r.detail   = @"This app ships no game data — you supply your own dump of the US cartridge.";
		return r;
	}
	r.bytes = attrs.fileSize;

	// A 64-byte header is all the classifier needs, and reading only that
	// keeps a poll over a folder full of files free.
	NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
	NSData *head = [fh readDataOfLength:64];
	[fh closeFile];
	if (head.length < 64) {
		r.verdict  = GERomVerdictUnknown;
		r.headline = @"That file is not a cartridge dump";
		r.detail   = @"It is too short to be an N64 ROM.";
		return r;
	}

	unsigned char h[64];
	memcpy(h, head.bytes, 64);
	GEByteOrder order = geOrderOfMagic(h);
	if (order == GEOrderUnknown) {
		r.verdict  = GERomVerdictUnknown;
		r.headline = @"That file is not a cartridge dump";
		r.detail   = @"It does not start like an N64 ROM. Compressed archives "
		             @"(.zip, .7z, .rar) have to be unpacked first.";
		return r;
	}
	geToZ64(h, sizeof(h), order);

	if (memcmp(h + 0x20, "GOLDENEYE", 9) != 0 || h[0x3C] != 'G' || h[0x3D] != 'E') {
		r.verdict  = GERomVerdictUnknown;
		r.headline = @"That is an N64 ROM, but not GoldenEye 007";
		r.detail   = @"The cartridge name inside the file is something else. A "
		             @"ROM hack or a patched build will not run here either.";
		return r;
	}

	if (r.bytes < kRomBytes) {
		r.verdict  = GERomVerdictTooSmall;
		r.headline = @"That copy is still arriving";
		r.detail   = [NSString stringWithFormat:
			@"A GoldenEye 007 cartridge is 12 MB and this file is %.1f MB so far. "
			@"If it is still copying in, wait — this screen re-checks by itself.",
			r.bytes / (1024.0 * 1024.0)];
		return r;
	}

	const unsigned char country = h[0x3E];
	if (country != 'E') {
		r.verdict  = GERomVerdictWrongRegion;
		r.headline = (country == 'P') ? @"That is the European cartridge"
		                              : @"That is the Japanese cartridge";
		r.detail   = @"It really is GoldenEye 007 — just not the version this "
		             @"release runs. Project Midas needs the US (NTSC-U) "
		             @"cartridge for now; the other regions are on the roadmap.";
		return r;
	}

	if (order != GEOrderZ64) {
		r.verdict  = GERomVerdictByteSwapped;
		r.headline = @"Converting your cartridge…";
		r.detail   = (order == GEOrderV64)
			? @"That is a .v64 dump. Project Midas is converting it — this takes a moment."
			: @"That is a .n64 dump. Project Midas is converting it — this takes a moment.";
		return r;
	}

	r.verdict  = GERomVerdictOK;
	r.headline = @"Cartridge found";
	r.detail   = @"Starting Project Midas.";
	return r;
}

/// Everything in Documents that could plausibly be a cartridge, best verdict
/// first. Cached per (path, size, mtime) so a 500 ms poll costs one stat(2)
/// per file once the answer is known.
+ (GERomCheck *)check
{
	static NSMutableDictionary<NSString *, NSArray *> *cache = nil;
	if (!cache) {
		cache = [NSMutableDictionary dictionary];
	}

	NSString *docs = GEShell.shared.documentsPath;
	NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:docs error:NULL];

	GERomCheck *best = nil;
	// Lower is better. Everything below Unknown is still worth SAYING, because
	// "you dropped in the PAL one" is a far more useful screen than "no ROM".
	NSDictionary *rank = @{ @(GERomVerdictOK): @0, @(GERomVerdictByteSwapped): @1,
	                        @(GERomVerdictWrongRegion): @2, @(GERomVerdictTooSmall): @3,
	                        @(GERomVerdictUnknown): @4, @(GERomVerdictMissing): @5 };

	for (NSString *name in names) {
		@autoreleasepool {
			NSString *path = [docs stringByAppendingPathComponent:name];
			NSDictionary *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL];
			if (!attrs || ![attrs.fileType isEqualToString:NSFileTypeRegular]) {
				continue;
			}
			// The shape filter: anything an N64 cartridge could be. It keeps
			// ge007.ini, ge007.eep, crash.txt, pacing.log and ge.args out of a
			// per-poll header read without hard-coding their names.
			unsigned long long sz = attrs.fileSize;
			NSString *ext = name.pathExtension.lowercaseString;
			BOOL romExt = [ext isEqualToString:@"z64"] || [ext isEqualToString:@"n64"] ||
			              [ext isEqualToString:@"v64"];
			if (!romExt && sz < 4u * 1024u * 1024u) {
				continue;
			}
			if (sz > 128u * 1024u * 1024u) {
				continue;
			}

			NSString *key = [NSString stringWithFormat:@"%llu/%f",
				sz, [(NSDate *)attrs[NSFileModificationDate] timeIntervalSince1970]];
			NSArray *hit = cache[path];
			GERomCheck *c;
			if (hit && [hit[0] isEqualToString:key]) {
				c = hit[1];
			} else {
				c = [self classifyFileAtPath:path];
				cache[path] = @[ key, c ];
			}

			if (!best || [rank[@(c.verdict)] intValue] < [rank[@(best.verdict)] intValue]) {
				best = c;
			}
		}
	}

	if (!best && sLastImportRejection) {
		return sLastImportRejection;
	}
	if (!best) {
		best = [GERomCheck new];
		best.verdict  = GERomVerdictMissing;
		best.headline = @"Add your GoldenEye 007 cartridge";
		best.detail   = @"Project Midas ships no game data: you supply your own "
		                @"dump of the US (NTSC-U) cartridge. Any filename is fine.";
	}

	// A .n64/.v64 is not an error, it is a conversion. Do it here, where the
	// poll loop will pick the result up on its next turn.
	if (best.verdict == GERomVerdictByteSwapped) {
		[self convertToZ64:best];
		best = [self checkConvertedOrKeep:best];
	}

	if (best.verdict == GERomVerdictOK) {
		if (![sAcceptedPath isEqualToString:best.path]) {
			sAcceptedPath = [best.path copy];
			best.sha1 = geSHA1OfFile(best.path);
			GELog(@"[onboard] accepted %@ (sha1 %@%@)",
				best.path.lastPathComponent, best.sha1,
				[best.sha1 isEqualToString:kSHA1_NTSCU] ? @", the reference dump" : @", NOT the reference dump");
		}
	}
	return best;
}

/// Read the whole swapped file, byte-swap it and write Documents/<converted>.
/// One shot: if the target already exists with the right size, do nothing.
+ (void)convertToZ64:(GERomCheck *)swapped
{
	NSString *dst = [GEShell.shared.documentsPath stringByAppendingPathComponent:kConvertedName];
	NSDictionary *have = [NSFileManager.defaultManager attributesOfItemAtPath:dst error:NULL];
	if (have && have.fileSize == swapped.bytes) {
		return;
	}

	// A failed write (disk full) must not be retried every 500 ms poll — that
	// re-reads 12 MB each time for as long as the screen is up. One attempt
	// per distinct source file; a new file (size) gets a fresh try.
	static unsigned long long sFailedForBytes = 0;
	if (sFailedForBytes == swapped.bytes) {
		return;
	}

	NSData *data = [NSData dataWithContentsOfFile:swapped.path options:0 error:NULL];
	if (data.length < 64) {
		return;
	}
	NSMutableData *out = [data mutableCopy];
	unsigned char *b = out.mutableBytes;
	geToZ64(b, out.length, geOrderOfMagic(b));
	if ([out writeToFile:dst atomically:YES]) {
		// The engine opens the ROM with plain fopen() at launch, and an app can
		// be launched while the device is still locked after a reboot. Data
		// protection would make that open fail with EPERM and look exactly like
		// a missing file, so the ROM is explicitly unprotected — it is the
		// player's own game data, not a secret.
		[NSFileManager.defaultManager setAttributes:@{ NSFileProtectionKey: NSFileProtectionNone }
		                               ofItemAtPath:dst error:NULL];
		GELog(@"[onboard] converted %@ to big-endian at %@",
			swapped.path.lastPathComponent, kConvertedName);
	} else {
		GELog(@"[onboard] could not write %@", dst);
		sFailedForBytes = swapped.bytes;
	}
}

+ (GERomCheck *)checkConvertedOrKeep:(GERomCheck *)fallback
{
	NSString *dst = [GEShell.shared.documentsPath stringByAppendingPathComponent:kConvertedName];
	if (![NSFileManager.defaultManager fileExistsAtPath:dst]) {
		return fallback;
	}
	GERomCheck *c = [self classifyFileAtPath:dst];
	return (c.verdict == GERomVerdictOK) ? c : fallback;
}

#pragma mark - importing a file (round 57)

/// Documents/Inbox is the system's drop box for this app (AirDrop, "Open
/// in"). Remove the handed-over file (if any) and, once empty, the folder,
/// which would otherwise show up in Files as a stray folder.
static void geDropInboxFile(NSString *_Nullable path)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	if (path) {
		[fm removeItemAtPath:path error:NULL];
	}
	NSString *inbox = [GEShell.shared.documentsPath stringByAppendingPathComponent:@"Inbox"];
	if ([fm contentsOfDirectoryAtPath:inbox error:NULL].count == 0) {
		[fm removeItemAtPath:inbox error:NULL];
	}
}

/// The one way a file from outside comes in: Choose ROM…, AirDrop, Files'
/// "Open in" / share sheet. Classifies it with the same header check as the
/// Documents poll (the poll then logs the sha1 against the reference and hands
/// the path to the engine), copies it into Documents under the player's own
/// name without clobbering anything, and drops the system's Inbox copy.
/// Returns the verdict; on refusal the onboarding screen shows it.
+ (GERomCheck *)importROMAtURL:(NSURL *)url
{
	GERomCheck *fail = [GERomCheck new];
	fail.verdict = GERomVerdictUnknown;
	if (!url.isFileURL) {
		fail.headline = @"That is not a file";
		fail.detail = @"";
		return fail;
	}

	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *docs = GEShell.shared.documentsPath.stringByStandardizingPath;
	NSString *src = url.path.stringByStandardizingPath;
	NSString *parent = src.stringByDeletingLastPathComponent;
	// /private/var vs /var: compare resolved paths.
	NSString *docsR = docs.stringByResolvingSymlinksInPath;
	NSString *parentR = parent.stringByResolvingSymlinksInPath;
	NSString *inboxR = [docsR stringByAppendingPathComponent:@"Inbox"];
	const BOOL fromInbox = [parentR isEqualToString:inboxR];
	const BOOL inDocs = [parentR isEqualToString:docsR];

	BOOL scoped = [url startAccessingSecurityScopedResource];
	GERomCheck *check = [self classifyFileAtPath:src];
	GELog(@"[onboard] import %@ (%@) -> verdict %ld (%@)", url.lastPathComponent,
	      fromInbox ? @"Inbox" : inDocs ? @"in place" : @"outside", (long)check.verdict, check.headline);

	if (check.verdict != GERomVerdictOK && check.verdict != GERomVerdictByteSwapped) {
		if (scoped) [url stopAccessingSecurityScopedResource];
		if (fromInbox) geDropInboxFile(src);
		sLastImportRejection = check;
		return check;
	}
	sLastImportRejection = nil;

	if (sAcceptedPath) {
		// A cartridge is already running; a second copy would only be one more
		// 12 MB file for the next launch's poll to choose between.
		GELog(@"[onboard] import: %@ already accepted, %@ not copied", sAcceptedPath.lastPathComponent,
		      url.lastPathComponent);
		if (scoped) [url stopAccessingSecurityScopedResource];
		if (fromInbox) geDropInboxFile(src);
		return check;
	}
	if (inDocs) {
		// Opened in place from Files ▸ On My iPhone ▸ Project Midas: it is
		// already where the poll looks.
		if (scoped) [url stopAccessingSecurityScopedResource];
		return check;
	}

	// Keep the player's own name. The engine is handed the PATH (D-034), so
	// there is nothing to rename it to. Never clobber a file already in
	// Documents (it may be the player's working ROM): "name-2.z64", ...
	NSString *dst = [docs stringByAppendingPathComponent:url.lastPathComponent];
	NSString *stem = url.lastPathComponent.stringByDeletingPathExtension;
	NSString *ext = url.lastPathComponent.pathExtension;
	for (int n = 2; [fm fileExistsAtPath:dst] && n < 100; n++) {
		NSString *name = [NSString stringWithFormat:@"%@-%d", stem, n];
		if (ext.length) name = [name stringByAppendingPathExtension:ext];
		dst = [docs stringByAppendingPathComponent:name];
	}
	NSError *err = nil;
	BOOL ok = fromInbox ? [fm moveItemAtPath:src toPath:dst error:&err]
	                    : [fm copyItemAtPath:src toPath:dst error:&err];
	if (scoped) [url stopAccessingSecurityScopedResource];
	if (!ok) {
		GELog(@"[onboard] import: could not copy %@: %@", url.lastPathComponent, err);
		fail.headline = @"Could not copy that file in";
		fail.detail = err.localizedDescription ?: @"";
		sLastImportRejection = fail;
		return fail;
	}
	// The engine opens the ROM with plain fopen() at launch, possibly while the
	// device is still locked after a reboot (see convertToZ64:).
	[fm setAttributes:@{ NSFileProtectionKey: NSFileProtectionNone } ofItemAtPath:dst error:NULL];
	if (fromInbox) {
		geDropInboxFile(nil);
	}
	GELog(@"[onboard] copied %@ into Documents as %@", url.lastPathComponent, dst.lastPathComponent);
	// The poll in +runUntilRomIsReady sees it on its next turn (≤ 0.5 s),
	// converts it if it needs converting, and hands over to the engine.
	return check;
}

#pragma mark - the screen

static UIWindow *sWindow = nil;
static GEOnboardingVC *sVC = nil;

+ (void)presentWithCheck:(GERomCheck *)check
{
	if (!sWindow) {
		UIWindowScene *scene = nil;
		for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
			if ([s isKindOfClass:UIWindowScene.class]) {
				scene = (UIWindowScene *)s;
				if (s.activationState == UISceneActivationStateForegroundActive) {
					break;
				}
			}
		}
		if (!scene) {
			// The scene has not connected yet. Nothing to show on; the caller
			// polls, so this will succeed on a later turn of the run loop.
			return;
		}
		sVC = [GEOnboardingVC new];
		sWindow = [[UIWindow alloc] initWithWindowScene:scene];
		sWindow.rootViewController = sVC;
		sWindow.windowLevel = UIWindowLevelNormal + 100;   // above SDL's, whenever it appears
		[sWindow makeKeyAndVisible];
		GELog(@"onboarding presented");
	}
	[sVC applyCheck:check];
}

+ (BOOL)presentDocumentPicker
{
	if (!sVC) {
		return NO;
	}
	[sVC pickFile];
	return YES;
}

+ (void)dismiss
{
	if (sWindow) {
		sWindow.hidden = YES;
		sWindow = nil;
		sVC = nil;
		GELog(@"onboarding dismissed — ROM accepted");
	}
}

+ (void)runUntilRomIsReady
{
	GERomCheck *c = [self check];
	if (c.verdict == GERomVerdictOK) {
		GELog(@"ROM OK at %@ — starting the engine", c.path);
		return;
	}

	GELog(@"onboarding — %@", c.headline);

	// Poll rather than wait on a notification. sceneDidBecomeActive is the
	// event that matters (the user came back from Files, or the picker
	// returned) and GESceneDelegate posts it, but a file can also land while
	// we are already active — iCloud Drive finishes a download, AirDrop
	// completes — and a 500 ms poll costs one stat(2) per file because every
	// classification is cached against (size, mtime).
	while (1) {
		@autoreleasepool {
			[self presentWithCheck:c];
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
			GERomCheck *next = [self check];
			if (next.verdict == GERomVerdictOK) {
				[self presentWithCheck:next];
				// One run-loop turn so the "Cartridge found" state is actually
				// drawn before the engine takes the main thread for several
				// seconds of sidecar conversion.
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.35, false);
				[self dismiss];
				GELog(@"ROM OK at %@ — starting the engine", next.path);
				return;
			}
			c = next;
		}
	}
}

@end

#pragma mark - the view controller

@implementation GEOnboardingVC

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor colorWithRed:0.04 green:0.05 blue:0.06 alpha:1.0];

	UIStackView *stack = [[UIStackView alloc] init];
	stack.axis = UILayoutConstraintAxisVertical;
	stack.spacing = 14.0;
	stack.alignment = UIStackViewAlignmentLeading;
	stack.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:stack];

	UILabel *title = [UILabel new];
	title.text = @"Project Midas";
	title.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
	title.textColor = [UIColor colorWithRed:0.85 green:0.72 blue:0.25 alpha:1.0];
	[stack addArrangedSubview:title];

	_headline = [UILabel new];
	_headline.font = [UIFont systemFontOfSize:26 weight:UIFontWeightBold];
	_headline.textColor = UIColor.whiteColor;
	_headline.numberOfLines = 0;
	[stack addArrangedSubview:_headline];

	_detail = [UILabel new];
	_detail.font = [UIFont systemFontOfSize:15];
	_detail.textColor = [UIColor colorWithWhite:0.78 alpha:1.0];
	_detail.numberOfLines = 0;
	[stack addArrangedSubview:_detail];

	// One sentence per way in. Nothing here is a filename or a hash: the
	// point of round 10c is that the player never has to type either.
	_ways = [UILabel new];
	_ways.font = [UIFont systemFontOfSize:14];
	_ways.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];
	_ways.numberOfLines = 0;
	// Round 57: the AirDrop line is real now (CFBundleDocumentTypes; the
	// open lands in +importROMAtURL:). Files' "Open in" / share works too.
#if TARGET_OS_VISION
	NSString *device = @"Apple Vision Pro", *onMy = @"On My Apple Vision Pro";
#else
	NSString *device = @"iPhone", *onMy = @"On My iPhone";
#endif
	_ways.text = [NSString stringWithFormat:
		@"Tap Choose ROM… to pick the file from iCloud Drive, a USB drive or anywhere else in Files.\n"
		@"Or copy it into Files ▸ %@ ▸ Project Midas.\n"
		@"Or AirDrop it to this %@ (or share it from Files) and choose Project Midas.\n"
		@"Any filename works, and .z64, .n64 and .v64 dumps are all accepted.", onMy, device];
	[stack addArrangedSubview:_ways];

	UIButtonConfiguration *cfg = [UIButtonConfiguration filledButtonConfiguration];
	cfg.title = @"Choose ROM…";
	cfg.baseBackgroundColor = [UIColor colorWithRed:0.85 green:0.72 blue:0.25 alpha:1.0];
	cfg.baseForegroundColor = [UIColor colorWithWhite:0.06 alpha:1.0];
	cfg.contentInsets = NSDirectionalEdgeInsetsMake(12, 22, 12, 22);
	_choose = [UIButton buttonWithConfiguration:cfg primaryAction:
		[UIAction actionWithHandler:^(UIAction *a) { (void)a; [self pickFile]; }]];
	_choose.accessibilityIdentifier = @"ChooseROM";
	[stack addArrangedSubview:_choose];

	UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:32],
		[stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-32],
		[stack.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
	]];
}

- (void)pickFile
{
	NSMutableArray<UTType *> *types = [NSMutableArray array];
	for (NSString *ext in @[ @"z64", @"n64", @"v64" ]) {
		UTType *t = [UTType typeWithFilenameExtension:ext];
		if (t) {
			[types addObject:t];
		}
	}
	[types addObject:UTTypeData];   // the catch-all: many dumps have no UTI

	UIDocumentPickerViewController *p =
		[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
	p.delegate = self;
	p.allowsMultipleSelection = NO;
	[self presentViewController:p animated:YES completion:nil];
	GELog(@"[onboard] presenting the document picker");
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
	didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	NSURL *url = urls.firstObject;
	if (!url) {
		return;
	}

	// Round 57: the same import as AirDrop and Files' "Open in" (asCopy:YES
	// already gives a temporary copy; the import still asks for a security
	// scope, which costs nothing when it is not needed).
	GERomCheck *check = [GEOnboarding importROMAtURL:url];
	if (check.verdict != GERomVerdictOK && check.verdict != GERomVerdictByteSwapped) {
		self.headline.text = check.headline;
		self.detail.text = check.detail;
	}
}

- (void)applyCheck:(GERomCheck *)check
{
	[self loadViewIfNeeded];
	self.headline.text = check.headline;
	self.detail.text = check.detail;
	self.choose.hidden = (check.verdict == GERomVerdictOK);
	self.ways.hidden = (check.verdict == GERomVerdictOK);
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
	return UIInterfaceOrientationMaskLandscape;
}

- (BOOL)prefersStatusBarHidden { return YES; }

@end
