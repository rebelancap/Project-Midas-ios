/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/hostinput.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_HOSTINPUT_H
#define PORT_HOSTINPUT_H

/*
 * hostinput — the ONE way host input reaches the simulation on iOS.
 *
 * Charter rule 3(b): "all input through the decomp's demo record/playback
 * hook (src/joy.c contsample); touch/sticks quantised into OSContPad before
 * the sim". This file is that seam, and it exists so that there is exactly
 * one, rather than one per input device.
 *
 * The shell (app/ios/GEInput.m) merges every source it has — GCController,
 * the touch overlay, the dev bridge's injection — into a single OSContPad
 * (u16 button mask + two s8 stick axes) and publishes it here with
 * geHostPadSet(). Once per SIM TICK, from inside joyConsumeSamplesWrapper()
 * at src/boss.c:546, geHostPlayback() copies that pad into the playback ring
 * and the whole game reads it through its own joyGet* accessors.
 *
 * Why the playback hook and not the SDL joystick path:
 *
 *   - it is sampled exactly once per tick, not once per host frame, so the
 *     input stream is in the sim's own time base (this is what a lockstep
 *     peer will exchange, Phase 7);
 *   - it is already an OSContPad, which is the netplay wire format;
 *   - `playbackcontcount >= 0` bypasses the g_ConnectedControllers guard in
 *     every accessor, so "is a gamepad plugged in" stops being an input to
 *     the simulation;
 *   - it is the game's own seam. No game code changes.
 *
 * Absolute look (geHostLookAdd) is deliberately NOT part of the pad. GE's
 * hipfire pitch is digital (C-up/C-down) and its yaw is a stick VELOCITY, so
 * a drag of N points cannot be expressed as a pad value without going
 * through a sensitivity curve and losing the 1:1 feel that makes touch look
 * work at all. Degrees are applied straight to the live camera on the sim
 * thread, once per tick, the same way port/src/input.c's D194 absolute-aim
 * mode does for the mouse. It is a port-layer camera write, not new game
 * logic.
 */

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- published by the shell, consumed by the sim -------------------------- */

/* The merged pad for controller 0. Called from the UI thread(s) as often as
 * it likes; the sim reads the latest value once per tick. Lock-free. */
void geHostPadSet(unsigned short button, signed char stickX, signed char stickY);

/* Round 29d (D-233): local multiplayer. Four controller ports. Port 0 is
 * player 1 and is what geHostPadSet/geHostLookAdd write (the shell merges
 * touch + controller 1 + the bridge into it); ports 1..3 are controllers 2-4,
 * one GCController each, quantised by the same shell code. The count is how
 * many ports the GAME sees (joyGetControllerCount, which sizes the front
 * end's Players row): the shell passes the highest held port + 1; clamped to
 * 1..4. Port N drives player N in a split-screen stage; the look of a port
 * with no player is drained and dropped. All lock-free, any thread. */
#define GE_HOST_MAX_PADS 4
void geHostPadSetN(int port, unsigned short button, signed char stickX, signed char stickY);
void geHostLookAddN(int port, float yawDeg, float pitchDeg);
void geHostPadCountSet(int count);
int  geHostPadCount(void);
/* Per-port sim-side counters (button edges the game read, last word). */
void geHostPadStatsN(int port, unsigned int *edges, unsigned int *lastButton);
/* Players in the running stage when it is a split-screen match (>= 2),
 * else 0 (solo, front end, title). One tick stale; any thread. */
int  geHostSplitScreenPlayers(void);
/* Player `i` (0-based) of a split-screen match: position, heading, health,
 * the game's own death counter, kills by that player, the right-hand gun and
 * aim mode (insightaimmode).
 * Returns 0 when there is no such player. Racy word reads, bridge only. */
