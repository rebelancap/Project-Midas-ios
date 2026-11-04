/**
 * portsegimg.h — where the port's synthesized "ROM segment" images live on
 * 64-bit hosts (round 6, docs/port-ios/09-round6-shim.md).
 *
 * port/src/segments.c backs a handful of N64 linker symbols (*SegmentRomStart)
 * with port-built images: the anim segments (animdata.c fills them from the
 * cart), the fonts, fontdl, the JPN glyph banks, the rareware-logo copy
 * buffer and the global image table stub. The game reaches every one of them
 * as `&_xxxSegmentRomStart` passed to romCopy(), whose PI shim takes a u32
 * device address, and through CART_SYM(); initanitable.c even adds the anim
 * base into s32 table words. On the 32-bit build they are exe arrays and the
 * u32 is the pointer. On a 64-bit host an exe array lives above 4 GiB, so
 * every one of those uses truncated it (pointer-contract rule 4).
 *
 * So on 64-bit hosts the symbols are ABSOLUTE: each names a fixed GUEST
 * address in a 16 MB region at the top of the cart window (0x1F000000, after
 * the ROM image at 0x10000000 and its sidecar headroom), committed at boot by
 * portSegmentsBindWindow() (main.c, before romdataInit). `&sym` is then the
 * guest address itself: romCopy -> CART_PTR(), CART_SYM(sym) -> the live
 * pointer, `(s32)&sym` arithmetic is lossless. This is the donor's
 * romassets_*.s scheme (guestmem.h CART_SYM). Port code that writes the
 * images goes through PORT_SEG_LIVE(sym); nothing may dereference `&sym`
 * directly (it is a small number in __PAGEZERO).
 *
 * The 32-bit build is untouched: nothing here is defined without
 * PLATFORM_64BIT.
 */
#ifndef PORT_SEGIMG_H
#define PORT_SEGIMG_H

#if defined(PLATFORM_64BIT)
#include "guestmem.h"

/* Plain hex (no suffix, no parentheses): segments.c stringifies them into
 * `.set` directives. Every image starts 16 KB aligned. */
#define PORT_GUEST_SEGIMG_BASE        0x1F000000
#define PORT_SEGIMG_ANIM_ENTRIES      0x1F000000 /* 0x169EC0 */
#define PORT_SEGIMG_ANIM_DATA         0x1F170000 /* 0x00E7E0 */
#define PORT_SEGIMG_FONT_GOTHIC       0x1F180000 /* 0x0024B0 */
#define PORT_SEGIMG_FONT_ZURICH       0x1F184000 /* 0x003540 */
#define PORT_SEGIMG_FONTDL            0x1F188000 /* 0x0000C0 */
#define PORT_SEGIMG_FONTDL_END        0x1F1880C0
#define PORT_SEGIMG_RARELOGO          0x1F18C000 /* 0x006800 (0x67F0 ALIGN64) */
#define PORT_SEGIMG_JFONT             0x1F194000 /* 0x00B700 */
#define PORT_SEGIMG_EFONT             0x1F1A0000 /* 0x001A80 */
#define PORT_SEGIMG_GLOBALIMAGETABLE  0x1F1A4000 /* 0x001400 */
#define PORT_SEGIMG_ALT_START         0x1F1A8000 /* 0x000010 (init.c, never taken) */
#define PORT_SEGIMG_END               0x1F1AC000
#define PORT_GUEST_SEGIMG_LIMIT       0x20000000

/* The live pointer behind an absolute segment symbol. */
#define PORT_SEG_LIVE(sym) ((unsigned char *) CART_SYM(sym))

#ifdef __cplusplus
extern "C" {
#endif
/* segments.c: commit the region and write the images' fixed content. */
void portSegmentsBindWindow(void);
#ifdef __cplusplus
}
#endif
#endif /* PLATFORM_64BIT */

#endif /* PORT_SEGIMG_H */
