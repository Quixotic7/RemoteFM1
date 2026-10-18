# RemoteFM1

**The M-VAVE FM-1 as a USB panel for an FM-1 firmware running on your computer.**

RemoteFM1 is a firmware for the FM-1 that does almost nothing by itself. It sends every key, button, encoder and the
MASTER pot to the computer, and shows whatever the computer sends back on its LEDs and its 240×240 screen. On the
computer, an FM-1 firmware built for the emulator runs at full speed and uses the real FM-1 as its panel. That lets
you try firmware changes without reflashing the unit, and run things the FM-1's own CPU and memory couldn't hold.

> **Status: early (M1, 2026-10-18).** It works end to end on one unit: ChoralRoot running on a Mac with the FM-1 as
> its panel, 20 frames a second, input and screen confirmed. Not released, no installer page yet. The plan and every
> measurement are in [PLAN.md](PLAN.md).

## How it works

```
 FM-1 (RemoteFM1)                                  Mac
 keys, buttons, encoders, MASTER  ──plain MIDI──►  libremotefm1 ──► the firmware (an .fm1core)
 LEDs, screen                     ◄─SysEx / bulk── screen encoder ◄── its screen and LEDs
```

- **Input** goes out as plain MIDI, so any MIDI app sees the FM-1 as a controller:
  - keys: notes on channel 16;
  - buttons: notes on channel 15;
  - encoders: relative CCs 16–22;
  - MASTER: 14-bit CC 7/39.
- **The screen** goes as 16×16 tiles. Only tiles that changed are sent, oldest first, each in the smallest of five
  encodings. A typical UI frame is about 2 KB; a full redraw of a busy screen is 2–10 KB.
- **Two transports:**
  - USB-MIDI SysEx, which works everywhere, including Web MIDI and the iPad;
  - a vendor bulk interface for macOS (libusb) and Chrome (WebUSB), about 830 KB/s.

  The host uses bulk when it can.
- The protocol is in [docs/PROTOCOL.md](docs/PROTOCOL.md), with its constants in [proto/rfm1.h](proto/rfm1.h).

On the host side, any firmware that exists as an **fm1core** can run: ChoralRoot, Felucca and Melodee today (through
[FM1VST](https://github.com/Quixotic7/FM1VST)'s engine). The stock M-VAVE firmware can't run on the host, because no
emulator exists for the FM-1's CPU.

## Using it

1. **Install RemoteFM1** on the FM-1 with any Felucca-family installer, for example `python3 tools/fm1_install.py
   build/remote.fwsc`. RemoteFM1 answers installers as the official V15 (`FM-1_015`, USB device "FM-1"), so every
   installer can write over it later.
2. **Without a host**, the screen reads REMOTE FM1 / WAITING FOR HOST and the key lights glow dim.
3. **Run a firmware on the Mac:**

   ```sh
   cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release && cmake --build host/build -j
   host/build/fm1remote --device --port FM-1 --stats                  # ChoralRoot
   host/build/fm1remote --device --port FM-1 --stats --core felucca   # or melodee
   host/build/fm1remote --device --port FM-1 --stats --pattern        # test screens only, no firmware
   ```

   - The sound plays on the Mac.
   - Each firmware's flash image is kept in `~/Library/Application Support/fm1remote/`.
   - Ctrl-C ends the session, and the FM-1 goes back to its waiting screen.
   - `fm1remote` expects the [FM1VST](https://github.com/Quixotic7/FM1VST) checkout next to this one (cores in
     `FM1VST/build/cores`).
4. **To leave RemoteFM1**, install the next firmware with its own installer, as you would over V15.

macOS keeps the MIDI port name of the first firmware it saw on the unit (for example "ChoralRoot FM-1"). The host
finds RemoteFM1 by asking (HELLO), never by name. If the port shows up offline after a firmware change, use
`--midi-restart` or Audio MIDI Setup > Rescan MIDI.

## Safety

RemoteFM1 is built on [TransitFM1](https://github.com/Quixotic7/TransitFM1) and keeps all of its flash safeguards:

- ChoralRoot's boot stub, byte for byte;
- its own staged update loader, which writes the next firmware with the first sector last;
- the boot guard;
- the M-UPGRADE updater as V15.

[docs/SAFETY.md](docs/SAFETY.md) explains them. RemoteFM1 stores nothing on the unit, and the data an earlier
firmware left in flash stays where it is.

Two bugs from TransitFM1 0.1 are fixed here, as in TransitFM1 0.1.1:

- **Loader re-stage:** re-staging the update loader read from flash while writing flash, which crashed the unit
  into the ROM's USB boot.
- **MIDI-in ring:** the ring was never drained, so a DAW sending MIDI shut the installer out.

Installing firmware is at your own risk. Keep the [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter)
or [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) at hand.

## Building the firmware

You need Docker on macOS or Windows (or Linux x86-64), Python 3 with Pillow, the JieLi toolchain and the JieLi AC79
SDK, exactly as for TransitFM1:

```sh
tools/get_toolchain.sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh                  # -> build/remote.fwsc; `--unit transit` builds TransitFM1 0.1 as it was
tests/run_tests.sh          # the firmware's host tests (stub, loader, update path, protocol, decoder, session)
host/run_tests.sh           # the host library's tests, including the cross test against the firmware's own code
```

## What is where

```
firmware/src/remote.c       the device app: USB service, waiting screen, panel input, LEDs, glue
firmware/src/rfm_app.c      the protocol layer (no hardware; also compiled into the host tests)
firmware/src/rfm_draw.c     the TILES decoder;  rfm_push.c: the screen push, which never blocks USB
firmware/                   everything else: TransitFM1's platform, boot stub, update loader, USB (+ vendor interface)
proto/, docs/PROTOCOL.md    the protocol
host/                       libremotefm1, the macOS back ends, fm1remote, tests
tools/m0_bench.py, m0_bulk.c, web/m0_bench.html   transport benchmarks (SysEx, bulk, Web MIDI / WebUSB)
PLAN.md                     the plan, the milestones and every measurement
```

## License

GPL-3.0-only, like everything it's built from. See [LICENSING.md](LICENSING.md) for who made what. RemoteFM1 comes from
TransitFM1, which comes from [Dinghy](https://github.com/zvenson/dinghy) (zvenson), which runs on
[Felucca](https://github.com/hugelton/Felucca)'s platform (Leo Kuroshita, Hügelton Instruments) as
[SLOOP](https://github.com/isod89/sloop-fm1) carries it. The boot stub is
[ChoralRoot FM-1](https://github.com/Quixotic7/ChoralRootFM1)'s. "M-VAVE" and "FM-1" belong to their owners;
RemoteFM1 is not made or endorsed by them.
