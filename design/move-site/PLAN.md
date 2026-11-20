# remotefm1.com/move: plan (draft, 2026-11-19, waiting for the owner)

The owner asked for a page at remotefm1.com/move that takes someone new to the FM-1, the Ableton Move, Schwung and
RemoteFM1 from nothing to playing, in a Move-inspired look. It should make the Wi-Fi firmware easy to flash, and
leave the main installer's default on the BLE build. It should explain the architecture first, then both Move routes
with install guides, and the difference between the routes should be clear at the top.

This folder holds the plan and a working mockup: `mockup/index.html` (+ `css/`, `js/`), with screenshots in `shots/`
(`move-site-desktop.png`, `move-site-desktop-routeB.png`, `move-site-phone.png`, 390 px wide). Nothing is deployed,
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
2. **Two ways to use a Move with an FM-1 firmware** (up top, as the owner asked). Two cards side by side (stacked on a
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
   RAM; WIFI.md). Then **the installer, embedded** with the Wi-Fi package preselected; the build cards and the
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

## 3. Look

- **The page is the Move's body:** near-black page (#141416), dark grey chassis cards (#29292c..#303033) with a lit top
  edge, pads that glow, an OLED (white mono text on black, a soft bloom), round knobs with a white pointer. The Move is
  drawn in CSS from the manual's figure (`AbletonMove/images/p007-002.png`) in percentages, so it scales to any width.
- **LED colours carry meaning** throughout: Route A cyan, Route B lime, FM-1 orange (remotefm1.com's tangerine),
  Wi-Fi / Schwung violet, Bluetooth blue, USB yellow.
- **Type:** remotefm1.com's own fonts (Unbounded titles, DM Sans text, Chakra Petch hardware legends, DM Mono values),
  so it is clearly part of the same site.
- **No Ableton marks:** no logo, no wordmark, no Ableton typeface; "Ableton Move" only in running text and the
  trademark line. The Move drawing is the hardware's shape, not its branding (no printed legends).
- Mobile first (one column, 16 px gutters, 44 px targets, no horizontal scroll); two columns from 900 px. Respects
  `prefers-reduced-motion`. Known mockup gap: on a phone the diagram's link labels are small (about 7 px); the real
  page stacks the nodes vertically under 600 px with labels at 12 px. Dark only, as the hardware is (a light theme is not planned; the owner may say otherwise).

## 4. The interactive pieces

| Piece | Behaviour | Data |
|---|---|---|
| Route chooser | 3 questions, score A/B, recommendation, opens that route's steps | inline |
| Architecture diagram | route toggle, node/link details, link-kind filter, moving "runs here" badge | `LINKS`, `NODES` in the JS |
| Controls explorer | press any control (mouse, touch, keyboard): role, per-route notes, OLED word | `CTL` in the JS (from rfm1cs-abletonmove's mapping; to be checked against v0.5.5 before launch) |
| Step checklists | tick, progress ring, kept in localStorage | `rfm1move.v1` |
| Build picker + installer | the cards select the package; the installer starts on Wi-Fi only here | `?variant=wifi` |
| Wi-Fi tabs | FM-1 / Move page | static |

## 5. The installer on this page (Wi-Fi preselected, BLE stays the default elsewhere)

- **One installer, a URL parameter.** `web/installer/index.html` learns `?variant=ble|wifi|usb` (default `ble`, as
  now). /move embeds `../installer/?variant=wifi&embed=1` in an iframe (same origin, so Web MIDI works; the iframe
  gets `allow="midi"`), or links to it on a phone (installing needs Chrome / Edge on a computer anyway: the embed shows
  "open this on a computer" on phones). `embed=1` hides the installer's own header and "Then" card.
- **releases.json:** each release gets a third package beside `usbOnly`: `"wifi": { "file": "remote-wifi-X.Y.Z.fwsc",
  "sha256": "...", "identity": "FM-1_729XXYYZZ" }`; `releaseFile(rel, "wifi")` returns it; the radio group grows a
  third option, shown only when the release has a Wi-Fi package; test_installer.mjs checks the list, SHA256SUMS and
  the files as for usbOnly.
- **Identity:** the Wi-Fi build answers `FM-1_729..` (dev `FM-1_72900`, releases `FM-1_729XXYYZZ`). Today's
  `remoteFm1Version()` matches `FM-1_[57]2XXYYZZ` only, so the installer would **not** recognise a Wi-Fi unit as
  RemoteFM1 (an install over it would be refused as an unknown firmware, and the way back to BLE would need V15 first).
  `fm1ota.js` must read `FM-1_729XXYYZZ` (and `FM-1_72900`) as "RemoteFM1 Wi-Fi X.Y.Z"; the "What is on my FM-1?"
  sentence names the build. `tools/fm1_install.py` on the wifi branch already does.
- **The main installer** keeps "USB + Bluetooth MIDI" checked by default and lists Wi-Fi as a third choice with one
  line ("for RemoteFM1 on an Ableton Move: remotefm1.com/move").

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
