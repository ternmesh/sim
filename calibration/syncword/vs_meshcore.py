#!/usr/bin/env python3
"""Sync-word cross-reception between Tern's Heltec V3 demo and a board running MeshCore.

The MeshCore board, a companion reached over TCP, is moved to a frequency nobody near uses, and
the Tern board put on it with the same modulation. Then, for each sync word given, the MeshCore
board sends adverts that go no further than one hop while the Tern board listens with that word,
and the Tern board sends test frames with that word while the MeshCore board listens, at each
power. MeshCore's own word, 0x12, should come first and last: it is the control. What the
MeshCore board sent and received is read from its counters. Its radio settings are put back at
the end.

    vs_meshcore.py TERN_PORT MESHCORE_HOST --freq 926500000 --sf 7 --bw 62500 > out.csv

Needs the meshcore package, and run.py beside this file.
"""

import argparse
import asyncio
import csv
import sys

from meshcore import MeshCore

import run



class Companion:
    """The MeshCore board. Its TCP connection goes quiet every few seconds on the board tried, so
    every call is given a few seconds and, failing, a new connection. Only the board's own
    counters are trusted: they outlast a connection."""

    def __init__(self, host, port):
        self.host, self.port, self.mc = host, port, None

    async def connect(self):
        if self.mc is not None:
            try:
                await asyncio.wait_for(self.mc.disconnect(), 5)
            except (asyncio.TimeoutError, OSError):
                pass
            self.mc = None
        for _ in range(20):
            try:
                self.mc = await asyncio.wait_for(MeshCore.create_tcp(self.host, self.port), 20)
            except (asyncio.TimeoutError, OSError):
                self.mc = None
            if self.mc is not None and self.mc.self_info:
                return
            await asyncio.sleep(3)
        raise RuntimeError("the MeshCore board does not take a connection")

    async def call(self, name, *a, retry=True, **kw):
        """A command's answer, or None if it was not to be tried again and got none."""
        for _ in range(10):
            try:
                r = await asyncio.wait_for(getattr(self.mc.commands, name)(*a, **kw), 6)
                if "reason" not in r.payload:
                    return r.payload
            except (asyncio.TimeoutError, OSError):
                pass
            await self.connect()
            if not retry:
                return None
        raise RuntimeError(f"the MeshCore board does not answer {name}")

    async def packets(self):
        return await self.call("get_stats_packets")

    async def adverts(self, n, gap):
        """Sends adverts that go one hop until the board's count of frames sent is n more. An
        advert whose answer was lost may have gone, so the count decides, not the answers."""
        start = (await self.packets())["sent"]
        for _ in range(4 * n):
            if (await self.packets())["sent"] - start >= n:
                return
            await self.call("send_advert", flood=False, retry=False)
            await asyncio.sleep(gap)
        raise RuntimeError("the MeshCore board would not send its adverts")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tern_port")
    ap.add_argument("meshcore_host")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--freq", type=int, required=True)
    ap.add_argument("--sf", type=int, required=True)
    ap.add_argument("--bw", type=int, required=True)
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--gap", type=int, default=500, help="ms between Tern's frames")
    ap.add_argument("--advert-gap", type=float, default=2.0, help="s between MeshCore's adverts")
    ap.add_argument("--powers", default="2,22")
    ap.add_argument("--words", default="12,5E,67,2B,34,12")
    ap.add_argument("--idle", type=int, default=0, help="s to listen first with nothing sent")
    args = ap.parse_args()
    words = [int(w, 16) for w in args.words.split(",")]
    powers = [int(p) for p in args.powers.split(",")]

    tern = run.Board("tern", args.tern_port)
    out = csv.writer(sys.stdout)
    out.writerow(["sender", "listener", "tx_sync", "rx_sync", "power_dbm", "sent", "preambles",
                  "headers", "header_errors", "crc_errors", "frames"])

    mc = Companion(args.meshcore_host, args.port)
    await mc.connect()
    was = dict(mc.mc.self_info)
    try:
        await mc.call("set_radio", args.freq / 1e6, args.bw / 1e3, args.sf, was["radio_cr"])
        tern.ask("bench on", r"bench on")
        # Bandwidth first if it narrows, so that the channel stays inside the band.
        for c in (f"bw {args.bw}", f"freq {args.freq}", f"bw {args.bw}", f"sf {args.sf}"):
            tern.ask(c, r"radio: \d+ Hz|inside the band|band,")
        m = tern.ask("sf %d" % args.sf, r"radio: (\d+) Hz, SF(\d+), (\d+) Hz")
        if [int(v) for v in m.groups()] != [args.freq, args.sf, args.bw]:
            raise RuntimeError(f"the Tern board is on {m.group(0)}")

        if args.idle:
            # What the MeshCore board receives with the Tern board silent: other people's.
            before = await mc.packets()
            await asyncio.sleep(args.idle)
            after = await mc.packets()
            out.writerow(["none", "meshcore", "", "0x12", "", 0, "", "", "",
                          after["recv_errors"] - before["recv_errors"],
                          after["recv"] - before["recv"]])
            sys.stdout.flush()

        for power in powers:
            await mc.call("set_tx_power", power)
            tern.ask(f"power {power}", r"power -?\d+ dBm")
            for w in words:
                tern.ask(f"sync {w:X}", r"sync word 0x")

                # MeshCore sends, Tern listens.
                before = await mc.packets()
                tern.ask("counts reset", r"counts reset")
                await mc.adverts(args.frames, args.advert_gap)
                await asyncio.sleep(1)
                after = await mc.packets()
                c = tern.counts()
                out.writerow(["meshcore", "tern", "0x12", f"0x{w:02X}", power,
                              after["sent"] - before["sent"], c["preambles"], c["headers"],
                              c["header_errors"], c["crc_errors"], c["frames"]])
                sys.stdout.flush()

                # Tern sends, MeshCore listens.
                before = await mc.packets()
                tern.ask("counts reset", r"counts reset")
                tern.ask(f"beacon {args.frames} {args.gap}", r"beacon: \d+ frames")
                while True:
                    try:
                        sent = int(tern.wait(r"beacon: done, (\d+) sent", 0.5).group(1))
                        break
                    except RuntimeError:
                        await asyncio.sleep(0.2)
                await asyncio.sleep(2)
                after = await mc.packets()
                out.writerow(["tern", "meshcore", f"0x{w:02X}", "0x12", power, sent, "", "", "",
                              after["recv_errors"] - before["recv_errors"],
                              after["recv"] - before["recv"]])
                sys.stdout.flush()
    finally:
        await mc.call("set_radio", was["radio_freq"], was["radio_bw"], was["radio_sf"],
                      was["radio_cr"])
        await mc.call("set_tx_power", was["tx_power"])
        await mc.connect()
        now = mc.mc.self_info
        print(f"the MeshCore board is back on {now['radio_freq']} MHz, {now['tx_power']} dBm",
              file=sys.stderr)
        await mc.mc.disconnect()
        try:
            tern.ask("bench off", r"bench off")
        except RuntimeError as e:
            print(e, file=sys.stderr)
        print("the Tern board is still off its region's channel: restart it", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
