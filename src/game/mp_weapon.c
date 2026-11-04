#include <ultra64.h>
#include <bondconstants.h>
#include "mp_weapon.h"
#include "assets/obseg/text/LmpweaponsE.h"
// data
//D:80048670
struct s_mp_weapon_set mp_weapon_set_slaps[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UNARMED, PROP_CHRTT33, 1.0, AMMO_9MM, 0, 1)
};

//D:80048730
struct s_mp_weapon_set mp_weapon_set_pistols[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPKSIL, PROP_CHRWPPKSIL, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPKSIL, PROP_CHRWPPKSIL, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPKSIL, PROP_CHRWPPKSIL, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_RUGER, PROP_CHRRUGER, 1.0, AMMO_MAGNUM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_RUGER, PROP_CHRRUGER, 1.0, AMMO_MAGNUM, 0x32, 1)
};

//D:800487F0
struct s_mp_weapon_set mp_weapon_set_knife[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_THROWKNIFE, PROP_CHRTHROWKNIFE, 1.0, AMMO_KNIFE, 0xA, 0)
};

//D:800488B0
struct s_mp_weapon_set mp_weapon_set_auto[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_WPPKSIL, PROP_CHRWPPKSIL, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPKSIL, PROP_CHRWPPKSIL, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_MP5K, PROP_CHRMP5K, 1.0, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_MP5K, PROP_CHRMP5K, 1.0, AMMO_9MM, 0x64, 1)
};

//D:80048970
struct s_mp_weapon_set mp_weapon_set_power[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_RUGER, PROP_CHRRUGER, 1.0, AMMO_MAGNUM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_RUGER, PROP_CHRRUGER, 1.0, AMMO_MAGNUM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_FNP90, PROP_CHRFNP90, 1.0, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_FNP90, PROP_CHRFNP90, 1.0, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AUTOSHOT, PROP_CHRAUTOSHOT, 1.0, AMMO_SHOTGUN, 0x1E, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AUTOSHOT, PROP_CHRAUTOSHOT, 1.0, AMMO_SHOTGUN, 0x1E, 1)
};

//D:80048A30
struct s_mp_weapon_set mp_weapon_set_sniper[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_RUGER, PROP_CHRRUGER, 1.0, AMMO_MAGNUM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_RUGER, PROP_CHRRUGER, 1.0, AMMO_MAGNUM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.0, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.0, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SNIPERRIFLE, PROP_CHRSNIPERRIFLE, 1.0, AMMO_RIFLE, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SNIPERRIFLE, PROP_CHRSNIPERRIFLE, 1.0, AMMO_RIFLE, 0x32, 1)
};

//D:80048AF0
struct s_mp_weapon_set mp_weapon_set_grenade[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_GRENADE, PROP_CHRGRENADE, 1.5, AMMO_GRENADE, 5, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_GRENADE, PROP_CHRGRENADE, 1.5, AMMO_GRENADE, 5, 0)
};

//D:80048BB0
struct s_mp_weapon_set mp_weapon_set_remote_m[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_WPPK, PROP_CHRWPPK, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPK, PROP_CHRWPPK, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UZI, PROP_CHRUZI, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UZI, PROP_CHRUZI, 1.5, AMMO_9MM, 0x64, 1),
#ifdef BUGFIX_R0
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
#else
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
#endif
    INLINE_S_MP_WEAPON_SET(ITEM_REMOTEMINE, PROP_CHRREMOTEMINE, 1.5, AMMO_REMOTEMINE, 5, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_REMOTEMINE, PROP_CHRREMOTEMINE, 1.5, AMMO_REMOTEMINE, 5, 0)
};


//D:80048C70
struct s_mp_weapon_set mp_weapon_set_glaunch[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_GRENADELAUNCH, PROP_CHRGRENADELAUNCH, 1.0, AMMO_GRENADEROUND, 6, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_GRENADELAUNCH, PROP_CHRGRENADELAUNCH, 1.0, AMMO_GRENADEROUND, 6, 1)
};

//D:80048D30
struct s_mp_weapon_set mp_weapon_set_timed_m[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_WPPK, PROP_CHRWPPK, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPK, PROP_CHRWPPK, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UZI, PROP_CHRUZI, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UZI, PROP_CHRUZI, 1.5, AMMO_9MM, 0x64, 1),
#ifdef BUGFIX_R0
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
#else
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
#endif
    INLINE_S_MP_WEAPON_SET(ITEM_TIMEDMINE, PROP_CHRTIMEDMINE, 1.5, AMMO_TIMEDMINE, 5, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_TIMEDMINE, PROP_CHRTIMEDMINE, 1.5, AMMO_TIMEDMINE, 5, 0)
};

