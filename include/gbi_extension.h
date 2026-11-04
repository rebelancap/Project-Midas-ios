#ifndef _GBI_EXT_H_
#    define _GBI_EXT_H_
#include <PR/gbi.h>

#define TRI4_Ext

/**
 * This file should contain most of the extensions to gbi avoiding loosing
 * changes should the OS ever be updated.
 */

/**
   These are some helper constants. Since they relate to nothing else except GBI
   macros, Ill place them here, to save reserving names that might be used
   elsewhere - though doubtfull.
 */
#define MASK_2 1
#define MASK_4 2
#define MASK_8 3
#define MASK_16 4
#define MASK_32 5
#define MASK_64 6
#define MASK_128 7
#define MASK_256 8
#define MASK_512 9
#define MASK_1024 10

typedef enum
{
    TEXTURETYPE_LOD,
    TEXTURETYPE_DETAIL,
    TEXTURETYPE_MIPMAP,
    TEXTURETYPE_TILE,
    TEXTURETYPE_TILE_PRESWAPPED
} TextureTypes;

/* Set the Texel Scale (0.0 - 1.0) for multiplying UV's eg, 0.5 * (31 << 6) = 31.0 */
#define CALC_TEXSCALE(scale) scale < 1.0f ? scale * 0x10000 : 0xffff

/* Set the Lower Right S Coordinate using texture width */
#define CALC_LRS(width, height, siz) ((((width) * (height) + siz##_INCR) >> siz##_SHIFT) - 1)

#define CALC_TILESIZE(texel)         ((texel)-1) << G_TEXTURE_IMAGE_FRAC



#ifdef TRI4_Ext
#    define G_TRI4 (G_IMMFIRST - 14)
#    define G_SETTEX 0xc0 /*   0 */
#endif

/* Fade Modes ... for GE? */
#define G_CC_MODULATEIFADE    TEXEL0, 0, SHADE, 0, 0, 0, 0, ENVIRONMENT
#define G_CC_MODULATERGBFADE  G_CC_MODULATEIFADE
#define G_CC_MODULATEIFADEA   TEXEL0, 0, SHADE, 0, TEXEL0, 0, ENVIRONMENT, 0
#define G_CC_MODULATEFADE     TEXEL0, 0, SHADE, 0, ENVIRONMENT, 0, TEXEL0, 0
#define G_CC_MODULATERGBFADEA G_CC_MODULATEIFADEA
#define G_CC_FADE             SHADE, 0, ENVIRONMENT, 0, SHADE, 0, ENVIRONMENT, 0
#define G_CC_FADEA            TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0
#define G_CC_DECALFADE        0, 0, 0, TEXEL0, 0, 0, 0, ENVIRONMENT
#define G_CC_DECALFADEA       0, 0, 0, TEXEL0, TEXEL0, 0, ENVIRONMENT, 0
#define G_CC_BLENDRGBFADEA    TEXEL0, SHADE, TEXEL0_ALPHA, SHADE, 0, 0, 0, ENVIRONMENT
#define G_CC_ADDRGBFADE       TEXEL0, 0, TEXEL0, SHADE, 0, 0, 0, ENVIRONMENT
#define G_CC_SHADEFADEA       0, 0, 0, SHADE, 0, 0, 0, ENVIRONMENT


/* Custom version of RM_AA_ZB_XLU_SURF with Z_UPD */
#define RM_CUSTOM_AA_ZB_XLU_SURF(clk) \
    RM_AA_ZB_XLU_SURF(clk) | Z_UPD



#define G_RM_CUSTOM_AA_ZB_XLU_SURF  RM_CUSTOM_AA_ZB_XLU_SURF(1)
#define G_RM_CUSTOM_AA_ZB_XLU_SURF2 RM_CUSTOM_AA_ZB_XLU_SURF(2)


#ifdef TRI4_Ext


/***
 ***  4 Triangles
 ***/
