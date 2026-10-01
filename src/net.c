#include "tsim/net.h"

#include <stdlib.h>
#include <string.h>

struct queued {
    uint64_t handle;
    struct tsim_tx tx;
};

struct node {
    void *routing;
    void *mac;
    struct queued *queue; /* by priority, then by handle */
    size_t queue_len;
    size_t queue_cap;
    /* The frame on the air. The radio carries a pointer to it as the frame's payload, so it stays
     * put until the radio reports the frame done, after every receiver has had it. */
    bool sending;
    struct queued air;
    struct tsim_ledger ledger;
    struct tsim_net_stats stats;
};

struct record {
    struct tsim_message_record r;
    uint8_t *got; /* a broadcast's destinations that have it: one bit per node, made on first use */
};

struct tsim_net {
    struct tsim_sched *sched;
    struct tsim_phy *phy;
    struct tsim_net_params params;
    const struct tsim_routing *routing;
    const struct tsim_mac *mac;
    uint32_t n;
    struct node *nodes;
    uint64_t next_handle;
    struct record *messages;
    size_t message_count;
    size_t message_cap;
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
    struct node *nd = &net->nodes[node];
    net->routing->rx(nd->routing, &rx);
    net->mac->kick(nd->mac);
}

static void on_tx_done(void *ctx, uint32_t node, const struct tsim_frame *frame) {
    (void)frame;
    struct tsim_net *net = ctx;
    struct node *nd = &net->nodes[node];
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
    net->phy = tsim_phy_create(sched, &params->phy, nodes, params->channel, &params->listen,
                               (struct tsim_phy_hooks){on_rx, on_tx_done, net});
    if (!net->nodes || !net->phy) {
        tsim_net_destroy(net);
        return NULL;
    }
    /* MACs first, so a routing plugin can send from start(). */
    for (uint32_t i = 0; i < nodes; i++) {
        net->nodes[i].mac = mac->create(net, i, mac_config);
        if (!net->nodes[i].mac) {
            tsim_net_destroy(net);
            return NULL;
        }
    }
    for (uint32_t i = 0; i < nodes; i++) {
        net->nodes[i].routing = routing->create(net, i, routing_config);
        if (!net->nodes[i].routing) {
            tsim_net_destroy(net);
            return NULL;
        }
    }
    if (routing->start) {
        for (uint32_t i = 0; i < nodes; i++) {
            routing->start(net->nodes[i].routing);
        }
    }
    return net;
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
    }
    tsim_phy_destroy(net->phy);
    for (size_t i = 0; i < net->message_count; i++) {
        free(net->messages[i].got);
    }
    free(net->messages);
    free(net->nodes);
    free(net);
}

struct tsim_phy *tsim_net_phy(struct tsim_net *net) { return net->phy; }

struct tsim_sched *tsim_net_sched(struct tsim_net *net) { return net->sched; }

uint32_t tsim_net_nodes(const struct tsim_net *net) { return net->n; }

void tsim_net_rng(const struct tsim_net *net, uint32_t node, enum tsim_stream stream,
                  struct tsim_rng *rng) {
    tsim_rng_init(rng, net->params.seed, ((uint64_t)stream << 32) | node);
}

uint64_t tsim_net_send(struct tsim_net *net, uint32_t node, const struct tsim_tx *tx) {
    if (node >= net->n || (int)tx->purpose < 0 || tx->purpose >= TSIM_PURPOSE_COUNT ||
        tx->len > TSIM_FRAME_MAX || tsim_lora_airtime(&tx->lora, tx->len) < 0) {
        return 0;
    }
    struct node *nd = &net->nodes[node];
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
    memmove(&nd->queue[at + 1], &nd->queue[at], (nd->queue_len - at) * sizeof *nd->queue);
    nd->queue[at] = (struct queued){net->next_handle++, *tx};
    nd->queue_len++;
    nd->stats.queued++;
    net->mac->kick(nd->mac);
    return nd->queue[at].handle;
}

static void remove_at(struct node *nd, size_t at) {
    memmove(&nd->queue[at], &nd->queue[at + 1], (nd->queue_len - at - 1) * sizeof *nd->queue);
    nd->queue_len--;
}

bool tsim_net_cancel(struct tsim_net *net, uint32_t node, uint64_t handle) {
    if (node >= net->n) {
        return false;
    }
    struct node *nd = &net->nodes[node];
    for (size_t i = 0; i < nd->queue_len; i++) {
        if (nd->queue[i].handle == handle) {
            remove_at(nd, i);
            nd->stats.cancelled++;
            return true;
        }
    }
    return false;
}

bool tsim_net_deliver(struct tsim_net *net, uint32_t node, uint64_t msg) {
    if (node >= net->n || msg == 0 || msg > net->message_count) {
        return false;
    }
    struct record *rec = &net->messages[msg - 1];
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
    net->nodes[node].stats.delivered++;
    return true;
}

const struct tsim_tx *tsim_net_head(const struct tsim_net *net, uint32_t node) {
    const struct node *nd = &net->nodes[node];
    return nd->queue_len ? &nd->queue[0].tx : NULL;
}

size_t tsim_net_queue_length(const struct tsim_net *net, uint32_t node) {
    return net->nodes[node].queue_len;
}

bool tsim_net_sending(const struct tsim_net *net, uint32_t node) {
    return net->nodes[node].sending;
}

bool tsim_net_transmit(struct tsim_net *net, uint32_t node) {
    if (node >= net->n) {
        return false;
    }
    struct node *nd = &net->nodes[node];
    if (nd->sending || nd->queue_len == 0) {
        return false;
    }
    nd->air = nd->queue[0];
    const struct tsim_tx *tx = &nd->air.tx;
    if (!tsim_phy_transmit(net->phy, node, tx->channel, &tx->lora, tx->len, tx->tx_dbm, &nd->air)) {
        return false;
    }
    remove_at(nd, 0);
    nd->sending = true;
    nd->ledger.frames[tx->purpose]++;
    nd->ledger.airtime[tx->purpose] += tsim_lora_airtime(&tx->lora, tx->len);
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
    struct tsim_message msg = {
        .id = net->message_count + 1,
        .src = src,
        .dst = dst,
        .len = len,
        .created = tsim_sched_now(net->sched),
    };
    net->messages[net->message_count++] = (struct record){
        .r = {.msg = msg, .wanted = dst == TSIM_BROADCAST ? net->n - 1 : 1},
    };
    net->routing->originate(net->nodes[src].routing, &msg);
    return msg.id;
}

const struct tsim_ledger *tsim_net_ledger(const struct tsim_net *net, uint32_t node) {
    return &net->nodes[node].ledger;
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
