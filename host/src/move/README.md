# rfm_move: the Ableton Move as a RemoteFM1 panel

An Ableton Move running [Move Remote](https://github.com/Quixotic7/abletonmoveremote) drives a RemoteFM1 host's
emulated FM-1: its pads, buttons and knobs become the FM-1's keys, buttons, encoders and MASTER (or MIDI into the
firmware), and the FM-1's LEDs and screen come back to the Move. `rfm_move` is the one portable C99 core every host
compiles (no OS calls, no allocation), so the mapping and the sequencer are written once. The
design is the owner's: `design/remotefm1move.movedesign.json` (ableton-move-designer v3; `pad-<row>-<col>`, row 1 at
the top). Licence: MPL-2.0, like the OMX-27 modules and the rest of the host library's core.

| File | What |
|---|---|
| `host/include/rfm/move.h` | the API |
| `host/src/move/move.c` | the mapping, the MIDI modes, the LEDs, the screen |
| `host/src/move/seq.c` | the sequencer: its data, undo journal, clipboard, clock and engine |
| `host/src/move/seq_ui.c` | Sequencer mode: the Move's controls on the sequencer, its LEDs and screen |
| `host/src/move/project.c` | the project blob, the 32 project slots, the autosave |
| `host/src/move/move_int.h` | what those four files share (not an API; the tests read the data model through it) |
| `host/third_party/moveremote/` | libmoveremote's host side, vendored (upstream `bf01bb9`, MPL-2.0) |
| `host/tests/test_move.c` | ctest `move` |
| `host/tests/test_move_seq.c` | ctest `move_seq`: the sequencer |
| `host/tests/move_e2e.c`, `move_e2e.sh` | ctest `move_e2e`: against Move Remote's simulator |

## The modes (Track 1..4)

Track 1..4 select the mode; the selected Track LED is lit in the mode's colour (1 `#c43aff`, 2 `#c8ff2a`, 3
`#1ae4ff`, 4 `#ff2a2a`), the others dim in theirs. Changing mode lets go of everything held (the sequencer plays
on). In modes 1..3 the named buttons, Left / Right and the volume knob are the FM-1's (below); the knobs are the
FM-1's except in Control mode. Sequencer mode has its own (section 4).

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
The scale and root are settings (`rfm_move_set_scale`: major, minor, dorian, mixolydian, the two pentatonics,
harmonic minor, blues). **Hold Track 2**: row 2's eight pads pick the scale in that order, the jog wheel the root
(C..B, wrapping); the held screen shows them. MIDI, Control and Sequencer mode share these settings.

### 3. Control mode

MIDI mode's pads, plus the 8 knobs as CCs on the same channel: each turn moves a value the core keeps (0..127,
64 at first, not saved) and sends it on a change. Four banks, the jog wheel picks one (clamped 1..4); the rings show
the value in the bank's colour. **Hold Track 3 and turn a knob** to change its CC number. Defaults: bank 1 CC 16..23,
bank 2 CC 24..31, bank 3 CC 102..109, bank 4 CC 110..117. The volume knob is still MASTER.

### 4. Sequencer

A Move-style sequencer that takes over the whole surface except the Track buttons and the volume knob (MASTER).
16 tracks, track n on MIDI channel n (per track, changeable); 16 scenes; a clip per track and scene; projects. It
keeps playing in every mode (a small play triangle at the end of the title bar says so); Back returns to FM-1 mode.

**Two views** (Menu toggles; Track view first):

