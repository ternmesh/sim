#ifndef TSIM_BASELINE_H
#define TSIM_BASELINE_H

#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/net.h"
#include "tsim/time.h"

/* The simplest MAC and routing that work: the floor every candidate has to beat, and what the
 * network's own tests run on. Neither is any real firmware's. */

/* ALOHA with a random start. When there is a frame to send and the radio is free, it waits a time
 * drawn uniformly from [0, max_delay] and sends, without listening first. */
struct tsim_aloha_config {
    tsim_time max_delay;
};

extern const struct tsim_mac tsim_aloha;

/* Naive flooding. Every node rebroadcasts every message it has not seen before, as long as hops
 * are left, except the message's destination, which keeps it. The header is the message id, the
 * origin, the destination (4 bytes each) and the hops left (1), so a message costs 13 bytes more
 * than its payload. A node remembers every id it has seen, so a full cache never sets off a
 * second flood. */
struct tsim_flood_config {
    uint16_t channel;
    struct tsim_lora lora;
    double tx_dbm;
    uint8_t hops; /* rebroadcasts a message may have along any path */
};

#define TSIM_FLOOD_HEADER 13

extern const struct tsim_routing tsim_flood;

#endif
