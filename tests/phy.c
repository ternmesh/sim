#include "tsim/phy.h"

#include <math.h>
#include <stdlib.h>

#include "check.h"

/* Every scenario sends at 14 dBm and sets the link losses by hand, so the power each receiver
 * sees is exact. Node 0 is the receiver under test unless a test says otherwise.
 *
 * Reference numbers, SF7/125 kHz, 16 bytes: a symbol is 1.024 ms, the frame 51.456 ms, the
 * receiver locks 5.12 ms after the preamble starts, and the last moment to start listening and
 * still lock is 7.25 symbols (7.424 ms) in. The noise floor is -117.03 dBm, so SF7 decodes down
 * to -124.53 dBm. */

#define TX_DBM 14.0
#define SF7_FRAME TSIM_NS(51456000)

struct rx_log {
    int count;
    uint32_t node[64];
    uint64_t frame[64];
    tsim_time at[64];
    int tx_done;
};

struct world {
    struct tsim_sched *sched;
    struct tsim_phy *phy;
    struct rx_log log;
    struct tsim_lora sf7;
    struct tsim_lora sf9;
};

static void on_rx(void *ctx, uint32_t node, const struct tsim_frame *f, double rssi, double snr) {
    (void)rssi;
    (void)snr;
    struct world *w = ctx;
    if (w->log.count < 64) {
        w->log.node[w->log.count] = node;
        w->log.frame[w->log.count] = f->id;
        w->log.at[w->log.count] = tsim_sched_now(w->sched);
        w->log.count++;
    }
}

static void on_tx_done(void *ctx, uint32_t node, const struct tsim_frame *f) {
    (void)node;
    (void)f;
    struct world *w = ctx;
    w->log.tx_done++;
}

static struct world *world_new(uint32_t nodes, const struct tsim_phy_params *params) {
    struct world *w = calloc(1, sizeof *w);
    w->sched = tsim_sched_create();
    w->sf7 = tsim_lora_default(7, 125000);
    w->sf9 = tsim_lora_default(9, 125000);
    struct tsim_phy_params defaults = tsim_phy_defaults();
    w->phy = tsim_phy_create(w->sched, params ? params : &defaults, nodes, 0, &w->sf7,
                             (struct tsim_phy_hooks){on_rx, on_tx_done, w});
    return w;
}

static void world_free(struct world *w) {
    tsim_phy_destroy(w->phy);
    tsim_sched_destroy(w->sched);
    free(w);
}

/* Makes node `from` arrive at node `to` at `dbm`. */
static void arrive(struct world *w, uint32_t from, uint32_t to, double dbm) {
    tsim_phy_set_loss(w->phy, from, to, TX_DBM - dbm);
}

struct send {
    struct world *w;
    uint32_t node;
    uint16_t channel;
    const struct tsim_lora *lora;
    uint32_t len;
};

static void send_now(struct tsim_sched *s, void *ctx) {
    (void)s;
    struct send *x = ctx;
    tsim_phy_transmit(x->w->phy, x->node, x->channel, x->lora, x->len, TX_DBM, NULL);
}

/* Schedules a 16-byte frame from `node` at `at`. The send has to outlive the run. */
static void send_at(struct world *w, struct send *x, tsim_time at, uint32_t node,
                    const struct tsim_lora *lora) {
    *x = (struct send){w, node, 0, lora, 16};
    tsim_sched_at(w->sched, at, send_now, x);
}

static bool received(const struct world *w, uint32_t node, uint64_t frame) {
    for (int i = 0; i < w->log.count; i++) {
        if (w->log.node[i] == node && w->log.frame[i] == frame) {
            return true;
        }
    }
    return false;
}

static void delivers_at_the_end_of_the_frame(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    uint64_t id = tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    CHECK_EQ_I64(id, 1);
    CHECK(tsim_phy_transmitting(w->phy, 1));
    CHECK(tsim_phy_receiving(w->phy, 0));
    CHECK(!tsim_phy_cad(w->phy, 1)); /* a radio does not hear itself */
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(w->log.count, 1);
    CHECK_EQ_I64(w->log.at[0], SF7_FRAME);
    CHECK_EQ_I64(w->log.tx_done, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 1)->tx, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 1)->tx_airtime, SF7_FRAME);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok, 1);
    CHECK(!tsim_phy_transmitting(w->phy, 1));
    CHECK(!tsim_phy_receiving(w->phy, 0));
    world_free(w);
}

static void hears_down_to_the_demodulation_floor(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -124.4); /* SNR -7.37 dB */
    arrive(w, 2, 0, -124.6); /* SNR -7.57 dB */
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    tsim_phy_transmit(w->phy, 2, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_S(2));
    CHECK(received(w, 0, 1));
    CHECK(!received(w, 0, 2));
    world_free(w);
}

