#ifndef TSIM_DISTVEC_H
#define TSIM_DISTVEC_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/node.h"
#include "tsim/time.h"

/* Candidate 3: announce-driven distance-vector routing - Tern's own design, Design Record §3.6, as
 * the research synthesis of 2026-10-01 left it. Unlike candidates 1 and 2 it is not a port of
 * anything: it is written from RFC 8966 (Babel), RFC 6206 (Trickle) and De Couto et al.'s ETX
 * (MobiCom 2003), never from an implementation's code. Where it departs from Babel it says so
 * below. It has no MAC of its own: until Tern's slotted MAC exists it runs over candidate 2's
 * dispatcher or candidate 1's contention window, whichever the scenario names.
 *
 * Addresses are node numbers, four bytes on the air - what Tern's blinded tags would cost. Every
 * number on the air is little-endian.
 *
 * Tiers. `relays` names the infrastructure nodes; the rest are leaves. Only infrastructure
 * forwards and re-announces other nodes' routes. A leaf announces itself, with its link
 * measurements, and nothing else. A route through a neighbour is usable only if that neighbour is
 * infrastructure or is itself the destination, so routing runs over the infrastructure graph and a
 * leaf is reached through the infrastructure node that hears it. "all" makes every node
 * infrastructure: an untiered mesh, for measuring what tiering is worth.
 *
 * Announces. A node's one periodic frame, charged as announce:
 *
 *     type 0x01 | sender 4 | announce seq 2 | source seq 2 | flags 1 | promise 2 | round 2
 *     | ihu count 1 | route count 1 | ihu: neighbour 4, receive rate 1 ...
 *     | route: destination 4, seq 2, metric 2 ...
 *
 * The announce seq numbers the sender's announces, so a receiver learns how many it missed - the
 * hello of Babel. The source seq is the sender's own route's sequence number; its route to itself,
 * metric 0, is implied. The flags say whether it is infrastructure. The round is how many frames it
 * takes the sender to name every neighbour it hears in its IHUs, a few to a frame. The promise is
 * the longest the sender may go before it announces again - Babel's hello interval,
 * for a sender whose interval Trickle varies: what is left of its current interval - an announce
 * its cap held back may go at any point of one
 * - then every interval it may keep quiet and the one it then announces in, each doubling up to
 * imax and taken whole, the time its cap takes to pay for a full frame, and the longest its
 * announces have lately waited in the MAC's queue, which the routing cannot bound - a MAC keeping
 * its own duty cycle, say. It is in seconds up to 32767, then with the top bit set in minutes up to
 * 32766, rounded up; all ones is no promise, a sender held back longer still, whose silence counts
 * no announce missed and never has it forgotten: only frames sent to it that fail count against it.
 * A configuration that would let Trickle and the cap alone keep a node quiet for more than 65535 s
 * is refused. Each IHU
 * ("I heard you") is the share of a neighbour's announces the sender received, in 255ths. The
 * routes are the sender's selected routes: what it would forward through, with the sequence number
 * of the source and the metric from the sender. A metric of 0xFFFF retracts a route. A table too
 * big for one frame goes out in slices: changed routes first, then the rest in turn, so every route
 * is repeated every so many announces. An announce the node's queue refuses is undone: it is
 * neither numbered nor charged to the cap, its round of IHUs goes back to where it was, and its
 * routes go back on the list of changes, so a retraction is never lost to the queue. One lost on
 * the air would leave a neighbour with the route, since routes do not expire, so a retraction goes
 * in three announces, the first as a change and the others in turn with the routes, never two in
 * one frame; and a node asked to forward towards a destination it retracted, which means the hop
 * before missed all three, retracts it again.
 *
 * Links. A neighbour is a node whose announces this node hears. Its receive rate d_f is the share
 * of its last 16 announces heard (Babel's hello history), with one more counted missed for every
 * promise it has let pass since it was last heard; the rate it reports back, d_r, is its IHU for
 * this node. The link's ETX is 1 / (d_f d_r), and a neighbour without an IHU for this node - one
 * that has not heard it - is not used at all: LoRa links are often one-way, and Babel assumes they
 * are not. Nor is one with an ETX over etx_max. A neighbour that has sent more announces without
 * naming this node than its round allows - one, when its round is a single frame; otherwise twice
 * the round and one more, for the frames that name new neighbours first - does not hear it, and its
 * IHU is taken away. That is counted in announces, not time, so however slowly a node with many
 * neighbours announces, its IHUs stand until their turn comes round again. A neighbour not heard
 * for neighbour_timeout, and for two of its promises, is forgotten; one that frames sent to it keep
 * failing to reach costs more and more (below).
 *
 * The metric is ETX times time on air: the link's cost is its ETX times the airtime of a
 * reference frame, ref_len bytes at the link's modulation, in milliseconds, at least 1. Every link
 * runs the one modulation until the slotted MAC lets links choose their own, so for now the
 * airtime is the same on every link and the metric is ETX times a constant. There is no queue
 * term. A route's metric is the neighbour's advertised metric plus the link's cost, up to 0xFFFE.
 *
 * Selection is Babel's. Each node keeps, for every source, a feasibility distance: the best
 * (seq, metric) it has itself advertised. An advertisement from a neighbour is feasible if its
 * seq is newer, or the same and its metric lower, so no route this node selects can lead back
 * through it - which is what keeps the routes loop-free while they change. Among feasible routes
 * through usable neighbours the lowest metric wins, with hysteresis: the current route is kept
 * unless another beats it by `hysteresis`. A node announces the metric it last announced again
 * while the true one stays within `change` of it at the same seq, so the noise in a link's ETX
 * does not drag the feasibility distance down to its luckiest moment; what it announces is still
 * above its next hop's, so this keeps the routes loop-free too.
 *
 * A node whose routes to a source are all infeasible is starved, and asks for a newer seq with a
 * seqno request. Requests for the same next hop share a frame:
 *
 *     type 0x02 | next hop 4 | count 1 | request: source 4, seq 2, hops 1 ...
 *
 * A request goes to the neighbour with the best of the infeasible routes, or to every neighbour
 * (next hop 0xFFFFFFFF) if none can be used, and a node that cannot answer it passes it on to its
 * next hop towards the source while hops last. The source answers by raising its seq and
 * announcing it - its next announce, which Trickle never suppresses, at imin - and a node with
 * a route at that seq or newer answers by announcing it. A node that has never announced a route
 * to the source has no feasibility distance, so nothing a seq could be newer than: it sends a
 * route request instead, a request with hops 0, which any neighbour with a route answers and none
 * passes on. A starved node asks at once, unless it asked within request_interval, and again every
 * request_interval until it has a route, five times in all.
 *
 * Departures from Babel: a node keeps at most four routes per source - always the selected one,
 * and otherwise those it could select now before those it could not, then the newer seq, then the
 * lower metric - as a node with a few kilobytes for its table would have to; there are no
 * route expiry timers, a route lasting as long as its neighbour does unless it is retracted; there
 * are no unicast hellos or IHU intervals of their own, everything riding on the announce; and the
 * announced metric is sticky, as above.
 *
 * When to announce: Trickle. Each node's announce interval runs from imin, doubling to imax; in
 * each interval it announces once, at a random point in its second half, unless it has heard
 * `redundancy` consistent announces in that interval already - or has changed routes of its own
 * waiting to go, or a request for its own seq to answer, which are never suppressed. An announce is
 * consistent unless it changes this node's routes. Anything that does - a route gained, lost or
 * moved to another neighbour, a metric that changes by more than `change` since it was last
 * announced, a newer seq, a neighbour found or lost - is an inconsistency, and sends the interval
 * back to imin. As in RFC 6206 an inconsistency at imin does nothing; departing from it, an
 * interval that ends with changes still waiting does not double, so a change that comes after the
 * node has announced in an imin interval goes within the next, not in the second half of one twice
 * as long. A request restarts the interval only when it raises the seq: one for a seq already
 * reached, at imin, leaves the firing to come where it was, so a stream of them cannot put the
 * answer off. A node that has kept quiet quiet_max intervals running announces anyway, so its
 * neighbours do not forget it.
 *
 * The cap. Announces and seqno requests together may take no more than `cap` of a node's time:
 * request_share of it for requests and the rest for announces, each kept as a token bucket that
 * holds cap_window of its share - at least one full-length frame - and starts full. Kept apart,
 * a node asking about many routes cannot spend what it needs to be heard at all. An announce is
 * built only when its bucket can pay for the longest it could come to, and charged its own
 * airtime when it is queued; otherwise it waits until the bucket refills. A request its bucket
 * cannot pay for is dropped, and asked again, as is one the queue refuses, uncharged. Neither share
 * of the ledger runs over its part of the cap by more than its bucket. An announce event sends up
 * to `burst` frames, as long as there are changed routes left to send and the bucket allows.
 *
 * Jitter. Every frame a node sends in answer to one it received - a relay, an acknowledgement, a
 * request passed on - waits a random time up to `jitter` of its own airtimes before it is queued,
 * and a node's own requests wait as long for a frame of eight. The nodes that heard the same frame
 * would otherwise answer it at the same instant, and a MAC that sends as soon as the channel is
 * clear, as candidate 2's does, would have them collide every time.
 *
 * Data. A message is
 *
 *     type 0x03 | next hop 4 | source 4 | destination 4 | id 4 | hops 1 | content
 *
 * sent to the next hop of the source's route and forwarded by each next hop in turn, with one hop
 * fewer, for at most hop_max hops; with no route it is not forwarded. The destination answers each
 * copy with an acknowledgement routed back the same way, charged as control:
 *
 *     type 0x04 | next hop 4 | acknowledger 4 | destination 4 | id 4 | hops 1
 *
 * Each hop listens for its next hop sending the frame on - the implicit acknowledgement; for the
 * last hop of a message, hearing the destination's acknowledgement does. Without it after
 * hop_wait, plus twice the frame's airtime from when it went, the hop sends the frame again, up to
 * hop_retries times, and then counts it against the link as four of the neighbour's announces
 * missed. A link that keeps losing frames soon costs more than another, or more than etx_max
 * allows; one that lost a frame to a busy moment recovers as its announces come in. The last hop
 * of an acknowledgement has nothing to hear and sends once. A forwarder that is sent a frame it
 * has already forwarded sends it on again if it no longer has it waiting, so a hop whose implicit
 * acknowledgement was lost hears one.
 *
 * The source waits for the acknowledgement ack_wait plus ack_factor times the route's metric in
 * milliseconds - the metric being airtime, it is a round trip's worth - counted from when its frame
 * goes on the air, however long it queued, or from when the queue refused it; and without one sends
 * the message again, up to `retries` times. A source with no route to the destination asks for one
 * and counts that as an attempt, waiting ack_wait. It gives up after the last. Answered or given
 * up, nothing of the message goes on the air again: an attempt still queued is taken back, and
 * those sent are no longer listened for or sent again.
 *
 * Broadcast, for now: a flood that infrastructure relays once,
 *
 *     type 0x05 | source 4 | id 4 | hops 1 | content
 *
 * with up to bcast_hops relays along any path, each relay waiting a random time up to bcast_window
 * airtimes of the frame and dropping it if it hears bcast_cancel copies first. Scoped broadcast
 * is MSH-34's.
 *
 * Power control (MSH-45), when `power` is on. A frame sent to one neighbour - a message's hop, an
 * acknowledgement's - goes only as loud as that neighbour needs, so it takes the channel from
 * fewer of the nodes around it: topology control, as in the ad-hoc literature. Announces, requests
 * and broadcasts still go at tx_dbm, since they are for every neighbour. Each announce then carries
 * the power it went at, one signed byte of dBm after the route count, which makes the head 17
 * bytes; and so does every data and acknowledgement frame, after the hops, which makes theirs 19.
 * The SNR a frame is heard at, less the SNR floor, is how much quieter it could have gone and
 * still been heard: what it went at less that is the neighbour's floor, the quietest it would
 * decode this node at, the channel's mean loss being the same both ways. A node keeps each
 * neighbour's floor, from its announces, as an average that weighs the latest a quarter.
 *
 * A frame to a neighbour goes at its floor plus margin_db plus the neighbour's boost, rounded up to
 * a whole dB and kept between tx_min_dbm and tx_dbm; at tx_dbm while the floor is unknown. A frame
 * sent on in answer to one received - a relay, or the destination's acknowledgement - goes loud
 * enough, too, for the node it came from, at that frame's floor plus margin_db: that node listens
 * for it, its implicit acknowledgement. Each try a hop sends again goes step_db louder, and a hop
 * given up adds step_db to the neighbour's boost, which one heard passed on takes 1 dB from: a
 * link that loses frames at the power it was given gets more, and gives it back as they get
 * through.
 *
 * With power_k, frames for every neighbour - announces, requests and broadcasts - go only as loud
 * as the power_k neighbours with the lowest floors need, with margin_db, rounded up and kept
 * between tx_min_dbm and tx_dbm: k-neighbour topology control (Blough et al., KNeigh), so a node in
 * a crowd hears and is heard by a few near neighbours, not hundreds. It goes at tx_dbm until it
 * knows power_k floors, which its first announces heard tell it, and is worked out again for each
 * announce and every housekeeping round. A neighbour quieter than this node needs to hear it is not
 * heard, and so not used: links stay ones heard both ways.
 *
 * Not yet here, and left out of MSH-41 for issues of their own: the store-and-forward floor, which
 * only shows its worth under mobility and churn, and per-link modulation, which needs the slotted
 * MAC. */

