# Native builds on remotefm1.com

*For firmware authors: ten minutes, once.*

remotefm1.com plays FM-1 firmwares in the browser. Every firmware on the FM-1 Firmware Hub runs there **emulated**:
your `.fwsc` package on fm1-emu, a model of the FM-1's chip. A **native build** is your firmware's own C code
compiled for the browser (WebAssembly), with no chip in between. It starts at once and is about 20 times lighter on
the computer, so it plays on phones and old laptops too.

You can publish one yourself. One more job in your release workflow builds it on GitHub, tests it, signs it and
attaches it to your release. The site picks it up from there.

## What you add

Your release workflow already has the Hub's job. Add the `native` job under it:

```yaml
  native:
    needs: build
    uses: Quixotic7/RemoteFM1/.github/workflows/fm1-native.yml@native-v1
    permissions: { contents: write, id-token: write, attestations: write }
    with: { id: myfw, family: felucca_web }
```

- `id`: your firmware's id on the Hub (the end of its Hub address).
- `family`: what your firmware is built on (below).

The whole file, with the Hub's job, is in [native-release.yml](native-release.yml). Copy it to
`.github/workflows/release.yml` and change the lines marked "yours".

1. **Push a version tag** (`v1.2`).
2. **Check the Actions tab.** When the run is green, `myfw-1.2.fm1web` is on the release, next to your `.fwsc`.
   Don't upload or rebuild the file by hand: the signature is for the file the run made.
