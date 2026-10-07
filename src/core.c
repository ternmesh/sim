#include "tsim/core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tern/forward.h"
#include "tern/route.h"
#include "tsim/rng.h"

/* Announces go before requests wait, as candidate 3 has them. */
#define PRIORITY_REQUEST 3
#define PRIORITY_ANNOUNCE 2
/* And frames that follow routes as candidate 3 has them: acknowledgements first, then what is
 * passed on, then a node's own. */
#define PRIORITY_REPLY 3
#define PRIORITY_RELAY 2
#define PRIORITY_OWN 1

/* A message's frame: the head, then four bytes that tell it from any other - the secured unicast
 * frame's destination tag, here the message's number - its content, and eight bytes where the
 * frame's check would be. */
#define AT_TAG TERN_FORWARD_HEAD
#define AT_CONTENT (TERN_FORWARD_HEAD + TERN_FORWARD_TAG)
#define CHECK_LEN 8

/* How soon a frame the queue refused is offered again. */
#define RETRY TSIM_S(1)

struct router {
    struct tsim_node *node;
    struct tsim_core_config config;
    struct tern_route route;
    struct tern_route_neighbour *neighbours;
    struct tern_route_dest *dests;
    struct tsim_timer *timer;
    /* The frame the core last gave, kept until it has gone: the core is told when it has. */
    uint8_t frame[TERN_ROUTE_FRAME_MAX];
    size_t len;
    int8_t dbm;
    uint64_t handle; /* in the node's queue, or 0 */
    /* And the same for the frames that follow routes. */
    struct tern_forward forward;
    struct tern_forward_slot *slots;
    uint64_t *fhandles; /* [slot] in the node's queue, or 0 */
    /* One the queue refused, kept to offer again. */
    struct tsim_tx refused;
    uint8_t refused_slot;
    bool has_refused;
    struct sessions *sessions;
};

/* Who sent each message of one network, by its number. A frame does not say where it came from:
 * on a device its destination knows from the session the frame's tag belongs to, which both ends
 * hold. This table stands for those sessions, and is read only by a message's destination, to
 * acknowledge it. Message numbers start again in every network, so each has a table of its own:
 * the nodes of one are those made with one config, and the table lasts as long as any of them. */
struct sessions {
    const void *config; /* whose network it is */
    uint32_t nodes;     /* routers that hold it */
    uint32_t *sender;   /* [message number] */
    size_t cap;
    struct sessions *next;
};

static struct sessions *networks;

static struct sessions *sessions_take(const void *config) {
    struct sessions *s = networks;
    for (; s && s->config != config; s = s->next) {
    }
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) {
            return NULL;
        }
        s->config = config;
        s->next = networks;
        networks = s;
    }
    s->nodes++;
    return s;
}

