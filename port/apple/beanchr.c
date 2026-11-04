/* Imported from GoldenEye-Decomp-ios engine/port/src/beanchr.c (first 5366bbe6, last changed 47ef757f; donor HEAD 55a3a9c8),
 * round 15, XBLA step 4.4 (docs/port-ios/24-round15-chrs.md). 64-bit host layer only.
 * Changes against the donor are marked "Midas round 15". */
/*
 * GoldenEye XBLA's HD characters on GoldenEye's own character models - see
 * port/include/beanchr.h and docs/tier-c.md "Round 26b: characters". Tier C
 * round 26b (C4 part 2, DECISIONS D-150..D-153).
 *
 * The fit is Perfect Dark's (perfect-dark-ios build/src/port/src/gebean.c,
 * MIT): beanRigFromModel() (the model's joints named as Bean's bones from
 * their rest positions - GoldenEye's star rest pose), beanFitRig() /
 * beanFitPalette() / beanFitBranch() (each Bean bone turned and stretched onto
 * the model's segment, the base and back least-squares on their three
 * children; palette entry m = Bean bind -> the model's rest about joint m),
 * beanPastTheJoint(), and the head seat (a head rigid on the neck at the
 * point the back's fit carries Bean's neck to). Where PD skins on the CPU
 * every frame (xblaMeshPose), this gives each vertex to its heaviest joint
 * and lets a triangle span two joints' matrices, as GoldenEye's own lists do
 * (segment 3 matrix loads between vertex batches): no per-frame work beyond
 * what fast3d does for an N64 list (D-151).
 *
 * MEMORY RULE (charter rule 3c, D-110): everything here is host malloc.
 */

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <ultra64.h>
#include <PR/gbi.h>
#include "platform.h"
#include "system.h"
#include "config.h"
#include "texpack.h"
#include "beanmesh.h"
#include "beanstage.h"
#include "beanmodel.h"
#include "beanchr.h"
#include "chrlight.h"
#include "bondconstants.h"
#include "bondtypes.h"
#include "guestmem.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

float floorf(float x);
float roundf(float x);
float sqrtf(float x);
float fabsf(float x);
float fminf(float x, float y);
float fmaxf(float x, float y);

extern ChrModelFileRecord c_item_entries[];
extern PropRecord *g_ActivePropsHead;
union ModelRwData *modelGetNodeRwData(Model *Objinst, ModelNode *root);
s32 modelFindNodeMtxIndex(ModelNode *node, s32 arg1);

#define BATCHVERTS 16
#define TILE 32
#define TILE_SCALE (TILE * 32.0f)
#define OPAQUE_ALPHA 0xf0
#define SOFT_PERCENT 1
#define MAXNODES 96
#define MAXMTX 64
#define HEAD_SCALE 0.213f

/* round 32c (D-300): the lit HD characters' lights - set by litParams(), `on`
 * by beanChrFrame() (see "light" below); every HD list carries its address */
static GfxChrLight chrLight;

/* ------------------------------------------------------------ the table */

#define BEANCHR_BODY  0
#define BEANCHR_WHOLE 1
#define BEANCHR_HEAD  2

struct chrrow {
    const char *file;
    u8 kind;
    const char *source;
};
#define CHRROW(file, kind, source) { file, kind, source }
static const struct chrrow chrRows[] = {
#include "beanchrtable.h"
};
#define NUM_CHRROWS ((s32)(sizeof(chrRows) / sizeof(chrRows[0])))

/* ------------------------------------------------------------ the skeleton */

enum {
    SK_BASE, SK_BACK, SK_NECK, SK_POSITION,
    SK_LF_SHOULDER, SK_LF_ELBOW, SK_LF_WRIST,
    SK_RT_SHOULDER, SK_RT_ELBOW, SK_RT_WRIST,
    SK_LF_HIP, SK_LF_KNEE, SK_LF_ANKLE,
    SK_RT_HIP, SK_RT_KNEE, SK_RT_ANKLE,
    SK_COUNT
};

static const char *const skelNames[SK_COUNT] = {
    "SKEL_BASE", "SKEL_BACK", "SKEL_NECK", "SKEL_POSITION",
    "SKEL_LF_SHOULDER", "SKEL_LF_ELBOW", "SKEL_LF_WRIST",
    "SKEL_RT_SHOULDER", "SKEL_RT_ELBOW", "SKEL_RT_WRIST",
    "SKEL_LF_HIP", "SKEL_LF_KNEE", "SKEL_LF_ANKLE",
    "SKEL_RT_HIP", "SKEL_RT_KNEE", "SKEL_RT_ANKLE",
};

/* The joint each bone's segment runs to (PD skelChild), -1 at a chain's end. */
static const s8 skelChild[SK_COUNT] = {
    SK_BACK, SK_NECK, -1, -1,
    SK_LF_ELBOW, SK_LF_WRIST, -1,
    SK_RT_ELBOW, SK_RT_WRIST, -1,
    SK_LF_KNEE, SK_LF_ANKLE, -1,
    SK_RT_KNEE, SK_RT_ANKLE, -1,
};

/* And the bone an end turns with (PD skelInherit). */
static const s8 skelInherit[SK_COUNT] = {
    -1, -1, SK_BACK, SK_BASE,
    -1, -1, SK_LF_ELBOW,
    -1, -1, SK_RT_ELBOW,
    -1, -1, SK_LF_KNEE,
    -1, -1, SK_RT_KNEE,
};

