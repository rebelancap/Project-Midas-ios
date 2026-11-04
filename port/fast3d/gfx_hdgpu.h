/* gfx_hdgpu.h - Midas round 29 (docs/port-ios/39-round29-hdmesh-gpu.md):
 * R12-1 option (a), the GPU route for G_HDMESH_EXT (the sibling's Route B,
 * GoldenEye-Decomp-ios D-120..D-123, re-derived for MIDAS's per-vertex path).
 *
 * gfx_pc.cpp (gfx_hdmesh) sets up the draw state through the interpreter's own
 * hoist (gfx_sp_tri1_v, state only), fills this block from it with the same
 * source text the CPU emission uses (gfx_tri_consts.inl), and hands it to
 * gfx_opengl.cpp, which draws the mesh range with glDrawElements through an
 * "HD twin" of the combiner's program: the same fragment shader object, and a
 * vertex shader that computes on the GPU what gfx_vtx_pos + gfx_sp_vertex's
 * unlit half + gfx_tri_emit.inl compute per vertex on the CPU. Its outputs are
 * the stream program's varyings, so the fragment side cannot tell.
 *
 * The uniform contract (every field is per draw; the shader text names each):
 *   mp      rsp.MP_matrix, rows (row-vector convention: x = ob . column 0 + row 3)
 *   asp     (k, ofs, yflip, z01): x' = (ofs * w + x) * k  (gfx_adjust_x_for_aspect_ratio;
 *           k = 1, ofs = 0 under fbActive), y' = y * yflip (invert_y), z01 = 1 when the
 *           API's depth is 0..1 (z = (z + w) / 2)
 *   fog     (fog_mul, fog_offset, G_FOG ? 1 : 0, rdp.fog_color.a): gfx_vtx_pos's fog byte
 *   fogrgb  the blender's fog colour (ge_b2f), alpha unused
 *   pixfog  (fog_mul, fog_offset) as the stream carries them (aGeFogP), 0 0 unused
 *   dec     (on, b, k, 4k): ZMODE_DEC's pull z += b*k / (w - k) for w > 4k
 *   tsf     (texture_scaling_factor.s, .t, 0, 0): U = (short)(tc * tsf >> 16)
 *   uva[t]  (su, sv, bu, bv) and uvb[t] (p, c, 1/width, 1/height): texcoord =
 *           ((U * su - bu) * p + c) * (1/width), bu/bv include G_SETTILEOFFSET_EXT,
 *           c is 0 for exact_uv pictures (gfx_tri_consts.inl)
 *   clampst[t] (cs, ct): the in-shader clamp's constants
 *   gray    the grayscale colour + its lerp factor
 *   in_kind[j] (rgb kind, alpha kind) and in_const[j] (rgba): combiner input j's
 *           source - 0 constant, 1 the vertex colour, 2 the vertex alpha (rgb = a,
 *           alpha = 0), 3 the N64 LOD fraction from w (gfx_tri_emit.inl's hack: only
 *           when the RDP detail fraction is not in use, else the CPU path)
 *   cull    0 none, 1 front, 2 back (the GPU culls; G_CULL_BOTH draws nothing)
 */
#ifndef GFX_HDGPU_H
#define GFX_HDGPU_H

#include <stdint.h>
#include <stdbool.h>

struct GfxHdMesh;
struct ShaderProgram;

struct GfxHdgUniforms {
    float mp[4][4];
    float asp[4];
    float fog[4];
    float fogrgb[4];
    float pixfog[4];
    float dec[4];
    float tsf[4];
    float uva[2][4];
    float uvb[2][4];
    float clampst[2][4];
    float gray[4];
    int32_t in_kind[8][2];
    float in_const[8][4];
    int32_t cull;
    int32_t invert_y;
    /* Phase 6 M3 (visionOS 3D, PD D-061): the world near clamp (k, q0, qm, dq),
     * all zero unless a WORLD-classed fold armed it; read only by the
     * GE_VISION_3D shader text (uHdStereo), so iOS and macOS ignore it */
    float stereo[4];
};

#ifdef __cplusplus
extern "C" {
#endif

/* gfx_opengl.cpp, render thread: draw `count` indices from `first` of the mesh
 * through prg's HD twin. false = not drawn (no GL support, the mesh's upload or
 * the twin's compile failed): the caller runs the fallback. */
bool gfx_opengl_hdg_draw(struct ShaderProgram* prg, const struct GfxHdgUniforms* u, struct GfxHdMesh* m,
                         uint32_t first, uint32_t count);
/* any thread: queue the mesh's upload / its GL objects' deletion for the render
 * thread's next frame start (hdmesh.h's contract) */
void gfx_opengl_hdg_queue_upload(struct GfxHdMesh* m);
void gfx_opengl_hdg_release(struct GfxHdMesh* m);
/* render thread: G_VTXPATCH_EXT recoloured the host Vtx at `vtx` - the uploaded
 * mesh that holds it gets the new colour bytes before its next draw */
void gfx_opengl_hdg_patch(const void* vtx, uint32_t rgba);
/* counters for the GE_HDMSTATS line (cumulative, render thread) */
struct GfxHdgGlStats {
    uint32_t live, uploads, upload_kb, releases, twins, twin_fail, patched, upload_fail;
};
void gfx_opengl_hdg_stats(struct GfxHdgGlStats* s);
/* render thread, the shader prewarm walk: build prg's HD twin ahead of its first
 * draw (true = built or already there) */
bool gfx_opengl_hdg_prewarm(struct ShaderProgram* prg);
/* a prewarm-list entry whose id1 carries this bit names the HD twin of the
 * program (id0, id1 without it) - gfx_cc.h leaves bits 28-31 of id1 to the
 * port and uses 28-30 */
#define GFX_HDG_LIST_BIT 0x80000000u

#ifdef __cplusplus
}
#endif

#endif
