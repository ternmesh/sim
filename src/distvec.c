#include "tsim/distvec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/nodeset.h"
#include "tsim/rng.h"

#define INF TSIM_DISTVEC_METRIC_INF
#define BROADCAST_HOP 0xFFFFFFFFu
#define FRAME_MAX 255
#define ROUTES 4 /* kept per destination */
#define HISTORY 16
#define RETRACTS 3 /* announces a retraction goes in, in turn with the routes: one may be lost */
#define HOP_MISS 4 /* announces a frame lost on its way to a next hop counts as missed */
#define SEEN 256
#define REQUEST_TRIES 5 /* a starved node asks this many times, request_interval apart */

#define TYPE_ANNOUNCE 0x01
#define TYPE_REQUEST 0x02
#define TYPE_DATA 0x03
#define TYPE_ACK 0x04
#define TYPE_BCAST 0x05

#define ANNOUNCE_HEAD 16
#define IHU_LEN 5
#define ROUTE_LEN 8
#define REQUEST_HEAD 6
#define ASK_LEN 7
#define ASKS_MAX ((FRAME_MAX - REQUEST_HEAD) / ASK_LEN)
#define DATA_HEAD 18
#define ACK_LEN 18
#define BCAST_HEAD 10
/* The longest IHU round an announce can tell: twice it and one more must still fit the 16-bit
 * count of announces a receiver keeps between IHUs. Only a node naming one neighbour to a frame
 * among more than 32767 would need more. */
#define ROUND_MAX 32767

#define FLAG_INFRA 0x01

/* A promise is two bytes: up to 32767 seconds, or with the top bit set a number of minutes up to
 * 32766, or - all ones - no promise at all, for a node whose MAC has held its announces back for
 * longer still. */
#define PROMISE_MINUTES 0x8000u
#define PROMISE_NONE 0xFFFFu
#define PROMISE_UNBOUNDED (INT64_MAX / 4)

/* Higher goes first. */
#define PRIORITY_CONTROL 3
#define PRIORITY_RELAY 2
#define PRIORITY_ANNOUNCE 2
#define PRIORITY_DATA 1

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Whether sequence number a is newer than b, modulo 2^16 (RFC 8966 §3.2.1). */
static bool newer(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)(a - b);
    return d != 0 && d < 0x8000;
}

static uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* What the seen cache knows a message's frames by. */
static uint64_t frame_key(uint8_t type, uint32_t src, uint32_t id) {
    uint64_t h = mix((uint64_t)src << 32 ^ id ^ (uint64_t)type << 56);
    return h ? h : 1; /* 0 marks an empty slot */
}

/* --- State --- */

/* A route to a destination through one neighbour, as the neighbour advertised it. */
struct entry {
    uint16_t slot; /* the neighbour's slot plus 1; 0 for none */
    uint16_t seq;
    uint16_t metric; /* the neighbour's, without the link */
};

struct dest {
    struct entry e[ROUTES];
    uint16_t sel; /* the selected route's slot plus 1, or 0 */
    bool has_fd;
    bool advertised;  /* a finite route to it has been announced, and not retracted since */
    bool urgent;      /* on the list of changed routes */
    bool listed;      /* in the frame being built */
    uint8_t retracts; /* times its retraction is still to go */
    uint8_t tries;    /* seqno requests sent while starved: 0 when not starved */
    uint16_t fd_seq;  /* the feasibility distance */
    uint16_t fd_metric;
    uint16_t adv_seq; /* what was last announced */
    uint16_t adv_metric;
    uint16_t asked_seq;
    tsim_time asked; /* when a seqno request about it was last sent or passed on, or -1 */
};

struct neighbour {
    uint32_t id;
    bool used;
    bool infra;
    bool ihu_owed; /* new: owed an IHU before the round-robin comes to it */
    bool owed_was; /* ihu_owed before the last frame built, should the queue refuse it */
    uint8_t dr;    /* its IHU for this node, in 255ths; 0 for none */
    uint8_t span;  /* how many of its last HISTORY announces the history covers */
    uint16_t history;
    uint16_t last_seq;
    uint16_t cost;      /* the link's, or INF */
    uint16_t cost_used; /* the cost every route through it was last chosen with */
    tsim_time heard;
    tsim_time promise; /* how soon it said it would announce again */
    uint16_t ihu_seq;  /* the announce that last named this node */
    double floor;      /* the quietest it decodes this node at, in dBm; NAN until known */
    double boost;      /* power control: dB more for frames to it, for hops it has lost */
};

/* A frame sent to a next hop, waiting to hear the next hop pass it on. */
struct hop {
    uint64_t handle;
    tsim_time due; /* -1 until the frame has gone */
    uint8_t tries;
    uint8_t type;
    uint8_t hops; /* as sent */
    uint32_t src;
    uint32_t dst;
    uint32_t id;
    uint32_t next;
    struct tsim_tx tx;
};

/* A frame sent in answer to one received, waiting out its delay. A broadcast relay then stays
 * until it has gone, so hearing more copies can still take it back out of the queue. */
struct held {
    uint64_t key; /* a broadcast relay's; 0 for others */
    bool listen;  /* sent to a next hop, which is then listened for */
    tsim_time due;
    uint64_t handle; /* 0 while it waits */
    uint8_t heard;
    struct tsim_tx tx;
};

/* A seqno or route request waiting to go out, with others for the same next hop. */
struct ask {
    uint32_t next;
    uint32_t dst;
    uint16_t seq;
    uint8_t hops; /* 0 for a route request */
};

/* A message of this node's waiting for its acknowledgement. */
struct awaiting {
    struct router *owner;
    struct awaiting *next;
    uint32_t id;
    uint32_t dst;
    uint8_t attempt;
    struct tsim_timer *timer;
    uint64_t handle; /* the attempt's frame, while it waits in the queue; 0 once it has gone */
    tsim_time wait;  /* how long to wait for the acknowledgement once it has */
    uint32_t len;
    uint8_t content[FRAME_MAX];
};

/* Airtime a node may still spend, in nanoseconds: a token bucket. */
struct bucket {
    double ns;
    double max_ns;
    double rate; /* nanoseconds earned per nanosecond */
    tsim_time at;
};

struct router {
    struct tsim_node *node;
    uint32_t self;
    uint32_t nodes;
    bool infra;
    uint32_t ann_head; /* an announce's head, and a data frame's: longer with power control */
    uint32_t data_head;
    double node_dbm; /* what frames for every neighbour go at */
    struct tsim_distvec_config config;
    struct tsim_rng rng;
    double ref_ms; /* the reference frame's airtime */

    uint16_t seq;     /* this node's own route */
    uint16_t ann_seq; /* its announces */
    struct dest *dest;
    uint16_t *slot_of; /* a node's neighbour slot plus 1, or 0 */
    struct neighbour *nb;
    size_t nb_count;
    size_t nb_cap;
    size_t ihu_cursor;
    size_t ihu_was; /* the IHU cursor before the last frame built, should the queue refuse it */
    uint32_t *urgent;
    size_t urgent_count;
    size_t urgent_cap;
    uint32_t cursor;     /* the next destination a slice starts from */
    uint32_t selected;   /* destinations with a selected route */
    uint32_t retracting; /* destinations with retractions still to go */
    tsim_time promised;  /* in its last announce */
    uint16_t round;      /* in its last announce, in frames */
    uint64_t ann_handle; /* its last announce queued, until sent, and when it was queued */
    tsim_time ann_queued;
    tsim_time mac_wait; /* the longest its announces have lately waited to be sent */
    bool changed;       /* something this node announces changed: a Trickle inconsistency */
    bool asked;         /* a seqno request for this node waits on its next announce */

    tsim_time interval; /* Trickle */
    tsim_time interval_end;
    bool fired;
    uint8_t heard;
    uint8_t quiet;
    struct tsim_timer *trickle;

    struct bucket announces; /* the cap, in two */
    struct bucket requests;
    bool announce_waiting;
    struct tsim_timer *cap_timer;

    struct hop *hops;
    size_t hop_count;
    size_t hop_cap;
    struct tsim_timer *hop_timer;

    struct held *held;
    size_t held_count;
    size_t held_cap;
    struct tsim_timer *out_timer;

    uint64_t seen[SEEN];
    unsigned seen_next;
    struct awaiting *awaiting;
    struct tsim_timer *house;
    uint32_t *starving; /* destinations asked about, to ask again until answered */
    size_t starving_count;
    size_t starving_cap;
    struct tsim_timer *request_timer;
    struct ask *asks; /* requests not yet sent */
    size_t ask_count;
    size_t ask_cap;
    struct tsim_timer *ask_timer;
};

struct tsim_distvec_config tsim_distvec_default(uint16_t channel, const struct tsim_lora *lora,
                                                double tx_dbm) {
    return (struct tsim_distvec_config){
        .channel = channel,
        .lora = *lora,
        .tx_dbm = tx_dbm,
        .relays = "all",
        .imin = TSIM_S(8),
        .doublings = 6,
        .redundancy = 3,
        .quiet_max = 2,
        .neighbour_timeout = TSIM_S(60 * 60),
        .cap = 0.005,
        .request_share = 0.25,
        .cap_window = TSIM_S(60),
        .burst = 4,
        .ihu_max = 8,
        .ref_len = 32,
        .etx_max = 8,
        .hysteresis = 0.1,
        .change = 0.25,
        .request_interval = TSIM_S(10),
        .hop_max = 32,
        .hop_retries = 2,
        .hop_wait = TSIM_S(4),
        .retries = 3,
        .ack_wait = TSIM_S(5),
        .ack_factor = 4,
        .jitter = 2,
        .bcast_hops = 4,
        .bcast_window = 3,
        .bcast_cancel = 2,
        .power = true,
        .tx_min_dbm = -9,
        .margin_db = 10,
        .step_db = 3,
        .snr_floor_db = -7.5 - 2.5 * (lora->sf - 7), /* Semtech's */
        .power_k = 8,
    };
}

