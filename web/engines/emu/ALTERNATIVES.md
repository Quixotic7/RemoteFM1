# Other FM-1 emulators

## w0ts/fm1-emulator (issue #1), checked 2026-10-27

[w0ts/fm1-emulator](https://github.com/w0ts/fm1-emulator) at `14aecb9` (branch `feat/upstream-merge`): hdavid's fork of
Simon Johansson's [fm1-emulator](https://github.com/simonjohansson/fm1-emulator). Rust, GPL-3.0-only, so it could ship on
this site. An interpreter: its JIT is off by default and, by its own notes, slower. There is no WebAssembly build yet,
though the library has no dependencies and its JIT is cfg-gated, so one looks possible.

Its "2.7x to 5.7x faster" is measured against Simon Johansson's upstream, not against fm1-emu.

**Speed.** x real time (emulated seconds per wall second), 360 MHz, 4 s after 3 s of boot, idle / chord held. Apple M5 Max,
load about 4. w0ts: its `examples/speed`. fm1-emu: `fm1-live --bench` (native, with this site's patches) and
`web/engines/emu/test.mjs --mhz=360` (the site's WebAssembly build in Node).

| Firmware | w0ts, native | fm1-emu, native, no JIT | fm1-emu, native, JIT | fm1-emu, WebAssembly (the site) |
| --- | ---: | ---: | ---: | ---: |
| Felucca 1.6.1 | 0.26 / 0.26 | 0.93 / 0.92 | 3.39 / 3.46 | 2.18 |
| Orbit 0.8.0 | 0.25 / 0.26 | 0.91 / 0.91 | 3.14 / 3.11 | 2.17 |
| M-VAVE V15 | 0.32 / 0.15 | 5.48 / 2.43 | 7.09 / 4.01 | 3.46 |
| X0X 1.1.0 | faults at boot | 0.82 / 0.82 | 2.05 / 1.94 | 1.90 |
| SLOOPdx 4.0 | faults at boot | 1.02 / 1.03 | 3.95 / 3.86 | 3.25 |

w0ts runs these firmwares at about a quarter of real time natively; in a browser it would be slower still. fm1-emu's
interpreter alone is about 3.5x faster, and the site's WebAssembly build about 8x faster than w0ts native.

**Compatibility.** X0X 1.1.0 and SLOOPdx 4.0 stop on w0ts at the same place, an unsupported instruction `0xffeb` at
`0x020004a0`, about 0.1 s into boot. The rest of the catalog was not run on w0ts.

**Determinism.** Both repeat exactly: w0ts gives the same state hash after 100 M instructions twice (Felucca); fm1-emu
writes byte-identical audio twice (Felucca, 3 s).

**Verdict.** fm1-emu stays the emu engine. w0ts models more of the board (Bluetooth, USB MIDI and audio hosts, an SD
card, NOR erase suspend, steady LED levels); those parts may be worth comparing against fm1-emu when a firmware needs
them.
