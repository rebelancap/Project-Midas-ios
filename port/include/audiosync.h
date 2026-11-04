/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/audiosync.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_AUDIOSYNC_H
#define PORT_AUDIOSYNC_H

/*
 * audiosync — Audio.SimLock: the sim tick is the audio manager's clock, and
 * the two never run concurrently. Design, rationale and the measurement that
 * produced it: docs/tick.md §9. Q-010 / round 10e.
 *
 * The leak this closes: sndSetupSound() pops an ALSoundState off a free list
 * that the audio thread (amMain -> alAudioFrame -> sndUnlinkClearSound)
 * refills, and game code branches on the pointer it gets back. The pop is
 * mutually excluded (osSetIntMask) but not ORDERED, so which state comes back
 * at tick k is a function of how far the audio thread has run — i.e. of wall
 * time. That breaks charter rule 3(c) and Phase 7 lockstep.
 *
 * Three touch points, all one-liners:
 *   src/sched.c  __scHandleRetrace   — stop forwarding retraces to the audio
 *                                      client while SimLock is on
 *   src/audi.c   amStartAudioThread  — audioSyncArm()
 *   src/audi.c   amMain              — the gated loop body
 * plus the pump in port/src/gesched.c geTickNextDelta().
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Config only: Audio.SimLock=1, or --fixed-step (which forces it). Safe to
 * call from any thread at any time; does NOT depend on the audio thread
 * having started. sched.c reads this. */
int audioSyncEnabled(void);

/* Called once from geTickInit() after configLoad(). */
void audioSyncInit(int fixedStep);

/* Game thread, from amStartAudioThread(): from here on the sim pumps the
 * audio manager. Before this the pump is a no-op (nothing to pump), which is
 * deterministic because amStartAudioThread() is itself called from game code
 * at a fixed point in the boot sequence. */
void audioSyncArm(void);

/* Sim thread, once per tick, at the very top of the tick and before any game
 * code of that tick runs. Grants the audio thread one frame on the right
 * parity and blocks (bounded) until its free-list writes have landed. */
void audioSyncPumpTick(void);

/* Audio thread: block until the sim grants a frame. Returns 0 if the manager
 * is shutting down and the loop should exit. */
int audioSyncWaitForTick(void);

/* Audio thread: this frame's amHandleFrameMessage() has returned, so every
 * free-list write it was going to make has landed. Releases the sim. */
void audioSyncFrameDone(void);

/* D-255 (round 30b): sim thread, from INSIDE a game-code busy-wait on the
 * audio manager (music.c's `while (alCSPGetState(seqp))` in
 * musicTrack{1,2,3}Play). Under SimLock the audio thread only runs on a
 * grant, and the grant is issued at the top of the NEXT tick, which the spin
 * never reaches: the sim and the audio manager block each other for ever
 * (a multiplayer match start hung at its first frame; the front end crawled
 * until --no-simlock, D-049). Each call grants ONE extra audio frame and
 * waits (bounded) for it, so the spin ends after exactly as many audio frames
 * as the sequence player needs to stop -- a function of the audio state,
 * which under SimLock is a function of the tick count. No-op (the N64 spin,
 * unchanged) when SimLock is off. */
void audioSyncSpinAssist(void);

/* Release anyone waiting (thread exit / MAIN_QUIT). */
void audioSyncStop(void);

/* Number of pump waits that hit Audio.SimLockTimeoutMs. MUST be 0 for a run
 * to be deterministic; a non-zero value means the sim ran a tick on a free
 * list the audio thread had not finished updating. */
unsigned audioSyncTimeouts(void);

/* How long the SIM has spent blocked on the audio thread, in microseconds,
 * plus the worst single wait and the number of audio frames granted. The
 * back-pressure number oracle.md round 10a §7b asked to size first. */
void audioSyncWaitStats(unsigned long long *totalUs, unsigned long long *maxUs,
                        unsigned long long *frames);

#ifdef __cplusplus
}
#endif

#endif /* PORT_AUDIOSYNC_H */
