# The web engine interface (WEB-PLAN.md section 1)

**Owned by the orchestrator.** Agents implement it exactly and report problems rather than editing it.

The site has three engines that run FM-1 firmwares in the browser:

| Engine | Code | Runs |
|---|---|---|
| **native** | `web/engines/native/`, agent W1 | FM1VST's own cores, compiled with Emscripten; and firmware authors' own builds of theirs (`docs/NATIVE-AUTHORS.md`) |
| **emu** | `web/engines/emu/`, agent W2 | fm1-emu's chip emulator |
| **rfm1** | `web/engines/rfm1/` | the visitor's own `.rfm1` files (section 6) |

The page (`web/site/`, agent W3) only ever talks to an engine through this interface, inside one AudioWorklet.
`web/remote/` (agent W4) connects a real FM-1 running RemoteFM1 to the page.

## 1. An engine module

An engine is an ES module that loads inside an `AudioWorkletGlobalScope`, so it can't use `fetch`, the DOM or
`importScripts`. The page fetches every byte it needs and passes it in.

```js
// web/engines/<name>/engine.js
export const ENGINE = { name: "native" | "emu" | "rfm1", version: "x.y" };

// Create one instance. `files` holds everything the engine needs, fetched by the page:
//   native: { wasm: ArrayBuffer, flags? }   (the core's .wasm; flags, for an author's build: its manifest's
//                                            { asyncify, abi }, checked against the module before it runs)
//   emu:    { wasm: ArrayBuffer, fwsc: Uint8Array }
//   rfm1:   { wasm: ArrayBuffer }        (the .rfm1 file itself, checked before it runs: section 6)
export async function createEngine({ sampleRate, files, flash /* Uint8Array | null */, options /* {} */ }) -> Engine
```

## 2. The `Engine` object (every call synchronous, made from the worklet thread)

```js
engine.info()      -> { id, name, version, license, source, credits, family,
                        latencyFrames,   // frames the engine runs ahead (0 for native)
                        keymap,          // [6][11] matrix id at (row, column), or null for the FM-1 default
                        buttonNames }    // [14] the firmware's own labels, in the FM1P_B_* order (FX SEL ENV LFO
                                         // EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+), or null; an entry may be
                                         // "" / null where the firmware gives none. native: fm1core_t.button_names
                                         // (wasm_glue string keys 100..113); rfm1: the descriptor's button_names
                                         // through rfm1_layout; emu: the printed names (no descriptor); mock: null.
                                         // The player (panel.js buttonLabels) draws the printed name for an empty
                                         // entry, and all printed with its "Firmware's button names" setting off.
                                         // docs/BUTTON-LABELS.md
engine.labels()    -> { seq, list, knobs, buttons, white, black } | null
                   //    the firmware's live panel labels (FM1VST docs/DYNAMIC-LABELS.md), or null: none (the engine
                   //    has no labels, the module was built before them, or the core's epoch is 0). seq: the core's
                   //    epoch (moves on any change); list: 49 entries in FM1CORE_LBL_* order (FIZZ's SysEx LABELS
                   //    order): 0..7 the knobs by role (SELECT ALGO PRESETS KNOB1..4 MASTER), 8..21 the buttons
                   //    (FM1P_B_*), 22..37 the white keys W1..W16, 38..48 the black keys B1..B11; each a string of
                   //    at most 10 printable ASCII characters ("" = no job here) or null (not given: the host's own).
                   //    knobs / buttons / white / black: list's slices. Cheap while seq stays (the same object).
                   //    rfm1: the optional exports rfm1_labels_epoch / rfm1_label; native: fmw_labels_epoch and
                   //    fmw_str keys 200..248 (wasm_glue.c, weak references; build.sh links with -u); emu, mock: null

engine.setPanel(p) // p = { keys: uint32, buttons: uint32,   // bit k: note key k (0 = F3); bit i: matrix button i
                   //       enc: Int32Array(7),              // detents since the last call, matrix order, + = CW
                   //       master: 0..1023,
                   //       keyVel: Uint8Array(27),          // 0 = none given
                   //       keyLean?: Uint8Array(27),        // a held key's pressure 0..100 (fm1core key_lean); the
                   //                                        // user's FM-1 with RemoteFM1 0.3.1 (TOUCH). native and
                   //                                        // rfm1 pass both to the core; emu ignores both
                   //       taps?: { keys, buttons } }       // went down and up since the last call
engine.render(left, right)  // fill both Float32Array(128) at sampleRate (one render quantum)
                            // the engine resamples from its 44.1 kHz internally
engine.screen()    -> { seq: number, pixels: Uint16Array(240*240) }
                   //    RGB565, the value as the panel shows it (host byte order, NOT the device's
                   //    big-endian bytes); a fresh copy or a stable view, valid until the next render
engine.leds()      -> { seq: number, levels: Uint8Array(44), playGreen: 0..255 }
                   //    levels[col * 4 + line], 0..255, fm1panel_t's layout
engine.midiIn(bytes: Uint8Array)           // raw MIDI bytes as from the FM-1's MIDI in (SysEx allowed)
engine.midiOut()   -> Uint8Array           // raw MIDI bytes the firmware sent since the last call (may be empty)
engine.flash()     -> { seq: number, bytes: Uint8Array | null }   // seq moves when the flash changed; bytes on request
engine.halted()    -> 0 | code             // after the firmware rebooted or entered UBOOT: the page reloads it
engine.dispose()
```

