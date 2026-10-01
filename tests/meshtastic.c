#include "tsim/meshtastic.h"

#include <math.h>

#include "tsim/net.h"

#include "check.h"

/* SF7 at 125 kHz, 14 dBm, and the default radio's noise: a link's SNR is 14 - loss + 117 dB. */
#define LOSS_LOUD 100.0  /* SNR 31 dB */
#define LOSS_FAINT 130.0 /* SNR 1 dB */

struct rig {
    struct tsim_sched *sched;
    struct tsim_net *net;
    struct tsim_meshtastic_config rc;
    struct tsim_meshtastic_mac_config mc;
};

static struct tsim_lora lora(void) { return tsim_lora_default(7, 125000); }

/* Configured, not yet built: a test changes rc and mc, then calls build(). */
static void rig_init(struct rig *r) {
    struct tsim_lora l = lora();
    r->rc = tsim_meshtastic_default(0, &l, 14.0);
    r->mc = tsim_meshtastic_mac_default(&l);
    /* A rebroadcast's window spans SNRs of 0 to 30 dB, which SF7 decodes. */
    r->mc.snr_min_db = 0;
    r->mc.snr_max_db = 30;
}

static void build(struct rig *r, uint32_t nodes, uint64_t seed) {
    struct tsim_net_params p = tsim_net_defaults(seed);
    r->sched = tsim_sched_create();
    r->net = tsim_net_create(r->sched, &p, nodes, &tsim_meshtastic, &r->rc, &tsim_meshtastic_mac,
                             &r->mc);
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

static void rig_close(struct rig *r) {
    tsim_net_destroy(r->net);
    tsim_sched_destroy(r->sched);
}

static tsim_time airtime(uint32_t len) {
    struct tsim_lora l = lora();
    return tsim_lora_airtime(&l, len);
}

static uint64_t frames(const struct rig *r, uint32_t node, enum tsim_purpose purpose) {
    return tsim_net_ledger(r->net, node)->frames[purpose];
}

static void slot_is_cad_plus_the_firmware_allowance(void) {
    /* LongFast: SF11 at 250 kHz, 8.192 ms symbols. */
    struct tsim_lora long_fast = tsim_lora_default(11, 250000);
    CHECK_EQ_I64(tsim_meshtastic_slot(&long_fast), TSIM_US(20480) + TSIM_US(7600));
    struct tsim_lora l = lora(); /* 1.024 ms symbols */
    CHECK_EQ_I64(tsim_meshtastic_slot(&l), TSIM_US(2560) + TSIM_US(7600));
}

static void a_broadcast_crosses_the_hop_limit_and_no_further(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 6, 1);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    const struct tsim_message_record *m = tsim_net_message(r.net, b);
    CHECK_EQ_U64(m->delivered, 4); /* nodes 1 to 4: three rebroadcasts after the first */
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1); /* node 1's rebroadcast acknowledged it */
    for (uint32_t i = 1; i <= 3; i++) {
        CHECK_EQ_U64(frames(&r, i, TSIM_PURPOSE_RELAY), 1);
    }
    CHECK_EQ_U64(frames(&r, 4, TSIM_PURPOSE_RELAY), 0);
    CHECK_EQ_U64(frames(&r, 5, TSIM_PURPOSE_RELAY), 0);
    rig_close(&r);

    rig_init(&r);
    r.rc.hop_limit = 0;
    line(&r, 3, 1);
    b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 1);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 0);
    rig_close(&r);
}

/* 17 bytes of header and payload type leave 238 for the content. */
static void a_message_one_frame_cannot_carry_is_refused(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 2, 1);
    uint64_t fits = tsim_net_originate(r.net, 0, 1, TSIM_FRAME_MAX - TSIM_MESHTASTIC_OVERHEAD);
    uint64_t over = tsim_net_originate(r.net, 0, 1, TSIM_FRAME_MAX - TSIM_MESHTASTIC_OVERHEAD + 1);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK(!tsim_net_message(r.net, fits)->refused);
    CHECK_EQ_U64(tsim_net_message(r.net, fits)->delivered, 1);
    CHECK(tsim_net_message(r.net, over)->refused);
    rig_close(&r);
}

