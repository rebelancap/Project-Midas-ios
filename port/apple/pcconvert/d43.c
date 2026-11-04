/* Imported from GoldenEye-Decomp-ios engine/port/src/pcconvert/d43.c at 55a3a9c8
 * (donor HEAD), Phase 1 round 7a
 * (docs/port-ios/11-round7a-pcconvert.md). Unchanged: every record size and field
 * offset it writes is pinned against Midas's headers by pcconvert_layout.c. */
/*
 * D43/D50: C port of tools_pc/d43_emit.py — convert every N64 model file to
 * PC layout and write RZ-compressed sidecars into pcmodels.bin.
 *
 * Structure follows the Python one-for-one so the two can be diffed:
 *   build_nodes -> d43BuildNodes      (mutable node map)
 *   visit_seq   -> d43VisitSeq        (exact modelIterateDisplayLists order)
 *   placement_order -> d43PlacementOrder
 *   layout + emit + compress -> d43Process
 *
 * Deliberately NOT ported: the Python's round-trip `validate()` and the
 * buffer-fit `chk()` cross-checks. Neither writes a byte of output — they are
 * development assertions about the emitted image and the runtime buffer sizes,
 * already green for this ROM, and re-running them on every boot would cost the
 * user seconds for nothing. scripts/verify-sidecars.sh compares the C output
 * against the validated Python output byte-for-byte, which subsumes them.
 */

#include <stdlib.h>
#include <string.h>


#include "pcconvert_internal.h"

/* ------------------------------------------------------------- constants ---*/
static int n64RecSize(int op)
{
    switch (op) {
    case 1: case 2: case 3: case 10: case 15: return 0x1C;
    case 4: case 21: return 0x14;
    case 8: case 22: return 0x10;
    case 9:  return 0x24;
    case 12: return 0x28;
    case 13: case 17: case 24: return 0x20;
    case 18: return 0x08;
    case 23: return 0x02;
    default: return -1;
    }
}
static int pcRecSize(int op)
{
    switch (op) {
    case 1: case 8: return 24;
    case 2: case 3: case 4: return 40;
    case 9: case 12: return 48;
    case 10: case 15: return 28;
    case 13: return 48;
    case 18: return 16;
    case 21: return 20;
    case 22: return 32;
    case 23: return 2;
    case 24: return 64;
    default: return -1;   /* 17 has an N64 size but no PC size (as in Python) */
    }
}

/* ------------------------------------------------------------- node map ---*/
typedef struct {
    uint32_t off;
    uint8_t  op;
    uint32_t data, parent, next, prev, child;
} D43Node;

typedef struct {
    const uint8_t *src;
    size_t         D;
    D43Node       *nodes;
    int            n, cap;
    int32_t       *byOff;      /* file offset -> node index, -1 if absent */
} D43Map;

static inline uint32_t be32o(const uint8_t *b, size_t o)
{
    return pcvBE32(b, o) & PCV_MASK;
}

static int d43AddNode(D43Map *m, uint32_t off)
{
    D43Node *n;
    if (m->n >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 256;
        D43Node *nn = (D43Node *)realloc(m->nodes,
                        (size_t)(sizeof(D43Node) * (size_t)nc));
        if (!nn)
            return -1;
        m->nodes = nn;
        m->cap = nc;
    }
    n = &m->nodes[m->n];
    n->off    = off;
    n->op     = (uint8_t)(pcvBE16(m->src, off) & 0xFF);
    n->data   = be32o(m->src, off + 4);
    n->parent = be32o(m->src, off + 8);
    n->next   = be32o(m->src, off + 0xC);
    n->prev   = be32o(m->src, off + 0x10);
    n->child  = be32o(m->src, off + 0x14);
    m->byOff[off] = m->n;
    return m->n++;
}

static int d43BuildNodes(D43Map *m, uint32_t R0)
{
    uint32_t *stack;
    int sp = 0, scap = 1024;

    stack = (uint32_t *)malloc((size_t)(sizeof(uint32_t) * (size_t)scap));
    if (!stack)
        return 0;
    stack[sp++] = R0;
    while (sp > 0) {
        uint32_t o = stack[--sp];
        int ni;
        D43Node *n;
        if ((size_t)o >= m->D || m->byOff[o] >= 0)
            continue;
        if ((size_t)o + 0x18 > m->D) {
            pcvErrf("node %#x runs past EOF", o);
            free(stack);
            return 0;
        }
        ni = d43AddNode(m, o);
        if (ni < 0) { free(stack); return 0; }
        n = &m->nodes[ni];
        if (sp + 3 > scap) {
            uint32_t *ns;
            scap *= 2;
            ns = (uint32_t *)realloc(stack,
                    (size_t)(sizeof(uint32_t) * (size_t)scap));
            if (!ns) { free(stack); return 0; }
            stack = ns;
        }
        if (n->op == 8) {
            uint32_t q = be32o(m->src, n->data + 8);
            if (q) stack[sp++] = q;
        } else if (n->op == 18) {
            uint32_t q = be32o(m->src, n->data + 0);
            if (q) stack[sp++] = q;
        } else if (n->child) {
            stack[sp++] = n->child;
        }
        if (n->next)
            stack[sp++] = n->next;
    }
    free(stack);
    return 1;
}

static D43Node *d43Get(D43Map *m, uint32_t off)
{
    int32_t i;
    if ((size_t)off >= m->D)
        return NULL;
    i = m->byOff[off];
    return (i >= 0) ? &m->nodes[i] : NULL;
}