Matrix ids and orders are RemoteFM1's and FM1VST's (`proto/rfm1.h`, FM1VST `core-api/fm1core.h`):

- **Buttons:** 0 OCT-, 1 OCT+, 2 FX, 3 SEL, 4 ENV, 5 LFO, 6 EDIT, 7 GLO, 8 HOME, 9 SAVE, 10 ARP, 11 SEQ, 12 PLAY,
  13 REC.
- **Keys:** 0..26 = F3..G5.
- **Encoders:** matrix order; the default role order is SELECT, ALGO, KNOB1..4, PRESETS, but each engine maps
  roles itself.

## 3. Worklet ↔ page messages (owned by W3; W1 and W2 don't need them)

The page creates `new AudioWorkletNode(ctx, "fm1-host")` and sends `{type: "load", engine, files, flash, options}`.
The host processor imports `engines/native/engine.js`, `engines/emu/engine.js` or `engines/rfm1/engine.js`.

| Direction | Message |
|---|---|
| page → worklet | `{type: "panel", ...p}` on every input change |
| page → worklet | `{type: "midi", bytes}` |
| page → worklet | `{type: "flashReq"}` |
| worklet → page | `{type: "frame", seq, pixels}` (transferable), at most 30 Hz and only on change |
| worklet → page | `{type: "leds", levels, playGreen}` |
| worklet → page | `{type: "midiOut", bytes}` |
| worklet → page | `{type: "flash", seq, bytes}` |
| worklet → page | `{type: "info", ...}` |
| worklet → page | `{type: "labels", seq, list}` when `labels()`'s seq moves (none while it is null; `list: null`, seq 0, when it goes back to null). The page merges it over `info.buttonNames` (js/panel.js `panelLabels`) |
| worklet → page | `{type: "halted", code}` |
| page → worklet | `{type: "ain", on, latency}` live audio input on / off for an .rfm1 cart with "ain1" (section 6); `latency`: the input's own, in seconds |
| worklet → page | `{type: "ainLevel", peak, latency, missing}` while input is on, with the frames (30 Hz): the input's peak, the latency the cart was told |
| worklet → page | `{type: "error", message}` |

## 4. Headless testing

Every engine also loads in Node, with no AudioWorklet: `createEngine` + `render` in a loop. Each agent ships a
Node test, `web/engines/<name>/test.mjs`. For each firmware it:

