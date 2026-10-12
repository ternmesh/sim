#!/usr/bin/env python3
"""Runs every candidate over the field-derived workloads (MSH-34) and tabulates each flow.

    workload.py --tsim PATH [--seeds N] [--jobs N] [--out DIR] [--only NAME,...]

The workloads are scenarios/workload/{metro,event,dm}.tsim, whose rates and sources are in
docs/workload.md. Each is run with every candidate below, on the same nodes, sites and messages,
over seeds 1 to N, and the mean over the seeds is printed for each flow:

  on time   the share of what was wanted that arrived within the deadline. A presence, position or
            telemetry broadcast is wanted only within its reach of its sender
  beyond    nodes reached outside a broadcast's reach, per message: what a scope would save
  airtime   seconds on the air in the measured hour, all nodes together
  per_s     on-time deliveries per second of that airtime

The candidates. Each incumbent carries the workload's flows as its own traffic, its own
background off - the flows are its NodeInfo, position and telemetry - and holds to the duty cycle
as its firmware does. Candidate 3 and the core are Tern; the scoped rows are the scoped broadcast
MSH-34 asked to be measured:

  meshtastic       managed flooding (candidate 1)
  meshcore         path routing with a flood fallback (candidate 2)
  distvec          candidate 3, unscoped: every broadcast goes bcast_hops relays
  distvec, geo     candidate 3 with broadcasts scoped by place (routing.bcast_geo): presence,
                   position and telemetry are passed on only by relays within their reach
  core             the firmware's routing, with a board's tables: the flood of draft/flooding.md
  core, 1 hop      the core with presence, position and telemetry asking for one relay hop, the
                   scope the flood's head already carries
  core, geo        not run: the firmware has no scope by place

Each run's report is kept in --out, so the table can be printed again with --only and no runs.
"""

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "..", "scenarios", "workload")

WORKLOADS = ["metro", "event", "dm"]
LOCAL = ["presence", "position", "telemetry"]

CANDIDATES = [
    ("meshtastic", ["routing=meshtastic", "mac=meshtastic", "duty_cycle=none",
                    "routing.duty_cycle=10%"]),
    ("meshcore", ["routing=meshcore", "mac=meshcore"]),
    ("distvec", ["routing=distvec", "mac=meshcore"]),
    ("distvec, geo", ["routing=distvec", "mac=meshcore", "routing.bcast_geo=yes"]),
    ("core", []),
    ("core, 1 hop", ["flow.%s.hops=1" % f for f in LOCAL]),
]


def slug(name):
    return name.replace(", ", "-").replace(" ", "")


def run(tsim, out, workload, name, settings, seed):
    path = os.path.join(out, "%s-%s-%d.json" % (workload, slug(name), seed))
    if os.path.exists(path):
        return path
    cmd = [tsim, "-s", "seed=%d" % seed]
    for s in settings:
        cmd += ["-s", s]
    cmd.append(os.path.join(SCENARIOS, workload + ".tsim"))
    report = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    json.loads(report)
    with open(path + ".part", "w") as f:
        f.write(report)
    os.replace(path + ".part", path)
    return path


def table(out, seeds, names):
    for workload in WORKLOADS:
        reports = {}
        for name, _ in CANDIDATES:
            if name not in names:
                continue
            got = []
            for seed in range(1, seeds + 1):
                path = os.path.join(out, "%s-%s-%d.json" % (workload, slug(name), seed))
                if os.path.exists(path):
                    with open(path) as f:
                        got.append(json.load(f))
            if got:
                reports[name] = got
        if not reports:
            continue
        flows = list(next(iter(reports.values()))[0]["flows"])
        print("\n%s (%s)" % (workload, ", ".join(
            "%s %d seeds" % (n, len(r)) for n, r in reports.items())))
        print("%-14s" % "" + "".join("%11s" % f[:10] for f in flows) +
              "%9s%9s%9s" % ("beyond", "airtime", "per_s"))
        for name, rs in reports.items():
            row = "%-14s" % name
            for f in flows:
                share = [r["flows"][f]["on_time"] / max(1, r["flows"][f]["wanted"]) for r in rs]
                row += "%10.1f%%" % (100 * sum(share) / len(share))
            beyond = [sum(r["flows"][f]["beyond"] for f in flows) /
                      max(1, sum(r["flows"][f]["messages"] for f in flows if f in LOCAL))
                      for r in rs]
            air = [r["airtime_s"]["total"] for r in rs]
            per_s = [r["on_time_per_airtime_s"] for r in rs]
            row += "%9.0f%9.0f%9.2f" % (sum(beyond) / len(rs), sum(air) / len(rs),
                                        sum(per_s) / len(rs))
            print(row)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tsim", required=True)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--out", default="workload-out")
    ap.add_argument("--only", help="print the table from --out for these candidates, no runs")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.only is not None:
        names = [n.strip() for n in args.only.split(",")] if args.only else \
            [n for n, _ in CANDIDATES]
        table(args.out, args.seeds, names)
        return 0
    jobs = [(w, n, s, seed) for seed in range(1, args.seeds + 1) for w in WORKLOADS
            for n, s in CANDIDATES]
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(run, args.tsim, args.out, *j): j for j in jobs}
        for done in concurrent.futures.as_completed(futures):
            w, n, _, seed = futures[done]
            try:
                done.result()
                print("done %s %s seed %d" % (w, n, seed), file=sys.stderr, flush=True)
            except subprocess.CalledProcessError as e:
                failed += 1
                print("FAILED %s %s seed %d: %s" % (w, n, seed, e.stderr.strip()),
                      file=sys.stderr, flush=True)
    table(args.out, args.seeds, [n for n, _ in CANDIDATES])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