static double promise_secs(const struct tsim_distvec_config *c, tsim_time interval, tsim_time rest);

const char *tsim_distvec_check(const struct tsim_distvec_config *c) {
    if (memchr(c->relays, 0, sizeof c->relays) == NULL || tsim_nodeset_contains(c->relays, 0) < 0) {
        return "relays is not all, or node numbers and ranges";
    }
    if (tsim_lora_airtime(&c->lora, c->ref_len) < 0) {
        return "ref_len has no airtime at the radio's modulation";
    }
    if (c->imin <= 0 || c->doublings > 16 || c->imin > (INT64_MAX >> 17)) {
        return "imin is not above 0, or doublings is over 16";
    }
    if (c->neighbour_timeout <= 0 || c->cap_window <= 0 || c->request_interval < 0 ||
        c->hop_wait < 0 || c->ack_wait <= 0) {
        return "a time is out of range";
    }
    if (!(c->cap > 0 && c->cap <= 1) || !(c->request_share > 0 && c->request_share < 1)) {
        return "cap is not above 0 and at most 1, or request_share is not between 0 and 1";
    }
    if (c->burst < 1 || c->burst > 16 || c->ihu_max > 48) {
        return "burst is not 1 to 16, or ihu_max is over 48";
    }
    if (!(c->etx_max >= 1 && c->etx_max <= 1e6) || !(c->hysteresis >= 0 && c->hysteresis <= 1) ||
        !(c->change >= 0 && c->change <= 1) || !(c->ack_factor >= 0 && c->ack_factor <= 1e3) ||
        !(c->bcast_window >= 0 && c->bcast_window <= 1e3) ||
        !(c->jitter >= 0 && c->jitter <= 1e3)) {
        return "a factor is out of range";
    }
    if (c->power &&
        (!(c->tx_dbm >= -128 && c->tx_dbm <= 127) || !(c->tx_min_dbm >= -128) ||
         !(ceil(c->tx_min_dbm) <= floor(c->tx_dbm)) || !(c->margin_db >= 0 && c->margin_db <= 60) ||
         !(c->step_db >= 0 && c->step_db <= 60) || !isfinite(c->snr_floor_db))) {
        return "with power control, tx_dbm and tx_min_dbm are not whole dBm apart within -128 to "
               "127, or a margin is out of range";
    }
    if (c->hop_max < 1 || c->bcast_hops > 254) {
        return "hop_max is 0, or bcast_hops is over 254";
    }
    tsim_time top = c->imin << c->doublings;
    if (promise_secs(c, top, top) > UINT16_MAX) {
        return "imax, quiet_max and the cap would let a node keep quiet longer than 65535 s";
    }
    return NULL;
}

static tsim_time now(const struct router *r) { return tsim_node_now(r->node); }

static tsim_time imax(const struct router *r) { return r->config.imin << r->config.doublings; }

static bool seen(const struct router *r, uint64_t key) {
    for (unsigned i = 0; i < SEEN; i++) {
        if (r->seen[i] == key) {
            return true;
        }
    }
    return false;
}

static void mark_seen(struct router *r, uint64_t key) {
    r->seen[r->seen_next] = key;
    r->seen_next = (r->seen_next + 1) % SEEN;
}

static struct tsim_tx frame(const struct router *r, enum tsim_purpose purpose, uint8_t priority) {
    return (struct tsim_tx){
        .channel = r->config.channel,
        .lora = r->config.lora,
        .tx_dbm = r->node_dbm,
        .purpose = purpose,
        .priority = priority,
    };
}

/* --- Power --- */

/* The quietest a neighbour would decode this node at: what its frame went at, a byte of dBm, less
 * how far above the floor it was heard. */
static double floor_of(const struct router *r, uint8_t sent, double snr) {
    return (int8_t)sent - (snr - r->config.snr_floor_db);
}

/* What a frame to a neighbour goes at: its floor and margin, and loud enough too for the node the
 * frame answers, whose floor is `back` - NAN for none. */
static double power_for(const struct router *r, const struct neighbour *n, double back) {
    const struct tsim_distvec_config *c = &r->config;
    if (!c->power) {
        return c->tx_dbm;
    }
    if (isnan(n->floor)) {
        return floor(c->tx_dbm);
    }
    double p = n->floor + c->margin_db + n->boost;
    if (!isnan(back) && back + c->margin_db > p) {
        p = back + c->margin_db;
    }
    double lo = ceil(c->tx_min_dbm), hi = floor(c->tx_dbm);
    p = ceil(p);
    return p < lo ? lo : p > hi ? hi : p;
}

/* The power frames for every neighbour go at: tx_dbm, or with power_k, loud enough for the
 * power_k neighbours with the lowest floors - k-neighbour topology control - while it knows that
 * many. */
static void node_power(struct router *r) {
    const struct tsim_distvec_config *c = &r->config;
    if (!c->power) {
        r->node_dbm = c->tx_dbm;
        return;
    }
    double hi = floor(c->tx_dbm), lo = ceil(c->tx_min_dbm);
    r->node_dbm = hi;
    if (!c->power_k) {
        return;
    }
    double low[UINT8_MAX]; /* the k lowest floors, in order */
    unsigned have = 0, k = c->power_k;
    for (size_t i = 0; i < r->nb_count; i++) {
        const struct neighbour *n = &r->nb[i];
        if (!n->used || isnan(n->floor) || (have == k && n->floor >= low[k - 1])) {
            continue;
        }
        unsigned at = have < k ? have++ : k - 1;
        while (at > 0 && low[at - 1] > n->floor) {
            low[at] = low[at - 1];
            at--;
        }
        low[at] = n->floor;
    }
    if (have == k) {
        double p = ceil(low[k - 1] + c->margin_db);
        r->node_dbm = p < lo ? lo : p > hi ? hi : p;
    }
}

/* --- Links --- */

static struct neighbour *slot(struct router *r, uint16_t s) { return &r->nb[s - 1]; }

/* The share of a neighbour's announces heard, in 255ths: its history, with an announce counted
 * missed for every promise it has let pass since it was last heard. */
static uint8_t heard_rate(const struct router *r, const struct neighbour *n) {
    tsim_time silent = now(r) - n->heard;
    int64_t missed = n->promise > 0 ? silent / n->promise : 0;
    if (missed >= HISTORY) {
        return 0;
    }
    unsigned history = (unsigned)n->history << missed & 0xFFFFu;
    int span = n->span + (int)missed > HISTORY ? HISTORY : n->span + (int)missed;
    return (uint8_t)(255 * __builtin_popcount(history) / span);
}

static uint16_t link_cost(const struct router *r, const struct neighbour *n) {
    uint8_t got = heard_rate(r, n);
    if (n->dr == 0 || got == 0) {
        return INF; /* not heard both ways */
    }
    double df = got / 255.0;
    double etx = 1.0 / (df * (n->dr / 255.0));
    if (etx > r->config.etx_max) {
        return INF;
    }
    double cost = ceil(etx * r->ref_ms);
    return cost < 1 ? 1 : cost >= INF ? INF - 1 : (uint16_t)cost;
}

/* Whether a route to `d` through slot `s` may be used: the neighbour is heard both ways, and is
 * infrastructure unless it is the destination. */
static bool usable(struct router *r, uint16_t s, uint32_t d) {
    const struct neighbour *n = slot(r, s);
    return n->used && n->cost != INF && (n->infra || n->id == d);
}

static uint16_t total(uint16_t metric, uint16_t cost) {
    if (metric == INF || cost == INF) {
        return INF;
    }
    uint32_t t = (uint32_t)metric + cost;
    return t >= INF ? INF - 1 : (uint16_t)t;
}

/* --- Routes --- */

static struct entry *entry_by(struct dest *d, uint16_t s) {
    for (int i = 0; i < ROUTES; i++) {
        if (d->e[i].slot == s) {
            return &d->e[i];
        }
    }
    return NULL;
}

static bool feasible(const struct dest *d, const struct entry *e) {
    return !d->has_fd || newer(e->seq, d->fd_seq) ||
           (e->seq == d->fd_seq && e->metric < d->fd_metric);
}

static void push_urgent(struct router *r, uint32_t d) {
    if (r->dest[d].urgent) {
        return;
    }
    if (r->urgent_count == r->urgent_cap) {
        size_t cap = r->urgent_cap ? 2 * r->urgent_cap : 16;
        uint32_t *grown = realloc(r->urgent, cap * sizeof *grown);
        if (!grown) {
            return; /* out of memory: the change goes out with the slices instead */
        }
        r->urgent = grown;
        r->urgent_cap = cap;
    }
    r->urgent[r->urgent_count++] = d;
    r->dest[d].urgent = true;
}

static void refill(struct router *r, struct bucket *b) {
    tsim_time t = now(r);
    b->ns += (double)(t - b->at) * b->rate;
    if (b->ns > b->max_ns) {
        b->ns = b->max_ns;
    }
    b->at = t;
}

static void bucket_init(struct bucket *b, double rate, tsim_time window, double frame_ns) {
    b->rate = rate;
    b->max_ns = (double)window * rate;
    if (b->max_ns < frame_ns) {
        b->max_ns = frame_ns; /* room for one frame, or nothing could ever go */
    }
    b->ns = b->max_ns;
    b->at = 0;
}

