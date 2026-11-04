// GEBridgeMidas.m — weak no-op stand-ins for the donor engine hooks the dev
// bridge (GEBridge.m, GE_PUBLIC=0 only) calls and Midas does not have.
//
// Round 9b. The donor's bridge drives instruments that live in ITS engine
// (GoldenEye-Decomp-ios engine/port/src/hostinput.c, video.c: boot-stage,
// teleport, aim, front-menu, combat/audio counters, screenshot serials). Midas
// has the seam in doc 16 §2 and geHostPlayerState (round 9b); the rest
// answer zeros here, so the bridge's tap / drag / touch / state / page
// commands (which act on the shell's own overlay) work, and a command that
// needs a missing hook simply reports 0. WEAK: the day the engine defines one
// for real, the linker takes the engine's.
//
// Compiled into a public build too (it is tiny and inert), but nothing calls
// it there: GEBridge.m is three stubs under GE_PUBLIC=1.
#include <stddef.h>

#define GE_WEAK __attribute__((weak))

/* Round 31 (R27-8): geHostBootStage, geHostStageIsBootable,
 * geHostBootableStageList and geHostFrontMenu are real now
 * (port/apple/hostinput.c). */

GE_WEAK void geHostFrontCursor(float *h, float *v) { if (h) *h = 0; if (v) *v = 0; }
GE_WEAK void geHostFrontMenuRequest(int menu) { (void)menu; }
GE_WEAK void geHostSaveCompleteDam(int folder) { (void)folder; }
GE_WEAK void geHostMpDevGun(int item) { (void)item; }
GE_WEAK void geHostMpDevNear(int n, int m) { (void)n; (void)m; }
GE_WEAK int  geHostMpPlayer(int i, float *x, float *y, float *z, float *theta,
                            float *health, int *dead, int *deaths, int *kills, int *weapon,
                            int *aiming)
{
	(void)i; (void)x; (void)y; (void)z; (void)theta; (void)health; (void)dead;
	(void)deaths; (void)kills; (void)weapon; (void)aiming;
	return 0;
}
GE_WEAK void geHostAudioStats(unsigned int *sfxStarts, int *peak, unsigned int *blocks)
{
	if (sfxStarts) *sfxStarts = 0; if (peak) *peak = 0; if (blocks) *blocks = 0;
}
GE_WEAK void geHostCombatStats(unsigned int *bulletHits, unsigned int *bloodStains,
                               unsigned int *bloodRelated)
{
	if (bulletHits) *bulletHits = 0; if (bloodStains) *bloodStains = 0; if (bloodRelated) *bloodRelated = 0;
}
GE_WEAK void geHostAmmo(int *magRight, int *magLeft) { if (magRight) *magRight = -1; if (magLeft) *magLeft = -1; }
GE_WEAK void geHostAllGunsRequest(void) {}
GE_WEAK void geHostAimState(int *aiming, int *weapon, float *sniperZoom, float *fovy,
                            float *sway, int *crouch, unsigned int *zoomPolls)
{
	if (aiming) *aiming = 0; if (weapon) *weapon = 0; if (sniperZoom) *sniperZoom = 0;
	if (fovy) *fovy = 0; if (sway) *sway = 0; if (crouch) *crouch = 0; if (zoomPolls) *zoomPolls = 0;
}
GE_WEAK void geHostAimAtNearestChr(void) {}
GE_WEAK void geHostTeleportRequest(int mask, float x, float y, float z, float theta, float verta)
{
	(void)mask; (void)x; (void)y; (void)z; (void)theta; (void)verta;
}
GE_WEAK int  geHostTeleportResult(unsigned int *serial) { if (serial) *serial = 0; return 0; }
GE_WEAK int  geHostTeleportInsideChr(void) { return 0; }
GE_WEAK void geHostAimTarget(float *x, float *y, float *z) { if (x) *x = 0; if (y) *y = 0; if (z) *z = 0; }
GE_WEAK void geHostSetInvincible(int on) { (void)on; }
GE_WEAK int  geHostGetInvincible(void) { return 0; }
GE_WEAK float geHostAimResult(unsigned int *serial) { if (serial) *serial = 0; return 0.0f; }
GE_WEAK unsigned int geHostInputReassertCount(void) { return 0; }
GE_WEAK int  geHostInputOwnsHook(void) { return 1; }
GE_WEAK void geHostInputTestStealHook(void) {}
GE_WEAK float videoGetPresentRate(void) { return 0.0f; }
GE_WEAK unsigned videoScreenshotSerial(void) { return 0; }
GE_WEAK const char *videoLastScreenshotPath(void) { return ""; }
