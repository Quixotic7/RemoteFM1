// SPDX-License-Identifier: GPL-3.0-only
// The recorder end to end in headless Chromium (a real AudioContext and AudioWorklet, muted; no window, a fresh
// profile, the browser and the server ended by their PIDs). For the native engine and then the emulated one (Felucca):
// start the player, Shift+R, hold a key through the computer-key map, Shift+R again; read the take's WAV back (header,
// length against the worklet's recorded span, sound while the key is held, Chromium's own decoder); with the page's
// volume at 0 a second take has the same level; the worklet's load with recording off and on; a take ended by an
// engine switch. Screenshots (desktop 1280 x 800, phone 390 x 844): idle, recording, the Takes popover.
//   node web/site/tools/rec-e2e.mjs [--shots=DIR] [--secs=6] [--rounds=2] [--engines=native,emu]
//   node web/site/tools/rec-e2e.mjs --soak [--soak=fw:engine,...] [--soak-secs=60] [--throttle=4]
// --soak (the owner's "it kept reloading the engine"): per firmware, a take of soak-secs while keys are played, then the
// same with the page's CPU throttled; exactly one take per start / stop, no engine load, halt or worklet error in a take.
// (the load: --rounds pairs of --secs with recording off, then on, a three-key chord held)
// Needs web/build/native/felucca.wasm, web/build/emu/fm1_web.wasm and web/build/packages/felucca-1.6.1.fwsc (the
// Hub's package; the Hub is mocked in the page, so nothing leaves the machine). Writes web/build/test/rec-*.png and
// rec-e2e.json.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join } from "node:path";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const WEB = join(ROOT, "web");
const OUT = join(WEB, "build", "test");
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS2 = arg("shots", "");
const SECS = +arg("secs", 6), ROUNDS = +arg("rounds", 2), ENGINES = arg("engines", "native,emu").split(",");
// --soak: long takes played like a person, on real engines, plain and with the page's CPU throttled (x THROTTLE)
const SOAK = arg("soak", process.argv.includes("--soak") ? "felucca:native,choralroot:native,choralroot:emu,melodee:emu,x0x:emu" : "");
const SOAK_S = +arg("soak-secs", 60), THROTTLE = +arg("throttle", 4), NOREC = process.argv.includes("--norec");
const EDITOR = process.argv.includes("--editor");   // (--editor: the firmware's hosted manager open in the Editor panel throughout)   // (--norec: the same play, no take: a baseline)
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
for (const f of ["build/native/felucca.wasm", "build/emu/fm1_web.wasm", "build/packages/felucca-1.6.1.fwsc"])
  if (!existsSync(join(WEB, f))) { console.error(`missing web/${f}`); process.exit(2); }
