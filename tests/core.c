#include "tsim/core.h"

#include <string.h>

#include "tsim/meshcore.h"
#include "tsim/net.h"
#include "tsim/phy.h"

#include "check.h"

/* SF7 at 125 kHz, 14 dBm, and the default radio's noise: a link's SNR is 14 - loss + 117 dB. */
#define LOSS_LOUD 100.0

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct tsim_core_config rc;
    struct tsim_meshcore_mac_config mc;
    uint32_t nodes;
};

/* `relays` is one byte a node, 1 for a relay, or NULL for every node one. */
static void build(struct rig *r, uint32_t nodes, uint64_t seed, const uint8_t *relays) {
    struct tsim_lora l = tsim_lora_default(7, 125000);
    struct tsim_net_params p = tsim_net_defaults(seed);
    p.queue_limit = 0;
    r->rc = tsim_core_default(0, &l, 14.0);
    r->rc.relay_pick = relays ? 1 : 0;
    r->rc.relay_set = relays;
    r->mc = tsim_meshcore_mac_default();
    r->nodes = nodes;
    r->sched = tsim_sched_create();
    r->net = tsim_net_create(r->sched, &p, nodes, &tsim_core, &r->rc, &tsim_meshcore_mac, &r->mc);
}

static void link(struct rig *r, uint32_t a, uint32_t b) {
    tsim_phy_set_loss(tsim_net_phy(r->net), a, b, LOSS_LOUD);
}

static void line(struct rig *r, uint32_t nodes, uint64_t seed, const uint8_t *relays) {
    build(r, nodes, seed, relays);
    for (uint32_t i = 0; i + 1 < nodes; i++) {
        link(r, i, i + 1);
    }
    tsim_net_start(r->net);
}

/* side x side, each node hearing the four next to it. */
static void grid(struct rig *r, uint32_t side, uint64_t seed) {
    build(r, side * side, seed, NULL);
    for (uint32_t y = 0; y < side; y++) {
        for (uint32_t x = 0; x < side; x++) {
            uint32_t i = y * side + x;
            if (x + 1 < side) {
                link(r, i, i + 1);
            }
            if (y + 1 < side) {
                link(r, i, i + side);
            }
        }
    }
    tsim_net_start(r->net);
}

static void rig_close(struct rig *r) {
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static void run(struct rig *r, tsim_time until) { tsim_sched_run_until(r->sched, until); }

static bool route(struct rig *r, uint32_t from, uint32_t to, uint32_t *next) {
    void *self = tsim_net_routing(r->net, from);
    return self && tsim_core.next_hop(self, to, next);
}

/* How many ordered pairs of nodes that are on hold a route, and how many of those routes, followed
 * from node to node, reach their destination. `loops` counts those that come back on themselves
 * instead. */
static void routes(struct rig *r, uint32_t *held, uint32_t *reach, uint32_t *loops) {
    *held = *reach = *loops = 0;
    for (uint32_t s = 0; s < r->nodes; s++) {
        for (uint32_t d = 0; d < r->nodes; d++) {
            uint32_t at = s, steps = 0, next;
            if (s == d || !route(r, s, d, &next)) {
                continue;
            }
            (*held)++;
            while (at != d && steps++ <= r->nodes && route(r, at, d, &next)) {
                at = next;
            }
            *reach += at == d;
            *loops += at != d && steps > r->nodes;
        }
    }
}

static uint64_t frames(const struct rig *r, uint32_t node, enum tsim_purpose purpose) {
    return tsim_net_ledger(r->net, node)->frames[purpose];
}

static void a_line_learns_every_route(void) {
    struct rig r;
    line(&r, 6, 1, NULL);
    run(&r, TSIM_S(600));
    uint32_t held, reach, loops, next;
    routes(&r, &held, &reach, &loops);
    CHECK_EQ_U64(held, 6 * 5);
    CHECK_EQ_U64(reach, 6 * 5);
    CHECK(route(&r, 0, 5, &next) && next == 1);
    CHECK(route(&r, 5, 0, &next) && next == 4);
    rig_close(&r);
}

static void its_frames_are_announces_and_requests_and_nothing_else(void) {
    struct rig r;
    line(&r, 6, 2, NULL);
    run(&r, TSIM_S(600));
    for (uint32_t i = 0; i < r.nodes; i++) {
        CHECK(frames(&r, i, TSIM_PURPOSE_ANNOUNCE) > 0);
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_DATA), 0);
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 0);
    }
    rig_close(&r);
}

