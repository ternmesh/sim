# Scenarios

A scenario is a text file that describes one run: where the nodes are, how the radio channel
behaves, which protocol runs, what traffic it carries and for how long. `tsim` runs one and prints
a report:

```bash
tsim scenarios/town.tsim
tsim -s seed=7 -s routing.hops=5 scenarios/town.tsim
```

Each `-s` is read as if it were a line added to the end of the file, so it overrides whatever the
file sets. To sweep a parameter, loop over `-s`.

## The format

Each line holds one `key = value` setting. A `#` starts a comment, and blank lines are ignored. If
a setting appears twice, the later value wins, except `traffic.send`, each of which adds a message.

Every time needs a unit: `ns`, `us`, `ms`, `s`, `min` or `h`, as in `500 ms` or `1.5 min`. A bare
number is an error, so a value can't be read in the wrong unit.

A `routing.*` or `mac.*` setting belongs to the plugin chosen by `routing =` or `mac =`. It can
appear before or after that choice. A plugin's radio settings start from the `radio.*` values and
can then be overridden.

When there's a problem, `tsim` reports the file name, the line and the reason, for example
`town.tsim:12: duration = 5: expected a time above 0`. A problem in a `-s` override is reported
against that override, and a missing required setting against the file as a whole.

## The run's clock

A run has three parts:

1. **Warmup:** a period for settling, so protocols that announce themselves can do so first. Its
   last `traffic.lead` carries traffic, so they settle on a loaded channel, not a quiet one.
2. **Duration:** the period in which traffic is generated.
3. **Deadline:** a final period with no new traffic, so the last messages have as long to arrive as
   the first.

