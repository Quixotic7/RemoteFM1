# RemoteFM1 0.3.0

Released 2026-10-30, built with `./build.sh --release 0.3.0` (and `--no-ble` for the USB-only package). It replaces
0.2.3. Both packages install as `FM-1_52000300`.

| Package | sha256 |
| --- | --- |
| `remote-0.3.0.fwsc`, USB + Bluetooth MIDI (the default) | `0403415f68afd8c087004ab65683378b3d52dd7669cd46fed11bdb7c923b9a9f` |
| `remote-usb-0.3.0.fwsc`, USB only | `acde3717ef90e5f60c43c2e14636537e34feb3c95020ff0b7872e666a108c584` |

## What changed from 0.2.3: the FM-1 on its own

Without a host, RemoteFM1 is now an instrument of its own (the full control map: README.md "Standalone",
STANDALONE-PLAN.md section 3). A host's SESSION still takes it over as its panel, exactly as in 0.2.

- **A DX7.** saadet's FM6 engine (Kerem Kilic; MSFA's DX7 maths, sample for sample as saadet renders it), 8 voices
  over the FM-1's two cores. Yamaha's 8 ROM cartridges (256 voices) built in; send your own 32-voice cartridges over
  SysEx (up to 3 user banks). PRESETS the voice (rolling into the next cartridge), GLO + SELECT the cartridge.
- **Playing.** Velocity keys, OCT-/OCT+ (both: panic), scales that snap the keys (GLO + PRESETS the root,
  GLO + ALGORITHM the scale), LATCH (hold LFO), an arp (tap / hold ARP), SELECT the tempo, MIDI clock in.
- **A looper.** REC records exactly what you play, from the first note; 16 loops on SEQ (white keys pick, launch
  instantly, at the bar or at the end); count-in and a metronome; save / load over SysEx.
- **A MIDI controller.** Notes and 5 banks of 4 CCs out over USB and Bluetooth at once; MIDI in from USB, Bluetooth
  and the **TRS jack** plays the DX7. **REMOTE MIDI** (GLO + FX): every key, button and knob just sends MIDI, with its
  own settings page (hold HOME): screen, absolute / relative knobs, channels, keys, octave keys, scale shortcuts,
  knob LEDs.
- **Looks.** A pixel-art UI with 9 themes and background colours (SAVE), Notes / Scope / Keys views (HOME).
- **For hosts.** The TRS jack's MIDI goes to the host's firmware during a session (MIDI_IN, `0x54`; `caps` bit 9). A
  bare HELLO no longer hands the unit over: only a SESSION start does.
- Settings live only in 0xF2000..0xF3FFF (and user banks in 0x93000..0x96FFF), where no other FM-1 firmware keeps
  data; Bluetooth pairing is now saved too.

## Testing

On an FM-1 on 2026-10-30: the release package installed as `FM-1_52000300`; the DX7's render hash on the unit equals
the computer's on one and two cores; macOS shows the FM-1's USB audio devices; the owner played it, MIDI over USB,
Bluetooth and TRS (an OMX-27), and the website with FIZZ getting the TRS jack's MIDI. The simulator suite (every
standalone feature), the site, installer and remote-link tests pass.

# RemoteFM1 0.2.3

Released 2026-10-28, built with `./build.sh --release 0.2.3` (and `--no-ble` for the USB-only package). It replaces
0.2.2. Both packages install as `FM-1_52000203`.

| Package | sha256 |
| --- | --- |
| `remote-0.2.3.fwsc`, USB + Bluetooth MIDI (the default) | `211f06f63e8ca09de2812c017db9bb229713ce8e5a102eba887031551b2b403c` |
| `remote-usb-0.2.3.fwsc`, USB only | `e94574ee081dac596479a3de4ec0fd2aa0cea109a2692551b07037ad8eb17e4e` |

## What changed from 0.2.2

- **Recording from the FM-1 over USB works.** In 0.2.1 and 0.2.2 a computer recording the FM-1's USB audio input got
  mostly silence (82-85 % zeros, 2-15 ms of sound every ~29 ms). felucca-bsp's stall check (the host stopped
  reading for over 20 ms) counted USB polls as if only its 2 kHz timer polled; RemoteFM1 also polls from its main
  loop, 100,000-225,000 times a second, so the stream was dropped every millisecond. It now counts milliseconds
  (felucca-bsp patch 0003, hook `FELUCCA_USB_MS`).
- Playing to the FM-1 over USB, Bluetooth, the screen and the protocol are unchanged.

## Testing

