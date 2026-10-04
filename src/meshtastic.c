#include "tsim/meshtastic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tsim/rng.h"

/* What the routing tells its MAC in a frame's hint: whether the frame is a rebroadcast, whether a
 * router is sending it, and the SNR it was heard at, in quarter dB. */
#define HINT_RELAY (1u << 31)
#define HINT_ROUTER (1u << 30)

static uint32_t hint_for(double snr_db, bool router) {
    double q = round(snr_db * 4.0);
    q = q < INT16_MIN ? INT16_MIN : q > INT16_MAX ? INT16_MAX : q;
    return HINT_RELAY | (router ? HINT_ROUTER : 0) | (uint16_t)(int16_t)q;
}

static double hint_snr(uint32_t hint) { return (int16_t)(uint16_t)(hint & 0xFFFF) / 4.0; }

tsim_time tsim_meshtastic_slot(const struct tsim_lora *lora) {
    return tsim_lora_symbol(lora) * 5 / 2 + TSIM_US(7600);
}

struct tsim_meshtastic_window tsim_meshtastic_window_default(const struct tsim_lora *lora) {
    return (struct tsim_meshtastic_window){
        .slot = tsim_meshtastic_slot(lora),
        .cw_min = 3,
        .cw_max = 8,
    };
}

uint64_t tsim_meshtastic_window_slots(const struct tsim_meshtastic_window *w) {
    return ((uint64_t)2 << w->cw_max) + 2 * (uint64_t)w->cw_max;
}

bool tsim_meshtastic_window_valid(const struct tsim_meshtastic_window *w) {
    return w->cw_min <= w->cw_max && w->cw_max <= 15 && w->slot > 0 &&
           (uint64_t)w->slot <= TSIM_MESHTASTIC_WAIT_MAX / tsim_meshtastic_window_slots(w);
}

/* The share of the run the radio has spent sending or receiving, as a percentage. */
static double utilisation(const struct tsim_node *node) {
    tsim_time now = tsim_node_now(node);
    if (now <= 0) {
        return 0;
    }
    double busy = (double)(tsim_node_tx_airtime(node) + tsim_node_rx_airtime(node));
    double percent = busy / (double)now * 100.0;
    return percent > 100.0 ? 100.0 : percent;
}

/* The window for a node's own traffic: cw_min on a quiet channel, cw_max on a full one. */
static int window_by_utilisation(const struct tsim_node *node,
                                 const struct tsim_meshtastic_window *w) {
    return (int)(utilisation(node) * (w->cw_max - w->cw_min) / 100.0) + w->cw_min;
}

/* --- The MAC --- */

struct mac {
    struct tsim_node *node;
    struct tsim_meshtastic_mac_config config;
    struct tsim_rng rng;
    struct tsim_timer *timer;
    uint64_t waiting; /* the frame the timer was drawn for */
};

struct tsim_meshtastic_mac_config tsim_meshtastic_mac_default(const struct tsim_lora *lora) {
    return (struct tsim_meshtastic_mac_config){
        .window = tsim_meshtastic_window_default(lora),
        .snr_min_db = -20,
        .snr_max_db = 10,
        .busy_chance = 0,
    };
}

/* What a frame's wait is drawn from: a fixed part, then 0 to `slots` slots. */
struct mac_wait {
    tsim_time fixed;
    uint64_t slots;
};

static struct mac_wait wait_for(const struct mac *m, const struct tsim_tx *tx) {
    const struct tsim_meshtastic_window *w = &m->config.window;
    if (!(tx->hint & HINT_RELAY)) {
        return (struct mac_wait){0, (uint64_t)1 << window_by_utilisation(m->node, w)};
    }
    double lo = m->config.snr_min_db;
    double hi = m->config.snr_max_db;
    double snr = hint_snr(tx->hint);
    snr = snr < lo ? lo : snr > hi ? hi : snr;
    int cw = (int)((snr - lo) * (w->cw_max - w->cw_min) / (hi - lo)) + w->cw_min;
    if (tx->hint & HINT_ROUTER) {
        return (struct mac_wait){0, 2 * (uint64_t)cw};
    }
    return (struct mac_wait){2 * (tsim_time)w->cw_max * w->slot, (uint64_t)1 << cw};
}