/**
 * Remarks:
 * Ryan: I've been learning a lot about PD's graphics microcode lately. It's likely that the decision to
 * implement tri4 was to reduce memory usage in RDRAM. But to fit 4 tris in one command they had to limit
 * themselves to 4 bits per vertex, which means they can only address up to 16 vertices. They could have
 * fit 32 vertices in DMEM if they wanted. In stock microcodes the vertex IDs are pre-multiplied by 10
 * which makes it easy for the RSP to calculate the vertex offset in the DMEM buffer (it can use a
 * single shift operation). But because PD is using a compact format there's more work for the RSP to
 * calculate it. So they've sacrificed RSP performance for memory saving.
*/

//cannot use 2tri with 4tri, so lets just make sure they are undefined so errors happen.
#undef gSP2Triangles
#undef gsSP2Triangles

/**
  Draw up to 4 triangles at a time.
  Vertex index is 0-15, to use a higher index use gSP1Triangle
  Triangles with all points set to 0 are not drawn.
*/
#define gSP4Triangles(pkt, x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4) \
{                                                                          \
    Gfx *_g = (Gfx *)(pkt);                                                \
                                                                           \
    _g->words.w0 = (_SHIFTL(G_TRI4, 24, 8)                                 \
            | _SHIFTL(z4, 12, 4)                                           \
            | _SHIFTL(z3, 8, 4)                                            \
            | _SHIFTL(z2, 4, 4)                                            \
            | _SHIFTL(z1, 0, 4));                                          \
    _g->words.w1 = (_SHIFTL(y4, 28, 4)                                     \
            | _SHIFTL(x4, 24, 4)                                           \
            | _SHIFTL(y3, 20, 4)                                           \
            | _SHIFTL(x3, 16, 4)                                           \
            | _SHIFTL(y2, 12, 4)                                           \
            | _SHIFTL(x2, 8, 4)                                            \
            | _SHIFTL(y1, 4, 4)                                            \
            | _SHIFTL(x1, 0, 4));                                          \
}

/**
  Draw up to 4 triangles at a time.
  Vertex index is 0-15, to use a higher index use gSP1Triangle
  Triangles with all points set to 0 are not drawn.
*/
#define gsSP4Triangles(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4) \
{                                                                      \
    {                                                                  \
        (_SHIFTL(G_TRI4, 24, 8)                                        \
            | _SHIFTL(z4, 12, 4)                                       \
            | _SHIFTL(z3, 8, 4)                                        \
            | _SHIFTL(z2, 4, 4)                                        \
            | _SHIFTL(z1, 0, 4)),                                      \
        (_SHIFTL(y4, 28, 4)                                            \
            | _SHIFTL(x4, 24, 4)                                       \
            | _SHIFTL(y3, 20, 4)                                       \
            | _SHIFTL(x3, 16, 4)                                       \
            | _SHIFTL(y2, 12, 4)                                       \
            | _SHIFTL(x2, 8, 4)                                        \
            | _SHIFTL(y1, 4, 4)                                        \
            | _SHIFTL(x1, 0, 4))                                       \
    }                                                                  \
}

/**
  Source compatible F3DEX gSP2Triangles for GE
  Vertex index is 0-15, to use a higher index use gSP1Triangle
*/
#define gSP2Triangles(pkt, x1, y1, z1, flag0, x2, y2, z2, flag1) \
	gSP4Triangles(pkt, x1, y1, z1, x2, y2, z2, 0, 0, 0, 0, 0, 0)
#define gsSP2Triangles(x1, y1, z1, flag0, x2, y2, z2, flag1) \
	gsSP4Triangles(x1, y1, z1, x2, y2, z2, 0, 0, 0, 0, 0, 0)