/* PD beanBoneNameIs(): SKEL_X, or SKEL_X_ followed only by _WPRE. */
static s32 boneNameIs(const char *name, const char *joint)
{
    const size_t len = strlen(joint);
    if (strncmp(name, joint, len) != 0) {
        return 0;
    }
    if (name[len] == '\0') {
        return 1;
    }
    if (name[len] != '_') {
        return 0;
    }
    for (const char *c = name + len; *c; c++) {
        if (*c != '_' && *c != 'W' && *c != 'P' && *c != 'R' && *c != 'E') {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------ small maths */

static f32 vecLen(const f32 *v)
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void vecCross(const f32 *a, const f32 *b, f32 *out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* PD rotationBetween(): the turn taking direction a onto direction b. */
static void rotationBetween(const f32 *a0, const f32 *b0, f32 r[3][3])
{
    const f32 la = vecLen(a0), lb = vecLen(b0);
    f32 a[3], b[3], v[3], vx[3][3], c, k;

    for (s32 i = 0; i < 3; i++) {
        for (s32 j = 0; j < 3; j++) {
            r[i][j] = i == j ? 1.0f : 0.0f;
        }
    }
    if (la < 1e-6f || lb < 1e-6f) {
        return;
    }
    for (s32 i = 0; i < 3; i++) {
        a[i] = a0[i] / la;
        b[i] = b0[i] / lb;
    }
    vecCross(a, b, v);
    c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (vecLen(v) < 1e-9f) {
        if (c < 0.0f) {
            for (s32 i = 0; i < 3; i++) {
                r[i][i] = -1.0f;
            }
        }
        return;
    }
    vx[0][0] = 0.0f;  vx[0][1] = -v[2]; vx[0][2] = v[1];
    vx[1][0] = v[2];  vx[1][1] = 0.0f;  vx[1][2] = -v[0];
    vx[2][0] = -v[1]; vx[2][1] = v[0];  vx[2][2] = 0.0f;
    k = 1.0f / (1.0f + c);
    for (s32 i = 0; i < 3; i++) {
        for (s32 j = 0; j < 3; j++) {
            f32 sq = 0.0f;
            for (s32 m = 0; m < 3; m++) {
                sq += vx[i][m] * vx[m][j];
            }
            r[i][j] += vx[i][j] + sq * k;
        }
    }
}

static void rotApply(const f32 r[3][3], const f32 *p, f32 *out)
{
    for (s32 i = 0; i < 3; i++) {
        out[i] = r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2];
    }
}

static void mat3Mul(const f32 a[3][3], const f32 b[3][3], f32 out[3][3])
{
    for (s32 i = 0; i < 3; i++) {
        for (s32 j = 0; j < 3; j++) {
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
}

static f32 mat3Det(const f32 m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

static s32 mat3Inverse(const f32 m[3][3], f32 out[3][3])
{
    const f32 det = mat3Det(m);
    if (fabsf(det) < 1e-12f) {
        return 0;
    }
    out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return 1;
}

/* ------------------------------------------------------------ the rig */

struct rig {
    s32 have[SK_COUNT];
    f32 joint[SK_COUNT][3];
    s32 mtx[SK_COUNT];
    f32 rot[SK_COUNT][3][3];
    f32 lin[SK_COUNT][3][3];
    f32 scale;
    s32 hasrest[MAXMTX];
    f32 rest[MAXMTX][3];
    s32 haspal[MAXMTX];
    f32 pal[MAXMTX][12];
    f32 necktarget[3];       /* where the back's fit carries Bean's neck (the head's seat) */
};

/* A node of a model file, walked from the file's own static tree (switch and
 * LOD children by their rodata, never through the per-instance Child links;
 * a body's head placeholder is not descended: the head is another file). */
struct walked {
    ModelNode *node;
    f32 rest[3];   /* group joints: the absolute rest (the sum of the origins above and its own) */
    u8 switched, lod;
};

struct walk {
    struct walked n[256];
    s32 num;
    ModelNode *headspot;
    s32 overflow;
};

static void walkTree(struct walk *w, ModelNode *first, const f32 *parent, s32 switched, s32 lod, s32 depth)
{
    for (ModelNode *n = first; n && depth < 64; n = n->Next) {
        const u32 type = n->Opcode & 0xff;
        f32 rest[3] = { parent[0], parent[1], parent[2] };
        s32 seen = 0;
        for (s32 i = 0; i < w->num && !seen; i++) {
            seen = w->n[i].node == n;
        }
        if (seen) {
            continue;
        }
        if ((type == MODELNODE_OPCODE_GROUP || type == MODELNODE_OPCODE_OP03) && n->Data) {
            rest[0] += n->Data->Group.Origin.x;
            rest[1] += n->Data->Group.Origin.y;
            rest[2] += n->Data->Group.Origin.z;
        } else if (type == MODELNODE_OPCODE_GROUPSIMPLE && n->Data) {
            rest[0] += n->Data->GroupSimple.Origin.x;
            rest[1] += n->Data->GroupSimple.Origin.y;
            rest[2] += n->Data->GroupSimple.Origin.z;
        }
        if (w->num < (s32)(sizeof(w->n) / sizeof(w->n[0]))) {
            struct walked *e = &w->n[w->num++];
            e->node = n;
            memcpy(e->rest, rest, sizeof(rest));
            e->switched = (u8)switched;
            e->lod = (u8)lod;
        } else {
            w->overflow = 1;
        }
        if (type == MODELNODE_OPCODE_HEAD) {
            if (!w->headspot) {
                w->headspot = n;
            }
            continue;
        }
        if (type == MODELNODE_OPCODE_SWITCH && n->Data) {
            walkTree(w, n->Data->Switch.Controls, rest, 1, lod, depth + 1);
        } else if (type == MODELNODE_OPCODE_LOD && n->Data) {
            walkTree(w, n->Data->LOD.Affects, rest, switched, lod || n->Data->LOD.MinDistance > 0.0f, depth + 1);
        } else if (type == MODELNODE_OPCODE_BSP && n->Data) {
            walkTree(w, n->Data->BSP.leftChild, rest, switched, lod, depth + 1);
            walkTree(w, n->Data->BSP.rightChild, rest, switched, lod, depth + 1);
        } else {
            walkTree(w, n->Child, rest, switched, lod, depth + 1);
        }
    }
}

struct limbjoint {
    f32 ax;
    s32 mtx;
    f32 rest[3];
};

/* PD beanRigFromModel(): the chrinfo root (GoldenEye's header node) is the
 * base; a group joint within 30 units of the middle line is the back below
 * y 200 and the neck above (first of each); the rest are three to a limb -
 * an arm above 200, a leg below, left where x is positive - ordered
 * outwards. GoldenEye's star rest pose. */
static s32 rigFromModel(const struct walk *w, struct rig *rig, char *why, size_t whylen)
{
    struct limbjoint limbs[4][8];
    s32 numlimb[4] = { 0, 0, 0, 0 };
    s32 basegroup = 0;

    memset(rig, 0, sizeof(*rig));
    for (s32 i = 0; i < w->num; i++) {
        const ModelNode *n = w->n[i].node;
        const u32 type = n->Opcode & 0xff;
        f32 rest[3];
        s32 mtx, skel = -1;

        if (type == MODELNODE_OPCODE_HEADER && n->Data) {
            mtx = n->Data->Header.MatrixIndex;
            rest[0] = rest[1] = rest[2] = 0.0f;
        } else if ((type == MODELNODE_OPCODE_GROUP || type == MODELNODE_OPCODE_OP03) && n->Data) {
            mtx = n->Data->Group.MatrixIDs[0];
            memcpy(rest, w->n[i].rest, sizeof(rest));
        } else {
            continue;
        }
        if (mtx >= 0 && mtx < MAXMTX) {
            rig->hasrest[mtx] = 1;
            memcpy(rig->rest[mtx], rest, sizeof(rest));
        }
        if (type == MODELNODE_OPCODE_HEADER) {
            skel = rig->have[SK_BASE] ? -1 : SK_BASE;
        } else if (!basegroup && fabsf(rest[0]) < 1.0f && fabsf(rest[1]) < 1.0f && fabsf(rest[2]) < 1.0f) {
            /* GoldenEye's guards hang the hips and the spine under a group
             * joint at the root (the pelvis, which the animation turns)
             * rather than under the header itself, as PD's Natalya does: that
             * joint is the base (D-151) */
            skel = SK_BASE;
            basegroup = 1;
        } else if (fabsf(rest[0]) < 30.0f) {
            skel = rest[1] < 200.0f ? SK_BACK : SK_NECK;
            if (rig->have[skel]) {
                skel = -1;
            }
        } else {
            const s32 limb = (rest[0] > 0.0f ? 0 : 1) * 2 + (rest[1] > 200.0f ? 0 : 1);
            if (numlimb[limb] < 8) {
                limbs[limb][numlimb[limb]].ax = fabsf(rest[0]);
                limbs[limb][numlimb[limb]].mtx = mtx;
                memcpy(limbs[limb][numlimb[limb]].rest, rest, sizeof(rest));
                numlimb[limb]++;
            }
        }
        if (skel >= 0) {
            rig->have[skel] = 1;
            rig->mtx[skel] = mtx;
            memcpy(rig->joint[skel], rest, sizeof(rest));
        }
    }
    /* limb 0 left arm, 1 left leg, 2 right arm, 3 right leg */
    for (s32 limb = 0; limb < 4; limb++) {
        static const s8 order[4][3] = {
            { SK_LF_SHOULDER, SK_LF_ELBOW, SK_LF_WRIST },
            { SK_LF_HIP, SK_LF_KNEE, SK_LF_ANKLE },
            { SK_RT_SHOULDER, SK_RT_ELBOW, SK_RT_WRIST },
            { SK_RT_HIP, SK_RT_KNEE, SK_RT_ANKLE },
        };
        if (numlimb[limb] != 3) {
            snprintf(why, whylen, "a limb of the model has %d joints, not 3", numlimb[limb]);
            return 0;
        }
        for (s32 i = 0; i < 3; i++) {
            for (s32 j = i + 1; j < 3; j++) {
                if (limbs[limb][j].ax < limbs[limb][i].ax) {
                    const struct limbjoint tmp = limbs[limb][i];
                    limbs[limb][i] = limbs[limb][j];
                    limbs[limb][j] = tmp;
                }
            }
        }
        for (s32 i = 0; i < 3; i++) {
            const s32 skel = order[limb][i];
            rig->have[skel] = 1;
            rig->mtx[skel] = limbs[limb][i].mtx;
            memcpy(rig->joint[skel], limbs[limb][i].rest, sizeof(rig->joint[skel]));
        }
    }
    for (s32 s = 0; s < SK_COUNT; s++) {
        if (s != SK_POSITION && !rig->have[s]) {
            snprintf(why, whylen, "the model has no joint for %s", skelNames[s]);
            return 0;
        }
        if (s != SK_POSITION && (rig->mtx[s] < 0 || rig->mtx[s] >= MAXMTX)) {
            snprintf(why, whylen, "%s's matrix %d is out of range", skelNames[s], rig->mtx[s]);
            return 0;
        }
    }
    rig->have[SK_POSITION] = 1;
    rig->mtx[SK_POSITION] = rig->mtx[SK_BASE];
    memcpy(rig->joint[SK_POSITION], rig->joint[SK_BASE], sizeof(rig->joint[0]));
    return 1;
}

/* PD beanFitBranch(): a bone with three children (base: back + hips; back:
 * neck + shoulders) as the least-squares linear map of Bean's three segments
 * onto the model's, the depth added as a fourth pair. */
static s32 fitBranch(struct rig *rig, const f32 bind[SK_COUNT][3], s32 a, const s8 children[3])
{
    f32 db[4][3], dj[4][3], cj[3];
    f32 sjb[3][3], sbb[3][3], inv[3][3], lin[3][3];
    f32 lb, lj;

    for (s32 c = 0; c < 3; c++) {
        for (s32 k = 0; k < 3; k++) {
            db[c][k] = (bind[(s32)children[c]][k] - bind[a][k]) * rig->scale;
            dj[c][k] = rig->joint[(s32)children[c]][k] - rig->joint[a][k];
        }
    }
    vecCross(db[1], db[2], db[3]);
    vecCross(dj[1], dj[2], cj);
    lb = vecLen(db[3]);
    lj = vecLen(cj);
    if (lb < 1e-6f || lj < 1e-6f) {
        return 0;
    }
    for (s32 k = 0; k < 3; k++) {
        dj[3][k] = cj[k] * lb / lj;
    }
    memset(sjb, 0, sizeof(sjb));
    memset(sbb, 0, sizeof(sbb));
    for (s32 p = 0; p < 4; p++) {
        for (s32 i = 0; i < 3; i++) {
            for (s32 j = 0; j < 3; j++) {
                sjb[i][j] += dj[p][i] * db[p][j];
                sbb[i][j] += db[p][i] * db[p][j];
            }
        }
    }
    if (!mat3Inverse((const f32 (*)[3])sbb, inv)) {
        return 0;
    }
    mat3Mul((const f32 (*)[3])sjb, (const f32 (*)[3])inv, lin);
    if (mat3Det((const f32 (*)[3])lin) <= 0.0f) {
        return 0;
    }
    memcpy(rig->lin[a], lin, sizeof(lin));
    return 1;
}

/* PD beanFitPalette(): each bone's map onto the rig, then palette entry m
 * (the first bone moving matrix m): Bean's bind about the bone's joint,
 * through its fit, onto the joint, less the rest the matrix carries. */
static void fitPalette(struct rig *rig, const f32 bind[SK_COUNT][3])
{
    static const s8 basechildren[3] = { SK_BACK, SK_LF_HIP, SK_RT_HIP };
    static const s8 backchildren[3] = { SK_NECK, SK_LF_SHOULDER, SK_RT_SHOULDER };

    for (s32 a = 0; a < SK_COUNT; a++) {
        const s32 b = skelChild[a];
        memcpy(rig->lin[a], rig->rot[a], sizeof(rig->lin[a]));
        if ((a == SK_BASE && fitBranch(rig, bind, a, basechildren))
                || (a == SK_BACK && fitBranch(rig, bind, a, backchildren))) {
            continue;
        }
        if (b >= 0) {
            f32 db[3], dj[3], u[3], stretch[3][3];
            f32 lb, lj, k;
            for (s32 i = 0; i < 3; i++) {
                db[i] = bind[b][i] - bind[a][i];
                dj[i] = rig->joint[b][i] - rig->joint[a][i];
            }
            lb = vecLen(db) * rig->scale;
            lj = vecLen(dj);
            if (lb < 1e-6f) {
                continue;
            }
            k = lj / lb;
            for (s32 i = 0; i < 3; i++) {
                u[i] = db[i] * rig->scale / lb;
            }
            for (s32 i = 0; i < 3; i++) {
                for (s32 j = 0; j < 3; j++) {
                    stretch[i][j] = (i == j ? 1.0f : 0.0f) + (k - 1.0f) * u[i] * u[j];
                }
            }
            mat3Mul((const f32 (*)[3])rig->rot[a], (const f32 (*)[3])stretch, rig->lin[a]);
        }
    }
    for (s32 a = 0; a < SK_COUNT; a++) {
        if (skelInherit[a] >= 0 && a != SK_POSITION) {
            memcpy(rig->lin[a], rig->rot[a], sizeof(rig->lin[a]));
        }
    }
    memcpy(rig->lin[SK_POSITION], rig->lin[SK_BASE], sizeof(rig->lin[SK_POSITION]));

    /* the neck goes where the back's fit carries Bean's (PD: the model's
     * shoulders sit higher up its back; onto the model's joint the collar
     * swallowed the neck) - and that is where a head is seated */
    {
        f32 d[3], fitted[3];
        for (s32 k = 0; k < 3; k++) {
            d[k] = (bind[SK_NECK][k] - bind[SK_BACK][k]) * rig->scale;
        }
        rotApply((const f32 (*)[3])rig->lin[SK_BACK], d, fitted);
        for (s32 k = 0; k < 3; k++) {
            rig->necktarget[k] = rig->joint[SK_BACK][k] + fitted[k];
        }
    }

    memset(rig->haspal, 0, sizeof(rig->haspal));
    for (s32 a = 0; a < SK_COUNT; a++) {
        const s32 m = rig->mtx[a];
        f32 sb[3], turned[3], target[3];
        if (!rig->have[a] || m < 0 || m >= MAXMTX || rig->haspal[m]) {
            continue;
        }
        for (s32 k = 0; k < 3; k++) {
            sb[k] = bind[a][k] * rig->scale;
        }
        rotApply((const f32 (*)[3])rig->lin[a], sb, turned);
        memcpy(target, a == SK_NECK ? rig->necktarget : rig->joint[a], sizeof(target));
        for (s32 r = 0; r < 3; r++) {
            const f32 rest = rig->hasrest[m] ? rig->rest[m][r] : rig->joint[a][r];
            for (s32 c = 0; c < 3; c++) {
                rig->pal[m][r * 4 + c] = rig->lin[a][r][c];
            }
            rig->pal[m][r * 4 + 3] = target[r] - rest - turned[r];
        }
        rig->haspal[m] = 1;
    }
}

/* PD beanFitRig(): the scale (the two skeletons' limb lengths) and each
 * bone's turn onto the rig. */
static s32 fitRig(struct rig *rig, const f32 bind[SK_COUNT][3], const s32 *havebind, char *why, size_t whylen)
{
    f32 num = 0.0f, den = 0.0f;
    for (s32 a = 0; a < SK_COUNT; a++) {
        const s32 b = skelChild[a];
        f32 dj[3], db[3];
        if (b < 0) {
            continue;
        }
        if (!havebind[a] || !havebind[b]) {
            snprintf(why, whylen, "Bean's skeleton has no %s or %s", skelNames[a], skelNames[b]);
            return 0;
        }
        for (s32 k = 0; k < 3; k++) {
            dj[k] = rig->joint[b][k] - rig->joint[a][k];
            db[k] = bind[b][k] - bind[a][k];
        }
        if (a != SK_BASE) {
            num += vecLen(dj);
            den += vecLen(db);
        }
        rotationBetween(db, dj, rig->rot[a]);
    }
    for (s32 a = 0; a < SK_COUNT; a++) {
        if (skelInherit[a] >= 0) {
            memcpy(rig->rot[a], rig->rot[skelInherit[a]], sizeof(rig->rot[a]));
        }
    }
    rig->scale = den > 0.0f ? num / den : HEAD_SCALE;
    fitPalette(rig, bind);
    if (getenv("GE_BEANRIGLOG")) {
        /* D-245 dev: how the head's turn (the back's segment turn) and the
         * torso's (the back's branch fit) carry Bean's forward and up */
        static const f32 fwd[3] = { 0.0f, 0.0f, 1.0f }, up[3] = { 0.0f, 1.0f, 0.0f };
        f32 rf[3], lf[3], ru[3], lu[3], sp[3];
        rotApply((const f32 (*)[3])rig->rot[SK_BACK], fwd, rf);
        rotApply((const f32 (*)[3])rig->lin[SK_BACK], fwd, lf);
        rotApply((const f32 (*)[3])rig->rot[SK_BACK], up, ru);
        rotApply((const f32 (*)[3])rig->lin[SK_BACK], up, lu);
        for (s32 k = 0; k < 3; k++) {
            sp[k] = bind[SK_NECK][k] - bind[SK_BACK][k];
        }
        sysLogPrintf(LOG_NOTE, "rig: spine lean (bind) %.1f deg fwd; head fwd pitch %.1f, torso fwd pitch %.1f; head up pitch %.1f, torso up pitch %.1f; scale %.4f",
                     __builtin_atan2f(sp[2], sp[1]) * 57.29578f, __builtin_atan2f(rf[1], rf[2]) * 57.29578f,
                     __builtin_atan2f(lf[1], lf[2]) * 57.29578f, __builtin_atan2f(ru[2], ru[1]) * 57.29578f,
                     __builtin_atan2f(lu[2], lu[1]) * 57.29578f, rig->scale);
    }
    return 1;
}

/* PD beanPastTheJoint(): a point skinned to a forearm or shin past the next
 * joint along the limb goes with that joint (the boiler suit's fists). */
static s32 pastTheJoint(s32 skel, const f32 *pos, const f32 bind[SK_COUNT][3], const s32 *havebind)
{
    s32 next;
    f32 along = 0.0f, len = 0.0f;
    switch (skel) {
    case SK_LF_ELBOW: next = SK_LF_WRIST; break;
    case SK_RT_ELBOW: next = SK_RT_WRIST; break;
    case SK_LF_KNEE: next = SK_LF_ANKLE; break;
    case SK_RT_KNEE: next = SK_RT_ANKLE; break;
    default: return skel;
    }
    if (!havebind[skel] || !havebind[next]) {
        return skel;
    }
    for (s32 k = 0; k < 3; k++) {
        const f32 axis = bind[next][k] - bind[skel][k];
        along += axis * (pos[k] - bind[skel][k]);
        len += axis * axis;
    }
    return len > 0.0f && along > len ? next : skel;
}

/* ------------------------------------------------------------ buffers */

struct gdlbuf {
    Gfx *g;
    s32 num, cap;
};

static void emit(struct gdlbuf *b, uintptr_t w0, uintptr_t w1)
{
    if (b->num >= b->cap) {
        const s32 cap = b->cap ? b->cap * 2 : 256;
        Gfx *n = realloc(b->g, sizeof(Gfx) * (size_t)cap);
        if (!n) {
            return;
        }
        b->g = n;
        b->cap = cap;
    }
    b->g[b->num].words.w0 = w0;
    b->g[b->num].words.w1 = w1;
    b->num++;
}

struct vtxbuf {
    Vtx *v;
    s16 *mtx;      /* each vertex's joint matrix (the blood paint map's, D-175) */
    f32 *nrm;      /* round 32c (D-300): each vertex's normal in the model's rest, 3 a vertex (build only) */
    s32 num, cap;
};

static void vtxAdd(struct vtxbuf *vb, const Vtx *v, s32 mtx, const f32 *nrm)
{
    if (vb->num >= vb->cap) {
        const s32 cap = vb->cap ? vb->cap * 2 : 256;
        Vtx *n = realloc(vb->v, sizeof(Vtx) * (size_t)cap);
        if (!n) {
            return;
        }
        vb->v = n;
        s16 *m = realloc(vb->mtx, sizeof(s16) * (size_t)cap);
        if (!m) {
            return;
        }
        vb->mtx = m;
        f32 *nr = realloc(vb->nrm, sizeof(f32) * 3 * (size_t)cap);
        if (!nr) {
            return;
        }
        vb->nrm = nr;
        vb->cap = cap;
    }
    vb->mtx[vb->num] = (s16)mtx;
    memcpy(&vb->nrm[vb->num * 3], nrm, sizeof(f32) * 3);
    vb->v[vb->num++] = *v;
}

static s32 clampS16(f32 f, s16 *out)
{
    const f32 r = roundf(f);
    if (r < -32768.0f || r > 32767.0f) {
        return 0;
    }
    *out = (s16)r;
    return 1;
}

static u16 texel5551(u32 r, u32 g, u32 b)
{
    const u16 t = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
    return (u16)((t >> 8) | (t << 8));
}

static u16 *newTile(u16 texel)
{
    u16 *tile = malloc(TILE * TILE * sizeof(u16));
    if (tile) {
        for (s32 i = 0; i < TILE * TILE; i++) {
            tile[i] = texel;
        }
    }
    return tile;
}

/* beanmodel.c emitTexture(): a 32x32 RGBA16 stand-in, wrap, its host address
 * in G_SETTIMG; the texture hook draws the decoded picture in its place. */
static void emitTexture(struct gdlbuf *l, const u16 *tile)
{
    emit(l, 0xba000e02, 0x00000000);
    emit(l, 0xfd100000, (uintptr_t)tile);
    emit(l, 0xf5100000, 0x07014050);
    emit(l, 0xe6000000, 0x00000000);
    emit(l, 0xf3000000, 0x073ff100);
    emit(l, 0xe7000000, 0x00000000);
    emit(l, 0xf5101000, 0x00014050);
    emit(l, 0xf2000000, 0x0007c07c);
}

/* ------------------------------------------------------------ the build */

/* A corner: its place in the model's rest, and the matrix it rides. */
struct cvert {
    f32 pos[3];
    f32 uv[2];
    f32 nrm[3];    /* round 32c (D-300): the release's normal, turned into the model's rest (unit) */
    u32 argb;
    s16 mtx;
};

struct ctri {
    struct cvert v[3];
    s16 tex;
    s8 skel;       /* the joint dominating it: the node(s) it is drawn with */
    u8 glass;
};

struct pic {
    u8 used, alpha, soft;
    s16 standin;
};

/* D-175: GoldenEye's blood on a hit body part is the part's own vertex alpha
 * (chr.c chrCreateBloodStain() paints the instance's copy of the node's
 * vertices, 20..169), which the character combiner turns into a lerp from
 * the environment colour (GoldenEye's dark red) to the texel. Each solid HD
 * vertex reads that alpha from the nearest N64 triangle, barycentric, once
 * for the near level of detail and once for the far one. */
#define MAXPSRC 32
struct pmap {
    u8 src;            /* the N64 node (centry psrc[]), 0xff none */
    u8 w[3];           /* barycentric weights, summing 255 */
    u16 vi[3];         /* its vertices in the node's Vertices[] */
};

struct paintmap {
    s32 numv;          /* the solid list's vertices, [0, numv) of out.vtx */
    struct pmap *map;  /* numv * 2: near, far */
    s32 numsrcs;
    u8 srcs[MAXPSRC];  /* the N64 nodes it reads */
};

struct cnode {
    ModelNode *node;
    s8 skel;           /* the joint whose matrix the node rides, -1 unknown */
    u8 candidate;      /* takes HD triangles / is covered by the HD mesh */
    u8 shared;         /* its lists are an earlier node's of the same joint (the near/far LOD pair) */
    u8 lod;            /* under a far level of detail */
    s32 numtris;
    s32 numvtx;        /* vertices in out.vtx */
    s32 solidvtx;      /* the solid list's, [0, solidvtx) */
    s16 *vmtx;         /* each vertex's matrix (build only) */
    struct paintmap *paint;   /* D-175, NULL if no N64 paint reaches it */
    u8 *rgb;           /* round 32c (D-300): numvtx * 6 - each vertex's unlit RGB, then its lit RGB */
    struct beanmodelnode out;
};

/* Round 32c (D-301, dev GE_BEANLITLOG): GoldenEye's own model's vertex
 * colours fitted against its surface normals, luma = amb + dir . n - the
 * measurement that GoldenEye bakes no light direction into its characters
 * (|dir| ~0.03, M-161), so the lit path's light is not taken from it. */
struct chrlight {
    f32 amb, dir[3];
    s32 samples;       /* N64 vertices it was fitted on, 0 = no fit */
    f32 inward;        /* share of the N64 triangles that faced in before the sign fix */
};

struct centry {
    ModelFileHeader *hdr;
    ModelFileHeader *body;   /* a head: the body it is seated on (NULL for a body) */
    const char *file;
    s16 row;
    s8 state;                /* 0 unbuilt, 1 built, -1 GoldenEye's own */
    u8 kind;
    ModelNode *headspot;     /* a body: its head placeholder */
    s32 necknode;            /* a body: its neck's plain list, where its head's HD lists are drawn (-1 none) */
    ModelNode *rootswitch;   /* a head: the toggle at its root (the whole head shown or hidden), or NULL */
    struct cnode headdraw;   /* a head: its HD lists (drawn at the body's neck node) */
    ModelNode *psrc[MAXPSRC];  /* D-175: the N64 nodes GoldenEye paints blood on (DLCOLLISION) */
    s32 numpsrc;
    s32 numnodes;
    struct cnode *nodes;
    struct rig *rig;         /* a body: its fit, which its heads are seated by */
    f32 bind[SK_COUNT][3];   /* a body: Bean's own bind, by joint */
    s32 havebind[SK_COUNT];
    u16 **standins;
    s32 numstandins;
    u32 meshbytes, picbytes;
    s32 numtris;
    s8 litOn;                /* round 32c: which colours the vertices hold now (Video.LitCharacters) */
    char why[160];
};

static struct centry *entries;
static s32 numEntries, capEntries;
static struct centry *retired;
static s32 numRetired;
static s32 prebuilt;
static u16 *whiteTile;
static u32 meshBytes, picBytes;
static f64 buildMs;
static u32 neckDraws;   /* dev (GE_BEANCHRLOG=2): HD heads handed out since the last report */

static s32 envOff(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANCHRS");
        v = e && *e == '0';
    }
    return v;
}

static s32 logOn(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANCHRLOG");
        v = e && *e ? (*e == '2' ? 2 : 1) : 0;
    }
    return v;
}

static s32 onlyAllows(const char *file)
{
    const char *e = getenv("GE_BEANCHRONLY");
    if (!e || !*e) {
        return 1;
    }
    const size_t n = strlen(file);
    for (const char *p = e; (p = strstr(p, file)) != NULL; p += n) {
        if ((p == e || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) {
            return 1;
        }
    }
    return 0;
}

/* Bond's solo body and head are copies of their files' headers in a weapon
 * buffer (bondview2.c), not c_item_entries' own: noted here by
 * beanChrAlias() so they are found by name all the same.
 *
 * Round 26 merge: the body copy IS the right hand's first-person gun header
 * (get_ptr_itemheader_in_hand(GUNRIGHT)), refilled in place with a G*Z gun
 * once the intro is over - and the head copy sits inside that hand's weapon
 * buffer. An alias that outlived its contents drew Bond's HD body and head
 * as the viewmodel (seen on lane 2 mid weapon switch, when hand_item is
 * briefly unarmed and 26a's gunBeanModelFile() does not claim the header).
 * So an alias holds only while the copy still carries what it was filled
 * with: same root node, skeleton and counts as at beanChrAlias() time. */
static struct {
    const ModelFileHeader *copy, *original;
    const ModelNode *root;
    const ModelSkeleton *skel;
    s16 numMatrices, numRecords;
} aliases[8];
static s32 numAliases;

void beanChrAlias(struct ModelFileHeader *copy, struct ModelFileHeader *original)
{
    s32 at = -1;
    for (s32 i = 0; i < numAliases && at < 0; i++) {
        if (aliases[i].copy == copy) {
            at = i;
        }
    }
    if (at < 0) {
        at = numAliases < 8 ? numAliases++ : 7;
    }
    aliases[at].copy = copy;
    aliases[at].original = original;
    aliases[at].root = copy->RootNode;
    aliases[at].skel = copy->Skeleton;
    aliases[at].numMatrices = copy->numMatrices;
    aliases[at].numRecords = copy->numRecords;
}

/* 1 when hdr is an alias copy that no longer holds what beanChrAlias() saw */
static s32 aliasStale(const ModelFileHeader *hdr)
{
    for (s32 i = 0; hdr && i < numAliases; i++) {
        if (aliases[i].copy == hdr) {
            /* round 28b (D-178): not numRecords - makeonebody() grows the
             * body copy's count after the alias (149 -> 153, the held item
             * and head records): that made Bond's swirl body N64 on the
             * oracle since the round 26 merge. A gun refill changes the root. */
            return hdr->RootNode != aliases[i].root || hdr->Skeleton != aliases[i].skel
                || hdr->numMatrices != aliases[i].numMatrices;
        }
    }
    return 0;
}

static const char *fileOf(const ModelFileHeader *hdr)
{
    for (s32 i = 0; i < numAliases; i++) {
        if (aliases[i].copy == hdr) {
            if (aliasStale(hdr)) {
                return NULL;   /* refilled since (a gun in the hand): not a character */
            }
            hdr = aliases[i].original;
            break;
        }
    }
    for (s32 i = 0; hdr && c_item_entries[i].header; i++) {
        if (c_item_entries[i].header == hdr) {
            return c_item_entries[i].filename;
        }
    }
    return NULL;
}

/* The watch's floating arm (Csuit_lf_handZ) is first person (the C4 guns
 * round's) and its rig is an arm only: never drawn from here. */
static s32 rowOf(const char *file)
{
    if (!file || strcmp(file, "Csuit_lf_handZ") == 0) {
        return -1;
    }
    for (s32 i = 0; i < NUM_CHRROWS; i++) {
        if (strcmp(chrRows[i].file, file) == 0) {
            return i;
        }
    }
    return -1;
}

static s32 fileExists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* beanmodel.c sourcePath(): the importer's copy, else Video.XblaPath's tree. */
static s32 sourcePath(const char *source, char *out, size_t len)
{
    snprintf(out, len, "%s/files/new/%s/default.bin", sysResolvePath(TEXPACK_XBLA_DIR), source);
    if (fileExists(out)) {
        return 1;
    }
    const char *src = texpackXblaPath();
    if (src && *src) {
        snprintf(out, len, "%s/files/new/%s/default.bin", src, source);
        if (fileExists(out)) {
            return 1;
        }
        snprintf(out, len, "%s/new/%s/default.bin", src, source);
        if (fileExists(out)) {
            return 1;
        }
    }
    return 0;
}

static f64 msSince(u64 t0)
{
    return (f64)(sysGetMicroseconds() - t0) / 1000.0;
}

/* ------------------------------------------------------------ triangles */

struct collect {
    struct ctri *tris;
    s32 num, cap;
    const struct rig *rig;
    const f32 (*bind)[3];
    const s32 *havebind;
    const s8 *boneskel;      /* pose bone -> SK_*, -1 */
    s32 numbones;
    /* what to take */
    s32 mode;                /* 0 body (not the neck), 1 whole, 2 the neck's alone (a Bond head off his own body),
                                3 a head file / a head off another body: rigid on the neck */
    f32 headrot[3][3];
    f32 headbind[3];         /* mode 3: the head file's own neck bind */
    s32 neckback;            /* mode 3: a vertex weighted mostly to the back rides the back */
    s32 neckonly;            /* mode 3: only the neck's triangles (a Bond head off another outfit's file) */
    s32 dropped, black, glass, unskinned, unres, shader;
    s32 perskel[SK_COUNT];
    FILE *dump;              /* dev (GE_BEANHEADDUMP): every source triangle and its fate */
};

static void addTri(struct collect *c, const struct ctri *t)
{
    if (c->num >= c->cap) {
        const s32 cap = c->cap ? c->cap * 2 : 2048;
        struct ctri *n = realloc(c->tris, sizeof(*n) * (size_t)cap);
        if (!n) {
            return;
        }
        c->tris = n;
        c->cap = cap;
    }
    c->tris[c->num++] = *t;
}

/* round 32c: v made unit; a zero vector (no normal in the file) becomes +y */
static void unitOr(f32 *v)
{
    const f32 l = vecLen(v);
    if (l > 1e-6f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    } else {
        v[0] = 0.0f;
        v[1] = 1.0f;
        v[2] = 0.0f;
    }
}

static void skinTri(void *arg, int tex, const struct beanskinvtx *v)
{
    struct collect *c = arg;
    const struct rig *rig = c->rig;
    s32 sk[3][4];
    f32 wt[3][4];
    f32 total[SK_COUNT];
    s32 dominant = -1;
    struct ctri t;

    memset(total, 0, sizeof(total));
    memset(&t, 0, sizeof(t));
    if (v[0].kind == BEANDRAW_BADPIC || v[0].kind == BEANDRAW_SPHEREMAP) {
        c->unres++;
        return;
    }
    if (v[0].kind == BEANDRAW_COLOUR && !v[0].ownmat && v[0].argb == 0xffffffffu && v[1].argb == 0xffffffffu
            && v[2].argb == 0xffffffffu) {
        c->shader++;   /* a shader-only placeholder (D-142): left out */
        return;
    }
    for (s32 i = 0; i < 3; i++) {
        for (s32 s = 0; s < 4; s++) {
            const s32 bone = v[i].bone[s];
            sk[i][s] = -1;
            wt[i][s] = 0.0f;
            if (bone < 0 || bone >= c->numbones || c->boneskel[bone] < 0 || v[i].weight[s] == 0) {
                continue;
            }
            sk[i][s] = c->boneskel[bone];
            if (c->mode != 3) {
                sk[i][s] = pastTheJoint(sk[i][s], v[i].pos, c->bind, c->havebind);
            }
            wt[i][s] = (f32)v[i].weight[s];
            total[sk[i][s]] += wt[i][s];
        }
    }
    for (s32 s = 0; s < SK_COUNT; s++) {
        if (total[s] > 0.0f && (dominant < 0 || total[s] > total[dominant])) {
            dominant = s;
        }
    }
    if (dominant < 0 && c->mode == 3) {
        dominant = SK_NECK;   /* a head drawn with no palette is the head's (PD) */
    }
    if (dominant < 0) {
        c->unskinned++;
        return;
    }
    if (dominant == SK_POSITION) {
        dominant = SK_BASE;
    }
    if (c->dump) {
        fprintf(c->dump, "T tex %d draw %u dom %d kind %u plain %u blend %u matalpha %u\n", tex, (unsigned)v[0].draw,
                dominant, (unsigned)v[0].kind, (unsigned)v[0].plain, (unsigned)v[0].blend, (unsigned)v[0].matalpha);
        for (s32 i = 0; i < 3; i++) {
            fprintf(c->dump, "  v %.3f %.3f %.3f uv %.5f %.5f argb %08x b %d %d %d %d w %u %u %u %u sk %d %d %d %d\n", v[i].pos[0],
                    v[i].pos[1], v[i].pos[2], v[i].uv[0], v[i].uv[1], (unsigned)v[i].argb, v[i].bone[0], v[i].bone[1],
                    v[i].bone[2], v[i].bone[3], v[i].weight[0], v[i].weight[1], v[i].weight[2], v[i].weight[3],
                    sk[i][0], sk[i][1], sk[i][2], sk[i][3]);
        }
    }
    if ((c->mode == 0 && dominant == SK_NECK) || (c->mode == 2 && dominant != SK_NECK)) {
        c->dropped++;
        return;
    }
    if (c->mode == 3) {
        if (c->neckonly && dominant != SK_NECK) {
            c->dropped++;
            return;
        }
        dominant = SK_NECK;
    }

    for (s32 i = 0; i < 3; i++) {
        struct cvert *cv = &t.v[i];
        u32 argb = 0xffffffffu;   /* PD: a character is drawn white; its pictures carry the colour */
        cv->uv[0] = v[i].uv[0];
        cv->uv[1] = v[i].uv[1];
        if (v[i].plain) {
            argb = v[i].argb == 0xff000000u ? 0xffffffffu : v[i].argb;
        }
        if (v[i].blend && v[i].matalpha < 0xff) {
            const u32 a = ((argb >> 24) * v[i].matalpha + 127) / 255;
            argb = (argb & 0x00ffffffu) | (a << 24);
            t.glass = 1;
        }
        cv->argb = argb;
        if (c->mode == 3) {
            f32 rel[3], p[3];
            rotApply((const f32 (*)[3])c->headrot, v[i].nrm, cv->nrm);   /* round 32c (D-300) */
            unitOr(cv->nrm);
            f32 back = 0.0f, sum = 0.0f;
            for (s32 k = 0; k < 3; k++) {
                rel[k] = v[i].pos[k] - c->headbind[k];
            }
            rotApply((const f32 (*)[3])c->headrot, rel, p);
            for (s32 k = 0; k < 3; k++) {
                cv->pos[k] = rig->necktarget[k] + p[k] * rig->scale;
            }
            for (s32 s = 0; s < 4; s++) {
                if (sk[i][s] >= 0) {
                    sum += wt[i][s];
                    back += sk[i][s] == SK_BACK ? wt[i][s] : 0.0f;
                }
            }
            cv->mtx = (s16)(c->neckback && sum > 0.0f && back * 2.0f > sum ? rig->mtx[SK_BACK] : rig->mtx[SK_NECK]);
            continue;
        }
        /* PD: each bone's palette entry takes the vertex from Bean's bind
         * onto the model's rest; blended by weight here, once (the rest
         * pose), and the vertex then rides its heaviest joint's matrix */
        f32 pos[3] = { 0.0f, 0.0f, 0.0f }, nb[3] = { 0.0f, 0.0f, 0.0f }, sb[3], sum = 0.0f;
        s32 mtx[4];
        f32 mw[4];
        s32 nm = 0;
        for (s32 k = 0; k < 3; k++) {
            sb[k] = v[i].pos[k] * rig->scale;
        }
        for (s32 s = 0; s < 4; s++) {
            const s32 b = sk[i][s];
            if (b < 0) {
                continue;
            }
            const s32 m = rig->mtx[b];
            if (m < 0 || m >= MAXMTX || !rig->haspal[m]) {
                continue;
            }
            const f32 *pl = rig->pal[m];
            for (s32 r = 0; r < 3; r++) {
                const f32 q = pl[r * 4] * sb[0] + pl[r * 4 + 1] * sb[1] + pl[r * 4 + 2] * sb[2] + pl[r * 4 + 3]
                    + rig->rest[m][r];
                pos[r] += q * wt[i][s];
                /* round 32c (D-300): the normal turned by the same blend (the
                 * palette's linear part; its stretch is mild and the sum is
                 * made unit) */
                nb[r] += (pl[r * 4] * v[i].nrm[0] + pl[r * 4 + 1] * v[i].nrm[1] + pl[r * 4 + 2] * v[i].nrm[2]) * wt[i][s];
            }
            sum += wt[i][s];
            s32 at = -1;
            for (s32 j = 0; j < nm; j++) {
                if (mtx[j] == m) {
                    at = j;
                }
            }
            if (at >= 0) {
                mw[at] += wt[i][s];
            } else {
                mtx[nm] = m;
                mw[nm] = wt[i][s];
                nm++;
            }
        }
        if (sum <= 0.0f || nm == 0) {
            /* a corner with no bone of its own: the triangle's joint */
            const s32 m = rig->mtx[dominant];
            const f32 *pl = rig->pal[m];
            for (s32 r = 0; r < 3; r++) {
                cv->pos[r] = pl[r * 4] * sb[0] + pl[r * 4 + 1] * sb[1] + pl[r * 4 + 2] * sb[2] + pl[r * 4 + 3]
                    + rig->rest[m][r];
                cv->nrm[r] = pl[r * 4] * v[i].nrm[0] + pl[r * 4 + 1] * v[i].nrm[1] + pl[r * 4 + 2] * v[i].nrm[2];
            }
            unitOr(cv->nrm);
            cv->mtx = (s16)m;
            continue;
        }
        s32 best = 0;
        for (s32 j = 1; j < nm; j++) {
            if (mw[j] > mw[best]) {
                best = j;
            }
        }
        for (s32 k = 0; k < 3; k++) {
            cv->pos[k] = pos[k] / sum;
            cv->nrm[k] = nb[k];
        }
        unitOr(cv->nrm);
        cv->mtx = (s16)mtx[best];
    }
    t.tex = (s16)tex;
    t.skel = (s8)dominant;
    c->glass += t.glass;
    c->perskel[dominant]++;
    addTri(c, &t);
}

/* ------------------------------------------------------------ the lists */

struct batch {
    Vtx v[BATCHVERTS];
    s16 mtx[BATCHVERTS];
    f32 nrm[BATCHVERTS][3];   /* round 32c: the first corner's normal of each vertex */
    s32 numv;
    u8 tris[64][3];
    s32 numtris;
    f32 shiftu, shiftv;
    s32 shifted;
};

/* The batch's vertices by matrix: each run loaded under its own joint's
 * matrix (segment 3, the model instance's), then the G_TRI4s. */
static void batchFlush(struct gdlbuf *l, struct vtxbuf *vb, struct batch *b, s32 *curmtx, s32 *loads)
{
    if (b->numv == 0) {
        return;
    }
    s32 order[BATCHVERTS], slot[BATCHVERTS];
    for (s32 i = 0; i < b->numv; i++) {
        order[i] = i;
    }
    /* stable, the current matrix's run first (one load fewer) */
    for (s32 i = 1; i < b->numv; i++) {
        const s32 o = order[i];
        s32 j = i;
        while (j > 0) {
            const s32 p = order[j - 1];
            const s32 kp = b->mtx[p] == *curmtx ? -1 : b->mtx[p];
            const s32 ko = b->mtx[o] == *curmtx ? -1 : b->mtx[o];
            if (kp <= ko) {
                break;
            }
            order[j] = p;
            j--;
        }
        order[j] = o;
    }
    for (s32 i = 0; i < b->numv; i++) {
        slot[order[i]] = i;
    }
    for (s32 i = 0; i < b->numv;) {
        const s32 m = b->mtx[order[i]];
        s32 n = 1;
        while (i + n < b->numv && b->mtx[order[i + n]] == m) {
            n++;
        }
        if (m != *curmtx) {
            emit(l, (0x01u << 24) | ((u32)(G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW) << 16) | (u32)sizeof(Mtx),
                 ((u32)SPSEGMENT_MODEL_MTX << 24) | ((u32)m * (u32)sizeof(Mtx)));
            *curmtx = m;
            (*loads)++;
        }
        const u32 offset = (u32)vb->num * (u32)sizeof(Vtx);
        for (s32 k = 0; k < n; k++) {
            vtxAdd(vb, &b->v[order[i + k]], m, b->nrm[order[i + k]]);
        }
        emit(l, (0x04u << 24) | ((u32)(n - 1) << 20) | ((u32)i << 16) | (u32)(n * (s32)sizeof(Vtx)),
             ((u32)SPSEGMENT_MODEL_VTX << 24) | offset);
        i += n;
    }
    for (s32 i = 0; i < b->numtris; i += 4) {
        u32 w0 = 0xb1u << 24;
        u32 w1 = 0;
        for (s32 k = 0; k < 4 && i + k < b->numtris; k++) {
            w1 |= (u32)slot[b->tris[i + k][0]] << (k * 8);
            w1 |= (u32)slot[b->tris[i + k][1]] << (k * 8 + 4);
            w0 |= (u32)slot[b->tris[i + k][2]] << (k * 4);
        }
        emit(l, w0, w1);
    }
    b->numv = 0;
    b->numtris = 0;
    b->shifted = 0;
}

static s32 batchFind(const struct batch *b, const Vtx *v, s32 mtx)
{
    for (s32 i = 0; i < b->numv; i++) {
        if (b->mtx[i] == mtx && memcmp(&b->v[i], v, sizeof(*v)) == 0) {
            return i;
        }
    }
    return -1;
}

static const struct ctri *sortTris;

static int compareTex(const void *a, const void *b)
{
    const struct ctri *ta = &sortTris[*(const s32 *)a];
    const struct ctri *tb = &sortTris[*(const s32 *)b];
    if (ta->tex != tb->tex) {
        return ta->tex - tb->tex;
    }
    return *(const s32 *)a - *(const s32 *)b;
}

/* One list of a node (beanmodel.c writeList() with matrices): triangles by
 * picture, 16-vertex batches, each vertex relative to its own joint's rest,
 * UVs S10.5 on the stand-in. Back faces culled (PD: HD characters cull,
 * props do not); G_CULL_BACK is what GoldenEye's lists leave on. */
static Gfx *writeList(struct vtxbuf *vb, const struct ctri *tris, s32 *list, s32 num, const struct rig *rig,
                      const struct pic *pics, u16 *const *standins, s32 alpha, s32 *dropped, u32 *bytes, s32 *loads)
{
    struct gdlbuf l;
    struct batch b;
    s32 curtex = -2, curmtx = -1;

    if (num == 0) {
        return NULL;
    }
    memset(&l, 0, sizeof(l));
    memset(&b, 0, sizeof(b));
    sortTris = tris;
    qsort(list, (size_t)num, sizeof(*list), compareTex);

    emit(&l, 0xe7000000, 0x00000000);   /* PIPESYNC */
    emit(&l, 0xba001001, 0x00010000);   /* texture LOD (fast3d: tile 0 for both texels) */
    emit(&l, 0xba001102, 0x00000000);   /* TEXTDETAIL clamp */
    emit(&l, 0xba000c02, 0x00002000);   /* bilinear */
    emit(&l, 0xb6000000, 0x00001000);   /* no front-face cull */
    emit(&l, 0xb7000000, 0x00002000);   /* back faces culled */
    emit(&l, (uintptr_t)G_CHRLIGHT_EXT << 24 | 1, (uintptr_t)&chrLight);   /* round 32c: lit when the set is on */
    for (s32 i = 0; i < num; i++) {
        const struct ctri *t = &tris[list[i]];
        Vtx rv[3];
        u8 idx[3];
        s32 ok = 1, need = 0;

        memset(rv, 0, sizeof(rv));
        if (t->tex != curtex) {
            batchFlush(&l, vb, &b, &curmtx, loads);
            emit(&l, alpha ? 0xbb002801 : 0xbb003001, 0xffffffff); /* G_TEXTURE on */
            emitTexture(&l, t->tex >= 0 && pics[t->tex].used ? standins[pics[t->tex].standin] : whiteTile);
            curtex = t->tex;
        }
        if (!b.shifted) {
            b.shiftu = floorf(t->v[0].uv[0]);
            b.shiftv = floorf(t->v[0].uv[1]);
            b.shifted = 1;
        }
        for (s32 pass = 0; pass < 2; pass++) {
            ok = 1;
            for (s32 k = 0; k < 3 && ok; k++) {
                const struct cvert *cv = &t->v[k];
                const f32 *rest = rig->rest[cv->mtx];
                ok = clampS16(cv->pos[0] - rest[0], &rv[k].v.ob[0]) && clampS16(cv->pos[1] - rest[1], &rv[k].v.ob[1])
                    && clampS16(cv->pos[2] - rest[2], &rv[k].v.ob[2])
                    && clampS16((cv->uv[0] - b.shiftu) * TILE_SCALE, &rv[k].v.tc[0])
                    && clampS16((cv->uv[1] - b.shiftv) * TILE_SCALE, &rv[k].v.tc[1]);
                rv[k].v.cn[0] = (u8)(cv->argb >> 16);
                rv[k].v.cn[1] = (u8)(cv->argb >> 8);
                rv[k].v.cn[2] = (u8)cv->argb;
                rv[k].v.cn[3] = (u8)(cv->argb >> 24);
            }
            if (ok || pass == 1) {
                break;
            }
            batchFlush(&l, vb, &b, &curmtx, loads);
            b.shiftu = floorf((t->v[0].uv[0] + t->v[1].uv[0] + t->v[2].uv[0]) / 3.0f);
            b.shiftv = floorf((t->v[0].uv[1] + t->v[1].uv[1] + t->v[2].uv[1]) / 3.0f);
            b.shifted = 1;
        }
        if (!ok) {
            (*dropped)++;
            continue;
        }
        for (s32 k = 0; k < 3; k++) {
            if (batchFind(&b, &rv[k], t->v[k].mtx) < 0) {
                need++;
            }
        }
        if (b.numv + need > BATCHVERTS || b.numtris >= 64) {
            const f32 su = b.shiftu, sv = b.shiftv;
            batchFlush(&l, vb, &b, &curmtx, loads);
            b.shiftu = su;
            b.shiftv = sv;
            b.shifted = 1;
        }
        for (s32 k = 0; k < 3; k++) {
            s32 at = batchFind(&b, &rv[k], t->v[k].mtx);
            if (at < 0) {
                at = b.numv++;
                b.v[at] = rv[k];
                b.mtx[at] = t->v[k].mtx;
                memcpy(b.nrm[at], t->v[k].nrm, sizeof(b.nrm[at]));
            }
            idx[k] = (u8)at;
        }
        b.tris[b.numtris][0] = idx[0];
        b.tris[b.numtris][1] = idx[1];
        b.tris[b.numtris][2] = idx[2];
        b.numtris++;
    }
    batchFlush(&l, vb, &b, &curmtx, loads);
    emit(&l, (uintptr_t)G_CHRLIGHT_EXT << 24, 0);   /* round 32c: the lights as they were */
    emit(&l, 0xba001001, 0x00000000);   /* texture LOD off again */
    emit(&l, 0xb7000000, 0x00002000);   /* culling back on */
    emit(&l, 0xb8000000, 0x00000000);   /* G_ENDDL */
    *bytes += (u32)l.cap * (u32)sizeof(Gfx);
    return l.g;
}

/* ------------------------------------------------------------ blood (D-175) */

static void freePaint(struct paintmap *pm)
{
    if (pm) {
        free(pm->map);
        free(pm);
    }
}

/* An N64 triangle of a paintable node, in the model's rest. */
struct ntri {
    u8 src, lod;
    u16 vi[3];
    f32 p[3][3];
    f32 lo[3], hi[3];
};

/* A list node's Primary is a SEGMENT token (segment 5, SPSEGMENT_MODEL_COL1:
 * an offset into the model file), resolved against BaseAddr - the file's
 * GUEST address (docs/pointer-contract.md: model.c writes fileramaddr) - as
 * fast3d resolves it once model.c has loaded segment 5 with that base. */
static const Gfx *n64List(const ModelRoData_DisplayList_CollisionRecord *ro)
{
    const uintptr_t tok = (uintptr_t)ro->Primary;
    const u32 base = GUEST_ADDR(ro->BaseAddr);
    if (tok == 0 || tok > 0xffffffffu || ((tok >> 24) & 0xff) != SPSEGMENT_MODEL_COL1 || base == 0) {
        return NULL;
    }
    return (const Gfx *)((const u8 *)GUEST_PTR(base) + (tok & 0xffffff));
}

/* The node's own list read for its triangles: G_MTX (segment 3) names the
 * joint each G_VTX batch (segment 4, the node's Vertices[]) rides, so each
 * vertex is its own coordinates plus that joint's rest - as the HD vertices
 * are written (writeList). GoldenEye's TRI4 (0xb1) and TRI1 (0xbf). */
static void readN64Tris(ModelNode *node, s32 src, s32 lod, s32 firstmtx, const struct rig *rig, struct ntri **tris,
                        s32 *num, s32 *cap)
{
    const ModelRoData_DisplayList_CollisionRecord *ro = &node->Data->DisplayListCollisions;
    const Gfx *g = n64List(ro);
    s32 slot[16], slotm[16], curm = firstmtx;

    for (s32 i = 0; i < 16; i++) {
        slot[i] = -1;
        slotm[i] = -1;
    }
    for (s32 steps = 0; g && steps < 8192; steps++, g++) {
        const u32 w0 = (u32)g->words.w0, w1 = (u32)g->words.w1;
        const u32 op = w0 >> 24;
        s32 t[4][3], nt = 0;
        if (op == 0xb8) {
            break;
        }
        if (op == 0x01 && (w1 >> 24) == SPSEGMENT_MODEL_MTX) {
            curm = (s32)((w1 & 0xffffff) / sizeof(Mtx));
            continue;
        }
        if (op == 0x04 && (w1 >> 24) == SPSEGMENT_MODEL_VTX) {
            const s32 n = (s32)((w0 >> 20) & 0xf) + 1, v0 = (s32)((w0 >> 16) & 0xf);
            const s32 base = (s32)((w1 & 0xffffff) / sizeof(Vertex));
            for (s32 k = 0; k < n && v0 + k < 16; k++) {
                slot[v0 + k] = base + k;
                slotm[v0 + k] = curm;
            }
            continue;
        }
        if (op == 0xb1) {
            for (s32 k = 0; k < 4; k++) {
                t[nt][0] = (s32)((w1 >> (k * 8)) & 0xf);
                t[nt][1] = (s32)((w1 >> (k * 8 + 4)) & 0xf);
                t[nt][2] = (s32)((w0 >> (k * 4)) & 0xf);
                if (t[nt][0] != t[nt][1] || t[nt][1] != t[nt][2]) {
                    nt++;
                }
            }
        } else if (op == 0xbf) {
            t[0][0] = (s32)(((w1 >> 16) & 0xff) / 10);
            t[0][1] = (s32)(((w1 >> 8) & 0xff) / 10);
            t[0][2] = (s32)((w1 & 0xff) / 10);
            nt = 1;
        }
        for (s32 k = 0; k < nt; k++) {
            struct ntri nn;
            s32 ok = 1;
            memset(&nn, 0, sizeof(nn));
            for (s32 c = 0; c < 3 && ok; c++) {
                const s32 sl = t[k][c];
                const s32 vi = sl < 16 ? slot[sl] : -1;
                const s32 m = sl < 16 ? slotm[sl] : -1;
                ok = vi >= 0 && vi < ro->numVertices && m >= 0 && m < MAXMTX && rig->hasrest[m];
                if (!ok) {
                    break;
                }
                nn.vi[c] = (u16)vi;
                nn.p[c][0] = ro->Vertices[vi].coord.x + rig->rest[m][0];
                nn.p[c][1] = ro->Vertices[vi].coord.y + rig->rest[m][1];
                nn.p[c][2] = ro->Vertices[vi].coord.z + rig->rest[m][2];
            }
            if (!ok) {
                continue;
            }
            for (s32 a = 0; a < 3; a++) {
                nn.lo[a] = fminf(nn.p[0][a], fminf(nn.p[1][a], nn.p[2][a]));
                nn.hi[a] = fmaxf(nn.p[0][a], fmaxf(nn.p[1][a], nn.p[2][a]));
            }
            nn.src = (u8)src;
            nn.lod = (u8)lod;
            if (*num >= *cap) {
                const s32 nc = *cap ? *cap * 2 : 512;
                struct ntri *r = realloc(*tris, sizeof(*r) * (size_t)nc);
                if (!r) {
                    return;
                }
                *tris = r;
                *cap = nc;
            }
            (*tris)[(*num)++] = nn;
        }
    }
}

/* Ericson, Real-Time Collision Detection 5.1.5: the point of triangle abc
 * nearest p, as barycentric weights; returns the squared distance. */
static f32 nearestOnTri(const f32 *p, const f32 *a, const f32 *b, const f32 *c, f32 *bary)
{
    f32 ab[3], ac[3], ap[3], bp[3], cp[3], q[3];
    for (s32 k = 0; k < 3; k++) {
        ab[k] = b[k] - a[k];
        ac[k] = c[k] - a[k];
        ap[k] = p[k] - a[k];
        bp[k] = p[k] - b[k];
        cp[k] = p[k] - c[k];
    }
#define DOT(x, y) ((x)[0] * (y)[0] + (x)[1] * (y)[1] + (x)[2] * (y)[2])
    const f32 d1 = DOT(ab, ap), d2 = DOT(ac, ap), d3 = DOT(ab, bp), d4 = DOT(ac, bp), d5 = DOT(ab, cp), d6 = DOT(ac, cp);
#undef DOT
    const f32 vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
    f32 u, v, w;
    if (d1 <= 0.0f && d2 <= 0.0f) {
        u = 1.0f; v = 0.0f; w = 0.0f;
    } else if (d3 >= 0.0f && d4 <= d3) {
        u = 0.0f; v = 1.0f; w = 0.0f;
    } else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        v = d1 / (d1 - d3); u = 1.0f - v; w = 0.0f;
    } else if (d6 >= 0.0f && d5 <= d6) {
        u = 0.0f; v = 0.0f; w = 1.0f;
    } else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        w = d2 / (d2 - d6); u = 1.0f - w; v = 0.0f;
    } else if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); u = 0.0f; v = 1.0f - w;
    } else {
        const f32 den = 1.0f / (va + vb + vc);
        v = vb * den; w = vc * den; u = 1.0f - v - w;
    }
    f32 d = 0.0f;
    for (s32 k = 0; k < 3; k++) {
        q[k] = u * a[k] + v * b[k] + w * c[k] - p[k];
        d += q[k] * q[k];
    }
    bary[0] = u;
    bary[1] = v;
    bary[2] = w;
    return d;
}

/* ------------------------------------------------------------ the head's seat (D-245) */

/* Horn (1987): the rotation turning the centred points a onto b best, from
 * their cross-covariance S[i][j] = sum a_i b_j - the largest eigenvector of
 * his 4x4 matrix (Jacobi), as a unit quaternion. */
static void hornRotation(const f64 S[3][3], f64 R[3][3])
{
    f64 N[4][4] = {
        { S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
        { S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
        { S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
        { S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] },
    };
    f64 V[4][4];
    for (s32 i = 0; i < 4; i++) {
        for (s32 j = 0; j < 4; j++) {
            V[i][j] = i == j ? 1.0 : 0.0;
        }
    }
    for (s32 sweep = 0; sweep < 50; sweep++) {
        f64 off = 0.0;
        for (s32 p = 0; p < 4; p++) {
            for (s32 q = p + 1; q < 4; q++) {
                off += N[p][q] * N[p][q];
            }
        }
        if (off < 1e-18) {
            break;
        }
        for (s32 p = 0; p < 4; p++) {
            for (s32 q = p + 1; q < 4; q++) {
                if (__builtin_fabs(N[p][q]) < 1e-30) {
                    continue;
                }
                const f64 th = (N[q][q] - N[p][p]) / (2.0 * N[p][q]);
                const f64 t = (th >= 0.0 ? 1.0 : -1.0) / (__builtin_fabs(th) + __builtin_sqrt(th * th + 1.0));
                const f64 c = 1.0 / __builtin_sqrt(t * t + 1.0), s = t * c;
                for (s32 k = 0; k < 4; k++) {
                    const f64 kp = N[k][p], kq = N[k][q];
                    N[k][p] = c * kp - s * kq;
                    N[k][q] = s * kp + c * kq;
                }
                for (s32 k = 0; k < 4; k++) {
                    const f64 pk = N[p][k], qk = N[q][k];
                    N[p][k] = c * pk - s * qk;
                    N[q][k] = s * pk + c * qk;
                }
                for (s32 k = 0; k < 4; k++) {
                    const f64 kp = V[k][p], kq = V[k][q];
                    V[k][p] = c * kp - s * kq;
                    V[k][q] = s * kp + c * kq;
                }
            }
        }
    }
    s32 best = 0;
    for (s32 i = 1; i < 4; i++) {
        if (N[i][i] > N[best][best]) {
            best = i;
        }
    }
    const f64 w = V[0][best], x = V[1][best], y = V[2][best], z = V[3][best];
    R[0][0] = w * w + x * x - y * y - z * z; R[0][1] = 2 * (x * y - w * z);         R[0][2] = 2 * (x * z + w * y);
    R[1][0] = 2 * (x * y + w * z);         R[1][1] = w * w - x * x + y * y - z * z; R[1][2] = 2 * (y * z - w * x);
    R[2][0] = 2 * (x * z - w * y);         R[2][1] = 2 * (y * z + w * x);         R[2][2] = w * w - x * x - y * y + z * z;
}

static f32 nearestOnTri(const f32 *p, const f32 *a, const f32 *b, const f32 *c, f32 *bary);

/* Trimmed ICP: the rigid turn + shift (about `pivot`) taking the points onto
 * the triangles' surface - the closest 80 % each round, 40 rounds. Returns
 * the RMS distance of the kept points after (and *before at identity). */
static f32 alignToTris(const f32 (*pts)[3], s32 np, const struct ntri *tris, s32 nt, const f32 *pivot, f32 R[3][3],
                       f32 tr[3], f32 *before)
{
    f32 (*cur)[3] = malloc(sizeof(*cur) * (size_t)np);
    f32 (*hit)[3] = malloc(sizeof(*hit) * (size_t)np);
    f32 *dist = malloc(sizeof(*dist) * (size_t)np), *sorted = malloc(sizeof(*sorted) * (size_t)np);
    f64 Racc[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, tacc[3] = { 0, 0, 0 };
    f32 rms = -1.0f;

    *before = -1.0f;
    if (!cur || !hit || !dist || !sorted || np < 8 || nt < 4) {
        goto out;
    }
    for (s32 iter = 0; iter <= 40; iter++) {
        for (s32 i = 0; i < np; i++) {
            for (s32 k = 0; k < 3; k++) {
                const f64 rel0 = pts[i][0] - pivot[0], rel1 = pts[i][1] - pivot[1], rel2 = pts[i][2] - pivot[2];
                cur[i][k] = (f32)(Racc[k][0] * rel0 + Racc[k][1] * rel1 + Racc[k][2] * rel2 + tacc[k] + pivot[k]);
            }
            f32 best = 1e30f, bary[3];
            for (s32 t = 0; t < nt; t++) {
                f32 out = 0.0f;
                for (s32 k = 0; k < 3; k++) {
                    const f32 dk = cur[i][k] < tris[t].lo[k] ? tris[t].lo[k] - cur[i][k]
                                 : cur[i][k] > tris[t].hi[k] ? cur[i][k] - tris[t].hi[k] : 0.0f;
                    out += dk * dk;
                }
                if (out >= best) {
                    continue;
                }
                const f32 d = nearestOnTri(cur[i], tris[t].p[0], tris[t].p[1], tris[t].p[2], bary);
                if (d < best) {
                    best = d;
                    for (s32 k = 0; k < 3; k++) {
                        hit[i][k] = bary[0] * tris[t].p[0][k] + bary[1] * tris[t].p[1][k] + bary[2] * tris[t].p[2][k];
                    }
                }
            }
            dist[i] = sorted[i] = best;
        }
        /* the 80th percentile by a partial selection sort is cheap enough at build */
        const s32 keep = np * 8 / 10;
        for (s32 i = 0; i <= keep && i < np; i++) {
            s32 m = i;
            for (s32 j = i + 1; j < np; j++) {
                if (sorted[j] < sorted[m]) {
                    m = j;
                }
            }
            const f32 tmp = sorted[i];
            sorted[i] = sorted[m];
            sorted[m] = tmp;
        }
        const f32 cut = sorted[keep < np ? keep : np - 1];
        f64 ca[3] = { 0, 0, 0 }, cb[3] = { 0, 0, 0 }, S[3][3], dR[3][3], sum = 0.0;
        s32 n = 0;
        for (s32 i = 0; i < np; i++) {
            if (dist[i] <= cut) {
                for (s32 k = 0; k < 3; k++) {
                    ca[k] += cur[i][k];
                    cb[k] += hit[i][k];
                }
                sum += dist[i];
                n++;
            }
        }
        if (n < 4) {
            break;
        }
        rms = sqrtf((f32)(sum / n));
        if (iter == 0) {
            *before = rms;
        }
        if (iter == 40) {
            break;
        }
        for (s32 k = 0; k < 3; k++) {
            ca[k] /= n;
            cb[k] /= n;
        }
        memset(S, 0, sizeof(S));
        for (s32 i = 0; i < np; i++) {
            if (dist[i] <= cut) {
                for (s32 r = 0; r < 3; r++) {
                    for (s32 c = 0; c < 3; c++) {
                        S[r][c] += (cur[i][r] - ca[r]) * (hit[i][c] - cb[c]);
                    }
                }
            }
        }
        hornRotation((const f64 (*)[3])S, dR);
        /* new = dR (cur - ca) + cb, cur = Racc rel + tacc + pivot */
        f64 nR[3][3], nt3[3];
        for (s32 r = 0; r < 3; r++) {
            for (s32 c = 0; c < 3; c++) {
                nR[r][c] = dR[r][0] * Racc[0][c] + dR[r][1] * Racc[1][c] + dR[r][2] * Racc[2][c];
            }
        }
        for (s32 r = 0; r < 3; r++) {
            const f64 q0 = tacc[0] + pivot[0] - ca[0], q1 = tacc[1] + pivot[1] - ca[1], q2 = tacc[2] + pivot[2] - ca[2];
            nt3[r] = dR[r][0] * q0 + dR[r][1] * q1 + dR[r][2] * q2 + cb[r] - pivot[r];
        }
        memcpy(Racc, nR, sizeof(Racc));
        memcpy(tacc, nt3, sizeof(tacc));
    }
out:
    for (s32 r = 0; r < 3; r++) {
        for (s32 c = 0; c < 3; c++) {
            R[r][c] = (f32)Racc[r][c];
        }
        tr[r] = (f32)tacc[r];
    }
    free(cur);
    free(hit);
    free(dist);
    free(sorted);
    return rms;
}

#define PAINT_REACH 48.0f   /* model units: an HD vertex further than this from every N64 triangle stays unpainted */

/* D-274 (round 31b): buildPaint's nearest-triangle search was every HD vertex
 * against every N64 triangle (4-5 ms of a 6 ms body build on the oracle; on the
 * phone it was most of Bond's 42-52 ms intro-swirl frame and every mid-level
 * guard spawn). A uniform grid of cell PAINT_REACH over the triangles' boxes
 * hands each vertex only the triangles whose box meets the cube of half-side
 * PAINT_REACH around it -- a superset of every triangle the brute loop could
 * accept, because its box test (out >= best, best starting at PAINT_REACH^2)
 * rejects any triangle whose box misses that cube. The candidates are visited
 * in ascending index with the same tests, so the map is bit-identical
 * (GE_BEANPAINTBRUTE=1 runs the old loop for the A/B; GE_BEANCHRLOG prints a
 * hash of each map). */
struct paintgrid {
    f32 org[3];
    s32 dim[3];
    s32 *start;   /* dim0*dim1*dim2 + 1 offsets into idx */
    s32 *idx;     /* triangle indices, ascending within a cell */
    s32 *stamp;   /* per triangle: the last query that collected it */
    s32 *cand;    /* one query's candidates */
    s32 query;
};

static void paintGridFree(struct paintgrid *g);

static s32 paintCell(f32 v, f32 org, s32 dim)
{
    s32 c = (s32)floorf((v - org) / PAINT_REACH);
    return c < 0 ? 0 : c >= dim ? dim - 1 : c;
}

static s32 paintGridBuild(struct paintgrid *g, const struct ntri *tris, s32 num)
{
    memset(g, 0, sizeof(*g));
    f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (s32 t = 0; t < num; t++) {
        for (s32 k = 0; k < 3; k++) {
            lo[k] = tris[t].lo[k] < lo[k] ? tris[t].lo[k] : lo[k];
            hi[k] = tris[t].hi[k] > hi[k] ? tris[t].hi[k] : hi[k];
        }
    }
    s64 cells = 1;
    for (s32 k = 0; k < 3; k++) {
        g->org[k] = lo[k];
        const f32 ext = hi[k] - lo[k];
        g->dim[k] = ext >= 0.0f && ext < 1e6f ? (s32)(ext / PAINT_REACH) + 1 : 1;
        cells *= g->dim[k];
    }
    if (num <= 0 || cells > (1 << 20)) {
        return 0;
    }
    s32 ok = 0;
    g->start = calloc((size_t)cells + 1, sizeof(s32));
    g->stamp = malloc(sizeof(s32) * (size_t)num);
    g->cand = malloc(sizeof(s32) * (size_t)num);
    if (!g->start || !g->stamp || !g->cand) {
        goto out;
    }
    for (s32 pass = 0; pass < 2; pass++) {
        for (s32 t = 0; t < num; t++) {
            s32 a[3], b[3];
            for (s32 k = 0; k < 3; k++) {
                a[k] = paintCell(tris[t].lo[k], g->org[k], g->dim[k]);
                b[k] = paintCell(tris[t].hi[k], g->org[k], g->dim[k]);
            }
            for (s32 z = a[2]; z <= b[2]; z++) {
                for (s32 y = a[1]; y <= b[1]; y++) {
                    for (s32 x = a[0]; x <= b[0]; x++) {
                        const s32 c = (z * g->dim[1] + y) * g->dim[0] + x;
                        if (pass == 0) {
                            g->start[c + 1]++;
                        } else {
                            g->idx[g->start[c]++] = t;   /* start[c] is the fill cursor here */
                        }
                    }
                }
            }
        }
        if (pass == 0) {
            for (s64 c = 0; c < cells; c++) {
                g->start[c + 1] += g->start[c];
            }
            g->idx = malloc(sizeof(s32) * (size_t)(g->start[cells] > 0 ? g->start[cells] : 1));
            if (!g->idx) {
                goto out;
            }
        }
    }
    /* the fill moved every start[c] to start[c+1]'s value: shift back */
    for (s64 c = cells; c > 0; c--) {
        g->start[c] = g->start[c - 1];
    }
    g->start[0] = 0;
    for (s32 t = 0; t < num; t++) {
        g->stamp[t] = -1;
    }
    ok = 1;
out:
    if (!ok) {
        paintGridFree(g);
    }
    return ok;
}

static int paintCompareIdx(const void *a, const void *b)
{
    const s32 x = *(const s32 *)a, y = *(const s32 *)b;
    return x < y ? -1 : x > y;
}

/* the candidates for p, ascending; returns their count */
static s32 paintGridQuery(struct paintgrid *g, const f32 *p)
{
    s32 a[3], b[3], n = 0;
    for (s32 k = 0; k < 3; k++) {
        a[k] = paintCell(p[k] - PAINT_REACH, g->org[k], g->dim[k]);
        b[k] = paintCell(p[k] + PAINT_REACH, g->org[k], g->dim[k]);
    }
    g->query++;
    for (s32 z = a[2]; z <= b[2]; z++) {
        for (s32 y = a[1]; y <= b[1]; y++) {
            for (s32 x = a[0]; x <= b[0]; x++) {
                const s32 c = (z * g->dim[1] + y) * g->dim[0] + x;
                for (s32 i = g->start[c]; i < g->start[c + 1]; i++) {
                    const s32 t = g->idx[i];
                    if (g->stamp[t] != g->query) {
                        g->stamp[t] = g->query;
                        g->cand[n++] = t;
                    }
                }
            }
        }
    }
    qsort(g->cand, (size_t)n, sizeof(s32), paintCompareIdx);
    return n;
}

static void paintGridFree(struct paintgrid *g)
{
    free(g->start);
    free(g->idx);
    free(g->stamp);
    free(g->cand);
    memset(g, 0, sizeof(*g));
}

/* The entry's paintable N64 nodes (every DLCOLLISION list with vertices) and
 * each HD node's solid vertices mapped onto them. firstmtx: the matrix a
 * list's vertices ride before its first G_MTX (-1: the node's own). */
static void buildPaint(struct centry *e, struct cnode *hd, s32 numhd, const struct rig *rig, s32 firstmtx)
{
    struct ntri *tris = NULL;
    s32 num = 0, cap = 0;
    u64 t0 = sysGetMicroseconds();

    e->numpsrc = 0;
    for (s32 i = 0; i < e->numnodes && e->numpsrc < MAXPSRC; i++) {
        ModelNode *n = e->nodes[i].node;
        if ((n->Opcode & 0xff) != MODELNODE_OPCODE_DLCOLLISION || !n->Data) {
            continue;
        }
        const ModelRoData_DisplayList_CollisionRecord *ro = &n->Data->DisplayListCollisions;
        if (!ro->Primary || !ro->Vertices || ro->numVertices <= 0) {
            continue;
        }
        const s32 fm = firstmtx >= 0 ? firstmtx : modelFindNodeMtxIndex(n, 0);
        readN64Tris(n, e->numpsrc, e->nodes[i].lod, fm, rig, &tris, &num, &cap);
        e->psrc[e->numpsrc++] = n;
    }
    s32 mapped = 0, total = 0;
    struct paintgrid grid;
    /* below ~128 triangles the brute loop is the faster of the two */
    const s32 useGrid = num >= 128 && !getenv("GE_BEANPAINTBRUTE") && paintGridBuild(&grid, tris, num);
    u64 mapHash = 0x9e3779b97f4a7c15ull;
    for (s32 i = 0; i < numhd && num > 0; i++) {
        struct cnode *cn = &hd[i];
        if (cn->shared || !cn->vmtx || cn->solidvtx <= 0 || !cn->out.vtx) {
            continue;
        }
        struct paintmap *pm = calloc(1, sizeof(*pm));
        struct pmap *map = pm ? calloc((size_t)cn->solidvtx * 2, sizeof(*map)) : NULL;
        if (!map) {
            free(pm);
            continue;
        }
        pm->numv = cn->solidvtx;
        pm->map = map;
        s32 any = 0;
        for (s32 v = 0; v < cn->solidvtx; v++) {
            const s32 m = cn->vmtx[v];
            f32 p[3];
            map[v * 2].src = map[v * 2 + 1].src = 0xff;
            if (m < 0 || m >= MAXMTX) {
                continue;
            }
            for (s32 k = 0; k < 3; k++) {
                p[k] = cn->out.vtx[v].v.ob[k] + rig->rest[m][k];
            }
            total++;
            const s32 ncand = useGrid ? paintGridQuery(&grid, p) : num;
            for (s32 lod = 0; lod < 2; lod++) {
                f32 best = PAINT_REACH * PAINT_REACH, bary[3], bb[3] = { 0, 0, 0 };
                s32 bt = -1;
                for (s32 ci = 0; ci < ncand; ci++) {
                    const s32 t = useGrid ? grid.cand[ci] : ci;
                    const struct ntri *nt = &tris[t];
                    f32 out = 0.0f;
                    if (nt->lod != lod) {
                        continue;
                    }
                    for (s32 k = 0; k < 3; k++) {
                        const f32 dk = p[k] < nt->lo[k] ? nt->lo[k] - p[k] : p[k] > nt->hi[k] ? p[k] - nt->hi[k] : 0.0f;
                        out += dk * dk;
                    }
                    if (out >= best) {
                        continue;
                    }
                    const f32 d = nearestOnTri(p, nt->p[0], nt->p[1], nt->p[2], bary);
                    if (d < best) {
                        best = d;
                        bt = t;
                        memcpy(bb, bary, sizeof(bb));
                    }
                }
                if (bt < 0) {
                    continue;
                }
                struct pmap *pmv = &map[v * 2 + lod];
                const s32 w0 = (s32)roundf(bb[0] * 255.0f), w1 = (s32)roundf(bb[1] * 255.0f);
                const s32 w0c = w0 < 0 ? 0 : w0 > 255 ? 255 : w0;
                const s32 w1c = w1 < 0 ? 0 : w1 > 255 - w0c ? 255 - w0c : w1;
                pmv->src = tris[bt].src;
                pmv->w[0] = (u8)w0c;
                pmv->w[1] = (u8)w1c;
                pmv->w[2] = (u8)(255 - w0c - w1c);
                memcpy(pmv->vi, tris[bt].vi, sizeof(pmv->vi));
                s32 seen = 0;
                for (s32 k = 0; k < pm->numsrcs; k++) {
                    seen |= pm->srcs[k] == pmv->src;
                }
                if (!seen && pm->numsrcs < MAXPSRC) {
                    pm->srcs[pm->numsrcs++] = pmv->src;
                }
                any = 1;
            }
            mapped += map[v * 2].src != 0xff || map[v * 2 + 1].src != 0xff;
        }
        for (s32 v = 0; v < cn->solidvtx * 2; v++) {
            const struct pmap *q = &map[v];
            mapHash = (mapHash ^ ((u64)q->src | (u64)q->w[0] << 8 | (u64)q->w[1] << 16 | (u64)q->w[2] << 24
                                  | (u64)q->vi[0] << 32 | (u64)q->vi[1] << 48)) * 0x100000001b3ull;
            mapHash = (mapHash ^ q->vi[2]) * 0x100000001b3ull;
        }
        if (!any) {
            freePaint(pm);
            continue;
        }
        cn->paint = pm;
    }
    /* the near/far twins draw the first one's lists: and its paint */
    for (s32 i = 0; i < numhd; i++) {
        for (s32 j = 0; hd[i].shared && j < i; j++) {
            if (!hd[j].shared && hd[j].out.vtx == hd[i].out.vtx) {
                hd[i].paint = hd[j].paint;
            }
        }
    }
    for (s32 i = 0; i < numhd; i++) {
        free(hd[i].vmtx);   /* build only */
        hd[i].vmtx = NULL;
    }
    if (logOn()) {
        sysLogPrintf(LOG_NOTE, "beanchr:   blood: %d N64 nodes, %d triangles; %d of %d solid HD vertices reach one; %.2f ms "
                     "(%s, map %016llx)", e->numpsrc, num, mapped, total, msSince(t0), useGrid ? "grid" : "brute",
                     (unsigned long long)mapHash);
    }
    if (useGrid) {
        paintGridFree(&grid);
    }
    free(tris);
}

/* D-274 (round 31b): the blood map of solo Bond's cutscene body and head is
 * never read. Those are bondview2.c's copies (beanChrAlias), drawn only in the
 * intro swirl and the outro, where nothing can shoot him -- and their build
 * landed on the swirl's frame, the 42-52 ms intro frame of every HD level on
 * the phone (round 31a), most of it this map (4-5 ms of a 6 ms body build on
 * the oracle). An alias copy gets no map (paintedOut() then draws the HD
 * lists unpainted, which is what a never-painted instance draws anyway).
 * GE_BEANPAINTALIAS=1 builds it as before (A/B). */
static s32 isAliasCopy(const ModelFileHeader *hdr)
{
    for (s32 i = 0; hdr && i < numAliases; i++) {
        if (aliases[i].copy == hdr) {
            return !aliasStale(hdr);
        }
    }
    return 0;
}

static void buildPaintUnlessCutscene(struct centry *e, struct cnode *hd, s32 numhd, const struct rig *rig,
                                     s32 firstmtx)
{
    if (isAliasCopy(e->hdr) && !getenv("GE_BEANPAINTALIAS")) {
        for (s32 i = 0; i < numhd; i++) {
            free(hd[i].vmtx);   /* build only, as buildPaint() frees it */
            hd[i].vmtx = NULL;
        }
        e->numpsrc = 0;
        if (logOn()) {
            sysLogPrintf(LOG_NOTE, "beanchr:   blood: none (solo Bond's cutscene copy, D-274)");
        }
        return;
    }
    buildPaint(e, hd, numhd, rig, firstmtx);
}

/* The instance's copy of a painted node's vertices: its alphas from the
 * N64 instance's own (the copy chrCreateBloodStain made). A copy lives until
 * its paint changes, then is kept PAINT_KEEP frames more (fast3d and the
 * camera-latch rerun read the frame's lists after it is built). */
#define PAINT_KEEP 60
struct paintinst {
    const Model *model;
    const ModelFileHeader *hdr, *body;
    s32 idx;                 /* node index, -1 the head's lists */
    u8 *snap;                /* the painted sources' alphas it was made from */
    s32 snaplen;
    u32 used;
    struct beanmodelnode out;
};
static struct paintinst *pinst;
static s32 numPinst, capPinst;
static struct {
    Vtx *vtx;
    u32 frame;
} *pdead;
static s32 numPdead, capPdead;
static u32 paintFrame;
static u32 paintCopies, paintDraws;   /* dev counters (GE_BEANCHRLOG) */

static void paintRetire(Vtx *v)
{
    if (!v) {
        return;
    }
    if (numPdead >= capPdead) {
        const s32 cap = capPdead ? capPdead * 2 : 64;
        void *n = realloc(pdead, sizeof(*pdead) * (size_t)cap);
        if (!n) {
            return;   /* leaked rather than freed under fast3d */
        }
        pdead = n;
        capPdead = cap;
    }
    pdead[numPdead].vtx = v;
    pdead[numPdead].frame = paintFrame;
    numPdead++;
}

/* rwdata of an N64 node of this instance: a body's own, or (head) the head
 * placeholder's (modelGetNodeRwData() walking the per-instance Parent link,
 * done here from the body's placeholder so the link need not be current). */
static union ModelRwData *headRwData(Model *model, const struct centry *body, const struct centry *head, ModelNode *n,
                                     u32 index);

static ModelRwData_DisplayList_CollisionRecord *paintRw(Model *model, const struct centry *in, const struct centry *body,
                                                         ModelNode *n)
{
    if (!body) {
        return &modelGetNodeRwData(model, n)->DisplayListCollisions;
    }
    union ModelRwData *rw = headRwData(model, body, in, n, n->Data->DisplayListCollisions.RwDataIndex);
    return rw ? &rw->DisplayListCollisions : NULL;
}

/* A head file's node's rwdata for this instance, through the body's own
 * head placeholder (D-181); NULL unless the placeholder holds this head. */
static union ModelRwData *headRwData(Model *model, const struct centry *body, const struct centry *head, ModelNode *n,
                                     u32 index)
{
    (void)n;
    if (!body || !body->headspot || !head) {
        return NULL;
    }
    union ModelRwData *hp = modelGetNodeRwData(model, body->headspot);
    if (!hp || hp->HeadPlaceholder.ModelFileHeader != head->hdr || !hp->HeadPlaceholder.RwDatas) {
        return NULL;
    }
    /* D52: the pool is word-indexed, as modelGetNodeRwData()'s */
    u32 *data = (u32 *)hp->HeadPlaceholder.RwDatas;
    return (union ModelRwData *)&data[index];
}

static u8 paintScratch[16384];

static const struct beanmodelnode *paintedOut(Model *model, const struct centry *in, const struct centry *body,
                                              const struct cnode *cn, s32 idx)
{
    const struct paintmap *pm = cn->paint;
    const Vertex *rv[MAXPSRC];
    s32 any = 0, len = 0;

    if (!pm || in->numpsrc <= 0) {
        return &cn->out;
    }
    memset(rv, 0, sizeof(rv));
    for (s32 k = 0; k < pm->numsrcs; k++) {
        const s32 s = pm->srcs[k];
        ModelNode *n = in->psrc[s];
        const ModelRoData_DisplayList_CollisionRecord *ro = &n->Data->DisplayListCollisions;
        ModelRwData_DisplayList_CollisionRecord *rw = paintRw(model, in, body, n);
        if (!rw || !rw->Vertices || rw->Vertices == ro->Vertices) {
            continue;   /* never painted: GoldenEye still draws the file's own */
        }
        if (len + ro->numVertices + 1 > (s32)sizeof(paintScratch)) {
            break;
        }
        rv[s] = rw->Vertices;
        paintScratch[len++] = (u8)s;
        for (s32 v = 0; v < ro->numVertices; v++) {
            paintScratch[len++] = rw->Vertices[v].a;
        }
        any = 1;
    }
    if (!any) {
        return &cn->out;
    }
    struct paintinst *pi = NULL;
    for (s32 i = 0; i < numPinst && !pi; i++) {
        if (pinst[i].model == model && pinst[i].hdr == in->hdr && pinst[i].body == in->body && pinst[i].idx == idx) {
            pi = &pinst[i];
        }
    }
    if (!pi) {
        if (numPinst >= capPinst) {
            const s32 cap = capPinst ? capPinst * 2 : 32;
            struct paintinst *n = realloc(pinst, sizeof(*n) * (size_t)cap);
            if (!n) {
                return &cn->out;
            }
            pinst = n;
            capPinst = cap;
        }
        pi = &pinst[numPinst++];
        memset(pi, 0, sizeof(*pi));
        pi->model = model;
        pi->hdr = in->hdr;
        pi->body = in->body;
        pi->idx = idx;
    }
    pi->used = paintFrame;
    if (pi->out.vtx && pi->snaplen == len && memcmp(pi->snap, paintScratch, (size_t)len) == 0) {
        paintDraws++;
        return &pi->out;
    }
    Vtx *v = malloc(sizeof(Vtx) * (size_t)(cn->numvtx > 0 ? cn->numvtx : 1));
    u8 *snap = malloc((size_t)len);
    if (!v || !snap) {
        free(v);
        free(snap);
        return &cn->out;
    }
    memcpy(v, cn->out.vtx, sizeof(Vtx) * (size_t)cn->numvtx);
    for (s32 i = 0; i < pm->numv && i < cn->numvtx; i++) {
        u32 a = 255;
        for (s32 l = 0; l < 2; l++) {
            const struct pmap *m = &pm->map[i * 2 + l];
            if (m->src == 0xff || !rv[m->src]) {
                continue;
            }
            const Vertex *src = rv[m->src];
            const u32 al = ((u32)m->w[0] * src[m->vi[0]].a + (u32)m->w[1] * src[m->vi[1]].a + (u32)m->w[2] * src[m->vi[2]].a
                            + 127) / 255;
            a = al < a ? al : a;
        }
        v[i].v.cn[3] = (u8)a;
    }
    if (getenv("GE_BEANPAINTLOG")) {
        /* round 30a dev (D-249): which list a paint copy is for (-1 the head's) and how much of it is painted */
        s32 painted = 0;
        for (s32 i = 0; i < cn->numvtx; i++) {
            painted += v[i].v.cn[3] < 250;
        }
        sysLogPrintf(LOG_NOTE, "beanchr: paint copy %s%s%s list %d: %d of %d vertices painted", in->file, in->body ? " on " : "",
                     in->body ? (fileOf(in->body) ? fileOf(in->body) : "?") : "", idx, painted, cn->numvtx);
    }
    paintRetire(pi->out.vtx);
    free(pi->snap);
    pi->snap = snap;
    memcpy(snap, paintScratch, (size_t)len);
    pi->snaplen = len;
    pi->out = cn->out;
    pi->out.vtx = v;
    paintCopies++;
    paintDraws++;
    return &pi->out;
}

/* Once a frame (beanChrFrame): free copies retired PAINT_KEEP frames ago;
 * an instance not drawn for a few seconds (dead and gone) is retired. */
static void paintSweep(void)
{
    s32 k = 0;
    paintFrame++;
    for (s32 i = 0; i < numPinst; i++) {
        if (paintFrame - pinst[i].used > 600) {
            paintRetire(pinst[i].out.vtx);
            free(pinst[i].snap);
            continue;
        }
        pinst[k++] = pinst[i];
    }
    numPinst = k;
    k = 0;
    for (s32 i = 0; i < numPdead; i++) {
        if (paintFrame - pdead[i].frame > PAINT_KEEP) {
            free(pdead[i].vtx);
            continue;
        }
        pdead[k++] = pdead[i];
    }
    numPdead = k;
}

/* A new level: every instance's copy retired (freed PAINT_KEEP frames on). */
static void paintLevel(void)
{
    for (s32 i = 0; i < numPinst; i++) {
        paintRetire(pinst[i].out.vtx);
        free(pinst[i].snap);
    }
    numPinst = 0;
}

static void freeEntry(struct centry *e)
{
    for (s32 i = 0; i < e->numnodes; i++) {
        if (e->nodes[i].shared) {
            continue;
        }
        free(e->nodes[i].out.vtx);
        free(e->nodes[i].out.solid);
        free(e->nodes[i].out.cut);
        free(e->nodes[i].out.xlu);
        free(e->nodes[i].vmtx);
        free(e->nodes[i].rgb);
        freePaint(e->nodes[i].paint);
    }
    free(e->nodes);
    free(e->headdraw.vmtx);
    free(e->headdraw.rgb);
    freePaint(e->headdraw.paint);
    free(e->headdraw.out.vtx);
    free(e->headdraw.out.solid);
    free(e->headdraw.out.cut);
    free(e->headdraw.out.xlu);
    for (s32 i = 0; i < e->numstandins; i++) {
        free(e->standins[i]);
    }
    free(e->standins);
    free(e->rig);
    memset(e, 0, sizeof(*e));
}

/* The coverage tally for the stage line (D-143's class). */
static struct {
    s32 bodies, heads;       /* built HD */
    s32 fellback;            /* entries left GoldenEye's own */
    s32 unmapped;            /* character files with no row (or a row left out) */
    s32 pictures, decoded;
    s32 refs;                /* unresolved references */
    s32 wholeN64;            /* characters seen drawn whole N64 for want of a head/body */
} ca;

/* Pictures: every one a triangle uses, decoded and bound to a stand-in. */
static s32 bindPictures(struct centry *e, struct beanlevel *bm, s32 numtex, const struct ctri *tris, s32 num,
                        struct pic **outpics, const char *source)
{
    struct pic *pics = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*pics));
    e->standins = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*e->standins));
    s32 failed = 0;
    if (!pics || !e->standins) {
        free(pics);
        return -1;
    }
    for (s32 t = 0; t < num; t++) {
        if (tris[t].tex >= 0 && tris[t].tex < numtex) {
            pics[tris[t].tex].used = 1;
        }
    }
    for (s32 p = 0; p < numtex; p++) {
        int32_t w, h;
        if (!pics[p].used) {
            continue;
        }
        ca.pictures++;
        uint8_t *rgba = beanLevelDecodeTexture(bm, p, &w, &h);
        const u32 total = rgba ? (u32)w * (u32)h : 0;
        u32 opaque = 0, rs = 0, gs = 0, bs = 0;
        for (u32 i = 0; i < total; i++) {
            opaque += rgba[i * 4 + 3] >= OPAQUE_ALPHA;
            rs += rgba[i * 4];
            gs += rgba[i * 4 + 1];
            bs += rgba[i * 4 + 2];
        }
        u16 *tile = newTile(total ? texel5551(rs / total, gs / total, bs / total) : 0xffff);
        if (!rgba) {
            int32_t sw = 0, sh = 0;
            beanLevelTextureSize(bm, p, &sw, &sh);
            sysLogPrintf(LOG_WARNING, "[xbla] unresolved: character %s (%s): picture %d (%dx%d) does not decode", e->file,
                         source, p, (int)sw, (int)sh);
            ca.refs++;
            failed++;
        }
        if (!tile || !rgba) {
            free(tile);
            texpackBeanPictureRelease(rgba);   /* D-317: it may be pinned (decoded ahead) */
            pics[p].used = 0;
            continue;
        }
        {
            const char *dd = getenv("GE_BEANHEADDUMP");   /* dev: the decoded picture as PPM + alpha PGM */
            if (dd && *dd) {
                char dp[1024];
                snprintf(dp, sizeof(dp), "%s/%s_pic%d_%dx%d.ppm", dd, e->file, (int)p, (int)w, (int)h);
                FILE *f = fopen(dp, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h);
                    for (u32 i = 0; i < total; i++) {
                        fwrite(&rgba[i * 4], 1, 3, f);
                    }
                    fclose(f);
                }
            }
        }
        pics[p].standin = (s16)e->numstandins;
        e->standins[e->numstandins++] = tile;
        pics[p].alpha = opaque < total;
        pics[p].soft = (u64)opaque * 100 < (u64)total * SOFT_PERCENT;
        e->picbytes += total * 4;
        ca.decoded++;
        texpackRegisterBeanPicture(tile, rgba, w, h);
    }
    *outpics = pics;
    return failed;
}

