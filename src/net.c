#include "tsim/net.h"

#include <stdlib.h>
#include <string.h>

struct queued {
    uint64_t handle;
    struct tsim_tx tx;
};

struct tsim_node {
    struct tsim_net *net;
    uint32_t index;
    void *routing;
    void *mac;
    struct queued *queue; /* by priority, then by handle */
    size_t queue_len;
    size_t queue_cap;
    /* The frame on the air. The radio carries a pointer to it as the frame's payload, so it stays
     * put until the radio reports the frame done, after every receiver has had it. */
    bool sending;
    struct queued air;
    tsim_time air_end;
    struct tsim_ledger ledger;
    struct tsim_net_stats stats;
    struct tsim_timer *timers; /* every timer its plugins hold, so destroy can free them */
};

struct tsim_timer {
    struct tsim_node *node;
    void (*fn)(void *ctx);
    void *ctx;
    struct tsim_event event;
    struct tsim_timer *prev;
    struct tsim_timer *next;
};

struct record {
    struct tsim_message_record r;
    uint8_t *content; /* r.msg.content points here */
    uint8_t *held;    /* nodes that hold it: one bit per node */
    uint8_t *got; /* a broadcast's destinations that have it: one bit per node, made on first use */
};

static bool bit_get(const uint8_t *bits, uint32_t i) { return bits[i / 8] & (1u << (i % 8)); }

static void bit_set(uint8_t *bits, uint32_t i) { bits[i / 8] |= (uint8_t)(1u << (i % 8)); }

struct tsim_net {
    struct tsim_sched *sched;
    struct tsim_phy *phy;
    struct tsim_net_params params;
    const struct tsim_routing *routing;
    const struct tsim_mac *mac;
    uint32_t n;
    struct tsim_node *nodes;
    uint64_t next_handle;
    struct record *messages;
    size_t message_count;
    size_t message_cap;
    bool started;
    tsim_net_delivered_fn observer;
    void *observer_ctx;
    tsim_net_finished_fn finished;
    void *finished_ctx;
};

static void on_rx(void *ctx, uint32_t node, const struct tsim_frame *frame, double rssi_dbm,
                  double snr_db) {
    struct tsim_net *net = ctx;
    const struct queued *sent = frame->payload;
    struct tsim_rx rx = {
        .bytes = sent->tx.bytes,
        .len = sent->tx.len,
        .channel = frame->channel,
        .lora = frame->lora,
        .rssi_dbm = rssi_dbm,
        .snr_db = snr_db,
    };
    if (sent->tx.carries) {
        bit_set(net->messages[sent->tx.carries - 1].held, node);
    }
    struct tsim_node *nd = &net->nodes[node];
    net->routing->rx(nd->routing, &rx);
    net->mac->kick(nd->mac);
}

static void on_tx_done(void *ctx, uint32_t node, const struct tsim_frame *frame) {
    (void)frame;
    struct tsim_net *net = ctx;
    struct tsim_node *nd = &net->nodes[node];
    nd->sending = false;
    if (net->routing->tx_done) {
        net->routing->tx_done(nd->routing, nd->air.handle);
    }
    net->mac->kick(nd->mac);
}

struct tsim_net_params tsim_net_defaults(uint64_t seed) {
    return (struct tsim_net_params){
        .phy = tsim_phy_defaults(),
        .channel = 0,
        .listen = tsim_lora_default(7, 125000),
        .queue_limit = 16,
        .seed = seed,
    };
}

