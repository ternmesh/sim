#include "tsim/distvec.h"

#include <string.h>

#include "tsim/meshcore.h"
#include "tsim/net.h"
#include "tsim/rng.h"

#include "check.h"

/* SF7 at 125 kHz, 14 dBm, and the default radio's noise: a link's SNR is 14 - loss + 117 dB. */
#define LOSS_LOUD 100.0
#define LOSS_NONE 1000.0

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct tsim_distvec_config rc;
    struct tsim_meshcore_mac_config mc;
    uint32_t nodes;
};

static void rig_init(struct rig *r) {
    struct tsim_lora l = tsim_lora_default(7, 125000);
    r->rc = tsim_distvec_default(0, &l, 14.0);
    r->mc = tsim_meshcore_mac_default();
}

static void build(struct rig *r, uint32_t nodes, uint64_t seed) {
    struct tsim_net_params p = tsim_net_defaults(seed);
    p.queue_limit = 0;
    r->nodes = nodes;
    r->sched = tsim_sched_create();
    r->net =
        tsim_net_create(r->sched, &p, nodes, &tsim_distvec, &r->rc, &tsim_meshcore_mac, &r->mc);
}

static void link(struct rig *r, uint32_t a, uint32_t b, double loss) {
    tsim_phy_set_loss(tsim_net_phy(r->net), a, b, loss);
}

static void line(struct rig *r, uint32_t nodes, uint64_t seed) {
    build(r, nodes, seed);
    for (uint32_t i = 0; i + 1 < nodes; i++) {
        link(r, i, i + 1, LOSS_LOUD);
    }
    tsim_net_start(r->net);
}

/* side x side, each node hearing the four next to it. */
static void grid(struct rig *r, uint32_t side, uint64_t seed) {
    build(r, side * side, seed);
    for (uint32_t y = 0; y < side; y++) {
        for (uint32_t x = 0; x < side; x++) {
            uint32_t i = y * side + x;
            if (x + 1 < side) {
                link(r, i, i + 1, LOSS_LOUD);
            }
            if (y + 1 < side) {
                link(r, i, i + side, LOSS_LOUD);
            }
        }
    }
    tsim_net_start(r->net);
}

static void rig_close(struct rig *r) {
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static const void *at(struct rig *r, uint32_t node) { return tsim_net_routing(r->net, node); }

static bool route(struct rig *r, uint32_t from, uint32_t to, uint32_t *next) {
    uint16_t metric;
    return tsim_distvec_route(at(r, from), to, next, &metric);
}

static uint64_t frames(const struct rig *r, uint32_t node, enum tsim_purpose purpose) {
    return tsim_net_ledger(r->net, node)->frames[purpose];
}

/* Whether following next hops from any node towards any destination ever comes back to a node it
 * has passed: a routing loop. */
static bool loop_anywhere(struct rig *r) {
    for (uint32_t d = 0; d < r->nodes; d++) {
        for (uint32_t s = 0; s < r->nodes; s++) {
            uint32_t at_node = s, steps = 0, next;
            while (at_node != d && route(r, at_node, d, &next)) {
                at_node = next;
                if (++steps > r->nodes) {
                    return true;
                }
            }
        }
    }
    return false;
}

static void a_line_converges_on_its_one_path(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 6, 1);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    for (uint32_t a = 0; a < 6; a++) {
        for (uint32_t b = 0; b < 6; b++) {
            uint32_t next;
            if (a == b) {
                CHECK(!route(&r, a, b, &next));
                continue;
            }
            CHECK(route(&r, a, b, &next));
            CHECK_EQ_U64(next, b > a ? a + 1 : a - 1);
        }
    }
    uint16_t near, far;
    CHECK(tsim_distvec_route(at(&r, 0), 1, NULL, &near));
    CHECK(tsim_distvec_route(at(&r, 0), 5, NULL, &far));
    CHECK(far >= 5 * near - 5 && far <= 5 * near + 5); /* five hops cost five times one */
    rig_close(&r);
}

static void a_message_crosses_the_line_and_is_acknowledged(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 6, 1);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint64_t m = tsim_net_originate(r.net, 0, 5, 40);
    tsim_sched_run_until(r.sched, TSIM_S(360));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1);
    for (uint32_t i = 1; i < 5; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
    }
    CHECK_EQ_U64(frames(&r, 5, TSIM_PURPOSE_RELAY), 0);
    CHECK(frames(&r, 5, TSIM_PURPOSE_CONTROL) >= 1); /* the acknowledgement */
    rig_close(&r);
}