/* The model's display-list nodes (from the walk) with the joint each rides. */
static s32 collectNodes(const struct walk *w, const struct rig *rig, struct cnode *out, s32 max)
{
    s32 count = 0;
    for (s32 i = 0; i < w->num && count < max; i++) {
        ModelNode *n = w->n[i].node;
        const u32 type = n->Opcode & 0xff;
        if ((type != MODELNODE_OPCODE_DLCOLLISION && type != MODELNODE_OPCODE_DL) || !n->Data) {
            continue;
        }
        struct cnode *c = &out[count++];
        memset(c, 0, sizeof(*c));
        c->node = n;
        c->skel = -1;
        if (rig) {
            const s32 m = modelFindNodeMtxIndex(n, 0);
            for (s32 s = 0; s < SK_COUNT && c->skel < 0; s++) {
                if (s != SK_POSITION && rig->have[s] && rig->mtx[s] == m) {
                    c->skel = (s8)s;
                }
            }
        }
        c->candidate = !w->n[i].switched;
        c->lod = w->n[i].lod;
    }
    return count;
}

static u8 *litColours(const Vtx *v, const f32 *nrm, s32 num);

static void buildLists(struct centry *e, struct cnode *nodes, s32 numnodes, s32 bodymode, const struct rig *rig,
                       struct ctri *tris, s32 num, const struct pic *pics, s32 *outdrawn, s32 *outdropped, s32 *outloads,
                       s32 *outcounts)
{
    s32 *list = malloc(sizeof(s32) * (size_t)(num > 0 ? num : 1));
    s32 dropped = 0, loads = 0, drawn = 0;
    if (!list) {
        return;
    }
    for (s32 i = 0; i < numnodes; i++) {
        struct cnode *cn = &nodes[i];
        struct vtxbuf vb;
        memset(&vb, 0, sizeof(vb));
        if (!cn->candidate) {
            continue;
        }
        if (bodymode) {
            /* the near and far levels of detail of one joint draw the same
             * HD lists (GoldenEye draws one of the two) */
            s32 twin = -1;
            for (s32 j = 0; j < i && twin < 0; j++) {
                if (nodes[j].candidate && !nodes[j].shared && nodes[j].skel == cn->skel) {
                    twin = j;
                }
            }
            if (twin >= 0) {
                cn->out = nodes[twin].out;
                cn->rgb = nodes[twin].rgb;
                cn->numtris = nodes[twin].numtris;
                cn->numvtx = nodes[twin].numvtx;
                cn->solidvtx = nodes[twin].solidvtx;
                cn->shared = 1;
                continue;
            }
        }
        for (s32 kind = 0; kind < 3; kind++) {
            s32 n = 0;
            for (s32 t = 0; t < num; t++) {
                const struct ctri *tri = &tris[t];
                /* a body's node takes its joint's triangles; a head's lists
                 * take them all */
                if (bodymode && tri->skel != cn->skel) {
                    continue;
                }
                const s32 tex = tri->tex >= 0 && pics[tri->tex].used ? tri->tex : -1;
                const s32 xlu = tri->glass || (tex >= 0 && pics[tex].soft);
                const s32 cut = !xlu && tex >= 0 && pics[tex].alpha;
                if ((kind == 0 && !xlu && !cut) || (kind == 1 && cut) || (kind == 2 && xlu)) {
                    list[n++] = t;
                }
            }
            for (s32 t = 0; t < n; t++) {
                if (tris[list[t]].tex >= 0 && !pics[tris[list[t]].tex].used) {
                    tris[list[t]].tex = -1;
                }
            }
            outcounts[kind] += n;
            cn->numtris += n;
            Gfx *g = writeList(&vb, tris, list, n, rig, pics, e->standins, kind != 0, &dropped, &e->meshbytes, &loads);
            if (kind == 0) cn->solidvtx = vb.num;
            if (kind == 0) cn->out.solid = g;
            if (kind == 1) cn->out.cut = g;
            if (kind == 2) cn->out.xlu = g;
        }
        cn->out.vtx = vb.v;
        cn->numvtx = vb.num;
        cn->vmtx = vb.mtx;
        cn->rgb = litColours(vb.v, vb.nrm, vb.num);   /* round 32c (D-300) */
        free(vb.nrm);
        e->meshbytes += (u32)vb.cap * (u32)sizeof(Vtx) + (u32)vb.num * 6u;
        drawn += cn->numtris > 0;
    }
    free(list);
    *outdrawn = drawn;
    *outdropped = dropped;
    *outloads = loads;
}


