/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/beanstage.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_BEANSTAGE_H
#define PORT_BEANSTAGE_H

/*
 * Tier C (round C1, DECISIONS D-110, docs/tier-c.md): GoldenEye XBLA's HD
 * level geometry drawn in place of GoldenEye's own rooms, at render time only.
 *
 * At a level's first frame the Bean level file (files/new/background/<name>)
 * is read (beanmesh.c), GoldenEye's own rooms are read raw from the ROM into
 * HOST scratch, Bean's triangles are dealt to the room whose own triangle is
 * nearest their middle (Perfect Dark's gebeanstage.c), and the served room is
 * written back as a host {Vtx[], Gfx opa[], Gfx xlu[]} in GoldenEye's own
 * room display-list form. bgRenderRoomPrimary/Secondary emit that instead of
 * the room's own list when beanStageRoomServed(); nothing the sim reads
 * (room loads, bullet hits, bounds, portals, mema/memp) is touched.
 *
 * Keys: Video.XblaModels (default 1, live). Dev env: GE_BEANROOM=<n> serves
 * room n instead of Bond's spawn room; GE_BEANSTAGE_ANY=1 builds an
 * unverified level row (beanstagetable.h).
 */

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bg.c load_bg_file(): a new level's bg is in; forget the last level's build. */
void beanStageLevelLoaded(void);
/* D-275: delete $C/xbla/levels caches of another builder version (startup) */
void beanStageCachePrune(void);
/* Midas round 22 (R16-2): the XBLA layer on disk changed (an import finished,
 * failed after clearing the old layer, or was removed). Any thread; the level
 * in play is rebuilt - rooms, props, characters, as a level load does - at the
 * top of a later beanStageFrame() (the sim thread's display-list build). */
void beanStageLayerChanged(void);

/* bg.c bgLevelRender(), once a frame before any room is drawn: builds on the
 * first frame Bond is placed (and the key is on and the level is paired). */
void beanStageFrame(void);

/* Whether room `room` is drawn from Bean's mesh this frame. */
int beanStageRoomServed(s32 room);

/* An unserved N64 room not drawn while every room is (GoldenEye's own far
 * cliffs, which would stand over Bean's mountains). */
int beanStageRoomHidden(s32 room);

/* The room's vertex segment and its opaque (xlu 0) or translucent (xlu 1)
 * list; the caller has already loaded the room matrix. */
Gfx *beanStageRenderRoom(Gfx *gdl, s32 room, int xlu);

/* Round C2 (D-112): every served room by distance, render only
 * (Video.XblaEveryRoom). bg.c calls the opaque pass before GoldenEye's room
 * loop and the translucent one before its secondary loop; while it draws,
 * beanStageRenderRoom() emits nothing (the portal pass still loads rooms). */
int beanStageDrawsEveryRoom(void);
/* bondviewUpdateCameraMatrices(): the eye, world units */
void beanStageSetEye(f32 x, f32 y, f32 z);
/* ... and the rooms' projection-view (field_10E0's float source, row vectors,
 * coordinates relative to current_model_pos times the visibility scale) */
void beanStageSetViewProj(f32 m[4][4]);
Gfx *beanStageRenderEveryRoom(Gfx *gdl, int xlu);

/* The level's backdrop (Dam's far mountains), before the rooms. */
Gfx *beanStageRenderBackdrop(Gfx *gdl);
int beanStageHasBackdrop(void);

/* Host bytes of the built rooms (lists, vertices, stand-ins, matrices) and of
 * the pinned pictures. */
void beanStageMemory(u32 *rooms, u32 *pictures);

/* Video.XblaModels. */
int beanStageModelsEnabled(void);
void beanStageSetModelsEnabled(int on);

/* One line for Settings -> Textures and the bridge (round 22 merge, D-136):
 * "off (N64 rooms)", "117 rooms from XBLA, every room", ... */
void beanStageDescribe(char *buf, int len);

/*
 * Round 25b (D-142): this level's XBLA coverage, filled by the build. state 1
 * built, -1 not served (`why`), 0 not tried yet. Draws by BEANDRAW_* kind
 * (beanmesh.h); pictures = distinct pictures the kept triangles use, decoded =
 * those that decoded; a room holding an unresolved triangle is left N64.
 * shaderTris: plain white triangles of a draw with no material (the release's
 * shader colours them), still drawn; shaderDecals: those dropped as decals.
 * Returns whether the build has been tried this level.
 */
struct beanstageaudit {
    int state;
    const char *why;
    char key[16];
    char bean[24];
    int rooms, served;
    int draws, pictureDraws, colourDraws, badpicDraws, unreadableDraws;
    int pictures, decoded;
    int unresolvedRooms, unresolvedTris;
    int shaderTris, shaderDecals;
};
int beanStageAudit(struct beanstageaudit *out);

#ifdef __cplusplus
}
#endif

#endif
