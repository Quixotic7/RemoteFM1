#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""The standalone's 16 loops to and from a computer (firmware/sa/sa_loop.c; docs/PROTOCOL.md section 8, SA_LOOP),
over USB-MIDI SysEx with python-rtmidi. The loops live in the unit's RAM only: save them before a power-off.

  tools/sa_loops.py info [--port FM-1]               each loop's events, length, tempo, bars
  tools/sa_loops.py save FILE.json [--port FM-1]     every loop with events into FILE.json
  tools/sa_loops.py load FILE.json [--port FM-1]     the loops in FILE.json into the unit (others left as they are)
  tools/sa_loops.py clear N [--port FM-1]            loop N (1..16) emptied
  tools/sa_loops.py --selftest                       the codec against a model of the unit (no MIDI)

The file: {"format": "remotefm1-loops", "version": 1, "loops": [{"slot": 1..16, "ms": length, "bpm": tempo,
"bars": 0 (free) or 1..8, "events": [[t_ms, "on"|"off"|"cc", note_or_cc, velocity_or_value], ...]}, ...]}.
Over Bluetooth MIDI a reply must find room on the link: use USB (or --chunk 6).
"""
import argparse
import json
import struct
import sys
import time

SX_HEAD = bytes([0xF0, 0x7D, 0x52, 0x46])
SA_LOOP, SA_LOOP_R = 0x0A, 0x4A
TYPES = ["on", "off", "cc"]
NLOOPS = 16


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


def frame(cmd, body):
    return SX_HEAD + bytes([cmd]) + pack7(body) + b"\xF7"


def ev_bytes(e):
    t, ty, d1, d2 = e
    return struct.pack("<IBBBB", int(t), TYPES.index(ty), int(d1) & 0x7F, int(d2) & 0x7F, 0)


def ev_of(b):
    t, ty, d1, d2, _ = struct.unpack("<IBBBB", b)
    return [t, TYPES[min(ty, 2)], d1, d2]


def req_info():
    return bytes([0])


def parse_info(r):
    loops = []
    for i in range(r[3]):
        n, ms, bpm, bars = struct.unpack_from("<HIHB", r, 4 + 9 * i)
        loops.append(dict(slot=i + 1, n=n, ms=ms, bpm=bpm, bars=bars))
    return dict(state=r[1], cur=r[2] + 1, loops=loops)


def req_get(slot, first, chunk):
    return struct.pack("<BBHB", 1, slot - 1, first, chunk)


def parse_get(r):
    _, slot, first, total, ms, bpm, bars, c = struct.unpack_from("<BBHHIHBB", r, 0)
    return first, total, [ev_of(r[14 + 8 * i:22 + 8 * i]) for i in range(c)]


def reqs_put(loop, chunk):
    """the messages that write one loop: BEGIN, EVENTS..., END"""
    s = loop["slot"] - 1
    out = [struct.pack("<BBIHB", 2, s, loop["ms"], loop.get("bpm", 120), loop.get("bars", 0))]
    ev = loop["events"]
    for i in range(0, len(ev), chunk):
        part = ev[i:i + chunk]
        out.append(struct.pack("<BBHB", 3, s, i, len(part)) + b"".join(ev_bytes(e) for e in part))
    out.append(bytes([4, s]))
    return out


class Port:
    def __init__(self, name):
        import rtmidi
        self.mi, self.mo = rtmidi.MidiIn(), rtmidi.MidiOut()
        self.mi.ignore_types(sysex=False)
        ins = [i for i, n in enumerate(self.mi.get_ports()) if name in n]
        outs = [i for i, n in enumerate(self.mo.get_ports()) if name in n]
        if not ins or not outs:
            sys.exit(f"no MIDI port named {name!r}")
        self.mi.open_port(ins[0])
        self.mo.open_port(outs[0])

    def ask(self, body, timeout=2.0):
        self.mo.send_message(list(frame(SA_LOOP, body)))
        end = time.time() + timeout
        while time.time() < end:
            m = self.mi.get_message()
            if not m:
                time.sleep(0.002)
                continue
            b = bytes(m[0])
            if b[:4] == SX_HEAD and len(b) > 6 and b[4] == SA_LOOP_R:
                return unpack7(b[5:-1])
        sys.exit("no answer (is it RemoteFM1's standalone build?)")


def save(ask, chunk):
    info = parse_info(ask(req_info()))
    loops = []
    for L in info["loops"]:
        if not L["ms"]:
            continue
        ev, first = [], 0
        while first < L["n"]:
            f, total, part = parse_get(ask(req_get(L["slot"], first, chunk)))
            if not part:
                break
            ev += part
            first += len(part)
        loops.append(dict(slot=L["slot"], ms=L["ms"], bpm=L["bpm"], bars=L["bars"], events=ev))
    return dict(format="remotefm1-loops", version=1, loops=loops)


def load(ask, doc, chunk):
    for L in doc["loops"]:
        for body in reqs_put(L, chunk):
            r = ask(body)
            if r[2] != 0:
                sys.exit(f"loop {L['slot']}: refused ({ {1: 'full', 2: 'bad', 3: 'busy recording'}.get(r[2], r[2]) })")


class Model:
    """the unit's SA_LOOP as firmware/sa/sa_loop.c answers it (for --selftest)"""

    def __init__(self):
        self.l = [dict(ms=0, bpm=120, bars=0, ev=[]) for _ in range(NLOOPS)]

    def ask(self, b):
        op = b[0]
        if op == 0:
            out = bytearray([0, 0, 0, NLOOPS])
            for L in self.l:
                out += struct.pack("<HIHB", len(L["ev"]), L["ms"], L["bpm"], L["bars"])
            return bytes(out)
        k = b[1]
        L = self.l[k]
        if op == 1:
            first, mx = struct.unpack_from("<HB", b, 2)
            part = L["ev"][first:first + min(mx, 126)]
            return struct.pack("<BBHHIHBB", 1, k, first, len(L["ev"]), L["ms"], L["bpm"], L["bars"],
                               len(part)) + b"".join(part)
        if op == 2:
            ms, bpm, bars = struct.unpack_from("<IHB", b, 2)
            self.l[k] = dict(ms=ms, bpm=bpm, bars=bars, ev=[])
        elif op == 3:
            first, n = struct.unpack_from("<HB", b, 2)
            if first != len(L["ev"]):
                return bytes([3, k, 2])
            L["ev"] += [b[5 + 8 * i:13 + 8 * i] for i in range(n)]
        elif op == 4:
            L["ev"].sort(key=lambda e: struct.unpack("<I", e[:4])[0])
        elif op == 5:
            self.l[k] = dict(ms=0, bpm=120, bars=0, ev=[])
        return bytes([op, k, 0])


