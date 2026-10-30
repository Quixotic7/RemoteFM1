// SPDX-License-Identifier: GPL-3.0-only
// The RemoteFM1 web installer's decisions (installer.js) against a simulated FM-1 on a fake Web MIDI, end to end in
// Node (TransitFM1's web/test_installer.mjs, GPL-3.0-only, ported): the released packages checked (releases.json,
// SHA256SUMS, the files and their identities agree; the USB-only choice fetches and checks its own file), installs over the firmwares that answer as V15, over RemoteFM1 itself (told by its
// HELLO) and over ChoralRoot; Bubba Machine and Sloop refused (Sloop only without the override); no fresh install
// before the visitor says the MIDI apps are closed; an install cut off finished from update mode; a cancel that
// sends nothing; an FM-1 on M-VAVE's V16 (FM-1_016) named as such and installed over; V16's package refused (past
// the app area: V15 first, then M-UPGRADE); the MIDI permission prompt said while it is pending (and Try again after
// 30 s), a blocked site told how to re-allow it. Never against a real unit: the only MIDI here is the fake's.
//   node web/installer/test_installer.mjs [remote-X.Y.fwsc] [FM-1.fwsc]   (default: firmware/ of releases.json;
//   M-VAVE's V15 / V16: V15_PKG / V16_PKG, else a MVaveOfficial/ folder beside a parent of this tree, else skipped)
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { createHash } from "node:crypto";
import { pack7, unpack7 } from "./fm1ota.js";
import { install, look, describe, packageFrom, releasedPackage, releaseFile, IDENTITY, rfPack7, rfUnpack7, requestMidi,
         MIDI_PROMPT, MIDI_LATE, MIDI_DENIED } from "./installer.js";
import { validateStockPackage } from "./fm1pkg.js";

const realTimeout = setTimeout;                    // the protocol's waits (seconds) shortened 20 times
globalThis.setTimeout = (f, ms, ...a) => realTimeout(f, ms >= 200 ? ms / 20 : ms, ...a);

let fails = 0, checks = 0;
const ok = (cond, what) => { checks++; if (!cond) fails++; console.log(`${what.padEnd(78)} ${cond ? "ok" : "FAIL"}`); };
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];