static tsim_time draw(struct mac *m, struct mac_wait wait) {
    return wait.fixed + (tsim_time)tsim_rng_below(&m->rng, wait.slots + 1) * m->config.window.slot;
}

/* The wait after finding the channel busy. Looking again at the same instant would find it the
 * same, so a wait of nothing is no wait at all: the draw is from the waits that take some time,
 * which is what drawing again until one does would come to, and a window with none - a router's
 * at cw 0 - waits one slot. */
static tsim_time draw_busy(struct mac *m, struct mac_wait wait) {
    if (wait.fixed > 0) {
        return draw(m, wait);
    }
    uint64_t n = wait.slots ? 1 + tsim_rng_below(&m->rng, wait.slots) : 1;
    return (tsim_time)n * m->config.window.slot;
}

static void mac_fire(void *ctx) {
    struct mac *m = ctx;
    const struct tsim_tx *head = tsim_node_head(m->node);
    if (!head) {
        return;
    }
    if (tsim_node_receiving(m->node) || tsim_node_cad(m->node) ||
        (m->config.busy_chance > 0 && tsim_rng_unit(&m->rng) < m->config.busy_chance)) {
        tsim_timer_start(m->timer, draw_busy(m, wait_for(m, head)));
        return;
    }
    tsim_node_transmit(m->node);
}

static void mac_kick(void *self) {
    struct mac *m = self;
    /* Still sending: the frame's end kicks again, and the next frame draws its wait then. */
    if (tsim_node_sending(m->node)) {
        return;
    }
    uint64_t head = tsim_node_head_handle(m->node);
    if (head == 0) {
        tsim_timer_stop(m->timer);
        m->waiting = 0;
        return;
    }
    if (head == m->waiting && tsim_timer_pending(m->timer)) {
        return;
    }
    /* A new head - the first, or one that took a cancelled frame's place - draws its own wait. */
    m->waiting = head;
    tsim_timer_start(m->timer, draw(m, wait_for(m, tsim_node_head(m->node))));
}

static void *mac_create(struct tsim_node *node, const void *config) {
    const struct tsim_meshtastic_mac_config *c = config;
    if (!tsim_meshtastic_window_valid(&c->window) || !(c->snr_min_db < c->snr_max_db) ||
        !(c->busy_chance >= 0 && c->busy_chance <= 1)) {
        return NULL;
    }
    struct mac *m = calloc(1, sizeof *m);
    if (!m) {
        return NULL;
    }
    m->node = node;
    m->config = *c;
    tsim_node_rng(node, TSIM_STREAM_MAC, &m->rng);
    m->timer = tsim_timer_create(node, mac_fire, m);
    if (!m->timer) {
        free(m);
        return NULL;
    }
    return m;
}

static void mac_destroy(void *self) {
    struct mac *m = self;
    tsim_timer_destroy(m->timer);
    free(m);
}

const struct tsim_mac tsim_meshtastic_mac = {
    .name = "meshtastic",
    .create = mac_create,
    .destroy = mac_destroy,
    .kick = mac_kick,
};

/* --- The routing --- */

/* The header, little-endian as on the air. */
#define AT_TO 0
#define AT_FROM 4
#define AT_ID 8
#define AT_FLAGS 12
#define AT_CHANNEL 13
#define AT_NEXT_HOP 14
#define AT_RELAY 15
#define AT_PORT 16

#define FLAG_HOPS 0x07u
#define FLAG_WANT_ACK 0x08u
#define FLAG_HOP_START_SHIFT 5

