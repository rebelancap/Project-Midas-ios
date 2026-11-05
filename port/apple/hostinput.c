/*
 * hostinput.c - the iOS shell's engine seam, over Midas's own code (round 9a,
 * docs/port-ios/16-round9a-ios-engine.md).
 *
 * The names and signatures are GoldenEye-Decomp-ios's (port/include/hostinput.h,
 * port/src/hostinput.c), so that donor's app/ios shell transplants with a rename
 * only. The IMPLEMENTATION is not the donor's: the donor feeds the sim through
 * joy.c's playback hook (g_ContPlaybackFunc) because its PC input layer is
 * PD's; Midas has its own pad pipeline (port/src/libultra_shim.c
 * osContGetReadData -> input_pc.c merges -> joy_port.c Port actions -> the N64
 * stick gate -> inputrec), and the host pad is merged INTO that pipeline at
 * one point (geHostInputPoll, GE_IOS arm), so Midas's own features --
 * Port actions, the stick gate, GE_INPUTREC -- see touch input exactly as they
 * see a controller. Look goes through Midas's direct-degree mouse path
 * (input_pc.c portInputMouseLookDeltaDeg, GE_IOS arm), so the game's own
 * permission predicate (portMouseLookPermitted), pitch inversion and aim-mode
 * crosshair apply to it unchanged.
 *
 * Built on every 64-bit host (port/apple/ glob) and inert unless the shell
 * calls it: nothing outside a GE_IOS arm reads this state, so the macOS
 * oracle and the headless gate are unchanged.
 *
 * Threading: the shell calls the setters from UIKit; the engine reads on the
 * game thread. Every shared word is a plain __atomic access (no header: the
 * decomp's include/ shadows the libc headers).
 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hostinput.h"
#include "game/bondview.h"
#include "game/player.h"

#define AT_LOAD(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define AT_STORE(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define AT_XCHG(p, v)   __atomic_exchange_n((p), (v), __ATOMIC_RELAXED)
#define AT_ADD(p, v)    __atomic_fetch_add((p), (v), __ATOMIC_RELAXED)

extern void portConLogf(const char *fmt, ...);
static void geHostApplyTeleport(void);
extern s32 g_StageNum;          /* src/boss.c */
extern s32 g_GlobalTimer;       /* src/game/lv.c */
extern s32 g_PortStatFps;       /* src/game/frametiming.c */
extern s32 getPlayerCount(void);

/* src/game/options.c: the game's own per-player options (the EEPROM folder) */
extern u32  cur_player_get_lookahead(void);
extern void cur_player_set_lookahead(u32 on);
extern u32  cur_player_get_sight_onscreen_control(void);
extern void cur_player_set_sight_onscreen_control(u32 on);
extern u32  cur_player_get_ammo_onscreen_setting(void);
extern void cur_player_set_ammo_onscreen_setting(u32 on);
extern s32  cur_player_get_autoaim(void);
extern void cur_player_set_autoaim(u32 on);

/* port_options.c: Midas's options table (midas.ini keys) */
extern s32 portOptFind(const char *key);
extern s32 portOptValueIni(s32 i, char *buf, s32 n);
extern s32 portOptSetIni(s32 i, const char *text);
extern void portOptSave(void);
extern void portOptSaveHold(s32 on);

/* input_pc.c. True when this slot uses N64 control styles (C-buttons
 * sidestep). False for port-driven play, including a phone with no pad. */
extern s32 portInputPadIsN64(s32 slot);

/* video.c. show_fps is a live [display] key with no options-table row, so
 * portOptFind cannot see it. The console writes the ini itself; this seam
 * does the same. */
extern s32 portVideoOptionGet(const char *key, s32 *out);
extern s32 portVideoOptionSet(const char *key, s32 value);
extern s32 portIniWriteStrs(const char *section, const char *const *keys, const char *const *vals, s32 n);

/* ---- state ---------------------------------------------------------------- */

/* packed button<<16 | (u8)x<<8 | (u8)y, one per port */
static unsigned int s_pads[GE_HOST_MAX_PADS];
static int s_padCount = 1;
static unsigned int s_padSets;
static unsigned int s_padEdges;
static unsigned int s_padEdgesN[GE_HOST_MAX_PADS];
static unsigned int s_padLastButtonN[GE_HOST_MAX_PADS];
static unsigned short s_padPrevButton[GE_HOST_MAX_PADS];   /* game thread only */
static int s_lookYawMdeg[GE_HOST_MAX_PADS];                 /* millidegrees */
static int s_lookPitchMdeg[GE_HOST_MAX_PADS];
static unsigned int s_reloadOnly;
static unsigned int s_polls;
static int s_active;

/* The game's options, queued by the settings page and applied on the game
 * thread at the next poll; -1 = nothing pending (donor shape). */
enum { GE_OPT_LOOKAHEAD = 0, GE_OPT_CROSSHAIR, GE_OPT_AMMO, GE_OPT_AUTOAIM, GE_OPT_COUNT };
static int s_pendingOption[GE_OPT_COUNT] = { -1, -1, -1, -1 };

