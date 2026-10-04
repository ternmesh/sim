#ifndef TSIM_MESHTASTIC_H
#define TSIM_MESHTASTIC_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/node.h"
#include "tsim/time.h"

/* Candidate 1: Meshtastic's managed flood, and the MAC it runs over.
 *
 * Both are written from Meshtasticator (github.com/meshtastic/Meshtasticator, CC BY 4.0), whose
 * lib/mac.py and lib/node.py port the firmware's timing and record the firmware version they were
 * checked against (2.7.15). Nothing here is taken from the firmware itself. Where Meshtasticator
 * and this port differ, the difference is named below; the compatibility mode that reproduces
 * Meshtasticator's results is the scenario's business, not these plugins'.
 *
 * The routing. Every frame is a 16-byte header - destination, sender, packet id (4 bytes each), a
 * flags byte holding the hops left, the want-ack bit and the hop count it started with, and a
 * channel hash, next-hop and relay byte - then one byte naming what the payload is, then the
 * payload. A message costs 17 bytes more than its content, and content over 238 bytes is refused.
 *
 *  - a message goes out with the configured hop limit, and wants an acknowledgement if want_ack
 *    is set - broadcasts too, as in Meshtasticator;
 *  - a node that hears a packet for the first time delivers it if it is for this node or for
 *    everyone, and otherwise, or for everyone, queues a rebroadcast with one hop fewer while hops
 *    are left - unless it is CLIENT_MUTE, which never rebroadcasts;
 *  - hearing the same packet again before the rebroadcast has gone cancels it: the second copy
 *    heard for a client, the third for a router. A packet heard again is never rebroadcast;
 *  - the destination of a direct message that wants one answers with an acknowledgement, a packet
 *    of its own that floods back the same way and is charged as control. With ack_duplicates it
 *    answers every copy it hears, so a sender whose first acknowledgement was lost is answered
 *    when it retries; without, only the first, as in Meshtasticator;
 *  - the sender of a message that wants an acknowledgement waits for one after its frame has gone
 *    out. Hearing any node rebroadcast the message counts (the implicit acknowledgement), as does
 *    the real one. Without either it sends the same frame again, up to `retries` times. Since a
 *    node rebroadcasts a packet only once, a retry is heard only by nodes that missed the first.
 *
 * The routing reports a message finished when it is acknowledged or the last retry's wait runs
 * out, and at once for one that wants no acknowledgement.
 *
 * Background traffic, if `background` is set: the packets a node sends of its own accord, written
 * from the firmware itself (2.7.15, src/mesh/Default.h, src/modules/NodeInfoModule.cpp,
 * PositionModule.cpp, Telemetry/DeviceTelemetry.cpp and src/mesh/MeshService.cpp):
 *
 *  - NodeInfo, its user record, broadcast every nodeinfo_interval (3 h);
 *  - position, every position_interval: 15 min for a client, 12 h for a router - by a node with a
 *    position to send, a share position_share of them, drawn per node;
 *  - device telemetry, every telemetry_interval: 1 h for a client, 12 h for a router.
 *
 * A client stretches the position and telemetry intervals by the number of nodes it has heard in
 * the last two hours - from broadcasts and packets for it, not those it only passes on - itself
 * included, n: by 0.6, 0.7, 0.8 or 1 up to 10, 20, 30 and 40 nodes, then 1 + throttle (n - 40),
 * throttle being 0.075 on most presets, 0.04 on MediumSlow, 0.02 on MediumFast and 0.01 on the
 * Short ones. n can be no more than the node database holds, nodedb_max: 80 on nRF52 boards, 100
 * on most ESP32s, 200 or 250 on an ESP32-S3 with 8 or 16 MB of flash. So 100 nodes heard on
 * LongFast send position every 82.5 min and telemetry every 5.5 h. A router does not stretch them.
 *
 * Position and telemetry are held back while the node's channel utilisation is 25% or more - 40%
 * for a router's telemetry - and tried again when the firmware next looks: every 5 s for position,
 * every minute for telemetry. NodeInfo is skipped at 40%, until its next turn. The utilisation is
 * the firmware's: the time the radio has spent sending or receiving in six 10-second periods, the
 * current one included, over a minute - not the MAC's average over the run.
 *
 * And a client that hears a packet from a node whose NodeInfo it has not had - while its database
 * is not full and the channel is under 25% - sends that node its own NodeInfo, asking for one back,
 * which the other sends as its answer. Neither is sent within 5 min of the node's last NodeInfo. A
 * router does not ask. In a mesh larger than the database, this stops once it fills.
 *
 * Each is a broadcast or a direct message like any other: flooded over hop_limit hops, wanting no
 * acknowledgement, charged as an announce. The node's own go behind its messages and
 * acknowledgements in its queue, as the firmware's BACKGROUND priority does; what it passes on goes
 * in the order heard, as everything relayed does. The sizes are worked out from the protobuf
 * definitions for typical values, the whole frame with its 16-byte header: NodeInfo 100 bytes,
 * position 52, telemetry 50.
 *
 * Each node is taken to have booted at a random time before the run: each kind first goes at a
 * random point of its unstretched interval, and the utilisation's periods start at a random
 * offset. But every node starts knowing no other, as one with its database wiped would, so the
 * NodeInfo exchanges of the first hour are heavier than a settled mesh's: a warmup covers them.
 * The interval stretch reads the count of nodes heard at most every 5 minutes, where the firmware
 * reads it as it changes. Smart position - more often while the node moves - is not modelled.
 *
 * The duty cycle, if duty_cycle is above 0: as Router::send() does, a node whose transmit time in
 * the current minute and the 59 before it is over duty_cycle percent of an hour sends nothing - a
 * rebroadcast or acknowledgement is dropped, and a message of its own is dropped too, booked as
 * TSIM_DROP_DUTY, and waited on as though it had gone, so a retry goes once the hour allows. The
 * firmware checks when a packet is handed to the radio's queue, not when it goes on the air, so
 * what is already queued still goes. While it is over half the duty cycle, as isTxAllowedAirUtil()
 * has it, telemetry waits and NodeInfo skips its turn, coming round again an interval later, as
 * NodeInfoModule::runOnce() does.
 *
 * Meshtasticator starts the acknowledgement wait when the message is queued; this port starts it
 * when the frame has been sent, as a radio that queues behind its own traffic has to. It also
 * cancels a queued frame only once the MAC has waited out its turn, where this port, as the
 * firmware does, cancels when it hears enough copies; ack_poll and cancel_late do it
 * Meshtasticator's way. Its
 * acknowledgement is a 2-byte payload; here it is the payload byte and the 4-byte id it answers,
 * because a receiver has to be told which packet is acknowledged.
 *
 * The MAC. Before each frame it waits a random number of slots, a slot being the time CAD takes
 * plus propagation, turnaround and processing, and sends if the radio is neither sending nor
 * receiving and CAD finds the channel clear; otherwise it draws another wait and tries again.
 *
 *  - a node's own frame waits from 0 to 2^CW slots, CW growing from cw_min to cw_max with the
 *    node's channel utilisation;
 *  - a rebroadcast waits by the SNR it was heard at, so that the farthest node goes first and the
 *    nearer ones hear it and cancel: CW grows from cw_min at snr_min_db to cw_max at snr_max_db. A
 *    router waits 0 to 2CW slots; a client waits out the longest a router could, 2 cw_max slots,
 *    and then 0 to 2^CW more.
 *
 * Looking again at the same instant would find the channel as busy as before, so the wait after
 * finding it busy is drawn only from the waits that take some time - what Meshtasticator's drawing
 * again until one does comes to - and a router whose window is 0 slots waits one.
 *
 * Channel utilisation is the share of the run so far the radio has spent sending or receiving,
 * as Meshtasticator computes it. The firmware averages over a recent window instead.
 *
 * The routing tells its MAC which frames are rebroadcasts, at what SNR, and whether a router sent
 * them, in each frame's hint. The MAC works without the routing - it then treats every frame as
 * the node's own - but the routing's timing assumes this MAC. */