- boots;
- checks the screen isn't blank within 3 s of rendered audio;
- presses a key (or the firmware's sound prelude) and checks the audio isn't silent;
- writes a PNG of the screen to `web/build/test/<engine>-<id>.png`.

## 5. Firmware ids and the catalog

`web/site/catalog.json` is built by W3. It has one entry per firmware: id (the Hub's id), name, version, author,
category, licence, source, which engines can run it, the native core's `.wasm` path if there is one, and how the
emu engine gets the package (the Hub's package URL). For the native engine the id equals FM1VST's core id; where the
two differ (FM1VST `nomansky` vs the Hub's `nmsfm`), the catalog maps them. An author's own native build
(`native.by: "author"`, its module under `engines/native/ext/<id>/<version>/`) takes the native slot when its version
is newer than FM1VST's core or the same (`nativeAlt` and `nativeWhy` name the other build); it is the same "native"
engine, and its saves are the firmware's native saves at that version.

## 6. The .rfm1 engine

An `.rfm1` file is a firmware's host build (FM1VST's fm1core, ABI 6) compiled to WebAssembly, with `rfm1_*` exports,
WASI-only imports and a JSON manifest in a custom section `rfm1` (the format: RemoteFM1IOS `docs/RFM1-FORMAT.md`,
version 1; RemoteFM1 for iOS runs the same files). Firmware releases carry them (FM1VST's `fm1_add_rfm1`; the set
at https://github.com/Quixotic7/RemoteFM1FW/releases/latest). The site hosts none: the visitor opens one from the
firmware browser's "Open a file..." or drops it on the page, and the page keeps it in IndexedDB by its SHA-256
(`web/site/js/library.js`, the browser's "Your firmware").

- `files`: `{ wasm }`, the file's bytes. `createEngine` throws, with the reason, for a file the format refuses (its
  section 7): the magic; the `rfm1` section and the manifest's required fields; imports other than
  `wasi_snapshot_preview1`'s `fd_write`, `fd_read`, `fd_close`, `fd_seek`, `clock_time_get`, `proc_exit` (each with
  its signature); a missing or mistyped export; a memory that is not the module's own, exported, fixed and at most
  32 MiB; a passive data segment; `rfm1_version() != 1`, `rfm1_get(6) == 0`; any `rfm1_layout` value other than
  fm1panel_t's and fm1core_t's wasm32 offsets; a descriptor whose id, name or version differ from the manifest's.
  `inspect(bytes)` (exported) makes the file's own checks alone and returns `{ manifest, credits, licenses }`.
- Boot as the format's section 7 step 4: `rfm1_flash_stage` (the saved flash, or 0, 0), `rfm1_options` with
  `"fm1core:staged"`, `rfm1_boot_options(-1, 0, -1)`, the panel's `ready` = 2, `rfm1_init(0)`. The clock, the panel
  writes, the LCD's byte swap and the resampler are the native engine's (`wasm_glue.c run_ms`, in JavaScript; the
  resampler and the MIDI packets are `web/engines/common/shared.js`, shared with it), so a file built from the same
  source as a native core sounds and draws the same, sample for sample.
- `info()` is the native engine's, with the manifest's `license` and `source` (its `source_url`), and also
  `authors`, `glueUrl` (the manifest's `glue_url`), `builtBy`, `licenseTexts` (the `rfm1.license.<n>` sections) and
  `credits` (`rfm1.credits`, else the descriptor's).
- `labels()`: the optional exports `rfm1_labels_epoch () -> i32` and `rfm1_label (which) -> ptr` (FM1VST's
  rfm1_shim.c adds both to every build; a file without them, or with other signatures, runs as before with labels()
  null; `inspect()` says `labels`). A label's pointer is bounds-checked, read up to 64 bytes and cut to 10 characters.
  Tests: `node web/engines/rfm1/labels-test.mjs` (fixtures/labels_core.rfm1, built by fixtures/build_labels.sh),
  `node web/engines/native/labels-test.mjs`, the page: `node web/site/tools/labels-e2e.mjs`.
- **Live audio input, capability `"ain1"`.** The manifest may list `"wants": [...]`, capability names the cart uses
  when the host has them (a host ignores names it doesn't know); a cart that can't run without one lists it in
  `"needs"` instead, and a host without it refuses the file (as with `fs1`; `needs` is a Fantasy CFW's, so with
  `"virtual": true`). `wants` must be a list of strings, else the file is refused. This site supports `ain1`.
  Two optional exports, called only when `ain1` is in `wants` or `needs` and the module has them (with these
  signatures; `inspect()` says `ain: "needs" | "wants" | null` and `ainExports`):
  - `rfm1_audio_in (const int16_t *stereo, uint32_t frames)`: interleaved stereo int16 at 44100 Hz (a mono source on
    both channels), in a block made with `rfm1_alloc` (as the output's). Called **immediately before**
    `rfm1_audio(out, frames)` for the same block with the same `frames`, so input frame i goes with output frame i.
    No call before a block means silence: the host makes none while input is off.
  - `rfm1_audio_in_info (uint32_t state, uint32_t latency_frames)`: `state` 0 no input (off, refused, unplugged),
    1 live; `latency_frames` the **round trip** at 44.1 kHz (0 unknown): a sound the cart outputs in output frame j
    comes back (speaker to mic, or an interface looped back) in input frame j + `latency_frames`. To line a take up
    with what the cart played, a cart takes input frame i as played at output frame i - `latency_frames`. Every host
    (web, iOS, Android, FM1VST) reports this same meaning: its output side, its input side and any buffering between
    them, as best it knows them. Called before the first `rfm1_audio_in`
    and when either changes (this site: the latency when it moves by 32 frames or more), and with (0, 0) when input
    goes off, the firmware changes or the instance is disposed.

  Order per 128-frame block while input is on: `rfm1_audio_in_info` (only on a change, at the start of a render),
  `rfm1_audio_in(in, 128)`, `rfm1_audio(out, 128)`. The page (js/ain-ui.js, js/audio.js `ainStart`) takes the
  visitor's input with `getUserMedia` (echo cancellation, noise suppression and gain control off) only after a click
  (or by itself when this firmware had it on and the browser already allows the microphone), into the host node's
  input; the worklet hands each quantum to the engine (`audioIn(on, latency)`, `audioInput(L, R)` before
  `render()`), which resamples it to 44.1 kHz (`shared.js AudioIn`, the output resampler's kernel) into a FIFO that
  starts with 256 frames of silence less what the output resampler already holds: the delay from the context's input
  to its output is then 256 frames at 44.1 kHz plus a frame or two (5.8 ms), fixed while input is on (the input is
  locked to the output's quanta: a quantum without input is silence, an extra one is dropped). `latency_frames` is
  that delay plus the capture's own (`MediaStreamTrack.getSettings().latency`) plus the context's output side
  (`baseLatency` + `outputLatency`; a value the browser doesn't tell counts as 0).
  The other engines (native, emu, mock) have no input. Tests: `node web/engines/rfm1/ain-test.mjs` (the manifest,
  the call order, silence when off, the info calls, an impulse's offset through the resamplers, missing and extra
  input; fixtures/ain_cart.c, built by fixtures/build_ain.sh), the page: `node web/site/tools/ain-e2e.mjs`
  (headless Chromium with a fake microphone).
- A trap ends the instance: `halted()` is then 0x100 | code after `proc_exit(code)`, 0x200 after any other trap;
  `render()` gives silence and the other calls do nothing (the flash stays readable, so the page can boot it again).
- Saves of an `.rfm1` firmware are the firmware's under engine `"rfm1"`, apart from its native and emulated ones.
- Its Node test: `node web/engines/rfm1/test.mjs [--dir DIR]` over the files in `web/build/rfm1/` (section 4's
  checks, a flash round trip, the reboot script, the render cost per quantum, refused files, and the audio against
  the same firmware's native core where `web/build/native/<id>.wasm` exists). The page end to end:
  `node web/site/tools/rfm1-e2e.mjs`.
