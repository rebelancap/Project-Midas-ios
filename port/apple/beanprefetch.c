/* Imported from GoldenEye-Decomp-ios engine/port/src/beanprefetch.c (first 47ef757f, last changed 9d96f186; donor HEAD 55a3a9c8),
 * round 15, XBLA step 4.4 (docs/port-ios/24-round15-chrs.md). 64-bit host layer only.
 * Changes against the donor are marked "Midas round 15". */
/*
 * Tier C files opened and their pictures decoded ahead - see
 * port/include/beanprefetch.h. Round 35a (D-317).
 *
 * MEMORY RULE (charter rule 3c, D-110): everything here is host malloc.
 */

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <ultra64.h>
#include "platform.h"
#include "system.h"
#include "config.h"
#include "texpack.h"
#include "beanmesh.h"
#include "beanstage.h"
#include "beanprefetch.h"

#if defined(_MEMA_H_) || defined(_MEMP_H_)
#error "Tier C must not see the sim's allocators (mema.h/memp.h): host malloc only (D-110)"
#endif

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

struct beanprefetch_entry {
    int16_t level;        /* LEVELID_* */
    const char *source;   /* files/new/<source>/default.bin */
};
#include "beanprefetch_table.h"

#define PF_MAX 16

enum { PF_QUEUED, PF_WORKING, PF_READY, PF_CLAIMED, PF_FAILED };

struct pfent {
    char path[1024];
    const char *source;
    int state;
    struct beanlevel *bm;   /* for the first take; later takes open the file again */
    uint8_t **pics;         /* numtex: pinned in texpack's registry, never ours to free */
    int32_t *w, *h;
    int numtex;
    uint32_t gen;           /* the registry generation the pictures went into */
    int takes;
    uint32_t bytes;
    double ms;
};

static struct pfent ents[PF_MAX];
static int numEnts;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int workerLive;
static volatile int abortWork;
static int loadDone;
static int levelFrames;
static int curLevel = -1;
static int cfgOn = 1;

PD_CONSTRUCTOR static void beanPrefetchConfigInit(void)
{
    configRegisterInt("Video.BeanPrefetch", &cfgOn, 0, 1);
}

static int logOn(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GE_BEANPREFETCHLOG");
        v = e && *e && *e != '0';
    }
    return v;
}

static int fileExists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* beanmodel.c / beanchr.c sourcePath(), the same three places */
static int sourcePath(const char *source, char *out, size_t len)
{
    snprintf(out, len, "%s/files/new/%s/default.bin", sysResolvePath(TEXPACK_XBLA_DIR), source);
    if (fileExists(out)) {
        return 1;
    }
    const char *src = texpackXblaPath();
    if (src && *src) {
        char base[512];
        snprintf(base, sizeof(base), "%s", src);
        snprintf(out, len, "%s/files/new/%s/default.bin", base, source);
        if (fileExists(out)) {
            return 1;
        }
        snprintf(out, len, "%s/new/%s/default.bin", base, source);
        if (fileExists(out)) {
            return 1;
        }
    }
    return 0;
}

static void *workerMain(void *arg)
{
    (void)arg;
    for (int i = 0; i < numEnts; i++) {
        pthread_mutex_lock(&lock);
        if (abortWork) {
            pthread_mutex_unlock(&lock);
            break;
        }
        struct pfent *e = &ents[i];
        if (e->state != PF_QUEUED) {
            pthread_mutex_unlock(&lock);
            continue;
        }
        e->state = PF_WORKING;
        pthread_mutex_unlock(&lock);

        const uint64_t t0 = sysGetMicroseconds();
        struct beanlevel *bm = beanModelOpenRaw(e->path);
        int numdraws = 0, numtex = 0, npics = 0, valid = 1;
        float uvscale = 0.0f;
        uint32_t gen = 0, bytes = 0;
        uint8_t **pics = NULL;
        int32_t *pw = NULL, *ph = NULL;
        if (bm) {
            beanModelInfo(bm, &numdraws, &numtex, &uvscale);
            uint8_t *used = calloc((size_t)(numtex > 0 ? numtex : 1), 1);
            pics = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*pics));
            pw = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*pw));
            ph = calloc((size_t)(numtex > 0 ? numtex : 1), sizeof(*ph));
            if (used && pics && pw && ph) {
                beanLevelUsedTextures(bm, used);
                for (int t = 0; t < numtex && !abortWork; t++) {
                    if (!used[t]) {
                        continue;
                    }
                    int32_t w = 0, h = 0;
                    uint8_t *rgba = beanLevelDecodeTexture(bm, t, &w, &h);
                    if (!rgba) {
                        continue;   /* the build will find it does not decode, as before */
                    }
                    /* registered under its own pixel address: uploaded by the
                     * render thread's pump (D-316) before anything draws it */
                    const uint32_t g = texpackRegisterBeanPictureGen(rgba, rgba, w, h);
                    if (!g) {
                        continue;   /* not pinned (registry full): freed there */
                    }
                    if (gen && g != gen) {
                        valid = 0;   /* a new level's registry: drop them all (they are its own now) */
                    }
                    gen = g;
                    pics[t] = rgba;
                    pw[t] = w;
                    ph[t] = h;
                    bytes += (uint32_t)w * (uint32_t)h * 4;
                    npics++;
                }
            }
            free(used);
        }
        pthread_mutex_lock(&lock);
        e->bm = bm;
        e->pics = pics;
        e->w = pw;
        e->h = ph;
        e->numtex = numtex;
        e->gen = valid ? gen : 0;
        e->bytes = bytes;
        e->ms = (double)(sysGetMicroseconds() - t0) / 1000.0;
        e->state = bm ? PF_READY : PF_FAILED;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
        sysLogPrintf(LOG_NOTE, "beanprefetch: %s %s: %d picture%s (%.1f MB) in %.1f ms", e->source,
                     bm ? "ready" : "did not open", npics, npics == 1 ? "" : "s", (double)bytes / (1024.0 * 1024.0), e->ms);
    }
    return NULL;
}

