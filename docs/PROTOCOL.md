# RemoteFM1 protocol, version 2 (M1; firmware 0.1 and 0.2)

The contract between the RemoteFM1 firmware (`firmware/`) and every host (`host/`: the C library, `fm1remote`, the
site's player in the browser, FM1VST). The constants are in `proto/rfm1.h`. The framing helpers (7-in-8 packing, the
bulk header) are M0's, in `firmware/src/remote_proto.h`, unchanged. Version 1 was the M0 bench. Version 2 keeps every
M0 command number and adds the session, LED, screen and panel messages.

Firmware 0.2 (on fm1-base and felucca-bsp) still speaks version 2. What it changed is additive: a third transport
(Bluetooth MIDI), two HELLO_R transport bits, its own identity, and fewer LED levels; 0.2.2 adds TILES' HOLD flag
(caps bit7), so that a host can show each screen change at once. Section 7 lists it.

Both files are **owned by the orchestrator**. An agent that finds a problem in the spec reports it and does not
edit them.

## 1. Transports and framing

One message = a command byte + a body (8-bit bytes, multi-byte fields **little endian** unless the field says BE).

| Transport | Framing | Where |
|---|---|---|
| SysEx over USB-MIDI (EP1) | `F0 7D 52 46 <cmd> <body, 7-in-8 packed> F7` | every host (CoreMIDI, Web MIDI, iPad) |
| Vendor bulk (the class 0xFF interface, EP3 OUT / EP3 IN, 64 B packets) | `'R' 'F' <cmd> 00 <len16> <seq32> <body>` (10-byte header) | macOS (libusb), Chrome (WebUSB) |
| SysEx over Bluetooth MIDI (0.2, the Bluetooth build) | as USB-MIDI's: `F0 7D 52 46 <cmd> <body, 7-in-8 packed> F7` | CoreMIDI, Web MIDI, iPad, once the FM-1 is connected as a Bluetooth MIDI device |

- A SysEx body is at most `RFM_SX_BODY_MAX` (1024) bytes before packing, over USB and over Bluetooth. A bulk body is
  at most `RFM_BK_BODY_MAX` (16384) bytes.
- **The vendor interface's number** depends on the build: 2 in 0.1 and 0.2.0 (after audio control and MIDI
  streaming); 4 from 0.2.1 (after USB audio in and out: 0 audio control, 1 MIDI, 2 audio in, 3 audio out; bcdDevice
  4.50). A host finds it by its class (0xFF, vendor specific), never by its number. It has a WebUSB BOS descriptor
  (vendor code 1, no landing page), bcdUSB 2.10.
- **Bluetooth MIDI** carries the same messages, in BLE-MIDI packets of at most 64 bytes (the ATT payload), at a few
  KB/s: enough for HELLO, a session, the panel's events, LEDS and a still or slowly changing screen, not for a moving
  one. The device never waits for the link: a reply that finds no room waits in a short queue (six messages with a
  body of at most 72 bytes; every session reply fits) and goes when there is room; a longer reply that finds no room
  is dropped. M0's diagnostics with long replies (SOURCE, STATS) are for USB. The device advertises "FM-1 XXXX" (four characters of its
  chip ID) and shows that name on its waiting screen; hosts find it as any FM-1 port, by HELLO (section 2). The
  updater (section 6) is USB only.
