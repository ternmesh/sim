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
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 1);
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
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
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
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 1);
    world_free(w);
}

/* A frame that begins while the radio is busy with another is missed only if the radio could have
 * decoded it, and only by a radio that is receiving, not one that is transmitting. */
static void missed_counts_only_frames_it_could_have_decoded(void) {
    struct world *w = world_new(4, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -140.0); /* under the floor */
    arrive(w, 3, 0, -100.0);
    arrive(w, 3, 1, -100.0);
    struct send s[3];
    send_at(w, &s[0], 0, 1, &w->sf7);
    send_at(w, &s[1], TSIM_MS(10), 2, &w->sf7);
    send_at(w, &s[2], TSIM_MS(20), 3, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 1)->rx_missed, 0);
    world_free(w);
}

/* B begins 1 ms before A ends, while node 0 is on A: missed then, but caught when A ends, early in
 * its preamble. It counts once, as received. */
static void a_frame_caught_after_all_is_not_missed(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -80.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, SF7_FRAME - TSIM_MS(1), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1) && received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok, 2);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
    world_free(w);
}

/* The same, but node 0 is sending A: B was never counted missed, so catching it takes nothing
 * back. */
static void a_frame_caught_after_sending_was_never_missed(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 2, 0, -80.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 0, &w->sf7);
    send_at(w, &b, SF7_FRAME - TSIM_MS(1), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
    world_free(w);
}

static void tune_now(struct tsim_sched *s, void *ctx);

/* Node 0 gives up A to send its own frame; B begins in the last 1 ms of it, while node 0 is
 * sending, so it was never missed, and catching it takes nothing back. */
static void a_frame_begun_while_sending_was_never_missed(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -80.0);
    struct send a;
    struct send own;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &own, TSIM_MS(10), 0, &w->sf7);
    send_at(w, &b, TSIM_MS(10) + SF7_FRAME - TSIM_MS(1), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 3));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
    world_free(w);
}

/* Node 0 has finished with A and is listening on SF7 when B begins on SF9; it retunes, at no cost,
 * in time to catch B. B began while it was idle, not receiving, so it was never missed. */
static void a_frame_begun_while_idle_was_never_missed(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.retune = 0;
    struct world *w = world_new(3, &p);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -80.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, SF7_FRAME + TSIM_MS(10), 2, &w->sf9);
    tsim_sched_at(w->sched, SF7_FRAME + TSIM_MS(11), tune_now, w);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1) && received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
    world_free(w);
}

/* Node 0 leaves A to retune to SF9, deaf for 1 ms, and B begins on SF9 in that millisecond: it was
 * never missed, so catching it when the retune ends takes nothing back. */
static void a_frame_begun_while_retuning_was_never_missed(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -80.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    tsim_sched_at(w->sched, TSIM_MS(10), tune_now, w);
    send_at(w, &b, TSIM_US(10500), 2, &w->sf9);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
    world_free(w);
}

/* Node 0 leaves A for SF9 with no time deaf, and catches B, which began on SF9 while node 0 was on
 * A but not tuned to it: never missed, so nothing to take back. */
static void a_frame_on_another_tuning_was_never_missed(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.retune = 0;
    struct world *w = world_new(3, &p);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -80.0);
    struct send a;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(5), 2, &w->sf9);
    tsim_sched_at(w->sched, TSIM_MS(6), tune_now, w);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
    world_free(w);
}

/* A, with a long preamble, is taken from node 0 by B, short and 10 dB louder; when B ends, A's
 * preamble still has time to run and node 0 catches it again. A counts once, by how that second
 * reception ends, not as taken too; and X, which began on A and was missed, stays missed, its
 * preamble over long before B ended. */
