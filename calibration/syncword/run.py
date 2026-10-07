#!/usr/bin/env python3
"""Sync-word cross-reception between two boards running Tern's Heltec V3 demo.

One board sends test frames with one sync word while the other listens with another, for every
pair of the words given, at each power, in both directions. The listener's counts of preambles
detected, headers that checked, errors and whole frames are written as CSV.

    run.py PORT_A PORT_B [--frames N] [--gap MS] [--powers -9,22] [--words 5E,67,2B,12,34] > out.csv

Needs pyserial and the firmware's bench commands (ternmesh/firmware, ports/heltec-v3).
"""

import argparse
import csv
import re
import sys
import time

import serial


class Board:
    def __init__(self, name, port):
        self.name = name
        # Raising DTR or RTS resets the board, so neither is touched.
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.1
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.buf = ""

    def read(self):
        self.buf += self.s.read(4096).decode("utf-8", "replace")

    def ask(self, line, expect, timeout=10.0):
        """Sends a command and returns the match of `expect` in what comes back."""
        self.read()
        self.buf = ""
        self.s.write(line.encode() + b"\n")
        return self.wait(expect, timeout, line)

    def wait(self, expect, timeout, what=""):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.read()
            m = re.search(expect, self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
        raise RuntimeError(f"board {self.name}: no '{expect}' after '{what}': {self.buf[-300:]!r}")

    def counts(self):
        m = self.ask("counts", r"counts: sync 0x(\w+), (-?\d+) dBm, sent (\d+), preambles (\d+), "
                     r"headers (\d+), header errors (\d+), crc errors (\d+), frames (\d+), "
                     r"test frames (\d+), others (\d+), mean (-?\d+) dBm, SNR (-?\d+) cB")
        keys = ["sent", "preambles", "headers", "header_errors", "crc_errors", "frames",
                "test_frames", "others", "rssi_dbm", "snr_cdb"]
        return dict(zip(keys, (int(v) for v in m.groups()[2:])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port_a")
    ap.add_argument("port_b")
    ap.add_argument("--frames", type=int, default=100)
    ap.add_argument("--gap", type=int, default=150, help="ms between the starts of frames")
    ap.add_argument("--powers", default="-9,22")
    ap.add_argument("--words", default="5E,67,2B,12,34")
    args = ap.parse_args()
    words = [int(w, 16) for w in args.words.split(",")]
    powers = [int(p) for p in args.powers.split(",")]
    boards = [Board("A", args.port_a), Board("B", args.port_b)]

    out = csv.writer(sys.stdout)
    out.writerow(["sender", "listener", "tx_sync", "rx_sync", "power_dbm", "sent", "preambles",
                  "headers", "header_errors", "crc_errors", "frames", "test_frames", "others",
                  "rssi_dbm", "snr_cdb"])
    span = args.frames * args.gap / 1000.0

    def row(tx, rx, t, r, power, sent):
        c = rx.counts()
        out.writerow([tx.name if tx else "none", rx.name, f"0x{t:02X}" if tx else "", f"0x{r:02X}",
                      power if tx else "", sent, c["preambles"], c["headers"], c["header_errors"],
                      c["crc_errors"], c["frames"], c["test_frames"], c["others"],
                      c["rssi_dbm"] if c["test_frames"] else "",
                      c["snr_cdb"] if c["test_frames"] else ""])
        sys.stdout.flush()

    try:
        for b in boards:
            b.ask("bench on", r"bench on")
        # What each listener counts with nobody sending, for as long as a run takes.
        for rx in boards:
            for r in words:
                rx.ask(f"sync {r:X}", r"sync word 0x")
                rx.ask("counts reset", r"counts reset")
                time.sleep(span)
                row(None, rx, 0, r, 0, 0)
        for tx, rx in (boards, boards[::-1]):
            for power in powers:
                tx.ask(f"power {power}", r"power -?\d+ dBm")
                for t in words:
                    tx.ask(f"sync {t:X}", r"sync word 0x")
                    for r in words:
                        rx.ask(f"sync {r:X}", r"sync word 0x")
                        rx.ask("counts reset", r"counts reset")
                        tx.ask("counts reset", r"counts reset")
                        tx.ask(f"beacon {args.frames} {args.gap}", r"beacon: \d+ frames")
                        sent = int(tx.wait(r"beacon: done, (\d+) sent", span + 30).group(1))
                        time.sleep(0.3)
                        row(tx, rx, t, r, power, sent)
    finally:
        for b in boards:
            try:
                b.ask("sync 5E", r"sync word 0x")
                b.ask("power 2", r"power")
                b.ask("bench off", r"bench off")
            except RuntimeError as e:
                print(e, file=sys.stderr)


if __name__ == "__main__":
    main()
