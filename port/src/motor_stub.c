/**
 * Rumble Pak API (docs/port/phase2-decisions.md D8: src/motor.c is dropped -
 * it drives raw PIF-ram DMA the shim cannot back). Callers (joy.c,
 * bondview.c) treat nonzero as "no pak"; returning 0 keeps their arming
 * logic happy.
 *
 * With video built, start/stop route to SDL controller rumble via
 * input_pc.c (portInputRumble). The channel comes from osMotorInit -
 * libultra stores it in pfs->channel and GE hands the same pfs back to
 * start/stop, so we do the same. GE pulses start/stop for intensity;
 * input_pc.c turns each start into a 500ms burst that stop cuts short.
 */
#include <ultra64.h>

s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void) mq;
    if (pfs != NULL) {
        pfs->channel = channel;
    }
    return 0;
}

s32 osMotorStart(OSPfs *pfs)
{
#ifdef GE_VIDEO
    extern void portInputRumble(s32 channel, s32 on);
    if (pfs != NULL) {
        portInputRumble(pfs->channel, 1);
    }
#else
    (void) pfs;
#endif
    return 0;
}

s32 osMotorStop(OSPfs *pfs)
{
#ifdef GE_VIDEO
    extern void portInputRumble(s32 channel, s32 on);
    if (pfs != NULL) {
        portInputRumble(pfs->channel, 0);
    }
#else
    (void) pfs;
#endif
    return 0;
}
