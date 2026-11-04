/* Imported from GoldenEye-Decomp-ios engine/port/src/beangun.c (first 6628f24c, last changed 6fec0083; donor HEAD 55a3a9c8),
 * round 14, XBLA step 4.3 (docs/port-ios/23-round14-props-guns.md). 64-bit host layer only.
 * Changes against the donor are marked "Midas round 14". */
/*
 * GoldenEye XBLA's HD first-person guns on GoldenEye's own gun models - see
 * port/include/beangun.h and docs/tier-c.md "What C4 guns built". Tier C
 * round 26a (DECISIONS D-144..D-146).
 *
 * Shape from Perfect Dark's gebean.c gebeanBuildFirstPerson() (perfect-dark-ios
 * vendor/dabs-mod, MIT) in its fpOnOwn form, where PD draws the release's gun
 * on GoldenEye's own first-person model out of the ROM: Bean's scale (1/4.7),
 * the original's place walked onto GoldenEye's vertices (here offline,
 * scripts/beanfit/gunfit.py), the flash cards (fpDrawIsCard: piece 0) kept off
 * the gun. PD skins Bean's bones to the host's matrices; this deals each bone
 * to the node it lies on, which is the same answer on GoldenEye's own model.
 *
 * MEMORY RULE (charter rule 3c, D-110): everything here is host malloc.
 */

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "bondconstants.h"
#include "bondtypes.h"
#include "beangun.h"
#include "system.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

/* engine/src/game/gun.c (PORT) */
const char *gunBeanModelFile(const ModelFileHeader *hdr);

/* The parts gunfire.c drives (its Switches[] indices): 1 the flash switch
 * (on for the frames of a shot), 8..13 the hand's pieces
 * (sub_GAME_7F05E978: on in the hand, off on the watch's inventory page). */
#define PART_FLASH 1
#define PART_HAND_FIRST 8
#define PART_HAND_LAST 13

/* dev: GE_BEANGUNS=0 leaves every first-person gun GoldenEye's own (the
 * cost A/B, M-071), props and rooms unchanged */
static s32 gunsOff(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANGUNS");
        v = e && *e == '0';
    }
    return v;
}

const char *beanGunFile(const ModelFileHeader *hdr)
{
    return gunsOff() ? NULL : gunBeanModelFile(hdr);
}

static s32 under(const ModelNode *node, const ModelNode *up)
{
    for (const ModelNode *n = node; n && up; n = n->Parent) {
        if (n == up) {
            return 1;
        }
    }
    return 0;
}

s32 beanGunNodeClass(const ModelFileHeader *hdr, const ModelNode *node)
{
    if (!hdr || !hdr->Switches) {
        return BEANGUN_NODE_GUN;
    }
    if (hdr->numSwitches > PART_FLASH && under(node, hdr->Switches[PART_FLASH])) {
        return BEANGUN_NODE_FLASH;
    }
    for (s32 k = PART_HAND_FIRST; k <= PART_HAND_LAST && k < hdr->numSwitches; k++) {
        if (under(node, hdr->Switches[k])) {
            return BEANGUN_NODE_HAND;
        }
    }
    return BEANGUN_NODE_GUN;
}

u32 beanGunTint(const char *source)
{
    if (source && (strcmp(source, "gun/goldengun") == 0 || strcmp(source, "prop/chrgolden") == 0)) {
        return 0xfff0c86eu;   /* PD fpTint[WEAPON_GE_GOLDENGUN] */
    }
    return 0;
}

s32 beanGunPictureIsGlove(s32 w, s32 h)
{
    return w == 512 && h == 511;
}

s32 beanGunDrawIsCard(s32 piece)
{
    return piece == 0;
}

/* the squared distance (one expression, shared by both searches below so
 * they round alike) */
static inline f32 sq3(f32 dx, f32 dy, f32 dz)
{
    return dx * dx + dy * dy + dz * dz;
}

