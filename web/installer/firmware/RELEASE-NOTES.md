# RemoteFM1 0.1.1

`remote-0.1.1.fwsc`, sha256 `a7408d6c0526e01ef80e81c7350082ff203beaca3eabee52899748fc730a96a7`, built with
`./build.sh --release 0.1.1`. It replaces 0.1.

## What changed from 0.1

0.1.1 is a safety release. The panel, the protocol and USB are the same as in 0.1.

- **A firmware too large for the app area is refused.** A package whose image reaches past 0x93000 (Baud Girl's
  can) is now refused twice. Step 1 refuses it before anything is staged. The update loader refuses it before
  anything is erased. Under 0.1, an installer writing such a package over RemoteFM1 could leave a truncated image
  that was committed, and the unit would stay dark. To install such a firmware, go through the official V15 first.
- **The update loader is TransitFM1 0.1.3's, byte for byte** (`ota.bin` sha256 `d8a959c7…`). It answers
  `ota-FM-1_900`, as 0.1's did. TransitFM1 0.1.2's `ota-FM-1_015`, which bricked a unit leaving to Baud Girl, is
  not in RemoteFM1.
- A crash during the panel's start-up shows the crash screen without starting the panel again inside itself.
- The version reads 0.1.1 on the waiting screen and in HELLO.

These are unchanged: ChoralRoot's boot stub (sha256 `baee55ed…`), the sector 0x4000 layout (`0f33c838…`), the
staged loader written through RAM, MIDI-in draining, and the identity `FM-1_015`.

## Testing

0.1 (sha256 `13035039…`) is the build that was tested on a unit, on 2026-10-18: ChoralRoot played from a Mac through
the FM-1. 0.1.1 has passed the host tests: power cuts after every flash operation for V15 and ChoralRoot 0.25.1 both
ways, plus both refusals of an oversized package. 0.1 is no longer offered here.

0.1.1 on a unit, 2026-10-18:
- **Installed with this page,** in Chrome, over 0.1. The update mode wrote for 30 s, and the unit came back
  answering RemoteFM1 v0.1.1 with its own loader staged ("loader 1": the new loader replaced 0.1's at first boot).
- **ChoralRoot 0.25.1 → RemoteFM1 0.1.1 → ChoralRoot 0.25.1** with `fm1_install.py`:
  - ChoralRoot's boot stub reported "boot stub v3 … marker ok";
  - ChoralRoot's stored data (128 user sounds, favourites, loops, settings) was identical before and after.
