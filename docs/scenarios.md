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

1. **Warmup:** a period with no traffic, so protocols that announce themselves can do so first.
2. **Duration:** the period in which traffic is generated.
3. **Deadline:** a final period with no new traffic, so the last messages have as long to arrive as
   the first.

Airtime and duty cycles are measured over all three parts.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `nodes` | required | number of nodes, 1 to 1048576 |
| `routing` | required | routing plugin: `flood`, `meshtastic` or `meshcore` |
| `mac` | required | MAC plugin: `aloha`, `meshtastic` or `meshcore` |
| `seed` | 1 | the seed for every random draw in the run: positions, shadowing, traffic, message content, plugins |
| `placement` | `uniform` | `uniform`, `grid`, `line`, or `file` to read them from `positions` |
| `positions` | none | for `file`: a file with one node per line, as `x y` in metres, in node order; `#` starts a comment. `tsim` reads a relative path from the scenario file's directory |
| `links` | none | a file with the loss of each link, which then replaces the channel model and the positions: one link per line, as `a b loss` in dB, or `a b there back` where the two directions differ; a link the file leaves out loses everything. `tsim` reads a relative path from the scenario file's directory |
| `area` | `5000 x 5000` | metres, for `uniform` |
| `spacing` | 1000 | metres between neighbours, for `grid` and `line` |
| `warmup` | `0 s` | time before traffic starts |
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
| `traffic.send` | none | one message at a set time, as `30 s, 2, all, 40`: when, from which node, to which node or to `all`, and how many bytes. Each adds one, up to 64. It uses none of the traffic process's draws, and in the closed loop its finishing starts no gap |
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

The routing and the MAC each keep their own copy of the window: the routing sizes its
acknowledgement wait by it. Settings that bound each other, such as `cw_min` and `cw_max`, are
checked once every setting is in, so they can be written in any order.

## The report

`tsim` prints one JSON object. For a given scenario and seed it is identical on every run on the
same platform, so two reports can be compared with `diff`.

The run's CPU time goes to stderr, because it differs between runs.

| Field | Meaning |
|---|---|
| `on_time_per_airtime_s` | **the headline:** deliveries that arrived within the deadline, per second of airtime spent by all nodes for any purpose |
| `duty_max`, `duty_max_node` | the busiest node's airtime as a fraction of the run, and which node that is (the lowest-numbered, if several tie) |
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
  runs `region.tsim` (1000 nodes, one hour of traffic) and `region-meshtastic.tsim` (the same,
  under candidate 1) as a release build with a 60-second budget.
