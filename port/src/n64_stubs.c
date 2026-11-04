/**
 * PC-port stubs for symbols exported by the N64-only sources the CMake build
 * excludes (crash.c, rmon.c, tlb_manage.c, tlb_hardwire.s, boot.c) but which
 * kept code still references. Each stub returns the value that makes its
 * callers take the benign / retail path — rationale per stub below.
 *
 * os*-named exports of those same files (osSyncPrintf, osReadHost,
 * osWriteHost) are deliberately NOT here: the libultra os shim track owns
 * every os* symbol.
 */
#include <ultra64.h>

#include "crash.h"
#include "rmon.h"
#include "tlb_manage.h"
#include "boot.h"
#include "tlb_resolve.h"

/* ---- crash.c (N64 crash-screen renderer) -------------------------------- */

/**
 * On N64 this registered the fault-handler thread and its message queue.
 * PC gets real crash reporting from the host OS/debugger; nothing to set up.
 * Caller: deb.c debInit() — return is void, no state consumed.
 */
void crashInit(void)
{
}

/**
 * On N64 this drew the crash/stderr text overlay directly into a 16-bit
 * framebuffer. Callers (sched.c CheckDisplayErrorBuffer*) only invoke it when
 * the stderr_* debug flags are raised, and consume no result (void). No-op:
 * the overlay text simply does not render on PC.
 */
void crashRenderFrame(u16 *buffer)
{
    (void) buffer;
}

/* ---- rmon.c (remote debug monitor over the Indy dev-station link) ------- */

/**
 * Thread entry (init.c osCreateThread). The real non-DEBUG build's rmonMain
 * is already an empty shell (no Indy host on retail); same here.
 */
void rmonMain(void)
{
}

/**
 * "Returns TRUE if rmon is disabled" (rmon.c). The retail (non-DEBUG) build
 * returns TRUE, which makes token.c tokenReadIo() clear the token string
 * instead of DMA-reading boot switches from the PI bus, and makes boss.c set
 * the retail debug-token defaults. Match retail: rmon disabled.
 */
s32 rmonGetToken(void)
{
    return TRUE;
}

/**
 * Retail (non-DEBUG) rmon.c returns -1 ("no status"); the only caller
 * (indy_comms.c indycommHost7F0D0124) discards the result anyway.
 */
s32 rmonStatus(void)
{
    return -1;
}

/* ---- tlb_manage.c (TLB pager for the compressed-code overlay scheme) ---- */

/**
 * On N64 this built the page-mapping table that let 7F00xxxx code be paged in
 * from ROM through TLB misses. On PC every function is resident in the
 * executable image; there is nothing to establish. Caller: boss.c
 * bossInitMainthreadData(), void.
 */
void tlbmanageEstablishManagementTable(void)
{
}

/**
 * Reset of the pager's entry counter on level load (lv.c lvInit). No pager on
 * PC, nothing to reset.
 */
void tlbmanageResetCurrentEntriesCount(void)
{
}

/**
 * On N64 this returned the base of the block the pager carved off the top of
 * RAM; boss.c uses it as the END of general heap:
 *   mempCheckMemflagTokens(start, (u32)tlbmanageGetTlbAllocatedBlock() - start)
 * A NULL/0 return would make that size wrap negative, so return a real static
 * block. NOTE: the heap-extent computation itself still depends on os-track
 * symbols (osVirtualToPhysical, _bssSegmentEnd) and must be replaced when the
 * PC memory layout lands — this stub only guarantees a valid pointer.
 */
static u8 tlbAllocatedBlockBacking[TLB_BLOCK_SIZE];

u8 (*tlbmanageGetTlbAllocatedBlock(void))[TLB_BLOCK_SIZE]
{
    return &tlbAllocatedBlockBacking;
}

/* ---- tlb_hardwire.s (TLB-miss exception handler code blob) -------------- */

/**
 * init.c mainproc copies the machine code at this label to K0BASE as the TLB
 * refill handler. Only its ADDRESS is taken; the copy loop belongs to the N64
 * boot chain that port/src/main.c replaces and never runs on PC.
 */
void resolve_TLBaddress_for_InvalidHit(void)
{
}

/* ---- boot.c (segment-address accessors for the boot decompressor) ------- */

/**
 * These five wrap linker-script segment symbols and feed exactly one caller:
 * init.c mainproc's boot-time decompression of the compressed code segment
 * (jump_decompressfile). That path is N64-only — on PC the code is already
 * loaded by the executable loader and port/src/main.c owns startup — so the
 * addresses are meaningless here; return 0. If the romdata track ever needs
 * real values it owns the segment symbols these wrapped.
 */
u32 get_csegmentSegmentStart(void)
{
    return 0;
}

u32 get_cdataSegmentRomStart(void)
{
    return 0;
}

u32 get_cdataSegmentRomEnd(void)
{
    return 0;
}

u32 get_inflateSegmentRomStart(void)
{
    return 0;
}

u32 get_inflateSegmentRomEnd(void)
{
    return 0;
}