3. **Ask to be listed.** Open an issue on [RemoteFM1](https://github.com/Quixotic7/RemoteFM1/issues) with your
   repository. The site's owner adds it to the list (`web/site/data/native-authors.json`).
4. **The site syncs every night.** It checks your file (below) and puts it in the player. Your firmware's card gets
   a ringed dot, and its sheet says **native · built by the author**, with the commit and a link to the build log.

## Pick your family

A native build reuses the glue that [FM1VST](https://github.com/Quixotic7/FM1VST) wrote for each kind of FM-1
firmware: the code between the firmware's own hardware layer and the browser. Pick the family your firmware comes
from.

| family | for firmwares built on | FM1VST builds |
|---|---|---|
| `felucca_web` | Felucca, with its browser unit `web/emu/felucca_web.c` | Felucca, GHOULBOX, JIANT; DUELL, RAINBOW (gated) |
| `fm1base` | Charles Vestal's fm1-base (`app.h`, `app.c`, `fm1.toml`) | AMB-1, LOOM-8 |
| `plat` | X0X's `plat.h` platform | FOMNI, FIMBA; X0X, Fart Machine One (gated) |
| `dinghy` | Dinghy (`app_main`, a main loop that never returns) | OberVave, Frodhes; FM-106 (gated) |
| `sloop` | SLOOP | SLOOP, SLOOP DX, ORBIT, No Man's Sky FM (all gated) |
| `choralroot` | ChoralRoot FM-1 | ChoralRoot, PurpleMonkey, CoralFlower |
| `melodee` | Melodee | Melodee |
| `custom` | anything else | your own glue |

**If FM1VST already builds your firmware**, `id` and `family` are all it needs: the build takes FM1VST's glue for
it and your repository at the tag in place of FM1VST's copy.

**If it does not** (a new fork, say), give the build two more things:

- `glue`: a folder in your repository with your core's two small files, copied from a sibling in FM1VST's
  `core-glue/<family>/` and renamed (for Felucca: `core_myfw.c` and `core_myfw_desc.c`). Your repository is at
  `../../cores/<id>/` from them.
- `core`: the `fm1_add_core` arguments, as FM1VST's own `core_<id>.cmake` gives them, after `SOURCE`:

```yaml
    with:
      id: myfw
      family: felucca_web
      glue: fm1core
      core: |
        UNIT core_myfw.c DESC core_myfw_desc.c SUBMODULES_RECURSIVE
        GEN ui_fonts.h ui_icons.h ui_keycaps.h ui_palettes.h felucca_tables.h felucca_fm6.h felucca_samples.h
        VERSION_FILE firmware/src/felucca.c VERSION_REGEX "^#define FELUCCA_VERSION \"v?([^\"]*)\""
```

**`custom`**: write the glue yourself against FM1VST's core ABI 6 (`core-api/fm1core.h`) with the helpers in
`core-glue/common` (README there), and put a `core.cmake` in your `glue` folder that calls
`fm1_add_core(<id> ${CMAKE_CURRENT_LIST_DIR} SOURCE ${FM1WEB_TREE} ...)`.

The other inputs, all optional:

| input | what |
|---|---|
| `test` | conformance lines over the glue's, when a key alone makes no sound: `sound btn PLAY 100` (a line replaces every line of the same kind) |
| `version` | the version, when the tag is not it (default: the tag without its `v`) |
| `core-id` | the core's own id, when FM1VST's differs from the Hub's |
| `usb-product`, `editor` | the USB name it answers with and its web editor (default: the Hub's) |
| `fm1vst-ref` | the FM1VST commit to build with (default: one known good) |

## What the build checks

The run fails, and nothing is attached, unless your firmware:

1. **builds** with Emscripten on GitHub's Ubuntu runner;
2. **boots** from an empty flash and **draws** a screen that is not blank within 3 seconds;
3. draws the **same first screen** as the same code built for the runner itself, pixel for pixel;
4. **sounds** when a key is held (after your `test` lines, if any);
5. passes its glue's **save and reboot** scripts, if it has them (a setting survives a power cycle; OCT- and OCT+
   held reach the update mode);
6. **renders 10 seconds** of audio without stopping.

The run's summary shows each line.

## What the site checks

Every night, for each listed repository's latest release:

1. **The signature.** `gh attestation verify` with your repository and RemoteFM1's workflow as the signer. Only
   `@native-v1` is trusted, built from your repository at a tag.
2. **The file.** Its manifest names your repository, that tag and that commit; the module's SHA-256 is the
   manifest's.
3. **What the module can reach.** Its imports must be exactly the functions the player gives a core, and no other:
   writing to the console, the clock, the coroutine. No network, no files, no page.
4. **The size.** At most 4 MB.
5. **The licences.** A firmware FM1VST keeps out of its public builds stays out here too, until its question is
   settled.
6. **A test run** of the same checks as the build, on the site's own runner.

Then it is copied to `engines/native/ext/<id>/<version>/` on the site.

## Which build plays

If FM1VST also builds your firmware natively, the **newer version** plays. At the same version, **yours** does. The
firmware's sheet says which, and why. The emulated engine is always one click away. Saves are kept per firmware,
engine and version, so a save from FM1VST's build of 1.2 opens in yours.

## Check a file yourself

```sh
gh attestation verify myfw-1.2.fm1web --repo you/myfw \
  --signer-workflow Quixotic7/RemoteFM1/.github/workflows/fm1-native.yml
```

The file is a gzipped tar: `tar tzf myfw-1.2.fm1web` shows `manifest.json`, the module and the test script.

## Try it before you tag

On a Mac or Linux computer with Emscripten, Node and Python 3 (Pillow with libraqm, fontTools):

```sh
git clone https://github.com/Quixotic7/RemoteFM1 && git clone https://github.com/Quixotic7/FM1VST
FM1VST_DIR=$PWD/FM1VST FM1N_TREE=$PWD/myfw FM1N_ID=myfw FM1N_FAMILY=felucca_web \
  FM1N_REPO=you/myfw FM1N_TAG=v1.2 FM1N_HUB=none bash RemoteFM1/.github/scripts/fm1-native.sh
```

It runs the same build and checks as GitHub and leaves `dist/myfw-<version>.fm1web`.

**If it does not work.** The run must be on GitHub's runner, from a tag, with all three permissions. A file changed
after the run fails its signature. A build of the `main` branch is a test: only a tag's build is published.
