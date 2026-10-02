#!/usr/bin/env python3
"""Cross-checks candidate 2 against MeshBench.

    meshbench.py reference [--seeds N] [--links]
    meshbench.py check --tsim PATH [-s key=value]...

`reference` drives MeshBench (https://github.com/MeshBench/meshbench, GPL-3.0-or-later) headless,
through its Python client, on the Fife network it ships: 56 nodes running MeshCore 1.17.1 itself -
46 repeaters, 9 companions and a room server - over terrain, which MeshBench downloads. It needs the
meshbench binary (on PATH, or named by MESHBENCH_BINARY) and the meshbench Python package; it is run
by hand, and what it writes is committed:

  fife.links      the loss of every link as MeshBench budgets it - path loss over the terrain, both
                  antennas and their feedlines - by node number only: no names, no positions. It
                  takes about an hour, so it is written only if missing or with --links
  reference.json  for each sender, arm and seed, what one flood cost and reached

Each run boots every node, lets the boot adverts settle, and then has one companion send a channel
message: AngusOutlaw1, and Rogue2-W-T, five repeater hops from it. Only that message's frames are
counted, from MeshBench's own record of each transmission, reception and miss:

  tx       frames sent: the companion's, and every relay
  reached  nodes that decoded the message, other than its sender
  rx       receptions decoded, first or duplicate, at every node

The arms are MeshCore as shipped (`control`) and with every repeater's receive delay set to 10, as
MeshBench's study of it did (`rx_delay`). Each arm sets the receive delay outright, because a
repeater keeps the setting from one run to the next.

`check` runs fife.tsim through tsim for each sender, arm and seed, and compares the means over the
seeds: tx and reached to within 10%, and rx, which varies far more from seed to seed, to within two
standard errors. It needs nothing but tsim. fife.tsim sets two things that are MeshBench's rather
than MeshCore's:

  routing.estimate_cr  MeshBench's firmware reckons its delays at coding rate 4/5 while the air runs
                       at 4/8, so its relays wait about 30% less than MeshCore's on a real radio
  mac.latched_header   MeshBench's radio never clears its header-valid flag when the driver reads a
                       packet, as RadioLib's readData() does on a real one. MeshCore's
                       CustomSX1262::isReceiving() reads that flag as a busy channel until its
                       stale-flag timeout, 3934 ms, clears it - and only when it looks, before
                       sending. So a repeater waits 3.9 s from the first header it hears, every
                       repeater that heard the same frame relays in the same few hundred
                       milliseconds, and the flood goes out in bursts about 4.5 s apart that collide
                       with each other. Without it tsim decodes twice the receptions MeshBench does,
                       with tx and reached a few percent high; with it the two agree on all three

Two of the study's findings, made in August 2026 on an older MeshBench, are worth setting beside
these. Its control flooded through every repeater, 93 frames on every seed counting the boot
adverts; on v0.1.0, as in tsim, a few repeaters miss each flood. And it found a receive delay of 10
saved 15% of the transmissions; on v0.1.0 the rx_delay arm is within the seeds' spread of the
control, as in tsim. Its third - that cancelling a relay heard from another node saves nothing on
its own - tsim reproduces when the cancelling applies to floods waiting out their receive delay
(routing.cancel_heard=waiting): with no delay there is nothing waiting to cancel.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, "..", "scenarios", "meshbench")

FIXTURE = "fixture-fife-strict.json"
SENDERS = ["AngusOutlaw1", "Rogue2-W-T"]
TEXT = "hello from tern sim 0123456789"
SEND_AT_S = 25
RUN_TO_S = 85

ARMS = {
    "control": {"repeater_cli": ["set rxdelay 0"], "tsim": []},
    "rx_delay": {"repeater_cli": ["set rxdelay 10"], "tsim": ["routing.rx_delay_base=10"]},
}

METRICS = ["tx", "reached", "rx"]

# How far tsim's mean over the seeds may be from MeshBench's, as a fraction of MeshBench's. rx is
# not given one: it turns on which frames collide, and from one seed to the next it varies by half
# its mean either way, so eight seeds pin it down to about 10%. It is held instead to two standard
# errors of the difference between the two means.
TOLERANCE = {"tx": 0.10, "reached": 0.10}


def kind_order(node):
    """Repeaters first, then companions, then the rest: fife.tsim's relays are 0 to 45."""
    return {"simple-repeater": 0, "advanced-repeater": 0, "companion": 1}.get(node["kind"], 2)