// a FM-1: the handshake, INFO (version: the backup protocol's answer, or none), step 1 (four reads and "success"),
// then the loader (six reads and the finish), then the firmware written
let FINAL = IDENTITY;                              // what an install leaves: the package's identity (set below)
class FakeFM1 {
  constructor(image, { identity = "FM-1_015", name = "FM-1", version = null, loader = "ota-FM-1_900", final = FINAL, remote = null } = {}) {
    this.image = image; this.served = 0; this.bad = 0; this.upgrades = 0; this.version = version; this.remote = remote; this.hellos = 0;
    this.loader = loader; this.final = final;
    this.access = { inputs: new Map(), outputs: new Map() };
    this.boot(identity, name);
  }
  boot(identity, name) {
    this.identity = identity; this.waiting = null; this.queue = [];
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
    if (d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x52 && d[3] === 0x46) {     // RemoteFM1's HELLO
      this.hellos++;
      if (d[4] === 0x01 && this.remote && (this.identity === "FM-1_015" || /^FM-1_52/.test(this.identity))) {
        const b = new Array(67).fill(0), put = (o, s) => [...new TextEncoder().encode(s)].forEach((c, i) => { b[o + i] = c; });
        put(0, "RemoteFM1"); b[16] = 2; put(17, this.remote); put(43, this.identity.slice(0, 8));
        this.tx([0xF0, 0x7D, 0x52, 0x46, 0x41, ...rfPack7(b), 0xF7]);
      }
      return;
    }
    if (d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x46 && d[3] === 0x4C) {     // INFO
      if (d[4] === 1 && this.version && !this.identity.startsWith("ota-"))
        this.tx([0xF0, 0x7D, 0x46, 0x4C, 1, ...new TextEncoder().encode(this.version), 0, 0xF7]);
      return;
    }
    if (eq(d, HS)) {
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
      if (addr === 0xE0000000) realTimeout(() => this.boot(this.loader, "Felucca Update"), 20);
      else if (addr === 0xF0000000) realTimeout(() => this.boot(this.final, "FM-1"), 20);
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

const UI = (answer = true) => {
  const u = { said: [], kinds: [], confirms: 0, prog: 0 };
  return Object.assign(u, { say(t, k = "") { u.said.push(t); u.kinds.push(k); }, progress(f) { u.prog = f; },
                            confirm() { u.confirms++; return answer; }, log() {} });
};

// the page: every element transit.js looks up is in index.html (0.1.1's first page lost #ver: no install button)
{
  const page = readFileSync(new URL("./index.html", import.meta.url), "utf8");
  const js = readFileSync(new URL("./installer.js", import.meta.url), "utf8");
  const ids = [...new Set([...js.matchAll(/(?:\$|put)\("([\w-]+)"/g)].map((m) => m[1]))];
  const missing = ids.filter((id) => !new RegExp(`id="${id}"`).test(page));
  ok(ids.length >= 10 && !missing.length, `the page has every element installer.js uses (${ids.length})${missing.length ? ": missing " + missing.join(", ") : ""}`);
  for (const t of [/no sound on\s+its own/, /ChoralRoot users: back up first/, /answers\s+installers as itself/, /Close every app that uses MIDI/, /Chrome or Edge/,
                   /USB only/, /USB \+ Bluetooth MIDI/])
    ok(t.test(page), `the page says ${t.source.replace(/\\s\+/g, " ")}`);
}
{
  const a = [0, 1, 127, 128, 200, 255, 7, 77, 254];
  ok(rfUnpack7(rfPack7(a)).join() === a.join(), "RemoteFM1's 7-in-8 packing round-trips");
}

// the release the page installs: releases.json, firmware/SHA256SUMS and the file agree
const rels = JSON.parse(readFileSync(new URL("./releases.json", import.meta.url), "utf8")).releases;
const relPath = new URL(`./firmware/remote-${rels[0].version}.fwsc`, import.meta.url);
const findOfficial = (name) => {   // M-VAVE's files are not in this tree
  for (let d = dirname(new URL(import.meta.url).pathname); d !== dirname(d); d = dirname(d))
    if (existsSync(join(d, "MVaveOfficial", name))) return join(d, "MVaveOfficial", name);
  return null;
};
const pkgPath = process.argv[2] || relPath.pathname;
const v15Path = process.argv[3] || process.env.V15_PKG || findOfficial("V15-FM-1.fwsc");
const v16Path = process.env.V16_PKG || findOfficial("V16-FM-1.fwsc");
const raw = new Uint8Array(readFileSync(pkgPath));
const sha = createHash("sha256").update(raw).digest("hex");
if (!process.argv[2]) {
  const sums = readFileSync(new URL("./firmware/SHA256SUMS", import.meta.url), "utf8");
  // every package of every release (the default one, and the USB-only one where there is one)
  const pkgs = rels.flatMap((r) => [releaseFile(r), ...(r.usbOnly ? [releaseFile(r, "usb")] : [])].map((f) => ({ r, ...f })));
  for (const p of pkgs) {
    const b = new Uint8Array(readFileSync(new URL(`./firmware/${p.file}`, import.meta.url)));
    const h = createHash("sha256").update(b).digest("hex");
    let prod = "";
    try { prod = (await packageFrom(b)).product; } catch (e) { prod = e.message; }
    ok(h === p.sha256 && sums.includes(`${h}  ${p.file}`) && prod === p.r.identity,
       `${p.file}: releases.json, SHA256SUMS and the file agree; installs as ${prod}`);
  }
  // what Pages publishes (firmware/ as it is): only the listed packages, no earlier one left beside them
  const listed = pkgs.map((p) => p.file).sort().join();
  const files = readdirSync(new URL("./firmware/", import.meta.url)).filter((f) => f.endsWith(".fwsc")).sort().join();
  const summed = sums.trim().split("\n").map((l) => l.split(/\s+/)[1]).sort().join();
  ok(files === listed && summed === listed, `firmware/ holds the listed packages only (${files})`);
  ok(rels[0].version === "0.3.0" && rels[0].identity === "FM-1_52000300" && rels[0].usbOnly && rels[0].license === "MPL-2.0"
     && rels.some((r) => r.version === "0.1.1" && r.identity === "FM-1_015" && r.license === "GPL-3.0-only") && !files.includes("remote-0.1.fwsc"),
     "RemoteFM1 0.3.0 is offered (USB + Bluetooth, and USB only), 0.2.3, 0.2.2, 0.2.1 and 0.1.1 kept as the older release, 0.1 not published");
  // the USB-only choice: its own file, its own SHA-256, the same identity
  const r0 = rels[0], usbRaw = new Uint8Array(readFileSync(new URL(`./firmware/${r0.usbOnly.file}`, import.meta.url)));
  const asked = [];
  const fetchAny = async (url) => { asked.push(url); return { ok: true, status: 200, arrayBuffer: async () => usbRaw.buffer.slice(usbRaw.byteOffset, usbRaw.byteOffset + usbRaw.length) }; };
  const u = await releasedPackage(r0, fetchAny, "usb");
  ok(asked.join() === `firmware/${r0.usbOnly.file}` && u.product === r0.identity && u.label === `RemoteFM1 ${r0.version} (USB only)`,
     "the USB-only choice: its own file fetched, checked, labelled");
  let e = "";
  try { await releasedPackage(r0, fetchAny); } catch (x) { e = x.message; }
  ok(/not the released one/.test(e), "the USB-only file served for the default package: refused (SHA-256)");
  e = "";
  try { await releasedPackage({ ...r0, identity: "FM-1_52000200" }, fetchAny, "usb"); } catch (x) { e = x.message; }
  ok(/installs as FM-1_52000300, not FM-1_52000200/.test(e), "a package whose identity is not the release's: refused");
  const old = rels.find((r) => r.version === "0.1.1");
  ok(releaseFile(old, "usb").file === "remote-0.1.1.fwsc", "a release without a USB-only package: the choice falls back to its one package");
}
const rel = { version: "9.9", sha256: sha };
const fakeFetch = (bytes) => async (url) => ({ ok: url === "firmware/remote-9.9.fwsc", status: 404, arrayBuffer: async () => bytes.buffer });

// the package
const pkg = await releasedPackage(rel, fakeFetch(raw));
FINAL = pkg.product;
ok((pkg.product === IDENTITY || /^FM-1_52\d{6}$/.test(pkg.product)) && !pkg.official && pkg.label === "RemoteFM1 9.9", `the released package: identity ${pkg.product}, labelled`);
let err = "";
try { const bad = raw.slice(); bad[5000] ^= 1; await releasedPackage(rel, fakeFetch(bad)); } catch (e) { err = e.message; }
ok(/not the released one/.test(err), "a package whose SHA-256 differs is refused before anything");
err = "";
try { await packageFrom(new Uint8Array(4000)); } catch (e) { err = e.message; }
ok(/not an FM-1|not for the FM-1/.test(err), "a file that is not an FM-1 package is refused");
if (v15Path) {
  const v15 = await packageFrom(new Uint8Array(readFileSync(v15Path)));
  ok(v15.official && v15.product === "FM-1_015" && /official V15/.test(v15.label), "M-VAVE's FM-1.fwsc: recognised as the official V15");
}

// what is on the FM-1
const st = async (o) => describe(await look(new FakeFM1(pkg.image, o).access));
ok(/official M-VAVE firmware V15 \(FM-1_015\).*TransitFM1/.test(await st({})), "look: FM-1_015 with no HELLO answer is stock V15 or TransitFM1");
{
  const s16 = await st({ identity: "FM-1_016" });
  ok(/official M-VAVE firmware V16 \(FM-1_016\)/.test(s16) && !/V15|TransitFM1|RemoteFM1/.test(s16), "look: FM-1_016 is the official V16 (no TransitFM1 / RemoteFM1 there)");
  ok(/firmware V14 \(FM-1_014\)\.$/.test(await st({ identity: "FM-1_014" })), "look: FM-1_014 is the official V14");
}
ok(/runs RemoteFM1 0\.1\.1 /.test(await st({ remote: "0.1.1" })), "look: RemoteFM1 told by its HELLO, with its version");
{                                                 // RemoteFM1 0.2 and later: its own identity (fm1-base's FM-1_52)
  const d = await st({ identity: "FM-1_52000200", remote: "0.2.0" });
  ok(d === "The FM-1 runs RemoteFM1 0.2.0 (FM-1_52000200)." && !/official|unknown/.test(d), "look: RemoteFM1 0.2 by its own identity and HELLO, never \"official\"");
  const e = await st({ identity: "FM-1_52000200" });
  ok(e === "The FM-1 runs RemoteFM1 0.2.0 (FM-1_52000200).", "look: RemoteFM1 0.2 by its identity alone (no HELLO answer)");
  ok(/development build/.test(await st({ identity: "FM-1_5200" })), "look: a RemoteFM1 development build (FM-1_5200)");
}
ok(/ChoralRoot 0\.25\.1\./.test(await st({ identity: "FM-1_920", name: "ChoralRoot FM-1", version: "ChoralRoot 0.25.1" })),
   "look: ChoralRoot by its INFO");
ok(/Bubba Machine/.test(await st({ identity: "FM-1_003", name: "FM-1 Bubba Machine" })), "look: Bubba Machine by its port");
ok(/update mode/.test(await st({ identity: "ota-FM-1_900", name: "Felucca Update" })), "look: update mode");
ok(/No FM-1 found/.test(describe(await look({ inputs: new Map(), outputs: new Map() }))), "look: nothing connected");

// installs
const run = async (o, opts = { midiClosed: true }, answer = true) => {
  const dev = new FakeFM1(pkg.image, o), ui = UI(answer);
  let r, e = null;
  try { r = await install(dev.access, pkg, ui, opts); } catch (x) { e = x; }
  return { dev, ui, r, e };
};
let t = await run({});
ok(t.r === "done" && t.dev.upgrades === 2 && t.dev.bad === 0 && t.dev.identity === FINAL && t.ui.confirms === 1 &&
   t.ui.prog === 1 && t.ui.kinds.at(-1) === "ok", "over the stock V15 (or TransitFM1): installed, both steps served right");
t = await run({ identity: "FM-1_920", name: "ChoralRoot FM-1", version: "ChoralRoot 0.25.1" });
ok(t.r === "done" && t.dev.bad === 0 && t.dev.identity === FINAL, "over ChoralRoot: installed");
{
  const dev = new FakeFM1(pkg.image, { identity: "FM-1_016" }), ui = UI();
  let asked = "";
  ui.confirm = (q) => { asked = q; ui.confirms++; return true; };
  const r = await install(dev.access, pkg, ui, { midiClosed: true });
  ok(r === "done" && dev.bad === 0 && dev.identity === FINAL && dev.hellos === 0 && /over the official V16\?/.test(asked),
     "over M-VAVE's V16 (FM-1_016): installed, asked as \"over the official V16\", no HELLO");
}
t = await run({ identity: "FM-1_910", name: "Felucca", version: "FELUCCA v1.0.1" });
ok(t.r === "done" && t.dev.bad === 0, "over Felucca 1.0.1: installed");
t = await run({ remote: "0.1.1" });
ok(t.r === "done" && t.dev.bad === 0 && t.dev.identity === FINAL && t.dev.hellos >= 1, "over RemoteFM1 (a reinstall): installed");
t = await run({ identity: "FM-1_52000200", remote: "0.2.0" });
ok(t.r === "done" && t.dev.bad === 0 && t.dev.identity === FINAL && t.dev.hellos >= 1, "over RemoteFM1 0.2 (FM-1_52000200): installed");
t = await run({}, {});
ok(t.r === "refused" && t.dev.upgrades === 0 && t.ui.confirms === 0 && /close every app that uses MIDI/i.test(t.ui.said.at(-1)),
   "before the MIDI apps are said to be closed: refused, nothing sent");
t = await run({ identity: "FM-1_920", name: "ChoralRoot FM-1", version: "ChoralRoot 0.25.1" });
ok(t.dev.hellos === 0, "HELLO is sent only to an FM-1 that answers as V15 (ChoralRoot got none)");
t = await run({ identity: "FM-1_003", name: "FM-1 Bubba Machine" }, { override: true, midiClosed: true });
ok(t.r === "refused" && t.dev.upgrades === 0 && t.ui.confirms === 0 && /Bubba/.test(t.ui.said.at(-1)),
   "over Bubba Machine: refused, even with the override; nothing sent");
t = await run({ identity: "FM-1_900", name: "Felucca", version: "FELUCCA SLOOP 2.2" });
ok(t.r === "refused" && t.dev.upgrades === 0 && /Sloop/.test(t.ui.said.at(-1)), "over Sloop: refused, nothing sent");
t = await run({ identity: "FM-1_900", name: "Felucca", version: "FELUCCA SLOOP 2.2" }, { override: true, midiClosed: true });
ok(t.r === "done" && t.dev.bad === 0, "over Sloop with the override: installed");
t = await run({}, { midiClosed: true }, false);
ok(t.r === "cancelled" && t.dev.upgrades === 0, "the confirmation declined: nothing sent");
t = await run({ identity: "ota-FM-1_900", name: "Felucca Update" });
ok(t.r === "resumed" && t.dev.upgrades === 1 && t.dev.bad === 0 && t.dev.identity === FINAL && t.ui.confirms === 0,
   "in update mode (an install cut off): the write finished, no question");
t = await run({ identity: "ota-FM-1_015", name: "ota-FM-1" });
ok(t.e && /another firmware/.test(t.e.message) && t.dev.upgrades === 0,
   "in the official loader's update mode: not written (finish it with M-VAVE's own package)");

if (v15Path) {   // TransitFM1's loader writing M-VAVE's V15 answers ota-FM-1_015: cut off, it is finished as the return to stock
  const v15 = await packageFrom(new Uint8Array(readFileSync(v15Path)));
  const dev = new FakeFM1(v15.image, { identity: "ota-FM-1_015", name: "ota-FM-1", final: "FM-1_015" }), ui = UI();
  const r = await install(dev.access, v15, ui, { midiClosed: true });
  ok(r === "resumed" && dev.bad === 0 && dev.identity === "FM-1_015", "in update mode as ota-FM-1_015 with M-VAVE's V15 chosen: the write finished");
}
t = await run({ identity: "FM-1_101", name: "FM-1+VA" });
ok(t.r === "refused" && t.dev.upgrades === 0, "over Baud Girl (FM-1_101, a version the table does not know): refused, nothing sent");

if (v16Path) {   // M-VAVE's V16: flash.bin 692224 B, to 0xA9000, past the app area (0x93000) RemoteFM1's loader writes
  const b16 = new Uint8Array(readFileSync(v16Path));
  let e = "";
  try { await packageFrom(b16); } catch (x) { e = x.message; }
  ok(/official V16/.test(e) && /official V15 first/.test(e) && /M-UPGRADE/.test(e), "M-VAVE's V16 package: refused before the FM-1 is asked: V15 first, then M-UPGRADE");
  e = "";
  try { await validateStockPackage(b16); } catch (x) { e = x.message; }
  ok(/official V15/.test(e) && /V16 afterwards, use M-VAVE's M-UPGRADE from V15/.test(e), "the return to stock takes only V15, and says how to go on to V16");
} else console.log("(M-VAVE's V16 not found: set V16_PKG; its checks skipped)");
if (v15Path) ok((await validateStockPackage(new Uint8Array(readFileSync(v15Path)))).product === "FM-1_015", "the return to stock: M-VAVE's V15 accepted");

// the MIDI permission: a fake navigator whose prompt the test answers
const fakeNav = (perm, { reject = null } = {}) => {
  const n = { asks: 0, pending: [], permissions: perm === null ? undefined : { query: async (d) => ({ state: d.name === "midi" && d.sysex ? perm : "granted" }) } };
  n.requestMIDIAccess = (o) => { n.asks++; n.sysex = o && o.sysex; return reject ? Promise.reject(reject) : perm === "granted" ? Promise.resolve({ ok: 1 }) : new Promise((res) => n.pending.push(res)); };
  return n;
};
const midiUi = () => { const u = { said: [], kinds: [], late: null }; return Object.assign(u, { say(t, k = "") { u.said.push(t); u.kinds.push(k); }, late: (r) => { u.retry = r; } }); };
const wait = (ms) => new Promise((r) => realTimeout(r, ms));
{
  const nav = fakeNav("prompt"), u = midiUi();
  const p = requestMidi(nav, u);
  await wait(10);
  ok(u.said[0] === MIDI_PROMPT && u.kinds[0] === "wait" && nav.asks === 1 && nav.sysex === true, "MIDI prompt pending: \"Allow MIDI: your browser is asking\" at once");
  await wait(1700);                                  // 30 s, shortened
  ok(u.said.at(-1) === MIDI_LATE && /Try again/.test(MIDI_LATE) && typeof u.retry === "function", "no answer after 30 s: the hint stays, with Try again");
  u.retry();
  ok(nav.asks === 2, "Try again asks the browser again");
  nav.pending.at(-1)(new FakeFM1(pkg.image, { identity: "FM-1_016" }).access);
  const a = await p;
  ok(u.said.at(-1) === "MIDI is allowed." && /official M-VAVE firmware V16/.test(describe(await look(a))),
     "allowed: the hint replaced, and the FM-1 found (V16)");
}
{
  const nav = fakeNav(null), u = midiUi();               // no Permissions API for MIDI: the hint after 1 s
  const p = requestMidi(nav, u);
  await wait(10);
  const before = u.said.length;
  await wait(120);
  ok(before === 0 && u.said[0] === MIDI_PROMPT, "no permission state known: the hint once the request has waited 1 s");
  nav.pending[0]({ ok: 1 });
  await p;
}
{
  const nav = fakeNav("granted"), u = midiUi();
  const a = await requestMidi(nav, u);
  await wait(120);
  ok(a.ok === 1 && u.said.length === 0, "MIDI already allowed: no hint");
}
{
  const nav = fakeNav("denied"), u = midiUi();
  let e = null;
  try { await requestMidi(nav, u); } catch (x) { e = x; }
  ok(e && e.message === MIDI_DENIED && /site-settings icon/.test(e.message) && /Allow/.test(e.message) && nav.asks === 0,
     "MIDI blocked for the site: how to allow it again (site settings), not the generic error");
}
{
  const nav = fakeNav("prompt", { reject: new DOMException("denied", "SecurityError") }), u = midiUi();
  let e = null;
  try { await requestMidi(nav, u); } catch (x) { e = x; }
  ok(e && e.name === "SecurityError", "MIDI refused at the prompt: the browser's SecurityError (the page's refusal text)");
}
{
  let e = "";
  try { await requestMidi({}, midiUi()); } catch (x) { e = x.message; }
  ok(/no Web MIDI/.test(e), "no Web MIDI: Chrome or Edge");
}

if (process.env.LONG_PKG) {   // a flash.bin past the app area (as Baud Girl's): refused before the FM-1 is asked anything
  let e = "";
  try { await packageFrom(new Uint8Array(readFileSync(process.env.LONG_PKG))); } catch (x) { e = x.message; }
  ok(/larger than RemoteFM1 can install/.test(e) && /official V15 first/.test(e), "a package past the app area: refused, go through V15");
}

console.log(fails ? `installer: ${fails} of ${checks} FAILED` : `installer tests passed (${checks})`);
process.exit(fails ? 1 : 0);