On an FM-1 on 2026-10-28, with a Mac: a 1503 Hz tone played to the FM-1's USB audio output and recorded back from its
input. 0.2.2: 15 % of the samples sound, 234 gaps over 1 ms in 7 s. 0.2.3: continuous, no gap over 1 ms, every 20 ms
window a clean sine. A RemoteFM1 session (HELLO) over USB-MIDI answered as before.

# RemoteFM1 0.2.2

Released 2026-10-27, built with `./build.sh --release 0.2.2` (and `--no-ble` for the USB-only package). It replaces
0.2.1. Both packages install as `FM-1_52000202`.

| Package | sha256 |
| --- | --- |
| `remote-0.2.2.fwsc`, USB + Bluetooth MIDI (the default) | `489857fcc91104cd4edf19c7c975c0e6258d311931830838e9d9e6913bb3cb89` |
| `remote-usb-0.2.2.fwsc`, USB only | `4414f8f0b0936cb22c3fac91889e0df2cfa274d6322844edf13af5408841fc4d` |

## What changed from 0.2.1

- **Bluetooth about 6x faster.** The radio now uses the 2M PHY, 251-byte link-layer packets (Data Length
  Extension), an ATT MTU of 247 (244-byte writes), 16 receive buffers and a 1024-packet input queue, and asks for a
  7.5 ms connection interval first. JieLi's SDK left all of these off. Measured on an FM-1 with a Mac, 1 KB SysEx
  frames: about 6 KB/s in 0.2.1, about 35 KB/s now (38 KB/s with more in flight), nothing lost with 14 KB
  unacknowledged. macOS took 2M, 130-byte link packets and 7.5 ms. HELLO_R `caps` bit 8 says so.
- **Each screen change appears whole over Bluetooth.** A new TILES flag, HOLD (`caps` bit 7): the FM-1 draws held
  frames into its off-screen copy and shows them with the frame that completes the change. Turning a knob that
  changes big text shows whole screens, never half old and half new. A change that stalls is shown by itself after
  1.5 s.
- **Hosts** (this site's player and libremotefm1): over Bluetooth they pace frames for the link (30 KB/s with 0.2.2,
  5.5 KB/s with older units), send busy tiles with 4 colours first and the exact colours once the screen is still
  for a second, and send each change as one snapshot. USB and the vendor bulk interface are unchanged.
- STATS_R carries what the Bluetooth link negotiated (interval, MTU, data length, PHY), for diagnosing a slow link.

## Testing

On an FM-1 on 2026-10-27: installed over 0.2.1 and development builds; over USB-MIDI SysEx a session, every TILES
encoding, FRAME_ACK and LEDS (about 300 small frames a second round trip); over Bluetooth MIDI with a Mac, this
site's player running ChoralRoot: page changes and a turning PRESETS knob shown whole, no timeouts or lost frames.

# RemoteFM1 0.2.1

Released 2026-10-27, built with `./build.sh --release 0.2.1` (and `--no-ble` for the USB-only package). It replaces
0.1.1. Both packages install as `FM-1_52000201`.

| Package | sha256 |
| --- | --- |
| `remote-0.2.1.fwsc`, USB + Bluetooth MIDI (the default) | `c4771c6cb6f38ac74b30d86a0f07b5f1ee90a3bcc270cfe7cd54fc3e74e5344f` |
| `remote-usb-0.2.1.fwsc`, USB only | `073b9e4f05f0aa783d92904a8d474ae817359a613295ee88f73b8d61dbd52f1e` |

## What changed from 0.1.1

- **Built on fm1-base and felucca-bsp** (MPL-2.0). TransitFM1's platform is gone. The firmware is MPL-2.0 from
  0.2; 0.1.1 stays GPL-3.0-only.
- **Bluetooth MIDI.** The panel and the screen work without a cable, with the same messages as over USB. The screen
  is slower that way (a few KB/s): fine for a still screen, not for a moving one. Updates still go over USB only.
- **USB audio.** The computer plays through the FM-1's speaker or headphones, with MASTER as the volume. The FM-1 is
  an audio input too. Turning MASTER no longer crackles (felucca-bsp patch 0002).
- **It answers installers as itself:** `FM-1_52XXYYZZ` (`FM-1_52000201` for 0.2.1), on the USB device "FM-1".
  0.1 answered as the official V15 (`FM-1_015`). An installer that does not know it yet may refuse it.