/* ------------------------------------------------------------ light (round 32c, D-300..D-302) */

/* Video.LitCharacters (round 32c, D-300): 1 = every HD character shaded by the
 * light GoldenEye's own model of it carries (below); 0 = unlit, the picture's
 * colour (PD's rule, every build before 32c). Render only, live: each built
 * character keeps both colour sets and the vertices are rewritten when the
 * key changes (beanChrFrame), the blood copies remade from them.
 * Default 1 since the round 32 merge (D-305): Austin's A/B verdict, "B, lit". */
static int cfgLit = 1;

PD_CONSTRUCTOR static void beanChrConfigInit(void)
{
    configRegisterInt("Video.LitCharacters", &cfgLit, 0, 1);
}

static s32 litLog(void)
{
    static s32 v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANLITLOG");   /* dev: each fit, and the HD normals' agreement */
        v = e && *e && *e != '0';
    }
    return v;
}

/* D-302: the light. GoldenEye's characters carry none of their own (their
 * vertex colours are colour, not light: D-301), so the lit look is F3D's
 * own lighting, view space (GoldenEye's modelview holds the camera): a key
 * from above the viewer's left shoulder and a sky from straight up the view,
 * over an ambient. A surface facing the eye is lit near full, the flanks
 * and the underside fall off: the shape reads round from every side, the
 * same in each split-screen viewport, with no per-frame work but fast3d's
 * per-vertex dot products. The level's own light is GoldenEye's, as before:
 * the floor tile's colour blended over the whole character by the render
 * mode (chr.c shadecol). */
