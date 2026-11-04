// GEXbla.h — texture packs and the GoldenEye XBLA import, the shell's half.
//
// Ported from GoldenEye-Decomp-ios app/ios/GEXbla.h (round 19c, D-095..D-098,
// D-141, D-202; sibling HEAD 55a3a9c8) in Project Midas round 16 (XBLA step 5,
// docs/port-ios/25-round16-shell.md). Midas changes: "Remove imported XBLA"
// also deletes the user's copy in Documents/xbla (otherwise the startup scan
// would import it again on the next launch), and the save goes through
// configSaveKey (Midas has no config.c).
//
// The engine does the work (port/apple/texpack.c, port/apple/xblaimport.c);
// this is the part a phone needs and a desktop does not:
//
//   * the Files picker behind "Import GoldenEye XBLA…" — the picked copy is
//     moved into Documents/xbla/ (excluded from iCloud backup, kept so a
//     Caches eviction can be re-imported from it at the next launch,
//     xblaImportAtStartup) and the engine's importer runs on its own thread
//     while the Settings row shows progress;
//   * the pack list behind "Texture pack", and its picker;
//   * Documents/texturepacks/ and Documents/xbla/ created at launch, so both
//     folders are there to drop into from Files.
#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface GEXbla : NSObject

/// Create Documents/texturepacks and Documents/xbla. Main thread, at launch.
+ (void)prepareFolders;

/// One line for the Settings "GoldenEye XBLA" status row: "27 HD · 1,372
/// same-size · 21 levels" / "Importing… n of m files" / "Failed: why" /
/// "Not imported".
+ (NSString *)statusLine;
/// Tier C: off, how many rooms of the current level are drawn from XBLA's
/// mesh and how, or why not. The bridge's `xbla_levels=`.
+ (NSString *)levelLine;
/// One line for the texture-pack row: "None", or the selected pack's name and
/// how many textures it replaces.
+ (NSString *)packLine;

/// Present the Files picker; `done` runs on the main thread when the import
/// has STARTED (or with an error message if the pick was not the release).
+ (void)presentImporterFrom:(UIViewController *)vc done:(void (^_Nullable)(NSString *_Nullable error))done;

/// The picker's completion, public so the bridge can drive it on a simulator:
/// move/copy `path` into Documents/xbla and start the engine's import. nil on
/// success, else what is wrong.
+ (nullable NSString *)adoptAndImport:(NSString *)path;

/// Pick a texture pack from the list (action sheet).
+ (void)presentPackPickerFrom:(UIViewController *)vc sourceView:(UIView *)view done:(void (^_Nullable)(void))done;

/// What the picker's choice does: select `name` ("" = None) and save. Also the
/// bridge's `texpack select`, so a gate drives the sheet's own path.
+ (void)selectPack:(NSString *)name;

/// Round 17: the "Remove imported XBLA" row. Asks first (destructive alert);
/// only Remove runs forgetImport, then `done` gets its result. Cancel does
/// nothing and does not call `done`.
+ (void)confirmForgetFrom:(UIViewController *)vc done:(void (^_Nullable)(NSString *_Nullable error))done;

/// Remove the imported layer (Library/Caches/xbla) and the user's copy in
/// Documents/xbla. nil on success, else why not (an import is running).
+ (nullable NSString *)forgetImport;

/// `xbla_*` / `texpack_*` lines for the bridge's `state`.
+ (NSString *)stateLines;

@end

NS_ASSUME_NONNULL_END
