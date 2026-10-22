// SPDX-License-Identifier: GPL-3.0-only
// Authors' native builds in the player, end to end (docs/NATIVE-AUTHORS.md): a built site (the Pages layout, with
// engines/native/ext/ filled by native_sync.mjs and the catalog by build_catalog.mjs --native-ext) served here, each
// firmware opened native in headless Chromium (no window, a fresh profile, the browser and the server ended by their
// PIDs; a real AudioContext and AudioWorklet, muted): the catalog entry is the author's build, its module is fetched
// from engines/native/ext/<id>/<version>/, it boots (the engine's info), draws (the frames the worklet posts are not
// blank), and sounds (an AnalyserNode on the worklet's output, while a key is held). A screenshot of each, and of the sheet's
// Engine section.
//   node web/site/tools/native-ext-e2e.mjs --site=DIR [--ids=felucca,amb] [--shots=DIR]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SITE = arg("site", ""), IDS = arg("ids", "felucca,amb").split(","), SHOTS = arg("shots", "");
if (!SITE) { console.error("native-ext-e2e: --site=DIR (a built site)"); process.exit(2); }
if (SHOTS) mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const profile = mkdtempSync(join(tmpdir(), "chromium-native-ext-"));
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", SITE], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,800",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
const stop = () => { try { chrome.kill(); } catch {} try { server.kill(); } catch {} };
process.on("exit", stop);
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
  while (Date.now() - t0 < ms) { if (await js(`!!(${expr})`)) return true; await sleep(100); }
  throw new Error(`timed out: ${what}`);
}
async function shot(name) {
  if (!SHOTS) return;
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  writeFileSync(join(SHOTS, `${name}.png`), Buffer.from(r.result.data, "base64"));
  console.log(`  shot ${name}.png`);
}
const key = (type) => cdp("Input.dispatchKeyEvent", { type, code: "KeyA", key: "a", windowsVirtualKeyCode: 65 });

// the worklet's frames, and its output's level (an AnalyserNode on the worklet node, before the page's volume)
const WATCH = `(() => { const p = FM1Rec.o.getPlayer(), W = window.__w = { frames: 0, colours: 0, peaks: [], errors: [] };
  const ff = p.ev.frame; p.ev.frame = (px, seq) => { W.frames++; const s = new Set(); for (let i = 0; i < px.length; i += 7) { s.add(px[i]); if (s.size > 64) break; } W.colours = Math.max(W.colours, s.size); if (ff) ff(px, seq); };
  const fe = p.ev.error; p.ev.error = (m) => { W.errors.push(String(m)); if (fe) fe(m); };
  const an = p.ctx.createAnalyser(); an.fftSize = 2048; p.node.connect(an);
  const buf = new Float32Array(an.fftSize);
  W.timer = setInterval(() => { an.getFloatTimeDomainData(buf); let m = 0; for (const v of buf) m = Math.max(m, Math.abs(v)); W.peaks.push({ t: performance.now(), peak: m }); }, 40);
  return true; })()`;

