# RemoteFM1 host side

The computer half of RemoteFM1 (protocol v2: `docs/PROTOCOL.md`, `proto/rfm1.h`). An FM-1 firmware built as an
`.fm1core` runs on the Mac, and a RemoteFM1 unit is its panel.

| Path | What |
|---|---|
| `include/rfm/`, `src/*.c` | **libremotefm1**, portable C99 with no OS calls: `codec` (both framings, every v2 body, link parsers), `screen` (the tile encoder), `tiles_ref` (a reference TILES decoder from the spec), `panel` (MIDI panel events, PANEL_R resync), `session` (discovery, SESSION, LEDs, frame flow control, recovery), `fake` (a fake unit) |
| `src/macos/` | back ends: CoreMIDI (`coremidi.c`), libusb vendor bulk (`usb.c`), the glue into a session (`host.c`) |
| `tests/` | ctest suites (ASan + UBSan through `run_tests.sh`) |
| `tools/fm1remote/` | runs a core with the unit as its panel (C++17; FM1VST's `engine/` compiled in, read-only) |

## Build and test

    cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release && cmake --build host/build -j
    host/run_tests.sh            # ASan + UBSan build in host/build-asan, then ctest (no device is opened)
    host/build-asan/test_screen --report ../FM1VST/build/cores/*.ppm    # the compression table

FM1VST is found next to the repo (`-DFM1VST_DIR=...` to override). No downloads: Apple clang, CMake, Homebrew
libusb 1.0 and the macOS frameworks.

## fm1remote

    host/build/fm1remote [--core choralroot|felucca|melodee|PATH] [--stats] [--transport auto|midi|bulk]
                         [--fps 20] [--budget BYTES] [--pattern] [--no-audio] [--device] ...

Without `--device` nothing is opened: the unit is the fake one, in the process, over a loopback at the M0 rates.
With `--device`, CoreMIDI candidates (real USB-MIDI entities with a source and a destination; `--port` narrows
them) each get a HELLO, and so does the vendor bulk interface on 1209:0001. Only an answer of "RemoteFM1" with
proto 2 counts. Frames go over bulk when it answers, else over SysEx. The flash image is kept in
`~/Library/Application Support/fm1remote/<core id>.bin`. Ctrl-C ends the session (SESSION 0) and saves the flash.

Threads: the core runs on the AUHAL render callback (or on a timer thread with `--no-audio`), alone, as
`fm1core.h` requires. The session runs on its own thread. Panel events go to the core through a lock-free queue
and are applied in Device's between hook. The LCD, LEDs and PLAY green come back through a lock-free triple
buffer, at most once per frame period and only when something changed.

## A browser (M4) back end

The library builds as it is with Emscripten (C99, malloc only, a millisecond clock passed in). A WASM back end
needs four things:

- Web MIDI with `sysex: true`: `output.send(bytes)` as the SX link's write, and `input.onmidimessage` feeding
  `rfm_session_rx`.
- WebUSB in Chrome: `claimInterface(n)` (the class-0xFF interface: 4 from firmware 0.2.1, 2 before),
  `transferOut(3, ...)`, and a few `transferIn(3, 16384)` kept queued as the BK link. The device needs a WebUSB
  descriptor for this.
- A timer for `rfm_session_poll` (`setInterval`, or a worker).
- The core in an AudioWorklet, with the same snapshot exchange (SharedArrayBuffer) that `engine.h` uses.

The CoreMIDI and libusb files are the only ones that do not port.
