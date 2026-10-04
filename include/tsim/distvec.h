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
 * Leaves and their parents (MSH-53), when `leaves` is parent_oracle. Above, infrastructure still
 * keeps and repairs a route to every leaf, so a tier of 200 relays in 1000 nodes repairs 1000
 * destinations with fewer paths around each break. Here it routes among itself only: no node
 * announces a route to a leaf, and a relay keeps its routes to the leaves it hears only for the
 * last hop. Each leaf takes a parent: of the infrastructure neighbours it can use, the one with
 * the best ETX, kept unless another beats it by `hysteresis`. A frame for a leaf this node has no
 * route to goes along its route to that leaf's parent, which hands it over; one for a leaf with no
 * parent, or a parent this node is but cannot reach the leaf from, goes nowhere, and nothing is
 * asked about a leaf. With power_k, frames for every neighbour reach the power_k nearest
 * infrastructure neighbours rather than the power_k nearest of any kind: a leaf must be heard by
 * relays, and a relay by its leaves and by the relays it routes over.
 *
 * Where a leaf's parent is, the node routing to it learns, for now, from an oracle: every leaf
 * writes its parent into `parents`, which every node reads, and a relay is its own parent - what a
 * lookup never wrong and never late would tell. It measures what routing over relays could gain
 * before the binding a real network would need, the leaf naming its parent and a source finding
 * that out, is designed.
 *
 * What it gained on the region (3 seeds, 100 to 400 relays, 0 and 20 dBm): unicast within 4
 * points of routed leaves either way - 22.7% against 20.5% with 200 relays at 0 dBm, 7.9% against
 * 8.6% at 20 dBm - where the oracle's routes deliver 58% and 97%; broadcast and deliveries per
 * second of airtime up, 13.4% to 25.4% and 3.5 to 5.2 with 200 relays at 20 dBm, mostly from
 * power_k counting relays only. The smaller table is not what was missing. After a quiet warmup,
 * relays' routes to each other reach 71% of pairs at 0 dBm, near the 69% the oracle finds
 * connected, and 75% at 20 dBm; after three hours of traffic, 21% and 8%. Load takes links down
 * faster than repair under the cap brings routes back, however few destinations there are.
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
 * naming this node than ihu_rounds of its rounds allow - ihu_rounds times the round, and one more
 * for the frames that name new neighbours first, never more than 32767 - does not hear it, and its
 * IHU is taken away. Two rounds cut thousands of good links an hour in the region, under load and
 * even on a quiet channel: most frames are lost there, and two IHUs in a row often enough. Each cut
 * starves the routes through it until a new seqno comes, which the saturated queue of urgent
 * updates takes hours to carry. Eight rounds made the region's unicast 18.8% to 25.0% at 0 dBm
 * and 9.8% to 13.9% at 20 dBm, for 3-5% of its deliveries per second of airtime, and the
 * town's 81.6% to 98.5% (MSH-52). The count is in announces, not time, so however slowly a node
 * with many neighbours announces, its IHUs stand until their turn comes round again. A neighbour
 * not heard for neighbour_timeout, and for two of its promises, is forgotten; one that frames sent
 * to it keep failing to reach soon goes unused (below).
 *
 * The metric is time on air: a link's cost is the airtime of a reference frame, ref_len bytes at
 * the link's modulation, in milliseconds, at least 1, and with `etx` that times the link's ETX.
 * Every link runs the one modulation unless per-link SF (below) is on, so the airtime is the same
 * on every link: the metric counts hops, or with `etx`, ETX times a constant. ETX is off by default
 * because, measured from announces heard, it measures the load on the channel more than the link:
 * as traffic loads the region its routes' metrics rise, fail the feasibility condition below, and
 * starve the nodes downstream, so on the region at 0 dBm unicast on time is 8% with it and 19%
 * without (MSH-48). It still decides, against etx_max, whether a link is used at all; 32 rather
 * than 8 lets a link the load is costing announces stay up, and still drops one that has stopped
 * carrying. There is no queue term. A route's metric is the neighbour's advertised metric plus the
 * link's cost, up to 0xFFFE.
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
 * missed. A link that keeps losing frames soon goes over etx_max and unused - with `etx`, it costs
 * more than another first - and one that lost a frame to a busy moment recovers as its announces
 * come in. The last hop of an acknowledgement has nothing to hear and sends once. A forwarder that
 * is sent a frame it has already forwarded sends it on again if it no longer has it waiting, so a
 * hop whose implicit acknowledgement was lost hears one.
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
 * the power it went at, one signed byte of dBm rounded up, after the route count, which makes the
 * head 17 bytes; and so does every data and acknowledgement frame, after the hops, which makes
 * theirs 19. The SNR a frame is heard at, less the SNR floor, is how much quieter it could have
 * gone and still been heard: what it went at less that is the neighbour's floor, the quietest it
 * would decode this node at, the channel's mean loss being the same both ways. A node keeps each
 * neighbour's floor, from its announces, as an average that weighs the latest a quarter.
 *
 * A frame to a neighbour goes at its floor plus margin_db plus the neighbour's boost, rounded up to
 * a whole dB and kept between tx_min_dbm and tx_dbm, which itself need not be whole; at tx_dbm
 * while the floor is unknown. A frame sent on in answer to one received - a relay, or the
 * destination's acknowledgement - goes loud enough, too, for the node it came from, at that frame's
 * floor plus margin_db: that node listens for it, its implicit acknowledgement. Each try a hop
 * sends again goes step_db louder, rounded up, and a hop given up adds step_db to the neighbour's
 * boost, which one heard passed on takes 1 dB from: a link that loses frames at the power it was
 * given gets more, and gives it back as they get through.
 *
 * With power_k, frames for every neighbour - announces, requests and broadcasts - go only as loud
 * as the power_k neighbours with the lowest floors need, with margin_db, rounded up and kept
 * between tx_min_dbm and tx_dbm: k-neighbour topology control (Blough et al., KNeigh), so a node in
 * a crowd hears and is heard by a few near neighbours, not hundreds. It goes at tx_dbm until it
 * knows power_k floors, which its first announces heard tell it, and is worked out again for each
 * announce and every housekeeping round. A neighbour quieter than this node needs to hear it is not
 * heard, and so not used: links stay ones heard both ways.
 *
 * Periodic seqnos (MSH-54), when seq_period is above 0: every seq_period, give or take 10%, a node
 * raises its own seq and announces it at once, as if a request had asked it to - DSDV's periodic
 * sequence numbers, under Babel's feasibility condition, so every route to it that feasibility
 * starved is feasible again once the new seq arrives, whether or not a request ever got through.
 * Off by default: measured on the region with 200 relays and parent_oracle, it did not help. Of
 * 200-300 thousand seqno requests an hour, 15-25 raised a seq there; with a new one every 5 to 30
 * minutes instead, a relay held 56 destinations unrouted rather than 95 at 0 dBm, but relay to
 * relay reach stayed near 20%, the urgent list grew fourfold and unicast fell 1-6 points: the new
 * seqs crossed eight hops of announces under load no better than the routes did.
 *
 * Routes on demand (MSH-43), when `routes` is demand - work in progress, and on the region worse
 * than proactive so far (below). A node announces itself and its IHUs and no other route, and
 * routes come from the traffic, under the same feasibility condition:
 *
 *  - every data, acknowledgement and route reply frame carries, after the head (and the power
 *    byte), the sender and its route to the frame's source - sender 4, seq 2, metric 2, life 2 -
 *    which any node that decodes it takes as the sender advertising that route, for `life`
 *    seconds at most. Putting a route in a frame sets the feasibility distance, as announcing it
 *    would, and a route goes out with what is left of its life, so a node never keeps one longer
 *    than the neighbour it had it from;
 *  - a node with a message and no route to where it goes floods a route request through
 *    infrastructure, at most once a request_interval for each destination, raising its own seq
 *    first, as AODV does, so the routes back to it are feasible wherever the request goes:
 *
 *        type 0x06 | origin 4 | id 4 | target 4 | target seq 2 | flags 1 | hops 1 | sender 4
 *        | origin seq 2 | metric 2 | life 2
 *
 *    Each node that hears it takes the route to the origin it carries; infrastructure that has
 *    not seen it, and now has a route back, passes it on with its own route, after a wait of up to
 *    bcast_window airtimes, dropping it on hearing req_cancel copies, for up to req_hops relays;
 *  - only the target answers - in a dense town every node with a route answering made eight
 *    replies a request - raising its seq, to at least the one asked for, and sending a route reply
 *    along the reverse route, laid out as an acknowledgement is, with itself as the source and the
 *    origin as the destination. Each hop takes the route to the target from it and passes it on,
 *    listening for it to go on as a data frame's hop does. A reply that reaches the origin sends
 *    the messages waiting on that route at once;
 *  - a route lasts route_ttl from when it was last heard of, and then is gone: nothing retracts
 *    it. A relay with no route for a message holds it, up to PARKED_MAX of them, for two
 *    request_intervals while it asks for one;
 *  - a leaf with parent_oracle sends what it has no route for to its parent. Leaves never pass
 *    requests on.
 *
 * Measured on the region (200 relays, parent_oracle, 3 seeds), unicast on time was 12-17% at 0 dBm
 * and 7-8% at 20 dBm, against 22.7% and 7.9% proactive. Requests took 45-50% of the airtime at
 * 0 dBm and reached the target 30% of the time: a request reached about 60 of the 200 relays, and
 * most that heard it at 20 dBm could not pass it on, holding no usable link back to the relay
 * they heard it from. Announces did not get cheaper either: without routes they were shorter but
 * more often, as many as the cap allowed - at 20 dBm Trickle wanted more than it.
 *
 * Per-link SF (MSH-49), when sf_min is above 0 - with links by strength, power control and
 * proactive routes. A strong link can go faster than the radio's SF: SF7 takes about a third of
 * SF9's airtime, for 5 dB less reach. A radio decodes only the SF it listens on, so sender and
 * receiver must meet. Here the receiver chooses and tells, as Tern's slotted MAC would not need
 * it to:
 *
 *  - each node listens on one SF, from sf_min to the radio's, and announces it in bits 4-6 of its
 *    flags, the SF less 7. It starts on the radio's. At every announce and housekeeping round it
 *    takes the fastest SF at which it keeps as many links as it would have on the radio's, or
 *    sf_k if that is more: neighbours - infrastructure, with parent_oracle - whose margins, each
 *    less the gap for the SF it is heard at, are link_margin_db or more, and link_band_db more to
 *    go faster. The gap is Semtech's, 2.5 dB an SF. A change of SF judges every link again and
 *    restarts Trickle, so its neighbours learn of it soon;
 *  - a frame to one neighbour goes at the SF that neighbour listens on, louder by the gap: a
 *    data, acknowledgement or probe frame, a request to one next hop. A link's cost is the
 *    reference frame's airtime at that SF, so routes favour the fast links;
 *  - a frame for every neighbour - an announce, a request to all, a broadcast - goes once at each
 *    SF a neighbour it has heard listens on, or at every SF from sf_min while it has heard none,
 *    so a node coming up is heard by whoever is there. Copies of an announce share its number,
 *    and its cap pays for them all; each bucket holds a full frame at every SF it may use. The
 *    neighbours being those it ever heard, a node keeps sending at the SF of one it can no longer
 *    reach until it forgets it, which keeps links between nodes on different SFs discoverable;
 *  - the implicit acknowledgement fails when a relay passes a frame on at an SF the hop before does
 *    not listen on. So every data and acknowledgement frame says, in a byte after the power byte,
 *    which SF its sender listens on, which makes their head 20 bytes; and a node that passes such
 *    a frame on - or answers a message with an acknowledgement - at another SF sends the hop
 *    before a hop acknowledgement at its own SF, charged as control:
 *
 *        type 0x0A | sender 4 | source 4 | destination 4 | id 4 | hops 1 | power 1 | type 1
 *
 *    the head of the frame passed on, as the next hop would carry it, with its type at the end.
 *    The hop before takes it as it would hearing the frame passed on.
 *
 * The MAC is untouched: it senses the channel only on the SF its radio listens on, as a radio
 * must, so a node listening fast does not hear a frame on the radio's SF before it sends one.
 * Sensing other SFs, or meeting without the copies, is the slotted MAC's to give.
 *
 * Off by default. Measured with tools/density.py (cds 200, parent_oracle, 3 seeds), unicast on
 * time from -5 to 20 dBm was 57.1%, 90.1%, 96.7%, 98.3% and 98.5% with sf_min 7, against 57.8%,
 * 66.3%, 63.4%, 68.1% and 70.3% off. But the region at one faster SF did as well for far less:
 * SF7 made 47.6%, 88.9%, 99.0%, 99.3% and 99.4%, and SF8 68.2%, 92.7%, 93.5%, 93.2% and 95.8%.
 * Per-link SF never beat the best single SF, and took 3 to 5 times its airtime per delivery: at
 * 0 dBm 8.2 deliveries a second of airtime against 25 at SF8 and 42 at SF7, and 11.7 at SF9.
 * Most of it is copies: with 70% of nodes still on SF9 there, every relay sends broadcasts and
 * announces at SF9 and a faster SF too; sending only at the SFs of neighbours it has links to
 * changed nothing (seed 1, -5 to 5 dBm). At -5 dBm it gained nothing, though SF8 alone gains 10
 * points: a node keeps every link it has, so a sparse network stays slow. Keeping fewer links, sf_k
 * 4, 2 or 1, went faster and lost routes: 59.2%, 52.3% and 30.8% at -5 dBm, 87.4%, 81.9% and 77.8%
 * at 0 (seed 1). Ending the ladder at SF8 made 61.7% and 88.2%.
 *
 * Where it pays is uneven density, which no one SF fits: the region's nodes half in a 3 km
 * town at its middle and half spread as before (3 seeds). At 0 dBm per-link SF made 83.7%,
 * against 58.1% off, 82.2% at SF8 and 81.2% at SF7, which cut the spread nodes off - relays'
 * routes reached 35-49% of pairs, against 75-87%. At 10 dBm SF7 joined them up and made 96.4%,
 * per-link SF 87.7%. The choice of one SF for the whole network matters far more than letting
 * links choose: from 0 dBm up the region wants SF7 or SF8, not SF9, whatever the routing.
 *
 * Not yet here, and left out of MSH-41 for an issue of its own: the store-and-forward floor,
 * which only shows its worth under mobility and churn. */

