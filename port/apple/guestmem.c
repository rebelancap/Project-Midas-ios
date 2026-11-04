/*
 * guestmem.c — the relocatable N64 address window. See port/include/guestmem.h
 * for the rationale; the short version is that Apple arm64 reserves the low
 * 4 GiB of every process for __PAGEZERO and will not let it be shrunk, so the
 * fixed high addresses Midas's port pins its arenas at (0x10000000..
 * 0x5xxxxxxx) are unmappable there. We relocate them into one 4 GiB-aligned,
 * 4 GiB PROT_NONE reservation and keep the addresses as offsets inside it.
 *
 * Imported from GoldenEye-Decomp-ios engine/port/src/guestmem.c (Apple arm
 * verbatim; its round-2 fix and on-device probe results apply). Midas
 * changes: logging through portConLogf, no platform.h/system.h, and the file
 * is only compiled for GE_HOST32=OFF (port/apple/, CMakeLists.txt) - the
 * 32-bit x86 build has no window at all (g_guestBase is a constant 0 there).
 *
 *   __APPLE__          -> probed, vacancy-checked 4 GiB window (below)
 *   _WIN32             -> identity; the window is unused
 *   other POSIX        -> identity; MAP_FIXED_NOREPLACE commits at the literals
 */

#include "guestmem.h"

extern void portConLogf(const char *fmt, ...); /* port_console.c */

uintptr_t g_guestBase = 0;

#if defined(__APPLE__)

#include <errno.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <TargetConditionals.h>

/*
 * The vacancy check, and why it is spelled twice.
 *
 * macOS has <mach/mach_vm.h> and mach_vm_region(). The iOS SDK ships that
 * header as a single line — `#error mach_vm.h unsupported.` — so the whole
 * mach_vm_* family is off the table on iOS and visionOS, and the symbol itself
 * is the kind of thing app review notices.
 *
 * vm_region_64() is the public, SDK-declared equivalent and IS present on
 * every Apple platform. On a 64-bit process vm_address_t and vm_size_t are
 * 64-bit, so it answers the same question with the same precision; the only
 * difference is the spelling of two typedefs. There is no 32-bit Apple target
 * here (arm64 and x86_64 only), so nothing is lost.
 *
 * GE_VM_REGION() therefore hides the one call, and the logic below — probe a
 * 4 GiB-aligned base, CHECK IT IS FREE, only then MAP_FIXED — is a single code
 * path exercised on macOS, the simulator and the device alike (D-008).
 */
#if TARGET_OS_IPHONE
typedef vm_address_t ge_vm_addr_t;
typedef vm_size_t    ge_vm_size_t;
#define GE_VM_REGION vm_region_64
#else
#include <mach/mach_vm.h>
typedef mach_vm_address_t ge_vm_addr_t;
typedef mach_vm_size_t    ge_vm_size_t;
#define GE_VM_REGION mach_vm_region
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/*
 * Is [base, base+len) entirely unmapped? MAP_FIXED on Darwin CLOBBERS an
 * existing mapping instead of failing, so every candidate base is checked
 * first. mach_vm_region() returns the first mapping at or above `addr`.
 */
static int guestRangeIsFree(uintptr_t base, size_t len)
{
    ge_vm_addr_t addr = (ge_vm_addr_t)base;
    ge_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;

    kern_return_t kr = GE_VM_REGION(mach_task_self(), &addr, &size,
                                    VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&info, &count,
                                    &object);
    if (kr == KERN_INVALID_ADDRESS)
        return 1;                       /* nothing mapped at or above base */
    if (kr != KERN_SUCCESS)
        return 0;
    return addr >= (ge_vm_addr_t)(base + len);
}