#define	gDPLoadTLUT06(pkt, a, b, c, d)				                                        \
{                                                                                           \
    Gfx *_g = (Gfx *)pkt;                                                                   \
    _g->words.w0 = _SHIFTL(G_LOADTLUT, 24, 8) | _SHIFTL((a), 14, 10) | _SHIFTL((b), 2, 10); \
    _g->words.w1 = _SHIFTL(0x06, 24, 8) | _SHIFTL((c), 14, 10) | _SHIFTL((d), 2, 10);       \
}
#define	gDPLoadTLUT07(pkt, a, b, c, d)				                                        \
{                                                                                           \
    Gfx *_g = (Gfx *)pkt;                                                                   \
    _g->words.w0 = _SHIFTL(G_LOADTLUT, 24, 8) | _SHIFTL((a), 14, 10) | _SHIFTL((b), 2, 10); \
    _g->words.w1 = _SHIFTL(0x07, 24, 8) | _SHIFTL((c), 14, 10) | _SHIFTL((d), 2, 10);       \
}

#define	gDPLoadTLUTCmd2(pkt, tile, count)				\
{									\
	Gfx *_g = (Gfx *)pkt;						\
									\
	_g->words.w0 = _SHIFTL(G_LOADTLUT, 24, 8) | 0xff0;			\
	_g->words.w1 = _SHIFTL((tile), 24, 3) | _SHIFTL((count), 14, 10) | 0xff0;\
}

/*
 Use Texture from bank.
 If detail, use shift eg 15 for 2x detail per base texel, 14 fpr 4x.

 Confirm params from decomp in CO expand function

 @param        cms: s-axis mirror, no-mirror, wrap, and clamp flags
 @param        cmt: t-axis mirror, no-mirror, wrap, and clamp flags eg G_TX_CLAMP
 @param       tile: Number of tiles
 @param     shifts: s-coordinate shift value or G_TX_NOLOD
 @param     shiftt: t-coordinate shift value or G_TX_NOLOD
 @param       type: texture type: E.g. TEXTURETYPE_DETAIL
 @param   minlevel: Clamp at tile 0 + frac (eg, 0.5)
 @param  detail_id: Detail Texture ID
 @param texture_id: Texture ID
 */
#define gsSPUseTexture(cms, cmt, tile, shifts, shiftt, type, minlevel, detail_id, texture_id) \
{                                                    \
    {                                                \
        (_SHIFTL(G_SETTEX, 24, 8) |                  \
         _SHIFTL((cms), 22, 2) |                     \
         _SHIFTL((cmt), 20, 2) |                     \
         _SHIFTL((tile), 18, 2) |                    \
         _SHIFTL((shifts), 14, 4) |                  \
         _SHIFTL((shiftt), 10, 4) |                  \
         _SHIFTL((type), 0, 3)),                     \
        (_SHIFTL((minlevel), 24, 8) |                \
         _SHIFTL((detail_id), 12, 12) |              \
         _SHIFTL((texture_id), 0, 12))               \
    }                                                \
}

#endif

/**
 * Like gDPSetPrimColor, but is useful when the input colour is already in
 * RGBA format. It avoids unnecessary bitshifting and masking.
 */
#define	gDPSetPrimColorViaWord(pkt, m, l, rgba)     \
{                                                   \
	Gfx *_g = (Gfx *)(pkt);                         \
	_g->words.w0 =	(_SHIFTL(G_SETPRIMCOLOR, 24, 8) \
			| _SHIFTL(m, 8, 8)                      \
			| _SHIFTL(l, 0, 8));                    \
	_g->words.w1 =  (rgba);                         \
}

#define gDPSetEnvColorViaWord(pkt, rgba) gDPSetColor(pkt, G_SETENVCOLOR, rgba)
#define gDPSetFogColorViaWord(pkt, rgba) gDPSetColor(pkt, G_SETFOGCOLOR, rgba)



