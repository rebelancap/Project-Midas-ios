/* Imported from GoldenEye-Decomp-ios engine/port/include/pcconvert.h at 55a3a9c8
 * (donor HEAD), Phase 1 round 7a (docs/port-ios/11-round7a-pcconvert.md).
 * Midas additions: pcconvertEnsureImage, pcconvertSetLog,
 * pcconvertRegionForCountry. Lives in port/apple/pcconvert/ (64-bit only). */
#ifndef PORT_PCCONVERT_H
#define PORT_PCCONVERT_H

#include <stddef.h>
#include <stdint.h>

/*
 * First-run sidecar conversion, natively in C — see port/apple/pcconvert/.
 * Called (round 7b) from romdata.c as soon as the ROM is loaded and BEFORE
 * pcmodelsReserveSize()/pccgReserveSize() read the sidecars.
 *
 * romPath: a resolved absolute/relative path to the user's ROM (.z64 order).
 * dataDir: where the sidecar trees live (desktop: <exe dir>/Data; iOS:
 *          Documents/Data -- regenerable ROM-derived data, never shipped).
 *
 * Returns 1 when both sidecar trees are present (already, or after a
 * successful conversion), -1 when they could not be produced.
 */
#ifdef __cplusplus
extern "C" {
#endif

int pcconvertEnsure(const char *romPath, const char *dataDir);

/* Same, from an in-memory z64-order cart image (romdata.c's normalised copy). */
int pcconvertEnsureImage(const uint8_t *rom, size_t romLen, const char *dataDir);

/* Where pcconvert's log lines go (one line, no trailing newline). NULL =
 * stderr. The game passes a portConLogf wrapper. */
void pcconvertSetLog(void (*sink)(const char *line));

/* "ntsc-final" / "pal-final" / "jpn-final" from the header country byte
 * (rom[0x3E]), or NULL. Names the pcmodels-<region>/pccg-<region> dirs. */
const char *pcconvertRegionForCountry(unsigned char country);

/* `ProjectMidas --pcconvert <rom> <outdir>`: run the converter and exit.
 * Returns -1 when argv is not a converter invocation (normal boot continues). */
int pcconvertCliMain(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PCCONVERT_H */
