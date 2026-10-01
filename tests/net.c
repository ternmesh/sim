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
    bool refuse;
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

static bool rec_originate(void *self, const struct tsim_message *msg) {
    struct log *log = ((struct self *)self)->log;
    log->originated++;
    log->msg = *msg;
    return !log->refuse;
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
    tsim_net_start(r->net);
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
    CHECK(!tsim_net_message(r.net, 1)->refused);

    /* A message its routing cannot carry is still originated, and counted against it. */
    r.log.refuse = true;
    uint64_t refused = tsim_net_originate(r.net, 0, 2, 10);
    CHECK_EQ_U64(refused, 2);
    CHECK(tsim_net_message(r.net, refused)->refused);
    CHECK(tsim_net_message(r.net, refused)->wanted == 1);
    CHECK_EQ_U64(tsim_net_stats(r.net, 0)->refused, 1);
    CHECK_EQ_U64(tsim_net_stats(r.net, 1)->refused, 0);
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
    CHECK(r.log.kicks[0] == 3); /* two sends and the cancel that took something back */
    rig_close(&r);
}

struct fired {
    struct tsim_node *node;
    struct tsim_timer *timer;
    int count;
    tsim_time at;
    bool self_destruct;
};

static void on_fire(void *ctx) {
    struct fired *f = ctx;
    f->count++;
    f->at = tsim_node_now(f->node);
    if (f->self_destruct) {
        tsim_timer_destroy(f->timer);
    }
}

static void timers_fire_once_at_their_time(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct fired f = {.node = tsim_net_node(r.net, 0)};
    f.timer = tsim_timer_create(f.node, on_fire, &f);
    CHECK(!tsim_timer_pending(f.timer));
    CHECK(tsim_timer_start(f.timer, TSIM_MS(5)));
    CHECK(tsim_timer_pending(f.timer));
    run_for(&r, TSIM_MS(4));
    CHECK(f.count == 0);
    run_for(&r, TSIM_MS(1));
    CHECK(f.count == 1);
    CHECK_EQ_I64(f.at, TSIM_MS(5));
    CHECK(!tsim_timer_pending(f.timer));

    /* Starting again replaces the time it was set to. */
    tsim_timer_start(f.timer, TSIM_MS(10));
    tsim_timer_start(f.timer, TSIM_MS(2));
    run_for(&r, TSIM_MS(20));
    CHECK(f.count == 2);
    CHECK_EQ_I64(f.at, TSIM_MS(7));

    tsim_timer_start(f.timer, TSIM_MS(1));
    tsim_timer_stop(f.timer);
    CHECK(!tsim_timer_start(f.timer, -1));
    CHECK(!tsim_timer_pending(f.timer));
    run_for(&r, TSIM_MS(20));
    CHECK(f.count == 2);
    tsim_timer_destroy(f.timer);
    rig_close(&r);
}

static void a_timer_may_destroy_itself_when_it_fires(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct fired f = {.node = tsim_net_node(r.net, 1), .self_destruct = true};
    f.timer = tsim_timer_create(f.node, on_fire, &f);
    tsim_timer_start(f.timer, TSIM_MS(1));
    run_for(&r, TSIM_MS(5));
    CHECK(f.count == 1);
    rig_close(&r);
}

