#include "tsim/meshcore.h"

#include <math.h>
#include <string.h>

#include "tsim/net.h"

#include "check.h"

/* SF7 at 125 kHz, 14 dBm, and the default radio's noise: a link's SNR is 14 - loss + 117 dB. */
#define LOSS_LOUD 100.0  /* SNR 31 dB */
#define LOSS_FAINT 138.0 /* SNR -7 dB, just above SF7's floor */

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct tsim_meshcore_config rc;
    struct tsim_meshcore_mac_config mc;
};

static struct tsim_lora lora(void) { return tsim_lora_default(7, 125000); }

/* Configured, not yet built: a test changes rc and mc, then calls build(). No adverts, so only
 * what a test sends is on the air, and floods without region codes, so frames are as long as
 * broadcast_len() says. */
static void rig_init(struct rig *r) {
    struct tsim_lora l = lora();
    r->rc = tsim_meshcore_default(0, &l, 14.0);
    r->rc.advert_interval = 0;
    r->rc.scoped = false;
    r->mc = tsim_meshcore_mac_default();
}

static void build(struct rig *r, uint32_t nodes, uint64_t seed) {
    struct tsim_net_params p = tsim_net_defaults(seed);
    p.queue_limit = 0;
    r->sched = tsim_sched_create();
    r->net =
        tsim_net_create(r->sched, &p, nodes, &tsim_meshcore, &r->rc, &tsim_meshcore_mac, &r->mc);
}

static void link(struct rig *r, uint32_t a, uint32_t b, double loss) {
    tsim_phy_set_loss(tsim_net_phy(r->net), a, b, loss);
}

/* Nodes in a row, each hearing only its neighbours, loudly. */
static void line(struct rig *r, uint32_t nodes, uint64_t seed) {
    build(r, nodes, seed);
    for (uint32_t i = 0; i + 1 < nodes; i++) {
        link(r, i, i + 1, LOSS_LOUD);
    }
    tsim_net_start(r->net);
}

static void clique(struct rig *r, uint32_t nodes, uint64_t seed) {
    build(r, nodes, seed);
    for (uint32_t a = 0; a < nodes; a++) {
        for (uint32_t b = a + 1; b < nodes; b++) {
            link(r, a, b, LOSS_LOUD);
        }
    }
    tsim_net_start(r->net);
}

static void rig_close(struct rig *r) {
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static uint64_t frames(const struct rig *r, uint32_t node, enum tsim_purpose purpose) {
    return tsim_net_ledger(r->net, node)->frames[purpose];
}

static uint64_t all_frames(const struct rig *r, uint32_t nodes, enum tsim_purpose purpose) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < nodes; i++) {
        n += frames(r, i, purpose);
    }
    return n;
}

/* A broadcast's frame: header, path length, then a channel hash, a MAC and the padded content. */
static uint32_t broadcast_len(uint32_t content, uint32_t hops) {
    return 2 + hops + 3 + (5 + content + 15) / 16 * 16;
}

static uint32_t ms(tsim_time t) { return (uint32_t)(t / TSIM_MS(1)); }

/* The tests below need no two of their first ten nodes to share a hash. */
static void the_first_nodes_have_hashes_of_their_own(void) {
    uint8_t seen[256] = {0};
    for (uint32_t i = 0; i < 10; i++) {
        uint8_t h[3], again[3];
        tsim_meshcore_hash(i, 3, h);
        tsim_meshcore_hash(i, 1, again);
        CHECK(h[0] == again[0]);
        CHECK(!seen[h[0]]);
        seen[h[0]] = 1;
    }
}

static void a_broadcast_is_relayed_once_by_every_relay_it_reaches(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 6, 1);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 5);
    CHECK(tsim_net_message(r.net, b)->finished);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_RELAY), 0);
    for (uint32_t i = 1; i < 6; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
    }
    rig_close(&r);
}

static void a_companion_relays_nothing(void) {
    struct rig r;
    rig_init(&r);
    strcpy(r.rc.relays, "0,2-3");
    line(&r, 4, 1);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 1); /* node 1 is a companion */
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 0);
    rig_close(&r);
}