static void d43GdlOf(const uint8_t *src, const D43Node *n, uint32_t *p, uint32_t *s)
{
    if (n->op == 4 || n->op == 24) {
        *p = be32o(src, n->data + 0);
        *s = be32o(src, n->data + 4);
    } else if (n->op == 22) {
        *p = be32o(src, n->data + 8);
        *s = 0;
    } else {
        *p = 0; *s = 0;
    }
}

/* ------------------------------------------------- growable u32 vectors ---*/
typedef struct { uint32_t *v; int n, cap; } U32Vec;
static void vecInit(U32Vec *a) { a->v = NULL; a->n = a->cap = 0; }
static void vecFree(U32Vec *a) { free(a->v); a->v = NULL; a->n = a->cap = 0; }
static int vecPush(U32Vec *a, uint32_t x)
{
    if (a->n >= a->cap) {
        int nc = a->cap ? a->cap * 2 : 64;
        uint32_t *nv = (uint32_t *)realloc(a->v,
                        (size_t)(sizeof(uint32_t) * (size_t)nc));
        if (!nv) return 0;
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = x;
    return 1;
}
static int vecHas(const U32Vec *a, uint32_t x)
{
    int i;
    for (i = 0; i < a->n; i++)
        if (a->v[i] == x) return 1;
    return 0;
}
static int cmpU32d(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* --------------------------------------------------------- visit / place ---*/
static int d43VisitSeq(D43Map *m, uint32_t R0, U32Vec *seq)
{
    const uint8_t *src = m->src;
    uint32_t node = R0;
    int32_t prevNode = -1;          /* offset, or -1 for None */
    uint32_t prevGdl = 0;
    int guard = 0;

    while (node && guard < 200000) {
        D43Node *n = d43Get(m, node);
        uint32_t gdl = 0;
        guard++;
        if (!n) {
            pcvErrf("visit: node %#x missing from map", node);
            return 0;
        }
        if (n->op == 4 || n->op == 22 || n->op == 24) {
            uint32_t p, s;
            d43GdlOf(src, n, &p, &s);
            if ((int32_t)node != prevNode)
                gdl = p;
            else if (s && s != prevGdl)
                gdl = s;
        } else if (n->op == 8) {
            n->child = be32o(src, n->data + 8);
        } else if (n->op == 18) {
            n->child = be32o(src, n->data + 0);
        } else if (n->op == 9) {
            uint32_t node1 = be32o(src, n->data + 0x18);
            uint32_t node2 = be32o(src, n->data + 0x1C);
            if (node1) {
                D43Node *a;
                uint32_t loop = node1;
                n->child = node1;
                a = d43Get(m, node1);
                if (!a) { pcvErrf("bsp: node %#x missing", node1); return 0; }
                a->prev = 0;
                for (;;) {
                    D43Node *l = d43Get(m, loop);
                    if (!l) { pcvErrf("bsp: node %#x missing", loop); return 0; }
                    if (!l->next || l->next == node2) break;
                    loop = l->next;
                }
                d43Get(m, loop)->next = node2;
                if (node2) {
                    D43Node *b = d43Get(m, node2);
                    if (!b) { pcvErrf("bsp: node %#x missing", node2); return 0; }
                    b->prev = loop;
                    loop = node2;
                    for (;;) {
                        D43Node *l = d43Get(m, loop);
                        if (!l->next || l->next == node1) break;
                        loop = l->next;
                    }
                    d43Get(m, loop)->next = 0;
                }
            } else {
                n->child = node2;
                if (node2) {
                    D43Node *b = d43Get(m, node2);
                    if (!b) { pcvErrf("bsp: node %#x missing", node2); return 0; }
                    b->prev = 0;
                }
            }
        }
        if (gdl) {
            if (!vecPush(seq, gdl)) return 0;
            prevNode = (int32_t)node;
            prevGdl = gdl;
            continue;
        }
        prevNode = -1;
        if (n->child) {
            node = n->child;
        } else {
            while (node) {
                D43Node *c = d43Get(m, node);
                if (!c) { pcvErrf("visit: node %#x missing", node); return 0; }
                if (c->next) { node = c->next; break; }
                node = c->parent;
            }
        }
    }
    return 1;
}

typedef struct { uint32_t off; uint8_t op; } D43Placed;

static int d43PlacementOrder(D43Map *m, uint32_t R0, D43Placed **outP, int *outN)
{
    uint32_t *stack;
    int sp = 0, scap = 1024;
    D43Placed *out = NULL;
    int n = 0, cap = 0;
    uint8_t *seen;

    seen = (uint8_t *)calloc((size_t)m->D, 1);
    stack = (uint32_t *)malloc((size_t)(sizeof(uint32_t) * (size_t)scap));
    if (!seen || !stack) { free(seen); free(stack); return 0; }
    stack[sp++] = R0;
    while (sp > 0) {
        uint32_t o = stack[--sp];
        D43Node *nd;
        uint32_t child, nxt;
        if ((size_t)o >= m->D || seen[o])
            continue;
        seen[o] = 1;
        nd = d43Get(m, o);
        if (!nd) { pcvErrf("place: node %#x missing", o); goto fail; }
        if (n >= cap) {
            int nc = cap ? cap * 2 : 256;
            D43Placed *np = (D43Placed *)realloc(out,
                    (size_t)(sizeof(D43Placed) * (size_t)nc));
            if (!np) goto fail;
            out = np; cap = nc;
        }
        out[n].off = o;
        out[n].op = nd->op;
        n++;
        child = nd->child;
        nxt = nd->next;
        if (sp + 3 > scap) {
            uint32_t *ns;
            scap *= 2;
            ns = (uint32_t *)realloc(stack,
                    (size_t)(sizeof(uint32_t) * (size_t)scap));
            if (!ns) goto fail;
            stack = ns;
        }
        if (nd->op == 8) {
            uint32_t a = be32o(m->src, nd->data + 8);
            if (a) stack[sp++] = a;
        } else if (nd->op == 18) {
            uint32_t c = be32o(m->src, nd->data + 0);
            if (c) stack[sp++] = c;
        } else if (child) {
            stack[sp++] = child;
        }
        if (nxt) stack[sp++] = nxt;
    }
    free(seen);
    free(stack);
    *outP = out;
    *outN = n;
    return 1;
fail:
    free(seen); free(stack); free(out);
    return 0;
}

/* ---------------------------------------------------------------- regions ---*/
typedef struct { uint32_t old, size, nw; } D43Region;
typedef struct { D43Region *r; int n, cap; } D43Regions;

static int regPush(D43Regions *R, uint32_t old, uint32_t size, uint32_t nw)
{
    if (R->n >= R->cap) {
        int nc = R->cap ? R->cap * 2 : 512;
        D43Region *nr = (D43Region *)realloc(R->r,
                (size_t)(sizeof(D43Region) * (size_t)nc));
        if (!nr) return 0;
        R->r = nr; R->cap = nc;
    }
    R->r[R->n].old = old;
    R->r[R->n].size = size;
    R->r[R->n].nw = nw;
    R->n++;
    return 1;
}

static int cmpRegion(const void *a, const void *b)
{
    const D43Region *x = (const D43Region *)a, *y = (const D43Region *)b;
    if (x->old != y->old) return x->old < y->old ? -1 : 1;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->nw != y->nw) return x->nw < y->nw ? -1 : 1;
    return 0;
}

/* Python's remap(): bisect_right on the sorted old-offset list, step back one,
 * then range-check. Returns -1 for "not in region map" (Python's None). */
static int64_t d43Remap(const D43Regions *R, uint32_t off)
{
    int lo = 0, hi = R->n, i;
    if (off == 0)
        return 0;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (R->r[mid].old <= off) lo = mid + 1; else hi = mid;
    }
    i = lo - 1;
    if (i < 0)
        return -1;
    if (R->r[i].old <= off && off < R->r[i].old + R->r[i].size)
        return (int64_t)(R->r[i].nw + (off - R->r[i].old));
    return -1;
}