/* Every node hears every other: the first rebroadcast is the second copy each other node hears. */
static uint64_t relays_in_a_clique(enum tsim_meshtastic_role role, uint64_t seed) {
    struct rig r;
    rig_init(&r);
    r.rc.role = role;
    build(&r, 5, seed);
    for (uint32_t a = 0; a < 5; a++) {
        for (uint32_t b = a + 1; b < 5; b++) {
            link(&r, a, b, LOSS_LOUD);
        }
    }
    tsim_net_start(r.net);
    uint64_t b = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, b)->delivered, 4);
    uint64_t relays = 0;
    for (uint32_t i = 0; i < 5; i++) {
        relays += frames(&r, i, TSIM_PURPOSE_RELAY);
    }
    CHECK_EQ_U64(tsim_net_stats(r.net, 0)->cancelled, 0);
    rig_close(&r);
    return relays;
}

static void a_second_copy_cancels_a_clients_rebroadcast_and_a_third_a_routers(void) {
    for (uint64_t seed = 1; seed <= 10; seed++) {
        CHECK_EQ_U64(relays_in_a_clique(TSIM_MESHTASTIC_CLIENT, seed), 1);
        CHECK_EQ_U64(relays_in_a_clique(TSIM_MESHTASTIC_ROUTER, seed), 2);
        CHECK_EQ_U64(relays_in_a_clique(TSIM_MESHTASTIC_CLIENT_MUTE, seed), 0);
    }
}

static void a_direct_message_is_acknowledged_back_along_the_flood(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 3, 1);
    uint64_t m = tsim_net_originate(r.net, 0, 2, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_CONTROL), 1); /* the acknowledgement */
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_RELAY), 0);   /* the destination keeps it */
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 1);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_CONTROL), 1); /* passing the acknowledgement back */
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 1);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_RELAY) + frames(&r, 0, TSIM_PURPOSE_CONTROL), 0);
    rig_close(&r);

    rig_init(&r);
    r.rc.want_ack = false;
    line(&r, 3, 1);
    m = tsim_net_originate(r.net, 0, 2, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    CHECK_EQ_U64(frames(&r, 2, TSIM_PURPOSE_CONTROL), 0);
    rig_close(&r);
}

/* Node 1 never rebroadcasts, so node 0's broadcast is never acknowledged. */
static uint64_t sends_to_a_mute_neighbour(uint8_t retries, bool want_ack, uint32_t dst) {
    struct rig r;
    rig_init(&r);
    r.rc.retries = retries;
    r.rc.want_ack = want_ack;
    /* Every node takes the same config, so node 0 is mute too, which changes nothing for a node
     * that only sends. */
    r.rc.role = TSIM_MESHTASTIC_CLIENT_MUTE;
    build(&r, 2, 1);
    link(&r, 0, 1, LOSS_LOUD);
    tsim_net_start(r.net);
    uint64_t m = tsim_net_originate(r.net, 0, dst, 10);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1); /* once, however often it came */
    uint64_t sent = frames(&r, 0, TSIM_PURPOSE_DATA);
    rig_close(&r);
    return sent;
}

static void an_unacknowledged_message_is_retried_and_an_acknowledged_one_is_not(void) {
    CHECK_EQ_U64(sends_to_a_mute_neighbour(3, true, TSIM_BROADCAST), 4);
    CHECK_EQ_U64(sends_to_a_mute_neighbour(0, true, TSIM_BROADCAST), 1);
    CHECK_EQ_U64(sends_to_a_mute_neighbour(3, false, TSIM_BROADCAST), 1);
    CHECK_EQ_U64(sends_to_a_mute_neighbour(3, true, 1), 1); /* node 1's real acknowledgement */
}

/* Node 0 listens on SF8 and sends on SF7, so it never hears an acknowledgement and retries every
 * time; node 1 hears each copy. How many acknowledgements does node 1 send? */
static uint64_t acks_to_a_deaf_sender(bool ack_duplicates) {
    struct rig r;
    rig_init(&r);
    r.rc.ack_duplicates = ack_duplicates;
    build(&r, 2, 1);
    link(&r, 0, 1, LOSS_LOUD);
    tsim_net_start(r.net);
    struct tsim_lora sf8 = tsim_lora_default(8, 125000);
    CHECK(tsim_node_tune(tsim_net_node(r.net, 0), 0, &sf8));
    uint64_t m = tsim_net_originate(r.net, 0, 1, 10);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), 4); /* the first and three retries */
    CHECK_EQ_U64(tsim_net_message(r.net, m)->delivered, 1);
    uint64_t acks = frames(&r, 1, TSIM_PURPOSE_CONTROL);
    rig_close(&r);
    return acks;
}

