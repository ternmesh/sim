#!/usr/bin/env python3
"""Cross-checks candidate 1's compatibility mode against Meshtasticator.

    meshtasticator.py reference --meshtasticator DIR
    meshtasticator.py check --tsim PATH [-s key=value]...

`reference` runs Meshtasticator's discrete-event simulator (https://github.com/meshtastic/Meshtasticator,
CC BY 4.0) with nodes that do not move, over several network sizes and seeds. It writes each map
Meshtasticator placed as a positions file, and the four rates it reports into reference.json, next
to scenarios/meshtasticator/compat.tsim. It needs a checkout of Meshtasticator and its Python
packages, and takes a few minutes; it is run by hand, and what it writes is committed.

`check` runs compat.tsim through tsim on each of those maps and compares its rates with the
reference, averaged over the seeds of each size, which is what CI runs. It needs nothing but tsim.

The rates, which are Meshtasticator's definitions in its own words:

  nodeReach             first receptions of a message, over messages times (nodes - 1)
  usefulness            first receptions, over every reception
  collisionRate         receptions lost to a collision, over every packet a node sensed - of
                        those that were sent, which Meshtasticator's own figure is not
  txAirUtilizationRate  time on the air, over nodes times the simulated time

compat.tsim reproduces what Meshtasticator does, including where it departs from the firmware:
its airtime formula, its sender's loop (traffic.closed, routing.ack_poll) and its late cancelling
(routing.cancel_late). Each is explained where it is set. `check -s` turns one off to show what it
is worth. One difference is left in, and accounts for tsim's airtime running a little high:

  Meshtasticator's originator looks for an acknowledgement in the list of every packet in the
  simulation, and counts a relay's copy of its message as acknowledged once that relay has heard
  the message again - so an originator that heard nothing can still stop retrying, if a node it
  cannot hear heard another rebroadcast. That is knowledge no radio has, and a plugin here sees only
  its own node, so tsim's originators retry more: at 10 nodes, nearly 40% of messages against
  Meshtasticator's under 10%.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, "..", "scenarios", "meshtasticator")

SIZES = [10, 20, 30, 50]
SEEDS = [1, 2, 3, 4, 5]

RATES = ["nodeReach", "usefulness", "collisionRate", "txAirUtilizationRate"]

# How far tsim's mean over a size's seeds may be from Meshtasticator's, as an absolute difference
# in the rate.
TOLERANCE = {
    "nodeReach": 0.05,
    "usefulness": 0.05,
    "collisionRate": 0.05,
    "txAirUtilizationRate": 0.01,
}


def positions_name(nodes, seed):
    return "n%d-s%d.positions" % (nodes, seed)


def run_meshtasticator(nodes, seed):
    """One run, with Meshtasticator's defaults except that nothing moves and nothing is drawn."""
    import random

    from lib.config import Config
    from lib.discrete_event_sim import DiscreteEventSim
    from lib.node import default_generate_node_list

    conf = Config()
    conf.NR_NODES = nodes
    conf.SEED = seed
    conf.MOVEMENT_ENABLED = False
    conf.GUI_ENABLED = False
    conf.PLOT = False
    random.seed(conf.SEED)
    node_configs = default_generate_node_list(conf)
    sim = DiscreteEventSim(conf, node_configs)
    sim.run_simulation()
    results = sim.get_results()
    rates = {r: float(results[r]) for r in RATES}
    # Meshtasticator decides which nodes sense a packet when the packet is made, and counts them
    # whether or not it was ever sent; in a congested run, thousands of rebroadcasts are still
    # queued when the clock stops, and their receivers dilute its collision rate. This counts only
    # the packets that went out, which is what tsim's counts are.
    sensed = collided = 0
    for p in results["packets"]:
        if p.endTime > 0:
            sensed += sum(p.sensedByN)
            collided += sum(s and c for s, c in zip(p.sensedByN, p.collidedAtN))
    rates["collisionRate"] = collided / sensed if sensed else 0.0
    positions = [(c.position.x, c.position.y) for c in node_configs]
    return positions, rates