static uint32_t d43GdlEnd(const uint8_t *src, size_t D, uint32_t g)
{
    size_t o = g;
    while (o + 8 <= D) {
        if ((pcvBE32(src, o) >> 24) == 0xB8)
            return (uint32_t)(o + 8);
        o += 8;
    }
    return (uint32_t)D;
}

/* ------------------------------------------------------------- per file ---*/
struct D43Result {
    uint8_t *comp;
    size_t   compLen;
};

static int d43Process(const char *name, const uint8_t *rom, size_t romLen,
                      struct D43Result *res)
{
    char key[PCV_NAME_LEN] = "";
    const struct PcvFileRow *row;
    const struct PcvNsNtRow *hdr;
    uint8_t *src = NULL;
    size_t D = 0;
    uint32_t NS, NT, R0, dstpos = 0, D_PC;
    D43Map map;
    U32Vec seq, refGdls, unvisited, allGdls, blobTargets, objOffs;
    D43Placed *placed = NULL;
    int nPlaced = 0;
    D43Regions R;
    int32_t *nodeNew = NULL, *recNew = NULL, *gdlNew = NULL;
    uint8_t *isColl = NULL;
    int32_t *pointUsage = NULL;   /* old off -> numVertices, -1 absent */
    U32Vec vtxOff, vtxNum, zeroVtx, puOffs;
    uint8_t *buf = NULL;
    /* Round 6 (carried from the Python, commit 84c571c8): the merged
     * embedded-image-blob regions, kept alive past the layout pass so the
     * emit pass can copy their raw bytes. Triples (old, size, new). */
    uint32_t *blobIv = NULL;
    int nBlobIv = 0;
    int i, k, ok = 0;

    memset(&map, 0, sizeof(map));
    memset(&R, 0, sizeof(R));
    vecInit(&seq); vecInit(&refGdls); vecInit(&unvisited); vecInit(&allGdls);
    vecInit(&blobTargets); vecInit(&objOffs);
    vecInit(&vtxOff); vecInit(&vtxNum); vecInit(&zeroVtx); vecInit(&puOffs);

    row = pcvFindRowVariants(name, key, sizeof(key));
    if (!row) {
        pcvErrf("%s: no filelist row", name);
        return 0;
    }
    if ((size_t)row->addr + row->size > romLen) {
        pcvErrf("%s: filelist row past EOF", name);
        return 0;
    }
    src = pcvInflateRZ(rom + row->addr, row->size, &D);
    if (!src) {
        pcvErrf("%s: decompress failed", name);
        return 0;
    }
    hdr = pcvFindNsNtVariants(key);
    if (!hdr) {
        pcvErrf("%s: no header data", name);
        free(src);
        return 0;
    }
    NS = hdr->ns;
    NT = hdr->nt;

    map.src = src;
    map.D = D;
    map.byOff = (int32_t *)malloc((size_t)(sizeof(int32_t) * D));
    if (!map.byOff) goto done;
    for (i = 0; i < (int)D; i++) map.byOff[i] = -1;

