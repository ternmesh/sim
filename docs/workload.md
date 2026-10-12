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
| `core, hops 1` | the core with presence, position and telemetry sent with a flood's `hops` at 1: no relay in a crowd passes them on, and a bridge does |
| `core, hops 2` | the same at 2: one relay in a crowd. `draft/flooding.md` already lets the section that defines a frame set fewer than `FLOOD_HOPS` (5), as cards do |

Each cell is the mean over seeds 1 to 3. The flow columns are the share of what was wanted that
arrived within 60 s.

| Column | Meaning |
|---|---|
| beyond | per local broadcast, the nodes outside its reach that got it |
| airtime | seconds on the air in the measured hour, all 1000 nodes together |
| per_s | on-time deliveries per second of that airtime |

### Metro: the realistic mix

| | presence | position | telemetry | chat | dm | emergency | beyond | airtime | per_s |
|---|---|---|---|---|---|---|---|---|---|
| meshtastic | 99.1% | 99.8% | 99.7% | 95.2% | 95.3% | 94.5% | 773 | 12727 | 10.9 |
| meshcore | 45.9% | 57.6% | 60.7% | 57.9% | 83.8% | 55.0% | 405 | 14378 | 5.3 |
| distvec | 87.6% | 92.5% | 92.5% | 93.4% | 99.3% | 92.8% | 711 | 8324 | 15.4 |
| distvec, geo | 96.0% | 96.5% | 96.7% | 98.9% | 99.3% | 98.8% | 457 | 2950 | 46.4 |
| core | 84.8% | 86.0% | 86.5% | 85.1% | 96.2% | 79.7% | 653 | 11643 | 10.3 |
| core, hops 1 | 61.7% | 63.1% | 65.2% | 97.1% | 100.0% | 97.0% | 247 | 4003 | 24.2 |
| core, hops 2 | 87.2% | 88.1% | 89.0% | 94.4% | 99.3% | 95.0% | 561 | 6029 | 20.8 |

### Event: broadcast-heavy

| | presence | position | telemetry | chat | dm | emergency | beyond | airtime | per_s |
|---|---|---|---|---|---|---|---|---|---|
| meshtastic | 5.9% | 7.3% | 7.2% | 2.9% | 3.3% | 3.3% | 114 | 21079 | 5.5 |
| meshcore | 16.3% | 20.8% | 17.8% | 17.3% | 45.0% | 23.8% | 110 | 13498 | 24.5 |
| distvec | 45.9% | 55.9% | 56.1% | 43.6% | 63.7% | 43.1% | 298 | 13599 | 66.0 |
| distvec, geo | 63.6% | 69.8% | 71.9% | 68.1% | 85.9% | 59.4% | 270 | 9570 | 119.9 |
| core | 43.4% | 49.1% | 44.9% | 43.2% | 73.0% | 53.0% | 256 | 15961 | 50.2 |
| core, hops 1 | 34.9% | 35.4% | 35.4% | 95.5% | 100.0% | 95.9% | 38 | 2935 | 216.5 |
| core, hops 2 | 59.7% | 60.9% | 59.8% | 75.4% | 97.4% | 81.0% | 236 | 8628 | 118.9 |

### DM-heavy

| | presence | position | telemetry | chat | dm | emergency | beyond | airtime | per_s |
|---|---|---|---|---|---|---|---|---|---|
| meshtastic | 73.4% | 75.8% | 74.7% | 48.4% | 45.4% | 44.8% | 425 | 24948 | 4.0 |
| meshcore | 15.8% | 24.1% | 25.3% | 19.5% | 50.5% | 16.2% | 122 | 20575 | 1.5 |
| distvec | 88.2% | 92.4% | 90.0% | 93.3% | 98.5% | 80.8% | 705 | 9001 | 14.2 |
| distvec, geo | 95.7% | 96.2% | 96.3% | 98.7% | 99.5% | 96.6% | 448 | 3679 | 37.4 |
| core | 81.5% | 85.1% | 85.0% | 76.5% | 95.1% | 80.6% | 633 | 12976 | 9.0 |
| core, hops 1 | 59.0% | 61.3% | 62.3% | 97.3% | 99.8% | 91.7% | 227 | 4625 | 20.3 |
| core, hops 2 | 85.8% | 86.8% | 88.1% | 94.3% | 99.2% | 97.0% | 527 | 6899 | 18.1 |