/* D-235, dev bridge `mpdev gun <item>` / `mpdev near <n> <m>`. */
void geHostMpDevGun(int item);
void geHostMpDevNear(int n, int m);
int  geHostMpPlayer(int i, float *x, float *y, float *z, float *theta,
                    float *health, int *dead, int *deaths, int *kills, int *weapon,
                    int *aiming);

/* D-062 (round 15a): the touch overlay's RELOAD control. GoldenEye has no
 * reload button -- B is bond_pressed_reload_activate() and src/game/lv.c
 * spends it on "activate the thing in front of you, ELSE reload" -- so the
 * RELOAD circle publishes B like USE does AND raises this flag while it is
 * held. src/game/lv.c reads it (iOS only) and skips the activate test for
 * that press, so RELOAD always reloads even when Bond is standing in front of
 * a door. Set from the UI thread, read on the sim thread; a LEVEL, not an
 * edge, because the shell republishes the whole touch contribution on every
 * touch event and a level cannot be lost the way a consumed edge can. */
void geHostReloadOnlySet(int on);
int  geHostReloadOnly(void);

/* Round 59 (docs/port-ios/79): Midas Port actions (port/src/joy_port.c) from
 * the shell, per pad. geHostPortActionFind maps a midas.ini action name
 * ("crouch_toggle", "next_inventory", "zoom_in", ...) to its index, -1 if
 * unknown. geHostPortActionSet holds action `act` on pad `pad` at `level`
 * (0 = up, 0..1 = a magnitude for zoom_in / zoom_out, 1 = a button) for
 * source `src` (0 = touch overlay, 1 = the shell's controller mapping);
 * sources compose by max, and every held action is pressed into the Port
 * suite once per pad poll. geHostPortActionClear(src, pad) drops one
 * source's actions on one pad (-1 = every pad). Safe from any thread. */
int  geHostPortActionFind(const char *name);
void geHostPortActionSet(int src, int pad, int act, float level);
void geHostPortActionClear(int src, int pad);
/* Player 1 is in aim mode (racy word read; 0 without a player). */
int  geHostPlayerAiming(void);

/* Accumulate an absolute view-angle delta in DEGREES (yaw +right, pitch
 * +down). Drained and applied once per tick. Lock-free. */
void geHostLookAdd(float yawDeg, float pitchDeg);

/* Drain and apply the accumulated look deltas to the live camera RIGHT NOW,
 * on the calling (game) thread. On a phone this happens inside the contsample
 * hook once per tick and nothing else calls it; it is public so the macOS
 * oracle's GE_LOOKSCRIPT harness can exercise the identical path (D-056). */
void geHostLookApplyNow(void);

/* Point Bond at the nearest character and alert it, on the calling (game)
 * thread. The oracle's GE_AIMCHR harness; on iOS the bridge's `inject
 * aim-chr` goes through geHostAimAtNearestChr() instead (D-050). */
void geHostAimApplyNow(void);
/* Round 20a: GE_AIMOBJ (gesched.c) -- aim at the nearest object prop. */
void geHostAimAtObject(int propnum, float yoff, float dtheta, int list);
void geHostAimAbsolute(float theta, float verta);

/* Round 20b: put Bond at (x, y, z) and/or set his facing (theta, degrees,
 * GE's vv_theta) and pitch (verta, degrees, +up) the way the game's own
 * respawn does (hostinput.c). y is the search height: the highest stand tile
 * at or below it wins and Bond stands on it at eye height, so the player_pos
 * that `state` prints round-trips. mask: 1 position, 2 theta, 4 verta.
 * Request = bridge thread, served on the sim thread at the next tick; the
 * result is the new room (>= 0), -1 no floor under the point, -2 no player,
 * and the serial rises once per served request. ApplyNow = the oracle's
 * GE_TELEPORT, on the game thread. */
