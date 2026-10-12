#ifndef TSIM_TRAFFIC_H
#define TSIM_TRAFFIC_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/net.h"
#include "tsim/time.h"

/* The application above every node: who sends what to whom, and when.
 *
 * Each node sends on its own Poisson process - the gap between two of its messages is exponential
 * with mean `interval`, rounded to the nanosecond and never under one - so nodes neither march in
 * step nor drift into it. Each message is a broadcast with probability `broadcast`, and otherwise
 * goes to another node chosen uniformly; its length is uniform over [len_min, len_max].
 *
 * The draws are the driver's, from streams of their own that no plugin can seed: a protocol is
 * told about a message when its node originates it, and not before. Nothing a protocol does
 * moves a draw - a message the routing refuses still uses its draws - so with the same seed every
 * candidate is offered the same messages at the same times. The gaps go through libm's log, so as
 * with tsim_rng_normal() their last bit can differ between C libraries.
 *
 * Closed, a node's next gap starts not when its last message was made but when its routing is
 * finished with it (tsim_node_finished()), so a node waiting on an acknowledgement makes nothing
 * new - as Meshtasticator's nodes behave. The draws are the same, but when they fall then depends
 * on the protocol, so candidates are no longer offered the same messages at the same times.
 *
 * Two settings give unicasts the locality real messaging has, which is what lets a route found
 * once be used again. With `peers`, each node picks that many other nodes at the start, and the
 * picks are made mutual: a node's unicasts go to one of its peers, chosen uniformly, and nobody
 * else. With `reply`, each unicast is answered with that probability, from its destination to its
 * source, an exponential delay of mean `reply_delay` after it was made. Whether, when and how long
 * are drawn with the message, from its sender's stream, and the answer is sent whether or not the
 * message arrived: a protocol that delivers more is not offered more. An answer is not answered,
 * and like any message it is sent only before stop. Neither touches the scripted sends, which take
 * no draws and so go to whom they name, unanswered: script the answer too. With both at 0 the
 * draws are exactly those of the plain process.
 *
 * Apart from the messages, a share of the nodes can each send a presence card every so often: a
 * broadcast of `card_len` bytes that says who the node is, for anyone browsing who is about. Which
 * nodes send them, and when, are drawn from streams of their own, so cards move none of the
 * messages' draws: the same seed offers the same messages with cards or without. A card node's
 * first card falls uniformly in the first `card_interval` after the start, and each next one
 * between half and one and a half intervals after the last, so that cards neither bunch nor
 * march in step. Cards are made in [start, stop) like messages, and never in the closed loop's
 * wait.
 *
 * Beside the plain process, a workload can run flows (MSH-34): named kinds of message, each on
 * processes of its own - a position every 15 minutes, a chat line now and then, a direct message
 * to a regular correspondent - so that one run carries the mix a real mesh does and reports on each
 * kind apart. A flow's messages all go one way: to every node, to one of the node's peers (which
 * needs `peers`), or to another node chosen uniformly. A share of the nodes send it, drawn once
 * each; each of those makes one on its own process, exponential as the plain one's or, for a flow
 * on a timer, between half and one and a half intervals apart as cards are, the first uniformly
 * in the first interval. A share of a flow's unicasts are answered as `reply` answers the plain
 * process's, the answer belonging to the flow. A broadcast of a flow carries the hops and reach
 * its flow sets (struct tsim_message): a routing that can scope a broadcast honours them, and the
 * books count one with a reach as wanted only by the nodes within it. Each flow draws from streams
 * of its own, per node, so adding a flow moves no draw of the plain process, of the cards or of
 * another flow. */

enum tsim_flow_to {
    TSIM_FLOW_ALL,  /* a broadcast */
    TSIM_FLOW_PEER, /* one of the node's peers */
    TSIM_FLOW_ANY,  /* another node, chosen uniformly */
};

/* The most a flow's broadcast may ask for: relay hops, one fewer than a hop byte holds, since a
 * routing counts its sender's send as one; and metres, what two bytes of tens of metres hold. */
#define TSIM_FLOW_HOPS_MAX 254
#define TSIM_FLOW_REACH_MAX 655350.0