    R0 = 4 * NS + 12 * NT;
    if (!d43BuildNodes(&map, R0)) goto done;
    if (map.n == 0) {
        pcvErrf("%s: empty tree", name);
        goto done;
    }
    if (!d43VisitSeq(&map, R0, &seq)) goto done;
    if (!d43PlacementOrder(&map, R0, &placed, &nPlaced)) goto done;

    nodeNew = (int32_t *)malloc((size_t)(sizeof(int32_t) * D));
    recNew  = (int32_t *)malloc((size_t)(sizeof(int32_t) * D));
    gdlNew  = (int32_t *)malloc((size_t)(sizeof(int32_t) * D));
    pointUsage = (int32_t *)malloc((size_t)(sizeof(int32_t) * D));
    isColl  = (uint8_t *)calloc((size_t)D, 1);
    if (!nodeNew || !recNew || !gdlNew || !pointUsage || !isColl) goto done;
    for (i = 0; i < (int)D; i++) {
        nodeNew[i] = -1; recNew[i] = -1; gdlNew[i] = -1; pointUsage[i] = -1;
    }

    /* ---------------------------------------------------- layout pass ---*/
    for (i = 0; i < (int)NS; i++) {
        if (!regPush(&R, 4u * (uint32_t)i, 8, dstpos)) goto done;
        dstpos += 8;
    }
    for (i = 0; i < (int)NT; i++) {
        if (!regPush(&R, 4 * NS + 12u * (uint32_t)i, 12, dstpos)) goto done;
        dstpos += 12;
    }

    for (i = 0; i < nPlaced; i++) {
        uint32_t no = placed[i].off;
        int op = placed[i].op;
        uint32_t data = be32o(src, no + 4);
        int nsz, psz;

        if (!regPush(&R, no, 48, dstpos)) goto done;
        nodeNew[no] = (int32_t)dstpos;
        dstpos += 48;

        nsz = n64RecSize(op);
        psz = pcRecSize(op);
        if (nsz < 0) {
            pcvErrf("%s: unknown opcode %d at %#x", name, op, no);
            continue;
        }
        (void)nsz;
        recNew[data] = (int32_t)dstpos;
        if (psz < 0) {
            pcvErrf("%s: opcode %d has no PC record size", name, op);
            continue;
        }
        if (!regPush(&R, data, (uint32_t)psz, dstpos)) goto done;
        dstpos += (uint32_t)psz;

        if (op == 4) {
            uint32_t nv = pcvBE16(src, data + 0x10);
            uint32_t vo = be32o(src, data + 0xC);
            if (vo) {
                if (!nv) {
                    if (!vecPush(&zeroVtx, vo)) goto done;
                } else {
                    if (!vecPush(&vtxOff, vo) || !vecPush(&vtxNum, nv)) goto done;
                    if (!regPush(&R, vo, 16 * nv, dstpos)) goto done;
                    dstpos += 16 * nv;
                }
            }
        } else if (op == 24) {
            int32_t nv  = pcvBES16(src, data + 0xC);
            int32_t ncv = pcvBES16(src, data + 0xE);
            uint32_t vo  = be32o(src, data + 8);
            uint32_t cvo = be32o(src, data + 0x10);
            uint32_t puo = be32o(src, data + 0x14);
            if (nv && vo) {
                if (!vecPush(&vtxOff, vo) || !vecPush(&vtxNum, (uint32_t)nv)) goto done;
                if (!regPush(&R, vo, 16u * (uint32_t)nv, dstpos)) goto done;
                dstpos += 16u * (uint32_t)nv;
            }
            if (ncv && cvo) {
                isColl[cvo] = 1;
                if (!vecPush(&vtxOff, cvo) || !vecPush(&vtxNum, (uint32_t)ncv)) goto done;
                if (!regPush(&R, cvo, 16u * (uint32_t)ncv, dstpos)) goto done;
                dstpos += 16u * (uint32_t)ncv;
            }
            if (nv && puo) {
                if (!regPush(&R, puo, 2u * (uint32_t)nv, dstpos)) goto done;
                dstpos += 2u * (uint32_t)nv;
                if (pointUsage[puo] < 0 && !vecPush(&puOffs, puo)) goto done;
                pointUsage[puo] = nv;
            }
        } else if (op == 22) {
            /* Upstream d061ca3 (D303): the op-22 count is the number of
             * star-flash ARMS, 4 vertices each -- dorottex() (model.c) reads
             * numVertices * 4 and steps src by 4 per arm, as does PD's
             * modelRenderNodeStarGunfire. Sized as `nv` vertices, 3/4 of
             * every muzzle star came from the neighbouring sidecar bytes
             * (long spikes). */
            int32_t nv = pcvBES32(src, data);
            uint32_t vo = be32o(src, data + 4);
            if (nv && vo) {
                if (!vecPush(&vtxOff, vo) || !vecPush(&vtxNum, 4u * (uint32_t)nv)) goto done;
                if (!regPush(&R, vo, 16u * 4u * (uint32_t)nv, dstpos)) goto done;
                dstpos += 16u * 4u * (uint32_t)nv;
            }
        }
    }