/* nearest vertex of node i to p (squared distance), model space */
static f32 nodeDist(const struct beangunnode *n, const f32 *p)
{
    const Vertex *v = n->verts;
    f32 best = 1e30f;
    for (s32 k = 0; k < n->numverts; k++) {
        const f32 x = v[k].coord.x + n->origin[0];
        const f32 y = v[k].coord.y + n->origin[1];
        const f32 z = v[k].coord.z + n->origin[2];
        const f32 d = sq3(x - p[0], y - p[1], z - p[2]);
        if (d < best) {
            best = d;
        }
    }
    return best;
}

/* round 35a (D-318): the deal, searched instead of scanned. Every
 * triangle's nearest list is the list of the nearest eligible VERTEX (on a
 * tie, the lowest list: what the per-list scan picks), so each eligible set
 * (all lists; the lists under one matrix; the hand's or the gun's in a glove
 * split) is put in a uniform grid once and searched outward from the
 * triangle's middle, cell shell by shell, until nothing nearer can remain.
 * Exact: a vertex's distance is the same float expression as the scan's
 * (sq3), a cell or shell is skipped only when its lower bound (cell bounds
 * widened by a thousandth of a cell, margin 1.0001) is above the best so far,
 * and a minimum does not depend on visiting order.
 * GE_BEANGUNDEALBRUTE=1 scans every vertex as before (the A/B);
 * GE_BEANGUNDEALCHECK=1 runs both, logs any triangle dealt differently and
 * times each. */
#define DEAL_MARGIN 1.0001f
#define DEAL_GRIDMAX 48

struct vgrid {
    s32 num;
    f32 *x, *y, *z;   /* in cell order */
    s16 *node;
    s32 *start;       /* cells + 1 */
    s32 dim[3];
    f32 lo[3], cs[3];
    f32 csmin;        /* smallest cell size along an axis with more than one cell */
};

static s32 dealBrute(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANGUNDEALBRUTE");
        v = e && *e == '1';
    }
    return v;
}

float floorf(float x);
float cbrtf(float x);
float sqrtf(float x);

static s32 cellOf(const struct vgrid *g, s32 a, f32 v)
{
    const f32 f = (v - g->lo[a]) / g->cs[a];
    s32 c = f < 0.0f ? 0 : f >= (f32)g->dim[a] ? g->dim[a] - 1 : (s32)floorf(f);
    return c < 0 ? 0 : c >= g->dim[a] ? g->dim[a] - 1 : c;
}

/* eligible: not the flash, has vertices; m >= 0 its matrix; cf 1 the hand's
 * lists only, 2 every list but the hand's */
static s32 eligible(const struct beangunnode *n, s32 m, s32 cf)
{
    if (n->cls == BEANGUN_NODE_FLASH || n->numverts <= 0) {
        return 0;
    }
    if (m >= 0 && n->mtx != m) {
        return 0;
    }
    if (cf == 1 && n->cls != BEANGUN_NODE_HAND) {
        return 0;
    }
    if (cf == 2 && n->cls == BEANGUN_NODE_HAND) {
        return 0;
    }
    return 1;
}

static void gridFree(struct vgrid *g)
{
    if (g) {
        free(g->x);
        free(g->node);
        free(g->start);
        free(g);
    }
}

