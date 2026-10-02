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

/* A set send, and the id of the message it made, which the closed loop leaves alone. */
struct scripted {
    struct tsim_traffic *traffic;
    struct tsim_send send;
    struct tsim_event event;
    uint64_t id;
};

struct tsim_traffic {
    struct tsim_net *net;
    struct tsim_sched *sched;
    struct tsim_traffic_params params;
    uint32_t n;
    struct source *sources;
    struct scripted *scripted;
    bool scripting; /* a set send is being originated, and may finish before its id is known */
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
    if (!p->closed) {
        plan(s, now);
    }
    /* A lone node has nobody to send to. */
    uint64_t id = t->n > 1 ? tsim_net_originate(t->net, s->node, dst, len) : 0;
    if (id) {
        t->made++;
    } else if (p->closed) {
        plan(s, now); /* nothing to wait for */
    }
}

static void send_scripted(struct tsim_sched *sched, void *ctx) {
    (void)sched;
    struct scripted *sc = ctx;
    sc->event = (struct tsim_event){0};
    sc->traffic->scripting = true;
    sc->id = tsim_net_originate(sc->traffic->net, sc->send.src, sc->send.dst, sc->send.len);
    sc->traffic->scripting = false;
    if (sc->id) {
        sc->traffic->made++;
    }
}

/* Closed loop: the node's next gap starts when its routing is done with this message - unless it
 * was a set send, which no gap was waiting on. */
static void on_finished(void *ctx, const struct tsim_message_record *record) {
    struct tsim_traffic *t = ctx;
    if (t->scripting) {
        return; /* a set send, finished as it was made */
    }
    for (uint32_t i = 0; i < t->params.send_count; i++) {
        if (t->scripted[i].id == record->msg.id) {
            return;
        }
    }
    plan(&t->sources[record->msg.src], tsim_sched_now(t->sched));
}

struct tsim_traffic *tsim_traffic_create(struct tsim_net *net,
                                         const struct tsim_traffic_params *params) {
    const struct tsim_traffic_params *p = params;
    uint32_t n = tsim_net_nodes(net);
    if (p->interval < 0 || p->len_min > p->len_max || p->len_max > TSIM_FRAME_MAX ||
        !(p->broadcast >= 0.0 && p->broadcast <= 1.0) || p->stop < p->start ||
        (p->send_count > 0 && !p->sends)) {
        return NULL;
    }
    for (uint32_t i = 0; i < p->send_count; i++) {
        const struct tsim_send *send = &p->sends[i];
        if (send->src >= n || send->dst == send->src ||
            (send->dst != TSIM_BROADCAST && send->dst >= n) || send->len > TSIM_FRAME_MAX) {
            return NULL;
        }
    }
    struct tsim_traffic *t = calloc(1, sizeof *t);
    if (!t) {
        return NULL;
    }
    t->net = net;
    t->sched = tsim_net_sched(net);
    t->params = *p;
    t->params.sends = NULL;
    t->n = n;
    t->sources = calloc(t->n, sizeof *t->sources);
    t->scripted = calloc(p->send_count ? p->send_count : 1, sizeof *t->scripted);
    if (!t->sources || !t->scripted) {
        free(t->sources);
        free(t->scripted);
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
        if (p->interval > 0) {
            plan(s, from);
        }
    }
    for (uint32_t i = 0; i < p->send_count; i++) {
        struct scripted *sc = &t->scripted[i];
        sc->traffic = t;
        sc->send = p->sends[i];
        sc->event =
            tsim_sched_at(t->sched, sc->send.at > now ? sc->send.at : now, send_scripted, sc);
        if (!tsim_sched_pending(t->sched, sc->event)) {
            tsim_traffic_destroy(t); /* out of memory */
            return NULL;
        }
    }
    if (p->closed) {
        tsim_net_observe_finished(net, on_finished, t);
    }
    return t;
}

void tsim_traffic_destroy(struct tsim_traffic *t) {
    if (!t) {
        return;
    }
    if (t->params.closed) {
        tsim_net_observe_finished(t->net, NULL, NULL);
    }
    for (uint32_t i = 0; i < t->n; i++) {
        tsim_sched_cancel(t->sched, t->sources[i].next);
    }
    for (uint32_t i = 0; i < t->params.send_count; i++) {
        tsim_sched_cancel(t->sched, t->scripted[i].event);
    }
    free(t->sources);
    free(t->scripted);
    free(t);
}

uint64_t tsim_traffic_made(const struct tsim_traffic *t) { return t->made; }