void geHostTeleportRequest(int mask, float x, float y, float z, float theta, float verta);
int  geHostTeleportResult(unsigned int *serial);
int  geHostTeleportApplyNow(int mask, float x, float y, float z, float theta, float verta);
/* Oracle GE_PROPLIST=<read>: log every live object/door prop (model, pos, room). */
void geHostPropListLog(void);
/* Round 20b: where the character the last `inject aim-chr` chose stands. */
void geHostAimTarget(float *x, float *y, float *z);

/* Ask the game to load a solo stage (LEVELID). Consumed on the game thread
 * at the next tick; 0 or negative cancels a pending request. A number that is
 * not one of the 21 solo stages is refused and logged (D-260). */
void geHostBootStage(int stage);

/* D-260 (round 30b): 1 if `stage` is a LEVELID a boot-stage may load -- the
 * 20 missions + Cuba, the same table the dev bridge and the oracle's
 * --boot-stage names resolve through. Anything else (e.g. 3) has no text bank:
 * langGetLangBankIndexFromStagenum spins in its default `while(1)` when the
 * stage is left, hanging the game thread. */
int geHostStageIsBootable(int stage);
/* The same table as "Name(NN) ..." for an error reply. */
const char *geHostBootableStageList(void);

/* configSave() from whatever thread the shell is on (sceneWillResignActive).
 * The EEPROM needs no equivalent: port/src/libultra.c rewrites $S/ge007.eep
 * in full on every osEepromLongWrite, so the save is already on disk. */
void geHostFlushSaves(void);

/* ---- read back by the shell (the :8777 bridge `state` command) ------------ */

/* Current stage id (g_StageNum), the port's own fps average, the number of
 * sim ticks this process has run, and the number of presented frames. Any
 * pointer may be NULL. Safe from any thread (all four are plain word reads). */
void geHostState(int *stage, float *fps, unsigned int *ticks, unsigned int *frames);

/* Round 12a / D-046 — take contdata slot PLAYBACK back if something else has
 * taken it. Asked once per rendered frame from geTickNextDelta(); a no-op
 * while a ramrom demo legitimately owns it. The host seam cannot police this
 * from inside its own playback func, because the failure is that the func
 * stops being called. See the comment on the implementation. */
void geHostInputReassert(void);
void geHostOracleProbes(void);  /* GE_BOOTSTAGE / GE_STAGEWATCH, round 28d */
unsigned int geHostInputReassertCount(void);
int geHostInputOwnsHook(void);
/* Dev-bridge only: steal the hook exactly as stop_demo_playback() does, so the
 * gate can assert the watchdog takes it back. */
void geHostInputTestStealHook(void);

/* Rising edge on any button of the pad the SHELL is publishing, read straight
 * from the atomic rather than through the sim's ring. The attract demo's
 * "press anything to skip" test goes through CONTDATA_REGULAR, which on iOS is
 * always empty — this is how a finger or a pad skips the demo. One call per
 * demo frame; it advances its own edge state. */
int geHostAnyButtonEdge(void);

/* Round 12a / D-046 — the seam's own counters, so a DEVICE log can say which
 * side of geHostPadSet() an input died on. `sets` counts geHostPadSet() calls
 * whose packed word DIFFERED from the one before (i.e. real input events, not
 * the pacer's 120 Hz re-publish of a resting pad); `edges` counts sim ticks on
 * which the pad the game read had a button bit RISE. A shell that logs both
 * turns "none of the buttons work" into one of two questions instead of six.
 * Any pointer may be NULL; safe from any thread. */
void geHostPadStats(unsigned int *sets, unsigned int *edges, unsigned int *lastButton);

/* Where Bond is and where he is looking, plus the live control style (D-030).
 * Read from the sim's own state on whatever thread asks; the values are one
 * tick stale at worst, which is what an instrument wants. */
void geHostPlayerState(float *x, float *y, float *z,
                       float *theta, float *verta, int *style);