static struct vgrid *gridBuild(const struct beangunnode *nodes, s32 numnodes, s32 m, s32 cf)
{
    struct vgrid *g = calloc(1, sizeof(*g));
    if (!g) {
        return NULL;
    }
    s32 num = 0;
    for (s32 i = 0; i < numnodes; i++) {
        num += eligible(&nodes[i], m, cf) ? nodes[i].numverts : 0;
    }
    g->num = num;
    f32 hi[3] = { -1e30f, -1e30f, -1e30f };
    g->lo[0] = g->lo[1] = g->lo[2] = 1e30f;
    for (s32 i = 0; i < numnodes; i++) {
        if (!eligible(&nodes[i], m, cf)) {
            continue;
        }
        const Vertex *v = nodes[i].verts;
        for (s32 k = 0; k < nodes[i].numverts; k++) {
            const f32 p[3] = { v[k].coord.x + nodes[i].origin[0], v[k].coord.y + nodes[i].origin[1],
                               v[k].coord.z + nodes[i].origin[2] };
            for (s32 a = 0; a < 3; a++) {
                g->lo[a] = p[a] < g->lo[a] ? p[a] : g->lo[a];
                hi[a] = p[a] > hi[a] ? p[a] : hi[a];
            }
        }
    }
    if (num == 0) {
        return g;
    }
    f32 ext[3], vol = 1.0f;
    for (s32 a = 0; a < 3; a++) {
        ext[a] = hi[a] - g->lo[a];
        if (ext[a] < 1.0f) {
            ext[a] = 1.0f;
        }
        vol *= ext[a];
    }
    const f32 side = cbrtf(vol / (f32)(num / 4 > 0 ? num / 4 : 1));
    s32 cells = 1;
    g->csmin = 1e30f;
    for (s32 a = 0; a < 3; a++) {
        s32 d = side > 0.0f ? (s32)(ext[a] / side + 0.5f) : 1;
        d = d < 1 ? 1 : d > DEAL_GRIDMAX ? DEAL_GRIDMAX : d;
        g->dim[a] = d;
        g->cs[a] = ext[a] / (f32)d;
        if (d > 1 && g->cs[a] < g->csmin) {
            g->csmin = g->cs[a];
        }
        cells *= d;
    }
    g->x = malloc(sizeof(f32) * 3 * (size_t)num);
    g->node = malloc(sizeof(s16) * (size_t)num);
    g->start = calloc((size_t)cells + 1, sizeof(s32));
    s32 *cellof = malloc(sizeof(s32) * (size_t)num);
    if (!g->x || !g->node || !g->start || !cellof) {
        free(cellof);
        gridFree(g);
        return NULL;
    }
    g->y = g->x + num;
    g->z = g->y + num;
    s32 n = 0;
    for (s32 i = 0; i < numnodes; i++) {
        if (!eligible(&nodes[i], m, cf)) {
            continue;
        }
        const Vertex *v = nodes[i].verts;
        for (s32 k = 0; k < nodes[i].numverts; k++, n++) {
            const f32 x = v[k].coord.x + nodes[i].origin[0];
            const f32 y = v[k].coord.y + nodes[i].origin[1];
            const f32 z = v[k].coord.z + nodes[i].origin[2];
            cellof[n] = (cellOf(g, 2, z) * g->dim[1] + cellOf(g, 1, y)) * g->dim[0] + cellOf(g, 0, x);
            g->start[cellof[n] + 1]++;
        }
    }
    for (s32 c = 0; c < cells; c++) {
        g->start[c + 1] += g->start[c];
    }
    s32 *fill = malloc(sizeof(s32) * (size_t)cells);
    if (!fill) {
        free(cellof);
        gridFree(g);
        return NULL;
    }
    memcpy(fill, g->start, sizeof(s32) * (size_t)cells);
    n = 0;
    for (s32 i = 0; i < numnodes; i++) {
        if (!eligible(&nodes[i], m, cf)) {
            continue;
        }
        const Vertex *v = nodes[i].verts;
        for (s32 k = 0; k < nodes[i].numverts; k++, n++) {
            const s32 at = fill[cellof[n]]++;
            g->x[at] = v[k].coord.x + nodes[i].origin[0];
            g->y[at] = v[k].coord.y + nodes[i].origin[1];
            g->z[at] = v[k].coord.z + nodes[i].origin[2];
            g->node[at] = (s16)i;
        }
    }
    free(fill);
    free(cellof);
    return g;
}

/* lower bound of the squared distance from p to cell (i, j, k), widened */
static f32 cellLow(const struct vgrid *g, const s32 *c, const f32 *p)
{
    f32 d = 0.0f;
    for (s32 a = 0; a < 3; a++) {
        const f32 w = g->cs[a] * 0.001f;
        const f32 lo = g->lo[a] + (f32)c[a] * g->cs[a] - w;
        const f32 hi = g->lo[a] + (f32)(c[a] + 1) * g->cs[a] + w;
        f32 dx = 0.0f;
        if (p[a] < lo) {
            dx = lo - p[a];
        } else if (p[a] > hi) {
            dx = p[a] - hi;
        }
        d += dx * dx;
    }
    return d;
}