#ifdef PORT
#ifdef PORT
/* ---------------------------------------------------------------------------
 * D-044 / D-045 (round 11c): the PD extended-GBI aspect seam, made reachable
 * from GE game code.
 *
 * fast3d already carries Perfect Dark's per-draw aspect machinery
 * (port/fast3d/gfx_pc.cpp gfx_update_aspect_mode / gfx_adjust_x_for_aspect_ratio)
 * and port/fast3d/gbiex.h names the opcode and the bits -- but only on the
 * DECODER side, so nothing in src/ could ever emit one. GE's real RSP ucode
 * has no handler for G_EXTRAGEOMETRYMODE_EXT, which is why every use below is
 * PORT-only: on the N64 these display lists are unchanged.
 *
 * GE's default for a screen-space draw is D-033's squeeze back to 4:3 about
 * the centre (PD's default is the opposite -- it fills, and elements opt IN to
 * G_ASPECT_CENTER_EXT). So the bit GE needs to opt in to is the other one:
 *   G_ASPECT_WIDE_EXT on its own, with no side bit = "this screen-space draw
 *   spans the WINDOW, not the 4:3 box" -- i.e. a backdrop.
 * Values mirror PD's src/include/gbiex.h exactly.
 * ------------------------------------------------------------------------- */
#ifndef G_EXTRAGEOMETRYMODE_EXT
#define G_EXTRAGEOMETRYMODE_EXT    0x3a
#endif
#ifndef G_ASPECT_LEFT_EXT
#define G_ASPECT_LEFT_EXT          0x00000010
#endif
#ifndef G_ASPECT_RIGHT_EXT
#define G_ASPECT_RIGHT_EXT         0x00000020
#endif
#ifndef G_ASPECT_WIDE_EXT
#define G_ASPECT_WIDE_EXT          0x00000040
#endif
#ifndef G_ASPECT_CENTER_EXT
#define G_ASPECT_CENTER_EXT        (G_ASPECT_LEFT_EXT | G_ASPECT_RIGHT_EXT)
#endif
#ifndef G_ASPECT_MODE_EXT
#define G_ASPECT_MODE_EXT          (G_ASPECT_CENTER_EXT | G_ASPECT_WIDE_EXT)
#endif

#define gSPExtraGeometryModeEXT(pkt, c, s)                                              \
{                                                                                       \
    Gfx* _g = (Gfx*)(pkt);                                                              \
                                                                                        \
    _g->words.w0 = _SHIFTL(G_EXTRAGEOMETRYMODE_EXT, 24, 8) | _SHIFTL(~(u32)(c), 0, 24); \
    _g->words.w1 = (u32)(s);                                                            \
}

#define gSPSetExtraGeometryModeEXT(pkt, word)   gSPExtraGeometryModeEXT((pkt), 0, word)
#define gSPClearExtraGeometryModeEXT(pkt, word) gSPExtraGeometryModeEXT((pkt), word, 0)

/* Bracket a backdrop: everything between these two fills the window instead of
 * being squeezed back to 4:3. No-op on a 4:3 window / Video.Widescreen=0. */
/* Midas round 2 (D-M02): Midas's fast3d honours G_EXTRAGEOMETRYMODE_EXT, so
 * the donor's D-044 backdrop brackets would change Midas's 64-bit rendering.
 * They are emitted only when portAspectWideBackdrops() is non-zero; the stub
 * in port/apple/donor_stubs.c returns 0 (Midas's behaviour). The macro
 * argument (`gdl++` at the call sites) is evaluated only inside the branch,
 * so a disabled bracket consumes no command slot. */
extern int portAspectWideBackdrops(void);
#define gSPAspectWideBeginEXT(pkt) do { if (portAspectWideBackdrops()) { gSPSetExtraGeometryModeEXT((pkt), G_ASPECT_WIDE_EXT); } } while (0)
#define gSPAspectWideEndEXT(pkt)   do { if (portAspectWideBackdrops()) { gSPClearExtraGeometryModeEXT((pkt), G_ASPECT_MODE_EXT); } } while (0)