/* D-051: the FRONT END's red crosshair, in GoldenEye's own 2D menu space
 * (src/game/front.c cursor_h_pos / cursor_v_pos). It is driven ONLY by
 * joyGetStickX/Y, so it is the one readout that says whether the host pad's
 * stick reaches the menus on BOTH axes — a screenshot cannot, and the pad
 * counters cannot either (the cursor consumes no button edge). Any pointer
 * may be NULL; safe from any thread, one tick stale at worst. */
void geHostFrontCursor(float *h, float *v);
/* Round 18b: the live front-end MENU (GE's enum), -1 when none. */
int geHostFrontMenu(void);
/* Round 18b: jump the front end to GE MENU <menu> at the next tick (Dam, Agent). */
void geHostFrontMenuRequest(int menu);
/* Round 18b: mark Dam completed on Agent in <folder> (0-3), front end only. */
void geHostSaveCompleteDam(int folder);

/* Round 10a: the audio pipeline's three observable numbers — SFX starts that
 * survived the D127 guard, the peak sample of the last mixed block, and the
 * number of mixed blocks queued. See port/include/audio.h. Any pointer may be
 * NULL; safe from any thread. */
void geHostAudioStats(unsigned int *sfxStarts, int *peak, unsigned int *blocks);

/* D-050 (round 13): the combat counters. bulletHits rises once per
 * chrHandleBulletHit(), bloodStains once per blood stain actually painted on a
 * character (src/game/chr.c). They are what lets the gate say "the shot hit a
 * guard and the decal code ran", instead of "a gun was fired at the scenery".
 * Any pointer may be NULL; safe from any thread. */
void geHostCombatStats(unsigned int *bulletHits, unsigned int *bloodStains,
                       unsigned int *bloodRelated);

/* D-062 (round 15a): rounds in each hand's magazine, or -1 when there is no
 * player. The RELOAD control's only observable effect, and the only thing a
 * gate can assert about it -- a screenshot of two small HUD digits cannot tell
 * a reload that worked from one that did not. Any pointer may be NULL. */
void geHostAmmo(int *magRight, int *magLeft);

/* D-138 (round 24a): the aim-mode readout. aiming = insightaimmode (-1 no
 * player); weapon = the right hand's ITEM_IDS; sniperZoom = the sniper's FOV
 * in degrees (60 wide .. 7 tight); fovy = the camera's zoom FOV this frame;
 * sway = the lean target (-75 left, 0, 75 right); crouch = crouchpos;
 * zoomPolls = how many polls the left stick published a zoom bit on. Any
 * pointer may be NULL; one tick stale at worst. */
void geHostAimState(int *aiming, int *weapon, float *sniperZoom, float *fovy,
                    float *sway, int *crouch, unsigned int *zoomPolls);

/* D-050 (round 13): the gate's aim lever. geHostAimAtNearestChr() asks the SIM
 * thread to point Bond at the nearest character and wake it; geHostAimResult()
 * returns the distance to whatever it found (-1 = nothing) and the serial that
 * rises once per served request. Everything after the turn is the game's own
 * code — the gate still fires through the contsample hook. Safe from any
 * thread. */
void  geHostAimAtNearestChr(void);
float geHostAimResult(unsigned int *serial);

/* ---- ge007.ini, by key, for the dev bridge's `settings get/set` -----------
 *
 * config.c has no set-by-key of its own (the ini and the F10 overlay both
 * write through the registered pointer), so these walk configForEachOption()
 * and do the same. Returns 0 on success, -1 when the key is not registered.
 * Both are safe to call from the bridge thread: every registered option is a
 * plain int/uint/float/char[] and a torn read of one is a wrong number on a
 * settings page, not a crash. */
int geHostConfigGet(const char *key, char *out, int outLen);
int geHostConfigSet(const char *key, const char *value);