/* A sender that missed the acknowledgement retries, and the destination answers again. */
static void a_retry_that_reaches_the_destination_is_acknowledged_again(void) {
    CHECK_EQ_U64(acks_to_a_deaf_sender(true), 4);
    CHECK_EQ_U64(acks_to_a_deaf_sender(false), 1); /* Meshtasticator's way */
}

/* A router at cw 0 has a rebroadcast ready at once, while CAD finds the channel busy with a frame
 * it came in too late to receive. It must wait until that frame ends, not look again at the same
 * instant for ever. */
static void a_busy_channel_always_moves_the_wait_on(void) {
    struct rig r;
    rig_init(&r);
    r.rc.role = TSIM_MESHTASTIC_ROUTER;
    r.rc.want_ack = false;
    r.rc.window.cw_min = r.rc.window.cw_max = 0;
    r.mc.window.cw_min = r.mc.window.cw_max = 0;
    build(&r, 3, 1);
    link(&r, 0, 1, 94.0);  /* node 0's frame arrives at -80 dBm */
    link(&r, 2, 1, 114.0); /* node 2's at -100 dBm: under node 0's, but there for CAD */
    tsim_net_start(r.net);
    struct tsim_node *n0 = tsim_net_node(r.net, 0);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    while (!tsim_node_sending(n0) && tsim_sched_step(r.sched)) {
    }
    /* 15 ms into node 0's 62 ms frame, after node 1 has locked on to it, node 2 starts a 330 ms one
     * (within one 10 ms slot of being asked to). */
    tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_MS(15));
    tsim_net_originate(r.net, 2, TSIM_BROADCAST, 200);
    /* Looking again at the same instant would never let the run end. */
    int steps = 0;
    while (steps < 100000 && tsim_sched_step(r.sched)) {
        steps++;
    }
    CHECK(steps < 100000);
    CHECK_EQ_U64(frames(&r, 1, TSIM_PURPOSE_RELAY), 1);
    CHECK(tsim_node_rx_airtime(tsim_net_node(r.net, 1)) > 0);
    rig_close(&r);
}

/* The wait before a node's own first frame, on a channel it has not used. */
static tsim_time first_wait(uint64_t seed) {
    struct rig r;
    rig_init(&r);
    line(&r, 2, seed);
    uint64_t m = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(1));
    tsim_time wait = tsim_net_message(r.net, m)->first - airtime(TSIM_MESHTASTIC_OVERHEAD + 10);
    rig_close(&r);
    return wait;
}

static void a_nodes_own_frame_waits_up_to_two_to_the_cw_min_slots(void) {
    struct tsim_lora l = lora();
    tsim_time slot = tsim_meshtastic_slot(&l);
    bool waited = false;
    for (uint64_t seed = 1; seed <= 40; seed++) {
        tsim_time w = first_wait(seed);
        CHECK(w >= 0 && w <= 8 * slot && w % slot == 0);
        waited |= w > 0;
    }
    CHECK(waited);
}

/* The wait node 1 drew before rebroadcasting what node 0 sent, in slots. */
static tsim_time relay_wait_by(enum tsim_meshtastic_role role, double loss, double noise_dbm,
                               uint64_t seed) {
    struct rig r;
    rig_init(&r);
    r.rc.role = role;
    r.rc.noise_dbm = noise_dbm;
    build(&r, 3, seed);
    link(&r, 0, 1, loss);
    link(&r, 1, 2, LOSS_LOUD);
    tsim_net_start(r.net);
    uint64_t m = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    const struct tsim_message_record *rec = tsim_net_message(r.net, m);
    CHECK_EQ_U64(rec->delivered, 2);
    /* Node 1 had it at `first`; node 2 at the end of node 1's rebroadcast. */
    tsim_time wait = rec->last - airtime(TSIM_MESHTASTIC_OVERHEAD + 10) - rec->first;
    rig_close(&r);
    tsim_time slot = r.mc.window.slot;
    CHECK(wait % slot == 0);
    return wait / slot;
}

static tsim_time relay_wait(enum tsim_meshtastic_role role, double loss, uint64_t seed) {
    return relay_wait_by(role, loss, NAN, seed);
}

