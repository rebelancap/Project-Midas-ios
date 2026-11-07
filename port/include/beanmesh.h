/* Imported from GoldenEye-Decomp-ios engine/port/include/beanmesh.h (first 93988236, last changed 47ef757f; donor HEAD 55a3a9c8),
 * round 13, XBLA step 4.1 (docs/port-ios/22-round13-rooms.md). 64-bit host layer only.
 * Changes against the donor are marked "Midas round 13". */
#ifndef PORT_BEANMESH_H
#define PORT_BEANMESH_H

/*
 * Tier C (round C1, DECISIONS D-110): GoldenEye XBLA's ("Project Bean")
 * rendergraphs read as triangles. docs/tier-c.md has the design.
 *
 * A Bean model or level file is a CAFF bundle (beantex.c) whose asset 1 is a
 * rendergraph: ".data" holds buffer descriptors {stride, _, offset, size} and
 * index-buffer rows {obj, offset, size, kind}, ".gpu" the vertex and index
 * buffers (and shaders and texels), ".stream" a command stream of tagged
 * records (size << 16 | type << 8). Walking the stream's first alternative at
 * every switch yields the draws: vertex buffer (0x2e), material (0x2d, which
 * names the picture), the draw itself (0x01 / 0x30), the pass (0x1a / 0x15),
 * the render states (0x0c: alpha test 0x60, its reference 0x64), the material
 * colour (0x06) and instancing matrices (0x21 ... 0x25).
 *
 * Ported from Perfect Dark's gebean.c (perfect-dark-ios vendor/dabs-mod,
 * MIT): beanLoad(), beanWalkStream(), beanMaterialTexture(), beanReadVb(),
 * beanVertex(), beanFindIndexBuffers(), beanTriangles32(),
 * gebeanLevelOpenSource() (the UV-scale sniff), beanTreeMeasure()/
 * beanTreeUvs() and gebeanLevelTriangles(). Nothing of the release is in this
 * repository: the file is the user's own, read at runtime.
 *
 * Plain host C: every allocation here is malloc (charter rule 3c, Tier C
 * never touches memp/mema).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct beanlevel;

/* One corner of a level triangle, in the file's own units. */
struct beanlevelvtx {
    float pos[3];
    float uv[2];      /* in repeats of the picture (the level's UV scale applied) */
    uint32_t argb;    /* the lit colour, ARGB */
    uint8_t blend;    /* its draw is in the release's blended pass */
    uint8_t plain;    /* its draw has no UV and no picture of its own */
    uint8_t alphatest;
    uint8_t alpharef;
    uint8_t water;    /* a stride-36 draw: the release's water (round C2) */
    uint8_t kind;     /* its draw's BEANDRAW_* (round 25b) */
    uint8_t ownmat;   /* its draw set a material since its vertex shader */
};

/*
 * Round 25b (D-142): what a draw's material comes to - the coverage audit.
 * PICTURE: a picture the file holds, read with the layout's UV. COLOUR: no
 * picture by design (the layout has no UV): its vertex or material colour.
 * SPHEREMAP: a picture of its own but no UV - the release sphere-maps it
 * (PD beanDrawIsSphereMapped); not drawn by this reader. BADPIC: names a
 * picture index the file does not hold. A triangle of a BADPIC draw is
 * handed over with tex -1; a model's SPHEREMAP triangle (round 26a, D-146)
 * with its own picture, the reflection map, for the caller to look up by the
 * normal (beanmodel.c draws it with GoldenEye's own texgen).
 */
#define BEANDRAW_PICTURE   0
#define BEANDRAW_COLOUR    1
#define BEANDRAW_SPHEREMAP 2
#define BEANDRAW_BADPIC    3
#define BEANDRAW_KINDS     4

struct beandrawcount {
    int draws;        /* every draw the stream walk found */
    int kinds[BEANDRAW_KINDS];
    int unreadable;   /* a vertex buffer this reader cannot read: its triangles are missing */
    int skipped;      /* left out by design (a model's pieces past 1, position-only buffers) */
};

/* Every draw classified as beanLevelTriangles() (model 0) or
 * beanModelTriangles() (model 1) would, without reading a triangle. */
void beanMeshCountDraws(const struct beanlevel *level, int model, struct beandrawcount *out);

struct beanlevelinfo {
    int numdraws;
    int numtextures;
    int numinsts;     /* instancing matrices (0x21) */
    int numibs;       /* index buffers, 16- and 32-bit */
    int numibs32;
    float uvscale;    /* 128, 256, 512 or 1024: a UV of `uvscale` is one repeat */
    int lit128, lit256; /* the shader literals the scale was sniffed from */
    uint32_t filebytes;
};

/*
 * Opens files/new/background/<name>/default.bin (any path). `name` is the
 * level's Bean name ("dam"), for the one level whose UV scale has no shader
 * literal (silo, 1/512). The whole file is read into one malloc'd buffer and
 * kept until beanLevelClose(). NULL if it is missing or not a rendergraph.
 */
struct beanlevel *beanLevelOpen(const char *path, const char *name);
void beanLevelClose(struct beanlevel *level);
void beanLevelInfo(const struct beanlevel *level, struct beanlevelinfo *out);
/* The whole file as read (for the built-level cache's key). */
const uint8_t *beanLevelFileBytes(const struct beanlevel *level, uint32_t *len);
/* Midas round 61 (doc 81): override the sniffed UV scale (a sky file is 1/16384). */
void beanLevelSetUvScale(struct beanlevel *level, float uvscale);

/*
 * Every triangle of the level with the picture its material draws (-1 for
 * none), instancing expanded (a draw under a 0x21 record is placed by its
 * matrix; tree buffers get made UVs). Corners that are not sane positions
 * drop the triangle. Returns how many were handed over.
 */
