#include "tsim/distvec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/channel.h"
#include "tsim/metrics.h"
#include "tsim/nodeset.h"
#include "tsim/phy.h"
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
#define TYPE_RREQ 0x06
#define TYPE_RREP 0x07
#define TYPE_PROBE 0x08
#define TYPE_PROBE_ACK 0x09

#define ANNOUNCE_HEAD 16
#define IHU_LEN 5
#define ROUTE_LEN 8
#define REQUEST_HEAD 6
#define ASK_LEN 7
#define ASKS_MAX ((FRAME_MAX - REQUEST_HEAD) / ASK_LEN)
#define DATA_HEAD 18
#define ACK_LEN 18
#define BCAST_HEAD 10
/* With demand routes, a data, acknowledgement or reply frame's route to its source, and a route
 * request's length: see the header. */
#define TRAIL_LEN 10
#define RREQ_LEN 27
#define PROBE_LEN 10  /* a probe or its answer */
#define PARKED_MAX 16 /* frames a relay holds while it asks for a route */
#define RREQ_SEQ 0x01 /* a route request's flag: it asks for the target's seq */
/* The longest IHU round an announce can tell, in its two bytes. */
#define ROUND_MAX 32767
/* The most announces a neighbour may go without an IHU, whatever its round: half the 16-bit count,
 * so that any announce over the next 32768 finds it expired. Capped at the count's top, only the
 * one announce reaching it exactly could, and with that one lost the count would wrap and keep the
 * link for 65536 more. */
#define IHU_AGE_MAX 0x7FFFu

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
    tsim_time until; /* with demand routes, when it is gone unless heard of again; 0 for never */
};

struct dest {
    struct entry e[ROUTES];
    uint16_t sel; /* the selected route's slot plus 1, or 0 */
    bool has_fd;
    bool advertised;  /* a finite route to it has been announced, and not retracted since */
    bool urgent;      /* on the list of changed routes */
    bool listed;      /* in the frame being built */
    bool leaf;        /* heard announcing itself as a leaf */
    bool had;         /* has had a selected route, so losing one is an outage */
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
    tsim_time promise;     /* how soon it said it would announce again */
    uint16_t ihu_seq;      /* the announce that last named this node */
    double floor;          /* the quietest it decodes this node at, in dBm; NAN until known */
    bool up;               /* with links by strength: its margins, both ways, put it in use */
    uint8_t lost;          /* with links by strength: hops lost to it since it was last heard */
    double boost;          /* power control: dB more for frames to it, for hops it has lost */
    bool probing;          /* the liveness probe: asked whether it is there, and not yet answered */
    uint8_t probes;        /* probes sent it unanswered */
    uint64_t probe_handle; /* its probe in the queue, until it goes; 0 for none */
    tsim_time probe_due;   /* when the next probe goes, or the last is given up on; -1 queued */
    bool mute;             /* left the probe unanswered: unused until something is heard from it */
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
    bool routeless;  /* its last attempt found no route, and waits on one */
    tsim_time wait;  /* how long to wait for the acknowledgement once it has */
    uint32_t len;
    uint8_t content[FRAME_MAX];
};

/* With demand routes, a frame to pass on that waits for the route its relay asked for. */
struct parked {
    uint32_t target;
    tsim_time until;
    double back;
    uint32_t len;
    uint8_t bytes[FRAME_MAX];
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
    uint32_t parent; /* a leaf's, with parent_oracle: a neighbour's id, or TSIM_DISTVEC_NO_PARENT */
    struct tsim_distvec_stats stats;
    uint32_t unrouted; /* destinations it announces, has had a route to and has none to now */
    tsim_time acc_at;  /* when unrouted and the urgent list were last counted into stats */
    uint32_t ann_head; /* an announce's head, and a data frame's: longer with power control */
    uint32_t data_head;
    uint32_t trail_at; /* with demand routes, where a data frame's route to its source goes */
    uint32_t rreq_id;  /* the last route request this node made */
    double node_dbm;   /* what frames for every neighbour go at */
    const struct tsim_distvec_oracle *oracle; /* routes handed down, or NULL */
    const struct tsim_distvec_oracle *truth;  /* with links oracle: whose links are used */
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
    struct tsim_timer *seq_timer;
    struct tsim_timer *park_timer; /* with demand routes: when parked frames next look for routes */
    struct tsim_timer *probe_timer;
    uint32_t probing; /* neighbours being probed */
    struct parked *parked;
    size_t parked_count;
};

struct tsim_distvec_config tsim_distvec_default(uint16_t channel, const struct tsim_lora *lora,
                                                double tx_dbm) {
    return (struct tsim_distvec_config){
        .channel = channel,
        .lora = *lora,
        .tx_dbm = tx_dbm,
        .relays = "all",
        .leaves = TSIM_DISTVEC_LEAVES_ROUTED,
        .routes = TSIM_DISTVEC_ROUTES_PROACTIVE,
        .route_ttl = TSIM_S(10 * 60),
        .req_hops = 16,
        .req_cancel = 2,
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
        .ihu_rounds = 8,
        .ref_len = 32,
        .etx_max = 32,
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
        .oracle_margin_db = 3,
        .link_margin_db = 3,
        .link_band_db = 3,
        .dead_hops = 24,
        .silent_max = TSIM_S(24 * 3600),
        .probe_tries = 6,
        .probe_wait = TSIM_S(5),
    };
}

static double promise_secs(const struct tsim_distvec_config *c, tsim_time interval, tsim_time rest);

