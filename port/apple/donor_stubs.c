/*
 * donor_stubs.c - inert stand-ins for the GoldenEye-Decomp-ios port-layer
 * hooks that its game sources call under `#ifdef PORT` (D-M02,
 * docs/port-ios/00-PLAN.md; enumerated in docs/port-ios/02-merge-feasibility.md
 * section 3). Round 2 merges those game sources; with these stubs the donor's
 * features (bean HD art, camlatch, texpacks, widescreen scaling, sidecar
 * table patch, shader prewarm) arrive DORMANT and Midas's behaviour is
 * unchanged: every hook is a no-op, every query answers "not served /
 * not active", and every scale factor is the identity.
 *
 * 64-bit build only (port/apple/ is compiled for GE_HOST32=OFF). Each stub
 * is replaced by a real implementation, behind a runtime switch defaulting to
 * Midas's behaviour, the day that feature is wanted.
 *
 * The 36 hooks and where the donor defines/declares them
 * (GoldenEye-Decomp-ios engine/):
 *
 *   bean Tier C characters  port/include/beanchr.h: real since round 15
 *     (port/apple/beanchr.c), no longer stubbed here
 *   bean Tier C models      port/include/beanmodel.h: real since round 14
 *     (port/apple/beanmodel.c, beangun.c), no longer stubbed here
 *   bean prefetch           port/include/beanprefetch.h: real since round 15
 *     (port/apple/beanprefetch.c), no longer stubbed here
 *   bean Tier C stage       port/include/beanstage.h: real since round 13
 *     (port/apple/beanstage.c), no longer stubbed here
 *   global image fixup      port/include/gimgfixup.h (port/src/gimgfixup.c)
 *     gimgFixupGlobalimagetable gimgSyncCompiledGlobalDLs
 *   sidecar table patch     port/include/pccg.h (port/src/pccg.c)
 *     pccgPatchTable          (real since round 7b: pcsidecar.c)
 *   camera latch (D-093)    port/src/camlatch.c (extern'd at the call sites)
 *     portCamLatchCommit portCamLatchGun portCamLatchRecord
 *   widescreen / distance   port/src/video.c (extern'd at the call sites)
 *     portDrawDistanceMultiplier portFillViewport portLodDistanceMultiplier
 *     portScaleAspect portScaleFovY portScreenSqueeze
 *   shader prewarm          port/include/shaderprewarm.h (port/src/shaderprewarm.c)
 *     shaderPrewarmRequest
 *   texture packs           port/include/texpack.h: real since round 11
 *     (port/apple/texpack.c), no longer stubbed here
 *
 * Signatures are copied from those declarations so the donor headers can be
 * imported later without conflicts. None of these names exists in Midas.
 */
#include <ultra64.h>
#include <stdint.h>

struct Model;
struct ModelNode;
struct ModelFileHeader;
struct beanmodelnode;

/* ---- bean Tier C characters (beanchr.h): real since round 15
 * (port/apple/beanchr.c, docs/port-ios/24-round15-chrs.md), no longer
 * stubbed here. */

/* ---- bean Tier C models (beanmodel.h): real since round 14
 * (port/apple/beanmodel.c + beangun.c, docs/port-ios/23-round14-props-guns.md),
 * no longer stubbed here. */

/* ---- bean prefetch (beanprefetch.h): real since round 15
 * (port/apple/beanprefetch.c, docs/port-ios/24-round15-chrs.md), no longer
 * stubbed here. */

/* ---- bean Tier C stage (beanstage.h): real since round 13
 * (port/apple/beanstage.c, docs/port-ios/22-round13-rooms.md), no longer
 * stubbed here. */

/* ---- global image fixup (gimgfixup.h) --------------------------------- */
/* The donor byte-swaps its own globalimagetable layout here; Midas's image
 * bank path does its own conversion, so round 2 must decide per call site. */
void gimgFixupGlobalimagetable(u8 *base) { (void) base; }
void gimgSyncCompiledGlobalDLs(u8 *base) { (void) base; }

/* ---- sidecar table patch (pccg.h): real since round 7b
 * (port/apple/pcconvert/pcsidecar.c, docs/port-ios/12-round7b-boot.md). */

/* ---- camera latch (camlatch.c, D-093) --------------------------------- */
void portCamLatchRecord(const float look[3], const float up[3], const float P[4][4],
                        const void *projMtx, const void *projViewMtx, int fp)
{
    (void) look;
    (void) up;
    (void) P;
    (void) projMtx;
    (void) projViewMtx;
    (void) fp;
}
void portCamLatchGun(const void *gdl, int begin)
{
    (void) gdl;
    (void) begin;
}
void portCamLatchCommit(const void *dlStart, const void *dlEnd)
{
    (void) dlStart;
    (void) dlEnd;
}

