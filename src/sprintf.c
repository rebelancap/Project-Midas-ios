#include <stdarg.h>
#include <ultra64.h>
/*#include <stddef.h>*/



char *proutSprintf(char *dst, const char *src, size_t count) {
    return (char *) memcpy((u8 *) dst, (u8 *) src, count) + count;
}

int sprintf(char *dst, const char *fmt, ...) {
    s32 written;
    va_list args;
    va_start(args, fmt);
    /* The donor's PORT arm passed &args (its xprintf.c takes a va_list *).
     * Midas builds port/src/printf_port.c instead, whose _Printf takes the
     * va_list itself: one form on every build. */
    written = _Printf(proutSprintf, dst, fmt, args);
    if (written >= 0) {
        dst[written] = 0;
    }
    return written-1;
}