enum tsim_meshtastic_role {
    TSIM_MESHTASTIC_CLIENT,
    TSIM_MESHTASTIC_CLIENT_MUTE,
    TSIM_MESHTASTIC_ROUTER,
};

/* The contention window, which the MAC draws from and the routing's acknowledgement wait is sized
 * by. */
struct tsim_meshtastic_window {
    tsim_time slot;
    uint8_t cw_min; /* the window is 2^cw slots; cw_min <= cw_max <= 15 */
    uint8_t cw_max;
};

/* The firmware's slot for a modulation: 2.5 symbols, the time CAD takes, plus 0.2 ms of
 * propagation, 0.4 ms of turnaround and 7 ms of processing. */
tsim_time tsim_meshtastic_slot(const struct tsim_lora *lora);

/* That slot, with cw_min 3 and cw_max 8. */
struct tsim_meshtastic_window tsim_meshtastic_window_default(const struct tsim_lora *lora);

/* The longest window, in slots, any wait is drawn from: a client's rebroadcast waits up to
 * 2 cw_max + 2^cw_max slots, and the acknowledgement wait counts 2^cw_max + 2 cw_max + 2^cw
 * slots, cw at most cw_max. */
uint64_t tsim_meshtastic_window_slots(const struct tsim_meshtastic_window *w); /* cw_max <= 15 */