/* A radio hears only the SF it is tuned to; the frame on another SF is not even detected. */
static void hears_only_its_tuning(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -60.0);
    tsim_phy_transmit(w->phy, 1, 0, &w->sf9, 16, TX_DBM, NULL);
    CHECK(!tsim_phy_receiving(w->phy, 0));
    CHECK(!tsim_phy_cad(w->phy, 0));
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(w->log.count, 0);

    CHECK(tsim_phy_tune(w->phy, 0, 0, &w->sf9));
    tsim_sched_run_until(w->sched, TSIM_S(2));
    tsim_phy_transmit(w->phy, 1, 0, &w->sf9, 16, TX_DBM, NULL);
    CHECK(tsim_phy_cad(w->phy, 0));
    tsim_sched_run_until(w->sched, TSIM_S(3));
    CHECK(received(w, 0, 2));
    CHECK(!tsim_phy_cad(w->phy, 0));
    world_free(w);
}

static void other_channels_do_not_interfere(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -100.0);
    arrive(w, 2, 0, -40.0);
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_phy_transmit(w->phy, 2, 1, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1));
    CHECK_EQ_I64(w->log.count, 1);
    world_free(w);
}

static void half_duplex(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -80.0);
    /* Transmitting when a frame starts: never heard. */
    tsim_phy_transmit(w->phy, 0, 0, &w->sf9, 16, TX_DBM, NULL);
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    CHECK_EQ_I64(tsim_phy_transmit(w->phy, 0, 0, &w->sf7, 16, TX_DBM, NULL), 0);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(!received(w, 0, 2));

    /* Starting to transmit mid-frame: the reception is abandoned. */
    struct send x;
    send_at(w, &x, TSIM_S(1) + TSIM_MS(10), 0, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    tsim_phy_transmit(w->phy, 2, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_S(2));
    CHECK(!received(w, 0, 3));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 1);
    world_free(w);
}

/* A louder frame that arrives after the receiver has locked cannot take it, but it can still
 * ruin the frame it is locked on. */
static void louder_frame_after_lock_destroys_but_is_not_heard(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -70.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(20), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    /* SIR -10 dB, + 2.14 dB because B covers 31.456 of A's 51.456 ms: below 6 dB. */
    CHECK_EQ_I64(w->log.count, 0);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_lost, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_preempted, 0);
    world_free(w);
}

static void louder_frame_during_the_preamble_takes_the_receiver(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -70.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(2), 2, &w->sf7); /* before the 5.12 ms lock */
    tsim_sched_run_until(w->sched, TSIM_S(1));
    /* B survives A: SIR 10 dB, + 0.17 dB for the 2 ms of B that A missed. */
    CHECK_EQ_I64(w->log.count, 1);
    CHECK(received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_preempted, 1);
    world_free(w);
}

static void slightly_louder_frame_does_not_take_the_receiver(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -77.0); /* 3 dB louder, under the 6 dB capture threshold */
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(2), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(w->log.count, 0);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_preempted, 0);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_lost, 1);
    world_free(w);
}

static void quieter_late_frame_is_survived(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -70.0);
    arrive(w, 2, 0, -80.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(20), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1));
    CHECK(!received(w, 0, 2));
    world_free(w);
}

/* Each interferer alone leaves 8.54 dB; two together leave 5.53 dB, under the 6 dB threshold.
 * A pairwise model would have kept the frame. */
static void interference_is_summed(void) {
    for (int interferers = 1; interferers <= 2; interferers++) {
        struct world *w = world_new(4, NULL);
        arrive(w, 1, 0, -70.0);
        arrive(w, 2, 0, -78.0);
        arrive(w, 3, 0, -78.0);
        struct send s[3];
        send_at(w, &s[0], 0, 1, &w->sf7);
        send_at(w, &s[1], TSIM_MS(6), 2, &w->sf7);
        if (interferers == 2) {
            send_at(w, &s[2], TSIM_MS(6), 3, &w->sf7);
        }
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1) == (interferers == 1));
        world_free(w);
    }
}

/* Interference counts for the part of the frame it overlaps. A frame 5 dB louder covering the
 * last tenth leaves 5 dB; covering the last twentieth, 8 dB. */
static void interference_is_weighted_by_overlap(void) {
    for (int twentieths = 1; twentieths <= 2; twentieths++) {
        struct world *w = world_new(3, NULL);
        arrive(w, 1, 0, -80.0);
        arrive(w, 2, 0, -75.0);
        struct send s[2];
        send_at(w, &s[0], 0, 1, &w->sf7);
        send_at(w, &s[1], SF7_FRAME - SF7_FRAME * twentieths / 20, 2, &w->sf7);
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1) == (twentieths == 1));
        world_free(w);
    }
}