static void every_node_of_a_grid_reaches_every_other(void) {
    struct rig r;
    rig_init(&r);
    grid(&r, 5, 2);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    uint32_t missing = 0;
    for (uint32_t a = 0; a < 25; a++) {
        for (uint32_t b = 0; b < 25; b++) {
            uint32_t next;
            missing += a != b && !route(&r, a, b, &next);
        }
    }
    CHECK_EQ_U64(missing, 0);
    CHECK(!loop_anywhere(&r));
    uint64_t m = tsim_net_originate(r.net, 0, 24, 40);
    tsim_sched_run_until(r.sched, TSIM_S(660));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    rig_close(&r);
}

/* Node 2 hears node 0 but node 0 never hears node 2: neither may route over that link. */
static void a_one_way_link_is_never_used(void) {
    struct rig r;
    rig_init(&r);
    build(&r, 3, 1);
    link(&r, 0, 1, LOSS_LOUD);
    link(&r, 1, 2, LOSS_LOUD);
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 0, 2, LOSS_LOUD);
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint32_t next;
    CHECK(route(&r, 2, 0, &next));
    CHECK_EQ_U64(next, 1);
    CHECK(route(&r, 0, 2, &next));
    CHECK_EQ_U64(next, 1);
    CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, 0)), 1);
    CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, 2)), 1);
    rig_close(&r);
}

/* A hub with more neighbours than one announce has room to name: each is named in turn, so every
 * link comes to be used both ways. */
static void every_neighbour_hears_its_ihu_in_turn(void) {
    struct rig r;
    rig_init(&r);
    r.rc.ihu_max = 4;
    build(&r, 13, 6);
    for (uint32_t i = 1; i < 13; i++) {
        link(&r, 0, i, LOSS_LOUD);
    }
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, 0)), 12);
    for (uint32_t i = 1; i < 13; i++) {
        CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, i)), 1);
        uint32_t next;
        CHECK(route(&r, i, i % 12 + 1, &next));
        CHECK_EQ_U64(next, 0);
    }
    rig_close(&r);
}

/* The hub hears its three neighbours before they can hear it, so the first IHU it owes each is
 * lost; the turns it takes after that must still come round to every one of them. */
static void an_ihu_lost_is_sent_again_in_turn(void) {
    struct rig r;
    rig_init(&r);
    build(&r, 4, 7);
    for (uint32_t i = 1; i < 4; i++) {
        tsim_phy_set_loss_from(tsim_net_phy(r.net), i, 0, LOSS_LOUD);
    }
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(120));
    for (uint32_t i = 1; i < 4; i++) {
        CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, i)), 0);
        link(&r, 0, i, LOSS_LOUD);
    }
    tsim_sched_run_until(r.sched, TSIM_S(1200));
    for (uint32_t i = 1; i < 4; i++) {
        CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, i)), 1);
    }
    CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, 0)), 3);
    rig_close(&r);
}

/* A link that goes one-way after it was in use: node 1 stops hearing node 0, and says so as node
 * 0's promised announces fail to come. Node 0, still hearing node 1, has to stop using the link
 * from what node 1 reports, well before anyone's neighbour timeout. */
static void a_link_gone_one_way_is_dropped_by_the_side_still_hearing(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 3, 8);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    uint32_t next;
    CHECK(route(&r, 0, 2, &next));
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 0, 1, LOSS_NONE);
    tsim_time gone = -1;
    for (tsim_time t = TSIM_S(600); t < TSIM_S(600) + r.rc.neighbour_timeout; t += TSIM_S(30)) {
        tsim_sched_run_until(r.sched, t);
        if (tsim_distvec_neighbours(at(&r, 0)) == 0) {
            gone = t;
            break;
        }
    }
    CHECK(gone > 0);
    CHECK(!route(&r, 0, 2, &next));
    rig_close(&r);
}

/* A MAC for one test: it sends whatever is queued as soon as the radio is free, except at node
 * 1 while the test holds that node's gate shut, so a queue of one frame there stays full and the
 * next send is refused. */
static bool gate_shut;
static void *gate_create(struct tsim_node *node, const void *config) {
    (void)config;
    return node;
}
static void gate_destroy(void *self) { (void)self; }
static void gate_kick(void *self) {
    struct tsim_node *node = self;
    if (!(gate_shut && tsim_node_index(node) == 1) && !tsim_node_sending(node)) {
        tsim_node_transmit(node);
    }
}
static const struct tsim_mac gate_mac = {"gate", gate_create, gate_destroy, gate_kick};

