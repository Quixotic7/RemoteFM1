<!-- SPDX-License-Identifier: MPL-2.0 -->
# saadet-fm6: provenance

The standalone instrument's DX7 engine is **FM6** from **saadet** by Kerem Kilic (Ellic Studio), a single-synth
fork of Melodee, which is a modified Felucca (Leo Kuroshita, Hügelton Instruments). FM6 is MSFA's DX7 synthesis
(Google's music-synthesizer-for-android, carried on in Dexed's `Source/msfa`) in C, with native fp32 operators.

- Repository: <https://github.com/keremimo/saadet>
- Commit: `2eff7f157ef73d8dd3a4f0875b9f9b66cb1c9820` ("fix(midi): backport clock drift fix for 0.1.1")
- Taken: 2026-10-30, for RemoteFM1 standalone milestone S0 (`STANDALONE-PLAN.md`).

## The files

| File here | saadet's file | SHA-256 at 2eff7f1 | Licence (its SPDX line) | Changed |
|---|---|---|---|---|
| `fm6_core.c` | `firmware/src/fm6_core.c` | `637141836680a451146560dc219646cfafa109d41000330b594670fdde405c61` | MPL-2.0 AND Apache-2.0 | yes, one product moved (below) |
| `fm6_msfa_ops.h` | `firmware/src/fm6_msfa_ops.h` | `a784bb99c22d884197142342cf6710d53cd8f351d4c7df89931dc348e594c217` | MPL-2.0 AND Apache-2.0 | no |
| `eng_fm6.c` | `firmware/src/eng_fm6.c` | `a8c576827146a614dc25fd67d615c6859f5495a0c0ef1b736c3d640b00ca2363` | MPL-2.0 AND Apache-2.0 | yes, below |
| `fm6_voice_policy.h` | `firmware/src/fm6_voice_policy.h` | `d1b025cefae8d28700def1c48290c9da5ce3378f2144661942e2e3162f8bc09e` | MPL-2.0 | no |
| `fm6_store.c` | `firmware/src/fm6_store.c` | `3a97898b5bc621607637dd1e57fd4495b0cb2893e26b600c853d3e417d8781e7` | MPL-2.0 | no (not compiled yet: DX7 SysEx in and out, for the user banks of S5) |
| `x0x/fastmath.h` | `firmware/src/x0x/fastmath.h` | `8c1f7d705bdcc16d7da14aaa16cc7fbba7075c2e6783428631d24731fdd7c4fd` | MPL-2.0 (Charles Vestal's fm1-base maths, Kerem Kilic's unsigned exponent shift) | no |
| `LICENSES/Apache-2.0-msfa.txt` | `LICENSES/Apache-2.0-msfa.txt` | `27dd23b62bc11d0806562397a1e782f86fc7dcacd1ba7a1b58d3c78a54795e62` | the Apache-2.0 text for MSFA's portions | no |

Commit `5b047d7` adds the files exactly as they are at 2eff7f1 (`shasum -a 256` of each equals the table's). Every file keeps its own header. The MPL-2.0 text is
RemoteFM1's `LICENSES/MPL-2.0.txt`. The note at the top of `Apache-2.0-msfa.txt` is saadet's (from Felucca's time:
it speaks of an integer port and GPL-3.0 for the Felucca firmware as a whole); it does not describe this tree, where
the firmware is MPL-2.0 file by file, as in saadet's fm1-base build today. The Apache-2.0 licence below it is the one
that applies to the MSFA portions.

## What changed, and why

`eng_fm6.c` and `fm6_core.c`, as little as possible (`git diff 5b047d7 -- .` shows all of it).

In `eng_fm6.c`:

1. `#include "melodee_fm6.h"` removed. That header is the output of saadet's `tools/gen_fm6_patches.py`
   (Felucca's factory voices, GPL-3.0-only): nothing of it may enter this firmware. It gave `FM6_INIT`,
   `FM6_FACTORY` and `FM6_NFACTORY`; `FM6_INIT` (the DX7's INIT VOICE, plain parameter data written from the DX7's
   documented init voice) is now `firmware/sa/sa_synth.c`'s.
2. `#include "eng_fm6_rom.h"` removed: saadet's own factory voices (F9..F24). RemoteFM1's built-in voices will be
   Yamaha's ROM cartridges (STANDALONE-PLAN.md).
3. `#ifndef FM6_NO_FACTORY` around the three parts that used them or saadet's engine table: `fm6_factory`,
   `fm6_track_loaded`, and the engine's table at the end (`N_FM6_ALG`, `FM6_PRESETS`, `ENG_FM6`: saadet's UI and
   preset model). `sa_synth.c` defines `FM6_NO_FACTORY`; without it the file is saadet's again (with the two headers).
4. A two-line notice after the licence header that the file was changed (Apache-2.0 section 4(b)).

In `fm6_core.c` (S0b, 2026-10-30):

5. `fm6_logfreq`'s detune, `(int32_t)((FM6_DETUNE[note] * (op[FP_DET] - 7)) >> 24)`, is now
   `fm6_detune_q24(FM6_DETUNE[note], op[FP_DET] - 7)`, a `noinline` function of its own (three lines above
   `fm6_logfreq`) computing the same expression. Why: inlined into `fm6_keyed`, that 64-bit product sat beside byte
   loads, which the FM-1's CPU now and then gets wrong (felucca-bsp `docs/CPU.md`; fm1-base's build listed
   `fm6_keyed`). The samples are the same (`tests/fm6_parity.sh`), and the build now reports no 64-bit multiply beside
   a load anywhere.
