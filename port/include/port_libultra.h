/**
 * Port-side interface to the libultra shim (port/src/libultra_shim.c).
 *
 * The shim keeps GE's os* call surface alive single-threaded (see
 * docs/port/phase2-decisions.md D8). The fake scheduler uses this header to
 * (a) provide the real event pump behind blocking osRecvMesg and
 * (b) find the queues the game registered for VI retrace / OS events so it
 * can manufacture the messages hardware used to post.
 */
#ifndef PORT_LIBULTRA_H
#define PORT_LIBULTRA_H

#include <ultra64.h>

/**
 * Called by osRecvMesg/osSendMesg when a queue is empty/full under
 * OS_MESG_BLOCK, up to a bounded number of times, re-checking the queue after
 * each call. The shim ships a weak no-op default; the fake scheduler provides
 * the real one ("pump host events + emit one retrace") so boss.c:458's
 * blocking recv makes progress.
 */
extern void portPumpEvents(void);

/**
 * Retrieve what the game registered via osViSetEvent (the VI retrace
 * queue+message, sched.c). Out-params may be NULL; *mq is NULL if
 * osViSetEvent was never called.
 */
void portOsViGetEvent(OSMesgQueue **mq, OSMesg *msg, u32 *retraceCount);

/**
 * Retrieve what the game registered via osSetEventMesg for `event`
 * (OS_EVENT_SI from joy.c, OS_EVENT_SP/DP/PRENMI from sched.c, ...).
 * Out-params may be NULL; *mq is NULL if nothing was registered.
 */
void portOsGetEventMesg(OSEvent event, OSMesgQueue **mq, OSMesg *msg);

#endif /* PORT_LIBULTRA_H */
