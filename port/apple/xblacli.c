/*
 * xblacli.c - driving the XBLA importer without a UI (round 10, XBLA step 1,
 * docs/port-ios/19-round10-xbla-import.md).
 *
 * Two entries, both reached through main.c's existing pre-boot hook
 * (pcconvertCliMain, port/apple/pcconvert/pcconvert.c, which calls
 * xblaCliMain first; a weak no-op there keeps the pcconvert host test
 * linking without this file):
 *
 *   ProjectMidas --xbla-import <src> [<cachedir>]
 *       Imports <src> (the Xbox 360 package, a .7z holding it, or the
 *       unpacked release folder; or a folder holding one of those, up to
 *       four levels down: xblaImportFind) into <cachedir>/xbla, synchronously, prints
 *       the result and exits 0 (imported) / 1 (not). <cachedir> defaults to
 *       the executable's directory, which is $C on the desktop.
 *
 *   GE_XBLA_IMPORT=<src> ProjectMidas ...
 *       The same import at startup, before anything else boots, then the run
 *       carries on as usual. Every launch with the variable set imports again
 *       (a dev knob; the shell's importer is step 5). GE_XBLA_CACHE=<dir>
 *       puts the layer in <dir>/xbla instead of the executable's directory.
 *
 * Without either, this returns -1 at once and touches nothing: no file is
 * opened, no key registered, no state written.
 *
 * Both run before main.c's portInitRootDir, so $C is pinned explicitly
 * (sysSetCacheRoot, hostpaths.c) for the import and released after it. On
 * iOS $C is the shell's GE_IOS_CACHES and is not pinned.
 *
 * Host memory only; the importer never sees the sim's allocators.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath. NB R9A-1: this defines PLATFORM_IOS; nothing here tests it. */
#endif
#include "xblaimport.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "xblacli.c must not see the sim's allocators (mema.h/memp.h): host memory only"
#endif

extern void sysSetCacheRoot(const char *dir);                 /* hostpaths.c */
extern const char *sysResolvePath(const char *path);          /* hostpaths.c */
extern void xblaKeysDump(void (*logf)(const char *fmt, ...)); /* xbla_keys.c */

static void xblaCliLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void xblaCliLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* The executable's directory, without the trailing slash. 0 if unknown. */
static int xblaCliExeDir(char *out, size_t len)
{
#if defined(__APPLE__) && !defined(GE_IOS)
    char exe[1024];
    char real[1024];
    uint32_t sz = sizeof(exe);
    const char *use;
    char *slash;

    if (_NSGetExecutablePath(exe, &sz) != 0)
    {
        return 0;
    }
    use = realpath(exe, real) != NULL ? real : exe;
    if ((size_t)snprintf(out, len, "%s", use) >= len)
    {
        return 0;
    }
    slash = strrchr(out, '/');
    if (slash == NULL)
    {
        return 0;
    }
    *slash = '\0';
    return 1;
#else
    (void)out;
    (void)len;
    return 0;
#endif
}

static int xblaCliImport(const char *src, const char *cacheDir)
{
    char dir[1024];
    int done = 0, total = 0, ok;

    if (cacheDir != NULL && cacheDir[0] != '\0')
    {
        snprintf(dir, sizeof(dir), "%s", cacheDir);
        mkdir(dir, 0755);
        sysSetCacheRoot(dir);
    }
    else if (xblaCliExeDir(dir, sizeof(dir)))
    {
        sysSetCacheRoot(dir);
    }
    /* else (iOS): $C is GE_IOS_CACHES already */
    xblaCliLog("ge: xbla: layer dir %s\n", sysResolvePath("$C/xbla"));
    xblaKeysDump(xblaCliLog);
    {
        /* A folder that is not itself the release but holds a copy (the
         * download's own 0000000000000000/... tree, a Downloads folder): the
         * first copy under it, as the shell's Documents scan finds one. */
        static char found[1024];
        struct stat st;
        if (xblaImportKindOf(src) == 0 && stat(src, &st) == 0 && S_ISDIR(st.st_mode)
            && xblaImportFind(src, found, (int)sizeof(found)))
        {
            xblaCliLog("ge: xbla: found %s\n", found);
            src = found;
        }
    }
    ok = xblaImportRun(src);
    xblaImportProgress(&done, &total);
    xblaCliLog("ge: xbla: %s: %s (state %d, %d/%d files read; map %d HD + %d same-size)\n",
               ok ? "OK" : "FAILED", xblaImportMessage(), xblaImportState(), done, total,
               xblaMapNumHd(), xblaMapNumSame());
    sysSetCacheRoot(NULL);
    return ok;
}

int xblaCliMain(int argc, char **argv)
{
    const char *env;
    int i;

    for (i = 1; i + 1 < argc; i++)
    {
        if (strcmp(argv[i], "--xbla-import") == 0)
        {
            const char *cache = (i + 2 < argc && argv[i + 2][0] != '-') ? argv[i + 2] : NULL;
            return xblaCliImport(argv[i + 1], cache) ? 0 : 1;
        }
    }
    env = getenv("GE_XBLA_IMPORT");
    if (env != NULL && env[0] != '\0')
    {
        (void)xblaCliImport(env, getenv("GE_XBLA_CACHE"));
    }
    return -1; /* not an importer invocation: boot as usual */
}
