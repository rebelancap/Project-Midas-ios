/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/audio.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

/*
 * Audio: SDL audio device + buffer queueing.
 * Modelled on the PD port's port/include/audio.h.
 *
 * libaudio (AL) runs on the CPU and produces mixed s16 stereo output. The AI
 * (osAiSetNextBuffer / osAiGetLength / osAiSetFrequency) is mapped onto this
 * layer, which queues the mixed samples to the SDL audio device.
 */

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the SDL audio device. Returns 0 on success. */
int  audioInit(void);
void audioDestroy(void);

/* Number of samples (stereo s16 frames) currently queued. */
s32  audioGetSamplesBuffered(void);

/* D204/F1: emulate the N64 AI_LEN_REG -- bytes remaining in the buffer the
 * DAC is *currently* playing, NOT the whole queue depth. src/audi.c does
 * arithmetic on this value that only holds inside the N64's range. */
u32  audioGetAiLengthBytes(void);

/* Queue the next block of mixed samples (len is in bytes). */
void audioSetNextBuffer(const s16 *buf, u32 len);

/*
 * Round 10a telemetry — permanent, not a diag probe, because "does a sound
 * effect actually play" had no answer on a device that a log could give.
 *
 * `sfxStarts` counts sndPlaySfx() calls that got past the D127 sanity guard
 * and reached sndSetupSound(); it is the number that was pinned at 0 on
 * Apple arm64 for every build up to and including OTA 0.0.0.1. `peak` is the
 * largest |sample| in the most recent mixed block handed to the DAC (0..32767)
 * and `blocks` the number of blocks queued. All three are plain word reads and
 * are safe from any thread; any pointer may be NULL.
 */
void audioNoteSfxStart(void);
void audioGetStats(unsigned int *sfxStarts, int *peak, unsigned int *blocks);

/*
 * D-089: host-side output gain, 0..1, multiplied into each mixed block just
 * before it is queued to SDL (ramped over ~0.2 s). Not a config key and never
 * read by the sim; the iOS shell's Other App Audio modes drive it. Any thread.
 * audioGetHostGain() returns the gain the ramp has actually reached.
 */
void  audioSetHostGain(float gain);
float audioGetHostGain(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_AUDIO_H */
