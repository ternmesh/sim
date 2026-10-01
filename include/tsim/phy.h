#ifndef TSIM_PHY_H
#define TSIM_PHY_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/channel.h"
#include "tsim/lora.h"
#include "tsim/sched.h"
#include "tsim/time.h"

/* The radio medium: every node's radio, the frames on the air, and which of them each receiver
 * decodes.
 *
 * A radio is half-duplex and is tuned to one channel, SF and bandwidth at a time. It hears only
 * frames that match its tuning; every other frame on its channel is interference. Its life is:
 *
 *  - listening: the first matching frame loud enough to decode (SNR at least the SF's demodulation
 *    floor) starts a reception;
 *  - receiving: for the first `lock_symbols` of the preamble the receiver can still be taken by a
 *    matching frame at least `capture_db` louder, and the frame it was on is lost; after that it
 *    is locked, and later frames are only interference;
 *  - at the frame's end it is decoded if, for every SF, the energy of that SF's interference
 *    while the receiver was on the frame leaves a signal-to-interference ratio at or above the
 *    isolation threshold for the pair. Interference is summed, never taken one frame at a time,
 *    and is weighted by how much of the reception it overlapped;
 *  - transmitting aborts any reception, and nothing is heard until the frame has gone out;
 *  - retuning takes `retune` of deafness, and so does transmitting with a modulation other than
 *    the one the radio listens on, which costs a retune when the frame ends.
 *
 * A radio that starts listening part-way through a matching frame - after a transmission, a
 * retune or another reception - can still catch it if enough of the preamble is left to lock on.
 *
 * Fading, if `fading_db` is set, varies every link's loss afresh for each frame and each receiver.
 *
 * Three switches trade this model for the simpler one LoRaSim and Meshtasticator use, so their
 * results can be reproduced: `pairwise` judges each interferer on its own, at full weight, and
 * only one the receiver could itself decode; `capture_anytime` lets a louder frame take the
 * receiver at any point of a reception, not only during the preamble; and CAD can be given a
 * margin below the demodulation floor and a delay before it notices a frame. They are off by
 * default, and the comparison runs with them off.
 *
 * Not modelled yet: frames of different bandwidths sharing a channel, whose interference is
 * counted as if it were the wanted SF's (the pessimistic case); the time CAD takes, which the
 * MAC charges itself; sleep. */

#define TSIM_SF_MIN 7
#define TSIM_SF_COUNT 6

struct tsim_phy_params {
    double noise_figure_db;
    /* Lowest SNR each SF demodulates at, SF7 first. */
    double snr_min_db[TSIM_SF_COUNT];
    /* Lowest signal-to-interference ratio a frame survives, as
     * isolation_db[wanted SF - 7][interfering SF - 7]. The diagonal is co-SF capture. */
    double isolation_db[TSIM_SF_COUNT][TSIM_SF_COUNT];
    /* How much louder a matching frame must be to take a receiver still in the preamble. */
    double capture_db;
    /* Preamble symbols a receiver needs before it is locked. */
    uint16_t lock_symbols;
    /* Time a radio is deaf after changing channel, SF or bandwidth. */
    tsim_time retune;

    /* A Gaussian of this standard deviation, in dB, added to a link's loss for each frame at each
     * receiver, drawn afresh every time: 0 for none. Drawn from fading_seed, the frame and the
     * receiver, so a run repeats. */
    double fading_db;
    uint64_t fading_seed;
    /* Judge interference one frame at a time, LoRaSim's way: a frame is lost if any one
     * interferer the receiver could decode leaves less than the pair's isolation threshold,
     * however little of it overlapped - except one that overlaps only the first
     * (preamble - lock_symbols) symbols of the later frame's preamble. Off: summed and weighted. */
    bool pairwise;
    /* Let a frame capture_db louder take the receiver at any point of a reception. */
    bool capture_anytime;
    /* CAD notices a frame down to this many dB below the demodulation floor ... */
    double cad_margin_db;
    /* ... once it has been on the air this long. */
    tsim_time cad_delay;
};

/* Datasheet noise figure and demodulation floors (6 dB; -7.5 dB at SF7 down to -20 dB at SF12),
 * the isolation matrix of Goursaud and Gorce, "Dedicated networks for IoT: PHY/MAC state of the
 * art and challenges" (EAI 2015), a 6 dB capture threshold, a 5-symbol lock and a 1 ms retune.
 * The last three, and the matrix, are placeholders until the bench rig (MSH-32) measures them.
 * No fading, and the three LoRaSim switches off. */