mkdirSync(OUT, { recursive: true });
if (SHOTS2) mkdirSync(SHOTS2, { recursive: true });

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(96)} ${c ? "ok" : "FAIL"}`); return c; };

// the Hub, mocked in the page: its catalog is the snapshot (Melodee at 1.4.4, the live Hub's); the packages in
// web/build/packages are served as the Hub's (their SHA-256 must match the catalog's); anything else a 404
const hubCat = JSON.parse(readFileSync(join(WEB, "site", "data", "hub-catalog.snapshot.json"), "utf8"));
const LOCAL_PKGS = ["felucca-1.6.1", "melodee-1.4.4", "x0x-1.1.0", "choralroot-0.25.1"].filter((p) => existsSync(join(WEB, "build", "packages", p + ".fwsc")));
{
  const m = hubCat.firmwares.find((f) => f.id === "melodee");
  if (m && LOCAL_PKGS.includes("melodee-1.4.4"))
    Object.assign(m, { version: "1.4.4", pkg: "fw/melodee-1.4.4.fwsc", sha256: "d349a44dc5c29c2af2ad14b6fc76071fc9e218ad4a83dcd25948bd8479aa3607", tag: "v1.4.4",
      release: "https://github.com/keremimo/melodee/releases/tag/v1.4.4", source: "https://github.com/keremimo/melodee/tree/v1.4.4" });
}
const HUB_MOCK = `(() => {
  const HUB = ${JSON.stringify(hubCat)}, PKGS = ${JSON.stringify(LOCAL_PKGS)}, real = window.fetch.bind(window);
  window.fetch = async (input, init) => {
    const u = String(input && input.url || input);
    if (!u.startsWith("https://thefirmwarehub.com/")) return real(input, init);
    if (u === "https://thefirmwarehub.com/catalog.json") return new Response(JSON.stringify(HUB), { status: 200, headers: { "content-type": "application/json" } });
    const p = PKGS.find((x) => u === "https://thefirmwarehub.com/fw/" + x + ".fwsc");
    if (p) return real("/web/build/packages/" + p + ".fwsc");
    return new Response("not found", { status: 404 });
  };
})();`;

const profile = mkdtempSync(join(WEB, "build", "chromium-profile-rec-"));
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,800",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
const stop = () => { try { chrome.kill(); } catch {} try { server.kill(); } catch {} };
process.on("exit", stop);
process.on("SIGINT", () => process.exit(130));
process.on("SIGTERM", () => process.exit(143));

const report = { runs: [] };
let ws, id = 0;
const waiting = new Map();
const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
async function js(expression, timeout = 60000) {
  const r = await cdp("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true, timeout });
  if (r.result?.exceptionDetails) throw new Error(`page: ${JSON.stringify(r.result.exceptionDetails).slice(0, 500)}`);
  return r.result?.result?.value;
}
async function until(expr, ms = 30000, what = expr) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await js(`!!(${expr})`)) return true; await sleep(100); }
  throw new Error(`timed out: ${what}`);
}
// trusted key events through the DevTools protocol (the page's own Keyboard handler: panel.js KEYMAP)
const KEYS = { KeyA: ["a", 65], KeyS: ["s", 83], KeyD: ["d", 68], KeyF: ["f", 70], KeyG: ["g", 71], KeyH: ["h", 72], KeyJ: ["j", 74], KeyK: ["k", 75],
  KeyR: ["r", 82], ShiftLeft: ["Shift", 16] };
async function key(code, type, shift = false) {
  const [k, vk] = KEYS[code];
  await cdp("Input.dispatchKeyEvent", { type, code, key: shift && k.length === 1 ? k.toUpperCase() : k, windowsVirtualKeyCode: vk, modifiers: shift ? 8 : 0 });
}
const shiftR = async () => { await key("ShiftLeft", "keyDown", true); await key("KeyR", "keyDown", true); await key("KeyR", "keyUp", true); await key("ShiftLeft", "keyUp"); };
async function shot(name) {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const buf = Buffer.from(r.result.data, "base64");
  writeFileSync(join(OUT, `rec-${name}.png`), buf);
  if (SHOTS2) writeFileSync(join(SHOTS2, `rec-${name}.png`), buf);
  console.log(`  shot rec-${name}.png`);
}
async function viewport(w, h, mobile) {
  await cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile });
  await sleep(400);
}

// the newest take's WAV, read back in the page: header, length, levels, Chromium's decoder
const READ_TAKE = (keyFrom, keyTo) => `(async () => {
  const R = await import("/web/site/js/recorder.js");
  const t = FM1Rec.takes[0], buf = new Uint8Array(await (await fetch(t.url)).arrayBuffer());
  const p = R.parseWav(buf), L = p.channel(0), Rt = p.channel(1);
  const rms = (a, b) => { a = Math.max(0, Math.floor(a * p.sampleRate)); b = Math.min(p.frames, Math.floor(b * p.sampleRate)); let s = 0; for (let i = a; i < b; i++) s += L[i] * L[i] + Rt[i] * Rt[i]; return b > a ? Math.sqrt(s / (2 * (b - a))) : 0; };
  let peak = 0; for (let i = 0; i < p.frames; i++) peak = Math.max(peak, Math.abs(L[i]), Math.abs(Rt[i]));
  const ctx = FM1Rec.o.getPlayer().ctx;
  let decoded = null; try { const ab = await ctx.decodeAudioData(buf.slice().buffer); decoded = { frames: ab.length, channels: ab.numberOfChannels, rate: ab.sampleRate }; } catch (e) { decoded = { error: String(e) }; }
  return { name: t.name, size: t.size, blobType: t.blob.type, frames: t.frames, startFrame: t.startFrame, endFrame: t.endFrame, ctxSpan: t.ctxSpan, clips: t.clips, bits: t.bits,
    ctxRate: ctx.sampleRate, hdr: { tags: String.fromCharCode(...buf.subarray(0, 4)) + String.fromCharCode(...buf.subarray(8, 16)) + String.fromCharCode(...buf.subarray(36, 40)),
    format: p.format, channels: p.channels, rate: p.sampleRate, bits: p.bits, byteRate: p.byteRate, blockAlign: p.blockAlign, riffSize: p.riffSize, dataBytes: p.dataBytes, fileBytes: p.fileBytes, frames: p.frames },
    rmsKey: rms(${keyFrom}, ${keyTo}), rmsAll: rms(0, p.frames / p.sampleRate), peak, decoded };
})()`;

// the load reports (worklet/host.js measure: render time incl. the capture, over audio time), tagged recording or not
const HOOK_LOADS = `(() => { window.__loads = []; const p = FM1Rec.o.getPlayer(), f = p.ev.load;
  p.ev.load = (m) => { window.__loads.push({ t: performance.now(), rec: FM1Rec.phase === "rec", load: m.load, p99Ms: m.p99Ms, maxMs: m.maxMs, lateMs: m.lateMs, peak: m.peak }); if (f) f(m); }; return true; })()`;

async function takeOnce(label, { holdS = 2, leadS = 0.5, tailS = 0.5, shotAt = null } = {}) {
  const n0 = await js("FM1Rec.takes.length");
  await shiftR();
  await until(`FM1Rec.phase === "rec" && FM1Rec.cur && FM1Rec.cur.startFrame !== undefined`, 5000, "recording started");
  await sleep(leadS * 1000);
  await key("KeyA", "keyDown");
  if (shotAt) { await sleep(shotAt * 1000); await shot(label); await sleep((holdS - shotAt) * 1000); } else await sleep(holdS * 1000);
  await key("KeyA", "keyUp");
  await sleep(tailS * 1000);
  await shiftR();
  await until(`FM1Rec.phase === "idle" && FM1Rec.takes.length === ${n0 + 1}`, 5000, "the take is in Takes");
  // the key is held from about leadS to leadS + holdS into the take: its middle second
  return js(READ_TAKE(leadS + 0.3, leadS + holdS - 0.2));
}
function checkTake(eng, t, holdS) {
  const sr = t.ctxRate, span = t.endFrame - t.startFrame, want = holdS + 1.0;
  ok(t.hdr.tags === "RIFFWAVEfmt data" && t.hdr.format === 1 && t.hdr.channels === 2 && t.hdr.bits === 24 && t.hdr.rate === sr,
     `${eng}: WAV header RIFF/WAVE/fmt/data, PCM, stereo, 24-bit, ${sr} Hz (the context's rate)`);
  ok(t.hdr.blockAlign === 6 && t.hdr.byteRate === sr * 6 && t.hdr.dataBytes === t.hdr.frames * 6 && t.hdr.riffSize === t.hdr.fileBytes - 8 && t.size === t.hdr.fileBytes,
     `${eng}: sizes: block 6, byte rate, data = frames x 6, RIFF = file - 8, Blob = file (${(t.size / 1e6).toFixed(2)} MB)`);
  ok(t.blobType === "audio/wav" && /^felucca-1\.6\.1-\d{8}-\d{6}$/.test(t.name), `${eng}: named ${t.name}.wav`);
  ok(Math.abs(t.hdr.frames - span) <= 128, `${eng}: ${t.hdr.frames} frames = the worklet's recorded span ${span} (+-1 quantum)`);
  ok(Math.abs(t.hdr.frames / sr - t.ctxSpan) < 0.1 && Math.abs(t.hdr.frames / sr - want) < 0.35,
     `${eng}: ${(t.hdr.frames / sr).toFixed(3)} s; the page's clock between the presses ${t.ctxSpan.toFixed(3)} s`);
  ok(t.rmsKey > 0.003 && t.peak > 0.01, `${eng}: sound while the key is held (RMS ${(20 * Math.log10(t.rmsKey)).toFixed(1)} dBFS, peak ${t.peak.toFixed(3)})`);
  ok(t.decoded && t.decoded.frames === t.hdr.frames && t.decoded.channels === 2, `${eng}: Chromium's decodeAudioData reads it (${t.decoded && (t.decoded.frames ?? t.decoded.error)} frames)`);
}
// the soak's watch on the page: every engine load, halt, worklet error, load report and toast, with the recorder's phase
const SOAK_HOOK = `(() => { const p = FM1Rec.o.getPlayer(), E = window.__soak = { loads: [], errors: [], halts: [], reports: [], toasts: [] };
  const L = p.load.bind(p); p.load = (...a) => { E.loads.push({ t: performance.now(), phase: FM1Rec.phase, engine: a[1], extra: JSON.stringify(a[3] || {}),
    stack: String(new Error().stack).split(String.fromCharCode(10)).slice(2, 7).map((x) => x.trim().replace(new RegExp("https?:[^ )]*/js/"), "")).join(" < ") }); return L(...a); };
  E.toEngine = 0; E.sysex = []; const M = p.midi.bind(p);
  p.midi = (b) => { E.toEngine++; if (b[0] === 0xF0 && E.sysex.length < 40) E.sysex.push(Array.from(b.slice(0, 8), (x) => x.toString(16)).join(" ")); return M(b); };
  const fe = p.ev.error; p.ev.error = (m) => { E.errors.push({ t: performance.now(), phase: FM1Rec.phase, m: String(m) }); if (fe) fe(m); };
  const fh = p.ev.halted; p.ev.halted = (c) => { E.halts.push({ t: performance.now(), phase: FM1Rec.phase, c }); if (fh) fh(c); };
  const fl = p.ev.load; p.ev.load = (m) => { E.reports.push({ t: performance.now(), rec: FM1Rec.phase === "rec", load: m.load, peak: m.peak, lateMs: m.lateMs, glitches: m.glitches }); if (fl) fl(m); };
  const tt = document.getElementById("toast");
  new MutationObserver(() => { if (tt.classList.contains("show") && E.toasts.at(-1)?.text !== tt.textContent) E.toasts.push({ t: performance.now(), text: tt.textContent }); })
    .observe(tt, { attributes: true, childList: true, characterData: true, subtree: true });
  return true; })()`;
