#ifndef TSIM_CORE_H
#define TSIM_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "tsim/lora.h"
#include "tsim/node.h"

/* The firmware's protocol core (ternmesh/firmware, src/route.c), run as a routing plugin: the code
 * a device runs, not a model of it. Candidate 3 (tsim/distvec.h) is where the design was worked
 * out and measured; the specification's routing draft was written from its defaults, and the core
 * from the specification. This plugin is how the core is measured on the scenarios candidate 3 was,
 * so that what is said of the design can be said of the firmware.
 *
 * It is built only when the firmware's source is there to build from (TSIM_FIRMWARE_DIR in
 * CMakeLists.txt), and TSIM_HAVE_CORE says whether it was.
 *
 * What the core has, the plugin runs: announces, links, routes and requests, as the specification
 * has them, in its frames. A node's routing id is its index plus one. What the core does not have
 * yet, the plugin does not make up: there are no frames that follow the routes, so every message
 * is refused, and a scenario measures the routes the nodes hold (the report's `routes` and `reach`)
 * and the airtime they cost, never delivery. Nor does anything choose relays; `relay_pick` names
 * them as it does for candidate 3, and with none picked every node is one.
 *
 * The radio gives the core what an SX1262 would: a signal-to-noise ratio in quarters of a decibel,
 * from -32 to 31.75.
 *
 * Against candidate 3, measured with tools/core.py compare (3 seeds): the share of ordered pairs of
 * nodes that hold a route as the warmup ends, the share whose routes followed from node to node
 * arrive, and the seconds on the air in the hour after, all nodes together. Candidate 3 is run
 * without traffic, as the core is, and with and without its re-attachment, which neither the
 * specification nor the core has.
 *
 *                               routes    reach    airtime
 *   town (town-distvec.tsim, SF9, every node a relay)
 *     candidate 3               99.99%   99.97%     2662 s
 *     without re-attachment     99.99%   99.97%     2612 s
 *     core                      99.99%   99.98%     2316 s
 *   region (region-distvec.tsim, SF9, every node a relay)
 *     candidate 3              100.00%   99.97%    14448 s
 *     without re-attachment     99.99%   99.96%    14846 s
 *     core                     100.00%   99.96%    14825 s
 *   region, fast (region-distvec-fast.tsim, SF7, 200 relays)
 *     candidate 3               99.63%   99.57%      842 s
 *     without re-attachment     97.21%   97.21%      665 s
 *     core                      97.11%   97.11%      693 s
 *   region, deployed (region-distvec-deployed.tsim, SF8 at 62.5 kHz, 200 relays)
 *     candidate 3               92.68%   78.46%     5762 s
 *     without re-attachment     85.01%   85.01%     4280 s
 *     core                      92.09%   92.09%     4317 s
 *
 * So the core is candidate 3 without re-attachment, to a tenth of a point, on three of the four,
 * and what it lacks on the fast preset is re-attachment: with seed 1 there, 25 leaves and a relay
 * have heard relays and hold no link to one that is up, each relay's announces going only as loud
 * as its nearest eight neighbours need. On the deployed preset it holds 7 points more than
 * candidate 3 without re-attachment, which is not explained; candidate 3's neighbour_timeout is
 * not it. With seed 1, no route of the core's went round in any of the four: every route held
 * that did not arrive ended at a node with none.
 *
 * None of this is delivery. The figures candidate 3 is judged on come from the frames that follow
 * routes, which the core has still to get. */
struct tsim_core_config {
    uint16_t channel;
    struct tsim_lora lora;
    double tx_dbm;     /* a node's full power, rounded to a whole dBm */
    double tx_min_dbm; /* and its least */
    /* Which nodes are relays: every node with relay_pick list, else those the driver picks, as
     * for candidate 3 (enum tsim_distvec_pick, but for elect). */
    uint8_t relay_pick;
    uint32_t relay_count;
    const uint8_t *relay_set; /* [node] 1 for a relay, shared; set by the driver */
    /* The tables a node keeps. A board has what its memory allows, and a neighbour or a
     * destination past the end of its table is one it does not know. */
    uint32_t neighbours;   /* 1..255 */
    uint32_t destinations; /* 0 for one each for every node */
};

struct tsim_core_config tsim_core_default(uint16_t channel, const struct tsim_lora *lora,
                                          double tx_dbm);

/* Why a config would be refused, or NULL. */
const char *tsim_core_check(const struct tsim_core_config *config);

/* Whether `node` is a relay under `config`. */
bool tsim_core_relay(const struct tsim_core_config *config, uint32_t node);

extern const struct tsim_routing tsim_core;

#endif
