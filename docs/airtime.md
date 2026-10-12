# The airtime budget, measured

The specification's `draft/airtime.md` (ternmesh/spec#42) gives every node an airtime budget in
three parts: routing, as Routes' cap has it; the node's own frames, 1% of its time held over
600 s; and, for a relay, the frames it passes on, 8% held over 60 s. A frame of its own that its
part does not hold waits; a frame passed on is dropped; an acknowledgement always goes. Its numbers
were set before the simulator kept one. This page is what keeping one does to candidate 3, beside
Meshtastic and MeshCore, on the town and on the region's fast and deployed presets (MSH-92).

## The answer

- **The relayed part does not bind at the traffic the comparison was made at.** On the fast
  preset nothing is dropped at 8%, and at 4% 0.04% at most: the busiest relay passes frames on
  for 1.0 to 2.2% of its hour.
  On the deployed preset 8% drops 0.07 to 1.2% of what relays are asked to pass on, 0.4 to 3.6%
  at the ten busiest and at most 5.3% at the busiest one, at -5 dBm; delivery does not move. 12%
  would drop next to nothing (0.07 to 0.5% at the ten busiest).
- **It starts to cost below 4%.** At 2% candidate 3 loses 6 to 7 points of unicast on the fast
  preset at -5 and 0 dBm, the ten busiest relays dropping 8 to 9%; on the deployed preset 4% loses
  2.6 points at -5 dBm and 2% 3 to 10 points. On the fast preset 8% is at least twice what the
  comparison's traffic needs; on the deployed preset it is about what that traffic needs where
  the network is sparsest.
- **The own part binds first, and on retries.** A message may go 24 times from its source: four
  tries, each hop sent three times, and once more another way (`RETRIES`, `HOP_RETRIES` and
  `SALVAGE` in `draft/forwarding.md`). A 60-byte frame takes 0.12 s on the fast preset but
  0.40 s at SF9 in the town and 0.71 s on the deployed preset, so where frames are lost one or two
  messages' retries spend the own part's 6 s. On the deployed preset at the comparison's own
  traffic it held frames 3,100 to 4,600 times an hour, about a minute at a time, the
  95th-percentile unicast delay rising from 73-117 s to 151-291 s. Unicast on time did not move, and deliveries
  per second of airtime rose 12 to 47%: the retries it held back were mostly lost anyway.
- **Under load the own part collapses delivery.** With a message from every node every 5 minutes,
  six times the comparison's, candidate 3 in the town falls from 19.9% of unicasts on time to
  4.9%, and on the deployed preset from 2.1-3.7% to 0.2-0.7%, the unicasts it still delivers
  there arriving up to 31 to 42 minutes late (95th percentile). With 2% the town delivers 18.7% and the deployed preset 1.8-3.0%, near the budget off;
  with 4% nothing in the town is held.
