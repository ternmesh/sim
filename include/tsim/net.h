#ifndef TSIM_NET_H
#define TSIM_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/node.h"
#include "tsim/phy.h"
#include "tsim/sched.h"
#include "tsim/time.h"

/* The network, as the driver of a run sees it: building the nodes, setting the links, making
 * messages and reading the books. Plugins never get this; they get their own node (tsim/node.h),
 * which is what keeps the radio model and the topology out of their reach. */

struct tsim_net_params {
    struct tsim_phy_params phy;
    uint16_t channel; /* what every radio listens on at the start */
    struct tsim_lora listen;
    uint32_t queue_limit; /* frames waiting per node; 0 is no limit */
    uint64_t seed;        /* the root of every stream tsim_node_rng() hands out, and of the fading:
                           * it replaces phy.fading_seed */
};

/* tsim_phy_defaults(), every radio on channel 0 at SF7/125 kHz, and 16 frames of queue. */
struct tsim_net_params tsim_net_defaults(uint64_t seed);

struct tsim_net;

/* A network of `nodes` nodes, each with its own instance of `routing` and `mac`. Every link
 * starts with infinite loss; set them through tsim_net_phy(), then call tsim_net_start(). The
 * configs must outlive it: a node powered up again is made from them. */
struct tsim_net *tsim_net_create(struct tsim_sched *sched, const struct tsim_net_params *params,
                                 uint32_t nodes, const struct tsim_routing *routing,
                                 const void *routing_config, const struct tsim_mac *mac,
                                 const void *mac_config);
/* Runs every routing's start(), once: after the links are set, so a protocol that announces
 * itself at start is heard, and before the scheduler runs. A second call does nothing. */
void tsim_net_start(struct tsim_net *net);

/* Destroys every plugin instance, then the radios; the scheduler can go on running without it. */
void tsim_net_destroy(struct tsim_net *net);

/* The radio model, for setting losses and reading radio statistics. Not for plugins. */
struct tsim_phy *tsim_net_phy(struct tsim_net *net);
struct tsim_sched *tsim_net_sched(struct tsim_net *net);
uint32_t tsim_net_nodes(const struct tsim_net *net);

/* A node, as its plugins see it - for a test that stands in for a plugin. */
struct tsim_node *tsim_net_node(struct tsim_net *net, uint32_t node);

/* A node's routing instance, as its create() returned it - for a test or a report that reads a
 * candidate's own state through that candidate's header. Never for a plugin. */
void *tsim_net_routing(struct tsim_net *net, uint32_t node);

/* The routing every node runs, as tsim_net_create() was given it. */
const struct tsim_routing *tsim_net_routing_plugin(const struct tsim_net *net);

/* Powers a node down or up (MSH-59), as a power cycle would: down, its radio hears and sends
 * nothing, and its routing and MAC are destroyed with their timers and queue - it holds no
 * message, and those it originated are finished, its routing being gone - while a frame already
 * on the air runs to its end; up, they are made again from the configs tsim_net_create() was
 * given, and started if the network has been. Returns false for a node out of range, or when
 * powering up runs out of memory, leaving it down. A node down has no routing to ask:
 * tsim_net_routing() gives NULL for it. */
bool tsim_net_power(struct tsim_net *net, uint32_t node, bool on);
bool tsim_net_on(const struct tsim_net *net, uint32_t node);

/* Makes a message at `src` and hands it to its routing. Returns its id, or 0 if a node is out of
 * range or powered down, `dst` is `src`, or `len` is over TSIM_FRAME_MAX. A message the routing
 * refuses keeps its record, marked refused: it stays in every delivery ratio's denominator, so a
 * protocol that cannot carry a workload's messages is charged for them rather than excused. */
uint64_t tsim_net_originate(struct tsim_net *net, uint32_t src, uint32_t dst, uint32_t len);

/* The frame a node has on the air now, or NULL. */
const struct tsim_tx *tsim_net_on_air(const struct tsim_net *net, uint32_t node);

/* Airtime a node has spent, by purpose. */
struct tsim_ledger {
    uint64_t frames[TSIM_PURPOSE_COUNT];
    tsim_time airtime[TSIM_PURPOSE_COUNT];
};

/* The ledger charges a frame its whole airtime when it goes on the air, so that a plugin cannot
 * leave a frame out of the books by any means. */
const struct tsim_ledger *tsim_net_ledger(const struct tsim_net *net, uint32_t node);

/* The ledger as of now: a frame still on the air counts as one frame, and only the part of its
 * airtime that has gone by. What a report cut off mid-frame should read, so no airtime after the
 * cutoff is counted, and no duty cycle runs over 1. */