/* Try to take exactly `len` bytes of PROT_NONE reservation at `base`. */
static int guestTryReserve(uintptr_t base, size_t len)
{
    void *at;

    if (!guestRangeIsFree(base, len))
        return 0;

    at = mmap((void *)base, len, PROT_NONE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (at == MAP_FAILED)
        return 0;
    if ((uintptr_t)at != base) {
        munmap(at, len);
        return 0;
    }
    return 1;
}

int guestMemInit(void)
{
    /*
     * Acquire the window by PROBING 4 GiB-aligned bases directly with a
     * 4 GiB MAP_FIXED PROT_NONE reservation.
     *
     * NOT overshoot-and-trim: an 8 GiB reservation is ENOMEM under a real
     * app's memory limit on device (measured on an iPhone Air,
     * docs/spike-cartbase.md §F) even though it succeeds on macOS and the
     * simulator. Measured on device: 1<<33 and 1<<36..1<<38 are EACCES (the
     * dyld shared cache), anything below 4 GiB and 1<<46 upward is ENOMEM,
     * and 1<<34, 1<<35 and 0x7000000000 are free.
     *
     * PROT_NONE costs no physical memory; only the sub-ranges that
     * guestMemCommit() maps over are ever backed.
     */
    static const uintptr_t candidates[] = {
        (uintptr_t)1 << 34,
        (uintptr_t)1 << 35,
        (uintptr_t)0x7000000000ULL,
    };

    const size_t len = (size_t)GUEST_WINDOW_SIZE;
    uintptr_t base = 0;
    size_t i;

    if (g_guestBase)
        return 0;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && !base; i++) {
        if (guestTryReserve(candidates[i], len))
            base = candidates[i];
    }

    /* Fallback: walk upward in 4 GiB steps from 0x7000000000, staying below
     * the 47-bit user VA limit. */
    for (uintptr_t a = (uintptr_t)0x7000000000ULL;
         !base && a < ((uintptr_t)1 << 46); a += GUEST_WINDOW_SIZE) {
        if (guestTryReserve(a, len))
            base = a;
    }

    if (!base) {
        portConLogf("ge: guestmem: no free 4 GiB-aligned 4 GiB window "
                    "(last errno=%d)\n", errno);
        return -1;
    }

    g_guestBase = base;
    portConLogf("ge: guestmem: guest window at %p (4 GiB, low32=0, "
                "probed)\n", (void *)g_guestBase);
    return 0;
}

void *guestMemCommit(uint32_t guestAddr, size_t len, int fd)
{
    void *want = GUEST_PTR(guestAddr);
    int flags = (fd >= 0) ? MAP_SHARED : (MAP_PRIVATE | MAP_ANONYMOUS);
    void *at;

    /* Inside our own PROT_NONE reservation: MAP_FIXED cannot clobber anything
     * that is not ours. */
    flags |= MAP_FIXED;

    at = mmap(want, len, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (at == MAP_FAILED)
        return NULL;
    if (at != want) {
        munmap(at, len);
        return NULL;
    }
    return at;
}

void guestMemRelease(uint32_t guestAddr, size_t len)
{
    if (!len)
        return;
    /* Put the reservation back rather than punching a hole in the window that
     * an unrelated allocation could later occupy. */
    mmap(GUEST_PTR(guestAddr), len, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

size_t guestMemReadableFrom(const void *p, size_t cap)
{
    uintptr_t start = (uintptr_t)p, end = start;
    int guard = 0;

    while (end - start < cap && guard++ < 64) {
        ge_vm_addr_t addr = (ge_vm_addr_t)end;
        ge_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = GE_VM_REGION(mach_task_self(), &addr, &size,
                                        VM_REGION_BASIC_INFO_64,
                                        (vm_region_info_t)&info, &count,
                                        &object);
        if (kr != KERN_SUCCESS || addr > (ge_vm_addr_t)end ||
            !(info.protection & VM_PROT_READ))
            break;                      /* hole or unreadable at `end` */
        end = (uintptr_t)(addr + size);
    }
    return (end - start < cap) ? (size_t)(end - start) : cap;
}

#elif defined(_WIN32)

/* guest == host; the window is unused (not a Midas target today). */
int guestMemInit(void)
{
    return 0;
}

void *guestMemCommit(uint32_t guestAddr, size_t len, int fd)
{
    (void)guestAddr; (void)len; (void)fd;
    return NULL;
}

void guestMemRelease(uint32_t guestAddr, size_t len)
{
    (void)guestAddr; (void)len;
}

size_t guestMemReadableFrom(const void *p, size_t cap)
{
    (void)p;
    return cap;
}

#else /* non-Apple POSIX: guest == host, literal addresses are mappable */

#include <errno.h>
#include <sys/mman.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

int guestMemInit(void)
{
    return 0;
}

void *guestMemCommit(uint32_t guestAddr, size_t len, int fd)
{
    void *want = GUEST_PTR(guestAddr);
    int flags = (fd >= 0) ? MAP_SHARED : (MAP_PRIVATE | MAP_ANONYMOUS);
    void *at;

#ifdef MAP_FIXED_NOREPLACE
    /* Linux 4.17+: fail instead of silently relocating or clobbering. */
    flags |= MAP_FIXED_NOREPLACE;
#endif

    at = mmap(want, len, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (at == MAP_FAILED)
        return NULL;
    if (at != want) {
        munmap(at, len);
        return NULL;
    }
    return at;
}

void guestMemRelease(uint32_t guestAddr, size_t len)
{
    if (len)
        munmap(GUEST_PTR(guestAddr), len);
}

size_t guestMemReadableFrom(const void *p, size_t cap)
{
    (void)p;
    return cap;
}

#endif
