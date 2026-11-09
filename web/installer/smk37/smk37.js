// SPDX-License-Identifier: GPL-3.0-only
// The SMK-37 test installer (index.html beside it; docs/SMK37.md): RemoteFM1's untested community builds for the
// M-Vave SMK-37 Pro, SMK-37 Elite, MKE-P37 and the Starrykey 37 Play, over Web MIDI. Linked from no page of the site:
// the link goes to testers. After ChoralRoot's SMK-37 test installer (web/smk37.html, GPL-3.0-only) and RemoteFM1's
// own installer (../installer.js), with tools/fm1_install.py's rules for the SMK-37:
//   - the package by the keyboard's model, from its update identity: SMK-37 Pro_ / SMK-37 Elite_ / MKE-P37_ (the stock
//     firmware's, and RemoteFM1's own once it runs); any other name of the SMK-37 (the Starrykey 37 Play's
//     STARRYKEY-37 PLAY_016) takes the Pro's, written with the same-keyboard rule (fm1ota.js sameKeyboard);
//   - its SHA-256 and its identity checked against releases-smk37.json in the browser before anything is sent;
//   - an FM-1 (any FM-1_ identity, any FM-1 update mode) refused before anything but the identity query is sent, as
//     are ChanceTheMaker's SMK firmwares (their own updater), an unknown firmware and another firmware's update mode
//     (only RemoteFM1's own SMK loader, ota-MODEL_9NN, is resumed: fm1_install.py's rule);
//   - an install cut off finished from that loader, with its model's package.
// The protocol is ../fm1ota.js and the package reading ../fm1pkg.js, both unchanged (they already read the SMK-37's
// 36 identity blocks and carry the same-keyboard rule). Everything that decides is here and runs without a page
// (test_smk_installer.mjs drives it against a fake keyboard); the DOM wiring is at the end, in the browser only.
import { Updater, classifyFirmware, sameKeyboard, OUR_LOADER, OTHER_SMK } from "../fm1ota.js";
import { productOf, logicalImage, sha256hex } from "../fm1pkg.js";

const INFO_REQ = [0xF0, 0x7D, 0x46, 0x4C, 0x01, 0xF7];   // the backup protocol's INFO (Felucca-family firmwares)

// the three models with a package of their own; every other SMK-37 name takes the Pro's
export const MODELS = { "SMK-37 PRO": "pro", "SMK-37 ELITE": "elite", "MKE-P37": "mke" };
export const isFm1 = (model) => /^(ota-)?FM-1$/i.test(model || "");
// a model name (with or without ota-) -> { key, any } | null (not an SMK-37). any: another model of the keyboard
export function modelOf(model) {
  const n = String(model || "").replace(/^ota-/i, "");
  if (isFm1(n)) return null;
  const k = MODELS[n.toUpperCase()];
  if (k) return { key: k, any: false };
  return sameKeyboard(n, "SMK-37 Pro") ? { key: "pro", any: true } : null;
}
// RemoteFM1's own SMK-37 identity (MODEL_72XXYYZZ, MODEL_7200 a development build; 52 before 0.3.7, still read)
// -> "0.3.1" | "development build" | null
export function smkRemoteVersion(identity) {
  if (/^(SMK-37 Pro|SMK-37 Elite|MKE-P37)_[57]200$/i.test(identity || "")) return "development build";
  const m = /^(SMK-37 Pro|SMK-37 Elite|MKE-P37)_[57]2(\d\d)(\d\d)(\d\d)$/i.exec(identity || "");
  return m ? `${+m[2]}.${+m[3]}.${+m[4]}` : null;
}
// the update modes that are resumed: RemoteFM1's SMK loader (ota-MODEL_900), as fm1_install.py (ota-MODEL_9NN)
export const resumable = (id) => OUR_LOADER(id) && /_9\d\d$/.test(id.text) && !!modelOf(id.model);

// a model's package in a release: the default one (USB + Bluetooth), or with variant "usb" its USB-only one
export function releaseFile(rel, key, variant = "") {
  const m = rel.models[key];
  if (variant === "usb" && m.usbOnly) return { file: m.usbOnly.file, sha256: m.usbOnly.sha256, identity: m.identity, usb: true, name: m.name };
  return { file: m.file, sha256: m.sha256, identity: m.identity, usb: false, name: m.name };
}