/* Whether both plugins can run on the window: cw_min <= cw_max <= 15, a slot above 0 - a MAC that
 * waits no time while the channel is busy would wait at the same instant for ever - and the longest
 * window short enough that a wait of it, and the acknowledgement wait built on it, fit in a
 * tsim_time: no more than TSIM_MESHTASTIC_WAIT_MAX. */
bool tsim_meshtastic_window_valid(const struct tsim_meshtastic_window *w);

#define TSIM_MESHTASTIC_WAIT_MAX (INT64_MAX / 4)

struct tsim_meshtastic_mac_config {
    struct tsim_meshtastic_window window;
    double snr_min_db; /* a rebroadcast heard at or below this waits the least */
    double snr_max_db; /* and at or above this the most; above snr_min_db */
    /* Each time it looks, the MAC also finds the channel busy with this probability: traffic from
     * outside the mesh, Meshtasticator's interference level. 0 to 1. */
    double busy_chance;
};

/* The default window for `lora`, the firmware's SNR range, -20 dB to 10 dB, and no outside traffic.
 */
struct tsim_meshtastic_mac_config tsim_meshtastic_mac_default(const struct tsim_lora *lora);

extern const struct tsim_mac tsim_meshtastic_mac;

#define TSIM_MESHTASTIC_HEADER 16
#define TSIM_MESHTASTIC_OVERHEAD 17 /* the header and the payload's type */
#define TSIM_MESHTASTIC_HOPS_MAX 7

struct tsim_meshtastic_config {
    uint16_t channel;
    struct tsim_lora lora;
    double tx_dbm;
    enum tsim_meshtastic_role role;
    /* Routers picked as distvec's infrastructure and meshcore's repeaters can be: with relay_pick
     * other than list - an enum tsim_distvec_pick - the driver picks relay_count nodes and points
     * relay_set at them, [node] 1; those are routers, and the rest take `role`. */
    uint8_t relay_pick;
    uint32_t relay_count;
    const uint8_t *relay_set;
    uint8_t hop_limit; /* rebroadcasts a packet may have, at most TSIM_MESHTASTIC_HOPS_MAX */
    bool want_ack;
    bool ack_duplicates;  /* acknowledge every copy of a message, not only the first */
    uint8_t retries;      /* sends after the first, for a message no one acknowledges */
    tsim_time processing; /* added to the acknowledgement wait, at most TSIM_MESHTASTIC_WAIT_MAX */
    /* The noise a rebroadcast's SNR is reckoned from, if not NaN: its SNR is then its RSSI less
     * this, as Meshtasticator reckons it from -119.25 dBm, rather than what the radio measured. */
    double noise_dbm;
    /* Meshtasticator's sender loop: the acknowledgement wait starts when the frame is queued, not
     * when it has gone, and an acknowledgement is noticed only when the wait runs out - which is
     * also when the message is finished, so the node's next message waits for it. */
    bool ack_poll;
    /* Meshtasticator's cancelling: a rebroadcast or retry no longer wanted stays queued, and is
     * withdrawn only when the MAC has waited out its turn and comes to send it. */
    bool cancel_late;
    struct tsim_meshtastic_window window;
    /* Background traffic, as described above; off by default. Each interval 0 is the firmware's
     * default for the node's role. */
    bool background;
    tsim_time nodeinfo_interval;
    tsim_time position_interval;
    tsim_time telemetry_interval;
    double position_share; /* 0 to 1 */
    uint16_t nodedb_max;   /* 2 to 250 */
    double throttle;       /* the interval stretch for each node heard over 40 */
    /* The region's duty cycle, as a percentage of the hour, as the firmware holds to it - 10 for
     * EU_868 - or 0 for none, as in the US. See above. */
    double duty_cycle;
};

/* The firmware's interval stretch for the preset `lora` matches - 0.04 for SF10 at 250 kHz, 0.02
 * for SF9 at 250 kHz, 0.01 for SF7 or SF8 at 250 kHz and SF7 at 500 kHz - and 0.075 for anything
 * else. */
double tsim_meshtastic_throttle(const struct tsim_lora *lora);

/* A client on `lora`, three hops, acknowledgements wanted, three retries, 4.5 s of processing,
 * duplicates acknowledged, and the SNR the radio measured. Background traffic is off; on, it takes
 * the firmware's intervals, a position for every node, a database of 100 and the preset's
 * throttle. */
struct tsim_meshtastic_config tsim_meshtastic_default(uint16_t channel,
                                                      const struct tsim_lora *lora, double tx_dbm);

extern const struct tsim_routing tsim_meshtastic;

#endif
