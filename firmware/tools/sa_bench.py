#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""The standalone DX7's bench on an FM-1 running RemoteFM1 (firmware/sa/sa_bench.c), over USB-MIDI SysEx with
python-rtmidi: what a block of FM6 costs, as a share of the block's real time, on one core and on two; and the render
hash, the FM-1's samples against the computer's.

  firmware/tools/sa_bench.py [--port FM-1]            ask; if the bench never ran, start it, then wait for it
  firmware/tools/sa_bench.py --start [--port FM-1]    start it again (any build: 1 and 8 voices; the bench build,
                                                      ./build.sh --bench, runs 1, 8, 12, 16 at boot by itself), on
                                                      one core and, when the second core runs, on two
  firmware/tools/sa_bench.py --hash [--port FM-1]     the render hash (SA_BENCH 02): firmware/sa/sa_hash.h's
                                                      performance on the FM-1 (one core, two), and on this computer
                                                      (firmware/tools/sa_hash_host.c, built here with cc at the unit's
                                                      SA_POLY): "MATCH" when they are equal
  firmware/tools/sa_bench.py --once                   ask once and print, do not wait
  firmware/tools/sa_bench.py --selftest               the reply's decoder on a made-up reply, and the host hash (no MIDI)

The message: F0 7D 52 46 09 <body, 7-in-8> F7 (firmware/src/remote_proto.h SA_BENCH; body 01 starts the bench, 02 the
hash); the reply SA_BENCH_R 0x49, its layout in firmware/sa/sa_bench.c (layout 2). The bench takes about 3 s of audio a
row, longer in the main loop's slices (a minute at most); the hash a few seconds. Exits 0 once the numbers are in (with
--hash: and they match), 1 on no answer or a mismatch, 2 if the unit does not know SA_BENCH (an older RemoteFM1 or
another firmware: it ignores it).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SX_HEAD = bytes([0xF0, 0x7D, 0x52, 0x46])
SA_BENCH, SA_BENCH_R = 0x09, 0x49
STATES = {0: "never asked", 1: "waiting to start", 2: "running", 3: "done"}
GAVE_UP = {0: "", 1: "misread a job", 2: "a job late", 3: "barred", 4: "faulted", 5: "did not answer at its start"}
FW = Path(__file__).resolve().parents[1]


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
    """SA_BENCH_R's body (layout 2) -> dict"""
    if len(body) < 48 or body[0] != 2:
        raise ValueError(f"SA_BENCH_R of {len(body)} bytes, layout {body[0] if body else '?'} (this tool reads 2)")
    (_, state, poly, flags, n, gave_up, frames, budget, misreads, stack, hstate, hn, _, h1, h2, hblocks, handed, kept,
     dropped) = struct.unpack_from("<BBBBBBHIIIBBHIIIIII", body, 0)
    rows = []
    for k in range(n):
        voices, cores, sounding, _, blocks, avg, worst = struct.unpack_from("<BBBBIII", body, 48 + 16 * k)
        rows.append(dict(voices=voices, cores=cores, sounding=sounding, blocks=blocks, avg=avg, worst=worst,
                         avg_share=avg / budget, worst_share=worst / budget))
    return dict(state=state, poly=poly, bench_build=bool(flags & 1), c1_built=bool(flags & 2), c1_on=bool(flags & 4),
                gave_up=gave_up, misreads=misreads, c1_stack=stack, frames=frames, budget=budget, rows=rows,
                hstate=hstate, hashes=[h1, h2][:hn], hblocks=hblocks, handed=handed, kept=kept, dropped=dropped)


def report(r):
    us = r["budget"] / 24.0
    c1 = ("core 1 on" if r["c1_on"] else "core 1 off" + (f" ({GAVE_UP.get(r['gave_up'], r['gave_up'])})"
                                                       if r["gave_up"] else "")) if r["c1_built"] else "one core"
    lines = [f"FM6 bench: {STATES.get(r['state'], r['state'])}; SA_POLY {r['poly']}"
             f"{' (bench build)' if r['bench_build'] else ''}; {c1}; a block is {r['frames']} frames, {us:.0f} us"]
    if r["c1_built"]:
        lines.append(f"  core 1: blocks it took {r['handed']}, did not take {r['kept']}, dropped {r['dropped']}; "
                     f"misreads {r['misreads']}; its stack {r['c1_stack']} B")
    if r["rows"]:
        lines.append("  voices  cores  sounding  blocks   average            worst")
        for row in r["rows"]:
            lines.append(f"  {row['voices']:6}  {row['cores']:5}  {row['sounding']:8}  {row['blocks']:6}   "
                         f"{row['avg_share'] * 100:5.1f} % ({row['avg'] / 24:6.0f} us)   "
                         f"{row['worst_share'] * 100:5.1f} % ({row['worst'] / 24:6.0f} us)")
        for cores in (1, 2):
            rs = [x for x in r["rows"] if x["cores"] == cores]
            one = [x for x in rs if x["voices"] == 1]
            many = [x for x in rs if x["voices"] > 1]
            if one and many:
                m = many[-1]
                per = (m["avg_share"] - one[0]["avg_share"]) / (m["voices"] - 1)
                lines.append(f"  {cores} core{'s' if cores > 1 else ''}: about {per * 100:.1f} % of the block a voice "
                             "more (average)")
    if r["hstate"]:
        lines.append(f"  render hash: {STATES.get(r['hstate'], r['hstate'])}: " +
                     ", ".join(f"{c + 1} core{'s' if c else ''} {h:08x}" for c, h in enumerate(r["hashes"])))
    return "\n".join(lines)


