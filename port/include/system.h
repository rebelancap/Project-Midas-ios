/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/system.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_SYSTEM_H
#define PORT_SYSTEM_H

/*
 * Platform primitives: time, logging, paths, exit.
 * Modelled on the PD port's port/include/system.h.
 */

#include <stdint.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Time --------------------------------------------------------------- */

/* Monotonic microseconds since an arbitrary origin. */
uint64_t sysGetMicroseconds(void);

/*
 * Base address the executable was loaded at (for turning runtime addresses
 * into file offsets for addr2line; ASLR moves the image on Windows).
 */
uintptr_t sysImageBase(void);
/* Wall-clock seconds since the epoch (for timestamps). */
int64_t  sysGetTime(void);
/* Sleep for the given number of microseconds. */
void     sysSleep(uint32_t micros);

/* --- Logging ------------------------------------------------------------ */

void sysLogPrintf(enum LogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Print a fatal error message and terminate the process. Used by fast3d for
 * unrecoverable conditions (bad Gfx command, OOM in the texture cache, ...).
 */
void sysFatalError(const char *fmt, ...)
    __attribute__((format(printf, 1, 2))) __attribute__((noreturn));

/* --- Paths -------------------------------------------------------------- */

/*
 * Resolve a path relative to the data dir. "$S/" prefixes the data dir,
 * "$E/" prefixes the executable dir. Returns a pointer to a static buffer.
 */
const char *sysResolvePath(const char *path);

/* Return the directory containing the executable. */
const char *sysGetExeDir(void);

/* --- Command line ------------------------------------------------------- */

/* Store the host argv (call once from main). */
void sysSetArgs(int argc, char **argv);
/* Nonzero if the given argument (e.g. "--debug-gl") was passed. */
int  sysArgCheck(const char *arg);
/* Override the vector sysGetTokenString() builds the game's token string from
 * (round 7a: keeps our long flags out of tokenFind's substring matching, and
 * is how --boot-stage becomes -level_XX). NULL restores the real argv. */
void sysSetTokenArgs(int argc, char **argv);
/* Value following the given argument on the command line, or NULL. */
const char *sysArgGetString(const char *arg);
/* The host argv[1..] joined with spaces, as the game's "cartridge token
 * string" (see token.c / osPiReadIo). Lets N64 debug args like "-level_09"
 * work on PC. Never NULL; empty string if no args. */
const char *sysGetTokenString(void);

/* --- CPU ---------------------------------------------------------------- */

/* Briefly yield/relax the CPU (busy-wait hint). Used by fast3d's frame sync. */
void sysCpuRelax(void);

/* --- Lifecycle ---------------------------------------------------------- */

void sysExit(int code);

/*
 * Start the cooperative thread kernel (green threads + vsync tick). Call
 * once from main() before running any game code; after this, blocking
 * osRecvMesg() calls in the game suspend/resume threads.
 */
void portKernelInit(void);

/*
 * D-065 (round 16b). The VI retrace is the port's frame clock: the scheduler
 * thread blocks on it, so whatever posts it decides the frame rate. The
 * default poster is portTickThread(), a chunked relative nanosleep() on a
 * default-QoS pthread — accurate enough on a desktop, 45-54 Hz of jitter on a
 * phone (D-063, docs/pacing.md §Round 16a).
 *
 * portSetExternalVIPacer(1) hands that job to the host instead: the tick
 * thread stops posting retraces (it keeps servicing timers and the hang
 * heartbeat) and the host calls portExternalVIPulse() once per intended
 * frame, off a clock that cannot jitter. On iOS that is the shell's
 * CADisplayLink, which is the panel's own clock (app/ios/GEPacing.m).
 *
 * portSetExternalVIPacer() may be called at any time, before or after
 * portKernelInit(): the tick thread reads the flag once per iteration, and if
 * the host's clock stops (iOS pauses the display link on background) the tick
 * thread resumes posting at 10 Hz so nothing can deadlock. GE_DETERM always
 * wins (the M-001 replay path generates its retraces inside osRecvMesg and
 * must not be touched), and portExternalVIPulse() is a no-op unless the
 * external pacer is armed. The sim rate is unaffected either way: gesched.c
 * counts whole 1/60 s ticks regardless of how often a frame is presented
 * (D-019).
 */
void portSetExternalVIPacer(int on);
void portExternalVIPulse(void);

/* 1 = a VI retrace is already queued, i.e. the scheduler's next BUILD is due
 * the moment it gets the thread back. The repeat-present pump uses it to know
 * when the idle window is over (D-065b). */
int  portVIRetracePending(void);
/* D-072: when the last retrace was posted (the repeat-present deadline). */
uint64_t portVILastRetraceUs(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_SYSTEM_H */
