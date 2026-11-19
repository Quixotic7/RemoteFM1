#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""Mockups of the FM-1's own Wi-Fi pages (the Wi-Fi build's standalone: HOME held, the settings, WI-FI): each a
240 x 240 PNG drawn with the firmware's primitives, font and Pixel theme (fm1gfx.py), and a 2x contact sheet.

  python3 design/wifi-settings/make_wifi_mockups.py      (writes the PNGs beside this file)

Only sa_gfx.c's calls are used (rect, chip, text, text_sh, chip_text, sprite), so the firmware can draw each screen
the same way with no new drawing code or font."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

import fm1gfx as g
from fm1gfx import C, W, Screen, fit

OUT = Path(__file__).resolve().parent

# ------------------------------------------------------------------ sprites (sg_sprite rows) ---
ARROW = ["a....", "aa...", "aaa..", "aaaa.", "aaaaa", "aaaa.", "aaa..", "aa...", "a...."]    # sa_screens.c SA_ARROW
ARROW_L = [r[::-1] for r in ARROW]
FAN = [                                  # Wi-Fi: arcs a (outer) .. c, the dot d; 13 x 11
    "....aaaaa....",
    ".aaaaaaaaaaa.",
    "aaa.......aaa",
    "a...bbbbb...a",
    "..bbbbbbbbb..",
    "..bb.....bb..",
    ".....ccc.....",
    "....ccccc....",
    ".............",
    "......d......",
    ".....ddd.....",
]
LOCK = [".aaa.", "a...a", "a...a", "aaaaa", "aa.aa", "aa.aa", "aaaaa"]
STAR = ["...a...", "..aaa..", "aaaaaaa", ".aaaaa.", "..aaa..", ".aa.aa.", ".a...a."]
CHECK = ["......aa", ".....aa.", "aa..aa..", ".aaaa...", "..aa...."]
CROSS = ["aa...aa", ".aa.aa.", "..aaa..", ".aa.aa.", "aa...aa"]
EYE = ["...aaaaa...", ".aa.....aa.", "a...aaa...a", "a..aaaaa..a", "a...aaa...a", ".aa.....aa.", "...aaaaa..."]
EYE_SHUT = ["...........", "...........", "a.........a", ".aa.....aa.", "...aaaaa...", "..a..a..a..", ".a...a...a."]
KEY = [".aaa.......", "a...a......", "a...aaaaaaa", "a...a...a.a", ".aaa....a.a"]
TRASH = ["...aaa...", "aaaaaaaaa", ".a.a.a.a.", ".a.a.a.a.", ".a.a.a.a.", ".a.a.a.a.", ".aaaaaaa."]
TRASH_OPEN = [".aaaaa...", "....aaaa.", ".a.a.a.a.", ".a.a.a.a.", ".a.a.a.a.", ".a.a.a.a.", ".aaaaaaa."]
JOIN = ["aaa......", "a.....a..", "a.....aa.", "a..aaaaaa", "a.....aa.", "a.....a..", "aaa......"]


def one(c):
    return lambda ch: c


def fan(sc, x, y, s, level, on, off):
    """the Wi-Fi fan, its arcs lit to level (0..4: the dot, then c, b, a), on / off colours"""
    lit = {"d": level >= 1, "c": level >= 2, "b": level >= 3, "a": level >= 4}
    sc.sprite(x, y, s, FAN, lambda ch: on if lit[ch] else off)


def level_of(rssi):                      # dBm -> 0..4 (0: not heard)
    if rssi == 0:
        return 0
    return 4 if rssi >= -55 else 3 if rssi >= -65 else 2 if rssi >= -75 else 1


def lvl_col(lv):
    return C["A4"] if lv >= 3 else C["A3"] if lv == 2 else C["A1"]


def bars(sc, x, ybot, lv, s=1, dim=None):
    """four bars rising (3 s wide, 1 s apart, 4..13 s tall), lit to lv in the level's colour"""
    for i in range(4):
        h = (4 + 3 * i) * s
        sc.rect(x + i * 4 * s, ybot - h, 3 * s, h, lvl_col(lv) if i < lv else (dim or C["D5"]))


