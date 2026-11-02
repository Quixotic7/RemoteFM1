# The firmware's own button names

A firmware names the FM-1's 14 buttons itself: ChoralRoot calls SEL "KEY", ARP "PERF", PLAY "LOOP"... The printed
faceplate (FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+) is the same for every firmware. A host that
draws the panel should show the running firmware's names, with the printed name small beside a renamed button.

## Where the names live

`core-api/fm1core.h` (FM1VST), the core descriptor `fm1core_t`:

```c
const char *button_names[FM1P_NB];   /* FM1P_NB == 14, in FM1P_B_* order */
```

Order: `FM1P_B_FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, OCT_DN, OCT_UP` (0..13). An entry may
be NULL or "": both mean "no name of its own", use the printed name.

### Native cores (FM1VST, any host loading a core library)

Read `desc->button_names[i]`, i = 0..13, from the descriptor the core's entry point returns, after the ABI check. The
strings are the core's static data: copy them, never free them.

### .rfm1 modules (FM1VST docs/RFM1-FORMAT.md, sections 2 and 5)

`rfm1_get(6)` returns the descriptor's address in linear memory; `rfm1_layout(i)` gives each field's byte offset.
`button_names` is 14 wasm32 pointers (4 bytes each, little-endian) at `desc + rfm1_layout(<button_names index>)`
(offset 36 under ABI 6; web/engines/rfm1/engine.js `C.button_names`). Each non-zero pointer is a NUL-terminated
UTF-8 string in the module's memory: check it lies inside memory and cap its length before reading.

### .fm1web (the website's native engine)

`web/engines/native/wasm_glue.c`: the string getter returns `button_names[k - 100]` for keys 100..113 (NULL: none).
`engine.info().buttonNames` (web/engine/API.md) carries the names to the page for every engine; fm1-emu has no
descriptor and gives the printed names.

## Drawing them (the website, web/site/js/panel.js)

- `buttonLabels(names, use)`: per button, the firmware's name trimmed, else the printed one; all printed when the
  array is missing or not 14 long, or when the player's "Firmware's button names" setting (on by default) is off.
- The button face shows that name upper-cased; where it differs from the printed one, the printed name is drawn
  small above the top row / below the bottom row, as on the faceplate (OCT-/OCT+ never get one).
- A long name shrinks to fit the cap (down to half size), then is cut with an ellipsis.
- The OMX-27 views (the AUX button, the button keys' names) use the same names.

## For RemoteFM1 for iOS (RemoteFM1IOS) and FM1VST

- FM1VST (PanelComponent): take `button_names` from the loaded core (native, or .rfm1 through its WASM host) when
  it loads; fall back per entry as above; redraw on a core change.
- RemoteFM1IOS (Faceplate / PanelButton): the app drives a unit, not a core, so it has no descriptor; it can show a
  firmware's names when the unit's firmware is known (names sent by the firmware, or a table by firmware id), else
  the printed ones. Same fallback, fit rules and setting.

## Live labels (what a control does now)

`button_names` is read once, at load. A firmware can also say, while it plays, what every control does right now
(SHIFT held, a page, a mode): FM1VST's `docs/DYNAMIC-LABELS.md` is the spec. No ABI change: old cores and hosts are
unaffected.

### The model: 49 slots (FM1VST fm1core.h `FM1CORE_LBL_*`)

| Slots | Controls |
| --- | --- |
| 0..7 | the knobs by role, `FM1P_E_*`: SELECT, ALGORITHM, PRESETS, KNOB 1..4, MASTER |
| 8..21 | the 14 buttons, `FM1P_B_*` (FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+) |
| 22..37 | the white keys W1..W16 (F3 G3 A3 B3 C4 .. G5) |
| 38..48 | the black keys B1..B11 (F#3 .. F#5) |

A label is 0..10 upper case printable ASCII characters. `""` = **the control has no job here** (drawn dim);
NULL = **not given**, the host's own: a button falls back to `button_names`, then the printed name; a knob to its
printed role name; a key shows nothing. It is the same model as FIZZ's SysEx LABELS reply (FMToneSeq
`docs/SYSEX.md`, "`20` LABELS"; `tools/fizz_sysex.mjs parseLabels`), which has no NULL: so the Remote FM-1 path can
later feed FIZZ's replies (every entry a string) into the same merge.

### Where they come from

- **Native cores**: two optional module symbols, found as `fm1core_latency_frames` is (dlsym, `LoadedCore::symbol`):
  `uint32_t fm1core_labels_epoch(void)` (moves on any change; 0 = no live labels) and
  `const char *fm1core_label(int32_t which)`. Clock thread; copy the string before the next call into the core.
- **.rfm1**: two optional exports, `rfm1_labels_epoch () -> i32` and `rfm1_label (which) -> ptr` (a C string in
  memory, 0 = NULL). Every .rfm1 built with FM1VST's `rfm1_shim.c` after the change has them (epoch 0 for a core
  without the symbols); older files don't: treat a missing export as "no live labels". Bounds-check the pointer and
  cap the length.
- **.fm1web**: `wasm_glue.c` `fmw_labels_epoch()` and `fmw_str(200 + which)`, from weak references; `build.sh` links
  with `-Wl,-u` for both symbols so a core library's member defining them is pulled in.

### The host's loop

After `frame()` (or at the UI's rate; 30 Hz is plenty) read the epoch. When it differs from the last one read (the
first read counts), read all 49 labels and redraw; epoch 0 = draw as without live labels. Per control: a live
string wins; `""` = the static name dimmed (a key: nothing); NULL = the static fallback above. The "Firmware's
button names" setting off: everything printed, live labels too. The printed name stays small beside a renamed
button, as for `button_names`.

### The website (done)

`engine.labels()` (web/engine/API.md: rfm1, native; emu and mock give null) -> the worklet's `LabelWatch` posts
`{type: "labels", seq, list}` when the epoch moves -> `main.js` keeps `state.labels` and `panelLabels(buttonNames,
labels, fwNames)` (panel.js) gives the buttons (dim where `""`), the knobs' legends and small labels at the keys'
feet, and the OMX-27 views' names. Tests: `web/site/test.mjs` (the merge, a fake engine), `web/engines/rfm1/
labels-test.mjs` (a real .rfm1 built with the shim: fixtures/build_labels.sh), `web/engines/native/labels-test.mjs`,
`web/site/tools/labels-e2e.mjs` (headless: GLO held relabels the drawn panel).

### To do elsewhere

- **FM1VST (the plugin)**: `LoadedCore::symbol(FM1CORE_LABELS_EPOCH_SYMBOL / FM1CORE_LABEL_SYMBOL)` for a native
  core, the two exports through its WASM host for an .rfm1; poll the epoch at the editor's timer; PanelComponent
  draws the merge (buttons, knob legends; keys if it labels them).
- **RemoteFM1IOS**: its .rfm1 runner (wasm3) calls the two optional exports when present (format version stays 1:
  `docs/RFM1-FORMAT.md` section 2 needs the two rows, in the original first, then FM1VST's copy); Faceplate /
  PanelButton draw the merge. For a unit over USB, FIZZ's SysEx LABELS replies can feed the same merge.
- **RemoteFM1Android**: the same as iOS, in its runner and panel views.