/* Picked repeaters stand in for the list: node 1 is left out of the set, whatever `relays` says. */
static void a_companion_left_out_of_a_picked_set_relays_nothing(void) {
    static const uint8_t set[4] = {1, 0, 1, 1};
    struct rig r;
    rig_init(&r);
    r.rc.relay_pick = 3; /* cds, as the driver would have picked */
    r.rc.relay_count = 3;
    r.rc.relay_set = set;
    line(&r, 4, 1);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 1);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 0);
    CHECK_EQ_U64(tsim_meshcore_relay(&r.rc, 1), 0);
    CHECK_EQ_U64(tsim_meshcore_relay(&r.rc, 2), 1);
    rig_close(&r);
}

/* A relay at hop count flood_max is not relayed. */
static void a_flood_goes_no_further_than_flood_max(void) {
    struct rig r;
    rig_init(&r);
    r.rc.flood_max = 2;
    line(&r, 6, 1);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 3); /* nodes 1 to 3 */
    CHECK_EQ_U64(frames(&r, 3, TSIM_PURPOSE_RELAY), 0);
    rig_close(&r);
}

/* The path count is six bits: at the default flood_max of 64 a flood still stops at 63 hops, and
 * the 64th node does not relay it with its count spilt into the hash size's bits. */
static void a_flood_stops_at_the_most_hops_its_count_holds(void) {
    struct rig r;
    rig_init(&r);
    CHECK_EQ_U64(r.rc.flood_max, 64);
    line(&r, 66, 1);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 64); /* nodes 1 to 64 */
    CHECK_EQ_U64(frames(&r, 63, TSIM_PURPOSE_RELAY), 1);
    CHECK_EQ_U64(frames(&r, 64, TSIM_PURPOSE_RELAY), 0);
    rig_close(&r);
}

/* A relay waits a whole number of milliseconds from 0 to 5t, t half the airtime of what it sends,
 * after hearing the flood, and nothing else delays it on a quiet line. */
static void a_flood_relay_waits_up_to_five_halves_of_its_airtime(void) {
    struct tsim_lora l = lora();
    uint32_t relayed = broadcast_len(20, 1);
    uint32_t t = ms(tsim_lora_airtime(&l, relayed - 2 + 2)) / 2; /* path and payload, plus 2 */
    uint32_t longest = 0;
    for (uint64_t seed = 1; seed <= 40; seed++) {
        struct rig r;
        rig_init(&r);
        line(&r, 3, seed);
        uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
        tsim_sched_run_until(r.sched, TSIM_S(60));
        const struct tsim_message_record *m = tsim_net_message(r.net, b);
        CHECK_EQ_U64(m->delivered, 2);
        /* Node 1 had it at the end of the first frame, node 2 at the end of the relay. */
        tsim_time wait = m->last - tsim_lora_airtime(&l, relayed) - m->first;
        CHECK(wait >= 0 && wait <= (tsim_time)(5 * t) * TSIM_MS(1));
        CHECK(wait % TSIM_MS(1) == 0);
        longest = ms(wait) > longest ? ms(wait) : longest;
        rig_close(&r);
    }
    CHECK(longest > 4 * t); /* the window is used to its end */
}

/* With MeshBench's latched header flag, the relay finds the channel busy from the first frame's
 * header, 12.25 symbols after its preamble, until the flag times out, and goes at the first look
 * after that: 120 to 360 ms later at most. */
static void a_latched_header_holds_the_relay_until_the_flag_times_out(void) {
    struct tsim_lora l = lora();
    tsim_time hold = TSIM_MS(3934);
    tsim_time header = tsim_lora_symbol(&l) * (4 * (tsim_time)l.preamble + 49) / 4;
    for (uint64_t seed = 1; seed <= 10; seed++) {
        struct rig r;
        rig_init(&r);
        r.mc.latched_header = hold;
        line(&r, 3, seed);
        uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
        tsim_sched_run_until(r.sched, TSIM_S(60));
        const struct tsim_message_record *m = tsim_net_message(r.net, b);
        CHECK_EQ_U64(m->delivered, 2);
        tsim_time sent = m->first - tsim_lora_airtime(&l, broadcast_len(20, 0));
        tsim_time relayed = m->last - tsim_lora_airtime(&l, broadcast_len(20, 1));
        CHECK(relayed > sent + header + hold);
        CHECK(relayed <= sent + header + hold + TSIM_MS(360));
        rig_close(&r);
    }
}

