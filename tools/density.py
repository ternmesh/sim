#!/usr/bin/env python3
"""Runs every candidate on the 1000-node region at several densities.

    density.py --tsim PATH [--seeds N] [--power DBM,...] [--jobs N] [-s key=value]...

A routing result means little without the density it was measured at: the same protocol ranks
differently where every node hears a dozen others and where it hears hundreds. This runs each of
scenarios/scale/region*.tsim - flooding, and candidates 1, 2 and 3 - at each transmit power and
seed, and prints for each candidate and power the mean over the seeds, and its standard deviation,
of:

  links    the links per node tsim reports: pairs that decode each other at the floor with
           nothing else on the air, which is the most any routing can use
  unicast  unicasts delivered on time, as a share of those sent
  bcast    broadcast destinations reached on time, as a share of those wanted
  per_s    on-time deliveries, unicast and broadcast, per second of airtime

Power stands in for density. Under the log-distance channel, sending Δ dB quieter loses as much as
standing 10^(Δ/10n) times further apart (n = 2.32 by default, so 5 dB is 1.64 times), and keeps
the map, the seeds and the shadowing field as they are; spreading the same nodes over a larger area
would change how far apart the field's cells are. On the default map the region's 20 dBm gives
about 470 links per node, and -5 dBm about 12.

Every scenario's settings can be overridden with -s, as with tsim: `-s traffic.peers=3 -s
traffic.reply=0.5` gives each node three regular correspondents who answer half its messages, which
is what lets a routed candidate use a route more than once.

The sweep is too long for CI, which runs each region scenario once, at 20 dBm. It wants a release
build: each run takes 5 to 60 s of CPU there, and the runs go in parallel.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, "..", "scenarios", "scale")

CANDIDATES = [
    ("flood", "region.tsim"),
    ("1 meshtastic", "region-meshtastic.tsim"),
    ("2 meshcore", "region-meshcore.tsim"),
    ("3 distvec", "region-distvec.tsim"),
]
METRICS = ["links", "unicast", "bcast", "per_s"]


def run(tsim, scenario, power, seed, sets):
    args = [tsim, "-s", "seed=%d" % seed, "-s", "radio.tx_dbm=%g" % power]
    for s in sets:
        args += ["-s", s]
    out = subprocess.run(args + [os.path.join(DIR, scenario)], capture_output=True, text=True,
                         check=True).stdout
    r = json.loads(out)
    u, b = r["unicast"], r["broadcast"]
    return {
        "links": r["links"]["degree_mean"],
        "unicast": 100.0 * u["on_time"] / u["wanted"] if u["wanted"] else 0.0,
        "bcast": 100.0 * b["on_time"] / b["wanted"] if b["wanted"] else 0.0,
        "per_s": r["on_time_per_airtime_s"],
    }


def cell(rows, metric):
    values = [row[metric] for row in rows]
    m = statistics.mean(values)
    sd = statistics.stdev(values) if len(values) > 1 else 0.0
    if metric == "links":
        return "%.0f" % m
    if metric == "per_s":
        return "%.2f ±%.2f" % (m, sd)
    return "%.1f%% ±%.1f" % (m, sd)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--tsim", required=True, help="the tsim binary, a release build")
    parser.add_argument("--seeds", type=int, default=3, help="seeds per candidate and power")
    parser.add_argument("--power", default="-5,0,5,10,20",
                        help="transmit powers in dBm, comma-separated, quietest (sparsest) first")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="runs at once")
    parser.add_argument("--json", metavar="PATH", help="also write every run's figures here")
    parser.add_argument("-s", dest="set", action="append", default=[], metavar="KEY=VALUE",
                        help="a setting to override in every scenario")
    args = parser.parse_args()
    if args.seeds < 1:
        parser.error("--seeds must be at least 1")
    try:
        powers = [float(p) for p in args.power.split(",")]
    except ValueError:
        parser.error("--power takes numbers, such as -5,0,20")

    cases = [(name, scenario, power, seed) for name, scenario in CANDIDATES for power in powers
             for seed in range(1, args.seeds + 1)]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda c: run(args.tsim, c[1], c[2], c[3], args.set), cases))
    runs = [dict(candidate=c[0], power_dbm=c[2], seed=c[3], **r) for c, r in zip(cases, results)]
    if args.json:
        with open(args.json, "w") as f:
            json.dump(runs, f, indent=1)

    width = 15
    print("%-13s %6s" % ("candidate", "dBm") + "".join("%*s" % (width, m) for m in METRICS))
    for name, _ in CANDIDATES:
        for power in powers:
            rows = [r for r in runs if r["candidate"] == name and r["power_dbm"] == power]
            print("%-13s %6g" % (name, power) + "".join("%*s" % (width, cell(rows, m))
                                                      for m in METRICS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