struct tsim_net *tsim_net_create(struct tsim_sched *sched, const struct tsim_net_params *params,
                                 uint32_t nodes, const struct tsim_routing *routing,
                                 const void *routing_config, const struct tsim_mac *mac,
                                 const void *mac_config) {
    if (nodes == 0 || !routing || !mac) {
        return NULL;
    }
    struct tsim_net *net = calloc(1, sizeof *net);
    if (!net) {
        return NULL;
    }
    net->sched = sched;
    net->params = *params;
    net->routing = routing;
    net->mac = mac;
    net->n = nodes;
    net->next_handle = 1;
    net->nodes = calloc(nodes, sizeof *net->nodes);
    struct tsim_phy_params phy = params->phy;
    phy.fading_seed = params->seed;
    net->phy = tsim_phy_create(sched, &phy, nodes, params->channel, &params->listen,
                               (struct tsim_phy_hooks){on_rx, on_tx_done, net});
    if (!net->nodes || !net->phy) {
        tsim_net_destroy(net);
        return NULL;
    }
    for (uint32_t i = 0; i < nodes; i++) {
        net->nodes[i].net = net;
        net->nodes[i].index = i;
    }
    /* MACs first, so a routing plugin can send from start(). */
    for (uint32_t i = 0; i < nodes; i++) {
        net->nodes[i].mac = mac->create(&net->nodes[i], mac_config);
        if (!net->nodes[i].mac) {
            tsim_net_destroy(net);
            return NULL;
        }
    }
    for (uint32_t i = 0; i < nodes; i++) {
        net->nodes[i].routing = routing->create(&net->nodes[i], routing_config);
        if (!net->nodes[i].routing) {
            tsim_net_destroy(net);
            return NULL;
        }
    }
    return net;
}

void tsim_net_start(struct tsim_net *net) {
    if (net->started) {
        return;
    }
    net->started = true;
    if (net->routing->start) {
        for (uint32_t i = 0; i < net->n; i++) {
            net->routing->start(net->nodes[i].routing);
        }
    }
}

void tsim_net_destroy(struct tsim_net *net) {
    if (!net) {
        return;
    }
    for (uint32_t i = 0; net->nodes && i < net->n; i++) {
        if (net->nodes[i].routing) {
            net->routing->destroy(net->nodes[i].routing);
        }
    }
    for (uint32_t i = 0; net->nodes && i < net->n; i++) {
        if (net->nodes[i].mac) {
            net->mac->destroy(net->nodes[i].mac);
        }
        free(net->nodes[i].queue);
        while (net->nodes[i].timers) {
            tsim_timer_destroy(net->nodes[i].timers);
        }
    }
    tsim_phy_destroy(net->phy);
    for (size_t i = 0; i < net->message_count; i++) {
        free(net->messages[i].content);
        free(net->messages[i].held);
        free(net->messages[i].got);
    }
    free(net->messages);
    free(net->nodes);
    free(net);
}

struct tsim_phy *tsim_net_phy(struct tsim_net *net) { return net->phy; }

struct tsim_sched *tsim_net_sched(struct tsim_net *net) { return net->sched; }

uint32_t tsim_net_nodes(const struct tsim_net *net) { return net->n; }

struct tsim_node *tsim_net_node(struct tsim_net *net, uint32_t node) {
    return node < net->n ? &net->nodes[node] : NULL;
}

void *tsim_net_routing(struct tsim_net *net, uint32_t node) {
    return node < net->n ? net->nodes[node].routing : NULL;
}

const struct tsim_routing *tsim_net_routing_plugin(const struct tsim_net *net) {
    return net->routing;
}

/* Marks a message finished and tells the application, once. */
static bool finish(struct tsim_net *net, uint64_t msg) {
    struct tsim_message_record *r = &net->messages[msg - 1].r;
    if (r->finished) {
        return false;
    }
    r->finished = true;
    if (net->finished) {
        net->finished(net->finished_ctx, r);
    }
    return true;
}