/* R27-8 (round 31): the bridge's boot-stage, queued by the shell and applied
 * on the game thread at the next pad read; -1 = nothing pending. */
static int s_pendingStage = -1;

/* D-051, narrowed for Midas. The donor (GoldenEye 1.1/HONEY) has no analog
 * sidestep: stick X is a turn, and look-drag is the turn, so a host stick X
 * became C-left/C-right. Midas's port-driven path strafes FROM analog X and
 * then clears those digital steps (bondview2.c), so folding the axis away
 * made the on-screen stick unable to strafe. Fold only while slot 0 is an
 * N64-styled pad. Menus keep the analog X either way (the front-end
 * crosshair reads the stick). GE_HOST_ANALOG_SIDESTEP=1 skips the fold. */
#define GE_HOST_SIDESTEP_MIN 36

/* ---- published by the shell ----------------------------------------------- */

void geHostPadSetN(int port, unsigned short button, signed char stickX, signed char stickY)
{
    unsigned int packed = ((unsigned int)button << 16)
                        | ((unsigned int)(unsigned char)stickX << 8)
                        |  (unsigned int)(unsigned char)stickY;
    unsigned int prev;

    if (port < 0 || port >= GE_HOST_MAX_PADS)
    {
        return;
    }
    prev = AT_XCHG(&s_pads[port], packed);
    if (prev != packed)
    {
        AT_ADD(&s_padSets, 1u);
    }
}

void geHostPadSet(unsigned short button, signed char stickX, signed char stickY)
{
    geHostPadSetN(0, button, stickX, stickY);
}

void geHostPadCountSet(int count)
{
    if (count < 1) count = 1;
    if (count > GE_HOST_MAX_PADS) count = GE_HOST_MAX_PADS;
    AT_STORE(&s_padCount, count);
}

int geHostPadCount(void)
{
    return AT_LOAD(&s_padCount);
}

void geHostPadStatsN(int port, unsigned int *edges, unsigned int *lastButton)
{
    if (port < 0 || port >= GE_HOST_MAX_PADS)
    {
        if (edges) *edges = 0;
        if (lastButton) *lastButton = 0;
        return;
    }
    if (edges) *edges = AT_LOAD(&s_padEdgesN[port]);
    if (lastButton) *lastButton = AT_LOAD(&s_padLastButtonN[port]);
}

void geHostPadStats(unsigned int *sets, unsigned int *edges, unsigned int *lastButton)
{
    if (sets) *sets = AT_LOAD(&s_padSets);
    if (edges) *edges = AT_LOAD(&s_padEdges);
    if (lastButton) *lastButton = AT_LOAD(&s_padLastButtonN[0]);
}

void geHostReloadOnlySet(int on)
{
    AT_STORE(&s_reloadOnly, on ? 1u : 0u);
}

/* src/game/lv.c (GE_IOS arm, imported in round 2): RELOAD skips the
 * activate test for that B press. */
int geHostReloadOnly(void)
{
    return (int)AT_LOAD(&s_reloadOnly);
}

void geHostLookAddN(int port, float yawDeg, float pitchDeg)
{
    if (port < 0 || port >= GE_HOST_MAX_PADS)
    {
        return;
    }
    AT_ADD(&s_lookYawMdeg[port], (int)(yawDeg * 1000.0f));
    AT_ADD(&s_lookPitchMdeg[port], (int)(pitchDeg * 1000.0f));
}

void geHostLookAdd(float yawDeg, float pitchDeg)
{
    geHostLookAddN(0, yawDeg, pitchDeg);
}

/* ---- consumed by the engine (game thread) --------------------------------- */

/* input_pc.c portInputMouseTickConsume, GE_IOS arm: this tick's host
 * look for player 1, in degrees (yaw +right, pitch +down, i.e. the mouse's
 * own signs). Ports 1-3 are drained and dropped: Midas's direct look is
 * player 1 only (bondview2.c), the same rule as its mouse. */
void geHostLookTake(float *yawDeg, float *pitchDeg)
{
    int y = AT_XCHG(&s_lookYawMdeg[0], 0);
    int p = AT_XCHG(&s_lookPitchMdeg[0], 0);
    int i;

    for (i = 1; i < GE_HOST_MAX_PADS; i++)
    {
        (void)AT_XCHG(&s_lookYawMdeg[i], 0);
        (void)AT_XCHG(&s_lookPitchMdeg[i], 0);
    }
    if (yawDeg) *yawDeg = (f32)y / 1000.0f;
    if (pitchDeg) *pitchDeg = (f32)p / 1000.0f;
}

/* Round 57 (docs/port-ios/77): Settings -> Controller's sensitivity sliders
 * and invert switch for the Look stick of the pads SDL owns (every real
 * controller on a phone; bondview2.c GE_IOS arms read these). 1.0 = Midas's
 * own rate (PORT_PADLOOK_* and GoldenEye's natural-look curve); a negative Y
 * inverts. Plain word stores: the shell writes on the main thread, the sim
 * thread reads once per tick, and a torn read cannot happen on a float. */
f32 g_portPadLookScaleX = 1.0f;
f32 g_portPadLookScaleY = 1.0f;