static void cellScan(const struct vgrid *g, s32 i, s32 j, s32 k, const f32 *p, f32 *best, s32 *bn)
{
    const s32 c[3] = { i, j, k };
    if (*bn >= 0 && cellLow(g, c, p) > *best * DEAL_MARGIN) {
        return;
    }
    const s32 cell = (k * g->dim[1] + j) * g->dim[0] + i;
    for (s32 v = g->start[cell]; v < g->start[cell + 1]; v++) {
        const f32 d = sq3(g->x[v] - p[0], g->y[v] - p[1], g->z[v] - p[2]);
        if (d < *best || (d == *best && g->node[v] < *bn)) {
            *best = d;
            *bn = g->node[v];
        }
    }
}

/* the nearest vertex's list (lowest list on a tie); -1 for an empty set.
 * Shells round the point's cell until something is found, then every cell
 * of the box the best distance so far spans (each skipped when its own lower
 * bound is already farther): a vertex as near as the best lies in that box,
 * since cellOf() is monotonic and the vertex was filed by it. */
static s32 gridNearest(const struct vgrid *g, const f32 *p)
{
    f32 best = 1e30f;
    s32 bn = -1;
    if (!g || g->num == 0) {
        return -1;
    }
    s32 c0[3], maxr = 0;
    for (s32 a = 0; a < 3; a++) {
        c0[a] = cellOf(g, a, p[a]);
        const s32 r = c0[a] > g->dim[a] - 1 - c0[a] ? c0[a] : g->dim[a] - 1 - c0[a];
        maxr = r > maxr ? r : maxr;
    }
    s32 ra = 0;   /* shells 0..ra are scanned */
    for (s32 r = 0; r <= maxr; r++) {
        ra = r;
        for (s32 k = c0[2] - r; k <= c0[2] + r; k++) {
            if (k < 0 || k >= g->dim[2]) {
                continue;
            }
            for (s32 j = c0[1] - r; j <= c0[1] + r; j++) {
                if (j < 0 || j >= g->dim[1]) {
                    continue;
                }
                const s32 edge = k == c0[2] - r || k == c0[2] + r || j == c0[1] - r || j == c0[1] + r;
                for (s32 i = c0[0] - r; i <= c0[0] + r; i += (edge || r == 0) ? 1 : 2 * r) {
                    if (i >= 0 && i < g->dim[0]) {
                        cellScan(g, i, j, k, p, &best, &bn);
                    }
                }
            }
        }
        if (bn >= 0) {
            break;
        }
    }
    if (bn < 0) {
        return -1;
    }
    /* the box of cells within the best distance (widened), outside the shells */
    const f32 rad = sqrtf(best * DEAL_MARGIN) * 1.001f + 1e-3f;
    s32 lo[3], hi[3];
    for (s32 a = 0; a < 3; a++) {
        lo[a] = cellOf(g, a, p[a] - rad);
        hi[a] = cellOf(g, a, p[a] + rad);
    }
    for (s32 k = lo[2]; k <= hi[2]; k++) {
        for (s32 j = lo[1]; j <= hi[1]; j++) {
            for (s32 i = lo[0]; i <= hi[0]; i++) {
                const s32 di = i > c0[0] ? i - c0[0] : c0[0] - i;
                const s32 dj = j > c0[1] ? j - c0[1] : c0[1] - j;
                const s32 dk = k > c0[2] ? k - c0[2] : c0[2] - k;
                if (di <= ra && dj <= ra && dk <= ra) {
                    continue;   /* scanned with the shells */
                }
                cellScan(g, i, j, k, p, &best, &bn);
            }
        }
    }
    return bn;
}

struct dealq {
    const struct beanguntri *tris;
    const struct beangunnode *nodes;
    s32 numnodes;
    s32 split;
    s32 brute;
    struct vgrid *grids[65][3];   /* [matrix + 1][class filter], built when first asked */
};

/* the triangle's nearest eligible node (lowest index on a tie), among those
 * drawn under matrix m (m < 0: any); -1 when none is eligible */
