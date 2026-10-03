#include "tsim/distvec.h"

#include <math.h>
#include <string.h>

#include "tsim/meshcore.h"
#include "tsim/metrics.h"
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
    r->rc.power = false; /* frames and links as these tests know them; power_rig turns it on */
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
    /* Every hop costs more. The first costs at least a perfect link - the reference frame's
     * airtime - and none more than etx_max of them; a further hop may add less than a link's cost,
     * since a node announces its last metric again while the true one stays within `change`. */
    struct tsim_lora l = r.rc.lora;
    double hop = (double)tsim_lora_airtime(&l, r.rc.ref_len) / (double)TSIM_MS(1);
    uint16_t last = 0;
    for (uint32_t d = 1; d < 6; d++) {
        uint16_t metric;
        CHECK(tsim_distvec_route(at(&r, 0), d, NULL, &metric));
        CHECK(metric > last);
        CHECK(metric <= d * hop * r.rc.etx_max + d);
        last = metric;
        if (d == 1) {
            CHECK(metric >= hop);
        }
    }
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
 * 0's promised announces fail to come, and at the latest once it forgets node 0 - after
 * neighbour_timeout and two of node 0's promises - by leaving it out of an announce that names
 * every node it hears. Node 0, still hearing node 1, has to stop using the link then, and not
 * hold on to node 1's last IHU for ever. */
static void a_link_gone_one_way_is_dropped_by_the_side_still_hearing(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 3, 8);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    uint32_t next;
    CHECK(route(&r, 0, 2, &next));
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 0, 1, LOSS_NONE);
    tsim_time gone = -1;
    for (tsim_time t = TSIM_S(600); t < TSIM_S(600) + 2 * r.rc.neighbour_timeout; t += TSIM_S(30)) {
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

/* A MAC for tests: it sends whatever is queued as soon as the radio is free, except at gate_node
 * while the test holds its gate shut - so a queue of one frame there stays full and the next send
 * is refused, or a frame there waits as long as the test likes. */
static bool gate_shut;
static uint32_t gate_node = 1;
static void *gate_create(struct tsim_node *node, const void *config) {
    (void)config;
    return node;
}
static void gate_destroy(void *self) { (void)self; }
static void gate_kick(void *self) {
    struct tsim_node *node = self;
    if (!(gate_shut && tsim_node_index(node) == gate_node) && !tsim_node_sending(node)) {
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

/* A line of three, announcing quickly, whose far link breaks. Node 1 times node 2 out and retracts
 * its route; node 0 hears none of node 1's next `lost` announces, the first of them the retraction.
 */
static void retract_unheard(struct rig *r, uint64_t seed, uint64_t lost) {
    rig_init(r);
    r->rc.imin = TSIM_S(2);
    r->rc.doublings = 3;
    r->rc.quiet_max = 0;
    r->rc.cap = 0.05;
    r->rc.neighbour_timeout = TSIM_S(120);
    line(r, 3, seed);
    tsim_sched_run_until(r->sched, TSIM_S(300));
    uint32_t next;
    CHECK(route(r, 0, 2, &next));
    link(r, 1, 2, LOSS_NONE);
    while (route(r, 1, 2, &next) && tsim_sched_now(r->sched) < TSIM_S(900)) {
        tsim_sched_run_until(r->sched, tsim_sched_now(r->sched) + TSIM_MS(20));
    }
    CHECK(!route(r, 1, 2, &next));
    struct tsim_phy *phy = tsim_net_phy(r->net);
    tsim_phy_set_loss_from(phy, 1, 0, LOSS_NONE);
    uint64_t until = frames(r, 1, TSIM_PURPOSE_ANNOUNCE) + lost;
    struct tsim_node *one = tsim_net_node(r->net, 1);
    while (frames(r, 1, TSIM_PURPOSE_ANNOUNCE) < until || tsim_node_sending(one)) {
        tsim_sched_run_until(r->sched, tsim_sched_now(r->sched) + TSIM_MS(20));
    }
    tsim_phy_set_loss_from(phy, 1, 0, LOSS_LOUD);
}

/* The retraction is lost on the air. Routes do not expire, so node 0 would keep its route through
 * node 1 for ever: the retraction has to be said again. */
static void a_retraction_lost_on_the_air_is_repeated(void) {
    struct rig r;
    retract_unheard(&r, 23, 1);
    tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_S(300));
    uint32_t next;
    CHECK(!route(&r, 0, 2, &next));
    CHECK(route(&r, 0, 1, &next));
    rig_close(&r);
}

/* Every time node 1 retracts, node 0 misses it. Node 0 still has the route; the first message it
 * sends along it finds the dead end, and node 1 retracts again. */
/* Node 0 misses node 1's first two announces after it retracts. A retraction goes in three: the
 * third still gets there - which it would not, were two of the three in one frame. */
static void a_retraction_goes_in_three_frames(void) {
    struct rig r;
    retract_unheard(&r, 23, 2);
    tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_S(300));
    uint32_t next;
    CHECK(!route(&r, 0, 2, &next));
    CHECK(route(&r, 0, 1, &next));
    rig_close(&r);
}

static void a_message_into_a_dead_end_brings_the_retraction_again(void) {
    struct rig r;
    retract_unheard(&r, 24, 3);
    tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_S(300));
    uint32_t next;
    CHECK(route(&r, 0, 2, &next)); /* stale */
    CHECK(route(&r, 0, 1, &next));
    tsim_net_originate(r.net, 0, 2, 20);
    tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_S(60));
    CHECK(!route(&r, 0, 2, &next));
    CHECK(route(&r, 0, 1, &next));
    rig_close(&r);
}

/* A frame no node makes anything of, to take up a place in a queue. */
static void occupy(struct rig *r, uint32_t node) {
    struct tsim_tx tx = {
        .lora = r->rc.lora,
        .tx_dbm = r->rc.tx_dbm,
        .purpose = TSIM_PURPOSE_CONTROL,
        .len = 1,
    };
    tx.bytes[0] = 0x7F;
    CHECK(tsim_node_send(tsim_net_node(r->net, node), &tx) != 0);
}

/* A hub names its twelve neighbours two to a frame, a round of six frames, and its queue refuses
 * every other announce. The refused frames' IHUs must go in the next: were the round to move on
 * without them, the same three slices would be refused every time, and their neighbours never
 * named. */
static void ihus_the_queue_refused_go_in_the_next_frame(void) {
    struct rig r;
    rig_init(&r);
    r.rc.ihu_max = 2;
    r.rc.imin = TSIM_S(10);
    r.rc.doublings = 0;
    r.rc.redundancy = 0;
    r.rc.cap = 0.05;
    r.rc.neighbour_timeout = TSIM_S(600);
    struct tsim_net_params p = tsim_net_defaults(25);
    p.queue_limit = 1;
    r.nodes = 13;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 13, &tsim_distvec, &r.rc, &gate_mac, NULL);
    for (uint32_t i = 1; i < 13; i++) {
        link(&r, 0, i, LOSS_LOUD);
    }
    gate_node = 0;
    gate_shut = false;
    tsim_net_start(r.net);
    struct tsim_node *hub = tsim_net_node(r.net, 0);
    uint64_t sent = 0, dropped = 0;
    uint32_t lost = 0; /* of 12 960 looks at a spoke */
    for (tsim_time t = 0; t < TSIM_S(4 * 3600); t += TSIM_MS(50)) {
        tsim_sched_run_until(r.sched, t);
        if (!gate_shut && frames(&r, 0, TSIM_PURPOSE_ANNOUNCE) > sent && !tsim_node_sending(hub)) {
            sent = frames(&r, 0, TSIM_PURPOSE_ANNOUNCE);
            gate_shut = true; /* the next announce finds the queue full */
            occupy(&r, 0);
            dropped = tsim_net_stats(r.net, 0)->dropped;
        } else if (gate_shut && tsim_net_stats(r.net, 0)->dropped > dropped) {
            gate_shut = false;
            tsim_node_transmit(hub);
        }
        if (t >= TSIM_S(3600) && t % TSIM_S(10) == 0) {
            for (uint32_t i = 1; i < 13; i++) {
                lost += tsim_distvec_neighbours(at(&r, i)) != 1;
            }
        }
    }
    CHECK(tsim_net_stats(r.net, 0)->dropped > 100);
    /* Allowing for an IHU now and then lost to a collision, this MAC having no carrier sense:
     * without the refusals, about 20. */
    CHECK(lost < 100);
    gate_node = 1;
    gate_shut = false;
    rig_close(&r);
}

