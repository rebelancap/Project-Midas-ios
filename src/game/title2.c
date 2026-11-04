#include <ultra64.h>
#ifdef PORT
#include <gbi_extension.h>
#endif
#include "title2.h"


f32 D_8002BB00 = 0; // Set but never read
f32 D_8002BB04 = 0; // Set but never read
f32 D_8002BB08 = 0; // Set but never read
f32 D_8002BB0C = 255.0; // Set but never read
f32 D_8002BB10 = 255.0; // Set but never read
f32 D_8002BB14 = 255.0; // Set but never read


/**
 * Address: 7F01B240
 * 
 * Render the gun barrel background in the folder select menu. 
 * Like all front end screens, this screen renders at the high res 440x330 resolution.
 * The 440x299 8-bit background texture is far too large to fit in the N64's 4KB texture cache (TMEM),
 * so instead the texture is read one 440 x 1 pixel row at a time. That row of pixels is then applied to a rectangle and the game
 * renders 299 one pixel tall rectangles, moving down the screen, until the screen is filled.
 * 
 * The game also gives the rectangles a color, starting darker at the top and lighter towards the bottom, creating a smooth vertical gradient.
 */
Gfx* titleRenderFolderMenuBackgroundLines(Gfx* gdl, u8 *image, s32 xOffset, struct FolderSelectColour* topColour, struct FolderSelectColour* bottomColour) 
{
    f32 topRed;
    f32 topGreen;
    f32 topBlue;
    f32 bottomRed;
    f32 bottomGreen;
    f32 bottomBlue;
    f32 temp_f14_2; // unused
    s32 i;

    topRed = topColour->red;
    topGreen = topColour->green;
    topBlue = topColour->blue;
    bottomRed = bottomColour->red;
    bottomGreen = bottomColour->green;
    bottomBlue = bottomColour->blue;
    D_8002BB0C = bottomRed;
    D_8002BB10 = bottomGreen;
    D_8002BB14 = bottomBlue;

    i = 0;

#ifdef PORT
    /* D-044 (round 11c): this is THE front-end backdrop -- the mottled
     * gun-barrel image behind every folder screen, and behind the intro
     * gunbarrel (title.c insert_sniper_sight_eye_intro). It is a screen-space
     * texture rectangle, so D-033 squeezed it back to 4:3 along with the
     * folder and the text, and a 16:9 window got black pillarbox bars either
     * side of it. A backdrop is the one screen-space class that should follow
     * the WINDOW, not the 4:3 box: bracket it G_ASPECT_WIDE_EXT. The 440x1
     * rows are stretched horizontally, which is what PD does with its own
     * backdrops and is invisible on this texture. No-op on a 4:3 window or
     * with Video.Widescreen=0. */
    gSPAspectWideBeginEXT(gdl++);

    /* D-049 (round 12c): load the WHOLE 440x299 image once, outside the loop.
     *
     * The 299 per-row gDPLoadTextureBlock calls below exist because the N64's
     * 4 KB TMEM cannot hold the image; on the host there is no TMEM and the
     * only thing a texture load buys is cost. In fast3d a texture change is
     * the one per-rectangle state change that forces a draw-call boundary
     * (`rdp.textures_changed` -> `gfx_flush(); import_texture()` in
     * gfx_sp_tri1), so 299 rows meant 299 draw calls, 299 cache lookups and
     * 299 resident GL textures. The PRIM colour, which is what actually
     * differs per row, is packed into the VERTEX buffer (G_CCMUX_PRIMITIVE in
     * gfx_sp_tri1), so it does NOT break a batch. Load once and the same 299
     * rectangles, with the same per-row prim colours, coalesce into three
     * draws (MAX_BUFFERED = 256 triangles) -- same texels, same colours, same
     * pixels, ~1/100th of the draw calls. M-011 has the numbers.
     *
     * Row i is then selected by the rectangle's own `t` coordinate instead of
     * by the load address -- see the loop body for why that is the row's
     * texel CENTRE with dtdy = 0 rather than the two obvious spellings.
     *
     * A plain gDPLoadTextureBlock cannot express 131,560 bytes (the RDP's lrs
     * field is 12 bits) and the libultra gDPLoadTextureTile macro splits the
     * load into TMEM-sized strips, so the load is spelled out here as
     * SETTIMG / SETTILE(LOADTILE) / LOADTILE / SETTILE(RENDERTILE) /
     * SETTILESIZE -- exactly what fast3d's gfx_dp_load_tile() consumes. */
    gDPSetTextureImage(gdl++, G_IM_FMT_I, G_IM_SIZ_8b, 440, image);
    gDPSetTile(gdl++, G_IM_FMT_I, G_IM_SIZ_8b, 55, 0, G_TX_LOADTILE, 0,
               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOLOD,
               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOLOD);
    gDPLoadSync(gdl++);
    gDPLoadTile(gdl++, G_TX_LOADTILE, 0, 0,
                439 << G_TEXTURE_IMAGE_FRAC, 298 << G_TEXTURE_IMAGE_FRAC);
    gDPPipeSync(gdl++);
    gDPSetTile(gdl++, G_IM_FMT_I, G_IM_SIZ_8b, 55, 0, G_TX_RENDERTILE, 0,
               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOLOD,
               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOLOD);
    gDPSetTileSize(gdl++, G_TX_RENDERTILE, 0, 0,
                   439 << G_TEXTURE_IMAGE_FRAC, 298 << G_TEXTURE_IMAGE_FRAC);
#endif

    while ((i + 1) < 300) 
    {
#ifndef PORT
        // Load a 440 wide by 1 pixel tall 8-bit intensity texture.
        gDPLoadTextureBlock(
            gdl++, image, G_IM_FMT_I, G_IM_SIZ_8b, 440, 1, 0, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
            G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD
        );
#endif

        gDPSetPrimColor(
            gdl++, 0, 0, topRed + (((bottomRed - topRed) * i) / 299.0f),
            topGreen + (((bottomGreen - topGreen) * i) / 299.0f), topBlue + (((bottomBlue - topBlue) * i) / 299.0f), 255
        );

#ifdef PORT
#ifdef PORT
        /* D-049: row i now comes from the `t` coordinate, not the load, and
         * dtdy is 0 so t is CONSTANT across the rectangle, at the row's TEXEL
         * CENTRE (i + 0.5, hence the +16 in S10.5). Every sample in the row then lands on texel row i exactly,
         * which is what the 1-row G_TX_CLAMP texture used to guarantee. The
         * two obvious spellings are both wrong at the edges: with the
         * original dtdy = 1<<10 the interpolated t runs [i, i+0.75] and
         * samples at high-contrast row boundaries round into the neighbouring
         * row (measured: 2.6 % of pixels differed), and with t = i exactly,
         * i/299 * 299 can land an ulp below i in the sampler. */
        #define GE_BG_ROW_T   (((i) << 5) + 16)
        #define GE_BG_ROW_DTDY 0
#else
        #define GE_BG_ROW_T    0
        #define GE_BG_ROW_DTDY (1 << 10)
#endif
#endif
        if (xOffset < 0) 
        {
            gSPTextureRectangle(
#ifdef PORT
                gdl++, 0, (i + 0x10) << 2, (440 << 2) - 1, ((((i + 1) + 0x10)) << 2) - 1, G_TX_RENDERTILE, (-xOffset) << 5, GE_BG_ROW_T,
                1 << 10, GE_BG_ROW_DTDY
#else
                gdl++, 0, (i + 0x10) << 2, (440 << 2) - 1, ((((i + 1) + 0x10)) << 2) - 1, G_TX_RENDERTILE, (-xOffset) << 5, 0,
                1 << 10, 1 << 10
#endif
            );
        } 
        else 
        {
            gSPTextureRectangle(
#ifdef PORT
                gdl++, xOffset << 2, (i + 0x10) << 2, (440 << 2) - 1, ((((i + 1) + 0x10)) << 2) - 1, G_TX_RENDERTILE, 0, GE_BG_ROW_T,
                1 << 10, GE_BG_ROW_DTDY
#else
                gdl++, xOffset << 2, (i + 0x10) << 2, (440 << 2) - 1, ((((i + 1) + 0x10)) << 2) - 1, G_TX_RENDERTILE, 0, 0,
                1 << 10, 1 << 10
#endif
            );
        }
#ifdef PORT
        #undef GE_BG_ROW_T
        #undef GE_BG_ROW_DTDY
#endif

        i++;
#ifndef PORT
        image += 440; // Advance the image data by 440 bytes, since each texel is 1 byte.
#endif
    }

#ifdef PORT
    gSPAspectWideEndEXT(gdl++);   /* D-044 */
#endif

    D_8002BB08 = topBlue;
    D_8002BB04 = topGreen;
    D_8002BB00 = topRed;

    return gdl;
}


