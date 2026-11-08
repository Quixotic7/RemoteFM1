#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""The SMK-37's BTIF sector (flash 0xC0000-0xC0FFF), read and rebuilt. docs/SMK37.md, "Bluetooth MIDI gone after
another firmware".

The sector holds one record in the JieLi SDK's VM format: id 102, the classic Bluetooth address (6 bytes). There are no
names or radio trims in it. Record: [CRC-16/XMODEM of the data, low byte] [id 7:0] [id 11:8 | len 3:0 << 4] [len 11:4]
[data]. A healthy unit's sector also has zeros at 0x200-0xFFF; a rebuilt one leaves them erased (0xFF).

  tools/smk37_btif.py show FILE                   the records in a 4 KiB sector file or a whole 1 MiB dump
  tools/smk37_btif.py build AA:BB:CC:DD:EE:FF OUT a 4 KiB sector holding that address, ready to write at 0xC0000
  tools/smk37_btif.py --selftest
"""
import sys

SECTOR = 0x1000
BTIF = 0xC0000
ID_BT_MAC = 102


def xmodem(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def record(rid, data):
    n = len(data)
    return bytes([xmodem(data) & 0xFF, rid & 0xFF, (rid >> 8) & 0x0F | (n & 0x0F) << 4, n >> 4]) + data


def build(mac):
    return record(ID_BT_MAC, mac).ljust(SECTOR, b"\xff")


def parse(sector):
    """[(offset, id, data)] up to the first record whose check fails (an erased header always fails)."""
    out, p = [], 0
    while p + 4 <= len(sector):
        h = sector[p:p + 4]
        rid, n = h[1] | (h[2] & 0x0F) << 8, h[2] >> 4 | h[3] << 4
        data = sector[p + 4:p + 4 + n]
        if h == b"\xff" * 4 or rid == 0 or n == 0 or len(data) != n or xmodem(data) & 0xFF != h[0]:
            break
        out.append((p, rid, data))
        p += 4 + n
    return out


def parse_mac(text):
    mac = bytes.fromhex(text.replace(":", "").replace("-", ""))
    if len(mac) != 6:
        raise SystemExit("the address is 6 bytes, as AA:BB:CC:DD:EE:FF")
    return mac


def show(path):
    raw = open(path, "rb").read()
    if len(raw) == 0x100000:
        raw = raw[BTIF:BTIF + SECTOR]
    elif len(raw) != SECTOR:
        raise SystemExit(f"{path}: {len(raw)} bytes; give the 4 KiB sector or the whole 1 MiB dump")
    recs = parse(raw)
    if not recs:
        state = "erased" if raw == b"\xff" * SECTOR else "no valid record (damaged)"
        print(f"{path}: {state}")
        return 1
    for off, rid, data in recs:
        what = "classic Bluetooth address " + data.hex(":").upper() if rid == ID_BT_MAC and len(data) == 6 else data.hex(" ")
        print(f"0x{BTIF + off:05X}  id {rid}: {what}")
    return 0 if any(r[1] == ID_BT_MAC for r in recs) else 1


def selftest():
    # checked against a healthy SMK-37 Pro's dump (its address is not kept here): header 66 60 00, the CRC's low byte
    assert xmodem(b"123456789") == 0x31C3
    mac = bytes.fromhex("02A1B2C3D4E5")
    s = build(mac)
    assert s[:10] == bytes([xmodem(mac) & 0xFF, 0x66, 0x60, 0x00]) + mac and len(s) == SECTOR
    assert s[10:] == b"\xff" * (SECTOR - 10)
    assert parse(s) == [(0, ID_BT_MAC, mac)]
    assert parse(b"\xff" * SECTOR) == [] and parse(b"\x00" * SECTOR) == []
    bad = bytearray(s); bad[5] ^= 1
    assert parse(bytes(bad)) == []
    assert record(0x123, b"\x01" * 0x45)[1:4] == bytes([0x23, 0x51, 0x04])
    print("selftest ok")
    return 0


def main(argv):
    if argv == ["--selftest"]:
        return selftest()
    if len(argv) == 2 and argv[0] == "show":
        return show(argv[1])
    if len(argv) == 3 and argv[0] == "build":
        open(argv[2], "wb").write(build(parse_mac(argv[1])))
        print(f"{argv[2]}: 4 KiB for 0xC0000, address {argv[1].upper()}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