/* An SF9 frame on the same channel is not heard by an SF7 receiver but still interferes, up to
 * the isolation threshold: SF7 survives SF9 at -18 dB SIR and not below. */
static void other_sfs_interfere_up_to_their_isolation(void) {
    double louder[2] = {15.0, 20.0};
    for (int i = 0; i < 2; i++) {
        struct world *w = world_new(3, NULL);
        arrive(w, 1, 0, -90.0);
        arrive(w, 2, 0, -90.0 + louder[i]);
        struct send s[2];
        send_at(w, &s[0], TSIM_MS(1), 1, &w->sf7);
        send_at(w, &s[1], 0, 2, &w->sf9); /* SF9 16 B is 164.9 ms: covers the whole SF7 frame */
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 2) == (i == 0));
        world_free(w);
    }
}

/* A receiver that starts listening part-way through a preamble locks on if at least
 * lock_symbols of it remain (here, until 7.424 ms in). Retuning to SF7 at 3 ms, with 1 ms of
 * deafness, catches it; at 7 ms it is too late. */
static void retuning_catches_a_preamble_only_in_time(void) {
    tsim_time retune_at[2] = {TSIM_MS(3), TSIM_MS(7)};
    for (int i = 0; i < 2; i++) {
        struct world *w = world_new(2, NULL);
        arrive(w, 1, 0, -80.0);
        tsim_phy_tune(w->phy, 0, 0, &w->sf9);
        tsim_sched_run_until(w->sched, TSIM_MS(10));
        tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
        tsim_sched_run_until(w->sched, TSIM_MS(10) + retune_at[i]);
        tsim_phy_tune(w->phy, 0, 0, &w->sf7);
        CHECK(!tsim_phy_cad(w->phy, 0)); /* deaf while retuning */
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1) == (i == 0));
        world_free(w);
    }
}

/* A preamble shorter than the lock needs cannot be locked on, even by a receiver listening from
 * its first symbol: 0 programmed symbols is 4.25 on air, under the 5 the default lock needs. */
static void preamble_too_short_to_lock_is_not_heard(void) {
    for (uint16_t preamble = 0; preamble <= 1; preamble++) {
        struct world *w = world_new(2, NULL);
        arrive(w, 1, 0, -80.0);
        struct tsim_lora short_preamble = w->sf7;
        short_preamble.preamble = preamble;
        tsim_phy_transmit(w->phy, 1, 0, &short_preamble, 16, TX_DBM, NULL);
        CHECK(tsim_phy_receiving(w->phy, 0) == (preamble == 1));
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1) == (preamble == 1));
        world_free(w);
    }
}

/* Transmitting on a modulation other than the one it listens on costs the radio a retune when the
 * frame ends; on the same one it listens straight away. */
static void transmitting_off_tuning_costs_a_retune(void) {
    tsim_time retune[2] = {TSIM_MS(1), TSIM_MS(10)};
    for (int i = 0; i < 2; i++) {
        struct tsim_phy_params p = tsim_phy_defaults();
        p.retune = retune[i];
        struct world *w = world_new(2, &p);
        arrive(w, 1, 0, -80.0);
        tsim_time sf9_frame = tsim_lora_airtime(&w->sf9, 16);
        tsim_phy_transmit(w->phy, 0, 0, &w->sf9, 16, TX_DBM, NULL);
        struct send s;
        send_at(w, &s, sf9_frame, 1, &w->sf7);
        tsim_sched_run_until(w->sched, TSIM_S(2));
        CHECK(received(w, 0, 2) == (i == 0));
        world_free(w);
    }
    struct tsim_phy_params p = tsim_phy_defaults();
    p.retune = TSIM_MS(10);
    struct world *w = world_new(2, &p);
    arrive(w, 1, 0, -80.0);
    tsim_phy_transmit(w->phy, 0, 0, &w->sf7, 16, TX_DBM, NULL);
    struct send s;
    send_at(w, &s, SF7_FRAME, 1, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(2));
    CHECK(received(w, 0, 2));
    world_free(w);
}

/* After a reception ends, the receiver can still lock on to a frame whose preamble is under way,
 * and judges it only on what it heard: A, 30 dB louder, overlapped B's first 2 ms, before the
 * receiver was listening to B, and does not count against it. A third frame that overlaps B once
 * the receiver is on it does. */
static void listens_again_after_a_reception(void) {
    for (int late = 0; late <= 1; late++) {
        struct world *w = world_new(4, NULL);
        arrive(w, 1, 0, -70.0);
        arrive(w, 2, 0, -100.0);
        arrive(w, 3, 0, -90.0);
        struct send s[3];
        send_at(w, &s[0], 0, 1, &w->sf7);
        send_at(w, &s[1], SF7_FRAME - TSIM_MS(2), 2, &w->sf7);
        if (late) {
            send_at(w, &s[2], SF7_FRAME + TSIM_MS(20), 3, &w->sf7);
        }
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1)); /* -30 dB of interference for 2 ms is nothing */
        CHECK(received(w, 0, 2) == !late);
        world_free(w);
    }
}