static void a_frame_taken_and_caught_again_counts_once(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.capture_anytime = true;
    struct world *w = world_new(4, &p);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -70.0);
    arrive(w, 3, 0, -80.0);
    struct tsim_lora long_preamble = w->sf7;
    long_preamble.preamble = 32;
    struct tsim_lora short_preamble = w->sf7;
    short_preamble.preamble = 6;
    struct send a;
    struct send x;
    struct send b;
    send_at(w, &a, 0, 1, &long_preamble);
    send_at(w, &x, TSIM_US(500), 3, &w->sf7);
    send_at(w, &b, TSIM_MS(1), 2, &short_preamble);
    b.len = 1;
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 3));
    CHECK(!received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_preempted, 0);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok + tsim_phy_stats(w->phy, 0)->rx_lost, 2);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 1); /* X */
    world_free(w);
}

static void tune_back(struct tsim_sched *s, void *ctx);

/* X, with a long preamble, begins while node 0 is on A and is missed. Node 0 retunes to SF9 and,
 * finding nothing there, back to SF7, both at no cost, while X's preamble still runs: it catches
 * X, and X counts once, as received, not as missed too. */
static void a_missed_frame_outlasts_a_retune_away_and_back(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.retune = 0;
    struct world *w = world_new(3, &p);
    arrive(w, 1, 0, -90.0); /* quiet enough not to spoil X */
    arrive(w, 2, 0, -80.0);
    struct tsim_lora long_preamble = w->sf7;
    long_preamble.preamble = 32;
    struct send a;
    struct send x;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &x, TSIM_MS(10), 2, &long_preamble);
    tsim_sched_at(w->sched, TSIM_MS(20), tune_now, w);
    tsim_sched_at(w->sched, TSIM_MS(21), tune_back, w);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 1); /* A */
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
    world_free(w);
}

/* Node 0 is on A, with a long preamble, when it retunes to SF9 and back at no cost: it catches A
 * again, and A counts once, as received, not as cut short too. */
static void a_frame_cut_short_and_caught_again_counts_once(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.retune = 0;
    struct world *w = world_new(2, &p);
    arrive(w, 1, 0, -80.0);
    struct tsim_lora long_preamble = w->sf7;
    long_preamble.preamble = 32;
    struct send a;
    send_at(w, &a, 0, 1, &long_preamble);
    tsim_sched_at(w->sched, TSIM_MS(1), tune_now, w);
    tsim_sched_at(w->sched, TSIM_MS(2), tune_back, w);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 0);
    world_free(w);
}

/* Node 0 is on A, with a long preamble, when it sends a short frame of its own; that done, A's
 * preamble still runs, and node 0 catches it again. A counts once, as received. */
static void a_frame_cut_short_by_a_send_and_caught_again_counts_once(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -80.0);
    struct tsim_lora long_preamble = w->sf7;
    long_preamble.preamble = 32;
    struct tsim_lora short_preamble = w->sf7;
    short_preamble.preamble = 6;
    struct send a;
    struct send own;
    send_at(w, &a, 0, 1, &long_preamble);
    send_at(w, &own, TSIM_MS(1), 0, &short_preamble);
    own.len = 1; /* 23.25 symbols, over well inside A's 31.25 */
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 0);
    world_free(w);
}

/* X, with a long preamble, begins while node 0 is on A and is missed; then B, short and 10 dB
 * louder, takes the receiver; when B ends, X's preamble still has time to run and node 0 catches
 * it. X was counted missed, so it is taken back, though another frame came between. */
static void a_frame_missed_before_a_capture_is_taken_back(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.capture_anytime = true;
    struct world *w = world_new(4, &p);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -80.0);
    arrive(w, 3, 0, -70.0);
    struct tsim_lora long_preamble = w->sf7;
    long_preamble.preamble = 32;
    struct tsim_lora short_preamble = w->sf7;
    short_preamble.preamble = 6;
    struct send a;
    struct send x;
    struct send b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &x, TSIM_MS(10), 2, &long_preamble);
    send_at(w, &b, TSIM_MS(11), 3, &short_preamble);
    b.len = 1; /* 23.25 symbols, so it ends inside X's 31.25 */
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 3));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_preempted, 1);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok + tsim_phy_stats(w->phy, 0)->rx_lost, 2);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_missed, 0);
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

/* A frame that would end past the last representable time is refused, and leaves the radio and
 * the queue as they were. */