/* Node 1's MAC lets a frame out only once in five minutes, far longer than its Trickle and cap
 * promise.
 * Its announces wait in the queue, and the promise has to say so: node 0 must not count the wait
 * as announces missed, and tell node 1 in its IHUs that it hears it less. */
static void a_node_whose_mac_holds_its_announces_promises_the_wait(void) {
    struct rig r;
    rig_init(&r);
    r.rc.imin = TSIM_S(2);
    r.rc.doublings = 0;
    r.rc.redundancy = 0;
    r.rc.cap = 0.05;
    struct tsim_net_params p = tsim_net_defaults(26);
    p.queue_limit = 1;
    r.nodes = 2;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 2, &tsim_distvec, &r.rc, &gate_mac, NULL);
    link(&r, 0, 1, LOSS_LOUD);
    gate_shut = false;
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    uint16_t before, metric;
    CHECK(tsim_distvec_route(at(&r, 1), 0, NULL, &before));
    gate_shut = true;
    uint32_t worse = 0;
    for (tsim_time t = TSIM_S(60); t < TSIM_S(3660); t += TSIM_S(1)) {
        tsim_sched_run_until(r.sched, t);
        if (t % TSIM_S(300) == 0) {
            tsim_node_transmit(tsim_net_node(r.net, 1)); /* the one frame in five minutes */
        }
        if (t >= TSIM_S(1200)) { /* once node 1 has seen how long its announces wait */
            worse += !tsim_distvec_route(at(&r, 1), 0, NULL, &metric) || metric > 2 * before;
        }
    }
    CHECK_EQ_U64(worse, 0);
    gate_shut = false;
    rig_close(&r);
}

/* Node 1's MAC holds its announces back 40 hours, longer than two of the most two bytes of
 * seconds can promise: were the promise cut short there, node 0 would forget it between frames.
 * The promise goes in minutes instead. */
static void a_node_held_back_for_days_is_kept(void) {
    struct rig r;
    rig_init(&r);
    r.rc.imin = TSIM_S(60);
    r.rc.doublings = 0;
    r.rc.redundancy = 0;
    r.rc.cap = 0.05;
    struct tsim_net_params p = tsim_net_defaults(27);
    p.queue_limit = 1;
    r.nodes = 2;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 2, &tsim_distvec, &r.rc, &gate_mac, NULL);
    link(&r, 0, 1, LOSS_LOUD);
    gate_shut = false;
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(tsim_distvec_neighbours(at(&r, 0)), 1);
    gate_shut = true;
    const tsim_time hold = TSIM_S(40 * 3600);
    uint32_t lost = 0;
    for (tsim_time t = TSIM_S(600); t < TSIM_S(600) + 6 * hold; t += TSIM_S(600)) {
        tsim_sched_run_until(r.sched, t);
        if ((t - TSIM_S(600)) % hold == 0 && t > TSIM_S(600)) {
            tsim_node_transmit(tsim_net_node(r.net, 1));
        }
        if (t >= TSIM_S(600) + 2 * hold + TSIM_S(600)) { /* once it has seen how long it waits */
            lost += tsim_distvec_neighbours(at(&r, 0)) != 1;
        }
    }
    CHECK(tsim_distvec_promise(at(&r, 1)) >= hold);
    CHECK_EQ_U64(lost, 0);
    gate_shut = false;
    rig_close(&r);
}

/* Node 1 sends an announce as if from `sender`, which it is not: a sender is only bytes in the
 * header, so one radio can stand in for any number of neighbours. The frame is infrastructure's,
 * promises another within ten minutes, names node 0 as heard in a round of one frame, and carries
 * one route. */
static void announce_as_then(struct rig *r, uint32_t sender, uint16_t ann_seq, uint32_t dst,
                             uint16_t seq, uint16_t metric, tsim_time wait) {
    struct tsim_tx tx = {
        .lora = r->rc.lora,
        .tx_dbm = r->rc.tx_dbm,
        .purpose = TSIM_PURPOSE_ANNOUNCE,
    };
    uint8_t *b = tx.bytes;
    uint8_t head[] = {0x01,
                      (uint8_t)sender,
                      (uint8_t)(sender >> 8),
                      0,
                      0,
                      (uint8_t)ann_seq,
                      0,
                      0,
                      0,
                      0x01,
                      0x58,
                      0x02,
                      1,
                      0,
                      1,
                      1};
    memcpy(b, head, sizeof head);
    uint8_t rest[] = {0,
                      0,
                      0,
                      0,
                      255,
                      (uint8_t)dst,
                      (uint8_t)(dst >> 8),
                      0,
                      0,
                      (uint8_t)seq,
                      0,
                      (uint8_t)metric,
                      (uint8_t)(metric >> 8)};
    memcpy(b + sizeof head, rest, sizeof rest);
    tx.len = sizeof head + sizeof rest;
    CHECK(tsim_node_send(tsim_net_node(r->net, 1), &tx) != 0);
    tsim_sched_run_until(r->sched, tsim_sched_now(r->sched) + wait);
}

static void announce_as(struct rig *r, uint32_t sender, uint16_t ann_seq, uint32_t dst,
                        uint16_t seq, uint16_t metric) {
    announce_as_then(r, sender, ann_seq, dst, seq, metric, TSIM_S(5));
}

/* Node 5, heard only through node 1's radio, announces with a round of `rotation` frames, naming
 * node 0 in its IHUs or not. */
static void announce_round(struct rig *r, uint16_t ann_seq, uint16_t rotation, bool name) {
    struct tsim_tx tx = {
        .lora = r->rc.lora,
        .tx_dbm = r->rc.tx_dbm,
        .purpose = TSIM_PURPOSE_ANNOUNCE,
    };
    uint8_t head[] = {0x01, 5, 0,    0,    0,    (uint8_t)ann_seq,  (uint8_t)(ann_seq >> 8),
                      0,    0, 0x01, 0x58, 0x02, (uint8_t)rotation, (uint8_t)(rotation >> 8),
                      name, 0};
    memcpy(tx.bytes, head, sizeof head);
    uint8_t ihu[] = {0, 0, 0, 0, 255};
    memcpy(tx.bytes + sizeof head, ihu, sizeof ihu);
    tx.len = (uint32_t)sizeof head + (name ? (uint32_t)sizeof ihu : 0);
    CHECK(tsim_node_send(tsim_net_node(r->net, 1), &tx) != 0);
    tsim_sched_run_until(r->sched, tsim_sched_now(r->sched) + TSIM_S(5));
}

/* Node 0 keeps its link to node 5 through ihu_rounds rounds of `rotation` frames and one more frame
 * without an IHU, and takes it away at the next: with a round of 4, 9 frames for 2 rounds and 33
 * for 8; with a round of 1, 3 and 9. */
static void links_last_ihu_rounds_of_unnamed_announces(uint8_t rounds, uint16_t rotation) {
    struct rig r;
    rig_init(&r);
    r.rc.ihu_rounds = rounds;
    build(&r, 6, 1);
    link(&r, 0, 1, LOSS_LOUD);
    tsim_net_start(r.net);
    announce_round(&r, 1, rotation, true);
    uint32_t next;
    CHECK(route(&r, 0, 5, &next) && next == 5);
    uint16_t allowed = (uint16_t)(rounds * rotation + 1);
    for (uint16_t k = 1; k < allowed; k++) {
        announce_round(&r, (uint16_t)(1 + k), rotation, false);
    }
    CHECK(route(&r, 0, 5, &next));
    announce_round(&r, (uint16_t)(1 + allowed), rotation, false);
    CHECK(!route(&r, 0, 5, &next));
    rig_close(&r);
}

/* A round so long that ihu_rounds of it pass the count's limit: the link still goes once 32767
 * announces have passed without an IHU, at whichever announce is heard first after that - here not
 * the one at the limit, which was lost. */
static void a_link_past_the_longest_wait_goes_whichever_announce_comes(void) {
    struct rig r;
    rig_init(&r);
    build(&r, 6, 1);
    link(&r, 0, 1, LOSS_LOUD);
    tsim_net_start(r.net);
    announce_round(&r, 1, 16384, true);
    uint32_t next;
    CHECK(route(&r, 0, 5, &next));
    announce_round(&r, 1 + 0x4000, 16384, false);
    CHECK(route(&r, 0, 5, &next));
    announce_round(&r, 1 + 0x8100, 16384, false);
    CHECK(!route(&r, 0, 5, &next));
    rig_close(&r);
}

