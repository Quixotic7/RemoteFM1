#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The FIZZ .rfm1's manifest, finished: fm1_add_rfm1 (FM1VST's tools/rfm1_pack.py) writes the descriptor's licence
(FIZZ's, MPL-2.0) and no glue_url. The file also carries FM1VST's glue (GPL-3.0-only), plutovg (MIT), FreeType's
rasterizer (FTL), Inter Tight (OFL-1.1) and Emscripten's runtime (MIT), so its licence is the expression of all of
them, and glue_url names FM1VST at the commit built. The `rfm1` section is replaced in place; the rest is kept.
Python 3 standard library only.

    manifest.py --in build/rfm1/fizz-X.rfm1 --out web/site/fw/fizz-X.rfm1 --license "MPL-2.0 AND ..." \\
        --glue-url https://github.com/Quixotic7/FM1VST/tree/<commit> --source-commit <FIZZ's commit>
"""
import argparse
import json
import sys


def leb(b, at):
    r = sh = 0
    while True:
        x = b[at]
        at += 1
        r |= (x & 0x7F) << sh
        if not x & 0x80:
            return r, at
        sh += 7


def enc(v):
    out = bytearray()
    while True:
        x, v = v & 0x7F, v >> 7
        out.append(x | 0x80 if v else x)
        if not v:
            return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--license", required=True)
    ap.add_argument("--glue-url", required=True)
    ap.add_argument("--source-commit", required=True, help="the commit source_url must name")
    a = ap.parse_args()
    b = open(a.src, "rb").read()
    if b[:8] != b"\0asm\x01\0\0\0":
        sys.exit("manifest.py: %s is not a WebAssembly module" % a.src)
    out, at, done = bytearray(b[:8]), 8, False
    while at < len(b):
        start, sid = at, b[at]
        size, p = leb(b, at + 1)
        end = p + size
        if sid == 0:
            n, q = leb(b, p)
            if b[q:q + n] == b"rfm1":
                m = json.loads(b[q + n:end].decode("utf-8"))
                if not m.get("source_url", "").endswith("@" + a.source_commit):
                    sys.exit("manifest.py: source_url %s is not at %s" % (m.get("source_url"), a.source_commit))
                # (the keys in the format's order: glue_url before built_by)
                new = {}
                for k, v in m.items():
                    if k == "built_by":
                        new["glue_url"] = a.glue_url
                    new[k] = a.license if k == "license" else v
                new.setdefault("glue_url", a.glue_url)
                body = enc(4) + b"rfm1" + json.dumps(new, ensure_ascii=False, indent=2).encode("utf-8")
                out += b"\0" + enc(len(body)) + body
                at, done = end, True
                print(json.dumps(new, ensure_ascii=False, indent=2))
                continue
        out += b[start:end]
        at = end
    if not done:
        sys.exit("manifest.py: %s has no rfm1 section" % a.src)
    open(a.out, "wb").write(out)


if __name__ == "__main__":
    main()
