// GEXbla.m — see GEXbla.h.
//
// Ported from GoldenEye-Decomp-ios app/ios/GEXbla.m (round 19c, D-095..D-098,
// D-141, D-202; sibling HEAD 55a3a9c8), Project Midas round 16 (XBLA step 5).
// Midas changes, all marked "Midas:": configSaveKey instead of configSave,
// the Files path names Project Midas, the status line carries both counts,
// and forgetImport also deletes Documents/xbla and the level cache.
// Round 17: Remove asks first (confirmForgetFrom:), and the picker-copy
// test matches NSTemporaryDirectory() only.
#import "GEXbla.h"
#import "GEShell.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// port/include/texpack.h
extern int texpackGetNumPacks(void);
extern const char *texpackGetPackName(int index);
extern const char *texpackGetSelected(void);
extern void texpackSetSelected(const char *name);
extern int texpackGetNumReplacements(void);
extern int texpackGetNumPackFiles(void);
extern int texpackGetNumUnplaced(void);
extern int texpackXblaEnabled(void);
extern int texpackXblaNumTextures(void);
extern void texpackReload(void);
// port/include/xblaimport.h
extern int xblaImportKindOf(const char *path);
extern int xblaImportStart(const char *src);
extern int xblaImportCheckSpace(long long extraBytes, char *msg, int msglen);
extern int xblaImportState(void);
extern void xblaImportProgress(int *done, int *total);
extern const char *xblaImportMessage(void);
extern int xblaImportedHd(void);
extern int xblaImportedSame(void);
extern const char *xblaImportedSource(void);
extern double xblaImportedSeconds(void);
extern void xblaImportForget(void);
extern int xblaMapNumHd(void);
// port/include/beanstage.h (Tier C)
extern void beanStageDescribe(char *buf, int len);
extern int beanStageModelsEnabled(void);
// port/include/config.h (port/apple/xblaconfig.c). Midas: no configSave.
extern int configSaveKey(const char *key);
// port/apple/hostpaths.c
extern const char *sysResolvePath(const char *path);

enum { kIdle = 0, kRunning, kDone, kFailed };

@interface GEXblaPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, copy, nullable) void (^done)(NSString *_Nullable error);
@end

static GEXblaPickerDelegate *sPickerDelegate;

@implementation GEXblaPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	(void)controller;
	NSURL *url = urls.firstObject;
	NSString *err = nil;
	if (url) {
		BOOL scoped = [url startAccessingSecurityScopedResource];
		err = [GEXbla adoptAndImport:url.path];
		if (scoped) [url stopAccessingSecurityScopedResource];
	}
	if (self.done) self.done(err);
	sPickerDelegate = nil;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
	(void)controller;
	sPickerDelegate = nil;
}
@end

@implementation GEXbla

+ (NSString *)docs { return GEShell.shared.documentsPath; }
+ (NSString *)xblaDir { return [self.docs stringByAppendingPathComponent:@"xbla"]; }
+ (NSString *)packsDir { return [self.docs stringByAppendingPathComponent:@"texturepacks"]; }
/// The derived layer ($C/xbla = Library/Caches/xbla): iOS may evict it, and
/// the next launch rebuilds it from Documents/xbla (xblaImportAtStartup).
+ (NSString *)layerDir { return @(sysResolvePath("$C/xbla")); }

+ (void)prepareFolders
{
	NSFileManager *fm = NSFileManager.defaultManager;
	[fm createDirectoryAtPath:self.xblaDir withIntermediateDirectories:YES attributes:nil error:nil];
	[fm createDirectoryAtPath:self.packsDir withIntermediateDirectories:YES attributes:nil error:nil];
	// The release is three quarters of a gigabyte and the user's own
	// download: it is not ours to put in their iCloud backup.
	NSURL *u = [NSURL fileURLWithPath:self.xblaDir];
	NSError *e = nil;
	if (![u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&e]) {
		GELog(@"xbla: could not exclude Documents/xbla from backup: %@", e.localizedDescription);
	}
	// Round 38: the same for texture packs. GoldenEye 007 HD is 2.2 GB
	// unpacked (plus its 2.4 GB zip until the user deletes it), and can be
	// downloaded again.
	u = [NSURL fileURLWithPath:self.packsDir];
	e = nil;
	if (![u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&e]) {
		GELog(@"texpack: could not exclude Documents/texturepacks from backup: %@", e.localizedDescription);
	}
}

