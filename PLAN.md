# RemoteFM1 — plan (draft, 2026-10-17)

RemoteFM1 is an FM-1 firmware that does almost nothing by itself. It reports every key, button, knob and encoder to a
computer, and lets the computer drive every LED and the whole screen. You can run an FM-1 firmware on a Mac, in a
browser or on an iPad, and use the real FM-1 as its panel without flashing the unit again.

Status: a plan only. Nothing is built yet. This repo is a clone of TransitFM1 (history kept, remote `transit`).
The owner's answers of 2026-10-17 are in section 9.

## 1. What it is, and what it is not

- The FM-1 becomes a USB terminal. Input goes up to the host, LEDs and pixels come down from it. The device runs no
  synth and no UI of its own.
- On the host, any firmware with an **fm1core** build runs as it does today in the emulator and in FM1VST: ChoralRoot,
  Felucca and Melodee now, Sloop later. The real panel replaces the emulator's on-screen panel.
- **The stock M-VAVE firmware cannot run on the host.** It is a pi32v2 binary and there is no CPU emulator for it
  (FM1-VST-PLAN phase 6, research). "Any FM-1 firmware" here means any firmware built from source as a core.
- Without a host, the unit shows a "waiting for host" screen with the battery level and its version. It keeps the
  update path, so `fm1_install.py` can put ChoralRoot back over USB, without the Transporter.

## 2. Hardware facts the design rests on

From ChoralRoot `firmware/hal` and `src/lcd.c`, and `tools/emu/emu_hooks.h`:

| Part | Fact |
|---|---|
| Screen | ST7789, 240×240, RGB565, on SPI1 at 20 MHz. One full frame is 115,200 B and takes **46 ms** to send over SPI, so about 21 fps at most for full frames |
| Input | 11×6 diode matrix: 27 note keys, 14 buttons, 7 detent encoders (SELECT, PRESETS, ALGORITHM, KNOB 1–4), and the MASTER pot on the ADC (0..1023). Keys 0..19 sit on the analog rows, which give velocity and pressure ("lean", MONK-1 / Velo) |
| LEDs | One colour each, on 11 columns × 4 lines, plus PLAY's green LED. TransitFM1's scan has four levels (off, backlight, glow, lit); ChoralRoot's has five (off, faint, dim, mid, lit) |
| USB | JieLi USB0, **full speed** (12 Mbit/s). TransitFM1: EP1 = MIDI bulk only; its `usb.c` also has CDC (EP2 notify, EP3 bulk) and UAC IN (EP4 iso) behind flags. **All SIE traffic is polled from the TIMER5 ISR at 2 kHz**, i.e. at most one 64 B packet per endpoint every 0.5 ms (128 KB/s). RemoteFM1 must poll far more often (see section 3) |
| RAM | 96 KiB `.data/.bss` plus a 336 KiB POOL. Without a synth, a full 115 KB frame buffer fits easily |
| Base size | TransitFM1's app body is about 56 KB, including its own 7.5 KB update loader |

The emulator already defines the contract. `emu_hal_t` has an "emulator → firmware" half (keys, buttons, taps, encoder
detents, master, key velocity and lean) and a "firmware → emulator" half (the four LED bitmaps, the green PLAY LED and
the 240×240 RGB565 panel). RemoteFM1 is that struct sent over USB: the device produces the first half and consumes the
second.

## 3. Bandwidth: 20 Hz needs compression

- Raw frames at 20 Hz: 115,200 × 20 = **2.3 MB/s**.
- Full-speed USB bulk has a ceiling of about 1.0–1.2 MB/s in practice.
- SysEx over USB-MIDI carries 3 data bytes per 4-byte event, 7 bits each, so at most about 65 % of the bulk rate. CoreMIDI,
  Web MIDI and iPadOS each add their own limits, which are **not measured yet** (M0).