#define TSIM_DISTVEC_METRIC_INF 0xFFFF

struct tsim_distvec_config {
    uint16_t channel;
    struct tsim_lora lora;
    double tx_dbm;
    char relays[96]; /* infrastructure, as tsim_meshcore_config.relays: "all" or "0-45,50" */

    tsim_time imin;    /* Trickle */
    uint8_t doublings; /* imax is imin times 2^doublings, 0..16 */
    uint8_t redundancy;
    uint8_t quiet_max;
    tsim_time neighbour_timeout;

    double cap;           /* the share of time routing may take, above 0 and at most 1 */
    double request_share; /* of the cap, for seqno requests: above 0 and below 1 */
    tsim_time cap_window; /* how much of its share each bucket holds */
    uint8_t burst;        /* frames an announce event may send, 1..16 */
    uint8_t ihu_max; /* IHU entries per announce frame, 0..48, less what leaves no route room */

    uint32_t ref_len;  /* bytes of the reference frame the metric is reckoned in */
    double etx_max;    /* at least 1 */
    double hysteresis; /* 0 to 1 */
    double change;     /* 0 to 1 */
    tsim_time request_interval;

    uint8_t hop_max;
    uint8_t hop_retries;
    tsim_time hop_wait;
    uint8_t retries;
    tsim_time ack_wait;
    double ack_factor;
    double jitter; /* airtimes a frame sent in answer to one received waits, at most */