/// Round 38: what is in Documents/texturepacks right now, read here rather
/// than from texpack.c. texpack's list is rebuilt by the render thread on its
/// next frame (texpackReload), so right after a pack is unzipped in Files it
/// is one look behind. Same rule as texpack.c listPacks(): every folder not
/// starting with '.', name under 128 bytes, sorted case-insensitively. Files
/// (a .zip not yet unzipped) are returned in *zips, not offered as packs.
+ (NSArray<NSString *> *)packFoldersZips:(NSArray<NSString *> **)zips
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSMutableArray<NSString *> *dirs = [NSMutableArray array];
	NSMutableArray<NSString *> *archives = [NSMutableArray array];
	for (NSString *name in [fm contentsOfDirectoryAtPath:self.packsDir error:nil]) {
		if ([name hasPrefix:@"."] || strlen(name.fileSystemRepresentation) >= 128) {
			continue;
		}
		BOOL isDir = NO;
		NSString *p = [self.packsDir stringByAppendingPathComponent:name];
		if ([fm fileExistsAtPath:p isDirectory:&isDir] && isDir) {
			[dirs addObject:name];
		} else if ([name.pathExtension.lowercaseString isEqualToString:@"zip"]
		           || [name.pathExtension.lowercaseString isEqualToString:@"7z"]
		           || [name.pathExtension.lowercaseString isEqualToString:@"rar"]) {
			[archives addObject:name];
		}
	}
	[dirs sortUsingSelector:@selector(caseInsensitiveCompare:)];
	if (zips) *zips = archives;
	return dirs;
}

/// How many levels the import carries the remaster's own geometry for — the
/// level files the importer copied to $C/xbla/files/new/background/<name>/.
+ (int)importedLevels
{
	NSString *bg = [self.layerDir stringByAppendingPathComponent:@"files/new/background"];
	NSFileManager *fm = NSFileManager.defaultManager;
	int n = 0;
	for (NSString *name in [fm contentsOfDirectoryAtPath:bg error:nil]) {
		NSString *f = [[bg stringByAppendingPathComponent:name] stringByAppendingPathComponent:@"default.bin"];
		if ([fm fileExistsAtPath:f]) {
			n++;
		}
	}
	return n;
}

+ (NSString *)statusLine
{
	const int st = xblaImportState();
	if (st == kRunning) {
		int d = 0, t = 0;
		xblaImportProgress(&d, &t);
		if (t > 0) {
			return [NSString stringWithFormat:@"Importing… %d of %d files", d, t];
		}
		return @"Importing…";
	}
	if (st == kFailed) {
		return [NSString stringWithFormat:@"Failed: %s", xblaImportMessage()];
	}
	const int hd = xblaImportedHd();
	if (hd > 0) {
		// Midas: both counts (the sibling shows the HD count only).
		const int lv = [self importedLevels];
		NSNumberFormatter *nf = [NSNumberFormatter new];
		nf.numberStyle = NSNumberFormatterDecimalStyle;
		return [NSString stringWithFormat:@"%d HD · %@ same-size · %d level%@", hd,
		        [nf stringFromNumber:@(xblaImportedSame())], lv, lv == 1 ? @"" : @"s"];
	}
	return @"Not imported";
}

+ (NSString *)levelLine
{
	char buf[96];
	beanStageDescribe(buf, (int)sizeof(buf));
	return @(buf);
}

+ (NSString *)packLine
{
	const char *sel = texpackGetSelected();
	if (!sel || !*sel) {
		return @"None";
	}
	const int n = texpackGetNumReplacements() + texpackGetNumUnplaced();
	return [NSString stringWithFormat:@"%s · %d textures", sel, n];
}

