// SPDX-License-Identifier: GPL-3.0-only
// Live audio input ("ain1", web/engine/API.md section 6) in the player, end to end: this tree's site (the dev layout,
// served from the repo root) in headless Chromium (no window, a fresh profile, the browser and the server ended by
// their PIDs; a real AudioContext and AudioWorklet, muted) with a fake microphone: a WAV made here (short noise bursts,
// 0.3 s apart, below ~6 kHz) played as the capture device (--use-fake-device-for-media-stream --use-file-for-fake-audio-capture,
// the permission granted by --use-fake-ui-for-media-stream). The cart: web/engines/rfm1/fixtures/ain_cart.rfm1
// ("wants": ["ain1"], its output is its input; green screen while input is live), listed in the catalog through the
// DevTools protocol as one of this site's .rfm1 files, beside labels_core.rfm1 (a cart without input) to switch to.
//   1. the cart runs, input off: the hint "This firmware can use audio input", the badge, silence, a grey screen
//   2. Turn on (the hint's button): live, green; the output carries the input: the mic (its MediaStreamSource) and the
//      host node's output recorded together in one AudioWorklet, cross-correlated: the lag (the round trip inside the
//      page) against the latency the cart was told, and the correlation
//   3. the sheet: Audio input on, the device picker, the level meter; About: "uses audio input"
//   4. Off (the sheet): the tracks stopped, the cart told state 0 (grey), silence; the choice kept (localStorage)
//   5. on again, then another firmware: the input stopped; back to the cart: on again by itself (remembered, allowed)
//   6. the cart as a Fantasy CFW with "ain1" in its needs: the prompt "This firmware uses audio input", About, the sheet
//   node web/site/tools/ain-e2e.mjs [--shots=web/build/test]
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, renameSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { rfm1Entry } from "../js/live.js";
import { inspect } from "../../engines/rfm1/engine.js";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test"));
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(110)} ${c ? "ok" : "FAIL"}`); return c; };

// the carts, as this site's .rfm1 files
const fx = (n) => readFileSync(join(ROOT, "web/engines/rfm1/fixtures", n));
const cartEntry = (bytes, file, what) => rfm1Entry({ file, author: "tests", category: "Other", tag: "test cart", what }, inspect(bytes).manifest,
  { sha256: createHash("sha256").update(bytes).digest("hex"), size: bytes.length });
const leb = (v) => { const o = []; do { let c = v & 0x7f; v = Math.floor(v / 128); if (v) c |= 0x80; o.push(c); } while (v); return o; };
const withManifest = (b, m) => { const nm = Buffer.from("rfm1"), js = Buffer.from(JSON.stringify(m)), body = [...leb(nm.length), ...nm, ...js]; return Buffer.from([...b, 0, ...leb(body.length), ...body]); };
const AIN = fx("ain_cart.rfm1"), LBL = fx("labels_core.rfm1");
// (6.) the same cart as a Fantasy CFW that needs its input
const NEEDS = withManifest(fx("ain_cart.wasm"), { ...inspect(AIN).manifest, wants: undefined, virtual: true, needs: ["ain1"] });
const entries = (needs) => [cartEntry(needs ? NEEDS : AIN, "fw/aintest-1.0.rfm1", "A test cart: its output is its audio input."), cartEntry(LBL, "fw/labeltest-1.0.rfm1", "A test cart without audio input.")];
let variant = "wants";
const files = () => ({ "/fw/aintest-1.0.rfm1": variant === "needs" ? NEEDS : AIN, "/fw/labeltest-1.0.rfm1": LBL });

// the fake microphone: 48 kHz mono, 6 s, a 6 ms noise burst every 0.3 s (a fixed seed)
const tmp = mkdtempSync(join(tmpdir(), "ain-e2e-")), profile = join(tmp, "profile"), wav = join(tmp, "mic.wav");
{
  const sr = 48000, n = sr * 6, b = Buffer.alloc(44 + 2 * n);
  b.write("RIFF", 0); b.writeUInt32LE(36 + 2 * n, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(sr, 24); b.writeUInt32LE(2 * sr, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36); b.writeUInt32LE(2 * n, 40);
  let seed = 12345;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 2 ** 32) * 2 - 1;
  // (noise low-passed below ~6 kHz: a lag between two samples, as the resamplers make, barely lowers the correlation)
  const x = new Float32Array(n);
  for (let i = 0; i < n; i++) x[i] = i % Math.round(0.3 * sr) < 0.006 * sr ? rnd() : 0;
  for (let pass = 0; pass < 3; pass++) for (let i = n - 1; i >= 3; i--) x[i] = (x[i] + x[i - 1] + x[i - 2] + x[i - 3]) / 4;
  for (let i = 0; i < n; i++) b.writeInt16LE(Math.round(x[i] * 40000), 44 + 2 * i);
  writeFileSync(wav, b);
}
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,800",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", "--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
  `--use-file-for-fake-audio-capture=${wav}`, `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