def selftest():
    for n in (0, 1, 6, 7, 8, 100, 1023):
        b = bytes((i * 37 + 5) & 0xFF for i in range(n))
        assert unpack7(pack7(b)) == b
    m = Model()
    doc = dict(format="remotefm1-loops", version=1, loops=[
        dict(slot=1, ms=848, bpm=120, bars=0, events=[[140, "on", 53, 100], [345, "off", 53, 64]]),
        dict(slot=7, ms=2000, bpm=96, bars=1,
             events=[[i * 7, ["on", "off", "cc"][i % 3], i % 128, (i * 3) % 128] for i in range(300)])])
    wire = lambda body: unpack7(frame(SA_LOOP, body)[5:-1])   # (through the framing both ways)
    load(lambda body: m.ask(wire(body)), doc, 100)
    back = save(lambda body: m.ask(wire(body)), 126)
    assert back == doc, (back["loops"][0], doc["loops"][0])
    print("sa_loops selftest: ok (framing, INFO, GET, BEGIN / EVENTS / END against a model)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", nargs="?", choices=["info", "save", "load", "clear"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--port", default="FM-1")
    ap.add_argument("--chunk", type=int, default=100, help="events a message (USB 100; Bluetooth 6)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.what:
        ap.error("what: info, save, load or clear")
    p = Port(a.port)
    if a.what == "info":
        i = parse_info(p.ask(req_info()))
        print(f"state {i['state']}, loop {i['cur']}")
        for L in i["loops"]:
            if L["ms"]:
                bars = f", {L['bars']} bars at {L['bpm']}" if L["bars"] else ""
                print(f"  {L['slot']:2}: {L['n']} events, {L['ms'] / 1000:.1f} s{bars}")
    elif a.what == "save":
        doc = save(p.ask, a.chunk)
        with open(a.arg, "w") as f:
            json.dump(doc, f)
        print(f"{len(doc['loops'])} loops saved to {a.arg}")
    elif a.what == "load":
        with open(a.arg) as f:
            doc = json.load(f)
        if doc.get("format") != "remotefm1-loops":
            sys.exit(f"{a.arg}: not a loops file")
        load(p.ask, doc, a.chunk)
        print(f"{len(doc['loops'])} loops loaded")
    elif a.what == "clear":
        p.ask(bytes([5, int(a.arg) - 1]))


if __name__ == "__main__":
    main()
