/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/floatvtx.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_FLOATVTX_H
#define PORT_FLOATVTX_H

/* D-189 (round 28c, adapted from upstream f242992, their D245 M-201):
 * a port-only full-precision vertex load for the sky and sky-water fans.
 *
 * On the N64, sky.c builds these triangles directly as RDP edge/texture
 * coefficients with 32-bit S/T. The port routes them through gSPVertex, so
 * both had to be squeezed into s16: positions as ndc * w / wScale (wScale =
 * the fan's max |w| / 30000 -- a Frigate water fan spans w 861..228470, so a
 * near vertex keeps ~113 steps across the whole NDC range, ~4 px at 960
 * wide), and S/T as tc >> k (the D227 safety valve, k = 4 on that fan: half
 * a texel). G_FLOATVTX_EXT hands fast3d the clip-space position and S/T as
 * floats; fast3d already stores every loaded vertex as floats.
 *
 * w0 = G_FLOATVTX_EXT << 24 | dest_index << 16 | count, w1 = PortFloatVtx *
 * (a host pointer into the dyn pool). s/t are in Vtx.tc units (S10.5, before
 * gSPTexture scaling). Unlit, unfogged, unculled only -- the sky's state. */
#define G_FLOATVTX_EXT 0x47

typedef struct PortFloatVtx {
    float x, y, z, w;   /* clip space */
    float s, t;         /* tc units */
    unsigned char r, g, b, a;
} PortFloatVtx;

#define gSPFloatVertexExt(pkt, ptr, n, v0)                                         \
    do {                                                                           \
        Gfx *_g = (Gfx *)(pkt);                                                    \
        _g->words.w0 = ((uintptr_t)G_FLOATVTX_EXT << 24) |                         \
                       (((uintptr_t)(v0) & 0xff) << 16) | ((uintptr_t)(n) & 0xffff); \
        _g->words.w1 = (uintptr_t)(ptr);                                           \
    } while (0)

#endif
