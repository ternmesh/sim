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
    """Rows of every file, by direction and radio settings, in time order: runs at another power,
    SF or bandwidth fade differently, and are kept apart."""
    links = {}
    for path in paths:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                if r.get("time_s") in (None, "", "time_s"):
                    continue
                key = (r["sender"], r["listener"], r.get("power_dbm") or "", r.get("sf") or "",
                       r.get("bw_hz") or "")
                heard = r["heard"] == "1"
                links.setdefault(key, []).append({
                    "t": float(r["time_s"]),
                    "seq": int(r["seq"]),
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


def sigma_from_diffs(diffs, rounding=True, independent=None):
    """sd of the differences over sqrt(2), the rounding taken off; with its standard error, from
    `independent` differences where the pairs overlap and fewer than all of them are."""
    if len(diffs) < 10:
        return None, None
    var = sd(diffs) ** 2 / 2 - (ROUNDING_VAR if rounding else 0)
    s = math.sqrt(max(var, 0.0))
    n = min(len(diffs), independent) if independent is not None else len(diffs)
    return s, s / math.sqrt(2 * max(n - 1, 1)) if s > 0 else 0.0


def typical_gap(rows):
    gaps = sorted(b["t"] - a["t"] for a, b in zip(rows, rows[1:]))
    return gaps[len(gaps) // 2] if gaps else None


def runs(rows):
    """The rows cut into runs: where the numbering does not go up (run.py numbers each run from 0,
    as where runs appended to one file meet), and where a pause is longer than one and a half
    gaps (a computer that slept). Nothing is ever paired across a cut."""
    gap = typical_gap(rows)
    out = [rows[:1]] if rows else []
    for a, b in zip(rows, rows[1:]):
        if b["seq"] <= a["seq"] or b["t"] - a["t"] > 1.5 * gap:
            out.append([])
        out[-1].append(b)
    return out


def next_frames(run):
    """Pairs of frames one after the other in a run, both heard. A miss between breaks a pair:
    across it, the gap is twice as long."""
    return [(a, b) for a, b in zip(run, run[1:])
            if a["heard"] and b["heard"] and b["seq"] == a["seq"] + 1]


def next_frame_diffs(rows):
    return [b["rssi"] - a["rssi"] for run in runs(rows) for a, b in next_frames(run)]


def lag_diffs_in(heard, lag):
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


def lag_diffs(rows, lag):
    return [d for run in runs(rows) for d in lag_diffs_in([r for r in run if r["heard"]], lag)]


def lag1_correlation(rows):
    pairs = [(a["rssi"], b["rssi"]) for run in runs(rows) for a, b in next_frames(run)]
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
    s["gap"] = typical_gap(rows)
    s["hours"] = (rows[-1]["t"] - rows[0]["t"]) / 3600
    s["rssi_mean"] = sum(rssi) / len(rssi)
    s["rssi_sd"] = sd(rssi)
    s["snr_mean"] = sum(snr) / len(snr) if snr else None
    s["r1"] = lag1_correlation(rows)
    s["sigma"] = {}
    # The time the runs were recording, without the pauses between them.
    span = sum(run[-1]["t"] - run[0]["t"] for run in runs(rows))
    for lag in LAGS:
        if lag is None:
            # Neighbouring differences share a frame, which widens the error as if there were
            # two thirds as many (correlation -1/2, so 1 + 2 * (1/2)^2 = 1.5).
            diffs = next_frame_diffs(rows)
            independent = int(len(diffs) / 1.5)
        else:
            # Pairs a lag long start at nearly every frame, and share most of their interval: the
            # runs hold only about span / lag that do not overlap.
            diffs, independent = lag_diffs(rows, lag), max(int(span / lag), 1)
        s["sigma"][lag] = (sigma_from_diffs(diffs, rounding, independent), len(diffs))
    return s


def lag_name(lag, gap):
    if lag is None:
        return "next frame" + (f" ({gap:.0f} s)" if gap else "")
    return f"{lag // 3600} h" if lag >= 3600 else f"{lag // 60} min"


def analyse(args):
    links = read(args.files)
    if not links:
        sys.exit("no rows")
    print(analyse_rows(links))


def analyse_rows(links):
    out = []
    for key, rows in sorted(links.items()):
        s = summarise(rows)
        tx, rx = key[:2]
        power, sf, bw = (tuple(key[2:]) + ("", "", ""))[:3]
        radio = ", ".join(x for x in (f"SF{sf}" if sf else "", f"{bw} Hz" if bw else "",
                                      f"{power} dBm" if power else "") if x)
        out.append(f"{tx} -> {rx}{f' ({radio})' if radio else ''}: {s['heard']} of {s['sent']} "
                   "frames heard")
        if s["heard"] < 2:
            out.append("  too few frames heard to say anything")
            out.append("")
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
    return "\n".join(out).rstrip()


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
        rows.append({"t": i * gap, "seq": i, "heard": heard,
                     "rssi": float(round(x)) if heard else None, "snr": None})
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

    # Twenty short runs appended to one file, an hour apart and each at its own level: the step
    # across a pause is not a frame-to-frame one.
    rows = []
    for k in range(20):
        run = made_up(rng, 50, 20, 2.0, 0, mean=-100 + rng.uniform(-20, 20))
        for r in run:
            r["t"] += k * (50 * 20 + 3600)
        rows += run
    s = summarise(rows)
    expect("twenty runs an hour apart, next frame", s["sigma"][None][0][0], 2.0, 0.25)

    # Runs restarted at once, within a gap of the last: only the numbering tells them apart.
    rows = []
    for k in range(20):
        run = made_up(rng, 50, 20, 2.0, 0, mean=-100 + rng.uniform(-20, 20))
        for r in run:
            r["t"] += k * 50 * 20
        rows += run
    s = summarise(rows)
    expect("twenty runs back to back, next frame", s["sigma"][None][0][0], 2.0, 0.25)

    # Hour-long pairs overlap: a day holds 24 that do not, and the error says so.
    s = summarise(made_up(rng, 8640, 10, 3.0, 0))
    (sig, err), n = s["sigma"][3600]
    expect("a day's 1 h error, from 24 independent pairs", err, round(sig / math.sqrt(2 * 23), 2),
           0.01)

    # Runs restarted about an hour after the last began, alternating 20 dB apart: hour-long pairs
    # must not reach across a restart.
    rows = []
    for k in range(12):
        run = made_up(rng, 400, 10, 2.0, 0, mean=-90 if k % 2 else -110)
        for r in run:
            r["t"] += k * 3600
        rows += run
    s = summarise(rows)
    expect("runs restarted an hour apart, 1 h", s["sigma"][3600][0][0], 2.0, 0.3)

    # Two days a month apart: the month is not time the hour-long pairs could have come from.
    first, second = made_up(rng, 8640, 10, 3.0, 0), made_up(rng, 8640, 10, 3.0, 0)
    for r in second:
        r["t"] += 30 * 86400
    s = summarise(first + second)
    (sig, err), n = s["sigma"][3600]
    expect("two days a month apart, 1 h error from 48", err, round(sig / math.sqrt(2 * 47), 2),
           0.01)

    # A run stopped early: one frame, or one heard, is reported, not a crash.
    for rows in (made_up(rng, 1, 20, 2.0, 0), made_up(rng, 50, 20, 2.0, 0, mean=-130, floor=-100)):
        s = summarise(rows)
        expect(f"{s['sent']} sent, {s['heard']} heard: summarised", s["heard"] < 2, True, 0)
    analyse_rows({("A", "B"): made_up(rng, 1, 20, 2.0, 0)})

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
