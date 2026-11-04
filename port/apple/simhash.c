/*
 * simhash.c - the per-tick sim state digest (round 8a, determinism gate).
 *
 * GE_SIMHASH=<file> writes one line per sim tick, `ge: hash: <tick> <hex>`,
 * to <file>; GE_SIMHASH=1 writes it to stdout. Unset = off (zero cost: one
 * cached getenv test per tick).
 *
 * Ported from GoldenEye-Decomp-ios engine/port/src/input.c
 * inputSimStateHash() (D-257): FNV-1a 64 over values, never addresses.
 * Midas's version covers more, because there is no netplay budget here:
 *
 *   - the tick number, the stage being run and the frame counter;
 *   - both RNG streams (g_randomSeed, g_chrObjRandomSeed);
 *   - per player: position, view theta/verta, health, armour, deaths, both
 *     hands' weapon + magazine, all 30 held-ammo counts;
 *   - per chr slot in use: chrnum, action type, sleep, flags, damage, and its
 *     prop's position;
 *   - the active prop list in order: type, flags and position of each prop,
 *     and the count.
 *
 * Called from portInputMouseTickConsume (input_pc.c, or libultra_shim.c's
 * weak headless default), which joy.c joyConsumeSamplesWrapper calls once at
 * the start of each sim tick, so the digest is the state the previous tick
 * left. The tick counter is bumped on the same call as inputrec.c's, so tick
 * N here is tick N in a GE_INPUTREC file.
 *
 * Pointers are only dereferenced, never hashed: a host address differs per
 * run (ASLR, the guest window base) and would make every run "diverge".
 *
 * 64-bit build only (port/apple/ is compiled for GE_HOST32=OFF).
 */
#include <ultra64.h>
#include <bondconstants.h>
#include <bondtypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/bondview.h"
#include "game/player.h"
#include "game/chr.h"

extern u64 g_randomSeed;
extern u64 g_chrObjRandomSeed;
extern PropRecord *g_ActivePropsHead;
extern s32 currentFrameCounter;
s32 lvlGetCurrentStageToLoad(void);
s32 getPlayerCount(void);

static s32 sOn = -1;
static s32 sDbgEvery; /* GE_SIMHASHDBG=n: a readable summary line every n ticks */
static u32 sDbgPlayers, sDbgChrs, sDbgProps;
static FILE *sOut;
static u32 sTick;
static u64 sH;

static void shMix(u32 x)
{
    s32 b;
    for (b = 0; b < 4; b++)
    {
        sH ^= (x >> (8 * b)) & 0xffu;
        sH *= 0x100000001b3ull;
    }
}

static void shF(f32 f)
{
    u32 u;
    memcpy(&u, &f, 4);
    shMix(u);
}

static void shPos(const coord3d *c)
{
    shF(c->f[0]);
    shF(c->f[1]);
    shF(c->f[2]);
}

static u64 simHashCompute(void)
{
    s32 i, n;
    sH = 0xcbf29ce484222325ull;

    shMix(sTick);
    shMix((u32) lvlGetCurrentStageToLoad());
    shMix((u32) currentFrameCounter);
    shMix((u32) g_randomSeed);
    shMix((u32) (g_randomSeed >> 32));
    shMix((u32) g_chrObjRandomSeed);
    shMix((u32) (g_chrObjRandomSeed >> 32));

    sDbgPlayers = sDbgChrs = sDbgProps = 0;
    n = getPlayerCount();
    for (i = 0; i < n && i < 4; i++)
    {
        const struct player *pl = g_playerPointers[i];
        s32 h;
        size_t a;
        if (pl == NULL)
        {
            continue;
        }
        shMix((u32) i);
        sDbgPlayers++;
        if (pl->prop != NULL)
        {
            shPos(&pl->prop->pos);
        }
        shF(pl->vv_theta);
        shF(pl->vv_verta);
        shF(pl->bondhealth);
        shF(pl->bondarmour);
        shMix((u32) pl->deathcount);
        for (h = 0; h < 2; h++)
        {
            shMix((u32) pl->hands[h].weaponnum);
            shMix((u32) pl->hands[h].weapon_ammo_in_magazine);
        }
        for (a = 0; a < sizeof(pl->ammoheldarr) / sizeof(pl->ammoheldarr[0]); a++)
        {
            shMix((u32) pl->ammoheldarr[a]);
        }
    }

    if (g_ChrSlots != NULL)
    {
        for (i = 0; i < g_NumChrSlots && i < 4096; i++)
        {
            const ChrRecord *c = &g_ChrSlots[i];
            if (c->chrnum < 0 || c->model == NULL)
            {
                continue;
            }
            shMix((u32) i);
            sDbgChrs++;
            shMix((u32) c->chrnum);
            shMix((u32) c->actiontype);
            shMix((u32) (u8) c->sleep);
            shMix((u32) c->chrflags);
            shF(c->damage);
            if (c->prop != NULL)
            {
                shPos(&c->prop->pos);
            }
        }
    }

    {
        u32 props = 0;
        const PropRecord *p;
        for (p = g_ActivePropsHead; p != NULL && props < 100000; p = p->next)
        {
            shMix(((u32) p->type << 8) | p->flags);
            shPos(&p->pos);
            props++;
        }
        shMix(props);
        sDbgProps = props;
    }
    return sH;
}

