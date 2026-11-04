/*
 * 64-bit replacement for src/game/decompress.c + src/game/zlib.c (round 6,
 * residual R1; donor GoldenEye-Decomp-ios engine/port/src/rzdecomp.c, its D32).
 * Both files are removed from the 64-bit build in CMakeLists.txt; the 32-bit
 * x86 build keeps them unchanged (D-M03).
 *
 * GE's "1172" files are a 2-byte header (0x11 0x72) followed by a raw deflate
 * stream. The original decompressdata() skipped the header and ran a
 * hand-rolled gzip-1.2.4 inflate whose Huffman tables are `struct huft`
 * arrays: 8 bytes on MIPS/i386, 16 bytes on LP64 (the union holds a pointer),
 * and the inflater's globals are 32-bit-shaped throughout. Midas already
 * routes the tables into a static workspace (decompress.c PC arm), which keeps
 * the donor's stack overflow away, but the safer, smaller fix is the donor's:
 * real zlib, which owns its table memory. The only symbols the rest of the
 * tree takes from the two files are decompressdata() and rzipGetSomething()
 * (nm of the 64-bit objects; rz_header_bytes and the rz_* globals have no
 * other users), and both are defined here.
 */

#include <ultra64.h>
#include "realzlib.h" /* host zlib by absolute path (CMake-generated) */
#include "guestmem.h"

struct huft; /* opaque; only used as a pointer parameter */

/* Input position just past the last consumed compressed byte, set by the most
 * recent decompressdata(). image.c uses it to locate the next texture section. */
static u8 *s_rz_nextin = NULL;

u32 decompressdata(u8 *src, u8 *dst, struct huft *huffman_table)
{
    z_stream strm = { 0 };
    u32 produced = 0;
    int ret;

    (void) huffman_table; /* real zlib allocates its own tables */

    /* callers pass a host pointer or a bare 32-bit GUEST address (music.c
     * computes its buffers through s32): repair the latter */
    src = (u8 *) GUEST_FIX(src);
    dst = (u8 *) GUEST_FIX(dst);

    /* raw deflate: windowBits = -15, no zlib/gzip wrapper */
    if (inflateInit2(&strm, -15) != Z_OK) {
        return 0;
    }

    /* Skip the 2-byte header (the original skipped it unconditionally too).
     * The compressed size is not in the header, so bound avail_in
     * generously: inflate stops at the final block and reads at most a few
     * bytes ahead of it (handed back in next_in), so a valid stream never
     * reaches past its own end by more than that. */
    strm.next_in = src + 2;
    /* 4 MiB, far above any single RZ file - but never past the end of the
     * mapping: zlib's fast path loads 8 bytes at a time up to avail_in, and a
     * file inflated from the top of the memp arena sits right below the
     * PROT_NONE reservation (round 7b: langInit faulted at guest 0x24000000) */
    strm.avail_in = (uInt) guestMemReadableFrom(src + 2, 0x400000);

    do {
        strm.next_out = dst + produced;
        strm.avail_out = 0x400000; /* the caller sized dst for the whole output, as on N64 */
        ret = inflate(&strm, Z_FINISH);
        produced = (u32) strm.total_out;
    } while (ret == Z_OK);

    s_rz_nextin = (u8 *) strm.next_in; /* just past the compressed data */
    inflateEnd(&strm);
    return produced;
}

/* decompress.h declares this uintptr_t under PORT: the cursor can point at a
 * stack buffer (texInflateZlib), so it is returned whole, never truncated. */
uintptr_t rzipGetSomething(void)
{
    return (uintptr_t) s_rz_nextin;
}
