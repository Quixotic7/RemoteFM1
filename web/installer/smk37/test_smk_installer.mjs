// SPDX-License-Identifier: GPL-3.0-only
// The SMK-37 test installer's decisions (smk37.js) against a simulated keyboard on a fake Web MIDI, end to end in Node
// (after ../test_installer.mjs and ChoralRoot's web/test_smk_installer.mjs, GPL-3.0-only). Never against a real unit:
// the only MIDI here is the fake's.
//   - the list: releases-smk37.json, firmware/SHA256SUMS and the six packages agree, each installs as its identity
//     (36 identity blocks), firmware/ holds those six only;
//   - the page: every element smk37.js uses is in index.html, noindex, linked from no page of the site, published by
//     the Pages workflow;
//   - nothing before the risk box is ticked; the SMK-37 Pro, the Elite and the MKE-P37 each get their own package (from
//     the stock firmware, found by its "SMK-37 ..." port, and over RemoteFM1 itself on its "FM-1" port), the Starrykey
//     37 Play the Pro's; the USB-only choice fetches and checks its own file; a package whose SHA-256 or identity
//     differ refused with nothing sent; an FM-1 (stock, RemoteFM1, ChoralRoot, its update mode) refused with nothing
//     fetched or sent; ChanceTheMaker's SMK firmware, an unknown firmware, M-UPGRADE's and an FM-1's update mode
//     refused; an install cut off finished with its loader's model's package; no fresh install before the MIDI box;
//     a cancel sends nothing; two devices refused.
//   node web/installer/smk37/test_smk_installer.mjs
import { readFileSync, readdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { pack7, unpack7 } from "../fm1ota.js";
import { logicalImage } from "../fm1pkg.js";
import { install, look, describe, modelOf, smkRemoteVersion, releaseFile, releasedPackage, TEXT } from "./smk37.js";

const realTimeout = setTimeout;                    // the protocol's waits (seconds) shortened 20 times
globalThis.setTimeout = (f, ms, ...a) => realTimeout(f, ms >= 200 ? ms / 20 : ms, ...a);

let fails = 0, checks = 0;
const ok = (cond, what) => { checks++; if (!cond) fails++; console.log(`${what.padEnd(84)} ${cond ? "ok" : "FAIL"}`); };
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const here = (p) => new URL(p, import.meta.url);
const read = (p) => readFileSync(here(p), "utf8");
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];

// ---- the list, the sums and the files ----
const list = JSON.parse(read("./releases-smk37.json"));
const rel = list.releases[0];
const sums = read("./firmware/SHA256SUMS");
const RAW = {};
for (const k of ["pro", "elite", "mke"]) for (const v of ["", "usb"]) {
  const f = releaseFile(rel, k, v);
  const b = new Uint8Array(readFileSync(here(`./firmware/${f.file}`)));
  RAW[f.file] = b;
  const h = createHash("sha256").update(b).digest("hex");
  let prod = "";
  try { prod = (await releasedPackage(rel, k, async () => ({ ok: true, arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.length) }), v)).product; }
  catch (e) { prod = e.message; }
  ok(h === f.sha256 && sums.includes(`${h}  ${f.file}`) && prod === f.identity && f.usb === (v === "usb"),
     `${f.file}: the list, SHA256SUMS and the file agree; installs as ${prod}`);
}
{
  const files = readdirSync(here("./firmware/")).filter((f) => f !== "SHA256SUMS").sort().join();
  const summed = sums.trim().split("\n").map((l) => l.split(/\s+/)[1]).sort().join();
  ok(files === Object.keys(RAW).sort().join() && summed === files, `firmware/ holds the six listed packages only`);
  ok(rel.version === "0.3.1" && rel.tag === "v0.3.1-smk37" && rel.models.pro.identity === "SMK-37 Pro_52000301" &&
     rel.models.elite.identity === "SMK-37 Elite_52000301" && rel.models.mke.identity === "MKE-P37_52000301" &&
     rel.notes.some((n) => /untested community build/.test(n)), "0.3.1-smk37: the three models' identities, an untested community build");
}

