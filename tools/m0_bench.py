#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 RemoteFM1 contributors
"""RemoteFM1 M0: the SysEx (USB-MIDI) half of the bench, over CoreMIDI with python-rtmidi.

  tools/m0_bench.py [OUT.json] [--port FM-1] [--seconds 5] [--sizes 32,128,512,2048,8192] [--bauds 4,2]
                    [--only hello,sink,source,echo,blit,tile]
  tools/m0_bench.py --selftest          the codec against firmware/src/remote_proto.h's golden vector (no MIDI)

It opens the MIDI port whose name contains --port, sends HELLO and goes on only if the unit answers as
"RemoteFM1" (M1, protocol 2: M0's diagnostics are kept) or "RemoteFM1-M0" (another firmware ignores the message:
the tool then says so and exits with 2, having sent nothing else). Then:
  sink    SINK messages of each size for --seconds; the host's rate, and what STATS says arrived (gaps, checksum
          errors, bytes dropped); flow control: a STATS round trip every window, at most two windows in flight
  source  SOURCE of each size for --seconds (stopped with a HELLO): the rate received, sequence gaps, bad sums
  echo    500 ECHO round trips of 8 bytes: min / median / p99 latency
  blit    BLIT_BENCH at each --bauds (SPI1 lsb / (baud + 1)): full frame, 225 tiles, 15 strips
  tile    225 TILEs of 16 x 16 (a whole screen) twice: the end-to-end image path over SysEx
The protocol is firmware/src/remote_proto.h; keep this file in step with it.
"""
import argparse
import json
import os
import queue
import statistics
import struct
import sys
import time

NAME = b"RemoteFM1"                      # M1 (proto 2); M0 answered NAMES[1] (proto 1)
NAMES = (NAME, b"RemoteFM1-M0")
SX_HEAD = bytes([0xF0, 0x7D, 0x52, 0x46])
HELLO, SINK, SOURCE, ECHO, STATS, STATS_RESET, BLIT_BENCH, TILE = range(1, 9)
HELLO_R, DATA, ECHO_R, STATS_R, ACK, BLIT_R, SOURCE_DONE = 0x41, 0x43, 0x44, 0x45, 0x46, 0x47, 0x50
STATS_NAMES = """uptime_ms
sx_rx_msgs sx_sink_msgs sx_sink_bytes sx_gaps sx_lost sx_chk sx_bad sx_ovf sx_held sx_tx_msgs sx_tx_bytes
sx_t_first sx_t_last
bk_rx_msgs bk_sink_msgs bk_sink_bytes bk_gaps bk_lost bk_chk bk_bad bk_ovf bk_held bk_tx_msgs bk_tx_bytes
bk_t_first bk_t_last bk_sync bk_frame_gaps
polls_main_ps polls_isr_ps usb_permille loops_ps usb_resets usb_setups ep1_rx ep1_tx ep3_rx ep3_tx
tiles tile_bytes midi_in_dropped lcd_timeouts src_us sx_rx_ps sx_tx_ps bk_rx_ps bk_tx_ps
usb_cinf usb_rx_bad loop_max_us frames decode_errs push_us_last push_us_max sessions""".split()


# ------------------------------------------------------------- codec ---

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
        msb = g[0]
        out += bytes(x | (((msb >> k) & 1) << 7) for k, x in enumerate(g[1:]))
    return bytes(out)


def sx_encode(cmd, body=b""):
    return SX_HEAD + bytes([cmd]) + pack7(body) + b"\xF7"


def sx_decode(msg):
    """-> (cmd, body) for a RemoteFM1 SysEx, else None"""
    msg = bytes(msg)
    if len(msg) < 6 or msg[:4] != SX_HEAD or msg[-1] != 0xF7:
        return None
    return msg[4], unpack7(msg[5:-1])


def rf_sum(b):
    s = 0
    for x in b:
        s = ((((s << 1) | (s >> 31)) & 0xFFFFFFFF) + x) & 0xFFFFFFFF
    return s


