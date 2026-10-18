# TransitFM1

> **This tree is RemoteFM1** (PLAN.md), branch `m0-bench`: the M0 measuring build. `./build.sh` now builds
> `build/remote.fwsc` (`--unit transit` builds TransitFM1 as it was, byte for byte; `--usb-mode 0|2` the other USB
> services). TransitFM1's safety parts below are unchanged underneath. The bench: `tools/m0_bench.py` (SysEx over
> CoreMIDI), `tools/m0_bulk.c` (vendor bulk over libusb), `web/m0_bench.html` (Web MIDI + WebUSB); the protocol is
> `firmware/src/remote_proto.h`. TransitFM1's README follows.

**A tiny firmware for the M-VAVE FM-1 to stop at between two others.**

Moving an FM-1 from one custom firmware to another is where units get bricked. The usual advice is to go back to
the official V15 first. But V15 has no protection against an interrupted write, so the step **out of** V15 into a
custom firmware is the riskiest one. If the cable comes out or the battery dies partway, the FM-1 can stay dark.

TransitFM1 is a stop that is safe to leave. It makes no sound and stores nothing. It shows what it is, then waits
for you to install the next firmware. Three things make it a good stopover:

- **Every installer treats it as the official V15.** It answers the update handshake as `FM-1_015`, and its USB
  device is named "FM-1". An installer that refuses other custom firmwares writes over TransitFM1 as it would over
  stock.
- **It brings ChoralRoot's flash safeguards.** That's ChoralRoot's boot stub, byte for byte, in front of the app
  (docs/SAFETY.md). It checks the firmware before starting it. If the write was cut off, it starts update mode
  instead: three short flashes, then `ota-FM-1` over USB. Holding **OCT-** at power-on does the same.
- **It writes the next firmware itself.** When you install another firmware from TransitFM1, its own update loader
  does the writing, whatever loader the package carries. It saves the new firmware's first sector for last. A cut
  at any point before that last sector leaves an FM-1 that comes back in update mode: switch it on, run the
  installer again.

> **Status: 0.1, built and host-tested only. It has not run on an FM-1 yet.** The boot stub and the loader order
> it builds on are ChoralRoot's, tested on a unit (cable pulled mid-write, rescued). The new parts — writing
> another firmware's package with sector 0x4000 last, and staging its own loader — are tested only on the host
> (`tests/run_tests.sh`). Try it first on a unit you can recover with the
> [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) or
> [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker). docs/SAFETY.md has the device test plan.

## How much of each write is protected

These are measured on the host. Power was cut after every flash erase and every programmed page, the real stub
code judged the flash as it stood, and the install was resumed (`tests/ldr_test.c`). "Unprotected" cuts leave
neither a working stub nor a whole firmware.