#define TSIM_DISTVEC_METRIC_INF 0xFFFF
#define TSIM_DISTVEC_NO_PARENT 0xFFFFFFFFu

/* Where routes come from: announced to every node, or found when traffic needs them. */
enum tsim_distvec_routes {
    TSIM_DISTVEC_ROUTES_PROACTIVE,
    TSIM_DISTVEC_ROUTES_DEMAND,
};

/* How a leaf is reached: by routes to it that infrastructure announces, or through its parent. */
enum tsim_distvec_leaves {
    TSIM_DISTVEC_LEAVES_ROUTED,
    TSIM_DISTVEC_LEAVES_PARENT_ORACLE,
};

/* How a node judges its links: by announces counted and IHUs, from the oracle's (below), or by
 * how strongly each end hears the other (MSH-58, below). */
enum tsim_distvec_links {
    TSIM_DISTVEC_LINKS_SENSED,
    TSIM_DISTVEC_LINKS_ORACLE,
    TSIM_DISTVEC_LINKS_STRENGTH,
};

/* Which nodes are infrastructure (MSH-56): those `relays` names, or `relay_count` the driver picks
 * over the oracle's links - every pair of nodes each decoding the other at tx_dbm with
 * oracle_margin_db to spare, by the mean loss - before the network starts:
 *  - degree: the nodes with the most links;
 *  - spaced: by position, the node nearest the middle of them all first, then each time the node
 *    farthest from every one picked so far;
 *  - cds: a greedy connected dominating set (Guha and Khuller, Algorithmica 1998): from the node
 *    with the most links, each time the node next to those picked that brings the most nodes still
 *    without a relay next to them, so every node either is one or has one as a neighbour and the
 *    relays of each part of the network join up. With a relay_count it stops there, and tops up
 *    with the most linked nodes if it finished short; 0 takes as many as it needs.
 * Which nodes are relays is the deployment's to say. These bound what siting could be worth.
 *
 * What it was worth on the region under the oracle (parent_oracle, 3 seeds): at 0 dBm, nodes
 * 0-199 left the oracle without a route at the source for 29.8% of unicasts and delivered 57.5% on
 * time; a cds of 200 left 0.8%, what the network's own gaps leave, and delivered 80.5%, and the
 * 108 it needs uncapped 75.9%. By degree, 200 made 77.1%; spaced, 50.0%, worse than the list,
 * relays far apart being what low power cannot join. At 10 dBm a cds of 100 made 92.1% against
 * 84.0% for nodes 0-99; at 20 dBm every rule joins up and makes 96-99%. Siting did not make hops
 * fail less: about half of data hops were decoded whatever the rule, a little fewer with more
 * messages routed. That is the MAC's to fix. */