- **Against nodes that write without pause it does what it is for.** With a twentieth of the
  nodes sending a message every 10 seconds, the other nodes' unicasts arrive on time 36 to 69% of
  the time on the fast preset with the budget, against 14 to 19% without, and 11 to 18% against
  3 to 10% on the deployed preset. The writers pay: their frames wait and nothing drops them, so
  every message they send goes out behind a backlog minutes long. See
  [Writing without pause](#writing-without-pause).
- **Own 2% and relayed 7% delivers more unicast, sooner, than the draft's values.** It keeps the
  budget at 9.6875% of a relay's time, as the draft's does. On the deployed preset it delivers
  23.1/24.2/12.3% of unicasts on time at -5/0/20 dBm against 22.5/23.4/11.9% with no budget and
  22.6/22.7/11.5% with the draft's, with a 95th-percentile delay of 72-126 s against 73-117 s and
  151-291 s. Its deliveries per second of airtime, 3.14/3.50/1.03, are above no budget's
  (2.91/3.41/0.94) and below the draft's (3.25/3.86/1.38), whose held retries leave more of the
  channel to broadcasts. In the town at six times the traffic, 2% of own holds the budget near
  off where 1% cuts unicast on time by three quarters.

At the comparison's traffic candidate 3 leads Meshtastic and MeshCore in deliveries per second
of airtime with the budget on as it did with it off. At six times that traffic on the deployed
preset, the own part at 1% takes it below both at -5 and 0 dBm (0.24 and 0.10 against MeshCore's
0.38 and 0.41); at 2% it stays above them.

## What was built

`tsim/budget.h` is the budget as the reference model `vectors/tools/airtime.py` keeps it: token
buckets in millionths of a nanosecond, each full at the start and holding at least one 255-byte
frame, the flood allowances of `draft/flooding.md` inside the parts. `tests/budget.c` replays
`vectors/airtime.json` frame by frame. The network keeps one for every node (`airtime.budget`,
`docs/scenarios.md`); candidate 3 marks each frame with the part that pays for it, and no other
routing marks any, so Meshtastic and MeshCore run as they did. A frame of the node's own its
buckets do not hold is passed over by the MAC with every frame of its own behind it, while frames
passed on and routing's go by. A frame passed on is dropped if its buckets do not hold it when the
MAC would send it, after the duty cycle and the routing's own check, and candidate 3 forgets it as
a withdrawn frame, so the hop before, hearing nothing, sends it again or another way; a flood
dropped so stays seen, and its later copies are ignored, as `draft/flooding.md` has it. A node keeps
at most 32 of its own frames waiting, a board's companion link's worth (LINK_MESSAGES); one more
is refused. `airtime.budget = shadow` sends everything and counts what would have waited or been
dropped.

The core is not measured. The firmware's forwarder has no way to drop a frame it was handed to pass
on: withdrawn, it goes back to the forwarder's queue and is offered again. That is for the firmware
to add.

## How it was run

`tools/density.py` as [sensitivity.md](sensitivity.md) runs it: 3 seeds, -5, 0 and 20 dBm, a
message from every node every 30 minutes, a quarter broadcast, `--quick`. `--fast` is every
candidate on SF7 at 125 kHz over the same 200 sites; `--deployed` each on its own preset,
candidate 3 on SF8 at 62.5 kHz with coding rate 4/8. Both hold every radio to the sub-band's 10%
as well. Release build, the core from the firmware commit CI pins (e300726). The budget-off rows
reproduce sensitivity.md's default rows to the digit.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTSIM_FIRMWARE_DIR=../firmware
cmake --build build
tools/density.py --tsim build/tsim --fast --quick --seeds 3 --power=-5,0,20 \
    --candidates 1,2,3 --json fast.json
tools/density.py --tsim build/tsim --fast --quick --seeds 3 --power=-5,0,20 \
    --candidates 3 --json fast-on.json -s airtime.budget=on
```

and the same with `--deployed`. Each row below adds its settings to the second command:
`-s airtime.relayed=4%`, `-s airtime.own=2%`, `-s 'traffic.interval=5 min'` for the heavier
traffic, `-s traffic.chatty=0.05 -s 'traffic.chatty_interval=10 s'` for nodes writing without
pause. density.py prints `held`, `drop`, `busy` and `hit` where a run keeps the budget; the
busiest relay's own figures are in its `--json`, under `busiest`.

The town is `scenarios/town-distvec.tsim`, every one of its 200 nodes a relay, at SF9 and 20 dBm,
with `town.tsim` (flooding) and `town-meshtastic.tsim` beside it, 5 seeds:

```bash
for seed in 1 2 3 4 5; do
    build/tsim -s seed=$seed -s airtime.budget=on scenarios/town-distvec.tsim
done
```

In the tables, per s is on-time deliveries per second of airtime; held, own frames that waited,
every node, an hour, those waiting as the hour began among them; wait, how long a wait took,
counting only its part within the hour - a frame waits while it is short or behind one of its
own that is, until it could go, and one held behind another and then for its own part waits
twice; dropped, frames passed on that were dropped as a share of all relays were
asked to pass on; ten busiest, the same at the ten nodes asked to pass on the most; relay share,
the busiest relay's airtime passing frames on over the hour.

## At the comparison's traffic

| | dBm | per s: Meshtastic / MeshCore / c3 off / c3 on | unicast on time, % off / on | broadcast, % off / on | held | dropped | ten busiest | relay share |
|---|---|---|---|---|---|---|---|---|
| fast | -5 | 3.34 / 1.99 / 25.85 / 25.85 | 60.6 / 60.6 | 39.9 / 39.9 | 0 | 0 | 0 | 2.2% |
| fast | 0 | 4.89 / 2.86 / 34.84 / 34.84 | 91.2 / 91.2 | 66.9 / 66.9 | 0 | 0 | 0 | 2.0% |
| fast | 20 | 3.79 / 2.48 / 56.01 / 56.01 | 98.6 / 98.6 | 90.2 / 90.2 | 0 | 0 | 0 | 1.0% |
| deployed | -5 | 0.65 / 0.43 / 2.91 / 3.25 | 22.5 / 22.6 | 20.8 / 22.2 | 3,111 | 1.2% | 3.6% | 5.6% |
| deployed | 0 | 0.78 / 0.44 / 3.41 / 3.86 | 23.4 / 22.7 | 23.3 / 24.6 | 4,629 | 0.66% | 1.8% | 4.5% |
| deployed | 20 | 0.18 / 0.34 / 0.94 / 1.38 | 11.9 / 11.5 | 5.7 / 8.4 | 4,410 | 0.07% | 0.4% | 3.0% |
| town | 20 | 0.55 (flooding 0.88) / - / 3.04 / 3.00 | 99.0 / 99.3 | 95.7 / 94.0 | 5 | 0 | 0 | 0.7% |

On the fast preset the budget changes nothing: no frame waits, none is dropped, and shadow mode
finds none it would have held or dropped. In the town 5 frames an hour wait, a node whose next hop
kept missing its two messages sending 31 frames in a minute; the differences are within the seeds'
spread (unicast ±0.4, deliveries per second ±0.13).

On the deployed preset the two parts do different things. Taken apart:

| deployed | dBm | per s | unicast, % | broadcast, % | held | wait, s | p95 delay, s | dropped | ten busiest |
|---|---|---|---|---|---|---|---|---|---|
| no budget | -5 / 0 / 20 | 2.91 / 3.41 / 0.94 | 22.5 / 23.4 / 11.9 | 20.8 / 23.3 / 5.7 | - | - | 75 / 73 / 117 | - | - |
| relayed part only | -5 / 0 / 20 | 2.95 / 3.34 / 0.92 | 22.2 / 23.7 / 12.1 | 20.9 / 22.8 / 5.5 | 0 | - | 76 / 76 / 115 | 1.3 / 0.54 / 0.06% | 3.8 / 1.8 / 0.3% |
| own 1%, relayed 8% (draft) | -5 / 0 / 20 | 3.25 / 3.86 / 1.38 | 22.6 / 22.7 / 11.5 | 22.2 / 24.6 / 8.4 | 3,111 / 4,629 / 4,410 | 63 / 64 / 63 | 151 / 279 / 291 | 1.2 / 0.66 / 0.07% | 3.6 / 1.8 / 0.4% |
| own 2%, relayed 8% | -5 / 0 / 20 | 3.04 / 3.50 / 0.91 | 22.8 / 24.8 / 12.0 | 21.4 / 23.6 / 5.4 | 310 / 689 / 434 | 27 / 29 / 29 | 78 / 77 / 121 | 1.1 / 0.50 / 0.05% | 3.5 / 1.4 / 0.2% |
| own 4%, relayed 8% | -5 / 0 / 20 | 2.94 / 3.36 / 0.91 | 22.9 / 24.1 / 12.2 | 20.8 / 22.9 / 5.5 | 1 / 6 / 0 | 3 / 5 / - | 74 / 77 / 117 | 1.3 / 0.57 / 0.04% | 3.9 / 1.7 / 0.2% |
| own 2%, relayed 7% | -5 / 0 / 20 | 3.14 / 3.50 / 1.03 | 23.1 / 24.2 / 12.3 | 22.0 / 23.7 / 6.2 | 348 / 666 / 441 | 28 / 29 / 28 | 72 / 77 / 126 | 2.0 / 0.92 / 0.12% | 5.8 / 2.8 / 0.5% |

The relayed part alone costs nothing measurable. The own part at 1% holds frames 3,100 to 4,600
times an hour, about a minute at a time, and a frame may be held more than once: behind one of
its own that is short, and then for its own part when that one has emptied it. The messages it
holds arrive late, past the 60 s deadline, or not at all, and since the channel is full they
mostly would not have arrived anyway. What it saves in retries goes to broadcasts and lifts
deliveries per second of airtime. At 2% it holds a tenth as many, for under half a minute, and
delay is back where it was.

## The relayed part, and the busiest relays

Does 8% bind the busiest relays over a minute where their hour's average does not? On the fast
preset, no: shadow mode, which charges every frame and counts what would have been dropped, finds
nothing at any power. On the deployed preset, a little, and only where the network is sparsest:
at -5 dBm the busiest relay passes frames on for 5.6% of its hour and drops 5.3% of what it is
asked; the ten busiest drop 3.6%, and the network 1.2%. A unicast that loses a frame to a spent
relay is still delivered on time 23% of the time at -5 dBm and 31% at 0 dBm, where 22 to 23% of
all unicasts are: it goes again from the hop before, or another way, and a drop at a spent relay
costs it no more than the full channel does.

Swept, unicast on time at -5 / 0 / 20 dBm, deliveries per second of airtime, and what the ten
busiest relays drop:

| relayed share | fast: unicast, % | fast: per s | fast: ten busiest | deployed: unicast, % | deployed: per s | deployed: ten busiest | town: unicast, % |
|---|---|---|---|---|---|---|---|
| 16% | as 8% | | | 21.8 / 22.4 / 11.2 | 3.08 / 3.88 / 1.33 | 0.3 / 0.5 / 0.07% | |
| 12% | as 8% | | | 22.4 / 22.8 / 11.2 | 3.15 / 3.92 / 1.33 | 0.3 / 0.5 / 0.07% | |
| 8% | 60.6 / 91.2 / 98.6 | 25.9 / 34.8 / 56.0 | 0 | 22.6 / 22.7 / 11.5 | 3.25 / 3.86 / 1.38 | 3.6 / 1.8 / 0.4% | 99.3 |
| 4% | 60.4 / 91.0 / 98.6 | 26.2 / 34.4 / 56.0 | 0.4 / 0.1 / 0% | 20.0 / 22.7 / 11.4 | 4.17 / 4.77 / 2.02 | 21 / 14 / 8% | 99.2 |
| 2% | 54.5 / 84.6 / 98.7 | 27.0 / 31.2 / 59.9 | 7.8 / 8.8 / 0.2% | 12.9 / 18.0 / 8.6 | 4.45 / 5.80 / 3.21 | 42 / 35 / 23% | 97.8 |
| 1% | 35.4 / 58.7 / 94.8 | 27.3 / 34.8 / 59.4 | 25 / 28 / 7.9% | 7.5 / 10.9 / 5.5 | 3.80 / 5.90 / 3.80 | 61 / 60 / 50% | 96.2 |

Every row's own part is the draft's 1%, so the deployed preset's own frames wait as in the 8%
row.

Read the deployed columns with care: dropping more raises deliveries per second of airtime while
unicast falls. What relays no longer pass on - mostly retries of frames that would have been lost
- goes to broadcasts, whose reach rises (from 8 to 25% at 8% to 20 to 32% at 2%). The headline is not the measure to set
this share by; unicast on time is, and it falls from 4% down.

With six times the traffic (a message every 5 minutes) the fast preset's busiest relay passes
frames on for 7.1% of its hour at -5 dBm, and at 8% drops 10% of what it is asked, the ten busiest
2.3%, with delivery unchanged (24.9% of unicasts against 24.4%). At 4% the ten busiest drop 27%
and unicast falls 5 points. In the town at that traffic the busiest relay passes on 3% of its hour;
8% drops 0.45% of relayed frames.

## The own part, and retries

1% of 600 s is 6 s. The draft's rationale counted it against one try of a message; a message may
go 24 times from its source, and where frames are lost it does. 24 tries of a 60-byte frame take
2.9 s on the fast preset, 9.7 s at the town's SF9 and 16.9 s on the deployed preset. So the own
part holds the frames of a node retrying hard, which is what it is for; but where the channel
is busy that is every node, and what it holds goes late.

| a message every 5 min | dBm | unicast on time, % | per s | held | wait, s |
|---|---|---|---|---|---|
| town, no budget | 20 | 19.9 | 0.85 | - | - |
| town, own 1% | 20 | 4.9 | 0.36 | 18,774 | 77 |
| town, own 2% | 20 | 18.7 | 0.88 | 1,627 | 18 |
| town, own 4% | 20 | 20.4 | 0.84 | 0 | - |
| town, flooding / Meshtastic | 20 | 24.8 / 1.5 | 1.08 / 0.11 | - | - |
| fast, no budget | -5 / 0 / 20 | 24.4 / 21.0 / 27.0 | 16.1 / 26.4 / 5.1 | - | - |
| fast, own 1% | -5 / 0 / 20 | 24.9 / 21.4 / 27.0 | 15.6 / 26.4 / 5.1 | 0 / 19 / 63 | - / 11 / 10 |
| deployed, no budget | -5 / 0 / 20 | 3.7 / 3.3 / 2.1 | 0.73 / 0.59 / 0.64 | - | - |
| deployed, own 1% | -5 / 0 / 20 | 0.7 / 0.2 / 0.7 | 0.24 / 0.10 / 0.17 | 76,765 / 87,131 / 61,639 | 210 / 219 / 211 |
| deployed, own 2% | -5 / 0 / 20 | 3.0 / 2.6 / 1.8 | 0.70 / 0.56 / 0.56 | 30,699 / 35,347 / 12,074 | 56 / 56 / 59 |
| fast, Meshtastic / MeshCore | -5 / 0 / 20 | 6.9 / 12.5 / 1.3 and 10.5 / 14.9 / 8.8 | 4.77 / 6.14 / 0.88 and 2.08 / 2.95 / 2.64 | - | - |
| deployed, Meshtastic / MeshCore | -5 / 0 / 20 | 2.8 / 1.6 / 0.1 and 2.8 / 2.8 / 1.3 | 0.55 / 0.35 / 0.09 and 0.38 / 0.41 / 0.21 | - | - |

Every one of these networks is already full without a budget; what matters is that the budget
does not make a full network worse. At 1% it does, by a factor of four in the town and more on
the deployed preset. At 2% it does not in the town, and costs the deployed preset under a point where
next to nothing is delivered anyway. On the fast preset, where a frame is short, 1% holds
almost nothing even at this traffic.

The flood allowances inside the own part are not what binds: the town with `airtime.floods = no`
held as many frames (18,901) and delivered as little (4.8%).

## Writing without pause

The draft asks what the own part costs a mesh where some nodes write without pause. Here a twentieth
of the nodes each send a message every 10 seconds, 180 times the others' rate
(`traffic.chatty`), and the report's `quiet` counts the other nodes' messages apart.

| | dBm | the others' unicast on time, % | the others' broadcast, % | all unicast on time, % | per s | held | wait, s |
|---|---|---|---|---|---|---|---|
| fast, no budget | -5 / 0 / 20 | 16.7 / 14.1 / 18.6 | 12.1 / 22.2 / 3.0 | 14.2 / 12.1 / 14.9 | 17.7 / 25.6 / 6.7 | - | - |
| fast, own 1% | -5 / 0 / 20 | 35.9 / 40.3 / 69.1 | 19.9 / 42.2 / 44.6 | 3.5 / 3.9 / 6.6 | 4.0 / 7.8 / 11.8 | 15,199 / 19,041 / 20,194 | 69 / 68 / 52 |
| fast, own 2% | -5 / 0 / 20 | 25.1 / 25.4 / 52.3 | 14.9 / 33.9 / 25.4 | 4.0 / 2.7 / 5.0 | 4.9 / 6.4 / 5.7 | 29,273 / 37,421 / 39,695 | 30 / 30 / 16 |
| deployed, no budget | -5 / 0 / 20 | 8.1 / 10.4 / 3.5 | 7.9 / 9.5 / 0.5 | 1.6 / 1.7 / 1.7 | 1.87 / 1.83 / 1.98 | - | - |
| deployed, own 1% | -5 / 0 / 20 | 17.3 / 18.0 / 10.5 | 16.6 / 19.7 / 6.7 | 1.7 / 1.7 / 1.0 | 2.08 / 2.67 / 0.97 | 7,721 / 9,296 / 8,942 | 297 / 268 / 276 |
| deployed, own 2% | -5 / 0 / 20 | 15.6 / 16.5 / 10.0 | 12.9 / 14.5 / 2.6 | 1.5 / 1.6 / 1.0 | 1.45 / 1.72 / 0.37 | 7,739 / 8,229 / 7,992 | 201 / 190 / 162 |

The writers' messages are nine in ten of all, so the columns for every message, the headline among
them, are theirs, and fall. The other nodes' columns are what the own part is for: with it the
rest of the mesh delivers 1.7 to 3.7 times as many unicasts on time, because the writers can no
longer fill the channel. At 2% the writers take more of it back.

A writer's messages come faster than its own part lets them go. They wait in order and none is
dropped, so its backlog fills and stays full: every message it then sends has waited the length of
the backlog, minutes, and arrives late, and beyond the 32 waiting its new messages are refused.
The draft says a frame of the node's own waits and nothing in it drops it; for a node writing past
its part that means only stale messages go. A bound on how long a frame of the node's own may
wait, or on how many may, belongs in the specification with the companion protocol's reason 4.

## What it means for the parameters

- `BUDGET_RELAY` 8% binds only at the busiest relays and costs no delivery at any traffic
  measured, the busiest relay passing frames on for at most 5.6% of its hour at the comparison's
  traffic and 7.1% at six times it. It begins to cost at 4%. 7% drops a little more - the ten
  busiest 5.8% at -5 dBm on the deployed preset - and costs no delivery either.
- `BUDGET_OWN` 1% binds on retries, not only on what a user writes, and at a heavier traffic
  than the comparison's it makes a full network markedly worse. 2% does not; 4% holds next to
  nothing. Against nodes that write without pause 1% protects the rest of the mesh better than
  2% does (36 to 69% of their unicasts on time on the fast preset against 25 to 52%); 2% still
  delivers 1.5 to 2.9 times what no budget does for them.
- 2% own and 7% relayed keep a relay's whole budget at 9.6875% of its time, within the 10% three
  profiles allow. On the deployed preset they delivered more unicast on time than the draft's
  values and than no budget, at every power, with delay back to no budget's.

## Not done

- The core: the firmware's forwarder cannot drop a frame it was given to pass on (above).
- Churn and movement (`--quick` leaves them out), and the windows: `BUDGET_RELAY_WINDOW` and
  `BUDGET_OWN_WINDOW` were not swept.
- A field workload: the chatty nodes are a stand-in, a fixed share at one fixed rate.
- Profiles on 500 kHz at SF9 (US915 and its neighbours): every run here is at 125 kHz or
  62.5 kHz, where a frame is longer, so the own part binds sooner there than it would on them.