+ (nullable NSString *)adoptAndImport:(NSString *)path
{
	if (xblaImportState() == kRunning) {
		return @"An import is already running.";
	}
	const int kind = xblaImportKindOf(path.fileSystemRepresentation);
	if (kind == 0) {
		return @"That is not GoldenEye XBLA. Pick the .7z you keep it in, the Xbox 360 "
		       @"package itself (584108A9/000D0000/30BA9271…), or the unpacked folder.";
	}
	NSString *src = path;
	// The picker's copy (asCopy:YES) lives in our own tmp/: a move is a
	// rename. A path from anywhere else (the bridge) is copied.
	// Round 17: only our own NSTemporaryDirectory() counts (symlinks resolved
	// on both sides: /private/var vs /var), not any path holding "/tmp/".
	NSString *tmpRoot = NSTemporaryDirectory().stringByResolvingSymlinksInPath;
	if (![tmpRoot hasSuffix:@"/"]) tmpRoot = [tmpRoot stringByAppendingString:@"/"];
	const BOOL ours = [path.stringByResolvingSymlinksInPath hasPrefix:tmpRoot];
	{
		// D-202: refuse before the copy and the import if the phone lacks room
		// for them (the import's ~395 MB + margin, plus the copy of a file).
		// Main thread, before the worker exists: this also resolves $C once.
		const BOOL copies = ![path hasPrefix:self.xblaDir] && !ours;
		NSDictionary *a = copies ? [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] : nil;
		const long long extra = [a[NSFileType] isEqual:NSFileTypeRegular] ? (long long)a.fileSize : 0;
		char why[256];
		if (!xblaImportCheckSpace(extra, why, (int)sizeof(why))) {
			GELog(@"xbla: %s", why);
			return @(why);
		}
	}
	if (![path hasPrefix:self.xblaDir]) {
		NSFileManager *fm = NSFileManager.defaultManager;
		[fm createDirectoryAtPath:self.xblaDir withIntermediateDirectories:YES attributes:nil error:nil];
		NSString *dst = [self.xblaDir stringByAppendingPathComponent:path.lastPathComponent];
		NSString *tmp = [self.xblaDir stringByAppendingPathComponent:
		                 [NSString stringWithFormat:@".adding-%@", NSUUID.UUID.UUIDString]];
		NSError *e = nil;
		BOOL ok = ours ? [fm moveItemAtPath:path toPath:tmp error:&e] : [fm copyItemAtPath:path toPath:tmp error:&e];
		if (!ok) {
			[fm removeItemAtPath:tmp error:nil];
			return [NSString stringWithFormat:@"Could not copy it into the app: %@", e.localizedDescription];
		}
		// Only a finished copy replaces what was there (PD D-075).
		[fm removeItemAtPath:dst error:nil];
		if (![fm moveItemAtPath:tmp toPath:dst error:&e]) {
			[fm removeItemAtPath:tmp error:nil];
			return [NSString stringWithFormat:@"Could not copy it into the app: %@", e.localizedDescription];
		}
		src = dst;
	}
	GELog(@"xbla: importing %@", src);
	// R10-4: the worker copies `src` (xblaImportStart's runSrc) before it
	// starts, and sysResolvePath's result buffer is per thread (hostpaths.c).
	if (!xblaImportStart(src.fileSystemRepresentation)) {
		return @"The import did not start.";
	}
	return nil;
}

+ (void)presentImporterFrom:(UIViewController *)vc done:(void (^)(NSString *))done
{
	NSMutableArray<UTType *> *types = [NSMutableArray array];
	UTType *t7z = [UTType typeWithFilenameExtension:@"7z"];
	if (t7z) [types addObject:t7z];
	[types addObject:UTTypeData];     // the bare package has no extension
	[types addObject:UTTypeFolder];   // the unpacked release
	UIDocumentPickerViewController *p =
		[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
	sPickerDelegate = [GEXblaPickerDelegate new];
	sPickerDelegate.done = done;
	p.delegate = sPickerDelegate;
	p.allowsMultipleSelection = NO;
	[vc presentViewController:p animated:YES completion:nil];
}

+ (void)presentPackPickerFrom:(UIViewController *)vc sourceView:(UIView *)view done:(void (^)(void))done
{
	texpackReload();   // a pack dropped in from Files since the last look
	NSArray<NSString *> *zips = nil;
	NSArray<NSString *> *packs = [self packFoldersZips:&zips];
	NSString *msg = @"Packs live in Files → Project Midas → texturepacks, one folder each. "
	                @"For GoldenEye 007 HD, download the repository zip from "
	                @"github.com/GhostlyDark/GoldenEye-007-HD, put it there and tap it to unzip.";
	if (zips.count > 0 && packs.count == 0) {
		// Round 38: a pack still zipped is not offered (it cannot be read),
		// but the player is told why the list is empty.
		msg = [NSString stringWithFormat:@"%@ is still zipped. In Files → Project Midas → "
		       @"texturepacks, tap it to unzip it, then pick the folder here.", zips.firstObject];
	}
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Texture pack"
		message:msg
		preferredStyle:UIAlertControllerStyleActionSheet];
	void (^pick)(NSString *) = ^(NSString *name) {
		[GEXbla selectPack:name];
		if (done) done();
	};
	[a addAction:[UIAlertAction actionWithTitle:@"None" style:UIAlertActionStyleDefault
	                                    handler:^(UIAlertAction *x) { (void)x; pick(@""); }]];
	for (NSString *name in packs) {
		[a addAction:[UIAlertAction actionWithTitle:name style:UIAlertActionStyleDefault
		                                    handler:^(UIAlertAction *x) { (void)x; pick(name); }]];
	}
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	a.popoverPresentationController.sourceView = view;
	a.popoverPresentationController.sourceRect = view.bounds;
	[vc presentViewController:a animated:YES completion:nil];
}

