#include <ultra64.h>
#include "objecthandler.h"

/*
 * Address: 0x7F0009E0
*/
#ifdef PORT
/* `unused` receives &dword_CODE_bss_80069458 (initanitable.c): a pointer. */
void initAnimationsBuffer(struct bondstruct_unk_animation_related* animBuffer, OSMesgQueue* mq, void *unused) {
#else
void initAnimationsBuffer(struct bondstruct_unk_animation_related* animBuffer, OSMesgQueue* mq, s32 unused) {
#endif
    D_80036414 = animBuffer; // Data structure with offset and two tables
#ifdef PORT
    D_80036418 = (s32)(uintptr_t)mq;   /* dead on the PC port (Indy dev hardware / unused); cast made explicit */   // Unused message queue
#else
    D_80036418 = mq;   // Unused message queue
#endif
#ifdef PORT
    D_8003641C = (s32)(uintptr_t)unused; /* never read (objecthandler.c); a BSS address, so this truncation must stay write-only */
#else
    D_8003641C = unused; // Some unused array
#endif
}

