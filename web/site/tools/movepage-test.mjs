// SPDX-License-Identifier: MPL-2.0
// remotefm1.com/move without a browser: the site as pages.yml assembles it (move/, fonts/, installer/) passes the deploy
// guard's /move check (web/site/tools/site_guard.mjs guardMovePage), a broken link or a missing file fails it; the
// release states (data/move.json) say what is not released yet; Route B comes first; the flasher offers only the Wi-Fi
// build and imports the installer's code instead of a copy. The browser half: web/site/tools/movepage-e2e.mjs.
//   node web/site/tools/movepage-test.mjs
import { readFileSync, mkdtempSync, cpSync, mkdirSync, rmSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { guardMovePage, MOVE_RUNTIME } from "./site_guard.mjs";

const WEB = fileURLToPath(new URL("../../", import.meta.url));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); };

// the published layout, as .github/workflows/pages.yml copies it
const site = mkdtempSync(join(tmpdir(), "move-site-"));
try {
  cpSync(join(WEB, "site/index.html"), join(site, "index.html"));
  cpSync(join(WEB, "site/fonts"), join(site, "fonts"), { recursive: true });
  cpSync(join(WEB, "site/move"), join(site, "move"), { recursive: true });
  mkdirSync(join(site, "installer"));
  for (const f of ["index.html", "installer.js", "fm1ota.js", "fm1pkg.js", "releases.json"]) cpSync(join(WEB, "installer", f), join(site, "installer", f));
  const has = (p) => existsSync(join(site, p)), read = (p) => readFileSync(join(site, p), "utf8");
  const e0 = guardMovePage({ has, read });
  ok(!e0.length, `the assembled site passes the /move guard${e0.length ? ": " + e0.join("; ") : ""}`);
  ok(MOVE_RUNTIME.every(has), `everything the pages load at run time is published (${MOVE_RUNTIME.length} files)`);
  // a broken link, a missing font, a damaged data file: each fails the guard
  const page = read("move/index.html");
  writeFileSync(join(site, "move/index.html"), page.replace('href="css/move.css"', 'href="css/gone.css"'));
  ok(guardMovePage({ has, read }).some((e) => /css\/gone\.css/.test(e)), "a link to a file the site lacks: refused");
  writeFileSync(join(site, "move/index.html"), page);
  rmSync(join(site, "fonts/Jersey10-Regular.woff"));
  ok(guardMovePage({ has, read }).some((e) => /Jersey10-Regular\.woff/.test(e)), "the Jersey 10 font missing: refused");
  cpSync(join(WEB, "site/fonts/Jersey10-Regular.woff"), join(site, "fonts/Jersey10-Regular.woff"));
  writeFileSync(join(site, "move/data/move.json"), "{ not json");
  ok(guardMovePage({ has, read }).some((e) => /not JSON/.test(e)), "move/data/move.json damaged: refused");
  rmSync(join(site, "move/flash/index.html"));
  ok(guardMovePage({ has, read }).some((e) => /move\/flash\/index\.html/.test(e)), "the flasher's page missing: refused");
} finally {
  rmSync(site, { recursive: true, force: true });
}

// the release states: nothing unreleased is shown as if it were out
const data = JSON.parse(readFileSync(new URL("../move/data/move.json", import.meta.url), "utf8"));
const rm = data.remotefm14move;
ok(typeof rm.released === "boolean" && (rm.released || (rm.bundled === null && rm.version === null)),
   `data/move.json: RemoteFM14Move ${rm.released ? "released " + rm.version : "not released (no version, no bundled count)"}`);
ok(data.moveRemote.version && /^https:\/\/github\.com\//.test(data.moveRemote.repo), `data/move.json: Move Remote ${data.moveRemote.version}`);
const html = readFileSync(new URL("../move/index.html", import.meta.url), "utf8");
ok(html.indexOf('data-route="B"') < html.indexOf('data-route="A"') && html.indexOf('data-path="B"') < html.indexOf('data-path="A"'),
   "Route B (RemoteFM1 on the Move) comes first, in the cards and the steps");
ok(/<title>RemoteFM1 for Move<\/title>/.test(html), "the title is RemoteFM1 for Move");
ok(/<iframe class="flasher-frame" src="flash\/"[^>]*allow="midi"/.test(html), "the Wi-Fi flasher is embedded (flash/, MIDI allowed)");
ok(!/Ableton(?:'s)? logo|ableton\.com\/.*\.(?:svg|png)/i.test(html), "no Ableton marks");
const fl = readFileSync(new URL("../move/flash/flash.js", import.meta.url), "utf8"), flh = readFileSync(new URL("../move/flash/index.html", import.meta.url), "utf8");
ok(/import\(base \+ "installer\.js"\)/.test(fl) && /wifiRelease/.test(fl) && /releaseFile\(rel, "wifi"\)/.test(fl) && /releasedPackage\(rel, fetch, "wifi"/.test(fl),
   "the flasher imports the installer's code and asks only for the Wi-Fi package");
ok(/<body data-flasher>/.test(flh) && !/pkg-ble|pkg-usb|type="file"/.test(flh), "the flasher page: no other package, no file picker");
const rels = JSON.parse(readFileSync(new URL("../../installer/releases.json", import.meta.url), "utf8")).releases;
const wifi = rels.find((r) => r.wifi);
ok(!wifi || (wifi.wifi.file && wifi.wifi.sha256 && /^FM-1_729\d{6}$/.test(wifi.wifi.identity)),
   wifi ? `releases.json: RemoteFM1 ${wifi.version} has a Wi-Fi package (${wifi.wifi.identity})` : "releases.json: no Wi-Fi package yet (the flasher says not released)");

console.log(fails ? `move page: ${fails} of ${checks} FAILED` : `move page tests passed (${checks})`);
process.exit(fails ? 1 : 0);