+ (void)selectPack:(NSString *)name
{
	// Live: the index rebuilds and the renderer drops its texture cache on
	// the next frame (texpack.c ensureIndex -> serial, gfx_pc.cpp).
	texpackSetSelected(name.UTF8String);
	configSaveKey("Video.TexturePack");   // Midas (R11-5): the setter does not save
	GELog(@"[settings] texture pack -> %@", name.length ? name : @"None");
}

+ (void)confirmForgetFrom:(UIViewController *)vc done:(void (^)(NSString *))done
{
	// Round 17: Remove deletes the user's copy too (R16-4), so it asks first.
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Remove imported XBLA?"
		message:@"This deletes the decoded XBLA textures and the copy of your XBLA release in "
		        @"Files → Project Midas → xbla. Your original, wherever else you keep it, "
		        @"is not touched. You can import it again later."
		preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
	                                    handler:^(UIAlertAction *x) {
		(void)x;
		GELog(@"xbla: Remove cancelled");
	}]];
	[a addAction:[UIAlertAction actionWithTitle:@"Remove" style:UIAlertActionStyleDestructive
	                                    handler:^(UIAlertAction *x) {
		(void)x;
		NSString *err = [GEXbla forgetImport];
		if (done) done(err);
	}]];
	[vc presentViewController:a animated:YES completion:nil];
}

+ (nullable NSString *)forgetImport
{
	if (xblaImportState() == kRunning) {
		return @"An import is running; wait for it to finish.";
	}
	xblaImportForget();   // tex/, same/, imported.txt, files/ (and texpackReload)
	// Midas: the rest of the derived layer (the level cache) and the user's
	// copy, or the next launch's startup scan imports it straight back.
	NSFileManager *fm = NSFileManager.defaultManager;
	NSError *e = nil;
	if ([fm fileExistsAtPath:self.layerDir] && ![fm removeItemAtPath:self.layerDir error:&e]) {
		GELog(@"xbla: could not remove %@: %@", self.layerDir, e.localizedDescription);
	}
	for (NSString *name in [fm contentsOfDirectoryAtPath:self.xblaDir error:nil]) {
		NSString *p = [self.xblaDir stringByAppendingPathComponent:name];
		if (![fm removeItemAtPath:p error:&e]) {
			GELog(@"xbla: could not remove %@: %@", p, e.localizedDescription);
		}
	}
	texpackReload();
	GELog(@"xbla: imported XBLA removed (Documents/xbla emptied, Caches/xbla deleted)");
	return nil;
}

+ (NSString *)stateLines
{
	int d = 0, t = 0;
	xblaImportProgress(&d, &t);
	return [NSString stringWithFormat:
		@"xbla_enabled=%d\nxbla_models=%d\nxbla_state=%d\nxbla_progress=%d/%d\nxbla_message=%s\n"
		@"xbla_imported_hd=%d\nxbla_imported_same=%d\nxbla_source=%s\nxbla_seconds=%.1f\n"
		@"xbla_layer_textures=%d\nxbla_map_hd=%d\nxbla_status=%@\n"
		@"texpack_selected=%s\ntexpack_packs=%d\ntexpack_files=%d\ntexpack_by_number=%d\n"
		@"texpack_by_texels=%d\nxbla_levels=%@\n",
		texpackXblaEnabled(), beanStageModelsEnabled(), xblaImportState(), d, t, xblaImportMessage(),
		xblaImportedHd(), xblaImportedSame(), xblaImportedSource(), xblaImportedSeconds(),
		texpackXblaNumTextures(), xblaMapNumHd(), [self statusLine],
		texpackGetSelected(), texpackGetNumPacks(), texpackGetNumPackFiles(),
		texpackGetNumReplacements(), texpackGetNumUnplaced(), [self levelLine]];
}

@end
