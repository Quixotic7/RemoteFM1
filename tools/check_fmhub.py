#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 RemoteFM1 contributors
"""fmhub.json (The Firmware Hub's card for RemoteFM1) against the hub's rules (thefirmwarehub.com/fmhub.md, kb/#fmhub).

  python3 tools/check_fmhub.py

Only the eight fields; `what` English plain text, 20..300 characters, no links, web addresses, < > { } or backticks;
`author` 2..60 characters; one known kind first; https:// links; `image` a path in the repository: a square PNG, WebP
or JPEG, 240..1024 px, 300 KB at most; `color` #RRGGBB and not too dark. The hub's own check is "Check your file" on
thefirmwarehub.com/kb/#fmhub, after the push.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIELDS = {"what", "author", "kinds", "play", "editor", "cheat", "image", "color"}
KINDS = {"synth", "groove", "drums", "chords", "tracker", "other"}


def image_size(b):
    if b[:8] == b"\x89PNG\r\n\x1a\n":
        return "PNG", int.from_bytes(b[16:20], "big"), int.from_bytes(b[20:24], "big")
    if b[:4] == b"RIFF" and b[8:12] == b"WEBP":
        from PIL import Image
        import io
        w, h = Image.open(io.BytesIO(b)).size
        return "WebP", w, h
    if b[:3] == b"\xff\xd8\xff":
        from PIL import Image
        import io
        w, h = Image.open(io.BytesIO(b)).size
        return "JPEG", w, h
    return None, 0, 0


def main():
    errs = []
    j = json.loads((ROOT / "fmhub.json").read_text())
    if set(j) - FIELDS:
        errs.append(f"fields the hub does not allow (the whole file would be unused): {sorted(set(j) - FIELDS)}")
    what = j.get("what", "")
    if not 20 <= len(what) <= 300:
        errs.append(f"what: {len(what)} characters (20..300)")
    if re.search(r"[<>{}`]|https?:|www\.|\.(com|org|net|io)\b", what):
        errs.append("what: no links, web addresses, < > { } or backticks")
    if "author" in j and not 2 <= len(j["author"]) <= 60:
        errs.append("author: 2..60 characters")
    kinds = j.get("kinds", [])
    if not kinds or kinds[0] not in KINDS:
        errs.append(f"kinds: the first must be one of {sorted(KINDS)}")
    for k in ("play", "editor", "cheat"):
        if k in j and not j[k].startswith("https://"):
            errs.append(f"{k}: an https:// address")
    if "image" in j:
        p = ROOT / j["image"]
        if not p.is_file():
            errs.append(f"image: {j['image']} is not in the repository")
        else:
            b = p.read_bytes()
            kind, w, h = image_size(b)
            if kind is None or w != h or not 240 <= w <= 1024 or len(b) > 300 * 1024:
                errs.append(f"image: {kind or 'not PNG/WebP/JPEG'} {w}x{h}, {len(b)} B (square, 240..1024 px, <= 300 KB)")
    c = j.get("color", "#FFFFFF")
    if not re.fullmatch(r"#[0-9A-Fa-f]{6}", c):
        errs.append("color: #RRGGBB")
    else:
        r, g, b = (int(c[i:i + 2], 16) for i in (1, 3, 5))
        if 0.2126 * r + 0.7152 * g + 0.0722 * b < 90:
            errs.append(f"color: {c} is too dark for the hub's dark page")
    for e in errs:
        print("fmhub.json:", e)
    if errs:
        sys.exit(1)
    print(f"fmhub.json: ok ({len(what)} characters of text; image {j.get('image')})")


if __name__ == "__main__":
    main()