| Control | Track view | Session view |
|---|---|---|
| pads | MIDI mode's layout, scale, octave (shared), on the track's channel; a press picks the step note | clips: 8 tracks across (Left / Right: 1-8, 9-16), 4 scenes down (Up / Down scroll 16) |
| steps | the 16 steps of the page | launch scene 1..16 |
| Left / Right | page | tracks 1-8 / 9-16 |
| Up / Down | octave (MIDI mode's) | scenes |
| knobs | Control mode's CC banks, live on the track's channel; with a step held: its CC locks | the same, live |
| jog | the CC bank; a step held: its notes' length (Shift: 1/6 step); a drum pad held: its note | the CC bank |

**Steps (track view).** A tap toggles the step note on that step (an empty step gets it on the press, so it sounds
at once). Hold a step: its notes and locks are on the screen and the pads light them; pads toggle notes in every held
step (up to 8 notes a step); knobs set CC locks (from the knob's value; Delete held removes one); the jog wheel sets
the length; another step pressed while one is held stretches the held step's notes to it. A step edited while held
is not toggled on its release. Steps past the clip's end do nothing. Each note has a velocity (the pad's, or 127 with
fixed velocity), a length in ticks (24 a step) and a micro offset (-12..+11 ticks).

**Clips (session view).** A press on a clip with content launches it at the next bar (now, when stopped); on the
playing clip it stops at the next bar; on an empty clip with Rec armed it records into it, growing a bar at a time
(up to 16) until pressed again or Rec is turned off, then ends on the next bar; on an empty clip otherwise it only
selects it (to program it in Track view). Shift + clip selects without launching. LEDs: empty off (the selected
slot faint white), content dim in the track's colour, queued blinking (8ths), playing pulsing on the beat,
recording red, muted tracks dimmer. Steps in session view light green for a playing scene, blink for a queued one.
A scene launch stops tracks that have no clip in that scene.

**Buttons.**

| Move | Sequencer |
|---|---|
| Play | start / stop (when following an external clock: start at its next bar). Shift + Play: every clip stops at the next bar |
| Rec | arm / disarm recording (arming while stopped also starts). Pads record into the selected track's playing clip |
| Capture | Move's capture: the pad notes of the last 8 bars become a clip on the selected track. Shift + Capture: quantize the clip |
| Sample | tap tempo (the average of up to 4 taps; ignored, and "EXT TEMPO" shown, when following) |
| Loop | held: the steps show the clip's pages; + step n: n pages long. Shift + Loop: double the loop |
| Copy | + step / clip / (Track 4 held) track: copy, then each further press pastes while Copy is held |
| Delete | + step / clip: cut (cleared, kept for the next Copy + target); + pad (track view): that note out of the clip; Track 4 held + step: the track cleared |
| Undo | undo (multi-level); Shift + Undo: redo |
| Mute | + step n (any view) or + clip: mute track n; alone: the selected track. A mute lets its notes go at once |
| Shift + step 1 / 2 / 3 / 4 / 5 | project grid / fixed velocity / quantize / double / clock source (AUTO, INT, EXT) |
| Shift + jog | tempo, 1 BPM a detent |
| Menu | Track <-> Session view (or out of the project grid) |
| Back | FM-1 mode (out of the project grid first) |
| Track 4 held | steps = tracks (select; the LEDs: selected bright, content dim, muted red); jog = its MIDI channel |

**Projects.** Shift + step 1 opens the grid: 32 projects on the pads, top-left = project 1 (current: red, saved:
white, empty: faint). A pad: the current project is saved, then that one is loaded (the host answers
`PROJECT_LOAD`; the pad blinks until it does), or, for an empty pad, a new empty project starts there (saved at once).
Delete + pad deletes a project (the current one empties). The current pad, Shift + step 1 or Back leaves. Changes
are autosaved 2 s after the last edit (`RFM_MOVE_AUTOSAVE_MS`), and on every switch.

**Screen.** The view and the project in the title (with play / armed / rec marks), the track and channel, the tempo
and its source (INT / EXT), the clip / page / step note (or scenes and tracks), the transport with bar.beat.16th,
the held step's notes, length, velocity, offset and locks (else Shift's states, a message, the selected clip in
session view, or the last knob), and a 16-cell bar: the page's steps (filled = notes) or the bar's 16ths, the
playhead above.

## Decisions where the design left a gap

The sequencer's (the design gives labels only):

- **Time**: 96 PPQN ticks (24 a 16th step); the external MIDI clock (24 PPQN) is 4 ticks a clock, interpolated at
  the measured tempo and never more than 3 ticks ahead of the last clock (each clock catches up, stamped "now").
  4/4 bars (16 steps = a page = a bar).
- **Clock source**: AUTO (default; per project) follows an external clock while clocks arrive (500 ms timeout,
  then the internal clock at the project's tempo, playing on), INT ignores it, EXT waits for it. Following: the
  clock's Start starts the sequencer at bar 1 on the next clock, Stop stops it, Song Position + Continue resumes at
  the position's next bar; Play on the Move starts at the external clock's next bar (counted from its last Start or
  Song Position, else from the first clock seen). Play on the Move never sends transport to the firmware.
- **Every clip plays from the song's bar it launched on**; launches and stops are quantized to the next bar; while
  stopped a clip press changes the track's clip at once. Each track has one playing clip (`active`, default scene 1
  for all, so Play plays scene 1) and one edited (`sel`, follows launches). Programming a track with no playing clip
  starts its edited clip.
- **Recording** quantizes to the nearest step and keeps the offset (micro), so Shift + Capture (or Shift + step 3)
  quantizes later; a note is written when it ends (stop, mode change and project change end it too). A take (from
  Rec on) is one undo. Recording into a track with no playing clip starts its edited clip at the current bar.
