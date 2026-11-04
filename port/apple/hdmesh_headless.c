/*
 * hdmesh_headless.c - the three fast3d entry points beanstage.c calls
 * (port/include/hdmesh.h), for the headless build only (GE_VIDEO=OFF has no
 * gfx_pc.cpp). Round 13, docs/port-ios/22-round13-rooms.md.
 *
 * Headless never renders, so bgLevelRender never runs beanStageFrame and
 * nothing is built; these exist so beanstage.c links. In the video build
 * gfx_pc.cpp defines them (round 12) and CMake drops this file.
 */
#include <stddef.h>
#include "hdmesh.h"

int gfx_hdmesh_enabled = 1;
int gfx_hdmesh_gpu = 0; /* round 29: Video.XblaMeshGpu's variable (never read headless) */
void gfxHdMeshQueueUpload(struct GfxHdMesh *m) { (void) m; }
void gfxHdMeshRelease(struct GfxHdMesh *m) { (void) m; }
