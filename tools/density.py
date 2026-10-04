#!/usr/bin/env python3
"""Runs every candidate on the 1000-node region at several densities, and scores each the same way.

    density.py --tsim PATH [--deployed | --fast [--sf N]] [--seeds N] [--power DBM,...]
               [--candidates N,...] [--quick] [--jobs N] [--json PATH] [-s key=value]...

A routing result means little without the density it was measured at: the same protocol ranks
differently where every node hears a dozen others and where it hears hundreds. This runs each of
scenarios/scale/region*.tsim - flooding, and candidates 1, 2 and 3 - at each transmit power and
seed, and prints for each candidate and power the mean over the seeds, and its standard deviation,
of the scorecard every comparison reports (MSH-66), in this order:

  unicast  unicasts delivered on time, as a share of those sent
  bcast    broadcast destinations reached on time, as a share of those wanted
  p50 p95  the median and 95th-percentile delay of the unicasts delivered, in seconds
  per_s    on-time deliveries, unicast and broadcast, per second of airtime after the warmup
  duty     the busiest node's share of the measured hour spent sending
  churn    unicast on time with nodes going down and coming back up: a quarter of the 200 sites
           (churn.nodes = relays, churn.share = 0.25), each down 30 minutes on average after two
           hours up, as MSH-59 measured candidate 3; for flooding, which has no sites, the same 50
           nodes' worth from all of them (churn.share = 0.05)
  move     unicast on time with a quarter of the leaves walking (move.share = 0.25: 200 of the
           800); for flooding, 200 of all the nodes (move.nodes = all, move.share = 0.2)

and after them, for context:

  links    the links per node tsim reports: pairs that decode each other at the floor with
           nothing else on the air, which is the most any routing can use
  reach    for a candidate that keeps routes, the share of ordered pairs of nodes its routes
           connected when the warmup ended, followed node to node: whether it had settled

churn and move each take a run of their own for every case, so the sweep takes three times as
long; --quick leaves them out. With -s routing.relay_pick=elect there are no sites when churn and
movement pick their nodes, so those runs take theirs from every node, as flooding's do.

Power stands in for density. Under the log-distance channel, sending Δ dB quieter loses as much as
standing 10^(Δ/10n) times further apart (n = 2.32 by default, so 5 dB is 1.64 times), and keeps
the map, the seeds and the shadowing field as they are; spreading the same nodes over a larger area
would change how far apart the field's cells are. On the default map the region's 20 dBm gives
about 470 links per node, and -5 dBm about 12.

Every scenario's settings can be overridden with -s, as with tsim: `-s traffic.peers=3 -s
traffic.reply=0.5` gives each node three regular correspondents who answer half its messages, which
is what lets a routed candidate use a route more than once. A setting only one candidate has, such
as candidate 3's `routing.sf_min`, wants `--candidates 3`: the others would refuse it.

--deployed runs scenarios/scale/region*-deployed.tsim instead: each candidate on the preset it is
deployed on - Meshtastic on LongFast, MeshCore, and candidate 3 and flooding with it, on the UK/EU
narrow preset - and candidates 1 to 3 over the same 200 sites, Meshtastic's routers, MeshCore's
repeaters and candidate 3's relays, the rest clients, companions and leaves. Without it every
candidate runs at SF9 and 125 kHz with every node relaying, which no deployment of either is.

--fast runs scenarios/scale/region*-fast.tsim (MSH-66): every candidate as deployed - the 200
sites, the incumbents' own traffic, the duty cycle - but all on one fast radio, SF7 at 125 kHz,
coding rate 4/5 and a 16-symbol preamble, so that only the routing and its MAC differ. --sf 8 runs
them at SF8, the sites picked again over links at SF8.

The sweep is too long for CI, which runs each region scenario once, at 20 dBm. It wants a release
build: each run takes 5 to 100 s of CPU there, the warmup included, and the runs go in parallel.
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
DEPLOYED = {s: s.replace(".tsim", "-deployed.tsim") for _, s in CANDIDATES}
FAST = {s: s.replace(".tsim", "-fast.tsim") for _, s in CANDIDATES}
METRICS = ["unicast", "bcast", "p50", "p95", "per_s", "duty", "churn", "move", "links", "reach"]

# The stresses the scorecard's churn and move columns are measured under: over the sites where
# there are sites, and the same number of nodes from all of them where there are none.
STRESS = {
    "churn": {
        "sited": ["churn.nodes=relays", "churn.share=0.25", "churn.up=2 h", "churn.down=30 min"],
        "flat": ["churn.nodes=all", "churn.share=0.05", "churn.up=2 h", "churn.down=30 min"],
    },
    "move": {
        "sited": ["move.nodes=leaves", "move.share=0.25"],
        "flat": ["move.nodes=all", "move.share=0.2"],
    },
}


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
        "p50": u["latency_p50_s"],
        "p95": u["latency_p95_s"],
        "per_s": r["on_time_per_airtime_s"],
        "duty": 100.0 * r["duty_max"],
        "reach": 100.0 * r["warmup"]["reach"],
    }


def stress_sets(name, stress, sets):
    """The settings a stress run adds, ahead of the caller's own so that those can tune it."""
    flat = name == "flood" or any(s.replace(" ", "") == "routing.relay_pick=elect" for s in sets)
    return STRESS[stress]["flat" if flat else "sited"]