def parse_hello(b):
    if len(b) < 51 or b[:16].rstrip(b"\0") not in NAMES:
        return None
    sxq, rxr, txr, sxm, bkm = struct.unpack_from("<HIIHH", b, 29)
    h = {"name": b[:16].rstrip(b"\0").decode(), "proto": b[16], "version": b[17:25].rstrip(b"\0").decode(),
         "transports": b[25], "usb_configured": bool(b[25] & 1), "bulk_built": bool(b[25] & 2),
         "usb_mode": b[26], "lcd_baud": b[27], "loader_state": struct.unpack_from("b", b, 28)[0],
         "sxq": sxq, "bk_rx_ring": rxr, "bk_tx_ring": txr, "sx_body_max": sxm, "bk_body_max": bkm,
         "identity": b[43:51].rstrip(b"\0").decode()}
    if len(b) >= 67:                    # proto 2 (docs/PROTOCOL.md section 2)
        w, hh, caps = struct.unpack_from("<HHH", b, 51)
        h.update({"screen": [w, hh], "caps": caps, "frames_in_flight": b[57], "session": b[58],
                  "fw_ver": b[59:67].rstrip(b"\0").decode()})
    return h


def parse_stats(b):
    n = b[0]
    v = struct.unpack_from(f"<{n}I", b, 1)
    return {STATS_NAMES[i] if i < len(STATS_NAMES) else f"st{i}": v[i] for i in range(n)}


def parse_blit(b):
    out = []
    for k in range(b[0]):
        baud, full, tiles, strips = struct.unpack_from("<BIII", b, 1 + 13 * k)
        out.append({"baud": baud, "spi_mhz": round(60 / (baud + 1), 1), "full_us": full, "tiles_us": tiles,
                    "strips_us": strips, "full_fps": round(1e6 / full, 1) if full else None})
    return out


def selftest():
    body = bytes([0x00, 0x7F, 0x80, 0xFF, 1, 2, 3, 0xAA])
    want = bytes.fromhex("F07D5246040C007F007F010203012AF7")        # tests/proto_test.c test_golden
    assert sx_encode(ECHO, body) == want, sx_encode(ECHO, body).hex()
    for n in list(range(0, 80)) + [1000, 8192]:
        b = os.urandom(n)
        p = pack7(b)
        assert len(p) == n + (n + 6) // 7 and all(x < 0x80 for x in p)
        assert unpack7(p) == b
        assert sx_decode(sx_encode(SINK, b)) == (SINK, b)
    h = bytearray(67)                   # a proto 2 HELLO_R (docs/PROTOCOL.md section 2)
    h[:9], h[16], h[43:51] = NAME, 2, b"FM-1_015"
    struct.pack_into("<HHHBB", h, 51, 240, 240, 0x2F, 2, 1)
    h[59:62] = b"0.1"
    ph = parse_hello(bytes(h))
    assert ph and ph["proto"] == 2 and ph["screen"] == [240, 240] and ph["caps"] == 0x2F and ph["session"] == 1
    assert ph["fw_ver"] == "0.1" and parse_hello(b"RemoteFM1-M0" + bytes(39))["name"] == "RemoteFM1-M0"
    assert parse_hello(b"RemoteFM1-XX" + bytes(39)) is None
    assert rf_sum(b"\x01\x02") == 4 and rf_sum(b"\xff" * 40) == rf_sum(bytes([0xff] * 40))
    print("m0_bench selftest ok")


