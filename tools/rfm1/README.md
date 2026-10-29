# FIZZ as the site's own .rfm1

`build_fizz.sh` builds [FIZZ](https://github.com/Quixotic7/FIZZ) (MPL-2.0) into `web/site/fw/fizz-<version>.rfm1`,
the file the site's player runs for `?fw=fizz` on its rfm1 engine.

- **What it uses.** FM1VST's `fm1_add_core` and `fm1_add_rfm1` (`cmake/FM1Cores.cmake`, `cmake/FM1Rfm1.cmake`) at the
  commit pinned in the script, and FIZZ at its release's commit. Neither is vendored here: give `FIZZ_SRC` and
  `FM1VST_DIR` (checkouts at those commits), or the script clones them into `build/rfm1-fizz/`. Emscripten is
  `emcc` on PATH, `~/emsdk`, or `FM1_EMCC`.
- **What it builds.** `fizz/CMakeLists.txt` is RemoteFM1IOS's `cores/fizz/core.cmake`: the same units, plutovg and
  defines, and `fizz/fizz_desc.c` points `source_url` at FIZZ's public repository at the commit.
  - The copy compiled leaves out FIZZ's `site/` and `core/tests/`, which are GPL.
  - `fizz/plutovg-ft-raster-wasm.patch` replaces the rasterizer's setjmp / longjmp with a flag under `__wasm__`. An
    .rfm1 has no exception handling, and Emscripten's other setjmp needs JavaScript. The rows drawn are the same:
    checked against the native build on shapes that overflow the pool.
  - `fizz/manifest.py` writes the file's licence expression and `glue_url` into the manifest.
  - Then the script checks the file (`rfm1_check.py`) and plays it in wasm3 (`rfm1_run`).
- **The licence.** `MPL-2.0 AND GPL-3.0-only AND MIT AND FTL AND OFL-1.1`:
  - FIZZ;
  - FM1VST's core glue and core-api (GPL-3.0-only);
  - plutovg and Emscripten's runtime with musl (MIT);
  - FreeType's rasterizer in plutovg (FTL);
  - Inter Tight (OFL-1.1).

  Their texts are the file's `rfm1.license.<n>` sections. The credits name the patch.
- **FIZZ's data.** The device keeps FIZZ's fonts and tables in a second blob, pushed after the install. In the host
  core they are ordinary constant data (`QD_XDATA` is empty off the device). The factory sounds and the demo patterns
  are compiled in, so a fresh flash boots to FIZZ's factory content and nothing is staged. Saves are FIZZ's own store
  in the 1 MiB flash image.
- **Memory.** 16 MiB fixed. After every screen and the demo, the highest byte written was at 6.7 MiB.

## A new FIZZ release

1. In `build_fizz.sh`, set `FIZZ_VERSION` and `FIZZ_COMMIT` (the tag's commit: `git rev-parse v<version>^{commit}`).
   Move `FM1VST_COMMIT` only to an FM1VST that is pushed. If FIZZ changed its sources, compare
   `fizz/CMakeLists.txt` with RemoteFM1IOS's `cores/fizz/core.cmake`. The plutovg check fails when plutovg's file
   list changes.
2. Run `tools/rfm1/build_fizz.sh`. If the patch no longer applies, see whether FIZZ now builds without setjmp in Wasm.
   If it does, drop the patch.
3. `node web/engines/rfm1/test.mjs --dir <a folder with only the new file>`. It reads `fizz/conformance.txt`; set
   `FM1VST_DIR` to an FM1VST checkout for its scripts.
4. Delete the old `web/site/fw/fizz-*.rfm1`. Write the new file's name and SHA-256 into `web/site/fw/SHA256SUMS`
   (`shasum -a 256 fizz-<version>.rfm1 > SHA256SUMS` in `web/site/fw/`) and into `web/site/data/site-rfm1.json`
   (`file`, `sha256`, and the release and source links).
5. `node web/site/tools/build_catalog.mjs`. Then run `node web/site/test.mjs` and `node web/site/tools/fizz-e2e.mjs`.