    /* record-referenced GDLs, in placement order, deduped */
    for (i = 0; i < nPlaced; i++) {
        uint32_t data = be32o(src, placed[i].off + 4);
        int op = placed[i].op;
        if (op == 4 || op == 24) {
            uint32_t q = be32o(src, data + 0);
            if (q && !vecHas(&refGdls, q) && !vecPush(&refGdls, q)) goto done;
            q = be32o(src, data + 4);
            if (q && !vecHas(&refGdls, q) && !vecPush(&refGdls, q)) goto done;
        } else if (op == 22) {
            uint32_t q = be32o(src, data + 8);
            if (q && !vecHas(&refGdls, q) && !vecPush(&refGdls, q)) goto done;
        }
    }
    for (i = 0; i < refGdls.n; i++) {
        uint32_t g = refGdls.v[i];
        int j, seen = 0;
        for (j = 0; j < seq.n; j++)
            if (seq.v[j] == g) { seen = 1; break; }
        if (!seen && !vecPush(&unvisited, g)) goto done;
    }
    qsort(unvisited.v, (size_t)unvisited.n, sizeof(uint32_t), cmpU32d);
    for (i = 0; i < unvisited.n; i++)
        if (!vecPush(&allGdls, unvisited.v[i])) goto done;
    for (i = 0; i < seq.n; i++)
        if (!vecPush(&allGdls, seq.v[i])) goto done;

    /* zero-vtx arrays sized from the next placed object */
    if (zeroVtx.n) {
        for (i = 0; i < nPlaced; i++) {
            if (!vecPush(&objOffs, placed[i].off)) goto done;
            if (recNew[be32o(src, placed[i].off + 4)] >= 0 &&
                !vecPush(&objOffs, be32o(src, placed[i].off + 4))) goto done;
        }
        for (i = 0; i < vtxOff.n; i++)
            if (!vecPush(&objOffs, vtxOff.v[i])) goto done;
        for (i = 0; i < allGdls.n; i++)
            if (!vecPush(&objOffs, allGdls.v[i])) goto done;
        qsort(objOffs.v, (size_t)objOffs.n, sizeof(uint32_t), cmpU32d);
        for (i = 0; i < zeroVtx.n; i++) {
            uint32_t vo = zeroVtx.v[i], nxt = (uint32_t)D, sz;
            int j;
            for (j = 0; j < objOffs.n; j++)
                if (objOffs.v[j] > vo) { nxt = objOffs.v[j]; break; }
            sz = nxt - vo;
            if (nxt <= vo || (sz % 16)) {
                pcvErrf("%s: zero-vtx array %#x bad size %#x", name, vo, sz);
                continue;
            }
            if (!vecPush(&vtxOff, vo) || !vecPush(&vtxNum, sz / 16)) goto done;
            if (!regPush(&R, vo, sz, dstpos)) goto done;
            dstpos += sz;
        }
    }

    /* embedded image blobs referenced by G_SETTIMG (seg 5) */
    for (i = 0; i < allGdls.n; i++) {
        size_t o = allGdls.v[i];
        while (o + 8 <= D) {
            uint32_t w0 = pcvBE32(src, o);
            uint32_t c = w0 >> 24;
            if (c == 0xB8)
                break;
            if (c == 0xFD && (pcvBE32(src, o + 4) >> 24) == 5) {
                uint32_t t = pcvBE32(src, o + 4) & PCV_MASK;
                if (!vecPush(&blobTargets, t)) goto done;
            }
            o += 8;
        }
    }
    if (blobTargets.n) {
        uint32_t *iv;
        int nIv = 0, nMerged = 0, w;
        qsort(blobTargets.v, (size_t)blobTargets.n, sizeof(uint32_t), cmpU32d);
        w = 0;
        for (i = 0; i < blobTargets.n; i++)
            if (w == 0 || blobTargets.v[w - 1] != blobTargets.v[i])
                blobTargets.v[w++] = blobTargets.v[i];
        blobTargets.n = w;
        iv = (uint32_t *)malloc((size_t)(sizeof(uint32_t) * 2u *
                                (size_t)blobTargets.n));
        if (!iv) goto done;
        for (i = 0; i < blobTargets.n; i++) {
            uint32_t t = blobTargets.v[i], end = (uint32_t)D;
            int j;
            for (j = 0; j < R.n; j++)
                if (R.r[j].old > t && R.r[j].old < end) end = R.r[j].old;
            iv[2 * nIv] = t;
            iv[2 * nIv + 1] = end;
            nIv++;
        }
        for (i = 0; i < nIv; i++) {
            if (nMerged && iv[2 * i] <= iv[2 * (nMerged - 1) + 1]) {
                if (iv[2 * i + 1] > iv[2 * (nMerged - 1) + 1])
                    iv[2 * (nMerged - 1) + 1] = iv[2 * i + 1];
            } else {
                iv[2 * nMerged] = iv[2 * i];
                iv[2 * nMerged + 1] = iv[2 * i + 1];
                nMerged++;
            }
        }
        blobIv = (uint32_t *)malloc(sizeof(uint32_t) * 3u * (size_t)nMerged);
        if (!blobIv) { free(iv); goto done; }
        for (i = 0; i < nMerged; i++) {
            uint32_t t = iv[2 * i], e = iv[2 * i + 1];
            blobIv[3 * i] = t;
            blobIv[3 * i + 1] = e - t;
            blobIv[3 * i + 2] = dstpos;
            if (!regPush(&R, t, e - t, dstpos)) { free(iv); goto done; }
            dstpos += e - t;
        }
        nBlobIv = nMerged;
        free(iv);
    }