/* Sends the requests waiting, as few frames as there are next hops, out of the requests' share of
 * the cap. Requests it cannot pay for are dropped; a starved node asks again (starved()). */
static void ask_fire(void *ctx) {
    struct router *r = ctx;
    struct bucket *b = &r->requests;
    refill(r, b);
    while (r->ask_count) {
        uint32_t next = r->asks[0].next;
        struct tsim_tx tx = frame(r, TSIM_PURPOSE_CONTROL, PRIORITY_CONTROL);
        uint32_t i = REQUEST_HEAD;
        size_t n = 0;
        for (size_t k = 0; k < r->ask_count && n < ASKS_MAX; k++) {
            if (r->asks[k].next == next) {
                n++;
            }
        }
        double cost = (double)tsim_lora_airtime(&tx.lora, REQUEST_HEAD + (uint32_t)n * ASK_LEN);
        if (b->ns < cost) {
            r->ask_count = 0;
            return;
        }
        tx.bytes[0] = TYPE_REQUEST;
        put32(tx.bytes + 1, next);
        n = 0;
        for (size_t k = 0; k < r->ask_count;) {
            const struct ask *a = &r->asks[k];
            if (a->next != next || n == ASKS_MAX) {
                k++;
                continue;
            }
            put32(tx.bytes + i, a->dst);
            put16(tx.bytes + i + 4, a->seq);
            tx.bytes[i + 6] = a->hops;
            i += ASK_LEN;
            n++;
            memmove(&r->asks[k], &r->asks[k + 1], (r->ask_count - k - 1) * sizeof *r->asks);
            r->ask_count--;
        }
        tx.bytes[5] = (uint8_t)n;
        tx.len = i;
        if (!tsim_node_send(r->node, &tx)) {
            r->ask_count = 0; /* the queue is full: dropped uncharged, and asked again */
            return;
        }
        b->ns -= (double)tsim_lora_airtime(&tx.lora, tx.len);
    }
}

/* Adds a request to those waiting to go to `next`, or folds it into one already there. They go
 * after a jitter, so the requests one change sets off at once share frames, and so do the
 * neighbours that heard the same request. */
static void request(struct router *r, uint32_t d, uint16_t seq, uint32_t next, uint8_t hops) {
    for (size_t k = 0; k < r->ask_count; k++) {
        struct ask *a = &r->asks[k];
        if (a->next == next && a->dst == d) {
            if (newer(seq, a->seq)) {
                a->seq = seq;
            }
            a->hops = hops > a->hops ? hops : a->hops;
            return;
        }
    }
    if (r->ask_count == r->ask_cap) {
        size_t cap = r->ask_cap ? 2 * r->ask_cap : 16;
        struct ask *grown = realloc(r->asks, cap * sizeof *grown);
        if (!grown) {
            return; /* out of memory: asked again on the next try */
        }
        r->asks = grown;
        r->ask_cap = cap;
    }
    r->asks[r->ask_count++] = (struct ask){.next = next, .dst = d, .seq = seq, .hops = hops};
    if (!tsim_timer_pending(r->ask_timer)) {
        struct tsim_lora l = r->config.lora;
        double window =
            r->config.jitter * (double)tsim_lora_airtime(&l, REQUEST_HEAD + 8 * ASK_LEN);
        tsim_timer_start(r->ask_timer, (tsim_time)(tsim_rng_unit(&r->rng) * window));
    }
}

/* Asks the neighbours about `d`: for a newer seq than this node's feasibility distance if it has
 * one, and otherwise - having never announced a route to `d`, it has nothing a seq could be newer
 * than - for whatever route they have. */
static void ask(struct router *r, uint32_t d) {
    struct dest *ds = &r->dest[d];
    ds->asked = now(r);
    if (!ds->has_fd) {
        ds->asked_seq = 0;
        request(r, d, 0, BROADCAST_HOP, 0);
        return;
    }
    /* Asked of the neighbour with the best of the infeasible routes, as RFC 8966 allows, so a
     * request goes up one path to the source rather than up every neighbour's. */
    uint16_t seq = (uint16_t)(ds->fd_seq + 1);
    uint32_t next = BROADCAST_HOP;
    uint16_t best = INF;
    for (int i = 0; i < ROUTES; i++) {
        const struct entry *e = &ds->e[i];
        if (!e->slot) {
            continue;
        }
        if (newer(e->seq, seq)) {
            seq = e->seq; /* ask for no less than a neighbour already has */
        }
        uint16_t t = usable(r, e->slot, d) ? total(e->metric, slot(r, e->slot)->cost) : INF;
        if (t < best) {
            best = t;
            next = slot(r, e->slot)->id;
        }
    }
    ds->asked_seq = seq;
    request(r, d, seq, next, r->config.hop_max);
}

/* A node with no feasible route to `d`, or none at all when it has a message for it, asks at once
 * unless it asked within request_interval, and then again every request_interval, REQUEST_TRIES
 * times in all, until a route comes: a request is a single frame, and as easily lost as any. */
static void starved(struct router *r, uint32_t d) {
    struct dest *ds = &r->dest[d];
    if (ds->tries == 0) {
        if (r->starving_count == r->starving_cap) {
            size_t cap = r->starving_cap ? 2 * r->starving_cap : 8;
            uint32_t *grown = realloc(r->starving, cap * sizeof *grown);
            if (!grown) {
                return;
            }
            r->starving = grown;
            r->starving_cap = cap;
        }
        r->starving[r->starving_count++] = d;
        ds->tries = 1;
        if (!tsim_timer_pending(r->request_timer)) {
            tsim_timer_start(r->request_timer, r->config.request_interval);
        }
    }
    tsim_time t = now(r);
    if (ds->asked >= 0 && t - ds->asked < r->config.request_interval) {
        return;
    }
    ask(r, d);
}

static void request_fire(void *ctx) {
    struct router *r = ctx;
    for (size_t i = 0; i < r->starving_count;) {
        uint32_t d = r->starving[i];
        struct dest *ds = &r->dest[d];
        if (ds->sel || ds->tries >= REQUEST_TRIES) {
            ds->tries = 0;
            r->starving[i] = r->starving[--r->starving_count];
            continue;
        }
        ds->tries++;
        ask(r, d);
        i++;
    }
    if (r->starving_count) {
        tsim_timer_start(r->request_timer, r->config.request_interval);
    }
}

/* Chooses the route to `d` afresh, and notes whether what this node announces about it changed. */
static void reselect(struct router *r, uint32_t d) {
    struct dest *ds = &r->dest[d];
    struct entry *cur = ds->sel ? entry_by(ds, ds->sel) : NULL;
    struct entry *best = NULL;
    uint16_t best_total = INF, cur_total = INF;
    bool infeasible = false;
    for (int i = 0; i < ROUTES; i++) {
        struct entry *e = &ds->e[i];
        if (!e->slot || e->metric == INF || !usable(r, e->slot, d)) {
            continue;
        }
        if (!feasible(ds, e)) {
            infeasible = true;
            continue;
        }
        uint16_t t = total(e->metric, slot(r, e->slot)->cost);
        if (e == cur) {
            cur_total = t;
        }
        if (t < best_total) {
            best = e;
            best_total = t;
        }
    }
    struct entry *chosen = best;
    if (cur && cur_total != INF && best != cur &&
        !((double)best_total < (double)cur_total * (1.0 - r->config.hysteresis))) {
        chosen = cur; /* not enough better to move */
    }
    bool was = ds->sel != 0;
    ds->sel = chosen ? chosen->slot : 0;
    r->selected = r->selected - was + (chosen != NULL);
    if (!chosen && infeasible) {
        starved(r, d);
    }
    if (!r->infra) {
        return; /* a leaf announces no routes */
    }
    if (!chosen) {
        if (ds->advertised || was) {
            push_urgent(r, d); /* to retract it */
            r->changed = true;
        }
        return;
    }
    uint16_t t = chosen == cur ? cur_total : best_total;
    double moved = fabs((double)t - (double)ds->adv_metric);
    if (!ds->advertised || newer(chosen->seq, ds->adv_seq) ||
        moved > r->config.change * (double)ds->adv_metric) {
        push_urgent(r, d);
        r->changed = true;
    }
}

/* How much a cached route is worth keeping. */
struct rank {
    bool selectable; /* usable and feasible now */
    uint16_t seq;
    uint16_t total;
};

static struct rank rank_of(struct router *r, uint32_t d, const struct entry *e) {
    uint16_t t = total(e->metric, slot(r, e->slot)->cost);
    return (struct rank){
        .selectable = t != INF && usable(r, e->slot, d) && feasible(&r->dest[d], e),
        .seq = e->seq,
        .total = t,
    };
}

/* Whether `a` is worth less than `b`. */
static bool below(struct rank a, struct rank b) {
    if (a.selectable != b.selectable) {
        return b.selectable;
    }
    if (a.seq != b.seq) {
        return newer(b.seq, a.seq);
    }
    return a.total > b.total;
}

