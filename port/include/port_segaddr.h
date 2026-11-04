/**
 * port_segaddr.h — fast3d's GBI address resolution on 64-bit hosts (round 6,
 * docs/port-ios/09-round6-shim.md). Header-only so the host test
 * (port/apple/tests/round6_test.c) runs the same code gfx_pc.cpp does.
 *
 * A GBI address word (w1, uintptr_t) is one of exactly three things:
 *   1. > 0xffffffff: a live host pointer (exe .data/.bss, heap, stack, or a
 *      window pointer the game passed whole). Nothing else is that wide, so
 *      this test goes FIRST: the segment tests look at the low 32 bits only,
 *      and a host pointer whose bits 24-27 named an installed segment used to
 *      be taken for it (donor round C2, "1-in-256 per pointer").
 *   2. < 0x10000000: a segment token 0x0Sxxxxxx, PD-tagged (bit 0) or GE
 *      untagged, resolved through the live segment bases. An unresolvable one
 *      (0 = NULL, an uninstalled slot) passes through unchanged.
 *   3. otherwise: a 32-bit GUEST address (a window pointer that went through a
 *      u32: osVirtualToPhysical, an s32 field) -> GUEST_PTR. The window base
 *      has zero low 32 bits, so this is lossless.
 * PORT_IS_HOST_ADDR (port_hostptr.h) is the same partition: "case 1 or 3".
 *
 * Segment bases (G_MW_SEGMENT) are stored LIVE: a base that went through a
 * u32 arrives as the bare guest address and is rebased; full-width pointers
 * and 0 (uninstall) pass through.
 */
#ifndef PORT_SEGADDR_H
#define PORT_SEGADDR_H

#if defined(PLATFORM_64BIT)
#include <stdint.h>
#include "guestmem.h"
#include "port_hostptr.h"

static inline void *portSegAddr64(uintptr_t w1, const uintptr_t *segbase)
{
    if (w1 > (uintptr_t) 0xffffffffULL) {
        return (void *) w1;
    }
    if (!PORT_IS_HOST_ADDR(w1)) {
        if (w1 & 1) {
            /* PD's tagged form (bit 0 stamped by its asset preprocessor) */
            const uintptr_t seg = (w1 & 0x0f000000) >> 24;
            if (seg && segbase[seg]) {
                return (void *) (segbase[seg] + (w1 & 0x00fffffe));
            }
        }
        {
            /* GE's untagged form (compiled-in and ROM display lists) */
            const uintptr_t seg = w1 >> 24;
            if (seg >= 1 && seg <= 15 && segbase[seg]) {
                return (void *) (segbase[seg] + (w1 & 0x00ffffff));
            }
        }
        return (void *) w1;
    }
    return GUEST_PTR(w1);
}

static inline uintptr_t portSegBase64(uintptr_t data)
{
    if (data != 0 && data <= (uintptr_t) 0xffffffffULL) {
        return (uintptr_t) GUEST_PTR(data);
    }
    return data;
}
#endif /* PLATFORM_64BIT */

#endif /* PORT_SEGADDR_H */