enum tsim_distvec_pick {
    TSIM_DISTVEC_PICK_LIST,
    TSIM_DISTVEC_PICK_DEGREE,
    TSIM_DISTVEC_PICK_SPACED,
    TSIM_DISTVEC_PICK_CDS,
};

struct tsim_distvec_oracle;

struct tsim_distvec_config {
    uint16_t channel;
    struct tsim_lora lora;
    double tx_dbm;
    char relays[96];    /* infrastructure, as tsim_meshcore_config.relays: "all" or "0-45,50" */
    uint8_t leaves;     /* enum tsim_distvec_leaves */
    uint8_t routes;     /* enum tsim_distvec_routes */
    uint8_t req_hops;   /* with demand routes: relays a route request crosses, at most */
    uint8_t req_cancel; /* with demand routes: copies heard that cancel a request, 0 never */
    uint32_t *parents;  /* with parent_oracle: [node], shared by every node; set by the driver */

    /* Picked infrastructure, above; with a pick, `relays` is ignored. */
    uint8_t relay_pick;       /* enum tsim_distvec_pick */
    uint32_t relay_count;     /* how many: at least 1, or with cds 0 for as many as it needs */
    const uint8_t *relay_set; /* [node] 1 for infrastructure, shared; set by the driver */

    tsim_time imin;    /* Trickle */
    uint8_t doublings; /* imax is imin times 2^doublings, 0..16 */
    uint8_t redundancy;
    uint8_t quiet_max;
    tsim_time neighbour_timeout;