/* Timers a plugin leaves running are stopped and freed with the network. */
static void destroy_frees_timers_left_running(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct fired f[3] = {{.node = tsim_net_node(r.net, 0)},
                         {.node = tsim_net_node(r.net, 0)},
                         {.node = tsim_net_node(r.net, 1)}};
    for (int i = 0; i < 3; i++) {
        f[i].timer = tsim_timer_create(f[i].node, on_fire, &f[i]);
        tsim_timer_start(f[i].timer, TSIM_MS(1 + i));
    }
    tsim_net_destroy(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(1));
    CHECK(f[0].count + f[1].count + f[2].count == 0);
    CHECK(tsim_sched_size(r.sched) == 0);
    tsim_sched_destroy(r.sched);
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

static void head_handle_names_the_frame_at_the_head(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct tsim_node *n0 = tsim_net_node(r.net, 0);
    CHECK_EQ_U64(tsim_node_head_handle(n0), 0);
    struct tsim_tx low = frame(TSIM_PURPOSE_DATA, 0, 10);
    struct tsim_tx high = frame(TSIM_PURPOSE_CONTROL, 1, 20);
    uint64_t first = tsim_node_send(n0, &low);
    CHECK_EQ_U64(tsim_node_head_handle(n0), first);
    uint64_t jumped = tsim_node_send(n0, &high);
    CHECK_EQ_U64(tsim_node_head_handle(n0), jumped);
    CHECK(tsim_node_cancel(n0, jumped));
    CHECK_EQ_U64(tsim_node_head_handle(n0), first);
    CHECK(tsim_node_transmit(n0));
    CHECK_EQ_U64(tsim_node_head_handle(n0), 0);
    rig_close(&r);
}

/* A node's own view of its channel: what it has sent and heard, a frame in flight counting for
 * the part gone by. */
static void a_node_knows_its_own_airtime(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    struct tsim_node *n0 = tsim_net_node(r.net, 0);
    struct tsim_node *n1 = tsim_net_node(r.net, 1);
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, 16);
    tsim_node_send(n0, &tx);
    CHECK(tsim_node_transmit(n0));
    run_for(&r, TSIM_MS(10));
    CHECK_EQ_I64(tsim_node_tx_airtime(n0), TSIM_MS(10));
    CHECK_EQ_I64(tsim_node_rx_airtime(n1), TSIM_MS(10));
    CHECK_EQ_I64(tsim_node_tx_airtime(n1) + tsim_node_rx_airtime(n0), 0);
    run_for(&r, TSIM_S(1));
    CHECK_EQ_I64(tsim_node_tx_airtime(n0), airtime(16));
    CHECK_EQ_I64(tsim_node_rx_airtime(n1), airtime(16));
    rig_close(&r);
}

/* The power node 1 hears node 0's first frame at, on a 100 dB link. */
static double heard_at(uint64_t seed, double fading_db) {
    struct rig r;
    struct tsim_net_params p = tsim_net_defaults(seed);
    p.phy.fading_db = fading_db;
    p.phy.fading_seed = 12345; /* replaced by the network's seed */
    rig_open(&r, p, 2);
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, 16);
    tsim_node_send(tsim_net_node(r.net, 0), &tx);
    tsim_node_transmit(tsim_net_node(r.net, 0));
    run_for(&r, TSIM_S(1));
    CHECK_EQ_I64(r.log.rx[1], 1);
    double rssi = r.log.rx_rssi;
    rig_close(&r);
    return rssi;
}

/* The radios' fading is seeded by the run, like everything else random in it. */
static void fading_follows_the_networks_seed(void) {
    CHECK(heard_at(1, 0) == -86.0);
    CHECK(heard_at(1, 3.0) != -86.0);
    CHECK(heard_at(1, 3.0) == heard_at(1, 3.0));
    CHECK(heard_at(1, 3.0) != heard_at(2, 3.0));
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

/* Puts message `msg` on the air from `from`, as a frame carrying its content and nothing else, and
 * lets it arrive. */
static bool carry(struct rig *r, uint32_t from, uint64_t msg) {
    const struct tsim_message *m = &tsim_net_message(r->net, msg)->msg;
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, m->len);
    memcpy(tx.bytes, m->content, m->len);
    tx.carries = msg;
    bool queued = tsim_node_send(tsim_net_node(r->net, from), &tx) != 0;
    bool sent = tsim_node_transmit(tsim_net_node(r->net, from));
    run_for(r, TSIM_S(1));
    return queued && sent;
}

