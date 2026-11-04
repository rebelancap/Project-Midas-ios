/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/beanchr.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_BEANCHR_H
#define PORT_BEANCHR_H

/*
 * Tier C round 26b (C4 part 2, DECISIONS D-150..D-153, docs/tier-c.md
 * "Round 26b: characters"): GoldenEye XBLA's ("Bean") HD character bodies and
 * heads drawn on GoldenEye's own character models, on GoldenEye's own skeleton
 * and animation, at render time only.
 *
 * A body (C*Z named in beanchrtable.h) is built once per level from
 * files/new/char/<name>: Bean's skeleton is fitted onto the model's star rest
 * pose (PD gebean.c beanRigFromModel/beanFitRig/beanFitPalette), each vertex
 * is placed in the model's rest by its Bean weights and given to its heaviest
 * joint's matrix, and each display-list node of a joint gets the triangles
 * that joint dominates. A list loads each vertex batch under its own joint's
 * matrix (segment 3, as GoldenEye's own lists do), so a triangle can span two
 * joints - the N64's own way of bending a limb. A head is built per (head,
 * body) pair: seated where the body's fit put Bean's neck, rigid on the neck
 * (and the back, for a head file's neck reaching into the collar).
 *
 * A character the layer cannot serve whole (body and head) stays GoldenEye's
 * own whole. Nothing the sim reads is touched (hit parts, bounds, LOD choice,
 * rwdata); all memory is host malloc.
 */

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Model;
struct ModelNode;
struct beanmodelnode;

/* What modelRenderNodeBean() does with a character's node: BEANMODEL_N64
 * (draw GoldenEye's own, or not a served character), BEANMODEL_DRAW (*out),
 * BEANMODEL_SKIP. */
s32 beanChrNode(struct Model *model, struct ModelNode *node, const struct beanmodelnode **out);

/* bg.c: a new level (the last level's builds retired), and once a frame
 * before beanModelFrame() (builds every live character once a level). */
void beanChrLevelLoaded(void);
void beanChrFrame(void);

/* bondview2.c: Bond's solo body/head header copy and the file it is of. */
struct ModelFileHeader;
void beanChrAlias(struct ModelFileHeader *copy, struct ModelFileHeader *original);

/* The characters' part of the [xbla] stage line. */
void beanChrDescribe(char *buf, u32 len, s32 *unresolved);

#ifdef __cplusplus
}
#endif

#endif
