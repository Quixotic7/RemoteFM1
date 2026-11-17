# rfm_move: the Ableton Move as a RemoteFM1 panel

An Ableton Move running [Move Remote](https://github.com/Quixotic7/abletonmoveremote) drives a RemoteFM1 host's
emulated FM-1: its pads, buttons and knobs become the FM-1's keys, buttons, encoders and MASTER (or MIDI into the
firmware), and the FM-1's LEDs and screen come back to the Move. `rfm_move` is the one portable C99 core every host
compiles (no OS calls, no allocation), so the mapping, and the sequencer that comes later, are written once. The
design is the owner's: `design/remotefm1move.movedesign.json` (ableton-move-designer v3; `pad-<row>-<col>`, row 1 at
the top). Licence: MPL-2.0, like the OMX-27 modules and the rest of the host library's core.

| File | What |
|---|---|
| `host/include/rfm/move.h` | the API |
| `host/src/move/move.c` | the mapping, the MIDI modes, the LEDs, the screen |
| `host/third_party/moveremote/` | libmoveremote's host side, vendored (upstream `bf01bb9`, MPL-2.0) |
| `host/tests/test_move.c` | ctest `move` |
| `host/tests/move_e2e.c`, `move_e2e.sh` | ctest `move_e2e`: against Move Remote's simulator |

## The modes (Track 1..4)

Track 1..4 select the mode; the selected Track LED is lit in the mode's colour (1 `#c43aff`, 2 `#c8ff2a`, 3
`#1ae4ff`, 4 `#ff2a2a`), the others dim in theirs. Changing mode lets go of everything held. In every mode the named
buttons, Left / Right and the volume knob are the FM-1's (below); the knobs are the FM-1's except in Control mode.

### 1. FM-1 mode

| Move | FM-1 |
|---|---|
| pads, row 2 (second from the top) | white keys F G A B C D E F: keys 0 2 4 6 7 9 11 12 (F3..F4) |
| pads, row 1 (top), columns 2 3 4 6 7 | F# G# A# C# D#: keys 1 3 5 8 10 (columns 1, 5, 8 off) |
| pads, row 4 (bottom) | white keys G A B C D E F G: keys 14 16 18 19 21 23 24 26 (G4..G5) |
| pads, row 3, columns 1 2 3 5 6 8 | F# G# A# C# D# F#: keys 13 15 17 20 22 25 (columns 4, 7 off) |
| steps 1..16 | the 16 white keys in order (key 0 F3 .. key 26 G5), velocity 100 |
| pad velocity / poly aftertouch | the key's velocity / pressure (0..127; `rfm_move_lean` gives key_lean 0..100) |
| Capture Sample Mute Delete Copy Shift Menu Undo Loop **Back** Play Rec Left Right | FX SEL ENV LFO EDIT GLO HOME SAVE ARP **SEQ** PLAY REC OCT- OCT+ |
| knob 1, 2, 5, 6, 7, 8 (3 and 4 unused) | PRESETS, ALGO, K1, K2, K3, K4 (detent for detent) |
| jog wheel turn | SELECT (the click does nothing: the FM-1 has no encoder push) |
| volume knob | MASTER, 16 a detent, 0..1023 |
| Up / Down | nothing (they are the MIDI modes' octave) |

Pads glow white (white keys) or pink `#ff7aa8` (black keys) at the FM-1's own LED level for that key (panel.js's
`ledAlpha` curve, as the OMX-27's `leds(keyLevel)`), never below a faint floor so the keyboard stays visible; a
pressed pad shows the mode colour. Steps mirror their white keys the same way. The button LEDs mirror the FM-1's
button LEDs (Play: white at PLAY's level plus green at PLAY's green level; Rec red; the rest white). Knob rings: the
mapped knobs in the mode colour, bright while touched.

### 2. MIDI mode

Pads play notes into the firmware's MIDI in (`RFM_MOVE_ACT_MIDI`: note on with the pad's velocity, poly
aftertouch, note off) on the selected channel (default 1). **Hold Track 2**: the top row's pads 1..4 pick the
layout (the current one bright), steps 1..16 pick the channel (the current one lit). Up / Down: the octave, -4..+4.

| Layout | Notes (base = 48 + root + 12 × octave: C3 at the defaults) |
|---|---|
| 1 Isomorphic in scale (default; C major) | bottom-left = root; +1 scale degree a column, +3 degrees (a 4th) a row |
| 2 Isomorphic chromatic | +1 semitone a column, +5 a row |
| 3 Drum | bottom-right = C2 (36), ascending leftwards, then the next row up from the right (36..67); hold a pad and turn the jog wheel to change its note (saved) |
| 4 Octave | each row one octave: 7 degrees and the octave above, rows an octave apart |

Pads: the root in the mode colour, in-scale notes white-ish, the rest dim, pressed ones white, notes past 0..127 off.
The scale and root are settings (`rfm_move_set_scale`: major, minor, dorian, mixolydian, the two pentatonics).

### 3. Control mode

MIDI mode's pads, plus the 8 knobs as CCs on the same channel: each turn moves a value the core keeps (0..127,
64 at first, not saved) and sends it on a change. Four banks, the jog wheel picks one (clamped 1..4); the rings show
the value in the bank's colour. **Hold Track 3 and turn a knob** to change its CC number. Defaults: bank 1 CC 16..23,
bank 2 CC 24..31, bank 3 CC 102..109, bank 4 CC 110..117. The volume knob is still MASTER.