/* Heard at about -7 dB on SF7, a flood scores about 0.045, and with a base of 10 waits
 * (10^(0.85 - score) - 1) airtimes before it is even looked at - delivered to its application
 * then, too. */
static void a_faint_flood_waits_out_its_receive_delay(void) {
    struct tsim_lora l = lora();
    uint32_t len = broadcast_len(20, 0);
    tsim_time air = tsim_lora_airtime(&l, len);
    double snr = 14.0 - LOSS_FAINT + 117.03;
    double score = (snr + 7.5) / 10.0 * (1.0 - len / 256.0);
    uint32_t delay = (uint32_t)((pow(10.0, 0.85 - score) - 1.0) * ms(air));
    CHECK(delay > 5 * ms(air));
    struct rig r;
    rig_init(&r);
    r.rc.rx_delay_base = 10;
    build(&r, 2, 1);
    link(&r, 0, 1, LOSS_FAINT);
    tsim_net_start(r.net);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    const struct tsim_message_record *m = tsim_net_message(r.net, b);
    CHECK_EQ_U64(m->delivered, 1);
    CHECK_EQ_I64(m->first, air + (tsim_time)delay * TSIM_MS(1));
    rig_close(&r);

    /* A loud one is looked at at once. */
    rig_init(&r);
    r.rc.rx_delay_base = 10;
    r.rc.tx_delay_factor = 0;
    line(&r, 3, 1);
    b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    m = tsim_net_message(r.net, b);
    CHECK_EQ_I64(m->first, air);
    CHECK_EQ_I64(m->last - m->first, tsim_lora_airtime(&l, broadcast_len(20, 1)));
    rig_close(&r);
}

/* In a clique the first relay is heard by every other relay still waiting. */
static uint64_t relays_in_a_clique(enum tsim_meshcore_cancel cancel_heard, uint64_t seed) {
    struct rig r;
    rig_init(&r);
    r.rc.cancel_heard = cancel_heard;
    clique(&r, 5, seed);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 4);
    uint64_t relays = all_frames(&r, 5, TSIM_PURPOSE_RELAY);
    rig_close(&r);
    return relays;
}

/* With no receive delay, nothing waits to be looked at: cancelling while waiting cancels nothing,
 * and only cancelling a queued relay does. */
static void hearing_a_relay_cancels_a_queued_one_only_when_queued(void) {
    uint64_t cancelled = 0;
    for (uint64_t seed = 1; seed <= 20; seed++) {
        CHECK_EQ_U64(relays_in_a_clique(TSIM_MESHCORE_CANCEL_NO, seed), 4);
        CHECK_EQ_U64(relays_in_a_clique(TSIM_MESHCORE_CANCEL_WAITING, seed), 4);
        uint64_t relays = relays_in_a_clique(TSIM_MESHCORE_CANCEL_QUEUED, seed);
        CHECK(relays >= 1 && relays < 4);
        cancelled += 4 - relays;
    }
    CHECK(cancelled >= 50); /* most of the 60 waiting relays */
}

/* Node 2 hears the flood faintly from node 0, and so waits out a receive delay of seconds, during
 * which it hears node 1's relay loudly. Waiting, it then delivers but does not relay. */