static void a_rebroadcast_waits_longer_the_louder_it_was_heard(void) {
    tsim_time faint_max = 0, loud_max = 0;
    for (uint64_t seed = 1; seed <= 40; seed++) {
        /* A client waits out the longest a router could, 2 cw_max = 16 slots, then 0 to 2^cw. */
        tsim_time faint = relay_wait(TSIM_MESHTASTIC_CLIENT, LOSS_FAINT, seed); /* cw 3 */
        tsim_time loud = relay_wait(TSIM_MESHTASTIC_CLIENT, LOSS_LOUD, seed);   /* cw 8 */
        CHECK(faint >= 16 && faint <= 16 + 8);
        CHECK(loud >= 16 && loud <= 16 + 256);
        faint_max = faint > faint_max ? faint : faint_max;
        loud_max = loud > loud_max ? loud : loud_max;
        /* A router waits 0 to 2 cw. */
        CHECK(relay_wait(TSIM_MESHTASTIC_ROUTER, LOSS_FAINT, seed) <= 6);
        CHECK(relay_wait(TSIM_MESHTASTIC_ROUTER, LOSS_LOUD, seed) <= 16);
    }
    CHECK(loud_max > 16 + 8);
    CHECK(faint_max > 16);
}

/* Reckoned from a noise floor 31 dB under its RSSI, a faint rebroadcast waits as a loud one does.
 */
static void a_rebroadcasts_snr_can_be_reckoned_from_a_fixed_noise_floor(void) {
    double noise = 14.0 - LOSS_FAINT - 31.0;
    tsim_time most = 0;
    for (uint64_t seed = 1; seed <= 40; seed++) {
        tsim_time w = relay_wait_by(TSIM_MESHTASTIC_CLIENT, LOSS_FAINT, noise, seed);
        CHECK(w >= 16 && w <= 16 + 256);
        most = w > most ? w : most;
    }
    CHECK(most > 16 + 8); /* past the faint window's 8 slots */
}

/* When node 0's messages were finished. */
struct finishes {
    struct tsim_sched *sched;
    int count;
    tsim_time at;
};

static void on_finished(void *ctx, const struct tsim_message_record *rec) {
    struct finishes *f = ctx;
    (void)rec;
    f->count++;
    f->at = tsim_sched_now(f->sched);
}

/* The wait for an acknowledgement of a 10-byte message on a quiet channel: two airtimes, 8 + 16 +
 * 32 slots and the processing time. */
static tsim_time quiet_ack_wait(void) {
    struct tsim_lora l = lora();
    return 2 * airtime(TSIM_MESHTASTIC_OVERHEAD + 10) + 56 * tsim_meshtastic_slot(&l) +
           TSIM_MS(4500);
}

/* Node 0 sends one broadcast; node 1 rebroadcasts it unless mute. When was it finished? */
static struct finishes finish_of(bool ack_poll, bool mute, uint8_t retries, bool want_ack) {
    struct rig r;
    rig_init(&r);
    r.rc.ack_poll = ack_poll;
    r.rc.retries = retries;
    r.rc.want_ack = want_ack;
    r.rc.role = mute ? TSIM_MESHTASTIC_CLIENT_MUTE : TSIM_MESHTASTIC_CLIENT;
    line(&r, 2, 1);
    struct finishes f = {r.sched, 0, -1};
    tsim_net_observe_finished(r.net, on_finished, &f);
    uint64_t m = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    if (!want_ack) {
        CHECK(f.count == 1 && f.at == 0); /* at once */
    }
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK(tsim_net_message(r.net, m)->finished);
    CHECK_EQ_U64(frames(&r, 0, TSIM_PURPOSE_DATA), mute ? 1u + retries : 1u);
    rig_close(&r);
    return f;
}

/* Finished when the rebroadcast is heard, or when the last retry's wait runs out. */
static void a_message_is_finished_when_acknowledged_or_given_up_on(void) {
    struct finishes f = finish_of(false, false, 3, true);
    CHECK(f.count == 1 && f.at > 0 && f.at < quiet_ack_wait());
    f = finish_of(false, true, 1, true);
    CHECK(f.count == 1 && f.at > 2 * quiet_ack_wait()); /* each wait from when its frame went */
    finish_of(false, false, 3, false);
}

/* Polling, the wait starts when the frame is queued, and the acknowledgement that came during it
 * is noticed only when it ends. */
static void polling_waits_from_the_queue_and_notices_an_acknowledgement_at_the_end(void) {
    struct finishes f = finish_of(true, false, 3, true);
    CHECK(f.count == 1 && f.at == quiet_ack_wait());
    f = finish_of(true, true, 1, true);
    CHECK(f.count == 1 && f.at == 2 * quiet_ack_wait());
}

/* Five clients that all hear each other: once the first rebroadcast has been heard, the other
 * three are no longer wanted. Are they still queued, and how many were cancelled, then and at the
 * end? */
struct late {
    uint64_t queued_then;
    uint64_t cancelled_then;
    uint64_t cancelled;
    uint64_t relays;
};