/* What the neighbour in slot `s` advertises about `d`. */
static void update(struct router *r, uint32_t d, uint16_t seq, uint16_t metric, uint16_t s) {
    if (d == r->self || d >= r->nodes) {
        return;
    }
    struct dest *ds = &r->dest[d];
    struct entry *e = entry_by(ds, s);
    if (metric == INF) {
        if (!e) {
            return;
        }
        *e = (struct entry){0};
    } else if (e) {
        e->seq = seq;
        e->metric = metric;
    } else {
        /* A free place, or else the worst route that is not the selected one, if this beats it -
         * worst by what makes a route worth keeping: one that could be selected now before one
         * that could not, then the newer seq, then the lower metric. Ranked by metric alone, four
         * stale routes cheaper than a newer one would keep it out, and the node starved. */
        struct entry fresh = {.slot = s, .seq = seq, .metric = metric};
        struct rank mine = rank_of(r, d, &fresh);
        struct entry *place = NULL;
        struct rank worst = {0};
        for (int i = 0; i < ROUTES; i++) {
            struct entry *x = &ds->e[i];
            if (!x->slot) {
                place = x;
                break;
            }
            struct rank k = rank_of(r, d, x);
            if (x->slot != ds->sel && (!place || !below(worst, k))) {
                worst = k;
                place = x;
            }
        }
        if (!place || (place->slot && !below(worst, mine))) {
            return;
        }
        *place = (struct entry){.slot = s, .seq = seq, .metric = metric};
    }
    reselect(r, d);
}

/* Every destination with a route through slot `s`, chosen afresh. */
static void reselect_through(struct router *r, uint16_t s) {
    for (uint32_t d = 0; d < r->nodes; d++) {
        if (d != r->self && entry_by(&r->dest[d], s)) {
            reselect(r, d);
        }
    }
}

static void trickle_reset(struct router *r);

/* A neighbour is gone: unheard for too long, or a frame sent to it never arrived. */
static void forget(struct router *r, uint16_t s) {
    struct neighbour *n = slot(r, s);
    if (!n->used) {
        return;
    }
    n->used = false;
    n->cost = INF;
    r->slot_of[n->id] = 0;
    for (uint32_t d = 0; d < r->nodes; d++) {
        struct entry *e = d != r->self ? entry_by(&r->dest[d], s) : NULL;
        if (e) {
            *e = (struct entry){0};
            reselect(r, d);
        }
    }
    r->changed = true;
    trickle_reset(r);
    r->changed = false;
}

static uint16_t neighbour(struct router *r, uint32_t id) {
    if (r->slot_of[id]) {
        return r->slot_of[id];
    }
    size_t i = 0;
    while (i < r->nb_count && r->nb[i].used) {
        i++;
    }
    if (i == r->nb_count) {
        if (r->nb_count == r->nb_cap) {
            size_t cap = r->nb_cap ? 2 * r->nb_cap : 16;
            if (cap > UINT16_MAX - 1) {
                return 0;
            }
            struct neighbour *grown = realloc(r->nb, cap * sizeof *grown);
            if (!grown) {
                return 0;
            }
            r->nb = grown;
            r->nb_cap = cap;
        }
        r->nb_count++;
    }
    r->nb[i] = (struct neighbour){
        .id = id, .used = true, .ihu_owed = true, .cost = INF, .cost_used = INF, .floor = NAN};
    r->slot_of[id] = (uint16_t)(i + 1);
    return (uint16_t)(i + 1);
}

/* --- Trickle and the cap --- */

static void announce(struct router *r);

static void trickle_begin(struct router *r) {
    r->heard = 0;
    r->fired = false;
    tsim_time half = r->interval / 2;
    tsim_time t = half + (tsim_time)tsim_rng_below(&r->rng, (uint64_t)(r->interval - half));
    r->interval_end = now(r) + r->interval;
    tsim_timer_start(r->trickle, t);
}

static void trickle_fire(void *ctx) {
    struct router *r = ctx;
    if (!r->fired) {
        r->fired = true;
        /* Changed routes waiting are an inconsistency of this node's own: never suppressed. */
        if (r->config.redundancy == 0 || r->heard < r->config.redundancy ||
            r->quiet >= r->config.quiet_max || r->urgent_count > 0 || r->asked) {
            r->quiet = 0;
            announce(r);
        } else {
            r->quiet++;
        }
        tsim_timer_start(r->trickle, r->interval_end - now(r));
        return;
    }
    /* With changes of its own still waiting - one that came after it announced in this interval -
     * it stays at imin rather than doubling, so they go within the next one. */
    if (r->urgent_count == 0 && !r->asked) {
        r->interval = r->interval * 2 > imax(r) ? imax(r) : r->interval * 2;
    }
    trickle_begin(r);
}

static void trickle_reset(struct router *r) {
    if (r->interval > r->config.imin) {
        r->interval = r->config.imin;
        trickle_begin(r);
    }
}

static void cap_fire(void *ctx) {
    struct router *r = ctx;
    r->announce_waiting = false;
    announce(r);
}

/* --- Announcing --- */

/* Writes one route into an announce, as this node advertises it, and keeps the feasibility
 * distance. Returns false if there is nothing to say about it. */
static bool advertise(struct router *r, uint32_t d, uint8_t *out) {
    struct dest *ds = &r->dest[d];
    uint16_t seq, metric;
    if (ds->sel) {
        const struct entry *e = entry_by(ds, ds->sel);
        seq = e->seq;
        metric = total(e->metric, slot(r, ds->sel)->cost);
        if (metric == INF) {
            return false;
        }
        /* Sticky: while the metric stays within `change` of what was last announced at this seq,
         * announce that again, so the noise in a link's ETX does not drag the feasibility distance
         * down to its luckiest moment and starve the neighbours when the noise goes the other way.
         * It stays loop-free: what is announced is still above the next hop's own metric. */
        if (ds->advertised && seq == ds->adv_seq && ds->adv_metric > e->metric &&
            fabs((double)metric - (double)ds->adv_metric) <=
                r->config.change * (double)ds->adv_metric) {
            metric = ds->adv_metric;
        }
        if (!ds->has_fd || newer(seq, ds->fd_seq) ||
            (seq == ds->fd_seq && metric < ds->fd_metric)) {
            ds->has_fd = true;
            ds->fd_seq = seq;
            ds->fd_metric = metric;
        }
        ds->advertised = true;
        ds->adv_seq = seq;
        ds->adv_metric = metric;
        r->retracting -= ds->retracts > 0;
        ds->retracts = 0;
    } else if (ds->advertised || ds->retracts) {
        /* A retraction lost on the air would leave a neighbour with the route for ever - routes
         * do not expire - so it goes RETRACTS times, the repeats in turn with the routes. */
        seq = ds->adv_seq;
        metric = INF;
        uint8_t left = (uint8_t)(ds->advertised ? RETRACTS - 1 : ds->retracts - 1);
        if (left > 0 && ds->retracts == 0) {
            r->retracting++;
        } else if (left == 0 && ds->retracts > 0) {
            r->retracting--;
        }
        ds->retracts = left;
        ds->advertised = false;
    } else {
        return false;
    }
    put32(out, d);
    put16(out + 4, seq);
    put16(out + 6, metric);
    return true;
}

/* The promise a node announcing `rest` before the end of an interval of `interval` makes, in whole
 * seconds: see promise_s(). */
static double promise_secs(const struct tsim_distvec_config *c, tsim_time interval,
                           tsim_time rest) {
    tsim_time top = c->imin << c->doublings;
    double ns = (double)rest;
    tsim_time i = interval;
    for (int k = 0; k <= c->quiet_max; k++) {
        i = i * 2 > top ? top : i * 2;
        ns += (double)i;
    }
    ns += (double)tsim_lora_airtime(&c->lora, FRAME_MAX) / (c->cap * (1 - c->request_share));
    return ceil(ns / (double)TSIM_S(1));
}

/* The longest this node may go before it announces again, in whole seconds, announcing now: what
 * is left of its current interval - an announce the cap held back can go at any point of it - then
 * quiet_max intervals it may keep quiet and the one it must announce in, each twice the last up to
 * imax and announced in at its very end at worst, then the time its bucket takes to pay for a full
 * frame, then the longest its announces have lately waited in the MAC's queue - which the routing
 * cannot bound, a MAC holding frames back for its own duty cycle, say. A neighbour that hears
 * nothing from it for that long counts an announce missed. tsim_distvec_check() refuses a
 * configuration whose Trickle and cap alone could keep it quiet for more than 65535 s; past what
 * the two bytes it goes in can say, the MAC's part makes it no promise at all. */
static double promise_s(const struct router *r) {
    tsim_time rest = r->interval_end - now(r);
    double s = promise_secs(&r->config, r->interval, rest > 0 ? rest : 0);
    tsim_time queued = r->ann_handle ? now(r) - r->ann_queued : 0; /* the one still waiting */
    tsim_time mac = queued > r->mac_wait ? queued : r->mac_wait;
    s += ceil((double)mac / (double)TSIM_S(1));
    return s < 1 ? 1 : s;
}

/* A promise of `s` seconds as it goes in an announce: rounded up, never down. */
static uint16_t promise_code(double s) {
    if (s <= PROMISE_MINUTES - 1) {
        return (uint16_t)s;
    }
    double minutes = ceil(s / 60);
    return minutes < PROMISE_NONE - PROMISE_MINUTES
               ? (uint16_t)(PROMISE_MINUTES | (uint16_t)minutes)
               : PROMISE_NONE;
}

static tsim_time promise_time(uint16_t code) {
    return code == PROMISE_NONE     ? PROMISE_UNBOUNDED
           : code & PROMISE_MINUTES ? (tsim_time)(code & ~PROMISE_MINUTES) * TSIM_S(60)
                                    : (tsim_time)code * TSIM_S(1);
}

/* How many IHUs an announce frame may carry: ihu_max, less what would leave infrastructure no
 * room for a single route. */
