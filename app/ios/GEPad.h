// GEPad.h — the N64 controller bits, and the shell's single publish point.
//
// Everything in the shell that produces input (GEController, GETouchOverlay,
// the bridge's injection) OR's its contribution into one GEPad and hands it to
// GEInput, which publishes the merged result to the engine exactly once per
// UI update through geHostPadSet(). One pad, one seam, one place to look when
// a button does the wrong thing (port/include/hostinput.h, charter rule 3b).
#pragma once

#include <stdint.h>

// From PR/os.h. Duplicated (not included) for the same reason port/src/input.c
// duplicates them: os.h declares `u8 errno;` inside OSContPad, which the libc
// <errno.h> macro rewrites, and this is an ObjC translation unit.
#define GE_CONT_A      0x8000
#define GE_CONT_B      0x4000
#define GE_CONT_G      0x2000  /* Z trigger  — FIRE            */
#define GE_CONT_START  0x1000
#define GE_CONT_UP     0x0800  /* D-pad */
#define GE_CONT_DOWN   0x0400
#define GE_CONT_LEFT   0x0200
#define GE_CONT_RIGHT  0x0100
#define GE_CONT_L      0x0020
#define GE_CONT_R      0x0010  /* R trigger — AIM              */
#define GE_CONT_E      0x0008  /* C-up      — look up / step fwd  */
#define GE_CONT_D      0x0004  /* C-down    — look down / step back */
#define GE_CONT_C      0x0002  /* C-left    — strafe left      */
#define GE_CONT_F      0x0001  /* C-right   — strafe right     */

// The N64 stick's usable range. The console's own hardware reached about ±80
// and the game's dead-zone/response tables are written against that, so 80 is
// full deflection here too — not 127.
#define GE_STICK_MAX   80

typedef struct {
	uint16_t button;
	int8_t   stickX;
	int8_t   stickY;
} GEPad;

// Absolute look for this update, in degrees (+x right, +y down). Not part of
// the pad: see port/include/hostinput.h for why degrees and not stick counts.
typedef struct {
	float yawDeg;
	float pitchDeg;
} GELook;
