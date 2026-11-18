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
| `host/src/move/move.c` | the mapping, the MIDI modes, the LEDs |
| `host/src/move/screen.c` | the screen: FM-1, MIDI and Control mode, the popups, the held Track buttons' menus |
| `host/src/move/seq.c` | the sequencer: its data, undo journal, clipboard, clock and engine |
| `host/src/move/seq_ui.c` | Sequencer mode: the Move's controls on the sequencer, its LEDs and screens |
| `host/src/move/project.c` | the project blob, the 32 project slots, the autosave |
| `host/src/move/move_int.h` | what those five files share (not an API; the tests read the data model through it) |
| `host/third_party/moveremote/` | libmoveremote's host side, vendored (upstream `bf01bb9`, MPL-2.0) |
| `host/tests/test_move.c` | ctest `move` |
| `host/tests/test_move_seq.c` | ctest `move_seq`: the sequencer |
| `host/tests/move_e2e.c`, `move_e2e.sh` | ctest `move_e2e`: against Move Remote's simulator |

## The modes (Track 1..4)

Track 1..4 select the mode; the selected Track LED is lit in the mode's colour (1 `#c43aff`, 2 `#c8ff2a`, 3
`#1ae4ff`, 4 `#ff2a2a`), the others dim in theirs. Changing mode lets go of everything held (the sequencer plays
on). In modes 1..3 the named buttons, Left / Right and the volume knob are the FM-1's (below); the knobs are the
FM-1's except in Control mode. Sequencer mode has its own (section 4).

**Switching.** All four Track buttons, in every mode, switch **only on a quick tap**: released within
`RFM_MOVE_TAP_MS` (300 ms) with nothing done while down. A press never switches, nor does a hold. At the tap's release
the new mode's name is drawn at once, over the whole screen (no frame, as big as it fits, centred), and the mode's own
screen only once the name has gone (a second; `rfm_move_screen_timed` keeps the host drawing). A tap on the Track
button of the mode already on turns its screens where it has several, naming each: Track 2 in MIDI mode, PADS / NOTES.

**Holding**, from any mode, gives that button's layer or menu over the mode that is on, which stays on (the menu's
screen appears once held 300 ms, or at once when used; no name at the release): Track 1 the Move's Shift layer
(below); Track 2 the MIDI menu (top pads the layout, the other pads the scale, steps the channel, knob 1 the root,
knob 2 the scale, Up / Down the octave); Track 3 the CC numbers of the bank (knobs); Track 4 the sequencer's track
menu (steps: tap select, double tap solo, hold mute; its knobs, jog and Up / Down). Let go: back to the mode as it was.

### 1. FM-1 mode

In FM-1 mode every popup (a knob, MASTER, a message) keeps to the left half, over the drawing: the FM-1's screen on the right stays in view.
**Track 1 held: the Move's Shift layer**, in every mode (FM-1, MIDI, Control, Sequencer; in FM-1 and MIDI mode
Shift itself stays the FM-1's GLO). With Track 1 held the steps are the
Move's Shift + step shortcuts on the sequencer (projects, setup, Workflow, Tempo with its taps, Metronome, Groove,
Layout, Keys & Scales, the arp, Full Velocity, New Clip, Double Loop, Quantize; the ones with a job lit), and Record,
Play, Capture, Undo, Loop and Sample their Shift + button (Shift + Record: the record settings). A menu opened this way
stays open after Track 1 is let go (full width; the jog, the knobs and, where it uses them, the pads and steps are
the menu's) until Back, which closes it rather than pressing the FM-1's SEQ. While Track 1 is held a small SHIFT badge
sits top left (the FM-1's screen stays on the right in FM-1 mode), and the icons under the steps light; the pads
stay what they were (FM-1 keys, MIDI notes), at once. A quick tap on Track 1 switches to FM-1 mode (named).
A knob's popup comes only with turning (its detents, from 0 each gesture): a touch alone lights the knob on the
drawing; the release drops the popup and its count. MASTER the same. The jog wheel (SELECT) has the same popup
while it turns (the firmware's SELECT label, its detents), gone and counted from 0 again a second after it stops.

| Move | FM-1 |
|---|---|
| pads, row 2 (second from the top) | white keys F G A B C D E F: keys 0 2 4 6 7 9 11 12 (F3..F4) |
| pads, row 1 (top), columns 2 3 4 6 7 | F# G# A# C# D#: keys 1 3 5 8 10 (columns 1, 5, 8 off) |
| pads, row 4 (bottom) | white keys G A B C D E F G: keys 14 16 18 19 21 23 24 26 (G4..G5) |
| pads, row 3, columns 1 2 3 5 6 8 | F# G# A# C# D# F#: keys 13 15 17 20 22 25 (columns 4, 7 off) |
| steps 1..16 | the 16 white keys in order (key 0 F3 .. key 26 G5), velocity 100 |
| pad velocity / poly aftertouch | the key's velocity / pressure (0..127; `rfm_move_lean` gives key_lean 0..100) |
| Capture Sample Mute Delete Copy Shift Menu Undo Loop **Back** Play Rec Left Right | FX SEL ENV LFO EDIT GLO HOME SAVE ARP **SEQ** PLAY REC OCT- OCT+ |
| knob 1, 2, 5, 6, 7, 8 (3 and 4 unused) | PRESETS, ALGO, K1, K2, K3, K4: an FM-1 detent every 2 of the Move's ticks (`RFM_MOVE_KNOB_TICKS`, the owner's: half as fast; a turn back or the knob let go forgets a half); the same in MIDI mode |
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
harmonic minor, blues, and since the owner asked for 24: phrygian, lydian, locrian, melodic minor, whole tone,
diminished half-whole and whole-half, Hungarian minor, phrygian dominant, in sen, hirajoshi, iwato, pelog, double
harmonic, Spanish 8-tone, bebop dominant; the first 8 keep their indexes, so older settings and projects mean the
same). **Hold Track 2**: rows 2, 3 and 4 of the pads (24) pick the scale in that order (lit cyan, the current one
full: a colour of their own), the top row the layout, the jog wheel or knob 1 the root (C..B, wrapping), knob 2 the
scale (both list knobs: one a few ticks); the screen shows them (the 24 as 3 rows of tabs, the current one a block),
and while a scale pad is pressed or knob 2 turns that scale's short name alone at 2x, its tab tall. MIDI mode has no Shift shortcuts (the
owner's: only the sequencer uses the Move's Shift; FM-1 mode has its Track 1 layer): Shift is the FM-1's GLO. MIDI
and Control mode share these settings; the sequencer's GLOBAL key is its project's.

