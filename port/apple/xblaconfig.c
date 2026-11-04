/*
 * xblaconfig.c - the donor's config registrar (port/include/config.h) over
 * Midas's midas.ini. Round 10, XBLA step 1
 * (docs/port-ios/xbla-port-proposal.md, docs/port-ios/19-round10-xbla-import.md).
 *
 * GoldenEye-Decomp-ios's texpack.c and beanstage.c keep their switches in
 * that port's own config.c (configRegisterInt/String + ge007.ini). Midas has
 * no config.c and is not getting one: its options live in midas.ini through
 * port_options.c / video.c, and portIniWriteStrs rewrites a section's keys in
 * place. This file is the few functions those donor files need:
 *
 *   configRegisterInt / configRegisterString   remember (key, variable)
 *   configLoad                                 read them from midas.ini
 *   configSaveKey                              write one back (portIniWriteStrs)
 *
 * Key spelling: the donor's dotted "Video.XblaTextures" is line
 * `XblaTextures=1` of midas.ini section `[video]`. Midas's own loader
 * (video.c videoLoadConfig) reads every line regardless of section and would
 * log "unknown control" for these; nothing writes them until a setter runs
 * (the iOS shell, step 5), so a default install never sees that line
 * (R10-3 in doc 19).
 *
 * Lazy by design: nothing here opens midas.ini until configLoad() is called,
 * and in step 1 only the XBLA getters (xbla_keys.c) call it. With no import
 * asked for, the engine does exactly what it did before this file existed.
 *
 * Host memory only. The variables are host statics of the port layer; the
 * sim never reads them, and this file must never see the sim's allocators.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include "config.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "xblaconfig.c must not see the sim's allocators (mema.h/memp.h): host state only"
#endif

extern FILE *portRootFopen(const char *name, const char *mode);                                         /* main.c */
extern int portIniWriteStrs(const char *section, const char *const *keys, const char *const *vals, int n); /* video.c; weak no-op headless */
extern void portConLogf(const char *fmt, ...);                                                          /* port_console.c */

enum { CFG_INT = 0, CFG_STR };

struct cfgkey {
    const char *key;   /* "Video.XblaTextures" */
    int type;
    void *ptr;
    int min, max;      /* CFG_INT; min == max: no clamp */
    int size;          /* CFG_STR buffer size */
};

#define CFG_MAX_KEYS 32

static struct cfgkey sKeys[CFG_MAX_KEYS];
static int sNumKeys;
static int sLoaded;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;

