#ifndef _IMAGE_BANK_H_
#define _IMAGE_BANK_H_
#include <ultra64.h>
#include <bondtypes.h>
#include "bondview.h"

extern struct sImageTableEntry *crosshairimage;

extern struct sImageTableEntry *mainfolderimages;
extern struct sImageTableEntry *mpstageselimages;
extern struct sImageTableEntry *genericimage;
extern struct sImageTableEntry *skywaterimages;
extern struct sImageTableEntry *monitorimages;
extern struct sImageTableEntry *mpcharselimages;
extern struct sImageTableEntry *mpradarimages;
extern struct sImageTableEntry *impactimages;
extern struct sImageTableEntry *explosion_smokeimages;
extern struct sImageTableEntry *scattered_explosions;
extern struct sImageTableEntry *flareimage2;
extern struct sImageTableEntry *glassoverlayimage;
extern struct sImageTableEntry *flareimage3;
extern struct sImageTableEntry *flareimage4;
extern struct sImageTableEntry *flareimage5;

extern u8* img_curpos;
extern s32 img_bitcount;
extern s32 *pGlobalimagetable;
extern s32 globalbank_rdram_offset;

void texReset(void);
u32 texReadBits(s32 bitCount);
#ifdef PORT
/* The three callers pass live HOST pointers (a stack decompression buffer,
 * the zlib cursor). The N64 prototype's s32 truncates them, which on the PC
 * port is a wild pointer -- the exe stack is above 4 GiB on every 64-bit
 * host, so this is a latent bug on Windows/Linux too, not just Apple arm64.
 * Take the argument at pointer width. */
void texSetBitstring(uintptr_t pos);
#else
void texSetBitstring(s32 pos);
#endif

#endif