static uint8_t ihu_room(const struct router *r) {
    uint32_t room = (FRAME_MAX - r->ann_head - (r->infra ? ROUTE_LEN : 0)) / IHU_LEN;
    return (uint8_t)(r->config.ihu_max < room ? r->config.ihu_max : room);
}

/* Builds one announce frame. Returns its length. */
static uint32_t build(struct router *r, uint8_t *b) {
    uint32_t i = r->ann_head;
    b[0] = TYPE_ANNOUNCE;
    if (r->config.power) {
        b[16] = (uint8_t)(int8_t)r->node_dbm;
    }
    put32(b + 1, r->self);
    put16(b + 5, r->ann_seq);
    put16(b + 7, r->seq);
    b[9] = r->infra ? FLAG_INFRA : 0;
    uint16_t promise = promise_code(promise_s(r));
    r->promised = promise_time(promise);
    put16(b + 10, promise);

    /* IHUs: neighbours owed one first, then the rest in turn from where the last frame stopped,
     * none twice. */
    uint8_t ihus = 0, ihu_max = ihu_room(r);
    size_t start = r->ihu_cursor;
    r->ihu_was = start;
    for (size_t k = 0; k < r->nb_count; k++) {
        r->nb[k].owed_was = r->nb[k].ihu_owed;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (size_t k = 0; k < r->nb_count && ihus < ihu_max; k++) {
            size_t at = pass ? (start + k) % r->nb_count : k;
            struct neighbour *n = &r->nb[at];
            if (!n->used || n->ihu_owed != (pass == 0)) {
                continue;
            }
            n->ihu_owed = pass == 0; /* listed in this frame, so skipped by the second pass */
            put32(b + i, n->id);
            b[i + 4] = heard_rate(r, n);
            i += IHU_LEN;
            ihus++;
            if (pass) {
                r->ihu_cursor = (at + 1) % r->nb_count;
            }
        }
    }
    size_t heard = 0;
    for (size_t k = 0; k < r->nb_count; k++) {
        r->nb[k].ihu_owed = false;
        heard += r->nb[k].used;
    }
    /* How many frames it takes to name every neighbour heard: a neighbour left out of more
     * than that has been left out of the round. */
    uint8_t per = ihu_room(r);
    size_t rotation = per ? (heard + per - 1) / per : 0;
    r->round = (uint16_t)(rotation > ROUND_MAX ? ROUND_MAX : rotation);
    put16(b + 12, r->round);
    b[14] = ihus;

    uint8_t routes = 0;
    if (r->infra) {
        size_t taken = 0;
        while (taken < r->urgent_count && i + ROUTE_LEN <= FRAME_MAX) {
            uint32_t d = r->urgent[taken++];
            r->dest[d].urgent = false;
            if (advertise(r, d, b + i)) {
                r->dest[d].listed = true;
                i += ROUTE_LEN;
                routes++;
            }
        }
        /* Then the rest in turn, none twice: a retraction repeated in the frame it first went in
         * would count as two of its RETRACTS and be lost with the one frame. */
        for (uint32_t k = 0; k < r->nodes && i + ROUTE_LEN <= FRAME_MAX; k++) {
            uint32_t d = r->cursor;
            r->cursor = (r->cursor + 1) % r->nodes;
            struct dest *ds = &r->dest[d];
            if (d != r->self && !ds->listed && (ds->sel || ds->retracts) &&
                advertise(r, d, b + i)) {
                i += ROUTE_LEN;
                routes++;
            }
        }
        for (size_t k = 0; k < taken; k++) {
            r->dest[r->urgent[k]].listed = false;
        }
        r->urgent_count -= taken;
        if (r->urgent_count) {
            memmove(r->urgent, r->urgent + taken, r->urgent_count * sizeof *r->urgent);
        }
    }
    b[15] = routes;
    return i;
}

/* How long the next announce frame will be, at most: what build() would put in it. */
static uint32_t planned(const struct router *r) {
    size_t named = 0;
    for (size_t k = 0; k < r->nb_count; k++) {
        named += r->nb[k].used;
    }
    uint32_t len = r->ann_head + (uint32_t)(named < ihu_room(r) ? named : ihu_room(r)) * IHU_LEN;
    if (r->infra) {
        uint64_t routes = (uint64_t)r->urgent_count + r->selected + r->retracting;
        uint64_t room = (FRAME_MAX - len) / ROUTE_LEN;
        len += (uint32_t)(routes < room ? routes : room) * ROUTE_LEN;
    }
    return len;
}

/* An announce the queue would not take: what it said goes on the list of changes again, and a
 * route it retracted counts as still announced, so the next announce says it all. The feasibility
 * distances it set stay set, which is only ever stricter than need be. */
static void unsent(struct router *r, const uint8_t *b) {
    /* The round of IHUs goes back to where it was: a refused frame must not take its share of
     * the round with it, or the same share might be refused every time. */
    r->ihu_cursor = r->ihu_was;
    for (size_t k = 0; k < r->nb_count; k++) {
        r->nb[k].ihu_owed = r->nb[k].owed_was;
    }
    const uint8_t *p = b + r->ann_head + (uint32_t)b[14] * IHU_LEN;
    for (uint8_t k = 0; k < b[15]; k++, p += ROUTE_LEN) {
        uint32_t d = get32(p);
        if (get16(p + 6) == INF) {
            r->dest[d].advertised = true;
        }
        push_urgent(r, d);
    }
}

static void announce(struct router *r) {
    if (r->announce_waiting) {
        return;
    }
    struct bucket *b = &r->announces;
    refill(r, b);
    for (int sent = 0; sent < r->config.burst; sent++) {
        if (sent > 0 && r->urgent_count == 0) {
            return;
        }
        /* A frame is built only when the bucket can pay for the longest it could come to. */
        tsim_time cost = tsim_lora_airtime(&r->config.lora, planned(r));
        if (b->ns < (double)cost) {
            r->announce_waiting = true;
            double wait = ((double)cost - b->ns) / b->rate;
            tsim_timer_start(r->cap_timer, (tsim_time)ceil(wait) + 1);
            return;
        }
        node_power(r);
        struct tsim_tx tx = frame(r, TSIM_PURPOSE_ANNOUNCE, PRIORITY_ANNOUNCE);
        tx.len = build(r, tx.bytes);
        uint64_t handle = tsim_node_send(r->node, &tx);
        if (!handle) {
            unsent(r, tx.bytes);
            return; /* the queue is full: no use building more */
        }
        r->ann_handle = handle;
        r->ann_queued = now(r);
        /* Numbered and charged only once queued: a gap in the numbers tells the neighbours of
         * announces lost on the air, which a refused one never reached. */
        r->ann_seq++;
        r->asked = false;
        b->ns -= (double)tsim_lora_airtime(&tx.lora, tx.len);
    }
}

static void on_announce(struct router *r, const uint8_t *b, uint32_t len, double snr) {
    if (len < r->ann_head) {
        return;
    }
    uint32_t from = get32(b + 1);
    uint16_t rotation = get16(b + 12);
    uint8_t ihus = b[14], routes = b[15];
    if (from >= r->nodes || from == r->self ||
        r->ann_head + (uint32_t)ihus * IHU_LEN + (uint32_t)routes * ROUTE_LEN > len) {
        return;
    }
    bool fresh = r->slot_of[from] == 0;
    uint16_t s = neighbour(r, from);
    if (!s) {
        return;
    }
    struct neighbour *n = slot(r, s);
    uint16_t seq = get16(b + 5);
    if (fresh) {
        n->history = 1;
        n->span = 1;
    } else {
        uint16_t gap = (uint16_t)(seq - n->last_seq);
        if (gap == 0 || gap >= 0x8000) {
            return; /* a copy, or older than the last: nothing new */
        }
        n->history = (uint16_t)(gap >= HISTORY ? 1u : (unsigned)n->history << gap | 1u);
        n->span = (uint8_t)(n->span + gap > HISTORY ? HISTORY : n->span + gap);
    }
    n->last_seq = seq;
    n->heard = now(r);
    n->infra = b[9] & FLAG_INFRA;
    n->promise = promise_time(get16(b + 10));
    if (r->config.power) {
        double floor = floor_of(r, b[16], snr);
        n->floor = isnan(n->floor) ? floor : 0.75 * n->floor + 0.25 * floor;
    }
    if (fresh) {
        n->ihu_seq = seq;
    }
    /* Its IHU for this node, if this frame has one. A sender names every neighbour it hears once
     * in `rotation` frames; one that has gone more than that without naming this node - twice
     * over, and one more, when it names them in turn, for the frames that name new neighbours
     * first - does not hear it. Counted in its announce numbers, so a frame lost on the air counts
     * too, and however slowly the sender announces, its last IHU stands until its round is done. */
    const uint8_t *p = b + r->ann_head;
    bool named = false;
    for (uint8_t k = 0; k < ihus; k++, p += IHU_LEN) {
        if (get32(p) == r->self) {
            n->dr = p[4];
            n->ihu_seq = seq;
            named = true;
        }
    }
    uint32_t allowed = rotation <= 1 ? 1u : 2u * rotation + 1u;
    if (!named && (uint16_t)(seq - n->ihu_seq) >= allowed) {
        n->dr = 0;
    }

    r->changed = fresh;
    uint16_t was = n->cost;
    n->cost = link_cost(r, n);
    bool flipped = (was == INF) != (n->cost == INF);
    if (flipped ||
        fabs((double)n->cost - (double)n->cost_used) > r->config.change * (double)n->cost_used) {
        n->cost_used = n->cost;
        r->changed |= flipped;
        reselect_through(r, s);
    }
    update(r, from, get16(b + 7), 0, s);
    if (n->infra) {
        for (uint8_t k = 0; k < routes; k++, p += ROUTE_LEN) {
            update(r, get32(p), get16(p + 4), get16(p + 6), s);
        }
    }
    if (r->changed) {
        trickle_reset(r);
    } else if (r->heard < UINT8_MAX) {
        r->heard++;
    }
    r->changed = false;
}

