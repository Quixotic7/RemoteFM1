# RemoteFM1

https://quixotic7.github.io/RemoteFM1/

Install here:

https://quixotic7.github.io/RemoteFM1/installer/


**The firmware for your firmware so you can run firmwares without installing firmwares**

RemoteFM1 is a firmware for the FM-1 that does almost nothing by itself. It sends every key, button, encoder and the
MASTER pot to the computer, and shows whatever the computer sends back on its LEDs and its 240×240 screen. On the
computer, an FM-1 firmware built for the emulator runs at full speed and uses the real FM-1 as its panel. That lets
you try firmware changes without reflashing the unit, and run things the FM-1's own CPU and memory couldn't hold.

> **Status: 0.2.3 released on the site (2026-10-28).** 0.2 moves the firmware onto
> [fm1-base](https://github.com/charlesvestal/fm1-base) and [felucca-bsp](https://github.com/hugelton/felucca-bsp) and
> adds Bluetooth MIDI; 0.2.1 adds USB audio; 0.2.2 makes Bluetooth about 6x faster and shows each screen change
> whole; 0.2.3 fixes recording from the FM-1 over USB. Tested on an FM-1 on 2026-10-28. 0.1.1 (on
> TransitFM1) stays there as the older release. The plan and every measurement are in [PLAN.md](PLAN.md).

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
- **Three transports:**
  - USB-MIDI SysEx, which works everywhere, including Web MIDI and the iPad;
  - a vendor bulk interface for macOS (libusb) and Chrome (WebUSB), about 830 KB/s;
  - Bluetooth MIDI SysEx (0.2): the same messages over the air, at a few KB/s, so a still screen and the panel more
    than a moving one (the host paces frames for it: about 5.5 KB/s, at most 20 a second, tiles reduced to 4 colours
    until the screen is still, then sent exactly; docs/PROTOCOL.md section 1). From 0.2.2 each screen change shows
    whole: the FM-1 keeps the old screen until the new one has all arrived, then switches at once (TILES' HOLD flag;
    a turning knob shows whole screens at the rate the link allows).

  The host uses bulk when it can. A session lives on the transport it started on; the panel's events go to that port
  (USB-MIDI for a USB session, Bluetooth for a Bluetooth one).
- The protocol is in [docs/PROTOCOL.md](docs/PROTOCOL.md), with its constants in [proto/rfm1.h](proto/rfm1.h).

On the host side, any firmware that exists as an **fm1core** can run: ChoralRoot, Felucca and Melodee today (through
[FM1VST](https://github.com/Quixotic7/FM1VST)'s engine). The stock M-VAVE firmware can't run on the host, because no
emulator exists for the FM-1's CPU.

## Using it

1. **Install RemoteFM1** on the FM-1 with [its web installer](https://quixotic7.github.io/RemoteFM1/installer/)
   (USB + Bluetooth MIDI, or USB only) or any Felucca-family installer, for example
   `python3 tools/fm1_install.py web/installer/firmware/remote-0.2.3.fwsc` (USB only: `remote-usb-0.2.3.fwsc`).
   0.2 answers installers as itself, `FM-1_52XXYYZZ` (release X.Y.Z; fm1-base's scheme), on the USB device "FM-1";
   0.1 answered as the official V15 (`FM-1_015`).
2. **Without a host**, the screen reads REMOTE FM1 / WAITING FOR HOST, with USB's state, the Bluetooth name it
   advertises ("FM-1 1F2D") and whether a computer is connected, and the battery; the key lights glow dim.
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

**Over Bluetooth** (0.2, the default package `remote-0.2.3.fwsc`; `remote-usb-0.2.3.fwsc` has no radio): Bluetooth
is on at every power-on. Connect the FM-1 in Audio MIDI Setup > MIDI Studio > Bluetooth (on a Mac) or the app's Bluetooth MIDI page;
its port is named after what it advertises, "FM-1 1F2D". The site and FM1VST send HELLO to any port whose name holds
"FM-1", so they find it there too. The update path stays USB-only.

**USB audio** (0.2.1): the Mac sees the FM-1 as a sound output ("FM-1" in System Settings > Sound and Audio MIDI
Setup) and plays through its speaker and headphones, with MASTER as its volume. Pick it as the output of fm1remote's
sound (or the Mac's) to hear the firmware on the unit. On the site, Settings > Playing > Sound output > FM-1 does it in
Chrome and Edge (Chrome asks once for the microphone, the only way it names the outputs; nothing is recorded) and in
Firefox (its output chooser); in Safari, pick FM-1 as the Mac's output. The FM-1 is an audio input too, carrying what it plays. The
USB interfaces are 0 audio control, 1 MIDI, 2 audio in, 3 audio out and 4 the vendor bulk pair (2 in 0.1 and 0.2.0);
hosts find the vendor interface by its class (0xFF), not its number.

**On an iPhone or iPad**, Safari and Chrome play the site's firmwares, but they have no Web MIDI, so the FM-1 can't
be their panel. [MIDIWeb Browser](https://apps.apple.com/us/app/midiweb-browser/id6757226617), an iOS browser that
adds Web MIDI, runs the whole site: the FM-1 as the panel over Bluetooth or USB, and its USB audio. Tested on an
iPhone 13 Pro Max on 2026-10-28. In any iOS browser, an FM-1 plugged in by USB becomes the phone's sound output by
itself (a Lightning iPhone needs a Lightning to USB camera adapter).

**Your own .rfm1 firmwares.** The site's player also runs `.rfm1` files: a firmware's own code built to WebAssembly
(FM1VST's core of it), the format RemoteFM1 for iOS runs. Download one from
[RemoteFM1FW's releases](https://github.com/Quixotic7/RemoteFM1FW/releases/latest) (or a firmware's own release),
then open it with the firmware browser's "Open a file..." or drop it on the page. It runs natively, as the site's own
native cores do, and stays in the browser under "Your firmware" (IndexedDB) with its saves, apart from the firmware's
native and emulated ones. The About page shows its licence, its authors and its source at the commit it was built
from. The site checks the file before it runs: its manifest, imports, exports and memory (web/engine/API.md
section 6).

## Safety

From 0.2, RemoteFM1 is an fm1-base firmware, with fm1-base's and felucca-bsp's safeguards: the boot guard (two failed
starts: safe mode, USB on), the crash screen and record, the watchdog, the USB soft key into the chip's update mode,
**REC held at power-on** for that same mode (also in safe mode; 0.1's OCT- went to its own loader), felucca-bsp's
update loader (`ota-FM-1_900`, writing the app area only), and the installers' way back to M-VAVE's firmware. A
package whose image reaches past the app area (M-VAVE's V16, Baud Girl's) is refused by RemoteFM1's installers before
anything is sent.

**What it writes in flash:** the Bluetooth build keeps the radio's address (and any pairing) in two 4 KiB sectors at
0xF2000..0xF3FFF, written at its first Bluetooth start (free in ChoralRoot's map and the stock firmware's); OCT- + OCT+
held at power-on (the panel calibration) writes 0xFC000..0xFDFFF. Nothing else: the data an earlier firmware left
elsewhere stays where it is. Every update stages its loader at 0xE0000..0xE4FFF, as 0.1's did. [docs/SAFETY.md](docs/SAFETY.md) is 0.1's (TransitFM1's) design, kept for its history.

Installing firmware is at your own risk. Keep the [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter)
or [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) at hand.

## Building the firmware

The firmware is fm1-base's: `base/` is fm1-base, a git submodule pinned at a commit (felucca-bsp inside it,
`base/bsp`). `./build.sh` copies it to `build/fm1-base`, applies RemoteFM1's patches (`firmware/patches/`: the vendor
bulk interface, beside USB audio, and the SysEx hooks in felucca-bsp's `usb.c`, SysEx over Bluetooth MIDI in fm1-base;
each one an upstream pull request, `firmware/patches/UPSTREAM.md`) and runs fm1-base's `tools/build.py` on the app in
`firmware/`. USB audio in and out are fm1-base's (`firmware/fm1.toml`).

You need Docker on macOS or Windows (or Linux x86-64), Python 3 with Pillow (with libraqm: on a Mac Homebrew's, which
`build.sh` finds in /opt/homebrew/lib), the JieLi toolchain (`base/tools/get_toolchain.sh`, in ~/.jieli/toolchain)
and the JieLi AC79 SDK (its Bluetooth libraries; never in this tree):

```sh
git submodule update --init --recursive
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh --release 0.2.3           # -> firmware/build/remotefm1-0.2.3.fwsc (USB + Bluetooth MIDI), FM-1_52000203
./build.sh --release 0.2.3 --no-ble  # -> firmware/build/remotefm1-usb-0.2.3.fwsc (USB only: no Bluetooth library)
./build.sh                           # a development build, FM-1_5200
tests/run_tests.sh                   # the app in fm1-base's host simulator (firmware/tests/), the protocol code,
                                     # the patched usb.c and Bluetooth SysEx, tools/fm1_install.py
host/run_tests.sh                    # the host library's tests, with the cross test against the firmware's own code
```

## What is where

```
firmware/app.h, app.c, fm1.toml    the app's contract with fm1-base, its build settings (fm1-base tools/build.py)
firmware/src/rfm_main.c     the app: transports (USB SysEx, USB bulk, Bluetooth SysEx), panel, LEDs, waiting screen
firmware/src/rfm_dev.c      what only the FM-1 has (felucca-bsp usb.c's rings, the panel's DMA); rfm_host.c: the
                            host simulator's stand-ins, commands and checks (firmware/tests/*.txt)
firmware/src/rfm_app.c      the protocol layer (no hardware; also compiled into the host tests)
firmware/src/rfm_draw.c     the TILES decoder;  rfm_push.c: the screen push, which never blocks USB
firmware/patches/           RemoteFM1's changes to fm1-base and felucca-bsp (MPL-2.0), applied by build.sh
base/                       fm1-base (git submodule), felucca-bsp in base/bsp
proto/, docs/PROTOCOL.md    the protocol
host/                       libremotefm1, the macOS back ends, fm1remote, tests
web/                        the site: the player, the installer, Use my FM-1 (web/remote)
tools/m0_bench.py, m0_bulk.c, web/m0_bench.html   transport benchmarks (SysEx, bulk, Web MIDI / WebUSB)
PLAN.md                     the plan, the milestones and every measurement
```

## License

The firmware (`firmware/`) is MPL-2.0 from 0.2, by its author's choice: it links JieLi's Bluetooth library (Apache-2.0,
binary only), which a GPL firmware could not. It is built on fm1-base (Charles Vestal) and felucca-bsp (Leo Kuroshita,
Hügelton Instruments), both MPL-2.0. Everything else (the site, `host/`, the tools) stays GPL-3.0-only, and so do the
firmware's versions before 0.2 (on TransitFM1, from [Dinghy](https://github.com/zvenson/dinghy) and
[Felucca](https://github.com/hugelton/Felucca)). See [LICENSING.md](LICENSING.md) for who made what. "M-VAVE" and
"FM-1" belong to their owners; RemoteFM1 is not made or endorsed by them.
