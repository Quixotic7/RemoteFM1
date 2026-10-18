# RemoteFM1 site: plan (2026-10-18)

A public site on RemoteFM1's GitHub Pages (`quixotic7.github.io/RemoteFM1`) where anyone can:

1. **play FM-1 firmwares in the browser**, with the computer keyboard, mouse, touch or a MIDI keyboard;
2. **install RemoteFM1** on their FM-1 from the page, over Web MIDI, with no tools;
3. **use their own FM-1 as the panel** of the firmware running in the browser, once it runs RemoteFM1. This is
   PLAN.md milestone M4.

Owner's decisions (2026-10-18): **both engines**, behind one picker; the site lives on **RemoteFM1's Pages**.

## 1. Two engines, one interface

| Engine | What it runs | Where the firmware comes from | Speed |
|---|---|---|---|
| **Native (ours)** | FM1VST's own cores, compiled to WebAssembly with Emscripten | built from source; only the **10 un-gated** cores, because a public site redistributes what it hosts: Felucca, GHOULBOX, JIANT FM, PurpleMonkey, CoralFlower, FoMni, FiMba-1, AMB-1, OberVave, Frodhes | fast (70–480× real time natively) |
| **Emulated** | [fm1-emu](https://github.com/joakimeriksson/fm1-emu) (MIT): the FM-1's chip, so any `.fwsc` runs unchanged | fetched at play time from each author's release, as fm1-emu's own page does, via the Hub's catalog; nothing is redistributed. Stock M-VAVE V15 only from the visitor's own file | 1.5–6× real time natively, less in the browser |

The picker lists each firmware once. It uses the native core where one exists and is un-gated; otherwise the
emulated engine. A firmware page can switch engines when both exist ("run on the real chip's code").

**The engine interface** (JavaScript, one per engine; the page never talks to an engine's internals):

```
engine.load(firmwareId | fwscBytes, savedFlash?) -> Promise
engine.panel(state)   // keys / buttons bitmasks, encoder detents, master 0..1023, key velocity; RemoteFM1 matrix order
engine.render(n)      // inside the AudioWorklet: n stereo frames at the context rate
engine.screen()       // 240×240 RGB565, plus a "changed" counter
engine.leds()         // 11×4 levels 0..255 + PLAY green (fm1panel_t's layout)
engine.midiIn(bytes) / engine.midiOut() -> bytes[]
engine.flash() -> Uint8Array, engine.flashDirty() -> counter   // saved per firmware and version in IndexedDB
engine.info()         // name, version, licence, source, credits (from the core descriptor / the Hub catalog)
```

Each engine runs inside an AudioWorklet (no SharedArrayBuffer: GitHub Pages can't send COOP/COEP headers). The
screen, LEDs and flash go to the page by `postMessage` at up to 30 Hz.

## 2. The page

- **The look:** FM1VST's new style. The FM-1 drawn edge to edge in the theme colours, Inter Tight, a corner "⋯"
  that opens a Graphite sheet. The firmware browser has search, category chips and cards from the Hub catalog. A
  screen-only view.
- **Input:** computer-keyboard keys as in FM1VST, mouse and touch on the drawn panel, and Web MIDI keyboards.
- **Saves:** per firmware and version in IndexedDB, with backup export and import (`.fm1save`-compatible if
  fm1-emu's format can be shared).
- **Use my FM-1:** connects to a unit running RemoteFM1, through Web MIDI everywhere it exists and WebUSB bulk in
  Chrome. libremotefm1 is compiled to WebAssembly (its portable core has no OS calls); the JS back ends replace
  CoreMIDI / libusb. The unit's panel then plays the browser firmware, and its screen and LEDs follow.
- **Credits:** every firmware's author, licence and source link. fm1-emu credited and linked on every emulated run.

## 3. Install RemoteFM1 (a page of the site)

- **Base:** TransitFM1's web installer (`web/fm1ota.js`, `fm1pkg.js`, GPL), the same M-UPGRADE protocol
  `fm1_install.py` uses.
- **The package:** RemoteFM1's own release package, served by the site (it's GPL and ours). A tagged release:
  **RemoteFM1 0.1**, built from the tested M1 firmware.
- **Before installing:** the page says what RemoteFM1 is, tells ChoralRoot users to take a backup first (with
  ChoralRoot's own page), and says how to go back to any other firmware (any installer works over RemoteFM1, which
  answers as V15).
- **The checks TransitFM1's installer already has:** identity, package CRC, and the update-mode resume after a cut.
  Plus TransitFM1 0.1.1's lessons: never start while a DAW floods the port; the 0.1.2 Baud Girl loader issue.
- **Tests:** `test_installer.mjs`-style, against a simulated FM-1, in CI.

## 4. Build and publish

- **Sources:** in `web/` on RemoteFM1's main.
  - FM1VST is a submodule, for the native cores' glue and firmware sources.
  - fm1-emu is a submodule, pinned.
- **A GitHub Actions workflow builds the site and deploys it to Pages:**
  - Emscripten builds the 10 native cores and libremotefm1;
  - rustup adds `wasm32-unknown-unknown` and builds fm1-emu's web crate;
  - it assembles `site/` and publishes it.
- **Locally:** Emscripten is in `~/emsdk`. Homebrew's Rust has no wasm32 standard library, so fm1-emu's web build
  needs either a local rustup install (the owner's OK) or the CI build only.
- **Turning Pages on:** the owner enables it once (repository Settings → Pages → Source: GitHub Actions); `gh` isn't
  installed here.

## 5. Work split (agents, disjoint files)

| Agent | Delivers |
|---|---|
| W1, native cores → WASM | an Emscripten build of the 10 un-gated cores behind the engine interface (Dinghy's coroutine on Emscripten fibers), a headless node test per core (boots, screen not blank, a key sounds) |
| W2, fm1-emu engine | fm1-emu's web crate behind the engine interface, the Hub catalog fetch, fetching packages at play time, stock V15 from a local file; built in CI (and locally if rustup is allowed) |
| W3, the site and the installer | the page, the panel, the AudioWorklet host, saves, Web MIDI input, the firmware browser, the RemoteFM1 installer page and its tests, the Pages workflow |
| W4, after W3: Use my FM-1 | libremotefm1 in WASM with Web MIDI / WebUSB back ends, wired to the page |

The orchestrator owns this plan and the engine interface, reviews each part, tries the site headless, and flashes
RemoteFM1 with the page's installer on the owner's unit (with their OK) before publishing.

## 6. Not on the site

- The 11 gated cores as native builds (most still run through the emulated engine, fetched from their authors).
- M-VAVE's firmware from M-VAVE (visitors bring their own file).
- Anything that writes to an FM-1 except the RemoteFM1 installer.
