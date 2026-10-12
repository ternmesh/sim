# The workload

MSH-27 compared the routing candidates on unicast: every node sends a message to another every
half hour. The field says unicast is not what fills a mesh's channel. Broadcast is. This page has
three parts:

- the traffic a mesh carries, measured where it could be and with every source named;
- the three scenarios built from it (`scenarios/workload/`);
- what each candidate does with them, scoped broadcast among them (MSH-34).

## What meshes carry

Two kinds of figure appear below:

- **[M]**: a figure someone else published.
- **[D]**: a figure derived here, collected on 2026-10-12. It comes from two public Meshview instances, or from arithmetic on [M] figures.

Meshview counts what MQTT gateways uploaded, deduplicated by packet id. That undercounts what is on
the air, and one week of it is noisy: Bay Mesh's daily total moved from 37.7k packets to 16k within
the week.

### Packet mix

| Kind | Bay Mesh [D] | South Carolina [D] | Elsewhere |
|---|---|---|---|
| NodeInfo (who a node is) | 40.4% | 14.3% | "40% of the packets in our mesh" [M]<sup>1</sup> |
| Position | 27.9% | 62.7% | |
| Telemetry | 12.3% | 14.3% | "the primary problem" in a Serbian mesh [M]<sup>2</sup> |
| Not decoded (direct messages, other keys) | 12.0% | 5.4% | |
| Text | **0.78%** | **0.73%** | "less than 2%" [M]<sup>1</sup>; 4.1% at Burning Man [M→D]<sup>3</sup> |
| Routing, traceroute, neighbours, store and forward, map | 5.4% | 2.0% | |
| MeshCore adverts | | | about 20% of packets in Portland, 81% of them from repeaters [M]<sup>4</sup>; 45-50% of bytes [M]<sup>5</sup> |

Sources:

- **Bay Mesh:** `meshview.bayme.sh/api/stats`, 2026-10-05 to 10-12, 163,175 packets.
- **South Carolina:** `meshview.scmesh.us`, the same method and window, 41,921 packets.

Numbered sources:

