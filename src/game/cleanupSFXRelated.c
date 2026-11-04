#include <ultra64.h>
#include <snd.h>
#include <PR/libaudio.h>
#include "bondview.h"

#ifndef TARGET_N64
#include <stdio.h>
#endif

void cleanupSFXRelated(void) {
#ifndef TARGET_N64
    /* Always-on diagnostic: what the tank handles held at unload */
    if (g_TankSfxState[0] != 0 || g_TankSfxState[1] != 0)
    {
        fprintf(stderr, "ge: snd: stage unload - tank sfx engine=%p (state %d) motion=%p (state %d)\n",
                (void *) g_TankSfxState[0], g_TankSfxState[0] ? (int) sndGetPlayingState(g_TankSfxState[0]) : -1,
                (void *) g_TankSfxState[1], g_TankSfxState[1] ? (int) sndGetPlayingState(g_TankSfxState[1]) : -1);
    }
#endif
    if (g_TankSfxState[0] != 0)
    {
        if (sndGetPlayingState(g_TankSfxState[0]) != AL_STOPPED)
        {
            sndDeactivate(g_TankSfxState[0]);
        }
    }

    if (g_TankSfxState[1] != 0)
    {
        if (sndGetPlayingState(g_TankSfxState[1]) != AL_STOPPED)
        {
            sndDeactivate(g_TankSfxState[1]);
        }
    }

}