// the package for a model as the site hosts it, its SHA-256 and identity the list's -> { product, image, label, file }
export async function releasedPackage(rel, key, fetchFn = fetch, variant = "") {
  const f = releaseFile(rel, key, variant);
  const label = `RemoteFM1 ${rel.version} for the ${f.name}${f.usb ? " (USB only)" : ""}`;
  let bytes;
  try {
    const r = await fetchFn(`firmware/${f.file}`, { cache: "no-cache" });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    bytes = new Uint8Array(await r.arrayBuffer());
  } catch (e) { throw new Error(`The package for ${label} could not be downloaded (${e.message}). Nothing was written.`); }
  if (await sha256hex(bytes) !== f.sha256)
    throw new Error(`The file for ${label} is not the released one (its SHA-256 does not match). Nothing was written. Reload the page.`);
  let product = "";
  try { product = productOf(bytes); } catch (_) { /* (reported below) */ }
  if (product !== f.identity)
    throw new Error(`The file for ${label} is not the released one (it installs as ${product || "nothing"}, not ${f.identity}). Nothing was written.`);
  return { product, image: logicalImage(bytes), label, file: f.file };
}

// the version text a Felucca-family firmware answers to INFO, or null (the stock firmware, RemoteFM1, others)
export function deviceInfo(input, output, ms = 1500) {
  return new Promise(async (resolve) => {
    try { await input.open(); await output.open(); } catch (_) { return resolve(null); }
    const done = (v) => { clearTimeout(timer); input.onmidimessage = null; resolve(v); };
    const timer = setTimeout(() => done(null), ms);
    input.onmidimessage = (e) => {
      const d = e.data;
      if (d.length < 6 || d[0] !== 0xF0 || d[1] !== 0x7D || d[2] !== 0x46 || d[3] !== 0x4C || d[4] !== 1) return;
      const a = Array.from(d.slice(5, -1)), end = a.indexOf(0);
      done(String.fromCharCode(...a.slice(0, end < 0 ? a.length : end)) || null);
    };
    try { output.send(INFO_REQ); } catch (_) { done(null); }
  });
}

export const TEXT = {
  accept: "Tick the box above first: this is an untested build.",
  midiClosed: "First close every app that uses MIDI (a DAW, a synth editor, other browser tabs with an FM-1 or SMK-37 page) and tick the box: their MIDI can stall an install.",
  none: "No SMK-37 found. Connect it with a USB data cable, switch it on, and close every other app that uses MIDI.",
  several: "More than one keyboard or FM-1 answers. Connect only the SMK-37 to install, then press Install again. Nothing was written.",
  fm1: "This is an FM-1: this page installs RemoteFM1's SMK-37 builds only, which must never go on an FM-1. Use RemoteFM1's own installer. Nothing was written.",
  notsmk: "The connected device is not an SMK-37 (it reports {id}). Nothing was written.",
  other: "This keyboard runs one of ChanceTheMaker's SMK firmwares ({id}): they have their own updater, which takes only their packages. Go back to the stock firmware with M-UPGRADE first. Nothing was written.",
  foreign: "The keyboard is in the update mode of another firmware ({id}): finish that update with its own updater (M-UPGRADE for the stock firmware). Nothing was written.",
  refused: "RemoteFM1 is not installed over {name} from here. Go back to the stock firmware with M-UPGRADE first. Nothing was written.",
  interrupted: "The install did not finish. Switch the keyboard on again (hold OCTAVE- while switching on if it does not come up in update mode) and press Install again: it finishes the write. To give up, install M-Vave's stock package for your model with M-UPGRADE.",
};
const fill = (t, v) => t.replace(/\{(\w+)\}/g, (_, k) => v[k] ?? "");

