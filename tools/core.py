#!/usr/bin/env python3
"""Measures the firmware's core against candidate 3, and checks it has not got worse.

    core.py compare --tsim PATH [--seeds N]
    core.py check --tsim PATH

The `core` routing is the firmware's own code (ternmesh/firmware, src/route.c; tsim/core.h), and
candidate 3 (`distvec`) is the design it was specified from. The two are compared on the routes
nodes hold once a network has settled, and what its announces and requests cost, with no messages
sent. Candidate 3 is run with its re-attachment and without, which the core does not have.

`compare` runs each pair of scenarios below and prints, for each, the mean over the seeds of:

  routes    the share of ordered pairs of nodes where the first holds a route to the second, as
            the warmup ends
  reach     the share where the routes, followed from node to node, arrive. A route that is held
            and does not arrive ends at a node with none, or goes round
  airtime   seconds on the air in the hour after the warmup, all nodes together

The core is run twice: with the tables a board has (ports/node/node.c, the default), and without
those limits, as candidate 3 has none.

`check` runs scenarios/core/town.tsim and fails unless nearly every pair holds a route, nearly
every route held arrives, and no node is on the air for more than its cap allows. It is what the
firmware's CI runs against a change to the core. It runs the core without a board's tables: the
town's 200 nodes are more than a board's 128 destinations, and what it checks is the routing, not
the memory. That routes never go round is not checked here
but in tests/core.c, which follows them while a network settles.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "..", "scenarios")

# (name, candidate 3's scenario, the core's)
PAIRS = [
    ("town", "town-distvec.tsim", "core/town.tsim"),
    ("region", "scale/region-distvec.tsim", "scale/region-core.tsim"),
    ("region, fast", "scale/region-distvec-fast.tsim", "scale/region-core-fast.tsim"),
    ("region, deployed", "scale/region-distvec-deployed.tsim", "scale/region-core-deployed.tsim"),
    ("region, US915", "scale/region-distvec-us915.tsim", "scale/region-core-us915.tsim"),
]

# The town, checked: what the core does there with room to spare for another seed, not what it
# might do. Its cap is 0.5% of a node's time, counted over a minute, and a node may spend a
# minute's worth at once.
ROUTES_MIN = 0.995
ADRIFT_MAX = 0.001  # routes held that do not arrive: over three seeds the core has 0.0001
DUTY_MAX = 0.006


# The core's tables are a board's by default (tsim/core.h). Candidate 3 has no such limits, so the
# core is set free of them to be compared with it, and run as a board as well.
UNBOUNDED = ["routing.neighbours=255", "routing.destinations=0", "routing.frames=16",
             "routing.flood_frames=16"]


def run(tsim, scenario, settings=()):
    cmd = [tsim]
    for s in settings:
        cmd += ["-s", s]
    cmd.append(os.path.join(SCENARIOS, scenario))
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def mean(tsim, scenario, settings, seeds):
    runs = [run(tsim, scenario, list(settings) + [f"seed={s}"]) for s in range(1, seeds + 1)]
    return {
        "routes": sum(r["warmup"]["routes"] for r in runs) / seeds,
        "reach": sum(r["warmup"]["reach"] for r in runs) / seeds,
        "airtime": sum(r["airtime_s"]["total"] for r in runs) / seeds,
    }


def compare(args):
    quiet = ["traffic.interval=none"]
    print("| scenario | routing | routes | reach | airtime, s |")
    print("|---|---|---|---|---|")
    for name, distvec, core in PAIRS:
        rows = [
            ("candidate 3", distvec, quiet),
            ("candidate 3, no re-attachment", distvec, quiet + ["routing.reattach=no"]),
            ("core, unbounded", core, UNBOUNDED),
            ("core, a board's tables", core, []),
        ]
        for label, scenario, settings in rows:
            m = mean(args.tsim, scenario, settings, args.seeds)
            print(
                f"| {name} | {label} | {100 * m['routes']:.2f}% | {100 * m['reach']:.2f}% "
                f"| {m['airtime']:.0f} |"
            )
            sys.stdout.flush()
    return 0


def check(args):
    r = run(args.tsim, "core/town.tsim", UNBOUNDED)
    routes, reach = r["warmup"]["routes"], r["warmup"]["reach"]
    failed = []
    if routes < ROUTES_MIN:
        failed.append(f"routes {routes:.4f} is under {ROUTES_MIN}")
    if routes - reach > ADRIFT_MAX:
        failed.append(f"reach {reach:.4f} is under routes {routes:.4f}: routes held do not arrive")
    if r["duty_max"] > DUTY_MAX:
        failed.append(f"node {r['duty_max_node']} was on the air {r['duty_max']:.4f} of the time")
    if r["airtime_s"]["data"] or r["airtime_s"]["relay"]:
        failed.append("the core sent a frame that is neither an announce nor a request")
    print(f"town: routes {routes:.4f}, reach {reach:.4f}, duty at most {r['duty_max']:.4f}")
    for f in failed:
        print(f"FAIL: {f}")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)
    for name, fn in (("compare", compare), ("check", check)):
        p = sub.add_parser(name)
        p.add_argument("--tsim", required=True, help="the tsim binary")
        if name == "compare":
            p.add_argument("--seeds", type=int, default=3)
        p.set_defaults(fn=fn)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