static uint64_t relays_sent(const struct rig *r) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < 5; i++) {
        n += frames(r, i, TSIM_PURPOSE_RELAY);
    }
    return n;
}

static struct late cancelling(bool late) {
    struct rig r;
    rig_init(&r);
    r.rc.cancel_late = late;
    build(&r, 5, 1);
    for (uint32_t a = 0; a < 5; a++) {
        for (uint32_t b = a + 1; b < 5; b++) {
            link(&r, a, b, LOSS_LOUD);
        }
    }
    tsim_net_start(r.net);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    while (relays_sent(&r) == 0 && tsim_sched_now(r.sched) < TSIM_S(60)) {
        tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + TSIM_MS(1));
    }
    tsim_sched_run_until(r.sched, tsim_sched_now(r.sched) + airtime(TSIM_MESHTASTIC_OVERHEAD + 10) +
                                      TSIM_MS(1));
    struct late out = {0};
    for (uint32_t i = 1; i < 5; i++) {
        out.queued_then += tsim_node_queue_length(tsim_net_node(r.net, i));
        out.cancelled_then += tsim_net_stats(r.net, i)->cancelled;
    }
    tsim_sched_run_until(r.sched, TSIM_S(60));
    for (uint32_t i = 1; i < 5; i++) {
        out.cancelled += tsim_net_stats(r.net, i)->cancelled;
        CHECK(tsim_node_queue_length(tsim_net_node(r.net, i)) == 0);
    }
    out.relays = relays_sent(&r);
    rig_close(&r);
    return out;
}

static void cancelling_late_waits_for_the_frames_turn(void) {
    struct late now = cancelling(false);
    CHECK(now.queued_then == 0 && now.cancelled_then == 3);
    CHECK(now.cancelled == 3 && now.relays == 1);
    struct late late = cancelling(true);
    CHECK(late.queued_then == 3 && late.cancelled_then == 0);
    CHECK(late.cancelled == 3 && late.relays == 1);
}

/* How many frames node 0 sends in a minute, when the channel looks busy with this chance. */
static uint64_t sent_when_busy(double chance) {
    struct rig r;
    rig_init(&r);
    r.mc.busy_chance = chance;
    line(&r, 2, 1);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    uint64_t sent = frames(&r, 0, TSIM_PURPOSE_DATA);
    rig_close(&r);
    return sent;
}

static void outside_traffic_holds_the_mac_off(void) {
    CHECK_EQ_U64(sent_when_busy(1.0), 0);
    CHECK_EQ_U64(sent_when_busy(0.5), 1);
}

/* Node 1 has a frame of its own to send while node 0's long frame is coming in: it waits for it. */
static void the_mac_holds_off_while_the_radio_is_receiving(void) {
    struct rig r;
    rig_init(&r);
    r.rc.hop_limit = 0;
    r.rc.want_ack = false;
    line(&r, 3, 1);
    uint64_t lng = tsim_net_originate(r.net, 0, TSIM_BROADCAST, 200);
    tsim_sched_run_until(r.sched, TSIM_MS(200)); /* past any first wait: on the air */
    uint64_t own = tsim_net_originate(r.net, 1, 2, 10);
    tsim_sched_run_until(r.sched, TSIM_S(60));
    const struct tsim_message_record *a = tsim_net_message(r.net, lng);
    const struct tsim_message_record *b = tsim_net_message(r.net, own);
    CHECK_EQ_U64(a->delivered, 1);
    CHECK_EQ_U64(b->delivered, 1);
    CHECK(b->first - airtime(TSIM_MESHTASTIC_OVERHEAD + 10) >= a->first);
    rig_close(&r);
}

static void a_seed_repeats_a_run(void) {
    uint64_t sent[2][4];
    for (int run = 0; run < 2; run++) {
        struct rig r;
        rig_init(&r);
        line(&r, 4, 7);
        for (uint32_t i = 0; i < 4; i++) {
            tsim_net_originate(r.net, i, TSIM_BROADCAST, 20);
        }
        tsim_sched_run_until(r.sched, TSIM_S(120));
        for (uint32_t i = 0; i < 4; i++) {
            sent[run][i] = tsim_net_stats(r.net, i)->queued;
        }
        rig_close(&r);
    }
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_U64(sent[0][i], sent[1][i]);
    }
}