/* What the payload is. The numbers are the firmware's port numbers for text and for routing. */
#define PORT_TEXT 1
#define PORT_ROUTING 5

#define ACK_LEN (TSIM_MESHTASTIC_OVERHEAD + 4)

/* Message ids are the network's, from 1. A node numbers its acknowledgements with the top bit set,
 * so its two kinds of packet never share an id. */
#define ACK_ID_BIT (1u << 31)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A packet this node has heard, by sender and id. */
struct heard {
    uint64_t key; /* sender << 32 | id; 0 is an empty slot, since no packet has id 0 */
    uint32_t times;
    uint64_t relay; /* the rebroadcast it queued, or 0 */
};

/* A message of this node's waiting for an acknowledgement. */
struct awaiting {
    struct router *owner;
    struct awaiting *next;
    uint32_t id;
    uint8_t retries; /* left */
    bool acked;      /* polling: an acknowledgement has come, to be noticed when the wait ends */
    struct tsim_timer *timer;
    struct tsim_tx tx;
    /* Its copies in the queue or on the air. Waiting from the queue, a retry can be queued before
     * the copy ahead of it has gone, so there can be one for every send, retries + 1. */
    uint16_t queued_count;
    uint64_t queued[];
};

struct router {
    struct tsim_node *node;
    uint32_t self;
    struct tsim_meshtastic_config config;
    struct heard *heard; /* open addressing, a power of two long */
    size_t heard_cap;
    size_t heard_count;
    struct awaiting *awaiting;
    uint32_t next_ack;
    /* Cancelling late: frames no longer wanted, withdrawn when the MAC comes to send them. */
    uint64_t *withdrawn;
    size_t withdrawn_count;
    size_t withdrawn_cap;
    uint64_t last_sent; /* the handle of its last frame to go on the air */
};

struct tsim_meshtastic_config tsim_meshtastic_default(uint16_t channel,
                                                      const struct tsim_lora *lora, double tx_dbm) {
    return (struct tsim_meshtastic_config){
        .channel = channel,
        .lora = *lora,
        .tx_dbm = tx_dbm,
        .role = TSIM_MESHTASTIC_CLIENT,
        .hop_limit = 3,
        .want_ack = true,
        .ack_duplicates = true,
        .noise_dbm = NAN,
        .retries = 3,
        .processing = TSIM_MS(4500),
        .window = tsim_meshtastic_window_default(lora),
    };
}

static size_t slot_of(uint64_t key, size_t cap) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (size_t)key & (cap - 1);
}

/* The record of a packet, made if it is new, or NULL when memory runs out. */
static struct heard *recall(struct router *r, uint64_t key) {
    if (2 * (r->heard_count + 1) > r->heard_cap) {
        size_t cap = r->heard_cap ? 2 * r->heard_cap : 64;
        struct heard *grown = calloc(cap, sizeof *grown);
        if (!grown) {
            return NULL;
        }
        for (size_t i = 0; i < r->heard_cap; i++) {
            if (r->heard[i].key) {
                size_t at = slot_of(r->heard[i].key, cap);
                while (grown[at].key) {
                    at = (at + 1) & (cap - 1);
                }
                grown[at] = r->heard[i];
            }
        }
        free(r->heard);
        r->heard = grown;
        r->heard_cap = cap;
    }
    size_t at = slot_of(key, r->heard_cap);
    while (r->heard[at].key && r->heard[at].key != key) {
        at = (at + 1) & (r->heard_cap - 1);
    }
    if (!r->heard[at].key) {
        r->heard[at].key = key;
        r->heard_count++;
    }
    return &r->heard[at];
}

static struct tsim_tx frame_for(const struct router *r, enum tsim_purpose purpose) {
    return (struct tsim_tx){
        .channel = r->config.channel,
        .lora = r->config.lora,
        .tx_dbm = r->config.tx_dbm,
        .purpose = purpose,
    };
}

