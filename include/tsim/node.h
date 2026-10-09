#ifndef TSIM_NODE_H
#define TSIM_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/rng.h"
#include "tsim/time.h"

/* One node, as its plugins see it: everything a routing plugin or a MAC may do, and nothing else.
 *
 * A node is three layers, and a candidate brings the top two:
 *
 *  - routing decides *what* is sent and to whom. It is told when its node originates a message
 *    and when a frame arrives, and it answers by queueing frames and by delivering messages;
 *  - the MAC decides *when*. It is kicked whenever there may be something to send, looks at the
 *    head of the node's queue, and puts it on the air when it judges the channel ready;
 *  - the radio is tsim/phy.h, shared by every candidate.
 *
 * A candidate's routing and MAC are written together and may share state of their own; this
 * header is only what every candidate must go through. Three rules in it keep the comparison
 * honest:
 *
 *  - a plugin learns only what was on the air. A received frame is its bytes, its power, its SNR
 *    and its modulation - not the index of the node that sent it, which a protocol has to carry
 *    in its own header, at its own cost in airtime. A plugin is handed its own node and nothing
 *    that leads to the radio model, the link losses or any other node; it reaches its radio only
 *    through the calls below;
 *  - every frame goes on the air through the queue and is charged to the airtime ledger when it
 *    does, under the purpose its sender declared. A plugin can misfile airtime but not hide it:
 *    the totals count every frame;
 *  - a message is delivered at most once to each of its destinations, only to them, and only by a
 *    node that holds it. A node holds a message once it has originated it or received a frame
 *    that carried it, and a frame carries a message only if the message's content is in its
 *    bytes - the network checks - so a plugin cannot deliver what never reached it, and cannot
 *    claim to forward a message in a frame too short to hold it.
 *
 * The driver's side - building the network, setting losses, reading the books - is tsim/net.h. */

#define TSIM_BROADCAST UINT32_MAX
#define TSIM_FRAME_MAX 255

/* What a frame is for, as its sender declares it. */
enum tsim_purpose {
    TSIM_PURPOSE_DATA,     /* a node's own message, including its own retries */
    TSIM_PURPOSE_RELAY,    /* forwarding another node's message */
    TSIM_PURPOSE_CONTROL,  /* route discovery and repair, acknowledgements */
    TSIM_PURPOSE_ANNOUNCE, /* periodic beacons and adverts */
    TSIM_PURPOSE_COUNT,
};

/* A frame a routing plugin wants sent. */
struct tsim_tx {
    uint16_t channel;
    struct tsim_lora lora; /* per frame, so a plugin can pick a modulation per link */
    double tx_dbm;
    enum tsim_purpose purpose;
    uint8_t priority; /* higher goes first; equal priorities go in the order queued */
    /* Not on the air: whatever a candidate's routing tells its own MAC about this frame - the
     * SNR a relayed frame was heard at, say. The network never reads it, and a MAC written for
     * no routing in particular ignores it. */
    uint32_t hint;
    /* Not on the air either: if `addressed`, the node this frame is meant for - its next hop. Only
     * the report reads it, to book what became of the frame there (tsim/metrics.h). */
    bool addressed;
    uint32_t to;
    /* The message this frame carries, whose content is at bytes[carries_at], or 0 for none. The
     * sender must hold it. Receivers that decode the frame then hold it too. */
    uint64_t carries;
    uint32_t carries_at;
    uint32_t len;
    uint8_t bytes[TSIM_FRAME_MAX];
};

/* A frame as a receiver has it. Valid only for the call it is passed to. */
struct tsim_rx {
    const uint8_t *bytes;
    uint32_t len;
    uint16_t channel;
    struct tsim_lora lora;
    double rssi_dbm;
    double snr_db;
};

/* A message the application at `src` hands its routing: `len` bytes of content for `dst`, or for
 * every other node when `dst` is TSIM_BROADCAST. The id is from 1 in the order messages were made;
 * a plugin carries it on the air in whatever field it would carry a packet id in. The content is
 * random bytes the network made, and a frame that carries the message carries them. */
struct tsim_message {
    uint64_t id;
    uint32_t src;
    uint32_t dst;
    uint32_t len;
    tsim_time created;
    const uint8_t *content; /* valid for the call it is passed to */
    /* A presence card (tsim/traffic.h): a broadcast that says who its sender is, not a message to
     * anyone. A routing carries it as any other broadcast; only the books keep it apart. */
    bool card;
};

