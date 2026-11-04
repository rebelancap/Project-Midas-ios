#ifndef PORT_GUESTMEM_H
#define PORT_GUESTMEM_H

/*
 * guestmem — the relocatable N64 address window.
 *
 * Imported from GoldenEye-Decomp-ios engine/port/include/guestmem.h (its
 * engine/docs/pointer-contract.md is normative here); Project Midas round 1,
 * docs/port-ios/04-round1-prereqs.md.
 *
 * Midas's 32-bit build classifies "a u32 >= 0x10000000" as a host pointer
 * and pins its arenas there (memp 0x20000000, high arena 0x30000000,
 * dedicated 0x50000000+, port/src/pcsched.c). On Apple arm64 nothing below
 * 4 GiB can ever be mapped: __PAGEZERO covers the low 4 GiB and cannot be
 * shrunk. So on 64-bit hosts the same layout is committed inside ONE
 * 4 GiB-aligned, 4 GiB-sized PROT_NONE reservation at a probed base
 * `g_guestBase` (port/apple/guestmem.c):
 *
 *   guest 0x10000000  ROM image (+ headroom to 0x20000000 for the cart
 *                     extension / sidecars)
 *   guest 0x20000000  memp arena
 *   guest 0x30000000  high bump arena
 *   guest 0x50000000+ dedicated high mappings
 *
 * Because g_guestBase has ZERO low 32 bits:
 *
 *   (u32)host_ptr  == the guest address, exactly (lossless truncation)
 *   guest addresses stay positive as s32 (everything is < 0x80000000)
 *
 * so every (u32) pointer cast into those regions keeps the value Midas uses
 * today. Only pointer RECONSTRUCTION from a 32-bit value has to change, via
 * GUEST_PTR()/CART_PTR() below.
 *
 * On Midas's 32-bit x86 build (no PLATFORM_64BIT) there is no window and no
 * guestmem.c: g_guestBase is the constant 0, every macro here collapses to
 * the identity, and nothing in that build changes.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(PLATFORM_64BIT)
extern uintptr_t g_guestBase;
#else
/* 32-bit x86: guest == host, no TU defines a variable (D-M03). */
#define g_guestBase ((uintptr_t)0)
#endif

#define GUEST_WINDOW_SIZE  ((uintptr_t)0x100000000ULL)

/* Apple arm64 pages are 16 KB; round every commit to it. */
#define GUEST_PAGE_SIZE      0x4000u
#define GUEST_PAGE_ROUND(n)  (((n) + (GUEST_PAGE_SIZE - 1)) & ~(size_t)(GUEST_PAGE_SIZE - 1))

/* Reconstruct a live host pointer from a 32-bit guest address. */
#define GUEST_PTR(x)   ((void *)(g_guestBase + (uintptr_t)(uint32_t)(x)))
/* Same, spelled for cart addresses (0x10xxxxxx + the sidecar extension). */
#define CART_PTR(x)    GUEST_PTR(x)
/* Live host pointer for an absolute cart/DRAM symbol (romassets_*.s). */
#define CART_SYM(sym)  ((void *)(g_guestBase + (uintptr_t)(uint32_t)(uintptr_t)&(sym)))
/* Truncate a host pointer back to its guest address (lossless by design). */
#define GUEST_ADDR(p)  ((uint32_t)(uintptr_t)(p))
/*
 * Repair a pointer that may have been round-tripped through an s32/u32 (the
 * decomp is full of `s32` parameters and fields that in fact carry pointers).
 * A value below 4 GiB cannot be a live host pointer on any 64-bit target, so
 * it must be a guest address; anything else passes through untouched. The
 * identity when g_guestBase == 0.
 */
#define GUEST_FIX(p) \
    ((void *)(((uintptr_t)(p) && (uintptr_t)(p) < GUEST_WINDOW_SIZE) \
              ? (uintptr_t)GUEST_PTR(p) : (uintptr_t)(p)))

/* Is this host pointer inside the guest window? */
#define GUEST_IN_WINDOW(p) \
    (((uintptr_t)(p) & ~(uintptr_t)0xFFFFFFFFULL) == g_guestBase)

/*
 * Reserve the guest window. Idempotent; the pcsched.c allocators call it
 * before their first commit. Returns 0 on success. 64-bit hosts only.
 */
int guestMemInit(void);

/*
 * Commit `len` bytes at guest address `guestAddr` inside the window.
 * fd < 0 => private anonymous (zeroed); fd >= 0 => MAP_SHARED view of that
 * object. `len` must be a multiple of the host page (16 KB on Apple arm64;
 * GUEST_PAGE_ROUND). Returns NULL on failure.
 */
void *guestMemCommit(uint32_t guestAddr, size_t len, int fd);

/* Release a committed range (back to PROT_NONE reservation / unmapped). */
void guestMemRelease(uint32_t guestAddr, size_t len);

/*
 * Round 7b: how many bytes from `p` on are readable without a fault (the
 * contiguous readable mappings starting at p), capped at `cap`. Lets a
 * streaming reader with no length (rzdecomp.c's inflate) stop at the end of
 * an arena instead of reading ahead into the PROT_NONE reservation. Returns
 * `cap` where the host cannot tell.
 */
size_t guestMemReadableFrom(const void *p, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PORT_GUESTMEM_H */
