/*
 * Round 12 (XBLA step 3, docs/port-ios/21-round12-opcodes.md): the dev-only
 * GE_EXT_PROBE harness. macOS video build only (CMakeLists.txt removes this
 * file from the iOS and headless builds; port/fast3d/gfx_pc.cpp has a weak
 * geExtProbeList() returning NULL for them).
 *
 * GE_EXT_PROBE=<variant> (unset or 0: off) builds, on the render thread, a small
 * host display list that fast3d runs over each finished gameplay frame
 * (gfx_run, after the game's own list). It exercises the five port opcodes the
 * donor's bean*.c emit, BEFORE any Bean file is linked:
 *
 *   - a 200x200 quad through G_HDMESH_EXT (a GfxHdMeshDraw over a 4-vertex
 *     GfxHdMesh in hdmesh.h's layout, plus its fallback list G_VTX + 2x G_TRI1
 *     + G_ENDDL, which is what Midas's fast3d draws), textured from a 32x32
 *     RGBA16 stand-in tile loaded with beanstage.c emitTexture()'s exact words;
 *     the stand-in carries a 32x32 picture (an 8x8 checker of 4-texel cells)
 *     registered with
 *     texpackRegisterBeanPicture(), so fast3d uploads the picture at its own
 *     size and marks it exact_uv (GE_EXT_PROBE_NOEXACT=1: the A/B);
 *   - G_MTX_FLOATS_EXT for the projection and for the quad's modelview;
 *   - a coplanar green reference quad (plain G_VTX + G_TRI1, an ordinary
 *     S15.16 G_MTX with the same translation) drawn first, so the depth bias
 *     shows as in front of / behind / tied with it;
 *   - G_SETDEPTHBIAS_EXT, G_SETTILEOFFSET_EXT, G_CHRLIGHT_EXT per variant, each
 *     reset after the quad.
 *
 * Variants: 1 bias 0 (coplanar tie, GL_LESS: the reference mostly wins),
 * 2 bias -8 (pulled toward the eye: in front), 3 bias +8 (pushed away:
 * behind), 4 = 2 + tile offset (s -2, t -1 texels: half and a quarter of a
 * checker cell), 5 = 2 with the float modelview moved (-120, +40), 6 = 2 lit through
 * G_CHRLIGHT_EXT (one red directional light, dim ambient; vertices carry the
 * normal (0,0,127)), 7 = 6 with the light's `on` clear (a no-op: unlit, the
 * normal bytes read as the dark-blue vertex colour, as the donor documents).
 *
 * Never part of a public build; reads game state only through
 * geHostInGameplay() (the sim's, read racily from the render thread, as the
 * shell's own HUD queries do). Writes nothing the sim reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ultra64.h>
#include "hostinput.h"
#include "texpack.h"
#include "hdmesh.h"
#include "chrlight.h"

#ifndef PORT
#error "extprobe.c is 64-bit port code"
#endif

/* fast3d opcodes (port/fast3d/gbiex_compat.h; fast3d's header is C++-side) */
#define PROBE_G_CLEAR_DEPTH_EXT   0x44
#define PROBE_G_SETDEPTHBIAS_EXT  0x50
#define PROBE_G_SETTILEOFFSET_EXT 0x4d
#define PROBE_G_HDMESH_EXT        0x4e
#define PROBE_G_CHRLIGHT_EXT      0x4f
#define PROBE_G_MTX_FLOATS_EXT    0x20

#define TILE 32
#define PIC  32  /* the picture: 32x32, an 8x8 checker of 4-texel cells */
#define CELL 4

static u16 standin[TILE * TILE];      /* the stand-in tile's texels (never seen: the picture replaces them) */
static Vtx quadVtx[4], quadVtxLit[4], refVtx[4], afterVtx[4];
static u16 quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
static struct GfxHdMesh mesh, meshLit;
static struct GfxHdMeshDraw meshDraw, meshDrawLit;
static Gfx fallback[8], fallbackLit[8];
static float projF[4][4], mvF[4][4];
static s32 mvFix[16];
static Vp viewport;
static GfxChrLight light;
static Gfx list[160];
static int variant = -1;