struct tsim_node;

/* A routing plugin. Each node gets its own instance from create(); start, if given, runs once
 * every node has one and the driver has laid out the links, so whatever it sends can be heard. */
struct tsim_routing {
    const char *name;
    void *(*create)(struct tsim_node *node, const void *config);
    void (*destroy)(void *self);
    void (*start)(void *self);
    /* Returns false if the protocol cannot carry the message at all - too long for what it
     * fragments, say. The message still counts as originated, and as refused. */
    bool (*originate)(void *self, const struct tsim_message *msg);
    void (*rx)(void *self, const struct tsim_rx *rx);
    void (*tx_done)(void *self, uint64_t handle); /* optional: a queued frame has been sent */
    /* Optional: the MAC is about to put this queued frame on the air. Returning false withdraws it
     * instead, as tsim_node_cancel() would have - for a protocol that decides whether a frame is
     * still wanted only once it is due. It must not send, cancel or transmit from inside. */
    bool (*sending)(void *self, uint64_t handle);
    /* Whether it calls tsim_node_finished() for every message it originates and does not refuse.
     * If not, a message is finished as soon as originate() returns. */
    bool reports_finished;
    /* Optional, for reports: the neighbour it would send a message for `dst` to now, if it holds a
     * route to it. For a protocol that keeps routes before it has traffic to carry - what shows
     * whether a warmup was long enough for its tables to fill. */
    bool (*next_hop)(const void *self, uint32_t dst, uint32_t *next);
};

/* A MAC. kick() is called whenever the queue or the radio may have changed: a frame was queued or
 * cancelled, the node's frame finished, or it received one. It may be called when there is
 * nothing to do, and it may transmit or cancel from inside the call. */
struct tsim_mac {
    const char *name;
    void *(*create)(struct tsim_node *node, const void *config);
    void (*destroy)(void *self);
    void (*kick)(void *self);
};

/* --- Who and when --- */

/* The node's index, which a protocol may use as its address. */
uint32_t tsim_node_index(const struct tsim_node *node);

/* How many nodes the network has, numbered from 0. What a protocol whose nodes know every other's
 * key - from adverts, or a contact list - can count on; never where they are or who hears whom. */
uint32_t tsim_node_count(const struct tsim_node *node);

/* The current simulated time. */
tsim_time tsim_node_now(const struct tsim_node *node);

/* A timer belonging to one node. The run's scheduler is not a plugin's to touch - it would show
 * every other node's activity and could run their events - so a plugin gets timers instead. A
 * timer lives until tsim_timer_destroy() or until the network is destroyed, which stops and frees
 * any a plugin left behind. */
struct tsim_timer;

/* A stopped timer that calls fn(ctx) when it fires. Returns NULL when memory runs out. */
struct tsim_timer *tsim_timer_create(struct tsim_node *node, void (*fn)(void *ctx), void *ctx);
void tsim_timer_destroy(struct tsim_timer *timer);

/* Sets the timer to fire `delay` from now, replacing any time it was set to. Returns false, and
 * leaves it stopped, for a negative delay or when memory runs out. */
bool tsim_timer_start(struct tsim_timer *timer, tsim_time delay);

/* Stops the timer if it is set. */
void tsim_timer_stop(struct tsim_timer *timer);

bool tsim_timer_pending(const struct tsim_timer *timer);

/* Which random stream a draw comes from, so that one layer's draws never shift another's. There is
 * no stream for traffic: that is the driver's (tsim/traffic.h), drawn from streams no plugin can
 * seed, so a protocol cannot know when its node will next be handed a message, or for whom. */
enum tsim_stream {
    TSIM_STREAM_MAC = 1,
    TSIM_STREAM_ROUTING,
};

/* Seeds `rng` with this node's stream for one purpose, from the run's seed. */
void tsim_node_rng(const struct tsim_node *node, enum tsim_stream stream, struct tsim_rng *rng);

/* --- What routing calls --- */

/* Queues a frame. Returns a handle, or 0 if the frame is invalid (no airtime for that modulation
 * and length, an unknown purpose, or a message it claims to carry that the node does not hold or
 * whose content is not where it says) or the queue is full, which counts as a drop. */
uint64_t tsim_node_send(struct tsim_node *node, const struct tsim_tx *tx);

