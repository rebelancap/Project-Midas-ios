/**
 * port_watchpages.h - what the watch's Game Options tab shows after the menu
 * overhaul (docs/port/menu-overhaul-spec.md sections 2, 3 and 12.2).
 *
 * The folder's Options tree lives behind the watch's two option tabs (see the
 * PortWatchPage enum below). port_watchpages.c owns the row tables, the preset
 * definitions and every value lookup; src/game/options.c owns the drawing,
 * the highlight and the input. Nothing here touches a display list or SDL,
 * so it builds in every configuration.
 */
#ifndef PORT_WATCHPAGES_H
#define PORT_WATCHPAGES_H

#ifndef TARGET_N64

#include <ultra64.h>

/* The folder's Options layout, mirrored on the watch (same tree as front.c's
 * sheets):
 *
 *   Game Options tab   GENERAL (the face: volume bars, Rare's general
 *                                 settings + screen, the two pickers)
 *                        GAMEPLAY -> HUD / PORTOPTS
 *                        DISPLAY  -> VIDEO / AUDIO / GRAPHICS
 *   Control tab        CONTROLLER (input device, then three submenus)
 *                        BINDS (rebind inputs: bindings mode + the bind list)
 *                        CONTROLLER_MORE (mouse and keyboard, or controller)
 *                        CTLGENERAL (also what a raphnet adapter's third row opens)
 *
 * ORDER IS LOAD-BEARING: options.c tells the two tabs' pages apart with
 * `page >= PORTWATCH_CONTROLLER`, so every Game Options page goes BEFORE it
 * and every Control page after. The numbers are not saved anywhere, but the
 * GE_WATCH capture recipe's sub-page field is this number. */
enum PortWatchPage {
    PORTWATCH_GENERAL = 0,
    PORTWATCH_DISPLAY,          /* picker: video / audio / graphics */
    PORTWATCH_GAMEPLAY,         /* picker: general settings / hud / port options */
    PORTWATCH_HUD,
    PORTWATCH_PORTOPTS,
    PORTWATCH_VIDEO,
    PORTWATCH_AUDIO,
    PORTWATCH_GRAPHICS,
    PORTWATCH_GENSETTINGS,      /* the port's General Settings (Rare's four are on the face) */
    PORTWATCH_CONTROLLER,       /* the Control Options tab */
    PORTWATCH_CONTROLLER_MORE,  /* the device's own page: mouse and keyboard, or controller */
    PORTWATCH_CTLGENERAL,       /* general control options */
    PORTWATCH_BINDS,            /* rebind inputs: the bind list, a page in (as the folder has it) */
    PORTWATCH_PAGES
};

enum PortWatchRowKind {
    PWROW_OPT = 0,    /* a port_options.c row, addressed by ini key */
    PWROW_PLAYEROPT,  /* one of Rare's own game_options_entries[] rows */
    PWROW_PRESET,     /* Nintendo 64 / Modern / custom */
    PWROW_SUBSCREEN,  /* opens another page */
    PWROW_BACK,       /* returns to the page it was opened from */
    PWROW_DEVICE,     /* the Controller page's device picker */
    PWROW_BIND,       /* one remappable action, showing the selected device's keys */
    PWROW_HINT,       /* a line of text, not selectable */
    PWROW_BINDMODE,   /* Port bindings or N64 bindings: which list is shown */
    PWROW_RESOLUTION, /* the window / fullscreen size: stepped, applied on A */
    PWROW_XHAIR,      /* crosshair_color's named list (front.c portXhair*), live */
    PWROW_ACTION,     /* a button that does something on the page (e.g. Reset to Default) */
    PWROW_LAYOUT      /* the pad's bind config (input_pc.c layouts), as the folder's row */
};
s32 portWatchRowAction(s32 page, s32 row);

/* The watch grid shows EIGHT rows at a fixed pitch, a number hard-coded in
 * draw_toggle_options, the navigation and the mouse hit test. Longer pages (the
 * Controller page) scroll a window of PORTWATCH_VISROWS in options.c, keeping
 * the highlight inside. */
/* General shows eight, because the two volume bars own the top of the face.
 * A page with no bars starts at the top instead and fits eleven. */
#define PORTWATCH_VISROWS      11
/* General shows nine since the window-resolution row joined it: rows run
 * YOFFSET_1 .. 80 + 15*8 = 200, and the tab strip sits around 220. */
#define PORTWATCH_GENERAL_ROWS 9

s32         portWatchPageRows(s32 page);
s32         portWatchRowKind(s32 page, s32 row);
s32         portWatchRowPlayerOption(s32 page, s32 row); /* PWROW_PLAYEROPT */
s32         portWatchRowSubPage(s32 page, s32 row);      /* PWROW_SUBSCREEN */
s32         portWatchRowOpt(s32 page, s32 row);          /* option index, -1 */

