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

/* A routing that holds every message until the test finishes it. */
static bool holder_originate(void *self, const struct tsim_message *msg) {
    (void)self;
    (void)msg;
    return true;
}

static const struct tsim_routing holder = {
    .name = "holder",
    .create = refuser_create,
    .destroy = refuser_destroy,
    .originate = holder_originate,
    .rx = refuser_rx,
    .reports_finished = true,
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

/* Closed, a node makes nothing new until its routing is done with the last, and its next gap is
 * drawn from then. */
static void closed_traffic_waits_for_the_last_message(void) {
    struct tsim_traffic_params p = params();
    p.closed = true;
    struct rig r;
    rig_open(&r, 2, &holder, false, &p);
    tsim_sched_run_until(r.sched, TSIM_S(1800));
    CHECK_EQ_U64(tsim_traffic_made(r.traffic), 2); /* one each, held */
    uint64_t first = tsim_net_message(r.net, 1)->msg.src == 0 ? 1 : 2;
    CHECK(tsim_node_finished(tsim_net_node(r.net, 0), first));
    tsim_sched_run_until(r.sched, TSIM_S(3600));
    CHECK_EQ_U64(tsim_traffic_made(r.traffic), 3);
    const struct tsim_message_record *next = tsim_net_message(r.net, 3);
    CHECK(next->msg.src == 0 && next->msg.created > TSIM_S(1800));
    rig_close(&r);
}

/* With a routing that is done with each message at once, closed traffic is open traffic: the same
 * draws in the same order, so the same messages at the same times. */
static void closed_traffic_with_nothing_to_wait_for_is_open_traffic(void) {
    struct tsim_traffic_params p = params();
    struct rig open;
    rig_open(&open, 5, &refuser, false, &p);
    tsim_sched_run_until(open.sched, p.stop);
    p.closed = true;
    struct rig closed;
    rig_open(&closed, 5, &refuser, false, &p);
    tsim_sched_run_until(closed.sched, p.stop);
    uint64_t made = tsim_traffic_made(open.traffic);
    CHECK(made > 100);
    CHECK_EQ_U64(tsim_traffic_made(closed.traffic), made);
    for (uint64_t id = 1; id <= made; id++) {
        const struct tsim_message *a = &tsim_net_message(open.net, id)->msg;
        const struct tsim_message *b = &tsim_net_message(closed.net, id)->msg;
        CHECK(a->src == b->src && a->dst == b->dst && a->len == b->len && a->created == b->created);
    }
    rig_close(&open);
    rig_close(&closed);
}

static void invalid_parameters_are_refused(void) {
    struct tsim_sched *sched = tsim_sched_create();
    struct tsim_net_params np = tsim_net_defaults(1);
    struct tsim_net *net = tsim_net_create(sched, &np, 3, &refuser, NULL, &tsim_aloha, &aloha);
    struct tsim_traffic_params p;

    p = params(), p.interval = -1;
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
    struct tsim_send bad[] = {
        {0, 3, 1, 10}, /* from a node that is not there */
        {0, 0, 3, 10}, /* to one that is not there */
        {0, 1, 1, 10}, /* to itself */
        {0, 0, 1, TSIM_FRAME_MAX + 1},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        p = params(), p.sends = &bad[i], p.send_count = 1;
        CHECK(tsim_traffic_create(net, &p) == NULL);
    }
    p = params(), p.send_count = 1;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.peers = 3; /* only two others */
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.reply = -0.1;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.reply = 1.1;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.reply = 0.5, p.closed = true;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.reply_delay = -1;
    CHECK(tsim_traffic_create(net, &p) == NULL);
    p = params(), p.peers = 2, p.reply = 1, p.closed = false;
    struct tsim_traffic *ok = tsim_traffic_create(net, &p);
    CHECK(ok != NULL);
    tsim_traffic_destroy(ok);

    tsim_net_destroy(net);
    tsim_sched_destroy(sched);
}

static void sends_are_made_when_set_and_nothing_else_without_an_interval(void) {
    struct tsim_send sends[] = {
        {TSIM_S(10), 1, TSIM_BROADCAST, 20},
        {TSIM_S(5), 0, 2, 7},
    };
    struct tsim_traffic_params p = params();
    p.interval = 0;
    p.sends = sends;
    p.send_count = 2;
    struct rig r;
    rig_open(&r, 3, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    CHECK_EQ_U64(tsim_traffic_made(r.traffic), 2);
    CHECK_EQ_U64(tsim_net_message_count(r.net), 2);
    const struct tsim_message *a = &tsim_net_message(r.net, 1)->msg;
    const struct tsim_message *b = &tsim_net_message(r.net, 2)->msg;
    CHECK(a->src == 0 && a->dst == 2 && a->len == 7 && a->created == TSIM_S(5));
    CHECK(b->src == 1 && b->dst == TSIM_BROADCAST && b->len == 20 && b->created == TSIM_S(10));
    rig_close(&r);
}

/* The process's messages are the same with a send among them or without. */
static void a_send_moves_none_of_the_process_draws(void) {
    struct tsim_traffic_params p = params();
    struct rig plain;
    rig_open(&plain, 5, &refuser, false, &p);
    tsim_sched_run_until(plain.sched, p.stop);
    struct tsim_send send = {TSIM_S(1000), 3, TSIM_BROADCAST, 40};
    p.sends = &send;
    p.send_count = 1;
    struct rig with;
    rig_open(&with, 5, &refuser, false, &p);
    tsim_sched_run_until(with.sched, p.stop);
    uint64_t made = tsim_traffic_made(plain.traffic);
    CHECK_EQ_U64(tsim_traffic_made(with.traffic), made + 1);
    uint64_t j = 1;
    for (uint64_t i = 1; i <= made; i++, j++) {
        const struct tsim_message *b = &tsim_net_message(with.net, j)->msg;
        if (b->created == TSIM_S(1000) && b->len == 40 && b->src == 3) {
            b = &tsim_net_message(with.net, ++j)->msg;
        }
        const struct tsim_message *a = &tsim_net_message(plain.net, i)->msg;
        CHECK(a->src == b->src && a->dst == b->dst && a->len == b->len && a->created == b->created);
    }
    rig_close(&plain);
    rig_close(&with);
}

/* In the closed loop a node's gap waits on its own messages, not on one it was told to send. */
static void a_send_finishing_starts_no_gap(void) {
    struct tsim_send send = {TSIM_S(1), 0, 1, 10};
    struct tsim_traffic_params p = params();
    p.closed = true;
    p.sends = &send;
    p.send_count = 1;
    struct rig r;
    rig_open(&r, 2, &holder, false, &p);
    tsim_sched_run_until(r.sched, TSIM_S(1800));
    CHECK_EQ_U64(tsim_traffic_made(r.traffic), 3); /* the send, and one each, held */
    CHECK(tsim_net_message(r.net, 1)->msg.created == TSIM_S(1));
    CHECK(tsim_node_finished(tsim_net_node(r.net, 0), 1));
    tsim_sched_run_until(r.sched, TSIM_S(3600));
    CHECK_EQ_U64(tsim_traffic_made(r.traffic), 3);
    rig_close(&r);
}

/* A send the routing refuses is finished inside the call that makes it, before its id is known:
 * still a send, so in the closed loop it starts no gap. The process's messages are those of the
 * same run without it. */
static void a_send_finished_as_it_is_made_starts_no_gap(void) {
    struct tsim_traffic_params p = params();
    p.closed = true;
    struct rig plain;
    rig_open(&plain, 2, &refuser, false, &p);
    tsim_sched_run_until(plain.sched, p.stop);
    struct tsim_send send = {TSIM_S(1), 0, 1, 10};
    p.sends = &send;
    p.send_count = 1;
    struct rig with;
    rig_open(&with, 2, &refuser, false, &p);
    tsim_sched_run_until(with.sched, p.stop);
    uint64_t made = tsim_net_message_count(plain.net);
    CHECK(made > 10);
    CHECK_EQ_U64(tsim_net_message_count(with.net), made + 1);
    rig_close(&plain);
    rig_close(&with);

    /* With no process at all, the send is all there is. */
    p.interval = 0;
    struct rig alone;
    rig_open(&alone, 2, &refuser, false, &p);
    tsim_sched_run_until(alone.sched, p.stop);
    CHECK_EQ_U64(tsim_net_message_count(alone.net), 1);
    CHECK(tsim_sched_size(alone.sched) == 0);
    rig_close(&alone);
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

/* Two picks each, made mutual: a node sends only to its peers, and to each of them, and they send
 * back. 166 messages a node over at most a dozen peers leaves none unused. */
static void peers_are_mutual_and_the_only_destinations(void) {
    enum { N = 40 };
    struct tsim_traffic_params p = params();
    p.broadcast = 0.0;
    p.peers = 2;
    p.stop = TSIM_S(10000);
    struct rig r;
    rig_open(&r, N, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    static bool sent[N][N];
    uint64_t count = tsim_net_message_count(r.net);
    CHECK(count > N * 100);
    for (uint64_t id = 1; id <= count; id++) {
        const struct tsim_message *m = &tsim_net_message(r.net, id)->msg;
        CHECK(m->dst < N && m->dst != m->src);
        if (m->dst < N) {
            sent[m->src][m->dst] = true;
        }
    }
    uint32_t pairs = 0;
    for (uint32_t a = 0; a < N; a++) {
        uint32_t mine = 0;
        for (uint32_t b = 0; b < N; b++) {
            CHECK(sent[a][b] == sent[b][a]);
            mine += sent[a][b];
        }
        CHECK(mine >= 2);
        pairs += mine;
    }
    /* 2 picks each, so 80 mutual pairs, less the picks two nodes made of each other. */
    CHECK(pairs > 2 * 70 && pairs <= 2 * 80);
    rig_close(&r);
}

/* With every unicast answered a millisecond or so later, each message is followed by its answer,
 * from its destination back; the answer is not answered. Half answered, about half are. */
static void unicasts_are_answered_and_answers_are_not(void) {
    struct tsim_traffic_params p = params();
    p.broadcast = 0.0;
    p.reply = 1.0;
    p.reply_delay = TSIM_MS(1);
    struct rig r;
    rig_open(&r, 5, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    uint64_t count = tsim_net_message_count(r.net);
    CHECK(count > 200 && count % 2 == 0);
    for (uint64_t id = 1; id + 1 <= count; id += 2) {
        const struct tsim_message *m = &tsim_net_message(r.net, id)->msg;
        const struct tsim_message *a = &tsim_net_message(r.net, id + 1)->msg;
        CHECK(a->src == m->dst && a->dst == m->src);
        CHECK(a->created >= m->created && a->created - m->created < TSIM_S(1));
    }
    rig_close(&r);

    p.reply = 0.5;
    p.stop = TSIM_S(20000);
    rig_open(&r, 5, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop);
    count = tsim_net_message_count(r.net);
    uint64_t asked = 0, answered = 0;
    for (uint64_t id = 1; id <= count; id++) {
        const struct tsim_message *m = &tsim_net_message(r.net, id)->msg;
        asked++;
        if (id < count) {
            const struct tsim_message *a = &tsim_net_message(r.net, id + 1)->msg;
            if (a->src == m->dst && a->dst == m->src && a->created - m->created < TSIM_S(1)) {
                answered++;
                id++;
            }
        }
    }
    CHECK(asked > 1000);
    CHECK(answered * 100 > asked * 45 && answered * 100 < asked * 55);
    rig_close(&r);
}

/* An answer that would fall at or after stop is not sent. */
static void an_answer_past_stop_is_not_sent(void) {
    struct tsim_traffic_params p = params();
    p.broadcast = 0.0;
    p.reply = 1.0;
    p.reply_delay = TSIM_S(1000000);
    struct rig r;
    rig_open(&r, 5, &refuser, false, &p);
    tsim_sched_run_until(r.sched, p.stop + TSIM_S(10000000));
    uint64_t count = tsim_net_message_count(r.net);
    CHECK(count > 100);
    for (uint64_t id = 1; id <= count; id++) {
        CHECK(tsim_net_message(r.net, id)->msg.created < p.stop);
    }
    CHECK(tsim_sched_size(r.sched) == 0);
    rig_close(&r);
}

/* Answers go whether or not the message arrived, so flooding and refusing are offered the same. */
static void answers_do_not_depend_on_the_protocol(void) {
    struct tsim_traffic_params p = params();
    p.peers = 2;
    p.reply = 0.5;
    p.reply_delay = TSIM_S(20);
    struct rig a, b;
    rig_open(&a, 6, &tsim_flood, true, &p);
    rig_open(&b, 6, &refuser, false, &p);
    tsim_sched_run_until(a.sched, p.stop);
    tsim_sched_run_until(b.sched, p.stop);
    uint64_t count = tsim_net_message_count(a.net);
    CHECK(count > 100);
    CHECK_EQ_U64(tsim_net_message_count(b.net), count);
    for (uint64_t id = 1; id <= count && id <= tsim_net_message_count(b.net); id++) {
        const struct tsim_message *x = &tsim_net_message(a.net, id)->msg;
        const struct tsim_message *y = &tsim_net_message(b.net, id)->msg;
        CHECK(x->src == y->src && x->dst == y->dst && x->len == y->len);
        CHECK_EQ_I64(x->created, y->created);
    }
    rig_close(&a);
    rig_close(&b);
}

/* Destroyed with answers still to come, it sends none of them. */
static void destroy_drops_the_answers_waiting(void) {
    struct tsim_traffic_params p = params();
    p.broadcast = 0.0;
    p.reply = 1.0;
    p.reply_delay = TSIM_S(600);
    struct rig r;
    rig_open(&r, 10, &refuser, false, &p);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    uint64_t made = tsim_net_message_count(r.net);
    CHECK(made > 0);
    tsim_traffic_destroy(r.traffic);
    r.traffic = NULL;
    CHECK(tsim_sched_size(r.sched) == 0);
    tsim_sched_run_until(r.sched, p.stop);
    CHECK_EQ_U64(tsim_net_message_count(r.net), made);
    rig_close(&r);
}

int main(void) {
    RUN(messages_come_at_the_configured_rate);
    RUN(messages_are_made_only_inside_the_window);
    RUN(no_two_of_a_nodes_messages_share_an_instant);
    RUN(a_gap_longer_than_the_window_stays_longer);
    RUN(destinations_and_lengths_cover_their_ranges);
    RUN(every_protocol_is_offered_the_same_messages);
    RUN(closed_traffic_waits_for_the_last_message);
    RUN(closed_traffic_with_nothing_to_wait_for_is_open_traffic);
    RUN(invalid_parameters_are_refused);
    RUN(sends_are_made_when_set_and_nothing_else_without_an_interval);
    RUN(a_send_moves_none_of_the_process_draws);
    RUN(a_send_finishing_starts_no_gap);
    RUN(a_send_finished_as_it_is_made_starts_no_gap);
    RUN(a_lone_node_sends_nothing);
    RUN(destroy_stops_the_traffic);
    RUN(peers_are_mutual_and_the_only_destinations);
    RUN(unicasts_are_answered_and_answers_are_not);
    RUN(an_answer_past_stop_is_not_sent);
    RUN(answers_do_not_depend_on_the_protocol);
    RUN(destroy_drops_the_answers_waiting);
    return CHECK_DONE();
}