### What it says

**MSH-27's answer stands on the field's mix.**

- Candidate 3 (`distvec`) and the core deliver direct messages at 95-100% on the metro and the
  DM-heavy mix, and at 64-73% at the event (86% with scope by place).
- Meshtastic delivers 95% on the metro, but 45% once DMs are six a node a day, and 3% at the event.
  Its queues overflow there: about 374,000 frames dropped in the hour, and a DM's 95th-percentile
  delay is 39 minutes.
- MeshCore delivers 45-84%.
- On deliveries per second of airtime, candidate 3 and the core lead every incumbent on every
  scenario, scoped or not, apart from the light metro. There, Meshtastic's 10.9 per second sits
  between the core's 10.3 and candidate 3's 15.4.

**On a light load, flooding everything works, at a price.**

- Meshtastic delivers 99% of local broadcasts on the metro, where Tern's unscoped floods deliver
  85-93%.
- It spends 12.7 ks of airtime to do it. Each local broadcast reaches about 770 nodes outside its
  5 km reach, to deliver to about 190 within it.
- The same price is what collapses it at the event.

**Scope is the largest single gain measured here.**

On the broadcast-heavy event, against unscoped flooding through the same relays:

- **Scope by place** (`distvec, geo`) raises every flow:
  - local broadcasts from 46-56% to 64-72%;
  - chat from 44% to 68%;
  - DMs from 64% to 86%;
  - on 30% less airtime, at 1.8 times the deliveries per second of airtime.
- **On the metro**, scope by place delivers 96-99% of everything, local broadcasts included.
  - It uses a third of candidate 3's unscoped airtime (2950 s against 8324 s), and a quarter of the
    core's.
  - That is 3 times the deliveries per second of airtime, and 4.2 times Meshtastic's.
- **Hops alone**, which the specification can already say:
  - At 2, the core's local broadcasts deliver as well or better than unscoped: 87-89% against
    85-86% on the metro, and 60-61% against 43-49% at the event.
  - Chat, DMs and alerts all rise.
  - Airtime falls by 48% on the metro and 46% at the event.
  - At 1, hops are too few for a 1.5-5 km reach: local broadcasts fall to 35-65%.
- **Hops are a poor proxy for distance.** At the same metro airtime, scope by place delivered 96%
  where hops 2 delivered 88%.

**Emergency alerts do not get through a broadcast-heavy channel on their own.**

At the event, an alert to every node reached:

| Candidate | Alert reached |
|---|---|
| Meshtastic | 3% |
| MeshCore | 24% |
| Tern, unscoped | 43-53% |
| Scoped by place | 59% |
| Hops 2 | 81% |
| Hops 1 | 96% |

The last one is only because local traffic was cut back hardest. Nothing in any candidate treats an
alert differently from a chat line. The alert gets through only when the rest of the load is kept
down.