6. A two-line notice after its licence header, as in 4.

Nothing else changed: the synthesis, the envelopes, the controllers, the LFO, the voice policy and the patch
formats are saadet's. Where saadet's instrument is needed, RemoteFM1 supplies it around the files instead:

- `firmware/sa/fm6_shim.h`: what `eng_fm6.c` reads of saadet's `core.h` (`track_t`, `voice_t`, `vmod_t`, the song's
  TUNE, `tuning_log`, the audio batch, the profiler's marks), reduced to one part, `SA_POLY` voices, the macros
  at 0. `MELODEE_DUAL_CORE` is 1, so FM6 renders with the caller's scratch (`fm6_run_into`), and saadet's audio
  batch (its `audio_batch.h` contract) is RemoteFM1's own queue: with `sa_synth_cores(2)`, `fm6_render` queues each
  voice as a job (`fm6_batch_kernel`: its controls and operators into a buffer of its own) and `sa_synth.c` runs
  about half of them on the second core, the rest on the first, then adds the buffers in queue order (integer sums:
  the same samples). saadet's own pairing (`audio_worker.h`) is off (`audio_worker_online` 0). `clamp`, `amp_at`, `audio_dc_decay`, `VOICE_FS` and `AUDIO_ONE` restate saadet's
  `dsp.c` and `audio_pcm.h` (MPL-2.0) exactly, credited in its header; `audio_dc_decay` is out of line here
  (`noinline`), so that its 64-bit multiplies do not sit beside the DC filter's loads in `fm6_dc_samples`
  (felucca-bsp `docs/CPU.md`: the FM-1's CPU now and then corrupts a register there), and in `.dsp_text` with its
  caller, in RAM. The result is the same. `sa_synth.c` also gives FM6 an inline `memset` (FM6's `fm6_plan`, in RAM
  code, would call the C library's in the flash).
- `firmware/sa/performance_options.h`: the one build option FM6 reads (`MELODEE_FM6_FAST`, 1 as in saadet).
- `firmware/sa/sa_synth.c`: the instrument (saadet's `voice.c` for an engine with its own envelopes): POLY
  allocation, gates, the start and end ramps, and per block `fm6_block`, `fm6_render`, `fm6_done`, `fm6_post`.
- The tables, which saadet generates with `tools/gen_tables.py` (GPL-3.0-only, not read, not used in the build):
  `firmware/tools/gen_fm6_tables.py` (below).
- saadet places its DSP code in sections `.dsp_text` and `.audio_text` (its own linker script copies them into
  executable RAM in the pool). fm1-base's `device/app.ld` did not know them: `firmware/patches/fm1-base/
  0005-app-dsp-in-ram.patch` puts them in `.ram_text` (RAM code, copied at boot), and extends fm1-base's RAM-code check
  so that calls are allowed only when they stay in RAM (FM6 calls between its RAM functions through function
  pointers, saadet's way of calling far code). saadet's calls from the flash into them are those pointers too. On a
  Mac's host build `sa_synth.c` drops the section names (Mach-O wants "segment,section").
- The second core follows the owner's FIZZ (`FMToneSeq/firmware/DUALCORE.md`, P2.14, and its `fm1.toml`): fm1-base's
  `cpu2 = true` with `cpu2_fallback = true`, started in `app_init` before the sound, one hand-off a 32-frame control
  block (`plat_cpu2_run`, then `plat_cpu2_wait`), core 1 the larger half of the sounding voices (FIZZ's P2.14b rule:
  `(n + 1) / 2`), its job from the flash calling FM6's RAM code, a job it does not take or drops done by core 0 in
  the same block. No FIZZ code is copied (only its settings); `fm6_render`'s batch path, `fm6_batch_kernel` and
  `fm6_ghost_batch_kernel` are saadet's, written for this. RemoteFM1 differs from FIZZ in: FIZZ's RAM code sits in
  the `.c1_text` window (exempt from the check), FM6's in `.ram_text` under the extended check; FIZZ splits the
  pool's top voices, RemoteFM1 the first jobs in FM6's queue (the samples are the same either way); FIZZ's Bluetooth
  package is its dual-core one too, as RemoteFM1's is.

## The tables

`firmware/tools/gen_fm6_tables.py` (MPL-2.0, written for RemoteFM1) makes `build/gen/fm6_tables.h` at every build
(fm1.toml `gen`) from MSFA's own formulas, which MSFA computes at start-up. The Apache-2.0 files it follows, each
checked to carry the Apache-2.0 header:

| Table | MSFA file | Where |
|---|---|---|
| `FM6_FREQ` (1025) | `freqlut.cc` `Freqlut::init` | Google, `app/src/main/jni/`, `f67d41d313b7dc85f6fb99e79e515cc9d208cfff` |
| `FM6_SIN` (1025, filled at boot: `fm6_tables_init`) | `sin.cc` `Sin::init` | Google, same |
| `FM6_COARSE` (32), `FM6_FINE` (128) | `dx7note.cc` `coarsemul`, `osc_freq` | Google, same |
| `FM6_LFO_UNIT` | `lfo.cc` `Lfo::init` | Google, same |
| `FM6_PEG_UNIT` | `pitchenv.cc` `PitchEnv::init` | Google, same |
| `FM6_DETUNE` (128) | `dx7note.cc` `osc_freq` (the note-dependent detune) | Dexed `Source/msfa/`, `fea2f0f0f2c397799f59b27eb11c038cd34b22c5` (Copyright 2016-2025 Pascal Gauthier, 2012 Google) |
| `FM6_LFO_INC` (100) | `lfo.cc` `lfoSource` and `Lfo::reset` (the DX7's measured LFO rates) | Dexed, same (Copyright 2013 Google) |
| `FM6_PORTA`, `FM6_GLISS` (128 each) | `porta.cpp` `Porta::init_sr` | Dexed, same (Copyright 2019 Jean Pierre Cimalando) |

Dexed's `Source/msfa/` files are Apache-2.0 each (Dexed as a whole is GPL-3.0; only those files were read, and only
those whose header says Apache-2.0). saadet's `tools/gen_tables.py` was not read. Its output was compared: saadet's
own generator, run as a program, makes its `melodee_tables.h`; `tests/fm6_parity.sh` prints every FM6 value of both
headers (2698: the arrays above, the units, and the sine after each one's start-up fill, as bits) and they are
identical. The constants of `fm6_core.c` itself (output level, envelope statics, velocity, key scaling, pitch EG
rates and steps, AMS, PMS) are saadet's file as it is.

## Is it the same FM6?

`tests/fm6_parity.sh SAADET_DIR` builds saadet's own instrument from a saadet checkout (its `voice.c`, `eng_fm6.c`,
its generated tables; `tests/fm6_ref.c` drives it) and RemoteFM1's (`sa_synth.c` with `SA_POLY` 16, saadet's voice
count), plays the same ten performances on both (`tests/sa_scenarios.h`: six voices, every algorithm family,
feedback including algorithm 4's loop, the six LFO waves, pitch EG, scaling, velocity, bend, the controllers,
portamento and glissando, voice stealing, edits and loads while notes sound) and compares them sample for sample:
identical, every sample of every render (2026-10-30, saadet 2eff7f1, Apple clang on arm64, `-O2 -ffp-contract=off`).
RemoteFM1's render is compared six ways, all identical: one core, the batch on one core, and the batch with a
simulated second core that runs its share at once, after the first core's, never, or takes it and gives it up.

## Credit

saadet and its FM6: Kerem Kilic (Ellic Studio). MSFA: Google Inc. (Raph Levien), Pascal Gauthier (Dexed), Jean Pierre
Cimalando (portamento). Felucca: Leo Kuroshita (Hügelton Instruments). fm1-base's maths: Charles Vestal.
