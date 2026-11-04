/*
 * hostpaths.c - sysResolvePath, the "$S/" "$C/" "$E/" data roots the iOS shell
 * resolves paths with (round 9a, docs/port-ios/16-round9a-ios-engine.md).
 *
 * Shape and roots from GoldenEye-Decomp-ios port/src/system.c (D-016):
 *
 *   $S -> the save/data root: Documents on iOS (GE_IOS_DOCUMENTS, published by
 *         the shell before geEngineMain); Midas's exe-relative root
 *         (main.c portRootPath) elsewhere.
 *   $C -> regenerable cache data: Library/Caches on iOS (GE_IOS_CACHES); the
 *         same as $S elsewhere.
 *   $E -> the read-only bundle on iOS (GE_IOS_BUNDLE); the exe dir elsewhere.
 *
 * Midas itself does not call this: its own files go through portRootPath(),
 * which main.c points at Documents on iOS. The shell uses it for the paths it
 * shares with the engine (the ROM it drops in, the log it tails). Anything not
 * starting with one of the three prefixes is returned unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern const char *portRootPath(const char *name);

#ifdef GE_IOS
enum { GE_IOS_ROOT_DOCS = 0, GE_IOS_ROOT_CACHES = 1, GE_IOS_ROOT_COUNT };

static const char *sysIosRoot(int which, const char *var, const char *fallbackSuffix)
{
    static char slots[GE_IOS_ROOT_COUNT][512];
    char *slot = slots[which];
    const char *env;

    if (slot[0] != '\0')
    {
        return slot;
    }
    env = getenv(var);
    if (env != NULL && env[0] != '\0')
    {
        snprintf(slot, 512, "%s", env);
    }
    else
    {
        /* the shell always sets these; this only keeps a bare-libmidas
         * harness from writing into the cwd */
        const char *home = getenv("HOME");
        snprintf(slot, 512, "%s%s", home != NULL ? home : ".", fallbackSuffix);
    }
    mkdir(slot, 0755); /* best effort; EEXIST is fine */
    return slot;
}
#endif

/* Round 10 (doc 19): the XBLA import's command-line entry
 * (port/apple/xblacli.c) runs before main.c has set the exe-relative root,
 * so it names the $C root itself. NULL / "" restores the normal roots. */
static char sCacheRoot[1024];

void sysSetCacheRoot(const char *dir)
{
    snprintf(sCacheRoot, sizeof(sCacheRoot), "%s", dir != NULL ? dir : "");
}

/* Round 16 (R10-4): one result buffer PER THREAD. The XBLA import runs on
 * its own worker (xblaImportStart, started by the iOS settings page) and
 * resolves "$C/xbla/..." throughout, while the game and render threads
 * resolve their own paths; a shared static let one thread's snprintf land
 * under another's pointer. Each thread now owns its buffer, so the old
 * contract (valid until this thread's next call) holds per thread. The roots
 * themselves (sysIosRoot's slots, sCacheRoot) are written once before any
 * worker exists. */
const char *sysResolvePath(const char *path)
{
    static __thread char out[1200];

    if (path == NULL)
    {
        return "";
    }
    if (sCacheRoot[0] != '\0' && strncmp(path, "$C/", 3) == 0)
    {
        snprintf(out, sizeof(out), "%s/%s", sCacheRoot, path + 3);
        return out;
    }
#ifdef GE_IOS
    if (strncmp(path, "$S/", 3) == 0)
    {
        snprintf(out, sizeof(out), "%s/%s", sysIosRoot(GE_IOS_ROOT_DOCS, "GE_IOS_DOCUMENTS", "/Documents"), path + 3);
        return out;
    }
    if (strncmp(path, "$C/", 3) == 0)
    {
        snprintf(out, sizeof(out), "%s/%s", sysIosRoot(GE_IOS_ROOT_CACHES, "GE_IOS_CACHES", "/Library/Caches"), path + 3);
        return out;
    }
    if (strncmp(path, "$E/", 3) == 0)
    {
        const char *base = getenv("GE_IOS_BUNDLE");
        snprintf(out, sizeof(out), "%s/%s", (base != NULL && base[0] != '\0') ? base : ".", path + 3);
        return out;
    }
#else
    if (strncmp(path, "$S/", 3) == 0 || strncmp(path, "$C/", 3) == 0 || strncmp(path, "$E/", 3) == 0)
    {
        /* one root on the desktop: the exe dir, where midas.ini and Data/ live */
        snprintf(out, sizeof(out), "%s", portRootPath(path + 3));
        return out;
    }
#endif
    return path;
}