| hop | cut points | rescued | unprotected |
| --- | --- | --- | --- |
| TransitFM1 → the official V15 (any firmware without ChoralRoot's stub) | 2432 | 2415 | **16**, the last sector only (about 0.1 s) |
| TransitFM1 ↔ ChoralRoot 0.25.1 | 2279 | 2278 | **0**: sector 0x4000 is the same bytes in both |
| TransitFM1 → a later TransitFM1 | 256 | 255 | **0** |
| the official V15 → TransitFM1 | 2432 | 2414 | 17, the first sector only (once per unit) |

Compare the plain address-order write that V15, Felucca and most custom firmwares use. Once the first sector is
written, no rescue code is left on the unit, so almost the whole one-to-two-minute write is unprotected.

## Using it

1. **Getting there.** Install `transit-X.Y.fwsc` with the installer of the firmware you are on now.
   `tools/fm1_install.py` here, ChoralRoot's `tools/fm1_install.py PACKAGE` and any installer that takes a
   Felucca-family package will do. From the official V15 it's the same: TransitFM1 installs like any custom
   firmware.
   - **Not from Bubba Machine.** Its updater only accepts M-VAVE's own package. Go back to V15 with Bubba's
     installer first.
2. **The screen** says `TRANSIT`, its version, "READY FOR THE NEXT FIRMWARE" and "AS V15 (FM-1_015)".
   - A red **LOADER NOT STAGED** line means TransitFM1 could not set up its rescue. It then refuses updates.
     Don't continue: report it.
3. **Leaving.** Open the installer of the firmware you want and install it. To that installer the FM-1 is an FM-1
   on V15. To go back to stock, install M-VAVE's `FM-1.fwsc`.
4. **If the install is cut off,** switch the FM-1 off and on. You'll see three short flashes of the key lights, a
   black screen, and `ota-FM-1` ("Felucca Update") over USB. Open the installer again and run the same install:
   it finishes the write. An installer that knows Felucca's update mode resumes it by itself. That covers ChoralRoot,
   Felucca, Dinghy and the FM-1 Firmware Hub. Don't switch it off and on twice in a row while it's in update mode.
   A second arrival with no normal boot in between drops into the chip's ROM USB boot (`WL82 UBOOT1.00`), where
   MvaveFM1Unbricker takes over.

Nothing of yours is kept on TransitFM1. What the previous firmware stored stays in its flash area untouched, unless
the next firmware's installer or the firmware itself deals with it. Back on V15, presets may read garbled after
Felucca-family firmwares: System > Reset Patches.

## Building

You need Linux on x86-64 (or macOS / Windows with Docker), Python 3, the JieLi toolchain and three files of the
JieLi AC79 SDK:

```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh                      # JieLi's clang for the chip, to ~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh                                  # -> build/transit.fwsc (identity FM-1_015)
./build.sh --release 0.1                    # -> build/transit-0.1.fwsc, SHA256SUMS
tests/run_tests.sh                          # the host tests of the update path
V15_PKG=FM-1.fwsc CR_PKG=choralroot.fwsc tests/run_tests.sh   # also the hops to V15 and to ChoralRoot
```

On macOS the build runs the toolchain in a `linux/amd64` Docker container (have Docker running). The build prints
the stub's hash and the hash of sector 0x4000. For the hops to and from ChoralRoot to stay unprotected-free, both
must be ChoralRoot's: stub `baee55ed…` (version 3) and sector `0f33c838…`. The build warns if the stub differs
from `firmware/stub/stub.lock`, and a release build stops.

The size: the app body is about 56 KB (ChoralRoot's is 543 KB), including a 7.5 KB copy of its own update loader.

## What is where

```
firmware/stub/        ChoralRoot's boot stub, unchanged (stub.lock: version 3, the same hash as ChoralRoot's)
firmware/loader/      the update loader: Felucca's, with ChoralRoot's order and TransitFM1's (ldr_core.c)
firmware/src/ota.c    step 1: our own loader staged, kept, restaged; never the package's
firmware/src/platform.c, transit.c   Dinghy's platform without the sound; the screen
tools/build.py        the stub, the loader, the app with the loader inside, the package (fm1pkg_make.py)
tests/                the stub's decision, step 1, the package builder, the loader's interruption model
docs/SAFETY.md        how it works, the numbers, what is not tested, the device test plan
```

## License

GPL-3.0-only, as Felucca, SLOOP, Dinghy and ChoralRoot. You may change and share it, and whoever gets your firmware
gets its source. Credits and third-party parts are in [LICENSING.md](LICENSING.md).

TransitFM1 is based on [Dinghy](https://github.com/zvenson/dinghy) by zvenson. Dinghy is
[Felucca](https://github.com/hugelton/Felucca)'s platform (Leo Kuroshita, Hügelton Instruments) as
[SLOOP](https://github.com/isod89/sloop-fm1) (isod89) carries it. The boot stub, the stub's write order and the
package marker are [ChoralRoot FM-1](https://github.com/Quixotic7/ChoralRootFM1)'s. "M-VAVE" and "FM-1" belong to
their owners: TransitFM1 is not made or endorsed by them, and installing firmware is at your own risk.
