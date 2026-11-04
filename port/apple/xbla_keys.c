/*
 * xbla_keys.c - the CLI's one-line report of the XBLA keys.
 *
 * Round 10 (docs/port-ios/19-round10-xbla-import.md) wrote this file to hold
 * the texpack and beanstage keys until their real files linked. Round 11
 * removed the texpack half when port/apple/texpack.c landed
 * (docs/port-ios/20-round11-texpack.md); round 13 removed the beanstage half
 * (Video.XblaModels, Video.XblaEveryRoom, beanStageCachePrune) when
 * port/apple/beanstage.c landed (docs/port-ios/22-round13-rooms.md):
 * beanstage.c registers those keys itself, as in the donor.
 *
 * What is left is xblaKeysDump, which xblacli.c prints after an import.
 * Nothing here opens a file. Host statics only, never sim state.
 */
#include <stdio.h>
#include "platform.h"
#include "texpack.h"
#include "beanstage.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "xbla_keys.c must not see the sim's allocators (mema.h/memp.h): host state only"
#endif

/* For the CLI's report: the switches as they stand (the defaults until main.c's
 * configLoad() has read midas.ini; the CLI runs before it). */
void xblaKeysDump(void (*logf)(const char *fmt, ...))
{
    logf("ge: xbla: keys Video.XblaTextures=%d Video.XblaModels=%d "
         "Video.TexturePrewarm=%d Video.TexturePack=\"%s\" Video.XblaPath=\"%s\"\n",
         texpackXblaEnabled(), beanStageModelsEnabled(), texpackPicturePrewarm(),
         texpackGetSelected(), texpackXblaPath());
}