uint64_t tsim_net_originate(struct tsim_net *net, uint32_t src, uint32_t dst, uint32_t len) {
    if (src >= net->n || (dst >= net->n && dst != TSIM_BROADCAST) || dst == src ||
        len > TSIM_FRAME_MAX) {
        return 0;
    }
    if (net->message_count == net->message_cap) {
        size_t cap = net->message_cap ? net->message_cap * 2 : 64;
        struct record *grown = realloc(net->messages, cap * sizeof *grown);
        if (!grown) {
            return 0;
        }
        net->messages = grown;
        net->message_cap = cap;
    }
    uint64_t id = net->message_count + 1;
    uint8_t *content = malloc(len ? len : 1);
    uint8_t *held = calloc((net->n + 7) / 8, 1);
    if (!content || !held) {
        free(content);
        free(held);
        return 0;
    }
    /* Random, so that no plugin can produce a message's content without having received it. Its
     * own stream space, apart from every node's. */
    struct tsim_rng rng;
    tsim_rng_init(&rng, net->params.seed, UINT64_C(0xC0) << 56 | id);
    for (uint32_t i = 0; i < len; i += 8) {
        uint64_t r = tsim_rng_next(&rng);
        for (uint32_t k = i; k < len && k < i + 8; k++) {
            content[k] = (uint8_t)(r >> (8 * (k - i)));
        }
    }
    bit_set(held, src);
    struct tsim_message msg = {
        .id = id,
        .src = src,
        .dst = dst,
        .len = len,
        .created = tsim_sched_now(net->sched),
        .content = content,
    };
    net->messages[net->message_count++] = (struct record){
        .r = {.msg = msg, .wanted = dst == TSIM_BROADCAST ? net->n - 1 : 1},
        .content = content,
        .held = held,
    };
    if (!net->routing->originate(net->nodes[src].routing, &msg)) {
        net->messages[msg.id - 1].r.refused = true;
        net->nodes[src].stats.refused++;
        finish(net, msg.id);
    } else if (!net->routing->reports_finished) {
        finish(net, msg.id);
    }
    return msg.id;
}

const struct tsim_ledger *tsim_net_ledger(const struct tsim_net *net, uint32_t node) {
    return &net->nodes[node].ledger;
}

void tsim_net_ledger_now(const struct tsim_net *net, uint32_t node, struct tsim_ledger *out) {
    const struct tsim_node *nd = &net->nodes[node];
    *out = nd->ledger;
    tsim_time ahead = nd->air_end - tsim_sched_now(net->sched);
    if (nd->sending && ahead > 0) {
        out->airtime[nd->air.tx.purpose] -= ahead;
    }
}

tsim_time tsim_ledger_airtime(const struct tsim_ledger *ledger) {
    tsim_time total = 0;
    for (int p = 0; p < TSIM_PURPOSE_COUNT; p++) {
        total += ledger->airtime[p];
    }
    return total;
}

const struct tsim_net_stats *tsim_net_stats(const struct tsim_net *net, uint32_t node) {
    return &net->nodes[node].stats;
}

const struct tsim_message_record *tsim_net_message(const struct tsim_net *net, uint64_t id) {
    if (id == 0 || id > net->message_count) {
        return NULL;
    }
    return &net->messages[id - 1].r;
}

uint64_t tsim_net_message_count(const struct tsim_net *net) { return net->message_count; }

void tsim_net_observe(struct tsim_net *net, tsim_net_delivered_fn fn, void *ctx) {
    net->observer = fn;
    net->observer_ctx = ctx;
}

void tsim_net_observe_finished(struct tsim_net *net, tsim_net_finished_fn fn, void *ctx) {
    net->finished = fn;
    net->finished_ctx = ctx;
}

/* --- A node, as its plugins see it --- */

uint32_t tsim_node_index(const struct tsim_node *node) { return node->index; }

uint32_t tsim_node_count(const struct tsim_node *node) { return tsim_net_nodes(node->net); }

tsim_time tsim_node_now(const struct tsim_node *node) { return tsim_sched_now(node->net->sched); }

static void timer_fire(struct tsim_sched *sched, void *ctx) {
    (void)sched;
    struct tsim_timer *t = ctx;
    t->event = (struct tsim_event){0};
    t->fn(t->ctx); /* last: it may destroy the timer */
}

struct tsim_timer *tsim_timer_create(struct tsim_node *node, void (*fn)(void *ctx), void *ctx) {
    struct tsim_timer *t = calloc(1, sizeof *t);
    if (!t) {
        return NULL;
    }
    *t = (struct tsim_timer){.node = node, .fn = fn, .ctx = ctx, .next = node->timers};
    if (node->timers) {
        node->timers->prev = t;
    }
    node->timers = t;
    return t;
}