    double cap;           /* the share of time routing may take, above 0 and at most 1 */
    double request_share; /* of the cap, for seqno requests: above 0 and below 1 */
    tsim_time cap_window; /* how much of its share each bucket holds */
    uint8_t burst;        /* frames an announce event may send, 1..16 */
    uint8_t ihu_max;    /* IHU entries per announce frame, 0..48, less what leaves no route room */
    uint8_t ihu_rounds; /* of a neighbour's IHU rounds without naming this node it may go, 2..64 */

    uint32_t ref_len;  /* bytes of the reference frame the metric is reckoned in */
    double etx_max;    /* at least 1; links over it are not used */
    double hysteresis; /* 0 to 1 */
    double change;     /* 0 to 1 */
    tsim_time request_interval;
    tsim_time seq_period; /* a node raises its own seq this often, give or take 10%; 0 never */
    tsim_time route_ttl;  /* with demand routes: how long a route lasts unheard of */

    uint8_t hop_max;
    uint8_t hop_retries;
    uint8_t retries;
    tsim_time hop_wait;
    tsim_time ack_wait;
    double ack_factor;
    double jitter; /* airtimes a frame sent in answer to one received waits, at most */

    uint8_t bcast_hops;
    double bcast_window;
    uint8_t bcast_cancel; /* copies heard, its own first one included, 0 for never */

