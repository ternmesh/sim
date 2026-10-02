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
 * on the protocol, so candidates are no longer offered the same messages at the same times. */

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
    bool closed; /* each gap starts when the node's previous message is finished */
    const struct tsim_send *sends; /* copied: they need not outlive the call */
    uint32_t send_count;
};

struct tsim_traffic;

/* Starts traffic on every node of `net`, which must outlive it. Returns NULL for an invalid
 * parameter - a negative interval, len_min over len_max or over TSIM_FRAME_MAX, a broadcast
 * fraction outside 0..1, stop before start, a send from or to a node that is not there, to its own
 * node or over TSIM_FRAME_MAX - or when memory runs out. A send is made whenever it falls, whatever
 * start and stop say, and in the closed loop its finishing starts no gap. */
struct tsim_traffic *tsim_traffic_create(struct tsim_net *net,
                                         const struct tsim_traffic_params *params);

/* Stops the traffic and frees it; the scheduler can go on running without it. */
void tsim_traffic_destroy(struct tsim_traffic *traffic);

/* Messages made so far, including any the routing refused. */
uint64_t tsim_traffic_made(const struct tsim_traffic *traffic);

#endif
