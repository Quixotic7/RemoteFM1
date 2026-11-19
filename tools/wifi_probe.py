#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""RemoteFM1 Wi-Fi, phase 0 (docs/WIFI.md): put an FM-1 running the Wi-Fi build on a network over USB, find it on the
network, and measure RemoteFM1's bulk framing over TCP (docs/PROTOCOL.md section 10).

  wifi_probe.py setup  [--port NAME] [--ssid SSID]   the network over USB MIDI (WIFI SET): the SSID is asked for if
                                                      not given, the password always asked for (getpass: typed, not
                                                      shown, never on the command line, never printed or logged);
                                                      then WIFI GET until the FM-1 has an address or gives up
  wifi_probe.py status [--port NAME]                  the network's state over USB (WIFI GET, the networks kept)
  wifi_probe.py forget [--port NAME] [K|all]          forget network K (0: the most recent) or all of them
  wifi_probe.py watch  [--port NAME] [--every 1] [--seconds N] [--log FILE.jsonl]
                                                      WIFI DIAG over USB every second while a host (the Move) drives
                                                      the unit over TCP: a line a second (the main loop, the socket
                                                      task, whole frames, PANEL_R, TCP's input, the session's starts
                                                      and ends, lwIP's retransmissions and drops), JSON to FILE
  wifi_probe.py mode   [--port NAME] whole|direct     whole frames over TCP (the default) or as they come (an A/B)
  wifi_probe.py find   [--timeout S] [--to ADDR]      UDP discovery ("RFM1?" to port 7741, broadcast): the FM-1s
  wifi_probe.py bench  [HOST] [--tcp-port 7740] [--sink-kb N] [--source-kb N] [--echo N] [--json FILE]
                                                      over TCP: HELLO, ECHO round trips, SINK (STATS confirms what
                                                      arrived), SOURCE, loss; HOST found by discovery if not given
  wifi_probe.py --sim [bench options]                 the same against the host simulator (firmware/tests: "rf serve")
  wifi_probe.py --selftest                            the codec's checks (no device, no network)

USB MIDI goes through tools/m0_bench.py's Link (python-rtmidi), as RemoteFM1's other tools. The FM-1 is picked by
HELLO among the MIDI ports named like an FM-1: the one whose HELLO_R says the Wi-Fi build (caps bit 12, identity
FM-1_729..). Nothing here installs or updates anything."""
import argparse
import getpass
import json
import os
import socket
import statistics
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m0_bench as mb  # noqa: E402  (the SysEx codec, HELLO_R / STATS_R parsers, the USB MIDI link)

TCP_PORT, UDP_PORT = 7740, 7741                  # proto/rfm1.h RFM_NET_TCP_PORT, RFM_NET_UDP_PORT
WIFI, WIFI_R = 0x17, 0x57                        # RFM_WIFI, RFM_WIFI_R
W_GET, W_SET, W_FORGET, W_SAVED, W_DIAG, W_MODE, W_SCAN, W_LIST = 0, 1, 2, 3, 4, 5, 6, 7
F_SECURED, F_KEPT, F_HEARD, F_JOINED = 1, 2, 4, 8
CAP_WIFI = 1 << 12
STATES = ["off", "starting", "on (no network)", "joining", "getting an address", "connected", "failed"]
STATUS = ["ok", "bad request", "not over this link (USB only)", "busy"]
NET_STATS = "accepts closes replaced rx_bytes tx_bytes rx_full tx_eagain udp_in udp_out polls open_fail".split()
# WIFI DIAG (op 4; docs/WIFI.md "Diagnostics"): after the sockets' counters, a version byte (1), then these u32s.
# _max / _min / h* since the last DIAG (the unit starts them again at every DIAG); the others since the start.
DIAG_LOOP = ("ms turn_max_us h_lt250us h_lt1ms h_lt4ms h_lt16ms h_lt64ms h_ge64ms outside_max_us net_gap_max_ms "
             "frame_us_max frame_us_last frame_turns_max frames_whole frames_over panel_r panel_r_gap_max_ms "
             "tx_drop rx_full_ms_max rx_full_events sess_starts end_host end_keepalive end_conn last_reset mode "
             "refused session_on").split()
SESSION_ON = {0: "usb", 1: "bulk", 2: "ble", 3: "tcp", 255: "none"}
DIAG_TCP = ("nrtx_events nrtx_max rto_max_ms snd_wnd_min rcv_wnd_min persist_events tx_pending_max tcp_xmit "
            "tcp_recv tcp_drop tcp_memerr link_recv link_drop pbuf_err").split()
RESET_WHY = ["-", "power-on", "audio end", "SESSION", "panel drawn by another"]
BK_HDR = 10


# ------------------------------------------------------------- codec ---

def bk_frame(cmd, body, seq):
    return b"RF" + bytes([cmd, 0]) + struct.pack("<HI", len(body), seq) + body


class BkParser:
    """the bulk framing from a byte stream: whole messages (cmd, seq, body)"""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b"RF")
            if i < 0:
                del self.buf[:max(0, len(self.buf) - 1)]
                return out
            if i:
                del self.buf[:i]
            if len(self.buf) < BK_HDR:
                return out
            cmd, _z, n, seq = struct.unpack_from("<BBHI", self.buf, 2)
            if len(self.buf) < BK_HDR + n:
                return out
            out.append((cmd, seq, bytes(self.buf[BK_HDR:BK_HDR + n])))
            del self.buf[:BK_HDR + n]


def wifi_set_body(ssid, password):
    s, p = ssid.encode("utf-8"), password.encode("utf-8")
    if not 1 <= len(s) <= 32 or len(p) > 63:
        raise ValueError("an SSID of 1..32 bytes and a password of at most 63")
    return bytes([W_SET, len(s)]) + s + bytes([len(p)]) + p


def parse_wifi_r(b):
    """WIFI_R (docs/PROTOCOL.md section 10) -> dict"""
    if len(b) < 32:
        return None
    op, status, state, rssi, ip, tport, uport, host, saved, hu, hp, hs, app16, rxf = \
        struct.unpack_from("<BBBb4sHHBBIIIHH", b, 0)
    n = 30
    sl = b[n]
    ssid = b[n + 1:n + 1 + sl].decode("utf-8", "replace")
    n += 1 + sl
    wl = b[n]
    why = b[n + 1:n + 1 + wl].decode("utf-8", "replace")
    n += 1 + wl
    r = {"op": op, "status": STATUS[status] if status < len(STATUS) else status, "status_code": status,
         "state": STATES[state] if state < len(STATES) else state, "state_code": state, "rssi": rssi,
         "ip": socket.inet_ntoa(ip), "tcp_port": tport, "udp_port": uport, "host_connected": bool(host),
         "networks_kept": saved, "heap_used": hu, "heap_peak": hp, "heap_size": hs, "app_heap": app16 * 16,
         "rx_frames_per_s": rxf, "ssid": ssid, "why": why}
    if op == W_SAVED and n < len(b):
        k, n, nets = b[n], n + 1, []
        for _ in range(k):
            ln = b[n]
            nets.append(b[n + 1:n + 1 + ln].decode("utf-8", "replace"))
            n += 1 + ln
        r["saved"] = nets
    elif len(b) >= n + 44:
        r["net"] = dict(zip(NET_STATS, struct.unpack_from("<11I", b, n)))
        n += 44
        nl, nt = len(DIAG_LOOP), len(DIAG_TCP)
        if op in (W_DIAG, W_MODE) and len(b) >= n + 1 + 4 * (nl + nt) and b[n] == 1:
            v = struct.unpack_from(f"<{nl + nt}I", b, n + 1)
            r["diag"] = dict(zip(DIAG_LOOP + DIAG_TCP, v))
    return r


def parse_wifi_list(b):
    """WIFI_R to SCAN / LIST: a page of the scan's networks (which 0) or the networks kept (1)"""
    if len(b) < 9 or b[0] not in (W_SCAN, W_LIST):
        return None
    r = {"op": b[0], "status": b[1], "state_code": b[2], "which": b[3], "scanning": b[4], "scans": b[5],
         "total": b[6], "first": b[7], "nets": []}
    k = 9
    for _ in range(b[8]):
        if k + 4 > len(b) or k + 4 + b[k + 3] > len(b):
            return None
        rssi = struct.unpack_from("<b", b, k)[0]
        r["nets"].append({"rssi": rssi, "flags": b[k + 1], "channel": b[k + 2],
                          "ssid": bytes(b[k + 4:k + 4 + b[k + 3]]).decode("utf-8", "replace")})
        k += 4 + b[k + 3]
    return r


def parse_discovery(data):
    if not data.startswith(b"RFM1!\n"):
        return None
    out = {}
    for line in data[6:].decode("utf-8", "replace").splitlines():
        k, _, v = line.partition("=")
        if k:
            out[k] = v
    return out


def selftest():
    mb.selftest()
    f = bk_frame(mb.ECHO, b"abc", 7)
    assert f == b"RF\x04\x00\x03\x00\x07\x00\x00\x00abc", f
    p = BkParser()
    msgs = p.feed(b"xxRF" + f[2:5]) + p.feed(f[5:] + bk_frame(mb.SINK, bytes(300), 8)[:20])
    assert msgs == [(mb.ECHO, 7, b"abc")], msgs
    assert p.feed(bk_frame(mb.SINK, bytes(300), 8)[20:]) == [(mb.SINK, 8, bytes(300))]
    b = wifi_set_body("Home", "pa55")
    assert b == bytes([1, 4]) + b"Home" + bytes([4]) + b"pa55"
    for bad in (("", "x"), ("x" * 33, ""), ("ok", "y" * 64)):
        try:
            wifi_set_body(*bad)
            raise AssertionError(bad)
        except ValueError:
            pass
    r = bytearray(30)
    struct.pack_into("<BBBb4sHHBBIIIHH", r, 0, 0, 0, 5, -51, bytes([192, 168, 1, 42]), 7740, 7741, 1, 2,
                     100000, 150000, 266240, 5400, 33)
    r += bytes([4]) + b"Home" + bytes([0]) + struct.pack("<11I", *range(11))
    w = parse_wifi_r(bytes(r))
    assert w["state"] == "connected" and w["ip"] == "192.168.1.42" and w["rssi"] == -51 and w["ssid"] == "Home"
    assert w["app_heap"] == 86400 and w["net"]["polls"] == 9 and w["host_connected"]
    r2 = bytearray(r[:30])
    r2[0] = W_SAVED
    r2 += bytes([4]) + b"Home" + bytes([0, 2, 4]) + b"Home" + bytes([4]) + b"Cafe"
    assert parse_wifi_r(bytes(r2))["saved"] == ["Home", "Cafe"]
    d = parse_discovery(b"RFM1!\nname=FM-1 1F2D\nid=FM-1_72900\nport=7740\nip=192.168.1.42\n")
    assert d == {"name": "FM-1 1F2D", "id": "FM-1_72900", "port": "7740", "ip": "192.168.1.42"}, d
    assert parse_discovery(b"nope") is None
    r3 = bytearray(r)
    r3[0] = W_DIAG
    nd = len(DIAG_LOOP) + len(DIAG_TCP)
    r3 += bytes([1]) + struct.pack(f"<{nd}I", *range(100, 100 + nd))
    w3 = parse_wifi_r(bytes(r3))
    assert w3["diag"]["ms"] == 100 and w3["diag"]["mode"] == 125 and w3["diag"]["pbuf_err"] == 100 + nd - 1, w3["diag"]
    w3["diag"]["session_on"] = 3
    w3["diag"]["last_reset"] = 3
    assert "reset SESSION" in diag_line(w3["diag"], None, w3)
    pg = bytes([W_LIST, 0, 5, 0, 0, 3, 2, 0, 2]) + struct.pack("<b", -52) + bytes([F_SECURED | F_HEARD, 6, 4]) + \
        b"Home" + struct.pack("<b", -71) + bytes([F_HEARD, 11, 4]) + b"Cafe"
    lw = parse_wifi_list(pg)
    assert lw["total"] == 2 and [n["ssid"] for n in lw["nets"]] == ["Home", "Cafe"] and lw["nets"][0]["rssi"] == -52
    assert parse_wifi_list(pg[:-1]) is None
    print("wifi_probe selftest ok")


# --------------------------------------------------------------- USB ---

def usb_link(port):
    """the Wi-Fi build's USB MIDI port, by HELLO (a stale CoreMIDI name says nothing)"""
    import rtmidi
    names = rtmidi.MidiOut().get_ports()
    cands = [n for n in names if (port.lower() in n.lower() if port else any(
        k in n.lower() for k in ("fm-1", "felucca", "usb-midi")))]
    cands = [n for n in cands if "update" not in n.lower()]
    if not cands:
        raise SystemExit(f"wifi_probe: no MIDI port like an FM-1 (ports: {names})")
    seen = []
    for n in cands:
        link = mb.Link(n)
        link.send(mb.HELLO, b"wifi_probe")
        r = link.wait(mb.HELLO_R, 1.5)
        h = mb.parse_hello(r[1]) if r else None
        if h and (h.get("caps", 0) & CAP_WIFI or h["identity"].startswith("FM-1_729")):
            print(f"FM-1 on {n}: {h['identity']} {h.get('fw_ver', '')}, the Wi-Fi build")
            return link
        seen.append(f"{n}: {h['identity'] + ' (not the Wi-Fi build)' if h else 'no HELLO_R'}")
        link.close()
    raise SystemExit("wifi_probe: no FM-1 with the Wi-Fi build on USB (" + "; ".join(seen) + ")")


def usb_wifi(link, body, timeout=2.0):
    link.drain()
    link.send(WIFI, body)
    r = link.wait(WIFI_R, timeout)
    return parse_wifi_r(r[1]) if r else None


def show(w):
    if not w:
        print("no WIFI_R (not the Wi-Fi build?)")
        return
    line = f"Wi-Fi {w['state']}"
    if w["ssid"]:
        line += f", network {w['ssid']!r}"
    if w["state_code"] == 5:
        line += f", address {w['ip']}, signal {w['rssi']} dBm, TCP {w['tcp_port']}"
    if w["why"]:
        line += f" ({w['why']})"
    print(line)
    print(f"  networks kept: {w['networks_kept']}; a host on TCP: {'yes' if w['host_connected'] else 'no'}; "
          f"heap {w['heap_used']} B used, {w['heap_peak']} B at most, of {w['heap_size']} B "
          f"(RemoteFM1's own {w['app_heap']} B)")
    if "saved" in w:
        for i, s in enumerate(w["saved"]):
            print(f"  {i}: {s}")


def cmd_setup(a):
    link = usb_link(a.port)
    try:
        ssid = a.ssid or input("Wi-Fi network (SSID): ").strip()
        password = getpass.getpass(f"Password for {ssid!r} (not shown; empty for an open network): ")
        try:
            body = wifi_set_body(ssid, password)
        except ValueError as e:
            raise SystemExit(f"wifi_probe: {e}")
        password = None                          # (the only copies left: body, then the FM-1's)
        w = usb_wifi(link, body)
        body = None
        if not w:
            raise SystemExit("wifi_probe: no answer to WIFI SET")
        if w["status_code"]:
            raise SystemExit(f"wifi_probe: WIFI SET refused: {w['status']}")
        print(f"sent; the FM-1 joins {ssid!r}")
        end = time.monotonic() + a.wait
        last = None
        while time.monotonic() < end:
            time.sleep(1.0)
            w = usb_wifi(link, bytes([W_GET]))
            if not w:
                continue
            if w["state"] != last:
                last = w["state"]
                print(f"  {w['state']}" + (f": {w['why']}" if w["why"] else ""))
            if w["state_code"] == 5 and w["ssid"] == ssid:
                show(w)
                print(f"next: wifi_probe.py bench {w['ip']}")
                return 0
            if w["state_code"] == 6:
                show(w)
                return 1
        print("wifi_probe: no address yet (wifi_probe.py status to look again)")
        return 1
    finally:
        link.close()


def cmd_status(a):
    link = usb_link(a.port)
    try:
        show(usb_wifi(link, bytes([W_SAVED])))
        w = usb_wifi(link, bytes([W_GET]))
        if w and "net" in w:
            print("  sockets: " + ", ".join(f"{k} {v}" for k, v in w["net"].items()))
    finally:
        link.close()
    return 0


def diag_line(d, prev, w):
    """one line of the watch: this second's maxima, the counters' change since the last line"""
    dd = lambda k: d[k] - (prev[k] if prev else d[k])
    h = "/".join(str(d[k]) for k in ("h_lt250us", "h_lt1ms", "h_lt4ms", "h_lt16ms", "h_lt64ms", "h_ge64ms"))
    return (f"{d['ms'] / 1000:9.1f}s turn {d['turn_max_us'] / 1000:6.1f}ms [{h}] out {d['outside_max_us'] / 1000:5.1f}ms "
            f"net {d['net_gap_max_ms']:4d}ms | frame {d['frame_us_max'] / 1000:5.1f}ms/{d['frame_turns_max']}t "
            f"+{dd('frames_whole')}w +{dd('frames_over')}o | PANEL_R +{dd('panel_r')} gap {d['panel_r_gap_max_ms']:5d}ms "
            f"txdrop +{dd('tx_drop')} | rxfull {d['rx_full_ms_max']:5d}ms +{dd('rx_full_events')} | "
            f"sess +{dd('sess_starts')} end h{dd('end_host')}/k{dd('end_keepalive')}/c{dd('end_conn')} "
            f"reset {RESET_WHY[d['last_reset']] if d['last_reset'] < len(RESET_WHY) else d['last_reset']} "
            f"on {SESSION_ON.get(d.get('session_on', 255), '?')} refused +{dd('refused')} | "
            f"rexmit +{dd('nrtx_events')} x{d['nrtx_max']} rto {d['rto_max_ms']}ms wnd s{d['snd_wnd_min']}/r{d['rcv_wnd_min']} "
            f"persist +{dd('persist_events')} txq {d['tx_pending_max']} | tcp +{dd('tcp_xmit')}/+{dd('tcp_recv')} "
            f"drop +{dd('tcp_drop')} mem +{dd('tcp_memerr')} | link +{dd('link_recv')} drop +{dd('link_drop')} "
            f"pbuf +{dd('pbuf_err')} | heap {w['heap_used']}/{w['heap_peak']}")


def cmd_watch(a):
    """WIFI DIAG every a.every seconds over USB while a host drives the unit over TCP; a line each, and a JSON
    record each to a.log (one per line)"""
    link = usb_link(a.port)
    log = open(a.log, "a") if a.log else None
    prev = None
    end = time.monotonic() + a.seconds if a.seconds else None
    try:
        print("watch: maxima per line (turn: the main loop's longest turn, [turns < 0.25/1/4/16/64 ms/longer], out: "
              "fm1-base between the app's turns, net: the socket task's longest absence), counters as +change")
        while end is None or time.monotonic() < end:
            t0 = time.monotonic()
            w = usb_wifi(link, bytes([W_DIAG]), timeout=1.0)
            if not w or "diag" not in w:
                print(f"{time.strftime('%H:%M:%S')} no DIAG answer (not the diagnostic build, or USB busy)")
            else:
                d = w["diag"]
                print(time.strftime("%H:%M:%S") + " " + diag_line(d, prev, w), flush=True)
                if log:
                    log.write(json.dumps({"t": time.time(), "diag": d, "net": w.get("net"), "heap_used": w["heap_used"],
                                          "heap_peak": w["heap_peak"], "rssi": w["rssi"], "host": w["host_connected"]})
                              + "\n")
                    log.flush()
                prev = d
            time.sleep(max(0.0, a.every - (time.monotonic() - t0)))
    except KeyboardInterrupt:
        pass
    finally:
        link.close()
        if log:
            log.close()
    return 0


def cmd_mode(a):
    """WIFI MODE: whole frames over TCP (the default) or frames as they come (an A/B, until the unit restarts)"""
    link = usb_link(a.port)
    try:
        w = usb_wifi(link, bytes([W_MODE, 1 if a.mode == "whole" else 0]))
        if not w or w["status_code"]:
            print(f"wifi_probe: MODE refused ({w['status'] if w else 'no answer'})")
            return 1
        print(f"frames over TCP: {'whole' if w['diag']['mode'] else 'as they come'}")
    finally:
        link.close()
    return 0


def cmd_scan(a):
    """the networks the FM-1 hears (2.4 GHz only), as RemoteFM14Move's page asks for them: SCAN, then LIST's pages"""
    link = usb_link(a.port)
    try:
        link.drain()
        link.send(WIFI, bytes([W_SCAN, 1]))
        r = link.wait(WIFI_R, 2.0)
        if not r or not parse_wifi_list(r[1]) or parse_wifi_list(r[1])["status"]:
            print("the scan was refused (Wi-Fi starting?): try again in a few seconds")
            return 1
        t0 = time.time()
        while True:
            time.sleep(0.5)
            nets, first, page = [], 0, None
            while True:
                link.drain()
                link.send(WIFI, bytes([W_LIST, 0, first]))
                r = link.wait(WIFI_R, 2.0)
                page = parse_wifi_list(r[1]) if r else None
                if not page:
                    print("no answer")
                    return 1
                nets += page["nets"]
                first += len(page["nets"])
                if not page["nets"] or first >= page["total"]:
                    break
            if not page["scanning"] or time.time() - t0 > 12:
                break
        for n in nets:
            f = n["flags"]
            print(f"  {n['rssi']:4d} dBm  ch {n['channel']:2d}  {'lock' if f & F_SECURED else 'open'}"
                  f"{'  kept' if f & F_KEPT else ''}{'  joined' if f & F_JOINED else ''}  {n['ssid']}")
        print(f"{len(nets)} networks")
    finally:
        link.close()
    return 0


def cmd_forget(a):
    link = usb_link(a.port)
    try:
        k = 0xFF if a.which == "all" else int(a.which)
        show(usb_wifi(link, bytes([W_FORGET, k])))
    finally:
        link.close()
    return 0


# ----------------------------------------------------------- network ---

def discover(timeout=2.0, to=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.2)
    found = {}
    end = time.monotonic() + timeout
    nxt = 0.0
    while time.monotonic() < end:
        if time.monotonic() >= nxt:              # asked a few times: a datagram may be lost
            for dst in ([to] if to else ["255.255.255.255"]):
                try:
                    s.sendto(b"RFM1?", (dst, UDP_PORT))
                except OSError:
                    pass
            nxt = time.monotonic() + 0.5
        try:
            data, (addr, _p) = s.recvfrom(512)
        except socket.timeout:
            continue
        d = parse_discovery(data)
        if d:
            d["from"] = addr
            found[addr] = d
    s.close()
    return list(found.values())


def cmd_find(a):
    fm1s = discover(a.timeout, "127.0.0.1" if a.sim else a.to)
    for d in fm1s:
        print(f"{d.get('name', '?')} at {d['from']}:{d.get('port', TCP_PORT)}  {d.get('id', '')} {d.get('ver', '')}"
              f"{'  (a host is connected)' if d.get('busy') == '1' else ''}")
    if not fm1s:
        print("no FM-1 answered (the same network? a router that drops broadcasts: bench HOST by its address)")
    return 0 if fm1s else 1


class Tcp:
    def __init__(self, host, port, tries=10):
        for k in range(tries):                   # (a unit that is joining, or a simulator still starting)
            try:
                self.s = socket.create_connection((host, port), timeout=5.0)
                break
            except (ConnectionRefusedError, socket.timeout, OSError) as e:
                if k == tries - 1:
                    raise SystemExit(f"wifi_probe: no TCP connection to {host}:{port}: {e}")
                time.sleep(0.5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.p = BkParser()
        self.q = []
        self.seq = 0
        self.rx_bytes = 0

    def send(self, cmd, body=b""):
        self.s.sendall(bk_frame(cmd, body, self.seq))
        self.seq += 1

    def recv_some(self, timeout):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(65536)
        except socket.timeout:
            return False
        if not d:
            raise SystemExit("wifi_probe: the FM-1 closed the connection")
        self.rx_bytes += len(d)
        self.q += self.p.feed(d)
        return True

    def wait(self, cmd, timeout=3.0, keep=None):
        """the next message cmd (others dropped, or handed to keep)"""
        end = time.monotonic() + timeout
        while True:
            while self.q:
                c, seq, b = self.q.pop(0)
                if c == cmd:
                    return b
                if keep:
                    keep(c, seq, b)
            left = end - time.monotonic()
            if left <= 0 or not self.recv_some(left):
                if left <= 0:
                    return None

    def close(self):
        self.s.close()


def bench(host, port, a):
    res = {"host": host, "tcp_port": port}
    t0 = time.perf_counter()
    c = Tcp(host, port)
    res["connect_ms"] = round((time.perf_counter() - t0) * 1e3, 1)
    c.send(mb.HELLO, b"wifi_probe")
    h = c.wait(mb.HELLO_R, 3.0)
    hello = mb.parse_hello(h) if h else None
    if not hello:
        raise SystemExit("wifi_probe: no HELLO_R over TCP")
    res["hello"] = hello
    print(f"HELLO_R over TCP: {hello['name']} {hello.get('fw_ver', '')} {hello['identity']}, caps 0x{hello['caps']:X}, "
          f"bulk body max {hello['bk_body_max']}, rings {hello['bk_rx_ring']} / {hello['bk_tx_ring']}")
    c.send(WIFI, bytes([W_GET]))
    w = c.wait(WIFI_R, 3.0)
    if w:
        res["wifi"] = parse_wifi_r(w)
        show(res["wifi"])
    c.send(mb.STATS_RESET)
    c.wait(mb.ACK, 3.0)

    # ECHO: round trips, small and 1 KB
    for size in (16, 1024):
        rtt = []
        for i in range(a.echo):
            body = struct.pack("<I", i) + bytes((i + k) & 0xFF for k in range(size - 4))
            t = time.perf_counter()
            c.send(mb.ECHO, body)
            r = c.wait(mb.ECHO_R, 3.0)
            if r is None:
                break
            if r != body:
                res.setdefault("echo_mismatch", 0)
                res["echo_mismatch"] += 1
            rtt.append((time.perf_counter() - t) * 1e3)
        if rtt:
            q = sorted(rtt)
            res[f"echo_{size}"] = {"n": len(rtt), "lost": a.echo - len(rtt), "min_ms": round(q[0], 2),
                                   "median_ms": round(statistics.median(q), 2),
                                   "p95_ms": round(q[int(0.95 * (len(q) - 1))], 2), "max_ms": round(q[-1], 2)}
            e = res[f"echo_{size}"]
            print(f"ECHO {size:5d} B x {e['n']}: min {e['min_ms']} ms, median {e['median_ms']} ms, "
                  f"95% {e['p95_ms']} ms, max {e['max_ms']} ms, lost {e['lost']}")

    # SINK: host -> FM-1, as fast as TCP takes it; STATS after (in order: it counts everything before it)
    total, msg = a.sink_kb * 1024, 4096
    chunks, sent, seq, t = [], 0, 0, time.perf_counter()
    while sent < total:
        n = min(msg, total - sent)
        data = bytes((seq * 7 + k) & 0xFF for k in range(n)) if seq < 4 else os.urandom(n)
        body = struct.pack("<I", seq) + data + struct.pack("<I", mb.rf_sum(data))
        chunks.append(bk_frame(mb.SINK, body, c.seq))
        c.seq += 1
        seq += 1
        sent += n
        if len(chunks) >= 16:
            c.s.sendall(b"".join(chunks))
            chunks = []
    if chunks:
        c.s.sendall(b"".join(chunks))
    t_sent = time.perf_counter() - t
    c.send(mb.STATS)
    st = c.wait(mb.STATS_R, 30.0)
    t_all = time.perf_counter() - t
    if st:
        s = mb.parse_stats(st)
        res["sink"] = {"bytes": total, "msgs": seq, "s": round(t_all, 3), "kB_s": round(total / t_all / 1024, 1),
                       "sent_s": round(t_sent, 3), "dev_bytes": s.get("bk_sink_bytes"), "dev_msgs": s.get("bk_sink_msgs"),
                       "gaps": s.get("bk_gaps"), "lost": s.get("bk_lost"), "chk": s.get("bk_chk"),
                       "frame_gaps": s.get("bk_frame_gaps"), "sync": s.get("bk_sync"), "loops_ps": s.get("loops_ps"),
                       "loop_max_us": s.get("loop_max_us")}
        k = res["sink"]
        ok = k["dev_bytes"] == total and not (k["gaps"] or k["lost"] or k["chk"] or k["frame_gaps"] or k["sync"])
        print(f"SINK {total // 1024} KB in {k['s']} s: {k['kB_s']} KB/s; the FM-1 counted {k['dev_bytes']} B in "
              f"{k['dev_msgs']} messages, gaps {k['gaps']}, lost {k['lost']}, checksum errors {k['chk']}, frame gaps "
              f"{k['frame_gaps']}: {'all there' if ok else 'LOSS'}")
    else:
        print("SINK: no STATS_R after it")

    # SOURCE: FM-1 -> host, DATA messages as fast as its ring and TCP take them
    total = a.source_kb * 1024
    want_len = min(4096, hello["bk_body_max"]) - 8
    got = {"msgs": 0, "bytes": 0, "bad": 0, "gaps": 0, "next": 0}

    def on_data(cmd, _seq, b):
        if cmd != mb.DATA or len(b) < 8:
            return
        sq, = struct.unpack_from("<I", b, 0)
        d = b[4:-4]
        if struct.unpack_from("<I", b, len(b) - 4)[0] != mb.rf_sum(d) or \
                any(d[k] != (sq * 7 + k) & 0xFF for k in (0, len(d) // 2, len(d) - 1) if d):
            got["bad"] += 1
        if sq != got["next"]:
            got["gaps"] += 1
        got["next"] = sq + 1
        got["msgs"] += 1
        got["bytes"] += len(d)
    t = time.perf_counter()
    c.send(mb.SOURCE, struct.pack("<IH", total, want_len))
    done = c.wait(mb.SOURCE_DONE, 60.0, keep=on_data)
    dt = time.perf_counter() - t
    if done:
        b, m, dev_us, aborted = struct.unpack_from("<IIIB", done, 0)
        res["source"] = {"bytes": got["bytes"], "msgs": got["msgs"], "s": round(dt, 3),
                         "kB_s": round(got["bytes"] / dt / 1024, 1), "dev_bytes": b, "dev_msgs": m,
                         "dev_kB_s": round(b / (dev_us / 1e6) / 1024, 1) if dev_us else None,
                         "aborted": aborted, "bad": got["bad"], "gaps": got["gaps"]}
        k = res["source"]
        print(f"SOURCE {got['bytes'] // 1024} KB in {k['s']} s: {k['kB_s']} KB/s ({k['dev_kB_s']} KB/s by the FM-1's "
              f"clock), {m} messages, gaps {k['gaps']}, bad {k['bad']}, aborted {aborted}")
    else:
        print(f"SOURCE: no SOURCE_DONE ({got['msgs']} DATA messages, {got['bytes']} B)")
    c.send(WIFI, bytes([W_GET]))
    w = c.wait(WIFI_R, 3.0)
    if w:
        res["wifi_after"] = parse_wifi_r(w)
        x = res["wifi_after"]
        print(f"heap after: {x['heap_used']} B used, {x['heap_peak']} B at most, of {x['heap_size']} B")
    c.close()
    return res


def cmd_bench(a):
    host = "127.0.0.1" if a.sim else a.host
    if not host:
        fm1s = discover(2.0, a.to)
        if not fm1s:
            raise SystemExit("wifi_probe: no FM-1 found by discovery: give its address (wifi_probe.py status shows it)")
        if len(fm1s) > 1:
            raise SystemExit("wifi_probe: more than one FM-1: " + ", ".join(f"{d.get('name')} {d['from']}" for d in fm1s))
        host = fm1s[0]["from"]
        print(f"found {fm1s[0].get('name')} at {host}")
    res = bench(host, a.tcp_port, a)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)
    sink, src = res.get("sink", {}), res.get("source", {})
    lossless = sink.get("dev_bytes") == sink.get("bytes") and not (sink.get("gaps") or sink.get("chk")) and \
        src.get("bytes") == a.source_kb * 1024 and not (src.get("gaps") or src.get("bad"))
    print("result: " + ("no loss" if lossless else "LOSS OR SHORT (see above)"))
    return 0 if lossless else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--sim", action="store_true", help="the host simulator on this computer (127.0.0.1)")
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("setup")
    p.add_argument("--port", default="")
    p.add_argument("--ssid", default="")
    p.add_argument("--wait", type=float, default=60.0)
    p = sub.add_parser("status")
    p.add_argument("--port", default="")
    p = sub.add_parser("scan", help="the networks the FM-1 hears (SCAN, then LIST's pages)")
    p.add_argument("--port", default="")
    p = sub.add_parser("forget")
    p.add_argument("--port", default="")
    p.add_argument("which", nargs="?", default="0")
    p = sub.add_parser("watch", help="WIFI DIAG every second over USB while a host drives the unit over TCP")
    p.add_argument("--port", default="")
    p.add_argument("--every", type=float, default=1.0)
    p.add_argument("--seconds", type=float, default=0.0, help="0: until Ctrl-C")
    p.add_argument("--log", default="", help="a JSON record per line appended here")
    p = sub.add_parser("mode", help="whole frames over TCP (default) or as they come")
    p.add_argument("--port", default="")
    p.add_argument("mode", choices=["whole", "direct"])
    p = sub.add_parser("find")
    p.add_argument("--timeout", type=float, default=2.0)
    p.add_argument("--to", default=None, help="a broadcast or unicast address instead of 255.255.255.255")
    p = sub.add_parser("bench")
    p.add_argument("host", nargs="?", default="")
    p.add_argument("--tcp-port", type=int, default=TCP_PORT)
    p.add_argument("--to", default=None)
    p.add_argument("--sink-kb", type=int, default=1024)
    p.add_argument("--source-kb", type=int, default=1024)
    p.add_argument("--echo", type=int, default=100)
    p.add_argument("--json", default="")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    if a.cmd == "setup":
        return cmd_setup(a)
    if a.cmd == "status":
        return cmd_status(a)
    if a.cmd == "forget":
        return cmd_forget(a)
    if a.cmd == "scan":
        return cmd_scan(a)
    if a.cmd == "find":
        return cmd_find(a)
    if a.cmd == "watch":
        return cmd_watch(a)
    if a.cmd == "mode":
        return cmd_mode(a)
    if a.cmd == "bench":
        return cmd_bench(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