static s32 dealNearest(struct dealq *q, s32 t, s32 m)
{
    const struct beanguntri *tri = &q->tris[t];
    const s32 cf = q->split ? (tri->glove != 0 ? 1 : 2) : 0;
    if (!q->brute && m < 64) {
        struct vgrid **g = &q->grids[m + 1][cf];
        if (!*g) {
            *g = gridBuild(q->nodes, q->numnodes, m, cf);
        }
        if (*g) {
            return gridNearest(*g, tri->mid);
        }
    }
    f32 best = 1e30f;
    s32 bi = -1;
    for (s32 i = 0; i < q->numnodes; i++) {
        if (!eligible(&q->nodes[i], m, cf)) {
            continue;
        }
        const f32 d = nodeDist(&q->nodes[i], tri->mid);
        if (d < best) {
            best = d;
            bi = i;
        }
    }
    return bi;
}

static s32 dealWith(const struct beanguntri *tris, s32 numtris, const struct beangunnode *nodes, s32 numnodes,
                    s16 *outnode, s32 *outbodymtx, s32 brute);

s32 beanGunDeal(const struct beanguntri *tris, s32 numtris, const struct beangunnode *nodes, s32 numnodes,
                s16 *outnode, s32 *outbodymtx)
{
    static int check = -1;
    if (check < 0) {
        const char *e = getenv("GE_BEANGUNDEALCHECK");
        check = e && *e == '1';
    }
    const s32 r = dealWith(tris, numtris, nodes, numnodes, outnode, outbodymtx, dealBrute());
    if (check && numtris > 0) {
        s16 *b = malloc(sizeof(s16) * (size_t)numtris);
        s32 bm = -1;
        if (b) {
            const s32 rb = dealWith(tris, numtris, nodes, numnodes, b, &bm, 1);
            s32 diff = 0;
            for (s32 t = 0; t < numtris; t++) {
                diff += b[t] != outnode[t];
            }
            const s32 bodymtx = outbodymtx ? *outbodymtx : -2;
            /* best of 5 of each, back to back (the cost A/B on a busy machine) */
            u64 best[2] = { ~0ull, ~0ull };
            for (s32 k = 0; k < 10; k++) {
                s32 scratch;
                const u64 t0 = sysGetMicroseconds();
                dealWith(tris, numtris, nodes, numnodes, b, &scratch, k & 1);
                const u64 us = sysGetMicroseconds() - t0;
                best[k & 1] = us < best[k & 1] ? us : best[k & 1];
            }
            sysLogPrintf(LOG_NOTE, "beangun: deal check: %d of %d triangles differ from the brute deal, groups %d/%d, body mtx %d/%d; "
                         "best of 5 %.2f ms searched, %.2f ms brute (%d lists)",
                         diff, numtris, r, rb, bodymtx, bm, (double)best[0] / 1000.0, (double)best[1] / 1000.0, numnodes);
            free(b);
        }
    }
    return r;
}

