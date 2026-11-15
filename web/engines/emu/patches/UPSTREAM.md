# fm1-emu patches

`web/engines/emu/build.sh` builds fm1-emu from a clean copy of the commit the submodule pins
(`web/vendor/fm1-emu`, upstream joakimeriksson/fm1-emu, now **master 2a55137**, v0.5.1 + 14, 2026-11-15) with these patches
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

## Dropped at the bump to 2a55137 (2026-11-15)

| Old patch | What it did | Where upstream has it |
| --- | --- | --- |
| 0005 the audio PLL reads as the stock boot leaves it | `stock_boot` set 0x119A0 / 0x119A4 to M-VAVE's PLL0 values, so felucca-bsp's checked audio start (RemoteFM1 0.3.4..0.3.6, AMB-1 1.3.2, CTL-1 1.0.3, ...) plays | d2bcc33 "PLL0 as the bootloader leaves it" (#153): the same two words, the same values, set in `Soc::new` (so every boot path, and kept for a reset). The old patch is in git history. |

0002 rebased onto 2a55137: only its context moved (`compile` takes a fourth argument in `fop_selftest`);
the change is the same; `fm1_jit_selftest()` is 0 (ops 7 / 8 included). 0004 applies unchanged. Neither has an
upstream PR yet.

What else the bump brings, for the site (fm1-web's exports used by `engine.js` are unchanged; new ones it
does not call yet: `fm1_flash_part` (the UC25HQ80 flash of the newer units), `panel_theme*`):
the JIT's region / block-move / core-1 probe work (#136, #154-#156), the two-core lock handover (#115),
the `6e e8` register shifts (#149), USB controller interrupts (#169), a package's resource region laid
into the flash as its loader writes it (#175: Melodee 1.6.1 boots and draws, Polly 0.6.4's band plays),
DAC-exact audio frame counting and LED fractions (d8a881a).

### Measured (2026-11-15, this Mac)

Node (`test.mjs`'s run, 320 MHz, one run each; the RTF is emulated / wall time with a three-note chord
held, about ±10 % run to run). Old: 9de1ef0 + 0002 0004 0005. New: 2a55137 + 0002 0004. Sound and
boot time are the same on both unless noted.

| package | old | new | note |
| --- | --- | --- | --- |
| FRASIL 3.3.1 | 2.73 | 3.26 | plays under PLAY |
| AMB-1 1.3.2 | 2.81 | 2.90 | plays (sounds before the key, so test.mjs's "louder after the key" check says FAIL on both) |
| CTL-1 1.0.3 | 12.9 | 14.3 | |
| Felucca 1.8.2 | 3.24 | 3.97 | |
| DUELL 1.2 | 1.14-1.34 | 1.57-1.74 | |
| Purple Monkey 0.12 | 2.14-2.32 | 2.01-2.41 | sounds before the key (as AMB-1) |
| Golana 1.3 | 3.20 | 3.49 | |
| Melodee 1.6.1 | 1.33-1.41 | 1.95-2.03 | old: sound, but the screen stayed on its first frame (lit 4584 px, no LCD frames in the browser); new: draws (#175) |
| ChoralRoot 0.41 | 1.81 | 2.39 | |
| X0X 1.2.2 | 2.16-2.44 | 1.92-2.07 | about 13 % slower, see below |
| Polly 0.6.4 | 13.1 | 13.8 | a different (quieter) level: its resource region is now in the flash (#175) |
| CHOPPAH 1.4.1 | 10.3 | 10.6 | |
| LOOM 0.16.2 | 6.47 | 8.16 | |
| Ravel 1.41.2 | 1.80-2.04 | 1.85-1.93 | |
| VESPER 2.0.2 | 2.89-3.21 | 3.02-3.14 | |
| Caravel 0.13.0 | 2.26 | 2.84 | |
| B3 Organ 0.4 | 2.80 | 3.03 | |
| PATCHWERK 1.0.3 | 4.17 | 4.73 | |
| SLOOP 2.8.5 | 2.77 | 3.10 | |
| SLOOP DX 4.0.1 | 2.39 | 3.42 | |
| Felucca-8 1.8.4 | 3.23 | 3.23 | |
| M-VAVE V15 | 2.63 | 2.60 | |
| Arctic 2.1, DX747 0.1 | | | boot and draw, silent under a key and PLAY on both |
| Bubba Machine 0.10.0-beta | | | silent on both (EMU_SKIP) |
| VIBE FM Link 0.4 | | | silent on both; upstream holds it back (it crashes) |

Headless Chromium, a real AudioWorklet (`bench-browser.mjs`, 320 MHz, 8 s): the mean load, old -> new.
X0X 1.2.2 0.51 -> 0.60, Melodee 1.6.1 0.66 -> 0.59, DUELL 1.2 0.81 -> 0.65 (peak 1.03 -> 0.82),
FRASIL 3.3.1 0.44 -> 0.42, ChoralRoot 0.41 0.43 -> 0.38, Felucca 1.8.2 0.37 -> 0.37, Purple Monkey 0.12
0.51 -> 0.51. No glitches or underruns on either.

X0X's slowdown: with 0004 alone on both, 9de1ef0 ~2.35 and 2a55137 ~2.05; across upstream's merges it
drops around #115 / #136 (the two-core lock handover, the wasm dispatch), with noise. Not chased further.

## 0005, dropped (its notes, for the history)

### 0005 (dropped): the audio PLL reads as the stock boot leaves it

Found 2026-11-08: RemoteFM1 0.3.4, 0.3.5 and 0.3.6 drew their screen in the player's emulator and played nothing,
while 0.3.3 and the native cores played. 0.3.4 moved onto fm1-base 317cb86, whose felucca-bsp (f878800 on, #9)
starts audio through `fm1_audio_init_checked`. That refuses an audio PLL it does not recognise: 0x119A0 must read
0x45400203 and 0x119A4 & 0x07FFFFFF must read 0x07503026, as M-VAVE's firmware and m-bootloader leave PLL0
running. fm1-emu's SFRs start at 0, so the start returned `FM1_AUDIO_ERR_CLOCK`. fm1-base's `main_fm1.c` drops
that result, so the app was silent with nothing logged. The patch sets both words in `stock_boot`. The test is
`web/engines/emu/own-test.mjs` (the latest release's packages, a held key must sound), which the Pages workflow
runs after it builds the emulator.

#### Pull request text (not needed: upstream's d2bcc33)

**fm1-soc: the audio PLL reads as the stock boot leaves it**

felucca-bsp's checked audio start (hugelton/felucca-bsp#9, `fm1_audio_init_checked`) refuses to start the I2S
unless the audio PLL's registers read the stock values: 0x119A0 = 0x45400203, and 0x119A4 = 0x3F503026 under the
mask 0x07FFFFFF. M-VAVE's firmware and m-bootloader leave PLL0 running like that. The emulator's SFRs start at 0,
so every app built on felucca-bsp f878800 or later (fm1-base 73f36ed and later) runs and draws but is silent;
fm1-base's `main_fm1.c` ignores the error. `stock_boot` now sets both words as the boot leaves them.