const stop = () => { try { process.kill(chrome.pid); } catch {} try { process.kill(server.pid); } catch {} };
process.on("exit", () => { stop(); try { renameSync(tmp, `${tmp}.done`); } catch {} });
process.on("SIGINT", () => process.exit(130));

let ws, id = 0;
const waiting = new Map();
const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
async function js(expression, timeout = 60000) {
  const r = await cdp("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true, timeout });
  if (r.result?.exceptionDetails) throw new Error(`page: ${JSON.stringify(r.result.exceptionDetails).slice(0, 500)}`);
  return r.result?.result?.value;
}
async function until(expr, ms, what) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await js(`(async () => !!(await (${expr})))()`)) return true; await sleep(100); }
  console.log("  page:", JSON.stringify(await js(`({ cur: window.FM1Site ? (({ entry, engine, info }) => ({ id: entry && entry.id, engine, ain: info && info.ain }))(FM1Site.current()) : null,
    ain: window.FM1Ain && { live: FM1Ain.live, busy: FM1Ain.busy, error: FM1Ain.error },
    toast: document.getElementById("toast")?.textContent, status: document.getElementById("status")?.textContent })`)));
  throw new Error(`timed out: ${what}`);
}
async function shot(name) {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from(r.result.data, "base64"));
  console.log(`  shot ${f}`);
}
const esc = () => cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
const running = (fw) => `window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "rfm1" && FM1Site.current().info.id === "${fw}"`;
const sheetText = () => js(`document.querySelector(".sheet-body").innerText`);
const player = "FM1Rec.o.getPlayer()";
// the host node's output level (an AnalyserNode), the peak over the last ms
const WATCH = `(() => { const p = ${player}; if (window.__w) clearInterval(window.__w.timer); const W = window.__w = { peaks: [] };
  const an = p.ctx.createAnalyser(); an.fftSize = 2048; p.node.connect(an); const buf = new Float32Array(an.fftSize);
  W.timer = setInterval(() => { an.getFloatTimeDomainData(buf); let m = 0; for (const v of buf) m = Math.max(m, Math.abs(v)); W.peaks.push({ t: performance.now(), peak: m }); }, 40);
  return true; })()`;