static s8 litWant = -1;   /* the colours every built entry is to hold (beanChrFrame) */

static void litLightSet(f32 amb, f32 key, f32 sky, const f32 *dir)
{
    f32 d[3] = { dir[0], dir[1], dir[2] };
    unitOr(d);
    memset(&chrLight.dir, 0, sizeof(chrLight.dir));
    memset(&chrLight.amb, 0, sizeof(chrLight.amb));
    const u8 ka = (u8)(fminf(fmaxf(amb, 0.0f), 1.0f) * 255.0f + 0.5f);
    const u8 kk = (u8)(fminf(fmaxf(key, 0.0f), 1.0f) * 255.0f + 0.5f);
    const u8 ks = (u8)(fminf(fmaxf(sky, 0.0f), 1.0f) * 255.0f + 0.5f);
    for (s32 k = 0; k < 3; k++) {
        chrLight.amb.col[k] = chrLight.amb.colc[k] = ka;
        chrLight.dir[0].col[k] = chrLight.dir[0].colc[k] = kk;
        chrLight.dir[0].dir[k] = (s8)roundf(d[k] * 127.0f);
        chrLight.dir[1].col[k] = chrLight.dir[1].colc[k] = ks;
    }
    chrLight.dir[1].dir[1] = 127;
    chrLight.numdir = 2;
}