- So 20 Hz full raw frames cannot happen on any transport. They need **dirty tiles plus cheap per-tile encodings**,
  modelled on VNC:
  - 16×16 tiles (225 per frame). The host keeps an exact copy of what the device shows and sends only the tiles that
    changed.
  - Each tile picks the smallest of: FILL (one colour), PAL2 / PAL4 / PAL16 (a small palette plus 1, 2 or 4 bits per
    pixel), RLE565, or RAW565. A flat FM-1 UI (Inter Tight text, a few colours) is mostly FILL and PAL tiles.
  - The host has a byte budget per frame. If a frame is over budget (a full-screen animation), it sends the oldest
    changed tiles first and the rest on the next frames. The screen catches up, nothing is dropped, and the whole
    screen stays correct.
  - Flow control by credit: the device acknowledges each frame number, and the host keeps at most 2 frames in flight.
    Latency stays bounded and the USB queue never builds up.
- The device does nothing else, so its main loop can service USB continuously (or from the USB IRQ) instead of
  TransitFM1's 2 kHz timer poll. M0 measures how far the SIE then goes.
- The device decodes one tile into a 512 B buffer and sends it to the panel with CASET/RASET. Neighbouring tiles in the
  same row are merged into one window. A 115 KB shadow in the POOL allows a later COPYRECT (scrolling) op.

## 4. Transport (decided: MIDI + vendor bulk)

| Transport | Mac native | Chrome / Edge | Safari | iPad / iPhone | Bandwidth |
|---|---|---|---|---|---|
| USB-MIDI (SysEx) | CoreMIDI | Web MIDI | no Web MIDI | CoreMIDI, class compliant | lowest (to measure) |
| Vendor bulk (EP3) | IOKit / libusb | WebUSB | no | no (no generic USB on iOS) | highest |
| USB audio OUT (iso) | yes | no | — | yes | ~1 MB/s, but a hack |

One protocol (the same message bytes) carried two ways:
- **USB-MIDI always.** Input and LEDs are tiny and need the lowest latency (1 ms bulk polling), and MIDI is the only
  transport that works everywhere, iPad included. Frames also go over SysEx, at a lower rate if M0 says so.
- **Vendor bulk on EP3 as the fast frame path** wherever it is available (the Mac app, Chrome). The host uses it
  whenever it can open it. It takes the endpoint of TransitFM1's CDC option, a vendor-class interface (0xFF) with a
  WebUSB descriptor, so Chrome can open it.
- USB names: the device keeps TransitFM1's "FM-1" product string and its FM-1_015 update identity. That way every
  installer can write over it, and the unit can always be moved back to ChoralRoot or stock. The host does not go by
  the name: it finds RemoteFM1 with a HELLO SysEx, which other firmwares ignore. (To check in M0: that stock V15 and
  ChoralRoot really ignore it.)

Input as MIDI also makes the unit a plain USB MIDI controller in any DAW, as a bonus:
- Keys: Note On / Off on channel 16 (velocity from the analog rows), poly aftertouch for lean.
- Buttons: Note On / Off on channel 15. Encoders: relative CC on channel 15 (signed detents). MASTER: a 14-bit CC
  pair.
- SysEx for everything else: HELLO / capabilities (version, unit ID, screen, LED map, keymap), battery, an optional
  stream of raw analog readings (for firmwares with their own velocity curves, such as ChoralRoot `cr_sense`), and
  frame acknowledgements.

Host to device: LED frame (5 levels per LED, about 30 bytes packed), TILES (frame number, tile list, encodings),
PRESENT, plus the existing ChoralRoot update / OTA SysEx, unchanged.

## 5. Repository and base

- `RemoteFM1/` is a clone of **TransitFM1** (8eed8aa), history kept: Dinghy → TransitFM1 → RemoteFM1. GPL-3.0-only;
  LICENSING.md gets a RemoteFM1 row. The TransitFM1, ChoralRoot and Felucca trees stay read-only.
