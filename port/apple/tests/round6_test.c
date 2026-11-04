/*
 * Round 6 host test (docs/port-ios/09-round6-shim.md, gate 3). macOS arm64.
 * Built and run by port/apple/tests/run_round6_test.sh against the real
 * port/apple/guestmem.c, port/src/libultra_shim.c (PI DMA, osVirtualToPhysical),
 * port/include/port_segaddr.h (fast3d's seg_addr / G_MW_SEGMENT logic) and
 * port/apple/rzdecomp.c (system zlib), plus Midas's own src/game/zlib.c +
 * decompress.c renamed to midas_* as the reference inflater.
 *
 * argv[1] (optional): a GoldenEye (U) z64 image. Its 1172 cdata stream at
 * 0x21990 is inflated by both inflaters and compared. Without it only the
 * synthetic zlib stream is checked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

#include <ultra64.h>
#include "guestmem.h"
#include "port_segaddr.h"
#include "realzlib.h"

/* ---- port stubs the linked TUs want ------------------------------------- */
void portConLogf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("    [log] ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
}

/* link-only stubs for the shim's controller/EEPROM paths (never called here) */
#define R6_STUB(name) void name(void) { fprintf(stderr, "round6_test: stub " #name " called\n"); abort(); }
R6_STUB(portConIsOpen)
R6_STUB(portInputPadGadgetChord)
R6_STUB(portInputPadMask)
R6_STUB(portInputPadMerge)
R6_STUB(portInputPadNeutral)
R6_STUB(portInputPollTick)
R6_STUB(portInputRecPad)
R6_STUB(portInputRecTick)
R6_STUB(portInputStickGate)
R6_STUB(portJoyPortPollCommit)
R6_STUB(portRootFopen)
R6_STUB(portRootPath)
R6_STUB(portSimHashTick)
R6_STUB(videoGetPad)
unsigned char ge_dbg_vec[256];

/* ---- an absolute guest symbol, exactly as segments.c defines them -------- */
__asm__("    .globl _r6_abs_seg\n"
        "    .set _r6_abs_seg, 0x1F000000\n");
extern unsigned char r6_abs_seg[];

/* ---- the inflaters ------------------------------------------------------- */
struct huft;
extern u32 decompressdata(u8 *src, u8 *dst, struct huft *h);      /* rzdecomp.c */
extern uintptr_t rzipGetSomething(void);                            /* rzdecomp.c */
extern u32 midas_decompressdata(u8 *src, u8 *dst, struct huft *h); /* src/game/decompress.c */
extern uintptr_t midas_rzipGetSomething(void);

/* one fixed call site for the warn-once-per-caller check */
static __attribute__((noinline)) u32 r6_v2p(void *p)
{
    u32 v = osVirtualToPhysical(p);
    __asm__ volatile("" ::: "memory"); /* keep it a real call, not a tail call */
    return v;
}

static int s_fail;
#define CHECK(cond, ...) do { \
        if (cond) { printf("  ok   "); } else { printf("  FAIL "); s_fail++; } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static unsigned char *commit(u32 guest, size_t len)
{
    return (unsigned char *) guestMemCommit(guest, GUEST_PAGE_ROUND(len), -1);
}

