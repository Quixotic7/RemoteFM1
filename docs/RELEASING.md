# Releasing RemoteFM1's firmware

From 0.3.2, every FM-1 release is built and signed on GitHub by The Firmware Hub's shared build workflow,
[zvenson/fm1-build](https://github.com/zvenson/fm1-build) at its reviewed tag `v1`. The hub
(<https://thefirmwarehub.com>) then shows **Built from source on GitHub** on RemoteFM1's card, and anyone can check that
a package is exactly what this repository's source builds. The web installer (remotefm1.com/installer) serves the same
bytes. Nothing needs a key or a secret: GitHub signs with the run's own identity (an artifact attestation).

The SMK-37 test builds (`v0.3.1-smk37...`, docs/SMK37.md) are not part of this: the release workflow runs on `vX.Y.Z`
tags only.

## What a release is

Two packages, both installing as `FM-1_52XXYYZZ` (0.3.2: `FM-1_52000302`; `firmware/fm1.toml` `id_prefix`):

| file | build | |
|---|---|---|
| `remote-V.fwsc` | `./build.sh --release V` | USB + Bluetooth MIDI. **The one The Firmware Hub lists** |
| `remote-usb-V.fwsc` | `./build.sh --release V --no-ble` | USB only, no Bluetooth library |

`tools/hub_build.sh V` makes both, the same way on a Mac (the toolchain in Docker) and on GitHub's Ubuntu runner:

- Pillow is pinned (11.1.0, with Raqm): fm1-base draws the UI fonts with it, and another version could draw them
  differently. On the runner it is installed; on a Mac the script stops if the version differs.
- **Yamaha's DX7 ROM cartridges** (the standalone's 256 voices) are never in this repository or a release. The script
  downloads the eight 32-voice dumps from Yamaha Black Boxes into `firmware/build/dx7-rom/` and stops unless each
  one's sha256 is the one pinned in the script (they equal the owner's `Presets/DX7-ROM`, so `DX7_ROM_DIR` pointing
  there skips the download). `firmware/tools/gen_dx7_rom.py` packs them into the build's `dx7_rom.h`; the script
  stops if the build does not have all eight.
- On a release, every sha256 is checked against `web/installer/releases.json` and the committed
  `web/installer/firmware/` before anything is signed.

The Mac build and a clean Ubuntu 24.04 build (Python 3.12, a fresh clone, submodules from GitHub) give the same bytes;
0.3.2 was checked that way before its tag.

## Steps

1. **Build on the Mac** (clean tree, Docker running):

   ```sh
   tools/hub_build.sh --site 0.3.3
   ```

   This builds both packages into `firmware/build/hub-0.3.3/`, copies them to `web/installer/firmware/` and adds them
   to its `SHA256SUMS`.

2. **List the release for the web installer**:
   - Add an entry at the top of `web/installer/releases.json`: version, date, tag, identity, `sha256`, `usbOnly`
     (file and sha256), and short notes.
   - Add a `# RemoteFM1 0.3.3` section at the top of `web/installer/firmware/RELEASE-NOTES.md`. The GitHub release's
     text is taken from it.
   - Update the version and identity that `web/installer/test_installer.mjs` expects for the newest release.

3. **Check**:

   ```sh
   tools/hub_build.sh --check web/installer/firmware 0.3.3
   node web/installer/test_installer.mjs
   python3 tools/check_fmhub.py
   tests/run_tests.sh
   ```

4. **Test on a real FM-1** with the very file from step 1, as the hub asks (thefirmwarehub.com/kb/#listed):
   - install it from the official V15 and from another firmware;
   - from RemoteFM1, install another firmware;
   - unplug once during Writing, plug in, press Install again: it continues.

5. **Commit, push `main`, then tag**:

   ```sh
   git tag v0.3.3
   git push origin v0.3.3
   ```

   Pushing `main` deploys the web installer with the new version. Pushing the tag runs
   `.github/workflows/release.yml`:
   - **draft** checks the tagged commit's `web/installer/` and makes a draft release with the notes and the licence
     files;
   - **build** is `zvenson/fm1-build@v1`. It runs `bash tools/hub_build.sh`, signs `remote-*.fwsc` and adds them
     with their `SHA256SUMS` to the draft;
   - **publish** verifies each file's attestation and checks the sha256s again, then publishes the release as Latest.

   The hub picks up the new release by itself within three hours.

Never replace or add a package on a published release by hand: the hub compares each file's sha256 with the signed
one, and a file that differs loses the key.

A run from the Actions tab (Run workflow, with a version) only builds and signs. It makes no release; the files are the
run's artifact `packages`.

### If JieLi's or Yamaha Black Boxes' server is down

The build job fails before anything is signed. The draft waits: re-run the failed jobs later. If a ROM file ever
changes on the server, the build stops at its sha256. Then find another source of the same bytes and change `ROM_URL`
in `tools/hub_build.sh`; the pinned sha256s stay.

## Checking a package

```sh
gh attestation verify remote-0.3.2.fwsc --repo Quixotic7/RemoteFM1 \
  --signer-workflow zvenson/fm1-build/.github/workflows/build.yml
```

It prints the tag, the commit and the build run. Add `--source-ref refs/tags/v0.3.2` to require that tag. Releases
before 0.3.2 were built on the owner's Mac and have no attestation; their sha256s are in `web/installer/releases.json`
and `web/installer/firmware/SHA256SUMS`.

## The Firmware Hub card

`fmhub.json` in the repository root sets the card's text, kind, links, picture (`docs/hub-card.png`, the standalone's
Notes screen shot by the host simulator, 240 x 240) and colour. The hub reads it from `main` every three hours. Edit
it, run `python3 tools/check_fmhub.py`, push, then check it with "Check your file" on thefirmwarehub.com/kb/#fmhub.
The name and version come from the repository and its releases.