- **Bluetooth: how a host paces frames.** A host tells a Bluetooth link from the HELLO_R that came over it: `sxq16`
  (byte 29) is the free send packets on the transport asked, 128 over Bluetooth, 960 over USB (3584 in M0 and 0.1),
  so `sxq <= 256` with `transports` bit4 set means Bluetooth (bit4 alone does not: it is set on USB replies too while
  a Bluetooth computer is connected). Measured on a unit (2026-10-27, macOS): about 6 KB/s of TILES body host →
  device, FRAME_ACK after 100–500 ms; its Bluetooth input (256 USB-MIDI packets, drained by the main loop) loses
  packets when more than about 4 KB is sent unacknowledged (3.4 KB was clean). Whole messages are then lost: the
  FRAME_ACK says fewer bytes, or a later frame's comes first. libremotefm1 paces a Bluetooth frame link so: 5500 B/s
  of TILES body (6.0–6.3 KB/s measured with 1 KB frames, ACKs back in < 100 ms; ~6.3 KB/s framed, the rest left for
  LEDS, keepalives, PANEL_R and BLE MIDI's packet headers), a budget of 550 bytes a frame (100 ms of the rate), at
  most 20 frames a second (a small change goes within 50 ms), the next frame no sooner than its body at the rate
  allows, at most 2048 framed TILES bytes unacknowledged (one frame alone always goes; a frame is at most ~640 bytes
  on the wire; on a link slower than the rate this cap, not the rate, sets the pace), a FRAME_ACK timeout of
  max(2 s, 500 ms + 3 × what is in flight at the rate), and SESSION retried after 1 s. With a USB port of the same
  unit it uses that one. USB-MIDI and bulk keep their pacing (about 280 KB/s and 800 KB/s, at most 2 frames
  unacknowledged; `host/include/rfm`).
- **Bluetooth: lossy tiles (host side only; the wire format is unchanged).** Over Bluetooth libremotefm1 sends a
  16×16 tile with more than 4 colours (`ble_colors`, 0 = lossless, 2..16) as its best 4 (median cut + k-means on the
  tile's own colours, each drawn in a colour the tile has) when that encodes smaller, mostly PAL at 2 bpp instead of
  4 bpp: a ChoralRoot page change of 11 KB becomes 6.4 KB (2.0 s → 1.2 s at 5500 B/s). Anti-aliased text loses
  some of its in-between greys; background and text colours stay exact. The host's model holds what the device
  shows and, per tile, the screen it was sent for; a tile is sent again only when that screen changes, so a still
  screen sends nothing and nothing flickers. Once no tile has changed for 1 s (`ble_refine_ms`), the reduced tiles
  are sent again exactly, one frame in flight at a time, so a change is never queued behind more than one frame;
  a change drops the refinement, which starts again when the screen is still. The device sees ordinary TILES
  either way.
- **Bluetooth: whole screen changes (0.2.2, `RFM_CAP_HOLD`; host option `ble_hold`, on by default).** A page change
  takes 0.5–2 s over Bluetooth, many frames, and a device that shows every frame as it comes shows it appearing in
  pieces (and, while a knob changes big text, half old and half new). To a unit with caps bit7, libremotefm1 sends
  each change as a **snapshot**: the screen is latched when the change's first frame goes; every frame of it that
  leaves tiles for later frames ends with END + HOLD (section 3: drawn into the shadow, not shown); the frame with
  its last tiles ends with END alone and shows the whole change at once. A screen submitted while a snapshot is part
  sent waits; when the snapshot is shown, the newest screen is the next one (those in between are skipped), so a
  turning knob shows whole screens at whatever rate the link allows. A change that has sent nothing yet may still be
  replaced by a newer screen. Refinement frames (exact colours once still) are not held: each shows at once, the
  same screen with its colours exact. A frame lost or late (a FRAME_ACK timeout, a FRAME_ACK out of order or short)
  and a session start end the snapshot: everything is sent again, from the newest screen. The unit shows what it
  holds by itself after `RFM_HOLD_MS` (1500 ms) without TILES, so a host that stalls mid-change never freezes it
  (at the paced rate the gap between frames is ~0.1–0.5 s). USB-MIDI and bulk never hold, and their bytes are as
  before; a unit before 0.2.2 (no bit7) is sent no HOLD.
- The device answers on the transport the request came in on. Panel events go out as plain MIDI (section 4) on the
  session's MIDI port.
- A host may use both on USB: plain MIDI for the panel events and bulk for everything else. That is the recommended
  setup wherever bulk is available.
- Plain MIDI sent to the device (notes, clock) is ignored, over USB and over Bluetooth: RemoteFM1 plays nothing.

## 2. Session

- **HELLO** (`0x01`, body: optional host name, ≤ 16 bytes) → **HELLO_R** (`0x41`), on the transport HELLO came in
  on. The 51 bytes of M0 come first (`firmware/src/remote_proto.h`):

  | offset | field |
  |---|---|
  | 0 | `name[16]`: "RemoteFM1" |
  | 16 | `proto8`: 2 |
  | 17 | `ver[8]`: the firmware's version ("0.2.2") |
  | 25 | `transports8` (`RFM_HELLO_TRANSPORTS`, bits `RFM_TR_*`, in rfm1.h from 0.2): bit0 USB configured, bit1 vendor bulk built in, bit2 RemoteFM1 SysEx heard over USB, bit3 bulk heard; 0.2: bit4 a Bluetooth computer connected, bit5 RemoteFM1 SysEx heard over Bluetooth |
  | 26 | `usb_mode8`: 1, the main loop polls USB |
  | 27 | `lcd_baud8` |
  | 28 | `loader_state` (int8): 0 updates work; -2 an unknown flash chip (no updates). 0.1: the state of its staged loader |
  | 29 | `sxq16`: SysEx send packets free for RemoteFM1 on the transport asked (USB's ring less the 64 kept for the updater, or Bluetooth's queue, 128) |
  | 31 | `bk_rx_ring32`, then 35 `bk_tx_ring32`: the vendor rings (32768 each) |
  | 39 | `sx_body_max16` (1024), then 41 `bk_body_max16` (16384): the longest body taken; a host keeps to them |
  | 43 | `identity[8]` (`RFM_HELLO_IDENTITY`): the first 8 characters of the update identity, "FM-1_520" for 0.2's `FM-1_52XXYYZZ` (0.1: "FM-1_015") |

  `name` is "RemoteFM1" and `proto` is 2 (M0 answered "RemoteFM1-M0", 1). Then come these fields:

  | offset | field |
  |---|---|
  | 51 | `screen_w16` (240), `screen_h16` (240) |
  | 55 | `caps16`: bit0 shadow frame buffer, bit1 COPY, bit2 RLE, bit3 PAL, bit4 battery reading, bit5 PLAY green LED, bit6 five LED levels (else: lit / dim / faint, mid shown as dim), bit7 TILES' HOLD flag (0.2.2, section 3) |
  | 57 | `max_frames_in_flight8` (2) |
  | 58 | `session8` (1 while a session is active) |
  | 59 | `fw_ver[8]` (the RemoteFM1 version string) |

  Length `RFM_HELLO_LEN` = 67. A host finds the unit **only** by HELLO, never by port name: CoreMIDI keeps the port
  name of whatever firmware it saw first. Over Bluetooth the port is named after what the FM-1 advertises
  ("FM-1 7938"), so a host sends HELLO to every port whose name holds "FM-1".

  `caps` from 0.2: 0x2F (shadow, COPY, RLE, PAL, PLAY green); from 0.2.2: 0xAF (and HOLD). A build without the
  shadow (`RFM_SHADOW 0`, for a build short of RAM) answers 0x2C: no shadow, no COPY, no HOLD. 0.2 has no battery
  reading (bit4 off, `battery_mv` 0) and three LED levels (bit6 off, section 3).

- **SESSION** (`0x10`, body: `start8 keepalive_ms16`).
  - `start = 1`: the host takes over. The device clears its screen and shadow to black (0x0000), turns all LEDs
    off, resets the frame numbering, sends **PANEL_R**, and from then on shows only what the host sends.
  - A SESSION body shorter than 3 bytes means keepalive 0; an empty body is ignored.
  - `start = 0`: the session ends; the device goes back to its waiting screen.
  - A SESSION start during a running session (a host retrying after a lost ACK) restarts the session: black
    shadow, LEDs off, frame numbering reset.
  - `keepalive_ms`: if no RemoteFM1 message of any kind (SINK included) arrives from the host for this long, the device ends the session by
    itself. 0 means never; a host normally uses 3000.
  - A session lives on the transport its SESSION start came in on: its PANEL_Rs go there, and its panel events go
    to that transport's MIDI port (section 4). A SESSION start on another transport moves the session there (a
    restart, as above).
  - A USB reset or unplug ends a USB session (SysEx or bulk); a Bluetooth disconnect ends a Bluetooth session. Neither
    ends the other's.
  - Reply: **ACK** (`0x46`, empty).

- **Waiting screen** (no session): "REMOTE FM1", the version and the identity, "WAITING FOR HOST", USB's state (no
  host, idle, host connected), in the Bluetooth build the advertised name and Bluetooth's state (connected, ready,
  starting, failed, off), the battery level or "powered over USB", the transports' rates and errors, and "UNKNOWN
  FLASH: NO UPDATES" when the flash chip is not the expected one (0.1: a red line when its loader wasn't staged). The
  key LEDs glow dim so the panel can be found, and a key or button lights while it is held. The device runs no other
  UI.