static void delivery_counts_each_destination_once(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 4);
    tsim_phy_set_loss(tsim_net_phy(r.net), 0, 2, 100.0);
    tsim_phy_set_loss(tsim_net_phy(r.net), 0, 3, 100.0);
    uint64_t u = tsim_net_originate(r.net, 0, 2, 5);
    CHECK(carry(&r, 0, u));
    tsim_time t = tsim_sched_now(r.sched);
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), u)); /* holds it, not its destination */
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 2), u));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 2), u));
    const struct tsim_message_record *m = tsim_net_message(r.net, u);
    CHECK(m->wanted == 1 && m->delivered == 1);
    CHECK_EQ_I64(m->first, t);

    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 5);
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 0), b)); /* its source */
    CHECK(carry(&r, 0, b));
    t = tsim_sched_now(r.sched);
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 1), b));
    run_for(&r, TSIM_MS(1));
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 3), b));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), b));
    m = tsim_net_message(r.net, b);
    CHECK(m->wanted == 3 && m->delivered == 2);
    CHECK_EQ_I64(m->first, t);
    CHECK_EQ_I64(m->last, t + TSIM_MS(1));

    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), 0));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), 3));
    CHECK(tsim_net_message(r.net, 3) == NULL);
    CHECK_EQ_U64(tsim_net_stats(r.net, 1)->delivered, 1);
    CHECK_EQ_U64(tsim_net_stats(r.net, 2)->delivered, 1);
    rig_close(&r);
}

/* Message ids are sequential, so a plugin could name every one; naming one is not having it. */
static void delivery_needs_the_message_to_have_arrived(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 3);
    uint64_t to1 = tsim_net_originate(r.net, 0, 1, 5);
    uint64_t to2 = tsim_net_originate(r.net, 0, 2, 5);
    for (uint64_t id = 0; id <= 10; id++) {
        CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), id));
        CHECK(!tsim_node_deliver(tsim_net_node(r.net, 2), id));
    }
    CHECK(tsim_net_message(r.net, to1)->delivered == 0);

    /* A frame with the content in it, but not marked as carrying the message, does not count. */
    const struct tsim_message *m = &tsim_net_message(r.net, to1)->msg;
    struct tsim_tx bare = frame(TSIM_PURPOSE_DATA, 0, m->len);
    memcpy(bare.bytes, m->content, m->len);
    tsim_node_send(tsim_net_node(r.net, 0), &bare);
    tsim_node_transmit(tsim_net_node(r.net, 0));
    run_for(&r, TSIM_S(1));
    CHECK(r.log.rx[1] == 1);
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), to1));

    CHECK(carry(&r, 0, to1));
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 1), to1));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 2), to2)); /* out of range: never had it */
    CHECK_EQ_U64(tsim_net_stats(r.net, 2)->delivered, 0);
    rig_close(&r);
}

static void a_frame_must_carry_what_it_claims(void) {
    struct rig r;
    rig_open(&r, tsim_net_defaults(1), 2);
    uint64_t id = tsim_net_originate(r.net, 0, 1, 8);
    const struct tsim_message *m = &tsim_net_message(r.net, id)->msg;
    struct tsim_node *n0 = tsim_net_node(r.net, 0);
    struct tsim_tx tx = frame(TSIM_PURPOSE_DATA, 0, 12);
    memcpy(tx.bytes + 4, m->content, 8);
    tx.carries = id;
    tx.carries_at = 4;

    struct tsim_tx wrong = tx;
    wrong.bytes[7] ^= 1;
    CHECK_EQ_U64(tsim_node_send(n0, &wrong), 0);
    struct tsim_tx short_frame = tx;
    short_frame.len = 11; /* one byte of content missing */
    CHECK_EQ_U64(tsim_node_send(n0, &short_frame), 0);
    struct tsim_tx past_end = tx;
    past_end.carries_at = 13;
    CHECK_EQ_U64(tsim_node_send(n0, &past_end), 0);
    struct tsim_tx unknown = tx;
    unknown.carries = 99;
    CHECK_EQ_U64(tsim_node_send(n0, &unknown), 0);
    /* The right bytes from a node that never received them. */
    CHECK_EQ_U64(tsim_node_send(tsim_net_node(r.net, 1), &tx), 0);
    CHECK_EQ_U64(tsim_net_stats(r.net, 0)->queued, 0);

    CHECK(tsim_node_send(n0, &tx) != 0);
    /* Once it has arrived, the receiver may forward it too. */
    CHECK(tsim_node_transmit(n0));
    run_for(&r, TSIM_S(1));
    CHECK(tsim_node_send(tsim_net_node(r.net, 1), &tx) != 0);
    rig_close(&r);
}