/** Row label, already lowercase with the trailing newline the watch wants.
 *  NULL for a PWROW_PLAYEROPT row: the caller uses the vanilla string. */
const char *portWatchRowLabel(s32 page, s32 row);

/** 0 = the row has no value list and is stepped (fov, anisotropy, ...);
 *  otherwise the number of values, so 2 and 3 can keep the vanilla look. */
s32         portWatchRowValueCount(s32 page, s32 row);
const char *portWatchRowValueTextAt(s32 page, s32 row, s32 idx);
s32         portWatchRowValueIndex(s32 page, s32 row);   /* -1 = off-list */
const char *portWatchRowValueText(s32 page, s32 row);    /* the live value */
s32         portWatchRowAvailable(s32 page, s32 row);
/** 1 = classic mode is on and owns this row: draw it dim, give it no input. */
s32         portWatchRowLocked(s32 page, s32 row);

void        portWatchRowStep(s32 page, s32 row, s32 dir);
void        portWatchRowSetIndex(s32 page, s32 row, s32 idx);

s32         portWatchRowSelectable(s32 page, s32 row); /* a hint row is not */
s32         portWatchParentPage(s32 page);
/** A page was just opened: VIDEO repairs classic mode's forced rows if they drifted. */
void        portWatchPageEntered(s32 page);

/** 0 = neither preset matches ("custom"), 1 = Nintendo 64, 2 = Modern. */
s32         portWatchPresetActive(void);
void        portWatchPresetApply(s32 which);

/* The Controller pages' device picker. 0 = mouse and keyboard; 1.. = the
 * connected pads in slot order (see portInputDeviceSlot in input_pc.c - a
 * device index is a position in a list that shrinks when a pad is unplugged,
 * NOT a pad slot). */
s32         portWatchDevice(void);
void        portWatchDeviceStep(s32 dir);
/** Show this device: what the press-a-button assign landed on (options.c). */
void        portWatchDeviceSet(s32 dev);
void        portWatchDeviceValidate(void);
s32         portWatchDeviceNoneConnected(void);
s32         portWatchDeviceIsRaphnet(s32 dev);

/* Drop-down list: a row whose values won't fit across the face opens a vertical
 * list instead of stepping through < > arrows. Only portWatchDropChoose applies
 * a value; closing any other way applies nothing (see port_watchpages.c). */
/* Rebinding from the Controller tab: A / Z on a bind row arms a capture and the
 * next key or mouse button pressed becomes that binding. Mouse and keyboard
 * only - a pad bind still goes through the launcher. */
s32         portWatchBindCaptureBegin(s32 page, s32 row);  /* 0 = this row cannot */
s32         portWatchBindCaptureTick(void);                /* 1 = finished */
s32         portWatchBindCaptureActive(void);
s32         portWatchBindCapturing(s32 page, s32 row);
void        portWatchBindCaptureCancel(void);
s32         portWatchBindUnbind(s32 page, s32 row);

s32         portWatchDropIsOpen(void);
s32         portWatchDropIsVisible(void);
s32         portWatchDropAnimDone(void);
f32         portWatchDropAnimW(void);
f32         portWatchDropAnimH(void);
void        portWatchDropAnimTick(void);
void        portWatchDropCloseNow(void);
s32         portWatchDropPage(void);
s32         portWatchDropRow(void);
s32         portWatchDropIndex(void);
s32         portWatchDropScroll(void);
s32         portWatchDropCount(void);
s32         portWatchDropLiveIndex(void);
s32         portWatchDropOpenOn(s32 page, s32 row);
const char *portWatchDropTextAt(s32 i);
void        portWatchDropOpen(s32 page, s32 row);
void        portWatchDropClose(void);
void        portWatchDropMove(s32 dir);
void        portWatchDropSetIndex(s32 i);
void        portWatchDropSetScroll(s32 i);
void        portWatchDropSetTop(s32 y);
s32         portWatchDropTop(void);
void        portWatchDropScrollFollow(s32 vis);
void        portWatchDropScrollBy(s32 n, s32 vis);
void        portWatchDropChoose(void);

/* Rows with a PENDING choice (any PWROW_OPT whose table entry sets `defer`,
 * e.g. display mode, plus the resolution row below) only apply on Accept (A /
 * Z), and leaving the row discards the pending value. This means scrolling past
 * a mode the monitor cannot show never strands anyone in it. */
s32         portWatchRowDeferred(s32 page, s32 row);
void        portWatchDeferBegin(s32 page, s32 row);
void        portWatchDeferCommit(void);
void        portWatchDeferCancel(void);

void        portWatchResBegin(void);   /* entering the row: seed from the live size */
void        portWatchResCommit(void);  /* Accept: apply */
void        portWatchResCancel(void);  /* moved away: forget */
void        portWatchResStep(s32 dir);
const char *portWatchResText(void);

#endif /* TARGET_N64 */
#endif /* PORT_WATCHPAGES_H */