int beanLevelTriangles(struct beanlevel *level,
                       void (*fn)(void *arg, int tex, const struct beanlevelvtx *v), void *arg);

/* Picture `tex` decoded to malloc'd RGBA8, rows as stored (the row order the
 * release's UVs address: v = 0 is the first row). NULL on failure. */
uint8_t *beanLevelDecodeTexture(const struct beanlevel *level, int tex, int32_t *w, int32_t *h);
/* Its size without decoding; 0 if unreadable. */
int beanLevelTextureSize(const struct beanlevel *level, int tex, int32_t *w, int32_t *h);

/* How the file slides picture `tex`, in repeats a second along u and v (its
 * texture-animation table, PD beanTexAnimation()); 0 when it does not
 * (round C2: Dam's reservoir). */
int beanLevelTextureScroll(const struct beanlevel *level, int tex, float *su, float *sv);

/* Round 29c (D-228, PD gebean.c glasspane): picture `tex` is a glass pane - a
 * material in the release's blended pass that lays one of the shared glass
 * maps over it. A pane whose picture went to DXT1 lost its alpha. */
int beanLevelTextureIsGlassPane(const struct beanlevel *level, int tex);

/*
 * Models (round C3, D-117): a prop's rendergraph (files/new/prop/<name>),
 * read rigid. beanModelOpen() takes the model's UV scale from its shaders'
 * literal, else PD's measure (beanShaderUvScale / beanMeasureUvScale). The
 * handle is a struct beanlevel: beanLevelClose(), beanLevelDecodeTexture()
 * and beanLevelTextureSize() apply.
 */
struct beanmodelvtx {
    float pos[3];
    float uv[2];       /* repeats of the picture */
    uint32_t argb;     /* ARGB as stored (the opaque-black rule is the caller's) */
    uint8_t blend;     /* in the release's blended pass */
    uint8_t matalpha;  /* the material colour's alpha (glass: 0.5..0.6), 255 for none */
    uint8_t alphatest;
    uint8_t alpharef;
    uint8_t plain;     /* the layout has no UV: drawn by its colour (tex -1) */
    uint16_t draw;     /* the draw it is from */
    int16_t bone;      /* the bone a skinned vertex rides (its palette's entry), -1 none */
    uint8_t kind;      /* its draw's BEANDRAW_* (round 25b) */
    uint8_t ownmat;    /* its draw set a material since its vertex shader */
    int8_t piece;      /* round 26a: a 0x30 draw's piece, -1 for a 0x01 draw (a gun's
                          piece-0 draws are its muzzle-flash cards) */
    float nrm[3];      /* round 26a: the vertex normal, file axes (sphere maps) */
};

struct beanlevel *beanModelOpen(const char *path);
/* Round 35a (D-317): beanModelOpen() first asks beanprefetch.c, which may
 * hand over a handle opened (and its pictures decoded) ahead on its worker;
 * the Raw form always reads the file (the worker's own, the audit's). */
struct beanlevel *beanModelOpenRaw(const char *path);
/* The pictures decoded ahead for this handle (numtex long, NULL = decode as
 * usual); beanLevelDecodeTexture() then returns those pointers, which stay
 * pinned in texpack's registry (a caller registers them as usual and frees
 * them only through texpackBeanPictureRelease()). */
void beanLevelAttachPictures(struct beanlevel *bm, uint8_t *const *pics, const int32_t *w, const int32_t *h);
/* used[t] = 1 for every picture a draw of the file names (numtex long). */
void beanLevelUsedTextures(const struct beanlevel *bm, uint8_t *used);
int beanModelTriangles(struct beanlevel *model,
                       void (*fn)(void *arg, int tex, const struct beanmodelvtx *v), void *arg);
void beanModelInfo(const struct beanlevel *model, int *numdraws, int *numtex, float *uvscale);


/*
 * Characters (round 26b, Tier C C4 part 2, DECISIONS D-150): the file's pose
 * skeleton (asset 2's SKEL_* names in pose order, the 'pose' record's
 * absolute binds) and its triangles with each corner's skin: up to four pose
 * bones (palette slot -> 0x13 palette -> 0x12 remap) and byte weights (stride
 * 36; strides 28/32 carry one bone at 255).
 */
#define BEAN_MAXBONES 32
struct beanskel {
    int numbones;
    float bind[BEAN_MAXBONES][3];
    int16_t parent[BEAN_MAXBONES];
    char name[BEAN_MAXBONES][24];
};
int beanModelSkeleton(const struct beanlevel *model, struct beanskel *out);
/* Round 28b: a palette bone as beanModelTriangles() reports it (v.bone, the
 * draw's 0x13 palette entry) to its pose bone through the 0x12 remap - the
 * index beanModelSkeleton()'s names are by (as beanModelSkinTriangles()). */
int beanModelPoseBone(const struct beanlevel *model, int bone);

struct beanskinvtx {
    float pos[3];
    float uv[2];
    uint32_t argb;
    int16_t bone[4];    /* pose bones, -1 none */
    uint8_t weight[4];  /* 0..255 */
    uint8_t blend, matalpha, plain, kind, ownmat;
    uint16_t draw;
    uint32_t vindex;    /* the vertex's index in its draw's buffer */
    float nrm[3];       /* round 32c (D-300): the 10:10:10 vertex normal, file axes (lit characters) */
};
int beanModelSkinTriangles(struct beanlevel *model,
                           void (*fn)(void *arg, int tex, const struct beanskinvtx *v), void *arg);
/* hist[stride]++ for every draw's vertex buffer (stride < max). */
int beanModelStrides(const struct beanlevel *model, int *hist, int max);

#ifdef __cplusplus
}
#endif

#endif