static void litParams(void)
{
    static s32 done;
    if (done) {
        return;
    }
    done = 1;
    f32 amb = 0.50f, key = 0.60f, sky = 0.10f, dir[3] = { -0.35f, 0.55f, 0.76f };
    const char *e = getenv("GE_BEANLIT");   /* dev: "amb,key,sky,dx,dy,dz" (0..1, the key's direction in view space) */
    if (e) {
        sscanf(e, "%f,%f,%f,%f,%f,%f", &amb, &key, &sky, &dir[0], &dir[1], &dir[2]);
    }
    litLightSet(amb, key, sky, dir);
}

/* Each vertex's unlit RGB (as written) and its lit bytes - the normal, s8
 * (Vtx.n under G_LIGHTING; alpha stays the vertex's) - 6 bytes a vertex. */
static u8 *litColours(const Vtx *v, const f32 *nrm, s32 num)
{
    if (num <= 0 || !v || !nrm) {
        return NULL;
    }
    u8 *rgb = malloc((size_t)num * 6);
    if (!rgb) {
        return NULL;
    }
    for (s32 i = 0; i < num; i++) {
        for (s32 k = 0; k < 3; k++) {
            f32 c = nrm[i * 3 + k];
            c = c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c;
            rgb[i * 6 + k] = v[i].v.cn[k];
            rgb[i * 6 + 3 + k] = (u8)(s8)roundf(c * 127.0f);
        }
    }
    return rgb;
}

static void litApplyNode(struct cnode *cn, s32 on)
{
    if (!cn->rgb || !cn->out.vtx || cn->shared) {
        return;
    }
    const s32 at = on ? 3 : 0;
    for (s32 i = 0; i < cn->numvtx; i++) {
        cn->out.vtx[i].v.cn[0] = cn->rgb[i * 6 + at];
        cn->out.vtx[i].v.cn[1] = cn->rgb[i * 6 + at + 1];
        cn->out.vtx[i].v.cn[2] = cn->rgb[i * 6 + at + 2];
    }
}

/* Rewrites a built entry's vertex colours to the key's set; 1 if it changed. */
static s32 litApply(struct centry *e)
{
    if (litWant < 0) {
        litWant = (s8)(cfgLit ? 1 : 0);
        litParams();
    }
    const s8 want = litWant;
    if (e->state != 1 || e->litOn == want) {
        return 0;
    }
    for (s32 i = 0; i < e->numnodes; i++) {
        litApplyNode(&e->nodes[i], want);
    }
    litApplyNode(&e->headdraw, want);
    e->litOn = want;
    return 1;
}

/* A display-list node's list, vertices and count, either record kind (the
 * list token resolved as n64List() does). */
static const Gfx *n64ListOf(ModelNode *n, const Vertex **verts, s32 *numv)
{
    const u32 type = n->Opcode & 0xff;
    if (!n->Data) {
        return NULL;
    }
    if (type == MODELNODE_OPCODE_DLCOLLISION) {
        const ModelRoData_DisplayList_CollisionRecord *ro = &n->Data->DisplayListCollisions;
        *verts = ro->Vertices;
        *numv = ro->numVertices;
        return n64List(ro);
    }
    if (type == MODELNODE_OPCODE_DL) {
        const ModelRoData_DisplayListRecord *ro = &n->Data->DisplayList;
        const uintptr_t tok = (uintptr_t)ro->Primary;
        const u32 base = GUEST_ADDR(ro->BaseAddr);
        *verts = ro->Vertices;
        *numv = ro->numVertices;
        if (tok == 0 || tok > 0xffffffffu || ((tok >> 24) & 0xff) != SPSEGMENT_MODEL_COL1 || base == 0) {
            return NULL;
        }
        return (const Gfx *)((const u8 *)GUEST_PTR(base) + (tok & 0xffffff));
    }
    return NULL;
}

struct litacc {
    f64 m[4][4], r[4];   /* the normal equations of luma = amb + dir . n */
    f64 sumy, sumw, miny, maxy;
    s32 samples, tris, inward;
};

/* One N64 node's list: every triangle's face normal (area-weighted) summed
 * at its corners, then each corner's vertex colour luma against its normal.
 * Positions as readN64Tris() takes them (own coordinates + the joint's rest). */
static void litFitNode(ModelNode *node, s32 firstmtx, const struct rig *rig, struct litacc *acc)
{
    const Vertex *verts = NULL;
    s32 numv = 0;
    const Gfx *g = n64ListOf(node, &verts, &numv);
    if (!g || !verts || numv <= 0 || numv > 4096) {
        return;
    }
    f32 (*nacc)[3] = calloc((size_t)numv, sizeof(*nacc));
    f32 *wacc = calloc((size_t)numv, sizeof(*wacc));
    f32 (*pos)[3] = calloc((size_t)numv, sizeof(*pos));
    s32 *tri = NULL, ntri = 0, ctri = 0;
    if (!nacc || !wacc || !pos) {
        free(nacc);
        free(wacc);
        free(pos);
        return;
    }
    s32 slot[16], curm = firstmtx;
    for (s32 i = 0; i < 16; i++) {
        slot[i] = -1;
    }
    f32 cen[3] = { 0, 0, 0 };
    s32 ncen = 0;
    for (s32 steps = 0; steps < 8192; steps++, g++) {
        const u32 w0 = (u32)g->words.w0, w1 = (u32)g->words.w1;
        const u32 op = w0 >> 24;
        s32 t[4][3], nt = 0;
        if (op == 0xb8) {
            break;
        }
        if (op == 0x01 && (w1 >> 24) == SPSEGMENT_MODEL_MTX) {
            curm = (s32)((w1 & 0xffffff) / sizeof(Mtx));
            continue;
        }
        if (op == 0x04 && (w1 >> 24) == SPSEGMENT_MODEL_VTX) {
            const s32 n = (s32)((w0 >> 20) & 0xf) + 1, v0 = (s32)((w0 >> 16) & 0xf);
            const s32 base = (s32)((w1 & 0xffffff) / sizeof(Vertex));
            for (s32 k = 0; k < n && v0 + k < 16; k++) {
                const s32 vi = base + k;
                slot[v0 + k] = -1;
                if (vi < 0 || vi >= numv || curm < 0 || curm >= MAXMTX || !rig->hasrest[curm]) {
                    continue;
                }
                slot[v0 + k] = vi;
                pos[vi][0] = verts[vi].coord.x + rig->rest[curm][0];
                pos[vi][1] = verts[vi].coord.y + rig->rest[curm][1];
                pos[vi][2] = verts[vi].coord.z + rig->rest[curm][2];
            }
            continue;
        }
        if (op == 0xb1) {
            for (s32 k = 0; k < 4; k++) {
                t[nt][0] = (s32)((w1 >> (k * 8)) & 0xf);
                t[nt][1] = (s32)((w1 >> (k * 8 + 4)) & 0xf);
                t[nt][2] = (s32)((w0 >> (k * 4)) & 0xf);
                if (t[nt][0] != t[nt][1] || t[nt][1] != t[nt][2]) {
                    nt++;
                }
            }
        } else if (op == 0xbf) {
            t[0][0] = (s32)(((w1 >> 16) & 0xff) / 10);
            t[0][1] = (s32)(((w1 >> 8) & 0xff) / 10);
            t[0][2] = (s32)((w1 & 0xff) / 10);
            nt = 1;
        }
        for (s32 k = 0; k < nt; k++) {
            s32 vi[3], ok = 1;
            for (s32 c = 0; c < 3 && ok; c++) {
                vi[c] = t[k][c] < 16 ? slot[t[k][c]] : -1;
                ok = vi[c] >= 0;
            }
            if (!ok) {
                continue;
            }
            if (ntri >= ctri) {
                const s32 nc = ctri ? ctri * 2 : 256;
                s32 *r = realloc(tri, sizeof(s32) * 3 * (size_t)nc);
                if (!r) {
                    break;
                }
                tri = r;
                ctri = nc;
            }
            memcpy(&tri[ntri * 3], vi, sizeof(vi));
            ntri++;
            for (s32 c = 0; c < 3; c++) {
                for (s32 a = 0; a < 3; a++) {
                    cen[a] += pos[vi[c]][a];
                }
                ncen++;
            }
        }
    }
    if (ntri > 0 && ncen > 0) {
        for (s32 a = 0; a < 3; a++) {
            cen[a] /= (f32)ncen;
        }
        /* GoldenEye's winding: the face normal's sign is whichever points
         * away from the part's own centre for most of its area */
        f64 out = 0.0, in = 0.0;
        f32 fn[3];
        for (s32 i = 0; i < ntri; i++) {
            const f32 *a = pos[tri[i * 3]], *b = pos[tri[i * 3 + 1]], *c = pos[tri[i * 3 + 2]];
            f32 e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            vecCross(e1, e2, fn);
            const f32 d = fn[0] * ((a[0] + b[0] + c[0]) / 3 - cen[0]) + fn[1] * ((a[1] + b[1] + c[1]) / 3 - cen[1])
                + fn[2] * ((a[2] + b[2] + c[2]) / 3 - cen[2]);
            if (d >= 0) out += d; else in -= d;
        }
        const f32 sign = out >= in ? 1.0f : -1.0f;
        for (s32 i = 0; i < ntri; i++) {
            const f32 *a = pos[tri[i * 3]], *b = pos[tri[i * 3 + 1]], *c = pos[tri[i * 3 + 2]];
            f32 e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            vecCross(e1, e2, fn);
            const f32 area = vecLen(fn);
            if (area <= 0.0f) {
                continue;
            }
            for (s32 c2 = 0; c2 < 3; c2++) {
                const s32 v = tri[i * 3 + c2];
                for (s32 k2 = 0; k2 < 3; k2++) {
                    nacc[v][k2] += fn[k2] * sign;
                }
                wacc[v] += area;
            }
        }
        acc->tris += ntri;
        acc->inward += sign < 0 ? ntri : 0;
        for (s32 v = 0; v < numv; v++) {
            if (wacc[v] <= 0.0f) {
                continue;
            }
            f32 n[3] = { nacc[v][0], nacc[v][1], nacc[v][2] };
            if (vecLen(n) < 1e-6f) {
                continue;
            }
            unitOr(n);
            const f64 y = (verts[v].r * 79 + verts[v].g * 156 + verts[v].b * 21) / (256.0 * 255.0);
            const f64 x[4] = { 1.0, n[0], n[1], n[2] };
            const f64 w = wacc[v];
            for (s32 i = 0; i < 4; i++) {
                for (s32 j = 0; j < 4; j++) {
                    acc->m[i][j] += w * x[i] * x[j];
                }
                acc->r[i] += w * x[i] * y;
            }
            acc->sumy += w * y;
            acc->sumw += w;
            acc->miny = acc->samples ? (y < acc->miny ? y : acc->miny) : y;
            acc->maxy = acc->samples ? (y > acc->maxy ? y : acc->maxy) : y;
            acc->samples++;
        }
    }
    free(tri);
    free(nacc);
    free(wacc);
    free(pos);
}

/* Solves the 4x4 normal equations (Gauss-Jordan, partial pivoting). */
static s32 litSolve(f64 m[4][4], f64 r[4], f64 out[4])
{
    for (s32 c = 0; c < 4; c++) {
        s32 p = c;
        for (s32 i = c + 1; i < 4; i++) {
            if (__builtin_fabs(m[i][c]) > __builtin_fabs(m[p][c])) p = i;
        }
        if (__builtin_fabs(m[p][c]) < 1e-9) {
            return 0;
        }
        for (s32 j = 0; j < 4; j++) {
            const f64 t = m[c][j]; m[c][j] = m[p][j]; m[p][j] = t;
        }
        const f64 tr = r[c]; r[c] = r[p]; r[p] = tr;
        for (s32 i = 0; i < 4; i++) {
            if (i == c) continue;
            const f64 f = m[i][c] / m[c][c];
            for (s32 j = 0; j < 4; j++) m[i][j] -= f * m[c][j];
            r[i] -= f * r[c];
        }
    }
    for (s32 i = 0; i < 4; i++) out[i] = r[i] / m[i][i];
    return 1;
}

/* dev (GE_BEANLITLOG, D-301): GoldenEye's own model of a body, every plain
 * list (near and far), fitted as luma = amb + dir . n and logged - the
 * measurement behind D-302's choice of light; nothing is built from it. */
static void litFitBody(const struct centry *e, const struct rig *rig)
{
    struct litacc acc;
    struct chrlight light;
    if (!litLog()) {
        return;
    }
    memset(&acc, 0, sizeof(acc));
    memset(&light, 0, sizeof(light));
    for (s32 i = 0; i < e->numnodes; i++) {
        litFitNode(e->nodes[i].node, modelFindNodeMtxIndex(e->nodes[i].node, 0), rig, &acc);
    }
    f64 sol[4];
    const f64 mean = acc.sumw > 0 ? acc.sumy / acc.sumw : 0.0;
    if (acc.samples >= 16 && acc.maxy - acc.miny > 0.05 && litSolve(acc.m, acc.r, sol)) {
        /* the residual from the (unsolved) sums would need the y^2 sum; the
         * spread is what the log reports */
        light.amb = (f32)sol[0];
        light.dir[0] = (f32)sol[1];
        light.dir[1] = (f32)sol[2];
        light.dir[2] = (f32)sol[3];
        light.samples = acc.samples;
    }
    light.inward = acc.tris ? (f32)acc.inward / (f32)acc.tris : 0.0f;
    {
        const f32 *d = light.dir;
        sysLogPrintf(LOG_NOTE, "beanlit: %s: %d N64 vertices on %d triangles (%.0f%% wound inward), luma mean %.3f min %.3f max "
                     "%.3f -> amb %.3f dir %.3f %.3f %.3f (|dir| %.3f)%s", e->file, acc.samples, acc.tris,
                     100.0f * light.inward, mean, acc.miny, acc.maxy, light.amb, d[0], d[1], d[2], vecLen(d),
                     light.samples ? "" : " - flat, no fit");
    }
}

/* dev (GE_BEANLITLOG): how the release's normals sit on its own surface -
 * the share of triangles whose corners' summed normal agrees with the
 * triangle's winding (back faces are culled, so the release winds outward). */
