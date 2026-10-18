#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 RemoteFM1 contributors
"""A stand-in for the RemoteFM1 M0 device on a CoreMIDI virtual port, to try tools/m0_bench.py without a unit:

  python3 tests/m0_sim.py [--name RF-SIM] [--silent] &
  python3 tools/m0_bench.py --port RF-SIM --seconds 1 /tmp/m0.json

It answers the bench protocol the way firmware/src/remote.c does (HELLO, SINK counted, SOURCE until stopped,
ECHO, STATS, STATS_RESET, BLIT_BENCH with made-up times, TILE counted). --silent answers nothing, like another
firmware: m0_bench must then stop after HELLO. It tests the host tool, not the device. Ends with Ctrl-C.
"""
import argparse
import struct
import sys
import queue
import time

sys.path.insert(0, __file__.rsplit("/", 2)[0] + "/tools")
import m0_bench as m  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="RF-SIM")
    ap.add_argument("--silent", action="store_true")
    a = ap.parse_args()
    import rtmidi
    out, inp = rtmidi.MidiOut(), rtmidi.MidiIn()
    out.open_virtual_port(a.name)
    inp.open_virtual_port(a.name)
    inp.ignore_types(sysex=False)
    st = {k: 0 for k in m.STATS_NAMES}
    sink = {"expect": None}
    src = {"on": False, "n": 0, "ml": 0, "seq": 0, "sent": 0, "start": 0.0}
    t0 = time.monotonic()
    outq = queue.Queue()                        # one thread sends (the main loop): the callback only queues

    def send(cmd, body=b""):
        outq.put(m.sx_encode(cmd, body))

    def source_done():
        src["on"] = False
        send(m.SOURCE_DONE, struct.pack("<IIIB", src["sent"], src["seq"],
                                        int((time.monotonic() - src["start"]) * 1e6), int(src["sent"] < src["n"])))

    def on(ev, _):
        msg, _dt = ev
        d = m.sx_decode(msg)
        if not d or a.silent:
            return
        cmd, b = d
        now = int((time.monotonic() - t0) * 1000)
        st["uptime_ms"] = now
        if cmd == m.SINK:
            seq = struct.unpack_from("<I", b)[0]
            data, s = b[4:-4], struct.unpack_from("<I", b, len(b) - 4)[0]
            st["sx_rx_msgs"] += 1
            st["sx_sink_msgs"] += 1
            if st["sx_sink_msgs"] == 1:
                st["sx_t_first"] = now
            st["sx_t_last"] = now
            st["sx_sink_bytes"] += len(data)
            st["sx_chk"] += m.rf_sum(data) != s
            if sink["expect"] is not None and seq != sink["expect"]:
                st["sx_gaps"] += 1
            sink["expect"] = seq + 1
            time.sleep(len(msg) / 500e3)        # ~500 KB/s, like a device that holds the host back
            return
        st["sx_rx_msgs"] += 1
        if cmd == m.HELLO:
            if src["on"]:
                source_done()
            h = bytearray(67)
            h[:len(m.NAME)] = m.NAME
            h[16] = 2
            struct.pack_into("<HHHBB", h, 51, 240, 240, 0x2F, 2, 0)
            h[59:62] = b"sim"
            h[17:20] = b"sim"
            h[25] = 3
            h[26] = 1
            h[27] = 4
            struct.pack_into("<HIIHH", h, 29, 3584, 32768, 32768, 1024, 16384)
            h[43:51] = b"FM-1_015"
            send(m.HELLO_R, bytes(h))
        elif cmd == m.SOURCE:
            n, ml = struct.unpack("<IH", b[:6])
            src.update(on=True, n=n, ml=ml, seq=0, sent=0, start=time.monotonic())
        elif cmd == m.ECHO:
            send(m.ECHO_R, b)
        elif cmd == m.STATS:
            send(m.STATS_R, bytes([len(m.STATS_NAMES)]) + struct.pack(f"<{len(m.STATS_NAMES)}I",
                                                                      *[st[k] & 0xFFFFFFFF for k in m.STATS_NAMES]))
        elif cmd == m.STATS_RESET:
            for k in st:
                st[k] = 0
            sink["expect"] = None
            send(m.ACK)
        elif cmd == m.BLIT_BENCH:
            bauds = list(b) or [4, 2]
            send(m.BLIT_R, bytes([len(bauds)]) + b"".join(
                struct.pack("<BIII", x, 77000 * (x + 1) // 5, 98000, 78000) for x in bauds))
        elif cmd == m.TILE:
            st["tiles"] += 1
            st["tile_bytes"] += len(b) - 4
        else:
            st["sx_bad"] += 1

    inp.set_callback(on)
    print(f"m0_sim: virtual port {a.name!r}" + (" (silent)" if a.silent else ""), flush=True)
    try:
        while True:
            try:
                out.send_message(outq.get(timeout=0.0005 if src["on"] else 0.01))
                continue
            except queue.Empty:
                pass
            if src["on"]:                       # one DATA message, ~400 KB/s at most
                if src["sent"] >= src["n"]:
                    source_done()
                    continue
                k = min(src["ml"], src["n"] - src["sent"])
                d = bytes(((src["seq"] * 7) + i) & 0xFF for i in range(k))
                out.send_message(m.sx_encode(m.DATA, struct.pack("<I", src["seq"]) + d +
                                             struct.pack("<I", m.rf_sum(d))))
                src["seq"] += 1
                src["sent"] += k
                time.sleep(k / 400e3)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