static void a_link_lasts_its_ihu_rounds(void) {
    links_last_ihu_rounds_of_unnamed_announces(2, 4);
    links_last_ihu_rounds_of_unnamed_announces(8, 4);
    links_last_ihu_rounds_of_unnamed_announces(2, 1);
    links_last_ihu_rounds_of_unnamed_announces(8, 1);
    struct rig r;
    rig_init(&r);
    CHECK_EQ_U64(r.rc.ihu_rounds, 8);
    r.rc.ihu_rounds = 1;
    CHECK(tsim_distvec_check(&r.rc) != NULL);
    r.rc.ihu_rounds = 65;
    CHECK(tsim_distvec_check(&r.rc) != NULL);
}

/* Node 1 sends node 0 a seqno request for node 0's own route, at `seq`, as if passed on from
 * further away. */
static void ask_for(struct rig *r, uint16_t seq) {
    struct tsim_tx tx = {
        .lora = r->rc.lora,
        .tx_dbm = r->rc.tx_dbm,
        .purpose = TSIM_PURPOSE_CONTROL,
    };
    uint8_t b[] = {0x02, 0, 0, 0, 0, 1, 0, 0, 0, 0, (uint8_t)seq, (uint8_t)(seq >> 8), 3};
    memcpy(tx.bytes, b, sizeof b);
    tx.len = sizeof b;
    CHECK(tsim_node_send(tsim_net_node(r->net, 1), &tx) != 0);
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

/* Node 1's queue is full for a while and refuses its announces. Node 0 heard none of them go
 * missing on the air, and must not count them against the link once node 1 is heard again. */
static void announces_the_queue_refused_are_not_counted_missed(void) {
    struct rig r;
    rig_init(&r);
    r.rc.imin = TSIM_S(8);
    r.rc.doublings = 0;
    r.rc.redundancy = 0;
    r.rc.cap = 0.05;
    struct tsim_net_params p = tsim_net_defaults(11);
    p.queue_limit = 1;
    r.nodes = 2;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 2, &tsim_distvec, &r.rc, &gate_mac, NULL);
    link(&r, 0, 1, LOSS_LOUD);
    gate_shut = false;
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(120));
    uint16_t before, after;
    CHECK(tsim_distvec_route(at(&r, 0), 1, NULL, &before));

    gate_shut = true;
    tsim_net_originate(r.net, 1, 0, 20); /* fills node 1's queue */
    uint64_t sent = tsim_net_ledger(r.net, 1)->frames[TSIM_PURPOSE_ANNOUNCE];
    tsim_sched_run_until(r.sched, TSIM_S(160)); /* five or so announces refused */
    CHECK_EQ_U64(tsim_net_ledger(r.net, 1)->frames[TSIM_PURPOSE_ANNOUNCE], sent);
    gate_shut = false;
    tsim_node_transmit(tsim_net_node(r.net, 1));
    tsim_sched_run_until(r.sched, TSIM_S(180));
    CHECK(tsim_distvec_route(at(&r, 0), 1, NULL, &after));
    CHECK(after <= before); /* no announce counted missed */
    rig_close(&r);
}

/* A change that comes after a node has announced in an imin interval goes in the next interval,
 * which stays imin - within one and a half imin - and does not wait in the second half of one
 * twice as long. */
static void a_change_after_announcing_at_imin_waits_no_longer_than_imin(void) {
    struct rig r;
    rig_init(&r);
    r.rc.imin = TSIM_S(60);
    r.rc.doublings = 3;
    build(&r, 6, 12);
    link(&r, 0, 1, LOSS_LOUD); /* node 1 speaks for nodes 2 to 5 */
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(1500));
    CHECK_EQ_I64(tsim_distvec_interval(at(&r, 0)), r.rc.imin << r.rc.doublings);

    announce_as(&r, 2, 0, 4, 0, 50); /* a new neighbour: back to imin */
    CHECK_EQ_I64(tsim_distvec_interval(at(&r, 0)), r.rc.imin);
    uint64_t count = tsim_net_ledger(r.net, 0)->frames[TSIM_PURPOSE_ANNOUNCE];
    while (tsim_net_ledger(r.net, 0)->frames[TSIM_PURPOSE_ANNOUNCE] == count) {
        tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_S(1));
    }
    announce_as(&r, 3, 0, 5, 0, 50); /* a change after it announced in this interval */
    CHECK_EQ_I64(tsim_distvec_interval(at(&r, 0)), r.rc.imin);
    count = tsim_net_ledger(r.net, 0)->frames[TSIM_PURPOSE_ANNOUNCE];
    tsim_time asked = tsim_sched_now(r.sched);
    while (tsim_net_ledger(r.net, 0)->frames[TSIM_PURPOSE_ANNOUNCE] == count &&
           tsim_sched_now(r.sched) < asked + 3 * r.rc.imin) {
        CHECK_EQ_I64(tsim_distvec_interval(at(&r, 0)), r.rc.imin);
        tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_S(1));
    }
    CHECK(tsim_sched_now(r.sched) <= asked + r.rc.imin * 3 / 2 + TSIM_S(1));
    rig_close(&r);
}

/* Node 1 keeps asking node 0 for a seq node 0 has already gone up to, several times an imin, as
 * starved nodes beyond it would. The requests must not keep putting off the announce that answers
 * them: it goes within an imin and a half of the first. */
static void requests_for_a_seq_reached_do_not_put_off_the_answer(void) {
    struct rig r;
    rig_init(&r);
    r.rc.imin = TSIM_S(60);
    r.rc.doublings = 3;
    line(&r, 2, 21);
    tsim_sched_run_until(r.sched, TSIM_S(1500));
    CHECK_EQ_I64(tsim_distvec_interval(at(&r, 0)), r.rc.imin << r.rc.doublings);
    uint64_t count = frames(&r, 0, TSIM_PURPOSE_ANNOUNCE);
    tsim_time asked = tsim_sched_now(r.sched);
    ask_for(&r, 1); /* a newer seq */
    while (frames(&r, 0, TSIM_PURPOSE_ANNOUNCE) == count &&
           tsim_sched_now(r.sched) < asked + 4 * r.rc.imin) {
        tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_S(20));
        ask_for(&r, 1); /* and again, for the seq it has now */
    }
    CHECK(tsim_sched_now(r.sched) <= asked + r.rc.imin * 3 / 2);
    rig_close(&r);
}

/* Node 0 hears 600 neighbours and names one to a frame, so its IHU round is 600 frames - more
 * than a byte holds, and more than twice it and one: told as 255, a neighbour would drop node 0's
 * IHU after 511 frames, before its turn came round. Node 1 speaks for the other 599. */
static void an_ihu_round_of_hundreds_of_frames_is_told_whole(void) {
    struct rig r;
    rig_init(&r);
    r.rc.ihu_max = 1;
    r.rc.imin = TSIM_S(60); /* the rest, which hear nobody, kept quiet */
    r.rc.doublings = 0;
    const uint32_t many = 601;
    build(&r, many, 22);
    link(&r, 0, 1, LOSS_LOUD);
    tsim_net_start(r.net);
    for (uint32_t v = 2; v < many; v++) {
        announce_as_then(&r, v, 0, v, 0, 50, TSIM_MS(200));
    }
    tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + 2 * r.rc.imin);
    CHECK_EQ_U64(tsim_distvec_round(at(&r, 0)), many - 1);
    rig_close(&r);
}

/* Node 1's message waits in its queue far longer than the acknowledgement wait. It is not sent
 * again meanwhile, nor given up: the wait starts when the frame goes. */
static void a_queued_message_is_neither_repeated_nor_given_up(void) {
    struct rig r;
    rig_init(&r);
    struct tsim_net_params p = tsim_net_defaults(13);
    p.queue_limit = 0;
    r.nodes = 2;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 2, &tsim_distvec, &r.rc, &gate_mac, NULL);
    link(&r, 0, 1, LOSS_LOUD);
    gate_shut = false;
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(120));
    uint32_t next;
    CHECK(route(&r, 1, 0, &next));

    gate_shut = true;
    uint64_t m = tsim_net_originate(r.net, 1, 0, 20);
    tsim_sched_run_until(r.sched, TSIM_S(720)); /* every retry's wait over, many times */
    CHECK(!tsim_net_message(r.net, m)->finished);
    gate_shut = false;
    tsim_node_transmit(tsim_net_node(r.net, 1));
    tsim_sched_run_until(r.sched, TSIM_S(780));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_DATA), 1);
    rig_close(&r);
}

