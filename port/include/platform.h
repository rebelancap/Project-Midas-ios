/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/platform.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_PLATFORM_H
#define PORT_PLATFORM_H

/*
 * Platform abstraction for the GE PC port.
 *
 * Provides the small set of host primitives the port layer needs, with
 * per-OS implementations selected at compile time. Modelled on the PD port's
 * port/include/platform.h.
 */

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32) || defined(_WIN64)
  #define PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IPHONE
    /* TARGET_OS_IPHONE is 1 on xrOS as well as iOS/iPadOS, so PLATFORM_IOS is
     * the Apple-mobile gate and PLATFORM_VISIONOS is the *additional* one, the
     * way PD's patch 0008 wrote it — which is why visionOS needed no further
     * engine change there. Phase 5 is where that gets exercised; today it only
     * has to exist so a visionOS build is a build setting, not a port. */
    #define PLATFORM_IOS 1
    #if defined(TARGET_OS_VISION) && TARGET_OS_VISION
      #define PLATFORM_VISIONOS 1
    #endif
  #else
    #define PLATFORM_MACOS 1
  #endif
  /* Everything Darwin, macOS and iOS alike. The POSIX crash handler
   * (port/src/crash.c) is written against Darwin, not against the desktop:
   * sigaction, ucontext, the arm64 __darwin_mcontext register names and
   * backtrace_symbols are identical on both, and gating it on PLATFORM_MACOS
   * alone silently compiled the WHOLE file away on iOS — the first iOS link
   * failed on crashDumpThreads, the heartbeat's hang dumper, which would
   * otherwise have become a no-op nobody noticed. Same bug class as D-012's
   * corollary: an `#if defined(PLATFORM_X86_64)` chain that quietly loses a
   * platform. */
  #define PLATFORM_APPLE 1
#elif defined(__linux__)
  #define PLATFORM_LINUX 1
#else
  #error "Unsupported platform"
#endif

/*
 * Round 8b: the GROUPINGS, defined once.
 *
 * Round 7b's crash.c bug was a disjunction written out by hand -- the POSIX
 * crash handler was gated `PLATFORM_LINUX || PLATFORM_MACOS` while this header
 * defines PLATFORM_IOS instead of PLATFORM_MACOS on an Apple-mobile target, so
 * the whole file compiled to nothing on iOS and only a link error against
 * crashDumpThreads gave it away. A hand-written disjunction that must be kept
 * in sync in four places WILL fall out of sync; a name defined once cannot.
 *
 * PLATFORM_APPLE  — macOS, iOS, iPadOS, visionOS. Darwin: sigaction, ucontext,
 *                   the __darwin_ register names, mach, backtrace_symbols.
 * PLATFORM_POSIX  — everything that is not Windows: Apple and Linux alike.
 *                   Use this, not `PLATFORM_LINUX || PLATFORM_APPLE`.
 *
 * The canonical set is exactly: WINDOWS, APPLE, MACOS, IOS, VISIONOS, LINUX,
 * POSIX, 64BIT, X86, X86_64, ARM. Anything else spelled PLATFORM_* in this
 * tree is a phantom that silently evaluates false -- scripts/check-platform-
 * macros.sh fails the build on one.
 */
#if !defined(PLATFORM_WINDOWS)
  #define PLATFORM_POSIX 1
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__)
  #define PLATFORM_64BIT 1
#endif

#if defined(_M_IX86) || defined(__i386__)
  #define PLATFORM_X86 1
#elif defined(_M_X64) || defined(__x86_64__)
  #define PLATFORM_X86_64 1
#elif defined(__aarch64__)
  #define PLATFORM_ARM 1
#endif

/* Constructor attribute: run before main(). */
#if defined(_MSC_VER)
  #define PD_CONSTRUCTOR
  #define PD_EXPORT
#else
  #define PD_CONSTRUCTOR __attribute__((constructor))
  #define PD_EXPORT __attribute__((visibility("default")))
#endif

/* Log levels (see system.h). */
enum LogLevel {
    LOG_ERROR = 0,
    LOG_WARNING,
    LOG_NOTE,
    LOG_INFO,
    LOG_DEBUG,
};

#endif /* PORT_PLATFORM_H */
