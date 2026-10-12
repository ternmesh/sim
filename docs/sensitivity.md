# Sensitivity of the ranking to the uncalibrated PHY

[docs/recommendation.md](recommendation.md) chose candidate 3 on its lead in deliveries per second
of airtime, on a radio model whose capture, inter-SF rejection and lock time are placeholders
until the bench rig (MSH-32) measures them, and whose channel stands in for terrain it has never
seen. This page sweeps those parameters and records where the ranking holds and where it does not
(MSH-74). It also answers MSH-78: why the firmware's core trailed candidate 3 plain on the
deployed preset.

## The answer

Candidate 3 leads Meshtastic and MeshCore in deliveries per second of airtime at every point swept
on the fast preset, by 2.4 times or more, all deliveries or unicast alone. On the deployed preset
it leads everywhere but in four places:

- **Per-frame fading (`phy.fading`) of 4 dB or more, deployed preset.** Candidate 3 still has
  4.6 to 11 times the deliveries per second of airtime, most of them broadcast. At -5 and 0 dBm
  its unicast delivery falls from 22–23% to 13–15% at 4 dB and 10% at 6 dB, below MeshCore's
  15–19%. Its unicast deliveries per second of airtime tie MeshCore's at 4 dB (1.0–1.3 times)
  and fall to 0.7 times at 6 dB, at -5 and 0 dBm. The headline in the recommendation is
  unicast, so this flips that headline.
- **All the shadowing shared between links (`channel.share` 1), deployed, 20 dBm.** Meshtastic
  delivers 0.76 per second of airtime and candidate 3 0.63 (0.8 times), its routes reaching
  29% of pairs. Candidate 3 still delivers more unicast, 9% against 5–6%, at 1.5 times the
  unicast deliveries per second of airtime. At 0.75 it leads again (1.8 times).
- **A 12-hour warmup, deployed, 20 dBm.** Not a PHY parameter, but it flips the same cell:
  candidate 3's broadcast falls from 6% to 1% once its routes have settled, and its deliveries
  per second of airtime from 0.94 to 0.14, against MeshCore's 0.32. Its unicast stays ahead,
  10% against 6%, at 1.4 times the unicast deliveries per second of airtime.
- **Churn, deployed, 20 dBm.** With a quarter of the sites going down and coming back, candidate
  3's unicast deliveries per second of airtime are 0.9 times MeshCore's, and 0.8 times with all
  the shadowing shared. It leads in all deliveries.

The deployed preset at 20 dBm is the cell where every candidate is poorest. The recommendation's
deployed column says that preset is full whatever routes over it. The flips at 20 dBm are within
that.

Fading is the one that matters. On the deployed preset it flips unicast at -5 and 0 dBm too. On
the fast preset it shrinks candidate 3's lead from 7–9 times to 2.4–4.1 times at -5 and 0 dBm.
There, candidate 3 still does the most per second of airtime but delivers fewer unicasts than
MeshCore: 36% against 47% at -5 dBm with 4 dB, and 35% and 53% against 56% and 62% with 6 dB.
The bench rig can measure fading. **How much a link's loss varies from frame to frame is a
must-measure item for MSH-32**, beside capture and lock time. How much of the shadowing is
shared between links cannot be measured on a bench. A field pilot's link table (MSH-69) is
what would settle it.

| Parameter | Default | Swept | Fast preset | Deployed preset |
|---|---|---|---|---|
| `phy.capture` (preamble capture, dB) | 6 | 0, 3, `capture_anytime` | holds at 0 and `capture_anytime` | holds |
| `phy.co_sf` (co-SF SIR threshold, dB) | 6 | 0, 3, 12 | holds at 12, 4.1 times or more | holds; unicast lead 1.2 times at 12 dB, 20 dBm |
| `phy.lock_symbols` | 5 | 3, 8 | holds at 8 | holds |
| `channel.sigma` (shadowing, dB) | 7.8 | 4, 12 | holds at 12 | holds |
| `channel.share` | 0.5 | 0, 0.75, 1 | holds at 1 | holds to 0.75; at 1, 20 dBm, all deliveries 0.8 times Meshtastic's |
| `channel.decorrelation` (m) | 100 | 25, 400 | not swept | holds |
| `channel.exponent` | 2.32 | 2.0, 3.0, 3.5 | holds at 2.0 and 3.5 | holds |
| `phy.fading` (dB per frame) | 0 | 2, 4, 6 | per airtime holds, 2.4 times or more; unicast delivery below MeshCore's at -5 dBm (4 dB) and at -5 and 0 dBm (6 dB) | holds at 2; unicast ties at 4 and flips at 6 |
| `phy.isolation_offset` (dB), per-link SF | 0 | -6, +6 | moves nothing (SF9 region) | moves nothing |
| churn and walking, full runs | - | at the points in the full-run table | holds per airtime; at exponent 3.5, -5 dBm, MeshCore delivers more unicasts under churn | all deliveries hold; at 20 dBm, unicast per airtime 0.8–0.9 times MeshCore's under churn |
| `warmup` | 6 h | 12 h | not swept | holds at -5 and 0 dBm; at 20 dBm, all deliveries 0.4 times MeshCore's |

