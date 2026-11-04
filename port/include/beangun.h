/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/beangun.h (first 6628f24c, last changed 8ec10cfd; donor HEAD 55a3a9c8),
 * round 14, XBLA step 4.3 (docs/port-ios/23-round14-props-guns.md). 64-bit host layer only. */
#ifndef PORT_BEANGUN_H
#define PORT_BEANGUN_H

/*
 * Tier C round 26a (C4 part 1, DECISIONS D-144..D-146, docs/tier-c.md
 * "What C4 guns built"): GoldenEye XBLA's ("Bean") HD first-person guns drawn
 * on GoldenEye's own first-person gun models (G*Z), render only.
 *
 * beanmodel.c builds a gun the way it builds a prop (C3): the release's
 * files/new/gun/<name> rendergraph placed in the model's space by the fitted
 * numbers (beanguntable.h, scripts/beanfit/gunfit.py: Bean's own 1/4.7 and the
 * origin within half a unit), each triangle dealt to a display-list node, each
 * node written as host lists that model.c draws in place of the node's own.
 * What is a gun's own lives here:
 *
 *  - which file a header holds: a player's hand header is a copy refilled in
 *    place for every gun drawn in that hand (gun.c gunBeanModelFile());
 *  - which nodes are GoldenEye's muzzle flash (under the model's part 1, the
 *    flash switch gunfire.c turns on for a shot): never dealt to, always
 *    GoldenEye's own, so the flash keeps GoldenEye's place, roll and timing;
 *    the release's flash cards (a gun file's 0x30 piece-0 draws) are left out;
 *  - the deal: a Bean bone that is not the gun's body (the slide, the trigger
 *    finger, the revolver's drum and hammer) goes whole to the node most of it
 *    is nearest, so it rides that node's matrix (gunfire.c's slide recoil, the
 *    drum's turn); the body's triangles each go to the nearest node that is
 *    drawn under the body's matrix, so the hand stays with the hand's lists
 *    (their switches: off on the watch's inventory page) and a shell with its
 *    shell's (the shotgun's ammo switches).
 *
 * Nothing the sim reads is touched: GoldenEye's model, its rwdata, matrices,
 * switches and animation are unchanged; all memory is host malloc.
 */

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ModelFileHeader;
struct ModelNode;

/* The G*Z file a player's first-person gun header holds now, else NULL. */
const char *beanGunFile(const struct ModelFileHeader *hdr);

/* A display-list node's role in a gun model. */
#define BEANGUN_NODE_GUN   0
#define BEANGUN_NODE_HAND  1   /* under one of the hand's parts (8..13): off on the watch page */
#define BEANGUN_NODE_FLASH 2   /* under the flash switch (part 1): GoldenEye's own, always */
s32 beanGunNodeClass(const struct ModelFileHeader *hdr, const struct ModelNode *node);

/* A gun file's draw that is not the gun: its muzzle-flash cards (0x30
 * piece 0), which the release draws only for the frame of a shot. */
s32 beanGunDrawIsCard(s32 piece);

/* A colour a gun's (or its pickup's) vertices are drawn with, ARGB, 0 for
 * their own (PD fpTint): the Golden Gun's pictures are near-white scratch and
 * shine maps the release lays under a gold reflection map on a second
 * sampler, so with one picture it drew silver. Its glove keeps its colours. */
u32 beanGunTint(const char *source);
/* Whether a picture of a gun file is the release's glove (the one 512x511
 * picture the pistols' and knives' hands are drawn with, PD beanDrawIsGlove). */
s32 beanGunPictureIsGlove(s32 w, s32 h);

struct beangunnode {
    const void *verts;       /* GoldenEye Vertex[] of the list (model units, relative to origin) */
    s32 numverts;
    f32 origin[3];
    s32 mtx;                 /* modelFindNodeMtxIndex() of the list */
    s32 cls;                 /* BEANGUN_NODE_* */
};

struct beanguntri {
    f32 mid[3];              /* the triangle's middle, model space */
    s16 group;               /* its Bean bone (palette entry), -1 for none */
    s8 glove;                /* round 31c (D-281): drawn with the glove picture - the hand's, never the gun's */
};

/* Deals every triangle to a node (outnode[t], an index into nodes; -1 when
 * no node takes it). Returns the number of bone groups dealt whole.
 * Round 31c (D-281): when the file has a glove, a glove triangle goes only to
 * a hand list and every other triangle only to a gun list - GoldenEye turns
 * the hand's lists off on the watch's inventory page, so a grip dealt to a
 * hand list vanished there and a knuckle dealt to a gun list hung beside it. */
s32 beanGunDeal(const struct beanguntri *tris, s32 numtris, const struct beangunnode *nodes, s32 numnodes,
                s16 *outnode, s32 *outbodymtx);

#ifdef __cplusplus
}
#endif

#endif