/* Node 1 retracts its routes through node 2 while its queue is full. The retractions it could not
 * queue must still go once it can, or node 0 - which still hears node 1, and whose routes never
 * expire - would keep them for ever. Node 1 loses node 2 first and is gated a little later, so
 * node 0 has heard it recently when it times node 2 out, and does not time node 1 out itself
 * before the gate opens. */
static void a_retraction_the_queue_refused_goes_later(void) {
    struct rig r;
    rig_init(&r);
    /* Announces quick enough that a two-minute neighbour timeout is two promises long. */
    r.rc.imin = TSIM_S(2);
    r.rc.doublings = 3;
    r.rc.quiet_max = 0;
    r.rc.cap = 0.05;
    r.rc.neighbour_timeout = TSIM_S(120);
    struct tsim_net_params p = tsim_net_defaults(9);
    p.queue_limit = 1;
    r.nodes = 4;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 4, &tsim_distvec, &r.rc, &gate_mac, NULL);
    for (uint32_t i = 0; i + 1 < 4; i++) {
        link(&r, i, i + 1, LOSS_LOUD);
    }
    gate_shut = false;
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    uint32_t next;
    CHECK(route(&r, 0, 3, &next));

    link(&r, 1, 2, LOSS_NONE);
    tsim_sched_run_until(r.sched, TSIM_S(700));
    CHECK(route(&r, 1, 3, &next)); /* not timed out yet */
    gate_shut = true;
    tsim_net_originate(r.net, 1, 0, 20); /* fills node 1's queue, and stays there */
    tsim_sched_run_until(r.sched, TSIM_S(790));
    CHECK(!route(&r, 1, 3, &next)); /* timed out, and its retractions refused */
    CHECK(route(&r, 0, 3, &next));
    gate_shut = false;
    tsim_node_transmit(tsim_net_node(r.net, 1));
    tsim_sched_run_until(r.sched, TSIM_S(1400));
    CHECK(!route(&r, 0, 3, &next));
    CHECK(!route(&r, 0, 2, &next));
    CHECK(route(&r, 0, 1, &next));
    rig_close(&r);
}

/* Node 1 sends an announce as if from `sender`, which it is not: a sender is only bytes in the
 * header, so one radio can stand in for any number of neighbours. The frame names node 0 as heard,
 * says it names every node heard, promises another within ten minutes, and carries one route. */
static void announce_as(struct rig *r, uint32_t sender, uint16_t ann_seq, uint32_t dst,
                        uint16_t seq, uint16_t metric) {
    struct tsim_tx tx = {
        .lora = r->rc.lora,
        .tx_dbm = r->rc.tx_dbm,
        .purpose = TSIM_PURPOSE_ANNOUNCE,
    };
    uint8_t *b = tx.bytes;
    uint8_t head[] = {
        0x01, (uint8_t)sender, 0, 0, 0, (uint8_t)ann_seq, 0, 0, 0, 0x03, 0x58, 0x02, 1, 1};
    memcpy(b, head, sizeof head);
    uint8_t rest[] = {0,
                      0,
                      0,
                      0,
                      255,
                      (uint8_t)dst,
                      0,
                      0,
                      0,
                      (uint8_t)seq,
                      0,
                      (uint8_t)metric,
                      (uint8_t)(metric >> 8)};
    memcpy(b + sizeof head, rest, sizeof rest);
    tx.len = sizeof head + sizeof rest;
    CHECK(tsim_node_send(tsim_net_node(r->net, 1), &tx) != 0);
    tsim_sched_run_until(r->sched, tsim_sched_now(r->sched) + TSIM_S(5));
}

/* Node 0's four places for routes to node 7 hold routes that are cheap but infeasible, at an old
 * seq. A newer route, dearer than any of them, has to take one of those places: it is the only one
 * node 0 could select. */
static void a_newer_route_displaces_infeasible_cheaper_ones(void) {
    struct rig r;
    rig_init(&r);
    build(&r, 10, 10);
    link(&r, 0, 1, LOSS_LOUD); /* nodes 2 to 9 are elsewhere: node 1 speaks for them */
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(30));
    const uint32_t d = 7;
    announce_as(&r, 6, 0, d, 0, 10); /* the best, selected and announced */
    for (uint32_t v = 2; v <= 4; v++) {
        announce_as(&r, v, 0, d, 0, 100);
    }
    tsim_sched_run_until(r.sched, TSIM_S(120)); /* node 0 announces it: feasibility distance ~82 */
    uint32_t next;
    CHECK(route(&r, 0, d, &next));
    CHECK_EQ_U64(next, 6);
    announce_as(&r, 6, 1, d, 0, TSIM_DISTVEC_METRIC_INF); /* retracted */
    announce_as(&r, 5, 0, d, 0, 100);                     /* the fourth infeasible one */
    CHECK(!route(&r, 0, d, &next));                       /* starved */
    announce_as(&r, 8, 0, d, 1, 500);                     /* newer, dearer, feasible */
    CHECK(route(&r, 0, d, &next));
    CHECK_EQ_U64(next, 8);
    rig_close(&r);
}