/* With room for every IHU it is allowed, an infrastructure node with that many neighbours still
 * leaves room for a route, and the nodes beyond its neighbours learn of each other. */
static void a_full_ihu_list_leaves_room_for_a_route(void) {
    struct rig r;
    rig_init(&r);
    r.rc.ihu_max = 48;
    build(&r, 52, 14);
    for (uint32_t i = 1; i < 50; i++) {
        link(&r, 0, i, LOSS_LOUD); /* 49 neighbours of the hub */
    }
    link(&r, 50, 1, LOSS_LOUD);
    link(&r, 51, 2, LOSS_LOUD);
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(1800));
    uint32_t next;
    CHECK(route(&r, 50, 51, &next));
    CHECK_EQ_U64(next, 1);
    rig_close(&r);
}

/* A node keeps its promise. Among neighbours enough to suppress it, its quiet intervals double as
 * they go, and the longest silence after an announce stays within what that announce promised.
 * With a cap low enough, announces are held back and go when the bucket refills - at any point of
 * an interval, not only its second half. */
static void keeps_its_promise(double cap) {
    struct rig r;
    rig_init(&r);
    r.rc.imin = TSIM_S(60);
    r.rc.doublings = 3;
    r.rc.redundancy = 1;
    r.rc.cap = cap;
    build(&r, 5, 15);
    for (uint32_t a = 0; a < 5; a++) {
        for (uint32_t b = a + 1; b < 5; b++) {
            link(&r, a, b, LOSS_LOUD);
        }
    }
    tsim_net_start(r.net);
    uint64_t count = 0;
    tsim_time last = -1, promised = 0, longest = 0;
    bool kept = true;
    for (tsim_time t = TSIM_S(1); t < TSIM_S(4 * 3600); t += TSIM_S(1)) {
        tsim_sched_run_until(r.sched, t);
        uint64_t c = frames(&r, 0, TSIM_PURPOSE_ANNOUNCE);
        if (c != count) {
            if (last >= 0) {
                kept = kept && t - last <= promised + TSIM_S(1);
                longest = t - last > longest ? t - last : longest;
            }
            count = c;
            last = t;
            promised = tsim_distvec_promise(at(&r, 0));
        }
    }
    CHECK(kept);
    CHECK(longest > 3 * r.rc.imin); /* it did keep quiet, through doubling intervals */
    rig_close(&r);
}

static void a_node_announces_within_its_promise(void) { keeps_its_promise(0.5); }

static void a_node_held_back_by_its_cap_announces_within_its_promise(void) {
    keeps_its_promise(0.0005);
}

/* A hub names its twelve neighbours two to a frame, and announces seldom: a round of its IHUs
 * takes hours, far longer than neighbour_timeout. Its neighbours have to keep the link until the
 * round comes back to them, not drop it for an IHU merely not yet due again. */
static void an_ihu_round_longer_than_the_timeout_keeps_the_links(void) {
    struct rig r;
    rig_init(&r);
    r.rc.ihu_max = 2;
    r.rc.imin = TSIM_S(60);
    r.rc.doublings = 3;
    r.rc.neighbour_timeout = TSIM_S(600);
    build(&r, 13, 17);
    for (uint32_t i = 1; i < 13; i++) {
        link(&r, 0, i, LOSS_LOUD);
    }
    tsim_net_start(r.net);
    /* The first round: until the hub has named every neighbour once. */
    tsim_time t = 0, named = -1;
    for (; t < TSIM_S(8 * 3600) && named < 0; t += TSIM_S(60)) {
        tsim_sched_run_until(r.sched, t);
        uint32_t linked = 0;
        for (uint32_t i = 1; i < 13; i++) {
            linked += tsim_distvec_neighbours(at(&r, i)) == 1;
        }
        named = linked == 12 ? t : -1;
    }
    CHECK(named >= 0);
    uint32_t lost = 0;
    for (; t < named + TSIM_S(6 * 3600); t += TSIM_S(60)) {
        tsim_sched_run_until(r.sched, t);
        for (uint32_t i = 1; i < 13; i++) {
            lost += tsim_distvec_neighbours(at(&r, i)) != 1;
        }
    }
    CHECK_EQ_U64(lost, 0);
    rig_close(&r);
}

/* The acknowledgement of a message's first attempt comes while its retry still waits in the
 * source's queue. Nothing of the message goes on the air once it is answered: the retry is taken
 * back. */
static void a_retry_still_queued_when_the_answer_comes_is_taken_back(void) {
    struct rig r;
    rig_init(&r);
    r.rc.ack_wait = TSIM_MS(100);
    r.rc.ack_factor = 0;
    r.rc.jitter = 0;
    struct tsim_net_params p = tsim_net_defaults(18);
    p.queue_limit = 0;
    r.nodes = 3;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 3, &tsim_distvec, &r.rc, &gate_mac, NULL);
    link(&r, 0, 1, LOSS_LOUD);
    link(&r, 1, 2, LOSS_LOUD);
    gate_node = 0;
    gate_shut = false;
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint32_t next;
    CHECK(route(&r, 0, 2, &next));

    uint64_t data = frames(&r, 0, TSIM_PURPOSE_DATA);
    uint64_t m = tsim_net_originate(r.net, 0, 2, 20);
    tsim_sched_run_until(r.sched, TSIM_S(300) + TSIM_MS(100)); /* the first attempt has gone */
    gate_shut = true; /* the retry, at 100 ms, waits; the answer comes at about 240 ms */
    tsim_sched_run_until(r.sched, TSIM_S(302));
    CHECK(tsim_net_message(r.net, m)->finished);
    gate_shut = false;
    tsim_node_transmit(tsim_net_node(r.net, 0));
    tsim_sched_run_until(r.sched, TSIM_S(330));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA) - data, 1);
    gate_node = 1;
    rig_close(&r);
}

/* Node 0's retry of a message waits in its queue when node 1, retrying the first copy, is heard
 * passing it on, so the retry is taken back as needless. The acknowledgement of that copy never
 * comes - node 1 cannot hear node 2 - so node 0 has to go on waiting, retrying and at last giving
 * up, not wait for ever on the frame it took back. */
static void a_retry_taken_back_still_ends_in_an_answer_or_giving_up(void) {
    struct rig r;
    rig_init(&r);
    r.rc.ack_wait = TSIM_S(1);
    r.rc.ack_factor = 0;
    r.rc.hop_wait = TSIM_S(10);
    struct tsim_net_params p = tsim_net_defaults(16);
    p.queue_limit = 0;
    r.nodes = 4;
    r.sched = tsim_sched_create();
    r.net = tsim_net_create(r.sched, &p, 4, &tsim_distvec, &r.rc, &gate_mac, NULL);
    for (uint32_t i = 0; i + 1 < 4; i++) {
        link(&r, i, i + 1, LOSS_LOUD);
    }
    gate_node = 0;
    gate_shut = false;
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint32_t next;
    CHECK(route(&r, 0, 3, &next));

    tsim_phy_set_loss_from(tsim_net_phy(r.net), 2, 1, LOSS_NONE); /* node 1 stops hearing node 2 */
    uint64_t m = tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(300) + TSIM_MS(500)); /* the first copy has gone */
    gate_shut = true;                           /* node 0's retry, after a second, waits */
    tsim_sched_run_until(r.sched, TSIM_S(320)); /* node 1 sends its copy again, at ten seconds */
    gate_shut = false;
    tsim_node_transmit(tsim_net_node(r.net, 0));
    tsim_sched_run_until(r.sched, TSIM_S(420));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1); /* node 3 has it; its answer is lost */
    CHECK(tsim_net_message(r.net, m)->finished);
    gate_node = 1;
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

/* With parents, relays route among themselves: leaf, relay, relay, relay, leaf. No relay announces
 * a route to a leaf, the relay hearing one keeps it for the last hop, and a message from leaf to
 * leaf goes along the relays' route to the far leaf's parent, which hands it over. */
