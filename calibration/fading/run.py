#!/usr/bin/env python3
"""Frame-to-frame fading between two boards running Tern's firmware, one frame at a time.

The boards take turns: one sends a single test frame and the other says whether it heard it, and
at what RSSI and SNR. Each frame is its own row of CSV, so fading.py can measure how much a link's
loss varies from one frame to the next - the simulator's `phy.fading` (MSH-32, MSH-76).

    run.py PORT_A PORT_B [--gap S] [--hours H] [--power DBM] [--sf N] [--bw HZ] >> link.csv

Every firmware with the bench commands will do (0.3.0 and later): a frame's figures are read from
`counts`, reset before each frame, so the mean the firmware gives is that one frame's. Needs
pyserial. Stop it with Ctrl-C at any time; every row is written as it is measured, and the boards
are put back to routing on the way out.
"""

import argparse
import csv
import re
import signal
import sys
import time

import serial

COUNTS = (r"counts: sync 0x(\w+), (-?\d+) dBm, sent (\d+), preambles (\d+), headers (\d+), "
          r"header errors (\d+), crc errors (\d+), frames (\d+), test frames (\d+), others (\d+), "
          r"mean (-?\d+) dBm, SNR (-?\d+) cB")
COLUMNS = ["time_s", "sender", "listener", "seq", "heard", "rssi_dbm", "snr_cdb", "preambles",
           "headers", "crc_errors", "power_dbm", "sf", "bw_hz"]
RADIO = r"radio: [^\n]*?, (\d+) Hz, SF(\d+), (\d+) Hz, CR 4/\d, (-?\d+) dBm"
RX_WAIT = 2.0  # seconds a listener is given to have a frame, from the sender's "done"


class Board:
    def __init__(self, name, port):
        self.name = name
        # Raising DTR or RTS resets the board, so neither is touched.
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.05
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

    def radio(self, args):
        """The power, SF and bandwidth the board is sending with, from `status`, so a capture says
        what it was made at even where no option set it; what was asked for, where the board does
        not say."""
        try:
            m = self.ask("status", RADIO, timeout=3)
            return {"power": m.group(4), "sf": m.group(2), "bw": m.group(3)}
        except RuntimeError:
            return {"power": "" if args.power is None else args.power, "sf": args.sf or "",
                    "bw": args.bw or ""}

    def counts(self):
        g = self.ask("counts", COUNTS).groups()
        keys = ["sent", "preambles", "headers", "header_errors", "crc_errors", "frames",
                "test_frames", "others", "rssi_dbm", "snr_cdb"]
        return dict(zip(keys, (int(v) for v in g[2:])))


def stop(signum, frame):
    raise KeyboardInterrupt


def main():
    # Ended by a terminal closing or by kill as by Ctrl-C, so the boards are routing again after.
    signal.signal(signal.SIGTERM, stop)
    if hasattr(signal, "SIGHUP"):
        signal.signal(signal.SIGHUP, stop)
    ap = argparse.ArgumentParser()
    ap.add_argument("port_a")
    ap.add_argument("port_b")
    ap.add_argument("--gap", type=float, default=10.0,
                    help="seconds between one board's frames; the two take turns inside it")
    ap.add_argument("--hours", type=float, default=0, help="stop after this long; 0 for never")
    ap.add_argument("--power", type=int, default=None,
                    help="dBm to send at; low enough that the RSSI is well under -50 dBm")
    ap.add_argument("--sf", type=int, default=None, help="spreading factor, if not the profile's")
    ap.add_argument("--bw", type=int, default=None, help="bandwidth in Hz, if not the profile's")
    ap.add_argument("--header", action="store_true", help="write the CSV header row first")
    args = ap.parse_args()
    boards = [Board("A", args.port_a), Board("B", args.port_b)]
    out = csv.writer(sys.stdout)
    if args.header:
        out.writerow(COLUMNS)
        sys.stdout.flush()

    try:
        for b in boards:
            b.ask("bench on", r"bench on")
            if args.power is not None:
                b.ask(f"power {args.power}", r"power -?\d+ dBm")
            if args.sf is not None:
                b.ask(f"sf {args.sf}", r"radio: ")
            if args.bw is not None:
                b.ask(f"bw {args.bw}", r"radio: ")
        radio = {b.name: b.radio(args) for b in boards}
        start = time.time()
        seq = 0
        while args.hours <= 0 or time.time() - start < args.hours * 3600:
            turn = time.monotonic()
            for tx, rx in (boards, boards[::-1]):
                rx.ask("counts reset", r"counts reset")
                tx.ask("counts reset", r"counts reset")
                tx.ask("beacon 1 0", r"beacon: 1 frames")
                sent = int(tx.wait(r"beacon: done, (\d+) sent", 10).group(1))
                # Timed when it has gone, not when asked for: a region's limit on transmitting can
                # hold a frame back for seconds, and the lag columns pair frames by this time.
                sent_at = time.time()
                if sent != 1:
                    continue  # nothing went on the air, so nothing was missed
                # "done" comes once the frame has left the sender. The listener is asked until it
                # has taken it, or for RX_WAIT, after which it was missed.
                end = time.monotonic() + RX_WAIT
                while True:
                    time.sleep(0.25)
                    c = rx.counts()
                    if c["test_frames"] > 0 or time.monotonic() > end:
                        break
                heard = 1 if c["test_frames"] > 0 else 0
                out.writerow([f"{sent_at:.3f}", tx.name, rx.name, seq, heard,
                              c["rssi_dbm"] if heard else "", c["snr_cdb"] if heard else "",
                              c["preambles"], c["headers"], c["crc_errors"],
                              radio[tx.name]["power"], radio[tx.name]["sf"],
                              radio[tx.name]["bw"]])
                sys.stdout.flush()
            seq += 1
            left = args.gap - (time.monotonic() - turn)
            if left > 0:
                time.sleep(left)
    except KeyboardInterrupt:
        pass
    finally:
        for b in boards:
            try:
                b.ask("bench off", r"bench off")
            except (RuntimeError, serial.SerialException) as e:
                print(e, file=sys.stderr)
        if args.sf is not None or args.bw is not None or args.power is not None:
            print("the boards are routing again, at the power, SF or bandwidth set here: reset "
                  "them to go back to their own", file=sys.stderr)


if __name__ == "__main__":
    main()
