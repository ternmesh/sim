#!/usr/bin/env python3
"""Measures what routing ids that change cost the firmware's core (tsim/core.h, rotate_s).

    rotate.py --tsim PATH [--seeds N] [--jobs N] [--presets fast,us915] [--json PATH]

An experiment for the specification's metadata-privacy analysis, not anything the firmware or the
specification has. A node that rotates takes a new address, and the routing id made from it, and
keeps everything else it holds; see the comment on rotate_s in include/tsim/core.h.

Each preset's network is let settle for SETTLE hours, the firmware's core with the board's tables
and the traffic tools/density.py gives the core (a message from each node every half hour, a
quarter of them broadcast, four regular correspondents, a minute to arrive in). Then every node
that rotates does so at once, and the hours after are measured one at a time: unicast and
broadcast on time, and the network's seconds on the air. With every node rotating at once, the
hours after one rotation are the shape of every epoch; the share of an epoch of length P that each
hour is gives what rotating every P costs, averaged over the epoch (`epoch` in the output).

Rotating at a time of each node's own (`spread`) is measured as it runs, the hour from SETTLE + 4 h,
by then with nodes rotating throughout.

Variants:

  none         no rotation: the baseline
  all          every node, at once; neighbours told the old id is retired
  all-silent   every node, at once; the old id left to be forgotten as a silent neighbour is
  leaves       leaves only, at once, retired; relays keep their ids
  spread-24h   every node, each at its own time once a day, retired
  spread-6h    the same every six hours
  all-4096-learn  as all-4096, with the learning allowance opened again at the rotation

Retiring the old ids network-wide, with the board's places (the spec's analysis, "Requirements
before rotation is turned on"; needs a firmware with the retired-destinations experiment,
tern_route_retire() and tern_route_epoch() in tern/route.h):

  net          retired on the air: a relay frees the place at once and passes the retirement on,
               RETRACTS (3) times, as a retraction goes
  net-once     the same, each retirement passed on once
  net-once-loss  as net-once, a fifth of the neighbours missing the node's own retirement
  epoch        nothing sent beyond all: each node marks its places old at the epoch's start, and a
               new destination takes one when the table is full
  epoch-loss   as epoch, a fifth of the neighbours missing the node's own retirement
  epoch-silent the same, without even the neighbours told: nothing at all on the air
  epoch-net    epoch and net-once together

and each with -4096 or -2048, the same with that many destinations a node has places for, where
the board has 1024 (ports/node/node.c): with 1000 nodes, 4096 is room for every old id and every
new one at once. A destination the network has forgotten is not dropped from a table until its
retraction has run its course, and a new one finds no place until then; with the board's places,
rotating every id at once leaves most of the new ones out (see the comment on rotate_s).
"""

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "..", "scenarios", "scale")

PRESETS = {
    "fast": "region-core-fast.tsim",  # SF7 at 125 kHz: EU868's settings
    "us915": "region-core-us915.tsim",  # SF9 at 500 kHz: what US915 boards ship
    "deployed": "region-core-deployed.tsim",  # SF8 at 62.5 kHz, MeshCore's preset
}

TRAFFIC = ["traffic.interval=30 min", "traffic.len=16..64", "traffic.broadcast=0.25",
           "traffic.lead=3 h", "deadline=60 s", "traffic.peers=4"]

SETTLE = 8  # hours before the rotation
AFTER = [0, 1, 2, 3, 5]  # hours after it, each measured for one

AMPLE = ["routing.destinations=4096"]  # room for every old id and every new one at once
DOUBLE = ["routing.destinations=2048"]

ONCE = ["routing.rotate_count=1"]  # for the hours long after one rotation
LEARN = ["routing.rotate_learn=1"]

# name: (settings, hours after the rotation measured). Each rotates at SETTLE hours, every
# SETTLE hours after, unless ONCE.
VARIANTS = {
    "none": ([], [0, 3]),
    "all": (["routing.rotate_retire=1"], AFTER),
    "all-silent": (["routing.rotate_retire=0"], AFTER),
    "leaves": (["routing.rotate_retire=1", "routing.rotate_leaves=1"], AFTER),
    "none-4096": (AMPLE, [0, 3]),
    "all-4096": (["routing.rotate_retire=1"] + AMPLE + ONCE, AFTER + [8, 11, 15]),
    "all-2048": (["routing.rotate_retire=1"] + DOUBLE, AFTER),
    "leaves-4096": (["routing.rotate_retire=1", "routing.rotate_leaves=1"] + AMPLE, AFTER),
    "all-4096-learn": (["routing.rotate_retire=1"] + AMPLE + ONCE + LEARN, AFTER + [8]),
    "net": (["routing.rotate_retire=2"] + ONCE, AFTER),
    "net-once": (["routing.rotate_retire=2", "routing.retire_repeats=1"] + ONCE, AFTER + [8, 11]),
    "net-once-loss": (["routing.rotate_retire=2", "routing.retire_repeats=1",
                       "routing.retire_loss_ppm=200000"] + ONCE, AFTER + [8, 11]),
    "epoch": (["routing.rotate_retire=1", "routing.rotate_epoch=1"] + ONCE, AFTER + [8, 11]),
    "epoch-loss": (["routing.rotate_retire=1", "routing.rotate_epoch=1",
                    "routing.retire_loss_ppm=200000"] + ONCE, AFTER + [8, 11]),
    "epoch-silent": (["routing.rotate_retire=0", "routing.rotate_epoch=1"] + ONCE,
                     AFTER + [8, 11]),
    "epoch-net": (["routing.rotate_retire=2", "routing.retire_repeats=1", "routing.rotate_epoch=1"]
                  + ONCE, AFTER + [8, 11]),
}
# Each node at its own time, measured SETTLE + 4 h on.
SPREAD = {
    "spread-24h": ["routing.rotate_s=86400", "routing.rotate_spread=1", "routing.rotate_retire=1"],
    "spread-6h": ["routing.rotate_s=21600", "routing.rotate_spread=1", "routing.rotate_retire=1"],
    "spread-24h-4096": ["routing.rotate_s=86400", "routing.rotate_spread=1",
                        "routing.rotate_retire=1"] + AMPLE,
}