/* Routing starts when the driver says the links are laid, not when the network is built. */
static void routing_starts_once_when_told(void) {
    struct log log = {0};
    struct tsim_sched *sched = tsim_sched_create();
    struct tsim_net_params p = tsim_net_defaults(1);
    struct tsim_net *net = tsim_net_create(sched, &p, 3, &recorder, &log, &manual, &log);
    CHECK(log.starts == 0);
    tsim_net_start(net);
    CHECK(log.starts == 3);
    tsim_net_start(net);
    CHECK(log.starts == 3);
    tsim_net_destroy(net);
    tsim_sched_destroy(sched);
}

struct seen {
    int calls;
    uint64_t msg;
    uint32_t node;
    uint32_t delivered;
};

static void on_delivered(void *ctx, const struct tsim_message_record *rec, uint32_t node) {
    struct seen *s = ctx;
    s->calls++;
    s->msg = rec->msg.id;
    s->node = node;
    s->delivered = rec->delivered;
}

/* Once per delivery that counts, after the record has counted it, and not once removed. */
static void the_observer_sees_each_delivery(void) {
    struct rig r;
    struct seen seen = {0};
    rig_open(&r, tsim_net_defaults(1), 4);
    tsim_phy_set_loss(tsim_net_phy(r.net), 0, 2, 100.0);
    tsim_net_observe(r.net, on_delivered, &seen);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 5);
    CHECK(carry(&r, 0, b));
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 1), b));
    CHECK(seen.calls == 1 && seen.msg == b && seen.node == 1 && seen.delivered == 1);
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 1), b));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 0), b));
    CHECK(!tsim_node_deliver(tsim_net_node(r.net, 3), b)); /* never heard it */
    CHECK(seen.calls == 1);
    tsim_net_observe(r.net, NULL, NULL);
    CHECK(tsim_node_deliver(tsim_net_node(r.net, 2), b));
    CHECK(seen.calls == 1);
    CHECK(tsim_net_message(r.net, b)->delivered == 2);
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
    RUN(head_handle_names_the_frame_at_the_head);
    RUN(a_node_knows_its_own_airtime);
    RUN(fading_follows_the_networks_seed);
    RUN(routing_starts_once_when_told);
    RUN(originate_hands_the_message_to_its_routing);
    RUN(a_frame_arrives_as_its_bytes);
    RUN(ledger_charges_each_frame_under_its_purpose);
    RUN(queue_goes_by_priority_then_order);
    RUN(cancel_takes_back_only_what_is_queued);
    RUN(send_returns_its_own_handle_when_the_mac_sends_at_once);
    RUN(a_full_queue_drops);
    RUN(send_refuses_what_cannot_go_on_air);
    RUN(delivery_counts_each_destination_once);
    RUN(delivery_needs_the_message_to_have_arrived);
    RUN(a_frame_must_carry_what_it_claims);
    RUN(the_observer_sees_each_delivery);
    RUN(streams_are_separate_and_repeatable);
    RUN(timers_fire_once_at_their_time);
    RUN(a_timer_may_destroy_itself_when_it_fires);
    RUN(destroy_frees_timers_left_running);
    RUN(destroy_leaves_the_scheduler_runnable);
    return CHECK_DONE();
}