static void header(struct tsim_tx *tx, const struct router *r, uint32_t to, uint32_t id,
                   bool want_ack, uint8_t port) {
    uint8_t hops = r->config.hop_limit;
    put32(tx->bytes + AT_TO, to);
    put32(tx->bytes + AT_FROM, r->self);
    put32(tx->bytes + AT_ID, id);
    tx->bytes[AT_FLAGS] =
        (uint8_t)(hops | (want_ack ? FLAG_WANT_ACK : 0) | hops << FLAG_HOP_START_SHIFT);
    tx->bytes[AT_CHANNEL] = 0;
    tx->bytes[AT_NEXT_HOP] = 0;
    tx->bytes[AT_RELAY] = (uint8_t)r->self;
    tx->bytes[AT_PORT] = port;
}

/* How long to wait for an acknowledgement of `tx`: two of its airtimes, the longest contention
 * window the channel's utilisation allows, two of the largest, one of the middle size, and the
 * processing time. */
static tsim_time ack_wait(const struct router *r, const struct tsim_tx *tx) {
    const struct tsim_meshtastic_window *w = &r->config.window;
    int cw = window_by_utilisation(r->node, w);
    uint64_t n = ((uint64_t)1 << cw) + 2 * (uint64_t)w->cw_max +
                 ((uint64_t)1 << ((w->cw_max + w->cw_min) / 2));
    return 2 * tsim_lora_airtime(&tx->lora, tx->len) + (tsim_time)n * w->slot +
           r->config.processing;
}

/* Takes back a frame the routing no longer wants: now, or, cancelling late, when the MAC comes to
 * send it. Its frames share one priority, so they leave the queue in the order they were queued,
 * and a handle no later than the last sent is no longer queued. */
static void withdraw(struct router *r, uint64_t handle) {
    if (!r->config.cancel_late) {
        tsim_node_cancel(r->node, handle);
        return;
    }
    if (handle <= r->last_sent) {
        return;
    }
    if (r->withdrawn_count == r->withdrawn_cap) {
        size_t cap = r->withdrawn_cap ? 2 * r->withdrawn_cap : 8;
        uint64_t *grown = realloc(r->withdrawn, cap * sizeof *grown);
        if (!grown) {
            tsim_node_cancel(r->node, handle); /* out of memory: cancelled now instead */
            return;
        }
        r->withdrawn = grown;
        r->withdrawn_cap = cap;
    }
    r->withdrawn[r->withdrawn_count++] = handle;
}