async function soakOne(fw, eng, throttle) {
  const tag = `${fw} ${eng}${throttle > 1 ? ` (CPU x${throttle})` : ""}${EDITOR ? " +editor" : ""}`;
  await cdp("Emulation.setCPUThrottlingRate", { rate: 1 });
  // each case from the default clock (Auto's step-downs are kept per firmware in the visitor's prefs)
  await js(`(() => { try { localStorage.removeItem("remotefm1-player"); } catch (_) {} return true; })()`);
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=${fw}&engine=${eng}` });
  try { await until(`window.FM1Rec && window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "${eng}" && FM1Site.current().entry.id === "${fw}"`, 90000, `${tag} running`); }
  catch (e) {
    console.log("  page:", JSON.stringify(await js(`({ url: location.href, cur: window.FM1Site ? (({ entry, engine, info }) => ({ id: entry && entry.id, engine, info: !!info }))(FM1Site.current()) : null,
      toast: document.getElementById("toast").textContent, status: document.getElementById("status").textContent })`)));
    throw e;
  }
  await js(SOAK_HOOK);
  if (EDITOR) {
    await until(`!document.getElementById("ed-btn").hidden`, 10000, "the Editor button");
    await js(`(document.getElementById("ed-btn").click(), true)`);
    await sleep(5000);   // (the manager loads and talks to the firmware)
    console.log(`  ${tag}: editor open: ${await js(`!document.getElementById("ed-panel").hidden`)}, frame ${await js(`!!document.querySelector("#ed-body iframe")`)}`);
  }
  await sleep(eng === "emu" ? 6000 : 1500);   // (the boot, the JIT, the clock governor's 4 s grace)
  if (throttle > 1) await cdp("Emulation.setCPUThrottlingRate", { rate: throttle });
  const n0 = await js("FM1Rec.takes.length");
  if (!NOREC) { await shiftR(); await until(`FM1Rec.phase === "rec"`, 10000, `${tag}: recording`); }
  const t0 = await js("performance.now()");
  // played like a person: a note every 300 ms, a chord now and then, the octave buttons once
  const NOTES = ["KeyA", "KeyS", "KeyD", "KeyF", "KeyG", "KeyH", "KeyJ", "KeyK"];
  const end = Date.now() + SOAK_S * 1000;
  for (let i = 0; Date.now() < end; i++) {
    const chord = i % 9 === 0 ? [NOTES[i % 8], NOTES[(i + 2) % 8], NOTES[(i + 4) % 8]] : [NOTES[(i * 3) % 8]];
    for (const k of chord) await key(k, "keyDown");
    await sleep(i % 9 === 0 ? 700 : 220);
    for (const k of chord) await key(k, "keyUp");
    await sleep(80);
  }
  const t1 = await js("performance.now()");
  if (NOREC) {
    const E = await js("window.__soak");
    console.log(`  ${tag} (no take): loads ${JSON.stringify(E.loads)}; toasts ${JSON.stringify(E.toasts.map((x) => x.text))}`);
    console.log(`  ${tag} (no take): reports ${JSON.stringify(E.reports.map((r) => [Math.round(r.t), +r.load.toFixed(2), +(r.peak ?? 0).toFixed(2), Math.round(r.lateMs), r.glitches]))}`);
    report.runs.push({ soak: tag, norec: true, loads: E.loads, toasts: E.toasts });
    ok(!E.loads.length, `${tag} (no take): no engine load (${E.loads.length})`);
    return;
  }
  const before = await js("FM1Rec.phase");   // ("rec", unless something ended the take meanwhile)
  await shiftR();
  await sleep(1500);
  let restarted = false;
  if (await js(`FM1Rec.phase === "rec"`)) { restarted = true; await shiftR(); }   // (that Stop started a new take: end it)
  await until(`FM1Rec.phase === "idle" && FM1Rec.takes.length >= ${n0 + 1}`, 10000, `${tag}: the take`);
  ok(before === "rec" && !restarted, `${tag}: still recording when Stop is pressed (${before}), and Stop stops`);
  await sleep(3000);   // (a reload must not start another take by itself)
  await cdp("Emulation.setCPUThrottlingRate", { rate: 1 });
  const E = await js("window.__soak"), n1 = await js("FM1Rec.takes.length"), ph = await js("FM1Rec.phase");
  const t = await js(READ_TAKE(0, 1));
  const inTake = (a) => a.filter((x) => x.t >= t0 - 50 && x.t <= t1);
  const rep = E.reports.filter((r) => r.rec), maxLoad = Math.max(0, ...rep.map((r) => r.load)), late = rep.reduce((s, r) => s + r.lateMs, 0);
  console.log(`  ${tag}: loads ${JSON.stringify(E.loads)} halts ${JSON.stringify(E.halts)} errors ${JSON.stringify(E.errors)}`);
  console.log(`  ${tag}: to the engine ${E.toEngine} messages, SysEx ${JSON.stringify(E.sysex.slice(0, 12))}`);
  console.log(`  ${tag}: load while recording max ${(100 * maxLoad).toFixed(0)} %, late ${late.toFixed(0)} ms; toasts ${JSON.stringify(E.toasts.map((x) => x.text))}`);
  ok(n1 === n0 + 1 && ph === "idle", `${tag}: one start / stop, exactly one take (${n1 - n0}), none started by itself`);
  ok(!inTake(E.loads).length, `${tag}: no engine load during the take (${inTake(E.loads).length})`);
  ok(!E.halts.length && !E.errors.length, `${tag}: no halt, no worklet error (${E.halts.length}, ${E.errors.length})`);
  ok(Math.abs(t.hdr.frames / t.ctxRate - (t1 - t0) / 1000) < 1.5 && Math.abs(t.hdr.frames - (t.endFrame - t.startFrame)) <= 128 && t.decoded.frames === t.hdr.frames,
     `${tag}: the take is the whole ${(t.hdr.frames / t.ctxRate).toFixed(1)} s, a valid WAV`);
  // Auto's step-down, when this computer couldn't keep up: it waits for the take, then applies (one engine load)
  const deferred = E.toasts.some((x) => /after this take/.test(x.text));
  if (deferred) ok(E.loads.some((l) => l.t > t1 && /"mhz"/.test(l.extra)), `${tag}: Auto's step-down waited for the take's end, then applied`);
  else console.log(`  ${tag}: Auto did not step down during the take`);
  report.runs.push({ soak: tag, deferred, takes: n1 - n0, loads: E.loads, halts: E.halts, errors: E.errors, toasts: E.toasts, maxLoad, lateMs: late, seconds: t.hdr.frames / t.ctxRate });
}

async function loadCompare(eng) {
  await js(HOOK_LOADS);
  for (const k of ["KeyA", "KeyD", "KeyG"]) await key(k, "keyDown");
  const phases = [];
  for (let i = 0; i < 2 * ROUNDS; i++) {
    const on = i % 2 === 1;
    if (on) { await shiftR(); await until(`FM1Rec.phase === "rec"`, 5000); }
    const t0 = await js("performance.now()");
    await sleep(SECS * 1000);
    const t1 = await js("performance.now()");
    if (on) { await shiftR(); await until(`FM1Rec.phase === "idle"`, 5000); }
    phases.push({ on, t0, t1 });
  }
  for (const k of ["KeyA", "KeyD", "KeyG"]) await key(k, "keyUp");
  const loads = await js("window.__loads");
  const pick = (on) => loads.filter((l) => phases.some((p) => p.on === on && l.t > p.t0 + 600 && l.t <= p.t1));
  const stat = (a) => {
    const m = (k) => a.reduce((s, x) => s + x[k], 0) / Math.max(1, a.length);
    return { n: a.length, load: +m("load").toFixed(4), p99Ms: +m("p99Ms").toFixed(3), maxMs: +Math.max(...a.map((x) => x.maxMs)).toFixed(3), lateMs: +a.reduce((s, x) => s + x.lateMs, 0).toFixed(1) };
  };
  const off = stat(pick(false)), on = stat(pick(true));
  // each pair's own difference (the chord's sound changes as it is held: a pair is the fairest comparison)
  const pairs = phases.filter((p) => !p.on).map((p, i) => { const q = phases[2 * i + 1], mean = (a) => a.reduce((s, x) => s + x.load, 0) / Math.max(1, a.length);
    return +(100 * (mean(loads.filter((l) => l.t > q.t0 + 600 && l.t <= q.t1)) - mean(loads.filter((l) => l.t > p.t0 + 600 && l.t <= p.t1)))).toFixed(2); });
  console.log(`  ${eng} load, on - off per pair (points): ${pairs.join(", ")}`);
  console.log(`  ${eng} load, recording off: ${JSON.stringify(off)}`);
  console.log(`  ${eng} load, recording on:  ${JSON.stringify(on)}`);
  ok(off.n >= 6 && on.n >= 6, `${eng}: load reports with recording off (${off.n}) and on (${on.n})`);
  ok(on.load - off.load < 0.02 && on.lateMs <= off.lateMs + 20, `${eng}: recording adds no measurable load (${(100 * off.load).toFixed(2)} % -> ${(100 * on.load).toFixed(2)} %), no late audio`);
  return { off, on };
}

try {
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  const page = targets.find((t) => t.type === "page");
  ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const errors = [], dialogs = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    // the page asks before it closes with unsaved takes: noted, and let go
    if (d.method === "Page.javascriptDialogOpening") { dialogs.push(d.params.type); ws.send(JSON.stringify({ id: ++id, method: "Page.handleJavaScriptDialog", params: { accept: true } })); }
    if (d.method === "Runtime.exceptionThrown") { errors.push(JSON.stringify(d.params.exceptionDetails).slice(0, 400)); console.error("exception:", errors.at(-1)); }
  };
  await cdp("Runtime.enable"); await cdp("Page.enable");
  await cdp("Page.addScriptToEvaluateOnNewDocument", { source: HUB_MOCK });

  if (SOAK) {
    for (const c of SOAK.split(",")) {
      const [fw, eng] = c.split(":");
      for (const th of [1, THROTTLE]) await soakOne(fw, eng, th);
    }
  }
  for (const eng of SOAK ? [] : ENGINES) {
    const run = { engine: eng };
    report.runs.push(run);
    await viewport(1280, 800, false);
    const d0 = dialogs.length;
    await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=felucca&engine=${eng}` });
    if (eng === "emu") ok(dialogs.length === d0 + 1 && dialogs.at(-1) === "beforeunload", "leaving with unsaved takes: the page asks first (beforeunload)");
    await until(`window.FM1Rec && window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "${eng}"`, 60000, `${eng} running`);
    await sleep(eng === "emu" ? 6000 : 1500);   // (the emulator's boot and JIT warm-up)
    ok(await js(`FM1Rec.o.getPlayer().ctx.state === "running" && !document.getElementById("rec").hidden && document.getElementById("rec-btn").getAttribute("aria-pressed") === "false"`),
       `${eng}: running; the record button shows, not pressed`);
    if (eng === "native") await shot("desktop-idle");

    // take 1: page volume 1
    const t1 = await takeOnce(eng === "native" ? "desktop-recording" : `${eng}-recording`, { shotAt: eng === "native" ? 1.2 : null });
    run.take = t1;
    checkTake(eng, t1, 2);
    ok(await js(`document.getElementById("rec-live").textContent === "Stopped, take saved" && !document.getElementById("takes").hidden && document.querySelectorAll("#takes .take").length === 1`),
       `${eng}: "Stopped, take saved" announced; the Takes popover lists the take`);
    if (eng === "native") {
      await shot("desktop-takes");
      // take 2: the page's volume at 0 records the same level
      await js(`(FM1Rec.o.getPlayer().setVolume(0), true)`);
      await sleep(300);
      const t2 = await takeOnce("vol0");
      const g = await js(`FM1Rec.o.getPlayer().out.gain.value`);
      const dB = 20 * Math.log10(t2.rmsKey / t1.rmsKey);
      run.vol0 = { gain: g, rmsKey: t2.rmsKey, dBvsVol1: dB };
      ok(g < 0.001 && t2.rmsKey > 0.003 && Math.abs(dB) < 3, `${eng}: page volume 0 (gain ${g.toFixed(4)}): the take is at full level (${dB.toFixed(2)} dB against volume 1)`);
      await js(`(FM1Rec.o.getPlayer().setVolume(1), true)`);
      // play / pause through the popover's <audio>
      await js(`(FM1Rec.closeTakes(), FM1Rec.openTakes(), document.querySelector("#takes .take-btn.play").click(), true)`);
      await sleep(600);
      ok(await js(`!FM1Rec.audio.paused && FM1Rec.audio.currentTime > 0.1 && FM1Rec.playing === FM1Rec.takes[0].key`), `${eng}: Play plays the take (an <audio> on its blob URL)`);
      await js(`(document.querySelector("#takes .take-btn.play").click(), true)`);
      await sleep(200);
      ok(await js(`FM1Rec.audio.paused && FM1Rec.playing === null`), `${eng}: Pause pauses it`);
      // rename, delete (two clicks), save (a download: the anchor's name)
      await js(`(() => { const t = FM1Rec.takes[0]; FM1Rec.renaming = t.key; FM1Rec.renderTakes(); const i = document.querySelector("#takes .take-name-in");
        i.value = "my take"; i.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" })); return true; })()`);
      ok(await js(`FM1Rec.takes[0].name === "my take" && document.querySelector("#takes .take-name").textContent === "my take"`), `${eng}: Rename renames the take`);
      ok(await js(`(() => { let got = null; const c = HTMLAnchorElement.prototype.click; HTMLAnchorElement.prototype.click = function () { got = this.download + " " + this.href.slice(0, 5); };
        document.querySelector('#takes .take-btn.primary').click(); HTMLAnchorElement.prototype.click = c; return got === "my take.wav blob:" && FM1Rec.takes[0].saved; })()`), `${eng}: Save downloads "my take.wav" from the blob URL`);
      await js(`(document.querySelectorAll("#takes .take-btn.danger")[0].click(), true)`);
      await js(`(document.querySelector("#takes .take-btn.danger-fill").click(), true)`);
      ok(await js(`FM1Rec.takes.length === 1 && document.querySelectorAll("#takes .take").length === 1`), `${eng}: Delete (asked twice) removes it`);
      // the settings' other choices: a 3 s count-in, 16-bit
      await js(`(FM1Rec.o.settings = () => ({ bits: 16, countIn: true }), true)`);
      await shiftR();
      await sleep(300);
      ok(await js(`FM1Rec.phase === "count" && document.getElementById("rec-time").textContent === "-3" && !document.getElementById("rec-time").hidden`), `${eng}: count-in: -3 shows, not recording yet`);
      await until(`FM1Rec.phase === "rec"`, 5000, "the count-in ends");
      await sleep(1000);
      await shiftR();
      await until(`FM1Rec.phase === "idle" && FM1Rec.takes.length === 2`, 5000);
      const c = await js(READ_TAKE(0, 0.5));
      ok(c.hdr.bits === 16 && c.hdr.blockAlign === 4 && c.decoded.frames === c.hdr.frames && Math.abs(c.hdr.frames / c.ctxRate - 1) < 0.15,
         `${eng}: after the count-in a 16-bit take of about 1 s (${(c.hdr.frames / c.ctxRate).toFixed(2)} s), Chromium decodes it`);
      await js(`(FM1Rec.o.settings = () => ({ bits: 24, countIn: false }), FM1Rec.closeTakes(), true)`);
    } else await shot("emu-takes");

    run.load = await loadCompare(eng);

    if (eng === "native") {
      // the phone: idle, recording, Takes
      await js(`(FM1Rec.closeTakes(), true)`);
      await viewport(390, 844, true);
      await shot("phone-idle");
      const t = await takeOnce("phone-recording", { shotAt: 1.0 });
      ok(t.hdr.frames > 0, "phone: a take");
      await sleep(300);
      await shot("phone-takes");
      ok(await js(`(() => { const p = document.getElementById("takes").getBoundingClientRect(); return p.left >= 0 && p.right <= innerWidth && document.documentElement.scrollWidth <= innerWidth; })()`),
         "phone: the Takes popover fits the screen, no sideways scroll");
      await js(`(FM1Rec.closeTakes(), true)`);
    } else {
      // an engine switch ends the take first
      const n0 = await js("FM1Rec.takes.length");
      await shiftR();
      await until(`FM1Rec.phase === "rec"`, 5000);
      await key("KeyA", "keyDown"); await sleep(1200); await key("KeyA", "keyUp");
      await js(`(document.getElementById("menu-btn").click(), true)`);
      await sleep(400);
      await js(`(() => { const b = [...document.querySelectorAll(".sheet .seg button")].find((x) => /^Native/.test(x.textContent)); b.click(); return true; })()`);
      await until(`FM1Rec.phase === "idle" && FM1Rec.takes.length === ${n0 + 1}`, 5000, "the take ended by the switch");
      await until(`FM1Site.current().engine === "native" && FM1Site.current().info`, 30000, "native after the switch");
      const t = await js(READ_TAKE(0.1, 1.0));
      ok(t.hdr.frames > 0.9 * t.ctxRate && Math.abs(t.hdr.frames - (t.endFrame - t.startFrame)) <= 128 && t.decoded.frames === t.hdr.frames,
         `emu -> native: the switch ended the take cleanly first (${(t.hdr.frames / t.ctxRate).toFixed(2)} s, valid WAV)`);
      // the visitor, not knowing it ended, presses Stop: that must not start a new take
      const n1 = await js("FM1Rec.takes.length");
      await shiftR();
      await sleep(600);
      ok(await js(`FM1Rec.phase === "idle" && FM1Rec.takes.length === ${n1} && /already stopped/.test(document.getElementById("toast").textContent)`),
         "a Stop pressed just after a take ended by itself starts no new take (it says so)");
    }
  }
  ok(!errors.length, `no page exceptions (${errors.length})`);
} catch (e) {
  console.error(e); fails++;
} finally {
  writeFileSync(join(OUT, "rec-e2e.json"), JSON.stringify(report, null, 1));
  try { ws && ws.close(); } catch {}
  stop();
}
console.log(`\n${checks - fails}/${checks} checks passed`);
process.exit(fails ? 1 : 0);