"Holds" means candidate 3 delivers more per second of airtime than the better of Meshtastic and
MeshCore at every power, counting all deliveries and counting unicast alone.

## How it was run

Each sweep is `tools/density.py` with one `-s` setting added to every candidate's scenario. It
runs Meshtastic (1), MeshCore (2) and candidate 3 (3) at -5, 0 and 20 dBm, 3 seeds each, as
density.py offers traffic: a message from every node every 30 minutes, a quarter broadcast.
`--fast` puts every candidate on SF7 at 125 kHz over the same 200 sites. `--deployed` puts each on
its own preset over the same 200 sites. `--quick` leaves out the churn and movement runs. Release
build, the core from the firmware commit CI pins (e300726):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTSIM_FIRMWARE_DIR=../firmware
cmake --build build
tools/density.py --tsim build/tsim --fast --quick --seeds 3 --power=-5,0,20 \
    --candidates 1,2,3 --json out.json -s channel.sigma=12
tools/density.py --tsim build/tsim --deployed --quick --seeds 3 --power=-5,0,20 \
    --candidates 1,2,3 --json out.json -s channel.sigma=12
```

The default rows are the same command without `-s`. They reproduce the fast preset's recorded
figures (`scenarios/scale/region-distvec-fast.tsim`, the current-defaults row) to the digit.

The isolation matrix had no setting of its own. `phy.co_sf` sets its diagonal, the SIR a frame
needs over the summed interference of its own SF. `phy.isolation_offset` moves every other entry
from the Goursaud and Gorce default; positive rejects less (docs/scenarios.md). Every run is
single-SF unless candidate 3 picks an SF per link (MSH-49), so the offset was swept with
`routing.sf_min=7`. On the region at SF9, against the incumbents at SF9 with every node a relay,
as MSH-49 measured it:

```bash
tools/density.py --tsim build/tsim --quick --seeds 3 --power=-5,0,20 --candidates 1,2
tools/density.py --tsim build/tsim --quick --seeds 3 --power=-5,0,20 --candidates 3 \
    -s routing.sf_min=7 -s routing.relay_pick=cds -s routing.relay_count=200 \
    -s phy.isolation_offset=6
tools/density.py --tsim build/tsim --deployed --quick --seeds 3 --power=-5,0,20 --candidates 3 \
    -s routing.sf_min=7 -s phy.isolation_offset=6
