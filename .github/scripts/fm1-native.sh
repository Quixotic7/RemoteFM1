#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# An author's native build, start to end: what the reusable workflow .github/workflows/fm1-native.yml runs, and what
# runs the same way on a computer (docs/NATIVE-AUTHORS.md "Try it before you tag").
#   1. web/engines/native/build.sh --ext: the author's tree on FM1VST's glue, to WebAssembly, and the same core with
#      the host's compiler (its first screen: the reference picture);
#   2. web/engines/native/test.mjs on the module: it boots, draws, sounds after the prelude, its flash and reboot
#      scripts (when the glue has them), 10 s of rendering, its first screen against the host build's;
#   3. web/engines/native/fm1web.mjs pack: <id>-<version>.fm1web (manifest.json, the module, the conformance script),
#      checked as the site will check it (its manifest, its imports, its size).
# The attestation and the upload to the release are the workflow's own steps, after this.
#
# Environment (the workflow's inputs; only FM1N_TREE, FM1N_ID and FM1N_FAMILY are required):
#   FM1N_TREE        the author's checkout at the release tag, with its submodules
#   FM1VST_DIR       an FM1VST checkout (its cmake/, core-api/, core-glue/; the firmware submodules are not needed)
#   FM1N_ID          the firmware's id on the site (the Hub's id)
#   FM1N_CORE_ID     the core's own id (fm1_add_core's; default FM1N_ID)
#   FM1N_FAMILY      felucca_web fm1base sloop dinghy choralroot plat melodee tonitor speak37 custom
#   FM1N_GLUE        a folder in the author's tree with their own glue units (and a core.cmake, or FM1N_CORE)
#   FM1N_CORE        the fm1_add_core arguments after SOURCE, as text (UNIT ... DESC ... GEN ... VERSION ...)
#   FM1N_TEST        conformance lines that replace the glue's (e.g. "sound btn PLAY 100"): a directive given here
#                    replaces every line of that directive in the glue's file
#   FM1N_VERSION     the version (default: the tag without a leading v, else the glue's)
#   FM1N_REPO, FM1N_TAG, FM1N_RUN   owner/repo, the tag, the run's URL (the workflow sets them)
#   FM1N_USB_PRODUCT, FM1N_EDITOR   (default: the Hub's catalog entry of FM1N_ID)
#   FM1N_HUB         the Hub's catalog (default https://thefirmwarehub.com/catalog.json; "none" for none)
#   FM1N_OUT         where the .fm1web goes (default dist/); FM1N_WORK the build's own folder (default build-fm1web/)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NATIVE="$ROOT/web/engines/native"
: "${FM1N_TREE:?FM1N_TREE: the checkout of the firmware}"
: "${FM1N_ID:?FM1N_ID: the firmware id}"
: "${FM1N_FAMILY:?FM1N_FAMILY: the glue family}"
: "${FM1VST_DIR:?FM1VST_DIR: an FM1VST checkout}"
export FM1VST_DIR
CORE_ID="${FM1N_CORE_ID:-$FM1N_ID}"
OUT="${FM1N_OUT:-$PWD/dist}"
WORK="${FM1N_WORK:-$PWD/build-fm1web}"
HUB="${FM1N_HUB:-https://thefirmwarehub.com/catalog.json}"
mkdir -p "$OUT" "$WORK"
OUT="$(cd "$OUT" && pwd)"
WORK="$(cd "$WORK" && pwd)"
summary() { echo "$*"; [ -z "${GITHUB_STEP_SUMMARY:-}" ] || echo "$*" >> "$GITHUB_STEP_SUMMARY"; }

ver="${FM1N_VERSION:-}"
if [ -z "$ver" ] && [ -n "${FM1N_TAG:-}" ]; then ver="${FM1N_TAG#v}"; fi

