# Licensing

RemoteFM1 has two licences, by part:

- **The firmware** (`firmware/`: the app, its build settings, its tests, its patches to fm1-base and felucca-bsp) is
  under the **Mozilla Public License 2.0** (`MPL-2.0`, full text in `LICENSES/MPL-2.0.txt`) from 0.2, by its author's
  choice. Each file says so on its first line (an SPDX identifier). MPL-2.0 applies file by file: a firmware built
  from it links JieLi's Bluetooth library (Apache-2.0, a binary), which a GPL firmware could not be given out with,
  and has no GPL file in it at all. `proto/rfm1.h`, the protocol's constants, is compiled into the firmware and is
  MPL-2.0 too.
- **The host library's portable core** (`host/src/` codec, session, panel, screen, tiles_ref and fake, their headers in
  `host/include/rfm/`, and `host/tests/`) and **the site's MIDI learn and OMX-27 modules** (`web/site/js/midi-map.js`,
  `omx27.js`, `omx27-views.js`, `omx-link.js`) are MPL-2.0 from 2026-10-30, by their author's decision, so that other
  hosts (FIZZ for iOS first) can use them in apps that are not GPL. Earlier commits of these files stay GPL-3.0-only.
- **Everything else** (the rest of the site in `web/`, the host library's macOS back ends in `host/src/macos/` and the
  tools in `tools/`, the tests in `tests/` unless a file says otherwise, the documents) is free software under the
  GNU General Public License, version 3 only
  (`GPL-3.0-only`, full text in `LICENSE`). You may use, study, change and share it. If you give someone a program
  built from it, give them its complete source under the same licence too.

RemoteFM1's firmware before 0.2 (0.1, 0.1.1) was built on TransitFM1's platform, from Dinghy and Felucca: those
versions stay under GPL-3.0-only (their source is in this repository's history, up to the move in PLAN.md's M6). 0.2
has none of that code: the platform it ran on was removed, and the app's own files, all written by RemoteFM1's
contributors, moved onto fm1-base.

## Who made what

