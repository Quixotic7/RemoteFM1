#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""The standalone DX7's bench on an FM-1 running RemoteFM1 (firmware/sa/sa_bench.c), over USB-MIDI SysEx with
python-rtmidi: what a block of FM6 costs, as a share of the block's real time.

  firmware/tools/sa_bench.py [--port FM-1]            ask; if the bench never ran, start it, then wait for it
  firmware/tools/sa_bench.py --start [--port FM-1]    start it again (any build: 1 and 8 voices; the bench build,
                                                      ./build.sh --bench, runs 1, 8, 12, 16 at boot by itself)
  firmware/tools/sa_bench.py --once                   ask once and print, do not wait
  firmware/tools/sa_bench.py --selftest               the reply's decoder on a made-up reply (no MIDI)

The message: F0 7D 52 46 09 <body, 7-in-8> F7 (firmware/src/remote_proto.h SA_BENCH; body 01 starts); the reply
SA_BENCH_R 0x49, its layout in firmware/sa/sa_bench.c. The run takes about 3 s of audio a voice count, longer in the
main loop's slices (half a minute at most). Exits 0 once the numbers are in, 1 on no answer, 2 if the unit does not
know SA_BENCH (an older RemoteFM1 or another firmware: it ignores it).
"""
import argparse
import struct
import sys
import time

SX_HEAD = bytes([0xF0, 0x7D, 0x52, 0x46])
SA_BENCH, SA_BENCH_R = 0x09, 0x49
STATES = {0: "never asked", 1: "waiting to start", 2: "running", 3: "done"}


def pack7(b):
    out = bytearray()
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << k for k, x in enumerate(g)))
        out += bytes(x & 0x7F for x in g)
    return bytes(out)


def unpack7(b):
    out = bytearray()
    for i in range(0, len(b), 8):
        g = b[i:i + 8]
        out += bytes(x | (((g[0] >> k) & 1) << 7) for k, x in enumerate(g[1:]))
    return bytes(out)


def decode(body):
    """SA_BENCH_R's body -> dict"""
    if len(body) < 10:
        raise ValueError(f"SA_BENCH_R of {len(body)} bytes")
    state, poly, flags, n, frames, budget = struct.unpack_from("<BBBBHI", body, 0)
    rows = []
    for k in range(n):
        voices, sounding, blocks, avg, worst = struct.unpack_from("<BBHII", body, 10 + 12 * k)
        rows.append(dict(voices=voices, sounding=sounding, blocks=blocks, avg=avg, worst=worst,
                         avg_share=avg / budget, worst_share=worst / budget))
    return dict(state=state, poly=poly, bench_build=bool(flags & 1), frames=frames, budget=budget, rows=rows)


def report(r):
    us = r["budget"] / 24.0
    lines = [f"FM6 bench: {STATES.get(r['state'], r['state'])}; SA_POLY {r['poly']}"
             f"{' (bench build)' if r['bench_build'] else ''}; a block is {r['frames']} frames, {us:.0f} us"]
    if r["rows"]:
        lines.append("  voices  sounding  blocks   average            worst")
        for row in r["rows"]:
            lines.append(f"  {row['voices']:6}  {row['sounding']:8}  {row['blocks']:6}   "
                         f"{row['avg_share'] * 100:5.1f} % ({row['avg'] / 24:6.0f} us)   "
                         f"{row['worst_share'] * 100:5.1f} % ({row['worst'] / 24:6.0f} us)")
        one = [x for x in r["rows"] if x["voices"] == 1]
        many = [x for x in r["rows"] if x["voices"] > 1]
        if one and many:
            m = many[-1]
            per = (m["avg_share"] - one[0]["avg_share"]) / (m["voices"] - 1)
            lines.append(f"  a voice more: about {per * 100:.1f} % of the block (average)")
    return "\n".join(lines)


class Link:
    def __init__(self, port):
        import rtmidi
        self.inp, self.out = rtmidi.MidiIn(), rtmidi.MidiOut()
        ins, outs = self.inp.get_ports(), self.out.get_ports()

        def pick(names):
            hits = [i for i, n in enumerate(names) if port.lower() in n.lower() and "update" not in n.lower()]
            exact = [i for i in hits if names[i].lower() == port.lower()]
            return (exact or hits or [None])[0]
        i, o = pick(ins), pick(outs)
        if i is None or o is None:
            raise SystemExit(f"sa_bench: no MIDI port matching {port!r} (inputs {ins}, outputs {outs})")
        self.name, self.acc, self.got = ins[i], b"", []
        self.inp.ignore_types(sysex=False, timing=True, active_sense=True)
        self.inp.set_callback(self._rx)
        self.inp.open_port(i)
        self.out.open_port(o)
        time.sleep(0.3)                 # (CoreMIDI may drop a message sent just after opening)

    def _rx(self, ev, _):
        msg = bytes(ev[0])
        if not msg or (msg[0] != 0xF0 and not self.acc):
            return
        buf = self.acc + msg
        while (s := buf.find(0xF0)) >= 0 and (e := buf.find(0xF7, s)) >= 0:
            m = buf[s:e + 1]
            if m[:4] == SX_HEAD and len(m) > 5 and m[4] == SA_BENCH_R:
                self.got.append(unpack7(m[5:-1]))
            buf = buf[e + 1:]
        self.acc = buf if buf.find(0xF0) >= 0 else b""

    def ask(self, body=b"", timeout=1.5):
        self.got.clear()
        self.out.send_message(list(SX_HEAD + bytes([SA_BENCH]) + pack7(body) + b"\xF7"))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.got:
                return decode(self.got[-1])
            time.sleep(0.02)
        return None


def selftest():
    rows = [(1, 1, 516, 9000, 12000), (8, 8, 516, 50000, 61000)]
    body = struct.pack("<BBBBHI", 3, 8, 0, len(rows), 256, 139319)
    for row in rows:
        body += struct.pack("<BBHII", *row)
    assert unpack7(pack7(body)) == body and unpack7(pack7(bytes(range(256)))) == bytes(range(256))
    r = decode(unpack7(pack7(body)))
    assert r["state"] == 3 and r["poly"] == 8 and len(r["rows"]) == 2 and r["rows"][1]["sounding"] == 8
    assert abs(r["rows"][1]["avg_share"] - 50000 / 139319) < 1e-9
    print(report(r))
    print("sa_bench: selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="FM-1", help="a part of the MIDI port's name (default FM-1)")
    ap.add_argument("--start", action="store_true", help="start the bench again")
    ap.add_argument("--once", action="store_true", help="ask once, do not wait")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    link = Link(a.port)
    r = link.ask(b"\x01" if a.start else b"")
    if r is None:
        print(f"sa_bench: no SA_BENCH_R from {link.name} (not RemoteFM1 with the standalone DX7?)")
        return 2
    if r["state"] == 0 and not a.once:
        r = link.ask(b"\x01")
    t0 = time.monotonic()
    while not a.once and r and r["state"] != 3 and time.monotonic() - t0 < 90:
        print(f"  {STATES.get(r['state'], r['state'])}: {len(r['rows'])} voice counts measured", flush=True)
        time.sleep(3)
        r = link.ask() or r
    print(report(r))
    return 0 if r["state"] == 3 or a.once else 1


if __name__ == "__main__":
    sys.exit(main())
