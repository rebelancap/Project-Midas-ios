#include <ultra64.h>
#ifndef TARGET_N64
#include <stdio.h>
#endif
#ifdef PORT
#include "guestmem.h"
#endif
#include "chrobjdata.h"
#include "image.h"
#include "math_asinfacosf.h"
#include "math_ceil.h"
#include "math_floor.h"
#include "math_unk_05A9E0.h"
#include "model.h"
#include "ob.h"
#include "objecthandler.h"
#include "quaternion.h"
#include "tex.h"

#ifndef TARGET_N64
#include <stdlib.h>

/**
 * PC arm of the model texture pass: convert ONE node display list.
 *
 * On N64 the loop below rewrites every node DL in the file's slack space,
 * expanding GE's G_SETTEX (0xC0) into real SETTIMG/SETTILE/LOADBLOCK sequences
 * via texLoadFromGdl. The renderer treats a raw G_SETTEX as a NOOP, so an
 * unconverted model draws with no texture or material state (all black).
 * Compiled-in models have no slack space, so each converted DL gets its own
 * allocation instead.
 *
 * DL length = scan to gSPEndDisplayList inclusive (node DLs are flat; embedded
 * gSPDisplayList words are copied through, as on N64). Known cost:
 * reloading a model reconverts an already-clean DL and leaks the previous
 * allocation.
 */
static Gfx *portConvertModelGdl(Gfx *src, struct texpool *pool)
{
    s32 n = 0;
    Gfx *dst;

    while ((u8)(src[n].words.w0 >> 24) != (u8)G_ENDDL)
    {
        n++;
        if (n > 0x4000)
        {
            return NULL; /* no terminator - leave the node untouched */
        }
    }
    n++; /* include the ENDDL */

    /* texLoadFromGdl expands each G_SETTEX into a bounded burst, so 4x plus
     * slack covers the worst case. Use portAllocHigh because these buffers
     * become G_DL targets, and a pointer below 0x10000000 would be misread as a
     * segment address. */
    {
        extern void *portAllocHigh(u32 size);
        dst = portAllocHigh((n * 4 + 96) * sizeof(Gfx));
    }
    if (dst == NULL)
    {
        return NULL;
    }

    texLoadFromGdl(src, n * (s32)sizeof(Gfx), dst, pool);
    return dst;
}

/* Persistent texture pool for compiled-in model files, whose texture loads and
 * G_SETTEX expansions run once per file and bake data pointers in permanently.
 * This arena must therefore never be recycled like the callers' stage/menu
 * scratch pools (see portModelTexPool). */
static struct texpool s_ModelPool;
static u8 *s_ModelArena;

struct texpool *portModelTexPool(void)
{
    if (s_ModelArena == NULL)
    {
        extern void *portAllocHigh(u32 size);
        s_ModelArena = portAllocHigh(0x400000); /* pool pointers bake into DLs */
        texInitPool(&s_ModelPool, s_ModelArena, 0x400000);
    }
    return &s_ModelPool;
}

/* NOTE: do not re-add per-stage scoping (a stage watermark) to these lookups;
 * it was tried and reverted because it regressed shadow corruption on Bunker 1
 * -> Caverns -> Bunker 1. Evidence points to one texture number resolving
 * through two different pools depending on the caller, so settle that first. */
#endif

#ifdef PORT
#include <stdio.h>
#include "pcmodels.h" /* D50: PC-layout model sidecars (Plan B, D48/D49) */
#include "pccg.h"     /* D69: PC-layout bg/stan sidecars */
extern resource_lookup_data_entry resource_lookup_data_array[]; /* ob.c */
#endif