static void refuses_a_frame_past_the_end_of_the_clock(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -80.0);
    tsim_sched_run_until(w->sched, INT64_MAX - TSIM_MS(10));
    CHECK_EQ_I64(tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL), 0);
    CHECK(!tsim_phy_transmitting(w->phy, 1));
    CHECK(!tsim_phy_receiving(w->phy, 0));
    CHECK_EQ_I64(tsim_sched_size(w->sched), 0);
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 1)->tx, 0);
    world_free(w);
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

static void tune_now(struct tsim_sched *s, void *ctx) {
    (void)s;
    struct world *w = ctx;
    tsim_phy_tune(w->phy, 0, 0, &w->sf9);
}

/* The time a radio spends on frames it is receiving, however each reception ends. */
/* Without a hold, the carrier flags are up exactly while the radio is receiving: the driver clears
 * them on reading the packet. */
static void the_carrier_is_the_reception_without_a_hold(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    CHECK(!tsim_phy_carrier(w->phy, 0));
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    for (tsim_time t = TSIM_MS(1); t < TSIM_MS(60); t += TSIM_MS(1)) {
        tsim_sched_run_until(w->sched, t);
        CHECK(tsim_phy_carrier(w->phy, 0) == tsim_phy_receiving(w->phy, 0));
    }
    CHECK(!tsim_phy_carrier(w->phy, 0));
    CHECK(!tsim_phy_carrier(w->phy, 1)); /* sending is not receiving */
    world_free(w);
}

/* A held header flag. The header of a 16-byte SF7 frame is demodulated 20.25 symbols in, at
 * 20.736 ms; with a hold of 100 ms the flag stays up until 120.736 ms, long after the frame. A
 * second frame, begun while it is up, does not extend it: the look that times the flag out reads
 * the channel clear, and so do the looks after it, though the radio is still on that frame. A
 * third, begun after, sets it again. */
static void a_held_header_flag_stays_up_for_the_hold_from_the_first_header(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    tsim_phy_hold_header(w->phy, 0, TSIM_MS(100));
    struct send a, b, c;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(90), 1, &w->sf7);
    b.len = 200;
    send_at(w, &c, TSIM_MS(500), 1, &w->sf7);

    tsim_sched_run_until(w->sched, TSIM_MS(10));
    CHECK(tsim_phy_carrier(w->phy, 0)); /* the preamble */
    tsim_sched_run_until(w->sched, TSIM_MS(60));
    CHECK(!tsim_phy_receiving(w->phy, 0));
    CHECK(tsim_phy_carrier(w->phy, 0)); /* the frame has gone, the flag has not */
    tsim_sched_run_until(w->sched, TSIM_NS(120736000));
    CHECK(tsim_phy_carrier(w->phy, 0));
    tsim_sched_run_until(w->sched, TSIM_NS(120736001));
    CHECK(tsim_phy_receiving(w->phy, 0)); /* b */
    CHECK(!tsim_phy_carrier(w->phy, 0));
    tsim_sched_run_until(w->sched, TSIM_MS(130));
    CHECK(tsim_phy_receiving(w->phy, 0));
    CHECK(!tsim_phy_carrier(w->phy, 0));

    tsim_sched_run_until(w->sched, TSIM_MS(510));
    CHECK(tsim_phy_carrier(w->phy, 0)); /* c's preamble */
    tsim_sched_run_until(w->sched, TSIM_NS(620736000));
    CHECK(tsim_phy_carrier(w->phy, 0));
    tsim_sched_run_until(w->sched, TSIM_NS(620736001));
    CHECK(!tsim_phy_carrier(w->phy, 0));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_ok, 3);
    world_free(w);
}

/* The flag is timed out only when looked at. Looked at first in the preamble of a frame that began
 * long after the flag was set, it is found stale and cleared, and that frame does not set it. */
