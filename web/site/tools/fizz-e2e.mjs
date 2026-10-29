// SPDX-License-Identifier: GPL-3.0-only
// FIZZ, this site's own .rfm1 (web/site/fw/, data/site-rfm1.json, built by tools/rfm1/build_fizz.sh), end to end: this
// tree's site (the dev layout, served from the repo root) in headless Chromium (no window, a fresh profile, the
// browser and the server ended by their PIDs; a real AudioContext and AudioWorklet, muted). ?fw=fizz boots on the
// rfm1 engine from the site's file; it draws, and PLAY (its demo pattern) sounds; the header's Source link and the
// About page give the manifest's licence, FIZZ's source at its commit, the glue, the credits and the licence texts;
// the autosave is FIZZ's under engine "rfm1"; the Native filter lists it with the native badge; the same file opened
// by the visitor lands in Your firmware, apart from the catalog entry; a file whose SHA-256 differs is refused.
// Screenshots: fizz-player (desktop), fizz-player-phone, fizz-about.
//   node web/site/tools/fizz-e2e.mjs [--shots=web/build/test]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test"));
const SITE_RFM1 = JSON.parse(readFileSync(join(ROOT, "web/site/data/site-rfm1.json"), "utf8")).firmwares.find((d) => /\/fizz-/.test(d.file));
const FILE = join(ROOT, "web/site", SITE_RFM1.file);
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const tmp = mkdtempSync(join(tmpdir(), "fizz-e2e-")), profile = join(tmp, "profile");
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,800",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
const stop = () => { try { chrome.kill(); } catch {} try { server.kill(); } catch {} };
process.on("exit", stop);
process.on("SIGINT", () => process.exit(130));

let ws, id = 0;
const waiting = new Map(), handlers = [];
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
const press = async (code, key, ms) => {
  await cdp("Input.dispatchKeyEvent", { type: "keyDown", code, key });
  await sleep(ms);
  await cdp("Input.dispatchKeyEvent", { type: "keyUp", code, key });
};
const esc = () => cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
// the catalog's FIZZ, running on the rfm1 engine from this site's file
const siteFizz = `window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "rfm1" && FM1Site.current().info.id === "fizz" && FM1Site.current().entry.rfm1 && FM1Site.current().entry.rfm1.from === "site"`;
const WATCH = `(() => { const p = FM1Rec.o.getPlayer(), W = window.__w = { frames: 0, colours: 0, peaks: [], errors: [] };
  const ff = p.ev.frame; p.ev.frame = (px, seq) => { W.frames++; const s = new Set(); for (let i = 0; i < px.length; i += 7) { s.add(px[i]); if (s.size > 64) break; } W.colours = Math.max(W.colours, s.size); if (ff) ff(px, seq); };
  const fe = p.ev.error; p.ev.error = (m) => { W.errors.push(String(m)); if (fe) fe(m); };
  const an = p.ctx.createAnalyser(); an.fftSize = 2048; p.node.connect(an);
  const buf = new Float32Array(an.fftSize);
  W.timer = setInterval(() => { an.getFloatTimeDomainData(buf); let m = 0; for (const v of buf) m = Math.max(m, Math.abs(v)); W.peaks.push({ t: performance.now(), peak: m }); }, 40);
  return true; })()`;
const STORED = `(async () => {
  const open = (n) => new Promise((res, rej) => { const r = indexedDB.open(n); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  const all = (d, s) => new Promise((res, rej) => { const r = d.transaction(s, "readonly").objectStore(s).getAll(); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  let lib = [];
  try { lib = await all(await open("remotefm1-firmware"), "rfm1"); } catch (_) { lib = []; }
  const sv = await all(await open("remotefm1-player"), "saves");
  return { lib: lib.map((r) => ({ id: r.manifest.id, sha: r.sha256 })), saves: sv.map((s) => [s.fw, s.engine, s.version, s.name, s.bytes.length]) };
})()`;

