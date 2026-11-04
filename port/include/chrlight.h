/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/chrlight.h (55a3a9c8;
 * added 86cdec2c), Midas round 12 (XBLA step 3, docs/port-ios/21). Needs Light_t
 * (PR/gbi.h) in scope. */
#ifndef PORT_CHRLIGHT_H
#define PORT_CHRLIGHT_H

/* Round 32c (D-300..D-302): lit HD characters. A port-only command around an
 * HD character's lists (beanchr.c writeList() emits it; GoldenEye's own lists
 * never contain it):
 *
 *   w0 = G_CHRLIGHT_EXT << 24 | 1, w1 = host pointer to a GfxChrLight: when
 *        its `on` is set, fast3d saves the current lights and the G_LIGHTING
 *        bit, loads these lights and sets G_LIGHTING (the vertices then carry
 *        their normal in Vtx.n, their alpha - GoldenEye's blood - as before);
 *   w0 = G_CHRLIGHT_EXT << 24 | 0: restores what the first saved.
 *
 * With `on` clear both are no-ops and the list draws exactly as an unlit one.
 * The lights are F3D's: directions in the space after the modelview (the
 * view: GoldenEye's modelview holds the camera), each vertex's colour =
 * ambient + sum of max(0, n . dir) * colour, clamped (gfx_sp_vertex). Saving
 * and restoring keeps every other lit draw (the glass shards, the shiny guns'
 * envmap light) exactly as before. */
#define G_CHRLIGHT_EXT 0x4f

typedef struct GfxChrLight {
    unsigned char on;          /* Video.LitCharacters as the vertices hold it */
    unsigned char numdir;      /* directional lights in dir[] (1..2) */
    unsigned char pad[2];
    Light_t dir[2];
    Light_t amb;               /* col = the ambient (dir unused) */
} GfxChrLight;

#endif
