#ifndef _DECOMPRESS_H_
#define _DECOMPRESS_H_
#include <ultra64.h>
#include <inflate/inflate.h>


u32 decompressdata(u8 *src, u8 *dst, struct huft *hlist);
#ifdef PORT
/* Returns a LIVE host pointer just past the compressed data. The N64 s32
 * return threw away half of it; the cursor can point at a stack buffer (see
 * texInflateZlib, whose `scratch`/`compbuffer` are stack arrays), so neither
 * truncation nor a guest-window rebase of the truncated value is valid. */
#include <stdint.h>
uintptr_t rzipGetSomething(void);
#else
s32 rzipGetSomething(void);
#endif

#endif
