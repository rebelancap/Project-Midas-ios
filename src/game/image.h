#ifndef _IMAGE_H_
#define _IMAGE_H_
#include <ultra64.h>
#include <bondconstants.h>
#include <bondtypes.h>

#define TEXFORMAT_RGBA32     0x00 // 32-bit RGBA (8/8/8/8)
#define TEXFORMAT_RGBA16     0x01 // 16-bit RGBA (5/5/5/1)
#define TEXFORMAT_RGB24      0x02 // 24-bit RGB (8/8/8)
#define TEXFORMAT_RGB15      0x03 // 15-bit RGB (5/5/5)
#define TEXFORMAT_IA16       0x04 // 16-bit grayscale+alpha
#define TEXFORMAT_IA8        0x05 // 8-bit grayscale+alpha (4/4)
#define TEXFORMAT_IA4        0x06 // 4-bit grayscale+alpha (3/1)
#define TEXFORMAT_I8         0x07 // 8-bit grayscale
#define TEXFORMAT_I4         0x08 // 4-bit grayscale
#define TEXFORMAT_RGBA16_CI8 0x09 // 16-bit 5551 paletted colour with 8-bit palette indexes
#define TEXFORMAT_RGBA16_CI4 0x0a // 16-bit 5551 paletted colour with 4-bit palette indexes
#define TEXFORMAT_IA16_CI8   0x0b // 16-bit 88 paletted greyscale+alpha with 8-bit palette indexes
#define TEXFORMAT_IA16_CI4   0x0c // 16-bit 88 paletted greyscale+alpha with 4-bit palette indexes

#define TEXCOMPMETHOD_UNCOMPRESSED0      0
#define TEXCOMPMETHOD_UNCOMPRESSED1      1
#define TEXCOMPMETHOD_HUFFMAN            2
#define TEXCOMPMETHOD_HUFFMANPERHCHANNEL 3
#define TEXCOMPMETHOD_RLE                4
#define TEXCOMPMETHOD_LOOKUP             5
#define TEXCOMPMETHOD_HUFFMANLOOKUP      6
#define TEXCOMPMETHOD_RLELOOKUP          7
#define TEXCOMPMETHOD_HUFFMANBLUR        8
#define TEXCOMPMETHOD_RLEBLUR            9

struct texpool {
	u8 *start;
    struct tex *end;
	u8 *leftpos;
	struct tex *rightpos;
};

struct tex {
	/*0x00*/ u16 texturenum : 12;
	/*0x00*/ u16 unk00_0c : 4;
	/*0x04*/ u8* data;
	/*0x08*/ u8 width;
	/*0x09*/ u8 height;
	/*0x0a*/ u8 unk0a;
	/*0x0b*/ u8 maxlod : 3;
	/*0x0b*/ u8 gbiformat : 3;
	/*0x0b*/ u8 depth : 2;
	/*0x0c*/ u32 lutmodeindex : 2;
	/*0x0c*/ u32 hasExplicitLods : 1;
	/*0x0c*/ u32 unk0c_03 : 1;
	/*0x0c*/ u32 next : 24;
};

#ifdef PORT
/* D67 (docs/dev/findings.md): N64 layout reconstruction for struct
 * image_entry. The decompiled field order cannot be right: texLoad() reads
 * `*(s32*)&entry & 0xFFFFFF` as the data offset (so dataoffset must occupy
 * bits 0-23 of word 0), while chrprop.c indexes entries with an 8-byte
 * stride (`(u8*)g_Textures + n*8`, so sizeof == 8). Declaring every field
 * u32 makes GCC pack the struct into exactly two words on both targets, and
 * putting dataoffset first satisfies the raw word read. The IMAGE() macro
 * initializer order is adjusted to match under PORT (see image.c).
 * NOTE (corrected round 19d, D-100): chrprop.c/propobj.c read
 * `((u8*)&entry)[0] & 0xf` as a hit type. The note that used to sit here
 * claimed that this is the low nibble of dataoffset "on the N64 too". It is
 * not: on the big-endian N64 byte 0 is the MOST significant byte of the word,
 * and the decomp's byte-matching layout {hitSound:4, hitTexture:4,
 * dataoffset:24} (MSB first) puts hitTexture in its low nibble. On this LE
 * layout byte 0 IS the low byte of dataoffset, so the raw read produced a
 * pseudo-random hit type 0..15 against the 13-entry g_HitTypeSounds[].
 * Read it by name: IMAGE_HIT_TYPE(). */
struct image_entry
{
    u32 dataoffset : 24; // runtime: cumulative start offset within the images segment (image_entries_load converts sizes -> offsets)
    u32 hitTexture : 4;  // HitType-Texture
    u32 hitSound   : 4;  // HitType-Sound (should be enum HIT_TYPE, but it needs to be unsigned)
    u32 flag3 : 4; // Detailflag1 used once with value 0x38D2 (S/T offset of detail)
    u32 flag4 : 4;
    u32 flag5 : 4; // DetailFlag2  difference from detail image, to large image, in terms of 2 to the power of value. It is subtracted from the small one, and absolute value or something. (Zoinkity)
    u32 flag6 : 4;
};
#else
struct image_entry
{
    u8 hitSound         : 4;  // HitType-Sound (should be enum HIT_TYPE, but it needs to be unsigned)
    u32 hitTexture : 4;  // HitType-Texture
    u32  dataoffset     : 24; // this is u32 Size:24 - 24bit size/address
    u32 flag3 : 4; // Detailflag1 used once with value 0x38D2 (S/T offset of detail)
	u32 flag4 : 4;
	u32 flag5 : 4; // DetailFlag2  difference from detail image, to large image, in terms of 2 to the power of value. It is subtracted from the small one, and absolute value or something. (Zoinkity)
	u32 flag6 : 4;
};
#endif