static void a_flood_heard_again_while_it_waits_is_not_relayed(void) {
    for (int mode = TSIM_MESHCORE_CANCEL_NO; mode <= TSIM_MESHCORE_CANCEL_WAITING; mode++) {
        struct rig r;
        rig_init(&r);
        r.rc.rx_delay_base = 10;
        r.rc.cancel_heard = (enum tsim_meshcore_cancel)mode;
        build(&r, 3, 1);
        link(&r, 0, 1, LOSS_LOUD);
        link(&r, 1, 2, LOSS_LOUD);
        link(&r, 0, 2, LOSS_FAINT);
        tsim_net_start(r.net);
        uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
        tsim_sched_run_until(r.sched, TSIM_S(60));
        CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 2);
        CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 1);
        CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_RELAY), mode == TSIM_MESHCORE_CANCEL_NO ? 1 : 0);
        rig_close(&r);
    }
}

static void on_finished(void *ctx, const struct tsim_message_record *rec) {
    (void)rec;
    (*(uint64_t *)ctx)++;
}

/* The first message floods and its path return comes back; the second goes direct, so a node
 * off the path - node 4, beside node 1 - relays the first and not the second. */
static void the_second_message_goes_direct_along_the_learned_path(void) {
    struct rig r;
    rig_init(&r);
    build(&r, 5, 1);
    link(&r, 0, 1, LOSS_LOUD);
    link(&r, 1, 2, LOSS_LOUD);
    link(&r, 2, 3, LOSS_LOUD);
    link(&r, 1, 4, LOSS_LOUD);
    tsim_net_start(r.net);
    uint64_t finished = 0;
    tsim_net_observe_finished(r.net, on_finished, &finished);
    uint64_t first = tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, first)->delivered, 1);
    CHECK(tsim_net_message(r.net, first)->finished);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1); /* acknowledged: no retry */
    uint64_t off_path = frames(&r, 4, TSIM_PURPOSE_RELAY) + frames(&r, 4, TSIM_PURPOSE_CONTROL);
    CHECK(off_path > 0);
    uint64_t second = tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(120));
    CHECK_EQ_U64(tsim_net_message(r.net, second)->delivered, 1);
    CHECK(tsim_net_message(r.net, second)->finished);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 2);
    CHECK_EQ_U64(frames(&r, 4, TSIM_PURPOSE_RELAY) + frames(&r, 4, TSIM_PURPOSE_CONTROL), off_path);
    CHECK_EQ_U64(finished, 2);
    rig_close(&r);
}

/* A destination nobody can reach: the first attempt and `retries` more, then it is given up. */
static void an_unanswered_message_is_tried_retries_more_times_then_given_up(void) {
    struct rig r;
    rig_init(&r);
    r.rc.retries = 2;
    build(&r, 3, 1);
    link(&r, 0, 1, LOSS_LOUD);
    tsim_net_start(r.net);
    uint64_t m = tsim_net_originate(r.net, 0, 2, 20);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 3);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 3); /* each attempt is a new packet */
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 0);
    rig_close(&r);
}

/* With the path learned through node 1, node 1 goes and node 2 takes its place: the direct attempt
 * names node 1 and is not answered, and the retry floods, through node 2, and arrives. */
static void a_path_that_fails_is_forgotten_and_the_retry_floods(void) {
    struct rig r;
    rig_init(&r);
    build(&r, 4, 1);
    link(&r, 0, 1, LOSS_LOUD);
    link(&r, 1, 3, LOSS_LOUD);
    tsim_net_start(r.net);
    uint64_t first = tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK(tsim_net_message(r.net, first)->finished);
    CHECK_EQ_U64(tsim_net_message(r.net, first)->delivered, 1);
    link(&r, 0, 1, INFINITY);
    link(&r, 1, 3, INFINITY);
    link(&r, 0, 2, LOSS_LOUD);
    link(&r, 2, 3, LOSS_LOUD);
    uint64_t second = tsim_net_originate(r.net, 0, 3, 20);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK(tsim_net_message(r.net, second)->finished);
    CHECK_EQ_U64(tsim_net_message(r.net, second)->delivered, 1);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA),
                 3); /* the first; the second direct, then flooded */
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_RELAY), 1);
    rig_close(&r);
}

/* Node 0 is part-way through sending when node 1 has its own frame due: node 1, receiving,
 * waits 120, 240 or 360 ms past the end and looks again, so node 0 then hears it. */