```

The full runs leave out `--quick` and use `--power=-5,20`. churn and move are the scorecard's:
a quarter of the 200 sites going down 30 minutes after every 2 hours up, and a quarter of the
leaves walking (tools/density.py).

In the tables each cell lists Meshtastic / MeshCore / candidate 3. The last column is candidate
3's deliveries per second of airtime over the better of the other two's: first all deliveries,
then unicast alone. Broadcast destinations are most of all deliveries (density.py's `bc_s`).

## Fast preset

| | dBm | unicast on time, % | broadcast on time, % | deliveries per s of airtime | c3 ÷ best other: all, unicast |
|---|---|---|---|---|---|
| default | -5 | 11 / 37 / 61 | 11 / 16 / 40 | 3.34 / 1.99 / 25.8 | 7.7, 8.6 |
| default | 0 | 31 / 53 / 91 | 29 / 24 / 67 | 4.89 / 2.86 / 34.8 | 7.1, 7.5 |
| default | 20 | 22 / 35 / 99 | 22 / 11 / 90 | 3.79 / 2.48 / 56.0 | 14.8, 7.7 |
| phy.capture 0 | -5 | 11 / 37 / 61 | 11 / 16 / 40 | 3.28 / 1.97 / 26.1 | 7.9, 8.8 |
| phy.capture 0 | 0 | 30 / 53 / 91 | 30 / 23 / 68 | 5.03 / 2.67 / 38.1 | 7.6, 8.2 |
| phy.capture 0 | 20 | 22 / 36 / 99 | 21 / 10 / 90 | 3.65 / 2.33 / 54.6 | 15.0, 7.5 |
| phy.capture_anytime yes | -5 | 11 / 38 / 61 | 11 / 17 / 40 | 3.33 / 2.10 / 26.8 | 8.1, 8.9 |
| phy.capture_anytime yes | 0 | 30 / 53 / 89 | 30 / 24 / 67 | 5.11 / 2.67 / 30.0 | 5.9, 6.9 |
| phy.capture_anytime yes | 20 | 35 / 35 / 99 | 36 / 15 / 95 | 6.21 / 2.04 / 75.8 | 12.2, 13.1 |
| phy.co_sf 12 | -5 | 6 / 22 / 52 | 7 / 8 / 22 | 2.40 / 2.07 / 12.8 | 5.3, 5.5 |
| phy.co_sf 12 | 0 | 18 / 33 / 62 | 17 / 12 / 39 | 4.09 / 2.73 / 18.7 | 4.6, 4.1 |
| phy.co_sf 12 | 20 | 20 / 27 / 88 | 21 / 9 / 64 | 4.85 / 2.86 / 31.3 | 6.4, 5.0 |
| phy.lock_symbols 8 | -5 | 11 / 37 / 61 | 11 / 17 / 40 | 3.31 / 2.04 / 25.8 | 7.8, 8.6 |
| phy.lock_symbols 8 | 0 | 29 / 52 / 91 | 29 / 24 / 67 | 4.96 / 2.83 / 35.0 | 7.0, 7.7 |
| phy.lock_symbols 8 | 20 | 22 / 36 / 98 | 22 / 12 / 90 | 3.82 / 2.63 / 54.8 | 14.4, 7.6 |
| channel.sigma 12 | -5 | 46 / 52 / 90 | 44 / 25 / 66 | 4.90 / 3.01 / 45.5 | 9.3, 9.9 |
| channel.sigma 12 | 0 | 53 / 47 / 97 | 52 / 21 / 79 | 5.62 / 2.73 / 47.7 | 8.5, 9.3 |
| channel.sigma 12 | 20 | 7 / 35 / 99 | 7 / 11 / 91 | 0.99 / 2.38 / 56.2 | 23.6, 8.2 |
| channel.share 1 | -5 | 10 / 27 / 55 | 11 / 11 / 19 | 3.62 / 2.08 / 19.6 | 5.4, 10.8 |
| channel.share 1 | 0 | 27 / 40 / 79 | 26 / 19 / 37 | 5.00 / 3.28 / 28.2 | 5.7, 8.6 |
| channel.share 1 | 20 | 27 / 32 / 96 | 27 / 10 / 68 | 6.01 / 2.48 / 44.6 | 7.4, 7.6 |
| channel.exponent 2.0 | -5 | 15 / 36 / 62 | 15 / 15 / 45 | 3.84 / 2.05 / 29.7 | 7.8, 8.5 |
| channel.exponent 2.0 | 0 | 33 / 49 / 91 | 33 / 21 / 65 | 4.67 / 2.86 / 32.4 | 6.9, 6.8 |
| channel.exponent 2.0 | 20 | 7 / 32 / 99 | 7 / 9 / 88 | 1.21 / 2.30 / 54.2 | 23.6, 7.6 |
| channel.exponent 3.5 | -5 | 5 / 30 / 43 | 5 / 14 / 16 | 1.38 / 1.60 / 11.6 | 7.2, 9.6 |
| channel.exponent 3.5 | 0 | 15 / 62 / 93 | 15 / 28 / 52 | 4.73 / 2.47 / 25.8 | 5.5, 8.3 |
| channel.exponent 3.5 | 20 | 60 / 49 / 96 | 58 / 18 / 92 | 11.4 / 2.44 / 46.0 | 4.0, 4.1 |
| phy.fading 4 | -5 | 20 / 47 / 36 | 20 / 20 / 25 | 4.45 / 2.18 / 11.2 | 2.5, 3.1 |
| phy.fading 4 | 0 | 39 / 59 / 59 | 39 / 26 / 51 | 5.16 / 2.77 / 19.4 | 3.8, 3.5 |
| phy.fading 4 | 20 | 11 / 37 / 97 | 11 / 11 / 89 | 1.67 / 2.34 / 44.9 | 19.2, 6.3 |
| phy.fading 6 | -5 | 32 / 56 / 35 | 32 / 23 / 37 | 4.79 / 2.24 / 13.9 | 2.9, 2.4 |
| phy.fading 6 | 0 | 48 / 62 / 53 | 47 / 27 / 62 | 5.05 / 2.67 / 20.9 | 4.1, 2.9 |

## Deployed preset

| | dBm | unicast on time, % | broadcast on time, % | deliveries per s of airtime | c3 ÷ best other: all, unicast |
|---|---|---|---|---|---|
| default | -5 | 11 / 14 / 22 | 10 / 5 / 21 | 0.65 / 0.43 / 2.91 | 4.5, 2.6 |
| default | 0 | 12 / 13 / 23 | 12 / 4 / 23 | 0.78 / 0.44 / 3.41 | 4.4, 2.7 |
| default | 20 | 1 / 6 / 12 | 1 / 2 / 6 | 0.18 / 0.34 / 0.94 | 2.8, 1.8 |
| phy.capture 0 | -5 | 11 / 14 / 22 | 11 / 5 / 22 | 0.66 / 0.43 / 3.02 | 4.6, 2.6 |
| phy.capture 0 | 0 | 13 / 13 / 24 | 11 / 5 / 23 | 0.76 / 0.48 / 3.41 | 4.5, 2.5 |
| phy.capture 0 | 20 | 1 / 7 / 12 | 1 / 2 / 6 | 0.17 / 0.42 / 0.97 | 2.3, 1.7 |
| phy.capture 3 | -5 | 11 / 14 / 22 | 11 / 5 / 21 | 0.66 / 0.44 / 2.89 | 4.4, 2.6 |
| phy.capture 3 | 0 | 13 / 13 / 25 | 12 / 4 / 23 | 0.78 / 0.46 / 3.35 | 4.3, 2.8 |
| phy.capture 3 | 20 | 1 / 6 / 11 | 1 / 2 / 6 | 0.18 / 0.36 / 1.01 | 2.8, 1.8 |
| phy.capture_anytime yes | -5 | 11 / 14 / 24 | 11 / 6 / 23 | 0.69 / 0.45 / 3.12 | 4.5, 2.8 |
| phy.capture_anytime yes | 0 | 14 / 12 / 26 | 13 / 5 / 28 | 0.85 / 0.43 / 3.99 | 4.7, 3.7 |
| phy.capture_anytime yes | 20 | 2 / 8 / 20 | 2 / 4 / 8 | 0.33 / 0.38 / 1.04 | 2.8, 3.5 |
| phy.co_sf 0 | -5 | 21 / 24 / 43 | 20 / 9 / 45 | 0.88 / 0.45 / 4.61 | 5.2, 3.7 |
| phy.co_sf 0 | 0 | 20 / 19 / 47 | 19 / 8 / 50 | 0.91 / 0.53 / 5.82 | 6.4, 4.4 |
| phy.co_sf 0 | 20 | 2 / 9 / 16 | 3 / 4 / 18 | 0.35 / 0.39 / 2.22 | 5.7, 2.0 |
| phy.co_sf 3 | -5 | 15 / 20 / 33 | 15 / 7 / 34 | 0.77 / 0.45 / 3.97 | 5.2, 3.1 |
| phy.co_sf 3 | 0 | 15 / 16 / 35 | 16 / 7 / 35 | 0.86 / 0.56 / 4.52 | 5.3, 3.4 |
| phy.co_sf 3 | 20 | 2 / 8 / 15 | 2 / 3 / 9 | 0.25 / 0.41 / 1.29 | 3.1, 2.0 |
| phy.co_sf 12 | -5 | 4 / 6 / 10 | 3 / 2 / 5 | 0.30 / 0.34 / 0.97 | 2.8, 1.9 |
| phy.co_sf 12 | 0 | 6 / 6 / 10 | 6 / 2 / 7 | 0.57 / 0.39 / 1.34 | 2.3, 1.8 |
| phy.co_sf 12 | 20 | 1 / 4 / 6 | 0 / 1 / 8 | 0.08 / 0.29 / 1.59 | 5.6, 1.2 |
| phy.lock_symbols 3 | -5 | 11 / 15 / 23 | 11 / 5 / 20 | 0.66 / 0.42 / 2.86 | 4.3, 2.5 |
| phy.lock_symbols 3 | 0 | 12 / 12 / 24 | 12 / 5 / 23 | 0.79 / 0.50 / 3.36 | 4.3, 2.8 |
| phy.lock_symbols 3 | 20 | 1 / 6 / 12 | 1 / 2 / 5 | 0.17 / 0.40 / 0.91 | 2.3, 1.9 |
| phy.lock_symbols 8 | -5 | 11 / 14 / 24 | 11 / 5 / 19 | 0.66 / 0.42 / 2.72 | 4.1, 2.7 |
| phy.lock_symbols 8 | 0 | 12 / 13 / 24 | 12 / 4 / 23 | 0.77 / 0.45 / 3.30 | 4.3, 2.6 |
| phy.lock_symbols 8 | 20 | 1 / 7 / 12 | 1 / 2 / 6 | 0.20 / 0.34 / 1.06 | 3.1, 1.7 |
| channel.sigma 4 | -5 | 2 / 5 / 10 | 3 / 2 / 3 | 0.15 / 0.24 / 0.59 | 2.5, 3.4 |
| channel.sigma 4 | 0 | 7 / 10 / 18 | 7 / 4 / 10 | 0.59 / 0.44 / 1.51 | 2.6, 2.6 |
| channel.sigma 4 | 20 | 1 / 6 / 11 | 1 / 2 / 9 | 0.25 / 0.36 / 1.70 | 4.8, 1.8 |
| channel.sigma 12 | -5 | 11 / 15 / 28 | 11 / 5 / 28 | 0.60 / 0.53 / 3.77 | 6.2, 2.6 |
| channel.sigma 12 | 0 | 7 / 12 / 23 | 6 / 5 / 30 | 0.36 / 0.55 / 4.01 | 7.2, 2.2 |
| channel.sigma 12 | 20 | 1 / 7 / 13 | 1 / 2 / 5 | 0.14 / 0.37 / 0.73 | 2.0, 1.8 |
| channel.share 0 | -5 | 13 / 19 / 31 | 12 / 7 / 17 | 0.65 / 0.45 / 1.67 | 2.6, 2.5 |
| channel.share 0 | 0 | 16 / 17 / 33 | 16 / 7 / 38 | 0.99 / 0.47 / 4.17 | 4.2, 2.9 |
| channel.share 0 | 20 | 0 / 6 / 10 | 0 / 2 / 8 | 0.00 / 0.35 / 1.34 | 3.8, 1.6 |
| channel.share 0.75 | -5 | 10 / 11 / 19 | 9 / 4 / 14 | 0.65 / 0.44 / 2.30 | 3.5, 2.8 |
| channel.share 0.75 | 0 | 10 / 10 / 19 | 9 / 4 / 17 | 0.67 / 0.45 / 2.71 | 4.1, 2.5 |
| channel.share 0.75 | 20 | 3 / 6 / 11 | 3 / 2 / 4 | 0.43 / 0.36 / 0.76 | 1.8, 1.7 |
| channel.share 1 | -5 | 8 / 9 / 14 | 8 / 4 / 8 | 0.68 / 0.45 / 1.55 | 2.3, 2.4 |
| channel.share 1 | 0 | 8 / 8 / 14 | 8 / 3 / 10 | 0.69 / 0.43 / 1.85 | 2.7, 2.3 |
| channel.share 1 | 20 | 5 / 6 / 9 | 5 / 2 / 3 | 0.76 / 0.39 / 0.63 | 0.8, 1.5 |
| channel.decorrelation 25 | -5 | 11 / 14 / 22 | 11 / 5 / 21 | 0.65 / 0.45 / 2.88 | 4.4, 2.3 |
| channel.decorrelation 25 | 0 | 12 / 13 / 25 | 12 / 5 / 23 | 0.79 / 0.54 / 3.30 | 4.2, 2.8 |
| channel.decorrelation 25 | 20 | 1 / 6 / 12 | 1 / 2 / 7 | 0.16 / 0.37 / 1.08 | 2.9, 1.8 |
| channel.decorrelation 400 | -5 | 11 / 14 / 22 | 11 / 5 / 20 | 0.67 / 0.40 / 2.77 | 4.1, 2.6 |
| channel.decorrelation 400 | 0 | 12 / 13 / 23 | 12 / 5 / 23 | 0.79 / 0.47 / 3.30 | 4.2, 2.6 |
| channel.decorrelation 400 | 20 | 1 / 6 / 12 | 1 / 2 / 7 | 0.18 / 0.40 / 1.11 | 2.8, 1.7 |
| channel.exponent 2.0 | -5 | 10 / 13 / 21 | 10 / 5 / 17 | 0.65 / 0.47 / 2.57 | 3.9, 2.5 |
| channel.exponent 2.0 | 0 | 9 / 11 / 20 | 9 / 4 / 19 | 0.63 / 0.53 / 2.96 | 4.7, 2.2 |
| channel.exponent 2.0 | 20 | 1 / 5 / 9 | 1 / 2 / 10 | 0.14 / 0.31 / 1.75 | 5.6, 1.5 |
| channel.exponent 3.0 | -5 | 12 / 17 / 26 | 12 / 7 / 19 | 0.70 / 0.45 / 2.28 | 3.2, 2.6 |
| channel.exponent 3.0 | 0 | 16 / 16 / 29 | 15 / 6 / 31 | 0.87 / 0.48 / 3.89 | 4.5, 3.0 |
| channel.exponent 3.0 | 20 | 3 / 9 / 18 | 3 / 3 / 10 | 0.33 / 0.50 / 1.45 | 2.9, 2.0 |
| channel.exponent 3.5 | -5 | 12 / 19 / 21 | 11 / 7 / 12 | 0.71 / 0.44 / 1.31 | 1.8, 2.1 |
| channel.exponent 3.5 | 0 | 15 / 18 / 30 | 15 / 8 / 31 | 0.89 / 0.49 / 3.47 | 3.9, 2.8 |
| channel.exponent 3.5 | 20 | 4 / 11 / 23 | 4 / 4 / 15 | 0.37 / 0.53 / 2.13 | 4.1, 2.3 |
| phy.fading 2 | -5 | 12 / 15 / 22 | 11 / 5 / 19 | 0.67 / 0.42 / 2.54 | 3.8, 2.3 |
| phy.fading 2 | 0 | 12 / 13 / 21 | 12 / 5 / 26 | 0.79 / 0.52 / 3.57 | 4.5, 2.2 |
| phy.fading 2 | 20 | 1 / 6 / 10 | 1 / 2 / 14 | 0.16 / 0.35 / 2.26 | 6.4, 1.6 |
| phy.fading 4 | -5 | 13 / 17 / 15 | 13 / 5 / 29 | 0.73 / 0.44 / 3.38 | 4.6, 1.3 |
| phy.fading 4 | 0 | 13 / 15 / 13 | 12 / 5 / 38 | 0.76 / 0.51 / 4.50 | 5.9, 1.0 |
| phy.fading 4 | 20 | 1 / 7 / 9 | 1 / 2 / 22 | 0.09 / 0.39 / 3.49 | 9.0, 1.2 |
| phy.fading 6 | -5 | 16 / 19 / 10 | 15 / 6 / 41 | 0.80 / 0.48 / 4.29 | 5.3, 0.7 |
| phy.fading 6 | 0 | 12 / 16 / 10 | 11 / 5 / 43 | 0.64 / 0.47 / 4.86 | 7.5, 0.7 |
| phy.fading 6 | 20 | 0 / 7 / 7 | 0 / 2 / 26 | 0.05 / 0.37 / 4.09 | 11.1, 1.0 |
| warmup 12 h | -5 | 11 / 14 / 23 | 11 / 5 / 20 | 0.68 / 0.43 / 2.80 | 4.1, 2.7 |
| warmup 12 h | 0 | 12 / 13 / 24 | 12 / 5 / 22 | 0.78 / 0.49 / 3.15 | 4.0, 2.7 |
| warmup 12 h | 20 | 1 / 6 / 10 | 1 / 2 / 1 | 0.21 / 0.32 / 0.14 | 0.4, 1.4 |

`channel.share` 1 at 20 dBm, per seed: Meshtastic 0.80, 0.69 and 0.78 deliveries per second of
airtime, candidate 3 0.68, 0.64 and 0.59. Candidate 3's routes reached 28–30% of pairs as the
warmup ended, against 35–43% at the default share.

With fading, candidate 3's broadcast rises (21% to 29–41% at -5 dBm) as its unicast falls. Why
its unicast falls is not explained here. It sends each frame at the power its neighbour needs,
with a 10 dB margin (`routing.margin`), over links it judges by strength.

## Inter-SF isolation, with per-link SF

| | dBm | unicast on time, % | broadcast on time, % | deliveries per s of airtime | c3 ÷ best other: all, unicast |
|---|---|---|---|---|---|
| SF9, offset -6 | -5 | 19 / 15 / 56 | 18 / 6 / 38 | 1.76 / 0.16 / 6.41 | 3.6, 5.0 |
| SF9, offset -6 | 0 | 23 / 14 / 88 | 22 / 5 / 62 | 1.90 / 0.17 / 9.71 | 5.1, 6.9 |
| SF9, offset -6 | 20 | 5 / 6 / 94 | 4 / 2 / 91 | 0.69 / 0.14 / 10.4 | 15.1, 14.4 |
| SF9, offset 0 | -5 | 19 / 15 / 57 | 18 / 6 / 38 | 1.76 / 0.16 / 6.44 | 3.6, 5.1 |
| SF9, offset 0 | 0 | 23 / 14 / 89 | 22 / 5 / 61 | 1.90 / 0.17 / 9.68 | 5.1, 7.1 |
| SF9, offset 0 | 20 | 5 / 6 / 94 | 4 / 2 / 91 | 0.69 / 0.14 / 10.2 | 14.8, 14.2 |
| SF9, offset +6 | -5 | 19 / 15 / 56 | 18 / 6 / 40 | 1.76 / 0.16 / 6.49 | 3.7, 4.9 |
| SF9, offset +6 | 0 | 23 / 14 / 89 | 22 / 5 / 61 | 1.90 / 0.17 / 9.93 | 5.2, 7.3 |
| SF9, offset +6 | 20 | 5 / 6 / 93 | 4 / 2 / 90 | 0.69 / 0.14 / 10.3 | 14.9, 14.4 |
| deployed, offset -6 | -5 | 11 / 14 / 40 | 10 / 5 / 42 | 0.65 / 0.43 / 3.81 | 5.9, 3.0 |
| deployed, offset -6 | 0 | 12 / 13 / 56 | 12 / 4 / 47 | 0.78 / 0.44 / 4.72 | 6.1, 4.4 |
| deployed, offset -6 | 20 | 1 / 6 / 31 | 1 / 2 / 42 | 0.18 / 0.34 / 3.39 | 10.0, 2.3 |
| deployed, offset 0 | -5 | 11 / 14 / 40 | 10 / 5 / 41 | 0.65 / 0.43 / 3.75 | 5.8, 3.0 |
| deployed, offset 0 | 0 | 12 / 13 / 56 | 12 / 4 / 46 | 0.78 / 0.44 / 4.67 | 6.0, 4.4 |
| deployed, offset 0 | 20 | 1 / 6 / 31 | 1 / 2 / 39 | 0.18 / 0.34 / 3.22 | 9.5, 2.3 |
| deployed, offset +6 | -5 | 11 / 14 / 40 | 10 / 5 / 41 | 0.65 / 0.43 / 3.75 | 5.8, 3.0 |
| deployed, offset +6 | 0 | 12 / 13 / 55 | 12 / 4 / 46 | 0.78 / 0.44 / 4.75 | 6.1, 4.4 |
| deployed, offset +6 | 20 | 1 / 6 / 30 | 1 / 2 / 40 | 0.18 / 0.34 / 3.38 | 9.9, 2.3 |

Moving every inter-SF entry 6 dB either way moves nothing by more than a point or 0.3 deliveries
per second of airtime. As an aside, per-link SF on the deployed preset (SF8 at 62.5 kHz, `sf_min` 7)
delivers 31–56% of unicasts against 12–23% at SF8 alone.

## Full runs, with churn and movement

| | dBm | unicast on time, % | under churn, % | walking, % | broadcast on time, % | deliveries per s of airtime | c3 ÷ best other: all, unicast |
|---|---|---|---|---|---|---|---|
| fast, channel.exponent 3.5 | -5 | 5 / 30 / 43 | 4 / 25 / 18 | 5 / 31 / 33 | 5 / 14 / 16 | 1.38 / 1.60 / 11.6 | 7.2, 9.6 |
| fast, channel.exponent 3.5 | 20 | 60 / 49 / 96 | 58 / 50 / 76 | 58 / 48 / 91 | 58 / 18 / 92 | 11.4 / 2.44 / 46.0 | 4.0, 4.1 |
| deployed, default | -5 | 11 / 14 / 22 | 11 / 14 / 17 | 10 / 14 / 15 | 10 / 5 / 21 | 0.65 / 0.43 / 2.91 | 4.5, 2.6 |
| deployed, default | 20 | 1 / 6 / 12 | 1 / 7 / 7 | 1 / 6 / 8 | 1 / 2 / 6 | 0.18 / 0.34 / 0.94 | 2.8, 1.8 |
| deployed, channel.share 1 | -5 | 8 / 9 / 14 | 8 / 9 / 11 | 8 / 9 / 10 | 8 / 4 / 8 | 0.68 / 0.45 / 1.55 | 2.3, 2.4 |
| deployed, channel.share 1 | 20 | 5 / 6 / 9 | 4 / 6 / 5 | 3 / 6 / 6 | 5 / 2 / 3 | 0.76 / 0.39 / 0.63 | 0.8, 1.5 |

The churn and walking columns are unicast on time. In those runs candidate 3 keeps its lead in all
deliveries per second of airtime, at 2.2 times the better incumbent's or more. In unicast alone it
does not everywhere. On the deployed preset at 20 dBm, its unicast deliveries per second of
airtime are 0.9 times MeshCore's under churn and 1.1 times walking, and 0.8 and 0.9 times with
all the shadowing shared. On the fast preset with a path-loss exponent of 3.5, at -5 dBm,
MeshCore delivers more unicasts under churn, 25% against 18%. Candidate 3 still does 3.3 times
its unicast deliveries per second of airtime there. Churn is the weak side the recommendation
already records: a route outlives the relay it leads to until announces correct it.

## The core on the deployed preset (MSH-78)

`tsim/core.h` has the core at 14.4% of unicasts on time on the deployed preset against candidate
3 plain's 19.1%, with no broadcasts. Plain is candidate 3 without re-attachment or rescue floods,
which the core has neither of. Re-measured with the firmware commit CI pins now (signed announces,
firmware#75), the core is further behind. Deployed preset, 20 dBm, 3 seeds, unicast on time,
the routes that arrive as the warmup ends, and unicast deliveries per thousand seconds of
airtime:

| | warmup 6 h | 9 h | 12 h | 18 h |
|---|---|---|---|---|
| candidate 3 plain | 19.1%  60%  15.7 | 10.7%  85%  7.0 | 8.3%  94%  5.2 | 10.0%  95%  6.2 |
| core, unbounded tables | 11.0%  31%  8.3 | 18.0%  66%  12.5 | 18.5%  89%  12.5 | 23.5%  97%  16.5 |

At 6 hours, the core with a board's tables delivers 11.1% (31%, 8.4), and candidate 3 as it is
9.2% (40%, 6.0), as recorded.

**The gap is a snapshot of two networks still settling, not a steady state.** When the 6-hour
warmup ends, the core holds routes for half as many pairs as candidate 3 plain. Its signed
announces cost more air under the same 0.5% cap: in seed 1's measured hour, 9.1 ks of announces
against 5.9 ks. For the share of pairs routed, the two deliver alike: unicast on time over that
share is 0.36 and 0.32.

Left longer, the core's routes fill in and it overtakes, 18–24% from 9 hours on. Candidate 3
plain gets worse as its routes fill in. With routes for 94% of pairs (seed 1, 12 h) it sends
26,154 data frames against the core's 15,887, and 23,658 of its hops find the next hop busy,
against 13,868. It delivers 171 messages on time against the core's 368, on a channel already
full. At 6 hours, the messages it had no route for were dropped at the source and cost no airtime.

What was ruled out, each 3 seeds at 6 hours:

- **The MAC.** The core over candidate 3's MAC (`-s mac=meshcore`) delivers 8.0%, less than over
  its own. Candidate 3 plain over the core's (`-s mac=listen`) delivers 26.1% at 6 hours and
  14.8% at 12, more than over its own.
- **The announce cap alone.** The core with `routing.announce_ppm` 6000 and 7500, where 3750 is
  the firmware's, holds routes for 45% and 54% of pairs and delivers 13.9% and 13.4%. More
  announces reach more pairs, and likely take air from the messages.
- **The board's tables.** 11.1% with them, 11.0% without.

So on the deployed preset the core, once settled, delivers 1.7 to 2.4 times what candidate 3
plain does. The likely reason is how its forwarder paces: one frame in the queue at a time, and a
pause of JITTER + 1 airtimes after each (`tsim/core.h`). That was not isolated here. The
firmware is not changed.

The commands, deployed preset, 20 dBm (`-s 'warmup=12 h'` and the like for the other columns):

```bash
tools/density.py --tsim build/tsim --deployed --quick --seeds 3 --power 20 \
    -s traffic.broadcast=0 --candidates 3 -s routing.reattach=no -s routing.rescue_hops=0