//D:80048DF0 
struct s_mp_weapon_set mp_weapon_set_prox_m[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_WPPK, PROP_CHRWPPK, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPK, PROP_CHRWPPK, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UZI, PROP_CHRUZI, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_UZI, PROP_CHRUZI, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_M16, PROP_CHRM16, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_PROXIMITYMINE, PROP_CHRPROXIMITYMINE, 1.5, AMMO_PROXMINE, 5, 0),
    INLINE_S_MP_WEAPON_SET(ITEM_PROXIMITYMINE, PROP_CHRPROXIMITYMINE, 1.5, AMMO_PROXMINE, 5, 0)
};

//D:80048EB0
struct s_mp_weapon_set mp_weapon_set_rockets[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_ROCKETLAUNCH, PROP_CHRROCKETLAUNCH, 1.5, AMMO_ROCKETS, 6, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_ROCKETLAUNCH, PROP_CHRROCKETLAUNCH, 1.5, AMMO_ROCKETS, 6, 1)
};

//D:80048F70
struct s_mp_weapon_set mp_weapon_set_lasers[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_LASER, PROP_CHRLASER, 1.5, AMMO_NONE, 0, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_LASER, PROP_CHRLASER, 1.5, AMMO_NONE, 0, 1)
};

//D:80049030
struct s_mp_weapon_set mp_weapon_set_golden[] = 
{
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_TT33, PROP_CHRTT33, 3.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_SKORPION, PROP_CHRSKORPION, 1.5, AMMO_9MM, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_AK47, PROP_CHRKALASH, 1.5, AMMO_RIFLE, 0x64, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_WPPKSIL, PROP_CHRWPPKSIL, 1.0, AMMO_9MM, 0x32, 1),
    INLINE_S_MP_WEAPON_SET(ITEM_GOLDENGUN, PROP_CHRGOLDEN, 1.5, AMMO_GGUN, 0xA, 1)
};

//D:800490F0
struct s_mp_weapon_set_text mp_weapon_set_text_table[] = 
{
    {getStringID(LMPWEAPONS, MPWEAPON_STR_00_SLAPPERSONLY), mp_weapon_set_slaps},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_01_PISTOLS), mp_weapon_set_pistols},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_0D_THROWINGKNIVES), mp_weapon_set_knife},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_02_AUTOMATICS), mp_weapon_set_auto},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_03_POWERWEAPONS), mp_weapon_set_power},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_04_SNIPERRIFLES), mp_weapon_set_sniper},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_05_GRENADES), mp_weapon_set_grenade},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_06_REMOTEMINES), mp_weapon_set_remote_m},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_07_GRENADELAUNCHERS), mp_weapon_set_glaunch},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_08_TIMEDMINES), mp_weapon_set_timed_m},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_09_PROXIMITYMINES), mp_weapon_set_prox_m},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_0A_ROCKETS), mp_weapon_set_rockets},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_0B_LASERS), mp_weapon_set_lasers},
    {getStringID(LMPWEAPONS, MPWEAPON_STR_0C_GOLDENGUN), mp_weapon_set_golden}
};

s32 mp_weapon_set = 0xB;


//increment mp_weapon_set by 1, capping at 0xE
void incrementMPWeaponSet(void)
{
    mp_weapon_set = (mp_weapon_set + 1) % 0xe;
}

#ifndef TARGET_N64
/* Custom weapon sets: PORT_MP_WEAPONSET_CUSTOM (one past Rare's fourteen) uses these eight slots,
 * filled from a saved .gewset file (port_wsets.c), with each weapon's other
 * slot data copied from the first Rare set that has it. */
#define PORT_MP_WEAPONSET_CUSTOM 14
#define PORT_MP_CATALOG_MAX 32

static struct s_mp_weapon_set mp_weapon_set_custom[8];
static struct s_mp_weapon_set port_mp_catalog[PORT_MP_CATALOG_MAX];
static s32 port_mp_catalog_n = -1;

/* Every distinct weapon in Rare's sets, sorted in the game's weapon (ITEM_IDS) order.
 * Each keeps the first slot Rare gave it, which is what a custom slot copies. */
