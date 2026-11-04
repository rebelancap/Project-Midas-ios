/**
 * PORT_IS_HOST_ADDR(v): is this 32-bit word a real host pointer, as opposed to
 * an N64 segment word (0x0Sxxxxxx) or a file offset?
 *
 * On PC, host pointers always live at or above 0x10000000: the Windows image is
 * rebased there, the Linux image sits at 0x08048000 (segments 8/9 are never
 * installed), and both arenas are pinned at 0x20000000+.
 *
 * The original Xbox can't follow that rule: the XBE base is forced to
 * 0x00010000, so the whole image (assets and static buffers) overlaps the N64
 * segment 0/1 word range. There the test is "above 0x10000000 OR inside the
 * image" (xbox_sys.c reads the image size from the XBE header at boot). Heap
 * allocations never hold display-list data, and every other display-list
 * allocation goes through the arenas. See docs/port/xbox-port.md.
 *
 * 64-bit hosts (PLATFORM_64BIT; macOS/iOS arm64): every arena, the ROM and
 * the dedicated mappings are committed inside the guest window
 * (guestmem.h), whose base has zero low 32 bits, so a pointer into them keeps
 * the same low word (>= 0x10000000) as on x86. Anything else the process
 * owns (exe .data/.bss, libc heap, stacks) lives above 4 GiB, so a full-width
 * value > 0xffffffff is always a live host pointer, never a segment word.
 * The test is therefore "value > 0xffffffff, OR low 32 bits >= 0x10000000"
 * (docs/port-ios/01-memory-and-assets.md section 7 step 3). It only sees a
 * truncated word's low 32 bits, and a truncated pointer from OUTSIDE the
 * window is not recoverable here; that is the rule-4 class.
 */
#ifndef PORT_HOSTPTR_H
#define PORT_HOSTPTR_H

#ifdef GE_XBOX
#ifdef __cplusplus
extern "C" {
#endif
int portXboxIsHostAddr(unsigned int v);
#ifdef __cplusplus
}
#endif
#define PORT_IS_HOST_ADDR(v) (portXboxIsHostAddr((unsigned int) (v)))
#elif defined(PLATFORM_64BIT)
#include <stdint.h>
#define PORT_IS_HOST_ADDR(v) \
    ((uintptr_t) (v) > (uintptr_t) 0xffffffffu || \
     (unsigned int) (uintptr_t) (v) >= 0x10000000u)
#else
#define PORT_IS_HOST_ADDR(v) ((unsigned int) (v) >= 0x10000000u)
#endif

#endif