/* One request of a frame's. */
static void on_ask(struct router *r, uint32_t d, uint16_t seq, uint8_t hops) {
    if (d >= r->nodes) {
        return;
    }
    if (hops == 0) {
        /* A route request: answered by whoever has a route, and never passed on. */
        if (d == r->self || (r->infra && r->dest[d].sel)) {
            if (d != r->self) {
                push_urgent(r, d);
            }
            trickle_reset(r);
        }
        return;
    }
    if (d == r->self) {
        /* Answered by the next announce, whatever Trickle would suppress: the seq rides on every
         * one. A new seq starts a fresh Imin; a request for one it has already gone up to only
         * brings it down to Imin, so a stream of them cannot keep putting the answer off. */
        r->asked = true;
        if (newer(seq, r->seq)) {
            r->seq = seq;
            r->interval = r->config.imin;
            trickle_begin(r);
        } else {
            trickle_reset(r);
        }
        return;
    }
    struct dest *ds = &r->dest[d];
    if (!r->infra) {
        return;
    }
    if (ds->sel && !newer(seq, entry_by(ds, ds->sel)->seq)) {
        push_urgent(r, d);
        trickle_reset(r);
        return;
    }
    tsim_time t = now(r);
    if (ds->sel && hops > 1 &&
        !(ds->asked >= 0 && ds->asked_seq == seq && t - ds->asked < r->config.request_interval)) {
        ds->asked = t;
        ds->asked_seq = seq;
        request(r, d, seq, slot(r, ds->sel)->id, (uint8_t)(hops - 1));
    }
}

static void on_request(struct router *r, const uint8_t *b, uint32_t len) {
    if (len < REQUEST_HEAD) {
        return;
    }
    uint32_t next = get32(b + 1);
    uint8_t n = b[5];
    if ((next != BROADCAST_HOP && next != r->self) || REQUEST_HEAD + (uint32_t)n * ASK_LEN > len) {
        return;
    }
    for (const uint8_t *p = b + REQUEST_HEAD; n > 0; n--, p += ASK_LEN) {
        on_ask(r, get32(p), get16(p + 4), p[6]);
    }
}

/* --- Next hops and their implicit acknowledgements --- */

/* A frame never reached the neighbour in slot `s`: evidence against the link, as good as HOP_MISS
 * of its announces missed. A link that keeps losing frames soon costs more than another, or more
 * than etx_max allows; one that lost a frame to a busy moment recovers as its announces come in. */
static void missed(struct router *r, uint16_t s) {
    struct neighbour *n = slot(r, s);
    double room = r->config.tx_dbm - r->config.tx_min_dbm;
    n->boost = n->boost + r->config.step_db > room ? room : n->boost + r->config.step_db;
    n->history = (uint16_t)(n->history << HOP_MISS);
    n->span = HISTORY;
    uint16_t was = n->cost;
    n->cost = link_cost(r, n);
    if (n->cost != was) {
        n->cost_used = n->cost;
        reselect_through(r, s);
    }
    if (r->changed) {
        trickle_reset(r);
        r->changed = false;
    }
}

static void arm_hops(struct router *r) {
    tsim_time next = -1;
    for (size_t i = 0; i < r->hop_count; i++) {
        if (r->hops[i].due >= 0 && (next < 0 || r->hops[i].due < next)) {
            next = r->hops[i].due;
        }
    }
    if (next < 0) {
        tsim_timer_stop(r->hop_timer);
    } else {
        tsim_time t = now(r);
        tsim_timer_start(r->hop_timer, next > t ? next - t : 0);
    }
}

static void drop_hop(struct router *r, size_t i) { r->hops[i] = r->hops[--r->hop_count]; }

/* Sends a data or acknowledgement frame to its next hop, and, if `listen`, waits to hear it passed
 * on. */
/* Returns the frame's handle, or 0 if the queue refused it. */
static uint64_t send_hop(struct router *r, const struct tsim_tx *tx, bool listen) {
    uint64_t handle = tsim_node_send(r->node, tx);
    if (!handle || !listen) {
        return handle;
    }
    if (r->hop_count == r->hop_cap) {
        size_t cap = r->hop_cap ? 2 * r->hop_cap : 8;
        struct hop *grown = realloc(r->hops, cap * sizeof *grown);
        if (!grown) {
            return handle; /* queued, but not listened for */
        }
        r->hops = grown;
        r->hop_cap = cap;
    }
    const uint8_t *b = tx->bytes;
    r->hops[r->hop_count++] = (struct hop){
        .handle = handle,
        .due = -1,
        .type = b[0],
        .next = get32(b + 1),
        .src = get32(b + 5),
        .dst = get32(b + 9),
        .id = get32(b + 13),
        .hops = b[17],
        .tx = *tx,
    };
    return handle;
}

static void hop_fire(void *ctx) {
    struct router *r = ctx;
    tsim_time t = now(r);
    for (size_t i = 0; i < r->hop_count;) {
        struct hop *h = &r->hops[i];
        if (h->due < 0 || h->due > t) {
            i++;
            continue;
        }
        if (h->tries < r->config.hop_retries) {
            h->tries++;
            h->due = -1;
            if (r->config.power) {
                double p = ceil(h->tx.tx_dbm + r->config.step_db), hi = floor(r->config.tx_dbm);
                h->tx.tx_dbm = p > hi ? hi : p;
                h->tx.bytes[18] = (uint8_t)(int8_t)lround(h->tx.tx_dbm);
            }
            h->handle = tsim_node_send(r->node, &h->tx);
            if (h->handle) {
                i++;
                continue;
            }
            drop_hop(r, i);
            continue;
        }
        uint32_t next = h->next;
        drop_hop(r, i);
        if (r->slot_of[next]) {
            missed(r, r->slot_of[next]);
        }
    }
    arm_hops(r);
}

/* A queued frame of this node's was taken back before it went. If it was a message's attempt the
 * source was waiting to send - a retry that an earlier copy, heard passed on, made needless - the
 * wait for the acknowledgement starts now, as it would have when the frame went: a withdrawn frame
 * never goes, and the message must not wait on it for ever. */
static void withdrawn(struct router *r, uint64_t handle) {
    for (struct awaiting *a = r->awaiting; a; a = a->next) {
        if (a->handle == handle) {
            a->handle = 0;
            tsim_timer_start(a->timer, a->wait);
            return;
        }
    }
}

/* Something passed on what a hop of this node's was waiting to hear: a data or acknowledgement
 * frame, one hop further on. */
static void overheard(struct router *r, const uint8_t *b) {
    uint8_t type = b[0], hops = b[17];
    uint32_t src = get32(b + 5), dst = get32(b + 9), id = get32(b + 13);
    for (size_t i = 0; i < r->hop_count;) {
        struct hop *h = &r->hops[i];
        bool passed = h->type == type && h->src == src && h->id == id && hops + 1 == h->hops;
        bool answered = h->type == TYPE_DATA && type == TYPE_ACK && h->next == h->dst &&
                        src == h->dst && dst == h->src && id == h->id;
        if (passed || answered) {
            if (h->due < 0 && tsim_node_cancel(r->node, h->handle)) {
                withdrawn(r, h->handle);
            }
            uint16_t s = r->slot_of[h->next];
            if (s && slot(r, s)->boost > 0) {
                slot(r, s)->boost = slot(r, s)->boost > 1 ? slot(r, s)->boost - 1 : 0;
            }
            drop_hop(r, i);
            continue;
        }
        i++;
    }
    arm_hops(r);
}

/* --- Messages --- */

static void hold(struct router *r, const struct tsim_tx *tx, uint64_t key, bool listen,
                 double airtimes);

/* A data or acknowledgement frame towards `dst`, along the selected route: at once if this node
 * made it, and after a jitter if it answers one received, whose sender's floor is `back` (NAN for
 * none). Returns false with no route; with one, `queued`, if given, is the handle of a frame this
 * node made, or 0 if the queue refused it. */
static bool route_frame(struct router *r, uint8_t type, uint32_t src, uint32_t dst, uint32_t id,
                        uint8_t hops, const uint8_t *content, uint32_t len,
                        enum tsim_purpose purpose, uint64_t carries, double back,
                        uint64_t *queued) {
    struct dest *ds = &r->dest[dst];
    if (!ds->sel) {
        return false;
    }
    const struct neighbour *n = slot(r, ds->sel);
    uint32_t next = n->id;
    struct tsim_tx tx = frame(r, purpose,
                              type == TYPE_ACK               ? PRIORITY_CONTROL
                              : purpose == TSIM_PURPOSE_DATA ? PRIORITY_DATA
                                                             : PRIORITY_RELAY);
    tx.bytes[0] = type;
    put32(tx.bytes + 1, next);
    put32(tx.bytes + 5, src);
    put32(tx.bytes + 9, dst);
    put32(tx.bytes + 13, id);
    tx.bytes[17] = hops;
    if (r->config.power) {
        tx.tx_dbm = power_for(r, n, back);
        tx.bytes[18] = (uint8_t)(int8_t)lround(tx.tx_dbm);
    }
    if (len) {
        memcpy(tx.bytes + r->data_head, content, len);
    }
    tx.len = r->data_head + len;
    if (carries) {
        tx.carries = carries;
        tx.carries_at = r->data_head;
    }
    /* The last hop of an acknowledgement has nothing to hear. */
    bool listen = !(type == TYPE_ACK && next == dst);
    if (purpose == TSIM_PURPOSE_DATA) {
        uint64_t handle = send_hop(r, &tx, listen);
        if (queued) {
            *queued = handle;
        }
    } else {
        hold(r, &tx, 0, listen, r->config.jitter);
    }
    return true;
}