# -------------------------------------------------------------- MIDI ---

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
            raise SystemExit(f"m0_bench: no MIDI port matching {port!r} (inputs {ins}, outputs {outs})")
        self.name = ins[i]
        self.q = queue.Queue()
        self.acc = b""
        self.inp.ignore_types(sysex=False, timing=True, active_sense=True)
        self.inp.set_callback(self._rx)
        self.inp.open_port(i)
        self.out.open_port(o)
        self.raw = None                 # a list: every RemoteFM1 message also lands here (SOURCE)
        time.sleep(0.3)                 # CoreMIDI may drop a message sent just after opening

    def _rx(self, ev, _):
        """SysEx re-split here from the bytes: RtMidi's CoreMIDI input can hand over two messages as one (a
        packet holding the end of one and the start of the next) or a message in pieces"""
        msg, _dt = ev
        if not msg:
            return
        if msg[0] != 0xF0 and not self.acc:
            return                      # a channel message (the panel's keys): not ours
        t = time.perf_counter()
        buf = self.acc + bytes(msg)
        while True:
            s = buf.find(0xF0)
            if s < 0:
                buf = b""
                break
            e = buf.find(0xF7, s + 1)
            n = buf.find(0xF0, s + 1)
            if 0 <= n and (e < 0 or n < e):
                buf = buf[n:]           # cut off: dropped
                continue
            if e < 0:
                buf = buf[s:]
                break
            self._msg(t, buf[s:e + 1])
            buf = buf[e + 1:]
        self.acc = buf

    def _msg(self, t, msg):
        if self.raw is not None and len(msg) > 5 and msg[4] == DATA:
            self.raw.append((t, msg))   # decoded after the run
            return
        d = sx_decode(msg)
        if d:
            self.q.put((t, d[0], d[1]))

    def send(self, cmd, body=b""):
        self.out.send_message(sx_encode(cmd, body))

    def send_raw(self, msg):
        self.out.send_message(msg)

    def wait(self, cmd, timeout=2.0):
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            try:
                t, c, b = self.q.get(timeout=left)
            except queue.Empty:
                break
            if c == cmd:
                return t, b
        return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        self.inp.close_port()
        self.out.close_port()


def settle(link, secs=0.3):
    """let whatever a test left in flight arrive, then forget it"""
    time.sleep(secs)
    link.drain()


def stats(link):
    link.send(STATS)
    r = link.wait(STATS_R, 3.0)
    return parse_stats(r[1]) if r else None


def reset(link):
    link.send(STATS_RESET)
    return link.wait(ACK, 3.0) is not None


# ------------------------------------------------------------- tests ---

def test_sink(link, size, secs, window=32768):
    settle(link)
    data = os.urandom(max(size, 3))[:size]
    tail = pack7(data[3:] + struct.pack("<I", rf_sum(data)))       # body[7:], a whole group boundary
    if size < 3:
        tail = None
    reset(link)
    link.drain()
    seq = sent = since = 0
    pending = 0
    t0 = time.perf_counter()
    stop = t0 + secs
    while time.perf_counter() < stop:
        if tail is None:
            msg = sx_encode(SINK, struct.pack("<I", seq) + data + struct.pack("<I", rf_sum(data)))
        else:
            msg = SX_HEAD + bytes([SINK]) + pack7(struct.pack("<I", seq) + data[:3]) + tail + b"\xF7"
        link.send_raw(msg)
        seq += 1
        sent += size
        since += len(msg)
        if since >= window:
            since = 0
            link.send(STATS)
            pending += 1
            if pending > 1:             # at most two windows in flight
                if not link.wait(STATS_R, 5.0):
                    return {"size": size, "error": "STATS timeout (flow control)"}
                pending -= 1
    for _ in range(pending):
        link.wait(STATS_R, 5.0)
    st = stats(link)
    t1 = time.perf_counter()
    if not st:
        return {"size": size, "error": "no STATS"}
    dev_ms = max(st["sx_t_last"] - st["sx_t_first"], 1)
    return {"size": size, "msgs_sent": seq, "bytes_sent": sent, "host_kBps": round(sent / (t1 - t0) / 1024, 1),
            "arrived_msgs": st["sx_sink_msgs"], "arrived_bytes": st["sx_sink_bytes"],
            "dev_kBps": round(st["sx_sink_bytes"] / dev_ms * 1000 / 1024, 1), "gaps": st["sx_gaps"],
            "lost": st["sx_lost"], "chk_err": st["sx_chk"], "bad": st["sx_bad"], "ovf": st["sx_ovf"],
            "held": st["sx_held"], "polls_main_ps": st["polls_main_ps"], "usb_permille": st["usb_permille"],
            "cin_f": st.get("usb_cinf"), "usb_rx_bad": st.get("usb_rx_bad")}


