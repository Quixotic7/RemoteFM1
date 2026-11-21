# remotefm1.com/move: plan (draft, 2026-11-19, waiting for the owner)

The owner asked for a page at remotefm1.com/move that takes someone new to the FM-1, the Ableton Move, Schwung and
RemoteFM1 from nothing to playing, in a Move-inspired look. It should make the Wi-Fi firmware easy to flash, and
leave the main installer's default on the BLE build. It should explain the architecture first, then both Move routes
with install guides, and the difference between the routes should be clear at the top.

This folder holds the plan and a working mockup: `mockup/index.html` (+ `css/`, `js/`), with screenshots in `shots/`
(`move-site-desktop.png`, `move-site-desktop-routeA.png`, `move-site-phone.png`, 390 px wide). Nothing is deployed,
and nothing in `web/` changes until the owner says yes.

## 1. The two routes (the facts the page rests on)

| | **Route A: the Move as a controller** | **Route B: RemoteFM1 on the Move** |
|---|---|---|
| Where the firmware runs | the browser (remotefm1.com), the iOS / Android app, FM1VST | the Move itself (wasm2c-compiled `.rfm1m`, three slots, Schwung FX) |
| Sound | the computer / phone (or an FM-1 on it) | the Move's speaker and outputs |
| On the Move | Schwung + the tool **Move Remote** (Quixotic7/abletonmoveremote, v0.1.1 released) | Schwung + the tool **RemoteFM1** (Quixotic7/remoteFM14Move, branch phase1, **not released**) |
| Link | Wi-Fi `move.local:7720` or USB-C `172.16.254.1`; from https, Chrome's local-network prompt or Move Remote's bridge window | none needed; the page at `remotefm1.local:7730` (or `move.local:7730`) over Wi-Fi |
| FM-1 | not needed; optionally a second panel (BLE/USB build) | not needed; optionally the panel: USB-A (any RemoteFM1 0.3+) or Wi-Fi (the Wi-Fi build) |
| Controls | rfm1cs-abletonmove (FM-1 / MIDI / Control / Sequencer modes on the track buttons) | the same core, in-process, plus the projects view (Shift + Step 1) and the catalog (Mute there) |

Schwung itself: installed with its desktop installer from schwung.dev (Mac / Windows / Linux; Move on Wi-Fi; back up
Sets first; DFU restore as the fallback); Schwung Manager at `move.local:7700`; Tools menu Shift + Step 13. Leaving a
standalone tool: Shift + touch the volume knob + click the jog wheel.

## 2. Information architecture (in page order)

1. **Hero.** One line ("Your Move, playing FM-1 firmwares"), two sentences on what this is, the animated Move with the
   FM-1 beside it (a dashed Wi-Fi link between them). CTAs: "Pick your way in" (to 2), "How it works" (to 3). A pointer
   to the glossary for complete newcomers.
2. **Two ways to use a Move with an FM-1 firmware** (up top, as the owner asked). Route B first, then Route A
   (the owner's decision). Two cards side by side (stacked on a
   phone), colour-coded (A cyan, B lime, the colours the rest of the page uses for each route), each with the same five
   rows: runs on / sound from / on the Move / link / FM-1 needed?, a "best for" line and its own "Set up" button.
   Below them, **the chooser**: three one-tap questions (where the sound comes out; whether a computer is on while
   playing; three firmwares at once with FX) -> a recommendation and a button that opens that route's steps.
3. **How it works: the architecture diagram.** An SVG with four nodes (computer / phone, Move, FM-1, internet), a
   Route A / Route B toggle, animated links coloured by kind (Wi-Fi violet, USB yellow, Bluetooth blue, internet grey),
   a glowing "firmware runs here" badge that jumps between the computer and the Move, and a sound icon by whichever
   device plays. Tapping a node or a link fills the side panel with two or three lines; the legend filters by link kind.
   The intro line says what an `.rfm1` is (one paragraph, no jargon past that).
   **Then the controls explorer:** a full Move drawn from the manual's own layout (OLED, 8 knobs, volume, jog,
   back/menu, 4 track buttons, 32 pads, 8 right-hand buttons, play/rec, 16 steps, arrows), lit as FM-1 mode (white and
   pink keys, the button LEDs). Press any control: its FM-1 role, its Route A / Route B specifics, and the word the
   Move's screen would show, in a mock OLED.