/* ---- GoldenEye's OWN player options, for the settings page ----------------
 *
 * A third store, and it is a third store because these are neither ours nor
 * the ini's: `game_options_entries[]` (src/game/options.c) is what the Q
 * Watch options screen edits and what file2.c packs into the EEPROM save, so
 * a value written here survives in ge007.eep alongside the player's times and
 * is the same value the in-game menu shows. Writing it into ge007.ini instead
 * would give the option two homes that silently disagree.
 *
 * Names (case-insensitive): "LookAhead", "Crosshair", "AmmoDisplay",
 * "AutoAim". Each is 0 or 1.
 *
 * Threading: the GET is a plain word read of a global array and is safe from
 * anywhere. The SET is QUEUED and applied on the game thread at the next sim
 * tick, because these words are read by the sim — charter rule 3(c) — and a
 * UIKit thread storing into them mid-tick is the shape of bug this port has
 * spent two rounds not having. Returns -1 for an unknown name, 0 otherwise. */
int geHostGameOptionGet(const char *name);
int geHostGameOptionSet(const char *name, int value);

/* 1 while the player is actually playing a mission: a real solo stage is
 * loaded, a player prop exists, and the Q Watch is neither open nor
 * transitioning (`pause_state == 0`, src/game/bondview.h:705). The settings
 * gear hides on this, so PAUSED deliberately reads as "not gameplay" — the
 * watch is how a player reaches a menu mid-mission and the gear has to be
 * there when they do. Every uncertain case (no player yet, front end, title)
 * answers 0, because the failure that matters is a gear nobody can reach.
 * Safe from any thread: three plain word reads. */
int geHostInGameplay(void);
/* round 9b: Bond position / heading for the shell dev bridge (donor name) */
void geHostPlayerState(float *x, float *y, float *z, float *theta, float *verta, int *style);

/* Round 17a, dev capture lever: GE's own INVINCIBLE cheat, so a driven
 * capture session can stand still in front of a prop for the minutes a
 * screenshot sweep takes. Set from the bridge (`cheat invincible 0|1`);
 * nothing in the game path calls it. */
void geHostSetInvincible(int on);
int  geHostGetInvincible(void);
void geHostInvincibleApplyNow(void);
/* Round 26a, oracle GE_GUNSCRIPT (dev lever): all guns once, then draw `item`
 * in the right hand / reload it. Game thread. */
void geHostGunNow(int item);
void geHostReloadNow(void);
/* Round 27b (D-164): chrnum of the character the last teleport put Bond
 * inside (within 60 units), or -1. The bridge appends it as `inside_chr=`. */
int  geHostTeleportInsideChr(void);

/* ---- wiring -------------------------------------------------------------- */

/* Install the playback hook. Called from gesched.c's lazy hook install, on
 * the game thread, after joyInit() and before the first consume. No-op when
 * a --play replay owns the ring (a replay is the sole input source by
 * definition) or off iOS. Returns 1 if it installed. */
int geHostInputInstall(void);

/* 1 when this build/run routes host input through the hook above. */
int geHostInputActive(void);

/* ---- Midas (round 9a, docs/port-ios/16): the engine-side consumers -------- *
 * Implemented in port/apple/hostinput.c; called only from GE_IOS arms. */
/* libultra_shim.c osContGetReadData: merge the host pads into this read
 * (`pads` is the OSContPad[MAXCONTROLLERS] being filled; OSContPad is an
 * anonymous struct typedef, hence void *). */
void geHostInputPoll(void *pads);
/* input_pc.c portInputMouseTickConsume: drain this tick's player-1 look. */
void geHostLookTake(float *yawDeg, float *pitchDeg);
/* libultra_shim.c contConnectedMask: the ports the shell publishes. */
unsigned int geHostPadMask(void);
/* Round 57: the Look-stick scale for SDL-owned pads (Settings -> Controller).
 * 1.0 = Midas's own rate; invert flips the vertical. bondview2.c reads it. */
void geHostSetPadLook(float scaleX, float scaleY, int invert);
void geHostGetPadLook(float *scaleX, float *scaleY);

#ifdef __cplusplus
}
#endif

#endif /* PORT_HOSTINPUT_H */