def cell(rows, metric):
    values = [row[metric] for row in rows if row.get(metric) is not None]
    if not values or (metric == "reach" and statistics.mean(values) < 0):
        return "-"
    m = statistics.mean(values)
    sd = statistics.stdev(values) if len(values) > 1 else 0.0
    if metric == "links":
        return "%.0f ±%.0f" % (m, sd)
    if metric in ("p50", "p95"):
        return "%.1f ±%.1f" % (m, sd)
    if metric == "per_s":
        return "%.2f ±%.2f" % (m, sd)
    return "%.1f%% ±%.1f" % (m, sd)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--tsim", required=True, help="the tsim binary, a release build")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--deployed", action="store_true",
                      help="run each candidate as deployed: its own preset, 200 shared sites")
    mode.add_argument("--fast", action="store_true",
                      help="run each candidate as deployed, but all on SF7 at 125 kHz")
    parser.add_argument("--sf", type=int, help="with --fast, the spreading factor instead of 7, "
                        "the sites picked again at it")
    parser.add_argument("--quick", action="store_true",
                        help="leave out the churn and move runs, and their columns")
    parser.add_argument("--seeds", type=int, default=3, help="seeds per candidate and power")
    parser.add_argument("--power", default="-5,0,5,10,20",
                        help="transmit powers in dBm, comma-separated, quietest (sparsest) first; "
                        "--power=-5,0 for a list starting below zero")
    parser.add_argument("--candidates", default="flood,1,2,3",
                        help="which to run, comma-separated: flood, 1, 2 and 3")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="runs at once")
    parser.add_argument("--json", metavar="PATH", help="also write every run's figures here")
    parser.add_argument("-s", dest="set", action="append", default=[], metavar="KEY=VALUE",
                        help="a setting to override in every scenario, but seed and radio.tx_dbm")
    args = parser.parse_args()
    if args.seeds < 1:
        parser.error("--seeds must be at least 1")
    try:
        powers = [float(p) for p in args.power.split(",")]
    except ValueError:
        parser.error("--power takes numbers, such as -5,0,20")
    if args.sf is not None:
        if not args.fast:
            parser.error("--sf goes with --fast")
        if not 7 <= args.sf <= 12:
            parser.error("--sf takes 7 to 12")
    for s in args.set:
        # The sweep sets these itself; an override would run every row at one value and label
        # it with another.
        if s.split("=", 1)[0].strip() in ("seed", "radio.tx_dbm"):
            parser.error("-s %s: the sweep sets seed and radio.tx_dbm; use --seeds and --power" % s)
        if args.sf is not None and s.split("=", 1)[0].strip() in ("radio.sf", "sites.sf"):
            parser.error("-s %s: --sf sets radio.sf and sites.sf" % s)
    sets = list(args.set)
    if args.sf is not None:
        sets = ["radio.sf=%d" % args.sf, "sites.sf=%d" % args.sf] + sets

    wanted = [c.strip() for c in args.candidates.split(",")]
    family = DEPLOYED if args.deployed else FAST if args.fast else None
    chosen = [(name, family[scenario] if family else scenario)
              for name, scenario in CANDIDATES if name.split()[0] in wanted]
    if len(chosen) != len(set(wanted)):
        parser.error("--candidates takes flood, 1, 2 and 3, comma-separated")

    stresses = [] if args.quick else list(STRESS)
    cases = [(name, scenario, power, seed, stress) for name, scenario in chosen
             for power in powers for seed in range(1, args.seeds + 1)
             for stress in [None] + stresses]

    def one(c):
        name, scenario, power, seed, stress = c
        extra = stress_sets(name, stress, args.set) if stress else []
        return run(args.tsim, scenario, power, seed, extra + sets)

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(one, cases))
    runs = [dict(candidate=c[0], power_dbm=c[2], seed=c[3], stress=c[4], **r)
            for c, r in zip(cases, results)]
    if args.json:
        with open(args.json, "w") as f:
            json.dump(runs, f, indent=1)

    # One row per candidate, power and seed: the run with nothing stressed, and the unicast of
    # each stressed run beside it.
    rows = {}
    for r in runs:
        key = (r["candidate"], r["power_dbm"], r["seed"])
        if r["stress"] is None:
            rows.setdefault(key, {}).update(r)
        else:
            rows.setdefault(key, {})[r["stress"]] = r["unicast"]

    metrics = [m for m in METRICS if m not in STRESS or m in stresses]
    width = 14
    print("%-13s %6s" % ("candidate", "dBm") + "".join("%*s" % (width, m) for m in metrics))
    for name, _ in chosen:
        for power in powers:
            cells = [r for (n, p, _), r in rows.items() if n == name and p == power]
            print("%-13s %6g" % (name, power) + "".join("%*s" % (width, cell(cells, m))
                                                      for m in metrics))
    return 0


if __name__ == "__main__":
    sys.exit(main())
