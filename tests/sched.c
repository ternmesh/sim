#include "tsim/sched.h"

#include <stdlib.h>

#include "check.h"
#include "tsim/rng.h"

/* A log the callbacks write into, so a test can see what ran, when, and in what order. */
struct log {
    int ids[4096];
    tsim_time at[4096];
    size_t len;
};

struct mark {
    struct log *log;
    int id;
};

static void record(struct tsim_sched *s, void *ctx) {
    struct mark *m = ctx;
    if (m->log->len < 4096) {
        m->log->ids[m->log->len] = m->id;
        m->log->at[m->log->len] = tsim_sched_now(s);
        m->log->len++;
    }
}

static void runs_in_time_order(void) {
    struct tsim_sched *s = tsim_sched_create();
    struct log log = {0};
    struct mark m[3] = {{&log, 0}, {&log, 1}, {&log, 2}};
    tsim_sched_at(s, TSIM_MS(30), record, &m[0]);
    tsim_sched_at(s, TSIM_MS(10), record, &m[1]);
    tsim_sched_at(s, TSIM_MS(20), record, &m[2]);
    CHECK_EQ_I64(tsim_sched_run_until(s, TSIM_S(1)), 3);
    CHECK_EQ_I64(log.len, 3);
    CHECK_EQ_I64(log.ids[0], 1);
    CHECK_EQ_I64(log.ids[1], 2);
    CHECK_EQ_I64(log.ids[2], 0);
    CHECK_EQ_I64(log.at[0], TSIM_MS(10));
    CHECK_EQ_I64(log.at[2], TSIM_MS(30));
    CHECK_EQ_I64(tsim_sched_now(s), TSIM_S(1));
    tsim_sched_destroy(s);
}

static void ties_run_in_scheduling_order(void) {
    struct tsim_sched *s = tsim_sched_create();
    struct log log = {0};
    struct mark m[100];
    for (int i = 0; i < 100; i++) {
        m[i] = (struct mark){&log, i};
        tsim_sched_at(s, TSIM_MS(5), record, &m[i]);
    }
    tsim_sched_run_until(s, TSIM_MS(5));
    CHECK_EQ_I64(log.len, 100);
    for (int i = 0; i < 100; i++) {
        CHECK_EQ_I64(log.ids[i], i);
    }
    tsim_sched_destroy(s);
}

static void cancel_removes_and_handles_go_stale(void) {
    struct tsim_sched *s = tsim_sched_create();
    struct log log = {0};
    struct mark m[3] = {{&log, 0}, {&log, 1}, {&log, 2}};
    struct tsim_event a = tsim_sched_at(s, TSIM_MS(1), record, &m[0]);
    struct tsim_event b = tsim_sched_at(s, TSIM_MS(2), record, &m[1]);
    struct tsim_event c = tsim_sched_at(s, TSIM_MS(3), record, &m[2]);
    CHECK(tsim_sched_pending(s, b));
    CHECK(tsim_sched_cancel(s, b));
    CHECK(!tsim_sched_pending(s, b));
    CHECK(!tsim_sched_cancel(s, b));
    CHECK_EQ_I64(tsim_sched_size(s), 2);
    tsim_sched_run_until(s, TSIM_MS(10));
    CHECK_EQ_I64(log.len, 2);
    CHECK_EQ_I64(log.ids[0], 0);
    CHECK_EQ_I64(log.ids[1], 2);
    /* Both ran, so their handles are stale too, even after the slots are reused. */
    CHECK(!tsim_sched_cancel(s, a));
    tsim_sched_at(s, TSIM_MS(20), record, &m[0]);
    CHECK(!tsim_sched_cancel(s, a));
    CHECK(!tsim_sched_cancel(s, c));
    CHECK_EQ_I64(tsim_sched_size(s), 1);
    CHECK(!tsim_sched_cancel(s, (struct tsim_event){0}));
    tsim_sched_destroy(s);
}