/** portInputMouseTickConsume (PORT): one sim tick is starting */
void portSimHashTick(void)
{
    {   /* round 18: tickprof.c's per-tick CPU sample (off unless asked) */
        extern void geTickProfTick(void);
        geTickProfTick();
    }
    sTick++; /* same numbering as inputrec.c: the tick that is starting */
    {   /* GE_TICKS=n (round 8b, R8A-5): exit cleanly once n sim ticks have run,
         * so replay runs end on the same tick instead of on a wall-paced pump
         * count (GE_FRAMES). Checked as tick n+1 starts: ticks 1..n are done. */
        static long sTickLimit = -1;
        if (sTickLimit < 0)
        {
            const char *t = getenv("GE_TICKS");
            sTickLimit = (t != NULL) ? atol(t) : 0;
        }
        if (sTickLimit > 0 && (long) sTick > sTickLimit)
        {
            if (sOut != NULL)
            {
                fflush(sOut);
            }
            printf("ge: tick limit reached: ticks=%ld\n", sTickLimit);
            exit(0);
        }
    }
    if (sOn < 0)
    {
        const char *e = getenv("GE_SIMHASH");
        sOn = 0;
        if (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0)
        {
            if (strcmp(e, "1") == 0)
            {
                sOut = stdout;
            }
            else
            {
                sOut = fopen(e, "w");
                if (sOut == NULL)
                {
                    fprintf(stderr, "ge: simhash: cannot create %s\n", e);
                    return;
                }
            }
            sOn = 1;
            fprintf(stderr, "ge: simhash: per-tick digest to %s\n", e);
            e = getenv("GE_SIMHASHDBG");
            sDbgEvery = (e != NULL) ? atoi(e) : 0;
        }
    }
    if (sOn)
    {
        u64 h = simHashCompute();
        fprintf(sOut, "ge: hash: %u %016llx\n", sTick, (unsigned long long) h);
        if (sDbgEvery > 0 && (sTick % (u32) sDbgEvery) == 0)
        {
            const struct player *pl = g_playerPointers[0];
            f32 x = 0.0f, z = 0.0f, th = 0.0f, hp = 0.0f;
            if (sDbgPlayers != 0 && pl != NULL)
            {
                if (pl->prop != NULL)
                {
                    x = pl->prop->pos.f[0];
                    z = pl->prop->pos.f[2];
                }
                th = pl->vv_theta;
                hp = pl->bondhealth;
            }
            fprintf(sOut, "ge: hash: dbg %u stage %d players %u chrs %u props %u p1 x %.2f z %.2f theta %.2f health %.3f ammo %d\n",
                    sTick, (int) lvlGetCurrentStageToLoad(), sDbgPlayers, sDbgChrs, sDbgProps,
                    (double) x, (double) z, (double) th, (double) hp,
                    (sDbgPlayers != 0 && pl != NULL) ? (int) pl->hands[0].weapon_ammo_in_magazine : -1);
        }
        fflush(sOut); /* the port can leave through exit() mid-run */
    }
}