**With broadcasts scoped, Tern's own routing is the largest user of airtime.** On the metro, scope
by place, announces are 1631 s of 2950 s (55%); unscoped they are 2828 of 8324 (34%). The research
behind this work flagged control traffic as the risk a routed design adds
([Research 01](https://linear.app/sailfin/document/research-01-field-pain-points-meshtastic-meshcore-reticulum-fa77bb0241f1), P2).

**Tern's flood under-delivers on a light load.** On the metro, where the channel is quiet, the
core's floods reach 85-86% within the reach and candidate 3's 88-93%, where Meshtastic's reach 99%.
Scope closes the gap for candidate 3 (96%) and narrows it for the core (88% at hops 2). Why the
unscoped flood leaves 7-15% behind on a quiet channel is [below](#why-the-flood-under-delivers-msh-104).

### Why the flood under-delivers (MSH-104)

`report.floods = yes` books, for each broadcast, every node that wanted it and missed it, and why
(docs/scenarios.md). On the metro, seed 1, the core missed 22,601 deliveries:

- 38% were `far`: no node that sent a copy was in reach of them. 17% were `quiet`, and 46% `lost`:
  copies came in above the floor and were lost on the air.
- 80 of the 638 broadcasts reached **no relay at all**, and those 80 were 75% of the misses. Each
  was sent once, by its source, while every relay in reach of it - 20 to 50 of them - was receiving
  another frame. A relay hears a 5 km circle, so even a quiet mesh keeps it receiving much of the
  time. Once a flood reached relays it did well.
- The flood had no answer to this: a node sent its own frame once and could not know it had gone
  nowhere. Meshtastic's sender listens for a rebroadcast and sends again.

The firmware now listens for a relay passing its own frame on and, hearing none within 16
airtimes, sends it once more (`routing.flood_resends`, `routing.flood_echo`; ternmesh/spec#49).
The core, three seeds, once against with one resend:

| | | Local | Chat | Alerts | Direct | Airtime |
|---|---|---|---|---|---|---|
| Metro | once | 85.0-86.3% | 81.3-88.0% | 62.7-95.5% | 95.6-97.0% | 11.6 ks |
| | again | 92.7-94.2% | 91.5-94.7% | 75.7-95.5% | 93.3-97.3% | 12.6 ks |
| Event | once | 43.0-47.6% | 40.5-45.1% | 50.9-56.9% | 65.8-84.4% | 16.0 ks |
| | again | 53.0-54.9% | 40.8-54.3% | 32.3-69.0% | 58.9-65.7% | 16.1 ks |
| DM-heavy | once | 82.9-84.3% | 72.0-80.2% | 61.3-95.0% | 94.0-95.8% | 13.0 ks |
| | again | 91.0-93.1% | 85.4-94.3% | 91.3-94.4% | 89.3-93.3% | 13.9 ks |

The frames sent again are few: 144 an hour on the metro. What the airtime pays for is the relays
carrying floods that used to die at the first hop, and that is what costs direct messages: their
first hops are lost to relayed frames. Two resends gained a point or two of broadcast for as much
again of direct messages. Listening longer (32 or 64 airtimes) heard no more copies: at the event,
where some 3,000 frames an hour went again, the copies were never sent, busy relays having dropped
them.

## Recommendation for the specification (MSH-30)

1. **A flood's scope in hops should be set per kind of frame, now.**
   - `draft/flooding.md` already lets the section that defines a frame set fewer than
     `FLOOD_HOPS`.
   - Position and telemetry, when they are specified, should start at `hops` 2. Cards should be
     looked at again: they carry a `CARD_HOPS` of their own.
   - This needs no new field, and on these workloads it halves the airtime and raises every flow.
2. **Scope by place should be the next scoped primitive, and not yet normative.**
   - It beats hops on every scenario.
   - It needs the sender's position on the air (8 bytes here), which [positions](https://github.com/ternmesh/spec/blob/main/draft/positions.md) keeps off by default for privacy.
   - It needs a rule for a node that does not know where it stands.
   - Those are design questions, not measurement ones. A coarse cell rather than a position, as
     MeshCore's regions are, is the natural next thing to measure.
3. **Emergency needs a priority class of its own.** It should have its own small allowance, ahead
   of floods in the queue. Scope helps an alert only by quieting everything else, and on the
   broadcast-heavy event no scope that keeps local traffic useful got alerts above 81%.
4. **Two follow-ups for the simulator and the firmware:**
   - why an unscoped Tern flood leaves 7-15% of a quiet channel behind;
   - what the announces cost once broadcasts are scoped.

## Reproducing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # with the firmware beside it, for the core rows
cmake --build build
tools/workload.py --tsim build/tsim --seeds 3 --out workload-out
```

The 63 runs take about an hour and a half on four cores. A build without the firmware runs every row but the
core's.
