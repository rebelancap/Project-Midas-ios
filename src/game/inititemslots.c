#include <ultra64.h>
#include <memp.h>
#include "player.h"
#include "bondinv.h"
#include "inititemslots.h"

void reinit_gunheld_totaltime(void) {
    s32 i;
  
    g_CurrentPlayer->equipallguns = FALSE;
    
    for (i = 0; i != 10; i++) {
        g_CurrentPlayer->gunheldarr[i].totaltime = -1;
    }
}

void alloc_additional_item_slots(s32 additionalentries) {
  g_CurrentPlayer->equipmaxitems = additionalentries + 0x1e;
#ifdef PORT
    /* D-100 (round 19d): 0x14 is the N64 sizeof(InvItem). The host InvItem is
     * 0x20 (its next/prev pointers widen), so the N64 size left bondinvReinitInv
     * writing type = -1 into 0x160+ bytes past this block (30 slots: 960 needed, 608 allocated), into the next
     * MEMPOOL_STAGE allocation, on every stage load. */
    g_CurrentPlayer->p_itemcur     = mempAllocBytesInBank((g_CurrentPlayer->equipmaxitems * sizeof(InvItem) + 0xfU | 0xf) ^ 0xf, MEMPOOL_STAGE);
#else
    g_CurrentPlayer->p_itemcur     = mempAllocBytesInBank((g_CurrentPlayer->equipmaxitems * 0x14 + 0xfU | 0xf) ^ 0xf, MEMPOOL_STAGE);
#endif
  bondinvReinitInv();
}