/*
 * D-055 (round 14a): the A/B lever for the D-052 SPANS, and only those.
 *
 * D-052 widened three FILLS -- the cloudless sky background (sky.c), the
 * fade/wash rectangle (bondview2.c currentPlayerDrawFade) and the framebuffer
 * clear (blood_animation.c) -- so they cover the window rather than a 4:3 box.
 * Those three are the round-13 change Austin's phone is rendering wrong, and
 * `Video.WideSpans = 0` turns them back into plain 4:3 draws WITHOUT touching
 * the D-044 backdrop sites, which keep the plain macro above and have been on
 * the hub since 0.0.0.3.
 *
 * Deliberately one macro pair rather than an `if` at each of the four sites:
 * the sites stay readable, and the lever cannot be half-applied. Note it takes
 * the display-list CURSOR (`gdl`), not `gdl++` -- the increment has to be
 * inside the branch or a disabled span would still eat a command slot.
 *
 * Default 1 = exactly what 0.0.0.5 emitted, so the macOS oracle and the M-001
 * replay digest are unchanged.
 */
extern int portWideSpans(void);
#define gSPAspectSpanBeginEXT(gdl) do { if (portWideSpans()) { gSPAspectWideBeginEXT((gdl)++); } } while (0)
#define gSPAspectSpanEndEXT(gdl)   do { if (portWideSpans()) { gSPAspectWideEndEXT((gdl)++); } } while (0)
#endif /* PORT */

/* Tier C (round C2, D-112): port-only opcodes emitted by beanstage.c alone,
 * the values Perfect Dark's port gave them (its gbiex.h). fast3d handles them;
 * GoldenEye's own lists never contain them.
 *   G_SETDEPTHBIAS_EXT  w1 = s16 push away from the eye, in depth steps
 *   G_SETTILEOFFSET_EXT w0 low 3 bits = tile, w1 = s16 s << 16 | s16 t, in
 *                       1/512ths of a texel subtracted from the tile origin */
#ifdef PORT
/* Midas round 12 (XBLA step 3, docs/port-ios/21 R12-2): 0x49 is already Midas's
 * own G_DXLU_GROUP_EXT (port/fast3d/gbiex_compat.h; src/game/bg.c, chrprop.c and
 * explosion.c emit it as a literal), so the depth bias takes the free 0x50 here.
 * Every Bean file emits it by this name; port/fast3d/gbiex_compat.h repeats the
 * same values for fast3d, which does not include this header. */
#define G_SETDEPTHBIAS_EXT  0x50
#else
#define G_SETDEPTHBIAS_EXT  0x49
#endif
#define G_SETTILEOFFSET_EXT 0x4d
/* Route B (D-120): w1 = host pointer to a struct GfxHdMeshDraw
 * (port/include/hdmesh.h) - one indexed draw of a built room's static mesh */
#define G_HDMESH_EXT        0x4e
/* gSPMatrix parameter bit: the matrix is 16 host floats, not N64 fixed point */
#define G_MTX_FLOATS_EXT    0x20
#ifdef PORT
/* Round 32c (D-300), the donor's port/include/chrlight.h: HD character lights
 * (w1 = host pointer to a GfxChrLight). Same value as chrlight.h. */
