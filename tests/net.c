#include "tsim/net.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* A routing plugin that records what the network tells it, and a MAC that sends only when the
 * test says so, so each test can drive the seam one step at a time. */

enum { MAX_NODES = 4 };

struct log {
    int starts;
    int originated;
    struct tsim_message msg;
    int rx[MAX_NODES];
    uint8_t rx_bytes[TSIM_FRAME_MAX];
    uint32_t rx_len;
    double rx_rssi;
    double rx_snr;
    uint64_t done[MAX_NODES];
    int kicks[MAX_NODES];
    bool eager;
};

struct self {
    struct log *log;
    struct tsim_node *handle;
    uint32_t node;
};

static void *self_create(struct tsim_node *node, const void *config) {
    struct self *s = malloc(sizeof *s);
    if (s) {
        *s = (struct self){(struct log *)config, node, tsim_node_index(node)};
    }
    return s;
}

static void self_destroy(void *self) { free(self); }

static void rec_start(void *self) { ((struct self *)self)->log->starts++; }

static void rec_originate(void *self, const struct tsim_message *msg) {
    struct log *log = ((struct self *)self)->log;
    log->originated++;
    log->msg = *msg;
}

static void rec_rx(void *self, const struct tsim_rx *rx) {
    struct self *s = self;
    s->log->rx[s->node]++;
    memcpy(s->log->rx_bytes, rx->bytes, rx->len);
    s->log->rx_len = rx->len;
    s->log->rx_rssi = rx->rssi_dbm;
    s->log->rx_snr = rx->snr_db;
}

static void rec_tx_done(void *self, uint64_t handle) {
    struct self *s = self;
    s->log->done[s->node] = handle;
}

static const struct tsim_routing recorder = {
    .name = "recorder",
    .create = self_create,
    .destroy = self_destroy,
    .start = rec_start,
    .originate = rec_originate,
    .rx = rec_rx,
    .tx_done = rec_tx_done,
};

/* Sends from inside the kick when the test asks it to. */
static void manual_kick(void *self) {
    struct self *s = self;
    s->log->kicks[s->node]++;
    if (s->log->eager) {
        tsim_node_transmit(s->handle);
    }
}

static const struct tsim_mac manual = {
    .name = "manual",
    .create = self_create,
    .destroy = self_destroy,
    .kick = manual_kick,
};

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct log log;
};

static void rig_open(struct rig *r, struct tsim_net_params params, uint32_t nodes) {
    memset(&r->log, 0, sizeof r->log);
    r->sched = tsim_sched_create();
    r->net = tsim_net_create(r->sched, &params, nodes, &recorder, &r->log, &manual, &r->log);
    tsim_phy_set_loss(tsim_net_phy(r->net), 0, 1, 100.0);
}

static void rig_close(struct rig *r) {
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static void run_for(struct rig *r, tsim_time t) {
    tsim_sched_run_until(r->sched, tsim_sched_now(r->sched) + t);
}

static struct tsim_tx frame(enum tsim_purpose purpose, uint8_t priority, uint32_t len) {
    return (struct tsim_tx){
        .channel = 0,
        .lora = tsim_lora_default(7, 125000),
        .tx_dbm = 14.0,
        .purpose = purpose,
        .priority = priority,
        .len = len,
    };
}

static tsim_time airtime(uint32_t len) {
    struct tsim_lora lora = tsim_lora_default(7, 125000);
    return tsim_lora_airtime(&lora, len);
}

static void originate_hands_the_message_to_its_routing(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 3);
    CHECK(r.log.starts == 3);
    run_for(&r, TSIM_MS(5));
    CHECK_EQ_U64(tsim_net_originate(r.net, 1, 2, 10), 1);
    CHECK(r.log.originated == 1);
    CHECK_EQ_U64(r.log.msg.id, 1);
    CHECK(r.log.msg.src == 1 && r.log.msg.dst == 2 && r.log.msg.len == 10);
    CHECK_EQ_I64(r.log.msg.created, TSIM_MS(5));

    CHECK_EQ_U64(tsim_net_originate(r.net, 1, 1, 10), 0); /* to itself */
    CHECK_EQ_U64(tsim_net_originate(r.net, 3, 1, 10), 0); /* no such node */
    CHECK_EQ_U64(tsim_net_originate(r.net, 0, 3, 10), 0);
    CHECK_EQ_U64(tsim_net_originate(r.net, 0, TSIM_BROADCAST, TSIM_FRAME_MAX + 1), 0);
    CHECK(r.log.originated == 1);
    CHECK_EQ_U64(tsim_net_message_count(r.net), 1);
    rig_close(&r);
}