def test_source(link, size, secs):
    settle(link)
    reset(link)
    link.drain()
    link.raw = []
    link.send(SOURCE, struct.pack("<IH", 0xFFFFFFFF, size))
    time.sleep(secs)
    link.send(HELLO)                    # stops it
    done = link.wait(SOURCE_DONE, 5.0)
    link.wait(HELLO_R, 3.0)
    time.sleep(0.2)
    raw, link.raw = link.raw, None
    exp = {}
    got = gaps = bad = 0
    last = None
    for _, m in raw:
        d = sx_decode(m)
        if not d or len(d[1]) < 8:
            bad += 1
            continue
        b = d[1]
        seq = struct.unpack_from("<I", b)[0]
        data, s = b[4:-4], struct.unpack_from("<I", b, len(b) - 4)[0]
        k = (seq * 7) & 0xFF
        if (k, len(data)) not in exp:
            e = bytes((k + i) & 0xFF for i in range(len(data)))
            exp[(k, len(data))] = (e, rf_sum(e))
        e, es = exp[(k, len(data))]
        if data != e or s != es:
            bad += 1
        if last is not None and seq != last + 1:
            gaps += 1
        last = seq
        got += len(data)
    span = raw[-1][0] - raw[0][0] if len(raw) > 1 else 0
    r = {"size": size, "msgs": len(raw), "bytes": got, "host_kBps": round(got / span / 1024, 1) if span else None,
         "gaps": gaps, "bad": bad}
    if done:
        db, dm, dus, ab = struct.unpack("<IIIB", done[1][:13])
        r.update({"dev_bytes": db, "dev_msgs": dm, "dev_kBps": round(db / max(dus, 1) * 1e6 / 1024, 1),
                  "aborted": ab})
    else:
        r["error"] = "no SOURCE_DONE"
    return r


def test_echo(link, n=500):
    settle(link)
    link.drain()
    lat = []
    lost = 0
    for i in range(n):
        body = struct.pack("<I", i) + b"echo"
        t0 = time.perf_counter()
        link.send(ECHO, body)
        while True:
            r = link.wait(ECHO_R, 1.0)
            if not r or r[1] == body:
                break
        if not r:
            lost += 1
            continue
        lat.append((r[0] - t0) * 1000)
    if not lat:
        return {"n": n, "lost": lost, "error": "no ECHO_R"}
    lat.sort()
    return {"n": n, "lost": lost, "min_ms": round(lat[0], 3), "median_ms": round(statistics.median(lat), 3),
            "p99_ms": round(lat[min(len(lat) - 1, int(len(lat) * 0.99))], 3), "max_ms": round(lat[-1], 3)}


def test_blit(link, bauds):
    settle(link)
    link.drain()
    link.send(BLIT_BENCH, bytes(bauds))
    r = link.wait(BLIT_R, 15.0)
    return parse_blit(r[1]) if r else {"error": "no BLIT_R"}