try {
  let served = false;
  for (let i = 0; i < 50 && !served; i++) { await sleep(200); try { served = (await (await fetch(`http://127.0.0.1:${PORT}/web/site/catalog.json`)).json()).firmwares.some((f) => f.id === "fizz"); } catch {} }
  if (!served) throw new Error(`no fizz in http://127.0.0.1:${PORT}/web/site/catalog.json`);
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const exceptions = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") { exceptions.push(JSON.stringify(d.params.exceptionDetails).slice(0, 400)); console.error("exception:", exceptions.at(-1)); }
    for (const h of handlers) h(d);
  };
  await cdp("Runtime.enable"); await cdp("Page.enable"); await cdp("DOM.enable");
  const base = `http://127.0.0.1:${PORT}/web/site/`;

  // 1. the deep link: the catalog's FIZZ on the rfm1 engine, its file from this site
  await cdp("Page.navigate", { url: `${base}?fw=fizz` });
  await until(`window.FM1Rec && ${siteFizz}`, 60000, "FIZZ on the rfm1 engine");
  const cur = await js(`(() => { const c = FM1Site.current(); return { version: c.entry.version, license: c.entry.license, local: !!c.entry.local,
    info: { name: c.info.name, version: c.info.version, license: c.info.license, source: c.info.source, authors: c.info.authors, credits: c.info.credits || "", lic: (c.info.licenseTexts || []).length } }; })()`);
  const fetched = await js(`performance.getEntriesByType("resource").map((r) => r.name).filter((n) => /\\.rfm1$/.test(n))`);
  ok(cur.info.name === "FIZZ" && cur.info.version === SITE_RFM1.file.replace(/^.*fizz-|\.rfm1$/g, "") && !cur.local,
     `?fw=fizz boots FIZZ ${cur.info.version} on the rfm1 engine (the catalog's entry, not a local file)`);
  ok(fetched.length === 1 && fetched[0].endsWith(`/web/site/${SITE_RFM1.file}`), `its .rfm1 came from this site: ${fetched.map((u) => u.replace(base, "")).join(", ")}`);
  ok(/^MPL-2\.0\b/.test(cur.info.license) && cur.info.license === cur.license && /github\.com\/Quixotic7\/FIZZ@[0-9a-f]{40}$/.test(cur.info.source) && cur.info.lic >= 5 && /plutovg/.test(cur.info.credits),
     `the manifest: ${cur.info.license}; ${cur.info.source}; ${cur.info.lic} licence texts; credits`);
  ok(await js(`new URL(location.href).searchParams.get("fw") === "fizz"`), "the address keeps ?fw=fizz (a deep link)");

  // 2. it draws, and PLAY (the demo pattern) sounds
  await js(WATCH);
  await sleep(1200);
  let w = await js("window.__w");
  ok(w.frames > 0 && w.colours >= 2, `it draws (${w.frames} frames in 1.2 s, ${w.colours >= 64 ? "64+" : w.colours} colours)`);
  const t0 = await js("performance.now()");
  await press("Minus", "-", 120);                       // (PLAY)
  await sleep(2500);
  w = await js("window.__w");
  const played = w.peaks.filter((p) => p.t > t0 + 300).map((p) => p.peak || 0);
  ok(played.length > 0 && Math.max(...played) > 0.01 && !w.errors.length, `PLAY starts its demo pattern: sound (peak ${played.length ? Math.max(...played).toFixed(3) : "none"}${w.errors.length ? "; errors " + w.errors.join(" | ") : ""})`);
  await shot("fizz-player");

  // 3. the header's Source link: FIZZ at its commit
  const src = await js(`(() => { const a = document.getElementById("fw-src"); return { href: a.href, title: a.title, off: a.classList.contains("off") }; })()`);
  ok(!src.off && /^https:\/\/github\.com\/Quixotic7\/FIZZ\/tree\/[0-9a-f]{40}$/.test(src.href) && /MPL-2\.0/.test(src.title), `the header's Source link: ${src.href}`);

  // 4. the sheet and the About page
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await sleep(500);
  const mainText = await js(`document.querySelector(".sheet-body").innerText`);
  ok(/native \(\.rfm1\)/.test(mainText) && /this site's \.rfm1 build/.test(mainText) && !/your file/i.test(mainText), "the sheet: native (.rfm1), this site's build (not your file)");
  await js(`(document.querySelector(".fw-card.slim").click(), true)`);
  await sleep(500);
  const about = await js(`document.querySelector(".sheet-body").innerText`);
  const nLic = await js(`document.querySelectorAll(".sheet-body details.lic").length`);
  ok(/MPL-2\.0 AND GPL-3\.0-only AND MIT AND FTL AND OFL-1\.1/.test(about) && /Quixotic7\/FIZZ @ [0-9a-f]{7}/.test(about) && /Quixotic7\/FM1VST @ [0-9a-f]{7}/.test(about)
     && /fizz-[\w.-]+\.rfm1/.test(about) && /SHA-256 [0-9a-f]{16}\.\.\. \(this site's\)/.test(about) && /Inter Tight/.test(about) && nLic === cur.info.lic && /MPL-2\.0 \(FIZZ\)/.test(about),
     `About: the licence expression, FIZZ's source and FM1VST's glue at their commits, the file, the credits, ${nLic} licence texts`);
  await shot("fizz-about");
  await esc(); await esc();

  // 5. the autosave: FIZZ's, under engine "rfm1" and its version
  await js(`(dispatchEvent(new Event("pagehide")), true)`);
  await until(`(${STORED}).then((s) => s.saves.some((x) => x[0] === "fizz" && x[1] === "rfm1"))`, 8000, "an rfm1 autosave of FIZZ");
  let stored = await js(STORED);
  const auto = stored.saves.find((x) => x[0] === "fizz" && x[1] === "rfm1");
  ok(auto && auto[2] === cur.info.version && auto[3] === "Autosave" && auto[4] === 1048576 && stored.lib.length === 0,
     `IndexedDB: its Autosave under engine "rfm1" (${auto && auto.slice(0, 4).join("/")}), nothing in Your firmware`);

  // 6. the firmware browser: its card has the native badge, and the Native filter lists it
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open") && document.querySelector('.cards .card[data-id="fizz"]')`, 5000, "FIZZ's card");
  ok(await js(`!!document.querySelector('.cards .card[data-id="fizz"] .card-badge') && document.querySelector('.cards .card[data-id="fizz"]').classList.contains("current")`), "its card: the native badge, current");
  await js(`(document.querySelector('.engine-filter button[data-engine="native"]').click(), true)`);
  await sleep(300);
  ok(await js(`!!document.querySelector('.cards .card[data-id="fizz"]')`), "the Native filter lists FIZZ");
  await js(`(document.querySelector('.engine-filter button[data-engine="emu"]').click(), true)`);
  await sleep(300);
  ok(await js(`!document.querySelector('.cards .card[data-id="fizz"]')`), "the Emulated filter does not (the Hub's FIZZ package needs its data blob: not emulated)");
  await js(`(document.querySelector('.engine-filter button[data-engine="all"]').click(), true)`);
  await sleep(200);

  // 7. phone width
  await esc(); await esc();
  await until(`!document.querySelector(".sheet.open")`, 5000, "the sheet closed");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(800);
  ok(await js(`document.documentElement.scrollWidth <= innerWidth + 1`), "no horizontal page scroll at 390 px");
  await shot("fizz-player-phone");
  await cdp("Emulation.clearDeviceMetricsOverride", {});
  await sleep(300);

  // 8. the same file opened by the visitor: Your firmware, apart from the catalog's FIZZ
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector('input[type=file][accept=".rfm1,.fwsc"]')`, 5000, "the Open a file... input");
  {
    const { root } = (await cdp("DOM.getDocument", { depth: 1 })).result;
    const { nodeId } = (await cdp("DOM.querySelector", { nodeId: root.nodeId, selector: 'input[type=file][accept=".rfm1,.fwsc"]' })).result;
    await cdp("DOM.setFileInputFiles", { nodeId, files: [FILE] });
  }
  await until(`window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "rfm1" && FM1Site.current().entry.local`, 30000, "the visitor's FIZZ file");
  stored = await js(STORED);
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector(".cards.mine .card[data-sha]")`, 5000, "Your firmware");
  const sep = await js(`({ mine: Array.from(document.querySelectorAll(".cards.mine .card[data-sha]"), (c) => c.classList.contains("current")),
    cat: !!document.querySelector('.cards:not(.mine) .card[data-id="fizz"]'), catCurrent: !!document.querySelector('.cards:not(.mine) .card[data-id="fizz"].current') })`);
  ok(stored.lib.length === 1 && stored.lib[0].id === "fizz" && stored.lib[0].sha === SITE_RFM1.sha256 && sep.mine.length === 1 && sep.mine[0] && sep.cat && !sep.catCurrent,
     "the visitor's own FIZZ file is in Your firmware (current), the catalog's FIZZ apart from it (not current)");
  ok(await js(`!new URL(location.href).searchParams.has("fw")`), "the visitor's file has no ?fw= deep link");
  await esc();

  // 9. a file that is not the catalog's (its SHA-256 differs): refused, with the reason
  await cdp("Fetch.enable", { patterns: [{ urlPattern: "*/fw/fizz-*.rfm1" }] });
  handlers.push((d) => {
    if (d.method !== "Fetch.requestPaused") return;
    const b = readFileSync(FILE); b[b.length - 1] ^= 1;
    cdp("Fetch.fulfillRequest", { requestId: d.params.requestId, responseCode: 200, responseHeaders: [{ name: "Content-Type", value: "application/wasm" }], body: b.toString("base64") });
  });
  await cdp("Page.navigate", { url: `${base}?fw=fizz` });
  await until(`/SHA-256 differs/.test(document.getElementById("toast").textContent)`, 30000, "the refusal");
  const why = await js(`document.getElementById("toast").textContent`);
  ok(/The \.rfm1 file of FIZZ is not the one the catalog lists \(its SHA-256 differs\)/.test(why) && await js(`FM1Site.current().engine === "mock"`), `a changed file is refused: "${why}"`);
  await cdp("Fetch.disable");
  ok(!exceptions.length, `no exceptions in the page${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
} catch (e) {
  console.error(e.stack || e);
  fails++;
} finally {
  stop();
}
console.log(fails ? `fizz e2e: ${fails} of ${checks} FAILED` : `fizz e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