/* Takes a frame back out of the queue. Returns false once it is on the air, or if it never was
 * queued. */
bool tsim_node_cancel(struct tsim_node *node, uint64_t handle);

/* Hands a message to this node's application. Returns true if this delivered it: the node holds
 * the message, is one of its destinations, and had not had it yet. */
bool tsim_node_deliver(struct tsim_node *node, uint64_t msg);

/* Why a node let go of a message it held without passing it on. */
enum tsim_drop {
    TSIM_DROP_NO_ROUTE,  /* it had nowhere to send it */
    TSIM_DROP_RETRIES,   /* the next hop never confirmed it, however often it was sent */
    TSIM_DROP_HOP_LIMIT, /* it had come as many hops as it was allowed */
    TSIM_DROP_QUEUE,     /* the queue was full: booked by the network, see below */
    TSIM_DROP_OTHER,
    TSIM_DROP_DUTY, /* sending it would have gone over the protocol's own duty-cycle limit */
    TSIM_DROP_COUNT,
};

/* Tells the books that this node let go of a message it holds without passing it on - a copy of
 * it, or one of its source's attempts - and why, with the next hop it was meant for, or
 * TSIM_BROADCAST for none. Only the report reads it (tsim/metrics.h); nothing in the network
 * changes. A frame carrying a message that a full queue refuses is booked as TSIM_DROP_QUEUE
 * without it. Returns false, booking nothing, for a message the node does not hold or a cause out
 * of range. */
bool tsim_node_drop(struct tsim_node *node, uint64_t msg, enum tsim_drop cause, uint32_t next);

/* Tells the application that this node's routing is done with a message the node originated: it
 * was acknowledged, or the routing gave up on it. Only for a routing that reports_finished; the
 * application may be waiting on it before making the node's next message (tsim/traffic.h).
 * Returns false, doing nothing, for a message this node did not originate or that is already
 * finished. */
bool tsim_node_finished(struct tsim_node *node, uint64_t msg);

/* --- What the MAC calls --- */

/* The frame at the head of the queue, or NULL. Valid until the queue next changes. */
const struct tsim_tx *tsim_node_head(const struct tsim_node *node);
size_t tsim_node_queue_length(const struct tsim_node *node);

/* The handle tsim_node_send() returned for the frame at the head of the queue, or 0 for none: how
 * a MAC tells the frame it drew a wait for from one that has taken its place. */
uint64_t tsim_node_head_handle(const struct tsim_node *node);

/* Whether the node's last frame is still on the air. */
bool tsim_node_sending(const struct tsim_node *node);

/* Puts the head of the queue on the air now, charging it to the ledger. Returns false, leaving it
 * queued, if the queue is empty, the node is still sending, or the radio refuses it - or, taking
 * it out of the queue, if the routing withdraws it (tsim_routing.sending), in which case the MAC
 * is kicked for the frame behind it. */
bool tsim_node_transmit(struct tsim_node *node);

/* --- The node's own radio --- */

/* Channel activity detection on what the radio is tuned to; see tsim_phy_cad(). */
bool tsim_node_cad(const struct tsim_node *node);

/* Whether the radio is part-way through receiving a frame. */
bool tsim_node_receiving(const struct tsim_node *node);

/* How long it has been on that frame, or -1 if it is on none. */
tsim_time tsim_node_received_for(const struct tsim_node *node);

/* When that frame ends, or -1 if it is on none. */
tsim_time tsim_node_receiving_until(const struct tsim_node *node);

/* Whether the radio's carrier flags are up, and holding its header flag; see tsim_phy_carrier()
 * and tsim_phy_hold_header(). */
bool tsim_node_carrier(struct tsim_node *node);
void tsim_node_hold_header(struct tsim_node *node, tsim_time hold);

/* Time the radio has spent transmitting, and receiving, up to now: what a radio's own channel
 * utilisation counter sees. A frame part-way out or in counts for the part gone by; see
 * tsim_phy_rx_airtime() for which receptions count. */
tsim_time tsim_node_tx_airtime(const struct tsim_node *node);
tsim_time tsim_node_rx_airtime(const struct tsim_node *node);

/* Retunes the receiver; see tsim_phy_tune(). */
bool tsim_node_tune(struct tsim_node *node, uint16_t channel, const struct tsim_lora *listen);

#endif