/* What arrives is the sender's bytes and how they sounded - nothing about who sent them. */
static void a_frame_arrives_as_its_bytes(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 3);
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, 5);
    memcpy(tx.bytes, "hello", 5);
    uint64_t h = tsim_node_send(tsim_net_node(r.net, 0), &tx);
    CHECK(h != 0);
    CHECK(r.log.kicks[0] == 1);
    CHECK(tsim_node_transmit(tsim_net_node(r.net, 0)));
    CHECK(tsim_node_sending(tsim_net_node(r.net, 0)));
    CHECK(tsim_node_queue_length(tsim_net_node(r.net, 0)) == 0);
    run_for(&r, TSIM_S(1));

    CHECK(r.log.rx[1] == 1);
    CHECK(r.log.rx[2] == 0); /* infinite loss */
    CHECK(r.log.rx_len == 5 && memcmp(r.log.rx_bytes, "hello", 5) == 0);
    CHECK(fabs(r.log.rx_rssi - (14.0 - 100.0)) < 1e-9);
    double noise = -174.0 + 10.0 * log10(125000.0) + 6.0;
    CHECK(fabs(r.log.rx_snr - (r.log.rx_rssi - noise)) < 1e-9);

    CHECK(!tsim_node_sending(tsim_net_node(r.net, 0)));
    CHECK_EQ_U64(r.log.done[0], h);
    CHECK(r.log.kicks[0] == 2); /* queued, then done */
    CHECK(r.log.kicks[1] == 1); /* received */
    rig_close(&r);
}

static void ledger_charges_each_frame_under_its_purpose(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct tsim_tx data = frame(TSIM_PURPOSE_DATA, 0, 10);
    struct tsim_tx control = frame(TSIM_PURPOSE_CONTROL, 0, 20);
    struct tsim_tx announce = frame(TSIM_PURPOSE_ANNOUNCE, 0, 30);
    tsim_node_send(tsim_net_node(r.net, 0), &data);
    tsim_node_send(tsim_net_node(r.net, 0), &control);
    uint64_t dropped = tsim_node_send(tsim_net_node(r.net, 0), &announce);

    CHECK(tsim_node_transmit(tsim_net_node(r.net, 0)));
    const struct tsim_ledger *l = tsim_net_ledger(r.net, 0);
    CHECK_EQ_U64(l->frames[TSIM_PURPOSE_DATA], 1);
    CHECK_EQ_I64(l->airtime[TSIM_PURPOSE_DATA], airtime(10));
    CHECK_EQ_U64(l->frames[TSIM_PURPOSE_CONTROL], 0); /* queued is not spent */

    CHECK(!tsim_node_transmit(tsim_net_node(r.net, 0))); /* still sending */
    run_for(&r, TSIM_S(1));
    CHECK(tsim_node_cancel(tsim_net_node(r.net, 0), dropped));
    CHECK(tsim_node_transmit(tsim_net_node(r.net, 0)));
    run_for(&r, TSIM_S(1));

    CHECK_EQ_U64(l->frames[TSIM_PURPOSE_CONTROL], 1);
    CHECK_EQ_I64(l->airtime[TSIM_PURPOSE_CONTROL], airtime(20));
    CHECK_EQ_U64(l->frames[TSIM_PURPOSE_ANNOUNCE], 0);
    CHECK_EQ_I64(tsim_ledger_airtime(l), airtime(10) + airtime(20));
    CHECK_EQ_I64(tsim_ledger_airtime(tsim_net_ledger(r.net, 1)), 0);
    rig_close(&r);
}

