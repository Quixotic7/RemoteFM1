# fm1-emu patches

`web/engines/emu/build.sh` builds fm1-emu from a clean copy of the commit the submodule pins
(`web/vendor/fm1-emu`, upstream joakimeriksson/fm1-emu, now **v0.4.0**, 9de1ef0) with these patches
applied in order. The submodule stays at upstream's commit. When upstream merges a fix, bump the pin
and delete its patch. FM1VST's `core-glue/fm1emu/patches` carries the same files.

| Patch | Fixes | Upstream |
| --- | --- | --- |
| `0002-fm1-core-the-wasm-JIT-computes-the-FPU-s-multiply-ac.patch` | Melodee's FPU multiply-accumulates leave compiled code (the rest of the old 0002 is in v0.4.0) | PR not opened yet (text below) |
| `0004-fm1-web-run-for-a-span-of-emulated-time-fm1_run_fram.patch` | X0X 1.1.0 sounds garbled in Chrome (bursty rendering) | PR not opened yet (text below) |

## Dropped at the bump to v0.4.0 (2026-11-02)

Each of these is in v0.4.0 in upstream's own form; the patch files are in git history (the pin
5fbb066 and the four patches, 2026-10-18).

| Old patch | What it did | Where v0.4.0 has it |
| --- | --- | --- |
| 0001 the P33 power-domain bridge | P33 (`0x13E00`) read back as 0, so Melodee 1.4.4 halted in `fm1_core1_failure()` before drawing | `crates/fm1-soc/src/p33.rs`, ab0f422 "fm1-soc: model the P33 power registers" (the same three-byte frames, a P33 and an RTC bank), and 62b262d: the watchdog and `PR_PWR` now reset the chip, which the old patch left open. Melodee 1.4.4 boots, draws and plays on plain v0.4.0 (test.mjs, FM1VST fm1emu_test 8). |
| 0002 the wasm JIT compiles the FPU, float if-blocks, `call R` / `goto R` | FPU ops and float `iff` blocks were helper calls; `call R` / `goto R` went through `step` | 7acdb87 (the FPU's ops inline, `fop_selftest`), 3256f88 (float if-blocks, either kind), aad1cfb (`Fast::CallR` / `GotoR` micro-ops). Not taken: the multiply-accumulates 7 / 8, which v0.4.0 leaves with the executor: the new 0002 is that part alone. |
| 0003 the TIMER4 count without a u128 division | a u128 division (a compiler-rt call in wasm) on every TIMER4 CNT read | 599f0ea `mul_div`: in u64 while the product fits, u128 only after 2^64 / 24 M cycles (35 minutes at 360 MHz, 53 at 240), the same value |

The site's own adaptation to v0.4.0 (in engine.js, not a patch): a compiled region now imports
its three helpers (`h_read`, `h_write`, `h_op`) as functions, passed to `fm1_jit_compile` as table
indices, instead of importing the whole table; without that every compile failed and the emulator
ran interpreted (5x slower). The table grows 256 slots at a time.

### Measured (2026-11-02, this Mac)

Headless Chromium, a real AudioWorklet (`bench-browser.mjs`, 10 s each): the mean load, the render
time per quantum's time (below 1 keeps up). Before: 5fbb066 with the four old patches. After: v0.4.0
with 0002 + 0004. Runs vary by about ±5-10 % from one to the next.

| package | MHz | before | after |
| --- | --- | --- | --- |
| Melodee 1.4.4 | 360 | 0.68 | 0.65-0.69 |
| Melodee 1.6.0 | 360 | 0.67-0.71 | 0.67-0.82 |
| X0X 1.1.0 | 360 | 0.56 | 0.54 |
| X0X 1.2.0 | 360 | 0.60 | 0.57 |
| Felucca 1.6.1 | 360 | 0.56 | 0.41 |
| ChoralRoot 0.25.1 | 360 | 0.42 | 0.37 |
| LOOM 0.12.1 | 360 | 0.31 | 0.17 |
| Ravel 0.90.0 | 360 | (did not run) | 0.59 |
| DUELL 1.1 | 240 / 360 | (halted at 2.2 s) | 1.09 / 1.51: runs, but too slow for real time here |

v0.4.0 without the new 0002 measured Melodee 1.4.4 0.68-0.72 and 1.6.0 0.72-0.73 in the same
session. No underruns anywhere; the only glitches are DUELL's (it does not keep up).

Firmwares v0.4.0 runs that 5fbb066 did not, from the Hub: DUELL 1.1 (halted 2.2 s in before;
now plays, under real time), Ravel 0.90.0 (no output in the browser before), Bubba Machine
0.10.0-beta (boots and draws now, but no sound under PLAY or the keys, natively too: it stays in
`EMU_SKIP` with that note). COSMOS and the SMK-37 mods are not FM-1 packages on the Hub.

## 0002: the wasm JIT computes the FPU's multiply-accumulates inline

### Pull request text (ready to paste)

Title: `fm1-core: the wasm JIT computes the FPU's multiply-accumulates inline`

Body:

> 0.4.0's wasm JIT computes the FPU's ops inline, but the multiply-accumulates (ops 7 and 8,
> `R += R * R (f)` / `R -= R * R (f)`) still go to the executor through `h_op`. Melodee, whose voices
> are float DSP, runs them millions of times a second on both cores.
>
> They now compile to what `fpu_d` computes: the product rounded first, then the add or subtract
> (not fused). The operands go in the order the executor's build passes a NaN on: b * a for the
> product (as op 2), then d + product / d - product. `fop_inline` lists 7 and 8, so
> `fop_selftest` checks them against the executor and the interpreter on every register choice;
> its instruction word and its operand cases now tell a binary op by its low nibble (not 0xf), as
> `decode_fpu` does, so 7 and 8 are encoded and tested as the binary ops they are.
> `fm1_jit_selftest()` returns 0.
>
> Melodee in headless Chromium's AudioWorklet at 360 MHz (the render time per quantum's time):
> 1.4.4 0.684 -> 0.652, 1.6.0 0.721 -> 0.673.