/* "Video.XblaTextures" -> section "[video]", name "XblaTextures". */
static int cfgSplit(const char *key, char *section, size_t secLen, const char **name)
{
    const char *dot = strchr(key, '.');
    size_t n, i;

    if (dot == NULL || dot == key || dot[1] == '\0')
    {
        return 0;
    }
    n = (size_t)(dot - key);
    if (n + 3 > secLen)
    {
        return 0;
    }
    section[0] = '[';
    for (i = 0; i < n; i++)
    {
        const char c = key[i];
        section[1 + i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    section[1 + n] = ']';
    section[2 + n] = '\0';
    *name = dot + 1;
    return 1;
}

static void cfgApply(struct cfgkey *k, const char *val)
{
    if (k->type == CFG_INT)
    {
        char *end = NULL;
        long v = strtol(val, &end, 0);

        if (end == val)
        {
            portConLogf("ge: config: %s=\"%s\" is not a number, kept %d\n", k->key, val, *(int *)k->ptr);
            return;
        }
        if (k->min != k->max)
        {
            if (v < k->min) v = k->min;
            if (v > k->max) v = k->max;
        }
        *(int *)k->ptr = (int)v;
    }
    else
    {
        snprintf((char *)k->ptr, (size_t)k->size, "%s", val);
    }
}

static char *cfgTrim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
    {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    {
        *--e = '\0';
    }
    return s;
}

/* Reads the keys [from, sNumKeys) from midas.ini. Caller holds sLock. */
static void cfgReadFile(int from)
{
    char line[1024];
    char section[64] = "";
    FILE *f;

    if (from >= sNumKeys)
    {
        return;
    }
    f = portRootFopen("midas.ini", "r");
    if (f == NULL)
    {
        return; /* no ini yet: every key keeps its default */
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *s = cfgTrim(line);
        char *eq;
        int i;

        if (s[0] == '\0' || s[0] == ';' || s[0] == '#')
        {
            continue;
        }
        if (s[0] == '[')
        {
            snprintf(section, sizeof(section), "%s", s);
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL || section[0] == '\0')
        {
            continue;
        }
        *eq = '\0';
        s = cfgTrim(s);
        for (i = from; i < sNumKeys; i++)
        {
            char sec[64];
            const char *name;

            if (cfgSplit(sKeys[i].key, sec, sizeof(sec), &name) && strcasecmp(sec, section) == 0
                && strcasecmp(name, s) == 0)
            {
                cfgApply(&sKeys[i], cfgTrim(eq + 1)); /* last line wins */
            }
        }
    }
    fclose(f);
}

static void cfgRegister(const char *key, int type, void *ptr, int min, int max, int size)
{
    int i;

    if (key == NULL || ptr == NULL)
    {
        return;
    }
    pthread_mutex_lock(&sLock);
    for (i = 0; i < sNumKeys; i++)
    {
        if (strcmp(sKeys[i].key, key) == 0)
        {
            break; /* re-registration: the newer variable wins */
        }
    }
    if (i == CFG_MAX_KEYS)
    {
        pthread_mutex_unlock(&sLock);
        portConLogf("ge: config: too many keys, %s not registered\n", key);
        return;
    }
    sKeys[i].key = key;
    sKeys[i].type = type;
    sKeys[i].ptr = ptr;
    sKeys[i].min = min;
    sKeys[i].max = max;
    sKeys[i].size = size;
    if (i == sNumKeys)
    {
        sNumKeys++;
    }
    if (sLoaded)
    {
        cfgReadFile(i); /* registered after the load: read it now */
    }
    pthread_mutex_unlock(&sLock);
}

void configRegisterInt(const char *key, int *value, int min, int max)
{
    cfgRegister(key, CFG_INT, value, min, max, 0);
}

void configRegisterString(const char *key, char *value, int bufSize)
{
    if (bufSize <= 0)
    {
        return;
    }
    cfgRegister(key, CFG_STR, value, 0, 0, bufSize);
}

void configLoad(void)
{
    pthread_mutex_lock(&sLock);
    cfgReadFile(0);
    sLoaded = 1;
    pthread_mutex_unlock(&sLock);
}

int configSaveKey(const char *key)
{
    char section[64];
    char val[600];
    const char *name = NULL;
    const char *k[1];
    const char *v[1];
    int i, found = -1;

    pthread_mutex_lock(&sLock);
    for (i = 0; key != NULL && i < sNumKeys; i++)
    {
        if (strcmp(sKeys[i].key, key) == 0)
        {
            found = i;
            break;
        }
    }
    if (found < 0 || !cfgSplit(sKeys[found].key, section, sizeof(section), &name))
    {
        pthread_mutex_unlock(&sLock);
        return 0;
    }
    if (sKeys[found].type == CFG_INT)
    {
        snprintf(val, sizeof(val), "%d", *(int *)sKeys[found].ptr);
    }
    else
    {
        snprintf(val, sizeof(val), "%s", (const char *)sKeys[found].ptr);
    }
    pthread_mutex_unlock(&sLock);
    k[0] = name;
    v[0] = val;
    return portIniWriteStrs(section, k, v, 1) ? 1 : 0;
}

/* Round 16 (XBLA step 5, docs/port-ios/25-round16-shell.md): the iOS
 * settings page and the dev bridge read and write these keys by name through
 * geHostConfigGet/Set (hostinput.c), the same entry as Midas's own options.
 * A set applies to the registered variable (the owners poll it: texpack.c
 * ensureIndex, beanstage.c per draw) and saves it at once (R11-5). */
int configGetString(const char *key, char *out, int outLen)
{
    int i;

    if (out == NULL || outLen <= 0)
    {
        return -1;
    }
    out[0] = '\0';
    pthread_mutex_lock(&sLock);
    for (i = 0; key != NULL && i < sNumKeys; i++)
    {
        if (strcmp(sKeys[i].key, key) == 0)
        {
            if (sKeys[i].type == CFG_INT)
            {
                snprintf(out, (size_t)outLen, "%d", *(int *)sKeys[i].ptr);
            }
            else
            {
                snprintf(out, (size_t)outLen, "%s", (const char *)sKeys[i].ptr);
            }
            pthread_mutex_unlock(&sLock);
            return 0;
        }
    }
    pthread_mutex_unlock(&sLock);
    return -1;
}

int configSetString(const char *key, const char *value)
{
    int i, found = -1;

    if (key == NULL || value == NULL)
    {
        return -1;
    }
    pthread_mutex_lock(&sLock);
    for (i = 0; i < sNumKeys; i++)
    {
        if (strcmp(sKeys[i].key, key) == 0)
        {
            found = i;
            cfgApply(&sKeys[i], value);
            break;
        }
    }
    pthread_mutex_unlock(&sLock);
    if (found < 0)
    {
        return -1;
    }
    configSaveKey(key);
    return 0;
}