4. **The words, for newcomers.** Eight expandable cards: FM-1, Ableton Move, Schwung, Schwung Manager, Move Remote,
   RemoteFM1 (player + firmware), RemoteFM1 on the Move, `.rfm1` and the catalog. Each a single line closed, two or
   three open.
5. **Set it up, step by step.** A Route A / Route B tab (the route buttons and the chooser open the right one), a
   progress ring, then numbered steps, each with a tick box and a **checkpoint** ("you should see ..."). Progress is
   kept per browser in localStorage (`rfm1move.v1`, wrapped in try/catch; the page works without it).
   - A (6 steps): install Schwung; install Move Remote from Schwung Manager; start it; open the player and connect
     (remotefm1.com "Use my Ableton Move", or the app's Settings › Ableton Move); pick a firmware and play; the way out.
     An optional line: the FM-1 as a second panel with the BLE build.
   - B (7 steps): install Schwung; install the RemoteFM1 tool (Schwung Manager listing, else Install Custom Module ›
     GitHub `Quixotic7/remoteFM14Move`, else the release tarball); launch (first launch unpacks ~45 MB); open the page on
     a phone (`remotefm1.local:7730`, fallbacks for Android); more firmwares (projects view, Mute, or the page's Browse
     all); the FM-1 as the panel (USB-A or Wi-Fi, linking to 6 and 7); the way out.
6. **Put RemoteFM1 on your FM-1 (flashing).** Three build cards: **USB + Bluetooth** (the default everywhere else:
   website, iOS / Android apps, FM1VST, Route A, and the Move's USB-A), **Wi-Fi** (Route B's wireless panel; no
   Bluetooth, no sound out of the FM-1), **USB only** (no radio). One sentence on why one radio per build (the FM-1's
   RAM; WIFI.md). Then **the Wi-Fi flasher, embedded** (remotefm1.com/move/flash/: the Wi-Fi build only, the owner's decision; the BLE and USB-only cards link to remotefm1.com/installer); the build cards and the
   installer's radios stay in sync. The line "changing your mind is the same page" (BLE over Wi-Fi, or back to V15).
7. **Put the FM-1 on your Wi-Fi.** Two tabs: **on the FM-1 itself** (HOME held › WI-FI row › OCT+ › pick › password on
   the keys › OCT+; a drawing of the Wi-Fi page) and **from the Move's page** (FM-1 on USB-A, `remotefm1.local:7730/?wifi`,
   Scan, password, Join, unplug; a phone drawing). 2.4 GHz only and the iPhone hotspot's "Maximise Compatibility" up top.
8. **When something's off (FAQ).** .local names (Android), Chrome's local-network prompt and macOS Local Network,
   the Wi-Fi FM-1 not found (same network, client isolation, 2.4 GHz), which firmware for Route A, going back to stock,
   "is this Ableton's / M-VAVE's?" (no).
9. **Footer.** Licence, sources, the trademark line ("Ableton and Move are trademarks of Ableton AG ..., named only to
   say what these tools work with"), independent-project disclaimer.

Navigation: a sticky header (brand "RemoteFM1 for Move", section links on desktop, "remotefm1.com" back to the player).
On the player itself, one entry point: the menu's "Use my Ableton Move" sheet gets a "New to this? remotefm1.com/move"
link, and the installer's page a line "Flashing for an Ableton Move? /move".

## 3. Look (reworked 2026-11-19: the owner's "binary black/white style inspired by the Ableton Move's screen, mixed with some colors")

- **1-bit base, as the Move's OLED:** pure black (#000) and pure white (#fff), nothing in between for surfaces. Cards
  and panels are black with a 1 px white outline and square corners; emphasis is **inverted blocks** (the section
  numbers, the "for Move" tag, the picked tab, an open glossary card or FAQ row, the chooser as one white panel with
  black type). Dividers are dashed 1 px lines. Grey (#a8a8a8) only for secondary text.
- **Dither:** ordered (Bayer 8 x 8) dither drawn by the page's JS onto small canvases at one pixel per 2 CSS px,
  scaled crisp (`image-rendering: pixelated`): a fade band under the hero, a short thinning rule under every section
  title; a 50 % checker behind the flasher; the Move's jog wheel is dithered too.
- **Colour as accents only**, each with one meaning: Route B lime, Route A cyan, FM-1 orange, Wi-Fi violet,
  Bluetooth blue, USB yellow. They appear on badges (the route tags, the "Checkpoint" labels, the Wi-Fi card's tag),
  lit pads and button LEDs, the diagram's links and the "firmware runs here" badge, hard offset shadows on the FM-1
  and the Wi-Fi card (6 px, no blur), and the primary button's shadow. No coloured strips along card edges (owner).
- **The Move** is drawn as 1-bit line art from the manual's figure (`AbletonMove/images/p007-002.png`, in % so it
  scales): a black body and 1 px white outlines for the knobs, buttons and pads; colour only where an LED is lit
  (pads, track buttons, the right-hand buttons in FM-1 mode). The hero's pads blink in steps, not fades.
- **Type:** headings, labels, buttons and the mock OLED in **Jersey 10**, a pixel display face (SIL OFL 1.1; the one
  RemoteFM1's own Wi-Fi screens already use, `firmware/third_party/jersey10`; the site ships it as
  `web/site/fonts/Jersey10-Regular.woff` + `OFL-jersey10.txt`, converted with fontTools). Small labels and values in
  DM Mono; paragraphs stay in DM Sans at 15-18 px for reading. Unbounded and Chakra Petch are not used on this page.
- **Drawn 1-bit widgets** (the owner, 2026-11-19: the style of 1-bit handheld game and app art; inspired by it, copying
  no game's or app's art or layout): framed panels with thick rounded 2.5 px outlines and a small inverted label tab
  (ROUTE B, NOT SURE?, INFO, CONTROL, WORDS / MEANS, STEPS, FLASHER, HELP); segmented LED meters (the route cards'
  comparison, the steps' progress, the flasher's progress, the phone's scan); big pixel numerals (the hero's 3 / 39 / 0,
  the chooser's 1/3, the steps' 0/7); round knobs with a pointer and the value under each (the flasher: version, build,
  image size, radio); inverted selection bars (the chooser's answers, the glossary's list beside its detail panel, the
  picked tab); dotted and crosshatch 1-bit fills (the hero's dotted field, the Move's pads, the diagram's node where
  the firmware runs, the phone's screen); a rounded pill button with a round glyph for Flash. The Move is drawn as
  1-bit pixel art with a stepping playhead column; the FM-1 is the owner's pixel FM-1.
- **Motion** is stepped (`steps()`): dashed links crawl, the FM-1 bobs, the badge jumps. All of it off under
  `prefers-reduced-motion`.
- **No Ableton marks:** no logo, no wordmark, no Ableton typeface; "Ableton Move" only in running text and the
  trademark line. The Move drawing is the hardware's shape, not its branding (no printed legends).
- Mobile first (one column, 16 px gutters, 44 px targets, no horizontal scroll); two columns from 900 px. Known mockup
  gap: on a phone the diagram's link labels are small; the real page stacks the nodes under 600 px with 12 px labels.
  Dark only, as the screen is.

## 4. The interactive pieces

| Piece | Behaviour | Data |
|---|---|---|
| Route chooser | 3 questions, score A/B, recommendation, opens that route's steps | inline |
| Architecture diagram | route toggle, node/link details, link-kind filter, moving "runs here" badge | `LINKS`, `NODES` in the JS |
| Controls explorer | press any control (mouse, touch, keyboard): role, per-route notes, OLED word | `CTL` in the JS (from rfm1cs-abletonmove's mapping; to be checked against v0.5.5 before launch) |
| Step checklists | tick, progress ring, kept in localStorage | `rfm1move.v1` |
| Build picker + installer | the cards select the package; the installer starts on Wi-Fi only here | `?variant=wifi` |
| Wi-Fi tabs | FM-1 / Move page | static |

## 5. The flasher on this page (Wi-Fi only here, BLE stays the default elsewhere)

- **Its own page, Wi-Fi only (owner's decision 3-4).** `web/site/move/flash/index.html` (remotefm1.com/move/flash/):
  the installer's code (`installer.js`, `fm1ota.js`, `fm1pkg.js`, `releases.json`, imported from `../../installer/`,
  nothing copied) with the package fixed to the release's `wifi` entry: no package choice, no file picker. /move
  embeds it in an iframe (same origin, so Web MIDI works; `allow="midi"`), drawn in this page's 1-bit look; on a phone
  it says "open this on a computer" (flashing needs Chrome / Edge on a computer). Going back to Bluetooth, USB only or
  V15 is remotefm1.com/installer, linked from it.
- **releases.json:** each release gets a third package beside `usbOnly`: `"wifi": { "file": "remote-wifi-X.Y.Z.fwsc",
  "sha256": "...", "identity": "FM-1_729XXYYZZ" }`; `releaseFile(rel, "wifi")` returns it (the /move flasher's only
  package; the main installer's radios are unchanged); test_installer.mjs checks the list, SHA256SUMS and the files
  as for usbOnly.
- **Identity:** the Wi-Fi build answers `FM-1_729..` (dev `FM-1_72900`, releases `FM-1_729XXYYZZ`). Today's
  `remoteFm1Version()` matches `FM-1_[57]2XXYYZZ` only, so the installer would **not** recognise a Wi-Fi unit as
  RemoteFM1 (an install over it would be refused as an unknown firmware, and the way back to BLE would need V15 first).
  `fm1ota.js` must read `FM-1_729XXYYZZ` (and `FM-1_72900`) as "RemoteFM1 Wi-Fi X.Y.Z"; the "What is on my FM-1?"
  sentence names the build. `tools/fm1_install.py` on the wifi branch already does.
- **The main installer** keeps "USB + Bluetooth MIDI" checked by default; it can flash over a Wi-Fi unit (the identity
  above) and links "Flashing for an Ableton Move? remotefm1.com/move" for the Wi-Fi build.

## 6. How it fits the site's build and deploy

- Source: `web/move/` is taken (it holds `build.sh` for the Move core's wasm). The page goes in **`web/site/move/`**
  (`index.html`, `move.css`, `move.js`; fonts from `../fonts/`), published at `_site/move/` = remotefm1.com/move/.
- `.github/workflows/pages.yml`, "The player and the installer" step: `web/site/move` is copied with the rest
  (`cp -R ... web/site/move _site/`). `site_guard.mjs check` gains: `move/index.html` exists and every local link in it
  resolves (the installer path, `../fonts/`).
- The installer change (5) ships in the same PR; `test_installer.mjs` covers the Wi-Fi package and the 729 identity.
- A small `web/site/tools/move-e2e`-style check in headless Chromium: the chooser, the diagram toggle, a tick kept
  across a reload, the iframe's variant.
- No build step, no framework: plain HTML/CSS/JS like the rest of web/site.

## 7. What must exist before launch

1. **RemoteFM14Move released** (owner's OK): a GitHub release of `remotefm1-module.tar.gz` (`make package`, with the
   bundled catalog), tagged; the Schwung Manager listing (or, until then, "Install from GitHub" working: the repo's
   default branch must carry what Schwung expects, e.g. a `release.json` like Move Remote's, pointing at the release
   asset; today the repo has only the `move-catalog` release and the work is on `phase1`, not main).
2. **The Move catalog published** for the release's ABI (exists as the `move-catalog` release; rebuild after the
   threads change so the objects are `mt`).
3. **RemoteFM1 Wi-Fi released:** the `wifi` branch merged (or a release from it), a version number (the next
   RemoteFM1 release, e.g. 0.4.0 or 0.3.7), `build.sh --wifi --release X.Y.Z` -> `remote-wifi-X.Y.Z.fwsc` as a release
   asset next to `remote-X.Y.Z.fwsc` and `remote-usb-X.Y.Z.fwsc`, its SHA-256 in `SHA256SUMS`, in
   `web/installer/firmware/`, and its entry in `releases.json`; release notes saying what the build has and lacks.
   Items it still lists as unverified on a unit (WIFI.md: the screen rate, discovery through other routers) checked
   on the owner's FM-1 first.
4. **The Hub** (thefirmwarehub.com): decide whether the Wi-Fi package is listed there (a separate entry, or not at all
   so the Hub keeps offering the BLE one); `fmhub.json` / the Hub's releases.json for RemoteFM1 untouched otherwise.
5. **The installer:** the 729 identity, the third package, `?variant=` (5 above), tests.
6. **Move Remote:** its README says 0.1.1+ for the bridge; its `release.json` still points at 0.1.0 (check what
   Schwung Manager reads before launch).
7. **iOS / Android:** the apps' Move controller (RemoteFM1IOS and RemoteFM1Android branches, rfm1cs-abletonmove 0.3.1)
   released, or the page says "in the next app update" for Route A on a phone.
8. **Pictures:** real photos of the Move with the FM-1 (Route B, FM-1 on Wi-Fi), the Move's screen at Move Remote's
   start and at RemoteFM1's first launch, Schwung Manager's Modules page with RemoteFM1, the FM-1's Wi-Fi page (the
   simulator shots from `firmware/tests/wifi_page.txt` will do), the Move page's Wi-Fi sheet (docs/move-page-phone-wifi.png),
   remotefm1.com's "Use my Ableton Move" sheet. A 20-second video of Route B would carry the hero (owner's call).
9. **Copy checked against the shipped versions:** the control mapping (rfm1cs-abletonmove 0.5.5), the Tools menu
   shortcut (Schwung's setting can make it Shift + touch Volume + Step 13), the bundled count and sizes.

## 8. Open questions for the owner

1. **URL and name:** `remotefm1.com/move/` with the title "RemoteFM1 for Move", and the Route B tool called
   "RemoteFM1" in Schwung (as now)? (The page uses "RemoteFM1 on the Move" in prose to tell it from the player.)
2. **Route order and default:** Route A left / first and the checklist opening on A, or lead with B (the release this
   page is for)? The mockup puts A first, the chooser's tie goes to B.
3. **Wi-Fi release:** its own version line (0.4.0 with all three packages from one tag) or a Wi-Fi-only release from
   the `wifi` branch first? And on the Hub, or only on remotefm1.com?
4. **Embed or link the installer** on /move: an iframe with Wi-Fi preselected (the mockup), or a big button to
   `/installer/?variant=wifi`?
5. **Photos / video:** will you shoot the Move + FM-1 photos, or should the page launch with drawings only?

## The owner's decisions (2026-11-19)
1. URL and name as proposed: remotefm1.com/move/, "RemoteFM1 for Move"; the Schwung tool stays "RemoteFM1".
2. Route B (RemoteFM1 on the Move) leads; Route A (the Move as a controller) second.
3. The Wi-Fi firmware ships in ONE RemoteFM1 release with all three packages (USB+Bluetooth stays the default everywhere else), but RemoteFM1 for Move gets its own flasher page that offers ONLY the Wi-Fi build.
4. That Wi-Fi-only flasher is embedded in the /move page (as in the mockup).
Still open: photos/video (owner-shot or drawings at launch).

## Built (2026-11-19, after the owner's "Yes, build it")

- **The page:** `web/site/move/` (index.html, css/move.css, js/move.js, img/fm1-pixel.png, data/move.json), published
  at `_site/move/` by pages.yml. Jersey 10 as `web/site/fonts/Jersey10-Regular.woff` + `OFL-jersey10.txt`. The
  diagram has a stacked layout below 600 px. What waits on a release is data, not copy: `data/move.json`
  (RemoteFM14Move's release, its bundled count and size, the Schwung Manager listing, the apps' Move controller) and
  `web/installer/releases.json` (a release with a `wifi` package).
- **The Wi-Fi flasher:** `web/site/move/flash/` imports `installer/installer.js` (no copy), fixed to the `wifi`
  package; "Not released yet" until releases.json lists one. The main installer: `remoteFm1Version` / `remoteFm1Build`
  read `FM-1_729XXYYZZ` and `FM-1_72900` (installs go Wi-Fi to Bluetooth and back), `releaseFile(rel, "wifi")`,
  `wifiRelease`, a link to /move; its default stays USB + Bluetooth, and it offers no Wi-Fi choice itself.
- **Checks:** `web/installer/test_installer.mjs` (the Wi-Fi identity, the variants, the default unchanged, an install
  over a Wi-Fi unit; `WIFI_PKG=` a real Wi-Fi package), `web/site/tools/movepage-test.mjs` (in pages.yml's list:
  the assembled site against site_guard's new `guardMovePage`), `web/site/tools/movepage-e2e.mjs` (headless
  Chromium: chooser, both routes of the diagram, steps kept, the embedded flasher, phone without sideways scroll, no
  console errors). docs/RELEASING.md: what the third package needs.
