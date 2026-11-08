<!-- SPDX-License-Identifier: MPL-2.0 -->
# RemoteFM1's patches to fm1-base and felucca-bsp

RemoteFM1 builds on [fm1-base](https://github.com/charlesvestal/fm1-base) (`base/`, a submodule pinned at `317cb86`)
and [felucca-bsp](https://github.com/hugelton/felucca-bsp) (`base/bsp/`, pinned by fm1-base at `5b0879b`). The
submodules stay as they are. `../../build.sh` copies `base/` to `build/fm1-base/` (rsync: only what changed), applies
the patches below with `git apply` (exactly, or the build stops), and builds from the copy. Every patch is a change to
MPL-2.0 files, published under MPL-2.0 like the files it changes, and each is meant to go upstream: the texts below
are the pull requests. Once a change is merged upstream, move the submodule to that commit and drop its patch.

To change a patch: edit the files in `build/fm1-base` (or a copy of `base/`), then diff against `base/` as the files
here are made (`diff -ruN`, paths `a/` and `b/` from the submodule's root, no timestamps).

The patches are opt-in for everyone else: a firmware that does not define the new macros builds byte for byte as
before (checked: fm1-base's template, and felucca-bsp's update loader `ota.bin`, are identical with and without them),
except the Bluetooth MIDI SysEx, which is new behaviour for every Bluetooth firmware (it was dropped before), and
felucca-bsp 0007, 0008 and 0009 (USB Host Off, a device's suspend, the root reset), fixes for every firmware with the USB host or that reads
`plat_usb_lost`.

## The move to fm1-base 317cb86 / felucca-bsp 5b0879b (2026-11-07)

From `934e40d` / `2cd696a`. What it brings RemoteFM1: felucca-bsp #12 (Julius Vitkauskas, 2d37ec8), an erase or a
program never goes back to XIP while the part is still busy (it waits up to 2 s for an erase and 50 ms for a page,
then resets the part, 0x66 / 0x99, and the chip, all from RAM), where the old driver gave up after 500 ms and switched
XIP back on under a part still erasing: a crash on a slower part, such as the UC25HQ80 (JEDEC 0xB36014) of the FM-1s
with the logo on the front. With it: #8's flash gate (write regions, CPU0 only with its interrupts off, the second
core in its idle loop), #20's queue of received SysEx frames, #26's fast install in the update loader (the loader
RemoteFM1's packages carry: 8,340 -> 11,782 B), and fm1-base's `device/fm1_flash_ok.h` (`FM1B_FLASH_JEDEC_OK`).

- **Dropped**: `fm1-base/0009-flash-b36014.patch`. `main_fm1.c` now takes `FM1B_FLASH_JEDEC_OK(id)`, 0x856014 or
  0xB36014, from `device/fm1_flash_ok.h` (fm1-base #35, with `tests/flash_ok_test.c`). The SMK-37 series defines it
  for its board (0xEB6014 and the FM-1's two) before that header.
- **Dropped from 0001**: `FELUCCA_CIN_F_SYSEX` as a switch. felucca-bsp 80db9c9 takes a CIN 0xF byte inside a SysEx
  (or with F0) as a SysEx byte for every firmware, as this patch did for RemoteFM1; the macro stays only as a counter
  hook (RemoteFM1's STATS_R `RF_ST_USB_CINF`). `fm1_lcd_done()` is now `fm1_lcd_xfer_done()`: felucca-bsp's panel
  driver (#9, `boards/fm1/fm1_lcd.h`) has a `fm1_lcd_done()` of its own (wait, deselect).
- **Kept on top of upstream's**: 0003 (the audio input's stall by time) beside `FELUCCA_UAC_STALL_POLLS` (f1f8ca2):
  that one counts polls, which RemoteFM1's main loop makes at no fixed rate; without `FELUCCA_USB_MS` the count is
  upstream's. `FELUCCA_UAC_CH` and 5b0879b's input packet pacing change nothing for 0002 / 0003 (two channels; the
  output's gain glide is in `uac_play`, untouched upstream).
- **Kept**: 0005 (upstream's `.app_ram` and `tools/ramcode.py` are the general way to RAM code now; saadet's FM6 names
  `.dsp_text` / `.audio_text` and calls through function pointers, which ramcode.py refuses without an fm1.toml
  `ram_code_allow` list, so it stays under 0005's check, `.app_ram` left to ramcode.py); 0006 (the flash gate's
  default write regions hold RemoteFM1's windows: 0xF2000..0xF3FFF in `FL_XDATA1`, the calibration's 0xFC000 in
  `FL_GLOB`); 0007 (`plat_dim_ns` is one width for the whole dim layer, not two levels for keys); 0008 (the Bluetooth
  sources are still globbed unsorted).
- **New**: `felucca-bsp/0006-ota-identity-whole.patch` and `fm1-base/0009-firmware-json-repo-root.patch`, below.
- **The identity reply**: fm1-base 3ae2a65 took the flash's JEDEC ID out of it again (no `OTA_FLASH_ID`: bytes 24..26
  stay 0), because The Firmware Hub's installer, which strips only trailing zeros, then found "No FM-1". RemoteFM1
  defines no `OTA_FLASH_ID` either: its reply is the text, then zeros, as before. felucca-bsp 41a6e86 answers a plain
  handshake with the identity cut to the name, `_` and three digits (`FM-1_520`, for M-UPGRADE 3.11) and only a
  handshake with the body 01 with the whole one; RemoteFM1's installers send the plain one and read the version from
  it, so `firmware/app.h` defines `FELUCCA_ID_WHOLE` (0006): the whole identity to every handshake, as before.
- **Defaults that moved**: fm1-base's `usb_midi_last` is true by default since 2026-10-31 (MIDI after the audio
  streams, bcdDevice 4.D0 with the vendor interface); `firmware/fm1.toml` sets it false, so the interfaces and
  bcdDevice 4.50 stay RemoteFM1 0.2.1's. The new opt-in parts (crash history, debug stats, `audio_aside`, headphones,
  `usb_audio_channels`, MIDI stamps, ...) stay off. A `--release` build now needs firmware.json's keys (author,
  description, tags, install, screenshot): `firmware/fm1.toml` has them, from `fmhub.json`.

## felucca-bsp: `felucca-bsp/0001-vendor-interface-and-app-hooks.patch`

`chip/usb.c`, `boards/fm1/fm1_lcd_hw.h`. PR title: **usb.c: an optional vendor bulk interface (WebUSB), SysEx and
EP1 hooks for apps; fm1_lcd_xfer_done()**

> RemoteFM1 (an FM-1 firmware that is only the panel of a firmware running on a computer) moves frames of its
> screen at 20 Hz: up to ~40 KB a frame, more than USB-MIDI carries. Its 0.1 (on TransitFM1) had a vendor-specific
> bulk interface beside USB-MIDI, which Chrome's WebUSB and libusb open; this brings it to felucca-bsp's `usb.c` as an
> option, with the small hooks an app needs to take its own SysEx at full speed. Nothing changes without the new
> macros (the update loader and fm1-base's template build byte for byte as before).
>
> - `FELUCCA_VENDOR=1`: a vendor-specific interface (class 0xFF) as the last interface: 2 after audio control and
>   MIDI streaming, or after the audio streams with `FELUCCA_UAC` (3; 4 with `FELUCCA_UAC_OUT`), with a bulk OUT / IN
>   pair on EP3 (the CDC option's endpoint: not with CDC, disk mode or in the loader, `#error` otherwise; UAC's
>   endpoint is EP4, and its EP4 OUT is polled, so the interrupt enables are EP1's and EP3's as before). With the USB
>   host (`FELUCCA_USB_HOST`, which uses EP0 and EP1 only): the vendor service is off while the port is a host
>   (`usb.config` 0; `usb_vendor_send` -1, `usb_vendor_room` 0), host entry (`usb_host_task`) drops what both rings
>   hold, as it drops CDC's, the updater's and the mirror's, and the next SET_CONFIGURATION sets EP3's buffers again
>   (`fm1_usb_host_attach` pointed EP1..3 at EP0's). bcdUSB 2.10
>   and a BOS descriptor with the WebUSB platform capability (vendor code 1, no landing page). bcdDevice (hosts cache
>   descriptors per version): 3.50 without USB audio (RemoteFM1 0.1's; note that it is also what felucca-bsp gives
>   audio in + out at 44.1 kHz, fm1-base's default, without the vendor interface); with USB audio 4.00 + the audio's
>   usual steps (4.50: in + out at 44.1 kHz), a version no layout without the vendor interface has. Two rings
>   (`FELUCCA_VENDOR_IN_N` / `_OUT_N`, powers of two, in `FELUCCA_VENDOR_RING_ATTR`) served by `usb_poll`; the app's
>   side: `usb_vendor_avail`, `usb_vendor_read`, `usb_vendor_room`, `usb_vendor_send` (a message whole or not at
>   all). EP3 OUT is NAKed while the in-ring is full; a short packet ends each burst out (no ZLP needed); a bus reset
>   drops what waits to go out.
> - `FELUCCA_SYSEX_TAP(b)`: every SysEx byte from USB-MIDI (and a status that ends one), in `usb_poll`'s context.
>   Nonzero: the message is the app's, and is not collected for `ota.c` from that byte on, so a long stream of the
>   app's SysEx never takes room in the updater's frame queue (`sx_frame`, #20), and messages longer than 640 bytes
>   reach the app whole.
> - `FELUCCA_EP1_ROOM()`: before an EP1 OUT packet is taken; 0 leaves it for a later poll (NAKed). An app uses it to
>   hold the host while its own SysEx ring is full, and to empty the MIDI-in ring from `usb_poll`'s context when it
>   plays no USB MIDI in (so a DAW's clock can never fill the ring and shut the updater out while an update session
>   keeps the main loop away).
> - `FELUCCA_USB_FRAME_DUE()`: when the 10 ms frame-number check is due. An app that calls `usb_poll` from its main
>   loop (100,000 times a second and more) needs it by the clock: by poll count, the frame number looked frozen and the
>   bus "suspended".
> - `FELUCCA_CIN_F_SYSEX`: evaluated for each CIN 0xF byte taken into a SysEx (felucca-bsp takes them since 80db9c9),
>   for an app's count of them.
> - `fm1_lcd_xfer_done()` in `boards/fm1/fm1_lcd_hw.h`: the running transfer is over (a look, no wait), for an app
>   that never waits for the panel (fm1-base's `lcd_idle()`).
>
> Tested: RemoteFM1's `tests/usb_midi_test.c` (the parser fed every SysEx length 6..3000, macOS's split, clocks inside,
> the tap's claim against the updater's frame queue, EP1 held); the vendor
> interface's descriptors alone (0.1's byte for byte) and after USB audio in + out (the configuration walked: five
> interfaces in order, each endpoint in one, the vendor pair on interface 4); the loader's `ota.bin` is identical with
> the patch. On an FM-1: RemoteFM1 0.1 ran the same descriptors and vendor service
> (M0: ~830 KB/s bulk with the main loop polling).

## felucca-bsp: `felucca-bsp/0007-usb-host-off-clean.patch` (2026-11-08)

`chip/usb_host.c`, `docs/USB_HOST.md`, `tests/usb_host_test.c`. PR title: **usb_host.c: Host Off undoes the host's
controller state and stays off the bus a second**

> On an FM-1 (ChoralRoot 0.31, the same host code), switching the host off left the Mac seeing nothing on the port
> until a power cycle. `uh_host_up` writes registers the device driver never writes: DEVCTL's session and host
> request, EP1's TX / RX types and intervals and its TX mode, the function address, and a bus reset may be left on
> POWER. `fm1_usb_reset` alone (the SIE's reset line) may not clear the OTG block's host state. `uh_host_down` now
> clears them (EP1's FIFO flushed too) before `fm1_usb_off`, and `usb_retry` makes the port a device a second later
> instead of at once, so a computer plugged in meanwhile sees a clean attach (as a cable plugged in). Fixed so on the
> unit in ChoralRoot (its HISTORY 3y262). The SIE model checks the registers after Off and the second's wait.

## felucca-bsp: `felucca-bsp/0008-usb-device-suspend-lost.patch` (2026-11-08)

`chip/usb.c`, `docs/USB_HOST.md`, `tests/usb_host_test.c`. PR title: **usb.c: a configured device's bus suspend
raises midi_usb_lost**

> `midi_usb_lost` (fm1-base's `plat_usb_lost`) asks the app to let go of the notes and pedal USB holds, and is
> raised when a host session ends. A computer that sleeps, or a cable pulled, ends a device's session the same way:
> the note-offs of what it held never come. A configured port's suspend (the suspend interrupt, or 100 ms of a
> frozen frame number) now raises it too, once per suspend; an unconfigured one (nothing was held) does not. Not in
> the update loader (its `ota.bin` is as before). A firmware that does not read `plat_usb_lost` sees no change.
> ChoralRoot does the same on the unit (its HISTORY 3y261: two FM-1s on a plain cable, the device kept the host's
> note). The SIE model checks a configured and an unconfigured suspend.

## felucca-bsp: `felucca-bsp/0009-usb-host-root-reset-retry.patch` (2026-11-08)

`chip/usb_host.c`, `docs/USB_HOST.md`, `tests/usb_host_test.c`. PR title: **usb_host.c: a 50 ms root reset, and a
device that does not answer reset again with a longer recovery; NO MIDI only for one that answered**

> `uh_connect` reset the root port for 30 ms, waited 30 ms and probed once; any failure was NO MIDI, once per
> attach. A second FM-1 on a plain cable (ChoralRoot, which serves USB from a 2 kHz timer) was "not MIDI" to
> RemoteFM1's host, while ChoralRoot's own host, which does the following, plays it. The reset is now 50 ms (USB
> 2.0's TDRSTR), the recovery 100 ms; a device that did not answer its first GET_DESCRIPTOR is reset again with the
> recovery doubled (100, 200, 400, 800, 1600 ms), up to `UH_ROOT_TRIES` (5) times, stopping as soon as it answers.
> NO MIDI is said only for a device that answered without a USB-MIDI interface; one that never answered says
> nothing (the port is still looking; no new message, so `UH_MSG_*` and fm1-base's `USBH_MSG_*` stay as they are).
> Enumeration blocks the main loop: a silent device now takes about 3.35 s of it, once per attach, where it took
> 60 ms. Hub ports are as before (ChoralRoot's idle retry rounds for a downstream device that did not finish are a
> larger change, not taken). The SIE model gains a device that answers 300 ms after a reset (found by the third)
> and one that never answers (five resets, nothing said); the failed-probe test expects five resets.

## felucca-bsp: `felucca-bsp/0002-uac-out-gain-glide.patch`

> **chip/usb.c: glide the USB audio output's gain across a block.** `uac_play` applied MASTER's gain as one value
> per render block, so turning MASTER stepped the level at each block's edge and crackled (heard on headphones and
> the speaker, RemoteFM1 0.2.1 on an FM-1, 2026-10-27). The gain now moves linearly from the last block's value to
> this one's, sample by sample. No change while MASTER is still. Applies after 0001; useful to every fm1-base
> firmware with `usb_audio_out`.

## felucca-bsp: `felucca-bsp/0003-uac-in-stall-by-time.patch`

> **chip/usb.c: FELUCCA_USB_MS(), the audio input's stall check by time.** A queued EP4 IN packet that waits more than
> 20 ms means the host stopped reading. Counted in polls (`FELUCCA_UAC_STALL_POLLS`, 40 at the 2 kHz timer), a
> firmware that also polls from its main loop, at no fixed rate, would drop the stream every frame; with
> `FELUCCA_USB_MS()` (a millisecond clock) the check is 20 ms whoever polls. Without the macro nothing changes.

## felucca-bsp: `felucca-bsp/0004-led-two-steady-levels.patch`

> **fm1_input.h: two more steady LED levels (fm1_led_lv, fm1_led_levels).** Each lit on the first 0..8 of every 8
> scan frames (~114 Hz), for keys shown at two brightnesses (RemoteFM1: a scale's root and its notes) beside the glow
> and `fm1_led_mid`. Zero (the default) changes nothing.

## felucca-bsp: `felucca-bsp/0006-ota-identity-whole.patch` (2026-11-07)

`chip/ota.c`. PR title: **ota.c: FELUCCA_ID_WHOLE, the whole identity to every handshake**

> Since 41a6e86 a plain handshake gets the identity cut to the name, `_` and three digits (M-UPGRADE 3.11's limit),
> and only a handshake with the body 01 the whole one. A firmware whose own installers read its version from the
> plain handshake (RemoteFM1's web installer and `fm1_install.py`: `FM-1_52000303` is 0.3.3, `FM-1_520` nothing) defines
> `FELUCCA_ID_WHOLE` and answers as before 41a6e86. The loader's reply is whole anyway. Without the macro nothing
> changes.

## fm1-base: `fm1-base/0001-ble-sysex-lcd-idle-sxq-host-sysex.patch`

`ble/midi/le_midi.c`, `device/radio.c`, `device/ble_none.c`, `device/lcd.c`, `device/unity.c`, `include/plat.h`,
`host/plat_host.c`. PR title: **SysEx over Bluetooth MIDI both ways; plat_ble_name, lcd_idle; the SysEx ring and the
Bluetooth queues sizable by the app; the simulator's `sysex` command**

> - **SysEx over Bluetooth MIDI**, both ways (it was dropped: `midi_parse` skipped it, `ble_midi_send_many` sent a
>   SysEx packet as a broken channel message). In: `midi_parse` turns a SysEx into USB-MIDI packets for
>   `ble_midi_receive` (CIN 4 three bytes that go on, 5 / 6 / 7 the last one, two or three), as a USB host sends them,
>   across BLE packets (a packet that goes on with a SysEx starts with its data), with real-time bytes inside and a
>   status ending it; per link, reset on a disconnect. The parser was rewritten around it: running status kept apart
>   from real time (a data byte after a clock no longer loops forever), a stray data byte skipped. Out: a SysEx's
>   bytes as they are, a timestamp before F0 and before F7, none before the data, a message going on in the next
>   notification where one is full. The app sees SysEx through `plat_midi_in` on `PORT_BLE` and sends it with
>   `plat_midi_send` (`include/plat.h` says how).
> - `device/radio.c`: a SysEx packet that finds the in-queue full loses the rest of its message too, so an app gets a
>   SysEx whole or cut short (the next message ends it), never with a hole in it. The queues are sizable by the app:
>   `BLE_IN_Q`, `BLE_OUT_Q` (powers of two; default 64 as before): SysEx comes as packets of three bytes.
> - `plat_ble_name(name)`: the name the FM-1 advertises ("FM-1 1F2D"), for an app's screen; `""` without Bluetooth.
>   The simulator makes it the same way from its own "chip ID".
> - `lcd_idle()` (device `lcd.c`, and the simulator): the last blit's transfer is over, for an app that never waits
>   for the panel (it blits again only then). Uses felucca-bsp's `fm1_lcd_xfer_done()`.
> - `device/unity.c`: `SXQ` (the SysEx send ring) only when the app's `app.h` did not give it.
> - The host simulator: `sysex [PORT] F0 .. F7`, a SysEx message in on Bluetooth (or TRS) as packets through
>   `plat_midi_in`, or on USB to the app's `app_sysex`.
>
> Tested: RemoteFM1's `tests/ble_sysex_test.c` runs this `le_midi.c`'s MIDI part (cut out of the file) on a computer:
> every SysEx length 0..400 through the encoder at ATT payloads of 20 and 64 and back through the parser, between
> notes and clocks; another sender's packets (continued SysEx, a clock inside, cut by a note, running status); 50,000
> random packets under ASan/UBSan. RemoteFM1's simulator scripts drive its protocol over this path. Not yet on an FM-1
> at the time of writing (RemoteFM1 0.2 is its first user).

## fm1-base: `fm1-base/0003-ble-radio-dle-2m-rx-interval.patch` (being measured; not for upstream yet)

`ble/midi/le_midi.c`, `tools/build.py`. PR title: **Bluetooth MIDI: optional Data Length Extension, 2M PHY, a larger
receive pool and a 7.5 ms interval request (fm1.toml ble_dle, ble_2m, ble_rx_bufs, ble_fast_interval)**

> JieLi's controller has Data Length Extension and the 2M PHY (`ll_data_extension.c`, `ll_multiple_phys.c` in
> `btctrler.a`), but the SDK's `lib_btctrler_config.c` says it has neither (`config_btctler_le_features =
> LE_ENCRYPTION`): `ll_hci_set_data_length` and `ll_hci_set_phy` return at once without the feature bits, and a
> central sees no DLE or 2M in the feature exchange, so every link runs 27-byte packets at 1M. With the options off a
> firmware builds as before. `ble_dle`: the feature, the controller's packet length 251 (the SDK's comment; its
> transmit buffers grow from 700 to 2940 B of the Bluetooth heap), and `le_midi.c` asks the computer for 251 octets
> once the ATT MTU is exchanged. `ble_2m`: the feature, and `le_midi.c` asks for 2M after the data length's change (or
> at the CCC write). `ble_rx_bufs = N`: the controller's receive pool, N × 294 B (the SDK's 5): a 251-byte packet
> takes 271 B of it until the stack has taken it, the two the radio receives into 275 B each. `ble_fast_interval`:
> 7.5..22.5 ms asked first, then the table as before. `FM1B_BLE_RADIO="dle,2m,rx16,ci"` sets them for a test build.

## fm1-base: `fm1-base/0004-ble-radio-readout.patch` (test only)

`ble/midi/le_midi.c`: `ble_midi_radio(b)`, 40 bytes of what the computer's link negotiated (interval, latency,
timeout, ATT MTU, the last data length change, the PHY, the connection-parameter result, the Bluetooth heap), for
RemoteFM1's STATS_R (docs/PROTOCOL.md section 5). Drop it with `firmware/src/rfm_main.c`'s `RF_RADIO_N` part.

## fm1-base: `fm1-base/0005-app-dsp-in-ram.patch`

`device/app.ld`, `tools/build.py`. PR title: **An app's DSP sections in the RAM code; the RAM code's calls checked by
their targets**

> - `app.ld`: `.dsp_text` and `.audio_text`, the sections an app's DSP code may name (saadet's FM6 does), go in
>   `.ram_text` (RAM, copied at boot, inside the PC guard's window), out of the XIP cache's way. Before, such code was an
>   orphan section, outside the image `tools/build.py` makes. No change for an app without those sections.
> - `tools/build.py`: the check that RAM code makes no calls (the flash driver runs with the flash off) now lets a call
>   through when it stays in RAM: a direct call whose target is inside `.ram_text`, or an indirect one (`call rN`) when
>   no instruction of the RAM code takes the address of a function in the flash. Any other call is reported as before
>   (an error without the radio, a note with it, for JieLi's `.volatile_ram_code`, which calls `memcpy` in the flash).
>   The second core's loop (`.c1_text`) stays exempt. The note says how many calls stayed in RAM.

RemoteFM1 standalone (STANDALONE-PLAN.md S0b) runs saadet's FM6 so (`firmware/third_party/saadet-fm6`): ~2.9 KB of
RAM code, 13 calls, all within RAM (FM6's own function pointers between its RAM functions, and its DC filter's helper).
FIZZ (the owner's) places its RAM code in the `.c1_text` window instead, which the check leaves out entirely; this
patch keeps FM6 under the check.

## fm1-base: `fm1-base/0006-app-store-window-and-mux.patch`

`device/unity.c`, `device/plat_fm1.c`, `host/plat_host.c`, `include/plat.h`. PR title: **The store kept to an app's
own window; an app's objects as records of one of its own**

> - `APP_STORE_OK(off, n)` (app.h): where `st_erase` / `st_prog` may write, instead of felucca-bsp's `FL_STORE_OK`.
>   An app that shares the FM-1 with other firmwares keeps its objects where none of them keeps data, and nothing of
>   it can ever write elsewhere. The simulator refuses a save whose object's sectors (`APP_STORE_MAP`, A and B) are
>   outside it, counted (`expect store_outside`), so a test catches an object placed outside.
> - `APP_STORE_MUX` (app.h): the platform's four `plat_store_*` are renamed `plat_store_raw_*` and the app defines
>   `plat_store_*` on them: it can keep several objects (fm1-base's own `OBJ_BLE` among them) as records inside one
>   A/B pair when it has only two sectors to its name.
>
> Without either macro nothing changes.

RemoteFM1 (src/rfm_store.c): every object in 0xF2000..0xF3FFF, the one window ChoralRoot, the stock firmware and
fm1-base's other firmwares leave free, plus the calibration's pair at 0xFC000. RemoteFM1 0.2 placed `OBJ_BLE` at 0xF2000,
outside `FL_STORE_OK`: every save of the radio's address, trim and bondings was refused (-8) on the unit.

## felucca-bsp 0005-cpu1-idle-relax.patch (2026-10-30)

Core 1's idle loop and CPU0's wait spun on shared RAM with csync nonstop; with USB audio on, macOS then never
created the FM-1's audio devices (enumeration and MIDI were fine). A short register-only pause per pass, as saadet's
fm1_multicore.h does (MPL), fixes it: checked on a unit (both FM-1 audio devices, core 1 on, render hash equal).

## fm1-base: `fm1-base/0008-build-sorted-ble-sources.patch` (2026-11-04)

`tools/build.py`. PR title: **build.py: link the Bluetooth sources in a sorted order**

> `tools/build.py` globs `ble/shim/*.c`, `ble/midi/*.c` and `ble/wifi/*.c` without sorting, so the link order, and so
> the image's bytes, follow the file system's directory order: APFS on a Mac and ext4 on GitHub's runner give
> different packages from the same source. Sorting them (as the lwIP sources already are) makes a Bluetooth build
> reproducible anywhere, which signed release builds (The Firmware Hub's fm1-build) need. A USB-only build does not
> change.

RemoteFM1 0.3.2's Bluetooth package is the first built with it (docs/RELEASING.md); FIZZ sorts the same lists in its
own `firmware/build.py`.

## fm1-base: `fm1-base/0009-flash-b36014.patch` (2026-11-04; dropped 2026-11-07)

`device/main_fm1.c`: the boot check took 0xB36014 (the UC25HQ80 of the FM-1s with the logo on the front) beside the
Puya's 0x856014. RemoteFM1 0.3.3 was the first with it. fm1-base now does the same itself (`device/fm1_flash_ok.h`,
`FM1B_FLASH_JEDEC_OK`, fm1-base #35), so the patch is gone; the SMK-37 series defines `FM1B_FLASH_JEDEC_OK` for its
board (0xEB6014, 0x856014, 0xB36014) before that header.

## fm1-base: `fm1-base/0009-firmware-json-repo-root.patch` (2026-11-07)

`tools/firmware_json.py`. PR title: **firmware_json.py: FM1B_REPO_ROOT, the repository for a layout of its own**

> firmware.json's `screenshot` is a path in the firmware's repository, and the build checks the file there: the
> parent of an APP_DIR named `app`, else APP_DIR. RemoteFM1's app is `firmware/` (and the SMK-37's a copy of it in
> `build/app-smk37/`), so its `docs/hub-card.png` could not be named, and a `--release` build refused.
> `FM1B_REPO_ROOT` in the environment names the repository instead (RemoteFM1's `build.sh` sets it). Unset, nothing
> changes.