static void with_parents_relays_route_among_themselves(void) {
    struct rig r;
    uint32_t parents[5];
    rig_init(&r);
    strcpy(r.rc.relays, "1-3");
    r.rc.leaves = TSIM_DISTVEC_LEAVES_PARENT_ORACLE;
    r.rc.parents = parents;
    line(&r, 5, 1);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    CHECK_EQ_U64(parents[0], 1);
    CHECK_EQ_U64(parents[4], 3);
    for (uint32_t i = 1; i <= 3; i++) {
        CHECK_EQ_U64(parents[i], i); /* a relay is its own */
    }
    uint32_t next;
    CHECK(route(&r, 1, 0, &next) && next == 0); /* the last hop, kept */
    CHECK(!route(&r, 2, 0, &next));             /* never announced */
    CHECK(!route(&r, 3, 0, &next));
    CHECK(!route(&r, 0, 4, &next));
    CHECK(route(&r, 0, 3, &next) && next == 1); /* a leaf hears the relays' routes */
    CHECK(tsim_distvec_next(at(&r, 0), 4, &next) && next == 1);
    CHECK(tsim_distvec_next(at(&r, 2), 4, &next) && next == 3);
    CHECK(tsim_distvec_next(at(&r, 3), 4, &next) && next == 4);
    CHECK(tsim_distvec_next(at(&r, 4), 0, &next) && next == 3);
    uint64_t m = tsim_net_originate(r.net, 0, 4, 20);
    tsim_sched_run_until(r.sched, TSIM_S(400));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK(tsim_net_message(r.net, m)->finished); /* acknowledged the same way back */
    for (uint32_t i = 1; i <= 3; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
    }
    rig_close(&r);
}

/* A leaf that hears no relay has no parent: nothing reaches it, and nobody asks about it. */
static void a_leaf_with_no_parent_is_not_asked_about(void) {
    struct rig r;
    uint32_t parents[3];
    rig_init(&r);
    strcpy(r.rc.relays, "2");
    r.rc.leaves = TSIM_DISTVEC_LEAVES_PARENT_ORACLE;
    r.rc.parents = parents;
    line(&r, 3, 1); /* leaf, leaf, relay */
    tsim_sched_run_until(r.sched, TSIM_S(300));
    CHECK_EQ_U64(parents[0], TSIM_DISTVEC_NO_PARENT);
    CHECK_EQ_U64(parents[1], 2);
    uint32_t next;
    CHECK(!tsim_distvec_next(at(&r, 2), 0, &next));
    uint64_t m = tsim_net_originate(r.net, 2, 0, 20);
    tsim_sched_run_until(r.sched, TSIM_S(400));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 0);
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_CONTROL), 0); /* no requests */
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_DATA), 0);
    rig_close(&r);

    struct tsim_distvec_config bad = r.rc;
    bad.leaves = TSIM_DISTVEC_LEAVES_PARENT_ORACLE + 1;
    CHECK(tsim_distvec_check(&bad) != NULL);
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

/* A link breaks under traffic, with links costed by ETX. The frame the hop before it cannot get
 * across counts against the link, and the dearer route it then announces is infeasible upstream,
 * so the nodes behind it stop sending into the break at once. The hop itself, with nothing more
 * crossing, keeps its route until the neighbour times out. */
static void a_broken_link_is_found_by_the_data_crossing_it(void) {
    struct rig r;
    rig_init(&r);
    r.rc.etx = true;
    r.rc.etx_max = 8;
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

/* Without ETX every link costs the same, so a hop lost - to the load, say - makes no route
 * dearer and none infeasible: the line keeps its routes, and their metrics, until the link goes
 * over etx_max or its neighbour times out. */
/* The books: a settled line has lost nothing. A link cut under a message first only costs more at
 * the node that sent into it, which is enough for the nodes behind to lose their routes across it -
 * outages, without a route for as long as they stay lost - and the link itself is booked lost once
 * it has gone quiet. */
static void a_link_lost_and_the_routes_across_it_are_booked(void) {
    struct rig r;
    rig_init(&r);
    r.rc.etx = true;
    r.rc.etx_max = 8;
    line(&r, 4, 1);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    struct tsim_distvec_stats st;
    for (uint32_t i = 0; i < 4; i++) {
        tsim_distvec_stats(at(&r, i), &st);
        CHECK_EQ_U64(st.outages, 0);
        for (int c = 0; c < TSIM_DISTVEC_DOWN_COUNT; c++) {
            CHECK_EQ_U64(st.down[c], 0);
        }
    }
    link(&r, 2, 3, LOSS_NONE);
    tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(420));
    tsim_distvec_stats(at(&r, 0), &st);
    CHECK_EQ_U64(st.outages, 1); /* its route to 3 */
    double unrouted = st.unrouted_s;
    CHECK(unrouted > 0);
    tsim_sched_run_until(r.sched, TSIM_S(480));
    tsim_distvec_stats(at(&r, 0), &st);
    CHECK(st.unrouted_s >= unrouted + 59.9); /* still without it, a minute on */
    tsim_sched_run_until(r.sched,
                         TSIM_S(480) + r.rc.neighbour_timeout + r.rc.neighbour_timeout / 4);
    tsim_distvec_stats(at(&r, 2), &st);
    uint64_t downs = 0;
    for (int c = 0; c < TSIM_DISTVEC_DOWN_COUNT; c++) {
        downs += st.down[c];
        CHECK_EQ_U64(st.down_strong[c], 0); /* no floors without power control */
    }
    CHECK_EQ_U64(downs, 1);
    CHECK_EQ_U64(st.outages, 1);
    rig_close(&r);
}

/* With seq_period, a node raises its own seq every period, give or take 10%, unasked, and the
 * routes to it carry the new one; without, a quiet line never raises one. */
static void seq_period_raises_the_seq_unasked(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 3, 1);
    tsim_sched_run_until(r.sched, TSIM_S(1200));
    CHECK_EQ_U64(tsim_distvec_seq(at(&r, 2)), 0);
    rig_close(&r);

    rig_init(&r);
    r.rc.seq_period = TSIM_S(60);
    line(&r, 3, 1);
    tsim_sched_run_until(r.sched, TSIM_S(1200));
    uint16_t seq = tsim_distvec_seq(at(&r, 2));
    CHECK(seq >= 18 && seq <= 23); /* 1200 s at 54 to 66 s, the first within one period */
    uint32_t next;
    CHECK(route(&r, 0, 2, &next) && next == 1);
    struct tsim_distvec_stats st;
    tsim_distvec_stats(at(&r, 2), &st);
    CHECK_EQ_U64(st.seq_raised, 0); /* none of them asked for */
    rig_close(&r);

    struct tsim_distvec_config bad = r.rc;
    bad.seq_period = -1;
    CHECK(tsim_distvec_check(&bad) != NULL);
}

static void a_lost_hop_makes_no_route_dearer(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 4, 1);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    struct tsim_lora l = r.rc.lora;
    uint16_t hop = (uint16_t)ceil((double)tsim_lora_airtime(&l, r.rc.ref_len) / (double)TSIM_MS(1));
    uint16_t metric;
    CHECK(tsim_distvec_route(at(&r, 0), 3, NULL, &metric));
    CHECK_EQ_U64(metric, 3 * hop);
    link(&r, 2, 3, LOSS_NONE);
    uint64_t m = tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(420));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 0);
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK(tsim_distvec_route(at(&r, 0), 3, NULL, &metric));
    CHECK_EQ_U64(metric, 3 * hop);
    tsim_sched_run_until(r.sched,
                         TSIM_S(420) + r.rc.neighbour_timeout + r.rc.neighbour_timeout / 4);
    uint32_t next;
    CHECK(!route(&r, 2, 3, &next));
    CHECK(!route(&r, 0, 3, &next));
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
    /* Up to 21 days at imax: more than the two bytes of a promise hold. */
    bad = c;
    bad.doublings = 16;
    CHECK(tsim_distvec_check(&bad) != NULL);
    struct tsim_distvec_config slow = c; /* 8 s to 2.3 h, and promising about 9 h */
    slow.doublings = 10;
    CHECK(tsim_distvec_check(&slow) == NULL);
}

/* The oracle's routes come from the links themselves: the fewest hops over links with its margin
 * to spare, and nothing announced or asked for. Nodes 0 to 4 are a line with a shortcut from 0 to
 * 2; the link from 0 to 4 is decoded, at 14 - 137 = -123 dBm over a -124.5 dBm floor, but with
 * less than the 3 dB margin, so it is not used; node 5 hears no one. */
