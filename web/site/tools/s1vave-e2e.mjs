// SPDX-License-Identifier: GPL-3.0-only
// S1Vave, an .rfm1 this site hosts (web/site/fw/, data/site-rfm1.json, built by FM1VST's fm1_add_rfm1 on its Dinghy
// glue: the firmware's main loop on the in-module Asyncify coroutine), end to end: this tree's site (the dev layout,
// served from the repo root) in headless Chromium (no window, a fresh profile; a real AudioContext and AudioWorklet,
// muted). ?fw=s1vave boots it on the rfm1 engine from the site's file; it draws, a held key sounds, and the header's
// Source link and the About page give its licence and source at its commit. Screenshots: s1vave-player, s1vave-about.
//   node web/site/tools/s1vave-e2e.mjs [--shots=web/build/test]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test"));
const SITE_RFM1 = JSON.parse(readFileSync(join(ROOT, "web/site/data/site-rfm1.json"), "utf8")).firmwares.find((d) => /\/s1vave-/.test(d.file));
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const tmp = mkdtempSync(join(tmpdir(), "s1vave-e2e-")), profile = join(tmp, "profile");
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
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
  console.log("  page:", JSON.stringify(await js(`({ cur: window.FM1Site ? (({ entry, engine, info }) => ({ id: entry && entry.id, engine, info: !!info }))(FM1Site.current()) : null,
    toast: document.getElementById("toast")?.textContent, status: document.getElementById("status")?.textContent })`)));
  throw new Error(`timed out: ${what}`);
}
async function shot(name) {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from(r.result.data, "base64"));
  console.log(`  shot ${f}`);
}
const key = (type, code, k) => cdp("Input.dispatchKeyEvent", { type, code, key: k });
const esc = () => cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
const onSite = `window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "rfm1" && FM1Site.current().info.id === "s1vave" && FM1Site.current().entry.rfm1 && FM1Site.current().entry.rfm1.from === "site"`;
const WATCH = `(() => { const p = FM1Rec.o.getPlayer(), W = window.__w = { frames: 0, colours: 0, peaks: [], errors: [] };
  const ff = p.ev.frame; p.ev.frame = (px, seq) => { W.frames++; const s = new Set(); for (let i = 0; i < px.length; i += 7) { s.add(px[i]); if (s.size > 64) break; } W.colours = Math.max(W.colours, s.size); if (ff) ff(px, seq); };
  const fe = p.ev.error; p.ev.error = (m) => { W.errors.push(String(m)); if (fe) fe(m); };
  const an = p.ctx.createAnalyser(); an.fftSize = 2048; p.node.connect(an);
  const buf = new Float32Array(an.fftSize);
  W.timer = setInterval(() => { an.getFloatTimeDomainData(buf); let m = 0; for (const v of buf) m = Math.max(m, Math.abs(v)); W.peaks.push({ t: performance.now(), peak: m }); }, 40);
  return true; })()`;

try {
  let served = false;
  for (let i = 0; i < 50 && !served; i++) { await sleep(200); try { served = (await (await fetch(`http://127.0.0.1:${PORT}/web/site/catalog.json`)).json()).firmwares.some((f) => f.id === "s1vave"); } catch {} }
  if (!served) throw new Error(`no s1vave in http://127.0.0.1:${PORT}/web/site/catalog.json`);
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const exceptions = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") { exceptions.push(JSON.stringify(d.params.exceptionDetails).slice(0, 400)); console.error("exception:", exceptions.at(-1)); }
  };
  await cdp("Runtime.enable"); await cdp("Page.enable");
  const base = `http://127.0.0.1:${PORT}/web/site/`;

  // 1. the deep link: S1Vave on the rfm1 engine, its file from this site
  await cdp("Page.navigate", { url: `${base}?fw=s1vave` });
  await until(`window.FM1Rec && ${onSite}`, 60000, "S1Vave on the rfm1 engine");
  const cur = await js(`(() => { const c = FM1Site.current(); return { license: c.entry.license, local: !!c.entry.local,
    info: { name: c.info.name, version: c.info.version, license: c.info.license, source: c.info.source, credits: c.info.credits || "", lic: (c.info.licenseTexts || []).length } }; })()`);
  const fetched = await js(`performance.getEntriesByType("resource").map((r) => r.name).filter((n) => /\\.rfm1$/.test(n))`);
  ok(cur.info.name === "S1Vave" && cur.info.version === "1.0" && !cur.local, `?fw=s1vave boots S1Vave ${cur.info.version} on the rfm1 engine (the catalog's entry)`);
  ok(fetched.length === 1 && fetched[0].endsWith(`/web/site/${SITE_RFM1.file}`), `its .rfm1 came from this site: ${fetched.map((u) => u.replace(base, "")).join(", ")}`);
  ok(cur.info.license === "GPL-3.0-only" && /github\.com\/AndrewGG88\/S1Vave@[0-9a-f]{40}$/.test(cur.info.source) && cur.info.lic >= 3 && /zvenson/.test(cur.info.credits),
     `the manifest: ${cur.info.license}; ${cur.info.source}; ${cur.info.lic} licence texts; credits name zvenson`);

  // 2. it draws, and a held key (A: the FM-1's key 7) sounds
  await js(WATCH);
  await sleep(1500);
  let w = await js("window.__w");
  ok(w.frames > 0 && w.colours >= 2, `it draws (${w.frames} frames in 1.5 s, ${w.colours >= 64 ? "64+" : w.colours} colours)`);
  const t0 = await js("performance.now()");
  await key("keyDown", "KeyA", "a"); await sleep(1500); await key("keyUp", "KeyA", "a");
  await sleep(300);
  w = await js("window.__w");
  const played = w.peaks.filter((p) => p.t > t0 + 200 && p.t < t0 + 1500).map((p) => p.peak || 0);
  ok(played.length > 0 && Math.max(...played) > 0.01 && !w.errors.length, `a held key sounds (peak ${played.length ? Math.max(...played).toFixed(3) : "none"}${w.errors.length ? "; errors " + w.errors.join(" | ") : ""})`);
  await shot("s1vave-player");

  // 3. the header's Source link: S1Vave at its commit
  const src = await js(`(() => { const a = document.getElementById("fw-src"); return { href: a.href, title: a.title, off: a.classList.contains("off") }; })()`);
  ok(!src.off && /^https:\/\/github\.com\/AndrewGG88\/S1Vave\/tree\/[0-9a-f]{40}$/.test(src.href) && /GPL-3\.0/.test(src.title), `the header's Source link: ${src.href}`);

  // 4. About: the licence, the source and FM1VST's glue, the file, the licence texts
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await sleep(500);
  await js(`(document.querySelector(".fw-card.slim").click(), true)`);
  await sleep(500);
  const about = await js(`document.querySelector(".sheet-body").innerText`);
  const nLic = await js(`document.querySelectorAll(".sheet-body details.lic").length`);
  ok(/GPL-3\.0-only/.test(about) && /AndrewGG88\/S1Vave @ [0-9a-f]{7}/.test(about) && /FM1VST/.test(about) && /s1vave-1\.0\.rfm1/.test(about) && nLic === cur.info.lic,
     `About: the licence, S1Vave's source at its commit, FM1VST, the file, ${nLic} licence texts`);
  await shot("s1vave-about");
  await esc(); await esc();
  ok(!exceptions.length, `no exceptions in the page${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
} catch (e) {
  console.error(e.stack || e);
  fails++;
} finally {
  stop();
}
console.log(fails ? `s1vave e2e: ${fails} of ${checks} FAILED` : `s1vave e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