static void the_mac_holds_off_while_the_radio_is_receiving(void) {
    struct tsim_lora l = lora();
    tsim_time first_air = tsim_lora_airtime(&l, broadcast_len(100, 0));
    for (uint64_t seed = 1; seed <= 10; seed++) {
        struct rig r;
        rig_init(&r);
        strcpy(r.rc.relays, "2");
        line(&r, 2, seed);
        uint64_t a = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 100);
        tsim_sched_run_until(r.sched, first_air / 2);
        uint64_t b = tsim_net_originate(r.net, 1, TSIM_BROADCAST, 20);
        tsim_sched_run_until(r.sched, TSIM_S(60));
        CHECK_EQ_U64(tsim_net_message(r.net, a)->delivered, 1);
        const struct tsim_message_record *m = tsim_net_message(r.net, b);
        CHECK_EQ_U64(m->delivered, 1);
        tsim_time sent = m->first - tsim_lora_airtime(&l, broadcast_len(20, 0));
        tsim_time after = sent - first_air;
        CHECK(after == TSIM_MS(120) || after == TSIM_MS(240) || after == TSIM_MS(360) ||
              /* a second look, still inside node 0's frame */
              (after > 0 && after <= TSIM_MS(720)));
        rig_close(&r);
    }
}

/* An airtime factor of 3599 is a budget of one second an hour: past it, a node waits. */
static void the_duty_cycle_budget_holds_a_node_back(void) {
    struct rig r;
    rig_init(&r);
    r.mc.airtime_factor = 3599;
    line(&r, 2, 1);
    for (int i = 0; i < 30; i++) {
        tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    }
    tsim_sched_run_until(r.sched, TSIM_S(600));
    struct tsim_lora l = lora();
    tsim_time air = tsim_lora_airtime(&l, broadcast_len(20, 0));
    uint64_t sent = frames(&r, 0, TSIM_PURPOSE_DATA);
    /* About a second's worth, and at most what ten minutes' refill adds. */
    CHECK(sent >= 1 && (tsim_time)sent * air <= TSIM_S(1) + TSIM_S(600) / 3600 + air);
    rig_close(&r);

    rig_init(&r);
    line(&r, 2, 1);
    for (int i = 0; i < 30; i++) {
        tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
    }
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 30);
    rig_close(&r);
}

static void a_repeater_adverts_every_interval_and_a_companion_never(void) {
    struct rig r;
    rig_init(&r);
    r.rc.advert_interval = TSIM_S(120);
    strcpy(r.rc.relays, "0");
    line(&r, 2, 1);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_ANNOUNCE), 5);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_ANNOUNCE), 0);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 0); /* zero-hop: never relayed */
    rig_close(&r);
}

/* With a full path in front of it, 171 bytes of content still fit a frame; 172 do not. */
static void a_message_a_frame_cannot_carry_is_refused(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 2, 1);
    uint64_t fits = tsim_net_originate(r.net, 0, 1, 171);
    uint64_t over = tsim_net_originate(r.net, 0, 1, 172);
    uint64_t wide = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 172);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK(!tsim_net_message(r.net, fits)->refused);
    CHECK_EQ_U64(tsim_net_message(r.net, fits)->delivered, 1);
    CHECK(tsim_net_message(r.net, over)->refused);
    CHECK(tsim_net_message(r.net, wide)->refused);
    rig_close(&r);
}

/* The firmware's airtime at 4/5 shortens the window at 4/8 by the ratio of the two. */
/* A scoped relay carries four bytes of region codes, but the firmware reckons its delay from the
 * path and payload only, so the window is the unscoped one's. */
static void a_scoped_relay_waits_as_long_as_an_unscoped_one(void) {
    struct tsim_lora l = lora();
    uint32_t t = ms(tsim_lora_airtime(&l, broadcast_len(20, 1))) / 2;
    uint32_t longest = 0;
    for (uint64_t seed = 1; seed <= 40; seed++) {
        struct rig r;
        rig_init(&r);
        r.rc.scoped = true;
        line(&r, 3, seed);
        uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
        tsim_sched_run_until(r.sched, TSIM_S(60));
        const struct tsim_message_record *m = tsim_net_message(r.net, b);
        tsim_time wait = m->last - tsim_lora_airtime(&l, broadcast_len(20, 1) + 4) - m->first;
        CHECK(wait >= 0 && wait <= (tsim_time)(5 * t) * TSIM_MS(1));
        longest = ms(wait) > longest ? ms(wait) : longest;
        rig_close(&r);
    }
    CHECK(longest > 4 * t);
}