**MIDI mode's screens** (a tap on Track 2 in MIDI mode turns them; kept in the settings, `midi_view`): **PADS**, the
pad map (below), full-width popups; **NOTES**, the FM-1's screen on the right half and on the left the notes held
(their names, 2x when they fit) and the chord they make (Cmaj7, Am, Dsus4, G7, m7b5 .., an inversion as a slash chord,
C/E; black at rest). In NOTES the popups keep to the left half and the knobs follow FM-1 mode's rule (a popup while
turning only).

### 3. Control mode

MIDI mode's pads, plus the 8 knobs as CCs on the same channel: each turn moves a value the core keeps (0..127,
64 at first, not saved) and sends it on a change. Four banks, the jog wheel picks one (clamped 1..4); the rings show
the value in the bank's colour. **Hold Track 3 and turn a knob** to change its CC number. Defaults: bank 1 CC 16..23,
bank 2 CC 24..31, bank 3 CC 102..109, bank 4 CC 110..117. The volume knob is still MASTER.

### 4. Sequencer

The owner's aim: a sequencer that feels like the Move's own, with 16 tracks instead of 4, and conditions, functions
and the rest of the TRIG page on top. It takes over the whole surface except the Track buttons (the four modes, as
everywhere) and the volume knob (MASTER). 16 tracks, each with its own MIDI channel (track n: channel n), rate,
groove (or the project's), key and scale (or the project's), arp, pad layout and CC numbers; 16 scenes; a clip per
track and scene; projects. It keeps playing in every mode (in FM-1 mode a small play triangle over the drawing says
so). Back only closes layers; it never changes the mode (only a quick Track tap does, the owner's).

**Tracks: Track 4 held** (the owner's). The 16 steps are the 16 tracks: **a tap selects** one, **a double tap solos**
it (on / off), **a hold mutes** it (on / off, 300 ms). LEDs: the selected track full, the others dim, muted red,
soloed blue. Solo: while any track is soloed only the soloed tracks sound. While Track 4 is held the knobs set the
bank's CC numbers ("K3 CC74"), the pads the layout (top row) and scale (row 2), Up / Down the rate, the jog the MIDI
channel; Copy / Delete + a step copy / clear a track; Mute + step mutes. The track view shows which of the 16 (a 4 x
4 in the top-right corner: tracks 1-4 the first column, the selected one filled). Session view's Left / Right move
its 8 columns through the 16 tracks.

**Two views** (Menu toggles; Track view first):

| Control | Track view | Session view |
|---|---|---|
| pads | the track's layout, the scale and octave (shared with MIDI mode), on the track's channel; what they play is what an empty step gets | clips (the owner's mockup): a column a track (8 in view), a row a clip slot (4 in view, the top row first) |
| steps | the 16 steps of the page | the 2 under each column: the first stops that track's clips (at the bar; lit while it plays something, blinking while it stops), the second mutes it (the track's colour; muted: red, dim) |
| Left / Right | page, round the loop's pages; in Loop's page edit, Right past the last page: a ghost page (below); a step held: micro timing (the MICRO popup) | the tracks, one at a time (1..16) |
| Up / Down | octave (MIDI mode's); a step held: transpose | the clip slots, one at a time |
| knobs | the knob page: TRIG (below), then Control mode's CC banks 1..4, live on the track's channel; a step held: its CC locks | nothing; a touch selects the track in that knob's column |
| volume knob | MASTER; a step held: its notes' velocity (a VELO popup) | MASTER |
| jog | the knob page (TRIG, banks 1..4); a step held: its notes' length (the LENGTH popup); Loop held or latched: the loop's length; a Shift menu: its value; a drum pad held: its note | the knob page |
| jog pressed | FILL while held (the FILL / !FILL conditions) | the same |

**Steps (track view)**, as on the Move, with the owner's changes (2026-11-17):

- **Pads, then a step.** Hold a note or a chord on the pads and press a step: the step gets exactly the held
  notes, with their velocities (what was on it goes).
- **A tap** (a press shorter than `RFM_MOVE_SEQ_HOLD_MS`, 300 ms): on an empty step it puts the *last notes* (the
  last step entered or edited, the last pad chord, or the root) at the release; on a step with notes it clears them.
  A tap shows no popup.
- **A hold** never clears a step. Holding a step with notes plays it once (a preview, on the track's channel); after
  300 ms its popup comes up. Holding an empty step leaves it empty: the pads pressed while it is held go into it.
  While a step is held: pads add or remove notes (up to 8); the volume knob sets the velocity; the jog wheel the
  length (2 ticks a detent; Shift: whole steps); Left / Right nudge (Shift: 1 tick); Up / Down transpose (Shift: an
  octave); the knobs set CC locks or (TRIG page) its trig parameters; a later step pressed stretches the notes to it.
- **Any page can be edited**, in the loop or not (outside it: dimmer LEDs, the screen's steps row a line).
- **Only Play starts or stops the transport** (the owner's; the Move starts it with the first note into an empty clip,
  this does not). Steps, held steps (their notes heard), pads, double taps, menus, Capture, Sample, Rec and project
  loads leave it as it was. The one exception: Rec armed in CAPTURE mode, whose first note starts playback and the
  recording (the owner's earlier request). A project loaded while playing keeps playing (from its first bar).
- **Step LEDs** (Move's): notes white, an empty step the track's colour dimly (all the same: the Move's LED shapes
  mark the beats), the playhead green, a held step the track's colour, steps outside the loop dim; a copied step
  blinking white, a cut one red; Step record's cursor red.

**Pages and the loop.** A clip's loop is a start and a length (`lstart`, `len`: any pages of the 16).

- **Loop held, or latched: page edit** (a tap on Loop latches it, its LED glowing, until a tap on Loop or Back): the
  steps are the 16 pages: the loop's in the track's colour, the one shown white, the playing one pulsing, the clip's
  others (outside the loop) dim, past the clip off. A step: show that page. Two steps held: the loop is those pages and
  the ones between (the page shown moves into it). A double tap (400 ms): the loop is that page alone. The jog:
  the length, a step a detent (Shift: a page), growing into new pages up to 16 (the owner's; the Move's wheel moves
  bars). The popup: the length in bars (+ steps), a bar over the 16 pages.
- **Left / Right** outside page edit (the owner's): the page shown, round the loop's pages only (loop 3..6: Right
  from 6 is 3, Left from 3 is 6); never a ghost page. A step held: micro timing, as before.
- **Ghost pages** (Move's "+" bar), in page edit only: Right past the loop's last page goes to a page that is not in
  the clip yet (it stays shown when page edit ends, for a step to go in; Left / Right then leave it); each
  Right or Left that adds or takes one away shows "BAR n" for a second. The page strip is then a 1 px line under the
  real pages, blank for the ghosts, a "+" at the far end, and the clip's steps / pages in a small box. The clip grows
  to that page only when a note goes in there (a step, a paste); leaving it adds nothing.
- **Pages cut, copied, pasted** (Loop held or latched): Delete + a page with notes cuts it (gone, an X on its step and
  in the popup's page strip), Copy + one copies it (dashed); Delete or Copy + an empty page pastes there (a pasted cut
  empties the clipboard). A page clipboard of its own, apart from the steps'. Back clears its mark.
- **Shift + Loop** / Shift + step 15: double the loop (its content copied into the new pages).

**The TRIG page** (the jog left of bank 1; its popup at 2x with a picture). Per held step, or with no step held
every step with notes in the clip:

| Knob | Trig parameter (Elektron's) |
|---|---|
| 1 MICRO | the notes' micro timing, a tick (1/24 step) a detent, -12..+11 |
| 2 RATCH | ratchets: 1..8 hits spread over the step (the notes shortened to fit) |
| 3 CHANCE | 1..100 % (100: none) |
| 4 COND | none, A:B (1:2 .. 8:8: plays on pass A of every B), FILL, !FILL, PRE, !PRE (the track's last condition), NEI, !NEI (the track before's), 1ST, !1ST |
| 5 FUNC | FIZZ's step functions: RESTART, REVERSE, FORWARD, FLIP, RANDOM (jump), RND FN, LEAP n, JUMP n (run when the step plays) |
| 6 AMOUNT | LEAP's n (-15..15, +2 at first) or JUMP's target step |
| 7 LENGTH | the notes' length through Elektron's table: 1/8, 1/4, 1/2, 3/4, 1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32, 64 steps, INF |

The trig parameters are CC locks numbered 128..133 (`RFM_MOVE_TRIG_*`, never sent), so undo, copy, cut, paste,
double and the blob carry them. The engine walks each track with a playhead (`pos`, `sub`, `tps`): the next step is
chosen half way through a step (functions, conditions and chance decided then), so early notes of the next step
sound in time.

**Clips (session view)**: a press on a clip launches it at the next bar (queued, blinking; stopped: at once, playing
with Play); on the playing clip it stays. **A double tap** (300 ms) launches it now, not at the bar: in phase
with the bar the song is in (the Move's manual has no double press, so in phase rather than from the clip's start;
stopped: at once, playing with Play). **A press on an empty slot makes a new empty clip there** (drawn as a clip),
selects it and launches it (with Rec armed: records into it, growing a bar at a time). **Shift + a clip** launches its
whole row, that scene across the 16 tracks; **Shift + Play** stops every clip (at the bar). Cut, copy and paste as
below.

**Copy and cut** (the owner's; no popups: the source is marked, dashed when copied, an X when cut, on the steps row,
the session grid and the LEDs). One clipboard for both:

- **Copy held**: a step (or clip) with content is copied; an empty step (or slot) gets the clipboard pasted (a copy
  or a cut). Copy never overwrites a full step: pressing one copies it again.
- **Delete held**: a step (or clip) with content is cut (gone, kept in the clipboard, an X on it); an empty one gets
  the clipboard pasted. So step, cut, press it again: pasted back, again: cut. A pasted cut empties the clipboard.
- Copy released after copying: the next plain press pastes there (Move's way). Copy alone: a waiting copy is
  dropped, else the clip is duplicated into the track's next empty slot. Delete alone: the selected clip deleted.
  Delete + pad (track view): that note out of the clip. Track 4 held + Copy / Delete + step: a track copied / cleared.

**Recording** (the record settings, Shift + Record: the jog the mode, knob 2 the style; saved with the project):

| Mode | Record |
|---|---|
| CAPTURE (default) | stopped: the owner's intentional capture. Record arms it (blinking); the first note played clears the selected clip, starts the transport and is its first step; the take records with micro timing; Record again ends it at the nearest bar line (just past one: there, at once; else the next), the clip as many pages as bars. Playing: Record records into the existing pages (as RECORD) |
| RECORD | Move's: armed while stopped (no start); Play then gives a bar's count-in, then the transport; records into the selected track's clip |
| STEP | Record toggles a cursor (a red step, a bar over it on screen): each note or chord played goes into the cursor's step, then the cursor moves a step on (the track's rate), round the loop |

| Style | |
|---|---|
| OVERDUB (default) | the take adds to what is there |
| REPLACE | the playhead clears what the take passes over (not the take's own notes) |

Live notes go in **at the press** (visible at once) at the step nearest where they were heard, with their micro
offset, on the track's rate grid; their played length at the release. The host's lead (`rfm_move_seq_set_lead_us`:
the web player's 30 ms) is taken off. A note being recorded is not played again by the clip while it is held.

**Capture** (the Capture button): the last phrase played on the pads becomes a clip on the selected track: back from
the last note until a silence of more than 2 bars (`RFM_MOVE_SEQ_CAPTURE_GAP`), at most 16 bars, rounded to whole
bars. In an empty project (no clips) and stopped, the phrase's own tempo is found (the best fit of a 16th grid and
whole bars, 70..180 BPM) and set. Playing: aligned to the song's bars. Shift + Capture: the capture forgotten.

**Sample** (the audio loop): a press arms it (blinking); it records the firmware's audio from the next bar line
(stopped: from the first bar once Play starts the transport); a press while recording ends it
at the next bar line, so the take is exactly N bars (`RFM_MOVE_ACT_AUDIO_REC`, frame-stamped; the web player's
worklet cuts at those frames). The take goes to the recorder's Takes, named with its tempo and bars. A waveform in
the screen's corner while it records. (Tap tempo: Shift + step 5.)

**Buttons.**

| Move | Sequencer |
|---|---|
| Play | start / stop (following an external clock: at its next bar). Shift + Play: every clip from its start (session view: every clip stops) |
| Record | the record mode's (above). Shift + Record: the record settings |
| Capture | Capture (above) |
| Sample | the audio loop (above) |
| Loop | held or latched: pages and the loop (above). Shift + Loop: double the loop |
| Copy, Delete | copy, cut, paste (above) |
| Undo | undo (multi-level); Shift + Undo: redo |
| Mute | held: the screen shows the 16 tracks (with notes filled, muted crossed out); + step n or + clip: mute track n; alone: the selected track |
| Shift (the icons) | while Shift is held or locked (and in FM-1 mode while Track 1 is held), the white icon LEDs under the steps (CC 16..31) light every step with a shortcut: dim, full where it is on (a toggle on, its menu open); off otherwise and in MIDI and Control mode. Written on change only, like every LED; the step pads' own LEDs are separate |
| Shift | double-tap (a tap, then a press within 300 ms): latched (its LED lit, every Shift combination without holding it); a press unlatches it. Shift + Sample: nothing (the Move has none) |
| Menu | Track <-> Session view (or out of the project grid) |
| Back | the universal escape: closes the latest layer still on, one a press (a Shift menu, the project grid, Loop's latch, Shift's lock, the step / clip clipboard's marks, the page clipboard's); with none: nothing (never a mode change) |
| Track 1..4 | the modes (FM-1, MIDI, Control, Sequencer); Track 4 held in the sequencer: the tracks and the track's settings (above; the screen: layout, scale, what changed at 2x, the channel and the rate as strips) |

**Shift + step** (the Move's printed functions, its manual's numbers; a menu opens on the screen, the same Shift +
step or Back closes it):

| Step | Function |
|---|---|
| 1 Set Overview | the project grid |
| 2 Setup | the clock source (AUTO, INT, EXT) |
| 3 Workflow Settings | menu: the jog the track's MIDI channel, knob 2 its rate, knob 3 Quantize's amount (5 % a detent) |
| 5 Tempo | each press is a tap (tap tempo: the average of up to 4 taps) and opens the menu: the tempo at 4x; the jog 1 BPM (Shift, or knob 2: 0.1); following the firmware's clock: locked (a padlock, EXT; taps ignored) |
| 6 Metronome | on / off (off at first): a click a beat, the bar's first higher (`RFM_MOVE_ACT_CLICK`, frame-stamped; the web player plays it after the volume, never into a recording); also in the count-in |
| 7 Groove | menu: the groove, 5 % a detent (Shift: 1 %), 0..150 %: the off 16ths late, 100 % a triplet swing (Move's). Knob 2: GLOBAL (the project's groove, the default) or TRACK (the track's own, starting from the global one) |
| 8 Layout | menu: the pads' top row picks the layout (the jog too) |
| 9 Keys & Scales | menu: row 2's pads the scale (knob 2 too), the top row the layout, the jog the root. Knob 3: GLOBAL (the project's key and scale, the default; a new project starts with MIDI mode's) or TRACK (the track's own) |
| 10 Velocity | menu, sticky (Back or Shift + step 10 closes it): the jog the option, DYNAMIC (as played), FIXED (every pad at the level), SOFT (a light touch plays louder), HARD (it takes a firmer one); knob 1 the level 1..127 (FIXED's). The screen: the name big, the curve (played in, out), the level big with its bar (dotted while not FIXED). Every pad that plays goes through it, in every mode (MIDI notes, FM-1 keys, the sequencer's live notes, recordings and step entry). A setting (saved with the others), not the project's; its icon LED is full while FIXED |
| 11 Repeat (arp) | menu: the track's arp, FIZZ's ARP (its defaults: 1/8, two octaves, QUANT 1/16). The jog turns its pages. Page 1: knobs PATTERN (UP .. AS PLAYED, 17), RATE (1/1 .. 1/64 of a bar), RANGE (octaves), CHANCE, MODE (OFF, ON, 1-ST, ONCE, HOLD), GATE, OCT DIST, QUANT (when a run starts: at once, or the next 1/n of the song's bars); the pattern's walk drawn. Pages 2 and 3: the MOD lane (x as played, . rest, - tie, R repeat, < lowest -oct, > highest +oct, " power chord, # every held note, 1..6 the n-th played) and the TRANSPOSE lane (-48..+48): the steps pick a lane step, knob 1 its value, knob 2 RESET (NORM, NOTE: a new note restarts it, MPAT / TPAT: as that lane comes round), knob 4 the lane's length. On, the track's pads play through it; recording, its notes go into the clip with their timing and length |
| 14 New Clip | the track's next empty slot gets a new clip, selected |
| 15 Double Loop | double the loop |
| 16 Quantize | the clip's micro timing quantized by the Workflow amount (100 %: on the grid) |

**Track rates**: 1/32T 1/32 1/16T 1/16 1/8T 1/8 1/4T 1/4 1/2T 1/2 (a step 8 .. 192 ticks at 96 PPQN); a step
always has 24 micro ticks, scaled to the rate.

**Projects mode** (the owner's: a mode of its own, just for picking a project; Shift + step 1, or Track 1 held +
step 1 from any mode). The pads are the 32 projects (top-left = project 1), each in its own colour (dim; the open
one full, pulsing on the beat while it plays; loading: blinking; empty: off). Everything else does nothing here:
steps, the jog, the knobs (but one), the Shift shortcuts, the Track buttons.

- **A pad**: that project is selected: the open one saved, that one loaded (the host answers `PROJECT_LOAD`), or, for
  an empty pad, a new empty project there (saved at once). Projects mode stays.
- **Delete + a project**: cut, into the host's buffer and gone at once (`RFM_MOVE_ACT_PROJECT_CUT`; an X on the
  screen, its pad red and blinking); **Copy + a project**: copied (`..._COPY`; dashed, blinking); **Delete or Copy +
  an empty pad**: the buffer pasted there (`..._PASTE`; a cut once, a copy again and again). The open project cut:
  saved first, an empty project in its slot.
- **A pad held + knob 1**: its colour, one of 16 (the Move's palette; one every few ticks), shown big on the screen;
  stored with the project (blob byte 18, `..._COLOR`: the host sets it in the stored blob and makes its check again).
- **Play**: the selected project plays (still loading: once loaded); Play again stops it.
- **Back** or **Menu** leaves.

Changes are autosaved 2 s after the last edit (`RFM_MOVE_AUTOSAVE_MS`), and on every switch. Blob version 3 (current) adds
to version 2 the project's groove, key and scale, each track's GLOBAL / TRACK choices, own key and scale and solo,
and the arp's QUANT, RESET and lanes. Version 2 (loop starts, made clips, rates, grooves, arps, layouts, CC numbers,
the record settings) and version 1 blobs still load (a version 2 groove becomes the track's own). The project's
colour is byte 18 of the header in every version (older blobs: 0, the slot's own colour).

**List knobs.** A menu knob that picks from a list (a scale, a root, a layout, a rate, an arp pattern, mode or
QUANT, RESET, a MOD lane value, the record mode and style, GLOBAL / TRACK, a condition, a step function, a project's
colour) moves one item every `RFM_MOVE_LIST_TICKS` (5) of the Move's ticks, the same helper everywhere
(`rfmv_list_ticks`); a turn back or the knob let go forgets the rest. Numbers (tempo, groove, gate, ..) as before;
the jog wheel stays 1:1.

**Screens**: track view, session view and the project grid are pictures (below, "The screen").

**Move's sequencer and this one.** Checked against the Move manual (`AbletonMove/move1-manual-en.md`). Where the
owner chose otherwise, the owner's way: Delete = cut / paste, Copy pastes to empty steps, the record modes (Capture
first), Sample = an audio loop (tap tempo on Shift + step 5), the Loop wheel a step a detent (Shift: a page; the
Move's: a bar, Shift a 16th), a hold never clears a step, an empty slot makes a clip. Left as they are:

- **The design's buttons**: Menu = Track / Session (a view, not a mode), Back = close a layer, the Track buttons = the four modes, so Track
  4 held + step stands in for Move's Track buttons (select, solo, mute) and Mute + step for Mute + Track.
- **Not here**: copying a range of steps, per-step automation shown as red steps, count-in on / off, the
  Move's Repeat for drums (step 11 is the arp).

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
  stopped a clip press changes the track's clip at once (playing with Play). Each track has one playing clip (`active`, default scene 1
  for all, so Play plays scene 1) and one edited (`sel`, follows launches). Programming a track with no playing clip
  starts its edited clip.
- **Recording** quantizes to the nearest step and keeps the offset (micro), so Shift + Capture (or Shift + step 3)
  quantizes later; a note is written when it ends (stop, mode change and project change end it too). A take (from
  Rec on) is one undo. Recording into a track with no playing clip starts its edited clip at the current bar.
- **The last notes** (step entry's memory) are not saved with the project; a new chord starts when a pad is
  pressed with no other pad held (pads pressed while a step is held edit the step instead).
- **The count-in** is a bar of the internal clock's ticks from the Play press (Rec armed) (T 0 exactly a bar later); following an
  external clock there is none (the start waits for its next bar, as Play does).
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
rfm_move_screen_timed(&m);                        /* 1: draw again as the clock runs (a popup up, playing) */
rfm_move_release_all(&m);                         /* the link dropped, or the host leaves Move control */
rfm_move_leds_invalidate(&m);                     /* a new connection: repaint */
rfm_move_set_master(&m, v); rfm_move_set_label(&m, enc, "CUTOFF"); rfm_move_set_keymap(&m, km);
rfm_move_save(&m.s, bytes); rfm_move_load(&s, bytes, n);   /* "RMV" + version 3, 80 bytes (versions 2 and 1, 78 and 77 bytes, load) */
```

`rfm_move_t` is about 150 KB with the sequencer: keep it static or on the heap. The sequencer:

```c
rfm_move_seq_advance(&m, frames, rate);           /* every audio block, at its start, playing or not (rate 1000000: us) */
rfm_move_seq_midi_in(&m, msg, n);                 /* the firmware's MIDI out realtime: F8 FA FB FC, F2 lsb msb */
rfm_move_seq_play(&m, on); rfm_move_seq_set_bpm(&m, bpm10); rfm_move_seq_set_clock(&m, RFM_MOVE_CLOCK_AUTO);
rfm_move_seq_bpm10(&m); rfm_move_seq_following(&m);
n = rfm_move_project_save(&m, out, cap);          /* export: <= RFM_MOVE_PROJECT_MAX (39072) bytes, -1 cap too small */
rfm_move_project_check(blob, n);                  /* import: 0 valid */
rfm_move_project_load(&m, slot, blob, n);         /* PROJECT_LOAD's answer (also at start); NULL/0 = empty; < 0 invalid */
rfm_move_project_dir(&m, used_bits); rfm_move_project_flush(&m); rfm_move_project_slot(&m);
rfm_move_project_info(&m, slot, blob, n);         /* a stored project's content, for the grid's bars and its popup */
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

## The screen

The owner's design (`Move Screen UI for RemoteFM1`: one HTML mockup a screen at 4x, the geometry in native pixels,
followed exactly). The Move's screen is small, so **at rest every screen is symbols and pictures only**; words come
in large popups (font5x7 at 2x, numbers at 4x) while a knob is touched or turned, a step or a project pad is held, and
for `RFM_MOVE_POPUP_MS` (1 s) after the release or the last turn. The popups time out on the sequencer's clock
(`rfm_move_seq_advance`, which hosts call in every mode); `rfm_move_screen_timed` says when the screen changes with
the clock alone (a popup up, playing), so the host draws it again then.

| Screen | What it draws |
|---|---|
| FM-1 mode | left: the owner's 64 × 64 drawing of the FM-1, pixel for pixel, live (below); right: the FM-1's screen, 64 × 64 black and white (a luminance threshold, `RFM_MOVE_THUMB_LUMA`; no dither) |
| MIDI mode | the 8 × 4 pad map (14 × 14 cells, 16 px apart): the root filled, in the scale outlined, out of it a 2 × 2 dot, a pressed pad filled with a 6 × 6 black square |
| Control mode | the 4 banks as tabs (the current one filled), the 8 values as full-height bars |
| Track view | the clip's pages on top (the shown one filled, in the loop outlined, past it dotted; 4 segments, 8 or 16 thinner ones past 4 or 8 pages), the page's notes as a piano roll (8 px a step, 3 px a semitone up from the page's lowest note, squeezed past 13 semitones; the beats dotted; the playhead a 1 px line), the 16 steps at the bottom (notes or the playhead filled, empty outlined, past the clip's end dotted) |
| Session view | the owner's mockup (Session.dc.html): the 8 tracks in view across, their 4 clip slots down (14 × 13 cells, 16 px apart, from y 4), as the pads are; a 14 × 2 bar over the selected track (muted: a dotted line); a clip empty (a 2 × 2 dot), with content (an outline), playing (filled, a black ▶), queued (a blinking outline), stopping (blinking), recording (filled, a black ●; blinking while it waits); copied dashed, cut crossed |
| Project grid | the 32 slots as the pads are: the current one filled, a saved one outlined, each with three bars (its tracks with notes in three groups, a bar's height the bits of their note count, at most 8), an empty slot a dot; a project loading blinks |

**FM-1 mode's drawing, live.** The drawing stays as drawn (its mini screen too) but for:

- **encoders**: the touched knob's 2 × 2 dot inverted in a 6 × 6 block. The left 2 × 2 are MASTER, SELECT (top) and
  PRESETS, ALGO (bottom), as on the FM-1 (and the web player's panel); the row of 4 over the buttons K1..K4. So:
  the volume knob MASTER, knob 1 PRESETS, knob 2 ALGO, knobs 5..8 K1..K4, the jog wheel SELECT (lit while touched,
  or a second after a turn). In FM-1 mode a touch alone marks the drawing; a turn brings the knob popup.
- **buttons**: a cell filled while the FM-1's LED for it is at `RFM_MOVE_SCREEN_LIT` (64) or more, or the button is
  held. The 6 × 2 grid in the panel's order, FX SEL ENV LFO EDIT GLO over HOME SAVE ARP SEQ PLAY REC; OCT- and OCT+
  are the box of two at the left (its 3 × 1 slots).
- **keys**: a held key's 2 × 4 block goes dark. The bottom row's 16 blocks are the 16 white keys (key 0 F3 .. key
  26 G5); the top row's 15 blocks sit in the gaps between them, and each black key is the block in the gap after its
  white key (F# after F ...); the gaps B-C and E-F have no key, their blocks stay as drawn.

**Popups** (a 1 px white frame over the screen):

| Popup | When | What |
|---|---|---|
| knob | a knob touched or turned: FM-1 encoders (in MIDI mode on a touch, in FM-1 mode on a turn), Control / Sequencer CCs, the volume knob; Sequencer mode: Shift + jog, Loop held, the jog's bank; a drum pad + jog | the label at 2x (the firmware's label, else SELECT .. PRESETS; `CC 16`; MASTER; TEMPO, LOOP, BANK, DRUM), the value at 2x with its unit small (% BPM BARS LOCK ..), a thick bar, a dial with its pointer; the right half hatched behind. An FM-1 encoder is relative: its value is this gesture's detents (+3), the bar from the middle, the dial turning 15° a detent |
| step | a step held 300 ms or more in track view (a tap: none) | its notes as inverted chips (the first two and "+n" past three), the velocity (a level) and the length in steps (a bracket) at 2x; the steps row stays below, the held step marked over it |
| trig | a knob touched or turned on the TRIG page | a picture (micro: a note in a step's box; ratchet: the hits; chance: a die; condition: the passes, the A-th filled; function: an arrow, a die, hops; length: a gate bar over steps) and the name and value at 2x |
| loop | Loop held or latched | the length in bars (+ steps) at 2x, the loop as a bar over the 16 pages, the pages (in the loop filled, the shown one marked) |
| bar | Left / Right adding or taking away a ghost page | "BAR n", 1 s |
| Shift menus | Shift + step 3, 5, 7, 8, 9, 11; Shift + Record (stay open: the same again or Back) | Workflow: CH, RATE, QUANT; Tempo: the BPM at 4x (EXT and a padlock when locked); Groove: two 16ths, the second late; Layout, Keys & Scales: tabs and a keyboard; Arp: the pattern's walk, its name, rate, mode and range; Record: the mode's picture and name, the style |
| mute | Mute held | the 16 tracks, muted crossed out |
| project | a project pad held in the grid (also once that project is open: the grid closes on the load) | its number at 4x, a bar a track (its notes; 16 thinner bars when tracks 9..16 have notes), ▶ the tempo, the clip symbol and the clips |
| message | an action's result (UNDO, CLOCK AUTO, FULL VEL, CLICK ON ..; not copy / cut / paste), 1 s | the words at 2x, a line or two |
| count-in | Play with Rec armed (Record mode), from stopped | the beats to go at 4x, a recording dot |

The newer of a control's popup and a message shows; the count-in shows under both. The core knows the current
project's content; for the others the host passes each stored blob once (`rfm_move_project_info`: at start and after
an import; saves, loads and deletes keep it up to date). A project it was not given is drawn outlined, without bars.

**Held Track buttons** fill the screen with their menu, in the same language (Track 1 has none: FM-1 mode stays):

- **Track 2**: the 4 layouts as tabs (the top row's pads 1..4), the 8 scales (row 2's pads), the root as a keyboard
  octave (the jog wheel), the 16 channels as the steps; the setting changed last at 2x (`C MIN`, `DRUM`, `CH 5`).
- **Track 3**: the bank's tabs, the 8 knobs (the one turned filled), its CC number at 2x (`CC 20`).
- **Track 4** (Sequencer): the 16 tracks as the steps (selected filled, with content outlined, empty dotted, muted
  flagged above), the selected track's channel as a strip of 16 (the jog wheel), `TR 3` or `CH 5` at 2x.

**Where it departs from the mockups**: the mockups' Silkscreen text is font5x7 at 2x (labels of 8 characters at
11 px a character, more cut to 8); in FM-1 mode a touch alone marks the encoder on the drawing and the popup comes
with the turn (it would otherwise cover the mark), and the jog wheel (SELECT) gets no popup, so the FM-1's screen
stays in view while its menus are scrolled; the step popup shows the first note's velocity and length (offsets and
CC locks are not on the screen any more; a held step's lock shows in the knob popup as `LOCK`); muted tracks, the
stopping and waiting states and the 16-page clips are drawn in the design's symbols, where the mockups had none.

## Tests

`host/run_tests.sh` runs `move` (the mapping against the design's labels, every button and knob, MASTER's
clamping, mode changes and release-all, the four layouts' notes, drum editing and the settings round trip, the CC
banks and reassignment, LED diffing; the screen: FM-1 mode at rest exactly the owner's drawing (the PNG's rows), the
live encoders, buttons and keys, the popups' life (held, then a second), ten golden screens: their PBMs land in
`host/build-asan/move_*.pbm`) and
`move_e2e`: Move Remote's simulator (`make sim` in an abletonmoveremote checkout; found beside this repo, or
`-DRFM_MOVEREMOTE_SIM=`, or `$MOVEREMOTE_SIM`) gets presses through its test FIFO, and the test checks the actions,
the LED writes, and the simulator's screen shot against the frame sent, then a short sequencer run (Track 4, two
steps, Play on the internal clock: the notes as MIDI actions at their frames, the step LEDs, the screen). Without the
simulator it is skipped.

`move_seq` (test_move_seq.c): the data model, step entry (held chords, the last notes, the root, a tap clearing,
the transport starting on the first note), the held step's nudge / transpose / velocity / length and its pad colours
(written to `move_seq_held_pads.ppm`), Loop + jog, Move's copy (release, then target), Copy and Delete alone,
polyphony, lengths and pages, playback at exact
frames (48 kHz, 44.1 kHz, microseconds), the external clock (start, stop, continue from a song position, Play at its
next bar, the timeout), quantized launches and scenes, mutes and every path that ends notes, recording (the count-in,
quantize, micro offsets, growing clips), Capture (Shift + Capture, Shift + Play's retrigger), copy / cut / paste / undo / redo and the journal's overflow, CC locks, scale
and root, the project grid's actions (sync and async answers, a failed load, delete), the autosave's debounce, the
blob round trip and a fuzzed loader (every truncation, 3000 random blobs, 20000 mutations with the check fixed up),
the project info for the grid (from blobs, a broken one refused), the popups' life (a held step, Loop, tempo, CCs,
a lock, messages over popups), and eight golden screens (track view, the step popup, session, project grid, the
project popup, the Track 4 menu, the count-in, a message: `move_seq_*.pbm` in the build directory).

## Later phases

- **FM1VST (Mac)**: a `MoveLink` beside `RemoteLink.cpp`: `mr_connect` / `mr_poll` on a thread, the actions into
  fm1panel_t, the LEDs and screen out each frame, sound on the Mac or (optional) the Move.
- **iOS (RFMKit)** and **Android (NDK)**: compile `move.c` and the vendored client as they compile libremotefm1.
- **The web player**: done over WebSocket (WEB-PLAN.md, "An Ableton Move as the controller": `web/move/build.sh`,
  `web/site/js/move-core.js`, `move-link.js`; `mr_client.c` compiles under Emscripten and the linker drops its
  sockets). Still to come: a way past https → `ws://` (docs/REMOTEFM1.md upstream lists the options).
- **The sequencer in the web player**: done (below, "The web player").

## The web player

`web/move/src/move_web.c` exports the sequencer (`mw_seq_*`, `mw_project_*`, `mw_act_frames`, `mw_blob`);
`web/site/js/move-core.js` wraps it (kinds 8..10 as `projectSave` / `projectLoad` / `projectDelete`, every action
with its `frame`).

**What the host does there.** The core stays on the page's main thread (the pads, LEDs and screen need it), so
`js/move-seq.js` (SeqClock) advances it ahead of the audio: every 10 ms and before each Move input, in blocks of 128
frames at the context's rate, up to the AudioContext's frame plus 30 ms (`LEAD_S`). An action at `frame` in a block
starting at absolute frame `pos` goes to the worklet as `{type: "midi", bytes, at: pos + frame}`; the worklet
(`web/site/worklet/host.js`, TimedMidi) gives it to the engine before the render of the quantum nearest to `at`, so a
note lands within half a quantum (64 frames) of its frame, and the core only ever counts the frames it is given: no
drift. Measured (move-e2e, Felucca at 44.1 kHz, 8 bars of 16ths at 120 BPM): every note in the engine within 63.5
frames of its ideal, mean 0.3, none late. A gap over 250 ms (a frozen tab, a suspended context) is skipped, not caught
up. Without running audio the wall clock stands in (MIDI at once). The sequencer stopping, a disconnect, a lost link
and pagehide cut the worklet's queue (`midiCut`: queued note offs now, note ons dropped); a note off given now drops a
queued note on of the same note. Live pads are not delayed.

**The firmware's MIDI clock.** The worklet posts the firmware's MIDI out with the frame of the quantum that made it;
the page takes F8 FA FB FC and F2 out of it (SysEx skipped) and feeds each to `rfm_move_seq_midi_in` at that frame
plus the lead, in the core's timeline. In AUTO the sequencer then follows the firmware's tempo, start and stop, its
notes the lead (30 ms) behind the firmware's own beat, steadily; 500 ms without a clock and it is on its own BPM again.
The Move section of the page says which (`clock INT` / `from the firmware`, and the BPM). Recorded pad notes land the
lead late too (the core runs that far ahead of what is heard); quantize takes it out.

| Firmware (web player) | Sends MIDI clock? |
|---|---|
| Hub firmwares on the emulated engine (fm1-emu) | no: fm1-emu models no MIDI out |
| Felucca (native) | no: receives clock and transport only (`midi_clock.c`); checked headless: PLAY, SEQ, ARP and keys for ~13 s of device time, no F8/FA/FC out |
| Melodee (native) | no: receive only, as Felucca |
| ChoralRoot (native) | yes, by default (Options > MIDI Clock: OUT, `cr_out.c`): F8 at its BPM all the time, FA / FC when its loop starts and stops; none with MIDI Clock IN or OFF |
| FIZZ (.rfm1) | yes while its sequencer plays on its own clock (SETTINGS CLOCK OUT, on by default): FA, F8, FC (`core/app/app.c` mo_clock) |
| fm1-base apps | only if the app sends it: the base's clock code (keremimo's, felucca-bsp `chip/usb.c`) is receive and transport sync |

**Projects.** IndexedDB `remotefm1-move`, store `projects`, a record a slot `{ slot, bytes, saved }`
(`js/move-projects.js`), apart from the firmwares' saves. At start the store's slots go to `rfm_move_project_dir`
and the last project opened (prefs `moveProject`) to `rfm_move_project_load`; PROJECT_SAVE / DELETE are written in
order, PROJECT_LOAD is answered from the store; the autosave is flushed on disconnect and pagehide. The page's Move
section lists the projects ("Project N": the blob has no name), with their size and date, Backup, Delete, Back up all
and Import (into a picked slot or the first free one; a set into its own slots).

The file (`.rfmmove` one project, `.rfmmoveset` several), little-endian:

| Bytes | Field |
|---|---|
| 4 | `RMVP` |
| 1 | version, 1 |
| 1 | kind: 1 one project, 2 a set |
| 2 | count, 1..32 (1 for kind 1) |
| per entry: 1 | slot 0..31 (unique) |
| 1 | flags, 0 |
| 4 | saved, Unix seconds (0 unknown) |
| 4 | len, 1..39072 |
| len | the core's blob (`RMSQ`, as `rfm_move_project_save` writes it) |

Nothing may follow the last entry; a file over 8 + 32 x (10 + 39072) bytes is not read; every blob must pass
`rfm_move_project_check`, or nothing is imported.
