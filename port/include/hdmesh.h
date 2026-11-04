/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/hdmesh.h (55a3a9c8;
 * last changed f7ccae3a), Midas round 12 (XBLA step 3, docs/port-ios/21). Round 29
 * (docs/port-ios/39) ports the GPU route Midas-side: gfx_hdmesh_gpu below. */
/*
 * Route B (Tier C, round "Route B", D-120): G_HDMESH_EXT -- a built Bean
 * room's triangles drawn from a static GPU mesh instead of G_VTX + G_TRI4.
 *
 * beanstage.c (game thread) owns the host data: the room's Vtx[] (GoldenEye's
 * 16-byte layout, exactly what its G_VTX loads read) and an index list made at
 * build from the very G_TRI4 words the room's list carries. fast3d (render
 * thread) uploads both once, at the first draw, as a static VBO {float pos[3],
 * float tc[2], u8 rgba[4]} + IBO (16-bit when the room has <= 65536 vertices)
 * and draws a run of them with one indexed draw through a shader that does on
 * the GPU what gfx_sp_vertex() does on the CPU for an unlit vertex. Same
 * per-vertex data, same state selection (gfx_draw_state_prepare()): same pixels.
 *
 * The command (gbi_extension.h G_HDMESH_EXT):
 *   w0 = G_HDMESH_EXT << 24, w1 = host pointer to a struct GfxHdMeshDraw.
 * A draw whose state the GPU path does not reproduce (lighting, texture
 * generation, Video.WrapFix pre-wrap) runs `fallback` instead: the same run as
 * G_VTX + G_TRI4 ending in G_ENDDL, i.e. the C2 path for that run.
 *
 * Lifetime: GL names are written on the render thread only. gfxHdMeshRelease()
 * may be called from any thread once nothing can draw the mesh again (the
 * retired set, one level later); it queues the names, and the render thread
 * deletes them at its next frame. The struct itself is the caller's to free
 * after the call.
 */
#ifndef PORT_HDMESH_H
#define PORT_HDMESH_H

#ifdef __cplusplus
extern "C" {
#endif

struct GfxHdMesh {
    const void *vtx;       /* Vtx[numvtx], host */
    const void *idx;       /* unsigned short or unsigned int [numidx], host */
    unsigned int numvtx;
    unsigned int numidx;
    unsigned int idx32;    /* 1: 32-bit indices */
    /* render thread only */
    unsigned int vao, vbo, ibo;
    int state;             /* 0 not uploaded, 1 live, -1 upload failed (always the fallback) */
};

struct GfxHdMeshDraw {
    struct GfxHdMesh *mesh;
    unsigned int first;    /* first index */
    unsigned int count;    /* indices (3 per triangle) */
    const void *fallback;  /* Gfx[]: the same run as G_VTX + G_TRI4, then G_ENDDL */
};

/* any thread: upload the mesh at the render thread's next frame start instead of
 * at its first draw (a level's meshes are queued as it is built, so walking into
 * a room never pays the glBufferData; Dam: 117 meshes, ~5 MB, one frame) */
void gfxHdMeshQueueUpload(struct GfxHdMesh *m);

/* any thread: the mesh's GL objects are deleted at the render thread's next
 * frame (and a still-queued upload is dropped) */
void gfxHdMeshRelease(struct GfxHdMesh *m);

/* fast3d: 0 = every G_HDMESH_EXT runs its fallback (dev GE_HDMESH_CPU=1: the
 * mesh lists' shape on the CPU path, to A/B the shader inside one list) */
extern int gfx_hdmesh_enabled;

/* Midas round 29 (docs/port-ios/39): 1 = a G_HDMESH_EXT draws its mesh range on
 * the GPU (gfx_pc.cpp gfx_hdmesh, gfx_hdgpu.h) whenever the GPU path reproduces
 * the state, else its fallback; 0 = always the fallback (the round 19/20 CPU
 * replay). Video.XblaMeshGpu (beanstage.c registers it); default 1 on iOS, 0
 * elsewhere; GE_HDMESH_GPU=0/1 overrides. Live. */
extern int gfx_hdmesh_gpu;

#ifdef __cplusplus
}
#endif

#endif
