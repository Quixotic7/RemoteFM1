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
| **Native (ours)** | FM1VST's own cores, compiled to WebAssembly with Emscripten | built from source; only the **21 un-gated** cores, because a public site redistributes what it hosts: ChoralRoot, Felucca, GHOULBOX, JIANT FM, PurpleMonkey, CoralFlower, Melodee, FoMni, FiMba-1, AMB-1, OberVave, Frodhes, LOOM-8, DUELL, Mini-Tone, FMini-1, Skiff, GR!MM, PIGEON, Tonitor, Speak37 (ChoralRoot and Melodee from 2026-10-19: their CZ-1 / Prophet-5 presets go out compiled into the firmware only, never as raw .syx: web/site/tools/no_raw_presets.mjs; LOOM-8 from 2026-10-21, with its factory 909 hi-hat, the owner's call; the other eight from 2026-10-22, DUELL's native core named "DUELL (unofficial build)" as FM1VST names it; Speak37's FM-1 build is not on the Hub, so it plays native only; PIGEON's native 0.4.1 stays its default under the Hub's 0.5.0 beta) | fast (70–480× real time natively) |
| **.rfm1** | a firmware's own code built to WebAssembly (FM1VST's `fm1_add_rfm1`), the format RemoteFM1 for iOS runs | the visitor's own files (Your firmware), and the site's own: FIZZ 0.12.0-beta.12 (2026-10-29), built from its release by `tools/rfm1/build_fizz.sh`, committed in `web/site/fw/` with its SHA-256 (`fw/SHA256SUMS`, checked by the deploy and the page); listed by `data/site-rfm1.json`. The Hub's FIZZ package is not emulated (it needs its data blob) | 37× real time in Node (FIZZ) |
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

- **The look:** RemoteFM1 for iOS's (2026-10-28; before, FM1VST's): the page is the faceplate, edge to edge in the
  theme's body colour, laid out like the app for the window's shape (a phone upright with a scrolling keybed, a phone on
  its side, a tablet or a desktop); raised caps with lit LEDs, knurled knobs, Chakra Petch / DM Sans / Unbounded; the
  app's five themes and "My theme" from five colours; a grabber over the faceplate and the header open the sheet, in
  the app's warm dark. The firmware browser has search, category chips and cards from the Hub catalog. A screen-only
  view.
- **Input:** computer-keyboard keys as in FM1VST, mouse and touch on the drawn panel, and Web MIDI keyboards.
  **MIDI learn** (`web/site/js/midi-map.js`, Settings > Playing > Knobs from MIDI > Learn): click a knob, MASTER or a
  button on the panel, then move a control on the MIDI device. A CC drives an encoder as a pot (absolute: the change
  since its last value, 1 step = 1 detent at Knob speed 1x) or as an endless encoder (relative: 64 ± n, two's
  complement, or sign bit with 1..63 up and 65..127 down); learn guesses the mode, and the sheet's list can change it.
  A CC on MASTER sets it 0..127 → 0..1023 with pickup: after a learn, a reload or a move by any other input, the pot
  waits until it reaches MASTER. A note or a CC (≥ 64 down) holds a button. A learned CC or note plays no key and
  doesn't reach the firmware's MIDI in. The mappings are global (not per firmware), in the page's prefs; MIDI comes
  back on by itself on the next visit when the browser already allows it. Tests: `web/site/test.mjs` and
  `web/site/tools/midi-learn-e2e.mjs` (a mocked "OMX-27" input).
- **An OMX-27 as the controller** (Settings > Use my OMX-27; `js/omx27.js` the wire, `js/omx27-views.js` the views,
  both without DOM, `js/omx-link.js` Web MIDI and the sheet): the OMX-27's REMOTE mode (its firmware's
  `SYSEX_SPEC.md`, branch `q7-2026-3`). Web MIDI with SysEx, the port named "OMX"; the MODE command `51 05 09` enters
  REMOTE mode (`52 02`; a MODE while already in it answers `52 03` then `52 02`, still connected). Its keys, encoder and
  pots drive the panel (source `SRC.OMX`); the page owns its 27 LEDs (`59` / `5A`, latched by `5B`, at most every
  30 ms, only changes) and its 128 x 32 screen (canvas text, 1-bit, SSD1306 pages rotated 180°, 7-bit chunks `5C`,
  latched by `5D`; one frame in flight on its `52 04` ACK, ≤ 15 fps, changed chunks only). Pots stream every 2-3 ms:
  the latest of each is taken every 10 ms. The views follow the owner's design (`design/RemoteFM1---OMX-27-Remote.json`,
  format in `design/OMX-27-Designer-FORMAT.md`): K1 Base / Seq / Keyboard / Quick Keys, K2 six themes (the mirrored keys'
  hue), K5 MASTER with midi-map.js's pickup, the encoder the selected knob. Shortcuts at half brightness, full when
  pressed or selected; keys that mirror the FM-1's are dark unless its LED is lit, at its level. While in REMOTE mode
  the port is skipped by the MIDI keyboard input and MIDI learn. Connected once, it connects by itself (MIDI allowed,
  the port there). Disconnect or closing the page clears the OMX; leaving REMOTE mode is AUX + encoder hold on it.
  Tests: `web/site/test.mjs` and `web/site/tools/omx-e2e.mjs` (a fake OMX-27, `tools/omx-fake.js`; pictures of its
  LEDs and screen per view in `web/build/test/omx-*.png`).