void geHostSetPadLook(float scaleX, float scaleY, int invert)
{
    f32 x = scaleX;
    f32 y = scaleY;

    if (!(x > 0.05f)) x = 0.05f;   /* also catches NaN */
    if (!(y > 0.05f)) y = 0.05f;
    if (x > 5.0f) x = 5.0f;
    if (y > 5.0f) y = 5.0f;
    if (invert) y = -y;
    if (x != g_portPadLookScaleX || y != g_portPadLookScaleY)
    {
        g_portPadLookScaleX = x;
        g_portPadLookScaleY = y;
        portConLogf("ge: hostinput: pad look scale x=%.3f y=%.3f (shell)\n", (double)x, (double)y);
    }
}

void geHostGetPadLook(float *scaleX, float *scaleY)
{
    if (scaleX) *scaleX = g_portPadLookScaleX;
    if (scaleY) *scaleY = g_portPadLookScaleY;
}

/* ---- round 59: Midas Port actions from the shell -------------------------
 * docs/port-ios/79. The touch overlay's CROUCH and GADGET buttons, the
 * aim-mode stick zoom, and the shell's own controller mapping (pads SDL does
 * not hold) drive Midas's Port actions (port/src/joy_port.c) per pad, the
 * same way a controller's custom map does: geHostInputPoll calls
 * portJoyPortPadDown for every action held here BEFORE the Port commit, so
 * the edge latch, the 0.25 s staleness, crouch_toggle's latch and the
 * shoot_next_prev chord are all Midas's own code. A level (0..1) is kept
 * as well, for the held actions read as a magnitude (zoom_in / zoom_out via
 * portJoyPortLevel, GE_IOS arm). Sources compose by max: 0 = touch overlay,
 * 1 = the shell's controller mapping. A press that comes and goes between
 * two polls still counts for one poll (s_actPressed). Inert unless the shell
 * calls it, like everything else in this file. */
#define GE_HOST_ACT_SRCS 2
#define GE_HOST_ACT_MAX  32
static int s_actLevelMilli[GE_HOST_ACT_SRCS][GE_HOST_MAX_PADS][GE_HOST_ACT_MAX];
static unsigned int s_actPressed[GE_HOST_MAX_PADS];
static unsigned int s_actZoomPolls;

extern s32 portJoyPortBindCount(void);
extern const char *portJoyPortBindName(s32 i);
extern void portJoyPortPadDown(s32 pad, s32 act);
extern s32 portJoyPortActZoomIn(void);
extern s32 portJoyPortActZoomOut(void);