/***
 * Perfect Dark:
 * void modeldef0f1a7560(struct modeldef *modeldef, u16 filenum, u32 arg2, struct modeldef *modeldef2, struct texpool *texpool, bool arg5)
 *
 * NTSC address 0x7F0762E0.
*/
void sub_GAME_7F0762E0(ModelFileHeader *objheader, u8 *name, u8 *dst, struct texpool *buffer)
{
    ModelNode *node;
    s32 romremaining;
    Gfx *gdl;
    s32 pcremaining;
    u32 replacementgdl;
    ModelNode *curnode;
    Gfx *curgdl;
    s32 delta;
    s32 filedata;
    s32 filenum;

#ifndef TARGET_N64
    /* Compiled-in models already hold real host pointers, so skip the N64
     * segment-offset rebase and instead load textures into a persistent pool
     * (caller scratch pools get reused and left baked pointers dangling,
     * turning repeat weapons black) and convert each node's G_SETTEX into its
     * own allocation. The iterator cursor MUST advance before the node is
     * repointed, because the Secondary lookup compares against the
     * pre-replacement pointer. */
    texLoadFromModelFileHeader(objheader, portModelTexPool());
    buffer = portModelTexPool();

    node = NULL;
    gdl = NULL;
    modelIterateDisplayLists(objheader, &node, &gdl);

    while (node != NULL && gdl != NULL)
    {
        Gfx *replacement;

        curnode = node;
        curgdl = gdl;
        modelIterateDisplayLists(objheader, &node, &gdl);

        replacement = portConvertModelGdl(curgdl, buffer);
        if (replacement != NULL)
        {
            modelNodeReplaceGdl((u32) objheader, curnode, curgdl, replacement);
        }
    }
    return;
#endif

    filedata = (s32) objheader->Switches;
    filenum = fileGetIndex((char *) name);

    romremaining = get_rom_remaining_buffer_for_index(filenum);
    pcremaining = get_pc_remaining_buffer_for_index(filenum);
    node = 0;
    modelIterateDisplayLists(objheader, &node, &gdl);

    if (gdl != 0)
    {
        name = (u8 *) ((pcremaining - ((s32) (((u8 *) objheader->Switches) + (((u32) gdl) & 0x00ffffff)))) + ((s32) filedata));
        
        /* The signed lvalue cast is required for the compiler to choose the target registers. */
        replacementgdl = (u32)*(s32 *)&gdl;
        
        delta = ((s32) ((romremaining + filedata) - (s32) name)) - ((s32) (((u8 *) objheader->Switches) + (((u32) gdl) & 0x00ffffff)));
        
#ifdef PORT
        /* The destination is a 32-bit GUEST address (romremaining/filedata are
         * s32 bank cursors); rebase it. The source is host-pointer arithmetic
         * off objheader->Switches and needs nothing. */
        texCopyGdls((Gfx *) (((u8 *) objheader->Switches) + (((u32) gdl) & 0x00ffffff)), (Gfx *) GUEST_PTR((romremaining + filedata) - (s32) name), (s32) name);
#else
        texCopyGdls((Gfx *) (((u8 *) objheader->Switches) + (((u32) gdl) & 0x00ffffff)), (Gfx *) ((romremaining + filedata) - (s32) name), (s32) name);
#endif

        texLoadFromModelFileHeader(objheader, buffer);

        if (node != 0)
        {
            do
            {
                curnode = node;
                curgdl = gdl;
                modelIterateDisplayLists(objheader, &node, &gdl);
                
                if (gdl != 0)
                {
                    name = (u8 *) (((s32) gdl) - ((s32) curgdl));
                }
                else
                {
                    name = (u8 *) ((((s32) (filedata + pcremaining)) - ((s32) objheader->Switches)) - (((u32) curgdl) & 0x00ffffff));
                }
                
                modelNodeReplaceGdl((u32) objheader, curnode, curgdl, (Gfx *) replacementgdl);
                
                replacementgdl += texLoadFromGdl( (Gfx *) ((((u8 *) objheader->Switches) + (((u32) curgdl) & 0x00ffffff)) + delta), (s32) name, (Gfx *) (((u8 *) objheader->Switches) + (replacementgdl & 0x00ffffff)), buffer);
            } 
            while (node != 0);
        }

        name = (u8 *) (((s32) (((u8 *) objheader->Switches) + (replacementgdl & 0x00ffffff))) - filedata);

#ifdef PORT
        /* `filedata` is an s32 copy of objheader->Switches, i.e. a POINTER put
         * through a 32-bit truncation. Every other use of it here is guest-
         * address arithmetic and is fine, but fileSetSize passes it on to
         * mempAddEntryOfSizeToBank, which compares it against pool->prevpos --
         * a live host pointer. With a relocated guest window the comparison
         * fails, mempAddEntryOfSizeToBank returns NOT_LAST_ALLOCATION, and the
         * MEMPOOL_STAGE trim SILENTLY does not happen: the first model load
         * keeps the whole rest of the bank, so the next one gets
         * mempGetBankSizeLeft()==0, is loaded with bytes==0 (raw romCopy of the
         * still-RZ-compressed sidecar, no decompressdata), and dies in
         * modelPromoteNodeOffsetsToPointers on the first node. Pass the live
         * pointer. (Lossless, so x86-64 is unchanged.) */
        fileSetSize(filenum, (u8 *) objheader->Switches, (((s32) name + 0xf) & (~0xf)), dst == 0);
#else
        fileSetSize(filenum, (u8 *) filedata, (((s32) name + 0xf) & (~0xf)), dst == 0);
#endif
    }
}