struct tsim_flow {
    char name[16];
    tsim_time interval; /* mean gap between one sending node's messages, above 0 */
    bool timer;         /* gaps uniform over half to one and a half intervals, not exponential */
    uint32_t len_min;
    uint32_t len_max;
    uint8_t to;     /* enum tsim_flow_to */
    double share;   /* fraction of the nodes that send it, 0..1 */
    double reply;   /* fraction of its unicasts answered, 0..1 */
    uint8_t hops;   /* a broadcast's relay hops, 0 for the routing's own */
    double reach_m; /* a broadcast's reach from its sender in metres, 0 for anywhere */
};

/* A message sent at a set time, as well as or instead of the ones the process makes: to put one
 * flood through a network and count what it costs, say. It uses none of the process's draws. */
struct tsim_send {
    tsim_time at; /* when; one in the past goes now */
    uint32_t src;
    uint32_t dst; /* TSIM_BROADCAST for every other node */
    uint32_t len;
};

struct tsim_traffic_params {
    tsim_time interval; /* mean gap between one node's messages; 0 for no process at all */
    uint32_t len_min;
    uint32_t len_max;
    double broadcast; /* fraction of messages that are broadcasts, 0..1 */
    tsim_time start;  /* messages are made in [start, stop) */
    tsim_time stop;
    uint64_t seed;
    bool closed;             /* each gap starts when the node's previous message is finished */
    uint32_t peers;          /* each node's regular correspondents, at least; 0 for anyone */
    double reply;            /* fraction of unicasts answered, 0..1; not with closed */
    tsim_time reply_delay;   /* mean time from a unicast to its answer */
    tsim_time card_interval; /* mean time between one card node's cards; 0 for no cards */
    uint32_t card_len;       /* bytes in a card */
    double card_share;       /* fraction of the nodes that send cards, 0..1 */
    /* A share of the nodes that write far more than the rest - a bot, a client in a loop - each
     * with a mean gap of `chatty_interval` instead of `interval`. Which they are is drawn from a
     * stream of their own, so with a share of 0 every draw is the plain process's. */
    double chatty_share;
    tsim_time chatty_interval;
    const struct tsim_send *sends; /* copied: they need not outlive the call */
    uint32_t send_count;
    struct tsim_flow flows[TSIM_FLOWS_MAX]; /* flow i + 1 is flows[i] */
    uint32_t flow_count;
};

struct tsim_traffic;

/* Starts traffic on every node of `net`, which must outlive it. Returns NULL for an invalid
 * parameter - a negative interval, len_min over len_max or over TSIM_FRAME_MAX, a broadcast
 * fraction outside 0..1, stop before start, more peers than other nodes, a
 * reply fraction outside 0..1 or with closed, a negative reply delay, a negative card interval, a
 * card share outside 0..1, a card over TSIM_FRAME_MAX, a flow whose interval is not above 0,
 * whose lengths are out of order or over TSIM_FRAME_MAX, whose share or reply is outside 0..1,
 * that goes to peers with none, that answers broadcasts, that scopes unicasts, whose hops are
 * over TSIM_FLOW_HOPS_MAX or reach negative or over TSIM_FLOW_REACH_MAX, or any flow with closed, a
 * send from or to a node that is not there, to its own node or over TSIM_FRAME_MAX - or when memory
 * runs out. A send is made whenever it falls, whatever start and stop say, and in the closed loop
 * its finishing starts no gap. */
struct tsim_traffic *tsim_traffic_create(struct tsim_net *net,
                                         const struct tsim_traffic_params *params);

/* Stops the traffic and frees it; the scheduler can go on running without it. */
void tsim_traffic_destroy(struct tsim_traffic *traffic);

/* Messages made so far, including any the routing refused, cards among them. */
uint64_t tsim_traffic_made(const struct tsim_traffic *traffic);

/* Cards made so far. */
uint64_t tsim_traffic_cards(const struct tsim_traffic *traffic);

/* Whether a node is one of the chatty share. */
bool tsim_traffic_chatty(const struct tsim_traffic *traffic, uint32_t node);

#endif