const peakSince = (ms) => js(`(() => { const t = performance.now() - ${ms}; return Math.max(0, ...window.__w.peaks.filter((p) => p.t > t).map((p) => p.peak)); })()`);
// the mic's source and the host node's output, recorded in step (one AudioWorklet with two inputs) for `sec` seconds
const TAP = `(async (sec) => {
  const p = ${player}, ctx = p.ctx;
  if (!window.__tapAdded) {
    const src = "registerProcessor('ain-tap', class extends AudioWorkletProcessor { constructor(o) { super(); this.n = o.processorOptions.n; this.a = new Float32Array(this.n); this.b = new Float32Array(this.n); this.at = 0; }" +
      " process(i) { if (this.at >= this.n) return false; const x = i[0][0], y = i[1][0], k = Math.min(128, this.n - this.at); for (let j = 0; j < k; j++) { this.a[this.at + j] = x ? x[j] : 0; this.b[this.at + j] = y ? y[j] : 0; }" +
      " this.at += k; if (this.at >= this.n) this.port.postMessage({ a: this.a, b: this.b }); return true; } });";
    await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([src], { type: "text/javascript" })));
    window.__tapAdded = true;
  }
  const n = Math.round(sec * ctx.sampleRate), tap = new AudioWorkletNode(ctx, "ain-tap", { numberOfInputs: 2, numberOfOutputs: 1, processorOptions: { n } });
  const sink = ctx.createGain(); sink.gain.value = 0; tap.connect(sink); sink.connect(ctx.destination);
  p.ain.src.connect(tap, 0, 0); p.node.connect(tap, 0, 1);
  const got = await new Promise((r) => { tap.port.onmessage = (e) => r(e.data); });
  try { p.ain.src.disconnect(tap); } catch (_) {} try { p.node.disconnect(tap); } catch (_) {} tap.disconnect(); sink.disconnect();
  return { rate: ctx.sampleRate, a: Array.from(got.a), b: Array.from(got.b) };
})`;
// the lag of b behind a (0..maxLag), with the normalised correlation
function xcorr(a, b, maxLag) {
  let best = 0, bestLag = 0;
  for (let lag = 0; lag <= maxLag; lag++) {
    let s = 0, ea = 0, eb = 0;
    for (let i = 0; i + lag < b.length; i++) { s += a[i] * b[i + lag]; ea += a[i] * a[i]; eb += b[i + lag] * b[i + lag]; }
    const c = ea > 0 && eb > 0 ? s / Math.sqrt(ea * eb) : 0;
    if (c > best) { best = c; bestLag = lag; }
  }
  // the peak's sub-sample position (parabolic, on the raw sums)
  const r = (lag) => { let s = 0; for (let i = 0; i + lag < b.length; i++) s += a[i] * b[i + lag]; return s; };
  const y0 = r(bestLag - 1), y1 = r(bestLag), y2 = r(bestLag + 1), d = y0 - 2 * y1 + y2;
  return { lag: d ? bestLag + 0.5 * (y0 - y2) / d : bestLag, corr: best };
}

