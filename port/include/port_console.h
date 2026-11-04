/**
 * port_console.h - the tilde console, modelled on the Perfect Dark port's but
 * with no SDL dependency: input_pc.c feeds it keys, so headless and no-video
 * builds still link it and collect the log.
 */
#ifndef PORT_CONSOLE_H
#define PORT_CONSOLE_H

#ifndef TARGET_N64

#include <ultra64.h>

/* Keys the console consumes. input_pc.c translates SDL scancodes into these
 * so no SDL header is needed here. */
enum PortConKey {
    PORTCON_KEY_ENTER = 1,
    PORTCON_KEY_BACKSPACE,
    PORTCON_KEY_PAGEUP,
    PORTCON_KEY_PAGEDOWN,
    PORTCON_KEY_HOME,
    PORTCON_KEY_END,
    PORTCON_KEY_ESCAPE,
    PORTCON_KEY_UP,        /* command history */
    PORTCON_KEY_DOWN
};

void portConInit(void);

/* showmsg: also flash the line in the 4-row overlay above the game. portConLogf also writes
 * to stderr, so it is a drop-in replacement for the port's `fprintf(stderr, "ge: ...")` lines. */
void portConPrint(s32 showmsg, const char *str);
void portConPrintf(s32 showmsg, const char *fmt, ...);
void portConLogf(const char *fmt, ...);

Gfx *portConRender(Gfx *gdl);

s32  portConIsOpen(void);
void portConToggle(void);
void portConClose(void);
void portConKey(s32 key);      /* enum PortConKey */
void portConTextChar(s32 ch);  /* one printable ASCII byte */

#endif /* TARGET_N64 */
#endif /* PORT_CONSOLE_H */