static void forget_awaiting(struct awaiting *a) {
    struct router *r = a->owner;
    for (struct awaiting **p = &r->awaiting; *p; p = &(*p)->next) {
        if (*p == a) {
            *p = a->next;
            break;
        }
    }
    tsim_timer_destroy(a->timer);
    free(a);
}

static void send_attempt(struct awaiting *a) {
    struct router *r = a->owner;
    struct dest *ds = &r->dest[a->dst];
    tsim_time wait = r->config.ack_wait;
    uint64_t handle = 0;
    if (route_frame(r, TYPE_DATA, r->self, a->dst, a->id, r->config.hop_max, a->content, a->len,
                    TSIM_PURPOSE_DATA, a->id, NAN, &handle)) {
        uint16_t metric = total(entry_by(ds, ds->sel)->metric, slot(r, ds->sel)->cost);
        wait += (tsim_time)(r->config.ack_factor * metric * (double)TSIM_MS(1));
        if (handle) {
            /* The wait starts when the frame goes: however long it queues behind other traffic,
             * no second copy joins it, and the message is not given up while one still waits. */
            a->handle = handle;
            a->wait = wait;
            return;
        }
        /* Refused by a full queue: tried again after the wait, as if it had been lost. */
    } else {
        ds->asked = -1; /* a message waits on it: ask now */
        starved(r, a->dst);
    }
    tsim_timer_start(a->timer, wait);
}

/* The routing is done with a message: answered, or given up. Its attempts still queued are taken
 * back, and those sent are no longer listened for or sent again, so nothing of it goes on the air
 * after the application has been told. */
static void finish(struct awaiting *a) {
    struct router *r = a->owner;
    for (size_t i = 0; i < r->hop_count;) {
        struct hop *h = &r->hops[i];
        if (h->type == TYPE_DATA && h->src == r->self && h->id == a->id) {
            if (h->due < 0) {
                tsim_node_cancel(r->node, h->handle);
            }
            drop_hop(r, i);
            continue;
        }
        i++;
    }
    arm_hops(r);
    tsim_node_finished(r->node, a->id);
    forget_awaiting(a);
}

static void ack_timeout(void *ctx) {
    struct awaiting *a = ctx;
    struct router *r = a->owner;
    if (a->attempt >= r->config.retries) {
        finish(a);
        return;
    }
    a->attempt++;
    send_attempt(a);
}

static void arm_out(struct router *r) {
    tsim_time next = -1;
    for (size_t i = 0; i < r->held_count; i++) {
        if (r->held[i].handle == 0 && (next < 0 || r->held[i].due < next)) {
            next = r->held[i].due;
        }
    }
    if (next < 0) {
        tsim_timer_stop(r->out_timer);
    } else {
        tsim_time t = now(r);
        tsim_timer_start(r->out_timer, next > t ? next - t : 0);
    }
}

static void out_fire(void *ctx) {
    struct router *r = ctx;
    tsim_time t = now(r);
    for (size_t i = 0; i < r->held_count;) {
        struct held *h = &r->held[i];
        if (h->handle == 0 && h->due <= t) {
            if (h->listen) {
                struct tsim_tx tx = h->tx;
                r->held[i] = r->held[--r->held_count];
                send_hop(r, &tx, true);
                continue;
            }
            h->handle = tsim_node_send(r->node, &h->tx);
            if (h->handle == 0 || h->key == 0) {
                r->held[i] = r->held[--r->held_count];
                continue;
            }
        }
        i++;
    }
    arm_out(r);
}

/* Holds `tx` for a random time up to `airtimes` of its own airtime before queueing it, so the
 * nodes that heard the same frame do not all answer it at the same instant: the MAC under this
 * routing may well send the moment the channel is clear. */
static void hold(struct router *r, const struct tsim_tx *tx, uint64_t key, bool listen,
                 double airtimes) {
    if (r->held_count == r->held_cap) {
        size_t cap = r->held_cap ? 2 * r->held_cap : 8;
        struct held *grown = realloc(r->held, cap * sizeof *grown);
        if (!grown) {
            return; /* out of memory: not sent */
        }
        r->held = grown;
        r->held_cap = cap;
    }
    double window = airtimes * (double)tsim_lora_airtime(&tx->lora, tx->len);
    r->held[r->held_count++] = (struct held){
        .key = key,
        .listen = listen,
        .due = now(r) + (tsim_time)(tsim_rng_unit(&r->rng) * window),
        .heard = 1,
        .tx = *tx,
    };
    arm_out(r);
}

/* Whether a data or acknowledgement frame is still on its way out of this node: held, or sent to
 * a next hop not yet heard passing it on. */
static bool passing_on(const struct router *r, uint8_t type, uint32_t src, uint32_t id) {
    for (size_t i = 0; i < r->held_count; i++) {
        const uint8_t *b = r->held[i].tx.bytes;
        if (b[0] == type && get32(b + 5) == src && get32(b + 13) == id) {
            return true;
        }
    }
    for (size_t i = 0; i < r->hop_count; i++) {
        const struct hop *h = &r->hops[i];
        if (h->type == type && h->src == src && h->id == id) {
            return true;
        }
    }
    return false;
}

static bool router_originate(void *self, const struct tsim_message *msg) {
    struct router *r = self;
    if (msg->id > UINT32_MAX) {
        return false;
    }
    if (msg->dst == TSIM_BROADCAST) {
        if (BCAST_HEAD + msg->len > FRAME_MAX) {
            return false;
        }
        struct tsim_tx tx = frame(r, TSIM_PURPOSE_DATA, PRIORITY_DATA);
        tx.bytes[0] = TYPE_BCAST;
        put32(tx.bytes + 1, r->self);
        put32(tx.bytes + 5, (uint32_t)msg->id);
        tx.bytes[9] = (uint8_t)(r->config.bcast_hops + 1);
        memcpy(tx.bytes + BCAST_HEAD, msg->content, msg->len);
        tx.len = BCAST_HEAD + msg->len;
        tx.carries = msg->id;
        tx.carries_at = BCAST_HEAD;
        mark_seen(r, frame_key(TYPE_BCAST, r->self, (uint32_t)msg->id));
        tsim_node_send(r->node, &tx);
        tsim_node_finished(r->node, msg->id);
        return true;
    }
    if (r->data_head + msg->len > FRAME_MAX) {
        return false;
    }
    struct awaiting *a = calloc(1, sizeof *a);
    if (a) {
        a->timer = tsim_timer_create(r->node, ack_timeout, a);
    }
    if (!a || !a->timer) {
        free(a);
        return false;
    }
    a->owner = r;
    a->id = (uint32_t)msg->id;
    a->dst = msg->dst;
    a->len = msg->len;
    memcpy(a->content, msg->content, msg->len);
    a->next = r->awaiting;
    r->awaiting = a;
    send_attempt(a);
    return true;
}

static void on_data(struct router *r, const uint8_t *b, uint32_t len, double snr) {
    uint8_t type = b[0], hops = b[17];
    double back = r->config.power ? floor_of(r, b[18], snr) : NAN;
    uint32_t next = get32(b + 1), src = get32(b + 5), dst = get32(b + 9), id = get32(b + 13);
    if (src >= r->nodes || dst >= r->nodes) {
        return;
    }
    overheard(r, b);
    if (type == TYPE_ACK && dst == r->self) {
        for (struct awaiting *a = r->awaiting; a; a = a->next) {
            if (a->id == id && a->dst == src) {
                finish(a);
                break;
            }
        }
        return;
    }
    if (next != r->self) {
        return;
    }
    if (type == TYPE_DATA && dst == r->self) {
        tsim_node_deliver(r->node, id);
        if (!route_frame(r, TYPE_ACK, r->self, src, id, r->config.hop_max, NULL, 0,
                         TSIM_PURPOSE_CONTROL, 0, back, NULL)) {
            starved(r, src);
        }
        return;
    }
    if (!r->infra || hops <= 1) {
        return;
    }
    uint64_t key = frame_key(type, src, id);
    if (seen(r, key) && passing_on(r, type, src, id)) {
        return; /* still trying to pass it on: the hop before will hear that */
    }
    mark_seen(r, key);
    bool data = type == TYPE_DATA;
    if (!route_frame(r, type, src, dst, id, (uint8_t)(hops - 1), b + r->data_head,
                     len - r->data_head, data ? TSIM_PURPOSE_RELAY : TSIM_PURPOSE_CONTROL,
                     data ? id : 0, back, NULL)) {
        /* Sent here, so the hop before still has the route: if it had it from this node, its
         * retraction never got there. Say it again. */
        struct dest *ds = &r->dest[dst];
        if (ds->has_fd && !ds->advertised && !ds->urgent) {
            if (!ds->retracts) {
                ds->retracts = 1;
                r->retracting++;
            }
            push_urgent(r, dst);
            trickle_reset(r);
        }
        starved(r, dst);
    }
}