try {
  // the server up and serving this site (not another one on the port) before the first page
  let served = false;
  for (let i = 0; i < 50 && !served; i++) { await sleep(200); try { served = "nativeAuthors" in (await (await fetch(`http://127.0.0.1:${PORT}/catalog.json`)).json()); } catch {} }
  if (!served) throw new Error(`no catalog.json with nativeAuthors at http://127.0.0.1:${PORT}/ (the server, or another one on the port)`);
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Page.javascriptDialogOpening") ws.send(JSON.stringify({ id: ++id, method: "Page.handleJavaScriptDialog", params: { accept: true } }));
    if (d.method === "Runtime.exceptionThrown") console.error("exception:", JSON.stringify(d.params.exceptionDetails).slice(0, 400));
  };
  await cdp("Runtime.enable"); await cdp("Page.enable");
  for (const fw of IDS) {
    await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/?fw=${fw}&engine=native&view=screen` });
    try { await until(`window.FM1Rec && window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "native" && FM1Site.current().entry.id === "${fw}"`, 60000, `${fw} running native`); }
    catch (e) {
      console.log("  page:", JSON.stringify(await js(`({ url: location.href, cur: window.FM1Site ? (({ entry, engine, info }) => ({ id: entry && entry.id, engine, info: !!info }))(FM1Site.current()) : null,
        toast: document.getElementById("toast")?.textContent, status: document.getElementById("status")?.textContent })`)));
      throw e;
    }
    const cur = await js(`(() => { const c = FM1Site.current(); return { native: c.entry.native, why: c.entry.nativeWhy || null, alt: c.entry.nativeAlt || null, info: { id: c.info.id, name: c.info.name, version: c.info.version, license: c.info.license } }; })()`);
    const fetched = await js(`performance.getEntriesByType("resource").map((e) => e.name).filter((n) => n.endsWith(".wasm"))`);
    ok(cur.native.by === "author" && /^engines\/native\/ext\//.test(cur.native.wasm) && /\/actions\/runs\//.test(cur.native.buildLog || ""),
       `${fw}: the catalog's native build is the author's (${cur.native.wasm}), with its build log`);
    ok(fetched.some((u) => u.endsWith(`/engines/native/ext/${cur.native.wasm.replace(/^engines\/native\/ext\//, "")}`)) && !fetched.some((u) => /engines\/native\/[a-z]+\.wasm$/.test(u)),
       `${fw}: the page fetched the module from engines/native/ext/ (${fetched.map((u) => u.replace(/^https?:\/\/[^/]+\//, "")).join(", ")})`);
    ok(cur.info.version === cur.native.version && cur.info.name, `${fw}: it booted: ${cur.info.name} ${cur.info.version}, ${cur.info.license}`);
    await js(WATCH);
    await sleep(1500);
    let w = await js("window.__w");
    ok(w.frames > 0 && w.colours >= 2, `${fw}: it draws (${w.frames} frames in 1.5 s, ${w.colours >= 64 ? "64+" : w.colours} colours)`);
    const t0 = await js("performance.now()");
    await key("keyDown");
    await sleep(1500);
    await key("keyUp");
    await sleep(300);
    w = await js("window.__w");
    const held = w.peaks.filter((p) => p.t > t0 + 200 && p.t < t0 + 1500).map((p) => p.peak || 0);
    await js("(clearInterval(window.__w.timer), true)");
    ok(held.length > 0 && Math.max(...held) > 0.005 && !w.errors.length, `${fw}: it sounds with a key held (peak ${held.length ? Math.max(...held).toFixed(3) : "none"}${w.errors.length ? "; errors " + w.errors.join(" | ") : ""})`);
    await shot(`native-ext-${fw}`);
    // the sheet's Engine section: who built it, and which build runs
    await js(`(document.getElementById("corner").click(), true)`);
    await sleep(600);
    const sheetText = await js(`document.body.innerText`);
    ok(/native · built by the author/.test(sheetText) && /build log/.test(sheetText) && (!cur.alt || /Also: FM1VST's core/.test(sheetText)),
       `${fw}: the sheet says "native · built by the author", the build log${cur.alt ? ", and FM1VST's core as the other build" : ""}`);
    await js(`(document.querySelector(".native-by")?.scrollIntoView({ block: "center" }), true)`);
    await sleep(300);
    await shot(`native-ext-${fw}-sheet`);
    await cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
  }
  // the authors' page
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/native/` });
  await sleep(800);
  const page = await js(`({ title: document.title, h1: document.querySelector("h1")?.textContent, wide: document.documentElement.scrollWidth <= innerWidth + 1 })`);
  ok(/Native builds/.test(page.title) && page.h1 && page.wide, `/native/: "${page.h1}", no horizontal scroll at 1280`);
  await shot("native-page");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(500);
  const narrow = await js(`document.documentElement.scrollWidth <= innerWidth + 1`);
  ok(narrow, "/native/: no horizontal page scroll at 390 px (tables scroll in their own box)");
  await shot("native-page-phone");
} catch (e) {
  console.error(e.stack || e);
  fails++;
} finally {
  stop();
}
console.log(fails ? `native-ext e2e: ${fails} of ${checks} FAILED` : `native-ext e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
