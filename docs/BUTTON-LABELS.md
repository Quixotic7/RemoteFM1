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
