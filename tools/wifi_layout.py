#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""RemoteFM1 Wi-Fi: the built image's RAM layout against fm1-base's examples/wifi (docs/WIFI.md).

  tools/wifi_layout.py build/app-wifi/build/remotefm1-wifi.elf     (./build.sh --wifi runs it)

fm1-base's device/app_wifi.ld: "keep the pool's layout as fm1-radio has it: the radio hardware writes somewhere in it
(unfound), and moving the stacks / heap / lwIP pools by even 4.6 KiB broke its reception". RemoteFM1's Wi-Fi build
keeps the pool before the Wi-Fi heap exactly as long as examples/wifi's (its canvas, 0x5A04 bytes), so the heap and
lwIP's pools start where they do there (src/rfm_wifi.c RF_POOL_PAD). This reads the ELF's symbols (no toolchain
needed), prints the layout, and fails (exit 1) if the heap is not at the example's offset, saying the pad that puts it
there. It also prints what RAMTEXT has left after .ram_text and .ram_bss (app.h RF_LOW).
"""
import struct
import sys

POOL = 0x01C28000                    # device/app_wifi.ld POOL's origin
HEAP_AT = 0x5A04                     # examples/wifi: the heap's offset in the pool (fm1-base 317cb86)
RAMTEXT = (0x01C00000, 0x6000)       # app_wifi.ld RAMTEXT


def symbols(path):
    b = open(path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 1:
        raise SystemExit(f"{path}: not a 32-bit ELF")
    e_shoff, = struct.unpack_from("<I", b, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", b, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", b, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    out = {}
    for s in secs:
        if s[1] != 2:                # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for k in range(s[5] // 16):
            name, value, size, _info, _other, _shndx = struct.unpack_from("<IIIBBH", b, s[4] + 16 * k)
            n = b[strtab[4] + name:b.index(b"\0", strtab[4] + name)].decode(errors="replace")
            if n:
                out.setdefault(n, (value, size))
    return out


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    s = symbols(sys.argv[1])
    need = ("heap", "_pool_start", "_pool_end", "memp_memory_PBUF_POOL_base", "_rt_end", "_rb_start", "_rb_end",
            "_bss_end", "heap2")
    miss = [n for n in need if n not in s]
    if miss:
        raise SystemExit(f"wifi_layout: no {', '.join(miss)} in {sys.argv[1]}")
    heap, hsize = s["heap"]
    off = heap - POOL
    memp = s["memp_memory_PBUF_POOL_base"][0]
    rt_left = RAMTEXT[0] + RAMTEXT[1] - s["_rb_end"][0]
    print(f"wifi     pool: heap at +0x{off:X} ({hsize // 1024} KB), lwIP's pools at +0x{memp - POOL:X}, "
          f"end +0x{s['_pool_end'][0] - POOL:X}; heap2 {s['heap2'][1] // 1024} KB in .bss")
    print(f"wifi     RAMTEXT: .ram_text to 0x{s['_rt_end'][0]:08X}, .ram_bss {s['_rb_end'][0] - s['_rb_start'][0]} B, "
          f"{rt_left} B left")
    if off != HEAP_AT:
        print(f"wifi_layout: the heap is at +0x{off:X}, examples/wifi's at +0x{HEAP_AT:X}: src/rfm_wifi.c RF_POOL_PAD "
              f"{HEAP_AT - off:+d} B", file=sys.stderr)
        return 1
    if memp != heap + hsize:
        print("wifi_layout: lwIP's pools do not follow the heap (as in examples/wifi)", file=sys.stderr)
        return 1
    print("  ok    the pool before the heap as examples/wifi's: the heap and lwIP's pools where they are there")
    return 0


if __name__ == "__main__":
    sys.exit(main())
