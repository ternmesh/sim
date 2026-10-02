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
    uint32_t *peers; /* into tsim_traffic.peer_list */
    uint32_t peer_count;
};

/* An answer waiting to be sent. */
struct reply {
    struct tsim_traffic *traffic;
    struct reply *prev, *next;
    struct tsim_event event;
    uint32_t src, dst, len;
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
    uint32_t *peer_list;
    struct reply *replies; /* waiting, newest first */
};

/* An exponential gap with the configured mean, never under one nanosecond: a gap that rounded to
 * zero would put two of a node's messages at one instant, and its first at the start itself. That
 * lengthens the mean only for intervals near a nanosecond, which no radio could carry anyway. */
static tsim_time exponential(struct tsim_rng *rng, tsim_time mean);

static tsim_time gap(struct source *s) {
    tsim_time t = exponential(&s->rng, s->traffic->params.interval);
    return t > 0 ? t : 1;
}

static void send_next(struct tsim_sched *sched, void *ctx);

/* An exponential draw of the given mean, rounded to the nanosecond. 1 - u is in (0, 1], so the log
 * is finite. Only what no time can hold is capped: a caller checks the draw against its window
 * before adding it, so one past the window, however long, falls outside it as it should. */
static tsim_time exponential(struct tsim_rng *rng, tsim_time mean) {
    double g = -log(1.0 - tsim_rng_unit(rng)) * (double)mean;
    if (!(g < (double)INT64_MAX)) {
        return INT64_MAX;
    }
    return (tsim_time)(g + 0.5);
}

static void unlink_reply(struct reply *r) {
    if (r->prev) {
        r->prev->next = r->next;
    } else {
        r->traffic->replies = r->next;
    }
    if (r->next) {
        r->next->prev = r->prev;
    }
}

static void send_reply(struct tsim_sched *sched, void *ctx) {
    (void)sched;
    struct reply *r = ctx;
    struct tsim_traffic *t = r->traffic;
    unlink_reply(r);
    if (tsim_net_originate(t->net, r->src, r->dst, r->len)) {
        t->made++;
    }
    free(r);
}

/* Draws whether the unicast just drawn is answered, and when and how long the answer is; and if
 * it is, and falls before stop, schedules it. */
static void plan_reply(struct source *s, uint32_t dst, tsim_time now) {
    struct tsim_traffic *t = s->traffic;
    const struct tsim_traffic_params *p = &t->params;
    bool answered = tsim_rng_unit(&s->rng) < p->reply;
    tsim_time delay = exponential(&s->rng, p->reply_delay);
    uint32_t len = p->len_min + (uint32_t)tsim_rng_below(&s->rng, p->len_max - p->len_min + 1);
    if (!answered || delay >= p->stop - now) {
        return;
    }
    struct reply *r = malloc(sizeof *r);
    if (!r) {
        return; /* out of memory: the answer is not sent */
    }
    *r = (struct reply){.traffic = t, .next = t->replies, .src = dst, .dst = s->node, .len = len};
    r->event = tsim_sched_at(t->sched, now + delay, send_reply, r);
    if (!tsim_sched_pending(t->sched, r->event)) {
        free(r);
        return;
    }
    if (t->replies) {
        t->replies->prev = r;
    }
    t->replies = r;
}

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
        if (p->peers) {
            dst = s->peers[tsim_rng_below(&s->rng, s->peer_count)];
        } else {
            dst = (uint32_t)tsim_rng_below(&s->rng, t->n - 1);
            if (dst >= s->node) {
                dst++;
            }
        }
    }
    uint32_t len = p->len_min + (uint32_t)tsim_rng_below(&s->rng, p->len_max - p->len_min + 1);
    tsim_time now = tsim_sched_now(sched);
    if (p->reply > 0 && dst != TSIM_BROADCAST) {
        plan_reply(s, dst, now);
    }
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

/* Each node picks `peers` others uniformly, from a stream of its own, and every pick is made
 * mutual: a node's peers are those it picked and those that picked it, each once, its own picks
 * first. */
static bool pick_peers(struct tsim_traffic *t) {
    uint32_t n = t->n, k = t->params.peers;
    uint32_t *picks = malloc((size_t)n * k * sizeof *picks);
    uint32_t *count = calloc(n, sizeof *count);
    uint8_t *mark = calloc(n, 1);
    t->peer_list = malloc((size_t)n * k * 2 * sizeof *t->peer_list);
    if (!picks || !count || !mark || !t->peer_list) {
        free(picks);
        free(count);
        free(mark);
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        struct tsim_rng rng;
        tsim_rng_init(&rng, t->params.seed, UINT64_C(0xC2) << 56 | i);
        mark[i] = 1;
        for (uint32_t m = 0; m < k; m++) {
            uint32_t j;
            do {
                j = (uint32_t)tsim_rng_below(&rng, n);
            } while (mark[j]);
            mark[j] = 1;
            picks[(size_t)i * k + m] = j;
        }
        mark[i] = 0;
        for (uint32_t m = 0; m < k; m++) {
            mark[picks[(size_t)i * k + m]] = 0;
        }
        count[i] += k;
        for (uint32_t m = 0; m < k; m++) {
            count[picks[(size_t)i * k + m]]++;
        }
    }
    /* Room for every pick and every pick of it, then the duplicates taken out. */
    size_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        t->sources[i].peers = t->peer_list + at;
        at += count[i];
    }
    for (uint32_t i = 0; i < n; i++) {
        struct source *s = &t->sources[i];
        for (uint32_t m = 0; m < k; m++) {
            s->peers[s->peer_count++] = picks[(size_t)i * k + m];
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t m = 0; m < k; m++) {
            struct source *s = &t->sources[picks[(size_t)i * k + m]];
            s->peers[s->peer_count++] = i;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        struct source *s = &t->sources[i];
        uint32_t kept = 0;
        for (uint32_t m = 0; m < s->peer_count; m++) {
            if (!mark[s->peers[m]]) {
                mark[s->peers[m]] = 1;
                s->peers[kept++] = s->peers[m];
            }
        }
        for (uint32_t m = 0; m < kept; m++) {
            mark[s->peers[m]] = 0;
        }
        s->peer_count = kept;
    }
    free(picks);
    free(count);
    free(mark);
    return true;
}

struct tsim_traffic *tsim_traffic_create(struct tsim_net *net,
                                         const struct tsim_traffic_params *params) {
    const struct tsim_traffic_params *p = params;
    uint32_t n = tsim_net_nodes(net);
    if (p->interval < 0 || p->len_min > p->len_max || p->len_max > TSIM_FRAME_MAX ||
        !(p->broadcast >= 0.0 && p->broadcast <= 1.0) || p->stop < p->start ||
        (p->send_count > 0 && !p->sends) || p->peers > (n ? n - 1 : 0) ||
        !(p->reply >= 0.0 && p->reply <= 1.0) || (p->reply > 0 && p->closed) ||
        p->reply_delay < 0) {
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
    if (p->peers && !pick_peers(t)) {
        tsim_traffic_destroy(t);
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
    while (t->replies) {
        struct reply *r = t->replies;
        tsim_sched_cancel(t->sched, r->event);
        t->replies = r->next;
        free(r);
    }
    free(t->sources);
    free(t->scripted);
    free(t->peer_list);
    free(t);
}

uint64_t tsim_traffic_made(const struct tsim_traffic *t) { return t->made; }