/**
 * Unreferenced.
 */
Gfx *sub_GAME_7F01B6E0(Gfx *gdl, u8 *image, s32 arg2)
{
    static f32 D_8002BB2C = 0;
    static f32 D_8002BB28 = 0;
    static f32 D_8002BB24 = 0;
    static f32 D_8002BB20 = 0;
    static f32 D_8002BB1C = 0;
    static f32 D_8002BB18 = 0;
    s32 i;
    s32 xOffset;

    i = 0;
    xOffset = 0;
    D_8002BB2C = arg2;
    D_8002BB28 = arg2;
    D_8002BB24 = arg2;
    D_8002BB20 = arg2;
    D_8002BB1C = arg2;
    D_8002BB18 = arg2;

    while ((i + 1) < 218)
    {
        gDPLoadTextureBlock(gdl++, image, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 1, 0, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK,G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

        gDPSetPrimColor(gdl++, 0, 0, D_8002BB2C + (((D_8002BB20 - arg2) * i) / 217.0f), D_8002BB28 + (((D_8002BB1C - arg2) * i) / 217.0f), D_8002BB24 + (((D_8002BB18 - arg2) * i) / 217.0f), 0xff);

        if (xOffset < 0)
        {
            gSPTextureRectangle(gdl++, 0, (i + 12) << 2, (320 << 2) - 1, ((i + 13) << 2) - 1, G_TX_RENDERTILE, (-xOffset) << 5, 0, 1 << 10, 1 << 10);
        }
        else
        {
            gSPTextureRectangle(gdl++, xOffset << 2, (i + 12) << 2, (320 << 2) - 1, ((i + 13) << 2) - 1, G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
        }

        i++;
        image += 0x280;
    }

    return gdl;
}