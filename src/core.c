#include "tsim/core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tern/route.h"
#include "tsim/rng.h"

/* Announces go before requests wait, as candidate 3 has them. */
#define PRIORITY_REQUEST 3
#define PRIORITY_ANNOUNCE 2

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
};

struct tsim_core_config tsim_core_default(uint16_t channel, const struct tsim_lora *lora,
                                          double tx_dbm) {
    return (struct tsim_core_config){
        .channel = channel,
        .lora = *lora,
        .tx_dbm = tx_dbm,
        .tx_min_dbm = -9,
        .neighbours = 255,
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

/* Offers the queue the frame in hand, asking the core for one if there is none, and sets the timer
 * for when the core next has something to do. */
static void service(struct router *r) {
    tsim_time now = tsim_node_now(r->node);
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
        if (r->handle == 0) {
            tsim_timer_start(r->timer, RETRY);
        }
        return;
    }
    /* Never at once: a core with nothing to send that says it is due now would spin. */
    tsim_time wait = tern_route_due(&r->route) - now;
    tsim_timer_start(r->timer, wait > TSIM_MS(1) ? wait : TSIM_MS(1));
}

static void router_fire(void *ctx) { service(ctx); }

static void router_rx(void *self, const struct tsim_rx *rx) {
    struct router *r = self;
    if (rx->channel != r->config.channel || !tern_route_frame(rx->bytes, rx->len)) {
        return;
    }
    double q = round(rx->snr_db * 4);
    int16_t snr_q = (int16_t)(q < -128 ? -128 : q > 127 ? 127 : q);
    tern_route_heard(&r->route, tsim_node_now(r->node), rx->bytes, rx->len, snr_q);
    service(r);
}

static void router_tx_done(void *self, uint64_t handle) {
    struct router *r = self;
    if (handle == 0 || handle != r->handle) {
        return;
    }
    tern_route_sent(&r->route, tsim_node_now(r->node));
    r->handle = 0;
    r->len = 0;
    service(r);
}

/* The core has no frames that follow its routes yet. */
static bool router_originate(void *self, const struct tsim_message *msg) {
    (void)self;
    (void)msg;
    return false;
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
    r->timer = tsim_timer_create(node, router_fire, r);
    if (!r->neighbours || !r->dests || !r->timer) {
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
    .next_hop = router_next_hop,
};