1. [meshtastic/firmware#8924](https://github.com/meshtastic/firmware/issues/8924)
2. [meshtastic/firmware#7963](https://github.com/meshtastic/firmware/issues/7963)
3. [Burners Without Borders](https://burnerswithoutborders.org/projects/meshtastic-meets-burning-man/): 28,817 packets and 1,189 messages on the busiest day, from 1,606 nodes
4. [MeshCore#1949](https://github.com/meshcore-dev/MeshCore/issues/1949)
5. [MeshCore#1223](https://github.com/meshcore-dev/MeshCore/issues/1223)

**What a mesh carries is mostly nodes describing themselves.** The text people read is under 1% of
packets. A workload of messages alone measures the 1%.

### Rates

Each figure is per node per day.

**Bay Mesh** [D]: 870 nodes seen in a day.

| Kind | Rate |
|---|---|
| NodeInfo | about 10, replies included |
| Position | about 7 |
| Telemetry | about 3 |
| Text | 0.22, from about 17% of nodes |

Bay Mesh's 1,000 most recent text messages give the text sizes:

- 999 of the 1,000 were broadcasts.
- 151 senders; the top five sent 45%.
- Main channel: median 16 bytes, mean 29.
- All channels: median 22 bytes, mean 37.6.

**Meshtastic's defaults** [M] (`src/mesh/Default.h`):

| Kind | Default |
|---|---|
| NodeInfo | every 3 h |
| Position | every hour; every 5 min while moving 100 m; every 6 h when still |
| Device telemetry | every hour |
| Router | 12 h each |
| Above 40 nodes online | every interval stretches with the count |

The sim's Meshtastic plugin has the older 15-minute position (`tsim/meshtastic.h`).

**MeshCore** [M] (`examples/simple_repeater/MyMesh.cpp`): a repeater floods its advert every 47 h
and sends one zero-hop every 2 min; a companion has no timer.

**Payload sizes** [D], Bay Mesh re-encoded:

| Kind | Median | 90th percentile |
|---|---|---|
| Position | 28 B | 36 B |
| Telemetry | 28 B | |
| NodeInfo | 79 B | 91 B |

A Tern [card](https://github.com/ternmesh/spec/blob/main/draft/cards.md) is 96 B.

**Events.** DEF CON had more than 2,000 nodes and published no rates
([blog](https://meshtastic.org/blog/that-one-time-at-defcon/)). Burning Man's busiest day was 18
packets and 0.74 text messages a node [M→D].

**Not measured anywhere found:**

- Direct-message rates. At Bay Mesh, at most the 12% not decoded, about 3 a node a day.
- How often alerts are sent. Bay Mesh's "Alerts" channel had 4 of 1,000 text messages.
- What share of broadcasts matter only locally. MeshCore's regions exist for that, and few use
  them ([MeshCore#2041](https://github.com/meshcore-dev/MeshCore/issues/2041)).

## The scenarios

The traffic is made of flows (`flow.<name>.*` in [scenarios.md](scenarios.md)). Each flow is a
named kind of message on processes of its own, and each is reported apart.

**Where a broadcast is wanted.** Presence, position and telemetry are of local interest. Each is
wanted only by nodes within a **reach** of its sender. A node beyond that reach which gets one is
counted as `beyond`, not as delivered. That is the airtime a scope could save. Chat and emergency
alerts are wanted by every node.

**Rates** are per sending node.

| Flow | `metro.tsim` | `event.tsim` | `dm.tsim` |
|---|---|---|---|
| network | 1000 nodes over 18 km, 20 dBm, 200 relays | 1000 nodes over 4 km, 14 dBm, 100 relays | as metro |
| presence, 96 B | every 3 h, within 5 km | every 1 h, within 1.5 km | as metro |
| position, 28 B | every 206 min, within 5 km | every 15 min, within 1.5 km | as metro |
| telemetry, 28 B | every 8 h, within 5 km | every 8 h, within 1.5 km | as metro |
| chat, 8-50 B, to all | 17% of nodes, every 18 h | 33% of nodes, every 8 h | as metro |
| dm, 8-50 B, to a peer | every 24 h, half answered | every 24 h, half answered | every 4 h, 70% answered |
| emergency, 40 B, to all | 2% of nodes, every 2 h | as metro | as metro |

All three share these settings:

- the fast preset: SF7, 125 kHz;
- the 10% duty cycle;
- four regular correspondents per node;
- a 6 h warmup, the last 3 h carrying traffic, then 1 h measured;
- a 60 s deadline.

**How each scenario was chosen.**

- **Metro** is Bay Mesh's measured mix, at Meshtastic's 3-hour NodeInfo. Its one DM a node a day
  is a guess inside the bound above.
- **Event** has people walking, so positions come at Meshtastic's old 15-minute default; nodes
  arrive and leave, so presence comes hourly; the channel is busy. Its chat is three lines a day
  from a third of the nodes, four times Burning Man's busiest day. This is the broadcast-heavy
  scenario.
- **DM-heavy** keeps MSH-27's unicast question answerable under the field's background. Its six
  DMs a day sit inside the two to ten that the field allows without measuring.
- **Emergency** is a probe, not a rate. Alerts are too rare to measure in an hour, so 20 nodes
  send one every two hours, to see whether an alert gets through the load.

## Results

`tools/workload.py --tsim build/tsim --seeds 3` runs every candidate below on each scenario.

The incumbents carry the flows as their own NodeInfo, position and telemetry. Their own background
is off, so nothing is counted twice. Meshtastic holds to the duty cycle as its firmware does.

The candidates:

| Row | What it is |
|---|---|
| `meshtastic` | managed flooding (candidate 1) |
| `meshcore` | path routing with a flood fallback (candidate 2) |
| `distvec` | candidate 3, every broadcast going `routing.bcast_hops` relays |
| `distvec, geo` | candidate 3 with broadcasts scoped by place (`routing.bcast_geo`) |
| `core` | the firmware's routing with a board's tables, flooding as `draft/flooding.md` has it |
| `core, 1 hop` | the core with presence, position and telemetry asking for one relay hop, the scope a flood's head already carries |

RESULTS