static void the_oracle_routes_by_the_fewest_hops_and_announces_nothing(void) {
    struct rig r;
    struct tsim_distvec_oracle o = {0};
    rig_init(&r);
    r.rc.oracle = true;
    r.rc.oracle_routes = &o;
    build(&r, 6, 1);
    for (uint32_t i = 0; i + 1 < 5; i++) {
        link(&r, i, i + 1, LOSS_LOUD);
    }
    link(&r, 0, 2, LOSS_LOUD);
    link(&r, 0, 4, 137);
    CHECK(tsim_distvec_oracle_build(&o, tsim_net_phy(r.net), &r.rc));
    tsim_net_start(r.net);
    uint32_t next = 0;
    CHECK(route(&r, 0, 4, &next) && next == 2);
    CHECK(route(&r, 4, 0, &next) && next == 3);
    CHECK(route(&r, 1, 3, &next) && next == 2);
    CHECK_EQ_U64(o.route[0 * 6 + 4].hops, 3);
    CHECK(!route(&r, 0, 5, &next));
    uint64_t m = tsim_net_originate(r.net, 0, 4, 40);
    uint64_t lost = tsim_net_originate(r.net, 1, 5, 40);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(tsim_net_message(r.net, lost)->delivered, 0);
    CHECK(tsim_net_message(r.net, lost)->finished);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 0); /* the shortcut, not the line */
    for (uint32_t i = 0; i < 6; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_ANNOUNCE), 0);
    }
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_CONTROL), 0); /* no requests for node 5 */
    rig_close(&r);
    tsim_distvec_oracle_free(&o);

    /* With only the ends relays, the line's middle passes nothing on: no route across it. */
    rig_init(&r);
    r.rc.oracle = true;
    r.rc.oracle_routes = &o;
    strcpy(r.rc.relays, "0,2");
    build(&r, 3, 1);
    link(&r, 0, 1, LOSS_LOUD);
    link(&r, 1, 2, LOSS_LOUD);
    CHECK(tsim_distvec_oracle_build(&o, tsim_net_phy(r.net), &r.rc));
    tsim_net_start(r.net);
    CHECK(!route(&r, 0, 2, &next));
    CHECK(route(&r, 0, 1, &next) && next == 1);
    CHECK(route(&r, 1, 2, &next) && next == 2);
    rig_close(&r);
    tsim_distvec_oracle_free(&o);

    struct tsim_distvec_config bad = r.rc;
    bad.oracle_margin_db = -1;
    CHECK(tsim_distvec_check(&bad) != NULL);
}

/* The link oracle judges links by the oracle's, not by announces: with no IHUs sent, sensing finds
 * no link both ways, and the link oracle every one with its margin. Nodes 0 to 3 are a line, and
 * the link from 0 to 3 is decoded, at -123 dBm over a -124.5 dBm floor, but with less than the 3 dB
 * margin, so the link oracle never uses it. Routes are the protocol's own, from announces. */
static void the_link_oracle_uses_its_links_and_no_others(void) {
    struct rig r;
    struct tsim_distvec_oracle o = {0};
    for (int oracle = 0; oracle < 2; oracle++) {
        rig_init(&r);
        r.rc.ihu_max = 0;
        r.rc.links = oracle ? TSIM_DISTVEC_LINKS_ORACLE : TSIM_DISTVEC_LINKS_SENSED;
        r.rc.oracle_routes = oracle ? &o : NULL;
        build(&r, 4, 1);
        for (uint32_t i = 0; i + 1 < 4; i++) {
            link(&r, i, i + 1, LOSS_LOUD);
        }
        link(&r, 0, 3, 137);
        CHECK(tsim_distvec_oracle_build(&o, tsim_net_phy(r.net), &r.rc));
        tsim_net_start(r.net);
        tsim_sched_run_until(r.sched, TSIM_S(300));
        uint32_t next = 0;
        if (!oracle) {
            CHECK(!route(&r, 0, 1, &next));
            CHECK(!route(&r, 0, 3, &next));
        } else {
            CHECK(route(&r, 0, 1, &next) && next == 1);
            CHECK(route(&r, 0, 3, &next) && next == 1);
            CHECK(route(&r, 3, 0, &next) && next == 2);
            CHECK(frames(&r, 0, TSIM_PURPOSE_ANNOUNCE) > 0);
            uint64_t m = tsim_net_originate(r.net, 0, 3, 40);
            tsim_sched_run_until(r.sched, TSIM_S(600));
            CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
        }
        rig_close(&r);
        tsim_distvec_oracle_free(&o);
    }

    struct tsim_distvec_config bad = r.rc;
    bad.links = TSIM_DISTVEC_LINKS_STRENGTH + 1;
    CHECK(tsim_distvec_check(&bad) != NULL);
}

/* By strength, SF7 at 14 dBm, without power control: a neighbour at loss L is heard at SNR
 * 131 - L, over a -7.5 dB floor, so with 138.5 - L dB of margin. With link_band 1, a link comes up
 * with 3 dB each way, at a loss of 135.5 or less, and goes down below 2, over 136.5. */
static void links_by_strength_come_up_and_go_down_on_margin(void) {
    struct rig r;
    rig_init(&r);
    r.rc.links = TSIM_DISTVEC_LINKS_STRENGTH;
    r.rc.link_band_db = 1;
    build(&r, 2, 1);
    link(&r, 0, 1, 137);
    tsim_net_start(r.net);
    uint32_t next = 0;
    tsim_sched_run_until(r.sched, TSIM_S(1800));
    CHECK(!route(&r, 0, 1, &next)); /* decoded, but with 1.5 dB */
    link(&r, 0, 1, 134);
    tsim_sched_run_until(r.sched, TSIM_S(5400));
    CHECK(route(&r, 0, 1, &next) && next == 1);
    CHECK(route(&r, 1, 0, &next) && next == 0);
    link(&r, 0, 1, 136); /* 2.5 dB: inside the band, so it stays */
    tsim_sched_run_until(r.sched, TSIM_S(9000));
    CHECK(route(&r, 0, 1, &next) && next == 1);
    link(&r, 0, 1, 137.5); /* 1 dB: below it */
    tsim_sched_run_until(r.sched, TSIM_S(12600));
    CHECK(!route(&r, 0, 1, &next));
    CHECK(!route(&r, 1, 0, &next));
    rig_close(&r);

    /* One way only: node 1 hears node 0, never the reverse, so neither uses the link. */
    rig_init(&r);
    r.rc.links = TSIM_DISTVEC_LINKS_STRENGTH;
    build(&r, 2, 1);
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 0, 1, LOSS_LOUD);
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 1, 0, LOSS_NONE);
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(1800));
    CHECK(!route(&r, 0, 1, &next));
    CHECK(!route(&r, 1, 0, &next));
    rig_close(&r);

    /* One way 4.5 dB, the other 2.6: the end hearing 2.6 reports 2, never 3, so neither end takes
     * the link up. */
    rig_init(&r);
    r.rc.links = TSIM_DISTVEC_LINKS_STRENGTH;
    build(&r, 2, 1);
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 0, 1, 134);
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 1, 0, 135.9);
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(1800));
    CHECK(!route(&r, 0, 1, &next));
    CHECK(!route(&r, 1, 0, &next));
    rig_close(&r);

    /* Without power control, a floor is reckoned from tx_dbm itself, not tx_dbm rounded to a power
     * byte: at 14.5 dBm, a loss of 135.75 leaves 3.25 dB, up, where 15 dBm would leave 2.75. */
    rig_init(&r);
    r.rc.links = TSIM_DISTVEC_LINKS_STRENGTH;
    r.rc.tx_dbm = 14.5;
    build(&r, 2, 1);
    link(&r, 0, 1, 135.75);
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(1800));
    CHECK(route(&r, 0, 1, &next) && next == 1);
    rig_close(&r);

    struct tsim_distvec_config bad = r.rc;
    bad.link_band_db = -1;
    CHECK(tsim_distvec_check(&bad) != NULL);
}

/* By strength, a hop lost is a frame lost, not a link: with node 1 deaf to node 0 a while, every
 * try of a message fails, and the link stays in use. */
static void a_lost_hop_takes_no_link_down_by_strength(void) {
    struct rig r;
    rig_init(&r);
    r.rc.links = TSIM_DISTVEC_LINKS_STRENGTH;
    line(&r, 2, 1);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint32_t next = 0;
    CHECK(route(&r, 0, 1, &next));
    tsim_phy_set_loss_from(tsim_net_phy(r.net), 0, 1, LOSS_NONE);
    uint64_t m = tsim_net_originate(r.net, 0, 1, 40);
    tsim_sched_run_until(r.sched, TSIM_S(420));
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 0);
    struct tsim_distvec_stats st;
    tsim_distvec_stats(at(&r, 0), &st);
    for (int c = 0; c < TSIM_DISTVEC_DOWN_COUNT; c++) {
        CHECK_EQ_U64(st.down[c], 0);
    }
    CHECK(route(&r, 0, 1, &next) && next == 1);
    rig_close(&r);
}