static void bad_configs_are_refused(void) {
    struct rig r;
    struct tsim_net_params p = tsim_net_defaults(1);
    struct tsim_sched *sched = tsim_sched_create();

    rig_init(&r);
    r.rc.hop_limit = TSIM_MESHTASTIC_HOPS_MAX + 1;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    rig_init(&r);
    r.rc.window.cw_min = 9;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    rig_init(&r);
    r.mc.window.cw_max = 16;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    rig_init(&r);
    r.mc.snr_max_db = r.mc.snr_min_db;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    /* A zero slot would have a MAC that finds the channel busy wait again at the same instant,
     * for ever. */
    rig_init(&r);
    r.mc.window.slot = 0;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    rig_init(&r);
    r.rc.window.slot = 0;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    rig_init(&r);
    r.rc.processing = TSIM_MESHTASTIC_WAIT_MAX + 1;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    rig_init(&r);
    r.rc.noise_dbm = INFINITY;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    rig_init(&r);
    r.mc.busy_chance = 1.5;
    CHECK(!tsim_net_create(sched, &p, 2, &tsim_meshtastic, &r.rc, &tsim_meshtastic_mac, &r.mc));
    tsim_sched_destroy(sched);
}

/* The longest window either plugin accepts: every wait drawn from it fits in a time. */
static void the_longest_window_fits_the_clock(void) {
    struct tsim_meshtastic_window w = {.slot = 1, .cw_min = 15, .cw_max = 15};
    CHECK_EQ_U64(tsim_meshtastic_window_slots(&w), 65536 + 30);
    w.slot = (tsim_time)(TSIM_MESHTASTIC_WAIT_MAX / tsim_meshtastic_window_slots(&w));
    CHECK(tsim_meshtastic_window_valid(&w));
    w.slot++;
    CHECK(!tsim_meshtastic_window_valid(&w));
    w.slot--;

    /* Run on it: a client's rebroadcast waits the most, and the acknowledgement wait is sized
     * from it, so both are drawn. */
    struct rig r;
    rig_init(&r);
    r.rc.window = w;
    r.rc.processing = TSIM_MESHTASTIC_WAIT_MAX;
    r.mc.window = w;
    line(&r, 3, 1);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 10);
    tsim_net_originate(r.net, 2, 0, 10);
    for (int i = 0; i < 64 && tsim_sched_step(r.sched); i++) {
    }
    rig_close(&r);
}

/* Destroying the network with messages still waiting for acknowledgement frees their timers. */
static void destroy_mid_flood_leaves_the_scheduler_runnable(void) {
    struct rig r;
    rig_init(&r);
    line(&r, 5, 1);
    tsim_net_originate(r.net, 0, TSIM_BROADCAST, 50);
    tsim_net_originate(r.net, 4, 0, 50);
    tsim_sched_run_until(r.sched, TSIM_MS(500));
    tsim_net_destroy(r.net);
    tsim_sched_run_until(r.sched, TSIM_S(600));
    CHECK_EQ_U64(tsim_sched_size(r.sched), 0);
    tsim_sched_destroy(r.sched);
}

int main(void) {
    RUN(slot_is_cad_plus_the_firmware_allowance);
    RUN(a_broadcast_crosses_the_hop_limit_and_no_further);
    RUN(a_message_one_frame_cannot_carry_is_refused);
    RUN(a_second_copy_cancels_a_clients_rebroadcast_and_a_third_a_routers);
    RUN(a_direct_message_is_acknowledged_back_along_the_flood);
    RUN(an_unacknowledged_message_is_retried_and_an_acknowledged_one_is_not);
    RUN(a_retry_that_reaches_the_destination_is_acknowledged_again);
    RUN(a_busy_channel_always_moves_the_wait_on);
    RUN(a_nodes_own_frame_waits_up_to_two_to_the_cw_min_slots);
    RUN(a_rebroadcast_waits_longer_the_louder_it_was_heard);
    RUN(a_rebroadcasts_snr_can_be_reckoned_from_a_fixed_noise_floor);
    RUN(outside_traffic_holds_the_mac_off);
    RUN(the_mac_holds_off_while_the_radio_is_receiving);
    RUN(a_message_is_finished_when_acknowledged_or_given_up_on);
    RUN(polling_waits_from_the_queue_and_notices_an_acknowledgement_at_the_end);
    RUN(cancelling_late_waits_for_the_frames_turn);
    RUN(a_seed_repeats_a_run);
    RUN(bad_configs_are_refused);
    RUN(the_longest_window_fits_the_clock);
    RUN(destroy_mid_flood_leaves_the_scheduler_runnable);
    return CHECK_DONE();
}