// what the connected keyboard is and runs -> { none } | { several, list } | { fm1, id } | { notsmk, id } | { other, id }
// | { loader, id, model } | { foreign, id } | { id, model, fw, port }
export async function look(access, log = () => {}) {
  const up = new Updater(access, log);
  const all = await up.all();
  if (!all.length) return { none: true };
  if (all.length > 1) return { several: true, list: all.map((d) => `${d.id.text} (${d.name})`) };
  const d = all[0], id = d.id;
  log(`device: ${id.text} (port ${d.name})`);
  if (isFm1(id.model)) return { fm1: true, id: id.text };
  if (OTHER_SMK(id)) return { other: true, id: id.text };
  const model = modelOf(id.model);
  if (!model) return { notsmk: true, id: id.text };
  if (/^ota-/i.test(id.model)) return resumable(id) ? { loader: true, id: id.text, model } : { foreign: true, id: id.text };
  const rv = smkRemoteVersion(id.text);
  const fw = rv ? { verdict: "allow", kind: "remotefm1", name: `RemoteFM1 ${rv}` }
    : classifyFirmware(id.text, await deviceInfo(d.input, d.output), d.name);
  log(`running: ${fw.name} -> ${fw.verdict} (${fw.kind})`);
  return { id: id.text, model, fw, port: d.name };
}

// a sentence for "What is on my SMK-37?"
export function describe(st, rel = null) {
  if (st.none) return TEXT.none;
  if (st.several) return `${TEXT.several} (${st.list.join(", ")})`;
  if (st.fm1) return TEXT.fm1;
  if (st.other) return fill(TEXT.other, st);
  if (st.notsmk) return fill(TEXT.notsmk, st);
  if (st.foreign) return fill(TEXT.foreign, st);
  const pkgName = rel ? ` Its package: ${rel.models[st.model.key].name}.` : "";
  if (st.loader) return `The keyboard is in RemoteFM1's update mode (${st.id}): an install was cut off. Press Install to finish it.${pkgName}`;
  if (st.fw.verdict !== "allow") return fill(TEXT.refused, st.fw);
  return `The keyboard runs ${st.fw.kind === "stock" ? `M-Vave's stock firmware (${st.id})` : `${st.fw.name} (${st.id})`}.${pkgName}`;
}

// install on the connected SMK-37, or finish an install that was cut off. ui: { say(text, kind), progress(0..1),
// confirm(text) -> bool, log(text) }; opts: { accepted (the risk box), midiClosed, variant ("" | "usb"), fetch }.
// Returns "done" | "resumed" | "refused" | "cancelled"; throws with a message the page shows (e.interrupted: the write
// had begun)
export async function install(access, rel, ui, opts = {}) {
  if (!opts.accepted) { ui.say(TEXT.accept, "bad"); return "refused"; }
  const st = await look(access, ui.log);
  if (!(st.loader || (st.fw && st.fw.verdict === "allow"))) { ui.say(describe(st), "bad"); return "refused"; }
  let began = false;
  const step = (k, n) => {
    if (k === "start") ui.say(`Starting the update on ${n}...`);
    if (k === "verify") ui.progress(Math.min(0.05, n / 1500));
    if (k === "loader") ui.say("The keyboard restarts into update mode...");
    if (k === "write") { began = true; ui.say("Writing. Do not unplug the keyboard or switch it off."); ui.progress(Math.min(0.99, 0.05 + n / 1350)); }
    if (k === "reboot") ui.say("Restarting...");
    if (k === "done") ui.progress(1);
  };
  const cut = (e) => { if (began || st.loader) e.interrupted = true; return e; };
  if (st.loader) {
    const pkg = await releasedPackage(rel, st.model.key, opts.fetch || fetch, opts.variant);
    ui.say(`The keyboard is in update mode (${st.id}): finishing the write of ${pkg.label}...`);
    began = true;
    let ok;
    try { ok = await new Updater(access, ui.log).resume(pkg.image, step, { package: pkg.product, anyModel: st.model.any }); }
    catch (e) { throw cut(e); }
    if (!ok) throw cut(new Error("The write did not finish."));
    ui.say(`Done: ${pkg.label} is installed.`, "ok");
    return "resumed";
  }
  if (!opts.midiClosed) { ui.say(TEXT.midiClosed, "bad"); return "refused"; }
  const pkg = await releasedPackage(rel, st.model.key, opts.fetch || fetch, opts.variant);
  const over = st.fw.kind === "stock" ? `M-Vave's stock firmware (${st.id})` : `${st.fw.name}`;
  if (!ui.confirm(`Install ${pkg.label} over ${over}? This build has not run on an SMK-37 yet. ` +
                  "Keep the keyboard connected directly to this computer; do not unplug it while it writes."))
    return "cancelled";
  ui.log(`install: ${pkg.file} (${pkg.product}) on ${st.id}${st.model.any ? " (another model of the SMK-37)" : ""}`);
  try { await new Updater(access, ui.log).install(pkg.image, pkg.product, step, st.model.any); }
  catch (e) { throw cut(e); }
  ui.say(`Done: ${pkg.label} is installed.`, "ok");
  return "done";
}

