/**
 * Room / model-part cache hooks, SIM-THREAD side (the cache itself lives on the
 * render thread, in port/fast3d/gfx_roomcache.inl).
 *
 * Why a separate side: with [display] render_thread on (automatic when tickhz
 * != 60), frame N's display list runs on the render thread while the sim builds
 * frame N+1. Only the thread running the DL may touch the cache tables, so the
 * game's calls become messages:
 *
 *  - A list REGISTRATION (bg.c: a room's OPA / XLU list; model.c: a part
 *    list) is a G_PORTCACHE_EXT command written into the DL right before the
 *    gSPDisplayList it names. The renderer consumes it at that G_DL in stream
 *    order, so a room is registered on the frame it first draws. It costs one
 *    8-byte command per list per frame, and nothing while room_cache = 0.
 *  - An INVALIDATION (vertices freed or edited, a room unloaded or its lights
 *    shot out, a stage loaded, the mode changed) goes through gfx_pc.cpp's
 *    gfx_roc_queue_op. The render thread applies it at its next frame start,
 *    after every DL that could still reference the old data; synchronous
 *    mode applies it at once.
 *
 * Read when touching: bg.c room render / delete, model.c modelRenderNodeDl /
 * modelRenderNodeGundl, vtxstore.c frees, chr.c per-instance vertex edits,
 * lightfixture.c, lv.c stage load, video.c room_cache.
 */
#include <ultra64.h>
#include <PR/gbi.h>

#define G_PORTCACHE_EXT 0x4a /* port/fast3d/gbiex_compat.h */

enum { ROC_OP_FORGET_VTX = 1, ROC_OP_FORGET_ROOM = 2, ROC_OP_STAGE = 3, ROC_OP_MODE = 4 }; /* gfx_pc.cpp */
extern void gfx_roc_queue_op(int op, int arg, const void *ptr);

/* the sim thread's mirror of [display] room_cache: gates the emitters so a
 * frame built with the cache off carries no markers */
s32 g_PortRoomCacheMode = 0;

/* bg.c: the next G_DL is room `room`'s list (room + 512 = its translucent list) */
Gfx *portRoomCacheEmit(Gfx *gdl, s32 room)
{
    if (g_PortRoomCacheMode == 0 || room < 0 || room >= 1024)
    {
        return gdl;
    }
    gdl->words.w0 = (u32) G_PORTCACHE_EXT << 24;
    gdl->words.w1 = (1u << 16) | (u32) room;
    return gdl + 1;
}

/* model.c: the next G_DL is a part list whose vertices load through segment
 * `vtxseg` (SPSEGMENT_MODEL_VTX for DLCOLLISION parts, SPSEGMENT_MODEL_COL1
 * for gun-DL nodes) */
Gfx *portPartCacheEmit(Gfx *gdl, s32 vtxseg)
{
    /* Gun-DL parts (segment 5: first-person guns, hands, watch) stay on the
     * interpreter because retained copies replayed stale geometry around pause,
     * watch and weapon switches. GE_ROCGUN=1 re-enables them and GE_ROCMODEL=0
     * disables the segment 4 (characters, props) parts, for diagnosis. */
    static s32 s_AllowGun = -1, s_AllowModel = -1;
    if (s_AllowGun < 0)
    {
        extern char *getenv(const char *);
        const char *e = getenv("GE_ROCGUN");
        s_AllowGun = (e != NULL && e[0] == '1');
        e = getenv("GE_ROCMODEL");
        s_AllowModel = !(e != NULL && e[0] == '0');
    }
    if (g_PortRoomCacheMode != 2 || vtxseg < 0 || vtxseg >= 16
        || (vtxseg == 5 && !s_AllowGun) || (vtxseg == 4 && !s_AllowModel))
    {
        return gdl;
    }
    gdl->words.w0 = (u32) G_PORTCACHE_EXT << 24;
    gdl->words.w1 = (2u << 16) | (u32) vtxseg;
    return gdl + 1;
}

/* vtxstore.c / chr.c: the vertex buffer at vtx is freed, replaced or edited
 * in place - every retained part keyed on it is stale */
void portPartCacheVerticesFreed(const void *vtx)
{
    if (g_PortRoomCacheMode == 2 && vtx != NULL)
    {
        gfx_roc_queue_op(ROC_OP_FORGET_VTX, 0, vtx);
    }
}

/* bg.c delete_room_data, lightfixture.c: the room's list or vertex colours change */
void portRoomCacheRoomFreed(s32 room)
{
    if (g_PortRoomCacheMode != 0)
    {
        gfx_roc_queue_op(ROC_OP_FORGET_ROOM, room, NULL);
    }
}

/* lv.c lvlStageLoad: the part table is void (memp reuses the addresses the
 * keys are made of); the front end (0x5A) stays on the interpreter */
void portPartCacheStageLoaded(s32 stage)
{
    if (g_PortRoomCacheMode != 0)
    {
        gfx_roc_queue_op(ROC_OP_STAGE, stage, NULL);
    }
}

/* video.c: [display] room_cache at boot and live - the render side clears its
 * tables and switches mode at its next frame start, this side gates now */
void portRoomCacheSetMode(s32 mode)
{
    if (mode < 0 || mode > 2)
    {
        mode = 0;
    }
    g_PortRoomCacheMode = mode;
    gfx_roc_queue_op(ROC_OP_MODE, mode, NULL);
}
