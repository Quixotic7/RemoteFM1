# RemoteFM1 standalone: plan (2026-10-30)

RemoteFM1 today does nothing without a host: it is the FM-1 as the panel of a firmware on a computer. This plan
gives it a life of its own: on its own, the FM-1 plays DX7 voices, works as a MIDI controller over USB and
Bluetooth, records loops, and runs an arp. When a RemoteFM1 host connects, it becomes the panel again, as now.

The design is the owner's approved mockup on the Claude Design canvas "RemoteFM1 Standalone" (2026-10-30): the
panel map, 18 screens, and 8 colour themes. The screens, colours, sizes and motion in this plan come from there.

## 1. Decisions (the owner, 2026-10-30)

| Question | Decision |
|---|---|
| DX7 engine | **saadet's FM6** (Kerem Kilic, <https://github.com/keremimo/saadet>, pinned at `2eff7f1`): MSFA's DX7 synthesis in C, `MPL-2.0 AND Apache-2.0`, Dexed's GPL parts already removed, built on fm1-base with Bluetooth. The firmware stays MPL-2.0, so the Bluetooth package stays legal (GPL code cannot ship next to JieLi's closed Bluetooth libraries) |
| Built-in voices | **Yamaha's DX7 ROM cartridges**, compiled in as packed voice data (as ChoralRoot and saadet compile in the Prophet-5, CZ-1 and Juno-106 factory sounds; the rights stay with Yamaha; no raw `.syx` in the repo or a release). Plus **user banks**: the user sends their own DX7 32-voice cartridges over SysEx |
| Look | The mockup: a pixel font (Jersey 10, OFL), solid black backgrounds, big readable values, stepped pixel shapes and motion, 8 themes (ChoralRoot, FIZZ, NES, C64, C64 Impossible Mission, SNES, Game Boy, SynthWave) |