## 3. Host → device: LEDs and screen

### LEDS (`0x11`), body 45 bytes

`lit[11] mid[11] dim[11] faint[11] flags8`. Each array is one byte per matrix column 0..10, in the same packed row
bits as the firmware's `fm1_led[col]`, which is the emulator's `emu_hal_t.led / led_mid / led_dim / led_faint`.
An LED in several arrays shows the brightest: lit > mid > dim > faint. `flags` bit0: PLAY's green LED on. A device
with fewer levels maps them down (`caps` bit6 off): TransitFM1's scan (0.1) has lit, glow and backlight, so lit → lit,
mid and dim → glow, faint → backlight; fm1-base's (0.2) has lit, a steady mid (about 12 %) and the glow (about 3 %),
so lit → lit, mid and dim → mid (with the glow under it), faint → the glow. Mid and dim look the same on both. No
reply. A host sends LEDS when they change, and at least once a second while a session is
active.

### TILES (`0x12`): the screen

Body: `frame16 flags8 nrects8`, then `nrects` rectangles, each `x8 y8 w8 h8 enc8 payload`.

- `1 ≤ w`, `1 ≤ h`, `x + w ≤ 240`, `y + h ≤ 240`. A rectangle never spans two messages. A frame may span many TILES
  messages with the same `frame` number.