struct tsim_phy_params tsim_phy_defaults(void);

struct tsim_frame {
    uint64_t id; /* from 1, in the order frames were sent; never wraps */
    uint32_t src;
    uint16_t channel;
    struct tsim_lora lora;
    uint32_t len;
    double tx_dbm;
    tsim_time start;
    tsim_time end;
    void *payload; /* the sender's, carried untouched */
};

/* Called from inside the scheduler. A frame passed to a hook is valid only for the call. A hook
 * may transmit or retune; it sees every radio already in the state the frame's end left it in. */
struct tsim_phy_hooks {
    void (*rx)(void *ctx, uint32_t node, const struct tsim_frame *frame, double rssi_dbm,
               double snr_db);
    void (*tx_done)(void *ctx, uint32_t node, const struct tsim_frame *frame);
    void *ctx;
};

struct tsim_phy_stats {
    uint64_t tx;
    tsim_time tx_airtime;
    uint64_t rx_ok;
    uint64_t rx_lost;      /* decoded to the end, then failed on interference */
    uint64_t rx_preempted; /* taken by a louder frame during the preamble */
    uint64_t rx_aborted;   /* cut short by transmitting or retuning */
    /* Time spent on frames it was receiving, however each reception ended: what the radio was busy
     * with, which is all a radio's own channel utilisation counter can know of what it heard. */
    tsim_time rx_airtime;
};

struct tsim_phy;

/* A medium of `nodes` radios, all listening on `channel` with `listen`'s modulation. Every link
 * starts with infinite loss: nothing hears anything until losses are set. */
struct tsim_phy *tsim_phy_create(struct tsim_sched *sched, const struct tsim_phy_params *params,
                                 uint32_t nodes, uint16_t channel, const struct tsim_lora *listen,
                                 struct tsim_phy_hooks hooks);
/* Frees the medium and cancels its pending events, so the scheduler can go on running without it.
 * Frames still on the air end silently. */
void tsim_phy_destroy(struct tsim_phy *phy);

/* Sets the loss of the link between a and b, in both directions. */
void tsim_phy_set_loss(struct tsim_phy *phy, uint32_t a, uint32_t b, double loss_db);
double tsim_phy_loss(const struct tsim_phy *phy, uint32_t a, uint32_t b);

/* Sets every link's loss from the channel model, for nodes standing at pos[0..nodes). */
void tsim_phy_set_losses(struct tsim_phy *phy, const struct tsim_channel_params *channel,
                         const struct tsim_pos *pos);

/* Puts a frame on the air now. Returns its id, or 0 - leaving every radio as it was - if the node
 * is already transmitting, the modulation and length have no airtime, the frame would end past
 * the end of the clock, or memory runs out. */
uint64_t tsim_phy_transmit(struct tsim_phy *phy, uint32_t node, uint16_t channel,
                           const struct tsim_lora *lora, uint32_t len, double tx_dbm,
                           void *payload);

/* Retunes a node's receiver. Retuning to what it is already tuned to does nothing; otherwise any
 * reception is aborted and the radio is deaf for `retune`. While transmitting, the new tuning
 * takes effect when the frame ends. Returns false for an invalid modulation. */
bool tsim_phy_tune(struct tsim_phy *phy, uint32_t node, uint16_t channel,
                   const struct tsim_lora *listen);

bool tsim_phy_transmitting(const struct tsim_phy *phy, uint32_t node);

/* Whether the node is part-way through receiving a frame. */
bool tsim_phy_receiving(const struct tsim_phy *phy, uint32_t node);

/* Channel activity detection, as answered at this instant: whether a frame the node could decode
 * - its tuning, loud enough, give or take cad_margin_db - has been on the air for cad_delay.
 * Frames on other SFs are invisible to it, however loud. False while transmitting or retuning. */
bool tsim_phy_cad(const struct tsim_phy *phy, uint32_t node);

const struct tsim_phy_stats *tsim_phy_stats(const struct tsim_phy *phy, uint32_t node);

/* rx_airtime as of now, counting the part of a reception still under way. */
tsim_time tsim_phy_rx_airtime(const struct tsim_phy *phy, uint32_t node);

#endif