static void queue_goes_by_priority_then_order(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    const uint8_t priority[] = {0, 2, 1, 2};
    for (uint32_t i = 0; i < 4; i++) {
        struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, priority[i], i + 1);
        tsim_node_send(tsim_net_node(r.net, 0), &tx);
    }
    const uint32_t expected[] = {2, 4, 3, 1};
    for (int i = 0; i < 4; i++) {
        CHECK(tsim_node_head(tsim_net_node(r.net, 0))->len == expected[i]);
        CHECK(tsim_node_transmit(tsim_net_node(r.net, 0)));
        run_for(&r, TSIM_S(1));
    }
    CHECK(tsim_node_head(tsim_net_node(r.net, 0)) == NULL);
    CHECK(!tsim_node_transmit(tsim_net_node(r.net, 0)));
    rig_close(&r);
}

static void cancel_takes_back_only_what_is_queued(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct tsim_tx tx = frame(TSIM_PURPOSE_RELAY, 0, 10);
    uint64_t sent = tsim_node_send(tsim_net_node(r.net, 0), &tx);
    CHECK(tsim_node_transmit(tsim_net_node(r.net, 0)));
    CHECK(!tsim_node_cancel(tsim_net_node(r.net, 0), sent));
    uint64_t waiting = tsim_node_send(tsim_net_node(r.net, 0), &tx);
    CHECK(!tsim_node_cancel(tsim_net_node(r.net, 1), waiting)); /* another node's */
    CHECK(tsim_node_cancel(tsim_net_node(r.net, 0), waiting));
    CHECK(!tsim_node_cancel(tsim_net_node(r.net, 0), waiting));
    CHECK(tsim_node_queue_length(tsim_net_node(r.net, 0)) == 0);
    const struct tsim_net_stats *s = tsim_net_stats(r.net, 0);
    CHECK_EQ_U64(s->queued, 2);
    CHECK_EQ_U64(s->cancelled, 1);
    rig_close(&r);
}

/* A MAC that sends from inside the kick moves the queue before send returns: here the new frame
 * goes straight out and the waiting one slides into the slot it was queued in. */
static void send_returns_its_own_handle_when_the_mac_sends_at_once(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct tsim_node *n0 = tsim_net_node(r.net, 0);
    struct tsim_tx low = frame(TSIM_PURPOSE_DATA, 0, 10);
    struct tsim_tx high = frame(TSIM_PURPOSE_CONTROL, 1, 20);
    uint64_t waiting = tsim_node_send(n0, &low);
    r.log.eager = true;
    uint64_t sent = tsim_node_send(n0, &high);
    CHECK(sent != waiting);
    CHECK(tsim_node_sending(n0));
    CHECK(tsim_node_head(n0)->len == 10);
    CHECK(!tsim_node_cancel(n0, sent)); /* already on the air */
    r.log.eager = false;
    run_for(&r, TSIM_S(1));
    CHECK_EQ_U64(r.log.done[0], sent);
    CHECK(tsim_node_cancel(n0, waiting));
    rig_close(&r);
}

static void a_full_queue_drops(void) {
    struct tsim_net_params p = tsim_net_defaults(1);
    p.queue_limit = 2;
    struct rig r;
    rig_open(&r, p, 2);
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, 10);
    CHECK(tsim_node_send(tsim_net_node(r.net, 0), &tx) != 0);
    CHECK(tsim_node_send(tsim_net_node(r.net, 0), &tx) != 0);
    CHECK_EQ_U64(tsim_node_send(tsim_net_node(r.net, 0), &tx), 0);
    CHECK(tsim_node_queue_length(tsim_net_node(r.net, 0)) == 2);
    CHECK_EQ_U64(tsim_net_stats(r.net, 0)->dropped, 1);
    /* The frame on the air is not in the queue. */
    CHECK(tsim_node_transmit(tsim_net_node(r.net, 0)));
    CHECK(tsim_node_send(tsim_net_node(r.net, 0), &tx) != 0);
    rig_close(&r);
}