def receive_loss(direction):
    """Everything between the two radios: the path, both antennas and their feedlines."""
    return -sum(t["db"] for t in direction["terms"]
                if t["name"] not in ("transmit power", "receiver sensitivity"))


def one_run(Workbench, fixture, seed, arm, sender, numbering):
    from datetime import timedelta

    with Workbench.headless(fixture=fixture, seed=seed) as wb:
        wb.wait_idle()
        wb.call("schedule.clear")
        wb.sim.start()
        for node in numbering:
            if kind_order(node) == 0:
                for line in ARMS[arm]["repeater_cli"]:
                    wb.console(node["name"]).send(line)
        wb.sim.run(simulated=timedelta(seconds=SEND_AT_S - 5))
        wb.schedule.add(sender, "public " + TEXT, at=timedelta(seconds=SEND_AT_S))
        wb.sim.run(simulated=timedelta(seconds=RUN_TO_S - SEND_AT_S + 5))
        events = wb.call("events.recent", {"limit": 100000})["events"]
    first = [e for e in events if e["kind"] == "tx" and e["from"] == sender]
    if not first:
        raise RuntimeError("seed %d: the message was never sent" % seed)
    ours = [e for e in events if e.get("message_id") == first[0]["message_id"]]
    rx = [e for e in ours if e["kind"] == "rx"]
    return {
        "tx": sum(1 for e in ours if e["kind"] == "tx"),
        "reached": len({e["to"] for e in rx if e["to"] != sender}),
        "rx": len(rx),
    }


def write_links(wb, numbering, version):
    import time

    wb.call("terrain.allow", {"on": True})
    wb.call("terrain.prefetch", {})
    wb.wait_idle()
    links = []
    for i in range(len(numbering)):
        for j in range(i + 1, len(numbering)):
            a, b = numbering[i]["name"], numbering[j]["name"]
            wb.call("link.pair", {"a": a, "b": b})
            while True:
                try:
                    r = wb.call("link.result")
                except Exception:  # not analysed yet
                    r = {}
                if r.get("from") == a and r.get("to") == b and "directions" in r:
                    break
                time.sleep(0.01)
            there = next(d for d in r["directions"] if d["from"] == a)
            back = next(d for d in r["directions"] if d["from"] == b)
            links.append((i, j, receive_loss(there), receive_loss(back)))
    relays = sum(1 for n in numbering if kind_order(n) == 0)
    with open(os.path.join(DIR, "fife.links"), "w") as f:
        f.write("# The loss of every link of MeshBench %s's %s, as MeshBench budgets it: path\n"
                "# loss over the terrain, both antennas and their feedlines. Nodes 0 to %d are\n"
                "# repeaters, %d to %d companions and %d a room server.\n"
                % (version, FIXTURE, relays - 1, relays,
                   relays + sum(1 for n in numbering if kind_order(n) == 1) - 1,
                   len(numbering) - 1))
        for i, j, there, back in links:
            if abs(there - back) < 0.005:
                f.write("%d %d %.2f\n" % (i, j, there))
            else:
                f.write("%d %d %.2f %.2f\n" % (i, j, there, back))