    bool etx;          /* links cost their ETX times the reference frame; off, every one the same */
    bool power;        /* power control, as above; off, all go at tx_dbm */
    double tx_min_dbm; /* the quietest a frame goes, a whole dBm at least below tx_dbm */
    double margin_db;  /* above the quietest a neighbour decodes at, 0 to 60 */
    double step_db;    /* added for each try a hop has lost, 0 to 60 */
    double snr_floor_db; /* the lowest SNR the modulation demodulates at */
    uint8_t power_k;     /* neighbours frames for all of them reach; 0 for tx_dbm */
    /* Per-link SF (MSH-49, above): the fastest SF a node may listen on, from 7 up to the radio's,
     * or 0 for every node on the radio's; and how many of its links it must keep to go faster -
     * all it would have on the radio's, up to sf_k. */
    uint8_t sf_min;
    uint8_t sf_k;

    /* The oracle, below: off for the protocol itself. */
    bool oracle;
    uint8_t links;           /* enum tsim_distvec_links */
    uint8_t dead_hops;       /* with links by strength: hops lost running that forget, 1..255 */
    double oracle_margin_db; /* a link's loss leaves at least this above the floor, both ways */

    /* With links by strength: a link comes up with this much margin each way, 0 to 60 dB, and goes
     * down below it less link_band_db, 0 to 60. */
    double link_margin_db;
    double link_band_db;
    tsim_time silent_max; /* with links by strength: unheard this long, forgotten */
    /* With links by strength, the liveness probe (MSH-61, below). */
    uint8_t probe_hops;   /* hops lost running, nothing heard between, that start one; 0 never */
    uint8_t probe_tries;  /* probes unanswered that forget the neighbour, 1..32 */
    tsim_time probe_wait; /* a probe's wait for its answer, and the most the next waits more */
    const struct tsim_distvec_oracle
        *oracle_routes; /* with either: set by the driver, never parsed */
};