static void portMpWeaponCatalogBuild(void)
{
    const s32 nsets = (s32) (sizeof(mp_weapon_set_text_table) / sizeof(mp_weapon_set_text_table[0]));
    s32 set, slot, k;

    port_mp_catalog_n = 0;
    for (set = 0; set < nsets; set++)
    {
        for (slot = 0; slot < 8; slot++)
        {
            const struct s_mp_weapon_set *w = &mp_weapon_set_text_table[set].weapon_set[slot];

            for (k = 0; k < port_mp_catalog_n && port_mp_catalog[k].itemID != w->itemID; k++)
            {
            }
            if (k == port_mp_catalog_n && port_mp_catalog_n < PORT_MP_CATALOG_MAX)
            {
                port_mp_catalog[port_mp_catalog_n++] = *w;
            }
        }
    }
    for (set = 1; set < port_mp_catalog_n; set++)
    {   /* insertion sort by item id; stable, and the first-seen slot is already kept */
        const struct s_mp_weapon_set w = port_mp_catalog[set];

        for (k = set; k > 0 && port_mp_catalog[k - 1].itemID > w.itemID; k--)
        {
            port_mp_catalog[k] = port_mp_catalog[k - 1];
        }
        port_mp_catalog[k] = w;
    }
}

s32 portMpWeaponCatalogCount(void)
{
    if (port_mp_catalog_n < 0)
    {
        portMpWeaponCatalogBuild();
    }
    return port_mp_catalog_n;
}

/* the ITEM_IDS of catalog entry k */
s32 portMpWeaponCatalogItem(s32 k)
{
    return (k >= 0 && k < portMpWeaponCatalogCount()) ? (s32) port_mp_catalog[k].itemID : ITEM_UNARMED;
}

/* where `item` sits in the catalog, -1 = not one Rare's sets use */
s32 portMpWeaponCatalogIndex(s32 item)
{
    s32 k;

    for (k = 0; k < portMpWeaponCatalogCount(); k++)
    {
        if ((s32) port_mp_catalog[k].itemID == item)
        {
            return k;
        }
    }
    return -1;
}

/* the eight weapons of built-in set `set` (a New Set starts from the set in force) */
void portMpWeaponSetItems(s32 set, s32 items[8])
{
    const struct s_mp_weapon_set *src = (set == PORT_MP_WEAPONSET_CUSTOM) ? mp_weapon_set_custom
                                      : mp_weapon_set_text_table[(set >= 0 && set < PORT_MP_WEAPONSET_CUSTOM) ? set : 0].weapon_set;
    s32 slot;

    for (slot = 0; slot < 8; slot++)
    {
        items[slot] = (s32) src[slot].itemID;
    }
}

/* fill the custom slots from eight weapons and make it the set in force;
 * a weapon not in the catalog becomes Slappers (a hand-edited file) */
void portMpWeaponCustomApply(const s32 items[8])
{
    s32 slot;

    for (slot = 0; slot < 8; slot++)
    {
        s32 k = portMpWeaponCatalogIndex(items[slot]);

        if (k < 0)
        {
            k = portMpWeaponCatalogIndex(ITEM_UNARMED);
        }
        mp_weapon_set_custom[slot] = port_mp_catalog[(k >= 0) ? k : 0];
    }
    mp_weapon_set = PORT_MP_WEAPONSET_CUSTOM;
}

s32 portMpWeaponIsCustom(void)
{
    return mp_weapon_set == PORT_MP_WEAPONSET_CUSTOM;
}

/* Random and Random Set weapon options. Both are rolled once per match into
 * mp_weapon_set_roll by portMpWeaponRoll (called from lvInit before
 * proplvreset2 places the weapon pads), using a private RNG so demos and seeded
 * runs are unaffected.
 *   Random      8 different weapons from the catalog (no Slappers), in the
 *               game's weapon order (so spawn-with-weapon gives slot 1, the weakest)
 *   Random Set  one of Rare's fourteen sets or a saved custom set, equally likely */
#define PORT_MP_WEAPONSET_RANDOM  15
#define PORT_MP_WEAPONSET_RANDSET 16

static struct s_mp_weapon_set mp_weapon_set_roll[8];

/* any port set (custom, random, random set): index 14 and up */
s32 portMpWeaponIsPortSet(void)
{
    return mp_weapon_set >= PORT_MP_WEAPONSET_CUSTOM;
}