static void a_stale_header_flag_clears_when_first_looked_at(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    tsim_phy_hold_header(w->phy, 0, TSIM_MS(100));
    struct send a, c;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &c, TSIM_MS(500), 1, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_MS(510));
    CHECK(tsim_phy_receiving(w->phy, 0));
    CHECK(!tsim_phy_carrier(w->phy, 0));
    tsim_sched_run_until(w->sched, TSIM_MS(530));
    CHECK(!tsim_phy_carrier(w->phy, 0));
    world_free(w);
}

/* The held flag goes up by timing, as MeshBench's radio raises it: two frames 2 dB apart from the
 * same instant spoil each other, header and all, and the one the receiver is on still sets it. */
static void a_held_header_flag_is_set_by_timing_even_through_a_collision(void) {
    struct world *w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -82.0);
    tsim_phy_hold_header(w->phy, 0, TSIM_MS(100));
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_phy_transmit(w->phy, 2, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_MS(60));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_lost, 1);
    CHECK(tsim_phy_carrier(w->phy, 0));
    world_free(w);
}

/* A reception cut short before its header sets nothing. */
static void a_reception_cut_short_before_its_header_sets_no_flag(void) {
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    tsim_phy_hold_header(w->phy, 0, TSIM_MS(100));
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_MS(10));
    tsim_phy_transmit(w->phy, 0, 0, &w->sf7, 16, TX_DBM, NULL); /* aborts it */
    tsim_sched_run_until(w->sched, TSIM_MS(70));
    CHECK(!tsim_phy_transmitting(w->phy, 0));
    CHECK(!tsim_phy_carrier(w->phy, 0));
    world_free(w);
}

static void rx_airtime_counts_every_reception_however_it_ends(void) {
    /* Decoded, and part-way through. */
    struct world *w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_MS(10));
    CHECK_EQ_I64(tsim_phy_rx_airtime(w->phy, 0), TSIM_MS(10));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_airtime, 0);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(tsim_phy_rx_airtime(w->phy, 0), SF7_FRAME);
    CHECK_EQ_I64(tsim_phy_rx_airtime(w->phy, 1), 0); /* sending is not receiving */
    world_free(w);

    /* Lost to interference: the whole frame was still spent on it. */
    w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -70.0);
    struct send a, b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(20), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_lost, 1);
    CHECK_EQ_I64(tsim_phy_rx_airtime(w->phy, 0), SF7_FRAME);
    world_free(w);

    /* Taken by a louder frame 2 ms in: 2 ms on the first, then all of the second. */
    w = world_new(3, NULL);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -70.0);
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(2), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_preempted, 1);
    CHECK_EQ_I64(tsim_phy_rx_airtime(w->phy, 0), TSIM_MS(2) + SF7_FRAME);
    world_free(w);

    /* Cut short by transmitting, and by retuning, 10 ms in. */
    w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(10), 0, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 1);
    CHECK_EQ_I64(tsim_phy_rx_airtime(w->phy, 0), TSIM_MS(10));
    world_free(w);

    w = world_new(2, NULL);
    arrive(w, 1, 0, -86.0);
    send_at(w, &a, 0, 1, &w->sf7);
    tsim_sched_at(w->sched, TSIM_MS(10), tune_now, w);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_aborted, 1);
    CHECK_EQ_I64(tsim_phy_rx_airtime(w->phy, 0), TSIM_MS(10));
    world_free(w);
}

static struct tsim_phy_params with_pairwise(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.pairwise = true;
    return p;
}

/* Pairwise, two interferers 8 dB down leave 8 dB each, which survives; summed, they did not. */
static void pairwise_takes_interferers_one_at_a_time(void) {
    struct tsim_phy_params p = with_pairwise();
    struct world *w = world_new(4, &p);
    arrive(w, 1, 0, -70.0);
    arrive(w, 2, 0, -78.0);
    arrive(w, 3, 0, -78.0);
    struct send s[3];
    send_at(w, &s[0], 0, 1, &w->sf7);
    send_at(w, &s[1], TSIM_MS(6), 2, &w->sf7);
    send_at(w, &s[2], TSIM_MS(6), 3, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 1));
    world_free(w);
}

/* Pairwise, an interferer counts at full weight: 3 dB louder over the last tenth is a loss,
 * where weighted by its overlap it left 7 dB. */
