/* Imported verbatim from GoldenEye-Decomp-ios engine/port/include/beanprefetch.h (55a3a9c8), Phase 1 round 2.
 * Included only from donor code under #ifdef PORT (64-bit); implementations are
 * inert stubs in port/apple/donor_stubs.c unless noted there. docs/port-ios/05. */
#ifndef PORT_BEANPREFETCH_H
#define PORT_BEANPREFETCH_H

/*
 * Round 35a (D-317): Tier C files a level will build AFTER its load frame
 * (the intro swirl's copies of Bond's body, head and held gun; the
 * first-person gun at the hand-over), opened and their pictures decoded
 * ahead on a worker thread, so the build in the frame that first draws them
 * skips the file read and the decodes, and its pictures are already up as
 * resident textures (gfx_pc.cpp, D-316).
 *
 * Which files: the generated per-level table beanprefetch_table.h
 * (scripts/gen-bean-prefetch.sh: the sources the oracle opened after each
 * level's load frame). A file not in it is built exactly as before; a listed
 * file that is never built costs its decode and its pictures' memory for the
 * level. Builds are bit-identical either way: the same open and decode, done
 * earlier.
 *
 * Key Video.BeanPrefetch (0 off, 1 on; default 1). No settings row.
 * GE_BEANPREFETCHLOG=1 logs every open after the load frame (the generator
 * reads these lines).
 */

#ifdef __cplusplus
extern "C" {
#endif

struct beanlevel;

/* bg.c load_bg_file(), BEFORE the last level's pictures are forgotten
 * (D-326): stops the worker and frees the last level's entries, so nothing
 * still decoding the old level registers into the new generation. */
void beanPrefetchDrain(void);
/* bg.c load_bg_file(), after the last level's pictures are forgotten. */
void beanPrefetchLevel(int levelid);
/* beanmodel.c beanModelFrame(), once a level frame (from the second on, a
 * build is "late"). */
void beanPrefetchFrame(void);
/* Midas round 22 (R16-2): the levelid of the last beanPrefetchLevel(), -1 none
 * (beanstage.c's mid-level rebuild queues the same level's files again). */
int beanPrefetchCurrentLevel(void);
/* beanModelOpen(): a handle opened ahead for `path`, or NULL (open it). */
struct beanlevel *beanPrefetchTake(const char *path);

#ifdef __cplusplus
}
#endif

#endif