/* The oracle: a yardstick for the protocol, never a candidate. With `oracle`, a node announces
 * nothing, asks for nothing and keeps no neighbours: its routes and its powers are handed to it
 * from the simulator's own links, which no protocol can know, and everything else - hops, their
 * retries and implicit acknowledgements, end-to-end acknowledgements, broadcasts, the MAC - is the
 * protocol's own. What it delivers is what this data path could with perfect routes that cost no
 * airtime: the most better routing could gain.
 *
 * A link is one each end decodes the other on with oracle_margin_db to spare, at tx_dbm, by the
 * mean loss. A route is a path of the fewest links through relays only, each hop to the neighbour
 * of those one hop nearer with the least loss. A hop goes at the power its next hop needs with
 * margin_db, and frames for every neighbour at what the power_k nearest need, as power control
 * would set them knowing every floor exactly; without power control, at tx_dbm.
 *
 * The link oracle (MSH-57), with `links` oracle, is the same yardstick for links alone: the
 * protocol runs as ever - announces, IHUs, Trickle, the cap, feasibility, requests - but a
 * neighbour, once an announce of its has been heard, is judged by the oracle's links, not by its
 * announces counted: it costs the reference frame if the link is one, as above, and nothing goes
 * over it if not; its floor is what the mean loss says; and it is never forgotten, nor marked down
 * for a hop lost. Announces keep their IHUs, so routing takes the airtime it would. What it
 * delivers, against the protocol's own, is what better link sensing could gain.
 *
 * Measured on the region (200 relays, parent_oracle, 3 seeds), unicast on time was 51% at 0 dBm
 * and 67% at 20 dBm, against 23% and 8% sensed and 58% and 97% under the oracle; relays' routes
 * to each other reached 70% and 100% of pairs, against 22% and 8% sensed. With links that never
 * flip, Trickle settles: announces took 12% of airtime, against 36% and 56% sensed. Link sensing,
 * not route propagation, is most of candidate 3's gap.
 *
 * Links by strength (MSH-58), with `links` strength: a link is judged by how strongly each end
 * hears the other, not how often. A neighbour's floor is reckoned, as power control reckons it,
 * from every announce of its heard and every frame of this node's it is heard passing on; its IHU
 * carries, in place of a receive rate, the margin it hears this node with: how far below tx_dbm
 * its floor lies, in whole dB rounded down. The link comes up once both margins are link_margin_db
 * or more, and goes down once either falls more than link_band_db below that. Every link up costs
 * the same, as with ETX off. A hop lost is a frame lost, not a link: it raises the power, and
 * nothing more, until dead_hops of them in a row, with nothing heard from the neighbour between,
 * say it is gone, and it is forgotten. Silence is not the signal: a neighbour is forgotten unheard
 * only after silent_max. The IHU still expires as with sensing.
 *
 * Measured on the region (200 relays, parent_oracle, 3 seeds), unicast on time was 50% at 0 dBm
 * and 52% at 20 dBm, against 23% and 8% sensed and the link oracle's 51% and 67%; relays' routes
 * to each other reached 70% and 93% of pairs. No link went down on strength. Forgetting on
 * silence after neighbour_timeout, an hour, made 40% and 12%: neighbours loud enough to be heard
 * each way, by the powers both ends announced at, went an hour unheard thousands of times an hour
 * at 20 dBm, three in four of them links between leaves no route uses. Forgetting on lost hops
 * costs more the sooner it comes, every one wrong in a network where nothing dies: dead_hops 3
 * made 21% and 25%, 6 made 36% and 37%, 12 made 44% and 47%. Sensing never forgetting made 30%
 * and 13%. Whether 24 lost hops - a dozen messages from each neighbour sending through a dead
 * node - is soon enough was measured under churn (MSH-59): a quarter of the 200 relays (cds)
 * going down for 30 minutes after every 2 hours up, about 10 down at a time. Unicast on time fell
 * from 66% to 50% at 0 dBm and from 70% to 45% at 20 dBm, and fewer dead_hops only made it worse
 * - 48% and 44% at 12, 46% and 36% at 6, 35% and 22% at 3 - the false alarms outweighing the dead
 * caught sooner. Either way data went on being sent to a relay down for 16 to 20 minutes, most of
 * its time down: routes still led into it, relays' routes to each other reaching 79% and 63% of
 * pairs against 95% and 100%. Forgetting on silence sooner fails worse, and without churn too:
 * silent_max 60 min made 48% and 11% with every node up, 15 min 22% and 7%, live neighbours going
 * that long unheard (MSH-60). A dead neighbour wants a signal that a live one never gives.
 *
 * Why live ones go unheard (MSH-60, report.announces; cds 200, 3 seeds): their announces are sent
 * and lost. Of the announces between relays the oracle links that reach the far end above its
 * floor, 36% were decoded at 0 dBm - 31% lost to interference, 31% to a receiver busy with
 * another frame - and 2.6% at 20 dBm, 43% and 50%. Of the silences of 15 minutes or more, 85% at
 * 0 dBm were of announces lost, 15% of none sent; at 20 dBm 72% were of announces too quiet to
 * reach, power control not meaning them to. Louder announces did not help: power_k 16, 32 and
 * power off decoded 38%, 38% and 39% at 0 dBm for 67% of unicasts on time either way, and at 20
 * dBm made 52%, 29% and 13%, the extra power costing more in collisions than it found. Strength
 * never used a link the oracle lacks. It used 95% of the oracle's relay links at 0 dBm, 2.7
 * points short of the link oracle there (66.3% against 69.0%), but 8% at 20 dBm, where routes
 * over the nearest links alone, more hops each, fell 10.7 short (70.3% against 81.0%), mostly in
 * retries: what announcing to power_k neighbours leaves unknown, and what learning more costs
 * more than it gains.
 *
 * The liveness probe (MSH-61), with links by strength and probe_hops above 0. A dead neighbour
 * wants a signal a live one never gives, and a live one always gives an answer when asked. After
 * probe_hops hops to a neighbour are lost in a row, with nothing heard from it between, the node
 * asks it directly, at tx_dbm whatever power control would set:
 *
 *     type 0x08 | target 4 | prober 4 | power 1
 *
 * and the target answers at once, after the jitter, at tx_dbm too, charged as control:
 *
 *     type 0x09 | prober 4 | target 4 | power 1
 *
 * Anything heard from the neighbour - the answer, an announce, a frame passed on, a probe or an
 * answer of its own to some other node - ends the probing, and every node that decodes a probe or
 * an answer hears its sender. Without one, the next probe goes probe_wait after the last went, and
 * a random time up to probe_wait more; once probe_tries have gone unanswered, the link goes out of
 * use, its routes kept, until something is heard from the neighbour again. Its routes stay in use
 * while it is asked. dead_hops and silent_max still forget as before.
 *
 * Off by default: measured on the region (cds 200, parent_oracle, 3 seeds) it lost more than it
 * found, with churn - a quarter of the relays down 30 minutes after every 2 hours up - or without.
 * A probe fares no better than any other frame there: 27-43% of them reached the target, so a
 * live neighbour easily left a dozen unanswered. With churn, unicast on time was 50.0% at 0 dBm
 * and 44.6% at 20 dBm without probing; probing after 2, 4 or 8 lost hops, 6 or 12 tries 5 or 10 s
 * apart, made 42.9-48.7% and 39.3-44.6%, though it cut the hops sent to a relay down by up to 45%.
 * Without churn, where every verdict is wrong, 66.3% and 70.3% fell to 53.6-60.4% and
 * 61.5-69.9%. Even probes all but never acted on - 32 tries 30 s apart - cost 2-6 points, their
 * airtime alone: 64.2% and 64.1% without churn, 44.7% and 41.1% with it. Forgetting the
 * neighbour on the verdict, rather than taking its link out of use, did worse still: 39-48% and
 * 32-44% with churn. Finding a dead relay sooner is not what is missing: routes around it come
 * over the same lossy announces, however soon it is found. */