static void pairwise_does_not_weigh_by_overlap(void) {
    for (int pairwise = 0; pairwise <= 1; pairwise++) {
        struct tsim_phy_params p = tsim_phy_defaults();
        p.pairwise = pairwise;
        struct world *w = world_new(3, &p);
        arrive(w, 1, 0, -80.0);
        arrive(w, 2, 0, -77.0);
        struct send s[2];
        send_at(w, &s[0], 0, 1, &w->sf7);
        send_at(w, &s[1], SF7_FRAME - SF7_FRAME / 10, 2, &w->sf7);
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1) == !pairwise);
        world_free(w);
    }
}

/* Pairwise, a frame too faint for the receiver to decode does not interfere: 4 dB below a frame
 * at 5 dB SNR, it is under the floor. Summed, it leaves 4 dB. */
static void pairwise_ignores_what_the_receiver_could_not_hear(void) {
    for (int pairwise = 0; pairwise <= 1; pairwise++) {
        struct tsim_phy_params p = tsim_phy_defaults();
        p.pairwise = pairwise;
        struct world *w = world_new(3, &p);
        arrive(w, 1, 0, -122.0);
        arrive(w, 2, 0, -126.0);
        struct send s[2];
        send_at(w, &s[0], 0, 1, &w->sf7);
        send_at(w, &s[1], TSIM_MS(6), 2, &w->sf7);
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1) == pairwise);
        world_free(w);
    }
}

/* Pairwise, a frame that overlaps no more than the first 3 symbols of its preamble (8 programmed,
 * less 5 to lock) does not collide with the one ending; 4 symbols does. */
static void pairwise_forgives_a_preamble_overlap(void) {
    for (int symbols = 2; symbols <= 4; symbols += 2) {
        struct tsim_phy_params p = with_pairwise();
        struct world *w = world_new(3, &p);
        arrive(w, 1, 0, -80.0);
        arrive(w, 2, 0, -80.0);
        struct send s[2];
        send_at(w, &s[0], 0, 1, &w->sf7);
        send_at(w, &s[1], SF7_FRAME - TSIM_US(1024) * symbols, 2, &w->sf7);
        tsim_sched_run_until(w->sched, TSIM_S(1));
        CHECK(received(w, 0, 1) == (symbols == 2));
        world_free(w);
    }
}

static void tune_back(struct tsim_sched *s, void *ctx) {
    (void)s;
    struct world *w = ctx;
    tsim_phy_tune(w->phy, 0, 0, &w->sf7);
}

/* The grace is for the later frame's first symbols, not for any short overlap. Node 0 is deaf
 * from a retune until 2 ms before B ends, then catches A, which started 4 ms before that - long
 * enough ago that A's 3-symbol grace has passed. B's last 2 ms are payload overlapping A's
 * payload, 3 dB under it: a collision, however short. */
static void pairwise_grace_covers_only_the_preamble(void) {
    struct tsim_phy_params p = with_pairwise();
    struct world *w = world_new(3, &p);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -83.0);
    struct send a, b;
    send_at(w, &b, 0, 2, &w->sf7);                      /* ends at SF7_FRAME */
    send_at(w, &a, SF7_FRAME - TSIM_MS(6), 1, &w->sf7); /* grace over at SF7_FRAME - 2.928 ms */
    tsim_sched_at(w->sched, SF7_FRAME - TSIM_US(3100), tune_now, w);
    tsim_sched_at(w->sched, SF7_FRAME - TSIM_US(3000), tune_back, w); /* listening 1 ms later */
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(!received(w, 0, 2));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_lost, 1); /* it caught A, and lost it */
    CHECK(!received(w, 0, 1));
    world_free(w);
}

/* With capture_anytime, a frame 10 dB louder 20 ms in - long after the lock - takes the receiver,
 * as it would only during the preamble otherwise. */