- **REC held at power-on** enters the chip's own USB update mode (0.1's key was OCT-, into its update loader).
- **fm1-base's safeguards:** the boot guard (two failed starts give a safe mode with USB on), the crash screen,
  the watchdog. Updates use felucca-bsp's loader (`ota-FM-1_900`), which writes the app area only.
- **The vendor bulk interface moved** from interface 2 to 4 (after USB audio in and out); bcdDevice 4.50. Hosts
  find it by its class.

## What it writes in flash

- 0xF2000..0xF3FFF: Bluetooth's address and any pairing, at the first Bluetooth start (the Bluetooth package only).
- 0xFC000..0xFDFFF: the panel calibration, only when OCT- and OCT+ are held at power-on.
- Every update stages its loader at 0xE0000..0xE4FFF, as 0.1's did.

Nothing else. These places are free in ChoralRoot's map and in the stock firmware's, so their data stays.

## The USB-only package

`remote-usb-0.2.1.fwsc` has no radio and none of JieLi's Bluetooth library. Like every FM-1 package, it still
carries the three JieLi SDK files the chip boots with (Apache-2.0).

## Testing

On an FM-1 on 2026-10-27, the USB + Bluetooth package:
- installed over RemoteFM1 0.2.0 and over FIZZ;
- over USB-MIDI SysEx: HELLO, a session, every TILES encoding, FRAME_ACK, LEDS (about 265 small frames a second
  round trip; a full frame drawn in 22 ms);
- over the vendor bulk interface (interface 4): about 845 KB/s in, 815 KB/s out, 0.16 ms echo; about 610 KB/s while
  USB audio plays, with no errors;
- over Bluetooth MIDI as "FM-1 7938": HELLO, a session, about 22 frames a second round trip, keys, buttons, encoders
  and MASTER, 155 frames of an interactive demo all acknowledged;
- USB audio on the speaker and on headphones (headphones mute the speaker), MASTER as the volume without crackle.

Tested on an FM-1 on 2026-10-27: both packages; installs from RemoteFM1 0.2.0, FIZZ and ChoralRoot 0.27, and from 0.2.1 to ChoralRoot 0.27. Not tested on an FM-1: the way back to M-VAVE's V15 from 0.2.

# RemoteFM1 0.1.1

`remote-0.1.1.fwsc`, sha256 `a7408d6c0526e01ef80e81c7350082ff203beaca3eabee52899748fc730a96a7`, built with
`./build.sh --release 0.1.1`. It replaces 0.1.

## What changed from 0.1

0.1.1 is a safety release. The panel, the protocol and USB are the same as in 0.1.

- **A firmware too large for the app area is refused.** A package whose image reaches past 0x93000 (Baud Girl's
  can) is now refused twice. Step 1 refuses it before anything is staged. The update loader refuses it before
  anything is erased. Under 0.1, an installer writing such a package over RemoteFM1 could leave a truncated image
  that was committed, and the unit would stay dark. To install such a firmware, go through the official V15 first.
- **The update loader is TransitFM1 0.1.3's, byte for byte** (`ota.bin` sha256 `d8a959c7…`). It answers
  `ota-FM-1_900`, as 0.1's did. TransitFM1 0.1.2's `ota-FM-1_015`, which bricked a unit leaving to Baud Girl, is
  not in RemoteFM1.
- A crash during the panel's start-up shows the crash screen without starting the panel again inside itself.
- The version reads 0.1.1 on the waiting screen and in HELLO.

These are unchanged: ChoralRoot's boot stub (sha256 `baee55ed…`), the sector 0x4000 layout (`0f33c838…`), the
staged loader written through RAM, MIDI-in draining, and the identity `FM-1_015`.

## Testing

0.1 (sha256 `13035039…`) is the build that was tested on a unit, on 2026-10-18: ChoralRoot played from a Mac through
the FM-1. 0.1.1 has passed the host tests: power cuts after every flash operation for V15 and ChoralRoot 0.25.1 both
ways, plus both refusals of an oversized package. 0.1 is no longer offered here.

0.1.1 on a unit, 2026-10-18:
- **Installed with this page,** in Chrome, over 0.1. The update mode wrote for 30 s, and the unit came back
  answering RemoteFM1 v0.1.1 with its own loader staged ("loader 1": the new loader replaced 0.1's at first boot).
- **ChoralRoot 0.25.1 → RemoteFM1 0.1.1 → ChoralRoot 0.25.1** with `fm1_install.py`:
  - ChoralRoot's boot stub reported "boot stub v3 … marker ok";
  - ChoralRoot's stored data (128 user sounds, favourites, loops, settings) was identical before and after.