#ifdef PORT
/* Round 7b (docs/port-ios/12-round7b-boot.md, doc 11 ripple 5): served
 * models come from the d43 sidecar, which copies the embedded seg-5 image
 * blobs as RAW cart bytes (big-endian) and remaps the G_SETTIMG operands to
 * their PC offsets, while the texconfig TextureID keeps the N64 offset. So
 * locate the blobs through the display lists, not the texconfigs, and
 * convert the raw bytes to fast3d's GE texel convention: 16-bit texels and
 * TLUT entries are native u16s, 32-bit texels native u32s, 4/8-bit texels
 * stay byte streams. Each load inflates a fresh copy of the file, so this
 * runs on every load (no registry: a later load can land at the same bank
 * address). Size and texel width come from the RDP commands that follow
 * the G_SETTIMG: LOADBLOCK texel count at the SETTIMG size, LOADTLUT entry
 * count, and the render tile's G_SETTILE size. */
#define PORT_INLINE_TEX_MAX 64
#define PORT_INLINE_REG_MAX 16

enum { PORT_ITEX_BYTES = 0, PORT_ITEX_SWAP16 = 1, PORT_ITEX_SWAP32 = 2 };

/* Round 8a (round 7b review, soft spots a + b). (a) Idempotence: the
 * conversion is in place, so a second pass over the SAME inflated buffer
 * would swap the texels back. Each converted file is remembered as
 * {filedata, filesize, fingerprint of the converted bytes}; a pass whose
 * ranges already hash to the remembered post-conversion fingerprint is the
 * same buffer again and is skipped. A fresh load landing at the same bank
 * address holds raw bytes, so its fingerprint differs and it is converted
 * (a raw image that hashes equal is one the swap leaves unchanged, so the
 * skip is still right). (b) Overlap: ranges are converted at unit
 * granularity against a per-file "already converted" byte map, so two
 * SETTIMGs naming overlapping ranges cannot swap the shared texels twice. */
typedef struct { const u8 *data; u32 size; u64 fp; } PortInlineTexReg;
static PortInlineTexReg s_portInlineTexReg[PORT_INLINE_REG_MAX];
static s32 s_portInlineTexRegNext;