static void refuses_the_past(void) {
    struct tsim_sched *s = tsim_sched_create();
    struct log log = {0};
    struct mark m = {&log, 0};
    tsim_sched_run_until(s, TSIM_MS(100));
    struct tsim_event e = tsim_sched_at(s, TSIM_MS(99), record, &m);
    CHECK_EQ_I64(e.slot, 0);
    e = tsim_sched_after(s, -1, record, &m);
    CHECK_EQ_I64(e.slot, 0);
    e = tsim_sched_at(s, TSIM_MS(100), record, &m); /* now itself is allowed */
    CHECK(e.slot != 0);
    CHECK_EQ_I64(tsim_sched_size(s), 1);
    tsim_sched_destroy(s);
}

/* A callback that schedules the next one: what a node's transmit loop will look like. */
struct chain {
    struct log *log;
    int left;
};

static void tick(struct tsim_sched *s, void *ctx) {
    struct chain *c = ctx;
    struct mark m = {c->log, c->left};
    record(s, &m);
    if (--c->left > 0) {
        tsim_sched_after(s, TSIM_MS(10), tick, c);
    }
}

static void callbacks_can_schedule(void) {
    struct tsim_sched *s = tsim_sched_create();
    struct log log = {0};
    struct chain c = {&log, 5};
    tsim_sched_at(s, 0, tick, &c);
    CHECK_EQ_I64(tsim_sched_run_until(s, TSIM_MS(25)), 3);
    CHECK_EQ_I64(tsim_sched_now(s), TSIM_MS(25));
    CHECK_EQ_I64(tsim_sched_run_until(s, TSIM_S(1)), 2);
    CHECK_EQ_I64(log.len, 5);
    CHECK_EQ_I64(log.at[4], TSIM_MS(40));
    CHECK(!tsim_sched_step(s));
    tsim_sched_destroy(s);
}

/* Random schedules and cancellations against a slow, obviously correct model: an array scanned
 * for the minimum (time, then scheduling order). Thousands of operations, so the heap's growth,
 * reuse of freed slots and removal from the middle all get exercised. */
struct model_ev {
    tsim_time when;
    uint64_t seq;
    int id;
    struct tsim_event handle;
    int live;
};

static void matches_a_reference_model(void) {
    enum { N = 3000 };
    static struct model_ev ev[N];
    static struct mark marks[N];
    struct tsim_sched *s = tsim_sched_create();
    struct log log = {0};
    struct tsim_rng r;
    tsim_rng_init(&r, 2026, 0);
    int expected[4096];
    size_t expected_len = 0;

    for (int i = 0; i < N; i++) {
        marks[i] = (struct mark){&log, i};
        tsim_time when = tsim_sched_now(s) + (tsim_time)tsim_rng_below(&r, 1000);
        ev[i] = (struct model_ev){when, (uint64_t)i, i, {0}, 1};
        ev[i].handle = tsim_sched_at(s, when, record, &marks[i]);
        CHECK(ev[i].handle.slot != 0);

        uint64_t action = tsim_rng_below(&r, 4);
        if (action == 0 && i > 0) {
            int victim = (int)tsim_rng_below(&r, (uint64_t)i);
            CHECK(tsim_sched_cancel(s, ev[victim].handle) == (ev[victim].live == 1));
            ev[victim].live = 0;
        } else if (action == 1) {
            /* Run one event in both, comparing the choice. */
            int best = -1;
            for (int j = 0; j <= i; j++) {
                if (ev[j].live && (best < 0 || ev[j].when < ev[best].when ||
                                   (ev[j].when == ev[best].when && ev[j].seq < ev[best].seq))) {
                    best = j;
                }
            }
            if (best >= 0) {
                ev[best].live = 0;
                if (expected_len < 4096) {
                    expected[expected_len++] = best;
                }
                CHECK(tsim_sched_step(s));
            }
        }
    }
    CHECK_EQ_I64(log.len, expected_len);
    for (size_t i = 0; i < expected_len && i < log.len; i++) {
        CHECK_EQ_I64(log.ids[i], expected[i]);
    }
    size_t live = 0;
    for (int i = 0; i < N; i++) {
        live += ev[i].live == 1;
    }
    CHECK_EQ_I64(tsim_sched_size(s), live);
    tsim_sched_destroy(s);
}

int main(void) {
    RUN(runs_in_time_order);
    RUN(ties_run_in_scheduling_order);
    RUN(cancel_removes_and_handles_go_stale);
    RUN(refuses_the_past);
    RUN(callbacks_can_schedule);
    RUN(matches_a_reference_model);
    return CHECK_DONE();
}