### 4. Sequencer

A placeholder for a later phase: the pads are MIDI mode's, the screen says the sequencer is coming.

## Decisions where the design left a gap

- **Back = SEQ.** The design labels Back "OCT-", a second OCT- beside Left, and maps nothing to SEQ.
- **MIDI modes keep the FM-1 mapping elsewhere**: only the pads, steps and Up / Down change in modes 2 and 4 (and
  the knobs and jog wheel in mode 3); Left / Right, the named buttons and the knobs stay the FM-1's.
- **Steps** do nothing in modes 2..4 unless Track 2 is held (the channel).
- **Up / Down transpose every layout**, the drum layout's saved notes included.
- **Drum note editing** takes the jog wheel only while a drum pad is held (else the jog wheel is SELECT, or the bank
  in Control mode). The held note still ends where it began.
- **Pads floor**: an FM-1 key whose LED is off still glows faintly (about 10 %), so the keyboard can be seen.
- **Steps' LEDs** are written as RGB note LEDs (LED_RGB kind 0, 16..31), like the pads. Not tried on a real Move
  yet: if the steps take only palette values, `led_id` in move.c is the place to change.
- A white key held by both a pad and a step (or a note by two pads) goes down with the first and up with the last.
- The CC values in Control mode start at 64 and are not saved: the knobs are relative and the target's value is
  unknown.

## API

```c
rfm_move_t m;                                     /* the host owns it; no allocation */
rfm_move_init(&m, &saved_or_NULL, on_action, ctx);
rfm_move_input(&m, &mr_event, now_ms);            /* from mr_client's on_input */
rfm_move_panel(&m, levels44, play_green);         /* levels[col * 4 + line] 0..255: fm1panel_t led[11][4] */
n = rfm_move_leds(&m, out, max);                  /* only the changed LEDs: mr_led_rgb / mr_led each */
rfm_move_screen(&m, rgb565_240x240_or_NULL, packed1024);   /* then mr_display */
rfm_move_release_all(&m);                         /* the link dropped, or the host leaves Move control */
rfm_move_leds_invalidate(&m);                     /* a new connection: repaint */
rfm_move_set_master(&m, v); rfm_move_set_label(&m, enc, "CUTOFF"); rfm_move_set_keymap(&m, km);
rfm_move_save(&m.s, bytes); rfm_move_load(&s, bytes, n);   /* "RMV" + version 1, 77 bytes */
```

Actions (`rfm_move_act_t`, the OMX-27 link's vocabulary): `KEY` (key 0..26, down, vel), `PRESS` (key, 0..127),
`BTN` (matrix button 0..13, down), `TURN` (matrix encoder 0..6, detents), `MASTER` (0..1023), `MIDI` (n bytes into
the firmware's MIDI in), `SETTINGS` (save them). Buttons and encoders are in proto/rfm1.h's matrix order, as
libremotefm1's `rfm_event_t`: buttons 0 OCT- 1 OCT+ 2 FX 3 SEL 4 ENV 5 LFO 6 EDIT 7 GLO 8 HOME 9 SAVE 10 ARP 11 SEQ
12 PLAY 13 REC; encoders 0 SELECT 1 ALGO 2..5 K1..K4 6 PRESETS (panel.js `labelToMatrix` / `roleToMatrix`).

The screen (128 × 64): the FM-1's screen as a 64 × 64 dithered thumbnail on the right; on the left the mode, and in
FM-1 mode the last knob touched or turned (the firmware's label when the host gave one, else SELECT .. PRESETS,
MASTER) with its turn or value; in the MIDI modes the layout or bank, channel, octave, key and the last control.
While a Track button is held, that mode's menu fills the screen.

## Tests

`host/run_tests.sh` runs `move` (the mapping against the design's labels, every button and knob, MASTER's
clamping, mode changes and release-all, the four layouts' notes, drum editing and the settings round trip, the CC
banks and reassignment, LED diffing, three golden screens: their PBMs land in `host/build-asan/move_*.pbm`) and
`move_e2e`: Move Remote's simulator (`make sim` in an abletonmoveremote checkout; found beside this repo, or
`-DRFM_MOVEREMOTE_SIM=`, or `$MOVEREMOTE_SIM`) gets presses through its test FIFO, and the test checks the actions,
the LED writes, and the simulator's screen shot against the frame sent. Without the simulator it is skipped.

## Later phases

- **FM1VST (Mac)**: a `MoveLink` beside `RemoteLink.cpp`: `mr_connect` / `mr_poll` on a thread, the actions into
  fm1panel_t, the LEDs and screen out each frame, sound on the Mac or (optional) the Move.
- **iOS (RFMKit)** and **Android (NDK)**: compile `move.c` and the vendored client as they compile libremotefm1.
- **The web player**: done over WebSocket (WEB-PLAN.md, "An Ableton Move as the controller": `web/move/build.sh`,
  `web/site/js/move-core.js`, `move-link.js`; `mr_client.c` compiles under Emscripten and the linker drops its
  sockets). Still to come: a way past https → `ws://` (docs/REMOTEFM1.md upstream lists the options).
- **The sequencer (mode 4)**: Move-style steps, 16 tracks by MIDI channel, session view; the settings image's version
  goes up with it.
