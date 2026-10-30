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
| FX SEL ENV LFO EDIT | CC bank A..E | |
| GLO | shift (held) | |
| HOME | next view: Notes, Scope, Keys | |
| SAVE | the theme menu | hold: save everything (CC map, loops, theme, settings) |
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
  80 KB. Loops are in the pool (16 loops x 2,048 events x 8 B = 256 KB at most; less if the pool is short); user
  banks and saved loops go in the data region. To measure in S0.

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