struct texcacheitem {
    s16 texturenum;
    u8 widths[7];
    u8 heights[7];
};

extern struct texcacheitem g_TexCacheItems[];
#if defined(PORT) || !defined(TARGET_N64)
/* D85: real storage, not a pointer -- see the definition in image.c. */
/* Midas/donor convergent fix (Midas had a g_PortGlobalTexPool struct with a
 * ptr_texture_alloc_start macro alias; same storage, every use already took
 * &ptr_texture_alloc_start). One definition for every PC build. */
extern struct texpool ptr_texture_alloc_start;
#else
extern struct texpool *ptr_texture_alloc_start;
#endif
#ifndef TARGET_N64
#define PORT_TEXTURES_MAX 8192 /* capacity of g_Textures on PC (2699 stock + sentinel, with headroom) */
#endif
extern struct image_entry g_Textures[];
#ifdef PORT

/* D-100: the surface hit type the bullet-impact paths index g_HitTypeSounds[]
 * with. The N64 spelled it `((u8 *)&g_Textures[n])[0] & 0xf` (byte 0's low
 * nibble on a big-endian word == hitTexture); never spell it as a byte offset
 * on the host -- see the note on struct image_entry. */
#ifdef PORT
/* texnum < 0 ("no texture": a hit whose G_SETTIMG names no pool texture --
 * D-102 recovers the rest) read g_Textures[-1] on the N64 too -- the
 * all-zero table that precedes it, i.e. HIT_DEFAULT. Say so instead of
 * reading out of bounds into whatever the host linker put there. Every real
 * entry's hitTexture/hitSound is 0..12 (images.def, audited round 20a), so a
 * recovered texnum always indexes inside the 13-entry g_HitTypeSounds[]. */
#define IMAGE_HIT_TYPE(texnum) ((texnum) < 0 ? 0 : (s32)g_Textures[(texnum)].hitTexture)
#else
#define IMAGE_HIT_TYPE(texnum) (((u8 *)&g_Textures[(texnum)])[0] & 0xf)
#endif
#ifdef PORT
/* D-102: texture number from a G_SETTIMG address word (bullet-hit surface). */
s32 texNumFromSetTimgAddr(uintptr_t addr);
extern s32 g_NumTexturesPort;
#endif
#endif
extern s32 g_TexCacheCount;
extern u32 bytes;

void nullsub_41(s32 arg0);

void texInflateHuffman(u8 *dst, s32 numiterations, s32 chansize);
void texInflateRle(u8 *dst, s32 blockstotal);
void texReadAlphaBits(u8 *image,s32 count);
void texSwapAltRowBytes(u8 *dst, s32 width, s32 height, s32 format);
void texBlur(u8 *pixels, s32 width, s32 height, s32 method, s32 chansize);
s32 texAlignIndices(u8 *src, s32 width, s32 height, s32 format, u8 *dst);
s32 texFindClosestColourIndexRGBA(u16 *palette, s32 numcolours, s32 r, s32 g, s32 b, s32 a);
s32 texFindClosestColourIndexIA(u16 *palette, s32 numcolours, s32 intensity, s32 alpha);
s32 texShrinkPaletted(u8 *src, u8 *dst, s32 srcwidth, s32 srcheight, s32 format, u16 *palette, s32 numcolours);
struct tex *texFindInPool(s32 texturenum, struct texpool *arg1);
s32 texFreeBytesInBuffer(struct texpool *arg0);
#ifdef PORT
/* D-067 (round 16b): the STAGE texture pool's health. texLoad()'s pool-full
 * arm points the display list at another texture's bytes and leaves nothing in
 * the pool for texSelect() to find -- a silent one-frame corruption, not a
 * warning. See image.c. */
extern unsigned int g_texPoolFullCount;
extern unsigned int g_texPoolFullLast;
void geSetStageTexPoolBytes(unsigned int n);
int  geStageTexPoolFreeBytes(void);
#endif
s32 texInflateNonZlib(u8 *src, u8 *dst, s32 arg2, s32 forcenumimages, struct texpool *arg4);
s32 texInflateZlib(u8 *src, u8 *dst, s32 arg2, s32 forcenumimages, struct texpool *arg4);
void texLoad(s32 *updateword, struct texpool *pool);
void texInitPool(struct texpool *arg0, u8 *arg1, s32 arg2);
void texLoadFromTextureNum(s32 texturenum, struct texpool *arg1);
void texLoadFromModelFileHeader(ModelFileHeader* arg0, struct texpool* arg1);

#endif