def reference(args):
    import shutil

    # This script is meshbench.py too: look for MeshBench's package everywhere but beside it.
    sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]
    sys.modules.pop("meshbench", None)
    from meshbench import Workbench

    binary = os.environ.get("MESHBENCH_BINARY") or shutil.which("meshbench")
    if not binary:
        sys.exit("meshbench is not on PATH, and MESHBENCH_BINARY is not set")
    os.environ["MESHBENCH_BINARY"] = binary
    fixture = os.path.join(os.path.dirname(os.path.realpath(binary)), "fixtures", FIXTURE)
    if not os.path.exists(fixture):
        sys.exit("cannot find %s beside %s" % (FIXTURE, binary))
    # Its usage starts "meshbench v0.1.0 - ...". Without -h it would open the desktop workbench.
    usage = subprocess.run([binary, "-h"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True).stdout
    version = usage.split()[1] if usage.startswith("meshbench ") else "unknown"

    with Workbench.headless(fixture=fixture, seed=1) as wb:
        wb.wait_idle()
        nodes = [n for n in wb.call("nodes.list")["nodes"] if n.get("firmware")]
        numbering = sorted(nodes, key=kind_order)
        if args.links or not os.path.exists(os.path.join(DIR, "fife.links")):
            write_links(wb, numbering, version)

    names = [n["name"] for n in numbering]
    senders = []
    cases = []
    for sender in SENDERS:
        number = names.index(sender)
        senders.append({"node": number, "message_bytes": len(sender) + 2 + len(TEXT)})
        for arm in ARMS:
            for seed in range(1, args.seeds + 1):
                result = one_run(Workbench, fixture, seed, arm, sender, numbering)
                cases.append({"sender": number, "arm": arm, "seed": seed, **result})
                print("%2d %-8s seed %d: %s" % (number, arm, seed, fmt(result)), file=sys.stderr)
    with open(os.path.join(DIR, "reference.json"), "w") as f:
        json.dump({"meshbench": version, "fixture": FIXTURE, "send_at_s": SEND_AT_S,
                   "senders": senders, "cases": cases}, f, indent=1)
        f.write("\n")


def run_tsim(tsim, ref, sender, arm, seed, sets):
    args = [tsim, "-s", "seed=%d" % seed, "-s",
            "traffic.send=%d s, %d, all, %d" % (ref["send_at_s"], sender["node"],
                                                sender["message_bytes"])]
    for s in ARMS[arm]["tsim"] + sets:
        args += ["-s", s]
    out = subprocess.run(args + [os.path.join(DIR, "fife.tsim")], capture_output=True, text=True,
                         check=True).stdout
    r = json.loads(out)
    return {
        "tx": r["frames"]["data"] + r["frames"]["relay"],
        "reached": r["broadcast"]["delivered"],
        "rx": r["rx_ok"],
    }


def mean(rows, metric):
    return sum(row[metric] for row in rows) / len(rows)


def variance_of_mean(rows, metric):
    m = mean(rows, metric)
    return sum((row[metric] - m) ** 2 for row in rows) / (len(rows) - 1) / len(rows)


def fmt(result):
    return "  ".join("%s %d" % (m, result[m]) for m in METRICS)


def check(args):
    with open(os.path.join(DIR, "reference.json")) as f:
        ref = json.load(f)
    failed = False
    print("sender  arm       metric   meshbench    tsim  difference   limit")
    for sender in ref["senders"]:
        for arm in ARMS:
            theirs = [c for c in ref["cases"] if c["sender"] == sender["node"] and c["arm"] == arm]
            if len(theirs) < 2:
                sys.exit("reference.json has %d seed(s) for sender %d, %s: rx's limit needs two"
                         % (len(theirs), sender["node"], arm))
            ours = [run_tsim(args.tsim, ref, sender, arm, c["seed"], args.set) for c in theirs]
            for metric in METRICS:
                a, b = mean(theirs, metric), mean(ours, metric)
                if metric in TOLERANCE:
                    limit = TOLERANCE[metric] * a
                else:
                    spread = variance_of_mean(theirs, metric) + variance_of_mean(ours, metric)
                    limit = 2 * spread ** 0.5
                bad = abs(b - a) > limit
                failed |= bad
                print("%6d  %-8s  %-7s  %9.1f  %6.1f  %+9.1f%%  %5.1f%%%s"
                      % (sender["node"], arm, metric, a, b, 100 * (b - a) / a, 100 * limit / a,
                         "  over" if bad else ""))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("reference", help="run MeshBench and write reference.json, and fife.links")
    p.add_argument("--seeds", type=int, default=8, help="seeds per sender and arm, at least 2")
    p.add_argument("--links", action="store_true", help="write fife.links even if it exists")
    p = sub.add_parser("check", help="run tsim on fife.tsim and compare with reference.json")
    p.add_argument("--tsim", required=True, help="the tsim binary")
    p.add_argument("-s", dest="set", action="append", default=[], metavar="KEY=VALUE",
                   help="a setting to override in fife.tsim, to see what it is worth")
    args = parser.parse_args()
    if args.command == "reference" and args.seeds < 2:
        parser.error("--seeds must be at least 2: check measures rx against the seeds' spread")
    if args.command == "reference":
        reference(args)
        return 0
    return check(args)


if __name__ == "__main__":
    sys.exit(main())