struct tsim_distvec_oracle_route {
    uint32_t next; /* TSIM_BROADCAST for no route */
    uint8_t hops;  /* at most 255 */
    float dbm;
};

struct tsim_distvec_oracle {
    uint32_t nodes;
    struct tsim_distvec_oracle_route *route; /* [src * nodes + dst] */
    float *node_dbm;                         /* per node */
    float *need; /* [a * nodes + b]: what a frame from a needs to be decoded at b, in dBm */
    double top;  /* the most a link's `need` may be, both ways */
};

struct tsim_phy;

/* Builds the oracle's routes and links for `config` from the links `phy` has now. Returns false,
 * leaving `oracle` empty, when memory runs out or the modulation is invalid. Takes time in
 * proportion to the node count times the links. */
bool tsim_distvec_oracle_build(struct tsim_distvec_oracle *oracle, const struct tsim_phy *phy,
                               const struct tsim_distvec_config *config);

void tsim_distvec_oracle_free(struct tsim_distvec_oracle *oracle);

/* Whether `node` is infrastructure under `config`: with a pick by relay_set, else by relays. */
bool tsim_distvec_relay(const struct tsim_distvec_config *config, uint32_t node);

struct tsim_pos;

/* Picks infrastructure by `config`'s relay_pick and relay_count from the links `phy` has now, and
 * for spaced the nodes' positions, setting set[node] to 1 for each relay and 0 for the rest.
 * Returns false when memory runs out, the modulation is invalid or the pick is list. */
bool tsim_distvec_pick_relays(const struct tsim_phy *phy, const struct tsim_pos *pos,
                              const struct tsim_distvec_config *config, uint8_t *set);

struct tsim_relay_tier;

/* How well `config`'s infrastructure joins up over the oracle's links, from those `phy` has now;
 * `present` only when some nodes are leaves. Returns false, leaving `out` zero, when memory runs
 * out or the modulation is invalid. */
