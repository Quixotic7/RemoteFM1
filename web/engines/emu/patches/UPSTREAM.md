# fm1-emu patches

`web/engines/emu/build.sh` builds fm1-emu from a clean copy of the commit the submodule pins
(`web/vendor/fm1-emu`, upstream joakimeriksson/fm1-emu) with these patches applied in order. The
submodule stays at upstream's commit. When upstream merges a fix, bump the pin and delete its patch.

| Patch | Fixes | Upstream |
| --- | --- | --- |
| `0001-fm1-soc-model-the-P33-power-domain-bridge-and-its-re.patch` | Melodee 1.4.4 never draws | PR not opened yet (text below) |

## 0001: the P33 power-domain bridge

### Problem

Melodee 1.4.4 (the Hub's package, sha256 `d349a44d…`) never drew a frame in fm1-emu. The screen
sequence stayed 0, there was no audio, and the emulator didn't halt, at 360 and at 640 MHz.

- cpu0 spins on `goto .` at `0x01C4A0D0`, called through a register from `fm1_cstart` (XIP
  `0x02039F40`, rets `0x02039F42`). That RAM function is Melodee's `fm1_core1_failure()`
  (`firmware/hal/fm1_sys.h`): `cli`, `fm1_core1_stop()`, `for (;;)`. cpu1 was never released.
- `fm1_cstart` (`firmware/src/main.c`) calls it because `fm1_power_init()`
  (`firmware/hal/fm1_power.h`) returned 0. That function replays stock V15's supply setup over P33
  and then reads `P3_ANA_CON0/5/6/9` back to check each field. This package is a no-fallback
  build, where a failed check halts the boot. With the fallback, it would run on one core.
- P33 (`JL_P33` at `0x13E00`: `PMU_CON`, `RTC_CON`, `SPI_CON`, `SPI_DAT`, SDK `WL82.h`) was
  unknown MMIO in fm1-emu, so every read returned 0. The check failed before `lcd_init`.

Other firmwares only write P33: the watchdog, the reboot and the stock V15 power setup. None of
them checks what it reads back, so the missing block went unnoticed.

### Fix

`crates/fm1-soc/src/p33.rs` models the bridge (SDK `p33.h` `p33_cs_h`/`p33_cs_l`, and the FM-1
firmwares' `fm1__p33_xfer`):

- `SPI_CON` bit 0 is the chip select, and bit 8 picks the RTC bank (bit-15 addresses).
- Writing `SPI_CON` with bit 4 set sends the byte in `SPI_DAT`. Transfers finish at once: the busy
  bit 1 reads 0, as before.
- A frame is three bytes: `op << 5 | addr[9:8]` (bit 7 marks a read; op 0 write, 1 or, 2 and,
  3 xor), then `addr[7:0]`, then the data or, for a read, a dummy. A read's value then shows in
  `SPI_DAT`.
- The registers are 2 × 1 KiB of storage that resets to 0, so a firmware that only writes sees no
  change. Two strobes don't read back: `P3_WDT_CON` bit 6 (the watchdog clear) and `P3_PR_PWR`
  bit 4 (the soft reset).
- `FM1_P33_TRACE` prints each frame. Stock V15's frames decode to sensible P3 registers (WDT_CON,
  PMU_CON0..2, LP_TMR, ANA_CON4/5), which supports this frame format.
- The patch adds a unit test and a row in `docs/stock-firmware.md`.

### Pull request text (ready to paste)

Title: `fm1-soc: model the P33 power-domain bridge (Melodee 1.4 boots)`

Body:

> JL_P33 (0x13E00: PMU_CON, RTC_CON, SPI_CON, SPI_DAT) was unknown MMIO, so every P33 read
> returned 0. Melodee 1.4 replays the stock V15 supply setup over P33 and checks each field it
> wrote (P3_ANA_CON0/5/6/9). The check failed, so the firmware never started CPU1 and, in the
> Hub's 1.4.4 build, halted in `fm1_core1_failure()` before the LCD was set up: no screen, no
> sound, no halt reported.
>
> This adds `crates/fm1-soc/src/p33.rs`. The bridge shifts three-byte frames (op and address
> bits 9:8, address bits 7:0, data) into a register file with a P33 bank and an RTC bank
> (SPI_CON bit 8). Write, or, and and xor update a register, and a read returns it in SPI_DAT.
> Transfers finish at once (the busy bit reads 0, as before). Registers reset to 0, so firmwares
> that only write P33 run as before. The watchdog-clear and soft-reset strobes don't read back.
> `FM1_P33_TRACE=1` prints the frames.
>
> Checked: Melodee 1.4.4 boots, draws, plays a chord on both cores (cpu1 completes the audio
> jobs) and saves to flash. The browser build passes its test for Felucca 1.6.1, X0X 1.1.0,
> SLOOP 2.7.1, sloopDX 4.0, ORBIT 0.7.1, ChoralRoot 0.25.1, FM-106 Beta4 and stock V15, with
> unchanged real-time factors. Stock V15's own P33 traffic decodes to the expected P3 registers.
> `cargo test -p fm1-soc` passes and includes a new test of the frame format.

### Reproduce

```
# native: before the fix cpu0 ends on `goto .` at 0x01C4A0D0 and cpu1 is never released
cargo build --release -p fm1-emu
fm1-emu --fwsc melodee-1.4.4.fwsc --ms 3000 --snap 2900   # work/lcd_2900.bmp: the home screen
FM1_P33_TRACE=1 fm1-emu --fwsc melodee-1.4.4.fwsc --ms 300 | grep ^p33   # the supply setup's frames

# RemoteFM1's browser build, in Node
web/engines/emu/build.sh && node web/engines/emu/test.mjs --mhz=360 --only=melodee
EMU_PATCHES=0 web/engines/emu/build.sh web/build/emu-base   # the unpatched build, for comparison
node web/engines/emu/test.mjs --mhz=360 --only=melodee --wasm=web/build/emu-base/fm1_web.wasm
```

### Not modelled (still open)

- `C1_CON` (`0x1EEE004`) bit 1. Melodee and the SDK's `EnableOtherCpu` hold CPU1 with `|= 2` and
  release it with `|= 8; &= ~2`. fm1-emu models only the stock IPC's pause (`|= 4`) and resume
  (`|= 8`). Melodee holds CPU1 only on a fault, a retirement or a reboot, so a normal session is
  unaffected.
- The registers have no analog effects: no watchdog reset, no P33 soft reset, and the reset
  source reads 0 (no power-on bit).