// ---- the page, and that nothing links it ----
{
  const page = read("./index.html"), js = read("./smk37.js");
  const ids = [...new Set([...js.matchAll(/\$\("([\w-]+)"\)/g)].map((m) => m[1]))];
  const missing = ids.filter((id) => !new RegExp(`id="${id}"`).test(page));
  ok(ids.length >= 12 && !missing.length, `the page has every element smk37.js uses (${ids.length})${missing.length ? ": missing " + missing.join(", ") : ""}`);
  ok(/<meta name="robots" content="noindex, nofollow">/.test(page), "the page: noindex, nofollow");
  ok(/has not run on an SMK-37/.test(page) && /M-UPGRADE/.test(page) && /SMK37\.md#what-to-test/.test(page) && /USB only/.test(page),
     "the page: untested, the way back (M-UPGRADE), the tester steps, the USB-only choice");
  const linkers = [];
  const walk = (dir) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = `${dir}/${e.name}`;
      if (e.isDirectory()) { if (e.name !== "smk37" && e.name !== "vendor" && e.name !== "node_modules") walk(p); }
      else if (/\.(html|js|mjs|json|css|xml|txt)$/.test(e.name) && /installer\/smk37|["'`(]\.{0,2}\/?smk37\//.test(readFileSync(p, "utf8"))) linkers.push(p);
    }
  };
  const web = new URL("../../", import.meta.url).pathname.replace(/\/$/, "");
  walk(web);
  ok(!linkers.length, `no page, script or list of the site links installer/smk37/${linkers.length ? ": " + linkers.join(", ") : ""}`);
  const wf = readFileSync(new URL("../../../.github/workflows/pages.yml", import.meta.url), "utf8");
  ok(/web\/installer\/smk37\/index\.html/.test(wf) && /_site\/installer\/smk37/.test(wf) && /installer\/smk37\/firmware && sha256sum -c/.test(wf) &&
     /node web\/installer\/smk37\/test_smk_installer\.mjs/.test(wf), "Pages publishes installer/smk37/ (its sums checked) and runs this test");
}

// ---- the decisions ----
ok(modelOf("SMK-37 Pro").key === "pro" && !modelOf("SMK-37 Pro").any && modelOf("ota-SMK-37 Elite").key === "elite" &&
   modelOf("MKE-P37").key === "mke" && modelOf("STARRYKEY-37 PLAY").key === "pro" && modelOf("STARRYKEY-37 PLAY").any &&
   modelOf("FM-1") === null && modelOf("ota-FM-1") === null && modelOf("MicroFreak") === null, "modelOf: the three models, the Starrykey the Pro's, never an FM-1");
ok(smkRemoteVersion("SMK-37 Elite_52000301") === "0.3.1" && smkRemoteVersion("MKE-P37_5200") === "development build" &&
   smkRemoteVersion("FM-1_52000301") === null && smkRemoteVersion("SMK-37 Pro_016") === null, "RemoteFM1's SMK identities told apart");