static u32 portMpWeaponRand(void)
{
    extern unsigned long long portProfNow(void);   /* pcsched.c */
    static u32 state;

    if (state == 0)
    {
        state = (u32) portProfNow() | 1u;
    }
    state ^= state << 13;   /* xorshift32 */
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void portMpWeaponRoll(void)
{
    extern void portConLogf(const char *fmt, ...);

    if (mp_weapon_set == PORT_MP_WEAPONSET_RANDOM)
    {
        s32 pool[PORT_MP_CATALOG_MAX];
        s32 n = 0;
        s32 k, j;

        for (k = 0; k < portMpWeaponCatalogCount(); k++)
        {
            if (port_mp_catalog[k].itemID != ITEM_UNARMED)
            {
                pool[n++] = k;
            }
        }
        for (k = 0; k < 8 && k < n; k++)
        {   /* partial Fisher-Yates: the first eight of a shuffle, no repeats */
            const s32 r = k + (s32) (portMpWeaponRand() % (u32) (n - k));
            const s32 t = pool[k];

            pool[k] = pool[r];
            pool[r] = t;
        }
        for (k = 1; k < 8; k++)
        {   /* the game's order (the catalog is sorted by item) */
            const s32 t = pool[k];

            for (j = k; j > 0 && pool[j - 1] > t; j--)
            {
                pool[j] = pool[j - 1];
            }
            pool[j] = t;
        }
        for (k = 0; k < 8; k++)
        {
            mp_weapon_set_roll[k] = port_mp_catalog[pool[(k < n) ? k : 0]];
        }
        portConLogf("ge: mp: random weapons rolled\n");
    }
    else if (mp_weapon_set == PORT_MP_WEAPONSET_RANDSET)
    {
        extern s32 portWsetCount(void);   /* port_wsets.c */
        extern const char *portWsetName(s32 i);
        extern s32 portWsetLoad(const char *name, s32 items[8]);
        const s32 nbuilt = PORT_MP_WEAPONSET_CUSTOM;   /* Rare's fourteen */
        const s32 r = (s32) (portMpWeaponRand() % (u32) (nbuilt + portWsetCount()));
        s32 slot;

        portMpWeaponCatalogCount();
        if (r < nbuilt)
        {
            for (slot = 0; slot < 8; slot++)
            {
                mp_weapon_set_roll[slot] = mp_weapon_set_text_table[r].weapon_set[slot];
            }
            portConLogf("ge: mp: random set rolled Rare's set %d\n", (int) r);
        }
        else
        {
            s32 items[8];

            portWsetLoad(portWsetName(r - nbuilt), items);
            for (slot = 0; slot < 8; slot++)
            {
                s32 k = portMpWeaponCatalogIndex(items[slot]);

                if (k < 0)
                {
                    k = portMpWeaponCatalogIndex(ITEM_UNARMED);
                }
                mp_weapon_set_roll[slot] = port_mp_catalog[(k >= 0) ? k : 0];
            }
            portConLogf("ge: mp: random set rolled custom set %s\n", portWsetName(r - nbuilt));
        }
    }
}
#endif

//return pointer to selected mp_weapon_set textID
u16* getPtrMPWeaponSetTextID(void)
{
#ifndef TARGET_N64
    if (mp_weapon_set >= PORT_MP_WEAPONSET_CUSTOM)
    {   /* front.c draws a port set's NAME itself; this is only a fallback */
        return &mp_weapon_set_text_table[0].textID;
    }
#endif
    return &mp_weapon_set_text_table[mp_weapon_set].textID;
}

//return pointer to selected mp_weapon_set data
struct s_mp_weapon_set* getPtrMPWeaponSetData(void)
{
#ifndef TARGET_N64
    if (mp_weapon_set == PORT_MP_WEAPONSET_CUSTOM)
    {
        return mp_weapon_set_custom;
    }
    if (mp_weapon_set == PORT_MP_WEAPONSET_RANDOM || mp_weapon_set == PORT_MP_WEAPONSET_RANDSET)
    {
        return mp_weapon_set_roll;   /* this match's roll (portMpWeaponRoll) */
    }
#endif
    return mp_weapon_set_text_table[mp_weapon_set].weapon_set;
}

#ifndef TARGET_N64
/* PORT: the Weapons list screen (front.c, menu 13's pick mode) names every
 * set, not only the selected one. */
s32 portMpWeaponSetCount(void)
{
    return (s32) (sizeof(mp_weapon_set_text_table) / sizeof(mp_weapon_set_text_table[0]));
}

u16 portMpWeaponSetTextID(s32 set)
{
    return mp_weapon_set_text_table[set].textID;
}
#endif

//set mp weapon set
void setMPWeaponSet(s32 setNUM)
{
    mp_weapon_set = setNUM;
}

//return mp weapon set
s32 getMPWeaponSet(void)
{
    return mp_weapon_set;
}