/* A relay that retransmits from inside the rx hook: the frame it repeats is on the air at once,
 * and the run stays consistent. */
struct relay {
    struct world *w;
    struct tsim_lora lora;
    int sent;
};

static void relay_rx(void *ctx, uint32_t node, const struct tsim_frame *f, double rssi,
                     double snr) {
    (void)rssi;
    (void)snr;
    struct relay *r = ctx;
    on_rx(r->w, node, f, rssi, snr);
    if (node == 1 && !r->sent) {
        r->sent = 1;
        tsim_phy_transmit(r->w->phy, 1, 0, &r->lora, 16, TX_DBM, NULL);
    }
}

static void relay_tx_done(void *ctx, uint32_t node, const struct tsim_frame *f) {
    struct relay *r = ctx;
    on_tx_done(r->w, node, f);
}

static void hooks_may_transmit(void) {
    struct world *w = world_new(3, NULL);
    struct relay r = {w, w->sf7, 0};
    tsim_phy_destroy(w->phy);
    struct tsim_phy_params p = tsim_phy_defaults();
    w->phy = tsim_phy_create(w->sched, &p, 3, 0, &w->sf7,
                             (struct tsim_phy_hooks){relay_rx, relay_tx_done, &r});
    arrive(w, 0, 1, -80.0);
    arrive(w, 1, 2, -80.0); /* 2 cannot hear 0 */
    tsim_phy_transmit(w->phy, 0, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 1, 1));
    CHECK(received(w, 2, 2));
    CHECK_EQ_I64(w->log.at[1], 2 * SF7_FRAME);
    world_free(w);
}

/* Losses filled from the channel model are its losses, symmetric, with no link to itself. */
static void losses_from_the_channel_model(void) {
    struct tsim_sched *s = tsim_sched_create();
    struct tsim_phy_params p = tsim_phy_defaults();
    struct tsim_lora sf7 = tsim_lora_default(7, 125000);
    struct tsim_phy *phy = tsim_phy_create(s, &p, 6, 0, &sf7, (struct tsim_phy_hooks){0});
    struct tsim_pos pos[6];
    for (int i = 0; i < 6; i++) {
        pos[i] = (struct tsim_pos){1000.0 * i, 0};
    }
    struct tsim_channel_params ch = tsim_channel_default(5);
    tsim_phy_set_losses(phy, &ch, pos);
    CHECK(tsim_phy_loss(phy, 0, 5) == tsim_phy_loss(phy, 5, 0));
    CHECK(fabs(tsim_phy_loss(phy, 1, 4) - tsim_channel_loss(&ch, 1, pos[1], 4, pos[4])) < 1e-9);
    CHECK(isinf(tsim_phy_loss(phy, 2, 2)));
    tsim_phy_destroy(phy);
    tsim_sched_destroy(s);
}

/* The scheduler can outlive the medium: destroying it with a frame on the air and a retune under
 * way leaves no event behind that points into it. */
static void destroy_cancels_pending_radio_events(void) {
    struct world *w = world_new(2, NULL);
    tsim_phy_transmit(w->phy, 0, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_phy_tune(w->phy, 1, 0, &w->sf9);
    CHECK_EQ_I64(tsim_sched_size(w->sched), 2);
    tsim_phy_destroy(w->phy);
    w->phy = NULL;
    CHECK_EQ_I64(tsim_sched_size(w->sched), 0);
    CHECK_EQ_I64(tsim_sched_run_until(w->sched, TSIM_S(1)), 0);
    world_free(w);
}

int main(void) {
    RUN(delivers_at_the_end_of_the_frame);
    RUN(hears_down_to_the_demodulation_floor);
    RUN(hears_only_its_tuning);
    RUN(other_channels_do_not_interfere);
    RUN(half_duplex);
    RUN(louder_frame_after_lock_destroys_but_is_not_heard);
    RUN(louder_frame_during_the_preamble_takes_the_receiver);
    RUN(slightly_louder_frame_does_not_take_the_receiver);
    RUN(quieter_late_frame_is_survived);
    RUN(interference_is_summed);
    RUN(interference_is_weighted_by_overlap);
    RUN(other_sfs_interfere_up_to_their_isolation);
    RUN(retuning_catches_a_preamble_only_in_time);
    RUN(preamble_too_short_to_lock_is_not_heard);
    RUN(transmitting_off_tuning_costs_a_retune);
    RUN(listens_again_after_a_reception);
    RUN(hooks_may_transmit);
    RUN(losses_from_the_channel_model);
    RUN(destroy_cancels_pending_radio_events);
    return CHECK_DONE();
}