static void a_leaf_never_forwards(void) {
    struct rig r;
    rig_init(&r);
    strcpy(r.rc.relays, "1,3");
    line(&r, 5, 1); /* leaf, infra, leaf, infra, leaf */
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint32_t next;
    CHECK(route(&r, 0, 1, &next));
    CHECK(!route(&r, 0, 3, &next)); /* only through leaf 2 */
    CHECK(!route(&r, 1, 3, &next));
    uint64_t m = tsim_net_originate(r.net, 0, 2, 20);
    uint64_t blocked = tsim_net_originate(r.net, 0, 4, 20);
    tsim_sched_run_until(r.sched, TSIM_S(400));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1); /* leaf to leaf, through infra 1 */
    CHECK_EQ_U64(tsim_net_message(r.net, blocked)->delivered, 0);
    CHECK(tsim_net_message(r.net, blocked)->finished);
    for (uint32_t i = 0; i < 5; i += 2) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 0);
    }
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 1);
    rig_close(&r);
}

static void trickle_backs_off_and_resets_on_a_new_neighbour(void) {
    struct rig r;
    rig_init(&r);
    build(&r, 5, 3);
    for (uint32_t a = 0; a < 4; a++) {
        for (uint32_t b = a + 1; b < 4; b++) {
            link(&r, a, b, LOSS_LOUD);
        }
    }
    tsim_net_start(r.net);
    tsim_time imax = r.rc.imin << r.rc.doublings;
    tsim_sched_run_until(r.sched, TSIM_S(3600));
    CHECK_EQ_I64(tsim_distvec_interval(at(&r, 0)), imax);
    uint64_t before = frames(&r, 0, TSIM_PURPOSE_ANNOUNCE);
    tsim_sched_run_until(r.sched, TSIM_S(7200));
    /* An hour at imax, suppressed by its three neighbours but for every third interval at least. */
    int64_t hour = (int64_t)(frames(&r, 0, TSIM_PURPOSE_ANNOUNCE) - before);
    CHECK(hour >= TSIM_S(3600) / imax / (r.rc.quiet_max + 1) - 1);
    CHECK(hour <= TSIM_S(3600) / imax + 1);

    link(&r, 0, 4, LOSS_LOUD); /* node 4 appears */
    tsim_time least = imax;
    for (tsim_time t = TSIM_S(7200); t < TSIM_S(7200) + 2 * imax; t += TSIM_S(1)) {
        tsim_sched_run_until(r.sched, t);
        tsim_time i = tsim_distvec_interval(at(&r, 0));
        least = i < least ? i : least;
    }
    CHECK_EQ_I64(least, r.rc.imin);
    uint32_t next;
    CHECK(route(&r, 1, 4, &next));
    CHECK_EQ_U64(next, 0);
    rig_close(&r);
}

static void announces_stay_under_the_cap(void) {
    struct rig r;
    rig_init(&r);
    r.rc.cap = 0.005;
    r.rc.imin = TSIM_S(1);
    r.rc.redundancy = 0; /* never suppressed: only the cap holds them back */
    r.rc.doublings = 0;
    build(&r, 12, 4);
    for (uint32_t a = 0; a < 12; a++) {
        for (uint32_t b = a + 1; b < 12; b++) {
            link(&r, a, b, LOSS_LOUD);
        }
    }
    tsim_net_start(r.net);
    tsim_time run = TSIM_S(3600);
    tsim_sched_run_until(r.sched, run);
    struct tsim_lora l = r.rc.lora;
    double share = r.rc.cap * (1 - r.rc.request_share); /* the announces' */
    double bucket = (double)r.rc.cap_window * share;
    double frame = (double)tsim_lora_airtime(&l, 255);
    bucket = bucket < frame ? frame : bucket;
    for (uint32_t i = 0; i < 12; i++) {
        double spent = (double)tsim_net_ledger(r.net, i)->airtime[TSIM_PURPOSE_ANNOUNCE];
        CHECK(spent <= (double)run * share + bucket);
        CHECK(spent >= (double)run * share * 0.9); /* and the cap is what limits them */
    }
    rig_close(&r);
}

