#!/usr/bin/env python3
"""Runs the first field test's drive (scenarios/core/drive.tsim) over seeds, each way apart.

    drive.py --tsim PATH [--seeds N] [--json PATH] [--label TEXT]
    drive.py --compare BEFORE.json AFTER.json

The scenario has the leaf and the desk relay write to each other. This runs it three times for
each seed: as written (both), with only the leaf's messages to the desk (up) and with only the
desk's to the leaf (down), the others' lines left out of a copy beside the scenario, which reads
drive.links from there. For each it prints, summed over the seeds:

  sent      unicast messages made in the window
  delivered of them, those that reached their destination at all
  on_time   and those that did within the deadline
  gave_up   messages their sources gave up on: no valid acknowledgement came back
  lost_ack  of the delivered, those whose source gave up all the same (delivered minus
            sent-but-not-given-up, never below 0): the acknowledgement lost, not the message

--compare prints two such summaries side by side, as the workflow does for two firmware commits.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIO = os.path.join(HERE, "..", "scenarios", "core", "drive.tsim")
LEAF, DESK = 5, 0


def variant(which):
    """The scenario's text with only the messages `which` way: both, up (leaf to desk), down."""
    keep = []
    with open(SCENARIO, encoding="utf-8") as f:
        for line in f:
            parts = [p.strip() for p in line.split("=", 1)[1].split(",")] if line.startswith(
                "traffic.send") else None
            if parts and which != "both":
                src = int(parts[1])
                if (which == "up") != (src == LEAF):
                    continue
            keep.append(line)
    return "".join(keep)


def run(tsim, which, seed):
    text = variant(which)
    with tempfile.NamedTemporaryFile("w", suffix=".tsim", dir=os.path.dirname(SCENARIO),
                                     delete=False, encoding="utf-8") as f:
        f.write(text)
        path = f.name
    try:
        out = subprocess.run([tsim, "-s", f"seed={seed}", path], capture_output=True, text=True,
                             check=True)
    finally:
        os.unlink(path)
    r = json.loads(out.stdout)
    u = r["unicast"]
    gave_up = r["losses"]["drops"]["retries"]
    return {"sent": u["messages"], "delivered": u["delivered"], "on_time": u["on_time"],
            "gave_up": gave_up,
            "lost_ack": max(0, u["delivered"] - (u["messages"] - gave_up))}, {
                "losses": r["losses"], "routing_costs": r.get("routing_costs")}


def summary(tsim, seeds):
    """The totals each way, and every run's losses as tsim reports them, for looking into."""
    out, losses = {}, {}
    for which in ("both", "up", "down"):
        tot = {"sent": 0, "delivered": 0, "on_time": 0, "gave_up": 0, "lost_ack": 0}
        for seed in range(1, seeds + 1):
            counts, lost = run(tsim, which, seed)
            for k, v in counts.items():
                tot[k] += v
            losses[f"{which}/{seed}"] = lost
        out[which] = tot
    return out, losses


def table(label, s):
    rows = [label, f"{'way':<10}{'sent':>6}{'delivered':>16}{'on_time':>9}{'gave_up':>9}"
            f"{'lost_ack':>10}"]
    for which, t in s.items():
        name = {"both": "both", "up": "leaf>desk", "down": "desk>leaf"}[which]
        pct = 100.0 * t["delivered"] / t["sent"] if t["sent"] else 0.0
        rows.append(f"{name:<10}{t['sent']:>6}{t['delivered']:>8} ({pct:5.1f}%){t['on_time']:>9}"
                    f"{t['gave_up']:>9}{t['lost_ack']:>10}")
    return "\n".join(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tsim")
    ap.add_argument("--seeds", type=int, default=10)
    ap.add_argument("--json")
    ap.add_argument("--label", default="")
    ap.add_argument("--compare", nargs=2)
    a = ap.parse_args()
    if a.compare:
        for path in a.compare:
            with open(path, encoding="utf-8") as f:
                d = json.load(f)
            print(table(d["label"], d["summary"]))
            print()
        return
    if not a.tsim:
        sys.exit("--tsim is needed")
    s, losses = summary(a.tsim, a.seeds)
    print(table(a.label, s))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"label": a.label, "seeds": a.seeds, "summary": s, "losses": losses}, f,
                      indent=2)


if __name__ == "__main__":
    main()