try {
  let served = false;
  for (let i = 0; i < 50 && !served; i++) { await sleep(200); try { served = "rfm1" in (await (await fetch(`http://127.0.0.1:${PORT}/web/site/catalog.json`)).json()).paths.engines; } catch {} }
  if (!served) throw new Error(`no catalog.json with paths.engines.rfm1 at http://127.0.0.1:${PORT}/web/site/`);
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const exceptions = [];
  ws.onmessage = async (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") { exceptions.push(JSON.stringify(d.params.exceptionDetails).slice(0, 400)); console.error("exception:", exceptions.at(-1)); }
    if (d.method === "Fetch.requestPaused") {
      const { requestId, request } = d.params;
      let body = null;
      const path = new URL(request.url).pathname.replace(/^\/web\/site/, "");
      if (/\/catalog\.json$/.test(path)) {
        const c = await (await fetch(request.url)).json();
        c.firmwares.unshift(...entries(variant === "needs"));
        body = Buffer.from(JSON.stringify(c));
      } else if (files()[path]) body = files()[path];
      if (body) cdp("Fetch.fulfillRequest", { requestId, responseCode: 200, responseHeaders: [{ name: "Content-Type", value: /json/.test(path) ? "application/json" : "application/wasm" }], body: body.toString("base64") });
      else cdp("Fetch.continueRequest", { requestId });
    }
  };
  await cdp("Runtime.enable"); await cdp("Page.enable"); await cdp("DOM.enable");
  await cdp("Fetch.enable", { patterns: [{ urlPattern: "*catalog.json*" }, { urlPattern: "*/fw/aintest-1.0.rfm1" }, { urlPattern: "*/fw/labeltest-1.0.rfm1" }] });
  const base = `http://127.0.0.1:${PORT}/web/site/`;
  await cdp("Page.navigate", { url: `${base}?fw=aintest&view=screen` });
  await until(`window.FM1Rec && window.FM1Ain && ${running("aintest")}`, 60000, "the cart running");
  await js(WATCH);

  // 1. input off
  ok(await js(`FM1Ain.supported && FM1Site.current().info.ain === "wants"`), "Chromium: audio input supported; the cart's info says ain \"wants\"");
  await until(`!document.querySelector(".ain-ask").hidden`, 5000, "the hint");
  const ask = await js(`document.querySelector(".ain-ask").innerText.replace(/\\s+/g, " ")`);
  await sleep(600);
  ok(/can use audio input/.test(ask) && /Turn on/.test(ask) && (await peakSince(500)) === 0 && await js(`FM1Site.screen().pixels[0] === 0x2104`),
     `input off: the hint "${ask}", silence, the cart's grey screen (state 0)`);
  await shot("ain-hint");

  // 2. Turn on
  await js(`(document.querySelector(".ain-ask .pill.primary").click(), true)`);
  await until(`FM1Ain.live && ${player}.ain`, 10000, "input on");
  await until(`FM1Site.screen().pixels[0] === 0x07e0`, 5000, "the cart's green screen (state 1)");
  await sleep(1000);
  const pk = await peakSince(800);
  const live = await js(`(({ live }) => ({ ...live }))(FM1Ain)`), cap = await js(`${player}.ain.latency`);
  ok(pk > 0.1 && await js(`document.querySelector(".ain-ask").hidden`), `on: green, the output carries the fake mic (peak ${pk.toFixed(3)}); the hint gone`);
  const miss0 = await js(`FM1Ain.live.missing || 0`);
  const rec = await js(`${TAP}(2.5)`, 30000);
  const xc = xcorr(rec.a, rec.b, 4000);
  const missed = (await js(`FM1Ain.live.missing || 0`)) - miss0;
  console.log(`  input quanta the worklet had none for while recording: ${missed / 128}`);
  const told = live.told || await js(`FM1Ain.live.told`), inner = (told - cap * 44100) * rec.rate / 44100;
  console.log(`  context ${rec.rate} Hz; capture latency ${(cap * 1000).toFixed(2)} ms; told ${told} frames (${(told / 44.1).toFixed(2)} ms)`);
  ok(xc.corr > 0.9 && Math.abs(xc.lag - inner) < 2,
     `the mic -> the output inside the page: lag ${xc.lag.toFixed(2)} frames = ${(xc.lag / rec.rate * 1000).toFixed(2)} ms (the cart told ${told} = this ${inner.toFixed(2)} + the capture's ${(cap * 1000).toFixed(2)} ms), correlation ${xc.corr.toFixed(3)}`);
  await shot("ain-on");

  // 3. the sheet, About
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await sleep(500);
  const main = await sheetText();
  ok(/Audio input\s*on/i.test(main) && await js(`!!document.querySelector('.sheet-body select[aria-label="Input device"]') && document.querySelectorAll('.sheet-body select[aria-label="Input device"] option').length >= 1`)
     && await js(`!!document.querySelector(".sheet-body .fw-card .ain-badge")`),
     "the sheet: Audio input on, the device picker, the firmware card's audio-in badge");
  let meter = 0;
  for (let i = 0; i < 12; i++) { await sleep(40); meter = Math.max(meter, await js(`parseFloat(document.querySelector(".ain-meter i").style.width) || 0`)); }
  ok(meter > 10, `the level meter moves (${meter}%)`);
  await shot("ain-sheet");
  await js(`(document.querySelector(".fw-card").click(), true)`);
  await sleep(500);
  ok(await js(`!!document.querySelector('.card[data-id="aintest"] .ain-badge') && !document.querySelector('.card[data-id="labeltest"] .ain-badge')`), "the firmware browser: the cart's card has the badge, the other cart's not");
  await esc(); await sleep(300);
  await js(`(document.querySelector(".fw-card.slim").click(), true)`);
  await sleep(500);
  ok(/uses audio input when you turn it on/.test(await sheetText()), "About: \"uses audio input when you turn it on\"");
  await esc(); await sleep(300);

  // 4. Off from the sheet
  await js(`(window.__tracks = ${player}.ain.stream.getTracks(), true)`);
  await js(`(Array.from(document.querySelectorAll('.sheet-body [aria-label="Audio input"] button')).find((b) => b.textContent === "Off").click(), true)`);
  await until(`!FM1Ain.live && !${player}.ain`, 5000, "input off");
  await until(`FM1Site.screen().pixels[0] === 0x2104`, 5000, "the cart's grey screen (state 0)");
  await sleep(700);
  ok(await js(`window.__tracks.every((t) => t.readyState === "ended")`) && (await peakSince(400)) === 0, "Off: the tracks stopped, the cart told state 0 (grey), silence");
  ok(await js(`JSON.parse(localStorage.getItem("remotefm1-ain")).fw.aintest.on === false`), "Off kept for this firmware (localStorage)");
  await esc(); await sleep(300);

  // 5. on, another firmware, back
  await js(`FM1Ain.start()`);
  await until(`FM1Ain.live`, 10000, "input on again");
  ok(await js(`JSON.parse(localStorage.getItem("remotefm1-ain")).fw.aintest.on === true`), "On kept for this firmware");
  await js(`(window.__tracks = ${player}.ain.stream.getTracks(), true)`);
  await js(`(document.getElementById("menu-btn").click(), true)`); await sleep(300);
  await js(`(document.querySelector(".fw-card").click(), true)`); await sleep(400);
  await js(`(document.querySelector('.card[data-id="labeltest"]').click(), true)`);
  await until(running("labeltest"), 20000, "the other cart");
  ok(await js(`!FM1Ain.live && !${player}.ain && window.__tracks.every((t) => t.readyState === "ended") && document.querySelector(".ain-ask").hidden`)
     && !/Audio input/.test(await js(`(document.getElementById("menu-btn").click(), document.querySelector(".sheet-body").innerText)`)),
     "another firmware (no ain1): the input stopped, its tracks ended; no prompt, no Audio input section");
  await esc(); await sleep(300);
  const perm = await js(`navigator.permissions.query({ name: "microphone" }).then((s) => s.state).catch(() => "?")`);
  await js(`(document.getElementById("menu-btn").click(), true)`); await sleep(300);
  await js(`(document.querySelector(".fw-card").click(), true)`); await sleep(400);
  await js(`(document.querySelector('.card[data-id="aintest"]').click(), true)`);
  await until(running("aintest"), 20000, "back to the cart");
  if (perm === "granted") {
    await until(`FM1Ain.live`, 8000, "input back on by itself");
    ok(true, "back to the cart: input on again by itself (remembered; the browser already allows the microphone)");
  } else {
    await sleep(1500);
    ok(await js(`!FM1Ain.live && !document.querySelector(".ain-ask").hidden`), `back to the cart (microphone permission "${perm}"): not on by itself, the hint offers it`);
  }

  // 6. needs
  variant = "needs";
  await js(`(localStorage.removeItem("remotefm1-ain"), window.__before = true)`);
  await cdp("Page.reload", {});
  await until(`!window.__before && window.FM1Ain && ${running("aintest")}`, 60000, "the needs cart");
  await until(`!document.querySelector(".ain-ask").hidden`, 5000, "the prompt");
  const ask2 = await js(`document.querySelector(".ain-ask").innerText.replace(/\\s+/g, " ")`);
  ok(await js(`FM1Site.current().info.ain === "needs"`) && /This firmware uses audio input/.test(ask2) && /Turn on input/.test(ask2) && await js(`!FM1Ain.live`),
     `needs: off at first (no click yet), the prompt "${ask2}"`);
  await shot("ain-needs");
  await js(`(document.getElementById("menu-btn").click(), true)`); await sleep(400);
  ok(/Audio input\s*off: it needs it/i.test(await sheetText()), "the sheet: Audio input \"off: it needs it\"");
  await js(`(document.querySelector(".fw-card.slim").click(), true)`); await sleep(400);
  ok(/uses audio input: it needs it/.test(await sheetText()), "About: \"uses audio input: it needs it\"");
  await esc(); await esc(); await sleep(300);
  await js(`(document.querySelector(".ain-ask .pill.primary").click(), true)`);
  await until(`FM1Ain.live && FM1Site.screen().pixels[0] === 0x07e0`, 10000, "input on (needs)");
  ok(true, "needs: Turn on input: live, green");
  ok(exceptions.length === 0, `no exception in the page (${exceptions.length})`);
} catch (e) {
  console.error(e);
  fails++;
} finally {
  stop();
}
console.log(fails ? `ain-e2e: ${fails} of ${checks} FAILED` : `ain-e2e: all ${checks} passed`);
process.exit(fails ? 1 : 0);
