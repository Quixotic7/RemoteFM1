# TransitFM1: two bugs found on a unit (2026-10-17)

From the RemoteFM1 session (RemoteFM1 is a clone of TransitFM1, so it hit both). Checked against TransitFM1
`aa877da` (main). Both bugs are in the 0.1 release on the web installer.

## 1. CRITICAL: re-staging the loader faults (unit ends in ROM UBOOT)

**What happens.** `firmware/src/ota.c:340`, in `ota_restage()`:

```c
if (ota_prog(OTA_AREA + 32u + i, OTA_OWN_LDR + 32u + i, n))
```

`OTA_OWN_LDR` is `TRANSIT_LDR` (`platform.c:80`), a `static const uint8_t[]` from `tools/build.py` that lands in
`.rodata`, so in XIP flash (0x02006064 in `build/transit.dis`). `ota_prog` → `fl_write(off, src_ram, n)`
(`hal/fm1_flash.h:239`) → `FL_FAR(fl_prog_ram)` (`:183`) reads `src[i]` while the SPI flash is out of XIP for the
page-program command. The first page faults.

**Where it runs.**
- At every boot whose staging area is not TransitFM1's own loader byte for byte (`platform.c:219`). That is
  **before `lcd_init` (`:223`)**, so `fm1_fault`'s red crash screen is drawn on a panel that isn't set up: the
  owner sees nothing, the unit reboots 8 s later. Two of those → the boot guard's USB rescue.
- In step 1 (`ota_stage` → `ota_restage`) when an installer starts an update. In the USB rescue,
  `bootguard.pending = 2`, so that fault sends the unit to the ROM's `WL80UBOOT1.00`. On macOS that means the
  Transporter.

**When the staging area isn't ours.** After a hop that stages somewhere else. Seen: the official V15 installing
TransitFM1 stages the package's loader at **0xE6000** (V15's place). The dump below has TransitFM1's 7481-byte
`ota.bin` there and 0xE0000 empty. So V15 → TransitFM1 may trip this on its **first boot**, every time. That's the
path most public users take. Please verify that first: if it holds, pull or flag the 0.1 release.
ChoralRoot → TransitFM1 does not trip it (ChoralRoot stages the package's loader at 0xE0000, which is ours), which
is why the earlier device tests passed.

**Seen on unit 5BD75BE3.**
1. The owner's V15 / Bubba Machine round trip, then TransitFM1.
2. Installs from TransitFM1 stalled in step 1 after the 2 header reads, with nothing written.
3. The unit showed "TRANSIT USB RESCUE" with no crash screen.
4. Another install attempt dropped it to UBOOT.
5. OCT- at power-on gave three flashes, then UBOOT again: the staging area was empty.

Transporter dump: `fm1-backups/5BD75BE3-uboot-transporter-20261017/fm1-transporter-dump-20261017-221324.bin`
(sha256 `d67dc997…`, two identical reads, analysis `.txt` beside it). App area = transit-0.1 exactly;
**0xE0000..0xE4FFF all 0xFF** (erased by `ota_restage`, nothing programmed); a stray TransitFM1 loader at 0xE6000.
Recovered with the Transporter's `restore` of ChoralRoot 0.25.1.

**Fix.**
- Copy each chunk (≤ 256 B) into a RAM buffer before `ota_prog` in `ota_restage`.
- Audit every other `ota_prog` / `fl_write` caller in `firmware/src` and `firmware/loader` for a source that may be
  in XIP: const arrays, string literals. `ota_stage_head` builds its header in a stack buffer, which is fine, but
  check the rest. `fl_write`'s parameter is already named `src_ram`.
- Make `fm1_fault` initialise the LCD first if it isn't yet, so a boot-time fault is visible.

**Tests that would have caught it.**
- In the host flash model, make a program call whose source lies inside the const loader array (or any declared
  XIP range) fail loudly.
- Add boot tests where the staging area is (a) erased, (b) a foreign loader at 0xE6000 with 0xE0000 erased (the
  layout V15 leaves), (c) M-VAVE's loader at 0xE0000. Each must end with our loader staged byte for byte.

## 2. MIDI-in ring never drained (the installer is shut out by a DAW)

`firmware/src/usb.c`: every incoming MIDI event (notes, clock 0xF8, transport) goes into `midi_in_q` (`MQ` = 64,
`:159`). `ep1_take` (`:774`) NAKs an EP1 OUT packet while fewer than `EP1_ROOM` (24, `:773`) slots are free.
Dinghy's audio ISR drained the ring; with the sound gone nothing reads `mi_r`. After about 40 events from any app
(Ableton Live was open), EP1 OUT stays NAKed until power-off, and the installer's M-UPGRADE SysEx can't arrive.
`--info` may still answer first, then step 1 stalls.

**Fix.** Drain and discard non-SysEx MIDI in the app build, from `platform_poll`. The loader build
(`FELUCCA_LOADER`) never drains; leave it unchanged so `build/loader/ota.bin` stays byte-identical (sha256
`68599862fbf2b116…`, check before and after). RemoteFM1's `m0-bench` branch (`../RemoteFM1/firmware/src/remote.c`)
drains it; read it for reference, but don't edit it.

**Test.** More than 100 clock bytes and notes, then a handshake that must still be answered.

## Rules

- Keep the stub untouched (the `stub.lock` check must pass; stub `baee55ed…`, sector 0x4000 `0f33c838…`).
- Run `V15_PKG=../MvaveFM1Unbricker/FM-1.fwsc CR_PKG=../fm1-backups/5BD75BE3-before-remotefm1-20261017/choralroot-0.25.1.fwsc tests/run_tests.sh`
  and `./build.sh` (Docker). Report the package sha256.
- Don't flash any unit from an agent. Before a device test, have the Transporter at hand. Test the V15 → TransitFM1
  hop on a unit, since that's the one that matters.
- Update `docs/SAFETY.md` ("On the unit") and the release notes; the README currently says 0.1 was tested on a unit.