def run(tsim, preset, seed, settings):
    args = [tsim, "-s", "seed=%d" % seed]
    for s in TRAFFIC + settings:
        args += ["-s", s]
    args.append(os.path.join(SCENARIOS, PRESETS[preset]))
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    d = json.loads(out)
    u, b = d["unicast"], d["broadcast"]
    return {
        "unicast": u["on_time"] / u["wanted"] if u["wanted"] else 0.0,
        "broadcast": b["on_time"] / b["wanted"] if b["wanted"] else 0.0,
        "airtime": d["airtime_s"]["total"],
        "announce": d["airtime_s"]["announce"],
        "routes": d["warmup"]["routes"],
        "reach": d["warmup"]["reach"],
    }


def jobs(presets, seeds, only, hours_only=()):
    for preset in presets:
        for seed in range(1, seeds + 1):
            for name, (extra, hours) in VARIANTS.items():
                if only and name not in only:
                    continue
                for k in hours:
                    if hours_only and k not in hours_only:
                        continue
                    settings = list(extra) + ["warmup=%d h" % (SETTLE + k), "duration=1 h"]
                    if not name.startswith("none"):
                        settings.append("routing.rotate_s=%d" % (SETTLE * 3600))
                    yield (preset, seed, name, k), settings
            for name, extra in SPREAD.items():
                if only and name not in only:
                    continue
                yield (preset, seed, name, 4), list(extra) + ["warmup=%d h" % (SETTLE + 4),
                                                              "duration=1 h"]


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--tsim", required=True)
    p.add_argument("--seeds", type=int, default=2)
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--presets", default="fast,us915")
    p.add_argument("--variants", default="", help="comma-separated; all of them by default")
    p.add_argument("--hours", default="", help="comma-separated hours after; all by default")
    p.add_argument("--json")
    a = p.parse_args()
    presets = a.presets.split(",")
    only = set(a.variants.split(",")) if a.variants else set()
    hours_only = {int(h) for h in a.hours.split(",")} if a.hours else set()
    todo = list(jobs(presets, a.seeds, only, hours_only))
    results = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(run, a.tsim, key[0], key[1], s): key for key, s in todo}
        for f in concurrent.futures.as_completed(futs):
            key = futs[f]
            results[key] = f.result()
            print("done", key, file=sys.stderr)
    table = {}
    for preset in presets:
        rows = table.setdefault(preset, {})
        names = [n for n in list(VARIANTS) + list(SPREAD) if not only or n in only]
        for name in names:
            hours = sorted({k for (pr, _, n, k) in results if pr == preset and n == name})
            rows[name] = {}
            for k in hours:
                rs = [results[(preset, s, name, k)] for s in range(1, a.seeds + 1)]
                rows[name][k] = {m: mean([r[m] for r in rs]) for m in rs[0]}
    # An epoch of P hours: the hours measured after the rotation, then the baseline's for the rest.
    for preset in presets:
        rows = table[preset]
        for name in [n for n in VARIANTS if n in rows and not n.startswith("none")]:
            ref = "none-4096" if name.endswith("-4096") and "none-4096" in rows else "none"
            if ref not in rows:
                continue
            base = {m: mean([rows[ref][k][m] for k in rows[ref]]) for m in rows[ref][0]}
            rows[name]["epoch"] = {}
            for P in [6, 24, 168]:
                acc = {m: 0.0 for m in base}
                for h in range(P):
                    measured = sorted(k for k in rows[name] if k != "epoch")
                    k = max(x for x in measured if x <= h)
                    # Hours past the last measured one are taken as settled when it was within a
                    # point and a half of the baseline, and as the last measured otherwise.
                    src = rows[name][k]
                    if h > measured[-1] and abs(src["unicast"] - base["unicast"]) < 0.015:
                        src = base
                    for m in acc:
                        acc[m] += src[m] / P
                rows[name]["epoch"][P] = acc
    for preset in presets:
        print("\n%s (%d seeds): unicast on time, broadcast on time, airtime s/h" % (preset, a.seeds))
        for name, hours in table[preset].items():
            for k, r in hours.items():
                if k == "epoch":
                    continue
                print("  %-12s h+%d  %5.1f%%  %5.1f%%  %8.0f" %
                      (name, k, 100 * r["unicast"], 100 * r["broadcast"], r["airtime"]))
            for P, r in hours.get("epoch", {}).items():
                print("  %-12s P=%-4dh  %5.1f%%  %5.1f%%  %8.0f" %
                      (name, P, 100 * r["unicast"], 100 * r["broadcast"], r["airtime"]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({p: {n: {str(k): v for k, v in h.items()} for n, h in t.items()}
                       for p, t in table.items()}, f, indent=1)


if __name__ == "__main__":
    main()
