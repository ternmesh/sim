#!/usr/bin/env python3
"""How much a link's loss varies from frame to frame, from what run.py measured.

    fading.py analyse link.csv [more.csv ...]
    fading.py check            the estimators against made-up links whose fading is known

The simulator's `phy.fading` is the standard deviation, in dB, of a Gaussian added to a link's loss
afresh for every frame (tsim/phy.h). A link's RSSI over a run varies for two reasons: that, and
slower drift - people moving, doors, the weather - which the simulator does not draw per frame.
The two are told apart by how far apart in time two frames are. For frames dt apart,

    sigma(dt) = sd(RSSI(t + dt) - RSSI(t)) / sqrt(2)

which is the per-frame fading alone when nothing drifts in dt, and grows with dt as drift adds to
it. sigma at the shortest gap measured is what `phy.fading` should be set to; the rest of the row
says how much of the variation is slower than that. The RSSI is whole dB, so a twelfth of a dB
squared is taken off each variance for the rounding.

Two things make a link's figures too small, and are warned of: frames that were not heard, which
are the weakest (the variance of the ones that were is cut short), and a receiver near 0 dBm,
whose RSSI is clipped. No dependency beyond the standard library.
"""

import argparse
import bisect
import csv
import math
import random
import sys

# The separations sigma is given at, in seconds; the first is "the next frame".
LAGS = [None, 60, 600, 3600, 6 * 3600]
ROUNDING_VAR = 1 / 12  # whole-dB RSSI: the variance a uniform rounding error adds
LOSS_WARN = 0.02
STRONG_WARN_DBM = -50


def read(paths):
    """Rows of every file, by direction (sender, listener), in time order."""
    links = {}
    for path in paths:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                if r.get("time_s") in (None, "", "time_s"):
                    continue
                key = (r["sender"], r["listener"])
                heard = r["heard"] == "1"
                links.setdefault(key, []).append({
                    "t": float(r["time_s"]),
                    "heard": heard,
                    "rssi": float(r["rssi_dbm"]) if heard else None,
                    "snr": float(r["snr_cdb"]) / 100 if heard and r["snr_cdb"] else None,
                })
    for rows in links.values():
        rows.sort(key=lambda r: r["t"])
    return links


