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
> - `FELUCCA_VENDOR=1`: a vendor-specific interface (class 0xFF) as the last interface: 2 after audio control and
>   MIDI streaming, or after the audio streams with `FELUCCA_UAC` (3; 4 with `FELUCCA_UAC_OUT`), with a bulk OUT / IN
>   pair on EP3 (the CDC option's endpoint: not with CDC, the USB host or in the loader, `#error` otherwise; UAC's
>   endpoint is EP4, and its EP4 OUT is polled, so the interrupt enables are EP1's and EP3's as before). bcdUSB 2.10
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
> the tap's claim against the updater's frame, EP1 held), both with and without `FELUCCA_CIN_F_SYSEX`; the vendor
> interface's descriptors alone (0.1's byte for byte) and after USB audio in + out (the configuration walked: five
> interfaces in order, each endpoint in one, the vendor pair on interface 4); the loader's `ota.bin` is identical with
> the patch. On an FM-1: RemoteFM1 0.1 ran the same descriptors and vendor service
> (M0: ~830 KB/s bulk with the main loop polling).

## felucca-bsp: `felucca-bsp/0002-uac-out-gain-glide.patch`

> **chip/usb.c: glide the USB audio output's gain across a block.** `uac_play` applied MASTER's gain as one value
> per render block, so turning MASTER stepped the level at each block's edge and crackled (heard on headphones and
> the speaker, RemoteFM1 0.2.1 on an FM-1, 2026-10-27). The gain now moves linearly from the last block's value to
> this one's, sample by sample. No change while MASTER is still. Applies after 0001; useful to every fm1-base
> firmware with `usb_audio_out`.

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
