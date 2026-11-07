/* Midas round 61 (docs/port-ios/81-round61-xbla-sky-domes.md, GitHub issue #1).
 * Ported from Perfect Dark's gebeansky.c (perfect-dark-ios vendor/dabs-mod
 * port/include/gebeansky.h, MIT): the release-dome path only. 64-bit host
 * layer only. */
#ifndef PORT_BEANSKY_H
#define PORT_BEANSKY_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GoldenEye XBLA's sky over a level served in HD (beanstage.c): a dome round
 * the eye with a cloud cap over it, the release's files/new/skydome/<name>.
 * The release's eleven sky files are one placeholder picture, a blue sky
 * over Surface's peaks, so only the three levels whose XBLA backdrop is that
 * daylit panorama take it: Surface 1 (sevx), Dam and Runway. Everywhere else
 * the level keeps GoldenEye's sky (no Community Edition here).
 *
 * beanSkyWanted() builds the level's dome on first use and says whether
 * skyRender() should draw it in place of GoldenEye's sky plane (Video.XblaModels
 * on, the level's backdrop drawn, the file imported). beanSkyRender() draws
 * it: the view's rotation only, no depth test. The caller fills the view with
 * the level's sky colour first and draws the level's water after it.
 */
int beanSkyWanted(void);
Gfx *beanSkyRender(Gfx *gdl);

/* A new level (beanStageLevelLoaded()): the last level's dome is retired. */
void beanSkyLevelReset(void);

#ifdef __cplusplus
}
#endif

#endif