static void drain(void)
{
    if (workerLive) {
        pthread_mutex_lock(&lock);
        abortWork = 1;
        pthread_mutex_unlock(&lock);
        pthread_join(worker, NULL);
        workerLive = 0;
    }
    for (int i = 0; i < numEnts; i++) {
        struct pfent *e = &ents[i];
        if (e->bm) {
            beanLevelClose(e->bm);
        }
        free(e->pics);   /* the pixels are the registry's */
        free(e->w);
        free(e->h);
        memset(e, 0, sizeof(*e));
    }
    numEnts = 0;
    abortWork = 0;
}

void beanPrefetchDrain(void)
{
    drain();
}

void beanPrefetchLevel(int levelid)
{
    drain();   /* normally already drained by beanPrefetchDrain() (D-326) */
    curLevel = levelid;
    loadDone = 0;
    levelFrames = 0;
    if (!cfgOn || !beanStageModelsEnabled()) {
        return;
    }
    for (size_t k = 0; k < sizeof(beanprefetch_table) / sizeof(beanprefetch_table[0]) && numEnts < PF_MAX; k++) {
        const struct beanprefetch_entry *t = &beanprefetch_table[k];
        if (t->level != levelid || !t->source || !*t->source) {
            continue;
        }
        struct pfent *e = &ents[numEnts];
        if (!sourcePath(t->source, e->path, sizeof(e->path))) {
            continue;
        }
        e->source = t->source;
        e->state = PF_QUEUED;
        numEnts++;
    }
    if (!numEnts) {
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef __APPLE__
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
    if (pthread_create(&worker, &attr, workerMain, NULL) == 0) {
        workerLive = 1;
        sysLogPrintf(LOG_NOTE, "beanprefetch: stage %d: %d file%s queued", levelid, numEnts, numEnts == 1 ? "" : "s");
    } else {
        numEnts = 0;
    }
    pthread_attr_destroy(&attr);
}

/* Midas round 22 (R16-2) */
int beanPrefetchCurrentLevel(void)
{
    return curLevel;
}

void beanPrefetchFrame(void)
{
    /* everything built in the level's first frame (the load window) is a
     * load build; an open from the second frame on is a late one */
    loadDone = ++levelFrames >= 2;
}

struct beanlevel *beanPrefetchTake(const char *path)
{
    struct pfent *e = NULL;
    for (int i = 0; path && i < numEnts && !e; i++) {
        if (strcmp(ents[i].path, path) == 0) {
            e = &ents[i];
        }
    }
    if (loadDone && logOn()) {
        sysLogPrintf(LOG_NOTE, "beanprefetch: late open stage %d %s (%s)", curLevel, path ? path : "?",
                     e ? "listed" : "not listed");
    }
    if (!e) {
        return NULL;
    }
    pthread_mutex_lock(&lock);
    if (e->state == PF_QUEUED) {
        e->state = PF_CLAIMED;   /* not started: the build opens it itself, the worker skips it */
        pthread_mutex_unlock(&lock);
        sysLogPrintf(LOG_NOTE, "beanprefetch: %s wanted before the worker reached it - opened in place", e->source);
        return NULL;
    }
    const uint64_t w0 = sysGetMicroseconds();
    while (e->state == PF_WORKING) {
        pthread_cond_wait(&cond, &lock);
    }
    const double waited = (double)(sysGetMicroseconds() - w0) / 1000.0;
    if (e->state != PF_READY) {
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    struct beanlevel *bm = e->bm;
    e->bm = NULL;
    const int first = e->takes++ == 0;
    const int valid = e->gen != 0 && e->gen == texpackBeanGeneration();
    pthread_mutex_unlock(&lock);
    if (!bm) {
        bm = beanModelOpenRaw(path);   /* a second build from the same file (a body and its own head) */
    }
    if (bm && valid && e->pics) {
        beanLevelAttachPictures(bm, e->pics, e->w, e->h);
    }
    sysLogPrintf(LOG_NOTE, "beanprefetch: %s served (%s take%s%s, waited %.1f ms)", e->source, first ? "first" : "another",
                 first ? ", file open ahead" : "", valid ? ", pictures decoded ahead" : ", pictures decoded now", waited);
    return bm;
}