bool tsim_distvec_tier(const struct tsim_phy *phy, const struct tsim_distvec_config *config,
                       struct tsim_relay_tier *out);

/* Every node infrastructure, leaves routed; Trickle from 8 s to 8 min (six doublings), redundancy
 * 3, announcing at least every third interval and forgetting a neighbour after an hour; a 0.5% cap,
 * a quarter of it for requests, with 1-minute buckets; up to 4 frames an event and 8 IHUs a frame,
 * a link kept through 8 rounds without one; a 32-byte reference frame, every link costing the same
 * (ETX off) and none used over ETX 32, 10% hysteresis and a 25% change threshold, a request every
 * 10 s while starved; a jitter of up to 2 airtimes; 32 hops, 2 hop retries after 4 s, 3 retries
 * waiting 5 s plus 4 times the metric; broadcasts over 4 hops, waiting up to 3 airtimes and dropped
 * on the second copy heard. Power control on, with power_k 8 - without it, the region's unicast
 * fell from 22% to 2% as density rose (MSH-45) - frames going no quieter than -9 dBm, the SX1262's
 * least, with a 10 dB margin and 3 dB more for each try lost, and the SNR floor Semtech's for the
 * SF: -7.5 dB at SF7, 2.5 dB lower for each SF above. Per-link SF off, with sf_k 8 for when it is
 * on. The oracle off and links sensed, with a 3 dB margin for either oracle.
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

/* Where the node would send a frame for `dst` next, as its frames go: its route to `dst`, or with
 * parent_oracle, its route to the leaf's parent. False if nowhere. */
bool tsim_distvec_next(const void *self, uint32_t dst, uint32_t *next);

/* What took a usable link out of use (MSH-54): its IHU for this node expired; an announce heard
 * left its receive rate too low for etx_max; housekeeping found it silent so long its rate fell
 * too low; a frame sent to it was lost; it was forgotten, unheard for neighbour_timeout; or it
 * left the liveness probe unanswered (MSH-61). */
enum tsim_distvec_down {
    TSIM_DISTVEC_DOWN_IHU,
    TSIM_DISTVEC_DOWN_RATE,
    TSIM_DISTVEC_DOWN_SILENT,
    TSIM_DISTVEC_DOWN_HOP,
    TSIM_DISTVEC_DOWN_TIMEOUT,
    TSIM_DISTVEC_DOWN_PROBE,
    TSIM_DISTVEC_DOWN_COUNT,
};

/* A node's books on its links and routes since it started, for reports. */
struct tsim_distvec_stats {
    uint64_t down[TSIM_DISTVEC_DOWN_COUNT];
    /* Of those, links its own measure called strong: the neighbour's floor, from its announces,
     * oracle_margin_db or more below tx_dbm. Unknown without power control, so never strong. */
    uint64_t down_strong[TSIM_DISTVEC_DOWN_COUNT];
    /* Routes lost to a destination the node announces and has had a route to before - an outage -
     * and, over time, how many such destinations it was without a route to and how many changed
     * routes waited on its urgent list, in destination-seconds. */
    uint64_t outages;
    double unrouted_s;
    double urgent_s;
    /* Repair: seqno requests and route requests the node made (not those passed on), starved
     * destinations it stopped asking about after its last try with no route come, and the times
     * a request raised its own seq. */
    uint64_t seqno_requests;
    uint64_t route_requests;
    uint64_t gave_up;
    uint64_t seq_raised;
    uint64_t route_replies; /* with demand routes: route requests it answered */
    /* The liveness probe: probes that went on the air, neighbours that answered one, and answers
     * it queued. */
    uint64_t probes;
    uint64_t probes_answered;
    uint64_t probe_acks;
    /* Now, not summed: of the destinations it announces, has had a route to and has none to, how
     * many it holds a route to through a usable neighbour that is infeasible, and how many none. */
    uint32_t unrouted_infeasible;
    uint32_t unrouted_empty;
};

/* The node's books, up to now. */
void tsim_distvec_stats(const void *self, struct tsim_distvec_stats *stats);

/* Whether the node can use its link to `nb`: heard both ways, within etx_max. */
bool tsim_distvec_uses(const void *self, uint32_t nb);

/* How many neighbours the node can use: heard both ways, within etx_max. */
uint32_t tsim_distvec_neighbours(const void *self);

/* The node's own route's sequence number. */
uint16_t tsim_distvec_seq(const void *self);

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

/* The SF the node listens on, and the SF a frame from it to neighbour `nb` would go at. */
uint8_t tsim_distvec_listen_sf(const void *self);
uint8_t tsim_distvec_sf_to(const void *self, uint32_t nb);

#endif