/* By strength, a neighbour is gone when dead_hops hops to it are lost with nothing heard from it
 * between, not when it goes quiet: node 1, the middle of a line, falls silent, and node 0 keeps it
 * until frames sent through it fail. A neighbour merely unheard is kept until silent_max. */
static void by_strength_a_neighbour_is_gone_when_frames_to_it_fail(void) {
    struct rig r;
    rig_init(&r);
    r.rc.links = TSIM_DISTVEC_LINKS_STRENGTH;
    r.rc.dead_hops = 3;
    line(&r, 3, 1);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    uint32_t next = 0;
    CHECK(route(&r, 0, 2, &next) && next == 1);
    for (uint32_t i = 0; i < 3; i += 2) {
        link(&r, 1, i, LOSS_NONE);
    }
    tsim_sched_run_until(r.sched, TSIM_S(600 + 3 * 3600)); /* past neighbour_timeout */
    CHECK(route(&r, 0, 1, &next) && next == 1);
    /* Each message here gives up on two hops. */
    uint64_t m = tsim_net_originate(r.net, 0, 2, 40);
    tsim_sched_run_until(r.sched, TSIM_S(900 + 3 * 3600));
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(tsim_net_message(r.net, m)->drops[TSIM_DROP_RETRIES], 2);
    CHECK(route(&r, 0, 1, &next)); /* two lost: not yet */
    m = tsim_net_originate(r.net, 0, 2, 40);
    tsim_sched_run_until(r.sched, TSIM_S(1200 + 3 * 3600));
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK(!route(&r, 0, 1, &next));
    CHECK(!route(&r, 0, 2, &next));
    struct tsim_distvec_stats st;
    tsim_distvec_stats(at(&r, 0), &st);
    CHECK_EQ_U64(st.down[TSIM_DISTVEC_DOWN_HOP], 1);
    CHECK_EQ_U64(st.down[TSIM_DISTVEC_DOWN_TIMEOUT], 0);
    /* Node 2 sent nothing through node 1: it keeps it until silent_max. */
    CHECK(route(&r, 2, 1, &next) && next == 1);
    tsim_sched_run_until(r.sched, TSIM_S(1200 + 25 * 3600));
    CHECK(!route(&r, 2, 1, &next));
    tsim_distvec_stats(at(&r, 2), &st);
    CHECK_EQ_U64(st.down[TSIM_DISTVEC_DOWN_TIMEOUT], 1);
    rig_close(&r);

    struct tsim_distvec_config bad = r.rc;
    bad.dead_hops = 0;
    CHECK(tsim_distvec_check(&bad) != NULL);
    /* Sensing never reads them: a config that leaves them 0 is still good there. */
    bad.links = TSIM_DISTVEC_LINKS_SENSED;
    bad.silent_max = 0;
    bad.link_band_db = -1;
    CHECK(tsim_distvec_check(&bad) == NULL);
}

/* By strength, housekeeping keeps time with silent_max, not neighbour_timeout: two nodes heard
 * early, while their promises are short, fall silent, and are forgotten soon after silent_max -
 * within 300 s here, where looking every 15 min, as neighbour_timeout would, took until 900 s. */
static void by_strength_silence_is_checked_as_often_as_silent_max_needs(void) {
    struct rig r;
    rig_init(&r);
    r.rc.links = TSIM_DISTVEC_LINKS_STRENGTH;
    r.rc.silent_max = TSIM_S(240);
    r.rc.imin = TSIM_S(1);
    r.rc.doublings = 14; /* imax 4.5 h: housekeeping by neighbour_timeout would come every 15 min */
    r.rc.quiet_max = 0;
    r.rc.cap = 0.5;
    CHECK(tsim_distvec_check(&r.rc) == NULL);
    line(&r, 2, 1);
    tsim_sched_run_until(r.sched, TSIM_S(20));
    uint32_t next = 0;
    CHECK(route(&r, 0, 1, &next) && next == 1);
    link(&r, 0, 1, LOSS_NONE);
    tsim_sched_run_until(r.sched, TSIM_S(20 + 360)); /* by neighbour_timeout's, still kept */
    CHECK(!route(&r, 0, 1, &next));
    struct tsim_distvec_stats st;
    tsim_distvec_stats(at(&r, 0), &st);
    CHECK_EQ_U64(st.down[TSIM_DISTVEC_DOWN_TIMEOUT], 1);
    rig_close(&r);
}

/* With power control, SF7 and 14 dBm: a neighbour at loss L is heard at SNR 131 - L, so its floor
 * is L - 124.5 dBm, and a frame to it goes at L - 114.5, rounded up, between -9 and 14. */
static void power_rig(struct rig *r, uint32_t nodes, const double *losses) {
    rig_init(r);
    r->rc.power = true;
    r->rc.power_k = 0;
    build(r, nodes, 1);
    for (uint32_t i = 0; i + 1 < nodes; i++) {
        link(r, i, i + 1, losses[i]);
    }
    tsim_net_start(r->net);
}

static void a_near_neighbour_is_sent_to_quieter_than_a_far_one(void) {
    struct rig r;
    double losses[] = {115, 122};
    power_rig(&r, 3, losses);
    CHECK_EQ_I64((int64_t)tsim_distvec_power(at(&r, 1), 0), 14); /* not heard yet */
    tsim_sched_run_until(r.sched, TSIM_S(300));
    CHECK_EQ_I64((int64_t)tsim_distvec_power(at(&r, 1), 0), 1);    /* 115 - 114.5, up */
    CHECK_EQ_I64((int64_t)tsim_distvec_power(at(&r, 1), 2), 8);    /* 122 - 114.5 */
    CHECK_EQ_I64((int64_t)tsim_distvec_node_power(at(&r, 1)), 14); /* power_k 0: every frame else */
    rig_close(&r);

    double near[] = {100, 100};
    power_rig(&r, 3, near);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    CHECK_EQ_I64((int64_t)tsim_distvec_power(at(&r, 1), 0), -9); /* no quieter than tx_min */
    rig_close(&r);
}

/* Every relay must be heard by the hop before it, which listens for it: on a line whose links
 * alternate near and far, a relay sending on over a near link still goes loud enough for the far
 * one it was sent over, so no hop has to try again. */
static void a_relay_goes_loud_enough_for_the_hop_before(void) {
    struct rig r;
    double losses[] = {100, 120, 100, 120, 100};
    power_rig(&r, 6, losses);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    uint64_t m = tsim_net_originate(r.net, 0, 5, 40);
    tsim_sched_run_until(r.sched, TSIM_S(360));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1);
    for (uint32_t i = 1; i < 5; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
    }
    rig_close(&r);
}

/* A link that has got worse since its floor was measured loses the first try, and the second goes
 * step_db louder. */
static void lost_and_tried_louder(double step) {
    struct rig r;
    double losses[] = {100, 120, 100};
    rig_init(&r);
    r.rc.power = true;
    r.rc.power_k = 0;
    r.rc.step_db = step;
    build(&r, 4, 1);
    for (uint32_t i = 0; i < 3; i++) {
        link(&r, i, i + 1, losses[i]);
    }
    tsim_net_start(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    CHECK_EQ_I64((int64_t)tsim_distvec_power(at(&r, 1), 2), 6);
    link(&r, 1, 2, 131); /* at 6 dBm, -8 dB: under the floor; at 9 dBm, -5 */
    uint64_t m = tsim_net_originate(r.net, 0, 3, 40);
    /* Before the source would send it again: its acknowledgement crosses the same link. */
    tsim_sched_run_until(r.sched, TSIM_S(300) + TSIM_MS(4800));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 2);
    rig_close(&r);
}

/* A step under a dB still raises a retry by a whole one, which here is what it lacked. */
static void a_hop_lost_at_its_power_is_tried_again_louder(void) {
    lost_and_tried_louder(3);
    lost_and_tried_louder(0.5);
}