static void forget(struct awaiting *a) {
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

/* Done with the message, acknowledged or given up on: the application may make the next. */
static void finish(struct awaiting *a) {
    tsim_node_finished(a->owner->node, a->id);
    forget(a);
}

/* Queues the message's frame and waits from when it has gone - or, polling, from now. If the
 * queue refuses it, it waits as though it had gone. */
static void send_awaited(struct awaiting *a) {
    struct router *r = a->owner;
    uint64_t handle = tsim_node_send(r->node, &a->tx);
    if (handle) {
        a->queued[a->queued_count++] = handle;
    }
    if (handle == 0 || r->config.ack_poll) {
        tsim_timer_start(a->timer, ack_wait(r, &a->tx));
    }
}

static void ack_timeout(void *ctx) {
    struct awaiting *a = ctx;
    if (a->acked || a->retries == 0) {
        finish(a);
        return;
    }
    a->retries--;
    send_awaited(a);
}

static void acknowledged(struct router *r, uint32_t id) {
    for (struct awaiting *a = r->awaiting; a; a = a->next) {
        if (a->id == id) {
            while (a->queued_count > 0) {
                withdraw(r, a->queued[--a->queued_count]);
            }
            if (r->config.ack_poll) {
                a->acked = true;
            } else {
                finish(a);
            }
            return;
        }
    }
}

static bool router_originate(void *self, const struct tsim_message *msg) {
    struct router *r = self;
    if (msg->len > TSIM_FRAME_MAX - TSIM_MESHTASTIC_OVERHEAD || msg->id >= ACK_ID_BIT) {
        return false; /* one frame or nothing, and an id the header can carry */
    }
    uint32_t id = (uint32_t)msg->id;
    struct tsim_tx tx = frame_for(r, TSIM_PURPOSE_DATA);
    header(&tx, r, msg->dst, id, r->config.want_ack, PORT_TEXT);
    memcpy(tx.bytes + TSIM_MESHTASTIC_OVERHEAD, msg->content, msg->len);
    tx.carries = msg->id;
    tx.carries_at = TSIM_MESHTASTIC_OVERHEAD;
    tx.len = TSIM_MESHTASTIC_OVERHEAD + msg->len;
    if (!r->config.want_ack) {
        tsim_node_send(r->node, &tx);
        tsim_node_finished(r->node, id);
        return true;
    }
    struct awaiting *a = calloc(1, sizeof *a + ((size_t)r->config.retries + 1) * sizeof(uint64_t));
    if (a) {
        a->timer = tsim_timer_create(r->node, ack_timeout, a);
    }
    if (!a || !a->timer) {
        free(a);
        tsim_node_send(r->node, &tx); /* out of memory: sent, but never retried */
        tsim_node_finished(r->node, id);
        return true;
    }
    a->owner = r;
    a->id = id;
    a->retries = r->config.retries;
    a->tx = tx;
    a->next = r->awaiting;
    r->awaiting = a;
    send_awaited(a);
    return true;
}

static void send_ack(struct router *r, uint32_t to, uint32_t id) {
    struct tsim_tx tx = frame_for(r, TSIM_PURPOSE_CONTROL);
    header(&tx, r, to, ACK_ID_BIT | r->next_ack++, false, PORT_ROUTING);
    put32(tx.bytes + TSIM_MESHTASTIC_OVERHEAD, id);
    tx.len = ACK_LEN;
    tsim_node_send(r->node, &tx);
}

static void router_rx(void *self, const struct tsim_rx *rx) {
    struct router *r = self;
    if (rx->len < TSIM_MESHTASTIC_OVERHEAD) {
        return;
    }
    const uint8_t *b = rx->bytes;
    uint32_t to = get32(b + AT_TO);
    uint32_t from = get32(b + AT_FROM);
    uint32_t id = get32(b + AT_ID);
    uint8_t hops = b[AT_FLAGS] & FLAG_HOPS;
    bool data = b[AT_PORT] == PORT_TEXT;
    bool ack = b[AT_PORT] == PORT_ROUTING && rx->len >= ACK_LEN;

    if (from == r->self) {
        if (data) {
            acknowledged(r, id); /* someone rebroadcast it: the implicit acknowledgement */
        }
        return;
    }
    struct heard *h = recall(r, (uint64_t)from << 32 | id);
    if (!h) {
        return; /* out of memory: treated as heard, which can stop a flood but never start one */
    }
    h->times++;
    if (h->times > 1) {
        /* A retry that reached the destination: its sender may have missed the acknowledgement. */
        if (to == r->self && data && (b[AT_FLAGS] & FLAG_WANT_ACK) && r->config.ack_duplicates) {
            send_ack(r, from, id);
        }
        uint32_t enough = r->config.role == TSIM_MESHTASTIC_ROUTER ? 3 : 2;
        if (h->relay && h->times >= enough) {
            withdraw(r, h->relay);
            h->relay = 0;
        }
        return;
    }

    if (to == r->self) {
        if (data) {
            tsim_node_deliver(r->node, id);
            if (b[AT_FLAGS] & FLAG_WANT_ACK) {
                send_ack(r, from, id);
            }
        } else if (ack) {
            acknowledged(r, get32(b + TSIM_MESHTASTIC_OVERHEAD));
        }
        return;
    }
    if (data && to == TSIM_BROADCAST) {
        tsim_node_deliver(r->node, id);
    }
    if (hops == 0 || r->config.role == TSIM_MESHTASTIC_CLIENT_MUTE || !(data || ack)) {
        return;
    }
    struct tsim_tx tx = frame_for(r, data ? TSIM_PURPOSE_RELAY : TSIM_PURPOSE_CONTROL);
    memcpy(tx.bytes, b, rx->len);
    tx.len = rx->len;
    tx.bytes[AT_FLAGS] = (uint8_t)((b[AT_FLAGS] & ~FLAG_HOPS) | (hops - 1));
    tx.bytes[AT_RELAY] = (uint8_t)r->self;
    tx.hint = hint_for(isnan(r->config.noise_dbm) ? rx->snr_db : rx->rssi_dbm - r->config.noise_dbm,
                       r->config.role == TSIM_MESHTASTIC_ROUTER);
    if (data) {
        tx.carries = id;
        tx.carries_at = TSIM_MESHTASTIC_OVERHEAD;
    }
    h->relay = tsim_node_send(r->node, &tx);
}

static void router_tx_done(void *self, uint64_t handle) {
    struct router *r = self;
    r->last_sent = handle;
    /* Anything withdrawn while it was on the air, too late to stop. */
    for (size_t i = 0; i < r->withdrawn_count;) {
        if (r->withdrawn[i] <= handle) {
            r->withdrawn[i] = r->withdrawn[--r->withdrawn_count];
        } else {
            i++;
        }
    }
    for (struct awaiting *a = r->awaiting; a; a = a->next) {
        for (uint16_t i = 0; i < a->queued_count; i++) {
            if (a->queued[i] == handle) {
                a->queued[i] = a->queued[--a->queued_count];
                if (!r->config.ack_poll) {
                    tsim_timer_start(a->timer, ack_wait(r, &a->tx));
                }
                return;
            }
        }
    }
}

/* Cancelling late: the MAC has waited out this frame's turn, and only now is it withdrawn. */
static bool router_sending(void *self, uint64_t handle) {
    struct router *r = self;
    for (size_t i = 0; i < r->withdrawn_count; i++) {
        if (r->withdrawn[i] == handle) {
            r->withdrawn[i] = r->withdrawn[--r->withdrawn_count];
            return false;
        }
    }
    return true;
}

static void *router_create(struct tsim_node *node, const void *config) {
    const struct tsim_meshtastic_config *c = config;
    if (c->hop_limit > TSIM_MESHTASTIC_HOPS_MAX || c->processing < 0 ||
        c->processing > TSIM_MESHTASTIC_WAIT_MAX || !tsim_meshtastic_window_valid(&c->window) ||
        (unsigned)c->role > TSIM_MESHTASTIC_ROUTER || (c->relay_pick && !c->relay_set) ||
        (!isnan(c->noise_dbm) && !(fabs(c->noise_dbm) <= 1e3))) {
        return NULL;
    }
    struct router *r = calloc(1, sizeof *r);
    if (!r) {
        return NULL;
    }
    r->node = node;
    r->self = tsim_node_index(node);
    r->config = *c;
    if (c->relay_pick && c->relay_set[r->self]) {
        r->config.role = TSIM_MESHTASTIC_ROUTER;
    }
    return r;
}

static void router_destroy(void *self) {
    struct router *r = self;
    while (r->awaiting) {
        forget(r->awaiting);
    }
    free(r->heard);
    free(r->withdrawn);
    free(r);
}

const struct tsim_routing tsim_meshtastic = {
    .name = "meshtastic",
    .create = router_create,
    .destroy = router_destroy,
    .originate = router_originate,
    .rx = router_rx,
    .tx_done = router_tx_done,
    .sending = router_sending,
    .reports_finished = true,
};