/* The core carries no messages yet, and the plugin must say so, not seem to. */
static void a_message_is_refused(void) {
    struct rig r;
    line(&r, 3, 3, NULL);
    run(&r, TSIM_S(600));
    uint64_t id = tsim_net_originate(r.net, 0, 2, 16);
    CHECK(id != 0);
    run(&r, TSIM_S(700));
    const struct tsim_message_record *m = tsim_net_message(r.net, id);
    CHECK(m && m->refused);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 0);
    rig_close(&r);
}

static void a_grid_never_loops_while_it_settles(void) {
    for (uint64_t seed = 1; seed <= 3; seed++) {
        struct rig r;
        grid(&r, 5, seed);
        uint32_t held = 0, reach = 0, loops = 0, ever = 0;
        for (tsim_time t = TSIM_S(5); t <= TSIM_S(1800); t += TSIM_S(5)) {
            run(&r, t);
            routes(&r, &held, &reach, &loops);
            ever += loops;
        }
        CHECK_EQ_U64(ever, 0);
        CHECK_EQ_U64(held, 25 * 24);
        CHECK_EQ_U64(reach, 25 * 24);
        rig_close(&r);
    }
}

/* A node powered down is made again when it comes back, having kept nothing: the core's starting
 * rule is what its neighbours and it then go by. */
static void a_node_that_restarts_is_routed_to_again(void) {
    struct rig r;
    grid(&r, 4, 4);
    run(&r, TSIM_S(1800));
    CHECK(tsim_net_power(r.net, 5, false));
    run(&r, TSIM_S(1860));
    CHECK(tsim_net_power(r.net, 5, true));
    run(&r, TSIM_S(1860 + 3600));
    uint32_t held, reach, loops;
    routes(&r, &held, &reach, &loops);
    CHECK_EQ_U64(loops, 0);
    CHECK_EQ_U64(held, 16 * 15);
    CHECK_EQ_U64(reach, 16 * 15);
    rig_close(&r);
}

/* With relays picked, a node that is not one is reached through one, and passes nothing on. */
static void only_relays_pass_routes_on(void) {
    /* 0 - 1 - 2 - 3, with 1 a leaf: nothing crosses it. */
    static const uint8_t set[] = {1, 0, 1, 1};
    struct rig r;
    line(&r, 4, 5, set);
    run(&r, TSIM_S(1800));
    uint32_t next;
    CHECK(tsim_core_relay(&r.rc, 0) && !tsim_core_relay(&r.rc, 1));
    CHECK(route(&r, 0, 1, &next) && next == 1);
    CHECK(route(&r, 2, 3, &next) && next == 3);
    CHECK(!route(&r, 0, 2, &next));
    CHECK(!route(&r, 3, 0, &next));
    rig_close(&r);
}

static void a_config_out_of_range_is_refused(void) {
    struct tsim_lora l = tsim_lora_default(7, 125000);
    struct tsim_core_config c = tsim_core_default(0, &l, 14.0);
    CHECK(tsim_core_check(&c) == NULL);
    c.neighbours = 0;
    CHECK(tsim_core_check(&c) != NULL);
    c.neighbours = 256;
    CHECK(tsim_core_check(&c) != NULL);
    c = tsim_core_default(0, &l, 14.0);
    c.tx_min_dbm = 15;
    CHECK(tsim_core_check(&c) != NULL);
}

int main(void) {
    RUN(a_line_learns_every_route);
    RUN(its_frames_are_announces_and_requests_and_nothing_else);
    RUN(a_message_is_refused);
    RUN(a_grid_never_loops_while_it_settles);
    RUN(a_node_that_restarts_is_routed_to_again);
    RUN(only_relays_pass_routes_on);
    RUN(a_config_out_of_range_is_refused);
    return CHECK_DONE();
}
