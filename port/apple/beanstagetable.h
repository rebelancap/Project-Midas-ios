/* Imported verbatim from GoldenEye-Decomp-ios engine/port/src/beanstagetable.h (last changed d923b829; donor HEAD 55a3a9c8),
 * round 10, XBLA step 1 (docs/port-ios/19-round10-xbla-import.md). 64-bit host layer only. */
/*
 * Tier C level pairing (round C1, D-110). Generated from Perfect Dark's
 * gebeanstagetable.h (perfect-dark-ios, tools/geconvert/fit) and this
 * engine's levelinfotable[] (src/game/bg.c): numbers and names only.
 *
 * { GoldenEye's level key (bg/bg_<key>_all_p.seg), Bean background name,
 *   Bean units -> GoldenEye bg units, verified }
 *
 * scale = PD's row scale (Bean -> GoldenEye X file units) x levelscale
 * (GoldenEye X file units are GoldenEye's bg units / levelscale). PD's
 * offsets are its converter's recentring and are 0 here. A row is
 * `verified` once an overlay probe (beanstage.c, "probe:" log line) has
 * measured it; beanStage builds only verified rows unless
 * GE_BEANSTAGE_ANY=1. Dam: probe residual in MEASUREMENTS M-050; every
 * other row: round 28a (C5, D-165, M-084) - the least-squares scale of the
 * matched pairs equals the row to 5-6 digits and the mean offset is under
 * 0.08 units on every level. `verified` 2 = probed but left N64 by the
 * round's look (see the row's comment; beanstage.c builds only 1).
 */
    { "arch", "archives", 0.5434775f, 1 }, /* PD 1.072400836 x levelscale 0.50678575 */
    { "ame", "library", 0.3787788f, 0 }, /* PD 0.573907333 x levelscale 0.65999997 (Library, Basement, Stack) */
    { "arec", "control", 0.8064567f, 1 }, /* PD 1.616580623 x levelscale 0.49886572 */
    { "ark", "facility", 0.6410249f, 1 }, /* PD 0.531318270 x levelscale 1.20648 */
    { "azt", "aztec", 0.5208272f, 1 }, /* PD 1.475407422 x levelscale 0.35300568 */
    { "cave", "cavern", 0.1344092f, 1 }, /* PD 0.501072787 x levelscale 0.26824287 */
    { "crad", "cradle", 0.4310432f, 1 }, /* PD 1.828668257 x levelscale 0.23571429 */
    { "cryp", "temple", 0.2358523f, 1 }, /* PD 0.921010121 x levelscale 0.25608 */
    { "dam", "dam", 1.0000028f, 1 }, /* PD 4.280101153 x levelscale 0.23363999 */
    { "depo", "depot", 0.1760548f, 1 }, /* PD 0.805820509 x levelscale 0.21847887 */
    { "dest", "frigate", 0.3676527f, 1 }, /* PD 0.821433927 x levelscale 0.44757429 */
    { "dish", "multitemple", 1.4705793f, 0 }, /* PD 3.119410587 x levelscale 0.47142857 */
    { "jun", "jungle", 0.0668459f, 1 }, /* PD 0.706146978 x levelscale 0.094662853 */
    { "oat", "caves", 0.1012346f, 0 }, /* PD 0.715800060 x levelscale 0.14142857 */
    { "pete", "streets", 0.4237299f, 1 }, /* PD 1.239411313 x levelscale 0.34187999 */
    { "ref", "complex", 0.3787843f, 0 }, /* PD 0.401740938 x levelscale 0.94285715 */
    { "run", "runway", 0.0455366f, 1 }, /* PD 0.508383572 x levelscale 0.089571431 */
    { "sev", "bunker", 0.2314813f, 1 }, /* PD 0.429214082 x levelscale 0.53931433 */
    { "sevb", "bunker2", 0.2314817f, 1 }, /* PD 0.429214887 x levelscale 0.53931433 */
    { "sevx", "surface", 0.2260403f, 1 }, /* PD 0.497385395 x levelscale 0.45445713 */
    { "silo", "silo", 0.2906909f, 1 }, /* PD 0.615140709 x levelscale 0.47256002 */
    { "stat", "statuepark", 0.0008010f, 1 }, /* PD 0.007471857 x levelscale 0.107202865 */
    { "tra", "train", 0.1953121f, 1 }, /* PD 1.300371514 x levelscale 0.15019713 */