tools/density.py --tsim build/tsim --deployed --quick --seeds 3 --power 20 \
    -s traffic.broadcast=0 --candidates core -s routing.neighbours=255 \
    -s routing.destinations=0 -s routing.frames=16 -s routing.flood_frames=16
```

The seed-1 frame and hop counts are from `tsim` run directly on `region-distvec-deployed.tsim`
and `region-core-deployed.tsim` with the same settings, and the core's traffic as density.py
gives it (`EXTRA` in tools/density.py).

## Not done

- The fast preset was not swept at `phy.capture` 3, `phy.co_sf` 0 and 3, `phy.lock_symbols` 3,
  `channel.sigma` 4, `channel.exponent` 3.0, `channel.share` 0 and 0.75, or
  `channel.decorrelation`. The deployed preset was, and none of those moved its ranking.
- Full runs with churn and movement were made only at the points in the table above, at -5 and
  20 dBm. None was made with fading, and the fast preset's default was not run again here: its
  full runs are recorded in `scenarios/scale/region-distvec-fast.tsim`.
- Per-frame fading was swept on the deployed preset at 2, 4 and 6 dB, and on the fast preset
  at 4 dB, and at 6 dB at -5 and 0 dBm only.
- `phy.retune` was not swept: no run here retunes but per-link SF.
- Why fading costs candidate 3 its unicast on the deployed preset, and why candidate 3 plain
  delivers less once its routes settle there, are not explained.
