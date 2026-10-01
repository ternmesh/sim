#ifndef TSIM_NET_H
#define TSIM_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/phy.h"
#include "tsim/rng.h"
#include "tsim/sched.h"
#include "tsim/time.h"

/* The network: every node's radio, MAC and routing, and the books kept on them.
 *
 * A node is three layers, and a candidate brings the top two:
 *
 *  - routing decides *what* is sent and to whom. It is told when its node originates a message
 *    and when a frame arrives, and it answers by queueing frames and by delivering messages;
 *  - the MAC decides *when*. It is kicked whenever there may be something to send, looks at the
 *    head of the node's queue, and puts it on the air when it judges the channel ready;
 *  - the radio is tsim/phy.h, shared by every candidate.
 *
 * A candidate's routing and MAC are written together and may share state of their own; the seam
 * below is only what every candidate must go through. Three rules in it keep the comparison
 * honest:
 *
 *  - a plugin learns only what was on the air. A received frame is its bytes, its power, its SNR
 *    and its modulation - not the index of the node that sent it, which a protocol has to carry
 *    in its own header, at its own cost in airtime;
 *  - every frame is charged to the airtime ledger when it goes on the air, under the purpose its
 *    sender declared. A plugin can misfile airtime but not hide it: the totals count every frame;
 *  - a message is delivered at most once to each of its destinations, and only to them. */

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

/* A message the application at `src` hands its routing: `len` bytes for `dst`, or for every
 * other node when `dst` is TSIM_BROADCAST. The id is from 1 in the order messages were made; a
 * plugin carries it on the air in whatever field it would carry a packet id in. */
struct tsim_message {
    uint64_t id;
    uint32_t src;
    uint32_t dst;
    uint32_t len;
    tsim_time created;
};

struct tsim_net;

/* A routing plugin. Each node gets its own instance from create(); start, if given, runs once
 * every node has one. A plugin that schedules events cancels them in destroy(). */
struct tsim_routing {
    const char *name;
    void *(*create)(struct tsim_net *net, uint32_t node, const void *config);
    void (*destroy)(void *self);
    void (*start)(void *self);
    void (*originate)(void *self, const struct tsim_message *msg);
    void (*rx)(void *self, const struct tsim_rx *rx);
    void (*tx_done)(void *self, uint64_t handle); /* optional: a queued frame has been sent */
};

/* A MAC. kick() is called whenever there may be something to send: a frame was queued, the
 * node's frame finished, or it received one. It may be called when there is nothing to do. */
struct tsim_mac {
    const char *name;
    void *(*create)(struct tsim_net *net, uint32_t node, const void *config);
    void (*destroy)(void *self);
    void (*kick)(void *self);
};

struct tsim_net_params {
    struct tsim_phy_params phy;
    uint16_t channel; /* what every radio listens on at the start */
    struct tsim_lora listen;
    uint32_t queue_limit; /* frames waiting per node; 0 is no limit */
    uint64_t seed;        /* the root of every stream tsim_net_rng() hands out */
};

/* tsim_phy_defaults(), every radio on channel 0 at SF7/125 kHz, and 16 frames of queue. */
struct tsim_net_params tsim_net_defaults(uint64_t seed);

/* A network of `nodes` nodes, each with its own instance of `routing` and `mac`. Every link
 * starts with infinite loss; set them through tsim_net_phy(). */
struct tsim_net *tsim_net_create(struct tsim_sched *sched, const struct tsim_net_params *params,
                                 uint32_t nodes, const struct tsim_routing *routing,
                                 const void *routing_config, const struct tsim_mac *mac,
                                 const void *mac_config);
/* Destroys every plugin instance, then the radios; the scheduler can go on running without it. */
void tsim_net_destroy(struct tsim_net *net);

struct tsim_phy *tsim_net_phy(struct tsim_net *net);
struct tsim_sched *tsim_net_sched(struct tsim_net *net);
uint32_t tsim_net_nodes(const struct tsim_net *net);

/* Which random stream a draw comes from, so that one layer's draws never shift another's. */
enum tsim_stream {
    TSIM_STREAM_MAC = 1,
    TSIM_STREAM_ROUTING,
    TSIM_STREAM_TRAFFIC,
};

/* Seeds `rng` with the stream for one node and one purpose. */
void tsim_net_rng(const struct tsim_net *net, uint32_t node, enum tsim_stream stream,
                  struct tsim_rng *rng);

/* --- What routing calls --- */

/* Queues a frame. Returns a handle, or 0 if the frame is invalid (no airtime for that modulation
 * and length, or an unknown purpose) or the queue is full, which counts as a drop. */
uint64_t tsim_net_send(struct tsim_net *net, uint32_t node, const struct tsim_tx *tx);

/* Takes a frame back out of the queue. Returns false once it is on the air, or if it never was
 * queued. */
bool tsim_net_cancel(struct tsim_net *net, uint32_t node, uint64_t handle);

/* Hands a message to the application at `node`. Returns true if this delivered it: `node` is one
 * of its destinations and had not had it yet. */
bool tsim_net_deliver(struct tsim_net *net, uint32_t node, uint64_t msg);

/* --- What the MAC calls --- */

/* The frame at the head of the queue, or NULL. Valid until the queue next changes. */
const struct tsim_tx *tsim_net_head(const struct tsim_net *net, uint32_t node);
size_t tsim_net_queue_length(const struct tsim_net *net, uint32_t node);

/* Whether the node's last frame is still on the air. */
bool tsim_net_sending(const struct tsim_net *net, uint32_t node);

/* Puts the head of the queue on the air now, charging it to the ledger. Returns false, leaving it
 * queued, if the queue is empty, the node is still sending, or the radio refuses it. */
bool tsim_net_transmit(struct tsim_net *net, uint32_t node);

/* --- Driving a run and reading it back --- */

/* Makes a message at `src` and hands it to its routing. Returns its id, or 0 if a node is out of
 * range, `dst` is `src`, or `len` is over TSIM_FRAME_MAX. */
uint64_t tsim_net_originate(struct tsim_net *net, uint32_t src, uint32_t dst, uint32_t len);

/* Airtime a node has spent, by purpose. */
struct tsim_ledger {
    uint64_t frames[TSIM_PURPOSE_COUNT];
    tsim_time airtime[TSIM_PURPOSE_COUNT];
};

const struct tsim_ledger *tsim_net_ledger(const struct tsim_net *net, uint32_t node);

/* Every purpose together. */
tsim_time tsim_ledger_airtime(const struct tsim_ledger *ledger);

struct tsim_net_stats {
    uint64_t queued;
    uint64_t dropped; /* refused because the queue was full */
    uint64_t cancelled;
    uint64_t delivered; /* messages delivered to this node's application */
};

const struct tsim_net_stats *tsim_net_stats(const struct tsim_net *net, uint32_t node);

/* A message and how far it got. */
struct tsim_message_record {
    struct tsim_message msg;
    uint32_t wanted;    /* destinations: 1, or every other node for a broadcast */
    uint32_t delivered; /* destinations that have it */
    tsim_time first;    /* when the first and the latest of them got it */
    tsim_time last;
};

/* The record for a message id, or NULL. Valid until the next message is originated. */
const struct tsim_message_record *tsim_net_message(const struct tsim_net *net, uint64_t id);

/* How many messages have been originated; ids run from 1 to this. */
uint64_t tsim_net_message_count(const struct tsim_net *net);

#endif