    /* GDLs last, tight-packed 16B slots (unvisited first, then visit order) */
    for (i = 0; i < allGdls.n; i++) {
        uint32_t g = allGdls.v[i];
        uint32_t end = d43GdlEnd(src, D, g);
        uint32_t nslots = (end - g) / 8;
        if ((end - g) % 8)
            pcvErrf("%s: GDL %#x not 8B aligned end", name, g);
        gdlNew[g] = (int32_t)dstpos;
        if (!regPush(&R, g, 16 * nslots, dstpos)) goto done;
        dstpos += 16 * nslots;
    }

    D_PC = dstpos;

    qsort(R.r, (size_t)R.n, sizeof(D43Region), cmpRegion);

    /* ------------------------------------------------------ emit pass ---*/
    buf = (uint8_t *)calloc((size_t)(D_PC ? D_PC : 1), 1);
    if (!buf) goto done;

#define PUT_PTR(dstOff, oldOff)                                              \
    do {                                                                     \
        uint32_t _o = (oldOff);                                              \
        if (_o) {                                                            \
            int64_t _n = d43Remap(&R, _o);                                   \
            if (_n < 0) {                                                    \
                pcvErrf("%s: ptr %#x not in region map", name, _o);          \
                _n = 0;                                                      \
            }                                                                \
            pcvPut32(buf, (dstOff), 0x05000000u | ((uint32_t)_n & PCV_MASK));\
            pcvPut32(buf, (dstOff) + 4, 0);                                  \
        }                                                                    \
    } while (0)
#define PUT_F32(dstOff, srcOff)                                              \
    do {                                                                     \
        size_t _d = (dstOff), _s = (srcOff);                                 \
        buf[_d] = src[_s + 3]; buf[_d + 1] = src[_s + 2];                    \
        buf[_d + 2] = src[_s + 1]; buf[_d + 3] = src[_s];                    \
    } while (0)

    /* Round 6 (the black MENU_GOLDENEYE_LOGO title screen; Python fix
     * 84c571c8 carried into the C port): the embedded image blobs were
     * reserved in the LAYOUT above -- so d43Remap()/the G_SETTIMG w1 rewrite
     * point at the right PC offset -- but their bytes were never copied into
     * `buf`, which calloc() zero-fills. Every P* file carrying its own pixels
     * (PgoldeneyelogoZ, PlegalpageZ, ...) therefore shipped a byte-perfect
     * display list pointing at a run of zero bytes, and fast3d rasterised
     * pure black. The RDP reads these bytes big-endian as-is, so this is a
     * raw copy with no byteswap. */
    for (i = 0; i < nBlobIv; i++)
        memcpy(buf + blobIv[3 * i + 2], src + blobIv[3 * i], blobIv[3 * i + 1]);

    for (i = 0; i < (int)NS; i++)
        PUT_PTR(8u * (uint32_t)i, be32o(src, 4u * (uint32_t)i));
    for (i = 0; i < (int)NT; i++) {
        uint32_t doff = 4 * NS + 12u * (uint32_t)i;
        uint32_t npos = 8 * NS + 12u * (uint32_t)i;
        uint32_t tid = pcvBE32(src, doff);
        pcvPut32(buf, npos, tid);
        memcpy(buf + npos + 4, src + doff + 4, 8);
        if ((tid >> 24) == 5 && d43Remap(&R, tid & PCV_MASK) < 0)
            pcvErrf("%s: texconfig seg-5 target %#x not in a blob region",
                    name, tid & PCV_MASK);
    }

