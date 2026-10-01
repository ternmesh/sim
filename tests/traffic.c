#include "tsim/traffic.h"

#include <stdlib.h>

#include "tsim/baseline.h"

#include "check.h"

/* A routing that takes nothing, to set beside one that floods. */
static int refuser_instance;

static void *refuser_create(struct tsim_node *node, const void *config) {
    (void)node;
    (void)config;
    return &refuser_instance;
}

static void refuser_destroy(void *self) { (void)self; }

static bool refuser_originate(void *self, const struct tsim_message *msg) {
    (void)self;
    (void)msg;
    return false;
}

static void refuser_rx(void *self, const struct tsim_rx *rx) {
    (void)self;
    (void)rx;
}

static const struct tsim_routing refuser = {
    .name = "refuser",
    .create = refuser_create,
    .destroy = refuser_destroy,
    .originate = refuser_originate,
    .rx = refuser_rx,
};

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct tsim_traffic *traffic;
};

static const struct tsim_flood_config flood = {
    .channel = 0,
    .lora = {.sf = 7, .bw_hz = 125000, .cr = 1, .preamble = 8, .crc = true},
    .tx_dbm = 14,
    .hops = 3,
};
static const struct tsim_aloha_config aloha = {TSIM_MS(500)};

/* `nodes` nodes that all hear each other when `linked`, running `routing`. */
static void rig_open(struct rig *r, uint32_t nodes, const struct tsim_routing *routing, bool linked,
                     const struct tsim_traffic_params *p) {
    struct tsim_net_params np = tsim_net_defaults(p->seed);
    r->sched = tsim_sched_create();
    r->net = tsim_net_create(r->sched, &np, nodes, routing, &flood, &tsim_aloha, &aloha);
    for (uint32_t a = 0; linked && a < nodes; a++) {
        for (uint32_t b = a + 1; b < nodes; b++) {
            tsim_phy_set_loss(tsim_net_phy(r->net), a, b, 100.0);
        }
    }
    tsim_net_start(r->net);
    r->traffic = tsim_traffic_create(r->net, p);
}