void tsim_timer_destroy(struct tsim_timer *t) {
    if (!t) {
        return;
    }
    tsim_timer_stop(t);
    if (t->prev) {
        t->prev->next = t->next;
    } else {
        t->node->timers = t->next;
    }
    if (t->next) {
        t->next->prev = t->prev;
    }
    free(t);
}

bool tsim_timer_start(struct tsim_timer *t, tsim_time delay) {
    tsim_timer_stop(t);
    if (delay < 0) {
        return false;
    }
    t->event = tsim_sched_after(t->node->net->sched, delay, timer_fire, t);
    return t->event.slot != 0;
}

void tsim_timer_stop(struct tsim_timer *t) {
    tsim_sched_cancel(t->node->net->sched, t->event);
    t->event = (struct tsim_event){0};
}

bool tsim_timer_pending(const struct tsim_timer *t) {
    return tsim_sched_pending(t->node->net->sched, t->event);
}

void tsim_node_rng(const struct tsim_node *node, enum tsim_stream stream, struct tsim_rng *rng) {
    tsim_rng_init(rng, node->net->params.seed, ((uint64_t)stream << 32) | node->index);
}

/* The record of a message the node holds, or NULL. */
static struct record *held_by(const struct tsim_node *nd, uint64_t msg) {
    struct tsim_net *net = nd->net;
    if (msg == 0 || msg > net->message_count || !bit_get(net->messages[msg - 1].held, nd->index)) {
        return NULL;
    }
    return &net->messages[msg - 1];
}

/* A frame that names a message must come from a node holding it and contain its content. */
static bool carries_what_it_claims(const struct tsim_node *nd, const struct tsim_tx *tx) {
    if (tx->carries == 0) {
        return true;
    }
    const struct record *rec = held_by(nd, tx->carries);
    uint32_t len = rec ? rec->r.msg.len : 0;
    return rec && tx->carries_at <= tx->len && len <= tx->len - tx->carries_at &&
           memcmp(tx->bytes + tx->carries_at, rec->content, len) == 0;
}

uint64_t tsim_node_send(struct tsim_node *nd, const struct tsim_tx *tx) {
    struct tsim_net *net = nd->net;
    if ((int)tx->purpose < 0 || tx->purpose >= TSIM_PURPOSE_COUNT || tx->len > TSIM_FRAME_MAX ||
        tsim_lora_airtime(&tx->lora, tx->len) < 0 || !carries_what_it_claims(nd, tx)) {
        return 0;
    }
    if (net->params.queue_limit && nd->queue_len >= net->params.queue_limit) {
        nd->stats.dropped++;
        return 0;
    }
    if (nd->queue_len == nd->queue_cap) {
        size_t cap = nd->queue_cap ? nd->queue_cap * 2 : 8;
        struct queued *grown = realloc(nd->queue, cap * sizeof *grown);
        if (!grown) {
            return 0;
        }
        nd->queue = grown;
        nd->queue_cap = cap;
    }
    size_t at = nd->queue_len;
    while (at > 0 && nd->queue[at - 1].tx.priority < tx->priority) {
        at--;
    }
    uint64_t handle = net->next_handle++;
    memmove(&nd->queue[at + 1], &nd->queue[at], (nd->queue_len - at) * sizeof *nd->queue);
    nd->queue[at] = (struct queued){handle, *tx};
    nd->queue_len++;
    nd->stats.queued++;
    /* The MAC may send or cancel from inside the kick, which moves the queue under `at`. */
    net->mac->kick(nd->mac);
    return handle;
}

static void remove_at(struct tsim_node *nd, size_t at) {
    memmove(&nd->queue[at], &nd->queue[at + 1], (nd->queue_len - at - 1) * sizeof *nd->queue);
    nd->queue_len--;
}

bool tsim_node_cancel(struct tsim_node *nd, uint64_t handle) {
    for (size_t i = 0; i < nd->queue_len; i++) {
        if (nd->queue[i].handle == handle) {
            remove_at(nd, i);
            nd->stats.cancelled++;
            nd->net->mac->kick(nd->mac);
            return true;
        }
    }
    return false;
}

