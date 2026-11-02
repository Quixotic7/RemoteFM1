<!-- SPDX-License-Identifier: GPL-3.0-only -->
# RemoteFM1 on the M-Vave SMK-37

RemoteFM1 also builds for the M-Vave SMK-37 (SMK-37 Pro, SMK-37 Elite, MKE-P37; the Donner Starrykey 37 Play takes
the Pro's package), a 37-key controller with the FM-1's chip (JieLi AC791N), the same boot loader and the same update
machinery. The FM-1 build is unchanged, byte for byte: everything for the SMK-37 is behind `BOARD_SMK37`, in a patch
series of its own (`firmware/patches/smk37/`: `felucca-bsp/` the board layer, `fm1-base/` the platform's side, `app/`
RemoteFM1's own), applied only to an SMK-37 build. The app's changes are a patch too, not edits of `firmware/`:
fm1-base's build hashes the app's source text into the image (`FM1B_BUILD`, the second core's bar), so any edit of
`firmware/`, even one under `#if BOARD_SMK37`, would change eight bytes of every FM-1 package. `./build.sh` with
`BOARD=smk37` builds from a patched copy, `build/app-smk37/`.

**To every host the SMK-37 is an FM-1.** RemoteFM1.com, the iOS and Android apps, FM1VST and libremotefm1 see exactly
what an FM-1 running RemoteFM1 shows them: the USB descriptors and the product name `FM-1`, the Bluetooth name
`FM-1 XXXX`, USB audio, the protocol, HELLO_R byte for byte (version `0.3.1`, the capabilities, the 240 x 240 screen,
the identity `FM-1_520`), PANEL_R with the FM-1's 27 keys, 14 buttons and 7 encoders. The firmware maps the SMK-37's
controls onto the FM-1's; no host knows or needs to know. Only the **update identity** (what an installer,
`fm1_install.py --info` and M-Vave's M-UPGRADE ask for) names the model, so that installers tell the boards apart and
M-UPGRADE still takes the unit's stock package for the way back.

**Status: untested on a unit.** Nobody on the project has an SMK-37. The board layer follows genkigenki's porting
guide and the facts ChoralRoot's tester-confirmed SMK-37 port relies on, and the simulator runs every control path,
but nothing here has run on an SMK-37 yet. [What to test](#what-to-test) lists what only a unit can answer.

## Controls

**Remote** is a host's session (a firmware on a computer drives the screen and LEDs); **standalone** is the waiting
screen and RemoteFM1's own DX7 instrument (its manual: the README's "Using it").

| SMK-37 | Remote (the FM-1 control it is) | Standalone |
| --- | --- | --- |
| Keys F3..G5 (the middle 27, keybed 5..31) | the FM-1's 27 keys, with velocity (TOUCH: velocity on every key, pressure 0) | the 27 keys: the DX7, with velocity |
| Keys C3..E3, G#5..C6 (the outer 10) | no FM-1 key: MIDI to the host's firmware, notes 48..52 and 80..84 with velocity | keys of their own: the DX7 at their pitch (the octave, the scale, LATCH, the arp, the loops as the 27 keys) |
| Pads 1..8 (top row), 9..16 (bottom row) | the FM-1's 16 white keys, F3 G3 A3 B3 C4 D4 E4 F4 / G4 A4 B4 C5 D5 E5 F5 G5, in reading order: a hit presses the key with the pad's velocity; the pad shows that key's LED (lit, mid, faint; warm white) | the same white keys (they play the DX7; in the menus, the loops' page and the settings they are the white keys) |
| A pad and its keybed key | one key, held while either is | the same |
| K1 | MASTER (the FM-1's pot: 96 of 4096 a detent, half the level at power-on) | MASTER (the volume) |
| K2 / K3 / K4 | SELECT / ALGORITHM / PRESETS | the same |
| K5..K8 | KNOB 1..4 | the same |
| Faders 1..4 | KNOB 1..4 as well: a fader turns its KNOB one detent a step (0..127), up clockwise; no turn from where it was at power-on | the same (a fast move speeds up as a fast knob turn does) |
| The 12 buttons, top row: ARP / NOTE REPEAT / SCALE / CHORD / GLOBE / BT | the FM-1's top row by place: FX / SEL / ENV / LFO / EDIT / GLO | the same |
| The 12 buttons, bottom row: PATCH / PARA / FX / SEQ / SEQ PLAY / SEQ REC | the FM-1's bottom row: HOME / SAVE / ARP / SEQ / PLAY / REC (PLAY's light: red or green, its two-colour LED) | the same |
| OCTAVE- / OCTAVE+ | OCT- / OCT+ | the same |
| `<` / `>` | SELECT one detent counter-clockwise / clockwise a press (as K2) | the same |
| The button lights | each button lit while its FM-1 button's LED is lit | the same |
| KNOB BANK / FADER BANK | no FM-1 button: MIDI to the host's firmware, CC 110 / 111 (127 pressed, 0 let go) | CC 110 / 111 out |
| Pitch wheel | MIDI to the host's firmware: pitch bend | bends the DX7, and out |
| Mod wheel | MIDI to the host's firmware: CC 1 | the DX7's mod wheel, and out |
| Pedal jack (a switch pedal) | MIDI to the host's firmware: CC 64 | sustain on the DX7, and out |
| OCTAVE- held at power-on | the chip's update mode (as REC at power-on on the FM-1) | |

"MIDI to the host's firmware" is RemoteFM1's MIDI_IN message (`docs/PROTOCOL.md`, the FM-1's TRS jack's MIDI in;
caps bit 9 is set on both boards), channel 1, whole messages, a wheel or the pedal at most every 6 ms (its
latest then). The SMK-37 has no TRS MIDI in, so nothing else uses that path. Of today's hosts the site and the iOS app
pass MIDI_IN to the firmware they run as its MIDI input; FM1VST (its released branch) and the Android app (no unit
link yet) drop it: there these controls do nothing. "Out" is MIDI out over USB and Bluetooth on the standalone's
channel (MIDI REMOTE: its CC CH, the outer keys on its KEY CH), as the keys' notes go.

### What the SMK-37 has not, or does not do here

- **Pressure (aftertouch)** on the keys: the SMK-37's keys have two contacts and no pressure sensing. A host that asks
  TOUCH gets the velocity of every key and pressure 0 (the FM-1 senses pressure on its analog keys). The pads sense
  pressure, but they are white keys here, and the FM-1's keys' pressure is not reported for them.
- **The FM-1's black-key LEDs**: no pad stands for a black key; a host's black-key lights do not show.
- **The dim glow and the mid level of the button LEDs**: the SMK-37's button LEDs are on or off; lit shows, mid and
  the glow do not. On the pads lit, mid and faint all show.
- **TRS MIDI**: there is no TRS MIDI in (its pin is a matrix row), and the jack's MIDI out is not driven by this build
  (its driver has not been confirmed on a unit).
- **The hardware calibration** (OCT- + OCT+ at power-on on the FM-1): the SMK-37's panel is the same on every unit, so
  there is nothing to teach; OCTAVE- + OCTAVE+ at power-on does nothing.
- **An expression pedal**: the jack is read as a switch (sustain) only.
- **MASTER across a power cycle**: K1 starts at half the level at every power-on (the FM-1's pot stays where it is).
- **The battery reading** is the stock firmware's three thresholds, not volts; HELLO_R reports no battery, as on the
  FM-1.
- In a host's session, KNOB BANK, FADER BANK, the wheels, the pedal and the outer keys reach the host's firmware only on
  the hosts that pass MIDI_IN on (above). `<`, `>` and the faders are FM-1 encoders, on every host.

## Flash map

The SMK-37's 1 MiB has ranges that are the unit's own and cannot be had again. RemoteFM1 never writes them: the
firmware's windows are checked at compile time (the app's `app.h` as `firmware/patches/smk37/app/` makes it,
felucca-bsp's `fm1_flash.h`, fm1-base's `plat.h`),
again by the store and the data area, and a third time in the flash driver itself (`FL_SMK_PROTECTED`), whatever a
caller allows. `tests/smk_flash_test.c` checks every sector.

| Flash | What | RemoteFM1 |
| --- | --- | --- |
| 0x00000-0x03FFF | the flash header, the SPL, the boot config | never |
| 0x04000-0x9CFFF | the app area (the stock PRCT) | the update loader only |
| 0x9D000-0x9EFFF | OBJ_RF: the settings, Bluetooth's address and bondings (A / B) | store |
| 0x9F000-0xA0FFF | the standalone's directory (A / B) | data |
| 0xA1000-0xAEFFF | the standalone's pool, sectors 0..13 (user banks, loops) | data |
| 0xAF000 | the store's owner tag `SMK37DATAREMOTE` | the platform, at a start |
| 0xB0000-0xB9FFF | the pool, sectors 14..23 | data |
| 0xBA000 | free | not written |
| 0xBB000-0xBFFFF | the update's staging and its record (0xBFF00) | the update |
| 0xC0000-0xC0FFF | BTIF (the stock firmware's Bluetooth pairing) | never |
| 0xC1000-0xC1FFF | USRTRIM: the factory key, pad and wheel curves (`trimdat`) | never (read: the keys' and pads' velocity curves) |
| 0xC2000-0xE9FFF | USRFLASH: the stock firmware's key / knob / fader mapping; the pool, sectors 24..63 | data, once the marker is gone |
| 0xEA000-0xEAFFF | USRFLASH's last sector: the stock end marker (`usrdata` at 0xEAFF8) | erased, whole, before the first write into USRFLASH; never programmed |
| 0xEB000-0xF3FFF | not in the stock region table | not written |
| 0xF4000-0xFDFFF | USR: the stock resources | never |
| 0xFE000-0xFEFFF | not listed | not written |
| 0xFF000-0xFFFFF | key_mac | never |

The FM-1's places (OBJ_RF at 0xF2000, the calibration at 0xFC000, the standalone's data at 0x97000..0xD8FFF) are not
used on the SMK-37: 0xFC000 is inside the SMK's USR. The 16 user DX7 banks and the 16 loops fit as on the FM-1 (the
pool keeps 64 sectors). There is no calibration object on the SMK-37.