def host_hash(poly):
    """sa_hash_host.c built here at SA_POLY poly: (one core's hash, two cores'), int"""
    with tempfile.TemporaryDirectory() as d:
        gen = Path(d)
        subprocess.run([sys.executable, str(FW / "tools/gen_fm6_tables.py"), "--out", str(gen / "fm6_tables.h")],
                       check=True, capture_output=True)
        exe = gen / "sa_hash_host"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O2", "-ffp-contract=off", "-w", "-DPLAT_HOST",
                        f"-DSA_POLY={poly}", f"-I{gen}", f"-I{FW / 'sa'}", "-o", str(exe),
                        str(FW / "tools/sa_hash_host.c"), str(FW / "sa/sa_synth.c"), "-lm"], check=True)
        return tuple(int(subprocess.run([str(exe), str(c)], check=True, capture_output=True, text=True)
                         .stdout.split()[1], 16) for c in (1, 2))


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
    body = bytearray(48)
    struct.pack_into("<BBBBBBHIIIBBHIIIIII", body, 0, 2, 3, 16, 7, 2, 0, 256, 139319, 0, 640, 3, 2, 0,
                     0x12345678, 0x12345678, 4134, 4000, 2, 0)
    for row in [(1, 1, 1, 0, 516, 9000, 12000), (8, 1, 8, 0, 516, 50000, 61000), (8, 2, 8, 0, 516, 27000, 33000)]:
        body += struct.pack("<BBBBIII", *row)
    body[4] = 3
    assert unpack7(pack7(bytes(body))) == bytes(body) and unpack7(pack7(bytes(range(256)))) == bytes(range(256))
    r = decode(unpack7(pack7(bytes(body))))
    assert r["state"] == 3 and r["poly"] == 16 and r["c1_on"] and len(r["rows"]) == 3 and r["rows"][2]["cores"] == 2
    assert abs(r["rows"][1]["avg_share"] - 50000 / 139319) < 1e-9 and r["hashes"] == [0x12345678, 0x12345678]
    print(report(r))
    h = {p: host_hash(p) for p in (8, 16)}
    assert all(a == b for a, b in h.values()), h
    print("sa_bench: host hashes " + ", ".join(f"SA_POLY {p} {a:08x}" for p, (a, _) in h.items()) +
          " (one core = two cores)")
    print("sa_bench: selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="FM-1", help="a part of the MIDI port's name (default FM-1)")
    ap.add_argument("--start", action="store_true", help="start the bench again")
    ap.add_argument("--hash", action="store_true", help="the render hash, against the computer's")
    ap.add_argument("--once", action="store_true", help="ask once, do not wait")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    link = Link(a.port)
    r = link.ask(b"\x02" if a.hash else b"\x01" if a.start else b"")
    if r is None:
        print(f"sa_bench: no SA_BENCH_R from {link.name} (not RemoteFM1 with the standalone DX7?)")
        return 2
    if not a.hash and r["state"] == 0 and not a.once:
        r = link.ask(b"\x01")
    key = "hstate" if a.hash else "state"
    t0 = time.monotonic()
    while not a.once and r and r[key] != 3 and time.monotonic() - t0 < 120:
        what = f"{len(r['hashes'])} hashes" if a.hash else f"{len(r['rows'])} rows"
        print(f"  {STATES.get(r[key], r[key])}: {what}", flush=True)
        time.sleep(3)
        r = link.ask() or r
    print(report(r))
    if a.hash and r["hstate"] == 3:
        mine = host_hash(r["poly"])
        ok = all(h == mine[i] for i, h in enumerate(r["hashes"]))
        print(f"  this computer, SA_POLY {r['poly']}: 1 core {mine[0]:08x}, 2 cores {mine[1]:08x}: "
              + ("MATCH" if ok else "MISMATCH"))
        return 0 if ok else 1
    return 0 if r[key] == 3 or a.once else 1


if __name__ == "__main__":
    sys.exit(main())