/* ---- widescreen / distance (video.c): identities ---------------------- */
f32 portScaleFovY(f32 fovy, s32 isTitleScreen)
{
    (void) isTitleScreen;
    return fovy;
}
s32 portFillViewport(void) { return 0; }
f32 portScaleAspect(f32 aspect) { return aspect; }
f32 portScreenSqueeze(void) { return 1.0f; }
f32 portDrawDistanceMultiplier(void) { return 1.0f; }
f32 portLodDistanceMultiplier(void) { return 1.0f; }

/* ---- shader prewarm (shaderprewarm.h) --------------------------------- */
void shaderPrewarmRequest(int levelid) { (void) levelid; }

/* ---- texture packs (texpack.h): round 11 links the real port/apple/texpack.c
 * (texpackRegisterTexture, texpackTexnumAt); the stubs that stood here are
 * gone (docs/port-ios/20-round11-texpack.md). */

/* ======================================================================
 * Round 2 (docs/port-ios/05-round2-merge.md): hooks the merged game
 * sources reference beyond the 36 above. Same rules: inert, Midas's
 * behaviour. Donor definitions are in GoldenEye-Decomp-ios engine/.
 * ====================================================================== */
#include <stdarg.h>
#include <time.h>

/* ---- audio telemetry / sim lock (port/src/audio.c, audiosync.c) ------- */
u64 audioDumpBytePos(void) { return 0; }
void audioNoteSfxStart(void) {}
int audioSyncEnabled(void) { return 0; }      /* Midas: no Audio.SimLock */
void audioSyncArm(void) {}
void audioSyncFrameDone(void) {}
int audioSyncWaitForTick(void) { return 1; }  /* never reached while disabled */

/* ---- sim tick / replay (port/src/gesched.c) ---------------------------- */
void geStackFuzz(void) {}
u32 geTickRngSeed(u32 wallCount) { return wallCount; } /* donor: unchanged unless GE_SEED */
void geTracePrintf(const char *path, const char *fmt, ...) { (void) path; (void) fmt; }

/* ---- logging / timing (port/src/system.c) ------------------------------ */
uint64_t sysGetMicroseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

/* ---- fast3d debug counters (port/fast3d/gfx_pc.cpp) -------------------- */
int gfxDbgLogoMenu;
unsigned long gfxDbgTriCount;
void videoRequestScreenshot(void) {}          /* port/src/video.c */

/* ---- donor config switches (port/src/config.c / video.c) --------------- */
s32 portNoHitFlash = 0;          /* donor default 0 == Midas */
f32 portScreenShakeScale = 1.0f; /* donor default 1.0 == Midas */
s32 portSkipIntro = 0;           /* donor default 0 == Midas */
int portWideSpans(void) { return 0; }            /* donor default 1; Midas has no D-052 spans */
int portAspectWideBackdrops(void) { return 0; }  /* Midas-only switch, gbi_extension.h (D-M02) */

/* ---- data-layout fixups: ROUND 7 DEPENDENCIES ---------------------------
 * In the donor these convert cart data into its 64-bit host layout. They are
 * no-ops here, so on 64-bit the data stays in whatever form Midas's own
 * loaders leave it (romdata.c / romfile_*.c). Round 7 (pcconvert) replaces
 * them. The *PcSize queries return the N64 size, i.e. no growth. */
/* pcmodelsPatchTable: real since round 7b (port/apple/pcconvert/pcsidecar.c). */
/* romdataFontPcSize / romdataFixupFont: real since round 7b (port/src/romdata.c). */
/* romdataAudioBankPcSize / romdataFixupAudioBank: real since the ripple round
 * (port/src/romdata.c, docs/port-ios/10-ripples.md A4). */
void romdataFixupAnimationData(u8 *blob, u32 blobSize, const s32 *tableA, const s32 *tableB)
{ (void) blob; (void) blobSize; (void) tableA; (void) tableB; }
void romdataFixupBriefing(u8 *blob) { (void) blob; }
void romdataFixupCseq(u8 *blob) { (void) blob; }
void romdataFixupLangBank(u8 *blob, u32 blobSize) { (void) blob; (void) blobSize; }
/* Round 7b: deliberately inert. music.c's own `#ifndef TARGET_N64` arm
 * right after each call swaps seqCount and every entry (address as a u32
 * under PORT, uncompressed_len, len) - the same fields the donor's version
 * swaps, so a real one here would double-swap the table. */
void romdataFixupMusicSeqTable(u8 *blob, u32 blobSize) { (void) blob; (void) blobSize; }
