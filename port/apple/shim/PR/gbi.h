/*
 * PC port shim for PR/gbi.h (see docs/internals.md).
 *
 * The real header (include/PR/gbi.h) already has 64-bit-ready Gwords and
 * gSP* macros that pack full 64-bit pointers into words.w1. However, the
 * Gfx union's command members (Gdma, Gtri, Gloadtile, ...) are guarded by
 * `IS_BIG_ENDIAN && !IS_64_BIT`, which is false on a little-endian 64-bit
 * host, leaving Gfx with only the `words` member.
 *
 * Even if we fake those macros to expose the N64-layout members, they are
 * FUNCTIONALLY WRONG on a 64-bit little-endian host: the N64 bitfield
 * layouts read the wrong bytes. For example, the N64 Gdma puts `cmd` at
 * bits 0-7, but the gSP* macros pack `cmd` at bits 24-31, so `dma.cmd`
 * reads 0 instead of the command.
 *
 * This shim therefore:
 *   1. Includes the real header with faked IS_BIG_ENDIAN/IS_64_BIT so all
 *      the types and gSP* macros are available.
 *   2. Defines LE-correct versions of the members the game code actually
 *      reads: Gdma, Gtri/Tri, Gloadtile.
 *   3. Defines a Gfx_le union with `words` + the LE-correct members + the
 *      N64-layout members (for completeness).
 *   4. #defines Gfx to Gfx_le so all game code and gSP* macros use it.
 *
 * The port layer (Phase 2+) is responsible for placing/convert-
 * ing display-list data to match these layouts.
 *
 * Inert in the N64 build (no -DPORT): pure pass-through.
 *
 * Project Midas: imported verbatim from GoldenEye-Decomp-ios
 * engine/port/shim/PR/gbi.h (include/PR/gbi.h is identical in both trees).
 * port/apple/shim is on the include path for GE_HOST32=OFF only, ahead of
 * include/; the 32-bit x86 build never sees this file. Midas's own game code
 * reads only `words` (its x86 Gfx has no command views either); the donor
 * sources merged in round 2 read .dma/.tri/.loadtile through these views.
 * docs/port-ios/04-round1-prereqs.md.
 */
#ifndef _PORT_SHIM_GBI_H_
#define _PORT_SHIM_GBI_H_
#if defined(PORT)
/*
 * The real header wraps Gwords and all command structs in
 * `#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)`. This GCC
 * does not define _LANGUAGE_C, so we define it here to pull in those types.
 */
#    ifndef _LANGUAGE_C
#        define _LANGUAGE_C 1
#    endif
#    undef IS_BIG_ENDIAN
#    undef IS_64_BIT
#    define IS_BIG_ENDIAN 1
#    define IS_64_BIT 0
#    include "include/PR/gbi.h"
#    undef IS_BIG_ENDIAN
#    undef IS_64_BIT
#    include <stdint.h>
#    define IS_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#    define IS_64_BIT (UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFU)

/* ------------------------------------------------------------------ */
/* LE-correct command layouts for the members the game code reads.     */
/* ------------------------------------------------------------------ */

/*
 * Gdma: the gSP* macros pack w0 = (cmd << 24) | (len & 0xFFFFFF),
 * w1 = addr. On a 64-bit LE host, that means:
 *   bits 0-15   = len
 *   bits 16-23  = par
 *   bits 24-31  = cmd
 *   bits 32-63  = 0 (unused)
 *   bits 64-127 = addr
 *
 * Round 5a (D69/D83): this struct used to declare a single `par : 24` over
 * bits 0-23, merging N64's THREE fields (`int cmd:8; unsigned par:8;
 * unsigned len:16;`) into two. `cmd` was right, but `dma.par` then returned
 * (par << 16) | len instead of par, and `dma.len` was a dead 32-bit hole.
 * Game code that reads the G_VTX header through this view got the wrong
 * answer: `bgBuildRoomVtxBounds`'s `((dma.par >> 4) & 0xf) + 1` read the low
 * nibble of `len` (== 16*n, so always 0 mod 16 -> nibble == n & 0xf) instead
 * of the (n-1) nibble of `par`, yielding n+1 vertices for every batch -- one
 * Vtx past the end on the LAST batch of a room, which is precisely the 107
 * "out of range" batches the D69 safety net was silently dropping across 67
 * Dam rooms. bg.c's D154 site had already hit the same hazard and worked
 * around it locally (reading words.w0 >> 16 by hand) without fixing the
 * shim. Split the fields the way the N64 header does; `cmd` keeps its
 * signedness (G_ENDDL is -72, a NEGATIVE constant, so a signed cmd is
 * load-bearing) and every `.dma.cmd` reader is unaffected.
 */
typedef struct {
    intptr_t  len : 16;   /* bits 0-15   = len  (n * sizeof(Vtx))   */
    intptr_t  par : 8;    /* bits 16-23  = par  ((n-1) << 4 | v0)   */
    intptr_t  cmd : 8;    /* bits 24-31  = cmd                      */
    intptr_t  pad : 32;   /* bits 32-63  = 0 (unused)               */
    uintptr_t addr;       /* bits 64-127 = addr (from packing)      */
} Gdma_le;

