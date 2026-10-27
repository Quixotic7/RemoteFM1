# Use my FM-1 (web/remote)

PLAN.md milestone M4 in the browser. An FM-1 running RemoteFM1 becomes the panel of the firmware that runs in
the page. Its keys, buttons, encoders and MASTER go in, and the page's screen and LEDs come out on the unit.

The protocol is libremotefm1's portable core, compiled to WebAssembly: `host/src/{codec,screen,panel,session}.c`,
unchanged, the same code fm1remote runs on macOS. `remote.js` is its back end, in place of CoreMIDI and libusb.

| File | What |
|---|---|
| `remote.js` | `RemoteFM1`, the class the page uses: the Web MIDI and WebUSB back ends, the timer, the panel |
| `src/rfm_web.c` | the small C API over `rfm_session_t` (`rw_*`), and the callbacks into JS |
| `src/fake_web.c` | test-only: `host/src/fake.c`, the fake unit, as `fk_*` |
| `build.sh` | Emscripten build into `web/build/remote/`: `remotefm1.mjs` (the site's, ~55 KB, wasm inlined) and `fakeunit.mjs` (tests) |
| `test/test_remote.mjs` | Node: remote.js + the WASM session against WASM fake units through mocked Web MIDI / WebUSB, on a simulated clock |
| `test/page.html`, `page_test.mjs`, `run_page_test.mjs` | the same in headless Chromium, with remote.js on every default (navigator, timers, clock, module URL) |
| `test/fakeworld.mjs` | the mocks: MIDI ports and a WebUSB device around fake units, other MIDI devices that must get nothing |

## Build and test

    web/remote/build.sh                       # needs emcc: ~/emsdk is sourced when emcc isn't on PATH
    node web/remote/test/test_remote.mjs      # 68 checks; RFM_VERBOSE=1 prints the session's log
    node web/remote/test/run_page_test.mjs    # headless Chromium; screenshot in web/build/test/remote-page.png

No test touches hardware. The page test replaces `navigator.requestMIDIAccess` and `navigator.usb` with the mocks
before remote.js runs.

## How it finds the unit

- **Web MIDI** (`requestMIDIAccess({sysex: true})`). A port is a candidate if its name contains "FM-1" or
  "RemoteFM1" and doesn't contain "update". Inputs and outputs are paired by name. Each candidate gets HELLO, and
  only a "RemoteFM1" proto 2 answer counts. The name proves nothing: macOS often keeps a stale "ChoralRoot FM-1".
  No other MIDI device is ever sent a byte; the tests check this with an IAC bus, a KeyStep and an updater port.
  RemoteFM1 0.2 over Bluetooth MIDI is found the same way: once it is connected (on a Mac, Audio MIDI Setup's
  Bluetooth panel), its port is named after what it advertises, "FM-1 1F2D", and gets HELLO like the others.
- **WebUSB** (Chrome). `requestDevice({filters: [{vendorId: 0x1209, productId: 0x0001, classCode: 0xFF}]})` opens
  the device and claims only the vendor interface (2; found by class 0xFF, else 2), on bulk EP 3. Four
  `transferIn(3, 16448)` are kept queued.
  - When bulk answers, frames, LEDs and the session go over bulk. Panel events always come over Web MIDI.
  - If USB is lost, the session falls back to MIDI by itself.
- The session (`host/include/rfm/session.h`) then runs as on macOS:
  - SESSION with a 3000 ms keepalive;
  - frames at 20 fps, with at most 2 in flight;
  - LEDs on a change and at least once a second;
  - PANEL_R as the heartbeat;
  - recovery from a reboot, an unplug or a lost ACK;
  - SESSION 0 on stop.
- It runs on the page's **main thread**, where Web MIDI lives, never in the AudioWorklet. A 4 ms `setInterval`
  drives it, and every incoming message drives it too. Background-tab timer throttling therefore can't starve the
  keepalive while the unit is talking (PANEL_R arrives every 500 ms).

## The API

```js
import { RemoteFM1 } from '../remote/remote.js';
const remote = new RemoteFM1(opts);
```

| Call | What |
|---|---|
| `new RemoteFM1({fps, transport, usb, keepaliveMs, moduleUrl})` | all optional. `fps` 20; `transport` `'auto'` (bulk when it answers, else MIDI), `'midi'` or `'bulk'`; `usb` `'ask'` (open the WebUSB chooser on the first connect when no FM-1 is paired yet), `'known'` (only already-paired devices) or `'off'`; `moduleUrl` defaults to `../build/remote/remotefm1.mjs` relative to remote.js |
| `await connect()` | **call it from a click** (the WebUSB chooser needs the user gesture). Resolves once the search runs, not when the unit is found. It then keeps a session up, reconnecting by itself, until `stop()` / `disconnect()`. It rejects only when there is neither Web MIDI with SysEx nor a WebUSB device (Safari; SysEx refused). After `stop()`, `connect()` resumes. |
| `await connect({gesture: false})` | never prompts: no WebUSB chooser, and Web MIDI only if its permission is already `'granted'`. The session is **quiet** (`status.quiet`): HELLO at most every `quietHelloMs` (2000) per port. Resolves `false` (status `idle`) when Web MIDI isn't permitted. A later click's `connect()` makes it a normal session (and may open the chooser then). |
| `await autoConnect()` | at page load: connects by itself wherever the browser already allows it, and again when the unit is plugged in later (MIDIAccess `statechange`, `navigator.usb` `connect`; debounced, a new session at most every `autoMinMs`, 2000). Checks `navigator.permissions.query({name: 'midi', sysex: true})`: only `'granted'` uses Web MIDI (`'prompt'`/`'denied'`: nothing is asked or sent); WebUSB only for an FM-1 paired before. A quiet session starts only while an FM-1-named port is there and ends when it goes. After `disconnect()` it waits for the next plug-in. Resolves `{midi, usb}`. |
| `setQuiet(on)` | a running session looks on quietly (the page does this when a click's search found nothing in 10 s) |
| `midiPermission(navigator?)` | exported: `'granted'`, `'prompt'`, `'denied'`, `'unknown'` or `'unsupported'`, without asking |
| `await stop()` | SESSION 0. It is sent **synchronously** on Web MIDI, so it is safe in `pagehide`. It resolves on the ACK or after about 300 ms. The unit goes back to its waiting screen, and every key is released (one last panel with all zeros). |
| `await disconnect()` | `stop()`, then removes the listeners and releases the USB interface; status `idle` |
| `setFrame(seq, pixels)` | `engine.screen()`'s `{seq, pixels}`: `Uint16Array(57600)` RGB565 in **host order** (it is swapped to the wire's BE inside). The pixels are copied at once. A repeated `seq` is ignored. Call it on every `frame` message, up to 30 Hz; the session sends at its own fps. |
| `setLeds(levels, playGreen)` | `engine.leds()`'s `Uint8Array(44)` (`levels[col*4+line]`) and PLAY green 0..255. Mapped as FM1VST's `fm1p_led_class`: lit ≥ 176, mid ≥ 96, dim ≥ 40, faint > 0; line l is row bit l+1; PLAY green on when ≥ 176. Unchanged values are ignored. |
| `onStatus(cb)` | returns an unsubscribe function. `cb` gets the status object below. |
| `onPanel(cb)` | returns an unsubscribe function. `cb` gets the panel object below. |
| `onLog(cb)` | the session's log lines (candidates, found, lost, resends), for a details pane |
| `status`, `stats()`, `unitPorts()` | the current status; the session's counters; `{inputId, outputId, name}` of the unit's MIDI ports once found |

**Status** (on every change):

```
{ state, transport, fps, why, unit, ports }
```

| Field | Values |
|---|---|
| `quiet` | `true` for a session started without a gesture (`autoConnect()`, `connect({gesture: false})`) |
| `state` | `'idle'` before `connect()` / after `disconnect()`; `'searching'` (HELLO rounds every second); `'starting'`; `'connected'`; `'lost'` (a session dropped; searching again); `'stopping'`; `'stopped'`; `'error'` (`connect()` failed) |
| `transport` | `'bulk'` or `'midi'` while connected, else `null` |
| `fps` | frames the unit acknowledged in the last second, updated each second. It is 0 while the screen doesn't change: nothing is sent then. |
| `why` | the session's reason text |
| `unit` | `{ name: 'RemoteFM1', fwVer, identity, caps, maxInFlight }` |
| `ports` | the unit's MIDI ports: `{ inputId, outputId, name }` |

**Panel** (`onPanel`): after each batch of panel events from the unit, and on loss / stop with everything zero.

```
{ keys, buttons, enc, master, keyVel, taps: {keys, buttons}, battery_mv, source: 'fm1' }
```

| Field | What |
|---|---|
| `keys` | the unit's held keys: bit k is key k, 0 = F3 |
| `buttons` | the unit's held buttons: bit i is matrix button i, in API.md's order |
| `enc` | `Int32Array(7)`: detents since the last panel object, matrix order, + = CW. Each delta is delivered once. |
| `master` | 0..1023, or `null` until the unit has reported it |
| `keyVel` | `Uint8Array(27)` of zeros: the FM-1 has no velocity, so the core's default applies |
| `taps` | keys / buttons that went down and up within one batch |

PANEL_R resyncs, such as a lost note-off or MASTER's last count, arrive the same way.

## The page's wiring (W3, the "USE MY FM-1" section of the sheet)

```js
import { RemoteFM1 } from '../remote/remote.js';

const remote = new RemoteFM1();
let fm1 = null;                                   // the unit's last panel; null when not connected

useMyFm1Button.onclick = () => remote.connect().catch(e => showRemoteStatus({ state: 'error', why: e.message }));
stopButton.onclick = () => remote.disconnect();
remote.onStatus(s => {
    showRemoteStatus(s);                          // e.g. "connected via bulk, 20 fps" / "searching" / "lost"
    if (s.state !== 'connected') fm1 = null;
});
remote.onPanel(p => { fm1 = p; postPanel(); });
addEventListener('pagehide', () => remote.stop());

// worklet -> page: forward the screen and the LEDs (before transferring the pixels anywhere else)
node.port.addEventListener('message', ({ data: m }) => {
    if (m.type === 'frame') remote.setFrame(m.seq, m.pixels);
    if (m.type === 'leds') remote.setLeds(m.levels, m.playGreen);
});
// after a firmware (re)load, a fresh frame arrives by itself; nothing else is needed
```

Merge the unit's panel with the page's own input (mouse, touch, computer keyboard, MIDI keyboard) into the one
`setPanel` state the worklet gets (API.md section 3, `{type: "panel", ...p}`):

```js
function postPanel() {
    const p = { ...local };                       // the page's own panel state
    if (fm1) {
        p.keys = (local.keys | fm1.keys) >>> 0;
        p.buttons = (local.buttons | fm1.buttons) >>> 0;
        p.enc = Int32Array.from(local.enc, (d, i) => d + fm1.enc[i]);
        if (fm1.master !== null) p.master = fm1.master;  // the pot on the unit wins while connected
        p.taps = { keys: (local.taps?.keys | fm1.taps.keys) >>> 0, buttons: (local.taps?.buttons | fm1.taps.buttons) >>> 0 };
        // keyVel: keep the page's own (the unit gives 0 = default)
        fm1 = { ...fm1, enc: new Int32Array(7), taps: { keys: 0, buttons: 0 } };   // deltas and taps go once
    }
    worklet.port.postMessage({ type: 'panel', ...p });
    local.enc.fill(0);
}
```

Two things the page **must** do while a unit is connected:

- **Leave the unit's MIDI ports out of the page's own Web MIDI.**
  - Its panel events are notes on channels 15 and 16 (`9F 53+k`, `9E b`) and CCs on channel 15. A MIDI-keyboard
    input that listens to every port would play them a second time. Skip the input `remote.unitPorts().inputId`, or,
    simplest, every port where `isCandidatePortName(name)` is true (exported by remote.js).
  - Never send the engine's `midiOut` to the unit's output.
- **Load remote.js on the main thread.** Web MIDI doesn't exist in Workers, and the AudioWorklet must not wait on
  MIDI or USB.

The site build: the Pages workflow runs `web/remote/build.sh` (Emscripten in CI, as for the native cores).
`web/build/remote/remotefm1.mjs` must then sit at `build/remote/remotefm1.mjs` beside `remote/remote.js` in the
published tree. Otherwise pass `new RemoteFM1({ moduleUrl })`. `fakeunit.mjs` and `test/` are not published.

## What only the real unit can show

These are for the orchestrator, with the owner's FM-1:

- the Web MIDI port names Chrome shows for the unit on macOS (and Windows / Linux if available), and that the
  pairing picks the right input for the output;
- Chrome's Web MIDI SysEx throughput to the unit. The MIDI budget is 14000 B per frame at 20 fps, as CoreMIDI
  carried; watch `stats().ack_ms_max` and `ack_timeouts`.
- WebUSB in Chrome on macOS:
  - the chooser lists the FM-1;
  - `open()` works while the OS owns the MIDI interfaces;
  - `claimInterface(2)` works;
  - bulk frames converge and the status says "via bulk";
  - four queued `transferIn` deliver in order.
- WebUSB on Windows: the firmware has no MS OS 2.0 descriptors, so WinUSB isn't bound to interface 2. Expect the
  claim to fail there and the session to fall back to MIDI (the log says so). Linux needs a udev rule for
  1209:0001.
- The unit's panel playing the browser firmware (keys, buttons, the 7 encoders, MASTER), its LEDs following, and
  `pagehide` / Stop putting it back on its waiting screen.

Credits: libremotefm1 and the RemoteFM1 protocol are RemoteFM1's own (GPL-3.0-only). Built with Emscripten (MIT).