/* With power_k, frames for every neighbour go loud enough for the k with the lowest floors. */
static void power_k_reaches_the_k_nearest(void) {
    struct rig r;
    rig_init(&r);
    r.rc.power = true;
    r.rc.power_k = 2;
    build(&r, 4, 1);
    link(&r, 0, 1, 100); /* floors -24.5, -14.5 and -4.5 */
    link(&r, 0, 2, 110);
    link(&r, 0, 3, 120);
    tsim_net_start(r.net);
    CHECK_EQ_I64((int64_t)tsim_distvec_node_power(at(&r, 0)), 14); /* none known yet */
    tsim_sched_run_until(r.sched, TSIM_S(300));
    CHECK_EQ_I64((int64_t)tsim_distvec_node_power(at(&r, 0)), -4); /* -14.5 + 10, up */
    rig_close(&r);
}

/* A tx_dbm that is not a whole dBm is still what full power means: not rounded down. */
static void a_fractional_tx_dbm_is_kept_at_the_top(void) {
    struct rig r;
    rig_init(&r);
    r.rc.power = true;
    r.rc.power_k = 0;
    r.rc.tx_dbm = 13.5;
    build(&r, 3, 1);
    link(&r, 0, 1, 100);
    link(&r, 1, 2, 128); /* its floor 3.5 dBm: 13.5 with the margin */
    tsim_net_start(r.net);
    CHECK(tsim_distvec_power(at(&r, 1), 2) == 13.5); /* not heard yet */
    CHECK(tsim_distvec_node_power(at(&r, 1)) == 13.5);
    tsim_sched_run_until(r.sched, TSIM_S(300));
    CHECK_EQ_I64((int64_t)tsim_distvec_power(at(&r, 1), 0), -9); /* 100 - 114.5, up to tx_min */
    CHECK(tsim_distvec_power(at(&r, 1), 2) == 13.5);             /* 14 is over the top */
    rig_close(&r);
}

static void power_settings_are_checked(void) {
    struct tsim_lora l = tsim_lora_default(9, 125000);
    struct tsim_distvec_config c = tsim_distvec_default(0, &l, 14.0);
    CHECK(c.snr_floor_db == -12.5);
    CHECK(c.power && c.power_k == 8); /* the default */
    CHECK(!c.etx && c.etx_max == 32);
    CHECK(tsim_distvec_check(&c) == NULL);
    struct tsim_distvec_config bad = c;
    bad.tx_min_dbm = 14.5; /* no whole dBm between */
    CHECK(tsim_distvec_check(&bad) != NULL);
    bad = c;
    bad.margin_db = -1;
    CHECK(tsim_distvec_check(&bad) != NULL);
    bad.power = false; /* not looked at without power control */
    CHECK(tsim_distvec_check(&bad) == NULL);
}

/* Each message's loss is booked where it happened: across a whole line, on time, every hop
 * closer; with the last link cut, given up at the hop before the destination; and once the line
 * has learnt of the cut, without a route at the source. */
static void a_lost_message_is_booked_where_it_was_lost(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 4, 1);
    struct tsim_lora l = tsim_lora_default(7, 125000);
    struct tsim_metrics *m = tsim_metrics_create(r.net, TSIM_S(60));
    CHECK(tsim_metrics_links(m, &l, 14.0));
    tsim_sched_run_until(r.sched, TSIM_S(300));
    tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(400));
    link(&r, 2, 3, LOSS_NONE);
    tsim_net_originate(r.net, 0, 3, 20);
    tsim_time forgotten = TSIM_S(400) + r.rc.neighbour_timeout + r.rc.neighbour_timeout / 4;
    tsim_sched_run_until(r.sched, forgotten);
    tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, forgotten + TSIM_S(100));
    struct tsim_report rep;
    tsim_metrics_report(m, &rep);
    const struct tsim_losses *ls = &rep.losses;
    CHECK_EQ_U64(rep.unicast.messages, 3);
    CHECK_EQ_U64(ls->on_time, 1);
    CHECK_EQ_U64(ls->dropped[TSIM_DROP_RETRIES][TSIM_PLACE_LAST_HOP], 1);
    CHECK_EQ_U64(ls->dropped[TSIM_DROP_NO_ROUTE][TSIM_PLACE_SOURCE], 1);
    CHECK(ls->drops[TSIM_DROP_NO_ROUTE] >= 1);
    CHECK_EQ_U64(ls->progress[TSIM_PROGRESS_LEVEL] + ls->progress[TSIM_PROGRESS_FARTHER] +
                     ls->progress[TSIM_PROGRESS_NOT_A_LINK],
                 0);
    CHECK(ls->progress[TSIM_PROGRESS_CLOSER] >= 3 + 2);
    CHECK(ls->hops[0][TSIM_PHY_WEAK] >= 1); /* sent across the cut */
    CHECK(ls->hops[0][TSIM_PHY_DECODED] >= 3 + 2);
    CHECK(ls->hops[1][TSIM_PHY_DECODED] >= 3); /* the acknowledgement */
    tsim_metrics_destroy(m);
    rig_close(&r);
}

int main(void) {
    RUN(a_line_converges_on_its_one_path);
    RUN(the_oracle_routes_by_the_fewest_hops_and_announces_nothing);
    RUN(the_link_oracle_uses_its_links_and_no_others);
    RUN(links_by_strength_come_up_and_go_down_on_margin);
    RUN(a_lost_hop_takes_no_link_down_by_strength);
    RUN(by_strength_a_neighbour_is_gone_when_frames_to_it_fail);
    RUN(by_strength_silence_is_checked_as_often_as_silent_max_needs);
    RUN(a_near_neighbour_is_sent_to_quieter_than_a_far_one);
    RUN(a_relay_goes_loud_enough_for_the_hop_before);
    RUN(a_hop_lost_at_its_power_is_tried_again_louder);
    RUN(power_k_reaches_the_k_nearest);
    RUN(a_fractional_tx_dbm_is_kept_at_the_top);
    RUN(power_settings_are_checked);
    RUN(a_message_crosses_the_line_and_is_acknowledged);
    RUN(every_node_of_a_grid_reaches_every_other);
    RUN(a_one_way_link_is_never_used);
    RUN(every_neighbour_hears_its_ihu_in_turn);
    RUN(an_ihu_lost_is_sent_again_in_turn);
    RUN(a_link_lasts_its_ihu_rounds);
    RUN(a_link_past_the_longest_wait_goes_whichever_announce_comes);
    RUN(a_link_gone_one_way_is_dropped_by_the_side_still_hearing);
    RUN(a_retraction_the_queue_refused_goes_later);
    RUN(a_retraction_lost_on_the_air_is_repeated);
    RUN(a_retraction_goes_in_three_frames);
    RUN(a_message_into_a_dead_end_brings_the_retraction_again);
    RUN(ihus_the_queue_refused_go_in_the_next_frame);
    RUN(a_node_whose_mac_holds_its_announces_promises_the_wait);
    RUN(a_node_held_back_for_days_is_kept);
    RUN(a_newer_route_displaces_infeasible_cheaper_ones);
    RUN(announces_the_queue_refused_are_not_counted_missed);
    RUN(a_change_after_announcing_at_imin_waits_no_longer_than_imin);
    RUN(requests_for_a_seq_reached_do_not_put_off_the_answer);
    RUN(an_ihu_round_of_hundreds_of_frames_is_told_whole);
    RUN(a_queued_message_is_neither_repeated_nor_given_up);
    RUN(a_full_ihu_list_leaves_room_for_a_route);
    RUN(a_node_announces_within_its_promise);
    RUN(a_retry_taken_back_still_ends_in_an_answer_or_giving_up);
    RUN(a_node_held_back_by_its_cap_announces_within_its_promise);
    RUN(an_ihu_round_longer_than_the_timeout_keeps_the_links);
    RUN(a_retry_still_queued_when_the_answer_comes_is_taken_back);
    RUN(a_leaf_never_forwards);
    RUN(with_parents_relays_route_among_themselves);
    RUN(a_leaf_with_no_parent_is_not_asked_about);
    RUN(trickle_backs_off_and_resets_on_a_new_neighbour);
    RUN(announces_stay_under_the_cap);
    RUN(a_broken_link_is_found_by_the_data_crossing_it);
    RUN(a_lost_hop_makes_no_route_dearer);
    RUN(a_link_lost_and_the_routes_across_it_are_booked);
    RUN(seq_period_raises_the_seq_unasked);
    RUN(a_lost_message_is_booked_where_it_was_lost);
    RUN(routes_stay_loop_free_while_links_change);
    RUN(a_broadcast_reaches_the_line_once_per_relay);
    RUN(the_config_is_checked);
    return CHECK_DONE();
}