    uint8_t bcast_hops;
    double bcast_window;
    uint8_t bcast_cancel; /* copies heard, its own first one included, 0 for never */

    bool power;          /* power control, as above; off, all go at tx_dbm */
    double tx_min_dbm;   /* the quietest a frame goes, a whole dBm at least below tx_dbm */
    double margin_db;    /* above the quietest a neighbour decodes at, 0 to 60 */
    double step_db;      /* added for each try a hop has lost, 0 to 60 */
    double snr_floor_db; /* the lowest SNR the modulation demodulates at */
    uint8_t power_k;     /* neighbours frames for all of them reach; 0 for tx_dbm */
};

/* Every node infrastructure; Trickle from 8 s to 8 min (six doublings), redundancy 3, announcing
 * at least every third interval and forgetting a neighbour after an hour; a 0.5% cap, a quarter
 * of it for requests, with 1-minute buckets; up to 4 frames an event and 8 IHUs a frame; a
 * 32-byte reference frame, ETX up to 8, 10% hysteresis and a 25% change threshold, a request
 * every 10 s while starved; a jitter of up to 2 airtimes; 32 hops, 2 hop retries after 4 s, 3
 * retries waiting 5 s plus 4 times the metric; broadcasts over 4 hops, waiting up to 3 airtimes
 * and dropped on the second copy heard. Power control on, with power_k 8 - without it, the
 * region's unicast fell from 22% to 2% as density rose (MSH-45) - frames going no quieter than
 * -9 dBm, the SX1262's least, with a 10 dB margin and 3 dB more for each try lost, and the SNR
 * floor Semtech's for the SF: -7.5 dB at SF7, 2.5 dB lower for each SF above.
 *
 * The cap is per node, so in a neighbourhood of n nodes routing may take n times it of the
 * channel: 2% - Reticulum's announce cap - saturated a 200-node town at SF9, which 0.5% did not.
 * It is a parameter to sweep, not a settled number. */
