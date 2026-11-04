/* pcsidecar.h - Midas glue between romdata.c and the pcconvert sidecars
 * (64-bit hosts only; Phase 1 round 7a, docs/port-ios/11-round7a-pcconvert.md).
 * The serving API itself is the donor's port/include/pcmodels.h + pccg.h. */
#ifndef PORT_PCSIDECAR_H
#define PORT_PCSIDECAR_H

#include <stdint.h>
#include "pcconvert.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Directory holding pcconvert.stamp and the pcmodels-<region>/pccg-<region>
 * trees (desktop: portRootPath("Data"); iOS: Documents/Data). Call before
 * pcsidecarPrepare / *ReserveSize. */
void pcsidecarSetDataDir(const char *dataDir);

/* First-run conversion from romdata.c's validated in-memory cart image:
 * 1 = sidecars present, -1 = could not produce them (abort boot). */
int pcsidecarPrepare(const uint8_t *rom, uint32_t romSize);

/* true when [p, p+len) is inside a loaded sidecar image (cart-range checks) */
int pcsidecarContains(const void *p, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PCSIDECAR_H */