const char *tsim_distvec_check(const struct tsim_distvec_config *c) {
    if (memchr(c->relays, 0, sizeof c->relays) == NULL || tsim_nodeset_contains(c->relays, 0) < 0) {
        return "relays is not all, or node numbers and ranges";
    }
    if (c->relay_pick > TSIM_DISTVEC_PICK_CDS ||
        (c->relay_pick != TSIM_DISTVEC_PICK_LIST && c->relay_pick != TSIM_DISTVEC_PICK_CDS &&
         c->relay_count == 0)) {
        return "relay_pick is not list, degree, spaced or cds, or relay_count is 0 for one but cds";
    }
    if (c->leaves > TSIM_DISTVEC_LEAVES_PARENT_ORACLE) {
        return "leaves is not routed or parent_oracle";
    }
    /* Frames carry a route's life in whole seconds: under one, it would go as none. */
    if (c->routes > TSIM_DISTVEC_ROUTES_DEMAND || c->route_ttl < TSIM_S(1) || c->req_hops < 1) {
        return "routes is not proactive or demand, route_ttl is under 1 s, or req_hops is 0";
    }
    if (tsim_lora_airtime(&c->lora, c->ref_len) < 0) {
        return "ref_len has no airtime at the radio's modulation";
    }
    if (c->imin <= 0 || c->doublings > 16 || c->imin > (INT64_MAX >> 17)) {
        return "imin is not above 0, or doublings is over 16";
    }
    if (c->neighbour_timeout <= 0 || c->cap_window <= 0 || c->request_interval < 0 ||
        c->seq_period < 0 || c->hop_wait < 0 || c->ack_wait <= 0) {
        return "a time is out of range";
    }
    if (!(c->cap > 0 && c->cap <= 1) || !(c->request_share > 0 && c->request_share < 1)) {
        return "cap is not above 0 and at most 1, or request_share is not between 0 and 1";
    }
    if (c->burst < 1 || c->burst > 16 || c->ihu_max > 48 || c->ihu_rounds < 2 ||
        c->ihu_rounds > 64) {
        return "burst is not 1 to 16, ihu_max is over 48, or ihu_rounds is not 2 to 64";
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
    if (c->links > TSIM_DISTVEC_LINKS_STRENGTH) {
        return "links is not sensed, oracle or strength";
    }
    /* Checked only where used, as the oracle's are: a config written before them may leave them 0.
     */
    bool strength = c->links == TSIM_DISTVEC_LINKS_STRENGTH && !c->oracle;
    if (strength && !(c->link_margin_db >= 0 && c->link_margin_db <= 60 && c->link_band_db >= 0 &&
                      c->link_band_db <= 60)) {
        return "link_margin or link_band is out of range";
    }
    if (strength && (c->dead_hops < 1 || c->silent_max <= 0)) {
        return "dead_hops is 0, or silent_max is not above 0";
    }
    if (strength && c->probe_hops &&
        (c->probe_tries < 1 || c->probe_tries > 32 || c->probe_wait <= 0)) {
        return "with probe_hops, probe_tries is not 1 to 32, or probe_wait is not above 0";
    }
    bool oracle = c->oracle || c->links == TSIM_DISTVEC_LINKS_ORACLE;
    if (oracle && !(c->oracle_margin_db >= 0 && c->oracle_margin_db <= 60)) {
        return "oracle_margin is out of range";
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

/* Whether leaves are reached through their parents rather than by routes to them. */
static bool by_parent(const struct router *r) {
    return r->config.leaves == TSIM_DISTVEC_LEAVES_PARENT_ORACLE;
}

/* Whether routes come on demand. The oracle's never do: it is handed them. */
static bool demand(const struct router *r) {
    return r->config.routes == TSIM_DISTVEC_ROUTES_DEMAND && !r->oracle;
}

/* Whether this node announces its route to `d`: infrastructure does, but never to a leaf when
 * leaves are reached through their parents, and with demand routes nobody does. */
static bool announces(const struct router *r, uint32_t d) {
    return r->infra && !(by_parent(r) && r->dest[d].leaf) && !demand(r);
}

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

/* What a frame says it went at: rounded up, so a receiver reckons its floor no lower than it is -
 * tx_dbm, the one power that need not be whole, at worst under a dB high. */
static uint8_t power_byte(double dbm) { return (uint8_t)(int8_t)ceil(dbm); }

/* What a frame to a neighbour goes at: its floor and margin, and loud enough too for the node the
 * frame answers, whose floor is `back` - NAN for none. */
static double power_for(const struct router *r, const struct neighbour *n, double back) {
    const struct tsim_distvec_config *c = &r->config;
    if (!c->power) {
        return c->tx_dbm;
    }
    if (isnan(n->floor)) {
        return c->tx_dbm;
    }
    double p = n->floor + c->margin_db + n->boost;
    if (!isnan(back) && back + c->margin_db > p) {
        p = back + c->margin_db;
    }
    double lo = ceil(c->tx_min_dbm);
    p = ceil(p);
    return p < lo ? lo : p > c->tx_dbm ? c->tx_dbm : p;
}

/* What a frame to `dst` goes at under the oracle: what its next hop needs, and loud enough too for
 * the node the frame answers, as power_for(). */
static double oracle_power(const struct router *r, uint32_t dst, double back) {
    const struct tsim_distvec_config *c = &r->config;
    double p = r->oracle->route[(size_t)r->self * r->nodes + dst].dbm;
    if (!isnan(back) && ceil(back + c->margin_db) > p) {
        p = ceil(back + c->margin_db);
    }
    double lo = ceil(c->tx_min_dbm);
    return p < lo ? lo : p > c->tx_dbm ? c->tx_dbm : p;
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
    double hi = c->tx_dbm, lo = ceil(c->tx_min_dbm);
    r->node_dbm = hi;
    if (!c->power_k) {
        return;
    }
    double low[UINT8_MAX]; /* the k lowest floors, in order */
    unsigned have = 0, k = c->power_k;
    for (size_t i = 0; i < r->nb_count; i++) {
        const struct neighbour *n = &r->nb[i];
        if (!n->used || isnan(n->floor) || (by_parent(r) && !n->infra) ||
            (have == k && n->floor >= low[k - 1])) {
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

/* What a frame from this node needs to be decoded at `id`, in dBm, by the link oracle's mean loss.
 */
static double true_need(const struct router *r, uint32_t id) {
    return r->truth->need[(size_t)r->self * r->nodes + id];
}

/* Whether the link oracle has a link between this node and `id`: each decodes the other. */
static bool true_link(const struct router *r, uint32_t id) {
    const struct tsim_distvec_oracle *o = r->truth;
    return o->need[(size_t)r->self * r->nodes + id] <= o->top &&
           o->need[(size_t)id * r->nodes + r->self] <= o->top;
}

/* Whether links are judged by strength. */
static bool by_strength(const struct router *r) {
    return r->config.links == TSIM_DISTVEC_LINKS_STRENGTH && !r->oracle;
}

/* What a frame went at, as a floor reckons it: the power byte at `power`, or without power control,
 * which leaves frames none, tx_dbm itself - any power, not only one a byte holds. */
static double sent_dbm(const struct router *r, const uint8_t *power) {
    return r->config.power ? (int8_t)*power : r->config.tx_dbm;
}

/* With links by strength, an IHU's byte is the margin its sender hears this node with, in whole
 * dB rounded down - never more than it measured - offset by 128 and kept off 0, which no IHU has.
 */
#define MARGIN_ZERO 128

static uint8_t margin_byte(double m) {
    double v = floor(m) + MARGIN_ZERO;
    return (uint8_t)(v < 1 ? 1 : v > UINT8_MAX ? UINT8_MAX : v);
}

/* With links by strength, the margin of a link: the least of how far below tx_dbm this node's
 * floor at the neighbour lies, and of the margin its IHU reports - -INFINITY without either. */
static double margin(const struct router *r, const struct neighbour *n) {
    double ours = isnan(n->floor) ? -INFINITY : r->config.tx_dbm - n->floor;
    double theirs = n->dr ? (double)n->dr - MARGIN_ZERO : -INFINITY;
    return ours < theirs ? ours : theirs;
}

/* With links by strength, whether the link is in use: once up with link_margin_db each way, until
 * it falls more than link_band_db below that. */
static void judge(const struct router *r, struct neighbour *n) {
    double m = margin(r, n), want = r->config.link_margin_db;
    n->up = m >= (n->up ? want - r->config.link_band_db : want);
}

static uint16_t link_cost(const struct router *r, const struct neighbour *n) {
    if (r->truth || by_strength(r)) {
        double cost = ceil(r->ref_ms); /* an ETX of 1 */
        bool up = r->truth ? true_link(r, n->id) : n->up && !n->mute;
        return !up ? INF : cost < 1 ? 1 : cost >= INF ? INF - 1 : (uint16_t)cost;
    }
    uint8_t got = heard_rate(r, n);
    if (n->dr == 0 || got == 0) {
        return INF; /* not heard both ways */
    }
    double etx = 1.0 / (got / 255.0 * (n->dr / 255.0));
    if (etx > r->config.etx_max) {
        return INF;
    }
    double cost = ceil((r->config.etx ? etx : 1.0) * r->ref_ms);
    return cost < 1 ? 1 : cost >= INF ? INF - 1 : (uint16_t)cost;
}

/* Whether a route to `d` through slot `s` may be used: the neighbour is heard both ways, and is
 * infrastructure unless it is the destination. */
static bool usable(struct router *r, uint16_t s, uint32_t d) {
    const struct neighbour *n = slot(r, s);
    return n->used && n->cost != INF && (n->infra || n->id == d);
}

/* A leaf's parent, with parent_oracle: of the infrastructure neighbours it can use, the one with
 * the best ETX, kept unless another beats it by `hysteresis` - written where every node can read
 * it. */
static void choose_parent(struct router *r) {
    if (r->infra || !by_parent(r)) {
        return;
    }
    const struct neighbour *cur =
        r->parent < r->nodes && r->slot_of[r->parent] ? slot(r, r->slot_of[r->parent]) : NULL;
    double best_q = 0, cur_q = 0; /* d_f d_r: the inverse of the link's ETX */
    uint32_t best = TSIM_DISTVEC_NO_PARENT;
    for (size_t i = 0; i < r->nb_count; i++) {
        const struct neighbour *n = &r->nb[i];
        if (!n->used || !n->infra || n->cost == INF) {
            continue;
        }
        /* With the link oracle or by strength, the margin the link has: above 0 for any used. */
        double q = r->truth ? r->truth->top - true_need(r, n->id) + 1
                   : by_strength(r)
                       ? margin(r, n) - r->config.link_margin_db + r->config.link_band_db + 1
                       : (double)heard_rate(r, n) * n->dr;
        if (n == cur) {
            cur_q = q;
        }
        if (q > best_q) {
            best_q = q;
            best = n->id;
        }
    }
    if (cur_q > 0 && !(cur_q < best_q * (1.0 - r->config.hysteresis))) {
        best = r->parent; /* not enough better to move */
    }
    r->parent = best;
    r->config.parents[r->self] = best;
}

/* A usable link that `cause` has just taken out of use, if `was` was its cost before: counted. */
static void link_down(struct router *r, const struct neighbour *n, uint16_t was,
                      enum tsim_distvec_down cause) {
    if (was == INF || n->cost != INF) {
        return;
    }
    r->stats.down[cause]++;
    if (!isnan(n->floor) && n->floor + r->config.oracle_margin_db <= r->config.tx_dbm) {
        r->stats.down_strong[cause]++;
    }
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

/* Whether a route is still there: with demand routes, not past its time. */
static bool alive(const struct router *r, const struct entry *e) {
    return e->until == 0 || e->until > now(r);
}

static void reselect(struct router *r, uint32_t d);

/* The selected route to `d`, or NULL for none. One past its time is reselected first, so a live
 * route cached beside it takes its place. */
static const struct entry *live_sel(struct router *r, uint32_t d) {
    struct dest *ds = &r->dest[d];
    const struct entry *e = ds->sel ? entry_by(ds, ds->sel) : NULL;
    if (e && !alive(r, e)) {
        reselect(r, d);
        e = ds->sel ? entry_by(ds, ds->sel) : NULL;
    }
    return e && alive(r, e) ? e : NULL;
}

/* The node whose route a frame for `dst` follows: `dst`, or with parent_oracle, the parent of a
 * leaf this node has no route to - TSIM_DISTVEC_NO_PARENT if it has none, and this node itself if
 * it is the parent but cannot reach the leaf. */
static uint32_t target_of(struct router *r, uint32_t dst) {
    if (!by_parent(r) || live_sel(r, dst)) {
        return dst;
    }
    return r->config.parents[dst];
}

/* With demand routes and parent_oracle, where a leaf sends what it has no route for: its parent,
 * if it can use the link. */
static struct neighbour *up(struct router *r) {
    if (!demand(r) || r->infra || !by_parent(r) || r->parent >= r->nodes) {
        return NULL;
    }
    uint16_t s = r->slot_of[r->parent];
    return s && usable(r, s, r->parent) ? slot(r, s) : NULL;
}

/* The neighbour a frame for `dst` goes to next, or NULL for none, and the metric of the way there:
 * through a leaf's parent, the route to the parent and a hop more. Changes nothing. */
static struct neighbour *next_toward(struct router *r, uint32_t dst, uint16_t *metric) {
    uint32_t t = target_of(r, dst);
    double hop = ceil(r->ref_ms);
    uint16_t one = (uint16_t)(hop < 1 ? 1 : hop);
    const struct entry *e = t < r->nodes && t != r->self ? live_sel(r, t) : NULL;
    if (!e) {
        struct neighbour *p = t < r->nodes && t != r->self ? up(r) : NULL;
        if (p && metric) {
            *metric = total(p->cost, one);
        }
        return p;
    }
    struct neighbour *n = slot(r, e->slot);
    if (metric) {
        uint16_t m = total(e->metric, n->cost);
        *metric = t == dst ? m : total(m, one);
    }
    return n;
}

static bool feasible(const struct dest *d, const struct entry *e) {
    return !d->has_fd || newer(e->seq, d->fd_seq) ||
           (e->seq == d->fd_seq && e->metric < d->fd_metric);
}

/* Counts the time since last called into the books' destination-seconds: call before either
 * count changes. */
static void account(struct router *r) {
    tsim_time t = now(r);
    double dt = (double)(t - r->acc_at) / (double)TSIM_S(1);
    r->stats.unrouted_s += (double)r->unrouted * dt;
    r->stats.urgent_s += (double)r->urgent_count * dt;
    r->acc_at = t;
}

static void push_urgent(struct router *r, uint32_t d) {
    if (r->dest[d].urgent || !announces(r, d)) {
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
    account(r);
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
        r->stats.route_requests++;
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
    r->stats.seqno_requests++;
    request(r, d, seq, next, r->config.hop_max);
}

/* A node with no feasible route to `d`, or none at all when it has a message for it, asks at once
 * unless it asked within request_interval, and then again every request_interval, REQUEST_TRIES
 * times in all, until a route comes: a request is a single frame, and as easily lost as any. */
static void starved(struct router *r, uint32_t d) {
    if (r->oracle) {
        return; /* no route is no path */
    }
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

/* A new seq of this node's own, announced at once as an answered request would be: every route to
 * it that feasibility had starved is feasible again once the new seq reaches it, with no request
 * having to get through. DSDV's periodic sequence numbers, under Babel's feasibility condition. */
static void trickle_begin(struct router *r);

static void seq_fire(void *ctx) {
    struct router *r = ctx;
    r->seq++;
    r->asked = true;
    r->interval = r->config.imin;
    trickle_begin(r);
    double period = (double)r->config.seq_period * (0.9 + 0.2 * tsim_rng_unit(&r->rng));
    tsim_timer_start(r->seq_timer, (tsim_time)period);
}

static void request_fire(void *ctx) {
    struct router *r = ctx;
    for (size_t i = 0; i < r->starving_count;) {
        uint32_t d = r->starving[i];
        struct dest *ds = &r->dest[d];
        if (ds->sel || ds->tries >= REQUEST_TRIES) {
            r->stats.gave_up += !ds->sel;
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
        if (!e->slot || e->metric == INF || !alive(r, e) || !usable(r, e->slot, d)) {
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
    if (!chosen && infeasible && !demand(r)) {
        starved(r, d); /* on demand, a route is asked for only when a message needs it */
    }
    if (!announces(r, d)) {
        return; /* a leaf announces no routes, and with parents nobody announces one to a leaf */
    }
    if (was != (chosen != NULL) && (ds->had || chosen)) {
        account(r);
        if (!chosen) {
            r->unrouted++;
            r->stats.outages++;
        } else if (ds->had) {
            r->unrouted--;
        }
        ds->had = true;
    }
    r->selected = r->selected - was + (chosen != NULL);
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
        .selectable = t != INF && alive(r, e) && usable(r, e->slot, d) && feasible(&r->dest[d], e),
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

/* What the neighbour in slot `s` advertises about `d`, for as long as `life` with demand routes -
 * never longer than route_ttl, nor than the neighbour has it for. */
static void update_for(struct router *r, uint32_t d, uint16_t seq, uint16_t metric, uint16_t s,
                       tsim_time life);

static void update(struct router *r, uint32_t d, uint16_t seq, uint16_t metric, uint16_t s) {
    update_for(r, d, seq, metric, s, r->config.route_ttl);
}

static void update_for(struct router *r, uint32_t d, uint16_t seq, uint16_t metric, uint16_t s,
                       tsim_time life) {
    if (d == r->self || d >= r->nodes) {
        return;
    }
    struct dest *ds = &r->dest[d];
    struct entry *e = entry_by(ds, s);
    /* On demand a route lasts route_ttl from when it was last heard of - but not a neighbour's
     * route to itself, which lasts as long as the link. */
    if (life > r->config.route_ttl) {
        life = r->config.route_ttl;
    }
    tsim_time until = demand(r) && !(metric == 0 && slot(r, s)->id == d) ? now(r) + life : 0;
    if (metric == INF) {
        if (!e) {
            return;
        }
        *e = (struct entry){0};
    } else if (e) {
        e->seq = seq;
        e->metric = metric;
        e->until = until;
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
        *place = (struct entry){.slot = s, .seq = seq, .metric = metric, .until = until};
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

static void probe_stop(struct router *r, struct neighbour *n, bool answered);

/* A neighbour is gone, for `cause`: unheard for too long, or by strength, frames sent to it lost
 * too many times running or a probe left unanswered. */
static void forget(struct router *r, uint16_t s, enum tsim_distvec_down cause) {
    struct neighbour *n = slot(r, s);
    if (!n->used) {
        return;
    }
    probe_stop(r, n, false);
    uint16_t was = n->cost;
    n->used = false;
    n->cost = INF;
    link_down(r, n, was, cause);
    r->slot_of[n->id] = 0;
    for (uint32_t d = 0; d < r->nodes; d++) {
        struct entry *e = d != r->self ? entry_by(&r->dest[d], s) : NULL;
        if (e) {
            *e = (struct entry){0};
            reselect(r, d);
        }
    }
    choose_parent(r);
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
        b[16] = power_byte(r->node_dbm);
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
            b[i + 4] = by_strength(r) ? margin_byte(r->config.tx_dbm - n->floor) : heard_rate(r, n);
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
            if (d != r->self && !ds->listed && (ds->sel || ds->retracts) && announces(r, d) &&
                advertise(r, d, b + i)) {
                i += ROUTE_LEN;
                routes++;
            }
        }
        for (size_t k = 0; k < taken; k++) {
            r->dest[r->urgent[k]].listed = false;
        }
        account(r);
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
    n->lost = 0;
    n->mute = false;
    probe_stop(r, n, true);
    n->infra = b[9] & FLAG_INFRA;
    n->promise = promise_time(get16(b + 10));
    if (r->config.power && r->truth) {
        n->floor = true_need(r, from);
    } else if (r->config.power || by_strength(r)) {
        double floor = sent_dbm(r, b + 16) - (snr - r->config.snr_floor_db);
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
    uint32_t rounds = r->config.ihu_rounds;
    uint32_t allowed = rounds * (rotation > 1 ? rotation : 1u) + 1u;
    if (allowed > IHU_AGE_MAX) {
        allowed = IHU_AGE_MAX;
    }
    if (!named && (uint16_t)(seq - n->ihu_seq) >= allowed) {
        n->dr = 0;
    }

    r->changed = fresh;
    uint16_t was = n->cost;
    judge(r, n);
    n->cost = link_cost(r, n);
    link_down(r, n, was, n->dr == 0 ? TSIM_DISTVEC_DOWN_IHU : TSIM_DISTVEC_DOWN_RATE);
    bool flipped = (was == INF) != (n->cost == INF);
    if (flipped ||
        fabs((double)n->cost - (double)n->cost_used) > r->config.change * (double)n->cost_used) {
        n->cost_used = n->cost;
        r->changed |= flipped;
        reselect_through(r, s);
    }
    r->dest[from].leaf = !n->infra;
    update(r, from, get16(b + 7), 0, s);
    choose_parent(r);
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
            r->stats.seq_raised++;
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

/* --- The liveness probe --- */

static void probe_arm(struct router *r) {
    tsim_time next = -1;
    for (size_t i = 0; r->probing && i < r->nb_count; i++) {
        const struct neighbour *n = &r->nb[i];
        if (n->used && n->probing && n->probe_due >= 0 && (next < 0 || n->probe_due < next)) {
            next = n->probe_due;
        }
    }
    if (next < 0) {
        tsim_timer_stop(r->probe_timer);
    } else {
        tsim_time t = now(r);
        tsim_timer_start(r->probe_timer, next > t ? next - t : 0);
    }
}

/* Asks the neighbour whether it is there, unless it is being asked already. */
static void probe_start(struct router *r, struct neighbour *n) {
    if (n->probing) {
        return;
    }
    n->probing = true;
    n->probes = 0;
    n->probe_handle = 0;
    n->probe_due = now(r);
    r->probing++;
    probe_arm(r);
}

/* Something was heard from the neighbour - `answered` - or it is forgotten: no more probes. */
static void probe_stop(struct router *r, struct neighbour *n, bool answered) {
    if (!n->probing) {
        return;
    }
    if (n->probe_handle) {
        tsim_node_cancel(r->node, n->probe_handle);
    }
    if (answered && n->probes) {
        r->stats.probes_answered++;
    }
    n->probing = false;
    n->probe_handle = 0;
    r->probing--;
    probe_arm(r);
}

/* A probe or its answer, `type`, from `from` to `to`, at tx_dbm. */
static struct tsim_tx probe_frame(const struct router *r, uint8_t type, uint32_t to,
                                  uint32_t from) {
    struct tsim_tx tx = frame(r, TSIM_PURPOSE_CONTROL, PRIORITY_CONTROL);
    tx.tx_dbm = r->config.tx_dbm;
    tx.bytes[0] = type;
    put32(tx.bytes + 1, to);
    put32(tx.bytes + 5, from);
    tx.bytes[9] = power_byte(tx.tx_dbm);
    tx.len = PROBE_LEN;
    return tx;
}

/* The neighbour in slot `s` left probe_tries probes unanswered: its link goes out of use, its
 * routes kept, until something is heard from it. */
static void silenced(struct router *r, uint16_t s) {
    struct neighbour *n = slot(r, s);
    probe_stop(r, n, false);
    n->mute = true;
    uint16_t was = n->cost;
    n->cost = INF;
    link_down(r, n, was, TSIM_DISTVEC_DOWN_PROBE);
    if (was != INF) {
        n->cost_used = INF;
        r->changed = true;
        reselect_through(r, s);
        choose_parent(r);
        trickle_reset(r);
        r->changed = false;
    }
}

static void probe_fire(void *ctx) {
    struct router *r = ctx;
    tsim_time t = now(r);
    for (size_t i = 0; r->probing && i < r->nb_count; i++) {
        struct neighbour *n = &r->nb[i];
        if (!n->used || !n->probing || n->probe_due < 0 || n->probe_due > t) {
            continue;
        }
        if (n->probes >= r->config.probe_tries) {
            silenced(r, (uint16_t)(i + 1));
            continue;
        }
        struct tsim_tx tx = probe_frame(r, TYPE_PROBE, n->id, r->self);
        n->probe_handle = tsim_node_send(r->node, &tx);
        if (n->probe_handle) {
            n->probes++;
            r->stats.probes++;
            n->probe_due = -1;
        } else {
            n->probe_due = t + r->config.probe_wait; /* the queue refused it: not counted */
        }
    }
    probe_arm(r);
}

/* A probe went on the air: its answer is waited for probe_wait, and the next probe a random time
 * up to probe_wait more. Returns whether `handle` was one. */
static bool probe_sent(struct router *r, uint64_t handle) {
    for (size_t i = 0; r->probing && i < r->nb_count; i++) {
        struct neighbour *n = &r->nb[i];
        if (n->used && n->probing && n->probe_handle == handle) {
            n->probe_handle = 0;
            n->probe_due = now(r) + r->config.probe_wait +
                           (tsim_time)(tsim_rng_unit(&r->rng) * (double)r->config.probe_wait);
            probe_arm(r);
            return true;
        }
    }
    return false;
}

static void heard_from(struct router *r, uint16_t s, const uint8_t *power, double snr);
static void hold(struct router *r, const struct tsim_tx *tx, uint64_t key, bool listen,
                 double airtimes);

/* A probe or an answer: its sender is heard, by strength, by every node that decodes it, and a
 * probe for this node is answered. */
static void on_probe(struct router *r, const uint8_t *b, uint32_t len, double snr) {
    if (len < PROBE_LEN) {
        return;
    }
    uint32_t to = get32(b + 1), from = get32(b + 5);
    if (from >= r->nodes || from == r->self) {
        return;
    }
    if (by_strength(r) && r->slot_of[from]) {
        heard_from(r, r->slot_of[from], b + 9, snr);
    }
    if (b[0] == TYPE_PROBE && to == r->self) {
        struct tsim_tx tx = probe_frame(r, TYPE_PROBE_ACK, from, r->self);
        hold(r, &tx, 0, false, r->config.jitter);
        r->stats.probe_acks++;
    }
}

/* --- Next hops and their implicit acknowledgements --- */

/* A frame never reached the neighbour in slot `s`: evidence against the link, as good as HOP_MISS
 * of its announces missed. A link that keeps losing frames soon goes over etx_max and unused -
 * with etx, it costs more than another first - and one that lost a frame to a busy moment recovers
 * as its announces come in. */
static void missed(struct router *r, uint16_t s) {
    struct neighbour *n = slot(r, s);
    double room = r->config.tx_dbm - r->config.tx_min_dbm;
    n->boost = n->boost + r->config.step_db > room ? room : n->boost + r->config.step_db;
    /* By strength, a lost hop is a lost frame - until dead_hops of them, with nothing heard from
     * it between: then the neighbour is gone. */
    if (by_strength(r) && ++n->lost >= r->config.dead_hops) {
        forget(r, s, TSIM_DISTVEC_DOWN_HOP);
        return;
    }
    if (by_strength(r) && r->config.probe_hops && n->lost >= r->config.probe_hops) {
        probe_start(r, n);
    }
    n->history = (uint16_t)(n->history << HOP_MISS);
    n->span = HISTORY;
    uint16_t was = n->cost;
    n->cost = link_cost(r, n);
    link_down(r, n, was, TSIM_DISTVEC_DOWN_HOP);
    if (n->cost != was) {
        n->cost_used = n->cost;
        reselect_through(r, s);
        choose_parent(r);
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
                double p = ceil(h->tx.tx_dbm + r->config.step_db), hi = r->config.tx_dbm;
                h->tx.tx_dbm = p > hi ? hi : p;
                h->tx.bytes[18] = power_byte(h->tx.tx_dbm);
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
        if (h->tx.carries) {
            tsim_node_drop(r->node, h->tx.carries, TSIM_DROP_RETRIES, next);
        }
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

/* With links by strength, a frame heard from the neighbour in slot `s`, at `snr`, that went at
 * the power byte at `power`: as an announce's, its strength is the link's, and it is heard. */
static void heard_from(struct router *r, uint16_t s, const uint8_t *power, double snr) {
    struct neighbour *n = slot(r, s);
    double floor = sent_dbm(r, power) - (snr - r->config.snr_floor_db);
    n->floor = isnan(n->floor) ? floor : 0.75 * n->floor + 0.25 * floor;
    n->heard = now(r);
    n->lost = 0;
    n->mute = false;
    probe_stop(r, n, true);
    uint16_t was = n->cost;
    judge(r, n);
    n->cost = link_cost(r, n);
    link_down(r, n, was, TSIM_DISTVEC_DOWN_RATE);
    if ((was == INF) != (n->cost == INF)) {
        n->cost_used = n->cost;
        r->changed = true;
        reselect_through(r, s);
        choose_parent(r);
        trickle_reset(r);
        r->changed = false;
    }
}

/* Something passed on what a hop of this node's was waiting to hear: a data or acknowledgement
 * frame, one hop further on, heard at `snr`. */
static void overheard(struct router *r, const uint8_t *b, double snr) {
    uint8_t type = b[0], hops = b[17];
    uint32_t src = get32(b + 5), dst = get32(b + 9), id = get32(b + 13);
    uint32_t from = TSIM_BROADCAST; /* who passed it on */
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
            /* Its next hop sent what passed it on, and the first copy of an acknowledgement, with
             * hop_max hops left; a relay may have sent a later one. */
            if (passed || hops == r->config.hop_max) {
                from = h->next;
            }
            if (s && slot(r, s)->boost > 0) {
                slot(r, s)->boost = slot(r, s)->boost > 1 ? slot(r, s)->boost - 1 : 0;
            }
            drop_hop(r, i);
            continue;
        }
        i++;
    }
    arm_hops(r);
    if (by_strength(r) && from < r->nodes && r->slot_of[from]) {
        heard_from(r, r->slot_of[from], b + 18, snr);
    }
}

/* --- Routes on demand --- */

/* The route to `d` this node would put in a frame, as an announce would: its own, or its selected
 * one, setting the feasibility distance as advertise() does. False for none. */
static bool offer(struct router *r, uint32_t d, uint16_t *seq, uint16_t *metric, uint16_t *life) {
    double ttl_s = floor((double)r->config.route_ttl / (double)TSIM_S(1));
    *life = (uint16_t)(ttl_s > UINT16_MAX ? UINT16_MAX : ttl_s);
    if (d == r->self) {
        *seq = r->seq;
        *metric = 0;
        return true;
    }
    const struct entry *e = d < r->nodes ? live_sel(r, d) : NULL;
    uint16_t m = e ? total(e->metric, slot(r, e->slot)->cost) : INF;
    if (m == INF) {
        return false;
    }
    struct dest *ds = &r->dest[d];
    if (!ds->has_fd || newer(e->seq, ds->fd_seq) || (e->seq == ds->fd_seq && m < ds->fd_metric)) {
        ds->has_fd = true;
        ds->fd_seq = e->seq;
        ds->fd_metric = m;
    }
    *seq = e->seq;
    *metric = m;
    if (e->until) {
        /* What is left of it, rounded down: a node that learns it never keeps it longer. */
        double left = floor((double)(e->until - now(r)) / (double)TSIM_S(1));
        if (left < 1) {
            return false;
        }
        if (left < *life) {
            *life = (uint16_t)left;
        }
    }
    return true;
}

/* The route to a data, acknowledgement or reply frame's source that its sender put in it, taken as
 * the sender advertising it. */
static void learn(struct router *r, const uint8_t *b) {
    uint32_t sender = get32(b + r->trail_at);
    uint16_t metric = get16(b + r->trail_at + 6);
    tsim_time life = (tsim_time)get16(b + r->trail_at + 8) * TSIM_S(1);
    if (sender < r->nodes && sender != r->self && r->slot_of[sender] && metric != INF) {
        update_for(r, get32(b + 5), get16(b + r->trail_at + 4), metric, r->slot_of[sender], life);
    }
}

/* Floods a route request for `t`, at most once a request_interval, out of the requests' share of
 * the cap. It asks for the seq after this node's feasibility distance, if it has one, so the
 * routes the reply leaves are feasible here. */
static void discover(struct router *r, uint32_t t) {
    struct dest *ds = &r->dest[t];
    tsim_time at = now(r);
    if (ds->asked >= 0 && at - ds->asked < r->config.request_interval) {
        return;
    }
    struct bucket *b = &r->requests;
    refill(r, b);
    struct tsim_tx tx = frame(r, TSIM_PURPOSE_CONTROL, PRIORITY_CONTROL);
    double cost = (double)tsim_lora_airtime(&tx.lora, RREQ_LEN);
    if (b->ns < cost) {
        return;
    }
    uint32_t id = ++r->rreq_id;
    /* A new seq of its own with each request, as AODV's: the routes back to it the request leaves
     * are feasible wherever it goes, however many earlier ones went a shorter way. */
    r->seq++;
    tx.bytes[0] = TYPE_RREQ;
    put32(tx.bytes + 1, r->self);
    put32(tx.bytes + 5, id);
    put32(tx.bytes + 9, t);
    put16(tx.bytes + 13, ds->has_fd ? (uint16_t)(ds->fd_seq + 1) : 0);
    tx.bytes[15] = ds->has_fd ? RREQ_SEQ : 0;
    tx.bytes[16] = r->config.req_hops;
    uint16_t seq, metric, life;
    offer(r, r->self, &seq, &metric, &life);
    put32(tx.bytes + 17, r->self);
    put16(tx.bytes + 21, seq);
    put16(tx.bytes + 23, metric);
    put16(tx.bytes + 25, life);
    tx.len = RREQ_LEN;
    if (!tsim_node_send(r->node, &tx)) {
        return;
    }
    b->ns -= cost;
    ds->asked = at;
    r->stats.route_requests++;
    mark_seen(r, frame_key(TYPE_RREQ, r->self, id));
}

/* --- Messages --- */

static void found(struct router *r, uint32_t t);

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
    const struct neighbour *n = NULL;
    uint32_t next;
    if (r->oracle) {
        next = r->oracle->route[(size_t)r->self * r->nodes + dst].next;
        if (next == TSIM_BROADCAST) {
            return false;
        }
    } else {
        n = next_toward(r, dst, NULL);
        if (!n) {
            return false;
        }
        next = n->id;
    }
    struct tsim_tx tx = frame(r, purpose,
                              type == TYPE_ACK || type == TYPE_RREP ? PRIORITY_CONTROL
                              : purpose == TSIM_PURPOSE_DATA        ? PRIORITY_DATA
                                                                    : PRIORITY_RELAY);
    tx.addressed = true;
    tx.to = next;
    tx.bytes[0] = type;
    put32(tx.bytes + 1, next);
    put32(tx.bytes + 5, src);
    put32(tx.bytes + 9, dst);
    put32(tx.bytes + 13, id);
    tx.bytes[17] = hops;
    if (r->config.power) {
        tx.tx_dbm = n ? power_for(r, n, back) : oracle_power(r, dst, back);
        tx.bytes[18] = power_byte(tx.tx_dbm);
    }
    if (demand(r)) {
        uint16_t seq = 0, metric = INF, life = 0;
        offer(r, src, &seq, &metric, &life);
        put32(tx.bytes + r->trail_at, r->self);
        put16(tx.bytes + r->trail_at + 4, seq);
        put16(tx.bytes + r->trail_at + 6, metric);
        put16(tx.bytes + r->trail_at + 8, life);
    }
    if (len) {
        memcpy(tx.bytes + r->data_head, content, len);
    }
    tx.len = r->data_head + len;
    if (carries) {
        tx.carries = carries;
        tx.carries_at = r->data_head;
    }
    /* The last hop of an acknowledgement or a reply has nothing to hear. */
    bool listen = !((type == TYPE_ACK || type == TYPE_RREP) && next == dst);
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

/* This node has no way to send a frame for `dst` on: it asks for a route to the node it would
 * follow one to - `dst`, or with parent_oracle the leaf's parent, never a leaf, which nobody
 * announces a route to. A leaf with no parent, or one of its own it cannot reach, it cannot ask
 * about. With `at_once`, a message waits on it, so it asks without waiting out request_interval. */
static void no_route(struct router *r, uint32_t dst, bool at_once) {
    uint32_t t = target_of(r, dst);
    if (t >= r->nodes || t == r->self) {
        return;
    }
    if (demand(r)) {
        discover(r, t);
        return;
    }
    if (at_once) {
        r->dest[t].asked = -1;
    }
    starved(r, t);
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
    tsim_time wait = r->config.ack_wait;
    uint64_t handle = 0;
    if (route_frame(r, TYPE_DATA, r->self, a->dst, a->id, r->config.hop_max, a->content, a->len,
                    TSIM_PURPOSE_DATA, a->id, NAN, &handle)) {
        uint16_t route_metric = INF;
        if (!r->oracle) {
            next_toward(r, a->dst, &route_metric);
        }
        double metric =
            r->oracle ? r->oracle->route[(size_t)r->self * r->nodes + a->dst].hops * ceil(r->ref_ms)
                      : route_metric;
        wait += (tsim_time)(r->config.ack_factor * metric * (double)TSIM_MS(1));
        a->routeless = false;
        if (handle) {
            /* The wait starts when the frame goes: however long it queues behind other traffic,
             * no second copy joins it, and the message is not given up while one still waits. */
            a->handle = handle;
            a->wait = wait;
            return;
        }
        /* Refused by a full queue: tried again after the wait, as if it had been lost. */
    } else {
        tsim_node_drop(r->node, a->id, TSIM_DROP_NO_ROUTE, TSIM_BROADCAST);
        a->routeless = true;
        no_route(r, a->dst, true); /* a message waits on it: ask now */
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
    for (size_t i = 0; i < r->parked_count; i++) {
        const uint8_t *b = r->parked[i].bytes;
        if (b[0] == type && get32(b + 5) == src && get32(b + 13) == id) {
            return true;
        }
    }
    return false;
}

/* Holds a data frame this relay has no route to pass on, until the route it asks for comes or two
 * request intervals go by. False if there is no room. */
static void unpark_expired(struct router *r) {
    for (size_t i = 0; i < r->parked_count;) {
        if (r->parked[i].until <= now(r)) {
            tsim_node_drop(r->node, get32(r->parked[i].bytes + 13), TSIM_DROP_NO_ROUTE,
                           TSIM_BROADCAST);
            r->parked[i] = r->parked[--r->parked_count];
        } else {
            i++;
        }
    }
}

static bool park(struct router *r, uint32_t target, const uint8_t *b, uint32_t len, double back) {
    tsim_time t = now(r);
    unpark_expired(r);
    if (!r->parked) {
        r->parked = malloc(PARKED_MAX * sizeof *r->parked);
    }
    if (!r->parked || r->parked_count == PARKED_MAX) {
        return false;
    }
    struct parked *p = &r->parked[r->parked_count++];
    p->target = target;
    p->until = t + 2 * r->config.request_interval;
    p->back = back;
    p->len = len;
    memcpy(p->bytes, b, len);
    if (!tsim_timer_pending(r->park_timer)) {
        tsim_timer_start(r->park_timer, r->config.request_interval);
    }
    return true;
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
    overheard(r, b, snr);
    if (demand(r)) {
        learn(r, b);
    }
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
    if (type == TYPE_RREP && dst == r->self) {
        found(r, src);
        return;
    }
    if (type == TYPE_DATA && dst == r->self) {
        tsim_node_deliver(r->node, id);
        if (!route_frame(r, TYPE_ACK, r->self, src, id, r->config.hop_max, NULL, 0,
                         TSIM_PURPOSE_CONTROL, 0, back, NULL)) {
            no_route(r, src, false);
        }
        return;
    }
    if (!r->infra || hops <= 1) {
        if (type == TYPE_DATA) {
            tsim_node_drop(r->node, id, r->infra ? TSIM_DROP_HOP_LIMIT : TSIM_DROP_OTHER,
                           TSIM_BROADCAST);
        }
        return;
    }
    uint64_t key = frame_key(type, src, id);
    if (seen(r, key) && passing_on(r, type, src, id)) {
        return; /* still trying to pass it on: the hop before will hear that */
    }
    mark_seen(r, key);
    bool data = type == TYPE_DATA;
    if (!route_frame(r, type, src, dst, id, (uint8_t)(hops - 1), b + r->data_head,
                     data ? len - r->data_head : 0,
                     data ? TSIM_PURPOSE_RELAY : TSIM_PURPOSE_CONTROL, data ? id : 0, back, NULL)) {
        uint32_t to = target_of(r, dst);
        if (data && demand(r) && to < r->nodes && to != r->self && park(r, to, b, len, back)) {
            no_route(r, dst, false);
            return; /* passed on when the route comes */
        }
        if (data) {
            tsim_node_drop(r->node, id, TSIM_DROP_NO_ROUTE, TSIM_BROADCAST);
        }
        if (type == TYPE_RREP) {
            return; /* the way back to the origin is gone: it asks again */
        }
        /* Sent here, so the hop before still has the route: if it had it from this node, its
         * retraction never got there. Say it again. */
        uint32_t t = target_of(r, dst);
        struct dest *ds = t < r->nodes ? &r->dest[t] : NULL;
        if (ds && !demand(r) && t != r->self && ds->has_fd && !ds->advertised && !ds->urgent) {
            if (!ds->retracts) {
                ds->retracts = 1;
                r->retracting++;
            }
            push_urgent(r, t);
            trickle_reset(r);
        }
        no_route(r, dst, false);
    }
}

/* Another copy heard of a flooded frame this node holds to pass on: counted, and with
 * bcast_cancel of them, the frame is dropped. */
static void heard_again(struct router *r, uint64_t key, uint8_t cancel) {
    for (size_t i = 0; i < r->held_count; i++) {
        struct held *h = &r->held[i];
        if (h->key != key) {
            continue;
        }
        if (h->heard < UINT8_MAX) {
            h->heard++;
        }
        if (cancel && h->heard >= cancel &&
            (h->handle == 0 || tsim_node_cancel(r->node, h->handle))) {
            r->held[i] = r->held[--r->held_count];
            arm_out(r);
        }
        break;
    }
}

static void on_bcast(struct router *r, const uint8_t *b, uint32_t len) {
    uint32_t src = get32(b + 1), id = get32(b + 5);
    uint8_t hops = b[9];
    uint64_t key = frame_key(TYPE_BCAST, src, id);
    if (seen(r, key)) {
        heard_again(r, key, r->config.bcast_cancel);
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

/* A reply reached this node, which asked for a route to `t`: the messages waiting on it go now. */
static void found(struct router *r, uint32_t t) {
    unpark_expired(r);
    for (size_t i = 0; i < r->parked_count;) {
        struct parked p = r->parked[i];
        if (p.target != t) {
            i++;
            continue;
        }
        /* Kept parked if the reply left no route this node can use: asked again, or dropped when
         * it expires. */
        const uint8_t *b = p.bytes;
        if (route_frame(r, TYPE_DATA, get32(b + 5), get32(b + 9), get32(b + 13),
                        (uint8_t)(b[17] - 1), b + r->data_head, p.len - r->data_head,
                        TSIM_PURPOSE_RELAY, get32(b + 13), p.back, NULL)) {
            r->parked[i] = r->parked[--r->parked_count];
        } else {
            i++;
        }
    }
    for (struct awaiting *a = r->awaiting; a; a = a->next) {
        if (a->routeless && a->handle == 0 && target_of(r, a->dst) == t &&
            next_toward(r, a->dst, NULL)) {
            tsim_timer_stop(a->timer);
            send_attempt(a);
        }
    }
}

/* A request_interval after a frame was parked: frames whose route has come since, from a reply or
 * from passing traffic, go; for the rest the route is asked for again - a request the cap had no
 * room for, or one never answered. */
static void park_fire(void *ctx) {
    struct router *r = ctx;
    unpark_expired(r);
    uint32_t targets[PARKED_MAX];
    size_t n = 0;
    for (size_t i = 0; i < r->parked_count; i++) {
        size_t j = 0;
        while (j < n && targets[j] != r->parked[i].target) {
            j++;
        }
        if (j == n) {
            targets[n++] = r->parked[i].target;
        }
    }
    for (size_t j = 0; j < n; j++) {
        if (live_sel(r, targets[j])) {
            found(r, targets[j]);
        } else {
            discover(r, targets[j]);
        }
    }
    if (r->parked_count) {
        tsim_timer_start(r->park_timer, r->config.request_interval);
    }
}

/* A route request: the route to its origin taken, then answered, passed on, or let be. */
static void on_rreq(struct router *r, const uint8_t *b, uint32_t len) {
    if (!demand(r) || len < RREQ_LEN) {
        return;
    }
    uint32_t origin = get32(b + 1), id = get32(b + 5), target = get32(b + 9);
    uint32_t sender = get32(b + 17);
    uint16_t want = get16(b + 13), metric = get16(b + 23);
    bool asks_seq = b[15] & RREQ_SEQ;
    uint8_t hops = b[16];
    if (origin >= r->nodes || target >= r->nodes || sender >= r->nodes || origin == r->self) {
        return;
    }
    uint64_t key = frame_key(TYPE_RREQ, origin, id);
    if (seen(r, key)) {
        heard_again(r, key, r->config.req_cancel);
        return;
    }
    mark_seen(r, key);
    if (sender != r->self && r->slot_of[sender] && metric != INF) {
        update_for(r, origin, get16(b + 21), metric, r->slot_of[sender],
                   (tsim_time)get16(b + 25) * TSIM_S(1));
    }
    if (target == r->self) {
        /* A new seq with each reply, at least the one asked for, so the routes it leaves are
         * feasible at every hop on the way back, as the request's were. */
        uint16_t next = (uint16_t)(r->seq + 1);
        r->seq = asks_seq && newer(want, next) ? want : next;
        r->stats.seq_raised++;
        /* A reply goes as the target's, so it takes an id of the target's own: the request's id
         * is the origin's, and two origins' requests for one target may share it. */
        if (route_frame(r, TYPE_RREP, target, origin, ++r->rreq_id, r->config.hop_max, NULL, 0,
                        TSIM_PURPOSE_CONTROL, 0, NAN, NULL)) {
            r->stats.route_replies++;
        }
        return;
    }
    uint16_t seq, back, life;
    if (!r->infra || hops <= 1 || !offer(r, origin, &seq, &back, &life)) {
        return; /* nothing to pass on, or no way for a reply to come back through here */
    }
    struct tsim_tx tx = frame(r, TSIM_PURPOSE_CONTROL, PRIORITY_CONTROL);
    memcpy(tx.bytes, b, RREQ_LEN);
    tx.bytes[16] = (uint8_t)(hops - 1);
    put32(tx.bytes + 17, r->self);
    put16(tx.bytes + 21, seq);
    put16(tx.bytes + 23, back);
    put16(tx.bytes + 25, life);
    tx.len = RREQ_LEN;
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
    case TYPE_RREQ:
        on_rreq(r, rx->bytes, rx->len);
        return;
    case TYPE_DATA:
    case TYPE_ACK:
    case TYPE_RREP:
        if (rx->len >= r->data_head) {
            on_data(r, rx->bytes, rx->len, rx->snr_db);
        }
        return;
    case TYPE_BCAST:
        if (rx->len >= BCAST_HEAD) {
            on_bcast(r, rx->bytes, rx->len);
        }
        return;
    case TYPE_PROBE:
    case TYPE_PROBE_ACK:
        on_probe(r, rx->bytes, rx->len, rx->snr_db);
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
    if (probe_sent(r, handle)) {
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

/* How long a neighbour may go unheard before it is forgotten: neighbour_timeout, or by strength,
 * silent_max. */
static tsim_time silence_limit(const struct router *r) {
    return by_strength(r) ? r->config.silent_max : r->config.neighbour_timeout;
}

/* How often a node looks over its links for neighbours gone quiet. */
static tsim_time house_period(const struct router *r) {
    tsim_time p = silence_limit(r) / 4;
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
        /* Never before it has let two of its promises pass: a node may go quiet that long. And
         * never with the link oracle, whose links stay; by strength, only after silent_max, its
         * frames lost telling sooner if it is gone. */
        if (!r->truth && t - n->heard > silence_limit(r) && t - n->heard > 2 * n->promise) {
            forget(r, s, TSIM_DISTVEC_DOWN_TIMEOUT);
            continue;
        }
        uint16_t was = n->cost;
        n->cost = link_cost(r, n);
        link_down(r, n, was, TSIM_DISTVEC_DOWN_SILENT);
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
    choose_parent(r);
    node_power(r);
    unpark_expired(r);
    tsim_timer_start(r->house, house_period(r));
}

static void router_start(void *self) {
    struct router *r = self;
    if (r->oracle) {
        r->node_dbm = r->config.power ? r->oracle->node_dbm[r->self] : r->config.tx_dbm;
        return; /* nothing to announce, and no neighbours to keep */
    }
    trickle_begin(r);
    tsim_timer_start(r->house, house_period(r));
    if (r->config.seq_period > 0) {
        tsim_timer_start(r->seq_timer,
                         (tsim_time)((double)r->config.seq_period * tsim_rng_unit(&r->rng)));
    }
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
    tsim_timer_destroy(r->seq_timer);
    tsim_timer_destroy(r->park_timer);
    tsim_timer_destroy(r->probe_timer);
    free(r->starving);
    free(r->asks);
    free(r->dest);
    free(r->slot_of);
    free(r->nb);
    free(r->urgent);
    free(r->hops);
    free(r->held);
    free(r->parked);
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
    r->infra = tsim_distvec_relay(c, r->self);
    r->config = *c;
    r->oracle = c->oracle ? c->oracle_routes : NULL;
    if (c->relay_pick != TSIM_DISTVEC_PICK_LIST && !c->relay_set) {
        free(r); /* picked relays come from the driver, once it has laid the links */
        return NULL;
    }
    r->truth = c->links == TSIM_DISTVEC_LINKS_ORACLE && !c->oracle ? c->oracle_routes : NULL;
    if ((c->oracle || c->links == TSIM_DISTVEC_LINKS_ORACLE) &&
        !c->oracle_routes) { /* the routes themselves come once the links are laid */
        free(r);
        return NULL;
    }
    r->parent = TSIM_DISTVEC_NO_PARENT;
    if (c->leaves == TSIM_DISTVEC_LEAVES_PARENT_ORACLE) {
        if (!c->parents) {
            free(r);
            return NULL;
        }
        c->parents[r->self] = r->infra ? r->self : TSIM_DISTVEC_NO_PARENT;
    }
    r->ann_head = ANNOUNCE_HEAD + (c->power ? 1 : 0);
    r->trail_at = DATA_HEAD + (c->power ? 1 : 0);
    r->data_head = r->trail_at + (demand(r) ? TRAIL_LEN : 0);
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
    r->seq_timer = tsim_timer_create(node, seq_fire, r);
    r->park_timer = tsim_timer_create(node, park_fire, r);
    r->probe_timer = tsim_timer_create(node, probe_fire, r);
    if (!r->probe_timer || !r->seq_timer || !r->park_timer || !r->dest || !r->slot_of ||
        !r->trickle || !r->cap_timer || !r->hop_timer || !r->out_timer || !r->house ||
        !r->request_timer || !r->ask_timer) {
        router_destroy(r);
        return NULL;
    }
    for (uint32_t d = 0; d < r->nodes; d++) {
        r->dest[d].asked = -1;
    }
    return r;
}

static bool router_next_hop(const void *self, uint32_t dst, uint32_t *next) {
    return tsim_distvec_next(self, dst, next);
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
    .next_hop = router_next_hop,
};

/* --- For tests and reports --- */

bool tsim_distvec_route(const void *self, uint32_t dst, uint32_t *next, uint16_t *metric) {
    const struct router *r = self;
    if (r->oracle && dst < r->nodes && dst != r->self) {
        const struct tsim_distvec_oracle_route *o =
            &r->oracle->route[(size_t)r->self * r->nodes + dst];
        if (o->next == TSIM_BROADCAST) {
            return false;
        }
        if (next) {
            *next = o->next;
        }
        if (metric) {
            *metric = (uint16_t)(o->hops * ceil(r->ref_ms));
        }
        return true;
    }
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

bool tsim_distvec_next(const void *self, uint32_t dst, uint32_t *next) {
    struct router *r = (struct router *)self; /* next_toward() changes nothing */
    if (r->oracle || dst >= r->nodes || dst == r->self) {
        uint16_t metric;
        return tsim_distvec_route(self, dst, next, &metric);
    }
    const struct neighbour *n = next_toward(r, dst, NULL);
    if (n && next) {
        *next = n->id;
    }
    return n != NULL;
}

void tsim_distvec_stats(const void *self, struct tsim_distvec_stats *stats) {
    struct router *r = (struct router *)self; /* brings the books up to now, and nothing else */
    account(r);
    *stats = r->stats;
    for (uint32_t d = 0; d < r->nodes; d++) {
        struct dest *ds = &r->dest[d];
        if (d == r->self || ds->sel || !ds->had || !announces(r, d)) {
            continue;
        }
        bool held = false;
        for (int i = 0; i < ROUTES; i++) {
            const struct entry *e = &ds->e[i];
            held |= e->slot && e->metric != INF && usable(r, e->slot, d);
        }
        stats->unrouted_infeasible += held;
        stats->unrouted_empty += !held;
    }
}

uint32_t tsim_distvec_neighbours(const void *self) {
    const struct router *r = self;
    uint32_t count = 0;
    for (size_t i = 0; i < r->nb_count; i++) {
        count += r->nb[i].used && r->nb[i].cost != INF;
    }
    return count;
}

bool tsim_distvec_uses(const void *self, uint32_t nb) {
    const struct router *r = self;
    if (nb >= r->nodes || !r->slot_of[nb]) {
        return false;
    }
    const struct neighbour *n = slot((struct router *)r, r->slot_of[nb]);
    return n->used && n->cost != INF;
}

tsim_time tsim_distvec_promise(const void *self) { return ((const struct router *)self)->promised; }

uint32_t tsim_distvec_round(const void *self) { return ((const struct router *)self)->round; }

double tsim_distvec_power(const void *self, uint32_t nb) {
    const struct router *r = self;
    if (nb >= r->nodes || !r->slot_of[nb]) {
        return r->config.tx_dbm;
    }
    return power_for(r, &r->nb[r->slot_of[nb] - 1], NAN);
}

double tsim_distvec_node_power(const void *self) { return ((const struct router *)self)->node_dbm; }

uint16_t tsim_distvec_seq(const void *self) { return ((const struct router *)self)->seq; }

tsim_time tsim_distvec_interval(const void *self) {
    return ((const struct router *)self)->interval;
}

/* --- The oracle --- */

void tsim_distvec_oracle_free(struct tsim_distvec_oracle *o) {
    free(o->route);
    free(o->node_dbm);
    free(o->need);
    *o = (struct tsim_distvec_oracle){0};
}

static int by_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

bool tsim_distvec_oracle_build(struct tsim_distvec_oracle *o, const struct tsim_phy *phy,
                               const struct tsim_distvec_config *c) {
    *o = (struct tsim_distvec_oracle){0};
    double floor_dbm = tsim_phy_floor_dbm(phy, &c->lora);
    uint32_t n = tsim_phy_nodes(phy);
    if (isnan(floor_dbm) || n == 0) {
        return false;
    }
    /* need[a * n + b]: what a frame from a needs to be decoded at b, by the mean loss. */
    float *need = malloc((size_t)n * n * sizeof *need);
    uint32_t *start = calloc((size_t)n + 1, sizeof *start);
    uint32_t *dist = malloc(n * sizeof *dist), *queue = malloc(n * sizeof *queue);
    o->route = malloc((size_t)n * n * sizeof *o->route);
    o->node_dbm = malloc(n * sizeof *o->node_dbm);
    float *near = malloc(n * sizeof *near);
    uint32_t *adj = NULL;
    bool ok = false;
    double top = c->tx_dbm - c->oracle_margin_db;
    if (!need || !start || !dist || !queue || !o->route || !o->node_dbm || !near) {
        goto done;
    }
    for (uint32_t a = 0; a < n; a++) {
        for (uint32_t b = 0; b < n; b++) {
            need[(size_t)a * n + b] = (float)(tsim_phy_loss(phy, a, b) + floor_dbm);
        }
    }
    for (uint32_t a = 0; a < n; a++) {
        for (uint32_t b = 0; b < n; b++) {
            start[a + 1] +=
                a != b && need[(size_t)a * n + b] <= top && need[(size_t)b * n + a] <= top;
        }
        start[a + 1] += start[a];
    }
    adj = malloc((size_t)start[n] * sizeof *adj + 1);
    if (!adj) {
        goto done;
    }
    double lo = ceil(c->tx_min_dbm);
    for (uint32_t a = 0; a < n; a++) {
        uint32_t k = 0;
        for (uint32_t b = 0; b < n; b++) {
            if (a != b && need[(size_t)a * n + b] <= top && need[(size_t)b * n + a] <= top) {
                near[k] = need[(size_t)a * n + b];
                adj[start[a] + k++] = b;
            }
        }
        /* What power control would settle on knowing every floor: power_k of them, with margin. */
        double p = c->tx_dbm;
        if (c->power && c->power_k && k >= c->power_k) {
            qsort(near, k, sizeof *near, by_float);
            p = ceil(near[c->power_k - 1] + c->margin_db);
            p = p < lo ? lo : p > c->tx_dbm ? c->tx_dbm : p;
        }
        o->node_dbm[a] = (float)p;
    }
    for (uint32_t d = 0; d < n; d++) {
        for (uint32_t a = 0; a < n; a++) {
            dist[a] = UINT32_MAX;
        }
        size_t head = 0, tail = 0;
        dist[d] = 0;
        queue[tail++] = d;
        while (head < tail) {
            uint32_t a = queue[head++];
            if (a != d && !tsim_distvec_relay(c, a)) {
                continue; /* a leaf can be reached, but never passes anything on */
            }
            for (uint32_t i = start[a]; i < start[a + 1]; i++) {
                if (dist[adj[i]] == UINT32_MAX) {
                    dist[adj[i]] = dist[a] + 1;
                    queue[tail++] = adj[i];
                }
            }
        }
        for (uint32_t a = 0; a < n; a++) {
            struct tsim_distvec_oracle_route *rt = &o->route[(size_t)a * n + d];
            *rt = (struct tsim_distvec_oracle_route){.next = TSIM_BROADCAST};
            if (a == d || dist[a] == UINT32_MAX) {
                continue;
            }
            float best = INFINITY;
            for (uint32_t i = start[a]; i < start[a + 1]; i++) {
                uint32_t b = adj[i];
                bool passes = b == d || tsim_distvec_relay(c, b);
                if (passes && dist[b] + 1 == dist[a] && need[(size_t)a * n + b] < best) {
                    best = need[(size_t)a * n + b];
                    rt->next = b;
                }
            }
            rt->hops = dist[a] > UINT8_MAX ? UINT8_MAX : (uint8_t)dist[a];
            double p = c->power ? ceil(best + c->margin_db) : c->tx_dbm;
            rt->dbm = (float)(p < lo ? lo : p > c->tx_dbm ? c->tx_dbm : p);
        }
    }
    o->nodes = n;
    ok = true;
done:
    o->need = need;
    o->top = top;
    free(start);
    free(dist);
    free(queue);
    free(near);
    free(adj);
    if (!ok) {
        tsim_distvec_oracle_free(o);
    }
    return ok;
}

bool tsim_distvec_relay(const struct tsim_distvec_config *c, uint32_t node) {
    if (c->relay_pick != TSIM_DISTVEC_PICK_LIST) {
        return c->relay_set && c->relay_set[node];
    }
    return tsim_nodeset_contains(c->relays, node) == 1;
}

/* The oracle's links as lists: b is one of a's neighbours, adj[start[a]..start[a + 1]), where each
 * decodes the other at tx_dbm with oracle_margin_db to spare. Both to be freed; false when memory
 * runs out or the modulation is invalid. */
static bool oracle_links(const struct tsim_phy *phy, const struct tsim_distvec_config *c,
                         uint32_t **start_out, uint32_t **adj_out) {
    *start_out = NULL;
    *adj_out = NULL;
    double floor_dbm = tsim_phy_floor_dbm(phy, &c->lora);
    uint32_t n = tsim_phy_nodes(phy);
    if (isnan(floor_dbm) || n == 0) {
        return false;
    }
    double top = c->tx_dbm - c->oracle_margin_db; /* as the oracle judges them, to the float */
    uint32_t *start = calloc((size_t)n + 1, sizeof *start);
    if (!start) {
        return false;
    }
    for (int pass = 0; pass < 2; pass++) {
        uint32_t k = 0;
        for (uint32_t a = 0; a < n; a++) {
            start[a] = k;
            for (uint32_t b = 0; b < n; b++) {
                if (a != b && (float)(tsim_phy_loss(phy, a, b) + floor_dbm) <= top &&
                    (float)(tsim_phy_loss(phy, b, a) + floor_dbm) <= top) {
                    if (pass) {
                        (*adj_out)[k] = b;
                    }
                    k++;
                }
            }
        }
        start[n] = k;
        if (!pass && !(*adj_out = malloc((size_t)k * sizeof **adj_out + 1))) {
            free(start);
            return false;
        }
    }
    *start_out = start;
    return true;
}

/* Adds to `set` the `count` nodes not in it of the most links, the lowest-numbered first among
 * equals; returns how many it added. */
static uint32_t pick_by_degree(const uint32_t *start, uint32_t n, uint8_t *set, uint32_t count) {
    uint32_t added = 0;
    while (added < count) {
        uint32_t best = n;
        for (uint32_t a = 0; a < n; a++) {
            if (!set[a] && (best == n || start[a + 1] - start[a] > start[best + 1] - start[best])) {
                best = a;
            }
        }
        if (best == n) {
            break;
        }
        set[best] = 1;
        added++;
    }
    return added;
}

static bool pick_spaced(const struct tsim_pos *pos, uint32_t n, uint8_t *set, uint32_t count) {
    double *gap = malloc(n * sizeof *gap);
    if (!gap) {
        return false;
    }
    double mx = 0, my = 0;
    for (uint32_t a = 0; a < n; a++) {
        mx += pos[a].x / n;
        my += pos[a].y / n;
    }
    for (uint32_t a = 0; a < n; a++) {
        gap[a] = INFINITY;
    }
    for (uint32_t k = 0; k < count && k < n; k++) {
        uint32_t best = n;
        double far = -1;
        for (uint32_t a = 0; a < n; a++) {
            double d = k ? gap[a] : -hypot(pos[a].x - mx, pos[a].y - my);
            if (!set[a] && (best == n || d > far)) {
                best = a;
                far = d;
            }
        }
        set[best] = 1;
        for (uint32_t a = 0; a < n; a++) {
            double d = hypot(pos[a].x - pos[best].x, pos[a].y - pos[best].y);
            gap[a] = d < gap[a] ? d : gap[a];
        }
    }
    free(gap);
    return true;
}

/* Guha and Khuller's first greedy algorithm, stopping at `count`. A node is white with no relay
 * in reach, grey with one next to it, and picked; gain[a] is the white nodes a would bring, itself
 * among them. Picking only grey nodes keeps each part's relays joined, and a part none reach yet
 * starts from its white node of the most gain. */
static bool pick_cds(const uint32_t *start, const uint32_t *adj, uint32_t n, uint8_t *set,
                     uint32_t count) {
    enum { WHITE, GREY, PICKED };
    uint8_t *colour = calloc(n, 1);
    uint32_t *gain = malloc(n * sizeof *gain);
    if (!colour || !gain) {
        free(colour);
        free(gain);
        return false;
    }
    for (uint32_t a = 0; a < n; a++) {
        gain[a] = start[a + 1] - start[a] + 1;
    }
    uint32_t white = n;
    for (uint32_t k = 0; k < count && white; k++) {
        uint32_t best = n;
        for (int from = GREY; from >= WHITE && (best == n || gain[best] == 0); from--) {
            best = n;
            for (uint32_t a = 0; a < n; a++) {
                if (colour[a] == from && (best == n || gain[a] > gain[best])) {
                    best = a;
                }
            }
        }
        if (best == n || gain[best] == 0) {
            break;
        }
        /* best and its white neighbours now have a relay in reach: none brings them again. */
        for (uint32_t i = start[best]; i <= start[best + 1]; i++) {
            uint32_t u = i < start[best + 1] ? adj[i] : best;
            if (colour[u] != WHITE) {
                continue;
            }
            colour[u] = GREY;
            white--;
            gain[u]--;
            for (uint32_t j = start[u]; j < start[u + 1]; j++) {
                gain[adj[j]]--;
            }
        }
        colour[best] = PICKED;
        set[best] = 1;
    }
    free(colour);
    free(gain);
    return true;
}

bool tsim_distvec_pick_relays(const struct tsim_phy *phy, const struct tsim_pos *pos,
                              const struct tsim_distvec_config *c, uint8_t *set) {
    uint32_t *start, *adj;
    uint32_t n = tsim_phy_nodes(phy);
    if (c->relay_pick == TSIM_DISTVEC_PICK_LIST || !oracle_links(phy, c, &start, &adj)) {
        return false;
    }
    memset(set, 0, n);
    uint32_t count = c->relay_count && c->relay_count < n ? c->relay_count : n;
    bool ok = true;
    switch (c->relay_pick) {
    case TSIM_DISTVEC_PICK_DEGREE:
        pick_by_degree(start, n, set, count);
        break;
    case TSIM_DISTVEC_PICK_SPACED:
        ok = pick_spaced(pos, n, set, count);
        break;
    case TSIM_DISTVEC_PICK_CDS:
        ok = pick_cds(start, adj, n, set, count);
        if (ok && c->relay_count) {
            uint32_t have = 0;
            for (uint32_t a = 0; a < n; a++) {
                have += set[a];
            }
            pick_by_degree(start, n, set, count - have);
        }
        break;
    }
    free(start);
    free(adj);
    return ok;
}

bool tsim_distvec_tier(const struct tsim_phy *phy, const struct tsim_distvec_config *c,
                       struct tsim_relay_tier *out) {
    *out = (struct tsim_relay_tier){0};
    uint32_t *start, *adj;
    uint32_t n = tsim_phy_nodes(phy);
    if (!oracle_links(phy, c, &start, &adj)) {
        return false;
    }
    uint8_t *relay = malloc(n);
    uint32_t *part = malloc(n * sizeof *part), *queue = malloc(n * sizeof *queue);
    bool ok = relay && part && queue;
    for (uint32_t a = 0; ok && a < n; a++) {
        relay[a] = tsim_distvec_relay(c, a);
        part[a] = UINT32_MAX;
    }
    uint64_t joined = 0;
    uint32_t leaves = 0, covered = 0;
    for (uint32_t a = 0; ok && a < n; a++) {
        if (!relay[a]) {
            leaves++;
            for (uint32_t i = start[a]; i < start[a + 1]; i++) {
                if (relay[adj[i]]) {
                    covered++;
                    break;
                }
            }
            continue;
        }
        out->count++;
        if (part[a] != UINT32_MAX) {
            continue;
        }
        /* A new set of relays: every relay its links among relays reach from a. */
        uint32_t head = 0, tail = 0;
        part[a] = out->components++;
        queue[tail++] = a;
        while (head < tail) {
            uint32_t b = queue[head++];
            for (uint32_t i = start[b]; i < start[b + 1]; i++) {
                if (relay[adj[i]] && part[adj[i]] == UINT32_MAX) {
                    part[adj[i]] = part[a];
                    queue[tail++] = adj[i];
                }
            }
        }
        out->largest = tail > out->largest ? tail : out->largest;
        joined += (uint64_t)tail * (tail - 1);
    }
    if (ok) {
        out->present = leaves > 0; /* with none, there is no tier to speak of */
        out->pairs = out->count > 1 ? (double)joined / ((double)out->count * (out->count - 1)) : 0;
        out->covered = leaves ? (double)covered / leaves : 1;
    }
    free(relay);
    free(part);
    free(queue);
    free(start);
    free(adj);
    return ok;
}
