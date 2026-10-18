# RemoteFM1 protocol, version 2 (M1)

The contract between the RemoteFM1 firmware (`firmware/`) and every host (`host/`: the C library, `fm1remote`, later
the browser and iPad). The constants are in `proto/rfm1.h`. The framing helpers (7-in-8 packing, the bulk header) are
M0's, in `firmware/src/remote_proto.h`, unchanged. Version 1 was the M0 bench. Version 2 keeps every M0 command
number and adds the session, LED, screen and panel messages.

Both files are **owned by the orchestrator**. An agent that finds a problem in the spec reports it and does not
edit them.

## 1. Transports and framing

One message = a command byte + a body (8-bit bytes, multi-byte fields **little endian** unless the field says BE).

| Transport | Framing | Where |
|---|---|---|
| SysEx over USB-MIDI (EP1) | `F0 7D 52 46 <cmd> <body, 7-in-8 packed> F7` | every host (CoreMIDI, Web MIDI, iPad) |
| Vendor bulk (interface 2, EP3 OUT / EP3 IN, 64 B packets) | `'R' 'F' <cmd> 00 <len16> <seq32> <body>` (10-byte header) | macOS (libusb), Chrome (WebUSB) |

- A SysEx body is at most `RFM_SX_BODY_MAX` (1024) bytes before packing. A bulk body is at most `RFM_BK_BODY_MAX`
  (16384) bytes.
- The device answers on the transport the request came in on. Panel events always go out as plain MIDI (section 4),
  whatever transport the session uses.
- A host may use both: plain MIDI for the panel events and bulk for everything else. That is the recommended setup
  wherever bulk is available.

## 2. Session

- **HELLO** (`0x01`, body: optional host name, ≤ 16 bytes) → **HELLO_R** (`0x41`). The 51 bytes of M0 come first,
  unchanged. `name` is now "RemoteFM1" and `proto` is 2. Then come these fields:

  | offset | field |
  |---|---|
  | 51 | `screen_w16` (240), `screen_h16` (240) |
  | 55 | `caps16`: bit0 shadow frame buffer, bit1 COPY, bit2 RLE, bit3 PAL, bit4 battery reading, bit5 PLAY green LED, bit6 five LED levels (else: lit / dim / faint, mid shown as dim) |
  | 57 | `max_frames_in_flight8` (2) |
  | 58 | `session8` (1 while a session is active) |
  | 59 | `fw_ver[8]` (the RemoteFM1 version string) |

  Length `RFM_HELLO_LEN` = 67. A host finds the unit **only** by HELLO, never by port name: CoreMIDI keeps the port
  name of whatever firmware it saw first.

- **SESSION** (`0x10`, body: `start8 keepalive_ms16`).
  - `start = 1`: the host takes over. The device clears its screen and shadow to black (0x0000), turns all LEDs
    off, resets the frame numbering, sends **PANEL_R**, and from then on shows only what the host sends.
  - `start = 0`: the session ends; the device goes back to its waiting screen.
  - `keepalive_ms`: if no message of any kind arrives from the host for this long, the device ends the session by
    itself. 0 means never; a host normally uses 3000.
  - A USB reset or unplug also ends the session.
  - Reply: **ACK** (`0x46`, empty).

- **Waiting screen** (no session): "REMOTE FM1", the version, "WAITING FOR HOST", the battery when known, and a red line
  when the update loader isn't staged. The key LEDs glow dim so the panel can be found. The device runs no other UI.

## 3. Host → device: LEDs and screen

### LEDS (`0x11`), body 45 bytes

`lit[11] mid[11] dim[11] faint[11] flags8`. Each array is one byte per matrix column 0..10, in the same packed row
bits as the firmware's `fm1_led[col]`, which is the emulator's `emu_hal_t.led / led_mid / led_dim / led_faint`.
An LED in several arrays shows the brightest: lit > mid > dim > faint. `flags` bit0: PLAY's green LED on. A device
with fewer levels maps them down: TransitFM1's scan has lit, glow and backlight, so lit → lit, mid and dim → glow,
faint → backlight. No reply. A host sends LEDS when they change, and at least once a second while a session is
active.

### TILES (`0x12`): the screen

Body: `frame16 flags8 nrects8`, then `nrects` rectangles, each `x8 y8 w8 h8 enc8 payload`.

- `1 ≤ w`, `1 ≤ h`, `x + w ≤ 240`, `y + h ≤ 240`. A rectangle never spans two messages. A frame may span many TILES
  messages with the same `frame` number.
- `flags` bit0 **END**: this message completes frame `frame`. The device pushes everything drawn since the last END
  to the panel, then sends **FRAME_ACK**. A frame may also be empty: a TILES with `nrects = 0` and END is a pure
  flush/ack.
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

**After the device resets or a session starts,** the host's model of the screen is all black, and the host sends
what differs.

## 4. Device → host

### Panel events: plain MIDI on EP1, always (in a session or not)

| what | message |
|---|---|
| note key `k` (0..26, matrix id `14 + k`, `emu_hal_t.keys` bit `k`; key 0 = F3) | Note On `9F 53+k 100` / Note Off `8F 53+k 0` (channel 16) |
| button `b` (matrix id 0..13, `emu_hal_t.buttons` bit `b`: 0 OCT-, 1 OCT+, 2 FX, 3 SEL, 4 ENV, 5 LFO, 6 EDIT, 7 GLO, 8 HOME, 9 SAVE, 10 ARP, 11 SEQ, 12 PLAY, 13 REC) | Note On `9E b 127` / Note Off `8E b 0` (channel 15) |
| matrix encoder `e` (0..6, `emu_hal_t.enc[e]`, + clockwise) | `BE 16+e 64+d`, with `d` the detents since the last report, clamped to −63..+63, never 0 |
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
| 20 | `master16` |
| 22 | `battery_mv16` (0 = unknown) |
| 24 | `flags8` (bit0 session active) |

Length `RFM_PANEL_LEN` = 25.

- Sent on PANEL_REQ (`0x13`, empty body), when a session starts, and every 500 ms during a session, on the session's
  transport.
- A host that sees a PANEL_R disagree with what the events told it believes PANEL_R. For encoders it uses the
  difference between counters.

### FRAME_ACK (`0x52`)

Body: `frame16 status8 draw_us32 bytes32 pending8`.

- `status` bit0: a decode error in this frame. bit1: the shadow was reset (a new session, or the device restarted),
  so the host's model is invalid and the host sends a full frame.
- `draw_us`: decode plus push time. `bytes`: the frame's body bytes received. `pending`: frames the device has
  queued.
- Length `RFM_ACK_LEN` = 12.

## 5. Kept from M0 (diagnostics)

ECHO `0x04` / ECHO_R `0x44`, STATS `0x05` / STATS_R `0x45`, STATS_RESET `0x06`, BLIT_BENCH `0x07` / BLIT_R `0x47`,
SINK `0x02`, SOURCE `0x03` / DATA `0x43` / SOURCE_DONE `0x50`, and M0's raw TILE `0x08`. They work as in M0, in and
out of a session.

## 6. Updater

The M-UPGRADE SysEx path (`F0 00 59 …`, the soft key, `fm1_install.py`) is untouched and works at any time, in a
session or not. The device answers it as FM-1_015, as TransitFM1 does. A host must never send it by accident: every
RemoteFM1 message starts `F0 7D 52 46`.