Airtime and duty cycles are measured over the last two, and only the messages made in them are
counted: the warmup is for settling, and what it cost is reported apart. The town and region
scenarios give every candidate the same warmup, long enough for candidate 3's route tables to fill:
3 h on the town, 6 h on the region. Traffic runs for the last 2 h and 3 h of it. After a quiet
warmup, candidate 3 does far better in the first hour of traffic than it goes on to: as the load
raises link costs its routes become infeasible (MSH-50), and a result taken then would be
misleading.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `nodes` | required | number of nodes, 1 to 1048576 |
| `routing` | required | routing plugin: `flood`, `meshtastic`, `meshcore` or `distvec` |
| `mac` | required | MAC plugin: `aloha`, `meshtastic` or `meshcore` |
| `seed` | 1 | the seed for every random draw in the run: positions, shadowing, traffic, message content, plugins |
| `placement` | `uniform` | `uniform`, `grid`, `line`, or `file` to read them from `positions` |
| `positions` | none | for `file`: a file with one node per line, as `x y` in metres, in node order; `#` starts a comment. `tsim` reads a relative path from the scenario file's directory |
| `links` | none | a file with the loss of each link, which then replaces the channel model and the positions: one link per line, as `a b loss` in dB, or `a b there back` where the two directions differ; a link the file leaves out loses everything. `tsim` reads a relative path from the scenario file's directory |
| `area` | `5000 x 5000` | metres, for `uniform` |
| `spacing` | 1000 | metres between neighbours, for `grid` and `line` |
| `warmup` | `0 s` | time before the measured window: before traffic starts, unless `traffic.lead` starts it sooner |
| `duration` | `1 h` | time during which traffic is generated |
| `deadline` | `60 s` | how long a message has to arrive to count as on time, and how long the run continues after the traffic stops |
| `queue` | 16 | frames each node may have waiting; 0 means no limit |
| `radio.channel` | 0 | channel number |
| `radio.sf` | 7 | spreading factor, 7 to 12 |
| `radio.bw` | 125000 | bandwidth in Hz |
| `radio.cr` | 1 | coding rate 4/(4+cr), 1 to 4 |
| `radio.preamble` | 8 | preamble length in symbols |
| `radio.tx_dbm` | 14 | transmit power in dBm |
| `channel.model` | `log_distance` | the median loss: `log_distance` (the settings below), or `3gpp_suburban` or `3gpp_urban`, the macro-cell models Meshtasticator calls 5 and 6 |
| `channel.freq` | 915 | MHz, for the 3GPP models |
| `channel.height` | 1 | metres both antennas stand above the ground, for the 3GPP models |
| `channel.pl0` | 128.95 | median path loss at `d0`, in dB |
| `channel.d0` | 1000 | reference distance in metres |
| `channel.exponent` | 2.32 | path loss exponent |
| `channel.sigma` | 7.8 | shadowing standard deviation in dB; 0 turns shadowing off |
| `channel.share` | 0.5 | fraction of the shadowing variance that is spatially correlated, 0 to 1 |
| `channel.decorrelation` | 100 | spacing of the correlated shadowing field, in metres |
| `phy.noise_figure` | 6 | receiver noise figure in dB |
| `phy.capture` | 6 | dB a new frame must exceed the current one by to take over a receiver during the preamble |
| `phy.lock_symbols` | 5 | preamble symbols before a receiver locks |
| `phy.retune` | `1 ms` | time a radio is deaf after changing channel or modulation |
| `phy.fading` | 0 | dB standard deviation of a Gaussian added to a link's loss afresh for every frame at every receiver |
| `phy.pairwise` | `no` | `yes` judges interference LoRaSim's way: each interferer alone, at full weight, and only one the receiver could decode |
| `phy.capture_anytime` | `no` | `yes` lets a frame `phy.capture` dB louder take the receiver at any point of a reception |
| `phy.cad_margin` | 0 | dB below the demodulation floor that CAD still notices a frame at |
| `phy.cad_delay` | `0 s` | how long a frame has to have been on the air before CAD notices it |
| `traffic.interval` | `15 min` | mean time between one node's messages (exponentially distributed), or `none` for no messages but those `traffic.send` sets |
| `traffic.len` | 32 | message length in bytes, either `32` or a range such as `16..64` |
| `traffic.broadcast` | 1 | fraction of messages sent as broadcasts; the rest go to one other node chosen at random |
| `traffic.send` | none | one message at a set time, as `30 s, 2, all, 40`: when (not during the warmup), from which node, to which node or to `all`, and how many bytes. Each adds one, up to 64. It uses none of the traffic process's draws, and in the closed loop its finishing starts no gap |
| `traffic.peers` | 0 | each node's regular correspondents: it picks this many others at the start, the picks are made mutual, and its unicasts go only to its peers, chosen uniformly. 0 sends each unicast to anyone, which leaves a route found once almost never used again |
| `traffic.reply` | 0 | fraction of the traffic process's unicasts answered, from the destination back to the source; a `traffic.send` is not, so script its answer too. Whether, when and how long are drawn with the message, and the answer goes whether or not the message arrived, so every candidate is offered the same messages. Answers are not answered. Not with `traffic.closed` |
| `traffic.reply_delay` | `2 min` | mean time from a unicast to its answer, exponentially distributed |
| `traffic.lead` | `0 s` | how long before the warmup ends the traffic process starts, at most the warmup. Its messages load the network but are not counted, only those made after the warmup are; the airtime spent on them after it is, since airtime is not told apart by message |
| `traffic.closed` | `no` | `yes` starts a node's next gap only when its routing is done with its last message - acknowledged, or given up on - as Meshtasticator's nodes do. The messages then depend on the protocol, so candidates are no longer offered the same ones |

The channel defaults come from Petäjäjärvi et al. (ITST 2015). The phy defaults are placeholders
until the bench rig measures real radios; see `tsim/phy.h`. The last five phy settings, with
the 3GPP channel models, are there to reproduce simpler simulators' results, and are off by
default.

### Plugins