- `flags` bit0 **END**: this message completes frame `frame`. The device pushes everything drawn since the last END
  to the panel, then sends **FRAME_ACK**. A frame may also be empty: a TILES with `nrects = 0` and END is a pure
  flush/ack.
- `flags` bit1 **HOLD** (0.2.2, `RFM_TILES_F_HOLD`, only where `caps` bit7 says so; meaningful only with END): the
  frame is drawn into the shadow and acknowledged when decoded (FRAME_ACK at once: `draw_us` is the decode alone),
  but nothing is pushed: the panel keeps what it shows. The next END **without** HOLD pushes everything drawn since
  the last push, held frames included, so the panel changes once, to the whole new screen (an empty END shows them
  too). In a session that has used HOLD the device reads no more of that transport's input while a push runs or is
  due (~22 ms for the whole screen), so a push never shows tiles of the frames after it. If no TILES arrives for
  `RFM_HOLD_MS` (1500 ms) while tiles are held, the device shows them by itself. A session start or end drops held
  tiles (never shown); outside a session, and in a build without the shadow, HOLD is ignored (an END as before).
  Units before 0.2.2 ignore bit1 (every flag but END), so a frame with HOLD is shown there as it comes: a host still
  sets it only when `caps` has bit7. Other flag bits are reserved (0) and ignored.
- Rectangles are applied **in order** into the device's shadow frame buffer (the screen as the host believes it
  is). Pixels are RGB565.

| enc | name | payload |
|---|---|---|
| `0x00` | RAW | `w*h` pixels, each **BE** 16-bit (as the panel takes it and as `emu_hal_t.lcd` stores it) |
| `0x01` | FILL | one **BE** 16-bit colour |
| `0x02` | PAL | `n8` (2..16), `n` × BE colour, then indices: `bpp` = 1 for n ≤ 2, 2 for n ≤ 4, 4 for n ≤ 16. Row-major; **each row starts on a byte boundary**; MSB-first within a byte |
| `0x03` | RLE | runs `len8 c16BE` (run length `len + 1`, 1..256) until exactly `w*h` pixels; runs continue across rows, left to right, top to bottom |
| `0x04` | COPY | `sx8 sy8`: the `w×h` block at (sx, sy) of the shadow, as it stands when this rectangle is applied, is copied to (x, y). Overlap behaves as `memmove` |

On a malformed rectangle (bad bounds, an unknown enc, a short payload, PAL `n` out of range, RLE overrun) the device
drops the rest of that message, still honours END if it was set, and reports the error in FRAME_ACK.

**Flow control.** The host keeps at most `max_frames_in_flight` (2) frames sent but not acknowledged. The device must
keep servicing USB while it pushes pixels (the SPI DMA runs in the background; the main loop keeps polling USB).

**Outside a session** the device decodes TILES into its shadow and acknowledges them, but doesn't push them (the
waiting screen stays), and it ignores LEDS.

**After the device resets or a session starts,** the host's model of the screen is all black, and the host sends
what differs.

## 4. Device → host

### Panel events: plain MIDI, always (in a session or not)

They go to the session's MIDI port: USB-MIDI (EP1) for a session over USB SysEx or bulk, Bluetooth MIDI for a session
over Bluetooth. With no session they go to USB (when configured) and to a connected Bluetooth computer. They wait in
a short queue (256 events) when a port has no room, and are dropped when nobody is connected.

| what | message |
|---|---|
| note key `k` (0..26, matrix id `14 + k`, `emu_hal_t.keys` bit `k`; key 0 = F3) | Note On `9F 53+k 100` / Note Off `8F 53+k 0` (channel 16) |
| button `b` (matrix id 0..13, `emu_hal_t.buttons` bit `b`: 0 OCT-, 1 OCT+, 2 FX, 3 SEL, 4 ENV, 5 LFO, 6 EDIT, 7 GLO, 8 HOME, 9 SAVE, 10 ARP, 11 SEQ, 12 PLAY, 13 REC) | Note On `9E b 127` / Note Off `8E b 0` (channel 15) |
| matrix encoder `e` (0..6, `emu_hal_t.enc[e]`, + clockwise) | `BE 16+e 64+d`, with `d` the detents since the last report, never 0; a turn of more than 63 detents is **split** into several messages of at most ±63, so nothing is lost. Deltas are in matrix sense: `emu_hal_t.enc[e]` moves by exactly `d` |
| MASTER pot (ADC 0..1023, `emu_hal_t.master`) | `BE 07 v14>>7` then `BE 27 v14&0x7F`, with `v14 = adc << 4`. Sent on a change of ≥ 2 counts, at most every 5 ms, and always when a session starts |