void tsim_net_ledger_now(const struct tsim_net *net, uint32_t node, struct tsim_ledger *out);

/* Every purpose together. */
tsim_time tsim_ledger_airtime(const struct tsim_ledger *ledger);

struct tsim_net_stats {
    uint64_t queued;
    uint64_t dropped; /* refused because the queue was full */
    uint64_t cancelled;
    uint64_t refused;   /* messages originated here that its routing could not carry */
    uint64_t delivered; /* messages delivered to this node's application */
};

const struct tsim_net_stats *tsim_net_stats(const struct tsim_net *net, uint32_t node);

/* A drop of a message, as tsim_node_drop() booked it. */
struct tsim_message_drop {
    enum tsim_drop cause; /* TSIM_DROP_COUNT for none */
    uint32_t node;
    uint32_t next;  /* the next hop it was meant for, or TSIM_BROADCAST */
    bool next_held; /* the next hop held the message already */
    tsim_time at;
};

/* A message and how far it got. */
struct tsim_message_record {
    struct tsim_message msg;
    uint32_t wanted;    /* destinations: 1, or every other node for a broadcast */
    uint32_t delivered; /* destinations that have it */
    tsim_time first;    /* when the first and the latest of them got it */
    tsim_time last;
    bool refused;  /* the source's routing could not carry it */
    bool finished; /* the source's routing is done with it; see tsim_node_finished() */
    /* For the books: when a frame carrying it first went on the air from its source, or -1; the
     * drops booked for it, by cause; and the first of them - passing over any whose next hop
     * already held the message, which lost only the confirmation, unless every one did. */
    tsim_time sent;
    uint32_t drops[TSIM_DROP_COUNT];
    struct tsim_message_drop drop;
};

/* The record for a message id, or NULL. Valid until the next message is originated. */
const struct tsim_message_record *tsim_net_message(const struct tsim_net *net, uint64_t id);

/* How many messages have been originated; ids run from 1 to this. */
uint64_t tsim_net_message_count(const struct tsim_net *net);

/* Called each time a message reaches one of its destinations, after the record counts it, with
 * the record and the node it reached. For the driver's bookkeeping (tsim/metrics.h): it must not
 * originate, send or destroy anything. One observer at a time; NULL removes it. */
typedef void (*tsim_net_delivered_fn)(void *ctx, const struct tsim_message_record *record,
                                      uint32_t node);
void tsim_net_observe(struct tsim_net *net, tsim_net_delivered_fn fn, void *ctx);

/* Called once for each message when its source's routing is finished with it, after the record
 * says so: a refused message, and every message of a routing that does not report_finished, as
 * tsim_net_originate() returns. For the application (tsim/traffic.h): it may schedule its next
 * message, but must not originate, send or destroy anything from inside, since it can be called
 * from inside tsim_net_originate() or a plugin. One observer at a time; NULL removes it. */
typedef void (*tsim_net_finished_fn)(void *ctx, const struct tsim_message_record *record);
void tsim_net_observe_finished(struct tsim_net *net, tsim_net_finished_fn fn, void *ctx);

/* An addressed frame (tsim_tx.addressed) as it ends, and what became of it at the node it was
 * meant for. `rival` is the purpose of the frame that cost it, as struct tsim_phy_hooks has it, or
 * -1. */
struct tsim_net_hop {
    uint32_t from;
    uint32_t to;
    enum tsim_purpose purpose;
    uint64_t carries;
    enum tsim_phy_fate fate;
    int rival;
    tsim_time start; /* when the frame began */
};

/* Every frame at every node watched, addressed or not (MSH-60): tsim_phy_watch() through the
 * network, with the purpose its sender gave it. Slow; for diagnosis. NULL `fn` stops it. */
struct tsim_net_heard {
    uint32_t from;
    uint32_t to;
    enum tsim_purpose purpose;
    enum tsim_phy_fate fate;
    tsim_time start; /* when the frame began */
};
typedef void (*tsim_net_heard_fn)(void *ctx, const struct tsim_net_heard *heard);
void tsim_net_observe_heard(struct tsim_net *net, tsim_net_heard_fn fn, void *ctx,
                            const bool *nodes);

/* Called for each addressed frame as it ends, before its sender's routing hears it is done. For
 * the driver's bookkeeping, under the same rules as tsim_net_observe(). One observer at a time;
 * NULL removes it. */
typedef void (*tsim_net_hop_fn)(void *ctx, const struct tsim_net_hop *hop);
void tsim_net_observe_hops(struct tsim_net *net, tsim_net_hop_fn fn, void *ctx);

#endif
