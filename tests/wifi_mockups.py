#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""The Wi-Fi build's own Wi-Fi screens, shot by the simulator (firmware/tests/wifi_page.txt: build/host/sim/wifi_NN_*.png),
against the mockups they were built from (design/wifi-settings/NN-*.png): the pixels that differ, screen by screen,
and a sheet (simulator | mockup | the differing pixels in red) beside the shots.

  python3 tests/wifi_mockups.py [SHOTS_DIR]        (default build/host/sim)

Exit 0 always: some screens move (a light running, a scan, the time waited) or show the simulator's own values (its
address, its signal), so a difference is reported, not failed. EXPECT lists the screens that must match exactly
(exit 1 when one does not)."""
import glob
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.path.join(ROOT, "design", "wifi-settings")
EXPECT = {"01", "04", "06", "07", "10", "13", "14"}   # (still screens with the mockups' own values; 08's lid shakes)


def main():
    shots = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "host", "sim")
    pairs = []
    for s in sorted(glob.glob(os.path.join(shots, "wifi_[0-9][0-9]_*.png"))):
        num = os.path.basename(s).split("_")[1]
        m = glob.glob(os.path.join(MOCK, num + "-*.png"))
        if m:
            pairs.append((num, s, m[0]))
    if not pairs:
        print("no shots in", shots)
        return 1
    W = 240
    sheet = Image.new("RGB", (3 * W + 40, len(pairs) * (W + 10) + 10), (40, 40, 40))
    bad = 0
    for i, (num, s, m) in enumerate(pairs):
        a, b = Image.open(s).convert("RGB"), Image.open(m).convert("RGB")
        pa, pb = a.load(), b.load()
        d = Image.new("RGB", (W, W), (0, 0, 0))
        pd = d.load()
        n = 0
        for y in range(W):
            for x in range(W):
                if pa[x, y] != pb[x, y]:
                    n += 1
                    pd[x, y] = (255, 40, 40)
                else:
                    r, g, bb = pa[x, y]
                    pd[x, y] = (r // 4, g // 4, bb // 4)
        y0 = 10 + i * (W + 10)
        sheet.paste(a, (10, y0))
        sheet.paste(b, (W + 20, y0))
        sheet.paste(d, (2 * W + 30, y0))
        must = num in EXPECT
        print("%s %-34s %6d pixels differ%s" % (num, os.path.basename(m), n,
                                                 "  (must match)" if must else ""))
        bad += must and n > 0
    out = os.path.join(shots, "wifi_vs_mockups.png")
    sheet.save(out)
    print("sheet:", out)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