int geHostPortActionFind(const char *name)
{
    s32 n = portJoyPortBindCount();
    s32 i;

    if (name == NULL)
    {
        return -1;
    }
    if (n > GE_HOST_ACT_MAX)
    {
        n = GE_HOST_ACT_MAX;
    }
    for (i = 0; i < n; i++)
    {
        const char *b = portJoyPortBindName(i);

        if (b != NULL && strcmp(b, name) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

void geHostPortActionSet(int src, int pad, int act, float level)
{
    int milli;
    int prev;

    if (src < 0 || src >= GE_HOST_ACT_SRCS || pad < 0 || pad >= GE_HOST_MAX_PADS
        || act < 0 || act >= GE_HOST_ACT_MAX)
    {
        return;
    }
    if (!(level > 0.0f))   /* also NaN */
    {
        milli = 0;
    }
    else
    {
        milli = (level >= 1.0f) ? 1000 : (int)(level * 1000.0f + 0.5f);
        if (milli < 1)
        {
            milli = 1;
        }
    }
    prev = AT_XCHG(&s_actLevelMilli[src][pad][act], milli);
    if (milli > 0 && prev == 0)
    {
        __atomic_fetch_or(&s_actPressed[pad], 1u << act, __ATOMIC_RELAXED);
    }
}

void geHostPortActionClear(int src, int pad)
{
    int p;
    int a;

    if (src < 0 || src >= GE_HOST_ACT_SRCS)
    {
        return;
    }
    for (p = 0; p < GE_HOST_MAX_PADS; p++)
    {
        if (pad >= 0 && p != pad)
        {
            continue;
        }
        for (a = 0; a < GE_HOST_ACT_MAX; a++)
        {
            AT_STORE(&s_actLevelMilli[src][p][a], 0);
        }
    }
}

static int geHostActMilli(int pad, int act)
{
    int best = 0;
    int s;

    for (s = 0; s < GE_HOST_ACT_SRCS; s++)
    {
        const int v = AT_LOAD(&s_actLevelMilli[s][pad][act]);

        if (v > best)
        {
            best = v;
        }
    }
    return best;
}

/* joy_port.c portJoyPortLevel, GE_IOS arm: the shell's level for one action. */
f32 geHostPortActionLevel(s32 pad, s32 act)
{
    if (pad < 0 || pad >= GE_HOST_MAX_PADS || act < 0 || act >= GE_HOST_ACT_MAX)
    {
        return 0.0f;
    }
    return (f32)geHostActMilli((int)pad, (int)act) / 1000.0f;
}

/* geHostInputPoll, before the Port commit: hold every shell action down for
 * this poll on its pad. */
static void geHostPortActionsPoll(void)
{
    const s32 zi = portJoyPortActZoomIn();
    const s32 zo = portJoyPortActZoomOut();
    int pad;
    int a;

    for (pad = 0; pad < GE_HOST_MAX_PADS; pad++)
    {
        const unsigned int pressed = AT_XCHG(&s_actPressed[pad], 0u);

        for (a = 0; a < GE_HOST_ACT_MAX; a++)
        {
            if (geHostActMilli(pad, a) > 0 || ((pressed >> a) & 1u))
            {
                portJoyPortPadDown(pad, a);
            }
        }
    }
    if (geHostActMilli(0, (int)zi) > 0 || geHostActMilli(0, (int)zo) > 0)
    {
        AT_ADD(&s_actZoomPolls, 1u);
    }
}

/* Player 1 is in aim mode right now (the touch stick zooms instead of
 * walking). Racy word read from the main thread; 0 when there is no player. */
int geHostPlayerAiming(void)
{
    struct player *p = g_playerPointers[0];

    if (g_StageNum <= 0 || g_StageNum == LEVELID_TITLE || p == NULL)
    {
        return 0;
    }
    return p->insightaimmode != 0;
}

/* D-138 contract (hostinput.h), real on Midas since round 59: player 1's aim
 * mode, right-hand weapon, sniper FOV, camera zoom FOV, lean target, crouch
 * position, and the polls a shell zoom level was held on. */
void geHostAimState(int *aiming, int *weapon, float *sniperZoom, float *fovy,
                    float *sway, int *crouch, unsigned int *zoomPolls)
{
    struct player *p = g_playerPointers[0];

    if (aiming) *aiming = -1;
    if (weapon) *weapon = -1;
    if (sniperZoom) *sniperZoom = 0;
    if (fovy) *fovy = 0;
    if (sway) *sway = 0;
    if (crouch) *crouch = 0;
    if (zoomPolls) *zoomPolls = AT_LOAD(&s_actZoomPolls);
    if (g_StageNum <= 0 || g_StageNum == LEVELID_TITLE || p == NULL)
    {
        return;
    }
    if (aiming) *aiming = (int)p->insightaimmode;
    if (weapon) *weapon = (int)p->hands[GUNRIGHT].weaponnum;
    if (sniperZoom) *sniperZoom = p->sniper_zoom;
    if (fovy) *fovy = p->zoominfovy;
    if (sway) *sway = p->swaytarget;
    if (crouch) *crouch = (int)p->crouchpos;
}

/* libultra_shim.c contConnectedMask, GE_IOS arm: the ports the shell
 * publishes are plugged in. */
unsigned int geHostPadMask(void)
{
    return (1u << AT_LOAD(&s_padCount)) - 1u;
}

static void geHostApplyPendingOptions(void)
{
    int i;

    for (i = 0; i < GE_OPT_COUNT; i++)
    {
        int v = AT_XCHG(&s_pendingOption[i], -1);

        if (v < 0)
        {
            continue;
        }
        switch (i)
        {
        case GE_OPT_LOOKAHEAD: cur_player_set_lookahead((u32)v); break;
        case GE_OPT_CROSSHAIR: cur_player_set_sight_onscreen_control((u32)v); break;
        case GE_OPT_AMMO:      cur_player_set_ammo_onscreen_setting((u32)v); break;
        case GE_OPT_AUTOAIM:   cur_player_set_autoaim((u32)v); break;
        default: break;
        }
    }
}

/* ---- boot-stage (R27-8, round 31) ------------------------------------------
 * The donor's D-260 shape (GoldenEye-Decomp-ios engine/port/src/hostinput.c
 * geHostBootStage / geHostApplyPendingStage): only the 21 solo stages, then
 * the ramrom/GE_STAGE launch recipe (folder 1, Agent, briefing) and the boss
 * loop's own stage switch (bossSetLoadedStage), from the title screens or a
 * running stage. The bridge (GEBridge.m cmdBootStage) refuses every other
 * menu. Dev bridge only: nothing else calls geHostBootStage. */
extern void fileValidateSaves(void);                 /* src/game/file2.c */
extern void fileSetCurrentFolder(u32 foldernum);     /* src/game/file.c */
extern void set_selected_difficulty(s32 difficulty); /* src/game/file.c */
extern void set_solo_and_ptr_briefing(s32 stage);    /* src/game/file.c */
extern void bossSetLoadedStage(s32 stage);           /* src/boss.c */
extern s32 get_currentmenu(void);                    /* src/game/front.c */

static const struct { const char *name; int num; } kBootStages[] = {
    {"Dam",33}, {"Facility",34}, {"Runway",35}, {"Surface1",36},
    {"Bunker1",9}, {"Silo",20}, {"Frigate",26}, {"Surface2",43},
    {"Bunker2",27}, {"Statue",22}, {"Archives",24}, {"Streets",29},
    {"Depot",30}, {"Train",25}, {"Jungle",37}, {"Control",23},
    {"Caverns",39}, {"Cradle",41}, {"Aztec",28}, {"Egypt",32},
    {"Cuba",54},
};

int geHostStageIsBootable(int stage)
{
    size_t i;

    for (i = 0; i < sizeof(kBootStages) / sizeof(kBootStages[0]); ++i)
    {
        if (kBootStages[i].num == stage)
        {
            return 1;
        }
    }
    return 0;
}

const char *geHostBootableStageList(void)
{
    static char buf[512];
    size_t i, n = 0;

    if (!buf[0])
    {
        for (i = 0; i < sizeof(kBootStages) / sizeof(kBootStages[0]) && n < sizeof(buf); ++i)
        {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s(%d)", i ? "," : "",
                                  kBootStages[i].name, kBootStages[i].num);
        }
    }
    return buf;
}

void geHostBootStage(int stage)
{
    if (stage > 0 && !geHostStageIsBootable(stage))
    {
        portConLogf("ge: hostinput: boot-stage %d refused, not a solo stage\n", stage);
        return;
    }
    AT_STORE(&s_pendingStage, stage);
    portConLogf("ge: hostinput: boot-stage %d requested\n", stage);
}

/* GE's MENU enum (front.h): 0-4 title/attract, 11 = MENU_RUN_STAGE. */
int geHostFrontMenu(void)
{
    return (int)get_currentmenu();
}

static void geHostApplyPendingStage(void)
{
    const int stage = AT_XCHG(&s_pendingStage, -1);

    if (stage <= 0)
    {
        return;
    }
    portConLogf("ge: hostinput: loading stage %d (menu %d, from stage %d)\n",
                stage, geHostFrontMenu(), (int)g_StageNum);
    fileValidateSaves();
    fileSetCurrentFolder(0 /* FOLDER1 */);
    set_selected_difficulty(0 /* DIFFICULTY_AGENT */);
    set_solo_and_ptr_briefing(stage);
    bossSetLoadedStage(stage);
}

static s8 geHostStickAdd(s8 a, s8 b)
{
    s32 v = (s32)a + (s32)b;

    return (s8)(v > 127 ? 127 : v < -128 ? -128 : v);
}

/*
 * libultra_shim.c osContGetReadData, GE_IOS arm: once per pad read, on
 * the game thread, AFTER Midas's own sources are merged and BEFORE the Port
 * suite commit and the N64 stick gate. Buttons OR in, sticks add (saturated),
 * so a Bluetooth controller (SDL) and the touch overlay compose.
 */
void geHostInputPoll(void *pads)
{
    OSContPad *data = (OSContPad *)pads;
    const int count = AT_LOAD(&s_padCount);
    const int inPlay = geHostInGameplay();
    static int analogSidestep = -1;
    int i;

    if (analogSidestep < 0)
    {
        const char *e = getenv("GE_HOST_ANALOG_SIDESTEP");

        analogSidestep = (e != NULL && e[0] == '1');
    }
    if (!s_active)
    {
        s_active = 1;
        portConLogf("ge: hostinput: iOS host pad merged into osContGetReadData (%d port(s))\n", count);
    }
    AT_ADD(&s_polls, 1u);
    geHostApplyPendingOptions();
    geHostApplyPendingStage();
    geHostApplyTeleport();

    for (i = 0; i < count && i < MAXCONTROLLERS; i++)
    {
        const unsigned int packed = AT_LOAD(&s_pads[i]);
        unsigned short button = (unsigned short)(packed >> 16);
        s8 sx = (s8)(unsigned char)(packed >> 8);
        const s8 sy = (s8)(unsigned char)packed;
        const unsigned short rose = (unsigned short)(button & ~s_padPrevButton[i]);

        s_padPrevButton[i] = button;
        if (rose != 0)
        {
            AT_ADD(&s_padEdgesN[i], 1u);
            if (i == 0)
            {
                AT_ADD(&s_padEdges, 1u);
            }
        }
        AT_STORE(&s_padLastButtonN[i], (unsigned int)button);

        if (inPlay && !analogSidestep && i == 0 && portInputPadIsN64(0))
        {
            if (sx <= -GE_HOST_SIDESTEP_MIN) button |= L_CBUTTONS;
            if (sx >= GE_HOST_SIDESTEP_MIN)  button |= R_CBUTTONS;
            sx = 0;
        }

        data[i].errno = 0;
        data[i].button |= button;
        data[i].stick_x = geHostStickAdd(data[i].stick_x, sx);
        data[i].stick_y = geHostStickAdd(data[i].stick_y, sy);
    }
    geHostPortActionsPoll();   /* round 59: before the Port commit */
}

/* The donor installs a playback hook here; Midas needs none (the merge is a
 * fixed call in osContGetReadData). Kept so the shell's call site links. */
int geHostInputInstall(void)
{
    return 1;
}

int geHostInputActive(void)
{
    return s_active;
}

/* ---- settings stores ------------------------------------------------------ */

/* show_fps is ini-only (port_options.c). The settings page drops any key
 * geHostConfigGet cannot read, so this one row is special-cased. */
static int geHostShowFpsGet(char *out, int outLen)
{
    s32 v = 0;

    if (out == NULL || outLen <= 0 || !portVideoOptionGet("show_fps", &v))
    {
        return -1;
    }
    snprintf(out, (size_t)outLen, "%d", (int)(v != 0));
    return 0;
}

static int geHostShowFpsSet(const char *value)
{
    s32 live = 0;
    char buf[16];
    const char *k[1];
    const char *v[1];

    if (value == NULL || !portVideoOptionSet("show_fps", atoi(value)))
    {
        return -1;
    }
    if (!portVideoOptionGet("show_fps", &live))
    {
        live = atoi(value) != 0;
    }
    snprintf(buf, sizeof(buf), "%d", (int)(live != 0));
    k[0] = "show_fps";
    v[0] = buf;
    portIniWriteStrs("[display]", k, v, 1);
    portConLogf("ge: hostinput: show_fps = %s (shell)\n", buf);
    return 0;
}

/* Midas's options table by midas.ini key (port_options.c: "fov", "maxfps",
 * "msaa", ...). NOT the donor's "Video.MSAA" names: the settings page is
 * rewritten over this table in round 9b. 0 = ok, -1 = no such row / bad
 * value (donor convention). */
/* Round 16 (XBLA step 5): the donor-registrar keys ("Video.XblaModels",
 * "Video.TexturePack", ...) that live in midas.ini [video] through
 * port/apple/xblaconfig.c, not in Midas's options table. Tried after it. */
extern int configGetString(const char *key, char *out, int outLen);
extern int configSetString(const char *key, const char *value);

int geHostConfigGet(const char *key, char *out, int outLen)
{
    const s32 i = portOptFind(key);

    if (out != NULL && outLen > 0)
    {
        out[0] = '\0';
    }
    if (key != NULL && strcmp(key, "show_fps") == 0)
    {
        return geHostShowFpsGet(out, outLen);
    }
    if (i < 0 && key != NULL && strchr(key, '.') != NULL)
    {
        return configGetString(key, out, outLen);
    }
    if (i < 0 || out == NULL || outLen <= 0)
    {
        return -1;
    }
    return portOptValueIni(i, out, outLen) ? 0 : -1;
}

int geHostConfigSet(const char *key, const char *value)
{
    const s32 i = portOptFind(key);

    if (key != NULL && strcmp(key, "show_fps") == 0)
    {
        return geHostShowFpsSet(value);
    }
    if (i < 0 && key != NULL && value != NULL && strchr(key, '.') != NULL)
    {
        if (configSetString(key, value) != 0)   /* applies live and saves midas.ini (R11-5) */
        {
            return -1;
        }
        portConLogf("ge: hostinput: %s = %s (shell)\n", key, value);
        return 0;
    }
    if (i < 0 || value == NULL)
    {
        return -1;
    }
    if (!portOptSetIni(i, value))   /* applies live and saves midas.ini */
    {
        return -1;
    }
    portConLogf("ge: hostinput: %s = %s (shell)\n", key, value);
    return 0;
}

static int geOptionIndex(const char *name)
{
    if (name == NULL) return -1;
    if (strcmp(name, "LookAhead") == 0)   return GE_OPT_LOOKAHEAD;
    if (strcmp(name, "Crosshair") == 0)   return GE_OPT_CROSSHAIR;
    if (strcmp(name, "AmmoDisplay") == 0) return GE_OPT_AMMO;
    if (strcmp(name, "AutoAim") == 0)     return GE_OPT_AUTOAIM;
    return -1;
}

/* A pending write wins, so the page reads back what it just wrote. */
int geHostGameOptionGet(const char *name)
{
    const int idx = geOptionIndex(name);
    int pending;

    if (idx < 0)
    {
        return -1;
    }
    pending = AT_LOAD(&s_pendingOption[idx]);
    if (pending >= 0)
    {
        return pending;
    }
    switch (idx)
    {
    case GE_OPT_LOOKAHEAD: return (int)cur_player_get_lookahead();
    case GE_OPT_CROSSHAIR: return (int)cur_player_get_sight_onscreen_control();
    case GE_OPT_AMMO:      return (int)cur_player_get_ammo_onscreen_setting();
    case GE_OPT_AUTOAIM:   return (int)cur_player_get_autoaim();
    default:               return -1;
    }
}

int geHostGameOptionSet(const char *name, int value)
{
    const int idx = geOptionIndex(name);

    if (idx < 0)
    {
        return -1;
    }
    AT_STORE(&s_pendingOption[idx], value ? 1 : 0);
    return 0;
}

/* sceneWillResignActive: the process may be killed with no atexit. The EEPROM
 * needs nothing (libultra_shim.c eepromFlush rewrites Data/Saves/eeprom.bin on
 * every write); midas.ini is rewritten on every option step, except while a
 * slider ramp holds it (portOptSaveHold) -- release that, and write once. */
void geHostFlushSaves(void)
{
    portOptSaveHold(0);
    portOptSave();
}

/* ---- read back by the shell ----------------------------------------------- */

/* Every uncertain answer is "no" (donor rule: a wrongly visible gear is
 * untidy, a wrongly hidden one is unreachable settings). */
int geHostInGameplay(void)
{
    struct player *p = g_CurrentPlayer;

    if (g_StageNum <= 0 || g_StageNum == LEVELID_TITLE)
    {
        return 0;
    }
    if (p == NULL || p->prop == NULL)
    {
        return 0;
    }
    return p->pause_state == 0;
}

/* ticks = pad reads this process has served (one per sim poll); frames =
 * g_GlobalTimer, as the donor reports. */
void geHostState(int *stage, float *fps, unsigned int *ticks, unsigned int *frames)
{
    if (stage) *stage = (int)g_StageNum;
    if (fps) *fps = (float)g_PortStatFps;
    if (ticks) *ticks = AT_LOAD(&s_polls);
    if (frames) *frames = (unsigned int)g_GlobalTimer;
}

int geHostSplitScreenPlayers(void)
{
    const s32 n = getPlayerCount();

    if (g_StageNum <= 0 || g_StageNum == LEVELID_TITLE || g_CurrentPlayer == NULL)
    {
        return 0;
    }
    return n >= 2 ? (int)n : 0;
}

/* ---- documented stubs: donor concepts Midas does not have ------------------
 * Listed in docs/port-ios/16 §2 with what round 9b/9c must decide. */

/* (geSetStageTexPoolBytes is real: src/game/image.c, the donor's D-064 pool
 * knob, arrived with the round-2 merge.) */

/* Donor gesched.c: GEPacing's `sim_ticks 0/1/2+` column - how many presents
 * since the last call carried 0, 1 and 2+ sim ticks. Round 9c: real, from
 * Midas's own tick counter (g_GlobalTickCount, the one the title-bar `tps`
 * reads). geTickNotePresent is called by gfx_sdl2.cpp's GE_IOS arm after each
 * present; both run on the presenting thread, so the bins need no lock. */
extern s32 g_GlobalTickCount;   /* src/game/lv.c */
static s32 s_tickAtPresent;
static int s_tickAtPresentValid;
static unsigned s_tickBins[3];

void geTickNotePresent(void)
{
    const s32 now = g_GlobalTickCount;
    if (s_tickAtPresentValid)
    {
        const s32 d = now - s_tickAtPresent;
        s_tickBins[d <= 0 ? 0 : d == 1 ? 1 : 2]++;
    }
    s_tickAtPresent = now;
    s_tickAtPresentValid = 1;
}

void geTickTakeStats(unsigned *t0, unsigned *t1, unsigned *t2)
{
    if (t0) *t0 = s_tickBins[0];
    if (t1) *t1 = s_tickBins[1];
    if (t2) *t2 = s_tickBins[2];
    s_tickBins[0] = s_tickBins[1] = s_tickBins[2] = 0;
}

/* Donor D-065 (libultra.c): the display link posts the VI retrace. Midas's
 * pcsched.c owns its VI clock (tickhz / vsync lock); recorded only, so the
 * shell's VI-pacer toggle is a no-op until 9c. */
static int s_extViPacer;
void portSetExternalVIPacer(int on)
{
    if (on != s_extViPacer)
    {
        portConLogf("ge: hostinput: external VI pacer %s requested (not wired in Midas, round 9a)\n",
                    on ? "ON" : "OFF");
    }
    s_extViPacer = on;
}

void portExternalVIPulse(void)
{
}

/* Round 9b: the donor's geHostPlayerState (GoldenEye-Decomp-ios
 * engine/port/src/hostinput.c, 55a3a9c8), so the shell's dev bridge `state`
 * can say which way Bond faces: R9A-2 (does a touch drag right turn him
 * right?) is a number, not a pixel diff. Read-only; style is not reported
 * (-1). */
void geHostPlayerState(float *x, float *y, float *z, float *theta, float *verta, int *style)
{
    struct player *p = g_CurrentPlayer;

    if (x) *x = 0;
    if (y) *y = 0;
    if (z) *z = 0;
    if (theta) *theta = 0;
    if (verta) *verta = 0;
    if (style) *style = -1;
    if (p == NULL)
    {
        return;
    }
    if (theta) *theta = (float)p->vv_theta;
    if (verta) *verta = (float)p->vv_verta;
    if (p->prop == NULL)
    {
        return;
    }
    if (x) *x = (float)p->prop->pos.f[0];
    if (y) *y = (float)p->prop->pos.f[1];
    if (z) *z = (float)p->prop->pos.f[2];
}

#if defined(GE_VISION_3D)
/* Round 56 (D-064): a monotonic clock for the crosshair depth's smoothing
 * (gunfire.c portXhairStereoDepth; 3D only, never a sim input). */
#include <time.h>
double geHostNowSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Round 52 (Phase 6 M8, docs/port-ios/72 §A1): the unit-scale instrument for
 * `3d state` - Bond's eye height (the game's own eyeheight), his prop's y (the
 * camera's height) and the stand tile's floor y under him, all world units. */
void geHostPlayerHeights(float *eyeheight, float *posY, float *stanY, float *persp)
{
    struct player *p = g_CurrentPlayer;

    if (eyeheight) *eyeheight = 0;
    if (posY) *posY = 0;
    if (stanY) *stanY = 0;
    if (persp) *persp = 0;
    if (p == NULL)
    {
        return;
    }
    if (eyeheight) *eyeheight = p->eyeheight;
    if (stanY) *stanY = p->stanHeight;
    if (persp && g_playerPerm != NULL) *persp = g_playerPerm->player_perspective_height;
    if (posY && p->prop != NULL) *posY = (float)p->prop->pos.f[1];
}
#endif

/* Round 53 (docs/port-ios/73): the dev bridge's `teleport <x> <y> <z> [theta
 * [verta]]` / `look <verta> [theta]`, ported from GoldenEye-Decomp-ios
 * engine/port/src/hostinput.c (round 20b, geHostApplyTeleportArgs; Midas had
 * only the shell's weak stubs, doc 67 §4 item 7). It does what GoldenEye's
 * own MP respawn does: the stand tile under the point, its floor height,
 * y = eyeheight + floor, change_player_pos_to_target, the prop/prev copies,
 * then bondviewUpdatePlayerRoom. Requested on the bridge thread, served on
 * the game thread in geHostInputPoll. Result: room >= 0, -1 no tile, -2 no
 * player. Needed to park the 3D gate at Dam's spillway grating and yard. */
#include "game/stan.h"
extern void bondviewUpdatePlayerRoom(struct player *player);

#define TP_POS   1
#define TP_THETA 2
#define TP_VERTA 4
static float        s_tpArgs[5];
static int          s_tpRequest;
static unsigned int s_tpServed;
static int          s_tpResult;

static int geHostApplyTeleportArgs(int mask, const float *a)
{
    struct player *p = g_CurrentPlayer;
    int result = 0;

    if (p == NULL || p->prop == NULL)
    {
        return -2;
    }
    if (mask & TP_POS)
    {
        coord3d probe, pos;
        StandTile *stan;
        f32 floorY;

        probe.f[0] = a[0]; probe.f[1] = a[1]; probe.f[2] = a[2];
        stan = stanFindTileBelowPos(&probe, NULL, NULL);
        if (stan == NULL)
        {
            return -1;
        }
        floorY = bondviewYPositionRelated(stan, probe.f[0], probe.f[2]);
        pos.f[0] = probe.f[0];
        pos.f[2] = probe.f[2];
        pos.f[1] = p->eyeheight + floorY;
        p->field_70 = floorY;
        p->stanHeight = floorY;
        p->field_6C = (f32)(floorY / 0.17000002f);
        change_player_pos_to_target(&p->field_488, &pos, stan);
        p->prop->pos.f[0] = p->bondprevpos.f[0] = pos.f[0];
        p->prop->pos.f[1] = p->bondprevpos.f[1] = pos.f[1];
        p->prop->pos.f[2] = p->bondprevpos.f[2] = pos.f[2];
        p->prop->stan = stan;
        p->field_3B8.f[0] = (f32)(p->field_488.pos.f[0] / 0.100000024f);
        p->field_3B8.f[1] = (f32)(p->field_488.pos.f[1] / 0.100000024f);
        p->field_3B8.f[2] = (f32)(p->field_488.pos.f[2] / 0.100000024f);
        bondviewUpdatePlayerRoom(p);
        result = (int)stan->room;
    }
    if (mask & TP_THETA)
    {
        f32 t = a[3];

        while (t < 0.0f) t += 360.0f;
        while (t >= 360.0f) t -= 360.0f;
        p->vv_theta = t;
    }
    if (mask & (TP_POS | TP_THETA))
    {
        const f32 rad = p->vv_theta * 3.14159265f / 180.0f;

        p->field_488.theta_transform.f[0] = -sinf(rad);
        p->field_488.theta_transform.f[1] = 0.0f;
        p->field_488.theta_transform.f[2] = cosf(rad);
    }
    if (mask & TP_VERTA)
    {
        f32 v = a[4];

        if (v > 90.0f) v = 90.0f;
        if (v < -90.0f) v = -90.0f;
        p->vv_verta = v;
        p->docentreupdown = FALSE;
        p->prevupdown = TRUE;
        p->automovecentre = FALSE;
        if (!p->insightaimmode)
        {
            p->movecentrerelease = TRUE;
        }
    }
    return result;
}

static void geHostApplyTeleport(void)
{
    float a[5];
    const int mask = __atomic_exchange_n(&s_tpRequest, 0, __ATOMIC_ACQUIRE);

    if (mask == 0)
    {
        return;
    }
    memcpy(a, s_tpArgs, sizeof(a));
    AT_STORE(&s_tpResult, geHostApplyTeleportArgs(mask, a));
    __atomic_fetch_add(&s_tpServed, 1u, __ATOMIC_RELEASE);
}

void geHostTeleportRequest(int mask, float x, float y, float z, float theta, float verta)
{
    s_tpArgs[0] = x; s_tpArgs[1] = y; s_tpArgs[2] = z;
    s_tpArgs[3] = theta; s_tpArgs[4] = verta;
    __atomic_store_n(&s_tpRequest, mask, __ATOMIC_RELEASE);
}

int geHostTeleportResult(unsigned int *serial)
{
    if (serial)
    {
        *serial = __atomic_load_n(&s_tpServed, __ATOMIC_ACQUIRE);
    }
    return AT_LOAD(&s_tpResult);
}

int geHostTeleportInsideChr(void)
{
    return -1;
}