static void emit(Gfx **g, uintptr_t w0, uintptr_t w1)
{
    (*g)->words.w0 = w0;
    (*g)->words.w1 = w1;
    (*g)++;
}

static void setVtx(Vtx *v, s16 x, s16 y, s16 s, s16 t, u8 r, u8 g, u8 b, u8 a)
{
    v->v.ob[0] = x;
    v->v.ob[1] = y;
    v->v.ob[2] = 0;
    v->v.flag = 0;
    v->v.tc[0] = s;
    v->v.tc[1] = t;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = a;
}

/* an N64 S15.16 matrix (the layout gfx_sp_matrix decodes): integer halves in
 * words 0..7, fractions in 8..15, two elements per word */
static void toFixed(s32 out[16], const float m[4][4])
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            const s32 a = (s32)(m[i][j] * 65536.0f);
            const s32 b = (s32)(m[i][j + 1] * 65536.0f);
            out[i * 2 + j / 2] = (s32)(((u32)a & 0xffff0000u) | ((u32)b >> 16));
            out[8 + i * 2 + j / 2] = (s32)(((u32)a << 16) | ((u32)b & 0xffffu));
        }
    }
}

static void identity(float m[4][4])
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] = i == j ? 1.0f : 0.0f;
        }
    }
}

static void buildOnce(void)
{
    /* the stand-in: a flat magenta, so a missing picture would be obvious */
    for (int i = 0; i < TILE * TILE; i++) {
        standin[i] = 0xf83f;
    }
    /* the picture: 32x32 RGBA8 (one texel per stand-in texel, so exact_uv's
     * half texel is half a picture texel), an 8x8 checker of 4-texel cells */
    u8 *rgba = (u8 *)malloc(PIC * PIC * 4);
    if (rgba) {
        for (int y = 0; y < PIC; y++) {
            for (int x = 0; x < PIC; x++) {
                u8 *p = rgba + (y * PIC + x) * 4;
                const int dark = ((x / CELL) + (y / CELL)) & 1;
                p[0] = dark ? 30 : 240;
                p[1] = dark ? 30 : 240;
                p[2] = dark ? 140 : 240;
                p[3] = 255;
            }
        }
        texpackRegisterBeanPicture(standin, rgba, PIC, PIC);
    }

    /* the HD quad: 200x200 units, S10.5 UVs spanning the 32x32 stand-in once */
    const s16 uv = TILE << 5;
    setVtx(&quadVtx[0], -100, -100, 0, uv, 255, 255, 255, 255);
    setVtx(&quadVtx[1], 100, -100, uv, uv, 255, 255, 255, 255);
    setVtx(&quadVtx[2], 100, 100, uv, 0, 255, 255, 255, 255);
    setVtx(&quadVtx[3], -100, 100, 0, 0, 255, 255, 255, 255);
    for (int i = 0; i < 4; i++) {
        quadVtxLit[i] = quadVtx[i];
        quadVtxLit[i].n.n[0] = 0;   /* Vtx_tn: the normal where the colour was */
        quadVtxLit[i].n.n[1] = 0;
        quadVtxLit[i].n.n[2] = 127;
        quadVtxLit[i].n.a = 255;
    }
    /* the reference: green, coplanar, over the HD quad's right half */
    setVtx(&refVtx[0], 0, -60, 0, 0, 40, 200, 60, 255);
    setVtx(&refVtx[1], 180, -60, 0, 0, 40, 200, 60, 255);
    setVtx(&refVtx[2], 180, 60, 0, 0, 40, 200, 60, 255);
    setVtx(&refVtx[3], 0, 60, 0, 0, 40, 200, 60, 255);
    /* drawn AFTER the HD quad, unlit, apart from it: proves G_CHRLIGHT_EXT's
     * restore (still lit, its colour bytes would be read as a normal) */
    setVtx(&afterVtx[0], 120, -140, 0, 0, 40, 200, 60, 255);
    setVtx(&afterVtx[1], 170, -140, 0, 0, 40, 200, 60, 255);
    setVtx(&afterVtx[2], 170, -90, 0, 0, 40, 200, 60, 255);
    setVtx(&afterVtx[3], 120, -90, 0, 0, 40, 200, 60, 255);

    for (int k = 0; k < 2; k++) {
        struct GfxHdMesh *m = k ? &meshLit : &mesh;
        struct GfxHdMeshDraw *d = k ? &meshDrawLit : &meshDraw;
        Gfx *f = k ? fallbackLit : fallback;
        m->vtx = k ? quadVtxLit : quadVtx;
        m->idx = quadIdx;
        m->numvtx = 4;
        m->numidx = 6;
        m->idx32 = 0;
        d->mesh = m;
        d->first = 0;
        d->count = 6;
        d->fallback = f;
        /* what beanstage.c's fallback holds: the run as G_VTX + triangles + G_ENDDL */
        gSPVertex(f++, (Vtx *)(k ? quadVtxLit : quadVtx), 4, 0);
        gSP1Triangle(f++, 0, 1, 2, 0);
        gSP1Triangle(f++, 0, 2, 3, 0);
        gSPEndDisplayList(f++);
    }

    /* projection (row vectors, as fast3d multiplies): fovy 60, 4:3, near 10, far 10000 */
    {
        const float fy = 1.7320508f, n = 10.0f, fa = 10000.0f;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                projF[i][j] = 0.0f;
            }
        }
        projF[0][0] = fy / (4.0f / 3.0f);
        projF[1][1] = fy;
        projF[2][2] = (n + fa) / (n - fa);
        projF[2][3] = -1.0f;
        projF[3][2] = 2.0f * n * fa / (n - fa);
    }
    {
        float m[4][4];
        identity(m);
        m[3][2] = -400.0f;
        toFixed(mvFix, m);
    }
    viewport.vp.vscale[0] = 320 * 2;
    viewport.vp.vscale[1] = 240 * 2;
    viewport.vp.vscale[2] = G_MAXZ / 2;
    viewport.vp.vscale[3] = 0;
    viewport.vp.vtrans[0] = 320 * 2;
    viewport.vp.vtrans[1] = 240 * 2;
    viewport.vp.vtrans[2] = G_MAXZ / 2;
    viewport.vp.vtrans[3] = 0;

    /* one red directional light from the viewer, dim ambient */
    light.on = 1;
    light.numdir = 1;
    light.dir[0].col[0] = light.dir[0].colc[0] = 255;
    light.dir[0].col[1] = light.dir[0].colc[1] = 70;
    light.dir[0].col[2] = light.dir[0].colc[2] = 70;
    light.dir[0].dir[0] = 0;
    light.dir[0].dir[1] = 0;
    light.dir[0].dir[2] = 127;
    light.amb.col[0] = light.amb.colc[0] = 30;
    light.amb.col[1] = light.amb.colc[1] = 30;
    light.amb.col[2] = light.amb.colc[2] = 30;
}