def reference(args):
    sys.path.insert(0, os.path.abspath(args.meshtasticator))
    commit = subprocess.run(["git", "-C", args.meshtasticator, "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=True).stdout.strip()
    cases = []
    for nodes in SIZES:
        for seed in SEEDS:
            positions, rates = run_meshtasticator(nodes, seed)
            name = positions_name(nodes, seed)
            with open(os.path.join(DIR, name), "w") as f:
                f.write("# x y in metres, as Meshtasticator %s placed %d nodes with seed %d\n"
                        % (commit[:8], nodes, seed))
                for x, y in positions:
                    f.write("%.3f %.3f\n" % (x, y))
            cases.append({"nodes": nodes, "seed": seed, "positions": name, **rates})
            print("%3d nodes, seed %d: %s" % (nodes, seed, fmt(rates)), file=sys.stderr)
    with open(os.path.join(DIR, "reference.json"), "w") as f:
        json.dump({"meshtasticator": commit, "cases": cases}, f, indent=1)
        f.write("\n")


def run_tsim(tsim, case, sets):
    """compat.tsim on one of the reference maps, and the same four rates out of its report."""
    args = [tsim, "-s", "nodes=%d" % case["nodes"], "-s", "positions=" + case["positions"], "-s",
            "seed=%d" % case["seed"]]
    for s in sets:
        args += ["-s", s]
    out = subprocess.run(args + [os.path.join(DIR, "compat.tsim")], capture_output=True, text=True,
                         check=True).stdout
    r = json.loads(out)
    b = r["broadcast"]
    sensed = r["rx_ok"] + r["rx_lost"] + r["rx_preempted"] + r["rx_aborted"] + r["rx_missed"]
    return {
        "nodeReach": b["delivered"] / b["wanted"] if b["wanted"] else 0.0,
        "usefulness": b["delivered"] / r["rx_ok"] if r["rx_ok"] else 0.0,
        "collisionRate": 1 - r["rx_ok"] / sensed if sensed else 0.0,
        "txAirUtilizationRate": r["airtime_s"]["total"] / r["nodes"] / r["elapsed_s"],
    }


def mean(rows, rate):
    return sum(row[rate] for row in rows) / len(rows)


def fmt(rates):
    return "  ".join("%s %.3f" % (r, rates[r]) for r in RATES)


def check(args):
    with open(os.path.join(DIR, "reference.json")) as f:
        ref = json.load(f)
    failed = False
    print("nodes  rate                  meshtasticator  tsim    difference")
    for nodes in sorted({c["nodes"] for c in ref["cases"]}):
        theirs = [c for c in ref["cases"] if c["nodes"] == nodes]
        ours = [run_tsim(args.tsim, c, args.set) for c in theirs]
        for rate in RATES:
            a, b = mean(theirs, rate), mean(ours, rate)
            bad = abs(b - a) > TOLERANCE[rate]
            failed |= bad
            print("%5d  %-20s  %14.3f  %6.3f  %+.3f%s"
                  % (nodes, rate, a, b, b - a, "  over %.2f" % TOLERANCE[rate] if bad else ""))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("reference", help="run Meshtasticator and write the maps and reference.json")
    p.add_argument("--meshtasticator", required=True, help="a checkout of Meshtasticator")
    p = sub.add_parser("check", help="run tsim on the maps and compare with reference.json")
    p.add_argument("--tsim", required=True, help="the tsim binary")
    p.add_argument("-s", dest="set", action="append", default=[], metavar="KEY=VALUE",
                   help="a setting to override in compat.tsim, to see what it is worth")
    args = parser.parse_args()
    if args.command == "reference":
        reference(args)
        return 0
    return check(args)


if __name__ == "__main__":
    sys.exit(main())