/* A link breaks under traffic. The frame the hop before it cannot get across counts against the
 * link, and the dearer route it then announces is infeasible upstream, so the nodes behind it
 * stop sending into the break at once. The hop itself, with nothing more crossing, keeps its
 * route until the neighbour times out. */
static void a_broken_link_is_found_by_the_data_crossing_it(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 4, 1);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint32_t next;
    CHECK(route(&r, 0, 3, &next));
    link(&r, 2, 3, LOSS_NONE);
    uint64_t m = tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(420));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 0);
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK(!route(&r, 0, 3, &next));
    CHECK(!route(&r, 1, 3, &next));
    CHECK(route(&r, 0, 2, &next)); /* the rest of the line is untouched */
    tsim_sched_run_until(r.sched,
                         TSIM_S(420) + r.rc.neighbour_timeout + r.rc.neighbour_timeout / 4);
    CHECK(!route(&r, 2, 3, &next));
    rig_close(&r);
}

/* Links on a grid break and mend while messages cross it; at no moment does any route lead back
 * to where it has been. */
static void routes_stay_loop_free_while_links_change(void) {
    struct rig r;
    rig_init(&r);
    r.rc.neighbour_timeout = TSIM_S(120);
    grid(&r, 4, 5);
    struct tsim_rng rng;
    tsim_rng_init(&rng, 5, 99);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint32_t loops = 0;
    for (int step = 0; step < 240; step++) {
        tsim_time t = TSIM_S(300) + step * TSIM_S(10);
        if (step % 6 == 0) {
            uint32_t a = (uint32_t)tsim_rng_below(&rng, 16);
            uint32_t b = (a % 4 < 3 && tsim_rng_below(&rng, 2)) ? a + 1 : a + 4;
            if (b < 16) {
                link(&r, a, b, tsim_rng_below(&rng, 2) ? LOSS_NONE : LOSS_LOUD);
            }
        }
        uint32_t src = (uint32_t)tsim_rng_below(&rng, 16);
        uint32_t dst = (src + 1 + (uint32_t)tsim_rng_below(&rng, 15)) % 16;
        tsim_net_originate(r.net, src, dst, 20);
        tsim_sched_run_until(r.sched, t);
        loops += loop_anywhere(&r);
    }
    CHECK_EQ_U64(loops, 0);
    rig_close(&r);
}

static void a_broadcast_reaches_the_line_once_per_relay(void) {
    struct rig r;
    rig_init(&r);
    r.rc.bcast_hops = 10;
    line(&r, 6, 1);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(120));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 5);
    for (uint32_t i = 1; i < 5; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
    }
    CHECK_EQ_U64(frames(&r, 5, TSIM_PURPOSE_RELAY), 1); /* it cannot know no one is beyond */
    rig_close(&r);
}

static void the_config_is_checked(void) {
    struct tsim_lora l = tsim_lora_default(7, 125000);
    struct tsim_distvec_config c = tsim_distvec_default(0, &l, 14.0);
    CHECK(tsim_distvec_check(&c) == NULL);
    struct tsim_distvec_config bad = c;
    bad.cap = 0;
    CHECK(tsim_distvec_check(&bad) != NULL);
    bad = c;
    strcpy(bad.relays, "3-1");
    CHECK(tsim_distvec_check(&bad) != NULL);
    bad = c;
    bad.burst = 0;
    CHECK(tsim_distvec_check(&bad) != NULL);
}

int main(void) {
    RUN(a_line_converges_on_its_one_path);
    RUN(a_message_crosses_the_line_and_is_acknowledged);
    RUN(every_node_of_a_grid_reaches_every_other);
    RUN(a_one_way_link_is_never_used);
    RUN(every_neighbour_hears_its_ihu_in_turn);
    RUN(an_ihu_lost_is_sent_again_in_turn);
    RUN(a_link_gone_one_way_is_dropped_by_the_side_still_hearing);
    RUN(a_retraction_the_queue_refused_goes_later);
    RUN(a_newer_route_displaces_infeasible_cheaper_ones);
    RUN(a_leaf_never_forwards);
    RUN(trickle_backs_off_and_resets_on_a_new_neighbour);
    RUN(announces_stay_under_the_cap);
    RUN(a_broken_link_is_found_by_the_data_crossing_it);
    RUN(routes_stay_loop_free_while_links_change);
    RUN(a_broadcast_reaches_the_line_once_per_relay);
    RUN(the_config_is_checked);
    return CHECK_DONE();
}