- **An Ableton Move as the controller** (Settings > Use my Ableton Move; `js/move-core.js` rfm_move in WebAssembly,
  `js/move-link.js` the protocol, the link and the sheet): a Move running [Move Remote](https://github.com/Quixotic7/abletonmoveremote)
  (its `docs/PROTOCOL.md`: `u16 len | u8 type | payload`, a WebSocket at `ws://<move>:7720/ws`). The mapping is the
  portable C core every host shares, [rfm1cs-abletonmove](https://github.com/Quixotic7/rfm1cs-abletonmove) (the
  `web/vendor/rfm1cs-abletonmove` submodule, pinned to a tag; its README has the tables: Track 1..4 = FM-1, MIDI,
  Control, Sequencer modes; `js/move-core.js` and `js/move-seq.js` re-export its `web/` files), built by `web/move/build.sh` (its `web/build.sh`) into `web/build/move/rfm_move.mjs` (a single-file ES module,
  ~45 KB, never committed; the Pages workflow builds it with the remote module; catalog `paths.move`). HELLO (proto 1),
  then CONFIG audio clock off (the page plays its own sound); INPUT records go through the core into actions on the
  panel (source `SRC.MOVE`, bit 256: the holds are 16-bit), MASTER through `mapAct`, MIDI into the firmware's MIDI in;
  the FM-1's LEDs go out as the core's changed LED writes only (`LED_RGB` / `LED`), the screen (the mode, the last
  control, a 64 x 64 thumbnail of the FM-1's) at most ~30 a second and only when it changed (`DISPLAY`); the firmware's
  live knob labels name the knobs on it. PING every second, nothing heard for 3 s is a lost link: everything the Move
  held is let go and it connects again (1, 2, 4 s); `BYE` for the exit gesture or another host ends it. The settings
  (mode, layout, channel, octave, CC banks, drum notes: `rfm_move_save`) and the address are kept in the page's prefs;
  connected once, it connects by itself on the next visit. The link is a transport interface (`connect`, `send`,
  `onmessage`, `onclose`, `close`, `name`): `MoveWebSocket` and `MoveBridge`. **https:** from the published site the
  `ws://` is still tried first, in the Connect click: Chrome's Local Network Access lets it through to `.local` names and
  private addresses (it may ask once to allow local network devices; the socket waits for the answer, and after 4 s
  the bridge is offered beside it). Browsers that refuse it (Safari, Firefox, Edge) throw `SecurityError` there, and the
  same click opens Move Remote 0.1.1's bridge window (`http://<move>:7720/bridge`, a popup that opens the `ws://`
  itself and relays it over postMessage, bridge protocol v1); a failed or unanswered direct try offers it on an "Open
  the bridge window" button (popups only from a click). The window is reused on reconnects, closing it is a link close,
  Disconnect closes it; no hello in 5 s means a Move Remote older than 0.1.1. Tests: `web/site/test.mjs` (framing, the wasm core against
  rfm1cs-abletonmove's `tests/test_move.c` answers, `MoveLink` on a fake transport and clock) and `web/site/tools/move-e2e.mjs`
  (headless Chromium against Move Remote's own simulator, `moveremote-sim`, driven through its test FIFO).
  **The sequencer (Track 4)** runs in the page's core and is timed by the audio clock (`js/move-seq.js` SeqClock): every
  10 ms and before each Move input the core is advanced in 128-frame blocks up to the AudioContext's frame plus a
  30 ms lead; each action's frame becomes an absolute context frame, and the worklet (`worklet/host.js` TimedMidi,
  `{type: "midi", at}`) hands it to the engine before the render of the quantum nearest to it. Measured in
  `move-e2e` (Felucca, 44.1 kHz, 8 bars of 16ths at 120 BPM, 132 notes): the schedule exact (0.5 frames off the ideal:
  5512.5 frames a 16th), each note in the engine within 63.5 frames of its ideal (half a quantum; mean 0.3), none late,
  no drift. A stop, a disconnect, a lost link and pagehide cut the worklet's queue (`midiCut`: its note offs now, its
  note ons dropped) and let every note go. The firmware's MIDI out reaches the page with its quantum's frame
  (`midiOut` `frame`); its F8 / FA / FB / FC / F2 go to `rfm_move_seq_midi_in` at that frame plus the lead, so in AUTO
  the sequencer follows the firmware's tempo and transport (its notes the lead, 30 ms, after the firmware's beat), and
  goes back to its own BPM 500 ms after the last clock. The Move section shows the clock source (INT / from the
  firmware) and the tempo. Which firmwares send clock: rfm1cs-abletonmove's README.md, "The web player". **Projects:** the 32
  slots in IndexedDB `remotefm1-move` (`js/move-projects.js`; not the firmwares' saves), the core's PROJECT_SAVE /
  LOAD / DELETE answered there, the last project (prefs `moveProject`) opened at start, the autosave flushed on
  disconnect and pagehide; the section lists them (size, date) with Backup (`.rfmmove`; all: `.rfmmoveset`), Import
  (into a picked or the first free slot; every project checked by `rfm_move_project_check`, a bad file refused with
  the reason) and Delete.
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
  **RemoteFM1 0.1.1** (0.1, the M1 firmware tested on the unit, plus TransitFM1 0.1.3's refusal of a package past
  the app area; web/installer/firmware/RELEASE-NOTES.md). 0.1's package is not published.
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