What is not taken from saadet, because it is GPL-3.0-only there: `tools/gen_tables.py` (the FM6 tables; we write our
own generator from MSFA's Apache-2.0 sources, which compute the same tables at start-up), `tools/gen_fm6_patches.py`
and its `melodee_fm6.h` (Felucca's factory voices), and anything under saadet's retired GPL platform.

## 2. Modes

- **Standalone** is the default at boot.
- **Panel mode**: a host's HELLO over USB-MIDI, vendor bulk or Bluetooth (the protocol in `docs/PROTOCOL.md`) hands the
  screen, LEDs and input to the host, exactly as RemoteFM1 0.2 does. The DX7 goes silent (all notes off), and loops
  and the arp stop.
- **Back to standalone** when the host says goodbye or its session times out. The state is kept: voice, channel,
  CC banks, loops and theme.
- Incoming MIDI in standalone plays the DX7 on the chosen channel. MIDI clock and start / stop drive the arp and
  loop playback when an external clock is present.

## 3. The controls (the mockup's panel map)

| Control | Tap / turn | Hold / with GLO |
|---|---|---|
| Keys (27) | play the DX7 voice and send notes on the channel, with velocity (fm1-base's key sensing) | |
| MASTER | volume | |
| SELECT | tempo; in menus: choose | |
| PRESETS | DX7 voice 1..32 | GLO + PRESETS: bank (ROM1A..ROM4B, then the user banks) |
| ALGORITHM | MIDI channel 1..16 | |
| KN1..KN4 | send the 4 CCs of the current bank | GLO + knob: pick which CC that knob sends |
| FX SEL ENV LFO EDIT | CC bank A..E (its screen stays until HOME or another screen; the knobs change it in place) | GLO + FX: MIDI REMOTE on / off (docs/PROTOCOL.md section 8) |
| GLO | shift (held) | |
| HOME | next view: Notes, Scope, Keys | |
| SAVE | the theme menu (ALGORITHM there: the background) | hold: save everything (CC map, loops, theme, settings) |
| ARP | arp on / off | hold: arp settings (mode, rate, octaves, gate) |
| SEQ | spare | |
| PLAY | play / stop the current loop; stops a recording and plays it | hold: the 16 loops; a picked loop queues in at the end of the current one |
| REC | record a loop exactly as played (free length by default) | hold: recording options (length, quantize, count-in, layer) |
| OCT- / OCT+ | octave -3..+3 | both: panic |

Every turn or press shows its screen from the mockup for about a second, then returns to the current HOME view.

## 4. Architecture

```
firmware/
  src/            RemoteFM1 0.2's host link (rfm_*), unchanged except for the mode switch
  sa/             the standalone instrument (new, MPL-2.0)
    sa_main.c       the mode switch, the instrument's loop, the control map
    sa_synth.c      the voice manager and the audio callback around FM6
    sa_bank.c       ROM and user banks, DX7 SysEx in (32-voice bulk, single voice), flash storage
    sa_midi.c       MIDI out (USB + BLE), MIDI in, clock
    sa_cc.c         5 banks x 4 CCs, the assignments
    sa_loop.c       16 loops: event recording as played, overdub, queueing, quantize, count-in
    sa_arp.c        the arp
    sa_ui*.c        the screens, the themes, the pixel font, the views (Notes, Scope, Keys)
    sa_store.c      persistence (settings, CC map, theme, loops)
  third_party/saadet-fm6/   fm6_core.c, fm6_msfa_ops.h, eng_fm6.c (adapted), fm6_voice_policy.h, fm6_store.c,
                            PROVENANCE.md (commit, files, changes), their licence texts
  tools/gen_fm6_tables.py   our own table generator, written from MSFA's Apache-2.0 sources
  tools/gen_dx7_rom.py      packs the Yamaha ROM cartridges from a local .syx path (never committed) into a header
  tools/gen_pixfont.py      rasterises Jersey 10 (OFL) at the sizes the screens use into bitmap fonts
```

- **The FM6 glue**: saadet's `eng_fm6.c` sits on saadet's engine API (`core.h`: engine_t, the voice slots, the
  part). `sa_synth.c` gives it the smallest API it needs: note on / off, the patch, the controllers, and a block
  render into fm1-base's audio ring. The FM6 maths stays as saadet has it, so FM6 renders the same samples as
  saadet's own build.
- **Polyphony**: start at 8 voices (ChoralRoot's measured cost for Dexed-class FM6 is about 25-31 % of the audio
  budget), then measure on the unit and raise it if the budget allows. The loops and the arp share the voices.
- **Screen**: fm1-base's canvas, the theme as a small palette struct (the mockup's tokens: bg, border, panel, ink,
  a1..a4, shadow, on), bitmap pixel fonts, stepped motion at the frame rate. Black backgrounds keep most tiles flat,
  which also helps panel mode's tile codec when a host mirrors the screen.
- **Budget**: RemoteFM1 0.2 is 78 KB of fm1-base's 581 KB app slot. Estimates: FM6 code plus tables about 60 KB,
  Yamaha's 8 ROM cartridges 32 KB (256 voices x 128 B packed), fonts and sprites about 30 KB, the instrument about
  80 KB. Loops are in the pool (16 loops x 2,048 events x 8 B = 256 KB at most; less if the pool is short); where
  user banks and saved loops go: section 7, question 3. To measure in S0.

### Measured on the unit (2026-10-30, Bluetooth bench package, 256-frame blocks of 5,805 us)

| FM6 voices | one core (S0, code in flash) | one core (S0b, hot code in RAM) | two cores (S0b, FIZZ's settings) |
|---|---|---|---|
| 1 | 7.6 % avg / 9.9 % worst | 7.8 / 9.1 | 8.2 / 8.5 |
| 8 | 47.5 / 49.8 | 48.6 / 50.9 | **28.3 / 29.5** |
| 12 | 70.3 / 73.4 | 71.9 / 75.0 | 41.4 / 43.0 |
| 16 | 93.1 / 97.0 | 95.2 / 99.0 | 54.6 / 56.4 |

RAM code alone does not help (the FPU, not flash, is the limit); two cores halve the cost. The unit's render hash
equals the computer's on one and two cores (3073bc34), so the FPU gives the same samples. **Polyphony: 8 voices on
two cores.** The unit went back to RemoteFM1 0.2.3 after each bench.

## 5. Milestones

Each milestone ends with the host simulator's tests, a build of both packages (Bluetooth, USB-only), a flash of the
RemoteFM1 unit, and the owner trying it.

| | What | Done when |
|---|---|---|
| **S0** | Vendor saadet's FM6 (pinned, PROVENANCE.md), write `gen_fm6_tables.py`, the glue, a host test that renders DX7 voices | the host build renders a DX7 voice, sample for sample the same as saadet's own simulator for the same voice and notes; flash, RAM and CPU measured on the unit |
| **S1** | Standalone shell: the mode switch, keys play FM6, PRESETS / GLO + PRESETS (ROM banks), ALGORITHM channel, OCT, MIDI out on USB + BLE, MIDI in, the Notes view, one theme, the pixel font | the owner plays the unit on its own and from a DAW over Bluetooth; a host still takes over as panel and hands back |
| **S2** | MIDI controller: CC banks A..E, KN1..4, GLO + knob assignment, the CC screens, save on SAVE hold | CCs reach a DAW over USB and BLE; assignments survive a power cycle |
| **S3** | Looper: record as played, PLAY / REC behaviour, the 16 loops with queueing, recording options, saving | 16 loops recorded, switched in time, and kept across a power cycle |
| **S4** | Arp and tempo: modes, rate, octaves, gate, SELECT tempo, external MIDI clock | the arp follows a DAW's clock |
| **S5** | The rest of the mockup: Scope and Keys views, the 8 themes and the theme menu, the furball, the cassette, user banks by SysEx, polish | every mockup screen on the unit |

## 6. Process

- Code by Opus subagents in worktrees off `standalone`, briefed one milestone at a time; Claude reviews and merges.
- Only Claude flashes, and only the RemoteFM1 unit (USB port "FM-1", identity FM-1_52000203), with a backup before
  the first standalone flash. The FIZZ unit ("ChoralRoot FM-1", FM-1_4600) belongs to the FIZZ session and is never
  touched.
- Nothing is pushed or released without the owner's word.

## 7. Open questions

1. ~~The Yamaha ROM cartridges~~ Done 2026-10-30 (the owner: "download them"): ROM1A..ROM4B from Yamaha Black Boxes
   (<https://yamahablackboxes.com/collection/yamaha-dx7-synthesizer/patches/>, `/patches/dx7/factory/romXX.syx`), kept
   outside every repo in `ChoralRootFM1/Presets/DX7-ROM/` (8 x 4,104 B, header, checksums and 32 voice names checked).
   `tools/gen_dx7_rom.py` reads them from that path; only the packed header goes into the build. ROM1A is the EU/JP
   master, ROM3A the US one; ROM3/4 repeat many ROM1/2 voices, so S1 may list each distinct voice once.
2. Saadet credit: Kerem Kilic in the README and the About screen, besides PROVENANCE.md (assumed yes).
3. **Where S2..S5 keep their data** (proposal, 2026-10-30; not implemented). The owner moves units between
   firmwares, and no other firmware's data may be lost. Since S1, RemoteFM1 writes only 0xF2000..0xF3FFF (one A/B
   object, OBJ_RF, whose records are Bluetooth's address, trim and bondings, up to 1,536 B, and the standalone's
   settings) and the panel calibration's 0xFC000..0xFDFFF (`firmware/app.h` APP_STORE_OK, fm1-base patch 0006,
   `tests/store_window_test.c`). What there is:

   | Flash | Size | Whose data is there | Writing it costs |
   |---|---|---|---|
   | 0xF2000..0xF3FFF, OBJ_RF's records | ~2 KB left of the object's 3,840 B | nobody's (ChoralRoot and the stock firmware leave it free; FIZZ's re-pushable blob may span it) | nothing |
   | XDATA0 0x93000..0x96FFF (just above fm1-base's app slot; fm1-base `plat_data_*`) | 16 KB | no data: it is ChoralRoot's and the stock firmware's code area (to 0x9CFFF); FIZZ keeps nothing there | RemoteFM1's own data there is lost whenever ChoralRoot or the stock firmware is installed (their code overwrites it). Bytes 0xF00..0xF07 of each sector must never read as an update record |
   | 0xFE000 (one sector) | 4 KB | ChoralRoot's flash map says free, but its storage.c names it the FM6 bank's copy B | maybe ChoralRoot's FM6 bank: avoid |
   | 0x97000..0xDFFFF (fm1-base's data area) | 292 KB | ChoralRoot's store (0x9D000..0xBAFFF) and USRFLASH (CZ banks, loop slots: 0xC2000..0xE5FFF); FIZZ's stores (0x97000..0xDFFFF); the stock VM and USRFLASH | their user data, unrecoverably |
   | 0xE5000..0xF1FFF, 0xF4000.. | | ChoralRoot's loop slots and sound pool, FIZZ's Bluetooth object, the stock USRFLASH; the stock resources from 0xF4000 (never) | their data |
   | RemoteFM1's own app slot above its image (~290 KB free) | | nobody's | not writable by the app (the loader's area), and erased by every update |

   Needs: S2's CC map (5 banks x 4 CCs: under 100 B) and S5's theme are small; S5's user banks are 4 KB each (+ a
   name); S3's 16 loops are up to 256 KB (16 x 2,048 events x 8 B), typically a few KB each.

   **Recommendation.** (a) S2's CC map, the theme and every small setting: more records in OBJ_RF (no new flash).
   (b) S5's user banks: XDATA0, up to three banks (12 KB) as raw sectors with their own header and CRC, written only
   when a bank arrives by SysEx; a ChoralRoot or stock install loses them, acceptable because a bank is sent again
   from the librarian that has it (the screen says when a bank is missing); the store-window test widened to exactly
   those sectors. (c) S3's loops live in RAM (the pool) and are saved to and loaded from the computer over SysEx (USB
   or Bluetooth: a page on the site, as the backup does), never in the unit's flash beside other firmwares; XDATA0's
   last 4 KB may keep one loop (~1,000 events packed to 4 B) across a power cycle if the owner wants that. Not
   recommended: claiming the data area (0x97000..0xDFFFF) behind an owner tag, as ChoralRoot's st_owner_check does:
   it would hold all 16 loops but destroy ChoralRoot's or FIZZ's user data on the first save. The owner decides
   before S3.

   **Done as recommended (2026-10-30, S2..S5).** (a) One OBJ_SA record (version 2, ~90 B) with the CC map and values,
   theme, view, arp, tempo and recording options, saved a little after a change and at once on SAVE held. (b) Three
   user banks in XDATA0: 0x93000..0x95FFF one sector each (bytes stored | 0x80, so no "AT" at 0xF06), their headers in
   0x96000; APP_STORE_OK widened to exactly 0x93000..0x96FFF, the only writers sa_ubank.c's two checked ones
   (tests/store_window_test.c, tests/run_tests.sh). (c) The loops in the pool (3,584 events + a 512-event take),
   saved and loaded over SysEx: SA_LOOP, docs/PROTOCOL.md section 8, tools/sa_loops.py; nothing of them in flash.