static void sessions_release(struct sessions *s) {
    if (!s || --s->nodes) {
        return;
    }
    for (struct sessions **p = &networks; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    free(s->sender);
    free(s);
}

static void sender_note(struct sessions *s, uint64_t id, uint32_t src) {
    if (id >= s->cap) {
        size_t cap = s->cap ? s->cap : 1024;
        while (cap <= id) {
            cap *= 2;
        }
        uint32_t *grown = realloc(s->sender, cap * sizeof *grown);
        if (!grown) {
            return;
        }
        /* A number never noted is no one's: no node has this index. */
        for (size_t i = s->cap; i < cap; i++) {
            grown[i] = UINT32_MAX;
        }
        s->sender = grown;
        s->cap = cap;
    }
    s->sender[id] = src;
}

static bool sender_of(const struct sessions *s, uint64_t id, uint32_t *src) {
    if (id >= s->cap || s->sender[id] == UINT32_MAX) {
        return false;
    }
    *src = s->sender[id];
    return true;
}

static uint32_t tag_get(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void tag_put(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

struct tsim_core_config tsim_core_default(uint16_t channel, const struct tsim_lora *lora,
                                          double tx_dbm) {
    return (struct tsim_core_config){
        .channel = channel,
        .lora = *lora,
        .tx_dbm = tx_dbm,
        .tx_min_dbm = -9,
        .neighbours = 255,
        .frames = 16,
        .salvage = 1,
    };
}

const char *tsim_core_check(const struct tsim_core_config *c) {
    if (!(c->tx_dbm >= -128 && c->tx_dbm <= 127) || !(c->tx_min_dbm >= -128) ||
        c->tx_min_dbm > c->tx_dbm) {
        return "tx_min_dbm must be no more than the radio's tx_dbm, and both a whole dBm in a byte";
    }
    if (c->neighbours < 1 || c->neighbours > 255) {
        return "neighbours must be 1 to 255";
    }
    if (c->frames < 1 || c->frames > 255 || c->salvage > TERN_FORWARD_SALVAGE_MAX) {
        return "frames must be 1 to 255, and salvage 0 to 4";
    }
    return NULL;
}

bool tsim_core_relay(const struct tsim_core_config *c, uint32_t node) {
    return c->relay_pick == 0 || (c->relay_set && c->relay_set[node]);
}

static struct tern_lora lora_of(const struct tsim_lora *l) {
    return (struct tern_lora){
        .sf = l->sf,
        .bw_hz = l->bw_hz,
        .cr = l->cr,
        .preamble = l->preamble,
        .implicit_header = l->implicit_header,
        .crc = l->crc,
        .ldro = l->ldro == TSIM_LDRO_ON    ? TERN_LDRO_ON
                : l->ldro == TSIM_LDRO_OFF ? TERN_LDRO_OFF
                                           : TERN_LDRO_AUTO,
    };
}

/* Offers the queue the routing frame in hand, asking the core for one if there is none. */
static void service_routes(struct router *r, tsim_time now) {
    if (r->handle != 0) {
        return; /* its end comes back here */
    }
    if (r->len == 0) {
        r->len = tern_route_poll(&r->route, now, r->frame, &r->dbm);
    }
    if (r->len != 0) {
        bool announce = r->frame[0] == TERN_HDR_ANNOUNCE;
        struct tsim_tx tx = {
            .channel = r->config.channel,
            .lora = r->config.lora,
            .tx_dbm = r->dbm,
            .purpose = announce ? TSIM_PURPOSE_ANNOUNCE : TSIM_PURPOSE_CONTROL,
            .priority = announce ? PRIORITY_ANNOUNCE : PRIORITY_REQUEST,
            .len = (uint32_t)r->len,
        };
        memcpy(tx.bytes, r->frame, r->len);
        r->handle = tsim_node_send(r->node, &tx);
    }
}

/* Tells the books a message of this node's is over, given up on. */
static void failed(struct router *r) {
    uint8_t tag[TERN_FORWARD_TAG];
    while (tern_forward_failed(&r->forward, tag)) {
        uint64_t id = tag_get(tag);
        tsim_node_drop(r->node, id, TSIM_DROP_RETRIES, TSIM_BROADCAST);
        tsim_node_finished(r->node, id);
    }
}

/* And the same for the frames that follow routes: every one the core has ready goes to the queue,
 * which sends them in the order of their priorities. */
static void service_frames(struct router *r, tsim_time now) {
    for (;;) {
        struct tsim_tx *tx = &r->refused;
        uint8_t slot = r->refused_slot;
        if (!r->has_refused) {
            uint8_t frame[TERN_FORWARD_FRAME_MAX];
            struct tern_forward_head h;
            enum tern_forward_kind kind;
            int8_t dbm;
            size_t len = tern_forward_poll(&r->forward, now, frame, &dbm, &kind, &slot);
            failed(r);
            if (len == 0) {
                return;
            }
            bool message = frame[0] == TERN_HDR_MESSAGE;
            *tx = (struct tsim_tx){
                .channel = r->config.channel,
                .lora = r->config.lora,
                .tx_dbm = dbm,
                .purpose = kind == TERN_FORWARD_OWN     ? TSIM_PURPOSE_DATA
                           : kind == TERN_FORWARD_RELAY ? TSIM_PURPOSE_RELAY
                                                        : TSIM_PURPOSE_CONTROL,
                .priority = kind == TERN_FORWARD_OWN     ? PRIORITY_OWN
                            : kind == TERN_FORWARD_RELAY ? PRIORITY_RELAY
                                                         : PRIORITY_REPLY,
                .carries = message ? tag_get(frame + AT_TAG) : 0,
                .carries_at = message ? AT_CONTENT : 0,
                .len = (uint32_t)len,
            };
            if (tern_forward_head_read(&h, frame, len)) {
                tx->addressed = true;
                tx->to = h.next - 1;
            }
            memcpy(tx->bytes, frame, len);
        }
        r->fhandles[slot] = tsim_node_send(r->node, tx);
        r->has_refused = r->fhandles[slot] == 0;
        r->refused_slot = slot;
        if (r->has_refused) {
            return;
        }
    }
}

/* Offers the queue what the core has to send, and sets the timer for when it next has something
 * to do. */
static void service(struct router *r) {
    tsim_time now = tsim_node_now(r->node), due, wait;
    service_routes(r, now);
    service_frames(r, now);
    if ((r->len != 0 && r->handle == 0) || r->has_refused) {
        tsim_timer_start(r->timer, RETRY); /* the queue refused one: offered again */
        return;
    }
    due = r->handle != 0 ? INT64_MAX : tern_route_due(&r->route);
    if (tern_forward_due(&r->forward) < due) {
        due = tern_forward_due(&r->forward);
    }
    if (due == INT64_MAX) {
        return; /* nothing but frames in the queue, whose end comes back here */
    }
    /* Never at once: a core with nothing to send that says it is due now would spin. */
    wait = due - now;
    tsim_timer_start(r->timer, wait > TSIM_MS(1) ? wait : TSIM_MS(1));
}

static void router_fire(void *ctx) { service(ctx); }

/* A frame that follows routes: for the core to pass on, or for this node. */
static void frame_rx(struct router *r, const struct tsim_rx *rx, int16_t snr_q) {
    tsim_time now = tsim_node_now(r->node);
    struct tern_forward_heard got;
    tern_forward_heard(&r->forward, now, rx->bytes, rx->len, snr_q, &got);
    if (got.got == TERN_FORWARD_MESSAGE) {
        uint64_t id = tag_get(rx->bytes + AT_TAG);
        uint8_t ack[TERN_ACK_LEN] = {TERN_HDR_ACK};
        /* Every copy is acknowledged: the one before may have been lost on its way back. */
        tsim_node_deliver(r->node, id);
        uint32_t src;
        if (sender_of(r->sessions, id, &src)) {
            memcpy(ack + AT_TAG, rx->bytes + AT_TAG, TERN_FORWARD_TAG);
            tern_forward_send(&r->forward, now, src + 1, ack, sizeof ack, false, got.back);
        }
    } else if (got.got == TERN_FORWARD_ACK) {
        if (tern_forward_acked(&r->forward, rx->bytes + AT_TAG)) {
            tsim_node_finished(r->node, tag_get(rx->bytes + AT_TAG));
        }
    }
}

static void router_rx(void *self, const struct tsim_rx *rx) {
    struct router *r = self;
    if (rx->channel != r->config.channel) {
        return;
    }
    double q = round(rx->snr_db * 4);
    int16_t snr_q = (int16_t)(q < -128 ? -128 : q > 127 ? 127 : q);
    if (tern_route_frame(rx->bytes, rx->len)) {
        tern_route_heard(&r->route, tsim_node_now(r->node), rx->bytes, rx->len, snr_q);
    } else if (tern_forward_frame(rx->bytes, rx->len)) {
        frame_rx(r, rx, snr_q);
    } else {
        return;
    }
    service(r);
}

/* The slot of a frame in the queue, or `frames` if it is none of this node's. */
static uint32_t frame_slot(const struct router *r, uint64_t handle) {
    uint32_t i = 0;
    for (; i < r->config.frames && r->fhandles[i] != handle; i++) {
    }
    return i;
}

static void router_tx_done(void *self, uint64_t handle) {
    struct router *r = self;
    if (handle == 0) {
        return;
    }
    if (handle == r->handle) {
        tern_route_sent(&r->route, tsim_node_now(r->node));
        r->handle = 0;
        r->len = 0;
    } else {
        uint32_t slot = frame_slot(r, handle);
        if (slot == r->config.frames) {
            return;
        }
        tern_forward_sent(&r->forward, tsim_node_now(r->node), (uint8_t)slot);
        r->fhandles[slot] = 0;
    }
    service(r);
}

/* The MAC is about to send a frame: one whose next hop has since been heard passing on an earlier
 * copy is taken back. */
static bool router_sending(void *self, uint64_t handle) {
    struct router *r = self;
    uint32_t slot = handle ? frame_slot(r, handle) : r->config.frames;
    if (slot == r->config.frames || tern_forward_wanted(&r->forward, (uint8_t)slot)) {
        return true;
    }
    tern_forward_withdrawn(&r->forward, tsim_node_now(r->node), (uint8_t)slot);
    r->fhandles[slot] = 0;
    tsim_timer_start(r->timer, TSIM_MS(1)); /* nothing may be sent from in here */
    return false;
}

/* A message goes as the secured unicast frame would: the head, its tag, its content and its
 * check. The core has no broadcast, and a broadcast is refused. */
static bool router_originate(void *self, const struct tsim_message *msg) {
    struct router *r = self;
    uint8_t frame[TERN_FORWARD_FRAME_MAX] = {TERN_HDR_MESSAGE};
    size_t len = AT_CONTENT + msg->len + CHECK_LEN;
    if (msg->dst == TSIM_BROADCAST || len > sizeof frame || msg->id > UINT32_MAX) {
        return false;
    }
    sender_note(r->sessions, msg->id, msg->src);
    tag_put(frame + AT_TAG, (uint32_t)msg->id);
    memcpy(frame + AT_CONTENT, msg->content, msg->len);
    if (!tern_forward_send(&r->forward, tsim_node_now(r->node), msg->dst + 1, frame, len, true,
                           INT8_MIN)) {
        tsim_node_drop(r->node, msg->id, TSIM_DROP_QUEUE, TSIM_BROADCAST);
        tsim_node_finished(r->node, msg->id);
        return true;
    }
    service(r);
    return true;
}

static void router_start(void *self) { service(self); }

static void router_destroy(void *self) {
    struct router *r = self;
    if (!r) {
        return;
    }
    tsim_timer_destroy(r->timer);
    free(r->neighbours);
    free(r->dests);
    free(r->slots);
    free(r->fhandles);
    sessions_release(r->sessions);
    free(r);
}

static void *router_create(struct tsim_node *node, const void *config) {
    const struct tsim_core_config *c = config;
    if (tsim_core_check(c)) {
        return NULL;
    }
    struct router *r = calloc(1, sizeof *r);
    if (!r) {
        return NULL;
    }
    r->node = node;
    r->config = *c;
    uint32_t self = tsim_node_index(node);
    size_t dests = c->destinations ? c->destinations : tsim_node_count(node);
    r->neighbours = calloc(c->neighbours, sizeof *r->neighbours);
    r->dests = calloc(dests, sizeof *r->dests);
    r->slots = calloc(c->frames, sizeof *r->slots);
    r->fhandles = calloc(c->frames, sizeof *r->fhandles);
    r->sessions = sessions_take(config);
    r->timer = tsim_timer_create(node, router_fire, r);
    if (!r->neighbours || !r->dests || !r->slots || !r->fhandles || !r->sessions || !r->timer) {
        router_destroy(r);
        return NULL;
    }
    struct tern_lora lora = lora_of(&c->lora);
    struct tern_route_config rc = tern_route_defaults(
        &lora, (int8_t)round(c->tx_dbm), (int8_t)round(c->tx_min_dbm), tsim_core_relay(c, self));
    struct tsim_rng rng;
    tsim_node_rng(node, TSIM_STREAM_ROUTING, &rng);
    /* A node that comes back from being powered down is made again, here: it has kept nothing, as
     * a board that kept no sequence number would not have. */
    tern_route_init(&r->route, &rc, self + 1, r->neighbours, c->neighbours, r->dests, dests, 0,
                    tsim_rng_next(&rng), tsim_node_now(node));
    struct tern_forward_config fc = tern_forward_defaults();
    fc.salvage = (uint8_t)c->salvage;
    tern_forward_init(&r->forward, &fc, &r->route, r->slots, c->frames, tsim_rng_next(&rng));
    return r;
}

static bool router_next_hop(const void *self, uint32_t dst, uint32_t *next) {
    const struct router *r = self;
    uint32_t id;
    uint16_t metric;
    if (!tern_route_next(&r->route, dst + 1, &id, &metric)) {
        return false;
    }
    *next = id - 1;
    return true;
}

const struct tsim_routing tsim_core = {
    .name = "core",
    .create = router_create,
    .destroy = router_destroy,
    .start = router_start,
    .originate = router_originate,
    .rx = router_rx,
    .tx_done = router_tx_done,
    .sending = router_sending,
    .reports_finished = true,
    .next_hop = router_next_hop,
};