static u64 portInlineTexFingerprint(const u8 *filedata, const u32 *off, const u32 *bytes, s32 n)
{
    u64 h = 0xcbf29ce484222325ull;
    s32 i;
    u32 k;
    for (i = 0; i < n; i++)
    {
        for (k = 0; k < bytes[i]; k++)
        {
            h ^= filedata[off[i] + k];
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

static void portSwapInlineTexturesPc(struct ModelFileHeader *objheader, u8 *filedata, u32 filesize,
                                     const char *name)
{
    u32 r_off[PORT_INLINE_TEX_MAX], r_bytes[PORT_INLINE_TEX_MAX];
    u8 r_kind[PORT_INLINE_TEX_MAX];
    s32 nr = 0;
    s32 nconv = 0, nskip = 0, noverlap = 0;
    ModelNode *node = NULL;
    Gfx *gdl = NULL;
    s32 guard = 0;
    s32 i;
    u64 fp;
    u8 *map;

    /* pass 1: collect the seg-5 image ranges from the display lists */
    modelIterateDisplayLists(objheader, &node, &gdl);
    while (node != NULL && gdl != NULL && guard++ < 4096)
    {
        Gfx *g;
        s32 n;

        for (g = gdl, n = 0; n < 8192; g++, n++)
        {
            u32 w0 = (u32) g->words.w0;
            u32 w1 = (u32) g->words.w1;
            u32 cmd = w0 >> 24;
            u32 off, imgsiz, rendersiz = 0xFF, bytes = 0, k;
            s32 tlut = 0;
            Gfx *h;
            s32 m;

            if (cmd == 0xB8) /* G_ENDDL */
            {
                break;
            }
            if (cmd != 0xFD || (w1 >> 24) != 0x05) /* G_SETTIMG, seg 5 */
            {
                continue;
            }
            off = w1 & 0x00FFFFFFu;
            imgsiz = (w0 >> 19) & 3;
            for (h = g + 1, m = 0; m < 16; h++, m++)
            {
                u32 c = (u32) h->words.w0 >> 24;
                u32 x1 = (u32) h->words.w1;
                if (c == 0xB8 || c == 0xFD)
                {
                    break;
                }
                if (c == 0xF3 && bytes == 0) /* G_LOADBLOCK: lrs = texels - 1 */
                {
                    bytes = ((((x1 >> 12) & 0xFFFu) + 1) << imgsiz) >> 1;
                }
                else if (c == 0xF0 && bytes == 0) /* G_LOADTLUT: 16-bit entries */
                {
                    bytes = (((x1 >> 14) & 0x3FFu) + 1) * 2;
                    tlut = 1;
                }
                else if (c == 0xF5 && ((x1 >> 24) & 7) != 7) /* G_SETTILE, not the load tile */
                {
                    rendersiz = ((u32) h->words.w0 >> 19) & 3;
                    break;
                }
            }
            for (k = 0; k < (u32) nr; k++)
            {
                if (r_off[k] == off && r_bytes[k] == bytes)
                {
                    break;
                }
            }
            if (k < (u32) nr)
            {
                continue; /* the same image again (exact duplicate) */
            }
            if (bytes == 0 || off + bytes > filesize || nr >= PORT_INLINE_TEX_MAX)
            {
                nskip++;
                fprintf(stderr, "ge: romdata: model %s: inline texture at +0x%X not converted (%u bytes, file %u)\n",
                        name, off, bytes, filesize);
                continue;
            }
            r_off[nr] = off;
            r_bytes[nr] = bytes;
            r_kind[nr] = (tlut || rendersiz == 2 || (rendersiz == 0xFF && imgsiz == 2)) ? PORT_ITEX_SWAP16
                       : (rendersiz == 3 || (rendersiz == 0xFF && imgsiz == 3)) ? PORT_ITEX_SWAP32
                       : PORT_ITEX_BYTES; /* 4/8-bit texels: byte streams, already the cart's order */
            nr++;
        }
        modelIterateDisplayLists(objheader, &node, &gdl);
    }
    if (nr == 0)
    {
        if (nskip)
        {
            fprintf(stderr, "ge: romdata: model %s: 0 inline texture(s) converted (some skipped)\n", name);
        }
        return;
    }

    /* (a) the same buffer converted already? */
    fp = portInlineTexFingerprint(filedata, r_off, r_bytes, nr);
    for (i = 0; i < PORT_INLINE_REG_MAX; i++)
    {
        const PortInlineTexReg *e = &s_portInlineTexReg[i];
        if (e->data == filedata && e->size == filesize && e->fp == fp)
        {
            fprintf(stderr, "ge: romdata: model %s: inline textures already in the host texel order (repeat pass skipped)\n",
                    name);
            return;
        }
    }

    /* pass 2: convert, each byte at most once (b) */
    map = (u8 *) calloc(filesize, 1);
    for (i = 0; i < nr; i++)
    {
        u8 *p = filedata + r_off[i];
        u8 *mp = map ? map + r_off[i] : NULL;
        u32 unit = (r_kind[i] == PORT_ITEX_SWAP32) ? 4 : (r_kind[i] == PORT_ITEX_SWAP16) ? 2 : 1;
        u32 k, j;
        s32 clash = 0;

        for (k = 0; k + unit <= r_bytes[i]; k += unit)
        {
            s32 seen = 0;
            if (mp != NULL)
            {
                for (j = 0; j < unit; j++)
                {
                    seen |= mp[k + j];
                }
                if (seen)
                {
                    clash = 1;
                    continue; /* converted through an earlier, overlapping SETTIMG */
                }
                for (j = 0; j < unit; j++)
                {
                    mp[k + j] = 1;
                }
            }
            if (unit == 2)
            {
                u8 t = p[k]; p[k] = p[k + 1]; p[k + 1] = t;
            }
            else if (unit == 4)
            {
                u8 a = p[k], b = p[k + 1];
                p[k] = p[k + 3]; p[k + 1] = p[k + 2]; p[k + 2] = b; p[k + 3] = a;
            }
        }
        noverlap += clash;
        nconv++;
    }
    if (map == NULL)
    {
        fprintf(stderr, "ge: romdata: model %s: no memory for the overlap map - ranges converted unchecked\n", name);
    }
    free(map);

    {
        PortInlineTexReg *e = NULL;
        for (i = 0; i < PORT_INLINE_REG_MAX; i++)
        {
            if (s_portInlineTexReg[i].data == filedata)
            {
                e = &s_portInlineTexReg[i]; /* a fresh load at this address replaces it */
                break;
            }
        }
        if (e == NULL)
        {
            e = &s_portInlineTexReg[s_portInlineTexRegNext];
            s_portInlineTexRegNext = (s_portInlineTexRegNext + 1) % PORT_INLINE_REG_MAX;
        }
        e->data = filedata;
        e->size = filesize;
        e->fp = portInlineTexFingerprint(filedata, r_off, r_bytes, nr);
    }

    {
        static s32 logged;
        if (logged++ < 8 || noverlap)
        {
            fprintf(stderr, "ge: romdata: model %s: %d inline texture(s) converted to the host texel order%s%s\n",
                    name, nconv, nskip ? " (some skipped)" : "", noverlap ? " (overlapping ranges converted once)" : "");
        }
    }
}
#endif

#if !defined(TARGET_N64) && !defined(PORT)
/* Three menu models (goldeneyelogo, legalpage, nintendologo) embed textures
 * inside the model file, referenced by raw gsDPSetTextureImage(0x05xxxxxx), and
 * on PC those big-endian u32 words come out byte-reversed. Swap each texture
 * back to the N64 byte image ONCE per file (a registry stops a double swap on
 * repeat menu visits); length = W*H*(4<<RenderDepth)/8. */

#define INLINE_TEX_SWAP_MAX 16

static void *s_InlineTexSwapped[INLINE_TEX_SWAP_MAX];

static void portSwapInlineTextures(struct ModelFileHeader *objheader, u8 *filedata)
{
    s32 i;
    s32 slot;

    for (slot = 0; slot < INLINE_TEX_SWAP_MAX; slot++)
    {
        if (s_InlineTexSwapped[slot] == filedata)
        {
            return;
        }
        if (s_InlineTexSwapped[slot] == NULL)
        {
            break;
        }
    }

    for (i = 0; i < objheader->numtextures; i++)
    {
        u32 id = objheader->Textures[i].TextureID;

        if ((id >> 24) == 0x05)
        {
            u32 *p = (u32 *) (filedata + (id & 0x00FFFFFFu));
            u32 depth = objheader->Textures[i].RenderDepth;
            u32 len = (((u32) objheader->Textures[i].Width *
                        objheader->Textures[i].Height) << depth) >> 1;
            u32 w;

            len = (len + 3) & ~3u;
            for (w = 0; w < len; w += 4)
            {
                u32 v = p[w >> 2];

                if (depth == 2)
                {
                    /* 16-bit texels (RGBA16/IA16): the importer reads
                     * NATIVE u16s (gfx_pc "GE texel convention"), so only
                     * the texel ORDER within each word flips */
                    p[w >> 2] = (v >> 16) | (v << 16);
                }
                else if (depth == 3)
                {
                    /* 32-bit texels: native u32s already in order */
                }
                else
                {
                    /* 4/8-bit formats are BYTE streams: restore the N64
                     * byte image with a full word swap */
                    p[w >> 2] = (v >> 24) | ((v >> 8) & 0xFF00u) |
                                ((v & 0xFF00u) << 8) | (v << 24);
                }
            }
        }
    }

    if (slot < INLINE_TEX_SWAP_MAX)
    {
        s_InlineTexSwapped[slot] = filedata;
    }
}
#endif

/***
 * NTSC addres 0x7F0764A4.
*/
void load_object_fill_header(struct ModelFileHeader *objheader, u8 *name, u8* dst, s32 size, struct texpool * buffer)
{
    void *filedata;

#ifndef TARGET_N64
    extern unsigned long long portProfNow(void);
    extern void portHitchNote(int cat, unsigned long long us);
    const unsigned long long hitchT0 = portProfNow(); /* hitchlog.c */

    /* Load-hitch accounting (frametiming.c): on N64 a model with its own pool
     * (the hands) re-inflates every texture, while the port's persistent pool
     * skips ones it already has. Charge them all from the header below and keep
     * texLoad quiet meanwhile. */
    extern void portHwLoadSuppress(s32 on);
    if (buffer != NULL)
    {
        portHwLoadSuppress(1);
    }
#endif
#ifdef PORT
    /* PC port (D50, Plan B D48/D49): serve PC-layout model sidecars from the
     * cart extension region. One-shot table patch (safe to call repeatedly);
     * by first model load obInit() has definitely run (boss.c:179). Must run
     * before _fileNameLoadTo* below reads hw_address/rom_size. */
    pcmodelsPatchTable();
    /* D69: same one-shot pattern for the bg/stan sidecars -- piggybacks on
     * this call site purely for a "definitely after obInit()" hook; it has
     * nothing to do with model loading. */
    pccgPatchTable();
    if (dst == 0) {
        /* D48.3: a stale poolRemaining from an earlier load of this file
         * would under-allocate the fresh bank chunk (P_old < round8(C_pc)+8)
         * and load_resource would fail with poolRemaining=0. Resetting to 0
         * forces the full-bank allocation; the bump allocator ends at the
         * same cursor either way (mempAddEntryOfSizeToBank shrinks it back). */
        resource_lookup_data_array[fileGetIndex((char *)name)].poolRemaining = 0;
    }
#endif

    if (dst != 0)
    {
        filedata = _fileNameLoadToAddr(name, 0, dst, size);
    }
    else
    {
        filedata = _fileNameLoadToBank(name, 0, 0x100, 4);
    }
    
    objheader->Switches = (struct ModelNode **)filedata;

#ifdef PORT
    /* PC port (D43/D45): the switches array is NS x 8B pointer slots, not NS x 4B words. */
    objheader->Textures = (struct ModelFileTextures *)((u8 *)filedata + sizeof(struct ModelNode *) * objheader->numSwitches);
#else
    // hmmmmmmmmmmmm
    objheader->Textures = (struct ModelFileTextures *)&((s32*)filedata)[objheader->numSwitches];
#endif
    
    objheader->RootNode = (struct ModelNode *)&objheader->Textures[objheader->numtextures];
    
#ifdef TARGET_N64
    sub_GAME_7F075A90(objheader, 0x5000000, filedata);
#elif defined(PORT)
    /* Round 7b (doc 11 ripple 4): every model on a 64-bit host is served from
     * the d43 sidecar, already in the host layout (8-byte pointer slots as
     * seg-5 tokens, 16-byte Gfx), so take the N64 promote path straight onto
     * the file's GUEST address - the donor's form. */
    {
        extern s32 romdataFileServed(s32 index);
        extern void fileSetSize(s32 filenum, u8 *ptr, u32 size, s32 reallocate);
        extern resource_lookup_data_entry resource_lookup_data_array[];
        s32 idx = fileGetIndex(name);
        u32 poolsz = (idx > 0) ? resource_lookup_data_array[idx].poolRemaining : 0;

        if (idx > 0 && romdataFileServed(idx))
        {
            /* EXTRAMEM grabs all remaining STAGE memory: commit the model's
             * real footprint (the PC inflated size) - see the x86 arm */
            if (poolsz > 0)
            {
                fileSetSize(idx, (u8 *) filedata, (poolsz + 0xf) & ~0xfu, dst == 0);
            }
            if (getenv("GE_MODELTIME") != NULL)
            {
                fprintf(stderr, "ge: modeltime: %-16s at guest %08X, %u bytes (dst %p)\n", (const char *) name,
                        GUEST_ADDR(filedata), (unsigned) poolsz, (void *) dst);
            }
            sub_GAME_7F075A90(objheader, 0x5000000, GUEST_ADDR(filedata));
            portSwapInlineTexturesPc(objheader, (u8 *) filedata, poolsz, (const char *) name);
        }
        else
        {
            /* unreachable with GE_COMPILED_ASSETS=OFF: there is no compiled
             * model to fall back on */
            static const struct ModelNode s_portEmptyModelNode;
            objheader->RootNode = (struct ModelNode *) &s_portEmptyModelNode;
            fprintf(stderr, "ge: romdata: model %s (index %d) is not served from the cart - rendering nothing\n",
                    (const char *) name, (int) idx);
        }
    }
#else
    {
        extern s32 romdataFileServed(s32 index);
        extern const u8 *romdataFileCompiled(s32 index);
        extern s32 romfileModelConvert(u8 *data, u32 size, s32 numSwitches, s32 numtextures, const char *name, const u8 *compiled);
        extern resource_lookup_data_entry resource_lookup_data_array[];
        s32 idx = fileGetIndex(name);

        if (romdataFileServed(idx))
        {
            /* The served file is the N64 model image (big-endian, 0x05000000
             * offsets): convert it to host layout, then take the N64 promote
             * path (offsets to real pointers). The inline-texture swap,
             * collision fixup and G_SETTEX expansion below then run on the
             * converted image unchanged. */
            extern void fileSetSize(s32 filenum, u8 *ptr, u32 size, s32 reallocate);
            const u8 *comp = romdataFileCompiled(idx);
            u32 poolsz = resource_lookup_data_array[idx].poolRemaining;
            const unsigned long long convT0 = portProfNow();
            s32 converted = romfileModelConvert((u8 *) filedata, poolsz,
                                                objheader->numSwitches, objheader->numtextures,
                                                (const char *) name, comp);
            {   /* GE_MODELTIME=1: where a model load's time goes */
                static s32 mt = -1;
                if (mt < 0) { mt = getenv("GE_MODELTIME") != NULL; }
                if (mt)
                {
                    fprintf(stderr, "ge: modeltime: %-16s load %.2f ms convert %.2f ms (%u bytes)\n", (const char *) name,
                            (double) (convT0 - hitchT0) / 1000.0, (double) (portProfNow() - convT0) / 1000.0, (unsigned) poolsz);
                }
            }

            /* A served model loads with FILELOADMETHOD_EXTRAMEM, which grabs
             * all remaining STAGE memory, and the PC texture path returns
             * before the N64-only fileSetSize that would reclaim it, so STAGE
             * fills after a few models. Commit the model's real STAGE footprint
             * now (filedata is still STAGE's last allocation); compiled-in
             * files never allocated STAGE and skip this. */
            if (poolsz > 0)
            {
                fileSetSize(idx, (u8 *) filedata, (poolsz + 0xf) & ~0xfu, dst == 0);
            }

            if (converted)
            {
                sub_GAME_7F075A90(objheader, 0x5000000, (u32) filedata);
            }
            else
            {
                /* The served file did not convert cleanly (romfile_model.c
                 * printed why), so point the model at a static empty node that
                 * renders nothing rather than promoting a garbage graph. Never
                 * taken on the stock cart. */
                static const struct ModelNode s_portEmptyModelNode; /* zero = NULL opcode, no children, no rodata */
                objheader->RootNode = (struct ModelNode *) &s_portEmptyModelNode;
                fprintf(stderr, "ge: romdata: model %s could not be converted - rendering nothing\n",
                        (const char *) name);
            }
        }
        else
        {
            /* PORT: compiled-in model data already holds real pointers, so vma
             * == fileramaddr makes the promote a no-op. */
            sub_GAME_7F075A90(objheader, (s32) filedata, (u32) filedata);
        }
    }
    portSwapInlineTextures(objheader, filedata);
    /* PORT: GE_COLAUDIT=1 proves the collision-vertex byte-order fixups
     * stayed exact across reloads of this shared compiled-in blob. */
    portModelCollisionAudit(objheader->RootNode, (const char *) name);
#endif
#ifndef TARGET_N64
    const unsigned long long texT0 = portProfNow();
#endif
    sub_GAME_7F0762E0(objheader, name, dst, buffer);
#ifndef TARGET_N64
    {   /* GE_MODELTIME=1: the model's textures */
        static s32 mt = -1;
        if (mt < 0) { mt = getenv("GE_MODELTIME") != NULL; }
        if (mt)
        {
            fprintf(stderr, "ge: modeltime: %-16s textures %.2f ms (%d, own pool %d)\n", (const char *) name,
                    (double) (portProfNow() - texT0) / 1000.0, (int) objheader->numtextures, buffer != NULL);
        }
    }
    if (buffer != NULL)
    {
        extern void portHwNoteTexture(s32 texnum, u32 romBytes);
        extern u32 portTexRomBytes(s32 texnum);
        extern s32 portTickHwPace(void);
        s32 t;

        portHwLoadSuppress(0);
        for (t = 0; portTickHwPace() && t < objheader->numtextures; t++)
        {
            /* texLoad replaced TextureID with the data pointer; the texture
             * number sits 8 bytes before it (portGunTexAudit, gun.c) */
            u32 id = objheader->Textures[t].TextureID;
#ifdef PORT
            s32 texnum = (id >= (u32) MAX_TEXTURES) ? *(s16 *) ((u8 *) GUEST_PTR(id) - 8) : (s32) id; /* id: GUEST (portModelTexPool, in the window) */
#else
            s32 texnum = (id >= (u32) MAX_TEXTURES) ? *(s16 *) ((u8 *) id - 8) : (s32) id;
#endif

            portHwNoteTexture(texnum, portTexRomBytes(texnum)); /* cart read + last-priced decode */
        }
    }
    portHitchNote(1 /* models */, portProfNow() - hitchT0);
#endif
}




void fileLoad(struct ModelFileHeader *header,char *name)
{
   load_object_fill_header(header,name,0,0,0);
   return;
}


void load_object_into_memory_unused_maybe(struct ModelFileHeader *header,int *recallstring,int *targetloc,int sizeleft)
{
   load_object_fill_header(header,recallstring,targetloc,sizeleft,0);
   return;
}