static void estimate_cr_reckons_the_delays_at_another_coding_rate(void) {
    struct tsim_lora l = lora();
    l.cr = 4;
    struct tsim_lora at45 = l;
    at45.cr = 1;
    uint32_t relayed = broadcast_len(20, 1);
    uint32_t t = ms(tsim_lora_airtime(&at45, relayed)) / 2;
    for (uint64_t seed = 1; seed <= 20; seed++) {
        struct rig r;
        rig_init(&r);
        r.rc.lora = l;
        r.rc.estimate_cr = 1;
        line(&r, 3, seed);
        uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 20);
        tsim_sched_run_until(r.sched, TSIM_S(60));
        const struct tsim_message_record *m = tsim_net_message(r.net, b);
        tsim_time wait = m->last - tsim_lora_airtime(&l, relayed) - m->first;
        CHECK(wait >= 0 && wait <= (tsim_time)(5 * t) * TSIM_MS(1));
        rig_close(&r);
    }
}

/* With 255 retries a message reaches attempt 255, and an acknowledgement for another node's
 * message still has to be told apart from every one of its attempts. Node 0 tries node 2, which
 * hears nobody, while node 3 sends to node 1 every second; nobody relays, so node 1's
 * acknowledgements go zero-hop, and node 0 overhears and looks at every one. */
static void the_last_attempt_of_the_most_retries_still_ends(void) {
    struct rig r;
    rig_init(&r);
    r.rc.retries = 255;
    strcpy(r.rc.relays, "9");
    build(&r, 4, 1);
    link(&r, 0, 1, LOSS_LOUD);
    link(&r, 0, 3, LOSS_LOUD);
    link(&r, 1, 3, LOSS_LOUD);
    tsim_net_start(r.net);
    uint64_t lost = tsim_net_originate(r.net, 0, 2, 20);
    tsim_time t = 0;
    while (!tsim_net_message(r.net, lost)->finished && t < TSIM_S(7200)) {
        tsim_net_originate(r.net, 3, 1, 20);
        t += TSIM_S(1);
        tsim_sched_run_until(r.sched, t);
    }
    CHECK(tsim_net_message(r.net, lost)->finished);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 256);
    uint64_t messages = tsim_net_message_count(r.net), delivered = 0;
    for (uint64_t id = 2; id <= messages; id++) {
        delivered += tsim_net_message(r.net, id)->delivered;
    }
    CHECK(messages > 100 && delivered + 5 >= messages - 1);
    rig_close(&r);
}

static void a_seed_repeats_a_run(void) {
    uint64_t relays[2];
    tsim_time last[2];
    for (int i = 0; i < 2; i++) {
        struct rig r;
        rig_init(&r);
        clique(&r, 6, 7);
        uint64_t b = tsim_net_originate(r.net, 2, TSIM_BROADCAST, 30);
        tsim_net_originate(r.net, 3, 5, 30);
        tsim_sched_run_until(r.sched, TSIM_S(120));
        relays[i] = all_frames(&r, 6, TSIM_PURPOSE_RELAY);
        last[i] = tsim_net_message(r.net, b)->last;
        rig_close(&r);
    }
    CHECK_EQ_U64(relays[0], relays[1]);
    CHECK_EQ_I64(last[0], last[1]);
}

