#ifndef _ULTRA64_H_
#define _ULTRA64_H_

#include <math.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#include <PR/ultratypes.h>
#include <PR/os.h>

#include <PR/gu.h>
#include <PR/mbi.h>
#include <PR/libaudio.h>
#include <PR/libultra.h>
#include <PR/sptask.h>
#include <PR/ucode.h>

#if defined(PORT) && !defined(__cplusplus)
/* 64-bit host only: prototypes (u8..f64/Gfx-typed) for functions the game
 * calls without a header in scope (docs/port-ios/08-round5-census.md); the
 * game-typed half is anchored at the end of src/bondtypes.h. */
#include "port_protos_base.h"
#endif

#endif