static void on_bcast(struct router *r, const uint8_t *b, uint32_t len) {
    uint32_t src = get32(b + 1), id = get32(b + 5);
    uint8_t hops = b[9];
    uint64_t key = frame_key(TYPE_BCAST, src, id);
    if (seen(r, key)) {
        for (size_t i = 0; i < r->held_count; i++) {
            struct held *h = &r->held[i];
            if (h->key != key) {
                continue;
            }
            if (h->heard < UINT8_MAX) {
                h->heard++;
            }
            if (r->config.bcast_cancel && h->heard >= r->config.bcast_cancel &&
                (h->handle == 0 || tsim_node_cancel(r->node, h->handle))) {
                r->held[i] = r->held[--r->held_count];
                arm_out(r);
            }
            break;
        }
        return;
    }
    mark_seen(r, key);
    tsim_node_deliver(r->node, id);
    if (!r->infra || hops <= 1 || (r->config.bcast_cancel && r->config.bcast_cancel <= 1)) {
        return;
    }
    struct tsim_tx tx = frame(r, TSIM_PURPOSE_RELAY, PRIORITY_RELAY);
    memcpy(tx.bytes, b, len);
    tx.bytes[9] = (uint8_t)(hops - 1);
    tx.len = len;
    tx.carries = id;
    tx.carries_at = BCAST_HEAD;
    hold(r, &tx, key, false, r->config.bcast_window);
}

static void router_rx(void *self, const struct tsim_rx *rx) {
    struct router *r = self;
    if (rx->len < 1) {
        return;
    }
    switch (rx->bytes[0]) {
    case TYPE_ANNOUNCE:
        on_announce(r, rx->bytes, rx->len, rx->snr_db);
        return;
    case TYPE_REQUEST:
        on_request(r, rx->bytes, rx->len);
        return;
    case TYPE_DATA:
    case TYPE_ACK:
        if (rx->len >= r->data_head) {
            on_data(r, rx->bytes, rx->len, rx->snr_db);
        }
        return;
    case TYPE_BCAST:
        if (rx->len >= BCAST_HEAD) {
            on_bcast(r, rx->bytes, rx->len);
        }
        return;
    default:
        return;
    }
}

static void router_tx_done(void *self, uint64_t handle) {
    struct router *r = self;
    if (handle == r->ann_handle) {
        /* How long it waited, remembered for the promises to come and forgotten an eighth an
         * announce. */
        tsim_time waited = now(r) - r->ann_queued, kept = r->mac_wait - r->mac_wait / 8;
        r->mac_wait = waited > kept ? waited : kept;
        r->ann_handle = 0;
        return;
    }
    for (struct awaiting *a = r->awaiting; a; a = a->next) {
        if (a->handle == handle) {
            a->handle = 0;
            tsim_timer_start(a->timer, a->wait);
            break;
        }
    }
    for (size_t i = 0; i < r->hop_count; i++) {
        struct hop *h = &r->hops[i];
        if (h->handle == handle) {
            h->due = now(r) + r->config.hop_wait + 2 * tsim_lora_airtime(&h->tx.lora, h->tx.len);
            arm_hops(r);
            return;
        }
    }
    for (size_t i = 0; i < r->held_count; i++) {
        if (r->held[i].handle == handle) {
            r->held[i] = r->held[--r->held_count];
            return;
        }
    }
}

/* --- Housekeeping and life --- */

/* How often a node looks over its links for neighbours gone quiet. */
static tsim_time house_period(const struct router *r) {
    tsim_time p = r->config.neighbour_timeout / 4;
    return (p < imax(r) ? p : imax(r)) + 1;
}

/* Forgets the neighbours unheard for neighbour_timeout - and for two of their promises - and
 * charges the rest for the announces they promised and have not sent. */
static void house_fire(void *ctx) {
    struct router *r = ctx;
    tsim_time t = now(r);
    for (size_t i = 0; i < r->nb_count; i++) {
        struct neighbour *n = &r->nb[i];
        uint16_t s = (uint16_t)(i + 1);
        if (!n->used) {
            continue;
        }
        /* Never before it has let two of its promises pass: a node may go quiet that long. */
        if (t - n->heard > r->config.neighbour_timeout && t - n->heard > 2 * n->promise) {
            forget(r, s);
            continue;
        }
        uint16_t was = n->cost;
        n->cost = link_cost(r, n);
        bool flipped = (was == INF) != (n->cost == INF);
        if (flipped || fabs((double)n->cost - (double)n->cost_used) >
                           r->config.change * (double)n->cost_used) {
            n->cost_used = n->cost;
            r->changed |= flipped;
            reselect_through(r, s);
        }
    }
    if (r->changed) {
        trickle_reset(r);
        r->changed = false;
    }
    node_power(r);
    tsim_timer_start(r->house, house_period(r));
}

static void router_start(void *self) {
    struct router *r = self;
    trickle_begin(r);
    tsim_timer_start(r->house, house_period(r));
}

static void router_destroy(void *self) {
    struct router *r = self;
    if (!r) {
        return;
    }
    while (r->awaiting) {
        forget_awaiting(r->awaiting);
    }
    tsim_timer_destroy(r->trickle);
    tsim_timer_destroy(r->cap_timer);
    tsim_timer_destroy(r->hop_timer);
    tsim_timer_destroy(r->out_timer);
    tsim_timer_destroy(r->house);
    tsim_timer_destroy(r->request_timer);
    tsim_timer_destroy(r->ask_timer);
    free(r->starving);
    free(r->asks);
    free(r->dest);
    free(r->slot_of);
    free(r->nb);
    free(r->urgent);
    free(r->hops);
    free(r->held);
    free(r);
}

static void *router_create(struct tsim_node *node, const void *config) {
    const struct tsim_distvec_config *c = config;
    if (tsim_distvec_check(c)) {
        return NULL;
    }
    struct router *r = calloc(1, sizeof *r);
    if (!r) {
        return NULL;
    }
    r->node = node;
    r->self = tsim_node_index(node);
    r->nodes = tsim_node_count(node);
    r->infra = tsim_nodeset_contains(c->relays, r->self) == 1;
    r->config = *c;
    r->ann_head = ANNOUNCE_HEAD + (c->power ? 1 : 0);
    r->data_head = DATA_HEAD + (c->power ? 1 : 0);
    node_power(r);
    tsim_node_rng(node, TSIM_STREAM_ROUTING, &r->rng);
    r->ref_ms = (double)tsim_lora_airtime(&c->lora, c->ref_len) / (double)TSIM_MS(1);
    r->dest = calloc(r->nodes, sizeof *r->dest);
    r->slot_of = calloc(r->nodes, sizeof *r->slot_of);
    r->interval = c->imin;
    double head = (double)tsim_lora_airtime(&c->lora, FRAME_MAX);
    bucket_init(&r->announces, c->cap * (1 - c->request_share), c->cap_window, head);
    bucket_init(&r->requests, c->cap * c->request_share, c->cap_window, head);
    r->trickle = tsim_timer_create(node, trickle_fire, r);
    r->cap_timer = tsim_timer_create(node, cap_fire, r);
    r->hop_timer = tsim_timer_create(node, hop_fire, r);
    r->out_timer = tsim_timer_create(node, out_fire, r);
    r->house = tsim_timer_create(node, house_fire, r);
    r->request_timer = tsim_timer_create(node, request_fire, r);
    r->ask_timer = tsim_timer_create(node, ask_fire, r);
    if (!r->dest || !r->slot_of || !r->trickle || !r->cap_timer || !r->hop_timer || !r->out_timer ||
        !r->house || !r->request_timer || !r->ask_timer) {
        router_destroy(r);
        return NULL;
    }
    for (uint32_t d = 0; d < r->nodes; d++) {
        r->dest[d].asked = -1;
    }
    return r;
}

const struct tsim_routing tsim_distvec = {
    .name = "distvec",
    .create = router_create,
    .destroy = router_destroy,
    .start = router_start,
    .originate = router_originate,
    .rx = router_rx,
    .tx_done = router_tx_done,
    .reports_finished = true,
};

/* --- For tests and reports --- */

bool tsim_distvec_route(const void *self, uint32_t dst, uint32_t *next, uint16_t *metric) {
    const struct router *r = self;
    if (dst >= r->nodes || dst == r->self || !r->dest[dst].sel) {
        return false;
    }
    const struct dest *ds = &r->dest[dst];
    const struct neighbour *n = &r->nb[ds->sel - 1];
    for (int i = 0; i < ROUTES; i++) {
        if (ds->e[i].slot == ds->sel) {
            if (next) {
                *next = n->id;
            }
            if (metric) {
                *metric = total(ds->e[i].metric, n->cost);
            }
            return true;
        }
    }
    return false;
}

uint32_t tsim_distvec_neighbours(const void *self) {
    const struct router *r = self;
    uint32_t count = 0;
    for (size_t i = 0; i < r->nb_count; i++) {
        count += r->nb[i].used && r->nb[i].cost != INF;
    }
    return count;
}

tsim_time tsim_distvec_promise(const void *self) { return ((const struct router *)self)->promised; }

uint32_t tsim_distvec_round(const void *self) { return ((const struct router *)self)->round; }

double tsim_distvec_power(const void *self, uint32_t nb) {
    const struct router *r = self;
    if (nb >= r->nodes || !r->slot_of[nb]) {
        return r->config.power ? floor(r->config.tx_dbm) : r->config.tx_dbm;
    }
    return power_for(r, &r->nb[r->slot_of[nb] - 1], NAN);
}

double tsim_distvec_node_power(const void *self) { return ((const struct router *)self)->node_dbm; }

tsim_time tsim_distvec_interval(const void *self) {
    return ((const struct router *)self)->interval;
}