    for (i = 0; i < nPlaced; i++) {
        uint32_t no = placed[i].off;
        int op = placed[i].op;
        uint32_t npos, d, r;

        if (nodeNew[no] < 0)
            continue;
        npos = (uint32_t)nodeNew[no];
        pcvPut16(buf, npos, pcvBE16(src, no));
        PUT_PTR(npos + 8,  be32o(src, no + 4));
        PUT_PTR(npos + 16, be32o(src, no + 8));
        PUT_PTR(npos + 24, be32o(src, no + 0xC));
        PUT_PTR(npos + 32, be32o(src, no + 0x10));
        PUT_PTR(npos + 40, be32o(src, no + 0x14));

        d = be32o(src, no + 4);
        if (recNew[d] < 0)
            continue;
        r = (uint32_t)recNew[d];
        switch (op) {
        case 1:
            pcvPut16(buf, r, pcvBE16(src, d));
            pcvPut16(buf, r + 2, (uint16_t)pcvBES16(src, d + 2));
            PUT_PTR(r + 8, be32o(src, d + 4));
            pcvPut16(buf, r + 16, pcvBE16(src, d + 8));
            pcvPut16(buf, r + 18, pcvBE16(src, d + 0xA));
            pcvPut16(buf, r + 20, pcvBE16(src, d + 0xC));
            break;
        case 2: case 3:
            for (k = 0; k < 3; k++) PUT_F32(r + 4 * (uint32_t)k, d + 4 * (uint32_t)k);
            pcvPut16(buf, r + 12, pcvBE16(src, d + 0xC));
            for (k = 0; k < 3; k++)
                pcvPut16(buf, r + 14 + 2 * (uint32_t)k,
                         (uint16_t)pcvBES16(src, d + 0xE + 2 * (uint32_t)k));
            PUT_PTR(r + 24, be32o(src, d + 0x14));
            PUT_F32(r + 32, d + 0x18);
            break;
        case 4: {
            uint32_t p = be32o(src, d), s = be32o(src, d + 4);
            if (p) pcvPut32(buf, r, 0x05000000u | ((uint32_t)gdlNew[p] & PCV_MASK));
            if (s) pcvPut32(buf, r + 8, 0x05000000u | ((uint32_t)gdlNew[s] & PCV_MASK));
            PUT_PTR(r + 24, be32o(src, d + 0xC));
            pcvPut16(buf, r + 32, pcvBE16(src, d + 0x10));
            buf[r + 34] = src[d + 0x12];
            break;
        }
        case 8:
            PUT_F32(r, d); PUT_F32(r + 4, d + 4);
            PUT_PTR(r + 8, be32o(src, d + 8));
            pcvPut16(buf, r + 16, pcvBE16(src, d + 0xC));
            break;
        case 9:
            for (k = 0; k < 3; k++) PUT_F32(r + 4 * (uint32_t)k, d + 4 * (uint32_t)k);
            for (k = 0; k < 3; k++) PUT_F32(r + 12 + 4 * (uint32_t)k, d + 0xC + 4 * (uint32_t)k);
            PUT_PTR(r + 24, be32o(src, d + 0x18));
            PUT_PTR(r + 32, be32o(src, d + 0x1C));
            pcvPut16(buf, r + 42, pcvBE16(src, d + 0x22));
            break;
        case 10:
            pcvPut32(buf, r, pcvBE32(src, d));
            for (k = 0; k < 6; k++) PUT_F32(r + 4 + 4 * (uint32_t)k, d + 4 + 4 * (uint32_t)k);
            break;
        case 12:
            for (k = 0; k < 3; k++) PUT_F32(r + 4 * (uint32_t)k, d + 4 * (uint32_t)k);
            for (k = 0; k < 3; k++) PUT_F32(r + 12 + 4 * (uint32_t)k, d + 0xC + 4 * (uint32_t)k);
            PUT_PTR(r + 24, be32o(src, d + 0x18));
            PUT_F32(r + 32, d + 0x1C);
            pcvPut16(buf, r + 36, pcvBE16(src, d + 0x20));
            break;
        case 13:
            for (k = 0; k < 2; k++) PUT_F32(r + 4 * (uint32_t)k, d + 4 * (uint32_t)k);
            for (k = 0; k < 2; k++) PUT_F32(r + 8 + 4 * (uint32_t)k, d + 8 + 4 * (uint32_t)k);
            PUT_PTR(r + 16, be32o(src, d + 0x10));
            PUT_PTR(r + 24, be32o(src, d + 0x14));
            PUT_F32(r + 32, d + 0x18);
            break;
        case 15:
            for (k = 0; k < 3; k++) PUT_F32(r + 4 * (uint32_t)k, d + 4 * (uint32_t)k);
            for (k = 0; k < 3; k++) PUT_F32(r + 12 + 4 * (uint32_t)k, d + 0xC + 4 * (uint32_t)k);
            PUT_F32(r + 24, d + 0x18);
            break;
        case 18:
            PUT_PTR(r, be32o(src, d));
            pcvPut16(buf, r + 8, pcvBE16(src, d + 4));
            break;
        case 21:
            for (k = 0; k < 3; k++) PUT_F32(r + 4 * (uint32_t)k, d + 4 * (uint32_t)k);
            pcvPut16(buf, r + 12, (uint16_t)pcvBES16(src, d + 0xC));
            pcvPut16(buf, r + 14, pcvBE16(src, d + 0xE));
            PUT_F32(r + 16, d + 0x10);
            break;
        case 22: {
            uint32_t p;
            pcvPut32(buf, r, pcvBE32(src, d));
            PUT_PTR(r + 8, be32o(src, d + 4));
            p = be32o(src, d + 8);
            if (p) pcvPut32(buf, r + 16, 0x05000000u | ((uint32_t)gdlNew[p] & PCV_MASK));
            break;
        }
        case 23:
            pcvPut16(buf, r, pcvBE16(src, d));
            break;
        case 24: {
            uint32_t p = be32o(src, d), s = be32o(src, d + 4);
            if (p) pcvPut32(buf, r, 0x05000000u | ((uint32_t)gdlNew[p] & PCV_MASK));
            if (s) pcvPut32(buf, r + 8, 0x05000000u | ((uint32_t)gdlNew[s] & PCV_MASK));
            PUT_PTR(r + 16, be32o(src, d + 8));
            pcvPut16(buf, r + 24, (uint16_t)pcvBES16(src, d + 0xC));
            pcvPut16(buf, r + 26, (uint16_t)pcvBES16(src, d + 0xE));
            PUT_PTR(r + 32, be32o(src, d + 0x10));
            PUT_PTR(r + 40, be32o(src, d + 0x14));
            pcvPut16(buf, r + 48, (uint16_t)pcvBES16(src, d + 0x18));
            pcvPut16(buf, r + 50, pcvBE16(src, d + 0x1A));
            break;
        }
        default:
            break;
        }
    }

