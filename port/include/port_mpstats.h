/**
 * port_mpstats.h - lifetime multiplayer stats per player profile, saved to
 * Profiles/<NAME>.gestats and shown on the Player Options Stats page (see
 * docs/port/mp-stats-tracker.md).
 */
#ifndef PORT_MPSTATS_H
#define PORT_MPSTATS_H

#ifndef TARGET_N64

#include <ultra64.h>

/* One seat's share of one match. Kills leave out suicides; deaths count
 * them, as Rare's endscreen metrics do (mpmenu.c mpCalculateAwards). */
struct PortMpMatchLine
{
    s32 won;            /* ranked 1st (ties and a winning team included) */
    s32 kills;
    s32 deaths;
    s32 suicides;
    s32 shots;          /* trigger pulls (SHOT_REGISTER_TOTAL) */
    s32 head;           /* hits, per pellet */
    s32 body;
    s32 limb;
    s32 other;          /* gun + hat hits */
    s32 streak;         /* most kills in one life */
    s32 longestlife60;  /* longest life, 1/60 s */
    u32 awards;         /* every AWARD_* bit earned this match (before the endscreen's random two) */
};

#define PORT_MPSTAT_AWARDS 17   /* AWARD_MOSTSUICIDAL (bit 0) .. AWARD_QUADKILL (bit 16) */

/* THE FILE'S KEYS, in file order. Append-only: an index is a key name. */
enum PortMpStat
{
    PORT_MPSTAT_MATCHES,
    PORT_MPSTAT_WINS,
    PORT_MPSTAT_ABANDONED,
    PORT_MPSTAT_SECONDS,
    PORT_MPSTAT_KILLS,
    PORT_MPSTAT_DEATHS,
    PORT_MPSTAT_SUICIDES,
    PORT_MPSTAT_SHOTS,
    PORT_MPSTAT_HEAD,
    PORT_MPSTAT_BODY,
    PORT_MPSTAT_LIMB,
    PORT_MPSTAT_OTHER,
    PORT_MPSTAT_STREAK,         /* best, not a sum */
    PORT_MPSTAT_LONGESTLIFE,    /* best, seconds */
    /* version 2: damage and awards */
    PORT_MPSTAT_DMG_DEALT,      /* to other players; 100 = one full bar (health + armour) */
    PORT_MPSTAT_DMG_TAKEN,      /* from anyone, gas and yourself included */
    PORT_MPSTAT_AWARD0,         /* + k: times AWARD bit k was earned (PORT_MPSTAT_AWARDS of them) */
    PORT_MPSTAT_COUNT = PORT_MPSTAT_AWARD0 + PORT_MPSTAT_AWARDS
};

/* lv.c lvInit: a new stage - the next match end may record */
void portMpStatsStageStart(void);
/* mpmenu.c: the pause menu's Exit was confirmed - this match is abandoned */
void portMpStatsAbandoned(void);
/* bondview2.c record_damage_kills: `bars` (1.0 = a full health or armour bar)
 * came off player `victim`, dealt by `attacker` (-1 = nobody: gas, the level) */
void portMpStatsDamage(s32 victim, s32 attacker, f32 bars);
/* mpmenu.c mpCalculateAwards: lines[seat] for seats 0..count-1 */
void portMpStatsRecord(const struct PortMpMatchLine *lines, s32 count, s32 seconds);
/* front.c's Stats page: NAME's totals, all zero when it has none yet */
void portMpStatsLoad(const char *name, s32 out[PORT_MPSTAT_COUNT]);

#endif /* !TARGET_N64 */
#endif /* PORT_MPSTATS_H */
