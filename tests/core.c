#include "tsim/core.h"

#include <string.h>

#include "tsim/baseline.h"
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

static void without_messages_its_frames_are_announces_and_requests(void) {
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

static uint64_t message(struct rig *r, uint32_t from, uint32_t to) {
    return tsim_net_originate(r->net, from, to, 16);
}

static void a_message_goes_hop_by_hop_and_is_acknowledged(void) {
    struct rig r;
    line(&r, 6, 3, NULL);
    run(&r, TSIM_S(600));
    uint64_t id = message(&r, 0, 5);
    CHECK(id != 0);
    run(&r, TSIM_S(660));
    const struct tsim_message_record *m = tsim_net_message(r.net, id);
    CHECK(m && !m->refused && m->delivered == 1 && m->finished);
    /* Once from its source, once from each node between, and the acknowledgement back. */
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1);
    for (uint32_t i = 1; i < 5; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
    }
    CHECK_EQ_U64(frames(&r, 5, TSIM_PURPOSE_RELAY), 0);
    rig_close(&r);
}

/* Message numbers start at 1 in every network: two running at once must not take each other's
 * senders for their own, or an acknowledgement goes to the wrong node. */
static void two_networks_at_once_keep_their_messages_apart(void) {
    struct rig a, b;
    line(&a, 6, 3, NULL);
    line(&b, 6, 4, NULL);
    run(&a, TSIM_S(600));
    run(&b, TSIM_S(600));
    uint64_t one = message(&a, 0, 5);
    uint64_t two = message(&b, 4, 1); /* the same number, from another node */
    CHECK(one == two);
    run(&a, TSIM_S(660));
    run(&b, TSIM_S(660));
    const struct tsim_message_record *m = tsim_net_message(a.net, one);
    CHECK(m && m->delivered == 1 && m->finished);
    m = tsim_net_message(b.net, two);
    CHECK(m && m->delivered == 1 && m->finished);
    /* Acknowledged at the first try, each of them: sent once from its source. */
    CHECK_EQ_U64(frames(&a, 0, TSIM_PURPOSE_DATA), 1);
    CHECK_EQ_U64(frames(&b, 4, TSIM_PURPOSE_DATA), 1);
    rig_close(&a);
    rig_close(&b);
}

/* A broadcast is flooded as the firmware floods a group's frame: along a line, where every relay
 * is a bridge, each node sends it once and each other node is delivered it once. */
static void a_broadcast_goes_the_length_of_a_line(void) {
    struct rig r;
    line(&r, 6, 3, NULL);
    run(&r, TSIM_S(600));
    uint64_t id = message(&r, 2, TSIM_BROADCAST);
    CHECK(id != 0);
    run(&r, TSIM_S(700));
    const struct tsim_message_record *m = tsim_net_message(r.net, id);
    CHECK(m && !m->refused && m->finished);
    CHECK_EQ_U64(m->delivered, 5);
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_DATA), 1);
    for (uint32_t i = 0; i < 6; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), i != 2);
    }
    rig_close(&r);
}

/* A node between goes down: the message is tried again, and given up, and the source is told. */
static void a_message_that_cannot_arrive_is_given_up(void) {
    struct rig r;
    line(&r, 4, 4, NULL);
    run(&r, TSIM_S(600));
    CHECK(tsim_net_power(r.net, 2, false));
    uint64_t id = message(&r, 0, 3);
    run(&r, TSIM_S(900));
    const struct tsim_message_record *m = tsim_net_message(r.net, id);
    CHECK(m && m->delivered == 0 && m->finished);
    CHECK(frames(&r, 0, TSIM_PURPOSE_DATA) >= 4);
    CHECK(frames(&r, 1, TSIM_PURPOSE_RELAY) >= 3);
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

/* Two nodes that send as soon as they have a frame, as a board with no listen-before-talk does,
 * each sending the other a message at the same instant. Their frames meet. With every wait fixed
 * they meet again at every try, and neither message arrives; with the firmware's wait before a
 * frame is sent again, both do. */
static uint32_t delivered_together(uint32_t retry_jitter, uint64_t seed) {
    struct tsim_lora l = tsim_lora_default(7, 125000);
    struct tsim_net_params p = tsim_net_defaults(seed);
    struct tsim_core_config rc = tsim_core_default(0, &l, 14.0);
    struct tsim_aloha_config ac = {.max_delay = 0};
    struct tsim_sched *sched = tsim_sched_create();
    p.queue_limit = 0;
    rc.retry_jitter = retry_jitter;
    struct tsim_net *net = tsim_net_create(sched, &p, 2, &tsim_core, &rc, &tsim_aloha, &ac);
    tsim_phy_set_loss(tsim_net_phy(net), 0, 1, LOSS_LOUD);
    tsim_net_start(net);
    tsim_sched_run_until(sched, TSIM_S(600));
    uint64_t a = tsim_net_originate(net, 0, 1, 16), b = tsim_net_originate(net, 1, 0, 16);
    tsim_sched_run_until(sched, TSIM_S(720));
    uint32_t n = tsim_net_message(net, a)->delivered + tsim_net_message(net, b)->delivered;
    tsim_net_destroy(net);
    tsim_sched_destroy(sched);
    return n;
}

static void frames_that_met_do_not_meet_at_every_try(void) {
    struct tsim_lora l = tsim_lora_default(7, 125000);
    CHECK_EQ_U64(tsim_core_default(0, &l, 14.0).retry_jitter, 4);
    for (uint64_t seed = 1; seed <= 20; seed++) {
        CHECK_EQ_U64(delivered_together(0, seed), 0);
        CHECK_EQ_U64(delivered_together(4, seed), 2);
    }
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
    /* The firmware holds the wait in a byte: 256 would be taken for none. */
    c = tsim_core_default(0, &l, 14.0);
    c.retry_jitter = 255;
    CHECK(tsim_core_check(&c) == NULL);
    c.retry_jitter = 256;
    CHECK(tsim_core_check(&c) != NULL);
    /* The rules for a full channel: a share is no more than all of the radio's time. */
    c = tsim_core_default(0, &l, 14.0);
    c.flood_last = 1;
    c.flood_busy_soft = 1;
    c.flood_busy_ppm = 1000000;
    CHECK(tsim_core_check(&c) == NULL);
    c.flood_busy_ppm = 1000001;
    CHECK(tsim_core_check(&c) != NULL);
    c.flood_busy_ppm = 0;
    c.flood_last = 2;
    CHECK(tsim_core_check(&c) != NULL);
    c.flood_last = 0;
    c.flood_busy_soft = 2;
    CHECK(tsim_core_check(&c) != NULL);
}

int main(void) {
    RUN(a_line_learns_every_route);
    RUN(without_messages_its_frames_are_announces_and_requests);
    RUN(a_message_goes_hop_by_hop_and_is_acknowledged);
    RUN(two_networks_at_once_keep_their_messages_apart);
    RUN(a_broadcast_goes_the_length_of_a_line);
    RUN(a_message_that_cannot_arrive_is_given_up);
    RUN(a_grid_never_loops_while_it_settles);
    RUN(a_node_that_restarts_is_routed_to_again);
    RUN(only_relays_pass_routes_on);
    RUN(frames_that_met_do_not_meet_at_every_try);
    RUN(a_config_out_of_range_is_refused);
    return CHECK_DONE();
}