def head(sc, label, col=None):
    return sc.chip_text(10, 8, 0, 22, 7, label, col or C["A2"], C["ON"])


def chip_r(sc, s, bg, fg, y=8):           # a value chip on the right (Arp's "120 BPM")
    w = sc.text_w(s, 1) + 14
    sc.chip_text(230 - w, y, w, 22, 7, s, bg, fg)
    return 230 - w


def icon_chip(sc, x, y, w, rows, s, bg, fg, txt=None, tfg=None):
    """a chip with a sprite (and a value after it), centred"""
    sc.chip(x, y, w, 22, bg)
    iw, ih = len(rows[0]) * s, len(rows) * s
    tw = sc.text_w(txt, 1) + 4 if txt else 0
    x0 = x + (w - iw - tw) // 2
    sc.sprite(x0, y + (22 - ih) // 2, s, rows, one(fg))
    if txt:
        sc.text(x0 + iw + 4, y + 6, 1, txt, tfg or fg)


# ------------------------------------------------------------------ the settings page, its WI-FI row ---
LABELS = g.SET_LABELS + ["WI-FI"]


def settings(name, value, mode, t=0):
    """sa_paint_settings with the Wi-Fi build's 11th row; mode: off, looking, joined"""
    sc = Screen()
    f, col = 10, C["A5"]                 # (COL[] goes on round: A5 for the 11th row)
    sc.chip_text(10, 8, 220, 22, 0, "SETTINGS", C["A2"], C["ON"])
    sc.text_c(38, 2, LABELS[f - 1], C["PANEL"])
    n = fit(LABELS[f], 3, 210)
    sc.text_sh((W - sc.text_w(LABELS[f], n)) // 2, 64, n, LABELS[f], C["INK"], 3, C["SHADOW"])
    n = fit(value, 5, 180)
    vx = (W - sc.text_w(value, n)) // 2
    sc.text_sh(vx, 100 + (5 - n) * 5, n, value, col if mode != "off" else C["A1"], 4 if n > 1 else 2, C["SHADOW"])
    if mode == "off":                    # OFF / ON: the two stripes, as USB HOST's
        for i in range(2):
            sc.rect(20 + i * 101, 164, 98, 8, C["A1"] if i == 0 else C["D5"])
    elif mode == "looking":              # a light running along four stripes (motion: 150 ms a step)
        for i in range(4):
            sc.rect(20 + i * 51, 164, 47, 8, col if i == t % 4 else C["D5"])
    else:                                # joined: the signal, four stripes rising (bars on their side)
        lv = 3
        for i in range(4):
            sc.rect(20 + i * 51, 172 - (2 + 2 * i), 47, 2 + 2 * i, lvl_col(lv) if i < lv else C["D5"])
    for i in range(len(LABELS)):
        sc.rect(228, 66 + 12 * i, 6, 8, col if i == f else C["PANEL"])
    # OCT+ goes in: the theme screen's arrow, beside the value (it nudges right every second: motion)
    sc.sprite(208, 100, 3, ARROW, one(C["A1"]))
    w = sc.text_w("GLO + FX FOR REMOTE", 1) + 16
    sc.chip_text((W - w) // 2, 210, w, 22, 7, "GLO + FX FOR REMOTE", C["PANEL"], C["INK"])
    sc.save(OUT / name)


# ------------------------------------------------------------------ the Wi-Fi page ---
AROUND = [  # (ssid, rssi, secured, kept, joined) as fm1-base's list: the strongest first, each SSID once
    ("Studio-2G", -48, 1, 1, 1),
    ("NETGEAR-47", -58, 1, 0, 0),
    ("Moonbase", -63, 1, 1, 0),
    ("Cafe Guest", -71, 0, 0, 0),
    ("iPhone (Q)", -74, 1, 0, 0),
    ("DIRECT-HP-LaserJet", -82, 1, 0, 0),
    ("xfinitywifi", -86, 0, 0, 0),
]


def tabs(sc, which, n_around, n_kept, scanning=False, t=0):
    """ALGORITHM's two lists, top right: the networks around (the fan, their count), the ones kept (the star)"""
    x = 116
    on = which == 0
    sc.chip(x, 8, 56, 22, C["A2"] if on else C["PANEL"])
    fan(sc, x + 8, 14, 1, (t % 4) + 1 if scanning else 4, C["ON"] if on else C["A2"], C["D2"] if on else C["MID"])
    sc.text(x + 28, 14, 1, str(n_around), C["ON"] if on else C["SOFT"])
    on = which == 1
    sc.chip(x + 60, 8, 54, 22, C["A3"] if on else C["PANEL"])
    sc.sprite(x + 68, 15, 1, STAR, one(C["ON"] if on else C["A3"]))
    sc.text(x + 82, 14, 1, f"{n_kept}/6", C["ON"] if on else C["SOFT"])


def hero(sc, ssid, rssi, ip, selected):
    """the network joined: the fan lit to its signal, the name big, the address"""
    if selected:
        sc.chip(6, 36, 216, 62, C["PANEL"])
        sc.chip(8, 38, 212, 58, C["BG"])
    fan(sc, 16, 44, 3, level_of(rssi), C["A4"], C["D5"])
    n = fit(ssid, 3, 160)
    sc.text_sh(60, 44 + (3 - n) * 5, n, ssid, C["INK"], 2, C["SHADOW"])
    sc.text(60, 76, 2 if sc.text_w(ip, 2) <= 158 else 1, ip, C["A4"])


def row(sc, y, ssid, rssi, secured, kept, joined, selected, marquee=0):
    """a network: its bars, its name (scale 2; a long one scrolls while picked), a star if kept, a lock if secured"""
    if selected:
        sc.chip(6, y - 6, 216, 30, C["PANEL"])
        sc.sprite(9, y + 1, 2, ["a.", "aa", "aa", "a."], one(C["A2"]))   # (the pick: a notch on the left)
    lv = level_of(rssi)
    bars(sc, 16, y + 16, lv, 1, C["PANEL"] if not selected else C["MID"])
    name = ssid
    n = 2
    maxw = 146
    cut = sc.text_w(name, 2) > maxw      # long: cut, a dim ".." after (picked: it scrolls, 250 ms a character)
    if cut:
        name = name[marquee:]
        while sc.text_w(name + "..", 2) > maxw:
            name = name[:-1]
    x = sc.text(38, y, n, name, C["A4"] if joined else C["INK"] if selected else C["SOFT"])
    if cut:
        sc.text(x, y, 2, "..", C["MID"])
    if kept:
        sc.sprite(186, y + 3, 2, STAR, one(C["A3"]))
    if secured:
        sc.sprite(204, y + 2, 2, LOCK, one(C["INK"] if selected else C["MID"]))


def scroll_dots(sc, n, cur, y0=104):
    for i in range(n):
        sc.rect(228, y0 + 10 * i, 6, 6, C["A2"] if i == cur else C["PANEL"])


def wifi_page(name):
    sc = Screen()
    head(sc, "WI-FI")
    tabs(sc, 0, len(AROUND), 3)
    hero(sc, "Studio-2G", -48, "192.168.1.42", False)
    rows = AROUND[1:5]
    for i, r in enumerate(rows):
        row(sc, 108 + 32 * i, *r, selected=i == 1)
    scroll_dots(sc, len(AROUND), 2)
    sc.save(OUT / name)


def wifi_scanning(name):
    sc = Screen()
    head(sc, "WI-FI")
    tabs(sc, 0, 3, 3, scanning=True, t=2)
    # the hero's place: a sweep. The fan's arcs light from the dot outwards, round and round (120 ms an arc), and a
    # ring widens behind it
    fan(sc, 16, 44, 3, 3, C["A2"], C["D5"])
    for i, w in enumerate((150, 110, 70)):     # three bars sweeping right, fading (the scan's 2-4 s)
        sc.rect(62, 50 + 14 * i, w, 6, [C["A2"], C["D2"], C["D5"]][i])
    found = [AROUND[0], AROUND[1], AROUND[2]]
    for i, r in enumerate(found):              # the networks found so far drop in, strongest first
        row(sc, 108 + 32 * i, *r, selected=False)
    y = 108 + 32 * 3                           # and a place for the next: a dim row breathing
    sc.rect(16, y + 4, 15, 12, C["D5"])
    sc.rect(38, y + 2, 120, 16, C["D5"])
    sc.save(OUT / name)


def wifi_empty(name):
    sc = Screen()
    head(sc, "WI-FI")
    tabs(sc, 0, 0, 0)
    fan(sc, 81, 50, 6, 0, C["A1"], C["PANEL"])    # the fan, dark, its dot orange: nothing heard
    sc.sprite(150, 108, 4, CROSS, one(C["A1"]))
    w = sc.text_w("2.4 GHz", 2) + 24                # the radio's one band: what to look for on a router / a phone
    sc.chip((W - w) // 2, 132, w, 34, C["A3"])
    sc.text((W - sc.text_w("2.4 GHz", 2)) // 2, 139, 2, "2.4 GHz", C["ON"])
    # SELECT up past the top looks again: the up arrow, nudging (motion)
    sc.sprite(112, 190, 3, ["..a..", ".aaa.", "aaaaa"], one(C["A2"]))
    sc.save(OUT / name)


# ------------------------------------------------------------------ a network picked ---
def tile(sc, x, y, w, h, rows, s, col, picked, blink=False):
    if picked:
        sc.chip(x, y, w, h, col)
        fg = C["ON"]
    else:
        sc.chip(x, y, w, h, C["PANEL"])
        sc.chip(x + 2, y + 2, w - 4, h - 4, C["BG"])
        fg = col
    iw, ih = len(rows[0]) * s, len(rows) * s
    sc.sprite(x + (w - iw) // 2, y + (h - ih) // 2, s, rows, one(fg))


def picked(name, armed=False):
    sc = Screen()
    head(sc, "WI-FI")
    chip_r(sc, "CH 6", C["PANEL"], C["INK"])
    fan(sc, 94, 38, 4, level_of(-63), C["A3"], C["D5"])
    ssid = "Moonbase"
    n = fit(ssid, 4, 200)
    sc.text_sh((W - sc.text_w(ssid, n)) // 2, 92, n, ssid, C["INK"], 3, C["SHADOW"])
    # what it is: kept (the star), secured (the lock)
    sc.sprite(96, 140, 2, STAR, one(C["A3"]))
    sc.sprite(132, 140, 2, LOCK, one(C["MID"]))
    # three tiles: JOIN, a new password, FORGET; SELECT picks, OCT+ does it
    sel = 2 if armed else 0
    tile(sc, 12, 168, 66, 58, JOIN, 4, C["A4"], sel == 0)
    tile(sc, 87, 168, 66, 58, KEY, 4, C["A3"], sel == 1)
    if armed:                                  # FORGET armed: red, the lid lifting and shaking; OCT+ again forgets
        tile(sc, 162, 168, 66, 58, TRASH_OPEN, 4, C["D1"], True)
        sc.chip(160, 166, 70, 4, C["A1"])
    else:
        tile(sc, 162, 168, 66, 58, TRASH, 4, C["A1"], False)
    sc.save(OUT / name)


# ------------------------------------------------------------------ the password ---
LAYERS = [  # (chip, black keys B1..B11, white keys W1..W16): FX, SEL, ENV, LFO pick one (GLO held: ABC a moment)
    ("abc", "abcdefghijk", "lmnopqrstuvwxyz "),
    ("ABC", "ABCDEFGHIJK", "LMNOPQRSTUVWXYZ "),
    ("123", "0123456789-", ".,!?@#$%&*()_+= "),
    ("#+=", "0123456789-", "'\"/\\:;<>[]{}|^`~"),
]
assert len({c for _, b, w in LAYERS for c in b + w}) == 95       # every printable ASCII character, once at least
BK = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14]        # the white key each black key follows (F F# G G# A A# B C ..)


def keybed(sc, layer, lit=None, y0=128):
    """the FM-1's keys as they lie: 11 black over 16 white (Elasticat's keybed), a character on each"""
    _, blk, wht = LAYERS[layer]
    for k in range(16):                        # white: 15 apart, 13 wide
        x = 1 + 15 * k
        on = lit == ("w", k)
        sc.rect(x, y0 + 46, 13, 64, C["A1"] if on else C["INK"])
        c = wht[k]
        if c == " ":
            sc.rect(x + 3, y0 + 98, 7, 2, C["BG"])     # SPACE: a bar
        else:
            sc.text(x + (13 - sc.text_w(c, 1)) // 2 + 0, y0 + 92, 1, c, C["ON"] if not on else C["ON"])
    for k in range(11):                        # black: over the gap after their white key
        x = 1 + 15 * (BK[k] + 1) - 8
        on = lit == ("b", k)
        sc.rect(x, y0, 14, 42, C["A1"] if on else C["PANEL"])
        c = blk[k]
        sc.text(x + (14 - sc.text_w(c, 1)) // 2, y0 + 26, 1, c, C["ON"] if on else C["INK"])


def password(name, typed, shown, layer, lit, last_visible=True, ssid="NETGEAR-47"):
    sc = Screen()
    # the network: a chip with its lock and name
    w = min(sc.text_w(ssid, 1) + 34, 176)
    sc.chip(10, 8, w, 22, C["A3"])
    sc.sprite(18, 12, 2, LOCK, one(C["ON"]))
    sc.text(34, 14, 1, ssid, C["ON"])
    # EDIT: shown / hidden, an eye
    eye = EYE if shown else EYE_SHUT
    sc.chip(190, 8, 40, 22, C["A2"] if shown else C["PANEL"])
    sc.sprite(199, 12, 2, [r[1:10] for r in eye], one(C["ON"] if shown else C["SOFT"]))
    # the field: the password (a window on it, the cursor in view), the cursor under its place
    n = len(typed)
    cur = n
    if shown:
        vis = typed
        while sc.text_w(vis, 2) > 200:
            vis = vis[1:]
        x = 14
        if len(vis) < n:
            sc.sprite(4, 44, 2, ["..a", ".aa", "aaa", ".aa", "..a"], one(C["MID"]))
        x = sc.text(x, 42, 2, vis, C["INK"])
        sc.rect(x + 1, 64, 12, 3, C["A1"])                    # the cursor (blinks 500 ms)
    else:
        vis_n = min(n, 13)
        x = 14
        if vis_n < n:
            sc.sprite(4, 44, 2, ["..a", ".aa", "aaa", ".aa", "..a"], one(C["MID"]))
        for i in range(vis_n):
            last = i == vis_n - 1 and last_visible
            if last:                                         # the last one typed shows a second, then a block
                x = sc.text(x, 42, 2, typed[-1], C["A1"]) + 2
            else:
                sc.rect(x + 2, 46, 10, 12, C["SOFT"])
                x += 15
        sc.rect(x + 1, 64, 12, 3, C["A1"])
    # the length: 63 ticks, the 8th taller (a WPA password's least), orange under 8, green from 8; the count
    ok = n >= 8
    for i in range(63):
        h = 12 if i == 7 else 8
        sc.rect(12 + 3 * i, 84 - h, 2, h, (C["A4"] if ok else C["A1"]) if i < n else C["D5"])
    sc.text(206, 72, 2 if n < 10 else 1, str(n), C["A4"] if ok else C["A1"])
    # the layers: FX SEL ENV LFO, left to right, as the buttons lie
    for i, (lab, _, _) in enumerate(LAYERS):
        x = 12 + 56 * i
        on = i == layer
        sc.chip(x, 94, 50, 24, C["A2"] if on else C["PANEL"])
        sc.text(x + (50 - sc.text_w(lab, 1)) // 2, 101, 1, lab, C["ON"] if on else C["SOFT"])
    keybed(sc, layer, lit)
    sc.save(OUT / name)


# ------------------------------------------------------------------ joining, joined, failed ---
def joining(name, stage, t):
    sc = Screen()
    head(sc, "WI-FI")
    chip_r(sc, "NETGEAR-47", C["PANEL"], C["INK"])
    fan(sc, 81, 44, 6, t, C["A2"], C["D5"])   # the arcs light from the dot out, again and again (150 ms an arc)
    # the steps: the radio's handshake (state 3), an address (state 4), done; the one under way breathing
    for i in range(3):
        x = 28 + 64 * i
        c = C["A2"] if i < stage else (C["D2"] if i == stage else C["D5"])
        sc.rect(x, 140, 56, 10, c)
    for i in range(3):
        x = 28 + 64 * i
        c = C["A2"] if i < stage else C["INK"] if i == stage else C["PANEL"]
        if i == 1:                              # an address: its two letters
            sc.text(x + (56 - sc.text_w("IP", 2)) // 2, 162, 2, "IP", c)
        else:
            rows = KEY if i == 0 else CHECK
            sc.sprite(x + (56 - len(rows[0]) * 3) // 2, 164, 3, rows, one(c))
    for i in range(8):                          # USB HOST's blocks along the bottom, running (the time waited)
        sc.rect(18 + 26 * i, 224, 22, 6, C["A2"] if i <= 4 else C["PANEL"])
    sc.save(OUT / name)


def joined(name):
    sc = Screen()
    head(sc, "WI-FI", C["A4"])
    sc.sprite(84, 40, 9, CHECK, one(C["A4"]))  # the check drops in and bounces once
    ssid = "NETGEAR-47"
    n = fit(ssid, 3, 220)
    sc.text_sh((W - sc.text_w(ssid, n)) // 2, 106, n, ssid, C["INK"], 3, C["SHADOW"])
    ip = "192.168.1.57"
    n = fit(ip, 3, 220)
    sc.text_sh((W - sc.text_w(ip, n)) // 2, 152, n, ip, C["A4"], 3, C["D2"])
    for i in range(8):
        sc.rect(18 + 26 * i, 224, 22, 6, C["A4"])
    sc.save(OUT / name)


def failed(name, why="WRONG PASSWORD"):
    sc = Screen()
    head(sc, "WI-FI", C["A1"])
    chip_r(sc, "NETGEAR-47", C["PANEL"], C["INK"])
    fan(sc, 81, 38, 6, 1, C["A1"], C["PANEL"])  # the fan dark, a cross over it (it shakes once, left right left)
    sc.sprite(144, 92, 4, CROSS, one(C["A1"]))
    n = fit(why, 3, 220)
    sc.text_sh((W - sc.text_w(why, n)) // 2, 132 + (3 - n) * 5, n, why, C["A1"], n, C["D1"])
    # OCT+: the password again (kept as typed), OCT-: the list
    tile(sc, 50, 172, 66, 46, KEY, 4, C["A3"], True)
    tile(sc, 124, 172, 66, 46, ARROW_L, 3, C["A2"], False)
    sc.save(OUT / name)


# ------------------------------------------------------------------ the networks kept ---
KEPT = [("Studio-2G", -48, 1, 1, 1), ("Moonbase", -63, 1, 1, 0), ("Grandma's House", 0, 1, 1, 0)]


def saved_list(name):
    sc = Screen()
    head(sc, "WI-FI")
    tabs(sc, 1, len(AROUND), len(KEPT))
    for i, (ssid, rssi, sec, kept, jn) in enumerate(KEPT):
        y = 48 + 34 * i
        sel = i == 1
        if sel:
            sc.chip(6, y - 7, 216, 32, C["PANEL"])
            sc.sprite(9, y + 1, 2, ["a.", "aa", "aa", "a."], one(C["A3"]))
        if jn:
            sc.sprite(16, y + 4, 2, CHECK, one(C["A4"]))
        else:
            sc.sprite(18, y + 2, 2, STAR, one(C["A3"] if rssi else C["MID"]))
        n = fit(ssid, 2, 150)
        sc.text(40, y + (2 - n) * 5, n, ssid, C["A4"] if jn else C["INK"] if rssi else C["MID"])
        bars(sc, 196, y + 16, level_of(rssi), 1, C["D5"] if rssi else C["PANEL"])   # not heard: empty bars
    # the six places: three kept, three free (the most recent first: a new network pushes the oldest out)
    for i in range(6):
        sc.chip(24 + 33 * i, 190, 27, 30, C["A3"] if i < len(KEPT) else C["PANEL"])
        if i < len(KEPT):
            sc.sprite(24 + 33 * i + 7, 198, 2, STAR, one(C["ON"]))
    sc.save(OUT / name)


# ------------------------------------------------------------------ all of them, and the sheet ---
SCREENS = [
    ("01-settings-wifi-off.png", "Settings, WI-FI row: OFF (KN1 on/off, OCT+ opens the Wi-Fi page)"),
    ("02-settings-wifi-looking.png", "WI-FI row while it looks for a kept network: LOOKING, a light runs"),
    ("03-settings-wifi-joined.png", "WI-FI row joined: the network's name, its signal as rising stripes"),
    ("04-wifi-page.png", "Wi-Fi page: joined network, address; networks around, one picked"),
    ("05-wifi-scanning.png", "Scanning: the fan sweeps, networks drop in strongest first"),
    ("06-wifi-empty.png", "Nothing heard: dark fan, 2.4 GHz (the only band the FM-1 hears)"),
    ("07-network-saved.png", "A kept network picked: JOIN, new password, FORGET (SELECT, OCT+)"),
    ("08-network-forget-armed.png", "FORGET armed: red, lid lifting; OCT+ again forgets it"),
    ("09-password-hidden.png", "Password, hidden: blocks, the last key shown a second; 63 ticks, 8th marked"),
    ("10-password-shown.png", "Password, shown (EDIT), the 123 layer (ENV); the key typed glows"),
    ("11-joining.png", "Joining: arcs light outwards; handshake, address, done"),
    ("12-joined.png", "Joined: check, name, address; back to the Wi-Fi page after 2 s"),
    ("13-failed.png", "Failed: the reason; OCT+ the password again (kept), OCT- the list"),
    ("14-saved-networks.png", "Networks kept (ALGORITHM): joined, heard, not heard; 6 places"),
]


def sheet():
    cols, sc2, pad, cap = 4, 2, 24, 48
    rows = (len(SCREENS) + cols - 1) // cols
    cw, ch = W * sc2 + pad, W * sc2 + cap + pad
    img = Image.new("RGB", (cols * cw + pad, rows * ch + pad + 50), (24, 24, 30))
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 16)
        ft = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 26)
    except OSError:
        f = ft = ImageFont.load_default()
    d.text((pad, 14), "RemoteFM1 Wi-Fi build: Wi-Fi on the FM-1 itself (mockups, Pixel theme, firmware font)",
           font=ft, fill=(244, 244, 244))
    for i, (fn, capt) in enumerate(SCREENS):
        x, y = pad + (i % cols) * cw, 60 + (i // cols) * ch
        im = Image.open(OUT / fn).resize((W * sc2, W * sc2), Image.NEAREST)
        img.paste(im, (x, y))
        d.rectangle([x - 1, y - 1, x + W * sc2, y + W * sc2], outline=(70, 70, 80))
        line, ly = fn[:2] + " ", y + W * sc2 + 6
        for word in capt.split():
            if d.textlength(line + " " + word, font=f) > W * sc2:
                d.text((x, ly), line, font=f, fill=(200, 200, 210))
                line, ly = "   " + word, ly + 19
            else:
                line += " " + word
        d.text((x, ly), line, font=f, fill=(200, 200, 210))
    img.save(OUT / "wifi-settings-sheet.png")


def main():
    settings(SCREENS[0][0], "OFF", "off")
    settings(SCREENS[1][0], "LOOKING", "looking", t=1)
    settings(SCREENS[2][0], "Studio-2G", "joined")
    wifi_page(SCREENS[3][0])
    wifi_scanning(SCREENS[4][0])
    wifi_empty(SCREENS[5][0])
    picked(SCREENS[6][0])
    picked(SCREENS[7][0], armed=True)
    password(SCREENS[8][0], "moonlight7", False, 0, ("b", 6))
    password(SCREENS[9][0], "Fm1-loves-wifi!", True, 2, ("w", 2))
    joining(SCREENS[10][0], 1, 3)
    joined(SCREENS[11][0])
    failed(SCREENS[12][0])
    saved_list(SCREENS[13][0])
    sheet()


if __name__ == "__main__":
    main()