// ---- the page (the browser only) ----
if (typeof document !== "undefined") {
  const $ = (id) => document.getElementById(id);
  let access = null, rel = null, busy = false;
  const ui = {
    say(text, kind = "") { $("status").textContent = text; $("status").className = "status " + kind; },
    progress(f) { $("bar").hidden = false; $("bar").value = Math.round(f * 1000); },
    confirm: (t) => window.confirm(t),
    log(t) { $("log").textContent += t + "\n"; },
  };
  const midi = async () => {
    if (access) return access;
    if (!navigator.requestMIDIAccess) throw new Error("This browser has no Web MIDI with SysEx: use Chrome or Edge on a computer.");
    const hint = setTimeout(() => ui.say("Allow MIDI: your browser is asking (the prompt by the address bar, or the site-settings icon left of the URL).", "wait"), 1000);
    try { access = await navigator.requestMIDIAccess({ sysex: true }); } finally { clearTimeout(hint); }
    return access;
  };
  const sync = () => { $("go").disabled = busy || !rel || !$("accept").checked; };
  const variant = () => ($("pkg-usb").checked ? "usb" : "");
  const run = async (what) => {
    if (busy) return;
    busy = true; sync();
    document.body.classList.add("busy");
    $("bar").hidden = true; $("interrupted").hidden = true; $("done").hidden = true;
    try { await what(); } catch (e) {
      ui.say(e && (e.name === "SecurityError" || e.name === "NotAllowedError") ? "The browser refused MIDI access: allow it (the site-settings icon left of the URL) and try again." : (e && e.message) || String(e), "bad");
      ui.log(String(e && e.stack || e));
      if (e && e.interrupted) $("interrupted").hidden = false;
    } finally {
      busy = false; sync();
      document.body.classList.remove("busy");
    }
  };
  window.addEventListener("beforeunload", (e) => { if (busy) { e.preventDefault(); e.returnValue = ""; } });
  $("accept").addEventListener("change", sync);
  $("go").addEventListener("click", () => run(async () => {
    const a = await midi();
    ui.say("Looking for the SMK-37...");
    const r = await install(a, rel, ui, { accepted: $("accept").checked, midiClosed: $("midi-closed").checked, variant: variant() });
    if (r === "done" || r === "resumed") $("done").hidden = false;
  }));
  $("check").addEventListener("click", () => run(async () => {
    const a = await midi();
    ui.say("Looking for the SMK-37...");
    ui.say(describe(await look(a, ui.log), rel));
  }));
  if (!navigator.requestMIDIAccess) $("browser-warn").hidden = false;
  // the player: ../../ on the published site, ../../site/ in the source tree (web/ served as is)
  fetch("../../site/catalog.json", { method: "HEAD" }).then((r) => { if (r.ok) for (const a of document.querySelectorAll("a.player")) a.href = "../../site/"; }, () => {});
  fetch("releases-smk37.json", { cache: "no-cache" }).then((r) => r.json()).then((list) => {
    rel = list.releases[0];
    $("go").textContent = `Install RemoteFM1 ${rel.version}`;
    $("ver").textContent = rel.version;
    $("date").textContent = rel.date;
    $("notes").replaceChildren(...rel.notes.map((n) => Object.assign(document.createElement("li"), { textContent: n })));
    const files = () => $("files").replaceChildren(...Object.keys(rel.models).map((k) => {
      const f = releaseFile(rel, k, variant()), li = document.createElement("li");
      const a = Object.assign(document.createElement("a"), { href: `firmware/${f.file}`, textContent: f.file });
      li.append(Object.assign(document.createElement("b"), { textContent: f.name }), ` (${f.identity}): `, a,
                Object.assign(document.createElement("span"), { className: "sha", textContent: ` SHA-256 ${f.sha256}` }));
      return li;
    }));
    files();
    for (const id of ["pkg-ble", "pkg-usb"]) $(id).addEventListener("change", files);
    sync();
  }, (e) => {
    ui.say("The release list could not be loaded. Reload the page.", "bad");
    ui.log(String(e && e.stack || e));
  });
}