#define G_CHRLIGHT_EXT      0x4f
#endif
#ifdef PORT
/* ------------------------------------------------------------------------ */
/* DISPLAY-LIST CLASS TAGS (visionOS stereo 3D, Phase 6 M3, round 47;       */
/* docs/port-ios/64 §2.4). From perfect-dark-ios overlay patch 0032         */
/* (src/include/gbiex.h PD_DLTAG_*, D-055), renamed GE_ and re-magicked.    */
/*                                                                          */
/* The stereo fold in port/fast3d/gfx_pc.cpp needs to know WHAT the list is  */
/* drawing: the world, the viewmodel and the flat things (watch, menus,     */
/* gauges, the front end) need different eye offsets, and the shape of the  */
/* projection matrix is not a property of the subject (PD D-055: the device */
/* counted as much "sky" as "world" when it guessed from the matrix).       */
/*                                                                          */
/* A tag is a G_NOOP whose w1 carries a magic in its high half. G_NOOP is   */
/* ignored by the N64 RSP and by gfx_pc unless a known marker is in w1, so a */
/* tag changes no draw, tri, vert or texture count anywhere. The magic      */
/* '3D' (0x3344) collides with none of this tree's other G_NOOP markers:     */
/* the sky pairing marker 0x5359xxxx ('SY', sky.c) and the high bytes 0x46  */
/* (exact fill colour, fr.c) and 0x47 (glyph run, textrelated.c). w1 == 0   */
/* (gDPNoOp) is "not ours".                                                  */
/*                                                                          */
/* CLASSES name the NEXT projection LOAD, one only; an untagged load is     */
/* WORLD and a G_MTX_MUL into the projection keeps the class of the load.   */
/* SPANS are sticky between BEGIN and END: FLAT spans nest (a counter), the */
/* GUN span is the viewmodel bracket (bondview2.c portCamLatchGun). GE's    */
/* sky is drawn as RDP triangles (sky.c skyRenderTri/Full), matrix-free, so  */
/* SKY is applied by gfx_rdp_triangle; the class tag is kept for sky.c's    */
/* matrix path (skyPortRenderPoly).                                         */
/* Emitted under #ifndef TARGET_N64 only.                                   */
/* ------------------------------------------------------------------------ */
#define GE_DLTAG_MAGIC          0x3344u   /* '3D' */
#define GE_DLTAG(x)             ((((unsigned int)GE_DLTAG_MAGIC) << 16) | ((unsigned int)(x) & 0xffffu))
#define GE_DLTAG_PAYLOAD(w1)    ((unsigned int)(w1) & 0xffffu)
#define GE_DLTAG_IS_OURS(w1)    ((((unsigned int)(w1)) >> 16) == (unsigned int)GE_DLTAG_MAGIC)
#define GE_DLCLASS_WORLD        0   /* the full eye offset, convergence and near law */
#define GE_DLCLASS_SKY          1   /* the world's far asymptote, no translation */
#define GE_DLCLASS_GUN          2   /* the viewmodel: own nearest vertex + the near law's shift */
#define GE_DLCLASS_FLAT         3   /* no fold: identical in both eyes, on the panel */
#define GE_DLSPAN_FLAT_BEGIN    8
#define GE_DLSPAN_FLAT_END      9
#define GE_DLSPAN_GUN_BEGIN     10
#define GE_DLSPAN_GUN_END       11
#define GE_DLSPAN_DEPTH_END     12  /* D-064: closes the crosshair's depth span */
/* D-064 (round 56, PD overlay 0035): the CROSSHAIR's view depth. A payload    */
/* >= GE_DLDEPTH_BASE opens a STICKY depth span: every rectangle until         */
/* GE_DLSPAN_DEPTH_END is shifted per eye as if it lay at that view depth.     */
/* Depth = (payload - BASE) / 4 view units; GE_DLDEPTH_FAR = no hit (the sky's */
/* own disparity, the far asymptote).                                          */
#define GE_DLDEPTH_BASE         0x0100u
#define GE_DLDEPTH_FAR          0xffffu
#define GE_DLDEPTH_ENCODE(d)    ((d) <= 0.0f ? GE_DLDEPTH_BASE + 4u : \
                                 (d) * 4.0f >= (float)(GE_DLDEPTH_FAR - GE_DLDEPTH_BASE) ? GE_DLDEPTH_FAR : \
                                 GE_DLDEPTH_BASE + (unsigned int)((d) * 4.0f))
#define gDPGeDlTag(pkt, x) gImmp1(pkt, G_NOOP, GE_DLTAG(x))
#endif /* PORT */