static void capture_anytime_lets_a_louder_frame_take_the_receiver_late(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.capture_anytime = true;
    struct world *w = world_new(3, &p);
    arrive(w, 1, 0, -80.0);
    arrive(w, 2, 0, -70.0);
    struct send a, b;
    send_at(w, &a, 0, 1, &w->sf7);
    send_at(w, &b, TSIM_MS(20), 2, &w->sf7);
    tsim_sched_run_until(w->sched, TSIM_S(1));
    CHECK(received(w, 0, 2));
    CHECK(!received(w, 0, 1));
    CHECK_EQ_I64(tsim_phy_stats(w->phy, 0)->rx_preempted, 1);
    world_free(w);
}

/* Thirty frames, one every 100 ms, from node 1 to nodes 0 and 2, each arriving 0.03 dB over the
 * floor before fading. Returns which arrived, as bits: node 0's in the low 32, node 2's above. */
static uint64_t faded(double fading_db, uint64_t seed) {
    struct tsim_phy_params p = tsim_phy_defaults();
    p.fading_db = fading_db;
    p.fading_seed = seed;
    struct world *w = world_new(3, &p);
    arrive(w, 1, 0, -124.5);
    arrive(w, 1, 2, -124.5);
    struct send s[30];
    for (int i = 0; i < 30; i++) {
        send_at(w, &s[i], TSIM_MS(100) * i, 1, &w->sf7);
    }
    tsim_sched_run_until(w->sched, TSIM_S(10));
    uint64_t bits = 0;
    for (int i = 0; i < w->log.count; i++) {
        bits |= (uint64_t)1 << ((w->log.frame[i] - 1) + (w->log.node[i] == 2 ? 32 : 0));
    }
    world_free(w);
    return bits;
}

static int popcount(uint64_t x) {
    int n = 0;
    for (; x; x &= x - 1) {
        n++;
    }
    return n;
}

static void fading_varies_each_frame_at_each_receiver_and_repeats(void) {
    uint64_t all = 0x3FFFFFFFull | 0x3FFFFFFFull << 32;
    CHECK_EQ_U64(faded(0, 1), all);
    uint64_t a = faded(3.0, 1);
    int heard = popcount(a);
    CHECK(heard > 15 && heard < 45); /* about half of 60 */
    CHECK_EQ_U64(faded(3.0, 1), a);  /* the same seed fades the same way */
    CHECK(faded(3.0, 2) != a);
    CHECK((a & 0x3FFFFFFFull) != a >> 32); /* the two receivers fade apart */
}

/* CAD notices a frame 1 dB under the floor only with a margin, and only after its delay. */
static void cad_has_a_margin_and_a_delay(void) {
    struct tsim_phy_params p = tsim_phy_defaults();
    struct world *w = world_new(2, &p);
    arrive(w, 1, 0, -125.5);
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    CHECK(!tsim_phy_cad(w->phy, 0));
    world_free(w);

    p.cad_margin_db = 3.0;
    p.cad_delay = TSIM_MS(10);
    w = world_new(2, &p);
    arrive(w, 1, 0, -125.5);
    tsim_phy_transmit(w->phy, 1, 0, &w->sf7, 16, TX_DBM, NULL);
    tsim_sched_run_until(w->sched, TSIM_MS(5));
    CHECK(!tsim_phy_cad(w->phy, 0));
    tsim_sched_run_until(w->sched, TSIM_MS(15));
    CHECK(tsim_phy_cad(w->phy, 0));
    world_free(w);
}

static void create_refuses_bad_fading_and_cad(void) {
    struct tsim_sched *s = tsim_sched_create();
    struct tsim_lora sf7 = tsim_lora_default(7, 125000);
    struct tsim_phy_hooks hooks = {0};
    struct tsim_phy_params p = tsim_phy_defaults();
    p.fading_db = -1;
    CHECK(!tsim_phy_create(s, &p, 2, 0, &sf7, hooks));
    p = tsim_phy_defaults();
    p.fading_db = NAN;
    CHECK(!tsim_phy_create(s, &p, 2, 0, &sf7, hooks));
    p = tsim_phy_defaults();
    p.cad_margin_db = NAN;
    CHECK(!tsim_phy_create(s, &p, 2, 0, &sf7, hooks));
    p = tsim_phy_defaults();
    p.cad_delay = -1;
    CHECK(!tsim_phy_create(s, &p, 2, 0, &sf7, hooks));
    tsim_sched_destroy(s);
}