- **Capture** keeps the last 256 pad events (track view). Stopped: the first note lands on step 1; playing: the
  clip starts on the bar of the first note and plays on in phase. 1..8 bars; into the edited clip if empty, else
  the track's first empty clip ("NO EMPTY CLIP" otherwise).
- **CC locks** are stored by CC number (not knob), so they follow the bank; sent at the step (no reset after).
- **Pads are shared with MIDI mode** (layout, scale, root, octave); only the channel is per track. Holding Track 2
  switches to MIDI mode as before (the settings are shared, the sequencer keeps playing).
- **Undo** is a journal of 2048 edits (note add / remove, lock, length) grouped by gesture (one step hold, one
  paste, one take); the oldest groups go when it is full; a single gesture larger than the journal is not undoable.
  Mutes, launches, tempo and channels are not undone.
- **Cut / paste**: Delete + item cuts it into the clipboard; the next Copy + target pastes it. A track copy goes
  clip by clip; Delete + track clears it without the clipboard.
- **Note-offs**: the sequencer's notes are its own (a table of 64; a note retriggered is ended first; when full the
  one due first ends). Stop, mute, a clip change, a channel change, a project load and `rfm_move_release_all` (the
  link drops: the sequencer stops) end them at once; a mode change does not stop the sequencer, its notes end on
  time. A live pad note and a sequenced note on the same channel and note are not reconciled.

- **Back = SEQ.** The design labels Back "OCT-", a second OCT- beside Left, and maps nothing to SEQ.
- **MIDI modes keep the FM-1 mapping elsewhere**: only the pads, steps and Up / Down change in mode 2 (and the
  knobs and jog wheel in mode 3); Left / Right, the named buttons and the knobs stay the FM-1's. Mode 4 takes over
  everything but the Track buttons and the volume knob.
- **Steps** do nothing in modes 2 and 3 unless Track 2 is held (the channel).
- **Up / Down transpose every layout**, the drum layout's saved notes included.
- **Drum note editing** takes the jog wheel only while a drum pad is held (else the jog wheel is SELECT, or the bank
  in Control mode). The held note still ends where it began.
- **Pads floor**: an FM-1 key whose LED is off still glows faintly (about 10 %), so the keyboard can be seen.
- **Steps' LEDs** are written as RGB note LEDs (LED_RGB kind 0, 16..31), like the pads. Not tried on a real Move
  yet: if the steps take only palette values, `led_id` in move.c is the place to change.
- A white key held by both a pad and a step (or a note by two pads) goes down with the first and up with the last.
- The CC values in Control mode start at 64 and are not saved: the knobs are relative and the target's value is
  unknown.

## API (the sequencer: below the panel's)

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

`rfm_move_t` is about 150 KB with the sequencer: keep it static or on the heap. The sequencer:

```c
rfm_move_seq_advance(&m, frames, rate);           /* every audio block, at its start, playing or not (rate 1000000: us) */
rfm_move_seq_midi_in(&m, msg, n);                 /* the firmware's MIDI out realtime: F8 FA FB FC, F2 lsb msb */
rfm_move_seq_play(&m, on); rfm_move_seq_set_bpm(&m, bpm10); rfm_move_seq_set_clock(&m, RFM_MOVE_CLOCK_AUTO);
rfm_move_seq_bpm10(&m); rfm_move_seq_following(&m);
n = rfm_move_project_save(&m, out, cap);          /* export: <= RFM_MOVE_PROJECT_MAX (37472) bytes, -1 cap too small */
rfm_move_project_check(blob, n);                  /* import: 0 valid */
rfm_move_project_load(&m, slot, blob, n);         /* PROJECT_LOAD's answer (also at start); NULL/0 = empty; < 0 invalid */
rfm_move_project_dir(&m, used_bits); rfm_move_project_flush(&m); rfm_move_project_slot(&m);
```

Every action has `frame`: for actions from `rfm_move_seq_advance`, the offset (in that call's units, from the
call's start) the event is due at; 0 (now) otherwise. Project actions: `PROJECT_SAVE` (index slot; `data` / `len`
the blob, valid during the callback), `PROJECT_LOAD` (index slot: the host calls `rfm_move_project_load`, at once
from the callback or later), `PROJECT_DELETE` (index slot). A blob: `RMSQ`, version 1, header (tempo, clock mode,
fixed velocity, selected track), 16 tracks (channel, mute, playing and edited clip), 256 clip lengths, the notes
(7 bytes each), the locks (4 bytes each), FNV-1a; little-endian; every field range-checked, keys strictly ascending,
at most 8 notes a step, the length exact. Empty: 608 bytes; 600 notes and 200 locks: 5.6 KB; the worst case
(4096 notes, 2048 locks): 37 472 bytes. Limits: 4096 notes and 2048 locks a project, 8 notes a step, clips 1..256
steps, 64 notes sounding.

