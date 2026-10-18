# The web engine interface (WEB-PLAN.md section 1)

**Owned by the orchestrator.** Agents implement it exactly and report problems rather than editing it.

The site has two engines that run FM-1 firmwares in the browser:

| Engine | Code | Runs |
|---|---|---|
| **native** | `web/engines/native/`, agent W1 | FM1VST's own cores, compiled with Emscripten |
| **emu** | `web/engines/emu/`, agent W2 | fm1-emu's chip emulator |

The page (`web/site/`, agent W3) only ever talks to an engine through this interface, inside one AudioWorklet.
`web/remote/` (agent W4) connects a real FM-1 running RemoteFM1 to the page.

## 1. An engine module

An engine is an ES module that loads inside an `AudioWorkletGlobalScope`, so it can't use `fetch`, the DOM or
`importScripts`. The page fetches every byte it needs and passes it in.

```js
// web/engines/<name>/engine.js
export const ENGINE = { name: "native" | "emu", version: "x.y" };

// Create one instance. `files` holds everything the engine needs, fetched by the page:
//   native: { wasm: ArrayBuffer }       (the core's .wasm)
//   emu:    { wasm: ArrayBuffer, fwsc: Uint8Array }
export async function createEngine({ sampleRate, files, flash /* Uint8Array | null */, options /* {} */ }) -> Engine
```

## 2. The `Engine` object (every call synchronous, made from the worklet thread)

```js
engine.info()      -> { id, name, version, license, source, credits, family,
                        latencyFrames,   // frames the engine runs ahead (0 for native)
                        keymap,          // [6][11] matrix id at (row, column), or null for the FM-1 default
                        buttonNames }    // [14] the firmware's own labels, in the FM1P_B_* order

engine.setPanel(p) // p = { keys: uint32, buttons: uint32,   // bit k: note key k (0 = F3); bit i: matrix button i
                   //       enc: Int32Array(7),              // detents since the last call, matrix order, + = CW
                   //       master: 0..1023,
                   //       keyVel: Uint8Array(27),          // 0 = none given
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
The host processor imports `engines/native/engine.js` or `engines/emu/engine.js`.

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
| worklet → page | `{type: "halted", code}` |
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
two differ (FM1VST `nomansky` vs the Hub's `nmsfm`), the catalog maps them.
