#!/usr/bin/env bash
# Build fm1-emu's web crate (web/vendor/fm1-emu, crates/fm1-web) into web/build/emu/fm1_web.wasm,
# with the flags fm1-emu's own web/build.sh uses (the exported, growable function table is what
# its WebAssembly JIT installs compiled blocks in). No bindings generator: plain extern "C" exports.
#   web/engines/emu/build.sh [out-dir]
# The build compiles a clean copy of the submodule's pinned commit (git archive) with our
# patches applied, web/engines/emu/patches/*.patch in order (UPSTREAM.md there: what each
# fixes and its upstream PR). The submodule itself stays at upstream's commit, untouched; when
# upstream takes a fix, bump the pin and delete its patch. EMU_PATCHES=0: build without them.
# Local (macOS): rustup's stable toolchain, which has the wasm32-unknown-unknown std (Homebrew's
# cargo has none). CI: plain cargo after `rustup target add wasm32-unknown-unknown`.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WEB="$(cd "$HERE/../.." && pwd)"
SRC="$WEB/vendor/fm1-emu"
OUT="${1:-$WEB/build/emu}"
[ -f "$SRC/Cargo.toml" ] || { echo "no $SRC: git submodule update --init web/vendor/fm1-emu" >&2; exit 1; }
# the commit the superproject pins (the submodule's checkout may be on a work branch)
PIN="$(git -C "$WEB/.." ls-files -s web/vendor/fm1-emu 2>/dev/null | awk '{print $2}')"
PIN="${PIN:-$(git -C "$SRC" rev-parse HEAD)}"
COPY="$WEB/build/emu-src"
rm -rf "$COPY.old" && { [ ! -d "$COPY" ] || mv "$COPY" "$COPY.old"; }
mkdir -p "$COPY"
git -C "$SRC" archive "$PIN" | tar -x -C "$COPY"
rm -rf "$COPY.old"
if [ "${EMU_PATCHES:-1}" != 0 ]; then
  # a repository of its own: inside ours (web/build), `git apply` would skip every path
  git -C "$COPY" init -q
  for p in "$HERE"/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "patch: $(basename "$p")"
    git -C "$COPY" apply --whitespace=nowarn "$p"
  done
fi
export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
export RUSTFLAGS="${RUSTFLAGS:-} -Cllvm-args=-inline-threshold=2000 -Clink-arg=--export-table -Clink-arg=--growable-table"
CARGO="${CARGO:-cargo}"
# rustup's stable toolchain when there is one (macOS: Homebrew's rustc is first on PATH and has no
# wasm32 std, so cargo is told which rustc to run; its rust-lld finds libLLVM in the toolchain's lib)
TC="${RUSTUP_TC:-$HOME/.rustup/toolchains/stable-aarch64-apple-darwin}"
if [ -d "$TC" ]; then
  CARGO="$TC/bin/cargo"
  export RUSTC="$TC/bin/rustc" DYLD_FALLBACK_LIBRARY_PATH="$TC/lib"
fi
# the copy and the target dir are ours (web/build is gitignored)
(cd "$COPY" && $CARGO build --release -p fm1-web --target wasm32-unknown-unknown --target-dir "$WEB/build/emu-target")
mkdir -p "$OUT"
cp "$WEB/build/emu-target/wasm32-unknown-unknown/release/fm1_web.wasm" "$OUT/fm1_web.wasm"
ls -la "$OUT/fm1_web.wasm"