/*
 * Gtri: G_TRI1's vertex indices live in w1, NOT w0.
 *
 * Round 6 audit. The old Tri_le/Gtri_le pair was wrong twice over and its
 * comment described a packing this build does not use:
 *   - It claimed `w0 = (cmd << 24) | (v0*2 << 16) | ... , w1 = 0`. That is
 *     the F3DEX/F3DLP form (`__gsSP1Triangle_w1` at gbi.h:2020). Nothing in
 *     this tree defines F3DEX_GBI/F3DLP_GBI/F3DEX_GBI_2, so the BASE F3D
 *     form is in force: `gSP1Triangle` (gbi.h:2131) puts ONLY the command in
 *     w0 and packs `flag<<24 | v0*10<<16 | v1*10<<8 | v2*10` into **w1**.
 *     The x10 is confirmed from three independent directions: the game's own
 *     `/ 10` in lightfixture.c, fast3d's `C1(16,8) / 10` G_TRI1 dispatch
 *     (gfx_pc.cpp), and bg.c's D154 parser, which reads the indices out of
 *     `words.w1` bytes 2/1/0 by hand.
 *   - It then placed the 4 vertex bytes at struct bytes 0-3, i.e. the low
 *     four bytes of the 8-byte `words.w0` -- which for G_TRI1 hold the 24
 *     zero pad bits. So `tri.tri.v[0..2]` returned 0, 0, 0 for every
 *     triangle, on every host, always.
 *
 * On the 16-byte PC Gfx the N64 w1 is at bytes 8-11 (little end first), so
 * the index bytes land in ASCENDING memory order v2, v1, v0, flag. An array
 * cannot preserve the decomp's `v[0] == v0` meaning across that reversal, so
 * the fields are named; the one reader (lightfixture.c) was updated.
 */
typedef struct {
    uintptr_t     w0;     /* bytes 0-7   = words.w0 (cmd in bits 24-31) */
    unsigned char v2;     /* byte 8   = w1 bits 0-7   = v2 * 10 */
    unsigned char v1;     /* byte 9   = w1 bits 8-15  = v1 * 10 */
    unsigned char v0;     /* byte 10  = w1 bits 16-23 = v0 * 10 */
    unsigned char flag;   /* byte 11  = w1 bits 24-31            */
    unsigned int  w1hi;   /* bytes 12-15 = w1 high word (unused) */
} Gtri_le;

/*
 * Gloadtile: the gSPLoadTile macro packs w0 = (cmd << 24) |
 * (sl << 12) | tl | ... On a 64-bit LE host, that means:
 *   bits 0-11   = tl
 *   bits 12-23  = sl
 *   bits 24-31  = cmd
 *   bits 32-34  = tile
 *   bits 35-39  = pad
 *   bits 40-51  = sh
 *   bits 52-63  = th
 * So the LE-correct layout is:
 *
 * Round 6 audit: the w0 half (tl/sl/cmd) was right and is the only half any
 * caller touches (unk_092E50.c writes .loadtile.sl/.tl into four static
 * G_SETTILESIZE slots). The w1 half was WRONG twice: it was placed at struct
 * bits 32-63, which on the 16-byte PC Gfx is the unused HIGH half of
 * `words.w0` and reads as zero -- the real w1 is at bytes 8-15; and within
 * the word the N64 order is th(0-11), sh(12-23), tile(24-26), pad(27-31),
 * i.e. the mirror of what was written. Corrected below for the next reader;
 * no behaviour changes, because nothing has ever read those four fields
 * (`grep -rn '\.loadtile\.' src port` -> sl/tl only).
 */
typedef struct {
    unsigned int tl   : 12;  /* w0 bits 0-11  */
    unsigned int sl   : 12;  /* w0 bits 12-23 */
    unsigned int cmd  : 8;   /* w0 bits 24-31 */
    unsigned int w0hi : 32;  /* bytes 4-7, unused on the PC layout */
    unsigned int th   : 12;  /* w1 bits 0-11  */
    unsigned int sh   : 12;  /* w1 bits 12-23 */
    unsigned int tile : 3;   /* w1 bits 24-26 */
    unsigned int pad  : 5;   /* w1 bits 27-31 */
    unsigned int w1hi : 32;  /* bytes 12-15, unused */
} Gloadtile_le;

/* ------------------------------------------------------------------ */
/* LE-correct Gfx union.                                               */
/* ------------------------------------------------------------------ */
typedef union {
    Gwords        words;
    Gdma_le       dma;
    Gtri_le       tri;
    Gloadtile_le  loadtile;
    /*
     * Round 6 audit: the fifteen N64-layout members that used to sit here
     * (line, popmtx, segment, setothermodeH/L, texture, perspnorm, setimg,
     * setcombine, setcolor, fillrect, settile, settilesize, loadtlut) are
     * REMOVED. They were labelled "not used by game code, kept for
     * completeness", and the first half of that was true -- `grep -rn
     * '(\.|->)(settile|setimg|segment|texture|...)\.' src port include`
     * finds nothing -- but the second half was a trap: every one of them is
     * a 32-bit N64 bitfield layout laid over a 16-byte little-endian Gfx, so
     * each reads the wrong bytes exactly the way Gdma_le did before round 5a
     * (D69/D83, 107 silently dropped vertex batches). Leaving a set of
     * plausible-looking wrong views in the union invites the next reader to
     * use one. If a member is genuinely needed, write it the way Gdma_le /
     * Gtri_le / Gloadtile_le are written -- against `words`, in this file,
     * with the bit positions spelled out -- and add a reader.
     */
    long long int force_structure_alignment;
} Gfx_le;

/* The PC Gfx is exactly two 64-bit words. Every layout above depends on it,
 * and so does every `gfx++` walk over a converted display list. */
typedef char port_gfx_le_is_16_bytes[(sizeof(Gfx_le) == 16) ? 1 : -1];

#define Gfx Gfx_le

#endif /* defined(PORT) */
#endif /* _PORT_SHIM_GBI_H_ */
