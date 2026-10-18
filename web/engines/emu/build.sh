#!/usr/bin/env bash
# Build fm1-emu's web crate (web/vendor/fm1-emu, crates/fm1-web) into web/build/emu/fm1_web.wasm,
# with the flags fm1-emu's own web/build.sh uses (the exported, growable function table is what
# its WebAssembly JIT installs compiled blocks in). No bindings generator: plain extern "C" exports.
#   web/engines/emu/build.sh [out-dir]
# Local (macOS): rustup's stable toolchain, which has the wasm32-unknown-unknown std (Homebrew's
# cargo has none). CI: plain cargo after `rustup target add wasm32-unknown-unknown`.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WEB="$(cd "$HERE/../.." && pwd)"
SRC="$WEB/vendor/fm1-emu"
OUT="${1:-$WEB/build/emu}"
[ -f "$SRC/Cargo.toml" ] || { echo "no $SRC: git submodule update --init web/vendor/fm1-emu" >&2; exit 1; }
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
# the target dir is ours (web/build is gitignored), so the submodule's tree stays clean
(cd "$SRC" && $CARGO build --release -p fm1-web --target wasm32-unknown-unknown --target-dir "$WEB/build/emu-target")
mkdir -p "$OUT"
cp "$WEB/build/emu-target/wasm32-unknown-unknown/release/fm1_web.wasm" "$OUT/fm1_web.wasm"
ls -la "$OUT/fm1_web.wasm"