Actions (`rfm_move_act_t`, the OMX-27 link's vocabulary): `KEY` (key 0..26, down, vel), `PRESS` (key, 0..127),
`BTN` (matrix button 0..13, down), `TURN` (matrix encoder 0..6, detents), `MASTER` (0..1023), `MIDI` (n bytes into
the firmware's MIDI in), `SETTINGS` (save them). Buttons and encoders are in proto/rfm1.h's matrix order, as
libremotefm1's `rfm_event_t`: buttons 0 OCT- 1 OCT+ 2 FX 3 SEL 4 ENV 5 LFO 6 EDIT 7 GLO 8 HOME 9 SAVE 10 ARP 11 SEQ
12 PLAY 13 REC; encoders 0 SELECT 1 ALGO 2..5 K1..K4 6 PRESETS (panel.js `labelToMatrix` / `roleToMatrix`).

The screen (128 × 64; Sequencer mode has its own, above): the FM-1's screen as a 64 × 64 black-and-white thumbnail (a luminance threshold, RFM_MOVE_THUMB_LUMA; no dither) on the right; on the left the mode, and in
FM-1 mode the last knob touched or turned (the firmware's label when the host gave one, else SELECT .. PRESETS,
MASTER) with its turn or value; in the MIDI modes the layout or bank, channel, octave, key and the last control.
While a Track button is held, that mode's menu fills the screen.

## Tests

`host/run_tests.sh` runs `move` (the mapping against the design's labels, every button and knob, MASTER's
clamping, mode changes and release-all, the four layouts' notes, drum editing and the settings round trip, the CC
banks and reassignment, LED diffing, three golden screens: their PBMs land in `host/build-asan/move_*.pbm`) and
`move_e2e`: Move Remote's simulator (`make sim` in an abletonmoveremote checkout; found beside this repo, or
`-DRFM_MOVEREMOTE_SIM=`, or `$MOVEREMOTE_SIM`) gets presses through its test FIFO, and the test checks the actions,
the LED writes, and the simulator's screen shot against the frame sent, then a short sequencer run (Track 4, two
steps, Play on the internal clock: the notes as MIDI actions at their frames, the step LEDs, the screen). Without the
simulator it is skipped.

`move_seq` (test_move_seq.c): the data model, step toggling and polyphony, lengths and pages, playback at exact
frames (48 kHz, 44.1 kHz, microseconds), the external clock (start, stop, continue from a song position, Play at its
next bar, the timeout), quantized launches and scenes, mutes and every path that ends notes, recording (quantize,
micro offsets, growing clips), Capture, copy / cut / paste / undo / redo and the journal's overflow, CC locks, scale
and root, the project grid's actions (sync and async answers, a failed load, delete), the autosave's debounce, the
blob round trip and a fuzzed loader (every truncation, 3000 random blobs, 20000 mutations with the check fixed up),
and three golden screens (track, session, project grid: `move_seq_*.pbm` in the build directory).

## Later phases

- **FM1VST (Mac)**: a `MoveLink` beside `RemoteLink.cpp`: `mr_connect` / `mr_poll` on a thread, the actions into
  fm1panel_t, the LEDs and screen out each frame, sound on the Mac or (optional) the Move.
- **iOS (RFMKit)** and **Android (NDK)**: compile `move.c` and the vendored client as they compile libremotefm1.
- **The web player**: done over WebSocket (WEB-PLAN.md, "An Ableton Move as the controller": `web/move/build.sh`,
  `web/site/js/move-core.js`, `move-link.js`; `mr_client.c` compiles under Emscripten and the linker drops its
  sockets). Still to come: a way past https → `ws://` (docs/REMOTEFM1.md upstream lists the options).
- **The sequencer in the web player**: `web/move/src/move_web.c` exports it (`mw_seq_advance`, `mw_seq_midi_in`,
  `mw_project_load` / `_save` / `_check` / `_dir` / `_flush`, `mw_act_frames`, `mw_blob`); `move-core.js` and the
  player still have to call them: the clock from the audio worklet, the firmware's MIDI clock if it sends one,
  the 32 projects' storage with export and import.
