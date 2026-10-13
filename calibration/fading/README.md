# Frame-to-frame fading

How much a link's loss varies from one frame to the next: the simulator's `phy.fading`. It is
the one PHY parameter that changes which routing comes out ahead ([docs/sensitivity.md](../../docs/sensitivity.md)).
On the deployed preset, candidate 3's unicast deliveries per second of airtime are 2.6 times
MeshCore's with none, tie with it at 4 dB and fall to 0.7 times at 6 dB. Every run so far has
used 0, a guess. This measures it (MSH-32, MSH-76).

## What it measures

A link's RSSI moves for two reasons: a change from frame to frame, which the simulator draws
afresh for every frame at every receiver, and slower drift (people moving, doors, the weather),
which it does not. They are told apart by time. For frames `dt` apart,

    sigma(dt) = sd(RSSI(t + dt) - RSSI(t)) / sqrt(2)

is the per-frame fading alone while nothing drifts within `dt`, and grows with `dt` as drift
adds to it. `fading.py` gives it for the next frame and for 1 minute, 10 minutes, 1 hour and 6
hours. The next frame's is the value for `phy.fading`. The rest of the row shows how much of the
variation is slower than that. A route is chosen from an announce and used minutes later, so the
minutes column says how stale a link's figure gets.

## Running it

Two boards on any firmware with the bench commands (0.3.0 and later), each on a USB port of one
computer, and pyserial:

```bash
run.py /dev/ttyUSB0 /dev/ttyUSB1 --power -9 --sf 8 --bw 62500 --hours 24 --header > house.csv
fading.py analyse house.csv
```

* **The boards take turns.** Each sends one test frame per `--gap` (10 s by default), and the
  other reports its RSSI and SNR. Each frame is one row, written as it is measured, so a run can
  be stopped at any time, and runs can be appended to one file (leave out `--header`).
* **Put them in different rooms**, and choose `--power` so the mean RSSI is well below -50 dBm,
  -80 to -110 is good. Closer or louder clips the receiver and reads too little fading.
  Farther or quieter loses frames, and because the weak frames are the ones lost, it also reads
  too little. `fading.py` warns of both.
* **`--sf 8 --bw 62500` is the deployed preset**, where fading changes the ranking. A wider
  channel averages over more of the multipath and fades less, so measure on the preset the
  figure is for. With neither option, the region's profile is used.
* **The boards do not route while it runs.** Bench mode turns routing and first contact off.
  The script turns it back on when it ends, by Ctrl-C or otherwise. After `--power`, `--sf` or
  `--bw`, reset the boards to go back to their own settings.
* A day gives about 8600 frames each way at the default gap, enough for the 6-hour column.
  At SF8 and 62.5 kHz a test frame is 0.26 s on the air, so each board sends for 2.6% of its
  time at the default gap. Where the region limits that (EU868's 1% sub-bands), use `--gap 30`
  or more.

`fading.py check` tests the estimators against made-up links whose fading is known. CI runs it.

## What it can say, and what it cannot

| Next-frame sigma | What the sensitivity sweeps found on the deployed preset |
|---|---|
| 2 dB or less | candidate 3's lead holds |
| about 4 dB | unicast per second of airtime ties MeshCore's |
| 6 dB or more | MeshCore delivers more unicast per second of airtime |

* **Two static boards indoors are one kind of link.** Outdoors at range, with a node moving,
  fading is likely larger. This bounds the figure from one side. A walking test needs a laptop
  carried with one of the boards, and the field pilot (MSH-69) is the fuller answer.
* **Each direction is reported apart.** The two should agree. If they do not, one board's
  receiver or antenna differs.
* **RSSI is in whole dB.** A twelfth of a dB squared is taken off each variance for it.

## Results

None yet.