static int memeq(const u8 *a, const u8 *b, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char *rom, *memp, *high, *seg;
    static uintptr_t segbase[16];
    static unsigned char exebuf[64];
    unsigned char stackbuf[64];
    OSMesgQueue mq;
    OSMesg msgs[4];
    OSIoMesg mb;
    u32 i;

    printf("round6_test: macOS arm64 host checks\n");

    /* 1. window */
    printf("[1] guest window\n");
    CHECK(guestMemInit() == 0, "guestMemInit: base %p", (void *) g_guestBase);
    CHECK(g_guestBase != 0 && (g_guestBase & 0xFFFFFFFFull) == 0, "base has zero low 32 bits");
    rom = commit(0x10000000u, 0x8000);
    seg = commit(0x1F000000u, 0x4000);
    memp = commit(0x20000000u, 0x8000);
    high = commit(0x30000000u, 0x4000);
    CHECK(rom && seg && memp && high, "commits ROM %p seg %p memp %p high %p", rom, seg, memp, high);

    /* 2. GUEST_ADDR / GUEST_PTR round trips */
    printf("[2] GUEST_ADDR / GUEST_PTR\n");
    {
        unsigned char *ps[4] = { rom + 0x1234, memp + 0x40, high + 0x10, seg + 0x80 };
        u32 want[4] = { 0x10001234u, 0x20000040u, 0x30000010u, 0x1F000080u };
        for (i = 0; i < 4; i++) {
            CHECK(GUEST_ADDR(ps[i]) == want[i] && GUEST_PTR(want[i]) == (void *) ps[i]
                  && (s32) GUEST_ADDR(ps[i]) > 0 && GUEST_IN_WINDOW(ps[i]),
                  "%p -> %08X -> %p (positive as s32, in window)", ps[i], GUEST_ADDR(ps[i]), GUEST_PTR(want[i]));
        }
        CHECK(GUEST_FIX((uintptr_t) 0x20000040u) == (void *) (memp + 0x40) && GUEST_FIX(exebuf) == (void *) exebuf
              && GUEST_FIX(0) == NULL, "GUEST_FIX repairs a truncated guest, passes host pointers and NULL");
        CHECK(!GUEST_IN_WINDOW(exebuf) && !GUEST_IN_WINDOW(stackbuf), "exe .bss %p and stack %p are outside", exebuf, stackbuf);
    }

    /* 3. absolute guest symbols (segments.c) */
    printf("[3] absolute segment symbols\n");
    CHECK((uintptr_t) r6_abs_seg == 0x1F000000u, "&abs symbol = %p (not slid)", (void *) r6_abs_seg);
    CHECK((unsigned char *) CART_SYM(r6_abs_seg) == seg, "CART_SYM(abs) = %p = the committed image", CART_SYM(r6_abs_seg));
    CHECK((s32) (uintptr_t) r6_abs_seg + 0x80 == 0x1F000080, "(s32)&sym + off stays a lossless guest address");

    /* 4. fast3d address resolution */
    printf("[4] seg_addr (port_segaddr.h)\n");
    segbase[5] = portSegBase64(GUEST_ADDR(memp)); /* G_MW_SEGMENT with a truncated base */
    segbase[3] = portSegBase64((uintptr_t) high);  /* ... and with a live one */
    CHECK(segbase[5] == (uintptr_t) memp && segbase[3] == (uintptr_t) high && portSegBase64(0) == 0,
          "G_MW_SEGMENT: truncated base rebased, live base kept, 0 kept");
    {
        uintptr_t w;
        void *r;

        r = portSegAddr64((uintptr_t) (memp + 0x100), segbase);
        CHECK(r == memp + 0x100, "window pointer (full width) -> %p", r);
        r = portSegAddr64(GUEST_ADDR(memp + 0x100), segbase);
        CHECK(r == memp + 0x100, "window pointer (truncated %08X) -> %p", GUEST_ADDR(memp + 0x100), r);
        w = (uintptr_t) exebuf;
        r = portSegAddr64(w, segbase);
        CHECK(r == exebuf, ">4 GiB exe pointer %p -> %p", (void *) w, r);
        /* a host pointer whose low word looks like segment 5: the old code's 1-in-256 case */
        w = (g_guestBase + 0x100000000ull) | 0x05000010u;
        r = portSegAddr64(w, segbase);
        CHECK(r == (void *) w, "host pointer %p with low word 0x05000010 is NOT taken for segment 5", (void *) w);
        r = portSegAddr64(0x05000010u, segbase);
        CHECK(r == memp + 0x10, "untagged token 0x05000010 -> seg5 + 0x10 = %p", r);
        r = portSegAddr64(0x03000021u, segbase);
        CHECK(r == high + 0x20, "PD-tagged token 0x03000021 -> seg3 + 0x20 = %p", r);
        r = portSegAddr64(0x07000010u, segbase);
        CHECK(r == (void *) 0x07000010u, "token for an uninstalled segment passes through (%p)", r);
        CHECK(portSegAddr64(0, segbase) == NULL, "0 -> NULL");
        r = portSegAddr64(0x1F000080u, segbase);
        CHECK(r == seg + 0x80, "bare cart guest 0x1F000080 -> %p", r);
    }

    /* 5. libultra shim */
    printf("[5] libultra shim: PI DMA, osVirtualToPhysical, OS_K0 macros\n");
    osCreateMesgQueue(&mq, msgs, 4);
    for (i = 0; i < 64; i++) {
        rom[0x200 + i] = (u8) (i * 3 + 1);
    }
    osPiStartDma(&mb, OS_MESG_PRI_NORMAL, OS_READ, 0x10000200u, stackbuf, 64, &mq);
    CHECK(memeq(stackbuf, rom + 0x200, 64), "osPiStartDma cart 0x10000200 -> stack buffer");
    osRecvMesg(&mq, NULL, OS_MESG_NOBLOCK);
    osPiStartDma(&mb, OS_MESG_PRI_NORMAL, OS_READ, 0x10000200u, (void *) (uintptr_t) GUEST_ADDR(memp + 0x400), 64, &mq);
    CHECK(memeq(memp + 0x400, rom + 0x200, 64), "osPiStartDma -> memp given as a bare guest address (GUEST_FIX)");
    osRecvMesg(&mq, NULL, OS_MESG_NOBLOCK);
    osPiRawStartDma(OS_READ, GUEST_ADDR(seg), exebuf, 16);
    CHECK(memeq(exebuf, seg, 16), "osPiRawStartDma from the absolute segment image -> exe .bss");
    exebuf[0] = 0x5A;
    osPiStartDma(&mb, OS_MESG_PRI_NORMAL, OS_READ, 0x00000000u, exebuf, 16, &mq);
    CHECK(exebuf[0] == 0x5A && osRecvMesg(&mq, NULL, OS_MESG_NOBLOCK) == 0,
          "device address 0 (unserved weak asset): copy skipped, completion still posted");
    CHECK(osVirtualToPhysical(memp + 8) == 0x20000008u, "osVirtualToPhysical(window) = 0x20000008, silent");
    CHECK(osVirtualToPhysical((void *) (uintptr_t) 0x30000010u) == 0x30000010u, "osVirtualToPhysical(bare guest) = identity, silent");
    printf("  (next line must be exactly one warning, naming r6_v2p)\n");
    for (i = 0; i < 3; i++) {
        (void) r6_v2p(exebuf); /* one call site, three calls: one warning */
    }
    CHECK(OS_K0_TO_PHYSICAL(exebuf) == (uintptr_t) exebuf, "OS_K0_TO_PHYSICAL(exe ptr) is the full-width identity");
    CHECK(OS_PHYSICAL_TO_K0(0x20000040u) == (void *) (memp + 0x40) && OS_PHYSICAL_TO_K0(exebuf) == (void *) exebuf,
          "OS_PHYSICAL_TO_K0 repairs a guest number, keeps a live pointer");

    /* 6. inflate */
    printf("[6] rzdecomp (system zlib) vs Midas's gzip-1.2.4 inflate\n");
    {
        static u8 plain[20000], comp[30000], out1[20000], out2[20000];
        z_stream zs = { 0 };
        u32 n1, n2, clen;

        for (i = 0; i < sizeof(plain); i++) {
            plain[i] = (u8) ((i * 7) ^ (i >> 5));
        }
        deflateInit2(&zs, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        comp[0] = 0x11;
        comp[1] = 0x72;
        zs.next_in = plain;
        zs.avail_in = sizeof(plain);
        zs.next_out = comp + 2;
        zs.avail_out = sizeof(comp) - 2;
        deflate(&zs, Z_FINISH);
        clen = (u32) zs.total_out;
        deflateEnd(&zs);
        n1 = decompressdata(comp, out1, NULL);
        CHECK(n1 == sizeof(plain) && memeq(out1, plain, n1), "synthetic 1172 stream: %u -> %u bytes, round trip exact", clen, n1);
        CHECK(rzipGetSomething() == (uintptr_t) (comp + 2 + clen), "rzipGetSomething = just past the stream (full pointer)");
        n2 = midas_decompressdata(comp, out2, NULL);
        CHECK(n2 == n1 && memeq(out1, out2, n1), "Midas's inflater agrees (%u bytes)", n2);

        if (argc > 1) {
            FILE *f = fopen(argv[1], "rb");
            static u8 cart[0x40000];
            u8 *cbuf1 = malloc(1u << 20), *cbuf2 = malloc(1u << 20); /* ROMDATA_CDATA_ALLOC */
            size_t got = 0;

            if (f != NULL) {
                got = fread(cart, 1, sizeof(cart), f);
                fclose(f);
            }
            if (got == sizeof(cart) && cart[0] == 0x80 && cart[0x21990] == 0x11 && cart[0x21991] == 0x72) {
                n1 = decompressdata(cart + 0x21990, cbuf1, NULL);
                n2 = midas_decompressdata(cart + 0x21990, cbuf2, NULL);
                CHECK(n1 == 0x3C550u /* romdata.c ROMDATA_CDATA_SIZE */,
                      "cart cdata @0x21990: rzdecomp inflates %u bytes (0x%X, the stock size)", n1, n1);
                CHECK(n1 == n2 && memeq(cbuf1, cbuf2, n1), "byte-identical to Midas's inflater");
                CHECK(rzipGetSomething() - (uintptr_t) cart == midas_rzipGetSomething() - (uintptr_t) cart,
                      "end-of-stream cursor agrees: cart+0x%lX", (unsigned long) (rzipGetSomething() - (uintptr_t) cart));
            } else {
                printf("  skip cart check: %s is not a z64 GoldenEye image (read %zu)\n", argv[1], got);
            }
            free(cbuf1);
            free(cbuf2);
        } else {
            printf("  skip cart check: no ROM path given\n");
        }
    }

    printf("round6_test: %s (%d failure%s)\n", s_fail ? "FAILED" : "PASSED", s_fail, s_fail == 1 ? "" : "s");
    return s_fail ? 1 : 0;
}