static void send_refuses_what_cannot_go_on_air(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, TSIM_FRAME_MAX + 1);
    CHECK_EQ_U64(tsim_node_send(tsim_net_node(r.net, 0), &tx), 0);
    tx = frame(TSIM_PURPOSE_DATA, 0, 10);
    tx.lora.sf = 13;
    CHECK_EQ_U64(tsim_node_send(tsim_net_node(r.net, 0), &tx), 0);
    tx = frame(TSIM_PURPOSE_COUNT, 0, 10);
    CHECK_EQ_U64(tsim_node_send(tsim_net_node(r.net, 0), &tx), 0);
    tx = frame(TSIM_PURPOSE_DATA, 0, 10);
    CHECK(tsim_net_node(r.net, 2) == NULL);
    CHECK_EQ_U64(tsim_net_stats(r.net, 0)->queued, 0);
    CHECK(r.log.kicks[0] == 0);
    rig_close(&r);
}

static void delivery_counts_each_destination_once(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 4);
    uint64_t u = tsim_net_originate(r.net, 0, 2, 5);
    run_for(&r, TSIM_MS(1));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), u)); /* not its destination */
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 2), u));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 2), u));
    const struct tsim_message_record *m = tsim_net_message(r.net, u);
    CHECK(m->wanted == 1 && m->delivered == 1);
    CHECK_EQ_I64(m->first, TSIM_MS(1));

    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 5);
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 0), b)); /* its source */
    run_for(&r, TSIM_MS(1));
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 1), b));
    run_for(&r, TSIM_MS(1));
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 3), b));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), b));
    m = tsim_net_message(r.net, b);
    CHECK(m->wanted == 3 && m->delivered == 2);
    CHECK_EQ_I64(m->first, TSIM_MS(2));
    CHECK_EQ_I64(m->last, TSIM_MS(3));

    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), 0));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), 3));
    CHECK(tsim_net_message(r.net, 3) == NULL);
    CHECK_EQ_U64(tsim_net_stats(r.net, 1)->delivered, 1);
    CHECK_EQ_U64(tsim_net_stats(r.net, 2)->delivered, 1);
    rig_close(&r);
}

static void streams_are_separate_and_repeatable(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct rig q;
    rig_open(&q, tsim_net_defaults(2), 2);
    struct tsim_rng a, b, c, d, e;
    tsim_node_rng(tsim_net_node(r.net, 0), TSIM_STREAM_MAC, &a);
    tsim_node_rng(tsim_net_node(r.net, 0), TSIM_STREAM_MAC, &b);
    tsim_node_rng(tsim_net_node(r.net, 1), TSIM_STREAM_MAC, &c);
    tsim_node_rng(tsim_net_node(r.net, 0), TSIM_STREAM_ROUTING, &d);
    tsim_node_rng(tsim_net_node(q.net, 0), TSIM_STREAM_MAC, &e);
    uint64_t first = tsim_rng_next(&a);
    CHECK(first == tsim_rng_next(&b));
    CHECK(first != tsim_rng_next(&c));
    CHECK(first != tsim_rng_next(&d));
    CHECK(first != tsim_rng_next(&e));
    rig_close(&r);
    rig_close(&q);
}

/* A frame on the air holds an event that points into the network. */
static void destroy_leaves_the_scheduler_runnable(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, 10);
    tsim_node_send(tsim_net_node(r.net, 0), &tx);
    tsim_node_send(tsim_net_node(r.net, 0), &tx);
    CHECK(tsim_node_transmit(tsim_net_node(r.net, 0)));
    tsim_net_destroy(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(1));
    CHECK(tsim_sched_size(r.sched) == 0);
    tsim_sched_destroy(r.sched);
}

int main(void) {
    RUN(originate_hands_the_message_to_its_routing);
    RUN(a_frame_arrives_as_its_bytes);
    RUN(ledger_charges_each_frame_under_its_purpose);
    RUN(queue_goes_by_priority_then_order);
    RUN(cancel_takes_back_only_what_is_queued);
    RUN(send_returns_its_own_handle_when_the_mac_sends_at_once);
    RUN(a_full_queue_drops);
    RUN(send_refuses_what_cannot_go_on_air);
    RUN(delivery_counts_each_destination_once);
    RUN(streams_are_separate_and_repeatable);
    RUN(destroy_leaves_the_scheduler_runnable);
    return CHECK_DONE();
}
