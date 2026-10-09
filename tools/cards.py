#!/usr/bin/env python3
"""Measures what presence cards cost, on the firmware's core over the 1000-node region.

    cards.py --tsim PATH [--seeds N] [--power DBM,...] [--jobs N] [--json PATH]

A presence card is a broadcast a node sends every so often to say who it is, so that people
nearby can find each other without a public channel. This runs scenarios/scale/region-core-cards.tsim
with the region's message load and, on top of it, each of:

  none            the load alone
  signed 1h       every node, a 123-byte signed card an hour, flooded as far as a group's frame
  unsigned 1h     the same at 59 bytes: an address and a name, no signature
  signed 20m      three an hour
  signed 1h, 2h   the signed card, starting with 2 hops (routing.card_hops) instead of 5
  signed 1h, 1h   and with 1
  signed 1h, 25%  a quarter of the nodes sending, the rest only listening
  signed 2h, ...  one every two hours, starting with 2 hops, and with 1
  1h, 2 hops, 25% a quarter of the nodes, hourly, 2 hops
  room 2h         a public room instead: a 67-byte line from every node every 2 hours
  room 15m        and every 15 minutes

and prints, for each and each power, the mean over the seeds of:

  uni      unicasts delivered on time, as a share of those sent
  group    group broadcast destinations reached on time, as a share of those wanted
  seen/h   cards (or room lines) each node received on time in the hour, on average
  air      seconds of airtime spent in the measured hour, every node
  duty     the busiest node's share of the hour spent sending

Power stands in for density, as in density.py. Each run takes 30 to 90 s of CPU in a release build.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIO = os.path.join(HERE, "..", "scenarios", "scale", "region-core-cards.tsim")

ARMS = [
    ("none", ["cards.interval=none"]),
    ("signed 1h", []),
    ("unsigned 1h", ["cards.len=32"]),
    ("signed 20m", ["cards.interval=20 min"]),
    ("signed 1h, 2 hops", ["routing.card_hops=2"]),
    ("signed 1h, 1 hop", ["routing.card_hops=1"]),
    ("signed 1h, 25%", ["cards.share=0.25"]),
    ("signed 2h, 2 hops", ["cards.interval=2 h", "routing.card_hops=2"]),
    ("signed 2h, 1 hop", ["cards.interval=2 h", "routing.card_hops=1"]),
    ("1h, 2 hops, 25%", ["routing.card_hops=2", "cards.share=0.25"]),
    ("room 2h", ["cards.interval=2 h", "cards.len=40"]),
    ("room 15m", ["cards.interval=15 min", "cards.len=40"]),
]


def run(tsim, arm, dbm, seed):
    args = [tsim, "-s", f"seed={seed}", "-s", f"radio.tx_dbm={dbm}"]
    for s in dict(ARMS)[arm]:
        args += ["-s", s]
    out = subprocess.run(args + [SCENARIO], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def share(d, key):
    return d[key]["on_time"] / d[key]["wanted"] if d[key]["wanted"] else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tsim", required=True)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--power", default="20,5")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--json")
    a = ap.parse_args()
    powers = [int(p) for p in a.power.split(",")]
    jobs = [(arm, p, s) for p in powers for arm, _ in ARMS for s in range(1, a.seeds + 1)]
    with ThreadPoolExecutor(a.jobs) as ex:
        reports = list(ex.map(lambda j: run(a.tsim, *j), jobs))
    rows = []
    for p in powers:
        for arm, _ in ARMS:
            rs = [r for (aa, pp, _), r in zip(jobs, reports) if aa == arm and pp == p]
            rows.append({
                "arm": arm, "dbm": p,
                "links": statistics.mean(r["links"]["degree_mean"] for r in rs),
                "uni": statistics.mean(share(r, "unicast") for r in rs),
                "group": statistics.mean(share(r, "broadcast") for r in rs),
                "seen_h": statistics.mean(r["card"]["on_time"] / r["nodes"] if "card" in r else 0
                                          for r in rs),
                "air": statistics.mean(r["airtime_s"]["total"] for r in rs),
                "duty": statistics.mean(r["duty_max"] for r in rs),
            })
    print(f"{'':18} {'dBm':>4} {'links':>6} {'uni':>6} {'group':>6} {'seen/h':>7} {'air':>7} {'duty':>6}")
    for r in rows:
        print(f"{r['arm']:18} {r['dbm']:>4} {r['links']:>6.0f} {r['uni']:>6.1%} {r['group']:>6.1%} "
              f"{r['seen_h']:>7.1f} {r['air']:>7.0f} {r['duty']:>6.1%}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
