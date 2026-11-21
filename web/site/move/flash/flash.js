// SPDX-License-Identifier: MPL-2.0
// remotefm1.com/move/flash/: RemoteFM1's Wi-Fi build, flashed with the installer's own code. Everything that decides
// (the release list, the package's SHA-256 and identity, what the FM-1 runs, what may be installed over, the update
// protocol) is web/installer/installer.js and fm1ota.js, imported from where the site has them; this file is the page.
// The package is fixed: releases.json's newest release with a "wifi" entry (installer.js wifiRelease / releaseFile
// "wifi"). None yet: the page says so and flashes nothing.

const $ = (id) => document.getElementById(id);
// the installer: ../../installer/ on the published site (move/flash/ beside installer/); ../../../installer/ in this
// repo's web/ served as it is (site/move/flash/ and installer/)
async function findInstaller() {
  const base = /\/site\/move\/flash\/$/.test(location.pathname.replace(/index\.html$/, "")) ? "../../../installer/" : "../../installer/";
  try {
    const r = await fetch(base + "releases.json", { cache: "no-cache" });
    if (r.ok) return { base, list: await r.json() };
  } catch (_) { /* below */ }
  return null;
}

const meter = $("f-meter");
const setMeter = (f) => {
  const of = +meter.dataset.of, lit = Math.round(f * of);
  if (meter.children.length !== of) meter.replaceChildren(...Array.from({ length: of }, () => document.createElement("i")));
  [...meter.children].forEach((c, i) => c.classList.toggle("on", i < lit));
  $("f-pct").textContent = f >= 1 ? "DONE · 100 %" : f > 0 ? `WRITING · ${Math.round(f * 100)} %` : $("f-pct").textContent;
};
setMeter(0);
const ui = {
  say(text, kind = "") { $("f-status").textContent = text; $("f-status").className = kind; tellHeight(); },
  progress(f) { setMeter(f); },
  confirm: (t) => window.confirm(t),
  log(t) { $("f-log").textContent += t + "\n"; },
};
// the page embedding this one sizes its frame to it
const tellHeight = () => { try { if (parent !== window) parent.postMessage({ flasherHeight: document.documentElement.scrollHeight }, location.origin); } catch (_) { /* (not embedded) */ } };
addEventListener("load", tellHeight);
new ResizeObserver(tellHeight).observe(document.body);

const found = await findInstaller();
if (!found) {
  ui.say("The release list could not be loaded. Reload the page.", "bad");
  $("f-meta").textContent = "";
} else {
  const { base, list } = found;
  $("f-installer").href = base;
  const I = await import(base + "installer.js");
  const rel = I.wifiRelease(list.releases || []);
  const f = rel && I.releaseFile(rel, "wifi");
  let access = null, busy = false;
  const hasMidi = !!navigator.requestMIDIAccess;
  $("f-browser").hidden = hasMidi;
  if (rel) {
    $("f-title").textContent = `RemoteFM1 ${rel.version} · Wi-Fi`;
    $("f-file").textContent = f.file;
    $("f-ver").textContent = rel.version;
    $("f-id").textContent = f.identity;
    const minor = +(rel.version.split(".")[1] || 0);
    $("f-kv").style.setProperty("--v", Math.min(1, .2 + minor / 10));
    $("f-meta").textContent = `Released ${rel.date}. SHA-256 ${f.sha256}`;
    $("f-go-t").textContent = `Flash RemoteFM1 ${rel.version} Wi-Fi`;
    $("f-go").disabled = !hasMidi;
  } else {
    $("f-title").textContent = "RemoteFM1 Wi-Fi";
    $("f-file").textContent = "not released yet";
    $("f-ver").textContent = "soon";
    $("f-id").textContent = "FM-1_729…";
    $("f-meta").textContent = `The newest release, RemoteFM1 ${list.releases[0].version}, has no Wi-Fi package.`;
    $("f-soon").hidden = false;
    $("f-go-t").textContent = "Not released yet";
    $("f-pct").textContent = "WAITING FOR THE RELEASE";
    $("f-go").disabled = true;
  }
  $("f-check").disabled = !hasMidi;

  const retry = $("f-retry");
  const midi = async () => {
    if (access) return access;
    try {
      access = await I.requestMidi(navigator, { say: ui.say, late(again) { retry.hidden = false; retry.onclick = () => { retry.hidden = true; again(); }; } });
    } finally { retry.hidden = true; retry.onclick = null; }
    return access;
  };
  const run = async (what) => {
    if (busy) return;
    busy = true;
    $("f-go").disabled = $("f-check").disabled = true;
    try { await what(); } catch (e) {
      ui.say(e && (e.name === "SecurityError" || e.name === "NotAllowedError") ? "The browser refused MIDI access: allow it (the site-settings icon left of the URL) and try again." : (e && e.message) || String(e), "bad");
      ui.log(String(e && e.stack || e));
    } finally {
      busy = false;
      $("f-go").disabled = !(rel && hasMidi);
      $("f-check").disabled = !hasMidi;
    }
  };
  $("f-go").addEventListener("click", () => run(async () => {
    $("f-next").hidden = true;
    ui.say("Downloading and checking the Wi-Fi package...");
    const pkg = await I.releasedPackage(rel, fetch, "wifi", base + "firmware/");
    const a = await midi();
    ui.say("Looking for the FM-1...");
    const r = await I.install(a, pkg, ui, { override: $("f-override").checked, midiClosed: $("f-midi-closed").checked });
    if (r === "done" || r === "resumed") { $("f-next").hidden = false; tellHeight(); }
    if (r === "refused" && !$("f-override").checked && /not installed over from here/.test($("f-status").textContent)) $("f-override-box").hidden = false;
  }));
  $("f-check").addEventListener("click", () => run(async () => {
    const a = await midi();
    ui.say("Looking for the FM-1...");
    ui.say(I.describe(await I.look(a, ui.log)));
  }));
}
document.body.dataset.ready = "1";
tellHeight();