**The owner tag.** A start that finds another tag at 0xAF000 (another firmware's, or none: the stock firmware's VM)
erases 0x9D000..0xAEFFF and 0xB0000..0xBAFFF once (a second or two), then writes RemoteFM1's tag. USRFLASH is not
cleared: the stock firmware's mapping stays as it is until RemoteFM1's first save that lands in USRFLASH; just before
that write the marker's sector is erased, so after a return to the stock firmware it rebuilds its factory mapping
there (it trusts the area only while the marker stands, and would otherwise read RemoteFM1's data as its mapping:
every key the same note). A custom mapping made under the stock firmware is lost then; the calibration never is.

**A unit with no factory record.** Every SMK-37 has `trimdat` at 0xC1224 (its stock firmware writes the defaults when
it is missing). If it is not there, this is not an SMK-37 (an FM-1 that was given this build by mistake): RemoteFM1
then writes nothing of its own (the FM-1's data and resources lie in these windows) and its update takes FM-1
packages, staged where the FM-1's update is, so the unit can be put right with an FM-1 package.

## Identities and packages

| Model | Package (`firmware/build/`) | Update identity |
| --- | --- | --- |
| SMK-37 Pro (and the Starrykey 37 Play) | `remotefm1-smk37[-usb][-0.3.1].fwsc` | `SMK-37 Pro_52000301` (dev `SMK-37 Pro_5200`) |
| SMK-37 Elite | `remotefm1-smk37-elite[-usb][-0.3.1].fwsc` | `SMK-37 Elite_52000301` (dev `SMK-37 Elite_5200`) |
| MKE-P37 | `remotefm1-smk37-mke[-usb][-0.3.1].fwsc` | `MKE-P37_52000301` (dev `MKE-P37_5200`) |

`-usb` is the build without Bluetooth, as on the FM-1. The identity is the model's name and RemoteFM1's own number
(`52` and the version, as the FM-1's `FM-1_52000301`); its update loader answers `ota-MODEL_900`, and the update's
record names the model's stock loader (`ota-SMK-37 Pro_016` etc.), so M-UPGRADE finds it under the name it expects
during a return to stock. The firmware's version (0.3.1, in HELLO_R and on the screen) is the FM-1's.

**The identity's length.** A package carries its identity in marker bytes, one a 0x30-byte block. The FM-1's packages
have 20 blocks; `SMK-37 Elite_52000301` has 21 characters, so the SMK-37's packages have 36 blocks, as M-Vave's own
SMK-37 v016 packages do (identities up to 27 characters: the device's identity reply holds 27). RemoteFM1's
`tools/fm1_install.py` finds 20 or 36 by the image's checksums. Another installer that counts blocks only while it
sees unused markers past the 20th would read the Pro's and the MKE-P37's packages (shorter names) but not the
Elite's.

**The package's own check.** The SMK-37 build's update takes a package only if its flash image's region table is the
SMK-37's (VM at 0x9D000, USRTRIM at 0xC1000): the stock SMK-37 packages, other SMK-37 ports' and RemoteFM1's. An FM-1
package (any: M-Vave's, RemoteFM1's, another firmware's) is refused before anything is written, by the running
firmware and again by RemoteFM1's SMK-37 update loader. The FM-1 build is unchanged and does **not** make this check
(nor did it before): an SMK-37 package given to an FM-1 by a tool that skips the model check would be taken. The
installers refuse it (`fm1_install.py`, the identity's model; M-UPGRADE, the same).

## Build

The toolchain as for the FM-1 (README.md "Building the firmware").

```sh
BOARD=smk37 ./build.sh                          # firmware/build/remotefm1-smk37.fwsc: SMK-37 Pro, Starrykey 37 Play
BOARD=smk37 MODEL=elite ./build.sh              # remotefm1-smk37-elite.fwsc
BOARD=smk37 MODEL=mke ./build.sh                # remotefm1-smk37-mke.fwsc
BOARD=smk37 ./build.sh --no-ble                 # remotefm1-smk37-usb.fwsc (no Bluetooth)
BOARD=smk37 ./build.sh --release 0.3.1          # remotefm1-smk37-0.3.1.fwsc, identity SMK-37 Pro_52000301
BOARD=smk37 ./build.sh --host                   # the simulator with the SMK-37's panel: build/host/remotefm1-smk37_host
BOARD=smk37 ./build.sh --web                    # the browser emulator: firmware/build/emu-smk37/ (below)
./build.sh                                      # the FM-1 build, as before
```

The SMK-37's builds run in a patched copy (`build/fm1-base` with the SMK-37's series on top, `build/app-smk37` for
the app) and copy their packages and simulator to `firmware/build/`. Build the FM-1 last before `tests/run_tests.sh`
(its own tests use the FM-1's files), as the porting guide advises. `SMK_SOURCES=1 ./build.sh` builds the FM-1 from
the SMK-37's patched sources (into `build/app-smk37/build/`): its code is the FM-1 build's byte for byte but for
`FM1B_BUILD` (the sources' hash), which checks that every SMK-37 change is behind `BOARD_SMK37`.

`tests/run_tests.sh` runs the SMK-37's tests after the FM-1's (the simulator's scenarios `firmware/tests/smk_*.txt`
and the FM-1's on the SMK-37 build, HELLO_R and PANEL_R compared with the FM-1 build's byte for byte, the decoder, the
flash map, and the update receiver against the built packages when they are there).

## Emulator

The SMK-37 build in the browser, for trying its controls without the keyboard: the simulator above compiled to
WebAssembly (fm1-base's `web/emu/emu_web.c` and its worklet, with `web_smk_*` exports from
`firmware/patches/smk37/fm1-base/0002-smk37-web.patch`) on a page of its own (`web/emu-smk37/`).

```sh
source ~/emsdk/emsdk_env.sh                     # Emscripten (emcc)
BOARD=smk37 ./build.sh --web                    # firmware/build/emu-smk37/ (MODEL=elite / mke: emu-smk37-elite/ ...)
python3 -m http.server -d firmware/build/emu-smk37 8037    # then http://localhost:8037/ and Start
node web/emu-smk37/test_emu_smk37.mjs           # its test, without a browser (tests/run_tests.sh runs it when emcc is there)
```

The page is an SMK-37 Pro: the screen, K1..K8 over the 16 pads, the four faders, the 18 buttons (each with its FM-1
role under it), the pitch and mod wheels, a sustain pedal switch and the 37 keys, the FM-1's 27 told apart from the
outer 10. Keys and pads take their velocity from where they are hit (the front edge hardest); the computer keyboard
plays the keys and the main buttons (the page lists them); Web MIDI in is the app's USB MIDI in; the settings the
firmware saves stay in the browser.

What it models: everything from the panel's state up. The controls set the state the scan would (`smk_raw_t`, the
knobs' detents), and the device's own map (`device/smk_panel.c`) turns it into the FM-1's controls and the extras'
MIDI, as on the unit; the button LEDs and the pads' colours are what the firmware gives the LED drivers. What it does
not: the matrix scan and its decoding (contacts, velocity timing, the factory curves: a hit's velocity goes in as it
is), the LCD and LED drivers, USB (descriptors, USB audio) and Bluetooth, the flash and the update. A host's session is
not part of the page; the standalone instrument is what it plays.

## Install

Have M-Vave's stock package for your model and M-UPGRADE ready first (see [Going back](#going-back)). A full flash
backup before the first install is the porting guide's advice (its docs/02-update-and-recovery.md, section 3): the
calibration and the Bluetooth and key data are the unit's own.

```sh
python3 tools/fm1_install.py firmware/build/remotefm1-smk37-0.3.1.fwsc    # your model's package
python3 tools/fm1_install.py --info
```

`fm1_install.py` finds the keyboard by its port (`SMK-37 ...` under the stock firmware, `FM-1` once RemoteFM1 runs) and
checks that the package is for it: an SMK-37 package goes on any SMK-37 model (one board; install your own model's, so
M-UPGRADE still knows the unit), never on an FM-1, and an FM-1 package never on an SMK-37, on the install and on the
resume of one that was cut off (the loader's model is checked the same way). The site's main installer and the apps
offer FM-1 packages only (the SMK-37 test installer: below). An install cut off: switch the keyboard on again (OCTAVE- held if it does not come up in
update mode) and run the install again.

The first start clears another firmware's data in RemoteFM1's windows (above): it takes a second or two longer, once.

### Install from the browser

The SMK-37 test installer, `/installer/smk37/` on the project's site (`https://remotefm1.com/installer/smk37/`
once deployed; in the tree `web/installer/smk37/`), Chrome or Edge on a computer. No page of the site links it and it
is not indexed: the maintainer sends testers the link. Tick that you accept the risk and that MIDI apps are closed,
pick USB + Bluetooth or USB only, and press Install. It reads the keyboard's update identity and picks your model's
package (`SMK-37 Pro_`, `SMK-37 Elite_`, `MKE-P37_`: the stock firmware's or RemoteFM1's; a Starrykey 37 Play or another
name of the SMK-37 gets the Pro's), checks its SHA-256 and identity against its list (`releases-smk37.json`) before
anything is written, and finishes an install that was cut off (RemoteFM1's update mode, `ota-MODEL_900`). It refuses
an FM-1 (any `FM-1_` identity, any FM-1 update mode) before anything but the identity query is sent, and another
firmware's update mode (M-UPGRADE's among them). `node web/installer/smk37/test_smk_installer.mjs` checks it against
a simulated keyboard.

## Going back

Install M-Vave's package for your model with M-UPGRADE. RemoteFM1 answers with the model's identity, takes the stock
package (it carries the SMK-37's region table), stages its loader and names it as M-UPGRADE expects. The calibration,
the Bluetooth pairing and key areas were never written. The stock firmware's key / knob / fader mapping is as it was,
or its factory one if RemoteFM1 saved into USRFLASH (its marker was erased first, so the stock firmware rebuilds it).

## Safety

- The unit's own ranges (above) are never written: refused at compile time, by the store, and in the flash driver.
- The factory calibration (USRTRIM) is read, never written: the keys' and pads' velocity follow each unit's own curves
  (the stock defaults when the record does not check out).
- Pins: PA2 (the FM-1's backlight) is never driven, PB1 (the charger's line) never touched, PC9 stays an analog input,
  PH8 is never the UART, the rows (PA2, PA5..PA7, PA9, PH6, PH8, PH9) are inputs only.
- The update takes SMK-37 packages only (and FM-1 packages only on a unit that is not an SMK-37).
- This build has not run on an SMK-37. Installing other firmware is at your own risk.

## What to test

What no test here can answer, in about this order. Note the model, the package and what you saw; a photo of the
screen or the pads helps.

1. **The first start.** From the stock firmware with `fm1_install.py`: the install finishes, the keyboard restarts
   into RemoteFM1's splash and the waiting screen (the screen at all: its pins, its setup, 20 MHz), the second start is
   quick. `fm1_install.py --info` says `SMK-37 Pro_52000301` (your model) and "running RemoteFM1 0.3.1 on an ...".
2. **The keys.** All 37 sound in the DX7 (the middle 27 and the outer 10 at their pitch), soft and hard strokes
   sound different (the factory curves); a key let go lets go; no stuck or doubled notes; fast repeated notes.
3. **The pads.** The top row is F3..F4's white keys, the bottom row G4..G5's (left to right); a hit plays with its
   velocity; the pads light as the white keys' LEDs do (the DX7 lights the keys that sound; a host's step row); no pad
   lit by its neighbour's colour (the LED chain's order and reset).
4. **The knobs.** K1 is the volume, one step a detent, clockwise up; K2..K8 one step a detent, clockwise +, none
   backwards (if one runs backwards: which).
5. **The buttons and their lights.** Each button does the job of the FM-1 button in its place (the table above); ARP
   (the FM-1's FX) lit at the start (CC bank A); `<` / `>` step SELECT; OCT+ lit while the octave is up; SEQ PLAY
   green while a loop plays, SEQ REC lit while recording; in a host's session its button lights (and PLAY's red and
   green halves) as the host lights the FM-1's.
6. **The wheels and the pedal.** The pitch wheel bends both ways and rests at no bend (after a first full sweep it
   reaches the ends); the mod wheel modulates; a switch pedal (plugged in before power-on) sustains; say whether it is
   normally open or closed.
7. **The faders.** Each turns its KNOB (1..4, as K5..K8): a full sweep about 127 detents, nothing at rest (no
   jitter), nothing at power-on whatever their position.
8. **A host.** RemoteFM1.com, the iOS app or FM1VST connects as to an FM-1 (USB and Bluetooth `FM-1 XXXX`), the
   screen streams, the 27 keys, pads, buttons and knobs play the host's firmware, its LEDs show on the pads and buttons.
   On the site or the iOS app: the outer keys, wheels and pedal reach the firmware as MIDI.
9. **USB audio.** The computer sees the audio device and plays through the SMK-37.
10. **Saving.** Send a DX7 bank, save loops (SAVE held: PARA), switch off and on: they are there. Do it until a save lands
    in USRFLASH (sixteen banks and a few loops).
11. **Going back.** M-UPGRADE installs your model's stock package; the stock firmware starts, every key its own note
    (the factory mapping rebuilt if RemoteFM1 saved into USRFLASH), the keys' and pads' feel as before.
12. **Update mode.** OCTAVE- held at power-on enters the chip's update mode.
13. **The battery.** On battery, the waiting screen's BATTERY n/3.

## Credits

The SMK-37's hardware facts (the matrix, the velocity method, the pads, the analog muxes, the lights, the flash map,
the panel's setup, the update's records and identities) are from genkigenki's porting guide
[fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) (M. Geisler, genkigenki / makkuro), and from what
ChoralRoot's SMK-37 port (genkigenki's, on [ChoralRootFM1](https://github.com/Quixotic7/ChoralRootFM1)) confirmed on
testers' units (REC's and PLAY's LED lines, the 20 MHz panel clock, the stock marker's handling). RemoteFM1's board
layer is written for felucca-bsp and fm1-base (MPL-2.0); no code of the guide's (which has none) or of ChoralRoot's
(GPL-3.0) is in it.