static void bad_configs_are_refused(void) {
    struct tsim_lora l = lora();
    struct tsim_meshcore_config ok = tsim_meshcore_default(0, &l, 14.0);
    struct tsim_meshcore_config c[13];
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        c[i] = ok;
    }
    c[0].hash_size = 0;
    c[1].hash_size = 4;
    c[2].flood_max = 0;
    c[3].flood_max = 65;
    c[4].rx_delay_base = 20.5;
    c[5].tx_delay_factor = 2.5;
    c[6].direct_tx_delay_factor = -0.1;
    strcpy(c[7].relays, "3-1");
    c[8].advert_interval = -1;
    c[9].estimate_cr = 5;
    strcpy(c[10].relays, "1,");
    c[11].cancel_heard = (enum tsim_meshcore_cancel)3;
    c[12].relay_pick = 3; /* picked, but no set to say by whom */
    struct tsim_meshcore_mac_config mc = tsim_meshcore_mac_default();
    struct tsim_sched *sched = tsim_sched_create();
    struct tsim_net_params p = tsim_net_defaults(1);
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        CHECK(tsim_net_create(sched, &p, 2, &tsim_meshcore, &c[i], &tsim_meshcore_mac, &mc) ==
              NULL);
    }
    mc.airtime_factor = -1;
    CHECK(tsim_net_create(sched, &p, 2, &tsim_meshcore, &ok, &tsim_meshcore_mac, &mc) == NULL);
    mc = tsim_meshcore_mac_default();
    mc.latched_header = -1;
    CHECK(tsim_net_create(sched, &p, 2, &tsim_meshcore, &ok, &tsim_meshcore_mac, &mc) == NULL);
    tsim_sched_destroy(sched);

    CHECK(tsim_meshcore_relays_valid("all"));
    CHECK(tsim_meshcore_relays_valid("0-45,50"));
    CHECK(!tsim_meshcore_relays_valid(""));
    CHECK(!tsim_meshcore_relays_valid("a"));
    CHECK(!tsim_meshcore_relays_valid("2-"));
}

static void destroy_mid_flood_leaves_the_scheduler_runnable(void) {
    struct rig r;
    rig_init(&r);
    r.rc.advert_interval = TSIM_S(30);
    r.rc.rx_delay_base = 10;
    clique(&r, 6, 1);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 30);
    tsim_net_originate(r.net, 1, 4, 30);
    tsim_sched_run_until(r.sched, TSIM_MS(150));
    tsim_net_destroy(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK(tsim_sched_size(r.sched) == 0);
    tsim_sched_destroy(r.sched);
}

int main(void) {
    RUN(the_first_nodes_have_hashes_of_their_own);
    RUN(a_broadcast_is_relayed_once_by_every_relay_it_reaches);
    RUN(a_companion_relays_nothing);
    RUN(a_companion_left_out_of_a_picked_set_relays_nothing);
    RUN(a_flood_goes_no_further_than_flood_max);
    RUN(a_flood_stops_at_the_most_hops_its_count_holds);
    RUN(a_flood_relay_waits_up_to_five_halves_of_its_airtime);
    RUN(a_latched_header_holds_the_relay_until_the_flag_times_out);
    RUN(a_faint_flood_waits_out_its_receive_delay);
    RUN(hearing_a_relay_cancels_a_queued_one_only_when_queued);
    RUN(a_flood_heard_again_while_it_waits_is_not_relayed);
    RUN(the_second_message_goes_direct_along_the_learned_path);
    RUN(an_unanswered_message_is_tried_retries_more_times_then_given_up);
    RUN(a_path_that_fails_is_forgotten_and_the_retry_floods);
    RUN(the_mac_holds_off_while_the_radio_is_receiving);
    RUN(the_duty_cycle_budget_holds_a_node_back);
    RUN(a_repeater_adverts_every_interval_and_a_companion_never);
    RUN(a_message_a_frame_cannot_carry_is_refused);
    RUN(a_scoped_relay_waits_as_long_as_an_unscoped_one);
    RUN(estimate_cr_reckons_the_delays_at_another_coding_rate);
    RUN(the_last_attempt_of_the_most_retries_still_ends);
    RUN(a_seed_repeats_a_run);
    RUN(bad_configs_are_refused);
    RUN(destroy_mid_flood_leaves_the_scheduler_runnable);
    return CHECK_DONE();
}
