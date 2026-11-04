#ifndef GBIEX_COMPAT_H
#define GBIEX_COMPAT_H

/*
 * PD-fast3d extension opcodes/bits that GE's gbi.h does not define.
 * Values are verbatim from perfect_dark src/include/gbiex.h (and PD's
 * PR/gbi.h for G_TRI4/G_RDPHALF_CONT). The EXT band 0x21-0x45 does not
 * collide with anything GE emits ({01,03,04,06} u [B1,C0] u [E4,FF] -
 * docs/port/ge-microcode.md), so the interpreter keeps these features.
 *
 * These are only ever emitted by port-side code (video.c / future EXT
 * users), never by game display lists.
 */

/* PD PR/gbi.h immediates (GE's plain-F3D gbi.h lacks both) */
#ifndef G_TRI4
#define G_TRI4                       0xb1 /* (G_IMMFIRST-14); GE emits it, decode is bit-identical */
#endif
#ifndef G_RDPHALF_CONT
#define G_RDPHALF_CONT               0xb2 /* (G_IMMFIRST-13); never emitted by GE */
#endif

/* PD gbiex.h EXT opcode band */
#define G_SETFB_EXT                  0x21
#define G_SETTIMG_FB_EXT             0x23
#define G_INVALTEXCACHE_EXT          0x34
#define G_TEXRECT_WIDE_EXT           0x37
#define G_FILLRECT_WIDE_EXT          0x38
#define G_SETGRAYSCALE_EXT           0x39
#define G_EXTRAGEOMETRYMODE_EXT      0x3a
#define G_SETINTENSITY_EXT           0x40
#define G_COPYFB_EXT                 0x41
#define G_IMAGERECT_EXT              0x42
#define G_RDPFLUSH_EXT               0x43
#define G_CLEAR_DEPTH_EXT            0x44
#define G_SETSUBPIXELOFFSET_EXT      0x45
#define G_DXLU_NEWVIEW_EXT           0x46 /* GE: player world render begins */
#define G_DXLU_WORLDEND_EXT          0x47 /* GE: world done - flush sorted XLU before the gun */
#define G_MIRRORPILLAR_EXT           0x48 /* GE: w1!=0 - mirror this overlay into the wide side bands */
#define G_DXLU_GROUP_EXT             0x49 /* GE: w1=1 begin sort-group (keyed at mean depth), w1=2 begin (drawn last), w1=3 begin room unit (mode 1; off unless GE_XLU_ROOMUNITS=1), w1=0 end */
#define G_CONSOLE2D_EXT              0x4c /* GE: w1 = w<<16 | h - the console's own 2D space (full window width, no 4:3 pin, window resolution); w1 = 0 ends it. 0x4b is the Coop branch's G_SUBSCREEN_EXT */
#define G_PORTCACHE_EXT              0x4a /* GE: the NEXT G_DL is a room list (w1 = 1<<16 | room) or a model part list (w1 = 2<<16 | vertex segment) for the room / part cache (roc_hooks.c) */
/* Midas round 12 (XBLA step 3, docs/port-ios/21): the donor's Tier C / Route B
 * opcodes, emitted only by its bean*.c (none linked before step 4). Same values
 * as include/gbi_extension.h's PORT arm; G_SETDEPTHBIAS_EXT is 0x50, not the
 * donor's 0x49 (that is G_DXLU_GROUP_EXT above, R12-2). */
#define G_SETDEPTHBIAS_EXT           0x50 /* w1 = s16 push away from the eye, in depth steps (glPolygonOffset units) */
#define G_SETTILEOFFSET_EXT          0x4d /* w0 low 3 bits = tile, w1 = s16 s << 16 | s16 t: 1/512 texel subtracted from the tile origin */
#define G_HDMESH_EXT                 0x4e /* w1 = host pointer to a GfxHdMeshDraw (port/include/hdmesh.h) */
#define G_VTXPATCH_EXT               0x51 /* Midas round 21: w1 = host block {n, n x (Vtx *, rgba)}: recolour Bean room Vtx (shot lights, beanstage.c) */
#define G_CHRLIGHT_EXT               0x4f /* w0 low byte 1 + w1 = host GfxChrLight: lit; 0: restore (port/include/chrlight.h) */
#define G_MTX_FLOATS_EXT             0x20 /* gSPMatrix parameter bit: 16 host floats, not S15.16 */

/* PD gbiex.h extra-geometry-mode bits (rsp.extra_geometry_mode) */
#define G_ASPECT_LEFT_EXT            0x00000010
#define G_ASPECT_RIGHT_EXT           0x00000020
#define G_ASPECT_WIDE_EXT            0x00000040
#define G_ASPECT_CENTER_EXT          (G_ASPECT_LEFT_EXT | G_ASPECT_RIGHT_EXT)
#define G_ASPECT_MODE_EXT            (G_ASPECT_CENTER_EXT | G_ASPECT_WIDE_EXT)
#define G_NO_CLIPPING_EXT            0x00000100
#define G_MODULATE_EXT               0x00000200

/* PD gbiex.h texture filter extension (othermode_h TEXTFILT band) */
#define G_TF_BLUR_EXT                (1 << G_MDSFT_TEXTFILT)

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

#endif /* GBIEX_COMPAT_H */
