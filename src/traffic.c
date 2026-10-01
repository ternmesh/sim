#include "tsim/traffic.h"

#include <math.h>
#include <stdlib.h>

#include "tsim/rng.h"
#include "tsim/sched.h"

struct source {
    struct tsim_traffic *traffic;
    uint32_t node;
    struct tsim_rng rng;
    struct tsim_event next;
};

struct tsim_traffic {
    struct tsim_net *net;
    struct tsim_sched *sched;
    struct tsim_traffic_params params;
    uint32_t n;
    struct source *sources;
    uint64_t made;
};

/* An exponential gap with the configured mean, rounded to the nanosecond. 1 - u is in (0, 1], so
 * the log is finite. Never under one nanosecond: a gap that rounded to zero would put two of a
 * node's messages at one instant, and its first at the start itself. That lengthens the mean only
 * for intervals near a nanosecond, which no radio could carry anyway. */
static tsim_time gap(struct source *s) {
    double g = -log(1.0 - tsim_rng_unit(&s->rng)) * (double)s->traffic->params.interval;
    /* Only what no time can hold is capped: plan() checks a gap against the window before adding
     * it, so a gap past the window, however long, falls outside it as it should. */
    if (!(g < (double)INT64_MAX)) {
        return INT64_MAX;
    }
    tsim_time t = (tsim_time)(g + 0.5);
    return t > 0 ? t : 1;
}

static void send_next(struct tsim_sched *sched, void *ctx);

/* Schedules the node's next message `after` from `from`, unless it would fall at or past stop. */
static void plan(struct source *s, tsim_time from) {
    const struct tsim_traffic_params *p = &s->traffic->params;
    tsim_time after = gap(s);
    s->next = (struct tsim_event){0};
    if (after < p->stop - from) {
        s->next = tsim_sched_at(s->traffic->sched, from + after, send_next, s);
    }
}

static void send_next(struct tsim_sched *sched, void *ctx) {
    struct source *s = ctx;
    struct tsim_traffic *t = s->traffic;
    const struct tsim_traffic_params *p = &t->params;
    /* Every draw for this message, then the next gap, whatever the routing does with it. */
    uint32_t dst = TSIM_BROADCAST;
    if (t->n > 1 && tsim_rng_unit(&s->rng) >= p->broadcast) {
        dst = (uint32_t)tsim_rng_below(&s->rng, t->n - 1);
        if (dst >= s->node) {
            dst++;
        }
    }
    uint32_t len = p->len_min + (uint32_t)tsim_rng_below(&s->rng, p->len_max - p->len_min + 1);
    tsim_time now = tsim_sched_now(sched);
    plan(s, now);
    /* A lone node has nobody to send to. */
    if (t->n > 1 && tsim_net_originate(t->net, s->node, dst, len)) {
        t->made++;
    }
}

struct tsim_traffic *tsim_traffic_create(struct tsim_net *net,
                                         const struct tsim_traffic_params *params) {
    const struct tsim_traffic_params *p = params;
    if (p->interval <= 0 || p->len_min > p->len_max || p->len_max > TSIM_FRAME_MAX ||
        !(p->broadcast >= 0.0 && p->broadcast <= 1.0) || p->stop < p->start) {
        return NULL;
    }
    struct tsim_traffic *t = calloc(1, sizeof *t);
    if (!t) {
        return NULL;
    }
    t->net = net;
    t->sched = tsim_net_sched(net);
    t->params = *p;
    t->n = tsim_net_nodes(net);
    t->sources = calloc(t->n, sizeof *t->sources);
    if (!t->sources) {
        free(t);
        return NULL;
    }
    /* The first message comes a gap after the start, not at it, so the nodes do not all send at
     * once. A start in the past begins now. */
    tsim_time now = tsim_sched_now(t->sched);
    tsim_time from = p->start > now ? p->start : now;
    for (uint32_t i = 0; i < t->n; i++) {
        struct source *s = &t->sources[i];
        s->traffic = t;
        s->node = i;
        /* Apart from every node's own streams, and from the network's message content. */
        tsim_rng_init(&s->rng, p->seed, UINT64_C(0xC1) << 56 | i);
        plan(s, from);
    }
    return t;
}

void tsim_traffic_destroy(struct tsim_traffic *t) {
    if (!t) {
        return;
    }
    for (uint32_t i = 0; i < t->n; i++) {
        tsim_sched_cancel(t->sched, t->sources[i].next);
    }
    free(t->sources);
    free(t);
}

uint64_t tsim_traffic_made(const struct tsim_traffic *t) { return t->made; }
