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
 * and nothing else. A broadcast goes as a group's frame would (tern/group.h), 31 bytes longer
 * than its content, flooded by the firmware's flooder (tern/flood.h): every node that hears it
 * for the first time is delivered it, as a member of the group would be, and every relay passes
 * it on or not as the flooder says, within the two allowances it keeps. Nothing chooses relays;
 * `relay_pick` names them as it does for candidate 3, and with none picked every node is one.
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
 * they bring.
 *
 * Broadcast, measured with tools/density.py --candidates 3,core --quick (3 seeds, a message from
 * every node every 30 minutes, a quarter of them broadcasts): unicasts on time and broadcast
 * destinations reached on time, at -5, 0, 5, 10 and 20 dBm.
 *
 *                     unicast                          broadcast
 *   fast (SF7, 200 relays)
 *     candidate 3     60.6 91.2 97.5 98.1 98.6         39.9 66.9 77.5 87.1 90.2
 *     core            60.8 92.0 98.9 98.0 96.1         39.1 65.8 78.0 83.2 84.8
 *   deployed (SF8 at 62.5 kHz, 200 relays)
 *     candidate 3     22.5 23.4 19.9 16.8 11.9         20.8 23.3 26.3 22.5  5.7
 *     core            21.1 23.2 20.0 17.7 21.1         20.2 27.5 32.1 31.7 17.9
 *   every node a relay (SF9)
 *     candidate 3     41.3 37.6 32.1 26.8 19.9         27.2 31.8 35.2 34.5 25.7
 *     core            38.2 40.5 36.8 38.7 34.6         27.3 33.6 38.4 42.5 45.3
 *
 * The core's scenarios run its own MAC, which listens first (mac = listen), and candidate 3's run
 * MeshCore's dispatcher. That is the 4 to 5 points of broadcast the core is short on the fast
 * preset at 10 and 20 dBm: under candidate 3's MAC (-s mac=meshcore) it reaches 88.4% and 89.5%
 * there, against candidate 3's 87.1% and 90.2%, and 88.4% for candidate 3 without re-attachment
 * at 20 dBm. Where the channel is full it is the core that reaches more.
 *
 * It is not the listening. MeshCore's dispatcher holds a frame a random time of its own before it
 * sends, and a relay held longer hears more copies and stays silent more often. The flooder's
 * own wait does the same under mac = listen, on less airtime; a wait drawn after a busy channel
 * (mac.window, in slots of 7 ms) does it too, and costs unicast its delay. Fast preset, broadcast
 * at 10 and 20 dBm and on-time deliveries per second of airtime (tools/density.py --fast --quick
 * --candidates core, 3 seeds):
 *
 *   flood_wait 8 (the default)     83.2% 84.8%    56.7 50.0
 *   flood_wait 12                  86.4% 86.8%    63.8 55.0
 *   flood_wait 16                  88.2% 88.7%    71.5 65.0
 *   flood_wait 24                  88.3% 90.3%    84.2 71.3
 *   mac.window 8                   83.5% 84.2%    57.7 49.2
 *   mac.window 32                  86.4% 87.8%    63.8 59.3
 *   mac.window 64                  86.7% 89.0%    63.4 61.0
 *   mac.window 128                 87.6% 91.4%    52.7 61.0
 *   mac = meshcore                 88.4% 89.5%    62.0 56.3
 *
 * A longer wait is not free where the channel is full. Unicast and broadcast on time, and the
 * median and 95th-percentile delay of a broadcast in seconds, at -5, 0, 5, 10 and 20 dBm:
 *
 *                     unicast                     broadcast                   delay at 0 and 20 dBm
 *   fast
 *     wait 8          60.8 92.0 98.9 98.0 96.1    39.1 65.8 78.0 83.2 84.8    2.4  5.8    0.9  2.8
 *     wait 16         60.7 90.5 98.2 97.9 96.1    39.2 66.8 78.7 88.2 88.7
 *     wait 24         60.7 90.6 98.6 98.0 96.1    39.1 66.4 79.3 88.3 90.3    5.5 13.7    1.0  3.8
 *   deployed
 *     wait 8          21.1 23.2 20.0 17.7 21.1    20.2 27.5 32.1 31.7 17.9   12.0 26.0   12.9 30.3
 *     wait 16         20.9 22.5 17.9 16.9 19.5    17.6 28.4 34.8 31.9 19.2   19.8 43.7   21.2 50.5
 *     wait 24         20.1 22.1 18.1 17.3 19.5    14.2 27.4 32.3 30.3 18.2   27.3 62.1   28.8 69.4
 *   every node a relay
 *     wait 8          38.2 40.5 36.8 38.7 34.6    27.3 33.6 38.4 42.5 45.3    5.6 11.5    5.8 12.0
 *     wait 16         35.4 34.3 30.1 29.3 23.8    28.7 37.5 44.7 49.0 49.7    8.4 18.4    8.8 19.3
 *     wait 24         34.0 32.4 28.5 27.1 20.2    28.6 38.9 47.0 52.3 51.5   11.4 25.6   11.5 26.0
 *
 * So on the fast preset a wait of 16 reaches 4 to 5 points more where nodes are many and costs
 * nothing but the delay. With every node a relay the floods that now get through take the
 * channel from unicast, 3 to 11 points of it, though deliveries per second of airtime rise 8 to
 * 27%. On the deployed preset a broadcast's delay nears the minute allowed, and where nodes are
 * few it arrives late.
 *
 * The flooder's settings, each alone, fast preset, broadcast at 10 and 20 dBm against the
 * defaults' 83.2% and 84.8%, with on-time deliveries per second of airtime against 56.7 and 50.0:
 * never cancelling (flood_copies = 0) 78.8% and 79.7%, at 33.6 and 31.4; cancelling on the third
 * copy 81.0% and 80.9%, at 40.1 and 35.1; a wait of 3 airtimes 75.0% and 78.0%; 8 hops 84.7% and
 * 85.1%, at 48.6 and 43.7; flood_sparse 16, 85.6% and 85.6%, at 52.3 and 45.1; 64 frames in hand,
 * no change. So hearing one more copy and staying silent reaches more, on less.
 *
 * The allowance. At that traffic it changes nothing: with both set to 1000000, every figure of the
 * fast preset is the same to the digit, and the other two are within a seed's spread. With a
 * message from every node every 5 minutes, three quarters of them broadcasts, it is the relays'
 * allowance that binds, and only where relays are few: on the fast preset at 0 dBm, unicast on
 * time 28.0% with it and 16.6% without, and broadcast 14.2% against 16.5%; at 10 and 20 dBm, and
 * on the deployed preset at any power, nothing either way. That load is more than the channel
 * holds with or without: unicast on time falls from 92.0%, 98.0% and 96.1% to 28.0%, 23.6% and
 * 39.2%. A thousand nodes each well inside an allowance of their own are together several
 * channels' worth, which no allowance kept node by node can see.
 *
 * Which goes first, and a relay that is busy. Two rules were tried against that, as settings of
 * this plugin, before the specification or the firmware had either: flood_last, which is still
 * here, and a busy share kept by the plugin, which cut a relay's floods off at flood_busy_ppm
 * or, with flood_busy_soft, passed fewer on the busier the radio was. The second of those is now
 * the specification's and the firmware's flooder's, flood_busy_ppm is its FLOOD_BUSY and on by
 * default, and the plugin's own is gone; the rows marked "the firmware's" are through that
 * code, and "as it is" is without the rule (flood_busy_ppm = 1000000). Unicast and broadcast on
 * time at 0, 10 and 20 dBm, and on-time deliveries per second of airtime (tools/density.py
 * --quick --candidates core, 3 seeds, flood_wait 8):
 *
 *                             unicast           broadcast         per second
 *   every node a relay (SF9)
 *     as it is                40.5 38.7 34.6    33.6 42.5 45.3    3.3 4.0 4.3
 *     flood_last              38.5 38.5 35.2    33.5 43.1 44.3    3.3 4.1 4.3
 *     flood_busy_ppm 100000   67.2 75.6 75.7     3.1  4.3  6.6    0.6 0.9 1.5
 *     flood_busy_ppm 400000   47.6 61.9 70.8    24.3 23.9 19.5    2.8 3.5 4.0
 *     flood_busy_ppm 600000   41.1 51.0 55.9    31.0 35.6 35.4    3.2 4.2 5.6
 *     200000, flood_busy_soft 42.6 48.3 47.9    30.0 40.7 43.7    3.2 4.8 6.3
 *     400000, flood_busy_soft 41.1 45.9 45.9    32.0 40.6 43.6    3.3 4.5 5.8
 *     the firmware's, 200000  41.9 48.6 47.0    30.2 40.3 43.5    3.2 4.8 6.3
 *   deployed
 *     as it is                23.2 17.7 21.1    27.5 31.7 17.9    4.5 5.4 3.4
 *     flood_last              24.4 18.8 20.5    27.4 30.9 18.9    4.5 5.3 3.6
 *     flood_busy_ppm 400000   37.1 34.8 29.3     6.3  2.5  1.7    1.3 0.6 0.5
 *     200000, flood_busy_soft 30.9 31.8 27.5    18.8 13.6  4.2    3.5 3.1 1.1
 *     400000, flood_busy_soft 27.7 28.7 28.9    22.5 17.3  4.8    4.0 3.8 1.2
 *     the firmware's, 200000  30.5 31.5 28.5    17.7 13.5  4.1    3.3 3.1 1.0
 *   fast
 *     as it is                92.0 98.0 96.1    65.8 83.2 84.8    33.4 56.7 50.0
 *     flood_last              91.8 97.9 96.1    64.7 82.9 84.6    30.7 52.6 47.4
 *     flood_busy_ppm 400000   91.0 98.1 96.5    64.7 82.7 84.3    30.3 56.5 60.3
 *     200000, flood_busy_soft 92.1 97.9 96.5    65.5 82.6 84.5    34.4 60.5 55.6
 *     the firmware's, 200000  92.2 98.1 96.3    66.2 82.5 84.4    35.1 58.2 56.7
 *
 * Sending a node's routed frames before its flooded ones (flood_last) does nothing: a relay
 * seldom holds both, and what they contend for is the air between nodes. A relay that passes no
 * flooded frame on while its radio is busy does: it is the floods that cost unicast its
 * deliveries, which reach 75% with them held back, against 35 to 40%. Cut off at a share of the
 * radio's time, the floods go altogether where the channel is full. Passing fewer on the busier
 * the radio is (flood_busy_soft) keeps most of them: with every node a relay, unicast gains 2 to
 * 13 points for 2 to 4 of broadcast, and deliveries per second of airtime rise by up to 47%; on
 * the fast preset, where the channel has room, nothing changes; on the deployed preset, which is
 * full at any power, unicast gains 6 to 14 points and broadcast loses 9 to 18. A flood_wait of
 * 16 with flood_busy_ppm set changes little: the share decides.
 *
 * A board's tables. Every figure above gave each node 255 neighbours, a destination for every
 * node and 16 frames of each kind. A board has 64, 128, 8 and 4 (ports/node/node.c), and by
 * default the plugin now has what the board has. The neighbours cost little; the destinations
 * are what a thousand nodes outgrow. A node holds a route only to a destination it has a place
 * for, and a relay can pass a frame on only toward one it holds, so the share of pairs whose
 * routes arrive falls with the table. Region, fast, seed 3, routes that arrive as the warmup
 * ends:
 *
 *   destinations    every node   512      256      128
 *   reach           96.5%        49.4%    24.6%    12.1%
 *
 * And with seed 1 and the traffic density.py offers (a quarter broadcast), unicast on time is
 * 96.3% with a place for every node and 12.1% with 128; peers limited to four (traffic.peers=4)
 * gives 12.8%, so the table does not keep the destinations a node sends to. Routing at the size of
 * a network larger than its table is not specified yet. For the design without the board's limits:
 * -s routing.neighbours=255 -s routing.destinations=0 -s routing.frames=16 -s
 * routing.flood_frames=16.
 *
 * An experiment (this branch, and the firmware's claude/leaf-default-route): relays with a place
 * for every node, leaves with few, and leaves handing a frame they hold no route for to their
 * nearest relay (default_hops 6). Region with the traffic density.py offers, 200 relays, seeds 1
 * and 2: unicast on time, broadcast on time, and the routes that arrive as the warmup ends.
 *
 *                                              fast                   deployed
 *   a board: 128 places everywhere             12.5%  85.2%  0.12     10.7%  54.0%  0.12
 *   unbounded                                  96.4%  84.5%  0.97     28.5%   3.6%  0.67
 *   1024 everywhere                            93.7%  81.9%  0.95     27.2%   3.6%  0.70
 *   relays 1024, leaves 128                    29.9%  83.4%  0.30     21.1%  36.4%  0.27
 *   relays 1024, leaves 128, default route     92.4%  81.5%  0.94     14.0%   0.6%  0.75
 *   relays 1024, leaves 32, default route      93.0%  81.2%  0.94     12.5%   0.5%  0.75
 *   relays 512, leaves 32, default route       49.3%  83.6%  0.50     -
 *
 * Where the channel has room, a relay's table is what decides: leaves need no more than 32
 * places with the default route, and relays need one for every node. Where it is full (deployed),
 * the default route costs a third more airtime, sending frames toward relays that hold no route
 * on, and both unicast and broadcast fall: it wants a guard before it is worth specifying.
 *
 * The cause is the retries: leaves send 4.2 times the data frames (deployed, seed 1) and as many
 * messages are given up, on a channel where the next hop is busy receiving. The guard: a leaf takes
 * its default route only while its radio's busy share, the flooder's, is under default_busy_ppm.
 * Relays 1024, leaves 32, seeds 1 and 2:
 *
 *                                 fast                  deployed
 *   no default route              -                     15.5%  40.0%
 *   default route, no guard       93.0%  81.2%          12.5%   0.5%
 *   guard at 10%                  35.8%  84.8%          15.6%  38.2%
 *   guard at 20%                  60.6%  82.1%          17.8%  33.9%
 *   guard at 30%                  80.0%  81.6%          20.1%  29.2%
 *   guard at 50%                  92.6%  80.0%          24.0%  14.5%
 *
 * Leaves on the fast preset are busy a fifth to a half of the time, so a guard much under a half
 * costs them their routes. At a half it costs them nothing, and on the deployed preset it gets 84%
 * of what unbounded tables deliver (28.5% unicast) with four times their broadcast (3.6%). Not
 * salvaging a frame given up on to a second relay changes nothing: the waste is in the retries
 * of messages that cannot arrive, not in the salvage. */
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
     * destination past the end of its table is one it does not know. By default these four are
     * the board's own, read from the firmware's ports/node/node.c when the core is built; the
     * figures above were measured with 255 neighbours, a destination for every node and 16
     * frames of each kind, which a board does not have (see the end of this comment). */
    uint32_t neighbours;   /* 1..255 */
    uint32_t destinations; /* 0 for one each for every node */
    /* A leaf's destinations, 0 for `destinations`, and the firmware's default route
     * (tern/route.h): a leaf with no route hands a frame to its nearest relay. default_hops and
     * default_busy_ppm are the firmware's own by default, DEFAULT_HOPS and DEFAULT_BUSY of the
     * specification's routing draft; default_hops 0 is no default route. */
    uint32_t leaf_destinations;
    uint32_t default_hops;
    uint32_t default_busy_ppm; /* the busy share past which a leaf uses no default route */
    /* Frames a node has in hand at once, its own and those it passes on: 1..255. One more is
     * dropped. */
    uint32_t frames;
    uint32_t salvage; /* other neighbours a frame given up on is tried at: 0..4 */
    /* Airtimes of itself a frame sent again waits, at most: 0..255, and the firmware's own by
     * default. With 0, two nodes whose frames met meet again at every try (scenarios/core/
     * together.tsim). */
    uint32_t retry_jitter;
    /* Broadcasts, flooded as the firmware floods a group's frame (tern/flood.h). Each of these is
     * the firmware's own by default. `flood_frames` is how many a node has in hand at once, its
     * own and those it passes on. The two allowances are millionths of a node's time, for its
     * own floods and for those it passes on, and 1000000 is no allowance at all. */
    uint32_t flood_frames;    /* 1..255 */
    uint32_t flood_hops;      /* what a flood starts with: 1..255 */
    uint32_t flood_sparse;    /* relay neighbours, at most, of a relay that spends no hop */
    uint32_t flood_wait;      /* airtimes a relay waits, at most: 0..255 */
    uint32_t flood_copies;    /* copies heard that drop a frame still waiting; 0 never */
    uint32_t flood_own_ppm;   /* 1..1000000 */
    uint32_t flood_relay_ppm; /* 1..1000000 */
    uint32_t flood_last;      /* 1: a flooded frame waits for every frame that follows a route */
    /* The specification's FLOOD_BUSY, in millionths: the share of the last half minute to minute
     * its radio spent sending or receiving, past which a relay passes fewer flooded frames on,
     * and none when it is never idle. 1000000 for never fewer. */
    uint32_t flood_busy_ppm;
    /* What a presence card's flood starts with (tsim/traffic.h), 0 for flood_hops. A card the
     * length of one goes with the firmware's card header, and its flooder holds it to CARD_HOPS
     * (2) whatever this says, so this can only lower it; one of another length is not a card the
     * firmware sends, and goes as a group's frame with these hops. */
    uint32_t card_hops; /* 0..255 */
};

struct tsim_core_config tsim_core_default(uint16_t channel, const struct tsim_lora *lora,
                                          double tx_dbm);

/* Why a config would be refused, or NULL. */
const char *tsim_core_check(const struct tsim_core_config *config);

/* Whether `node` is a relay under `config`. */
bool tsim_core_relay(const struct tsim_core_config *config, uint32_t node);

extern const struct tsim_routing tsim_core;

#endif
