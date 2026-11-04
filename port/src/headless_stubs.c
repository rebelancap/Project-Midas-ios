/**
 * Headless-build stubs (GE_HEADLESS=ON, e.g. the Linux AddressSanitizer
 * configuration): audio_pc.c and video.c need SDL2, which the -m32
 * sanitizer environment does not have. The game runs silent with the
 * null renderer; these satisfy the symbols their callers reference.
 */
#include <ultra64.h>

void portAudioPump(void)
{
}

void portAudioPumpForce(void)
{
}

/* blood_animation.c evicts CPU-animated textures from the fast3d cache;
 * no renderer, no cache. */
void gfx_texture_cache_delete(const unsigned char *orig_addr)
{
    (void)orig_addr;
}

/* texReset clears the whole fast3d texture cache at every stage load. */
void gfx_texture_cache_clear_c(void)
{
}

/* GE HW pacing: the renderer's per-frame RDP workload; no renderer, all zero. */
#include "../fast3d/gfx_api.h"
struct PortHwLoad g_PortHwLoadLast;

/* roc_hooks.c's voids for the room / part cache (gfx_pc.cpp); no renderer, no cache. */
void gfx_roc_queue_op(int op, int arg, const void *ptr)
{
    (void)op; (void)arg; (void)ptr;
}

/* GE_GFX_STATS tri counters, normally owned by video.c */
u32 ge_dbg0;
u32 ge_dbg1;
u32 ge_dbg2;

/* sky.c hands fast3d the sky-plane fan vertices via this to embed a G_NOOP
 * sequence marker; no renderer, no marker, just a stable non-zero sequence. */
uint32_t ge_sky_push(int n, const float *x, const float *y, const float *s, const float *t,
                      const float *iw, const float *r, const float *g, const float *b,
                      const float *a, float stmul)
{
    (void)n; (void)x; (void)y; (void)s; (void)t; (void)iw;
    (void)r; (void)g; (void)b; (void)a; (void)stmul;
    return 0;
}

/* midas.ini [display] getters, normally owned by video.c (GE_VIDEO-only).
 * Headless has no midas.ini to read, so return the same compiled-in defaults
 * video.c uses (see s_CfgFixShootThrough/s_CfgDistanceFading/etc. there). */
s32 portFixShootThrough(void)
{
    return 1;
}

s32 portDistanceFadingEnabled(void)
{
    return 0;
}

s32 portTrueEnemyRocketsEnabled(void)
{
    return 1;
}

s32 portAdzyMode(void)
{
    return 0;
}

s32 portVideoScopeSteady(void)
{
    return 0;
}

/* midas.ini [input] mouse_sens_aim, normally owned by input_pc.c (GE_VIDEO-only). */
f32 portInputMouseAimScale(void)
{
    return 1.0f;
}

/* Round 7b (docs/port-ios/12-round7b-boot.md): options/HUD/input hooks that
 * video.c and input_pc.c gained after these stubs were last synced - the
 * headless link was missing all 14. Values are the ini defaults (hud/health
 * not wide, crosshair follows the global, no pads, no keyboard). */
s32 portHealthWide(void) { return 0; }
s32 portHudWideCfg(void) { return -1; }
void portHudWideAdopt(s32 wide) { (void) wide; }
s32 portMpHudWide(void) { return 0; }
s32 portPlayerCrosshairOwn(s32 player) { (void) player; return -2; }
s32 portPlayerCrosshairColor(s32 player) { (void) player; return -1; }
void portPlayerCrosshairSet(s32 player, s32 rgb) { (void) player; (void) rgb; }
s32 portInputDeviceModeRaw(s32 dev) { (void) dev; return 0; }
s32 portInputLayoutInUse(s32 dev) { (void) dev; return 0; }
const char *portInputLayoutShown(s32 dev) { (void) dev; return ""; }
s32 portInputPadDigMoveP1(s32 *dx, s32 *dy) { *dx = 0; *dy = 0; return 0; }
s32 portInputPadResetDefaults(s32 dev) { (void) dev; return 0; }
void portVideoBindsResetDefaults(void) {}
void portVideoKbdDigitalMove(s32 *dx, s32 *dy) { *dx = 0; *dy = 0; }