- What TransitFM1 brings and RemoteFM1 keeps unchanged: ChoralRoot's boot stub byte for byte (`stub.lock`), its own
  staged update loader (the next firmware's first sector written last), the boot guard, the M-UPGRADE updater path,
  `tools/build.py`, the Docker build, and `tests/run_tests.sh`.
- What RemoteFM1 adds: the remote protocol (`firmware/src/remote.c`), the vendor interface in `usb.c`, faster USB
  service, the tile decoder, a "waiting for host" screen in place of `transit.c`'s, and a HELLO answer.
- What it may take from ChoralRoot later (copied, credited): the five-level LED scan and the analog-row velocity
  sense (MONK-1 / Velo), for the raw-analog stream. The protocol carries five LED levels from the start; the device
  maps them to the levels its scan has.
- **TransitFM1 0.1 has never run on an FM-1.** So the first RemoteFM1 flash also tests the TransitFM1 base on the
  unit: M0 follows TransitFM1's device test plan (docs/SAFETY.md) before anything else.

## 6. Host side

- `host/` — a small portable C library, `libremotefm1`: protocol encode and decode, the tile encoder with its copy of the
  device screen, flow control, and the transport back ends. It builds for macOS, iOS and WASM.
- `fm1remote` — a CLI for macOS. It loads a `.fm1core` (with FM1VST's `engine/` CoreLoader and Device), runs it in real
  time, plays the audio on the Mac, and connects the device to `emu_hal_t`. This is the fastest way to iterate.
- FM1VST — a "Remote FM-1" switch in the plugin and the Standalone. The physical panel then drives the core, and the
  plugin's own panel mirrors it.
- Browser — the existing WASM emulator (`web/emu_web.c`) plus Web MIDI (and WebUSB in Chrome), as a page.
- iPad / iOS — later, through JUCE's iOS target or a small native app on CoreMIDI.

## 7. Milestones

- **M0 — base on the unit, and measure.** First TransitFM1's own device test plan on the unit (it boots, OCT- update
  mode, ChoralRoot installs over it and back). Then a measuring build of RemoteFM1 that only echoes and counts: SysEx
  and vendor bulk throughput both ways with USB serviced from the main loop, and SPI blit time per tile. Measured from
  the Mac (CoreMIDI, libusb), Chrome (Web MIDI, WebUSB) and an iPad if one is at hand. The numbers set the frame
  budget in section 3.
- **M1 — device firmware.** Input to MIDI, LED frames, tile decoder, waiting screen, HELLO, update path. Tested first
  on the host: the firmware builds natively against the emulator's HAL stand-ins (as ChoralRoot's `tools/emu` does),
  and a fake host drives it through scripted frames. Then on the unit.
- **M2 — host library and `fm1remote`.** ChoralRoot, Felucca and Melodee cores running on the Mac with the FM-1 as
  their panel. Acceptance: 20 Hz on ChoralRoot's normal screens, key-to-sound latency measured, a screen that is
  always correct after any burst.
- **M3 — FM1VST "Remote FM-1".**
- **M4 — browser page** (Web MIDI, with WebUSB when it is there).
- **M5 — options:** audio from the host out of the FM-1's own output (UAC playback), iPad app, COPYRECT.

## 8. Rules carried over

- Agents write the code from briefs; only the orchestrator flashes.
- The owner (2026-10-17): RemoteFM1 may go on **any unit that is plugged in**. Still: one device session at a time,
  `--info` first, a full backup of that unit before its first RemoteFM1 flash, and the checks afterwards. A stock unit
  is backed up through the Transporter (it has no backup protocol).