| | |
| --- | --- |
| The firmware app: `firmware/app.c`, `app.h`, `fm1.toml`, `words.tsv`, `firmware/src/` (`rfm_main.c`, `rfm_dev.c`, `rfm_host.c`, `rfm_app.c`, `rfm_draw.[ch]`, `rfm_push.[ch]`, `remote_proto.h`), `firmware/tests/`, the changes in `firmware/patches/`, `build.sh`, `proto/rfm1.h` | RemoteFM1 contributors, MPL-2.0 |
| [fm1-base](https://github.com/charlesvestal/fm1-base), `base/` (a git submodule, used as it is; `firmware/patches/fm1-base/` changes it at build time) | Charles Vestal, with Leo Kuroshita's Felucca code by his permission ([felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4)), MPL-2.0 |
| [felucca-bsp](https://github.com/hugelton/felucca-bsp), `base/bsp/` (fm1-base's submodule; `firmware/patches/felucca-bsp/` changes it at build time) | Leo Kuroshita (@kurogedelic), Hügelton Instruments; Kerem Kilic (Ellic Studio), the USB host; keremimo, the MIDI clock and transport sync ([felucca-bsp#3](https://github.com/hugelton/felucca-bsp/pull/3)), MPL-2.0 |
| `host/src/{codec,session,panel,screen,tiles_ref,fake}.c`, `host/include/rfm/{codec,session,panel,screen,tiles_ref,fake}.h`, `host/tests/`, `web/site/js/{midi-map,omx27,omx27-views,omx-link}.js` | RemoteFM1 contributors, MPL-2.0 (from 2026-10-30, the author's decision) |
| `docs/PROTOCOL.md`, `host/` (the rest: `host/src/macos/`, `host/include/rfm/macos.h`), `tools/m0_bench.py`, `tools/m0_bulk.c`, `web/m0_bench.html`, `web/remote/`, `tests/` (`proto_test.c`, `rfm_draw_test.c`, `session_test.c`, `usb_midi_test.c`, `run_tests.sh`), `README.md`, `PLAN.md` | RemoteFM1 contributors, GPL-3.0-only |
| `tests/ble_sysex_test.c` (a test of fm1-base's Bluetooth MIDI, for its upstream) | RemoteFM1 contributors, MPL-2.0 |
| `tools/fm1_install.py`, `web/installer/fm1ota.js`, `web/installer/fm1pkg.js`, `tests/install_test.py` | Leo Kuroshita (Felucca's tool), with the changes of Dinghy, TransitFM1, ChoralRoot FM-1 and RemoteFM1 contributors, GPL-3.0-only |
| The site's player and catalog (`web/site/` except the four MPL-2.0 modules above, `web/managers/`, `web/engine/`), the installer page | RemoteFM1 contributors, GPL-3.0-only; the cores and firmwares it runs are their authors' (`web/vendor/`: fm1-emu, FM1VST, each under its own licence) |
| `tools/rfm1/` (`build_fizz.sh`, `fizz/CMakeLists.txt`, `fizz/manifest.py`, `fizz/conformance.txt`, `README.md`): the build of FIZZ's .rfm1 | RemoteFM1 contributors, GPL-3.0-only; `fizz/fizz_desc.c` MPL-2.0 (FIZZ's descriptor, as RemoteFM1IOS has it); `fizz/plutovg-ft-raster-wasm.patch` FTL (a change to FreeType's rasterizer in FIZZ's plutovg) |
| `web/site/fw/fizz-*.rfm1`: [FIZZ](https://github.com/Quixotic7/FIZZ) built to WebAssembly, served by the site | Michael P Jones (Quixotic7) and the parts' authors: `MPL-2.0 AND GPL-3.0-only AND MIT AND FTL AND OFL-1.1` (FIZZ; FM1VST's core glue; plutovg and Emscripten's runtime with musl; FreeType's rasterizer; Inter Tight). Its source: FIZZ at the commit in its manifest, FM1VST at its `glue_url`, and `tools/rfm1/`; its licence texts are in the file |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| JieLi AC79 SDK: the Bluetooth libraries (`btctrler.a`, `btstack.a`, `crypto_toolbox_Osize.a`, `lib_ccm_aes.a`, `wl_rf_common.a`), the few SDK sources fm1-base's build compiles with them, `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin`: read from your SDK checkout at build time, linked or placed in the package. No SDK file is in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |
| fm1-base's `ble/midi/le_midi.c`: portions from the SDK's `le_trans_data.c` (JieLi) | MPL-2.0 AND Apache-2.0 | `base/LICENSES/Apache-2.0.txt` |
| The fonts fm1-base renders at build time (Barlow Semi Condensed, Terminus) | SIL OFL 1.1 | `base/assets/fonts/` |
| The standalone DX7: FM6 from [saadet](https://github.com/keremimo/saadet) by Kerem Kilic (Ellic Studio), at `2eff7f1` (`fm6_core.c`, `fm6_msfa_ops.h`, `eng_fm6.c` (two includes dropped, the factory voices left out), `fm6_core.c` (the detune's product in a helper), `fm6_voice_policy.h`, `fm6_store.c`, `x0x/fastmath.h`); its MSFA portions are Google Inc.'s, Pascal Gauthier's and Jean Pierre Cimalando's (Apache-2.0). Provenance and changes: `firmware/third_party/saadet-fm6/PROVENANCE.md`. RemoteFM1's own around it (`firmware/sa/`, `firmware/tools/gen_fm6_tables.py`, `gen_sa_config.py`, `gen_pixfont.py`, `gen_dx7_rom.py`, `sa_bench.py`, `sa_hash_host.c`, `firmware/tests/standalone*.txt`, `tests/sa_*`, `tests/fm6_*`): RemoteFM1 contributors, MPL-2.0 | MPL-2.0, and MPL-2.0 AND Apache-2.0 where MSFA's portions are | `firmware/third_party/saadet-fm6/LICENSES/Apache-2.0-msfa.txt`, `LICENSES/MPL-2.0.txt` |
| The standalone screens' pixel font: Jersey 10 by Sarah Cadigan-Fried (The Soft Type Project), `firmware/third_party/jersey10/Jersey10-Regular.ttf` as Google Fonts has it; `firmware/tools/gen_pixfont.py` rasterises it at build time into a bitmap font (`firmware/build/gen/sa_font.h`, not committed) that the firmware draws | SIL OFL 1.1 | `firmware/third_party/jersey10/OFL.txt` |
| The standalone instrument's built-in voices: Yamaha's DX7 ROM cartridges ROM1A..ROM4B (256 voices), compiled into the firmware as packed voice data by `firmware/tools/gen_dx7_rom.py` from the builder's own copy of the `.syx` files (`DX7_ROM_DIR`); the rights stay with Yamaha. No `.syx` file and no generated header is in this tree or in a release: only the compiled firmware carries the data, as ChoralRoot and saadet compile in the Prophet-5, CZ-1 and Juno-106 factory sounds. A build without the files has only the DX7's INIT VOICE | Yamaha Corporation's (not RemoteFM1's to license) | (none in this tree) |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. RemoteFM1 is not made or endorsed by M-VAVE, and its screen says what it is. 0.1 answered the update protocol
with the official firmware's identity (`FM-1_015`) so that installers accepted it; 0.2 answers as itself
(`FM-1_52XXYYZZ`).
