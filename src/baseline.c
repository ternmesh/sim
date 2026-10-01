#include "tsim/baseline.h"

#include <stdlib.h>
#include <string.h>

#include "tsim/rng.h"

/* --- ALOHA --- */

struct aloha {
    struct tsim_node *node;
    struct tsim_aloha_config config;
    struct tsim_rng rng;
    struct tsim_timer *timer;
};

static void aloha_fire(void *ctx) {
    struct aloha *a = ctx;
    /* Still sending: the frame's end kicks again. */
    tsim_node_transmit(a->node);
}

static void aloha_kick(void *self) {
    struct aloha *a = self;
    /* Nothing left to send: the start drawn for a cancelled frame is not the next frame's. */
    if (!tsim_node_head(a->node)) {
        tsim_timer_stop(a->timer);
        return;
    }
    if (tsim_timer_pending(a->timer) || tsim_node_sending(a->node)) {
        return;
    }
    tsim_time delay = 0;
    if (a->config.max_delay > 0) {
        delay = (tsim_time)tsim_rng_below(&a->rng, (uint64_t)a->config.max_delay + 1);
    }
    tsim_timer_start(a->timer, delay);
}

static void *aloha_create(struct tsim_node *node, const void *config) {
    struct aloha *a = calloc(1, sizeof *a);
    if (!a) {
        return NULL;
    }
    a->node = node;
    a->config = *(const struct tsim_aloha_config *)config;
    tsim_node_rng(node, TSIM_STREAM_MAC, &a->rng);
    a->timer = tsim_timer_create(node, aloha_fire, a);
    if (!a->timer) {
        free(a);
        return NULL;
    }
    return a;
}

static void aloha_destroy(void *self) {
    struct aloha *a = self;
    tsim_timer_destroy(a->timer);
    free(a);
}

const struct tsim_mac tsim_aloha = {
    .name = "aloha",
    .create = aloha_create,
    .destroy = aloha_destroy,
    .kick = aloha_kick,
};

/* --- Naive flooding --- */

struct flood {
    struct tsim_node *node;
    uint32_t self; /* this node's address */
    struct tsim_flood_config config;
    uint8_t *seen; /* one bit per message id */
    size_t seen_bytes;
};

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Marks an id seen. Returns false if it already was, or if memory ran out (treated as seen, so a
 * failure can only stop a flood, never start one). */
static bool first_sight(struct flood *fl, uint32_t id) {
    size_t byte = id / 8;
    if (byte >= fl->seen_bytes) {
        size_t size = fl->seen_bytes ? fl->seen_bytes : 64;
        while (size <= byte) {
            size *= 2;
        }
        uint8_t *grown = realloc(fl->seen, size);
        if (!grown) {
            return false;
        }
        memset(grown + fl->seen_bytes, 0, size - fl->seen_bytes);
        fl->seen = grown;
        fl->seen_bytes = size;
    }
    uint8_t bit = (uint8_t)(1u << (id % 8));
    if (fl->seen[byte] & bit) {
        return false;
    }
    fl->seen[byte] |= bit;
    return true;
}

static struct tsim_tx frame_for(const struct flood *fl, enum tsim_purpose purpose) {
    return (struct tsim_tx){
        .channel = fl->config.channel,
        .lora = fl->config.lora,
        .tx_dbm = fl->config.tx_dbm,
        .purpose = purpose,
    };
}

static void flood_originate(void *self, const struct tsim_message *msg) {
    struct flood *fl = self;
    if (msg->len > TSIM_FRAME_MAX - TSIM_FLOOD_HEADER) {
        return;
    }
    uint32_t id = (uint32_t)msg->id;
    first_sight(fl, id);
    struct tsim_tx tx = frame_for(fl, TSIM_PURPOSE_DATA);
    put32(tx.bytes, id);
    put32(tx.bytes + 4, msg->src);
    put32(tx.bytes + 8, msg->dst);
    tx.bytes[12] = fl->config.hops;
    tx.len = TSIM_FLOOD_HEADER + msg->len;
    tsim_node_send(fl->node, &tx);
}

static void flood_rx(void *self, const struct tsim_rx *rx) {
    struct flood *fl = self;
    if (rx->len < TSIM_FLOOD_HEADER) {
        return;
    }
    uint32_t id = get32(rx->bytes);
    uint32_t dst = get32(rx->bytes + 8);
    uint8_t hops = rx->bytes[12];
    if (!first_sight(fl, id)) {
        return;
    }
    if (dst == fl->self || dst == TSIM_BROADCAST) {
        tsim_node_deliver(fl->node, id);
    }
    if (dst == fl->self || hops == 0) {
        return;
    }
    struct tsim_tx tx = frame_for(fl, TSIM_PURPOSE_RELAY);
    memcpy(tx.bytes, rx->bytes, rx->len);
    tx.bytes[12] = (uint8_t)(hops - 1);
    tx.len = rx->len;
    tsim_node_send(fl->node, &tx);
}

static void *flood_create(struct tsim_node *node, const void *config) {
    struct flood *fl = calloc(1, sizeof *fl);
    if (!fl) {
        return NULL;
    }
    fl->node = node;
    fl->self = tsim_node_index(node);
    fl->config = *(const struct tsim_flood_config *)config;
    return fl;
}

static void flood_destroy(void *self) {
    struct flood *fl = self;
    free(fl->seen);
    free(fl);
}

const struct tsim_routing tsim_flood = {
    .name = "flood",
    .create = flood_create,
    .destroy = flood_destroy,
    .originate = flood_originate,
    .rx = flood_rx,
};
