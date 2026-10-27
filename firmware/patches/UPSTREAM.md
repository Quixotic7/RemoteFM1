<!-- SPDX-License-Identifier: MPL-2.0 -->
# RemoteFM1's patches to fm1-base and felucca-bsp

RemoteFM1 builds on [fm1-base](https://github.com/charlesvestal/fm1-base) (`base/`, a submodule pinned at `934e40d`)
and [felucca-bsp](https://github.com/hugelton/felucca-bsp) (`base/bsp/`, pinned by fm1-base at `2cd696a`). The
submodules stay as they are. `../../build.sh` copies `base/` to `build/fm1-base/` (rsync: only what changed), applies
the patches below with `git apply` (exactly, or the build stops), and builds from the copy. Every patch is a change to
MPL-2.0 files, published under MPL-2.0 like the files it changes, and each is meant to go upstream: the texts below
are the pull requests. Once a change is merged upstream, move the submodule to that commit and drop its patch.

To change a patch: edit the files in `build/fm1-base` (or a copy of `base/`), then diff against `base/` as the files
here are made (`diff -ruN`, paths `a/` and `b/` from the submodule's root, no timestamps).

Both patches are opt-in for everyone else: a firmware that does not define the new macros builds byte for byte as
before (checked: fm1-base's template, and felucca-bsp's update loader `ota.bin`, are identical with and without them),
except the Bluetooth MIDI SysEx, which is new behaviour for every Bluetooth firmware (it was dropped before).

## felucca-bsp: `felucca-bsp/0001-vendor-interface-and-app-hooks.patch`

`chip/usb.c`, `boards/fm1/fm1_lcd_hw.h`. PR title: **usb.c: an optional vendor bulk interface (WebUSB), SysEx and
EP1 hooks for apps; fm1_lcd_done()**

> RemoteFM1 (an FM-1 firmware that is only the panel of a firmware running on a computer) moves frames of its
> screen at 20 Hz: up to ~40 KB a frame, more than USB-MIDI carries. Its 0.1 (on TransitFM1) had a vendor-specific
> bulk interface beside USB-MIDI, which Chrome's WebUSB and libusb open; this brings it to felucca-bsp's `usb.c` as an
> option, with the small hooks an app needs to take its own SysEx at full speed. Nothing changes without the new
> macros (the update loader and fm1-base's template build byte for byte as before).
>
> - `FELUCCA_VENDOR=1`: a vendor-specific interface (class 0xFF) as interface 2, after audio control and MIDI
>   streaming, with a bulk OUT / IN pair on EP3 (the CDC option's endpoint: not with CDC, UAC, the USB host or in the
>   loader, `#error` otherwise). bcdUSB 2.10 and a BOS descriptor with the WebUSB platform capability (vendor code 1,
>   no landing page), bcdDevice 3.50. Two rings (`FELUCCA_VENDOR_IN_N` / `_OUT_N`, powers of two, in
>   `FELUCCA_VENDOR_RING_ATTR`) served by `usb_poll`; the app's side: `usb_vendor_avail`, `usb_vendor_read`,
>   `usb_vendor_room`, `usb_vendor_send` (a message whole or not at all). EP3 OUT is NAKed while the in-ring is full;
>   a short packet ends each burst out (no ZLP needed); a bus reset drops what waits to go out.
> - `FELUCCA_SYSEX_TAP(b)`: every SysEx byte from USB-MIDI (and a status that ends one), in `usb_poll`'s context.
>   Nonzero: the message is the app's, and is not collected for `ota.c` from that byte on, so a long stream of the
>   app's SysEx never holds the updater's one-frame slot (`sx_frame`), and messages longer than 640 bytes reach the
>   app whole.
> - `FELUCCA_EP1_ROOM()`: before an EP1 OUT packet is taken; 0 leaves it for a later poll (NAKed). An app uses it to
>   hold the host while its own SysEx ring is full, and to empty the MIDI-in ring from `usb_poll`'s context when it
>   plays no USB MIDI in (so a DAW's clock can never fill the ring and shut the updater out while an update session
>   keeps the main loop away).
> - `FELUCCA_USB_FRAME_DUE()`: when the 10 ms frame-number check is due. An app that calls `usb_poll` from its main
>   loop (100,000 times a second and more) needs it by the clock: by poll count, the frame number looked frozen and the
>   bus "suspended".
> - `FELUCCA_CIN_F_SYSEX`: a CIN 0xF packet (one unparsed byte, USB-MIDI 1.0 section 4) inside a SysEx, or with an F0,
>   is taken as a SysEx byte. macOS sends a long SysEx's odd byte at a split so: without it, a 2 KB message arrives one
>   byte short (RemoteFM1's M0 measured it). The update's frames are short enough not to hit it, which is why it is
>   opt-in here; consider making it the default.
> - `fm1_lcd_done()` in `boards/fm1/fm1_lcd_hw.h`: the running transfer is over (a look, no wait), for an app that
>   never waits for the panel (fm1-base's `lcd_idle()`).
>
> Tested: RemoteFM1's `tests/usb_midi_test.c` (the parser fed every SysEx length 6..3000, macOS's split, clocks inside,
> the tap's claim against the updater's frame, EP1 held), both with and without `FELUCCA_CIN_F_SYSEX`; the loader's
> `ota.bin` is identical with the patch. On an FM-1: RemoteFM1 0.1 ran the same descriptors and vendor service
> (M0: ~830 KB/s bulk with the main loop polling).

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
>   for the panel (it blits again only then). Uses felucca-bsp's `fm1_lcd_done()`.
> - `device/unity.c`: `SXQ` (the SysEx send ring) only when the app's `app.h` did not give it.
> - The host simulator: `sysex [PORT] F0 .. F7`, a SysEx message in on Bluetooth (or TRS) as packets through
>   `plat_midi_in`, or on USB to the app's `app_sysex`.
>
> Tested: RemoteFM1's `tests/ble_sysex_test.c` runs this `le_midi.c`'s MIDI part (cut out of the file) on a computer:
> every SysEx length 0..400 through the encoder at ATT payloads of 20 and 64 and back through the parser, between
> notes and clocks; another sender's packets (continued SysEx, a clock inside, cut by a note, running status); 50,000
> random packets under ASan/UBSan. RemoteFM1's simulator scripts drive its protocol over this path. Not yet on an FM-1
> at the time of writing (RemoteFM1 0.2 is its first user).
