// SPDX-License-Identifier: GPL-3.0-only
// The recorder end to end in headless Chromium (a real AudioContext and AudioWorklet, muted; no window, a fresh
// profile, the browser and the server ended by their PIDs). For the native engine and then the emulated one (Felucca):
// start the player, Shift+R, hold a key through the computer-key map, Shift+R again; read the take's WAV back (header,
// length against the worklet's recorded span, sound while the key is held, Chromium's own decoder); with the page's
// volume at 0 a second take has the same level; the worklet's load with recording off and on; a take ended by an
// engine switch. Screenshots (desktop 1280 x 800, phone 390 x 844): idle, recording, the Takes popover.
//   node web/site/tools/rec-e2e.mjs [--shots=DIR] [--secs=6] [--rounds=2] [--engines=native,emu]
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
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
for (const f of ["build/native/felucca.wasm", "build/emu/fm1_web.wasm", "build/packages/felucca-1.6.1.fwsc"])
  if (!existsSync(join(WEB, f))) { console.error(`missing web/${f}`); process.exit(2); }
mkdirSync(OUT, { recursive: true });
if (SHOTS2) mkdirSync(SHOTS2, { recursive: true });

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(96)} ${c ? "ok" : "FAIL"}`); return c; };

// the Hub, mocked in the page: its catalog is the snapshot; Felucca's package is the local copy; anything else a 404
const hub = readFileSync(join(WEB, "site", "data", "hub-catalog.snapshot.json"), "utf8");
const HUB_MOCK = `(() => {
  const HUB = ${hub}, real = window.fetch.bind(window);
  window.fetch = async (input, init) => {
    const u = String(input && input.url || input);
    if (!u.startsWith("https://thefirmwarehub.com/")) return real(input, init);
    if (u === "https://thefirmwarehub.com/catalog.json") return new Response(JSON.stringify(HUB), { status: 200, headers: { "content-type": "application/json" } });
    if (u === "https://thefirmwarehub.com/fw/felucca-1.6.1.fwsc") return real("/web/build/packages/felucca-1.6.1.fwsc");
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
const KEYS = { KeyA: ["a", 65], KeyD: ["d", 68], KeyG: ["g", 71], KeyR: ["r", 82], ShiftLeft: ["Shift", 16] };
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

  for (const eng of ENGINES) {
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