static void litCheckNormals(const struct centry *e, const struct ctri *tris, s32 num)
{
    if (!litLog()) {
        return;
    }
    s32 agree = 0, n = 0, plain = 0;
    for (s32 t = 0; t < num; t++) {
        const struct ctri *c = &tris[t];
        f32 e1[3], e2[3], fn[3];
        for (s32 k = 0; k < 3; k++) {
            e1[k] = c->v[1].pos[k] - c->v[0].pos[k];
            e2[k] = c->v[2].pos[k] - c->v[0].pos[k];
        }
        vecCross(e1, e2, fn);
        if (vecLen(fn) <= 0.0f) {
            continue;
        }
        const f32 s0 = fn[0] * (c->v[0].nrm[0] + c->v[1].nrm[0] + c->v[2].nrm[0])
            + fn[1] * (c->v[0].nrm[1] + c->v[1].nrm[1] + c->v[2].nrm[1])
            + fn[2] * (c->v[0].nrm[2] + c->v[1].nrm[2] + c->v[2].nrm[2]);
        agree += s0 > 0.0f;
        n++;
        plain += c->tex < 0 && ((c->v[0].argb | c->v[1].argb | c->v[2].argb) & 0x00ffffffu) != 0x00ffffffu
            ? 1 : (c->tex < 0 && ((c->v[0].argb & c->v[1].argb & c->v[2].argb) & 0x00ffffffu) != 0x00ffffffu);
    }
    sysLogPrintf(LOG_NOTE, "beanlit: %s HD: %d of %d triangles' normals agree with their winding; %d coloured untextured",
                 e->file, agree, n, plain);
}

static s32 openSkeleton(struct beanlevel *bm, f32 bind[SK_COUNT][3], s32 havebind[SK_COUNT], s8 *boneskel,
                        s32 *numbones)
{
    struct beanskel sk;
    memset(havebind, 0, sizeof(s32) * SK_COUNT);
    memset(bind, 0, sizeof(f32) * SK_COUNT * 3);
    *numbones = beanModelSkeleton(bm, &sk);
    for (s32 b = 0; b < BEAN_MAXBONES; b++) {
        boneskel[b] = -1;
    }
    for (s32 b = 0; b < *numbones; b++) {
        for (s32 s = 0; s < SK_COUNT; s++) {
            if (boneNameIs(sk.name[b], skelNames[s])) {
                boneskel[b] = (s8)s;
                if (!havebind[s]) {
                    memcpy(bind[s], sk.bind[b], sizeof(bind[s]));
                    havebind[s] = 1;
                }
                break;
            }
        }
    }
    return *numbones;
}

/* Whether some head row is cut off this body's own file (a Bond outfit). */
static s32 bodyCarriesHeadRow(const char *source)
{
    for (s32 i = 0; i < NUM_CHRROWS; i++) {
        if (chrRows[i].kind == BEANCHR_HEAD && strcmp(chrRows[i].source, source) == 0) {
            return 1;
        }
    }
    return 0;
}

static void fail(struct centry *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(struct centry *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof(e->why), fmt, ap);
    va_end(ap);
    e->state = -1;
    sysLogPrintf(LOG_NOTE, "beanchr: %s%s%s: %s - GoldenEye's own", e->file ? e->file : "?", e->body ? " on " : "",
                 e->body ? (fileOf(e->body) ? fileOf(e->body) : "?") : "", e->why);
}

s32 modelFindNodeMtxIndex(ModelNode *node, s32 arg1);

static void buildBody(struct centry *e)
{
    const u64 t0 = sysGetMicroseconds();
    const struct chrrow *row = &chrRows[e->row];
    struct walk *w = calloc(1, sizeof(*w));
    struct rig *rig = calloc(1, sizeof(*rig));
    struct collect c;
    struct pic *pics = NULL;
    char path[1024], why[128];
    s8 boneskel[BEAN_MAXBONES];
    s32 numbones = 0, numdraws, numtex;
    f32 uvscale, zero[3] = { 0.0f, 0.0f, 0.0f };

    e->state = -1;
    memset(&c, 0, sizeof(c));
    if (!w || !rig) {
        free(w);
        free(rig);
        fail(e, "out of memory");
        return;
    }
    walkTree(w, e->hdr->RootNode, zero, 0, 0, 0);
    e->headspot = w->headspot;
    if (getenv("GE_BEANRIGLOG")) {
        for (s32 i = 0; i < w->num; i++) {
            const ModelNode *n = w->n[i].node;
            const u32 type = n->Opcode & 0xff;
            s32 mtx = -1;
            if (type == MODELNODE_OPCODE_HEADER && n->Data) mtx = n->Data->Header.MatrixIndex;
            else if ((type == MODELNODE_OPCODE_GROUP || type == MODELNODE_OPCODE_OP03) && n->Data) mtx = n->Data->Group.MatrixIDs[0];
            else if (type == MODELNODE_OPCODE_GROUPSIMPLE && n->Data) mtx = n->Data->GroupSimple.Group1;
            sysLogPrintf(LOG_NOTE, "rig: %s node %d type %u mtx %d findmtx %d rest %.1f %.1f %.1f%s", e->file, i, type, mtx,
                         modelFindNodeMtxIndex((ModelNode *)n, 0), w->n[i].rest[0], w->n[i].rest[1], w->n[i].rest[2],
                         n == w->headspot ? " HEADSPOT" : "");
        }
    }
    if (!rigFromModel(w, rig, why, sizeof(why))) {
        free(w);
        free(rig);
        fail(e, "%s", why);
        return;
    }
    if (getenv("GE_BEANRIGLOG")) {
        sysLogPrintf(LOG_NOTE, "rig: %s base mtx %d back mtx %d (%.1f %.1f %.1f) neck mtx %d (%.1f %.1f %.1f); headspot findmtx %d",
                     e->file, rig->mtx[SK_BASE], rig->mtx[SK_BACK], rig->joint[SK_BACK][0], rig->joint[SK_BACK][1],
                     rig->joint[SK_BACK][2], rig->mtx[SK_NECK], rig->joint[SK_NECK][0], rig->joint[SK_NECK][1],
                     rig->joint[SK_NECK][2], w->headspot ? modelFindNodeMtxIndex(w->headspot, 0) : -2);
    }
    if (!sourcePath(row->source, path, sizeof(path))) {
        free(w);
        free(rig);
        fail(e, "%s not in the imported layer", row->source);
        return;
    }
    struct beanlevel *bm = beanModelOpen(path);
    if (!bm) {
        free(w);
        free(rig);
        fail(e, "%s is not a rendergraph this reads", row->source);
        return;
    }
    beanModelInfo(bm, &numdraws, &numtex, &uvscale);
    if (!openSkeleton(bm, e->bind, e->havebind, boneskel, &numbones)) {
        beanLevelClose(bm);
        free(w);
        free(rig);
        fail(e, "%s has no skeleton", row->source);
        return;
    }
    if (!fitRig(rig, (const f32 (*)[3])e->bind, e->havebind, why, sizeof(why))) {
        beanLevelClose(bm);
        free(w);
        free(rig);
        fail(e, "%s", why);
        return;
    }
    c.rig = rig;
    c.bind = (const f32 (*)[3])e->bind;
    c.havebind = e->havebind;
    c.boneskel = boneskel;
    c.numbones = numbones;
    c.mode = row->kind == BEANCHR_WHOLE ? 1 : 0;
    {
        const char *dd = getenv("GE_BEANBODYDUMP");   /* dev (round 54): a directory; each body's skinning as text */
        char dp[1024];
        if (dd && *dd) {
            snprintf(dp, sizeof(dp), "%s/%s_body.txt", dd, e->file);
            c.dump = fopen(dp, "w");
        }
    }
    beanModelSkinTriangles(bm, skinTri, &c);
    if (c.dump) {
        fclose(c.dump);
        c.dump = NULL;
    }

    /* A guard's body file carries the release's own head; its face's
     * picture is the one most of the neck's triangles use. The skin ring
     * left round the collar on that picture (weighted to the back) is the
     * file's own head section, which the release switches off on a body that
     * wears a head file (PD "noskin"): left out, unless a head row is cut
     * off this very body (a Bond outfit: his own face closes it). */
    s32 ring = 0;
    if (row->kind == BEANCHR_BODY && !bodyCarriesHeadRow(row->source)) {
        /* count the neck's triangles per picture with a second walk */
        struct collect n = c;
        n.tris = NULL;
        n.num = n.cap = 0;
        n.mode = 2;
        memset(n.perskel, 0, sizeof(n.perskel));
        beanModelSkinTriangles(bm, skinTri, &n);
        s32 *count = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(s32));
        s32 face = -1;
        for (s32 t = 0; count && t < n.num; t++) {
            if (n.tris[t].tex >= 0 && n.tris[t].tex < numtex) {
                count[n.tris[t].tex]++;
            }
        }
        for (s32 p = 0; count && p < numtex; p++) {
            if (count[p] > 0 && (face < 0 || count[p] > count[face])) {
                face = p;
            }
        }
        free(count);
        free(n.tris);
        if (face >= 0) {
            s32 kept = 0;
            for (s32 t = 0; t < c.num; t++) {
                if (c.tris[t].tex == face && c.tris[t].skel == SK_BACK) {
                    ring++;
                    continue;
                }
                c.tris[kept++] = c.tris[t];
            }
            c.num = kept;
        }
    }

    if (c.unres) {
        sysLogPrintf(LOG_WARNING, "[xbla] unresolved: character %s (%s): %d triangles of a sphere-mapped or missing-picture draw",
                     e->file, row->source, c.unres);
        ca.refs++;
    }
    if (c.num == 0 || c.unres) {
        free(c.tris);
        beanLevelClose(bm);
        free(w);
        free(rig);
        fail(e, c.num == 0 ? "%s has no triangles this reads" : "%s has unresolved references", row->source);
        return;
    }
    e->nodes = calloc(MAXNODES, sizeof(*e->nodes));
    e->numnodes = e->nodes ? collectNodes(w, rig, e->nodes, MAXNODES) : 0;
    /* A triangle whose joint has no plain list of its own goes to the
     * nearest joint up the chain that has one (else the base's). */
    s32 haslist[SK_COUNT];
    memset(haslist, 0, sizeof(haslist));
    for (s32 i = 0; i < e->numnodes; i++) {
        if (e->nodes[i].candidate && e->nodes[i].skel >= 0) {
            haslist[(s32)e->nodes[i].skel] = 1;
        }
    }
    static const s8 up[SK_COUNT] = {
        -1, SK_BASE, SK_BACK, SK_BASE,
        SK_BACK, SK_LF_SHOULDER, SK_LF_ELBOW,
        SK_BACK, SK_RT_SHOULDER, SK_RT_ELBOW,
        SK_BASE, SK_LF_HIP, SK_LF_KNEE,
        SK_BASE, SK_RT_HIP, SK_RT_KNEE,
    };
    s32 moved = 0, orphan = 0;
    for (s32 t = 0; t < c.num; t++) {
        s32 s = c.tris[t].skel;
        while (s >= 0 && !haslist[s]) {
            s = up[s];
        }
        if (s < 0) {
            orphan++;
            s = SK_BASE;
        }
        moved += s != c.tris[t].skel;
        c.tris[t].skel = (s8)s;
    }
    if (logOn() && orphan && !haslist[SK_BASE]) {
        for (s32 i = 0; i < w->num; i++) {
            const ModelNode *n = w->n[i].node;
            const u32 type = n->Opcode & 0xff;
            sysLogPrintf(LOG_NOTE, "beanchr:   walk %3d op %2d rest %7.1f %7.1f %7.1f mtx %d sw %d lod %d", i, type,
                         w->n[i].rest[0], w->n[i].rest[1], w->n[i].rest[2],
                         (type == MODELNODE_OPCODE_GROUP || type == MODELNODE_OPCODE_OP03) ? n->Data->Group.MatrixIDs[0]
                         : type == MODELNODE_OPCODE_HEADER ? n->Data->Header.MatrixIndex
                         : modelFindNodeMtxIndex((ModelNode *)n, 0), w->n[i].switched, w->n[i].lod);
        }
        for (s32 sk = 0; sk < SK_COUNT; sk++) {
            sysLogPrintf(LOG_NOTE, "beanchr:   %-16s mtx %2d rest %7.1f %7.1f %7.1f", skelNames[sk], rig->mtx[sk],
                         rig->joint[sk][0], rig->joint[sk][1], rig->joint[sk][2]);
        }
    }
    if (orphan && !haslist[SK_BASE]) {
        free(c.tris);
        beanLevelClose(bm);
        free(w);
        fail(e, "no plain display list for %d triangles", orphan);
        free(rig);
        e->rig = NULL;
        return;
    }
    const s32 failed = bindPictures(e, bm, numtex, c.tris, c.num, &pics, row->source);
    if (failed != 0) {
        free(pics);
        free(c.tris);
        beanLevelClose(bm);
        free(w);
        free(rig);
        fail(e, failed < 0 ? "out of memory" : "%d pictures do not decode", failed);
        return;
    }
    s32 drawn = 0, dropped = 0, loads = 0, counts[3] = { 0, 0, 0 };
    litFitBody(e, rig);                     /* round 32c (D-301) */
    litCheckNormals(e, c.tris, c.num);
    buildLists(e, e->nodes, e->numnodes, 1, rig, c.tris, c.num, pics, &drawn, &dropped, &loads, counts);
    buildPaintUnlessCutscene(e, e->nodes, e->numnodes, rig, -1);
    e->necknode = -1;
    for (s32 i = 0; i < e->numnodes && e->necknode < 0; i++) {
        if (e->nodes[i].candidate && e->nodes[i].skel == SK_NECK && e->nodes[i].numtris == 0) {
            e->necknode = i;
        }
    }
    e->meshbytes += (u32)e->numstandins * TILE * TILE * sizeof(u16);
    e->numtris = c.num;
    e->rig = rig;
    e->state = 1;
    meshBytes += e->meshbytes;
    picBytes += e->picbytes;
    ca.bodies++;
    const f64 ms = msSince(t0);
    buildMs += ms;
    sysLogPrintf(LOG_NOTE, "beanchr: %s <- %s (%s): %d triangles (%d solid, %d cut-out, %d translucent; %d glass; neck %d left to the "
                 "head, %d skin-ring, %d unskinned, %d shader-only, %d moved up a joint, %d dropped) on %d of %d lists, "
                 "%d matrix loads, scale %.4f, %d pictures %u KB, mesh %u KB, uv 1/%.0f; %.2f ms",
                 e->file, row->source, row->kind == BEANCHR_WHOLE ? "whole" : "body", c.num, counts[0], counts[1], counts[2],
                 c.glass, c.dropped, ring, c.unskinned, c.shader, moved, dropped, drawn, e->numnodes, loads, rig->scale,
                 e->numstandins, e->picbytes / 1024, e->meshbytes / 1024, uvscale, ms);
    if (logOn()) {
        for (s32 s = 0; s < SK_COUNT; s++) {
            sysLogPrintf(LOG_NOTE, "beanchr:   %-16s mtx %2d rest %7.1f %7.1f %7.1f bind %7.2f %7.2f %7.2f tris %d",
                         skelNames[s], rig->mtx[s], rig->joint[s][0], rig->joint[s][1], rig->joint[s][2], e->bind[s][0],
                         e->bind[s][1], e->bind[s][2], c.perskel[s]);
        }
        for (s32 i = 0; i < e->numnodes; i++) {
            sysLogPrintf(LOG_NOTE, "beanchr:   node %2d op %2d skel %2d cand %d tris %d", i, e->nodes[i].node->Opcode & 0xff,
                         e->nodes[i].skel, e->nodes[i].candidate, e->nodes[i].numtris);
        }
    }
    free(pics);
    free(c.tris);
    beanLevelClose(bm);
    free(w);
}

/* D-245 (dev, GE_BEANHEADSEAT=0|1|2): measure the HD head against
 * GoldenEye's own head file on this body (its near lists, read in the model's
 * rest under the neck's matrix, as GoldenEye draws them under the HEAD
 * placeholder's joint) and log the turn + shift that takes the one onto the
 * other; 1 applies the turn about the neck joint, 2 the turn and the shift.
 * Not applied by default: the N64 head files carry hats and blocky hair, so
 * the fit is a shape match good to a few degrees, and neither arm changed what
 * D-227 is about (the release's hair reaching the collar). */
static void headSeat(struct centry *e, const struct centry *body, struct collect *c)
{
    const struct rig *rig = body->rig;
    const s32 m = rig->mtx[SK_NECK];
    struct ntri *tris = NULL;
    s32 num = 0, cap = 0;
    const char *ev = getenv("GE_BEANHEADSEAT");

    if (!ev) {
        return;   /* dev only: measured, not applied, by default (D-245) */
    }

    for (s32 i = 0; i < e->numnodes; i++) {
        ModelNode *n = e->nodes[i].node;
        if ((n->Opcode & 0xff) != MODELNODE_OPCODE_DLCOLLISION || !n->Data || e->nodes[i].lod) {
            continue;
        }
        const ModelRoData_DisplayList_CollisionRecord *ro = &n->Data->DisplayListCollisions;
        if (!ro->Primary || !ro->Vertices || ro->numVertices <= 0) {
            continue;
        }
        readN64Tris(n, 0, 0, m, rig, &tris, &num, &cap);
    }
    f32 (*pts)[3] = c->num > 0 ? malloc(sizeof(*pts) * (size_t)c->num * 3) : NULL;
    s32 np = 0;
    /* every corner, thinned to about 600 points (the fit needs the shape, not each vertex) */
    const s32 step = c->num * 3 > 600 ? (c->num * 3 + 599) / 600 : 1;
    for (s32 i = 0, q = 0; pts && i < c->num; i++) {
        for (s32 k = 0; k < 3; k++, q++) {
            if (q % step == 0) {
                memcpy(pts[np++], c->tris[i].v[k].pos, sizeof(pts[0]));
            }
        }
    }
    f32 R[3][3], tr[3], before = -1.0f;
    const f32 *pivot = rig->rest[m];
    const f32 after = pts ? alignToTris((const f32 (*)[3])pts, np, tris, num, pivot, R, tr, &before) : -1.0f;
    if (after >= 0.0f) {
        const f32 rx = __builtin_atan2f(R[2][1], R[2][2]) * 57.29578f, ry = -__builtin_asinf(fmaxf(-1.0f, fminf(1.0f, R[2][0]))) * 57.29578f,
                  rz = __builtin_atan2f(R[1][0], R[0][0]) * 57.29578f;
        sysLogPrintf(LOG_NOTE, "beanchr: head seat %s on %s: HD->N64 turn x %.1f y %.1f z %.1f deg, shift %.1f %.1f %.1f; "
                     "rms %.1f -> %.1f (%d HD points, %d N64 triangles)", e->file, body->file, rx, ry, rz, tr[0], tr[1],
                     tr[2], before, after, np, num);
        const s32 how = (s32)strtol(ev, NULL, 10);
        for (s32 i = 0; how && i < c->num; i++) {
            for (s32 k = 0; k < 3; k++) {
                f32 rel[3], q[3];
                for (s32 a = 0; a < 3; a++) {
                    rel[a] = c->tris[i].v[k].pos[a] - pivot[a];
                }
                rotApply((const f32 (*)[3])R, rel, q);
                for (s32 a = 0; a < 3; a++) {
                    c->tris[i].v[k].pos[a] = q[a] + pivot[a] + (how >= 2 ? tr[a] : 0.0f);
                }
            }
        }
    }
    free(pts);
    free(tris);
}

