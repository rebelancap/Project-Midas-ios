#include <ultra64.h>
#include <bondtypes.h>
#include "stan.h"


/**
 * NTSC address 0x7F0067C0.
*/
#ifndef TARGET_N64
/* Debug (GE_DUMP_BOND): logs each pad's final tile outcome, since a name match
 * can still be rejected by the inside-triangle test; rc 1=direct 2=walk-recovered 0=no tile. */
static void padres_rc(s32 rc, struct StandTile *t)
{
    extern void ge_dbg_vec(const char *tag, const f32 *v, s32 n);
    static s32 n;
    f32 v[3];
    v[0] = (f32) n++;
    v[1] = (f32) rc;
    v[2] = (f32) (u32) t;
    ge_dbg_vec("padrc n/rc/tile", v, 3);
}
#endif

s32 init_pathtable_something(struct PadRecord *pad, char *tilename, struct StandTile **tile_stack)
{
    struct coord3d coord;

    *tile_stack = stanMatchTileName(tilename);

#ifndef TARGET_N64
    { /* Debug (GE_DUMP_BOND): per-pad resolution audit printing the name match,
       * parsed id halves and the scaled position fed to the fallback finder. */
        extern void ge_dbg_vec(const char *tag, const f32 *v, s32 n);
        extern void stanPackId(char *id, u16 *hi, u8 *lo);
        static s32 count;
        u16 hi = 0;
        u8 lo = 0;
        f32 v[7];
        stanPackId(tilename, &hi, &lo);
        v[0] = (f32) count++;
        v[1] = (f32) hi;
        v[2] = (f32) lo;
        v[3] = (f32) (u32) *tile_stack;
        v[4] = pad->pos.f[0];
        v[5] = pad->pos.f[2];
        v[6] = (f32) (u8) tilename[0];
        ge_dbg_vec("padres n/hi/lo/match/x/z/c0", v, 7);
    }
#endif
    
    if ((*tile_stack == NULL) || (isPointInsideTriStandTileUnscaled_Maybe(*tile_stack, pad->pos.f[0], pad->pos.f[2]) == 0))
    {
        coord.f[0] = pad->pos.f[0];
        coord.f[1] = pad->pos.f[1];
        coord.f[2] = pad->pos.f[2];

        *tile_stack = sub_GAME_7F0AFB78(&coord.f[0], &coord.f[1], &coord.f[2], 0);

        if ((*tile_stack != NULL) && (walkTilesBetweenPoints_NoCallback(tile_stack, coord.f[0], coord.f[2], pad->pos.f[0], pad->pos.f[2]) != 0))
        {
#ifndef TARGET_N64
            padres_rc(2, *tile_stack);
#endif
            return 2;
        }

        *tile_stack = NULL;

#ifndef TARGET_N64
        padres_rc(0, *tile_stack);
#endif
        return 0;
    }

#ifndef TARGET_N64
    padres_rc(1, *tile_stack);
#endif
    return 1;
}
 