    /* vertex arrays */
    for (i = 0; i < vtxOff.n; i++) {
        uint32_t vo = vtxOff.v[i], nv = vtxNum.v[i];
        int64_t np = d43Remap(&R, vo);
        uint32_t npos;
        if (np < 0) {
            pcvErrf("%s: vtx %#x not in region map", name, vo);
            continue;
        }
        npos = (uint32_t)np;
        if (isColl[vo]) {
            for (k = 0; k < (int)nv; k++) {
                uint32_t o = vo + 16u * (uint32_t)k, q = npos + 16u * (uint32_t)k;
                uint32_t v;
                pcvPut16(buf, q,     (uint16_t)pcvBES16(src, o));
                pcvPut16(buf, q + 2, (uint16_t)pcvBES16(src, o + 2));
                pcvPut16(buf, q + 4, (uint16_t)pcvBES16(src, o + 4));
                pcvPut16(buf, q + 6, (uint16_t)pcvBES16(src, o + 6));
                v = pcvBE32(src, o + 8);
                if (v == 0) {
                    /* nothing */
                } else if ((v >> 24) == 5) {
                    int64_t n = d43Remap(&R, v & PCV_MASK);
                    if (n < 0) {
                        pcvErrf("%s: LinkedTo %#x not in map", name, v);
                        n = 0;
                    }
                    pcvPut32(buf, q + 8, 0x05000000u | ((uint32_t)n & PCV_MASK));
                } else {
                    pcvErrf("%s: LinkedTo %#x neither null nor seg-5", name, v);
                }
                pcvPut16(buf, q + 12, (uint16_t)pcvBES16(src, o + 0xC));
                pcvPut16(buf, q + 14, (uint16_t)pcvBES16(src, o + 0xE));
            }
        } else {
            for (k = 0; k < (int)nv; k++) {
                uint32_t o = vo + 16u * (uint32_t)k, q = npos + 16u * (uint32_t)k;
                int j;
                for (j = 0; j < 6; j++)
                    pcvPut16(buf, q + 2u * (uint32_t)j,
                             (uint16_t)pcvBES16(src, o + 2u * (uint32_t)j));
                memcpy(buf + q + 12, src + o + 0xC, 4);
            }
        }
    }

    /* D120: PointUsage[] s16 index chains */
    for (i = 0; i < puOffs.n; i++) {
        uint32_t puo = puOffs.v[i];
        int32_t punv = pointUsage[puo];
        int64_t pn = d43Remap(&R, puo);
        if (pn < 0) {
            pcvErrf("%s: PointUsage %#x not in region map", name, puo);
            continue;
        }
        for (k = 0; k < punv; k++)
            pcvPut16(buf, (uint32_t)pn + 2u * (uint32_t)k,
                     (uint16_t)pcvBES16(src, puo + 2u * (uint32_t)k));
    }

    /* GDL slots */
    for (i = 0; i < allGdls.n; i++) {
        uint32_t g = allGdls.v[i];
        uint32_t end = d43GdlEnd(src, D, g);
        uint32_t npos = (uint32_t)gdlNew[g];
        uint32_t j = 0, o = g;
        while (o + 8 <= end) {
            uint32_t w0 = pcvBE32(src, o);
            uint32_t w1 = pcvBE32(src, o + 4);
            uint32_t q = npos + 16 * j;
            uint32_t c = w0 >> 24;
            if ((c == 0x04 || c == 0xFD) && (w1 >> 24) == 5) {
                int64_t n = d43Remap(&R, w1 & PCV_MASK);
                if (n < 0) {
                    pcvErrf("%s: GDL %#x op%#x w1 %#x not in map", name, g, c, w1);
                    n = 0;
                }
                w1 = 0x05000000u | ((uint32_t)n & PCV_MASK);
            }
            pcvPut32(buf, q, w0);
            pcvPut32(buf, q + 4, 0);
            pcvPut32(buf, q + 8, w1);
            pcvPut32(buf, q + 12, 0);
            if (c == 0xB8)
                break;
            j++;
            o += 8;
        }
    }

#undef PUT_PTR
#undef PUT_F32

    res->comp = pcvDeflateRZ(buf, D_PC, &res->compLen);
    ok = res->comp != NULL;

done:
    free(buf);
    free(blobIv);
    free(map.nodes);
    free(map.byOff);
    free(nodeNew); free(recNew); free(gdlNew); free(pointUsage); free(isColl);
    free(placed);
    free(R.r);
    vecFree(&seq); vecFree(&refGdls); vecFree(&unvisited); vecFree(&allGdls);
    vecFree(&blobTargets); vecFree(&objOffs);
    vecFree(&vtxOff); vecFree(&vtxNum); vecFree(&zeroVtx); vecFree(&puOffs);
    free(src);
    return ok;
}

/* ------------------------------------------------------------------ main ---*/
static int isModelName(const char *n)
{
    size_t l;
    if (!n || !n[0])
        return 0;
    if (n[0] != 'C' && n[0] != 'G' && n[0] != 'P')
        return 0;
    l = strlen(n);
    if (n[l - 1] != 'Z')
        return 0;
    return strstr(n, "_stan") == NULL;
}

int pcvD43(const uint8_t *rom, size_t romLen, PcvImage *im)
{
    int t, nDone = 0, nModels = 0;

    for (t = 0; t < pcconvTableNameCount; t++) {
        const char *name = pcconvTableNames[t];
        struct D43Result res;
        if (!isModelName(name))
            continue;
        nModels++;
        res.comp = NULL;
        res.compLen = 0;
        if (!d43Process(name, rom, romLen, &res))
            continue;
        pcvImageEmit(im, name, res.comp, res.compLen);
        free(res.comp);
        nDone++;
    }

    /* d43_emit.py pads the tail of pcmodels.bin to a 16-byte multiple
     * (d69/d88 do not pad pccg.bin) — keep the difference. */
    if (im->bin.len % 16)
        pcvBufResize(&im->bin, (im->bin.len + 15u) & ~(size_t)15u);

    pcvLog("pcconvert: d43 converted %d / %d model files", nDone, nModels);
    return pcvErrCount() ? 0 : 1;
}
