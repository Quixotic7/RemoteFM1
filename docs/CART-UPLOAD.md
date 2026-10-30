# Sending a DX7 cartridge to the FM-1 (user banks)

RemoteFM1 0.3.0's standalone instrument keeps a DX7 32-voice bulk dump sent to it as a user bank, USER1..USER3
(firmware `sa/sa_ubank.c`), listed after the 8 ROM cartridges (GLO + SELECT). A fourth replaces the oldest. From
0.3.1 (branch `more-storage`): 16 banks, and a query of the capacity and the stored banks' names (PROTOCOL.md section
8 there). Installing ChoralRoot or the stock firmware overwrites the window: the banks are gone and must be sent again.
The website does this from the player's settings sheet, USE MY FM-1 (`web/site/js/cart.js`).

## The bytes

One SysEx message, exactly 4104 bytes:

| Offset | Bytes | |
|---|---|---|
| 0 | `F0 43 0n 09 20 00` | Yamaha, sub-status 0 / channel n (any 0..15; send 0), format 9 (32 voices), byte count 0x2000 (`20 00`) |
| 6 | 4096 | 32 packed voices (VMEM), 128 bytes each, every byte 0..0x7F; voice k's name is bytes 118..127 of it (ASCII) |
| 4102 | 1 | checksum: `(128 - (sum of the 4096 bytes & 0x7F)) & 0x7F` |
| 4103 | `F7` | |

The unit drops anything else: a wrong length, header (`20 00`) or checksum, or a data byte >= 0x80. Many cartridge
files carry a wrong checksum: recompute it before sending (the site does, and says "checksum corrected"). A file may
hold several dumps back to back; send each as its own message, one at a time (see pacing).

## How to send it (a host app, e.g. the iOS app)

1. **The unit must be the instrument, not in a session.** The dump is collected only while `sa_standalone()` is true
   (`src/rfm_main.c`): with no host session. If your app holds a RemoteFM1 session (HELLO / SESSION), end it first
   (the session's end message, or close the transport) and wait ~0.5 s.
2. **Transport**: USB-MIDI (the "FM-1" port; never the updater's port) is the fast, sure way. Bluetooth MIDI works
   too (the firmware collects BLE SysEx as well), but at a few KB/s and with a 256-packet input: pace it (e.g. 512
   bytes, then wait ~100 ms) or prefer USB. CoreMIDI: one `MIDISendSysex` (or `MIDIEventList` of SysEx UMP / a
   `MIDIPacketList`) with the whole 4104 bytes is fine over USB.
3. **No reply.** 0.3.0 sends no ack. On a good dump the unit writes the flash (two sector erases, ~100 ms), switches
   to the new bank, selects its first voice and shows the preset screen (`USERn`): that is the confirmation for the
   user. A dump that arrives while the previous one is still being written is dropped: wait ~1 s between banks.
4. **After**: tell the user the limits (3 banks on 0.3.0, the oldest replaced; 16 from 0.3.1). When 0.3.1's query is
   there, ask it after sending to show what is stored (the site's hook: `queryUserBanks` in `web/site/js/cart.js`).

Never ship cartridge `.syx` files in the app or the repos: users bring their own.

## If an ack is wanted

0.3.0 is released, so the site does without one. A later firmware could answer a kept bank on the transport it came
in on, e.g. a RemoteFM1 section-8 message `F0 7D 52 46 <cmd> <slot8 status8 seq8> F7` sent from `sa_ub_take()`'s
caller in `sa_main.c` (status 0 kept, 1 bad checksum, 2 flash error); 0.3.1's bank query covers most of the need.
