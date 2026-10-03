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

/* What became of a frame at the one receiver it was marked for (tsim_phy_mark()): decoded, or the
 * reason it was not, as that radio last had it. */
enum tsim_phy_fate {
    TSIM_PHY_DECODED,
    TSIM_PHY_WEAK,       /* below the demodulation floor there */
    TSIM_PHY_INTERFERED, /* received to the end, then failed on interference */
    TSIM_PHY_TAKEN,      /* taken by a louder frame */
    TSIM_PHY_BUSY,       /* began while it was receiving another, which it kept */
    TSIM_PHY_DEAF,       /* transmitting, retuning or tuned elsewhere */
    TSIM_PHY_FATE_COUNT,
};

/* Called from inside the scheduler. A frame passed to a hook is valid only for the call. A hook
 * may transmit or retune; it sees every radio already in the state the frame's end left it in.
 *
 * `marked`, if given, is called once for each frame marked for a receiver, as the frame ends and
 * before tx_done: with the receiver, the fate, and the tag of the frame that cost it - for
 * interfered, the one that brought the receiver the most interference energy; for taken, the
 * louder frame; for busy, the one it kept; for deaf, the one it was sending, if any - or -1. */
struct tsim_phy_hooks {
    void (*rx)(void *ctx, uint32_t node, const struct tsim_frame *frame, double rssi_dbm,
               double snr_db);
    void (*tx_done)(void *ctx, uint32_t node, const struct tsim_frame *frame);
    void *ctx;
    void (*marked)(void *ctx, uint32_t node, const struct tsim_frame *frame,
                   enum tsim_phy_fate fate, int rival);
};