#endif
#if 0 //Rare - so far - didnt seem to use this
#undef gDPLoadTextureBlock
/**
 Override of Load Texture Block without the PipeSync.
 Loads a Texture and sets its tile
 @param   timg: Address of the texture image. E.g. IMAGESEG(IMAGE_SMOKE_0)
 @param    fmt: Texture image format: E.g. G_IM_FMT_IA
 @param    siz: Pixel component size: E.g. G_IM_SIZ_8b
 @param  width: Texture image width
 @param height: Texture image height
 @param    pal: Location of palette for 4-bit color index texture (0 - 15)
 @param    cms: s-axis mirror, no-mirror, wrap, and clamp flags
 @param    cmt: t-axis mirror, no-mirror, wrap, and clamp flags
 @param  masks: s-axis mask (0 - 15 or G_TX_NOMASK)
 @param  maskt: t-axis mask (0 - 15 or G_TX_NOMASK)
 @param shifts: s-coordinate shift value or G_TX_NOLOD
 @param shiftt: t-coordinate shift value or G_TX_NOLOD
 */
#define gDPLoadTextureBlock(pkt, timg, fmt, siz, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt)                               \
    {                                                                                                                                      \
        gDPSetTextureImage(pkt, fmt, siz##_LOAD_BLOCK, 1, timg);                                                                           \
        gDPSetTile(pkt, fmt, siz##_LOAD_BLOCK, 0, 0, G_TX_LOADTILE, 0, cmt, maskt, shiftt, cms, masks, shifts);                            \
        gDPLoadSync(pkt);                                                                                                                  \
        gDPLoadBlock(pkt, G_TX_LOADTILE, 0, 0, (((width) * (height) + siz##_INCR) >> siz##_SHIFT) - 1, CALC_DXT(width, siz##_BYTES));      \
        gDPSetTile(pkt, fmt, siz, (((width)*siz##_LINE_BYTES) + 7) >> 3, 0, G_TX_RENDERTILE, pal, cmt, maskt, shiftt, cms, masks, shifts); \
        gDPSetTileSize(pkt, G_TX_RENDERTILE, 0, 0, ((width)-1) << G_TEXTURE_IMAGE_FRAC, ((height)-1) << G_TEXTURE_IMAGE_FRAC)              \
    }

#undef gsDPLoadTextureBlock
/**
 Override of Load Texture Block (static) without the PipeSync.
 Loads a Texture and sets its tile
 @param   timg: Address of the texture image. E.g. IMAGESEG(IMAGE_SMOKE_0)
 @param    fmt: Texture image format: E.g. G_IM_FMT_IA
 @param    siz: Pixel component size: E.g. G_IM_SIZ_8b
 @param  width: Texture image width
 @param height: Texture image height
 @param    pal: Location of palette for 4-bit color index texture (0 - 15)
 @param    cms: s-axis mirror, no-mirror, wrap, and clamp flags
 @param    cmt: t-axis mirror, no-mirror, wrap, and clamp flags
 @param  masks: s-axis mask (0 - 15 or G_TX_NOMASK)
 @param  maskt: t-axis mask (0 - 15 or G_TX_NOMASK)
 @param shifts: s-coordinate shift value or G_TX_NOLOD
 @param shiftt: t-coordinate shift value or G_TX_NOLOD
 */
#define gsDPLoadTextureBlock(timg, fmt, siz, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt)                             \
                                                                                                                                     \
    gsDPSetTextureImage(fmt, siz##_LOAD_BLOCK, 1, timg),                                                                             \
    gsDPSetTile(fmt, siz##_LOAD_BLOCK, 0, 0, G_TX_LOADTILE, 0, cmt, maskt, shiftt, cms, masks, shifts),                              \
    gsDPLoadSync(),                                                                                                                  \
    gsDPLoadBlock(G_TX_LOADTILE, 0, 0, (((width) * (height) + siz##_INCR) >> siz##_SHIFT) - 1, CALC_DXT(width, siz##_BYTES)),        \
    gsDPSetTile(fmt, siz, ((((width)*siz##_LINE_BYTES) + 7) >> 3), 0, G_TX_RENDERTILE, pal, cmt, maskt, shiftt, cms, masks, shifts), \
    gsDPSetTileSize(G_TX_RENDERTILE, 0, 0, ((width)-1) << G_TEXTURE_IMAGE_FRAC, ((height)-1) << G_TEXTURE_IMAGE_FRAC)




#endif
#endif
