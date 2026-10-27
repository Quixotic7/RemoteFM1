#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_install.py's package checks, without a device: the released RemoteFM1 packages (each one
web/installer/releases.json lists, with its identity) and M-VAVE's official V15 and V16 pass package_damage (the CRC
is checked only for the entries stored plain); a flipped byte in flash.bin or ota.bin is caught; V15 loads without
--force; V16 is refused up front, even with --force (its flash.bin reaches past the app area RemoteFM1's loader
writes), with the way there: V15, then M-UPGRADE.

  python3 tests/install_test.py
  V15_PKG=path/FM-1.fwsc V16_PKG=path/FM-1.fwsc python3 tests/install_test.py   (default: a MVaveOfficial/ folder
                                       beside a parent of this tree; M-VAVE's files are not in it: skipped without)
"""
import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_install as I  # noqa: E402

fails = checks = 0


def ok(cond, what):
    global fails, checks
    checks += 1
    fails += not cond
    print(f"{what:<84} {'ok' if cond else 'FAIL'}")


def official(env, name):
    if os.environ.get(env):
        return Path(os.environ[env])
    for p in ROOT.parents:
        if (p / "MVaveOfficial" / name).is_file():
            return p / "MVaveOfficial" / name
    return None


def refusal(path, force=False):
    try:
        I.load_package(str(path), force)
    except I.InstallError as e:
        return str(e)
    return ""


def flipped(raw, logical_off):
    """raw with one byte of the logical image flipped (past the 20 interleave blocks: raw offset + 20)"""
    b = bytearray(raw)
    b[logical_off + I.BLOCKS] ^= 0x01
    return bytes(b)


def entry(image, name):
    return next(f for f in I._files(image) if f[4] == name)


# every package the site offers (web/installer/releases.json: each release's default package and its USB-only one)
REL_DIR = ROOT / "web" / "installer" / "firmware"
for r in json.loads((ROOT / "web" / "installer" / "releases.json").read_text())["releases"]:
    for name in [f"remote-{r['version']}.fwsc"] + ([r["usbOnly"]["file"]] if r.get("usbOnly") else []):
        p = REL_DIR / name
        ok(I.package_damage(I.logical_image(p.read_bytes())) is None, f"the released {name}: no damage")
        product, _, off = I.load_package(str(p), False)
        ok(product == r["identity"] and off is None, f"the released {name}: {product}, loads without --force, not an official package")
rel = REL_DIR / f"remote-{json.loads((ROOT / 'web' / 'installer' / 'releases.json').read_text())['releases'][0]['version']}.fwsc"
raw = rel.read_bytes()
img = I.logical_image(raw)
_, _, fo, fs, _ = entry(img, "flash.bin")
ok(I.package_damage(I.logical_image(flipped(raw, fo + fs // 2))) == "flash.bin: CRC", "a flipped byte in flash.bin: caught")
_, _, oo, os_, _ = entry(img, "ota.bin")
ok(I.package_damage(I.logical_image(flipped(raw, oo + 3))) == "ota.bin: CRC", "a flipped byte in ota.bin: caught")
ok(I.package_damage(img[:0x400 + 100]) == "flash.bin: cut short", "a package cut short: caught")

with tempfile.TemporaryDirectory() as d:
    bad = Path(d) / "bad.fwsc"
    bad.write_bytes(flipped(raw, fo + 7))
    ok("damaged package (flash.bin: CRC)" in refusal(bad, True), "load_package: a damaged flash.bin refused, even with --force")

v15, v16 = official("V15_PKG", "V15-FM-1.fwsc"), official("V16_PKG", "V16-FM-1.fwsc")
if v15:
    r15 = v15.read_bytes()
    i15 = I.logical_image(r15)
    ok(I.package_damage(i15) is None, "M-VAVE's V15: no damage (USR, isd_config.ini, ... stored encrypted: not CRC-checked)")
    product, _, off = I.load_package(str(v15), False)
    ok(product == "FM-1_015" and off == "V15", "M-VAVE's V15: recognised by its SHA-256, loads without --force")
    ok(I.flash_size(i15) == I.APP_HI, "M-VAVE's V15: flash.bin is the app area's size (0x93000)")
    _, _, fo, fs, _ = entry(i15, "flash.bin")
    with tempfile.TemporaryDirectory() as d:
        bad = Path(d) / "FM-1.fwsc"
        bad.write_bytes(flipped(r15, fo + 0x4000 + 123))
        e = refusal(bad)
        ok("flash.bin: CRC" in e, "M-VAVE's V15 with a flipped byte in flash.bin: refused as damaged")
else:
    print("(M-VAVE's V15 not found: set V15_PKG; its checks skipped)")
if v16:
    r16 = v16.read_bytes()
    i16 = I.logical_image(r16)
    ok(I.package_damage(i16) is None, "M-VAVE's V16: no damage (isd_config.ini, farg.cfg, ... encrypted: not CRC-checked)")
    ok(I.product_of(r16) == "FM-1_016" and I.flash_size(i16) == 692224, "M-VAVE's V16: FM-1_016, flash.bin 692224 B")
    e = refusal(v16)
    ok("official V16" in e and "official V15 first" in e and "M-UPGRADE" in e and "no Felucca" not in e,
       "M-VAVE's V16: refused up front (past the app area): V15 first, then M-UPGRADE")
    ok(refusal(v16, True) == e, "M-VAVE's V16: refused with --force too")
    _, _, fo, fs, _ = entry(i16, "flash.bin")
    ok(I.package_damage(I.logical_image(flipped(r16, fo + fs - 9))) == "flash.bin: CRC", "M-VAVE's V16 with a flipped byte in flash.bin: caught")
else:
    print("(M-VAVE's V16 not found: set V16_PKG; its checks skipped)")

# --info's words for an identity: RemoteFM1's own (fm1-base's FM-1_52 since 0.2) never "official"
D = I.describe_identity
ok(D("FM-1_52000200") == "running RemoteFM1 0.2.0" and D("FM-1_52011003") == "running RemoteFM1 1.10.3",
   "--info: FM-1_52XXYYZZ is RemoteFM1 X.Y.Z")
ok("RemoteFM1" in D("FM-1_5200") and "official" not in D("FM-1_5200"), "--info: FM-1_5200 is a RemoteFM1 development build")
ok("official V15" in D("FM-1_015") and "RemoteFM1 0.1" in D("FM-1_015"), "--info: FM-1_015 is V15, or RemoteFM1 0.1")
ok(D("FM-1_016") == "running M-VAVE's official V16" and "update loader" in D("ota-FM-1_900"), "--info: V16, a loader")
ok(D("FM-1_46000100") == "running" and D("FM-1_099") == "running" and D("FM-1_520") == "running",
   "--info: another firmware (FIZZ, m-bootloader's installer, a short 52) not named")
# RemoteFM1 0.2's own package, when built (./build.sh): fm1-base's, with the loader marker, the app area's size
for pkg in sorted((ROOT / "firmware" / "build").glob("remotefm1*.fwsc")):
    r = pkg.read_bytes()
    product, _, off = I.load_package(str(pkg), False)
    ok(product.startswith("FM-1_52") and off is None and I.package_damage(I.logical_image(r)) is None and
       I.flash_size(I.logical_image(r)) <= I.APP_HI, f"the built {pkg.name}: {product}, loads without --force, no damage")

print(f"install tests: {fails} of {checks} FAILED" if fails else f"install tests passed ({checks})")
sys.exit(1 if fails else 0)
