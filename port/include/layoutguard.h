/*
 * layoutguard.h -- compile-time guards for raw byte offsets (round 19d, D-100).
 *
 * The bug class: an offset written as a literal against the N64 layout of a
 * struct -- ((u8 *)p)[0x28], *(s32 *)((u8 *)p + 0xC4), memcpy(d, s, 0xc0),
 * mempAllocBytesInBank(n * 0x14) -- reads or writes the WRONG field on a
 * 64-bit host, where every pointer field widens and everything after it
 * moves. No warning sees it; engine/docs/pointer-contract.md section "Raw
 * offsets are host-layout bugs" has the census and the rule.
 *
 * The rule, in short: read a field by NAME (GE_FIELD below is just that,
 * spelled so a reviewer can grep it). Where a raw offset must stay -- a file
 * format, a GBI word, a layout that is identical on both ABIs today -- pin it
 * with GE_RAW_OFFSET/GE_RAW_SIZE next to the site, so the day somebody adds a
 * pointer to the struct the BUILD breaks instead of a frame.
 *
 * Inert in the N64 build (no PORT): IDO has no _Static_assert.
 *
 * Project Midas: imported verbatim from GoldenEye-Decomp-ios
 * engine/port/include/layoutguard.h. PORT is defined for the 64-bit build
 * only, so on Midas's 32-bit x86 build the guards expand to nothing too.
 */
#ifndef PORT_LAYOUTGUARD_H
#define PORT_LAYOUTGUARD_H

#include <stddef.h>

/* A named field read; replaces ((T *)((u8 *)ptr + 0xNN))[0]. */
#define GE_FIELD(ptr, Struct, field) (((Struct *)(ptr))->field)

#if defined(PORT)
#define GE_RAW_OFFSET(Struct, field, off)                                          \
    _Static_assert(offsetof(Struct, field) == (off),                               \
                   "raw offset " #off " into " #Struct "." #field                  \
                   " no longer matches the host layout -- read it by name "        \
                   "(D-100, pointer-contract.md 'Raw offsets are host-layout bugs')")
#define GE_RAW_SIZE(Struct, size)                                                  \
    _Static_assert(sizeof(Struct) == (size),                                       \
                   "a raw stride/size of " #size " assumes sizeof(" #Struct ")"     \
                   " -- it changed (D-100, pointer-contract.md)")
#else
#define GE_RAW_OFFSET(Struct, field, off)
#define GE_RAW_SIZE(Struct, size)
#endif

#endif /* PORT_LAYOUTGUARD_H */
