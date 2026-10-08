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
 * What the core has, the plugin runs: announces, links, routes and requests, and the frames that
 * follow routes (tern/forward.h), as the specification has them, in its frames. A node's routing
 * id is its index plus one. A message goes as a secured unicast frame would, 23 bytes longer than
 * its content, with its number where the frame's tag is; its destination acknowledges every copy,
 * and its source sends it again until it is acknowledged or given up. A frame does not say where
 * it came from, and on a device its destination knows from the session: here it looks the
 * message's number up in a table the nodes of one network share, which stands for those sessions
 * and nothing else. What the core does not have, the plugin does not make up: there is no
 * broadcast, so a broadcast is refused, and scenarios that compare delivery are run with
 * traffic.broadcast = 0. Nor does anything choose relays; `relay_pick` names them as it does for
 * candidate 3, and with none picked every node is one.
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
 *     core                      97.09%   97.09%      687 s
 *   region, deployed (region-distvec-deployed.tsim, SF8 at 62.5 kHz, 200 relays)
 *     candidate 3               92.68%   78.46%     5762 s
 *     without re-attachment     85.01%   85.01%     4280 s
 *     core                      92.07%   92.07%     4319 s
 *
 * So the core is candidate 3 without re-attachment, to a tenth of a point, on three of the four,
 * and what it lacks on the fast preset is re-attachment: with seed 1 there, 25 leaves and a relay
 * have heard relays and hold no link to one that is up, each relay's announces going only as loud
 * as its nearest eight neighbours need. On the deployed preset it holds 7 points more than
 * candidate 3 without re-attachment, which is not explained; candidate 3's neighbour_timeout is
 * not it. With seed 1, no route of the core's went round in any of the four: every route held
 * that did not arrive ended at a node with none.
 *
 * A board has fewer places for neighbours than these runs gave it, 255, and on the last two of
 * these a node at full power is heard by 280 and 490 others. Routes held with `neighbours` 32 and
 * 64 (3 seeds), first with the core keeping the neighbours it heard first, as it did until
 * 2026-10-07, then keeping every link that is up and otherwise the nearest:
 *
 *                               32 places          64 places
 *   town                      100.00%  100.00%   100.00%  100.00%
 *   region                     98.53%   99.87%    99.98%   99.99%
 *   region, fast               64.39%   87.58%    92.62%   96.15%
 *   region, deployed           17.45%   81.72%    76.53%   90.88%
 *
 * With 255 places the two agree to 0.02 of a point. With 32 or 64 on the fast preset some seeds
 * do not settle: six hours on they still send requests, and 3 ks of announces an hour against
 * 0.6 ks, which is not explained.
 *
 * Delivery, with every node sending a message to another at random every half hour and none
 * broadcast (-s traffic.broadcast=0, 3 seeds): messages that arrived within the minute allowed,
 * and how many did for each second on the air, all nodes together. Candidate 3 is run as it is,
 * and plain, without re-attachment or rescue floods, which the core has neither of; the
 * incumbents as their own scenarios have them.
 *
 *                               town            region, fast     region, deployed
 *   Meshtastic                  21.0%  0.008    19.0%  0.013      1.1%  0.001
 *   MeshCore                    -               30.3%  0.026      5.7%  0.004
 *   candidate 3                100.0%  0.122    99.4%  0.538      9.2%  0.006
 *   candidate 3, plain          -               96.8%  0.390     19.1%  0.016
 *   core, 255 neighbours       100.0%  0.132    96.7%  0.438     14.4%  0.013
 *   core, 64 neighbours         99.9%  0.133    95.2%  0.320     11.6%  0.010
 *
 * So on the fast preset the core delivers what candidate 3 does plain, and what candidate 3 has
 * over both there is re-attachment and rescue. On the deployed preset, where the channel is full
 * and most of what is sent is lost, the core is 5 points short of candidate 3 plain, which is not
 * explained, and both are ahead of candidate 3 as it is, whose rescue floods cost more there than
 * they bring. */
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
    /* Frames a node has in hand at once, its own and those it passes on: 1..255. One more is
     * dropped. */
    uint32_t frames;
    uint32_t salvage; /* other neighbours a frame given up on is tried at: 0..4 */
    /* Airtimes of itself a frame sent again waits, at most: 0..255, and the firmware's own by
     * default. With 0, two nodes whose frames met meet again at every try (scenarios/core/
     * together.tsim). */
    uint32_t retry_jitter;
};

struct tsim_core_config tsim_core_default(uint16_t channel, const struct tsim_lora *lora,
                                          double tx_dbm);

/* Why a config would be refused, or NULL. */
const char *tsim_core_check(const struct tsim_core_config *config);

/* Whether `node` is a relay under `config`. */
bool tsim_core_relay(const struct tsim_core_config *config, uint32_t node);

extern const struct tsim_routing tsim_core;

#endif
