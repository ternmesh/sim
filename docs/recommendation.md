# Which routing design proceeds

**Recommendation:** candidate 3 proceeds. Tern routes with announce-driven distance vector, as
`tsim/distvec.h` has it:

- Babel's feasibility condition
- a metric of ETX times time on air, over links heard both ways
- Trickle announces held under a hard per-node airtime cap
- relays that forward, and leaves that do not

Managed flooding (candidate 1, Meshtastic's design) and path routing with a flood fallback
(candidate 2, MeshCore's) do not.

This answers MSH-27. The decision has already been acted on, and this page records why:

- The specification's [routes](https://github.com/ternmesh/spec/blob/main/draft/routing.md) were
  written from candidate 3's defaults.
- The firmware's routing core was written from the specification.
- `tsim/core.h` links that core back in, so what is measured of the design can be measured of the
  code a board runs.

Every figure below is already recorded in the header or scenario file named beside it, with the
commands that produced it. None is new here.

## The headline: delivery per second of airtime

The comparison is delivery per unit of airtime, not delivery alone. Delivery alone is what makes
flooding look acceptable until the network is dense enough to collapse.

Every node sends a message to another at random every half hour, with no broadcasts. The scenarios
are 200 nodes over 8 km, and 1000 nodes over 18 km on the deployed presets and on one fast radio
for all. Each cell gives unicast delivered on time, then deliveries per second of airtime. Three
seeds (`tsim/core.h`):

|                    | town (200)     | region, fast (1000) | region, deployed (1000) |
|--------------------|----------------|---------------------|-------------------------|
| Meshtastic         | 21.0%  0.008   | 19.0%  0.013        | 1.1%  0.001             |
| MeshCore           | -              | 30.3%  0.026        | 5.7%  0.004             |
| candidate 3        | 100.0%  0.122  | 99.4%  0.538        | 9.2%  0.006             |
| candidate 3, plain | -              | 96.8%  0.390        | 19.1%  0.016            |
| firmware core *    | 100.0%  0.132  | 96.7%  0.438        | 14.4%  0.013            |

"Plain" is candidate 3 without re-attachment or rescue floods, which the core has neither of.

\* These core figures used tables larger than a board's: 255 neighbours, a destination for
every node, and 16 frames of each kind. The board-table figures below come from a different
workload from the table's: the one `tools/density.py` offers, where a quarter of messages are
broadcasts. So they compare with each other, not with the table:

- **Every port's 1024 destinations and 128 neighbours** (firmware#79, the leaf's default route
  from firmware#80; 200 relays, `traffic.peers=4`): 96.9% of unicasts on time on the fast preset,
  and 99.3% on US915 (`tsim/core.h`, seeds 1 to 3).
- **128 destinations, as the board had before firmware#79:** 12.5% on time, against 96.4% with
  unbounded tables on the same workload (`tsim/core.h`, seeds 1 and 2). A relay can pass a frame
  on only toward a destination it has a place for.

Routing in a network larger than a board's table is not specified yet. No board-table figure for
the deployed preset is recorded with the current defaults.

**On one fast radio, candidate 3 does 15 to 40 times the incumbents' work for each second of
airtime, and delivers nearly everything.** The firmware's core delivers what candidate 3 does
plain.

**On the deployed presets every candidate is poor**, and that is the honest finding about those
presets rather than about routing. A slow narrow channel shared by a thousand nodes is full, and
most of what is sent is lost. Candidate 3 still leads there: as it is, by 1.5 times MeshCore's
airtime efficiency, and plain by four times. How badly the presets do is the reason the specification's
[radio settings](https://github.com/ternmesh/spec/blob/main/draft/phy.md) choose a fast profile.

## The questions MSH-27 asked

**Where each candidate's knee is.** In `tools/density.py`, transmit power stands in for density.
On the fast preset that runs from about 5 links per node at -5 dBm to about 284 at 20 dBm. Unicast
on time, from -5 to 20 dBm, 3 seeds, with a quarter of messages broadcast as `density.py` offers
them (`scenarios/scale/region-distvec-fast.tsim`):

| | -5 dBm | 0 dBm | 5 dBm | 10 dBm | 20 dBm |
|---|---|---|---|---|---|
| flooding | 14% | 35% | 47% | 51% | 46% |
| Meshtastic | 11% | 31% | 43% | 50% | 22% |
| MeshCore | 37% | 53% | 49% | 45% | 35% |
| candidate 3, at the first sweep | 47% | 89% | 97% | 97% | 96% |
| candidate 3, current defaults | 61% | 91% | 98% | 98% | 99% |

- **MeshCore** peaks at 0 dBm and falls as density rises.
- **Meshtastic** peaks at 10 dBm and loses more than half its delivery by 20 dBm, as its queues
  stop draining.
- **Candidate 3** has no knee in the range. On its current defaults it leads at every density,
  the sparsest included: there, its sparse-network and re-attachment settings took it from 47% to
  61%, past MeshCore's 37%.

The incumbents' rows, and the first candidate 3 row, are from the first sweep. The current-defaults
row is from the same file's later runs, with `link_margin_db` 0, sparse broadcasts and
re-attachment where few relays are heard.

The firmware core tracks candidate 3 across the same range (`tsim/core.h`).

**Whether the airtime metric prefers multi-hop fast links over single-hop slow ones, or is gamed.**
Per-link spreading factor was the test of that (MSH-49, `tsim/distvec.h`): letting each link pick
its own SF.

- From 0 dBm up, it beat a single slow SF: 88.6–98.6% against 63–70%.
- One faster SF for the whole network did as well for far less airtime: SF7 made 88.9–99.4% over
  the same range.

The metric does prefer fast hops. But the choice that matters is one fast SF for the network, not
an SF per link. Per-link SF stays off by default, and the specification has a single profile per
region.

**Whether tiering changes the answer.** Yes, it is part of it. Routes computed among relays only,
with leaves reached through them (MSH-53, MSH-56, MSH-68), are what kept candidate 3's announces
under its cap at a thousand nodes. The specification's
[roles](https://github.com/ternmesh/spec/blob/main/draft/routing.md#roles) are relays and leaves.

**Churn and mobility** (MSH-59). Unicast on time on the fast preset, -5 to 20 dBm, candidate 3 on
its current defaults: salvage, rescue floods, and the sparse-network and re-attachment settings
(`scenarios/scale/region-distvec-fast.tsim`, 3 seeds):

| Stress | Unicast on time, -5 to 20 dBm |
|---|---|
| Nothing stressed | 61, 91, 98, 98 and 99% |
| A quarter of the 200 relay sites going down and coming back | 46, 74, 90, 90 and 87% |
| A quarter of the leaves walking | 49, 72, 82, 90 and 94% |

This is candidate 3's weak side. Churn and movement cost it 5 to 19 points, where every
incumbent moves by 4 or less, because a route outlives the relay or leaf it leads to until
announces correct it. Under both, it still stays ahead of every incumbent at every density. At the
sparsest point, MeshCore manages 35% under churn and 37% under movement.

**Whether the DTN store-and-forward floor helps under mobility.** Not measured: it was never
built (`tsim/distvec.h`). Salvage and rescue floods answered most of what mobility cost, as above.
A floor is not needed for this decision. It stays open as a question for leaves that sleep.

## What this does not settle

- **The PHY is not calibrated.** Capture, inter-SF rejection, lock time and retune time are
  placeholders until the bench rig (MSH-32) measures them.
- **Per-frame fading flips the unicast headline on the deployed preset.** The sweeps
  ([docs/sensitivity.md](sensitivity.md), MSH-74) cover capture, co-SF and inter-SF rejection,
  lock time, shadowing, path-loss exponent and fading. On the fast preset candidate 3 leads at
  every point, by 2.4 times or more, though fading costs it unicast delivery where the network
  is sparse. On the deployed preset, fading of 4 dB from frame to frame ties its unicast
  deliveries per second of airtime with MeshCore's, and 6 dB puts it at 0.7 times MeshCore's.
  It still leads in all deliveries, which are mostly broadcast. Two cells at 20 dBm on the
  deployed preset also flip: all shadowing shared between links, and a 12-hour warmup. Fading
  is a must-measure item for the bench rig (MSH-32).
- **The core trailed candidate 3 plain on the deployed preset** because the 6-hour warmup ends
  before either has settled, and the core's routes settle more slowly: its signed announces
  cost more air under the same cap. Given 12 hours it delivers 18.5% against candidate 3
  plain's 8.3% (MSH-78, [docs/sensitivity.md](sensitivity.md)).
- **Relay election is not in the specification.** The scenarios pick relays as a connected
  dominating set, with knowledge of the whole network.
- **The workload is synthetic.** A field-derived traffic mix and scoped broadcast are MSH-34.

Each of these could move a number. The fading flip is confined to the deployed preset, where every
candidate is poor and which the specification's radio settings do not choose. A measured fading
that flipped the fast preset too would reopen this page.

## What would reopen this

- A PHY parameter, measured on the bench or swept across its published range, under which a
  flooding or path-routing design delivers more per second of airtime than candidate 3.
- A field pilot (MSH-69) whose measured link table, replayed here, does the same.
