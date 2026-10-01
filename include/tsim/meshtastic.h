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
 *    of its own that floods back the same way and is charged as control;
 *  - the sender of a message that wants an acknowledgement waits for one after its frame has gone
 *    out. Hearing any node rebroadcast the message counts (the implicit acknowledgement), as does
 *    the real one. Without either it sends the same frame again, up to `retries` times. Since a
 *    node rebroadcasts a packet only once, a retry is heard only by nodes that missed the first.
 *
 * Meshtasticator starts the acknowledgement wait when the message is queued; this port starts it
 * when the frame has been sent, as a radio that queues behind its own traffic has to. Its
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
};

/* The default window for `lora`, and the firmware's SNR range, -20 dB to 10 dB. */
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
    uint8_t hop_limit; /* rebroadcasts a packet may have, at most TSIM_MESHTASTIC_HOPS_MAX */
    bool want_ack;
    uint8_t retries;      /* sends after the first, for a message no one acknowledges */
    tsim_time processing; /* added to the acknowledgement wait, at most TSIM_MESHTASTIC_WAIT_MAX */
    struct tsim_meshtastic_window window;
};

/* A client on `lora`, three hops, acknowledgements wanted, three retries, 4.5 s of processing. */
struct tsim_meshtastic_config tsim_meshtastic_default(uint16_t channel,
                                                      const struct tsim_lora *lora, double tx_dbm);

extern const struct tsim_routing tsim_meshtastic;

#endif