- Never run `tools/emu/test_cr.sh` from two agents at once (ChoralRoot's harness, if M1 reuses it).

## 9. Decisions (owner, 2026-10-17)

1. Base: **TransitFM1** (the owner meant it; `FM-1-transporter` was the wrong path).
2. Transport: **USB-MIDI everywhere plus vendor bulk** where it works.
3. Audio: **later, as an option** (M5). The sound stays on the computer at first.
4. Test unit: **any unit that is plugged in** (section 8).

## 10. M0 log

**2026-10-17, the TransitFM1 base on unit 5BD75BE3** (before any RemoteFM1 code):
- Backup first: `fm1-backups/5BD75BE3-before-remotefm1-20261017/` (ChoralRoot 0.25.1 data backup, plus the 0.25.1
  release package, sha256 `ef57eacc…22504d5`, as the way back).
- TransitFM1 0.1 rebuilt bit for bit (`transit-0.1.fwsc` sha256 `9bcee26a…`; stub `baee55ed…` v3, sector 0x4000
  `0f33c838…`, the same as ChoralRoot's). `tests/run_tests.sh` with V15 and ChoralRoot 0.25.1: all passed
  (ChoralRoot ↔ TransitFM1: 2279 cut points, 0 unprotected).
- SAFETY.md test 1, ChoralRoot 0.25.1 → TransitFM1 with ChoralRoot's installer: done, "the FM-1 runs FM-1_015". The USB
  device is named "FM-1". **CoreMIDI keeps the old port name** ("ChoralRoot FM-1", the same VID/PID), so the host must
  find RemoteFM1 by HELLO and never by port name.
- SAFETY.md test 5, TransitFM1 → ChoralRoot 0.25.1: done, `--debug` "boot stub v3: booted the app … marker ok". A
  second data backup equals the first except for its timestamp: TransitFM1 left the stored data alone.
- Not done (they need hands on the unit): the screen check, test 2 (OCT- at power-on), test 4 (cable pull), test 3
  (to V15) and test 6 (another firmware's installer).

**2026-10-17, the M0 bench (branch `m0-bench`, b93228c, `build/remote.fwsc` sha256 `ef8248a5…`):**
- First flash over ChoralRoot 0.25.1: written. The unit enumerated with the new descriptors (bcdUSB 2.10, bcdDevice
  3.50, interfaces 0 audio control, 1 MIDI streaming, 2 vendor), so macOS accepts them. CoreMIDI then kept the port
  offline until a MIDI rescan (MIDIRestart). This will happen whenever the descriptors change: the host app should
  offer the rescan.
- The bench never answered: by then the owner had put TransitFM1 back on the same unit for their own TransitFM1 tests
  (TransitFM1 adcfcde). No numbers yet.
- The second flash, from TransitFM1, stopped in step 1 after 2 requests, with nothing written. TransitFM1 bug:
  nothing drains its MIDI-in ring, so after ~40 events from a DAW (Live was open) EP1 OUT is NAKed until power-off,
  and the updater's SysEx is shut out with it. RemoteFM1 drains the ring. TransitFM1's fix is proposed as its own
  session.
- After the owner power-cycled the unit (Live no longer sending), two installs from TransitFM1 (remote.fwsc, then
  ChoralRoot 0.25.1) each stopped in step 1 after the 2 header reads, with "nothing was written". After the second
  one the unit left MIDI and came back as the chip's ROM USB boot `WL80UBOOT1.00` (no MIDI port). The installer never
  sends the UBOOT soft key, and no other tool ran. So TransitFM1's own fallbacks put it there: the boot guard (a
  reset in its USB rescue → ROM) or the stub (rescue with no staged loader → ROM). The TransitFM1 on the unit is the
  owner's, from the web installer, after their V15 / Bubba Machine round trip. Its step 1 re-stages its own loader
  (it erases 0xE0000.. first) when the staged one is not its own. Suspects: that re-stage, or a watchdog reset while
  step 1 waits. The app area should be intact; a single power-cycle should boot it again. Otherwise the Transporter.
- One power-cycle from UBOOT gave TRANSIT USB RESCUE (the boot guard's second failed boot); another power-cycle, the same,
  and no red CRASH screen either time. So the failed boots are not faults: most likely a hang before `lcd_init` (the
  boot-time `ota_boot_cleanup` / `ota_own_staged` / `ota_restage` flash work) until the watchdog resets it. RemoteFM1
  has the same boot code: find the cause before relying on it. Next: OCT- at power-on (the stub hands over to the
  staged loader, bypassing the app), install ChoralRoot 0.25.1 through the loader; if the stub goes to UBOOT
  instead, the Transporter (dump first).
- OCT- at power-on: three flashes, then the ROM's UBOOT (the staged loader failed too). **Transporter dump**
  (`fm1-backups/5BD75BE3-uboot-transporter-20261017/`, sha256 `d67dc997…`, both reads identical): app == TransitFM1
  0.1 exactly; **staging area 0xE0000..0xE4FFF entirely erased**; a stray TransitFM1 loader at 0xE6000 (V15's
  staging place, from the owner's V15 → TransitFM1 hop); 0x93000..0xE5FFF erased (the ChoralRoot user data did not
  survive the owner's V15 / Bubba Machine round trip).
- **Root cause (TransitFM1 and RemoteFM1 alike):** `ota_restage` passes `TRANSIT_LDR`, a `const` array in XIP flash
  (0x02006064), straight to `ota_prog` → `fl_prog_ram`, which reads its source while the flash is out of XIP for
  programming. The first page faults, before `lcd_init` (so no visible crash screen), twice → USB rescue, whose
  step 1 re-stages again → fault → ROM. It never ran on a unit before: every earlier boot found its own loader
  staged (installed by ChoralRoot, which stages the package's loader). It runs after any hop that stages elsewhere
  (V15 at 0xE6000). Fix: copy each page into a RAM buffer before `ota_prog` (and check `ota_stage_head`).
- Recovered: Transporter `restore` of ChoralRoot 0.25.1, 134 sectors written, final read equal to the expected image
  (sha256 `c1ed23bd…`).

**2026-10-17 late, M0 measured** (m0-bench 68d2785, `remote.fwsc` sha256 `15e4266a…`, installed from ChoralRoot
0.25.1 on 5BD75BE3; USB serviced from the main loop, mode 1; macOS, CoreMIDI via python-rtmidi, libusb 1.0.30;
raw JSON in `results/m0/`):

| | SysEx (USB-MIDI, CoreMIDI) | Vendor bulk (libusb) |
|---|---|---|
| host → device, payload | 222 KB/s (32 B msgs) … 313 KB/s (512 B) … 375 KB/s (8 KB) | 598 KB/s (64 B transfers) … 830 KB/s (≥ 4 KB) |
| device → host, payload | 350 KB/s (32 B) … 490 KB/s (512 B) | 630 KB/s (64 B) … 855 KB/s |
| round trip, small message | median 0.53 ms, p99 0.71 ms | median 0.19 ms, p99 0.35 ms |
| raw 16×16 tiles to the screen | 656 tiles/s = 336 KB/s = 2.9 raw screens/s | 1708 tiles/s = 854 KB/s = 7.6 raw screens/s |

- SPI blit (BLIT_BENCH): full frame 51.4 ms at 20 MHz (baud 2, 19.5 fps), 82.1 ms at 12 MHz (baud 4, the bench's
  default). 225 separately windowed 16×16 tiles cost only ~4 % more than one full frame (53.4 ms), and 15 strips
  51.5 ms. **Use baud 2**; per-tile windows are nearly free.
- The main loop polls USB ~115,000–132,000 times a second and spends ~36–47 % of its time in it.
- Frame budget at 20 Hz: bulk ≈ 41 KB per frame (36 % of a raw frame), SysEx ≈ 15 KB per frame (13 %). Bulk carries
  a typical UI at 20 Hz with FILL / PAL tiles easily; SysEx carries it for mostly-static screens and degrades to the
  oldest-first catch-up under full-screen animation. As planned in section 3.
- The update path survives: `--info` answered during and after the benches; ChoralRoot 0.25.1 installed over
  RemoteFM1, stub "marker ok", the stored data unchanged.
- The descriptors came up with the MIDI port online this time; the first install's offline port was CoreMIDI's.
- **Bench bug to fix in M1:** SysEx SINK messages of 2048 and 8192 B arrive exactly one byte short each (512 B and
  under are whole), so every one fails its checksum. Find whether the host (rtmidi / CoreMIDI splitting) or the
  device (usb.c SysEx parsing) drops it. The updater's frames are ≤ 640 B, but a real parsing bug would matter there.
- Not measured yet: Chrome (Web MIDI, WebUSB: `web/m0_bench.html`), an iPad, the 2 kHz poll (`remote-usb0.fwsc`),
  input latency from the panel.

**2026-10-18, M1 built** (branch `m1`: m1-device + m1-host + the cross test, `build/remote.fwsc` sha256 `13035039…`;
stub, sector 0x4000 and loader `68599862…` unchanged):
- Device (RemoteFM1 0.1, protocol 2, docs/PROTOCOL.md): session with keepalive, waiting screen, LEDS (five levels →
  this HAL's three), TILES into a 115,200 B shadow with all five encodings (`rfm_draw.c`, host-compilable), a
  non-blocking push of dirty tiles at 20 MHz with USB served throughout (`rfm_push.c`), FRAME_ACK, PANEL_R, panel
  events as plain MIDI. **The M0 SysEx bug is fixed and confirmed on the unit**: macOS sends a long SysEx's odd byte
  as CIN 0xF; the old parser dropped it. Now `cin_f` = 1 per 2 KB / 8 KB message and 0 checksum errors.
- Host (`host/`): libremotefm1 (portable core: codec, session, panel, screen encoder; macOS back ends CoreMIDI and
  libusb) and `fm1remote` (an .fm1core on FM1VST's engine, the FM-1 as panel, audio on the Mac). 9 ctest suites.
- Cross test (`host/tests/test_cross.c`): the host session and encoder against the device's own rfm_app / rfm_draw /
  rfm_push, both transports, with faults; 202,187 checks. It found one misreading per side (device: no shadow-reset
  bit after power-on; host: dropped 1-count MASTER corrections), both fixed, and the spec now says so.
- Through the real device code: a UI frame ~1.9 KB, a single value ~240 B, real core screens from black 2.3–10.6 KB
  in one frame; full-screen noise settles in 3 frames on bulk, 9 on SysEx. Device draw 7.5 ms for a UI frame, ~45 ms
  for a full repaint.
- The unit ran the m1-device build (496808d2…) briefly: HELLO, SysEx fix, blit OK. The owner then took it for
  TransitFM1 tests. Next: flash `m1` (13035039…) and run `fm1remote --device --port FM-1 --stats` (first `--pattern`,
  then ChoralRoot, then `--transport midi`, Felucca, Melodee); input latency by hand.
- **2026-10-18, first live runs** (`m1` 13035039… on 5BD75BE3, installed over TransitFM1 0.1.3; RemoteFM1 re-staged its
  own loader at first boot from RAM, HELLO "loader 1"; the XIP fix works on hardware):
  - `fm1remote --device --pattern`: bulk + MIDI found by HELLO at once; 20.0 fps sent. UI pattern 360–480 B per
    frame, ACK 4–11 ms. Full-screen gradient animation ~35 KB per frame at the bulk limit, ACK ~96 ms, 2 in flight,
    the oldest-first catch-up working. Clean start / stop.
  - `fm1remote --device --core choralroot` (FM1VST's 2026-10-08 core, fresh flash), 120 s: the session up, ChoralRoot's
    boot screen in 1,250 B, nothing sent while it is still, no errors, flash saved on exit. **Input not tested yet**
    (nobody at the unit): next run with the owner playing.
  - **Second ChoralRoot run, the owner playing (120 s): "Screen is good, input good".** 302 panel messages on the
    port (a second CoreMIDI client listened beside fm1remote): all 14 buttons, all 7 encoders (every detent arrived),
    11 different keys; 294 input events reached the core, its screen and LEDs followed (up to ~13 fps while
    playing, ~1.3 KB per frame, ACK ≤ 68 ms, no errors). MASTER sat at its minimum (0) during the run; read
    afterwards with PANEL_REQ it gave 528 at about half way, so the pot reads correctly.
  - **M1 done.** Open: MIDI CC 7/39 from a MASTER move not watched live on the unit (covered by the host and cross
    tests); battery mV not calibrated (0, cap off); Felucca / Melodee and `--transport midi` not run on the unit yet;
    key-to-sound latency not measured.
