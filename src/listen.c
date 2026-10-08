#include "tsim/listen.h"

#include <stdlib.h>

#include "tsim/rng.h"

struct listen {
    struct tsim_node *node;
    struct tsim_listen_config config;
    struct tsim_rng rng;
    struct tsim_timer *timer;
    bool turning; /* it has looked, found the channel clear, and is turning round to send */
};

struct tsim_listen_config tsim_listen_default(const struct tsim_lora *lora) {
    tsim_time detect = 5 * tsim_lora_symbol(lora) + TSIM_MS(1);
    return (struct tsim_listen_config){
        .detect = detect,
        .turnaround = TSIM_MS(1),
        .slot = detect + TSIM_MS(1),
    };
}

static void look(struct listen *l) {
    if (!tsim_node_head(l->node) || tsim_node_sending(l->node)) {
        return;
    }
    tsim_time on = tsim_node_received_for(l->node);
    if (on >= 0 && on >= l->config.detect) {
        /* Not at the very instant the frame ends: the radio would still be on it. */
        tsim_time left = tsim_node_receiving_until(l->node) - tsim_node_now(l->node);
        uint64_t slots = tsim_rng_below(&l->rng, (uint64_t)l->config.window + 1);
        tsim_timer_start(l->timer, (left > 0 ? left : 1) + (tsim_time)slots * l->config.slot);
        return;
    }
    l->turning = true;
    tsim_timer_start(l->timer, l->config.turnaround);
}

static void listen_fire(void *ctx) {
    struct listen *l = ctx;
    if (l->turning) {
        l->turning = false;
        if (tsim_node_transmit(l->node) || tsim_node_sending(l->node)) {
            return; /* its end kicks again */
        }
    }
    look(l);
}

static void listen_kick(void *self) {
    struct listen *l = self;
    if (!tsim_node_head(l->node)) {
        tsim_timer_stop(l->timer);
        l->turning = false;
        return;
    }
    if (tsim_timer_pending(l->timer) || tsim_node_sending(l->node)) {
        return;
    }
    look(l);
}

static void *listen_create(struct tsim_node *node, const void *config) {
    const struct tsim_listen_config *c = config;
    if (c->detect < 0 || c->turnaround < 0 || c->slot < 0) {
        return NULL;
    }
    struct listen *l = calloc(1, sizeof *l);
    if (!l) {
        return NULL;
    }
    l->node = node;
    l->config = *c;
    tsim_node_rng(node, TSIM_STREAM_MAC, &l->rng);
    l->timer = tsim_timer_create(node, listen_fire, l);
    if (!l->timer) {
        free(l);
        return NULL;
    }
    return l;
}

static void listen_destroy(void *self) {
    struct listen *l = self;
    tsim_timer_destroy(l->timer);
    free(l);
}

const struct tsim_mac tsim_listen = {
    .name = "listen",
    .create = listen_create,
    .destroy = listen_destroy,
    .kick = listen_kick,
};
