#include "tsim/baseline.h"

#include "check.h"

enum { MAX_NODES = 5 };

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
};

static struct tsim_flood_config flood_config(uint8_t hops) {
    return (struct tsim_flood_config){
        .channel = 0,
        .lora = tsim_lora_default(7, 125000),
        .tx_dbm = 14.0,
        .hops = hops,
    };
}

static void rig_open(struct rig *r, uint32_t nodes, uint8_t hops, tsim_time max_delay,
                     uint64_t seed) {
    struct tsim_net_params p = tsim_net_defaults(seed);
    struct tsim_flood_config fc = flood_config(hops);
    struct tsim_aloha_config ac = {max_delay};
    r->sched = tsim_sched_create();
    r->net = tsim_net_create(r->sched, &p, nodes, &tsim_flood, &fc, &tsim_aloha, &ac);
}

static void rig_close(struct rig *r) {
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

/* Nodes in a row, each hearing only its neighbours. */
static void line(struct rig *r, uint32_t nodes, uint8_t hops) {
    rig_open(r, nodes, hops, 0, 1);
    for (uint32_t i = 0; i + 1 < nodes; i++) {
        tsim_phy_set_loss(tsim_net_phy(r->net), i, i + 1, 100.0);
    }
}

static tsim_time frame_airtime(uint32_t payload) {
    struct tsim_lora lora = tsim_lora_default(7, 125000);
    return tsim_lora_airtime(&lora, TSIM_FLOOD_HEADER + payload);
}

static uint64_t frames(const struct rig *r, uint32_t node, enum tsim_purpose purpose) {
    return tsim_net_ledger(r->net, node)->frames[purpose];
}

static void flood_crosses_a_line(void) {
    struct rig r;
    line(&r, 5, 3);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    const struct tsim_message_record *m = tsim_net_message(r.net, b);
    CHECK(m->delivered == 4 && m->wanted == 4);
    CHECK_EQ_I64(m->first, frame_airtime(10));
    CHECK_EQ_I64(m->last, 4 * frame_airtime(10));

    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_RELAY), 0);
    for (uint32_t i = 1; i <= 3; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
        CHECK_EQ_I64(tsim_ledger_airtime(tsim_net_ledger(r.net, i)), frame_airtime(10));
    }
    CHECK_EQ_I64(tsim_ledger_airtime(tsim_net_ledger(r.net, 4)), 0); /* no hops left */
    rig_close(&r);
}

static void hop_limit_stops_the_flood(void) {
    struct rig r;
    line(&r, 5, 2);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    CHECK(tsim_net_message(r.net, b)->delivered == 3);
    CHECK_EQ_U64(frames(&r, 3, TSIM_PURPOSE_RELAY), 0);
    CHECK_EQ_U64(tsim_net_stats(r.net, 4)->delivered, 0);
    rig_close(&r);
}

static void unicast_stops_at_its_destination(void) {
    struct rig r;
    line(&r, 5, 3);
    uint64_t u = tsim_net_originate(r.net, 0, 2, 10);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    CHECK(tsim_net_message(r.net, u)->delivered == 1);
    CHECK_EQ_U64(tsim_net_stats(r.net, 2)->delivered, 1);
    CHECK_EQ_U64(tsim_net_stats(r.net, 1)->delivered, 0); /* relayed it, did not keep it */
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 1);
    CHECK_EQ_I64(tsim_ledger_airtime(tsim_net_ledger(r.net, 2)), 0);
    CHECK_EQ_I64(tsim_ledger_airtime(tsim_net_ledger(r.net, 3)), 0);
    rig_close(&r);
}

/* 0 reaches 1 and 2, which both reach 3. */
static void diamond(struct rig *r, tsim_time max_delay, uint64_t seed) {
    rig_open(r, 4, 3, max_delay, seed);
    struct tsim_phy *phy = tsim_net_phy(r->net);
    tsim_phy_set_loss(phy, 0, 1, 100.0);
    tsim_phy_set_loss(phy, 0, 2, 100.0);
    tsim_phy_set_loss(phy, 1, 3, 100.0);
    tsim_phy_set_loss(phy, 2, 3, 100.0);
}

/* Without a random start, 1 and 2 relay at the same instant and 3 hears neither - but both are
 * still charged for the airtime. */
static void relays_at_the_same_instant_collide(void) {
    struct rig r;
    diamond(&r, 0, 1);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    CHECK(tsim_net_message(r.net, b)->delivered == 2);
    CHECK_EQ_U64(tsim_net_stats(r.net, 3)->delivered, 0);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 1);
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_RELAY), 1);
    CHECK_EQ_U64(tsim_phy_stats(tsim_net_phy(r.net), 3)->rx_lost, 1);
    rig_close(&r);
}

/* A random start over 2 s, against a 56 ms frame, separates them nearly every time; and a node
 * that hears the message twice relays it once. */
static void a_random_start_separates_them(void) {
    int reached = 0;
    for (uint64_t seed = 1; seed <= 20; seed++) {
        struct rig r;
        diamond(&r, TSIM_S(2), seed);
        tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
        tsim_sched_run_until(r.sched, TSIM_S(30));
        reached += (int)tsim_net_stats(r.net, 3)->delivered;
        CHECK(frames(&r, 3, TSIM_PURPOSE_RELAY) <= 1);
        CHECK(tsim_net_stats(r.net, 3)->delivered <= 1);
        rig_close(&r);
    }
    CHECK(reached >= 16);
}

static void a_seed_repeats_a_run(void) {
    tsim_time airtime[2][4];
    tsim_time last[2];
    for (int k = 0; k < 2; k++) {
        struct rig r;
        diamond(&r, TSIM_S(2), 42);
        uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
        tsim_sched_run_until(r.sched, TSIM_S(30));
        for (uint32_t i = 0; i < 4; i++) {
            airtime[k][i] = tsim_ledger_airtime(tsim_net_ledger(r.net, i));
        }
        last[k] = tsim_net_message(r.net, b)->last;
        rig_close(&r);
    }
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_I64(airtime[0][i], airtime[1][i]);
    }
    CHECK_EQ_I64(last[0], last[1]);
}

/* Pending random starts and frames on the air both hold events that point into the network. */
static void destroy_mid_flood_leaves_the_scheduler_runnable(void) {
    struct rig r;
    rig_open(&r, MAX_NODES, 3, TSIM_S(1), 7);
    for (uint32_t i = 0; i + 1 < MAX_NODES; i++) {
        tsim_phy_set_loss(tsim_net_phy(r.net), i, i + 1, 100.0);
    }
    for (uint32_t i = 0; i < MAX_NODES; i++) {
        tsim_net_originate(r.net, i, TSIM_BROADCAST, 10);
    }
    tsim_sched_run_until(r.sched, TSIM_MS(700));
    tsim_net_destroy(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(20));
    CHECK(tsim_sched_size(r.sched) == 0);
    tsim_sched_destroy(r.sched);
}

int main(void) {
    RUN(flood_crosses_a_line);
    RUN(hop_limit_stops_the_flood);
    RUN(unicast_stops_at_its_destination);
    RUN(relays_at_the_same_instant_collide);
    RUN(a_random_start_separates_them);
    RUN(a_seed_repeats_a_run);
    RUN(destroy_mid_flood_leaves_the_scheduler_runnable);
    return CHECK_DONE();
}