static s32 dealWith(const struct beanguntri *tris, s32 numtris, const struct beangunnode *nodes, s32 numnodes,
                    s16 *outnode, s32 *outbodymtx, s32 brute)
{
    s16 *near = malloc(sizeof(s16) * (size_t)(numtris > 0 ? numtris : 1));
    s32 *count = calloc(257, sizeof(s32));          /* triangles per group; 256 = no bone */
    s32 *votes = calloc((size_t)257 * (size_t)(numnodes > 0 ? numnodes : 1), sizeof(s32));
    struct dealq *q = calloc(1, sizeof(*q));
    s32 dealtWhole = 0;
    s32 body = 256;
    s32 bodymtx = -1;

    if (outbodymtx) {
        *outbodymtx = -1;
    }
    if (!near || !count || !votes || !q) {
        free(near);
        free(count);
        free(votes);
        free(q);
        for (s32 t = 0; t < numtris; t++) {
            outnode[t] = -1;
        }
        return 0;
    }

    /* round 31c (D-281): the glove's triangles are the hand's, the rest the
     * gun's, when the file has both and the model both kinds of list */
    s32 hasglove = 0, hashand = 0, hasgun = 0;
    for (s32 t = 0; t < numtris; t++) {
        hasglove |= tris[t].glove != 0;
    }
    for (s32 i = 0; i < numnodes; i++) {
        hashand |= nodes[i].cls == BEANGUN_NODE_HAND && nodes[i].numverts > 0;
        hasgun |= nodes[i].cls == BEANGUN_NODE_GUN && nodes[i].numverts > 0;
    }
    const s32 split = hasglove && hashand && hasgun;
    q->tris = tris;
    q->nodes = nodes;
    q->numnodes = numnodes;
    q->split = split;
    q->brute = brute;

    /* every triangle's nearest node of any class but the flash */
    for (s32 t = 0; t < numtris; t++) {
        const s32 bi = dealNearest(q, t, -1);
        near[t] = (s16)bi;
        const s32 g = tris[t].group >= 0 && tris[t].group < 256 ? tris[t].group : 256;
        count[g]++;
        if (bi >= 0) {
            votes[(size_t)g * (size_t)numnodes + (size_t)bi]++;
        }
    }

    /* the body: the bone most triangles ride (a file with no bones is all
     * body) */
    if (count[256] == 0) {
        s32 most = 0;
        for (s32 g = 0; g < 256; g++) {
            if (count[g] > most) {
                most = count[g];
                body = g;
            }
        }
    }

    /* the body's matrix: the one most of its triangles' nearest lists are
     * drawn under */
    {
        s32 mv[64];
        memset(mv, 0, sizeof(mv));
        for (s32 t = 0; t < numtris; t++) {
            const s32 g = tris[t].group >= 0 && tris[t].group < 256 ? tris[t].group : 256;
            if ((g == body || g == 256) && near[t] >= 0 && nodes[near[t]].mtx >= 0 && nodes[near[t]].mtx < 64) {
                mv[nodes[near[t]].mtx]++;
            }
        }
        s32 most = 0;
        for (s32 m = 0; m < 64; m++) {
            if (mv[m] > most) {
                most = mv[m];
                bodymtx = m;
            }
        }
    }

    /* each bone's matrix: the body's own; a moving part's bone (the slide,
     * the trigger finger, the drum) the matrix other than the body's that
     * most of its triangles lie nearest, when a quarter of them do - else it
     * does not move in GoldenEye and stays with the body */
    s32 gmtx[257];
    for (s32 g = 0; g < 257; g++) {
        gmtx[g] = bodymtx;
        if (g == body || g == 256 || count[g] == 0) {
            continue;
        }
        s32 mv[64];
        memset(mv, 0, sizeof(mv));
        for (s32 i = 0; i < numnodes; i++) {
            if (nodes[i].mtx >= 0 && nodes[i].mtx < 64) {
                mv[nodes[i].mtx] += votes[(size_t)g * (size_t)numnodes + (size_t)i];
            }
        }
        s32 bestm = -1, bestv = 0;
        for (s32 m = 0; m < 64; m++) {
            if (m != bodymtx && mv[m] > bestv) {
                bestv = mv[m];
                bestm = m;
            }
        }
        if (bestm >= 0 && bestv * 4 >= count[g]) {
            gmtx[g] = bestm;
            dealtWhole++;
        }
    }

    /* each triangle: the nearest list drawn under its bone's matrix (a
     * hand's list, a shell's, the frame's - visibility follows the list) */
    for (s32 t = 0; t < numtris; t++) {
        const s32 g = tris[t].group >= 0 && tris[t].group < 256 ? tris[t].group : 256;
        const s32 m = gmtx[g];
        const s32 bi = m >= 0 ? dealNearest(q, t, m) : -1;
        outnode[t] = (s16)(bi >= 0 ? bi : near[t]);
    }
    if (outbodymtx) {
        *outbodymtx = bodymtx;
    }
    for (s32 m = 0; m < 65; m++) {
        for (s32 cf = 0; cf < 3; cf++) {
            gridFree(q->grids[m][cf]);
        }
    }
    free(near);
    free(count);
    free(votes);
    free(q);
    return dealtWhole;
}