| Setting | Default | Meaning |
|---|---|---|
| `routing.hops` (`flood`) | 3 | how many times a message can be rebroadcast along any path |
| `mac.max_delay` (`aloha`) | `1 s` | longest random wait before sending |
| `routing.role` (`meshtastic`) | `client` | `client`, `client_mute` (never rebroadcasts) or `router` (waits less to rebroadcast, and cancels on the third copy heard, not the second) |
| `routing.hop_limit` (`meshtastic`) | 3 | rebroadcasts a packet may have, 0 to 7 |
| `routing.want_ack` (`meshtastic`) | `yes` | whether a message wants an acknowledgement (`yes` or `no`); broadcasts take an implicit one |
| `routing.ack_duplicates` (`meshtastic`) | `yes` | whether a destination acknowledges every copy of a message it hears, so a retry is answered when the first acknowledgement was lost; `no` acknowledges only the first, as Meshtasticator does |
| `routing.retries` (`meshtastic`) | 3 | sends after the first, for a message that is not acknowledged |
| `routing.noise` (`meshtastic`) | none | dBm a rebroadcast's SNR is reckoned from, as its RSSI less this (Meshtasticator uses -119.25), instead of the SNR the radio measured |
| `routing.ack_poll` (`meshtastic`) | `no` | `yes` starts the wait for an acknowledgement when the frame is queued, not when it has gone, and notices an acknowledgement only when the wait runs out, as Meshtasticator's sender does |
| `routing.cancel_late` (`meshtastic`) | `no` | `yes` leaves a rebroadcast or retry that is no longer wanted in the queue until the MAC has waited out its turn, and withdraws it only then, as Meshtasticator does; `no` cancels it on hearing enough copies, as the firmware does |
| `routing.processing` (`meshtastic`) | `4.5 s` | added to the wait for an acknowledgement, at most a quarter of the clock |
| `routing.slot`, `mac.slot` (`meshtastic`) | from the radio | the contention slot: 2.5 symbols plus 7.6 ms; above 0, and short enough that 2^(`cw_max` + 1) slots fit in a quarter of the clock |
| `routing.cw_min`, `mac.cw_min` (`meshtastic`) | 3 | smallest contention window, as a power of two |
| `routing.cw_max`, `mac.cw_max` (`meshtastic`) | 8 | largest, at most 15 |
| `mac.snr_min`, `mac.snr_max` (`meshtastic`) | -20, 10 | the SNR range, in dB, over which a rebroadcast's window grows from `cw_min` to `cw_max` |
| `mac.busy_chance` (`meshtastic`) | 0 | chance, each time the MAC looks, that the channel is busy with traffic from outside the mesh: Meshtasticator's interference level |
| `routing.relays` (`meshcore`) | `all` | which nodes are repeaters and relay, as `all` or numbers and ranges such as `0-45,50`; the rest are companions |
| `routing.hash_size` (`meshcore`) | 1 | bytes per node on a path, 1 to 3 |
| `routing.scoped` (`meshcore`) | `yes` | whether floods carry region codes, 4 bytes |
| `routing.flood_max` (`meshcore`) | 64 | a flood that has made this many hops is not relayed, 1 to 64 |
| `routing.rx_delay_base` (`meshcore`) | 0 | how long a received flood waits before it is looked at, the worse it was heard the longer; 0 for not at all, as MeshCore 1.17 ships, up to 20 |
| `routing.tx_delay_factor`, `routing.direct_tx_delay_factor` (`meshcore`) | 0.5, 0.3 | a relay waits 0 to 5 times this many of its own airtimes, flooded or direct; 0 to 2 |
| `routing.retries` (`meshcore`) | 3 | attempts after the first, for a direct message no one acknowledges |
| `routing.advert_interval` (`meshcore`) | `2 min` | how often a repeater announces itself to its neighbours, or `none` |
| `routing.cancel_heard` (`meshcore`) | `no` | not MeshCore's: MeshBench's idea of dropping a relay on hearing another node relay the packet first, while it waits out its receive delay (`waiting`) or until it is sent (`queued`) |
| `routing.estimate_cr` (`meshcore`) | `radio` | the coding rate the firmware reckons its delays and timeouts from; MeshBench's firmware reckons at 4/5 (`1`) whatever the air runs at |
| `mac.airtime_factor` (`meshcore`) | 1 | the duty cycle budget is 1/(1 + this) of an hour |
| `mac.latched_header` (`meshcore`) | `no` | not MeshCore's: how long the radio's header flag stays set once a header sets it, as on MeshBench, whose radio never clears it on reading a packet; MeshCore's driver then times it out after `3934 ms` |
| `routing.relays` (`distvec`) | `all` | which nodes are infrastructure, which forward and announce routes, as `all` or numbers and ranges such as `0-45,50`; the rest are leaves |
| `routing.leaves` (`distvec`) | `routed` | how a leaf is reached: `routed`, by the routes to it infrastructure announces; or `parent_oracle`, through its parent - infrastructure routing among itself only, each leaf taking its best infrastructure neighbour as parent, and the node routing to a leaf learning its parent from an oracle, as a lookup never wrong or late would tell it (MSH-53). With power control and `power_k`, frames for every neighbour then reach the `power_k` nearest infrastructure neighbours |
| `routing.imin`, `routing.doublings` (`distvec`) | `8 s`, 6 | Trickle's shortest announce interval, and how many times it doubles to the longest (0 to 16) |
| `routing.redundancy` (`distvec`) | 3 | consistent announces heard in an interval that suppress this node's; 0 never suppresses |
| `routing.quiet_max` (`distvec`) | 2 | intervals running a node may keep quiet before it announces anyway |
| `routing.neighbour_timeout` (`distvec`) | `1 h` | how long a neighbour may go unheard before it is forgotten |
| `routing.cap` (`distvec`) | 0.005 | the share of a node's time announces and seqno requests may take together, above 0 and at most 1; per node, so a neighbourhood of n nodes may spend n times it |
| `routing.request_share` (`distvec`) | 0.25 | the part of the cap kept for seqno requests, above 0 and below 1; the rest is the announces' |
| `routing.cap_window` (`distvec`) | `1 min` | how much of its share each of the two buckets holds, at least one full-length frame |
| `routing.burst` (`distvec`) | 4 | announce frames one announce event may send while changed routes are waiting, 1 to 16 |
| `routing.ihu_max` (`distvec`) | 8 | neighbours an announce frame reports hearing, 0 to 48; the rest take their turn in later frames |
| `routing.ihu_rounds` (`distvec`) | 8 | rounds of a neighbour's IHUs that may go by without naming this node before the link is taken to be one-way, 2 to 64; 2 cut thousands of good links an hour under the region's losses; 8 took the town's unicast from 82% to 98% (MSH-52) |
| `routing.ref_len` (`distvec`) | 32 | bytes of the frame whose airtime, times the link's ETX with `routing.etx`, is the link's cost |
| `routing.etx` (`distvec`) | `no` | `yes` costs each link its ETX, measured from the announces heard, times the reference frame's airtime; `no` costs every link heard both ways the same, so the metric counts hops. ETX measures the channel's load more than the link, and under traffic its routes starve (MSH-48) |
| `routing.etx_max` (`distvec`) | 32 | links with a higher ETX are not used, whether or not `routing.etx` costs them by it |
| `routing.hysteresis` (`distvec`) | 0.1 | how much better, as a share, another route has to be to replace the current one |
| `routing.change` (`distvec`) | 0.25 | how far, as a share, a route's metric must move before it is announced at once, and within which the last metric announced is announced again |
| `routing.request_interval` (`distvec`) | `10 s` | how often a node with no feasible route to a destination asks again, five times at most |
| `routing.hop_max` (`distvec`) | 32 | hops a message may take, 1 to 255 |
| `routing.hop_retries`, `routing.hop_wait` (`distvec`) | 2, `4 s` | how often a hop sends a frame again when it does not hear its next hop pass it on, and how long it waits, beyond twice the frame's airtime |
| `routing.retries` (`distvec`) | 3 | sends after the first, for a message the destination does not acknowledge |
| `routing.ack_wait`, `routing.ack_factor` (`distvec`) | `5 s`, 4 | the source waits this long, plus this many times the route's metric in milliseconds, for the acknowledgement |
| `routing.jitter` (`distvec`) | 2 | the longest a frame sent in answer to one received waits before it is queued, in its own airtimes |
| `routing.bcast_hops` (`distvec`) | 4 | relays a broadcast may have along any path, 0 to 254 |
| `routing.bcast_window` (`distvec`) | 3 | the longest a broadcast relay waits, in airtimes of the frame |
| `routing.bcast_cancel` (`distvec`) | 2 | copies of a broadcast heard, the first included, that drop a relay still waiting; 0 never drops one |
| `routing.power` (`distvec`) | `yes` | power control: frames to one neighbour go at its floor plus `routing.margin`, and a relay loud enough for the hop before too |
| `routing.power_k` (`distvec`) | 8 | with `routing.power`, announces, requests and broadcasts go loud enough for this many nearest neighbours; 0 sends them at `radio.tx_dbm` |
| `routing.tx_min`, `routing.margin`, `routing.step` (`distvec`) | -9, 10, 3 | power control's quietest frame in dBm, its margin above a neighbour's floor in dB, and the dB each lost try adds |
| `routing.snr_floor` (`distvec`) | Semtech's for the SF | the SNR the radio demodulates down to, in dB, from which a neighbour's floor is reckoned |
| `routing.seq_period` (`distvec`) | `0 s` | how often each node raises its own sequence number unasked, give or take 10%, so routes that feasibility starved come back without a request getting through (DSDV's periodic seqnos). `0 s` never. Measured on the region it did not help (MSH-54) |
| `routing.routes` (`distvec`) | proactive | `proactive` announces routes to every node; `demand` (MSH-43, work in progress) announces none, and finds routes with route requests and learns them from the traffic: see `tsim/distvec.h` |
| `routing.route_ttl` (`distvec`) | 10 min | with demand routes, how long a route lasts unheard of |
| `routing.req_hops` (`distvec`) | 16 | with demand routes, relays a route request crosses, at most |
| `routing.req_cancel` (`distvec`) | 2 | with demand routes, copies of a route request a relay hears before it drops its own; 0 never |
| `routing.oracle` (`distvec`) | `no` | `yes` makes candidate 3 a yardstick, not a candidate: no announces or requests, and routes and powers handed down from the simulator's own links - the fewest hops over links with `routing.oracle_margin` to spare - with the data path, broadcasts and MAC unchanged. What it delivers is the most better routing could gain |
| `routing.oracle_margin` (`distvec`) | 3 | dB above the floor, at `radio.tx_dbm` by the mean loss, that both ends of a link the oracle uses must have, 0 to 60 |

The routing and the MAC each keep their own copy of the window: the routing sizes its
acknowledgement wait by it. Settings that bound each other, such as `cw_min` and `cw_max`, are
checked once every setting is in, so they can be written in any order.

## The report

`tsim` prints one JSON object. For a given scenario and seed it is identical on every run on the
same platform, so two reports can be compared with `diff`.

The run's CPU time goes to stderr, because it differs between runs.

| Field | Meaning |
|---|---|
| `links` | how connected the map is at the scenario's `radio.*` modulation and power: `degree_mean`, `degree_min` and `degree_max` links per node, and `component_max`, the nodes in the largest set the links join. A link is two nodes that each decode the other with nothing else on the air (mean loss, no fading). Read every other figure against it: a protocol ranks differently where nodes hear a dozen others and where they hear hundreds |
| `warmup` | the warmup's length `s`, its airtime over all nodes `airtime_s`, and, for routing that keeps routes, how far they had got when it ended: `routes`, the share of ordered pairs of nodes where the source held a route to the destination, and `reach`, where following each node's route in turn got there. Both are -1 for routing that keeps none |
| `health` | `distvec` only, not under the oracle: how its links and routes held up over the window (MSH-54). `down_per_h`: usable links going out of use, all nodes, by cause - `ihu` (its IHU for the node expired), `rate` (an announce heard left its receive rate under `etx_max`), `silent` (housekeeping found it silent too long), `hop` (a frame sent to it was lost), `timeout` (forgotten after `neighbour_timeout`); `strong_per_h` the same for links the node's own floor measure put `oracle_margin` or more under `tx_dbm`. `outages_per_h`: routes relays lost to destinations they announce and had a route to; `outage_mean_s` how long one lasted; `unrouted_mean` how many such destinations a relay was without a route to, and `urgent_mean` how many changed routes waited on its urgent list, on average. `relay_reach_begin` and `relay_reach_end`: the share of ordered pairs of relays whose routes, followed as frames go, get there, as the window begins and as the run ends. `seqno_requests_per_h`, `route_requests_per_h`: requests nodes made, not those passed on; `gave_up_per_h`: starved destinations given up on after the last try; `seq_raised_per_h`: requests that raised a seq; `route_replies_per_h`: with demand routes, route requests answered. `unrouted_infeasible`, `unrouted_empty`: as the run ends, per relay, destinations without a route that it holds only infeasible routes to, and none |
| `losses` | where unicast was lost (MSH-55). Every unicast message of the window has one fate, and they sum to `unicast.messages`: `on_time`, `late`, `refused`; `dropped`, by the cause of its first drop - `no_route`, `retries` (the next hop never confirmed it), `hop_limit`, `queue` (a full queue refused a frame carrying it), `other` - and where: `source`, `last_hop` (meant for the destination itself) or `partway`; `unheard`, when every drop was a retry whose next hop already had the message, so only the confirmation was lost; then, with no drop booked, `vanished` if its source is done with it and `pending` if not. Drops are what the routing books, so only `distvec` (and its oracle) fill `dropped`; for others a lost message is `vanished` or `pending`. `drops`: every drop booked, by cause, delivered messages too. `late_wait_s`, `late_transit_s`: for late deliveries, the mean time from origination to the source's first frame carrying it, and from there to the destination. The rest is the network's, for any routing that names its frames' next hop: `hops`, frames meant for one node by what became of them there - `decoded`, `weak` (below the floor), `interfered` (received to the end and lost on interference), `taken` (by a louder frame), `busy` (receiving another), `deaf` (transmitting or retuning) - for `data` (carrying a unicast message) and `control` (carrying none); `rivals`, of the data hops, by the purpose of the frame that cost them; `progress`, where the data hops pointed, against the links at `radio.*` - to a node with fewer hops to the destination (`closer`), as many (`level`), more (`farther`), or `not_a_link` |
| `on_time_per_airtime_s` | **the headline:** deliveries that arrived within the deadline, per second of airtime spent by all nodes for any purpose |
| `duty_max`, `duty_max_node` | the busiest node's airtime after the warmup as a fraction of that time, and which node that is (the lowest-numbered, if several tie) |
| `duty_mean` | the average across all nodes |
| `unicast`, `broadcast` | for each kind of message: `messages` originated, how many the routing `refused`, destinations `wanted`, `delivered` and delivered `on_time`, and latency at the 50th and 95th percentiles and the maximum |
| `airtime_s`, `frames` | totals, broken down by declared purpose: data, relay, control, announce |
| `queue_dropped` | frames refused because a node's queue was full |
| `rx_ok`, `rx_lost`, `rx_preempted`, `rx_aborted` | reception outcomes summed over every radio; see `tsim/phy.h` |
| `rx_missed` | frames a radio could have decoded that began while it was receiving another, and that it never caught, summed over every radio; with the four above, each frame counts once at each radio |

Unicast and broadcast are reported separately. A broadcast has a destination in every other node,
so in a combined total broadcasts would swamp the unicasts, and unicast is what path routing is
for.

Every destination counts toward `wanted`, whether its message was refused, dropped, lost or late.

The latency percentiles are rounded down by at most 1.6%. The maximum latency is exact.

## The scenarios here

- **`scenarios/*.tsim`:** CTest runs each of these through `tsim`, so a scenario that stops parsing
  or running fails CI.
- **`scenarios/meshtasticator/`:** candidate 1 set up as Meshtasticator runs, on 20 maps
  Meshtasticator placed (10 to 50 nodes, five seeds each), with Meshtasticator's results on them in
  `reference.json`. CTest runs `tools/meshtasticator.py check`, which runs `compat.tsim` on every
  map and fails if a size's average is too far from Meshtasticator's; see the script for how the
  reference was made and where the two simulators are known to differ.
- **`scenarios/meshbench/`:** candidate 2 on MeshBench's Fife network: 56 nodes, with every link's
  loss as MeshBench budgets it over the terrain in `fife.links`, by node number only, and what one
  flood cost in MeshBench in `reference.json`. CTest runs `tools/meshbench.py check`, which runs
  `fife.tsim` for each seed and arm and fails if a mean is too far from MeshBench's; the script says
  how the reference was made.
- **`scenarios/scale/*.tsim`:** these are too large for a sanitized debug build. CI's `scale` job
  runs `region.tsim` (1000 nodes, six hours' warmup with traffic for the last three, then one hour
  of traffic measured), and the same under each candidate (`region-meshtastic.tsim`,
  `region-meshcore.tsim`, `region-distvec.tsim`), as a release build with a 120-second budget each. At their 20 dBm the region is close to one collision domain, about 470
  links per node; `tools/density.py --tsim build/tsim` runs all four at several powers and seeds,
  from about 12 links per node to that, and prints each candidate's delivery at each density. Power
  stands in for spacing: under the log-distance channel, 5 dB quieter loses what standing 1.64
  times further apart does. The script says more.
