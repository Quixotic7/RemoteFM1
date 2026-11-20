# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""The standalone screens' drawing (firmware/sa/sa_gfx.c) on the host, for mockups: the same primitives, the same
pixel font (firmware/build/gen/sa_font.h: Jersey 10 as a grid font), the Pixel theme's tokens (firmware/sa/sa_theme.h)
through RGB565 as the panel shows them. A screen drawn here with the firmware's calls is the screen the firmware
would draw (checked against the simulator's shot of the settings page: check_against_sim)."""
import re
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
FW = HERE.parent.parent / "firmware"
W = H = 240


def _565(rgb):
    r, g, b = rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255
    c = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
    r, g, b = c >> 11 & 31, c >> 5 & 63, c & 31        # back to RGB888 as the simulator's shots do
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


# the Pixel theme (sa_theme.h SA_TH_FULL "Pixel"), the default
PIXEL = dict(BG=0x000000, BORDER=0x000000, PANEL=0x333c57, INK=0xf4f4f4, A1=0xef7d57, A2=0x73eff7, A3=0xffcd75,
             A4=0xa7f070, SHADOW=0x5d275d, ON=0x000000, MID=0x566c86, SOFT=0x94b0c2, A5=0x41a6f6, D1=0xb13e53,
             D2=0x257179, D5=0x29366f)
C = {k: _565(v) for k, v in PIXEL.items()}


def _font():
    src = (FW / "build" / "gen" / "sa_font.h").read_text()
    g = re.search(r"SA_PF_G\[SA_PF_N\] = \{(.*?)\n\};", src, re.S).group(1)
    glyphs = [tuple(int(v) for v in m.groups()) for m in re.finditer(r"\{(-?\d+), (-?\d+), (-?\d+)\}", g)]
    b = re.search(r"SA_PF_BITS\[SA_PF_N\]\[SA_PF_ROWS\] = \{(.*?)\n\};", src, re.S).group(1)
    bits = [[int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{4}", row)]
            for row in re.findall(r"\{(0x[^}]*)\}", b)]
    assert len(glyphs) == 97 and len(bits) == 97
    return glyphs, bits


GLYPHS, BITS = _font()
ROWS, CAP = 14, 10


def gi(ch):
    o = ord(ch)
    if 32 <= o < 127:
        return o - 32
    if o == 0xB0:
        return 95
    if o == 0xF8:
        return 96
    return ord("?") - 32


class Screen:
    """One 240 x 240 frame; the methods are sa_gfx.c's sg_* (no bands: the whole frame at once)."""

    def __init__(self, bg=None):
        self.img = Image.new("RGB", (W, H), bg or C["BG"])
        self.px = self.img.load()

    def rect(self, x, y, w, h, c):
        x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, W), min(y + h, H)
        for yy in range(y0, y1):
            for xx in range(x0, x1):
                self.px[xx, yy] = c

    def chip(self, x, y, w, h, c):              # sg_chip: corners stepping in 4 then 2 over two rows each
        self.rect(x + 4, y, w - 8, 2, c)
        self.rect(x + 2, y + 2, w - 4, 2, c)
        self.rect(x, y + 4, w, h - 8, c)
        self.rect(x + 2, y + h - 4, w - 4, 2, c)
        self.rect(x + 4, y + h - 2, w - 8, 2, c)

    @staticmethod
    def text_w(s, n):
        return sum(GLYPHS[gi(ch)][0] for ch in s) * n

    def text(self, x, y, n, s, c):              # sg_text: capitals' top at y
        top = y - 2 * n
        for ch in s:
            adv, x0, _ = GLYPHS[gi(ch)]
            gx = x + x0 * n
            for r, bits in enumerate(BITS[gi(ch)]):
                col = 0
                while bits:
                    while not bits & 1:
                        bits >>= 1
                        col += 1
                    run = 0
                    while bits & 1:
                        bits >>= 1
                        run += 1
                    self.rect(gx + col * n, top + r * n, run * n, n, c)
                    col += run
            x += adv * n
        return x

    def text_sh(self, x, y, n, s, c, d, sh):
        self.text(x + d, y + d, n, s, sh)
        return self.text(x, y, n, s, c)

    def text_c(self, y, n, s, c):
        self.text((W - self.text_w(s, n)) // 2, y, n, s, c)

    def chip_text(self, x, y, w, h, px, s, bg, fg):
        tw = self.text_w(s, 1)
        if not w:
            w = tw + 2 * px
        self.chip(x, y, w, h, bg)
        self.text(x + (w - tw) // 2, y + (h - CAP) // 2, 1, s, fg)
        return w

    def sprite(self, x, y, s, rows, col):       # sg_sprite: '.' clear, else col(ch) (None skips)
        for r, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch == "." or col(ch) is None:
                    continue
                self.rect(x + i * s, y + r * s, s, s, col(ch))

    def save(self, path):
        self.img.save(path)


def fit(s, nmax, wmax, nmin=1):
    """the largest scale <= nmax at which s fits wmax (the firmware's `for (n = 5; n > 1 && w > max; n--)`)"""
    n = nmax
    while n > nmin and Screen.text_w(s, n) > wmax:
        n -= 1
    return n


def paint_settings(sc, labels, f, value, nv, cur, col_tokens, colour):
    """sa_screens.c sa_paint_settings, as it is (the Wi-Fi row's version is in make_wifi_mockups.py)"""
    sc.chip_text(10, 8, 220, 22, 0, "SETTINGS", C["A2"], C["ON"])
    if f > 0:
        sc.text_c(38, 2, labels[f - 1], C["PANEL"])
    n = fit(labels[f], 3, 210)
    sc.text_sh((W - sc.text_w(labels[f], n)) // 2, 64, n, labels[f], C["INK"], 3, C["SHADOW"])
    n = fit(value, 5, 214)
    sc.text_sh((W - sc.text_w(value, n)) // 2, 100 + (5 - n) * 5, n, value, colour, 4 if n > 1 else 2, C["SHADOW"])
    sw = (200 - 3 * (nv - 1)) // nv
    x0 = (W - (nv * sw + 3 * (nv - 1))) // 2
    for i in range(nv):
        sc.rect(x0 + i * (sw + 3), 164, sw, 8, colour if i == cur else C["D5"])
    if f + 1 < len(labels):
        sc.text_c(182, 2, labels[f + 1], C["PANEL"])
    for i in range(len(labels)):
        sc.rect(228, 66 + 12 * i, 6, 8, colour if i == f else C["PANEL"])
    w = sc.text_w("GLO + FX FOR REMOTE", 1) + 16
    sc.chip_text((W - w) // 2, 210, w, 22, 7, "GLO + FX FOR REMOTE", C["PANEL"], C["INK"])


SET_LABELS = ["SCREEN", "KNOBS", "CC CHANNEL", "KEY CHANNEL", "KEYS", "OCT KEYS", "SCALE SHORTCUTS", "KNOB LEDS",
              "VELOCITY", "USB HOST"]


def check_against_sim(sim_png):
    """the USB HOST row (off), drawn here, against the Wi-Fi build simulator's shot of it: pixels that differ"""
    sc = Screen()
    paint_settings(sc, SET_LABELS, 9, "OFF", 2, 0, None, C["A2"])
    ref = Image.open(sim_png).convert("RGB")
    a, b = sc.img.load(), ref.load()
    return sum(1 for y in range(H) for x in range(W) if a[x, y] != b[x, y])


if __name__ == "__main__":
    import sys
    print("pixels differing:", check_against_sim(sys.argv[1]))
