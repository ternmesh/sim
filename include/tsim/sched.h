#ifndef TSIM_SCHED_H
#define TSIM_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tsim/time.h"

/* The discrete-event scheduler: a clock and a queue of callbacks ordered by time.
 *
 * Events at the same time run in the order they were scheduled, so a run is a pure function of
 * its inputs and its seed. Scheduling and cancelling are O(log n). */
struct tsim_sched;

typedef void (*tsim_event_fn)(struct tsim_sched *sched, void *ctx);

/* Names one scheduled event. A handle outlives its event: once the event has run or been
 * cancelled, the handle goes stale and cancelling it again does nothing. The zero handle is
 * never issued, so it can mean "nothing scheduled". */
struct tsim_event {
    uint32_t slot;
    uint32_t gen;
};

struct tsim_sched *tsim_sched_create(void);
void tsim_sched_destroy(struct tsim_sched *sched);

/* The current simulated time: the time of the event running now, or of the last one run. */
tsim_time tsim_sched_now(const struct tsim_sched *sched);

/* Schedules fn(sched, ctx) at an absolute time, which must not be earlier than now. Returns the
 * zero handle on a time in the past or when memory runs out. */
struct tsim_event tsim_sched_at(struct tsim_sched *sched, tsim_time when, tsim_event_fn fn,
                                void *ctx);

/* The same, `delay` after now. */
struct tsim_event tsim_sched_after(struct tsim_sched *sched, tsim_time delay, tsim_event_fn fn,
                                   void *ctx);

/* Removes a scheduled event. Returns false if it has already run, was already cancelled, or is
 * the zero handle. */
bool tsim_sched_cancel(struct tsim_sched *sched, struct tsim_event event);

/* Whether the handle still names an event waiting to run. */
bool tsim_sched_pending(const struct tsim_sched *sched, struct tsim_event event);

/* How many events are waiting. */
size_t tsim_sched_size(const struct tsim_sched *sched);

/* Runs the earliest event. Returns false, and does nothing, when the queue is empty. */
bool tsim_sched_step(struct tsim_sched *sched);

/* Runs every event at or before `end`, including ones those events schedule, then leaves the
 * clock at `end`. Returns the number of events run. */
size_t tsim_sched_run_until(struct tsim_sched *sched, tsim_time end);

#endif
