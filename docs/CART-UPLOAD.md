# Sending a DX7 cartridge to the FM-1 (user banks)

RemoteFM1 0.3.0's standalone instrument keeps a DX7 32-voice bulk dump sent to it as a user bank, USER1..USER3
(firmware `sa/sa_ubank.c`), listed after the 8 ROM cartridges (GLO + SELECT). A fourth replaces the oldest. From
0.3.1: 16 slots (USER1..USER16), a list of what is stored, a dump sent into a chosen slot (replacing it), a slot
deleted, and a reply to every dump (section "0.3.1: list, replace, delete, confirm" below; PROTOCOL.md section 8). Installing ChoralRoot or the stock firmware overwrites the window: the banks are gone and must be sent again.
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
3. **0.3.0: no reply.** On a good dump the unit writes the flash (two sector erases, ~100 ms), switches to the new
   bank, selects its first voice and shows the preset screen (`USERn`): that is the confirmation for the user. A dump
   that arrives while the previous one is still being written is dropped: wait ~1 s between banks. **0.3.1** answers
   every dump with STORED (below): wait for it before the next.
4. **After**: tell the user the limits (3 banks on 0.3.0, the oldest replaced; 16 from 0.3.1), or show 0.3.1's list.

Never ship cartridge `.syx` files in the app or the repos: users bring their own.

## 0.3.1: list, replace, delete, confirm

All of these are RemoteFM1 SysEx (PROTOCOL.md sections 2 and 8): `F0 7D 52 46 <cmd> <body, 7-in-8 packed> F7`, the
request `cmd` SA_INFO `0x0B`, every reply SA_INFO_R `0x4B`, each reply body starting with its op. They need no HELLO.
Multi-byte numbers little endian. 0.3.0 answers none of them: a host that hears nothing in ~1 s says "RemoteFM1 0.3.0:
up to 3 banks, sending only" and falls back to a plain send (no TARGET, no DELETE, no STORED).

| op | request body | reply body | when |
|---|---|---|---|
| 0 BANKS | `00` | `00 max count busy next loops` + `count` x `slot arrival32 crc32 name[10]` | at once |
| 1 NAME | `01 slot name[1..10]` | `01 slot status` | at once (written a moment after) |
| 2 TARGET | `02 slot` | `02 slot status` | at once: the next dump goes to `slot` |
| 3 DELETE | `03 slot` | `03 slot status` | when the directory is written (~30 ms; up to ~0.5 s behind a save) |
| 4 STORED | (none: unsolicited) | `04 slot status` | after every DX7 32-voice dump, once it is written or refused |

**Statuses** (all ops): `0` done; `1` a bad dump (a checksum, a header or a length that is not a 32-voice bank's: the
unit's own checks, section "The bytes"); `2` a bad request (slot >= 16; DELETE / NAME of an empty slot); `3` busy
(DELETE or NAME: one already waits; STORED: the dump came while another was being written, dropped); `4` not
standalone (a host holds a session: dumps are not kept, TARGET and DELETE refused); `5` the flash write failed (the old
state stays, nothing half-written).

**Slots.** Slot `k` (0..15) is always shown as USER`k+1`: deleting USER2 leaves USER1, USER3.. as they are (no
renumbering). BANKS lists only the stored banks, each with its slot, in slot order; a slot not listed is empty. On the
unit, GLO + SELECT and PRESETS step through the stored banks only (an empty slot is skipped). `next` is where a plain
dump goes: the first empty slot, else the oldest bank's (replaced).

**TARGET.** `02 slot` arms "the next DX7 dump goes into `slot`, replacing what is there (or filling it if empty)". It
holds for one dump only, which must end within 10 s of the TARGET (else it goes to `next` as usual). Any dump that
completes uses it up, refused ones too (send TARGET again before retrying). Without TARGET a dump goes to `next` exactly
as 0.3.0/0.3.1 did.

**DELETE.** `03 slot`: the bank is removed by writing a new directory without it (the A/B directory of `sa_store.c`:
a power cut before the new directory is complete leaves the bank there; after, it is gone; never half). The reply comes
once that write is done, so `03 slot 0` means gone for good. If the bank being played was deleted, the unit switches to
the next stored user bank in slot order (or, if it was the last, the bank before it, possibly a ROM cartridge), voice 1;
if another bank was played, it stays.

**STORED.** Sent on the transport the dump came in on (USB-MIDI or Bluetooth MIDI), on its own, after every complete
DX7 32-voice dump (`F0 43 0n 09 ...`): `04 slot 0` once the bank's directory is written (the bank is in the list, the
unit plays its first voice), about 0.1-0.2 s after the dump's `F7`; or `04 slot status` with 1, 3, 4 or 5 (`slot` then
the one it would have gone to). A host waits up to ~2 s for it; no STORED at all means 0.3.0 (or a dump the unit did not
see as a DX7 bank). Other SysEx gets no STORED.

**Standalone only.** Dumps are kept, and TARGET and DELETE taken, only while the unit is the instrument (no host
session: `sa_standalone()`); in a session they answer status 4 (a dump: STORED `04 slot 4`). BANKS and NAME work in
both.

### Example exchanges (bodies before packing)

- List: host `00` -> unit `00 10 02 00 01 10` + `00 ..arr.. ..crc.. "BRASS 1   "` + `04 ..arr.. ..crc.. "PIANO 2   "`
  (16 slots, 2 stored: USER1 and USER5; a plain dump would go to slot 1 = USER2).
- Replace USER5: host `02 04` -> unit `02 04 00`; host sends the 4104-byte dump -> ~0.15 s later unit `04 04 00`.
- Plain send: host sends the dump -> unit `04 01 00` (it became USER2).
- Bad checksum after a TARGET: unit `04 04 01` (nothing written; the TARGET used up).
- Delete USER1: host `03 00` -> unit `03 00 00` once written. Then BANKS lists only slot 4.
- In a session: host `02 04` -> `02 04 04`; a dump -> `04 01 04`.

The full exchange as packed bytes: `F0 7D 52 46 0B 00 02 04 F7` is TARGET slot 4 (one group: the high-bits byte `00`,
then `02 04`); its reply `F0 7D 52 46 4B 00 02 04 00 F7`.

The site does all of this from USE MY FM-1 (`web/site/js/cart.js`: `queryUserBanks`, `sendCartTo`, `deleteBank`).