bool tsim_node_deliver(struct tsim_node *nd, uint64_t msg) {
    struct tsim_net *net = nd->net;
    uint32_t node = nd->index;
    struct record *rec = held_by(nd, msg);
    if (!rec) {
        return false;
    }
    const struct tsim_message *m = &rec->r.msg;
    if (m->dst == TSIM_BROADCAST) {
        if (node == m->src) {
            return false;
        }
        if (!rec->got) {
            rec->got = calloc((net->n + 7) / 8, 1);
            if (!rec->got) {
                return false;
            }
        }
        uint8_t bit = (uint8_t)(1u << (node % 8));
        if (rec->got[node / 8] & bit) {
            return false;
        }
        rec->got[node / 8] |= bit;
    } else if (node != m->dst || rec->r.delivered > 0) {
        return false;
    }
    tsim_time now = tsim_sched_now(net->sched);
    if (rec->r.delivered == 0) {
        rec->r.first = now;
    }
    rec->r.last = now;
    rec->r.delivered++;
    nd->stats.delivered++;
    if (net->observer) {
        net->observer(net->observer_ctx, &rec->r, node);
    }
    return true;
}

bool tsim_node_finished(struct tsim_node *nd, uint64_t msg) {
    struct tsim_net *net = nd->net;
    if (msg == 0 || msg > net->message_count || net->messages[msg - 1].r.msg.src != nd->index) {
        return false;
    }
    return finish(net, msg);
}

const struct tsim_tx *tsim_node_head(const struct tsim_node *nd) {
    return nd->queue_len ? &nd->queue[0].tx : NULL;
}

size_t tsim_node_queue_length(const struct tsim_node *nd) { return nd->queue_len; }

bool tsim_node_sending(const struct tsim_node *nd) { return nd->sending; }

bool tsim_node_transmit(struct tsim_node *nd) {
    if (nd->sending || nd->queue_len == 0) {
        return false;
    }
    const struct tsim_routing *routing = nd->net->routing;
    if (routing->sending && !routing->sending(nd->routing, nd->queue[0].handle)) {
        remove_at(nd, 0);
        nd->stats.cancelled++;
        nd->net->mac->kick(nd->mac);
        return false;
    }
    nd->air = nd->queue[0];
    const struct tsim_tx *tx = &nd->air.tx;
    if (!tsim_phy_transmit(nd->net->phy, nd->index, tx->channel, &tx->lora, tx->len, tx->tx_dbm,
                           &nd->air)) {
        return false;
    }
    remove_at(nd, 0);
    nd->sending = true;
    nd->ledger.frames[tx->purpose]++;
    tsim_time airtime = tsim_lora_airtime(&tx->lora, tx->len);
    nd->air_end = tsim_sched_now(nd->net->sched) + airtime;
    nd->ledger.airtime[tx->purpose] += airtime;
    return true;
}

bool tsim_node_cad(const struct tsim_node *nd) { return tsim_phy_cad(nd->net->phy, nd->index); }

bool tsim_node_receiving(const struct tsim_node *nd) {
    return tsim_phy_receiving(nd->net->phy, nd->index);
}

bool tsim_node_carrier(struct tsim_node *nd) { return tsim_phy_carrier(nd->net->phy, nd->index); }

void tsim_node_hold_header(struct tsim_node *nd, tsim_time hold) {
    tsim_phy_hold_header(nd->net->phy, nd->index, hold);
}

bool tsim_node_tune(struct tsim_node *nd, uint16_t channel, const struct tsim_lora *listen) {
    return tsim_phy_tune(nd->net->phy, nd->index, channel, listen);
}

uint64_t tsim_node_head_handle(const struct tsim_node *nd) {
    return nd->queue_len ? nd->queue[0].handle : 0;
}

tsim_time tsim_node_tx_airtime(const struct tsim_node *nd) {
    struct tsim_ledger now;
    tsim_net_ledger_now(nd->net, nd->index, &now);
    return tsim_ledger_airtime(&now);
}

tsim_time tsim_node_rx_airtime(const struct tsim_node *nd) {
    return tsim_phy_rx_airtime(nd->net->phy, nd->index);
}