static void rig_close(struct rig *r) {
    tsim_traffic_destroy(r->traffic);
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static struct tsim_traffic_params params(void) {
    return (struct tsim_traffic_params){
        .interval = TSIM_S(60),
        .len_min = 10,
        .len_max = 12,
        .broadcast = 0.5,
        .start = 0,
        .stop = TSIM_S(3600),
        .seed = 1,
    };
}

/* 50 nodes for 10000 s at one message a minute each: 8333 expected, with a standard deviation of
 * about 91. */
static void messages_come_at_the_configured_rate(void) {
    struct tsim_traffic_params p = params();
    p.stop = TSIM_S(10000);
    struct rig r;
    rig_open(&r, 50, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    uint64_t made = tsim_traffic_made(r.traffic);
    CHECK(made > 8333 - 400 && made < 8333 + 400);
    CHECK_EQ_U64(tsim_net_message_count(r.net), made);
    rig_close(&r);
}

static void messages_are_made_only_inside_the_window(void) {
    struct tsim_traffic_params p = params();
    p.interval = TSIM_S(5);
    p.start = TSIM_S(100);
    p.stop = TSIM_S(200);
    struct rig r;
    rig_open(&r, 4, &refuser, false, &p);
    tsim_sched_run_until(r.sched, TSIM_S(1000));
    uint64_t count = tsim_net_message_count(r.net);
    CHECK(count > 40);
    for (uint64_t id = 1; id <= count; id++) {
        tsim_time t = tsim_net_message(r.net, id)->msg.created;
        CHECK(t > p.start && t < p.stop);
    }
    CHECK_EQ_U64(tsim_traffic_made(r.traffic), count);
    rig_close(&r);
}

/* At an interval of a nanosecond most gaps round to nothing; none may, or a node would send two
 * messages at one instant, and its first at the start itself. */
static void no_two_of_a_nodes_messages_share_an_instant(void) {
    enum { N = 20 };
    struct tsim_traffic_params p = params();
    p.interval = TSIM_NS(1);
    p.start = TSIM_NS(100);
    p.stop = TSIM_NS(200);
    struct rig r;
    rig_open(&r, N, &refuser, false, &p);
    tsim_sched_run_until(r.sched, TSIM_NS(300));
    tsim_time last[N];
    for (int i = 0; i < N; i++) {
        last[i] = p.start;
    }
    uint64_t count = tsim_net_message_count(r.net);
    CHECK(count > N);
    CHECK(count <= N * 99);
    for (uint64_t id = 1; id <= count; id++) {
        const struct tsim_message *m = &tsim_net_message(r.net, id)->msg;
        CHECK(m->created > last[m->src]);
        last[m->src] = m->created;
    }
    rig_close(&r);
}

/* At a mean of 200 years over a window of 170, a node sends at all with probability
 * 1 - e^-0.85, about 57%. A gap is never cut short to fit the window: one past it stays past it. */
static void a_gap_longer_than_the_window_stays_longer(void) {
    enum { N = 1000 };
    const tsim_time year = TSIM_S(365 * 24 * 3600);
    struct tsim_traffic_params p = params();
    p.interval = 200 * year;
    p.stop = 170 * year;
    struct rig r;
    rig_open(&r, N, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    bool sent[N] = {false};
    uint64_t count = tsim_net_message_count(r.net);
    for (uint64_t id = 1; id <= count; id++) {
        sent[tsim_net_message(r.net, id)->msg.src] = true;
    }
    int senders = 0;
    for (int i = 0; i < N; i++) {
        senders += sent[i];
    }
    /* 573 expected, with a standard deviation of about 16. */
    CHECK(senders > 500 && senders < 650);
    rig_close(&r);
}

static void destinations_and_lengths_cover_their_ranges(void) {
    enum { N = 5 };
    struct tsim_traffic_params p = params();
    p.broadcast = 0.0;
    struct rig r;
    rig_open(&r, N, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    bool pair[N][N] = {{false}};
    bool len[3] = {false};
    uint64_t count = tsim_net_message_count(r.net);
    for (uint64_t id = 1; id <= count; id++) {
        const struct tsim_message *m = &tsim_net_message(r.net, id)->msg;
        CHECK(m->dst != TSIM_BROADCAST && m->dst < N && m->dst != m->src);
        CHECK(m->len >= 10 && m->len <= 12);
        if (m->dst < N && m->len >= 10 && m->len <= 12) {
            pair[m->src][m->dst] = true;
            len[m->len - 10] = true;
        }
    }
    for (uint32_t a = 0; a < N; a++) {
        for (uint32_t b = 0; b < N; b++) {
            CHECK(pair[a][b] == (a != b));
        }
    }
    CHECK(len[0] && len[1] && len[2]);
    rig_close(&r);

    p.broadcast = 1.0;
    rig_open(&r, N, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    count = tsim_net_message_count(r.net);
    CHECK(count > 0);
    for (uint64_t id = 1; id <= count; id++) {
        CHECK(tsim_net_message(r.net, id)->msg.dst == TSIM_BROADCAST);
    }
    rig_close(&r);
}

/* Flooding over linked nodes and refusing everything over unlinked ones are as different as two
 * runs get; with one seed they are still offered the same messages, at the same times. */
static void every_protocol_is_offered_the_same_messages(void) {
    struct tsim_traffic_params p = params();
    p.len_min = 230; /* the flood refuses anything over 242, so some of these too */
    p.len_max = 255;
    struct rig a, b;
    rig_open(&a, 6, &tsim_flood, true, &p);
    rig_open(&b, 6, &refuser, false, &p);
    tsim_sched_run_until(a.sched, p.stop);
    tsim_sched_run_until(b.sched, p.stop);
    uint64_t count = tsim_net_message_count(a.net);
    CHECK(count > 100);
    CHECK_EQ_U64(tsim_net_message_count(b.net), count);
    uint64_t relayed = 0;
    for (uint32_t i = 0; i < 6; i++) {
        relayed += tsim_net_ledger(a.net, i)->frames[TSIM_PURPOSE_RELAY];
    }
    CHECK(relayed > 0);
    for (uint64_t id = 1; id <= count && id <= tsim_net_message_count(b.net); id++) {
        const struct tsim_message *x = &tsim_net_message(a.net, id)->msg;
        const struct tsim_message *y = &tsim_net_message(b.net, id)->msg;
        CHECK(x->src == y->src && x->dst == y->dst && x->len == y->len);
        CHECK_EQ_I64(x->created, y->created);
    }
    rig_close(&a);
    rig_close(&b);
}

static void invalid_parameters_are_refused(void) {
    struct tsim_sched *sched = tsim_sched_create();
    struct tsim_net_params np = tsim_net_defaults(1);
    struct tsim_net *net = tsim_net_create(sched, &np, 3, &refuser, NULL, &tsim_aloha, &aloha);
    struct tsim_traffic_params p;

    p = params(), p.interval = 0;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.len_min = 20, p.len_max = 19;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.len_max = TSIM_FRAME_MAX + 1;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.broadcast = -0.1;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.broadcast = 1.1;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.start = TSIM_S(10), p.stop = TSIM_S(9);
    CHECK(tsim_traffic_create(net, &p) == NULL);

    tsim_net_destroy(net);
    tsim_sched_destroy(sched);
}

static void a_lone_node_sends_nothing(void) {
    struct tsim_traffic_params p = params();
    struct rig r;
    rig_open(&r, 1, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    CHECK_EQ_U64(tsim_traffic_made(r.traffic), 0);
    CHECK_EQ_U64(tsim_net_message_count(r.net), 0);
    rig_close(&r);
}

static void destroy_stops_the_traffic(void) {
    struct tsim_traffic_params p = params();
    struct rig r;
    rig_open(&r, 10, &refuser, false, &p);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    uint64_t made = tsim_net_message_count(r.net);
    CHECK(made > 0);
    tsim_traffic_destroy(r.traffic);
    r.traffic = NULL;
    tsim_sched_run_until(r.sched, p.stop);
    CHECK_EQ_U64(tsim_net_message_count(r.net), made);
    CHECK(tsim_sched_size(r.sched) == 0);
    rig_close(&r);
}

int main(void) {
    RUN(messages_come_at_the_configured_rate);
    RUN(messages_are_made_only_inside_the_window);
    RUN(no_two_of_a_nodes_messages_share_an_instant);
    RUN(a_gap_longer_than_the_window_stays_longer);
    RUN(destinations_and_lengths_cover_their_ranges);
    RUN(every_protocol_is_offered_the_same_messages);
    RUN(invalid_parameters_are_refused);
    RUN(a_lone_node_sends_nothing);
    RUN(destroy_stops_the_traffic);
    return CHECK_DONE();
}