/* A link needs both ends to decode each other at the floor. 0-1 do, comfortably; 1 hears 2 but
 * 2 cannot hear 1; 2-3 are just above the floor, -124.53 dBm. */
static void links_count_pairs_that_decode_both_ways(void) {
    struct world *w = world_new(5, NULL);
    arrive(w, 0, 1, -120);
    tsim_phy_set_loss_from(w->phy, 2, 1, TX_DBM + 120);
    tsim_phy_set_loss_from(w->phy, 1, 2, TX_DBM + 130);
    arrive(w, 2, 3, -124.5);
    arrive(w, 3, 4, -124.6);
    struct tsim_phy_links l;
    CHECK(tsim_phy_links(w->phy, &w->sf7, TX_DBM, &l));
    CHECK(fabs(l.degree_mean - 4.0 / 5) < 1e-9);
    CHECK_EQ_I64(l.degree_min, 0);
    CHECK_EQ_I64(l.degree_max, 1);
    CHECK_EQ_I64(l.component_max, 2);
    /* 10 dB less power and nothing is a link; at SF9, 5 dB deeper, all of them are. */
    CHECK(tsim_phy_links(w->phy, &w->sf7, TX_DBM - 10, &l));
    CHECK(l.degree_mean == 0 && l.degree_max == 0 && l.component_max == 1);
    arrive(w, 1, 2, -120);
    CHECK(tsim_phy_links(w->phy, &w->sf9, TX_DBM, &l));
    CHECK(fabs(l.degree_mean - 8.0 / 5) < 1e-9);
    CHECK_EQ_I64(l.degree_min, 1);
    CHECK_EQ_I64(l.degree_max, 2);
    CHECK_EQ_I64(l.component_max, 5);
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
    RUN(missed_counts_only_frames_it_could_have_decoded);
    RUN(a_frame_caught_after_all_is_not_missed);
    RUN(a_frame_caught_after_sending_was_never_missed);
    RUN(a_frame_begun_while_sending_was_never_missed);
    RUN(a_frame_begun_while_idle_was_never_missed);
    RUN(a_frame_begun_while_retuning_was_never_missed);
    RUN(a_frame_on_another_tuning_was_never_missed);
    RUN(a_frame_taken_and_caught_again_counts_once);
    RUN(a_missed_frame_outlasts_a_retune_away_and_back);
    RUN(a_frame_cut_short_and_caught_again_counts_once);
    RUN(a_frame_cut_short_by_a_send_and_caught_again_counts_once);
    RUN(a_frame_missed_before_a_capture_is_taken_back);
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
    RUN(refuses_a_frame_past_the_end_of_the_clock);
    RUN(destroy_cancels_pending_radio_events);
    RUN(rx_airtime_counts_every_reception_however_it_ends);
    RUN(the_carrier_is_the_reception_without_a_hold);
    RUN(a_held_header_flag_stays_up_for_the_hold_from_the_first_header);
    RUN(a_stale_header_flag_clears_when_first_looked_at);
    RUN(a_held_header_flag_is_set_by_timing_even_through_a_collision);
    RUN(a_reception_cut_short_before_its_header_sets_no_flag);
    RUN(pairwise_takes_interferers_one_at_a_time);
    RUN(pairwise_does_not_weigh_by_overlap);
    RUN(pairwise_ignores_what_the_receiver_could_not_hear);
    RUN(pairwise_forgives_a_preamble_overlap);
    RUN(pairwise_grace_covers_only_the_preamble);
    RUN(capture_anytime_lets_a_louder_frame_take_the_receiver_late);
    RUN(fading_varies_each_frame_at_each_receiver_and_repeats);
    RUN(cad_has_a_margin_and_a_delay);
    RUN(create_refuses_bad_fading_and_cad);
    RUN(links_count_pairs_that_decode_both_ways);
    return CHECK_DONE();
}