## 0004: run for a span of emulated time

Rebased onto v0.4.0 unchanged (its fm1-web now also has `fm1_ahead` / `Pace`, next to which the two
exports sit). Still needed: `engine.js` paces the machine by time with a 40 ms lead (`fm1_run_frames`).

### Pull request text (ready to paste)

Title: `fm1-web: run for a span of emulated time (fm1_run_frames, fm1_time_frames)`

Body:

> `fm1_render` runs the machine until the I2S DMA has produced the frames asked for. With
> 512-frame DMA halves (X0X 1.1.0) a host's 128-frame quanta then cost nothing three times and a
> whole half's emulation, 10 ms of a 2.7 ms quantum, the fourth time. Chrome's 256-frame audio
> callbacks miss their deadline every 11.6 ms: 156 glitches in 10 s at 360 MHz, heard as garbled,
> distorted audio, though the machine keeps up on average (load 0.68).
>
> `fm1_run_frames(frames, max_insns)` runs a span of emulated time and queues the audio;
> `fm1_render(n, 0)` hands it out without running; `fm1_time_frames()` reports the emulated time.
> A host can then pace the machine by time: a quantum's span per quantum, with a few tens of ms
> buffered ahead, so the work per quantum is even. RemoteFM1's engine buffers 40 ms: X0X has no
> glitches left at 240-360 MHz, and Melodee's LEDs stop flickering (their on-fractions are taken
> over even stretches of emulated time).
>
> v0.4.0's `fm1_ahead` (`Pace`) evens the work out inside `fm1_render`, running at most one DMA
> half ahead. This is the other way round: the host picks the span and the lead, so a lead longer
> than a half (RemoteFM1's 40 ms) also absorbs a slow quantum (a JIT compile, a GC).
> `fm1_render(n, 0)` still runs nothing with `fm1_ahead` at 0, its default.