- Events go out in the order they were detected, with no batching delay.
- A DAW sees the same messages, so RemoteFM1 also works as a plain MIDI controller.

### PANEL_R (`0x53`): the whole panel, for resync

Body:

| offset | field |
|---|---|
| 0 | `keys32` |
| 4 | `buttons16` |
| 6 | `enc[7]` (int16, absolute detent counters since boot, wrapping) |
| 20 | `master16`: the ADC value 0..1023 (not v14), exact. The host takes it even inside the MIDI deadband |
| 22 | `battery_mv16` (0 = unknown) |
| 24 | `flags8` (bit0 session active) |

Length `RFM_PANEL_LEN` = 25.

- Sent on PANEL_REQ (`0x13`, empty body), when a session starts, and every 500 ms during a session, on the session's
  transport.
- Every 500 ms is also the host's heartbeat: no PANEL_R for about 1.6 s, or one without the session flag, means
  the session is gone.
- A host that sees a PANEL_R disagree with what the events told it believes PANEL_R. For encoders it uses the
  difference between counters.

### FRAME_ACK (`0x52`)

Body: `frame16 status8 draw_us32 bytes32 pending8`.

- `status` bit0: a decode error in this frame. Malformed input includes bytes after the last rectangle and a PAL
  index ≥ n. A TILES body too short to carry a frame number gets no ACK, and the next FRAME_ACK has bit0 set.
- `status` bit1: the shadow was reset. It is set on the first FRAME_ACK after a session start **and** on the first
  FRAME_ACK after power-on, in a session or not. The host's model is then invalid, and the host sends a full frame.
- `draw_us`: decode plus push time.
- `bytes`: the sum of that frame's TILES body lengths, headers included.
- `pending`: ENDs still queued after this one. One push may cover several ENDs; each still gets its own ACK, in
  order.
- Length `RFM_ACK_LEN` = 12.

Idea for a later version, not in protocol 2: FRAME_ACK status bit2 "no session", so a host notices a lost session
at once instead of through the heartbeat.

## 5. Kept from M0 (diagnostics)

ECHO `0x04` / ECHO_R `0x44`, STATS `0x05` / STATS_R `0x45`, STATS_RESET `0x06`, BLIT_BENCH `0x07` / BLIT_R `0x47`,
SINK `0x02`, SOURCE `0x03` / DATA `0x43` / SOURCE_DONE `0x50`, and M0's raw TILE `0x08`. They work as in M0, in and
out of a session.

## 6. Updater

The M-UPGRADE SysEx path (`F0 00 59 …`, the soft key, `fm1_install.py`) is untouched and works at any time over USB,
in a session or not; RemoteFM1's own messages never hold its frame slot, and 64 packets of the USB SysEx send ring are
always left to its replies. 0.2 answers it as itself, `FM-1_52XXYYZZ` (release X.Y.Z, two digits each; a development
build `FM-1_5200`), on the USB device "FM-1", and updates through felucca-bsp's loader (`ota-FM-1_900`). 0.1 answered
as `FM-1_015`, as TransitFM1 does. Bluetooth carries no update. A host must never send it by accident: every
RemoteFM1 message starts `F0 7D 52 46`.

## 7. What 0.2 added (all within version 2)

- Bluetooth MIDI SysEx as a third transport (section 1), in the Bluetooth build; HELLO_R `transports` bits 4 and 5.
- Sessions per transport: panel events to the session's port, a Bluetooth disconnect ends a Bluetooth session.
- The vendor interface's number: 4 from 0.2.1, with USB audio in and out beside it (2 before). Found by class.
- The identity `FM-1_52XXYYZZ` (HELLO_R `identity` "FM-1_520"); `loader_state` -2 for an unknown flash chip.
- Three LED levels (mid and dim merge), no battery reading: `caps` 0x2F.
- 0.2.2: TILES' HOLD flag (section 3), `caps` bit7 (0xAF), `RFM_HOLD_MS`; libremotefm1 sends a Bluetooth screen
  change whole (section 1).
