#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# The site's type: Inter Tight (OFL 1.1, fonts/OFL.txt), the variable font FM1VST embeds (plugin/fonts), subset to
# Latin-1 and the punctuation the page prints, kept variable (weights 100..900), saved as WOFF (zlib: no brotli here).
#   python3 web/site/tools/make_font.py <FM1VST>/plugin/fonts/InterTight-VF.ttf web/site/fonts/InterTight-VF-latin.woff
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

src, out = sys.argv[1], sys.argv[2]
font = TTFont(src)
opts = subset.Options()
opts.layout_features = ["kern", "liga", "calt", "tnum", "case", "ccmp", "locl", "mark", "mkmk"]
opts.name_IDs = ["*"]
opts.name_languages = ["*"]
opts.notdef_outline = True
opts.glyph_names = False
opts.flavor = "woff"
sub = subset.Subsetter(opts)
unicodes = list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) + list(range(0x2010, 0x2028)) + \
    list(range(0x2030, 0x203B)) + [0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x2715, 0x2713, 0x00B7, 0x22EF,
                                   0x25B6, 0x2026, 0x2039, 0x203A]
sub.populate(unicodes=unicodes)
sub.subset(font)
font.flavor = "woff"
font.save(out)