def sd(xs):
    if len(xs) < 2:
        return None
    m = sum(xs) / len(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def sigma_from_diffs(diffs, rounding=True):
    """sd of the differences over sqrt(2), the rounding taken off; with its standard error."""
    if len(diffs) < 10:
        return None, None
    var = sd(diffs) ** 2 / 2 - (ROUNDING_VAR if rounding else 0)
    s = math.sqrt(max(var, 0.0))
    return s, s / math.sqrt(2 * (len(diffs) - 1)) if s > 0 else 0.0


def next_frame_diffs(rows):
    """Differences between frames one after the other, both heard. A miss between breaks a pair:
    across it, the gap is twice as long."""
    diffs = []
    for a, b in zip(rows, rows[1:]):
        if a["heard"] and b["heard"]:
            diffs.append(b["rssi"] - a["rssi"])
    return diffs


def lag_diffs(heard, lag):
    """Differences between heard frames lag seconds apart, give or take a quarter of it: each frame
    paired with the one nearest lag later, every frame once at most as the first."""
    ts = [r["t"] for r in heard]
    diffs = []
    for i, r in enumerate(heard):
        j = bisect.bisect_left(ts, r["t"] + lag, lo=i + 1)
        best = None
        for k in (j - 1, j):
            if i < k < len(ts) and (best is None or abs(ts[k] - r["t"] - lag) <
                                    abs(ts[best] - r["t"] - lag)):
                best = k
        if best is not None and abs(ts[best] - r["t"] - lag) <= lag / 4:
            diffs.append(heard[best]["rssi"] - r["rssi"])
    return diffs


def lag1_correlation(rows):
    pairs = [(a["rssi"], b["rssi"]) for a, b in zip(rows, rows[1:]) if a["heard"] and b["heard"]]
    if len(pairs) < 10:
        return None
    xs, ys = zip(*pairs)
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    cov = sum((x - mx) * (y - my) for x, y in pairs)
    vx = sum((x - mx) ** 2 for x in xs)
    vy = sum((y - my) ** 2 for y in ys)
    return cov / math.sqrt(vx * vy) if vx > 0 and vy > 0 else None


def summarise(rows, rounding=True):
    heard = [r for r in rows if r["heard"]]
    s = {"sent": len(rows), "heard": len(heard)}
    if not heard:
        return s
    rssi = [r["rssi"] for r in heard]
    snr = [r["snr"] for r in heard if r["snr"] is not None]
    gaps = [b["t"] - a["t"] for a, b in zip(rows, rows[1:])]
    s["gap"] = sorted(gaps)[len(gaps) // 2] if gaps else None
    s["hours"] = (rows[-1]["t"] - rows[0]["t"]) / 3600
    s["rssi_mean"] = sum(rssi) / len(rssi)
    s["rssi_sd"] = sd(rssi)
    s["snr_mean"] = sum(snr) / len(snr) if snr else None
    s["r1"] = lag1_correlation(rows)
    s["sigma"] = {}
    for lag in LAGS:
        diffs = next_frame_diffs(rows) if lag is None else lag_diffs(heard, lag)
        s["sigma"][lag] = (sigma_from_diffs(diffs, rounding), len(diffs))
    return s


def lag_name(lag, gap):
    if lag is None:
        return "next frame" + (f" ({gap:.0f} s)" if gap else "")
    return f"{lag // 3600} h" if lag >= 3600 else f"{lag // 60} min"


def analyse(args):
    links = read(args.files)
    if not links:
        sys.exit("no rows")
    out = []
    for (tx, rx), rows in sorted(links.items()):
        s = summarise(rows)
        out.append(f"{tx} -> {rx}: {s['heard']} of {s['sent']} frames heard")
        if not s["heard"]:
            continue
        out.append(f"  over {s['hours']:.1f} h, a frame every {s['gap']:.0f} s")
        snr = f", SNR {s['snr_mean']:.1f} dB" if s["snr_mean"] is not None else ""
        out.append(f"  mean RSSI {s['rssi_mean']:.1f} dBm{snr}; sd over the whole run "
                   f"{s['rssi_sd']:.2f} dB")
        if s["r1"] is not None:
            out.append(f"  correlation of one frame with the next: {s['r1']:.2f}")
        out.append("  sigma, dB, for frames this far apart:")
        for lag in LAGS:
            (sig, err), n = s["sigma"][lag]
            val = f"{sig:.2f} +/- {err:.2f}" if sig is not None else "too few pairs"
            out.append(f"    {lag_name(lag, s['gap']):>16}: {val}  ({n} pairs)")
        loss = 1 - s["heard"] / s["sent"]
        if loss > LOSS_WARN:
            out.append(f"  WARNING: {loss:.0%} of frames were not heard. The weakest are the ones "
                       "missing, so these figures are too small: send louder or move closer.")
        if s["rssi_mean"] > STRONG_WARN_DBM:
            out.append(f"  WARNING: a mean RSSI above {STRONG_WARN_DBM} dBm. The receiver may be "
                       "clipping and the boards near enough to see each other's near field: "
                       "send quieter or move apart.")
        first = s["sigma"][None][0][0]
        if first is not None:
            out.append(f"  for the simulator: -s phy.fading={first:.1f}")
        out.append("")
    print("\n".join(out).rstrip())


# --- check ------------------------------------------------------------------------------------


def made_up(rng, n, gap, fading, drift_per_h, mean=-95.0, floor=None):
    """A link with Gaussian per-frame fading of `fading` dB on a random walk that wanders
    `drift_per_h` dB (sd) an hour, rounded to whole dB as the radio gives it."""
    rows, slow = [], 0.0
    step = drift_per_h * math.sqrt(gap / 3600)
    for i in range(n):
        slow += rng.gauss(0, step)
        x = mean + slow + rng.gauss(0, fading)
        heard = floor is None or x >= floor
        rows.append({"t": i * gap, "heard": heard, "rssi": float(round(x)) if heard else None,
                     "snr": None})
    return rows


def check(args):
    rng = random.Random(1)
    failed = []

    def expect(what, got, want, tol):
        ok = got is not None and abs(got - want) <= tol
        shown = got if got is None or isinstance(got, bool) else round(got, 2)
        print(f"{'ok' if ok else 'FAIL'}: {what}: {shown} "
              f"(want {want} +/- {tol})")
        if not ok:
            failed.append(what)

    # Fading alone: every separation gives it back, and frames do not follow one another.
    for fading in (2.0, 4.0, 6.0):
        s = summarise(made_up(rng, 4000, 20, fading, 0))
        expect(f"{fading} dB, no drift, next frame", s["sigma"][None][0][0], fading, 0.2)
        expect(f"{fading} dB, no drift, 1 h", s["sigma"][3600][0][0], fading, 0.25)
        expect(f"{fading} dB, no drift, correlation", s["r1"], 0.0, 0.06)

    # Fading on drift: the next frame still gives the fading, and the hour gives more.
    s = summarise(made_up(rng, 8000, 20, 3.0, 4.0))
    expect("3 dB on 4 dB/h of drift, next frame", s["sigma"][None][0][0], 3.0, 0.2)
    want_h = math.sqrt(3.0 ** 2 + 4.0 ** 2 / 2)
    expect("3 dB on 4 dB/h of drift, 1 h", s["sigma"][3600][0][0], round(want_h, 2), 0.4)
    expect("3 dB on 4 dB/h, whole-run sd above the hour's", s["rssi_sd"] > s["sigma"][3600][0][0],
           True, 0)

    # Rounding: whole dB adds a twelfth of a dB squared, which is taken off.
    s = summarise(made_up(rng, 20000, 20, 0.6, 0))
    expect("0.6 dB, rounding taken off", s["sigma"][None][0][0], 0.6, 0.08)

    # A link at its floor: the misses are the weak frames, and the figure comes out too small -
    # which is why analyse warns.
    rows = made_up(rng, 4000, 20, 4.0, 0, mean=-100, floor=-100)
    s = summarise(rows)
    expect("4 dB at the floor reads low", s["sigma"][None][0][0] < 3.0, True, 0)

    if failed:
        sys.exit(f"{len(failed)} failed")
    print("all passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("analyse", help="the figures for each direction of a link")
    a.add_argument("files", nargs="+")
    a.set_defaults(fn=analyse)
    c = sub.add_parser("check", help="the estimators against made-up links")
    c.set_defaults(fn=check)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