struct tsim_distvec_config tsim_distvec_default(uint16_t channel, const struct tsim_lora *lora,
                                                double tx_dbm);

/* Why `config` would be refused, or NULL if it would not. */
const char *tsim_distvec_check(const struct tsim_distvec_config *config);

extern const struct tsim_routing tsim_distvec;

/* --- For tests and reports: never for another plugin --- */

/* A node's selected route to `dst`, from its routing instance (tsim_net_routing()): the next hop
 * and the metric, or false if it has none. A node's route to itself is not a route. */
bool tsim_distvec_route(const void *self, uint32_t dst, uint32_t *next, uint16_t *metric);

/* How many neighbours the node can use: heard both ways, within etx_max. */
uint32_t tsim_distvec_neighbours(const void *self);

/* The node's current Trickle interval. */
tsim_time tsim_distvec_interval(const void *self);

/* The promise the node would make if it announced now: the longest it may then go before it
 * announces again. */
tsim_time tsim_distvec_promise(const void *self);

/* The IHU round its last announce told: how many frames it takes to name every neighbour. */
uint32_t tsim_distvec_round(const void *self);

/* What a frame from the node to neighbour `nb` would go at, answering none, and what its frames for
 * every neighbour go at, in dBm. */
double tsim_distvec_power(const void *self, uint32_t nb);
double tsim_distvec_node_power(const void *self);

#endif
