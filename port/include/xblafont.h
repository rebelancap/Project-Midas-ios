#ifndef PORT_XBLAFONT_H
#define PORT_XBLAFONT_H

/*
 * The GoldenEye XBLA release's own two fonts on the game's text (Midas round
 * 43, docs/port-ios/60-round43-xbla-fonts-hud.md).
 *
 * The method is Perfect Dark's (perfect-dark-ios vendor/dabs-mod
 * port/src/xblafont.c, MIT): what is drawn is a *picture of a glyph*, not a
 * font. The ROM glyph's cell, width, baseline and the font's kerning still lay
 * the text out; the release's ink is seated inside the columns the ROM's ink
 * filled, on a line fitted once per font through the ink of both (one scale
 * and one offset), so nothing reflows and nothing overlaps.
 *
 * The importer (xblaimport.c) writes each release font as
 * $C/xbla/font/<name>.png (the atlas) and <name>.txt (the glyph boxes):
 *   doc0  = Bank Gothic (the ROM's ptrFontBankGothic: watch, HUD, ammo counter)
 *   alps3 = Zurich      (the ROM's ptrFontZurichBold: menus, briefing, legal)
 *
 * The game names a glyph by the address of its texels: load_font_tables()
 * registers every character (font, index, pixels) as it loads the fonts, and
 * the renderer's texture-cache miss asks xblaFontFind() whether the texels
 * being drawn are one of them (address and bytes both: the stage pool is
 * reused, so a stale address answers no). Render only: nothing here is read
 * by the sim.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBLAFONT_BANKGOTHIC 0
#define XBLAFONT_ZURICH 1
#define XBLAFONT_NUM 2

/* Game thread, load_font_tables(): character `index` (0 = '!') of ROM font
 * `font` is `width` x `height` I8 texels at `pixels`, rows (width + 7) & ~7
 * bytes apart, drawn `baseline` rows below the line's top. Copies the texels
 * once (they never change); afterwards only the address is updated. */
void xblaFontRegisterGlyph(int32_t font, int32_t index, const void *pixels, int32_t width, int32_t height,
                           int32_t baseline);

/* Render thread. Which registered glyph the texels at `addr` are: 1 and the
 * font and index, or 0. */
int xblaFontFind(const void *addr, uint32_t bytes, int32_t *font, int32_t *index);

/* Render thread. The release's picture for a glyph: malloc'd RGBA8, top row
 * first, an integer multiple of the whole tile the game uploads (its row
 * stride by its height), with r = g = b = a = coverage like the ROM's I8.
 * NULL when there is nothing to draw (no such glyph, no ink): the ROM's own
 * glyph is drawn then. Free with free(). */
uint8_t *xblaFontPicture(int32_t font, int32_t index, int32_t *outW, int32_t *outH);

/* Render thread, at a texpack index rebuild: forgets the release fonts and
 * every built picture, and says how many of the two fonts the layer holds. */
int xblaFontRescan(void);

#ifdef __cplusplus
}
#endif

#endif