struct tsim_phy_stats {
    uint64_t tx;
    tsim_time tx_airtime;
    /* What became of each frame the radio could have decoded, other than its own: each counts once,
     * by how it last ended here. A frame taken, cut short or missed that the radio catches after
     * all, its preamble not yet over, counts by how that reception ends instead. */
    uint64_t rx_ok;
    uint64_t rx_lost;      /* decoded to the end, then failed on interference */
    uint64_t rx_preempted; /* taken by a louder frame */
    uint64_t rx_aborted;   /* cut short by transmitting or retuning */
    uint64_t rx_missed;    /* began while it was receiving another, which it kept */
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
/* Sets the loss from a to b only: for a link whose two ends differ - in transmit power, say, which
 * a frame's tx_dbm cannot carry when every node sends at the radio's. */
void tsim_phy_set_loss_from(struct tsim_phy *phy, uint32_t from, uint32_t to, double loss_db);
double tsim_phy_loss(const struct tsim_phy *phy, uint32_t a, uint32_t b);

/* For losses that change while frames are on the air (MSH-59): on, each frame begun from then on
 * keeps the losses from its sender as they were when it went on the air, for everything it is
 * judged by - whether a radio locks on, what it brings as interference, CAD - so a change mid-frame
 * neither cuts a reception short nor reaches back over the part already heard. Off, every frame
 * reads the losses as they are. Off by default; the same results either way while they hold. */
void tsim_phy_keep_losses(struct tsim_phy *phy, bool on);

/* How many radios it has. */
uint32_t tsim_phy_nodes(const struct tsim_phy *phy);

/* How connected the medium is at one modulation and power. A link is a pair of nodes that each
 * decode the other with nothing else on the air: the mean loss, no fading, at least the SF's
 * demodulation floor. That is the most any routing can use; interference only takes from it. */
struct tsim_phy_links {
    double degree_mean; /* links per node */
    uint32_t degree_min;
    uint32_t degree_max;
    uint32_t component_max; /* nodes in the largest set the links join */
};

/* The weakest a frame at `lora` is decoded at, in dBm, with nothing else on the air and no fading:
 * the noise in its bandwidth and the SF's demodulation floor. NAN for an invalid modulation. */
double tsim_phy_floor_dbm(const struct tsim_phy *phy, const struct tsim_lora *lora);

/* The links every node has, sending at `tx_dbm` with `lora`. Returns false, leaving `out` zero,
 * when memory runs out or the modulation is invalid. Takes time and memory in proportion to the
 * square of the node count. */
bool tsim_phy_links(const struct tsim_phy *phy, const struct tsim_lora *lora, double tx_dbm,
                    struct tsim_phy_links *out);

/* Sets every link's loss from the channel model, for nodes standing at pos[0..nodes). */
void tsim_phy_set_losses(struct tsim_phy *phy, const struct tsim_channel_params *channel,
                         const struct tsim_pos *pos);

/* Watches every frame begun from now on at every node in `nodes` ([node], kept by the caller)
 * but its sender (MSH-60): as each ends, `fn` is told its fate at each, as tsim_phy_mark() would
 * book it for that node alone. Slow: for diagnosis, not for every run. NULL `nodes` stops it. */
typedef void (*tsim_phy_heard_fn)(void *ctx, uint32_t node, const struct tsim_frame *frame,
                                  enum tsim_phy_fate fate);
void tsim_phy_watch(struct tsim_phy *phy, tsim_phy_heard_fn fn, void *ctx, const bool *nodes);

/* Powers a radio down or up (MSH-59). Down, it hears nothing - a frame meant for it is booked deaf
 * - senses no carrier and sends nothing: what it was receiving is lost, though a frame it is
 * sending runs to its end. Up again, it listens as it was tuned. Radios start up. */
void tsim_phy_power(struct tsim_phy *phy, uint32_t node, bool on);
bool tsim_phy_on(const struct tsim_phy *phy, uint32_t node);

/* Puts a frame on the air now. Returns its id, or 0 - leaving every radio as it was - if the node
 * is already transmitting or powered down, the modulation and length have no airtime, the frame
 * would end past the end of the clock, or memory runs out. */
uint64_t tsim_phy_transmit(struct tsim_phy *phy, uint32_t node, uint16_t channel,
                           const struct tsim_lora *lora, uint32_t len, double tx_dbm,
                           void *payload);

/* Marks a frame for the books, as it goes on the air - straight after tsim_phy_transmit(), at the
 * same instant: `tag` is any number from 0 to 127 the caller files it under, reported for it as a
 * rival, and `node`, unless it is out of range or the sender, is the receiver whose fate the
 * frame's end reports to the marked hook. Changes nothing about what anyone receives. Returns
 * false for a frame no longer on the air or a tag out of range. */
bool tsim_phy_mark(struct tsim_phy *phy, uint64_t frame, uint32_t node, int tag);

/* Retunes a node's receiver. Retuning to what it is already tuned to does nothing; otherwise any
 * reception is aborted and the radio is deaf for `retune`. While transmitting, the new tuning
 * takes effect when the frame ends. Returns false for an invalid modulation. */
bool tsim_phy_tune(struct tsim_phy *phy, uint32_t node, uint16_t channel,
                   const struct tsim_lora *listen);

bool tsim_phy_transmitting(const struct tsim_phy *phy, uint32_t node);

/* Whether the node is part-way through receiving a frame. */
bool tsim_phy_receiving(const struct tsim_phy *phy, uint32_t node);

/* Whether the radio's carrier flags are up, which is how an SX126x driver tells that the channel is
 * busy: preamble detected, from the start of a reception, then header valid, from when the frame's
 * header is demodulated. A driver clears both on reading the packet, so this is
 * tsim_phy_receiving() - unless the header flag is held, when asking is the driver looking, and
 * may clear it. */
bool tsim_phy_carrier(struct tsim_phy *phy, uint32_t node);

/* Holds a node's header flag, as a radio that never clears it when the driver reads a packet would
 * leave it. A reception that begins while the flag is clear sets it at the frame's header - its
 * preamble raising the carrier until then - and it stays set whatever is heard after. Only the
 * driver's stale-flag timeout clears it: a look that finds it set for longer than `hold` clears
 * both flags and reads the channel clear, and so does every later look until a reception that
 * begins after that one. 0 releases the hold; the flag starts clear either way.
 *
 * The flag goes up by timing alone, once the reception has lasted to the header, as MeshBench's
 * virtual SX1262 raises it "after the LoRa header would have been demodulated": a header a
 * collision spoils sets it too. A real radio would need the header to get through, but its driver
 * clears the flag on reading, so only a hold - which exists to reproduce MeshBench - sees the
 * difference. Checking the header against the interference over it puts rx outside the MeshBench
 * check's limit, for both a sender and an arm. */
void tsim_phy_hold_header(struct tsim_phy *phy, uint32_t node, tsim_time hold);

/* Channel activity detection, as answered at this instant: whether a frame the node could decode
 * - its tuning, loud enough, give or take cad_margin_db - has been on the air for cad_delay.
 * Frames on other SFs are invisible to it, however loud. False while transmitting or retuning. */
bool tsim_phy_cad(const struct tsim_phy *phy, uint32_t node);

const struct tsim_phy_stats *tsim_phy_stats(const struct tsim_phy *phy, uint32_t node);

/* rx_airtime as of now, counting the part of a reception still under way. */
tsim_time tsim_phy_rx_airtime(const struct tsim_phy *phy, uint32_t node);

#endif