// a keyboard: the handshake, INFO (version: the backup protocol's answer, or none), step 1 (four reads and
// "success"), then the loader (six reads and the finish), then the firmware written; port names as on a unit
class FakeKbd {
  constructor(image, { identity, name = "SMK-37 Pro", loader = "ota-SMK-37 Pro_900", final = "SMK-37 Pro_52000301", version = null } = {}) {
    this.image = image; this.served = 0; this.bad = 0; this.upgrades = 0; this.queries = 0; this.version = version;
    this.loader = loader; this.final = final;
    this.access = { inputs: new Map(), outputs: new Map() };
    this.boot(identity, identity.startsWith("ota-") ? "FM-1" : name);
  }
  boot(identity, name) {
    this.identity = identity; this.port = name; this.waiting = null; this.queue = [];
    for (const m of [this.access.inputs, this.access.outputs]) { for (const p of m.values()) p.state = "disconnected"; m.clear(); }
    const id = Math.random().toString(36).slice(2);
    this.input = { id: "i" + id, name, state: "connected", onmidimessage: null, open: async () => {} };
    this.output = { id: "o" + id, name, state: "connected", open: async () => {}, send: (d) => {
      if (this.output.state !== "connected") throw new Error("InvalidStateError");
      realTimeout(() => this.rx(Array.from(d)), 1);
    } };
    this.access.inputs.set(this.input.id, this.input);
    this.access.outputs.set(this.output.id, this.output);
  }
  tx(bytes) { const i = this.input; realTimeout(() => { if (i.state === "connected" && i.onmidimessage) i.onmidimessage({ data: Uint8Array.from(bytes) }); }, 1); }
  rx(d) {
    if (d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x46 && d[3] === 0x4C) {     // INFO
      if (d[4] === 1 && this.version && !this.identity.startsWith("ota-"))
        this.tx([0xF0, 0x7D, 0x46, 0x4C, 1, ...new TextEncoder().encode(this.version), 0, 0xF7]);
      return;
    }
    if (eq(d, HS)) {
      this.queries++;
      const t = [...new TextEncoder().encode(this.identity)];
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (eq(d, UPGRADE)) {
      this.upgrades++;
      this.queue = this.identity.startsWith("ota-")
        ? [...Array.from({ length: 6 }, (_, k) => [k * 512, 512]), [0xF0000000, 8]]
        : [[0, 64], [0x40, 160], [0x1000, 512], [0xE0000000, 8]];
      this.next();
    } else if (this.waiting) {
      const u = unpack7(d.slice(1, -1));
      const [addr, len] = this.waiting;
      const got = u.slice(14, 14 + (addr >= 0xE0000000 ? 8 : len));
      const want = addr >= 0xE0000000 ? [...new TextEncoder().encode("success"), 0] : Array.from(this.image.subarray(addr, addr + len));
      if (!eq(got, want)) this.bad++;
      this.waiting = null;
      this.served++;
      if (addr === 0xE0000000) realTimeout(() => this.boot(this.loader, "FM-1"), 20);
      else if (addr === 0xF0000000) realTimeout(() => this.boot(this.final, "FM-1"), 20);   // RemoteFM1's USB name
      else this.next();
    }
  }
  next() {
    const r = this.queue.shift();
    if (!r) return;
    this.waiting = r;
    const [addr, len] = r;
    const u = [0, 0x59, 0x30, 0, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    u.push(~s & 0xFF);
    this.tx([0xF0, ...pack7(u), 0xF7]);
  }
}
const both = (...devs) => {
  const inputs = new Map(), outputs = new Map();
  for (const d of devs) { for (const [k, v] of d.access.inputs) inputs.set(k, v); for (const [k, v] of d.access.outputs) outputs.set(k, v); }
  return { inputs, outputs };
};
const UI = (answer = true) => {
  const u = { said: [], confirms: 0, prog: 0 };
  return Object.assign(u, { say(t) { u.said.push(t); }, progress(f) { u.prog = f; }, confirm() { u.confirms++; return answer; }, log() {} });
};
const served = (over = {}) => {
  const asked = [];
  const f = async (url) => {
    asked.push(url);
    const b = over[url] || RAW[url.replace(/^firmware\//, "")];
    return b ? { ok: true, status: 200, arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.length) } : { ok: false, status: 404 };
  };
  f.asked = asked;
  return f;
};
const img = (k, v = "") => logicalImage(RAW[releaseFile(rel, k, v).file]);
const GO = { accepted: true, midiClosed: true };
const last = (ui) => ui.said[ui.said.length - 1] || "";

// nothing before the risk box
{
  const dev = new FakeKbd(img("pro"), { identity: "SMK-37 Pro_016" });
  const ui = UI(), f = served();
  const r = await install(dev.access, rel, ui, { midiClosed: true, fetch: f });
  ok(r === "refused" && last(ui) === TEXT.accept && dev.queries === 0 && !f.asked.length, "the risk box not ticked: refused, nothing fetched or sent (not even the identity query)");
}
// the three models from the stock firmware (its "SMK-37 ..." port), the Starrykey the Pro's
for (const [k, identity, name] of [["pro", "SMK-37 Pro_016", "SMK-37 Pro"], ["elite", "SMK-37 Elite_016", "SMK-37 Elite"],
  ["mke", "MKE-P37_016", "MKE-P37"], ["pro", "STARRYKEY-37 PLAY_016", "Starrykey 37"]]) {
  const m = rel.models[k], model = m.identity.split("_")[0];
  const dev = new FakeKbd(img(k), { identity, name, loader: `ota-${model}_900`, final: m.identity });
  const ui = UI(), f = served();
  const r = await install(dev.access, rel, ui, { ...GO, fetch: f });
  ok(r === "done" && dev.upgrades === 2 && dev.bad === 0 && dev.identity === m.identity && dev.port === "FM-1" && ui.prog === 1 &&
     f.asked.join() === `firmware/${m.file}` && ui.confirms === 1,
     `${identity} on port "${name}": ${m.file}, written, back as ${dev.identity} on "FM-1"`);
}
{
  const st = await look(new FakeKbd(img("pro"), { identity: "SMK-37 Pro_016" }).access);
  ok(/stock firmware \(SMK-37 Pro_016\).*SMK-37 Pro \(and the Starrykey 37 Play\)/.test(describe(st, rel)), "look: the stock firmware, and the package it gets");
}
// over RemoteFM1 itself (its "FM-1" port, the model's identity): the same model's package
{
  const dev = new FakeKbd(img("elite"), { identity: "SMK-37 Elite_52000301", name: "FM-1", loader: "ota-SMK-37 Elite_900", final: "SMK-37 Elite_52000301" });
  const ui = UI(), f = served();
  const r = await install(dev.access, rel, ui, { ...GO, fetch: f });
  ok(r === "done" && dev.bad === 0 && f.asked.join() === `firmware/${rel.models.elite.file}` && dev.identity === "SMK-37 Elite_52000301",
     "RemoteFM1 on an Elite (port \"FM-1\"): the Elite's package again");
  ok(/runs RemoteFM1 0\.3\.1 \(SMK-37 Elite_52000301\)/.test(describe(await look(new FakeKbd(img("elite"), { identity: "SMK-37 Elite_52000301", name: "FM-1" }).access), rel)),
     "look: RemoteFM1 on an SMK-37, by its identity");
}
// the USB-only choice
{
  const dev = new FakeKbd(img("mke", "usb"), { identity: "MKE-P37_016", name: "MKE-P37", loader: "ota-MKE-P37_900", final: "MKE-P37_52000301" });
  const ui = UI(), f = served();
  const r = await install(dev.access, rel, ui, { ...GO, variant: "usb", fetch: f });
  ok(r === "done" && dev.bad === 0 && f.asked.join() === `firmware/${rel.models.mke.usbOnly.file}` && /\(USB only\) is installed/.test(last(ui)),
     "USB only on an MKE-P37: its own file fetched, checked and written");
}
// a package whose SHA-256 or identity differ
{
  const dev = new FakeKbd(img("pro"), { identity: "SMK-37 Pro_016" });
  const ui = UI(), f = served({ [`firmware/${rel.models.pro.file}`]: RAW[rel.models.pro.usbOnly.file] });
  let e = "";
  try { await install(dev.access, rel, ui, { ...GO, fetch: f }); } catch (x) { e = x.message; }
  ok(/SHA-256 does not match/.test(e) && dev.upgrades === 0 && dev.served === 0, "the Pro's file not the listed one (SHA-256): refused, nothing sent");
  const bad = RAW[rel.models.pro.file].slice(); bad[90000] ^= 1;
  e = "";
  try { await install(dev.access, rel, UI(), { ...GO, fetch: served({ [`firmware/${rel.models.pro.file}`]: bad }) }); } catch (x) { e = x.message; }
  ok(/SHA-256 does not match/.test(e) && dev.upgrades === 0, "one byte changed: refused, nothing sent");
  const wrong = { ...rel, models: { ...rel.models, pro: { ...rel.models.pro, sha256: rel.models.elite.sha256, file: rel.models.elite.file } } };
  e = "";
  try { await install(dev.access, wrong, UI(), { ...GO, fetch: served() }); } catch (x) { e = x.message; }
  ok(/installs as SMK-37 Elite_52000301, not SMK-37 Pro_52000301/.test(e) && dev.upgrades === 0, "a listed file of another model under the Pro's name: refused (its identity), nothing sent");
  e = "";
  try { await install(dev.access, rel, UI(), { ...GO, fetch: async () => ({ ok: false, status: 404 }) }); } catch (x) { e = x.message; }
  ok(/could not be downloaded \(HTTP 404\)/.test(e) && dev.upgrades === 0, "a package that cannot be downloaded: said so, nothing sent");
}
// an FM-1, at install and at resume: refused before anything is fetched or sent (but the identity query)
for (const [identity, version, what] of [["FM-1_015", null, "an FM-1 on V15"], ["FM-1_52000301", null, "an FM-1 on RemoteFM1"],
  ["FM-1_920", "ChoralRoot 0.27", "an FM-1 on ChoralRoot"], ["ota-FM-1_900", null, "an FM-1 in RemoteFM1's update mode (resume)"],
  ["ota-FM-1_015", null, "an FM-1 in M-VAVE's update mode (resume)"]]) {
  const dev = new FakeKbd(img("pro"), { identity, name: "FM-1", version });
  const ui = UI(), f = served();
  const r = await install(dev.access, rel, ui, { ...GO, fetch: f });
  ok(r === "refused" && last(ui) === TEXT.fm1 && dev.upgrades === 0 && dev.served === 0 && !f.asked.length && !ui.confirms,
     `${what}: refused, nothing fetched or sent`);
}
// other firmwares and update modes
for (const [identity, name, re, what] of [["SMK-37_931", "SMK-37", /ChanceTheMaker/, "ChanceTheMaker's SMK firmware"],
  ["ota-SMK-37_931", "SMK-37", /ChanceTheMaker/, "its updater"], ["SMK-37 Pro_500", "SMK-37 Pro", /not installed over/, "an unknown SMK-37 firmware"],
  ["ota-SMK-37 Pro_016", "FM-1", /update mode of another firmware/, "M-UPGRADE's update mode (ota-SMK-37 Pro_016)"],
  ["ota-SMK-37 Elite_720", "FM-1", /update mode of another firmware/, "another port's loader (ota-SMK-37 Elite_720)"],
  ["MicroFreak_100", "Arturia SMK", /not an SMK-37/, "another device on a matching port"]]) {
  const dev = new FakeKbd(img("pro"), { identity, name });
  const ui = UI(), f = served();
  const r = await install(dev.access, rel, ui, { ...GO, fetch: f });
  ok(r === "refused" && re.test(last(ui)) && dev.upgrades === 0 && !f.asked.length, `${what}: refused, nothing fetched or sent`);
}
{
  const ui = UI();
  ok(await install({ inputs: new Map(), outputs: new Map() }, rel, ui, GO) === "refused" && last(ui) === TEXT.none, "no keyboard: said so");
}
// an install cut off: finished from RemoteFM1's loader with its model's package (the MIDI box is not asked)
for (const k of ["elite", "pro"]) {
  const m = rel.models[k], model = m.identity.split("_")[0];
  const dev = new FakeKbd(img(k), { identity: `ota-${model}_900`, final: m.identity });
  const ui = UI(), f = served();
  const r = await install(dev.access, rel, ui, { accepted: true, fetch: f });
  ok(r === "resumed" && dev.served === 7 && dev.bad === 0 && dev.identity === m.identity && f.asked.join() === `firmware/${m.file}` && !ui.confirms,
     `resume: ota-${model}_900 finished with ${m.file}, back as ${m.identity}`);
}
{
  const dev = new FakeKbd(img("pro"), { identity: "ota-SMK-37 Pro_900" });
  const st = await look(dev.access);
  ok(st.loader && /RemoteFM1's update mode \(ota-SMK-37 Pro_900\)/.test(describe(st, rel)), "look: RemoteFM1's update mode, an install cut off");
}
// the MIDI box, a cancel, two devices
{
  const dev = new FakeKbd(img("pro"), { identity: "SMK-37 Pro_016" });
  const ui = UI(), f = served();
  ok(await install(dev.access, rel, ui, { accepted: true, fetch: f }) === "refused" && last(ui) === TEXT.midiClosed && dev.upgrades === 0 && !f.asked.length,
     "a fresh install before the MIDI box is ticked: refused, nothing sent");
  const ui2 = UI(false);
  ok(await install(dev.access, rel, ui2, { ...GO, fetch: served() }) === "cancelled" && ui2.confirms === 1 && dev.upgrades === 0, "cancelled at the question: nothing sent");
  const fm1 = new FakeKbd(img("pro"), { identity: "FM-1_015", name: "FM-1" });
  const ui3 = UI();
  ok(await install(both(dev, fm1), rel, ui3, { ...GO, fetch: served() }) === "refused" && last(ui3).startsWith(TEXT.several) && dev.upgrades === 0 && fm1.upgrades === 0,
     "an SMK-37 and an FM-1 both connected: refused, nothing sent to either");
}
// a write cut off: the error says so (the page then shows how to finish it)
{
  const dev = new FakeKbd(img("pro"), { identity: "SMK-37 Pro_016" });
  const origNext = dev.next.bind(dev);
  dev.next = function () { if (this.identity.startsWith("ota-") && this.served >= 6) { for (const p of [this.input, this.output]) p.state = "disconnected"; return; } origNext(); };
  let e = null;
  try { await install(dev.access, rel, UI(), { ...GO, fetch: served() }); } catch (x) { e = x; }
  ok(e && e.interrupted && /disconnected/.test(e.message), "the cable pulled while writing: an error marked interrupted (the page shows how to finish)");
}

console.log(fails ? `SMK INSTALLER TESTS FAILED (${fails} of ${checks})` : `smk installer tests passed (${checks})`);
process.exit(fails ? 1 : 0);