/* beanstage.c emitTexture(): gDPLoadTextureBlock of a 32x32 RGBA16, wrap, mask 5 */
static void emitTexture(Gfx **g, const u16 *tile)
{
    emit(g, 0xba000e02, 0x00000000);             /* G_TT_NONE */
    emit(g, 0xfd100000, (uintptr_t)tile);        /* SETTIMG RGBA 16b */
    emit(g, 0xf5100000, 0x07014050);             /* SETTILE load tile, wrap, mask 5 */
    emit(g, 0xe6000000, 0x00000000);             /* LOADSYNC */
    emit(g, 0xf3000000, 0x073ff100);             /* LOADBLOCK 1024 texels, dxt 256 */
    emit(g, 0xe7000000, 0x00000000);             /* PIPESYNC */
    emit(g, 0xf5101000, 0x00014050);             /* SETTILE render tile, line 8 */
    emit(g, 0xf2000000, 0x0007c07c);             /* SETTILESIZE 32x32 */
}

static void buildList(void)
{
    Gfx *g = list;
    const int lit = variant == 6 || variant == 7;
    const s16 bias = variant == 1 ? 0 : variant == 3 ? 8 : -8;

    light.on = variant == 7 ? 0 : 1;
    identity(mvF);
    mvF[3][2] = -400.0f;
    if (variant == 5) {
        mvF[3][0] = -120.0f;
        mvF[3][1] = 40.0f;
    }

    gDPPipeSync(g++);
    emit(&g, (uintptr_t)PROBE_G_CLEAR_DEPTH_EXT << 24, 0);
    gSPViewport(g++, &viewport);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetTexturePersp(g++, G_TP_PERSP);
    gDPSetTextureLOD(g++, G_TL_TILE);
    gDPSetTextureDetail(g++, G_TD_CLAMP);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gDPSetTextureFilter(g++, G_TF_BILERP);
    gDPSetRenderMode(g++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
    gSPClearGeometryMode(g++, 0xffffffff);
    gSPSetGeometryMode(g++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    gSPMatrix(g++, (Mtx *)projF, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH | PROBE_G_MTX_FLOATS_EXT);

    /* the reference quad: an ordinary S15.16 modelview, untextured */
    gSPMatrix(g++, (Mtx *)mvFix, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPTexture(g++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPVertex(g++, refVtx, 4, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    gSP1Triangle(g++, 0, 2, 3, 0);
    gDPPipeSync(g++);

    /* the HD quad: a host float modelview, the stand-in, the opcodes */
    gSPMatrix(g++, (Mtx *)mvF, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH | PROBE_G_MTX_FLOATS_EXT);
    emit(&g, 0xbb003001, 0xffffffff);            /* G_TEXTURE on, scale 1 (beanstage.c) */
    gDPSetCombineMode(g++, G_CC_MODULATERGB, G_CC_MODULATERGB);
    emitTexture(&g, standin);
    if (variant == 4) {
        const u32 s = (u32)(-2 * 512) & 0xffffu, t = (u32)(-1 * 512) & 0xffffu;
        emit(&g, (uintptr_t)PROBE_G_SETTILEOFFSET_EXT << 24 | 0, (uintptr_t)(s << 16 | t));
    }
    emit(&g, (uintptr_t)PROBE_G_SETDEPTHBIAS_EXT << 24, (uintptr_t)(u32)(s32)bias);
    if (lit) {
        emit(&g, (uintptr_t)PROBE_G_CHRLIGHT_EXT << 24 | 1, (uintptr_t)&light);
    }
    emit(&g, (uintptr_t)PROBE_G_HDMESH_EXT << 24, (uintptr_t)(lit ? &meshDrawLit : &meshDraw));
    if (lit) {
        emit(&g, (uintptr_t)PROBE_G_CHRLIGHT_EXT << 24, 0);
    }
    emit(&g, (uintptr_t)PROBE_G_SETDEPTHBIAS_EXT << 24, 0);
    emit(&g, (uintptr_t)PROBE_G_SETTILEOFFSET_EXT << 24 | 0, 0);
    gDPPipeSync(g++);
    gSPTexture(g++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPMatrix(g++, (Mtx *)mvFix, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPVertex(g++, afterVtx, 4, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    gSP1Triangle(g++, 0, 2, 3, 0);
    gSPEndDisplayList(g++);
    if ((size_t)(g - list) > sizeof(list) / sizeof(list[0])) {
        fprintf(stderr, "extprobe: list overflow\n");
        abort();
    }
}

const void *geExtProbeList(void)
{
    static int logged = 0;
    if (variant < 0) {
        const char *e = getenv("GE_EXT_PROBE");
        variant = e ? atoi(e) : 0;
        if (variant < 0 || variant > 7) {
            variant = 0;
        }
        if (variant > 0) {
            buildOnce();
        }
    }
    if (variant == 0 || !geHostInGameplay()) {
        return NULL;
    }
    buildList();
    if (!logged) {
        logged = 1;
        printf("extprobe: variant %d drawing (G_HDMESH_EXT quad, stand-in %p, picture %dx%d)\n",
               variant, (void *)standin, PIC, PIC);
        fflush(stdout);
    }
    return list;
}
