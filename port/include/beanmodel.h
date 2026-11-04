/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/beanmodel.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_BEANMODEL_H
#define PORT_BEANMODEL_H

/*
 * Tier C round C3 (DECISIONS D-117..D-119, docs/tier-c.md "What C3 built"):
 * GoldenEye XBLA's ("Bean") HD props drawn in place of GoldenEye's own prop
 * models, at render time only.
 *
 * A prop model file (P*Z) named in beanproptable.h is built once per level,
 * on the game thread, the first time a level frame sees it: Bean's
 * files/new/prop/<name> rendergraph (beanmesh.c) is placed in the model's
 * space by the fitted scale and centre (scripts/beanfit/propfit.py), each
 * triangle is dealt to the display-list node whose own vertices are nearest
 * (so a wheel or a door leaf rides its own matrix), and every node that got
 * triangles is written as host {Vtx[], Gfx solid[], Gfx cut[], Gfx xlu[]}.
 * modelRenderNodeDl() draws that instead of the node's own list; the model's
 * other plain lists are not drawn; lists under a switch keep GoldenEye's own
 * (screens, toggled parts). The model data the sim reads (hit boxes, bounds,
 * collision vertices, rwdata) is never touched; all memory is host malloc.
 *
 * Key: Video.XblaModels (the C1 key, live). Dev env: GE_BEANPROPS=0 (no prop
 * swap), GE_BEANPROPLOG=1 (a line per build), GE_BEANPROPONLY=<Pname,...>.
 */

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ModelFileHeader;
struct ModelNode;

/* What modelRenderNodeDl() does with a display-list node. */
#define BEANMODEL_N64   0   /* draw GoldenEye's own list */
#define BEANMODEL_DRAW  1   /* draw *out instead */
#define BEANMODEL_SKIP  2   /* draw nothing: the HD mesh covers it */

struct beanmodelnode {
    Vtx *vtx;          /* segment SPSEGMENT_MODEL_VTX */
    Gfx *solid;        /* opaque pictures; NULL if none */
    Gfx *cut;          /* cut-outs (TEX_EDGE), after the solid */
    Gfx *xlu;          /* glass and soft pictures, the translucent pass */
};

s32 beanModelNode(struct ModelFileHeader *hdr, struct ModelNode *node, const struct beanmodelnode **out);

/* bondview2.c (round 28b, D-177): a player's held item header copied out of
 * PitemZ_entries for the intro swirl - drawn as the file it is a copy of. */
void beanModelAlias(struct ModelFileHeader *copy, struct ModelFileHeader *original);

/* bg.c load_bg_file(): a new level; the last level's builds are retired. */
void beanModelLevelLoaded(void);
/* bg.c bgLevelRender(), once a frame: builds every live prop's model once. */
void beanModelFrame(void);

/* round 35a: microseconds the game thread has spent in HD prop, gun and
 * character builds since boot (GE_FRAMEPROF's build_us is its per-frame
 * delta). Written on the game thread only. */
extern volatile u64 g_beanBuildUs;

/* Round 39 (doc 52): GoldenEye draws the first-person guns with no depth
 * buffer (gunRenderFirstPersonGunModels, zbufferenabled 0): its own lists are
 * culled and ordered for that. The release's HD gun and hand are drawn both
 * faces in picture order, so with no depth test the far side of the hand
 * painted over its near side (grey-brown triangles on Bond's hand). Between
 * beanModelGunPass(1) and beanModelGunPass(0) an HD list is drawn with the
 * depth buffer instead: beanModelGunDepth() answers 0 outside the pass (or
 * with GE_BEANGUNDEPTH=0), 1 inside it, and 2 for the pass's first HD list,
 * before which the caller clears the depth buffer (the world's depth would
 * hide a gun held against a wall). A pass that draws no HD list emits nothing. */
void beanModelGunPass(s32 on);
s32 beanModelGunDepth(void);

/* Host bytes of the built meshes (lists, vertices, stand-ins) and of their
 * pictures; models built / fallen back. */
void beanModelMemory(u32 *meshes, u32 *pictures, s32 *built, s32 *fellback);

#ifdef __cplusplus
}
#endif

#endif