def test_tile(link, frames=2, window=16384):
    settle(link)
    reset(link)
    link.drain()
    t0 = time.perf_counter()
    since = pending = 0
    for f in range(frames):
        for ty in range(15):
            for tx in range(15):
                c = ((tx * 17 + f * 90) & 0xFF, (ty * 17) & 0xFF, 128)
                px = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)
                body = bytes([tx * 16, ty * 16, 16, 16]) + struct.pack(">H", px) * 256
                link.send(TILE, body)
                since += 600
                if since >= window:
                    since = 0
                    link.send(STATS)
                    pending += 1
                    if pending > 1:
                        link.wait(STATS_R, 5.0)
                        pending -= 1
    for _ in range(pending):
        link.wait(STATS_R, 5.0)
    st = stats(link)
    dt = time.perf_counter() - t0
    if not st:
        return {"error": "no STATS"}
    return {"tiles_sent": frames * 225, "tiles_drawn": st["tiles"], "seconds": round(dt, 3),
            "tiles_ps": round(st["tiles"] / dt, 1), "frame_ms": round(dt / frames * 1000, 1), "bad": st["sx_bad"]}


def table(rows, cols):
    w = [max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in cols]
    print("  " + "  ".join(c.rjust(n) for c, n in zip(cols, w)))
    for r in rows:
        print("  " + "  ".join(str(r.get(c, "")).rjust(n) for c, n in zip(cols, w)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", help="write the results here as JSON")
    ap.add_argument("--port", default="FM-1", help="part of the MIDI port's name (default FM-1)")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--sizes", default="32,128,512,2048,8192")
    ap.add_argument("--bauds", default="4,2")
    ap.add_argument("--only", default="sink,source,echo,blit,tile")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    sizes = [int(x) for x in a.sizes.split(",")]
    only = set(a.only.split(","))
    link = Link(a.port)
    print(f"port {link.name}")
    hello = None
    for _ in range(2):
        link.send(HELLO)
        r = link.wait(HELLO_R, 1.5)
        if r:
            hello = parse_hello(r[1])
            if hello:
                break
    if not hello:
        link.close()
        print("m0_bench: no RemoteFM1 answer on this port: the unit runs another firmware (or none). "
              "Nothing else was sent.")
        return 2
    print(f"HELLO  {hello['name']} v{hello['version']}  proto {hello['proto']}  identity {hello['identity']}  "
          f"usb mode {hello['usb_mode']}  lcd baud {hello['lcd_baud']}  loader {hello['loader_state']}")
    res = {"port": link.name, "hello": hello, "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    try:
        if "sink" in only:
            print(f"\nSINK host -> device, {a.seconds:g} s each (KB = 1024 B of payload)")
            res["sink"] = [test_sink(link, s, a.seconds) for s in sizes]
            table(res["sink"], ["size", "msgs_sent", "host_kBps", "dev_kBps", "arrived_bytes", "gaps", "chk_err",
                                "bad", "ovf", "held", "cin_f", "polls_main_ps", "usb_permille", "error"])
        if "source" in only:
            print(f"\nSOURCE device -> host, {a.seconds:g} s each")
            res["source"] = [test_source(link, s, a.seconds) for s in sizes]
            table(res["source"], ["size", "msgs", "bytes", "host_kBps", "dev_kBps", "gaps", "bad", "aborted", "error"])
        if "echo" in only:
            print("\nECHO 8 bytes, 500 round trips")
            res["echo"] = test_echo(link)
            table([res["echo"]], ["n", "lost", "min_ms", "median_ms", "p99_ms", "max_ms", "error"])
        if "blit" in only:
            print("\nBLIT_BENCH (us)")
            res["blit"] = test_blit(link, [int(x) for x in a.bauds.split(",")])
            if isinstance(res["blit"], list):
                table(res["blit"], ["baud", "spi_mhz", "full_us", "full_fps", "tiles_us", "strips_us"])
            else:
                print(" ", res["blit"])
        if "tile" in only:
            print("\nTILE 16 x 16 over SysEx, 2 screens")
            res["tile"] = test_tile(link)
            table([res["tile"]], ["tiles_sent", "tiles_drawn", "seconds", "tiles_ps", "frame_ms", "bad", "error"])
        res["stats"] = stats(link)
    finally:
        link.close()
    if a.out:
        with open(a.out, "w") as f:
            json.dump(res, f, indent=1)
        print(f"\nwrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