# ---- 1. the build
args=(--ext "$FM1N_TREE" --id "$CORE_ID" --family "$FM1N_FAMILY" --out "$WORK/build" --host)
[ -z "$ver" ] || args+=(--version "$ver")
if [ -n "${FM1N_GLUE:-}" ]; then
    g="$FM1N_TREE/$FM1N_GLUE"
    [ -d "$g" ] || { echo "fm1-native: glue: no folder $FM1N_GLUE in the repository" >&2; exit 2; }
    args+=(--glue "$g")
fi
if [ -n "${FM1N_CORE:-}" ]; then
    printf '%s\n' "$FM1N_CORE" > "$WORK/core-args.txt"
    args+=(--core "$WORK/core-args.txt")
fi
bash "$NATIVE/build.sh" "${args[@]}"
meta="$WORK/build/meta.json"
wasm="$WORK/build/$CORE_ID.wasm"

# ---- 2. the conformance test (the glue's script, with the author's lines over it)
conf="$WORK/conformance.txt"
node -e '
const fs = require("fs"), [meta, src, test, out] = process.argv.slice(1);
const m = JSON.parse(fs.readFileSync(meta, "utf8"));
const base = m.conformance ? fs.readFileSync(src + "/" + m.conformance, "utf8").split("\n") : [];
const extra = (test || "").split("\n").map((l) => l.trim()).filter((l) => l && !l.startsWith("#"));
const word = (l) => l.replace(/#.*/, "").trim().split(/\s+/)[0];
const over = new Set(extra.map(word));
const lines = [`# ${m.id}: ${m.conformance ? "FM1VST " + m.conformance : "no conformance file in the glue"}` + (extra.length ? ", with the workflow input test: over it" : "")];
for (const l of base) if (!over.has(word(l)) || !word(l)) lines.push(l);
lines.push(...extra);
fs.writeFileSync(out, lines.join("\n") + "\n");' "$meta" "$WORK/build/src" "${FM1N_TEST:-}" "$conf"
ref=()
[ -f "$WORK/build/$CORE_ID-host.ppm" ] && ref=(--reference "$WORK/build/$CORE_ID-host.ppm")
set +e
node "$NATIVE/test.mjs" --wasm "$wasm" --id "$CORE_ID" --conformance "$conf" "${ref[@]}" --out "$WORK/test" \
    --json "$WORK/test.json" | tee "$WORK/test.log"
st=${PIPESTATUS[0]}
set -e
summary "### $FM1N_ID: the conformance test"
summary '```'
grep -E '^  (ok|FAIL|  )' "$WORK/test.log" | sed 's/^  //' >> "${GITHUB_STEP_SUMMARY:-/dev/null}" || true
summary '```'
[ "$st" -eq 0 ] || { echo "fm1-native: the conformance test failed (above)" >&2; exit 1; }

# ---- 3. the package
pk=(pack --build "$WORK/build" --report "$WORK/test.json" --conformance "$conf" --out "$OUT" --id "$FM1N_ID")
[ -z "${FM1N_REPO:-}" ] || pk+=(--repo "$FM1N_REPO")
[ -z "${FM1N_TAG:-}" ] || pk+=(--tag "$FM1N_TAG")
[ -z "${FM1N_RUN:-}" ] || pk+=(--run "$FM1N_RUN")
[ -z "${FM1N_USB_PRODUCT:-}" ] || pk+=(--usb-product "$FM1N_USB_PRODUCT")
[ -z "${FM1N_EDITOR:-}" ] || pk+=(--editor "$FM1N_EDITOR")
[ "$HUB" = none ] || pk+=(--hub "$HUB")
file="$(node "$NATIVE/fm1web.mjs" "${pk[@]}")"
sha="$(node -e 'process.stdout.write(require("crypto").createHash("sha256").update(require("fs").readFileSync(process.argv[1])).digest("hex"))' "$file")"
summary ""
summary "**$(basename "$file")**: $(wc -c < "$file" | tr -d ' ') bytes, SHA-256 \`$sha\`"
if [ -n "${GITHUB_OUTPUT:-}" ]; then
    { echo "file=$file"; echo "sha256=$sha"; echo "name=$(basename "$file")"; } >> "$GITHUB_OUTPUT"
fi
echo "$file"