/* A head on a body (built per pair: its seat is the body's fit). */
static void buildHead(struct centry *e, const struct centry *body)
{
    const u64 t0 = sysGetMicroseconds();
    const struct chrrow *row = &chrRows[e->row];
    const struct chrrow *brow = &chrRows[body->row];
    struct walk *w = calloc(1, sizeof(*w));
    struct collect c;
    struct pic *pics = NULL;
    char path[1024];
    s8 boneskel[BEAN_MAXBONES];
    f32 bind[SK_COUNT][3];
    s32 havebind[SK_COUNT];
    s32 numbones = 0, numdraws, numtex;
    f32 uvscale, zero[3] = { 0.0f, 0.0f, 0.0f };
    const s32 fromchar = strncmp(row->source, "char/", 5) == 0;
    const s32 own = fromchar && strcmp(row->source, brow->source) == 0;

    e->state = -1;
    memset(&c, 0, sizeof(c));
    if (!w) {
        fail(e, "out of memory");
        return;
    }
    {
        /* the head file's own tree: its top nodes and their children, never
         * up through the body's placeholder its tops are linked under */
        ModelNode *root = e->hdr->RootNode;
        walkTree(w, root, zero, 0, 0, 0);
        for (s32 i = 0; i < w->num; i++) {
            w->n[i].switched = 0;   /* every list of a head is covered, toggled or not */
        }
    }
    if (!sourcePath(row->source, path, sizeof(path))) {
        free(w);
        fail(e, "%s not in the imported layer", row->source);
        return;
    }
    struct beanlevel *bm = beanModelOpen(path);
    if (!bm) {
        free(w);
        fail(e, "%s is not a rendergraph this reads", row->source);
        return;
    }
    beanModelInfo(bm, &numdraws, &numtex, &uvscale);
    openSkeleton(bm, bind, havebind, boneskel, &numbones);
    c.rig = body->rig;
    c.boneskel = boneskel;
    c.numbones = numbones;
    if (own) {
        /* Bond's head off his own outfit: the neck's triangles, skinned
         * with the body exactly as the whole file would be */
        c.bind = (const f32 (*)[3])body->bind;
        c.havebind = body->havebind;
        c.mode = 2;
    } else {
        static const f32 upv[3] = { 0.0f, 1.0f, 0.0f };
        if (!havebind[SK_NECK] || (fromchar && !havebind[SK_BACK])) {
            beanLevelClose(bm);
            free(w);
            fail(e, "%s has no neck", row->source);
            return;
        }
        if (fromchar) {
            /* a head off another body: turned so Bean's spine points up */
            f32 spine[3];
            for (s32 k = 0; k < 3; k++) {
                spine[k] = bind[SK_NECK][k] - bind[SK_BACK][k];
            }
            rotationBetween(spine, upv, c.headrot);
        } else {
            rotationBetween(upv, upv, c.headrot);
        }
        memcpy(c.headbind, bind[SK_NECK], sizeof(c.headbind));
        c.bind = (const f32 (*)[3])bind;
        c.havebind = havebind;
        c.mode = 3;
        c.neckback = !fromchar;   /* a head file's neck reaching into the collar rides the back (PD neckback) */
        c.neckonly = fromchar;    /* a Bond head off another outfit: that file's neck, rigid (PD) */
    }
    {
        const char *dd = getenv("GE_BEANHEADDUMP");   /* dev: a directory */
        char dp[1024];
        if (dd && *dd) {
            snprintf(dp, sizeof(dp), "%s/%s_on_%s.txt", dd, e->file, body->file);
            c.dump = fopen(dp, "w");
        }
    }
    beanModelSkinTriangles(bm, skinTri, &c);
    if (c.dump) {
        for (s32 t = 0; t < c.num; t++) {
            const struct ctri *ct = &c.tris[t];
            fprintf(c.dump, "O tex %d skel %d", ct->tex, ct->skel);
            for (s32 i = 0; i < 3; i++) {
                fprintf(c.dump, " | %.2f %.2f %.2f m%d uv %.5f %.5f %08x", ct->v[i].pos[0], ct->v[i].pos[1],
                        ct->v[i].pos[2], ct->v[i].mtx, ct->v[i].uv[0], ct->v[i].uv[1], (unsigned)ct->v[i].argb);
            }
            fprintf(c.dump, "\n");
        }
        fclose(c.dump);
        c.dump = NULL;
    }
    if (c.unres) {
        sysLogPrintf(LOG_WARNING, "[xbla] unresolved: character %s (%s): %d triangles of a sphere-mapped or missing-picture draw",
                     e->file, row->source, c.unres);
        ca.refs++;
    }
    if (c.num == 0 || c.unres) {
        free(c.tris);
        beanLevelClose(bm);
        free(w);
        fail(e, c.num == 0 ? "%s has no triangles this reads" : "%s has unresolved references", row->source);
        return;
    }
    e->nodes = calloc(MAXNODES, sizeof(*e->nodes));
    e->numnodes = e->nodes ? collectNodes(w, NULL, e->nodes, MAXNODES) : 0;
    /* Every list of the head file is the HD head's (toggled glasses, hats,
     * second hair: Bean's head carries its own, PD) and draws nothing; the HD
     * head is drawn at the body's own neck list, which is drawn whenever the
     * body is (a toggle inside the head would hide it with the part). The
     * whole-head toggle at the file's root is honoured (beanChrNode). */
    if (body->necknode < 0) {
        free(c.tris);
        beanLevelClose(bm);
        free(w);
        fail(e, "its body has no plain neck list to draw the head at");
        return;
    }
    e->rootswitch = e->hdr->RootNode && (e->hdr->RootNode->Opcode & 0xff) == MODELNODE_OPCODE_SWITCH ? e->hdr->RootNode : NULL;
    headSeat(e, body, &c);
    const s32 failed = bindPictures(e, bm, numtex, c.tris, c.num, &pics, row->source);
    if (failed != 0) {
        free(pics);
        free(c.tris);
        beanLevelClose(bm);
        free(w);
        fail(e, failed < 0 ? "out of memory" : "%d pictures do not decode", failed);
        return;
    }
    s32 drawn = 0, dropped = 0, loads = 0, counts[3] = { 0, 0, 0 };
    e->headdraw.candidate = 1;
    litCheckNormals(e, c.tris, c.num);
    buildLists(e, &e->headdraw, 1, 0, body->rig, c.tris, c.num, pics, &drawn, &dropped, &loads, counts);
    buildPaintUnlessCutscene(e, &e->headdraw, 1, body->rig, body->rig->mtx[SK_NECK]);
    for (s32 i = 0; i < e->numnodes; i++) {
        e->nodes[i].candidate = 1;   /* every list of the head file draws nothing */
        e->nodes[i].numtris = 0;
    }
    e->meshbytes += (u32)e->numstandins * TILE * TILE * sizeof(u16);
    e->numtris = c.num;
    e->state = 1;
    meshBytes += e->meshbytes;
    picBytes += e->picbytes;
    ca.heads++;
    const f64 ms = msSince(t0);
    buildMs += ms;
    sysLogPrintf(LOG_NOTE, "beanchr: %s on %s <- %s (%s): %d triangles (%d solid, %d cut-out, %d translucent; %d dropped) over %d "
                 "lists, %d matrix loads, %d pictures %u KB, mesh %u KB, uv 1/%.0f; %.2f ms",
                 e->file, body->file, row->source, own ? "his own outfit's neck" : "rigid on the neck", c.num, counts[0],
                 counts[1], counts[2], dropped, e->numnodes, loads, e->numstandins, e->picbytes / 1024, e->meshbytes / 1024,
                 uvscale, ms);
    free(pics);
    free(c.tris);
    beanLevelClose(bm);
    free(w);
}

/* ------------------------------------------------------------ entries */

static struct centry *entryFor(ModelFileHeader *hdr, ModelFileHeader *body)
{
    static struct centry *last;
    if (last && last >= entries && last < entries + numEntries && last->hdr == hdr && last->body == body) {
        return last;
    }
    for (s32 i = 0; i < numEntries; i++) {
        if (entries[i].hdr == hdr && entries[i].body == body) {
            return last = &entries[i];
        }
    }
    if (numEntries >= capEntries) {
        const s32 cap = capEntries ? capEntries * 2 : 64;
        struct centry *n = realloc(entries, sizeof(*n) * (size_t)cap);
        if (!n) {
            return NULL;
        }
        entries = n;
        capEntries = cap;
        last = NULL;
    }
    struct centry *e = &entries[numEntries++];
    memset(e, 0, sizeof(*e));
    e->hdr = hdr;
    e->body = body;
    e->file = fileOf(hdr);
    e->row = (s16)rowOf(e->file);
    if (e->row < 0 || !onlyAllows(e->file)) {
        e->state = -1;
        snprintf(e->why, sizeof(e->why), "%s", e->row < 0 ? "no row" : "GE_BEANCHRONLY");
        if (e->row < 0 && !body) {
            ca.unmapped++;
            sysLogPrintf(LOG_NOTE, "beanchr: character model %s has no row - GoldenEye's own", e->file ? e->file : "(not a C*Z file)");
        }
        e->row = -1;
    } else if ((chrRows[e->row].kind == BEANCHR_HEAD) != (body != NULL)) {
        /* a head file drawn as a body, or a body file grafted as a head */
        e->state = -1;
        snprintf(e->why, sizeof(e->why), "worn as a %s", body ? "head" : "body");
    }
    return last = e;
}

static void ensureBuilt(struct centry *e)
{
    if (e->state != 0) {
        return;
    }
    if (!whiteTile) {
        whiteTile = newTile(0xffff);
    }
    if (e->body) {
        /* entryFor may move the array: look this one up again after */
        ModelFileHeader *hdr = e->hdr, *body = e->body;
        struct centry *b = entryFor(body, NULL);
        if (b) {
            ensureBuilt(b);
        }
        e = entryFor(hdr, body);
        b = entryFor(body, NULL);
        if (!e) {
            return;
        }
        if (!b || b->state != 1 || !b->rig) {
            e->state = -1;
            snprintf(e->why, sizeof(e->why), "its body is GoldenEye's own");
            return;
        }
        const u64 tb = sysGetMicroseconds();
        buildHead(e, b);
        g_beanBuildUs += sysGetMicroseconds() - tb;
    } else {
        const u64 tb = sysGetMicroseconds();
        buildBody(e);
        g_beanBuildUs += sysGetMicroseconds() - tb;
    }
    if (e->state < 0) {
        ca.fellback++;
    }
    litApply(e);   /* round 32c: a new build takes the key's colours before its first draw */
}

/* The model's head file (the instance's own: heads are dealt per guard). */
static ModelFileHeader *headOf(Model *model, const struct centry *body)
{
    if (!body->headspot) {
        return NULL;
    }
    union ModelRwData *rw = modelGetNodeRwData(model, body->headspot);
    return rw ? rw->HeadPlaceholder.ModelFileHeader : NULL;
}

/* ------------------------------------------------------------- public */

s32 beanChrNode(struct Model *model, struct ModelNode *node, const struct beanmodelnode **out)
{
    if (!model || !model->obj || !model->chr || envOff() || !beanStageModelsEnabled()) {
        return BEANMODEL_N64;
    }
    /* round 26 merge: an entry cached while an alias copy held Bond must not
     * be served once the copy holds a gun (its node pointers may coincide) */
    if (aliasStale(model->obj)) {
        return BEANMODEL_N64;
    }
    struct centry *b = entryFor(model->obj, NULL);
    if (!b || b->state < 0) {
        return BEANMODEL_N64;
    }
    ensureBuilt(b);
    b = entryFor(model->obj, NULL);
    if (!b || b->state != 1) {
        return BEANMODEL_N64;
    }
    ModelFileHeader *head = headOf(model, b);
    struct centry *h = NULL;
    if (head && chrRows[b->row].kind == BEANCHR_WHOLE) {
        return BEANMODEL_N64;   /* a whole character wearing a head file: not a pairing the table makes */
    }
    if (head && aliasStale(head)) {
        return BEANMODEL_N64;
    }
    if (head) {
        h = entryFor(head, model->obj);
        if (h && h->state == 0) {
            ensureBuilt(h);
            h = entryFor(head, model->obj);
            b = entryFor(model->obj, NULL);
        }
        if (!h || h->state != 1 || !b) {
            return BEANMODEL_N64;   /* the whole character stays GoldenEye's */
        }
    }
    const struct centry *in = NULL;
    s32 at = -1;
    for (s32 i = 0; i < b->numnodes && at < 0; i++) {
        if (b->nodes[i].node == node) {
            in = b;
            at = i;
        }
    }
    for (s32 i = 0; h && i < h->numnodes && at < 0; i++) {
        if (h->nodes[i].node == node) {
            in = h;
            at = i;
        }
    }
    static s32 headboth = -1;   /* Midas round 28 (doc 38 item 7): once, not per node per frame */
    if (headboth < 0) {
        headboth = getenv("GE_BEANHEADBOTH") != NULL;
    }
    if (in == h && h && headboth) {
        return BEANMODEL_N64;   /* dev (D-245): GoldenEye's head drawn too, over the HD one */
    }
    if (at < 0) {
        return BEANMODEL_N64;
    }
    const struct cnode *cn = &in->nodes[at];
    if (!cn->candidate) {
        return BEANMODEL_N64;   /* under a switch: GoldenEye's own (shows and hides with it) */
    }
    if (in == b && at == b->necknode && h) {
        /* the head's HD lists, at the body's neck (the body has no triangles
         * of its own here); hidden with the head's root toggle */
        if (h->rootswitch) {
            /* round 28b (D-181): through this body's own placeholder, not
             * modelGetNodeRwData()'s walk up the head's Parent link - the
             * link is per file and may still name another body's
             * placeholder (a head file worn by two body files): Silo crashed
             * here (0.0.0.17 too) on a pointer read out of the wrong pool */
            union ModelRwData *rw = headRwData(model, b, h, h->rootswitch, h->rootswitch->Data->Switch.RwDataIndex);
            if (!rw || !rw->Switch.visible) {
                return BEANMODEL_SKIP;
            }
        }
        if (h->headdraw.numtris == 0) {
            return BEANMODEL_SKIP;
        }
        neckDraws++;
        *out = paintedOut(model, h, b, &h->headdraw, -1);
        return BEANMODEL_DRAW;
    }
    if ((node->Opcode & 0xff) == MODELNODE_OPCODE_DLCOLLISION) {
        union ModelRwData *rw = modelGetNodeRwData(model, node);
        if (!rw || !rw->DisplayListCollisions.gdl) {
            return BEANMODEL_SKIP;   /* GoldenEye draws nothing here this frame */
        }
    }
    if (cn->numtris == 0) {
        return BEANMODEL_SKIP;
    }
    *out = paintedOut(model, b, NULL, cn, at);
    return BEANMODEL_DRAW;
}

void beanChrLevelLoaded(void)
{
    /* as beanModelLevelLoaded(): free the set retired a level ago (the
     * camera-latch rerun may still read the last level's lists), retire this */
    for (s32 i = 0; i < numRetired; i++) {
        freeEntry(&retired[i]);
    }
    free(retired);
    retired = entries;
    numRetired = numEntries;
    entries = NULL;
    numEntries = capEntries = 0;
    prebuilt = 0;
    paintLevel();
    meshBytes = picBytes = 0;
    buildMs = 0.0;
    memset(&ca, 0, sizeof(ca));
}

void beanChrFrame(void)
{
    /* dev: GE_BEANCHRLOG=2 logs every character's position every 100 frames */
    static u32 frames;
    paintSweep();
    /* round 32c (D-300): Video.LitCharacters is live - every built
     * character's vertices take the key's colours, and the blood copies made
     * from the other set are remade */
    {
        s32 changed = 0;
        litParams();
        litWant = (s8)(cfgLit ? 1 : 0);
        for (s32 i = 0; i < numEntries; i++) {
            changed |= litApply(&entries[i]);
        }
        if (changed) {
            paintLevel();
        }
        chrLight.on = (u8)litWant;   /* the lists light only once every vertex holds its normal */
    }
    if (logOn() > 1 && (frames++ % 100) == 0) {
        s32 k = 0;
        sysLogPrintf(LOG_NOTE, "beanchr: frame %u: %.1f HD characters drawn a frame (neck lists, both passes) over the last 100; "
                     "blood: %u painted lists drawn, %u copies made, %d live, %d retired",
                     frames - 1, neckDraws / 100.0, paintDraws, paintCopies, numPinst, numPdead);
        neckDraws = 0;
        paintDraws = paintCopies = 0;
        for (PropRecord *pr = g_ActivePropsHead; pr && k < 4096; pr = pr->next, k++) {
            if ((pr->type == PROP_TYPE_CHR || pr->type == PROP_TYPE_PLAYER) && pr->chr) {
                sysLogPrintf(LOG_NOTE, "beanchr: frame %u chr %d type %d at %.0f,%.0f,%.0f room %d", frames - 1,
                             pr->chr->chrnum, pr->type, pr->pos.x, pr->pos.y, pr->pos.z, pr->rooms[0]);
            }
        }
    }
    if (envOff() || !beanStageModelsEnabled() || !g_ActivePropsHead) {
        return;
    }
    /* After the level's first frame, a character placed later (Bond's own
     * body for the intro, a clone, a spawned guard) is built here off the
     * draw, checked every 30 frames; the stage line is then followed by one
     * "[xbla] characters (later)" line with the new totals. */
    static u32 later;
    if (prebuilt) {
        if ((later++ % 30) != 0) {
            return;
        }
        const s32 before = ca.bodies + ca.heads + ca.fellback;
        s32 k = 0;
        for (PropRecord *pr = g_ActivePropsHead; pr && k < 4096; pr = pr->next, k++) {
            if ((pr->type != PROP_TYPE_CHR && pr->type != PROP_TYPE_PLAYER) || !pr->chr || !pr->chr->model
                    || !pr->chr->model->obj) {
                continue;
            }
            Model *model = pr->chr->model;
            struct centry *b = entryFor(model->obj, NULL);
            if (!b) {
                continue;
            }
            ensureBuilt(b);
            b = entryFor(model->obj, NULL);
            ModelFileHeader *head = b && b->state == 1 ? headOf(model, b) : NULL;
            struct centry *h = head ? entryFor(head, model->obj) : NULL;
            if (h) {
                ensureBuilt(h);
            }
        }
        if (ca.bodies + ca.heads + ca.fellback != before) {
            char line[512];
            s32 unres = 0;
            beanChrDescribe(line, sizeof(line), &unres);
            sysLogPrintf(LOG_NOTE, "[xbla] characters (later): %s; %d unresolved reference%s", line, unres,
                         unres == 1 ? "" : "s");
        }
        return;
    }
    later = 1;
    prebuilt = 1;
    const u64 t0 = sysGetMicroseconds();
    s32 n = 0, chrs = 0;
    for (PropRecord *pr = g_ActivePropsHead; pr && n < 4096; pr = pr->next, n++) {
        /* guards (CHR) and Bond's own body in third person (PLAYER) */
        if ((pr->type != PROP_TYPE_CHR && pr->type != PROP_TYPE_PLAYER) || !pr->chr || !pr->chr->model
                || !pr->chr->model->obj) {
            continue;
        }
        Model *model = pr->chr->model;
        chrs++;
        struct centry *b = entryFor(model->obj, NULL);
        if (!b) {
            continue;
        }
        ensureBuilt(b);
        b = entryFor(model->obj, NULL);
        if (b && b->state == 1) {
            ModelFileHeader *head = headOf(model, b);
            if (head) {
                struct centry *h = entryFor(head, model->obj);
                if (h) {
                    ensureBuilt(h);
                }
            }
        }
        if (logOn()) {
            b = entryFor(model->obj, NULL);
            ModelFileHeader *head = b && b->state == 1 ? headOf(model, b) : NULL;
            sysLogPrintf(LOG_NOTE, "beanchr: chr %d %s head %s at %.0f,%.0f,%.0f", pr->chr->chrnum,
                         fileOf(model->obj) ? fileOf(model->obj) : "?", head && fileOf(head) ? fileOf(head) : "-",
                         pr->pos.x, pr->pos.y, pr->pos.z);
        }
    }
    sysLogPrintf(LOG_NOTE, "beanchr: level characters: %d live, %d bodies and %d heads HD, %d GoldenEye's own; %.1f ms "
                 "(builds %.1f); mesh %u KB, pictures %u KB",
                 chrs, ca.bodies, ca.heads, ca.fellback, msSince(t0), buildMs, meshBytes / 1024, picBytes / 1024);
}

void beanChrDescribe(char *buf, u32 len, s32 *unresolved)
{
    s32 n = 0;
    char reasons[256] = "";
    for (s32 i = 0; i < numEntries && n < 3; i++) {
        if (entries[i].state < 0 && entries[i].row >= 0) {
            const size_t at = strlen(reasons);
            snprintf(reasons + at, sizeof(reasons) - at, "%s%s: %s", n ? "; " : "", entries[i].file ? entries[i].file : "?",
                     entries[i].why);
            n++;
        }
    }
    snprintf(buf, len, "characters %s%d bodies and %d heads HD, %d GoldenEye's own%s%s%s, %d unmapped, pictures %d of %d resolved",
             envOff() ? "off (GE_BEANCHRS=0) " : "", ca.bodies, ca.heads, ca.fellback, n ? " (" : "", reasons, n ? ")" : "",
             ca.unmapped, ca.decoded, ca.pictures);
    if (unresolved) {
        *unresolved = ca.refs;
    }
}
