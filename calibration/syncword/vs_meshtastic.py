#!/usr/bin/env python3
"""Sync-word cross-reception between Tern's Heltec V3 demo and a board running Meshtastic.

The Tern board is put on the Meshtastic board's channel and modulation. Then, for each sync word
given, the Meshtastic board sends text messages while the Tern board listens with that word, and
the Tern board sends test frames with that word while the Meshtastic board listens, at each
power. Meshtastic's own word, 0x2B, should come first and last: it is the control. What the
Meshtastic board sent and received is read from its debug log.

    vs_meshtastic.py TERN_PORT MESHTASTIC_PORT --freq 926875000 --sf 11 --bw 250000 > out.csv

Before it, on the Meshtastic board: a region, a private channel on a frequency slot nobody
near uses, hop_limit 0, and security.debug_log_api_enabled true. Needs the meshtastic package,
and run.py beside this file.
"""

import argparse
import csv
import re
import sys
import time

import meshtastic.serial_interface
from pubsub import pub

import run

log = []


def on_log(line, interface=None):  # a named function: pubsub keeps only a weak reference
    log.append(line)


def count(lines, pattern):
    return sum(1 for line in lines if re.search(pattern, line))


def stats(lines):
    """The last of the radio's running totals in these lines: (rxGood, rxBad)."""
    for line in reversed(lines):
        m = re.search(r"rxGood=(\d+),rxBad=(\d+)", line)
        if m:
            return int(m.group(1)), int(m.group(2))
    raise RuntimeError("the Meshtastic board logged no totals")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tern_port")
    ap.add_argument("meshtastic_port")
    ap.add_argument("--freq", type=int, required=True)
    ap.add_argument("--sf", type=int, required=True)
    ap.add_argument("--bw", type=int, required=True)
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--gap", type=int, default=1000, help="ms between Tern's frames")
    ap.add_argument("--text-gap", type=float, default=2.0, help="s between Meshtastic's texts")
    ap.add_argument("--powers", default="2,22")
    ap.add_argument("--words", default="2B,5E,67,12,34,2B")
    ap.add_argument("--idle", type=int, default=0, help="s to listen first with nothing sent")
    ap.add_argument("--log", help="a file for the Meshtastic board's log")
    args = ap.parse_args()
    words = [int(w, 16) for w in args.words.split(",")]
    powers = [int(p) for p in args.powers.split(",")]

    pub.subscribe(on_log, "meshtastic.log.line")
    tern = run.Board("tern", args.tern_port)
    out = csv.writer(sys.stdout)
    out.writerow(["sender", "listener", "tx_sync", "rx_sync", "power_dbm", "sent", "preambles",
                  "headers", "header_errors", "crc_errors", "frames", "false_preambles"])

    def connect():
        m = meshtastic.serial_interface.SerialInterface(args.meshtastic_port)
        time.sleep(4)
        return m

    def totals(m):
        """Has the Meshtastic board log its totals, which it does when it sends."""
        start = len(log)
        m.sendText("totals")
        time.sleep(3)
        return stats(log[start:])

    mesh = None
    try:
        tern.ask("bench on", r"bench on")
        # Bandwidth first if it narrows, so that the channel stays inside the band.
        for c in (f"bw {args.bw}", f"freq {args.freq}", f"bw {args.bw}", f"sf {args.sf}"):
            tern.ask(c, r"radio: \d+ Hz|inside the band|band,")
        m = tern.ask("sf %d" % args.sf, r"radio: (\d+) Hz, SF(\d+), (\d+) Hz")
        if [int(v) for v in m.groups()] != [args.freq, args.sf, args.bw]:
            raise RuntimeError(f"the Tern board is on {m.group(0)}")
        mesh = connect()
        if args.idle:
            # What the Meshtastic board receives with the Tern board silent: other people's.
            good, bad = totals(mesh)
            start = len(log)
            time.sleep(args.idle)
            heard = log[start:]
            good2, bad2 = totals(mesh)
            out.writerow(["none", "meshtastic", "", "0x2B", "", 0, "", "", "", bad2 - bad,
                          good2 - good, count(heard, r"false preamble")])
            sys.stdout.flush()
        for power in powers:
            if mesh.localNode.localConfig.lora.tx_power != power:
                mesh.localNode.localConfig.lora.tx_power = power
                mesh.localNode.writeConfig("lora")
                time.sleep(2)
                mesh.close()
                time.sleep(25)  # it restarts
                mesh = connect()
            tern.ask(f"power {power}", r"power -?\d+ dBm")
            for w in words:
                tern.ask(f"sync {w:X}", r"sync word 0x")

                # Meshtastic sends, Tern listens.
                tern.ask("counts reset", r"counts reset")
                start = len(log)
                for i in range(args.frames):
                    mesh.sendText(f"sync word test {i}")
                    time.sleep(args.text_gap)
                time.sleep(2)
                c = tern.counts()
                out.writerow(["meshtastic", "tern", "0x2B", f"0x{w:02X}", power,
                              count(log[start:], r"^Started Tx"), c["preambles"], c["headers"],
                              c["header_errors"], c["crc_errors"], c["frames"], ""])
                sys.stdout.flush()

                # Tern sends, Meshtastic listens.
                good, bad = totals(mesh)
                tern.ask("counts reset", r"counts reset")
                start = len(log)
                tern.ask(f"beacon {args.frames} {args.gap}", r"beacon: \d+ frames")
                sent = int(tern.wait(r"beacon: done, (\d+) sent",
                                     args.frames * args.gap / 1000 + 30).group(1))
                time.sleep(2)
                heard = log[start:]
                good2, bad2 = totals(mesh)
                if good2 - good != count(heard, r"^Lora RX"):
                    print(f"0x{w:02X}: the totals say {good2 - good} received, the log "
                          f"{count(heard, r'^Lora RX')}", file=sys.stderr)
                out.writerow(["tern", "meshtastic", f"0x{w:02X}", "0x2B", power, sent, "", "", "",
                              bad2 - bad, good2 - good, count(heard, r"false preamble")])
                sys.stdout.flush()
    finally:
        if args.log:
            with open(args.log, "w") as f:
                f.write("\n".join(log) + "\n")
        if mesh is not None:
            mesh.close()
        try:
            tern.ask("bench off", r"bench off")
        except RuntimeError as e:
            print(e, file=sys.stderr)
        print("the Tern board is still off its region's channel: restart it", file=sys.stderr)


if __name__ == "__main__":
    main()
