#include "tsim/metrics.h"

#include "tsim/baseline.h"
#include "tsim/phy.h"

#include "check.h"

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct tsim_metrics *metrics;
};

/* Nodes in a row, each hearing only its neighbours, flooding with no random start, so every
 * delivery lands at a multiple of one frame's airtime. */
static void line(struct rig *r, uint32_t nodes, tsim_time max_delay, tsim_time deadline,
                 uint64_t seed) {
    struct tsim_net_params p = tsim_net_defaults(seed);
    struct tsim_flood_config fc = {
        .channel = 0, .lora = tsim_lora_default(7, 125000), .tx_dbm = 14.0, .hops = 10};
    struct tsim_aloha_config ac = {max_delay};
    r->sched = tsim_sched_create();
    r->net = tsim_net_create(r->sched, &p, nodes, &tsim_flood, &fc, &tsim_aloha, &ac);
    for (uint32_t i = 0; i + 1 < nodes; i++) {
        tsim_phy_set_loss(tsim_net_phy(r->net), i, i + 1, 100.0);
    }
    tsim_net_start(r->net);
    r->metrics = tsim_metrics_create(r->net, deadline);
}

static void rig_close(struct rig *r) {
    tsim_metrics_destroy(r->metrics);
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static tsim_time frame_airtime(uint32_t payload) {
    struct tsim_lora lora = tsim_lora_default(7, 125000);
    return tsim_lora_airtime(&lora, TSIM_FLOOD_HEADER + payload);
}

/* A percentile is the floor of its bucket: at most 1/64 under the true value, never over it. */
static bool near_below(tsim_time got, tsim_time want) {
    return got <= want && got >= want - want / 64;
}

/* Node 0's broadcast reaches 1, 2, 3 and 4 at one, two, three and four frame times. */
static void latency_and_deadline_follow_each_delivery(void) {
    const tsim_time a = frame_airtime(10);
    struct rig r;
    line(&r, 5, 0, 2 * a + a / 2, 1);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    struct tsim_report rep;
    tsim_metrics_report(r.metrics, &rep);
    CHECK_EQ_U64(rep.broadcast.messages, 1);
    CHECK_EQ_U64(rep.broadcast.wanted, 4);
    CHECK_EQ_U64(rep.broadcast.delivered, 4);
    CHECK_EQ_U64(rep.broadcast.on_time, 2);
    CHECK(near_below(rep.broadcast.latency_p50, 2 * a)); /* the 2nd of 4 */
    CHECK(near_below(rep.broadcast.latency_p95, 4 * a)); /* the 4th of 4 */
    CHECK_EQ_I64(rep.broadcast.latency_max, 4 * a);
    CHECK_EQ_U64(rep.unicast.messages, 0);
    CHECK_EQ_I64(rep.unicast.latency_p50, 0);
    CHECK_EQ_I64(rep.elapsed, TSIM_S(10));
    CHECK_EQ_I64(rep.deadline, 2 * a + a / 2);
    rig_close(&r);
}

/* A refused message and an unreachable one stay in the denominator, each in its own kind. */
static void unicast_and_broadcast_are_counted_apart(void) {
    struct rig r;
    line(&r, 3, 0, TSIM_S(5), 1);
    tsim_net_originate(r.net, 0, 2, 10);
    tsim_net_originate(r.net, 0, 1, 250); /* over what one flood frame carries */
    tsim_sched_run_until(r.sched, TSIM_S(5));
    tsim_net_originate(r.net, 2, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    struct tsim_report rep;
    tsim_metrics_report(r.metrics, &rep);
    CHECK_EQ_U64(rep.unicast.messages, 2);
    CHECK_EQ_U64(rep.unicast.refused, 1);
    CHECK_EQ_U64(rep.unicast.wanted, 2);
    CHECK_EQ_U64(rep.unicast.delivered, 1);
    CHECK_EQ_U64(rep.unicast.on_time, 1);
    CHECK_EQ_I64(rep.unicast.latency_max, 2 * frame_airtime(10));
    CHECK_EQ_U64(rep.broadcast.messages, 1);
    CHECK_EQ_U64(rep.broadcast.refused, 0);
    CHECK_EQ_U64(rep.broadcast.wanted, 2);
    CHECK_EQ_U64(rep.broadcast.delivered, 2);
    rig_close(&r);
}

/* The books add up to the ledgers, and the headline is on-time deliveries over their airtime. */
static void airtime_and_duty_add_up_from_the_ledgers(void) {
    const tsim_time a = frame_airtime(10);
    struct rig r;
    line(&r, 4, 0, TSIM_S(5), 1);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(5));
    tsim_net_originate(r.net, 1, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    struct tsim_report rep;
    tsim_metrics_report(r.metrics, &rep);
    /* 0's: sent by 0, relayed by 1 and 2, and 3 has no one left to reach but still relays. 1's:
     * sent by 1, relayed by 0 and 2, and by 3. */
    CHECK_EQ_U64(rep.frames[TSIM_PURPOSE_DATA], 2);
    CHECK_EQ_U64(rep.frames[TSIM_PURPOSE_RELAY], 6);
    CHECK(rep.airtime_s[TSIM_PURPOSE_DATA] == (double)(2 * a) / 1e9);
    CHECK(rep.airtime_total_s == (double)(8 * a) / 1e9);
    /* Every node sent two frames - 0 and 1 one of each, 2 and 3 two relays - so the busiest is a
     * tie, and the first of it is named. */
    CHECK(rep.duty_max_node == 0);
    CHECK(rep.duty_max == (double)(2 * a) / (double)TSIM_S(10));
    CHECK(rep.duty_mean == (double)(8 * a) / 4.0 / (double)TSIM_S(10));
    CHECK_EQ_U64(rep.broadcast.on_time, 6);
    CHECK(rep.on_time_per_airtime_s == 6.0 / ((double)(8 * a) / 1e9));
    CHECK(rep.rx_ok >= 6);
    rig_close(&r);
}

/* A report cut off mid-frame counts only the airtime that has gone by: the ledger charged the
 * whole frame when it went on the air, and the rest of it falls after the cutoff. */
static void a_report_mid_frame_counts_only_what_was_sent(void) {
    const tsim_time a = frame_airtime(10);
    struct rig r;
    line(&r, 2, 0, TSIM_S(5), 1);
    tsim_net_originate(r.net, 0, 1, 10);
    tsim_sched_run_until(r.sched, a / 2);
    struct tsim_report rep;
    tsim_metrics_report(r.metrics, &rep);
    CHECK_EQ_I64(tsim_net_ledger(r.net, 0)->airtime[TSIM_PURPOSE_DATA], a);
    CHECK_EQ_U64(rep.frames[TSIM_PURPOSE_DATA], 1);
    CHECK(rep.airtime_s[TSIM_PURPOSE_DATA] == (double)(a / 2) / 1e9);
    CHECK(rep.airtime_total_s == (double)(a / 2) / 1e9);
    CHECK(rep.duty_max == 1.0); /* on the air the whole run so far, and no more */
    CHECK_EQ_U64(rep.unicast.delivered, 0);
    CHECK(rep.on_time_per_airtime_s == 0);

    tsim_sched_run_until(r.sched, TSIM_S(1));
    tsim_metrics_report(r.metrics, &rep);
    CHECK(rep.airtime_total_s == (double)a / 1e9);
    CHECK_EQ_U64(rep.unicast.delivered, 1);
    rig_close(&r);
}

/* One delivery, of a latency anywhere up to 100 s: the percentiles are its bucket's floor and the
 * longest is exact, at every scale the histogram covers. */
static void percentiles_hold_their_precision_at_every_scale(void) {
    for (uint64_t seed = 1; seed <= 20; seed++) {
        struct rig r;
        line(&r, 2, TSIM_S(100), TSIM_S(1000), seed);
        uint64_t id = tsim_net_originate(r.net, 0, 1, 10);
        tsim_sched_run_until(r.sched, TSIM_S(200));
        const struct tsim_message_record *m = tsim_net_message(r.net, id);
        struct tsim_report rep;
        tsim_metrics_report(r.metrics, &rep);
        CHECK(m->delivered == 1);
        CHECK(near_below(rep.unicast.latency_p50, m->first));
        CHECK(near_below(rep.unicast.latency_p95, m->first));
        CHECK_EQ_I64(rep.unicast.latency_max, m->first);
        rig_close(&r);
    }
}

/* A message made before the window is left out of it, its deliveries after the window began as
 * much as those before: only the one made after counts. */
static void a_message_before_the_window_is_not_counted(void) {
    const tsim_time a = frame_airtime(10);
    struct rig r;
    line(&r, 5, 0, TSIM_S(5), 1);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, 2 * a + a / 2); /* nodes 1 and 2 have it; 3 and 4 are to come */
    tsim_metrics_begin(r.metrics);
    tsim_sched_run_until(r.sched, TSIM_S(10));
    tsim_net_originate(r.net, 4, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(20));
    struct tsim_report rep;
    tsim_metrics_report(r.metrics, &rep);
    CHECK_EQ_U64(rep.broadcast.messages, 1);
    CHECK_EQ_U64(rep.broadcast.wanted, 4);
    CHECK_EQ_U64(rep.broadcast.delivered, 4);
    CHECK_EQ_U64(rep.broadcast.on_time, 4);
    CHECK_EQ_I64(rep.broadcast.latency_max, 4 * a); /* the second's own, from node 4 to node 0 */
    CHECK(near_below(rep.broadcast.latency_p50, 2 * a));
    rig_close(&r);
}

/* Destroying the metrics stops the network calling into them. */
static void destroy_stops_watching(void) {
    struct rig r;
    line(&r, 3, 0, TSIM_S(5), 1);
    tsim_metrics_destroy(r.metrics);
    r.metrics = NULL;
    uint64_t id = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(5));
    CHECK(tsim_net_message(r.net, id)->delivered == 2);
    rig_close(&r);
}

int main(void) {
    RUN(latency_and_deadline_follow_each_delivery);
    RUN(unicast_and_broadcast_are_counted_apart);
    RUN(airtime_and_duty_add_up_from_the_ledgers);
    RUN(a_report_mid_frame_counts_only_what_was_sent);
    RUN(percentiles_hold_their_precision_at_every_scale);
    RUN(a_message_before_the_window_is_not_counted);
    RUN(destroy_stops_watching);
    return CHECK_DONE();
}
