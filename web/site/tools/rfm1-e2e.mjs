// SPDX-License-Identifier: GPL-3.0-only
// The visitor's .rfm1 files in the player, end to end: this tree's site (the dev layout, served from the repo root) in
// headless Chromium (no window, a fresh profile, the browser and the server ended by their PIDs; a real AudioContext
// and AudioWorklet, muted). An .rfm1 opened through the browser's "Open a file..." input boots on the rfm1 engine
// (the engine's info, the manifest's licence and source in the About page), draws and sounds with a key held; its
// autosave is the firmware's under engine "rfm1"; after a reload it is still in Your firmware and opens again; a
// second file dropped on the page joins it; a damaged file is refused with the engine's reason; one is removed.
//   node web/site/tools/rfm1-e2e.mjs [--dir=web/build/rfm1] [--shots=web/build/test]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, readdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const DIR = arg("dir", join(ROOT, "web/build/rfm1")), SHOTS = arg("shots", join(ROOT, "web/build/test"));
const pick = (id) => { const f = readdirSync(DIR).find((x) => x.startsWith(`${id}-`) && x.endsWith(".rfm1")); if (!f) throw new Error(`no ${id}-*.rfm1 in ${DIR}`); return f; };
const FIRST = pick("choralroot"), SECOND = pick("felucca");
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const tmp = mkdtempSync(join(tmpdir(), "rfm1-e2e-")), profile = join(tmp, "profile");
// a damaged copy (truncated) for the refusal
const damaged = join(tmp, "damaged.rfm1");
{ const b = readFileSync(join(DIR, FIRST)); writeFileSync(damaged, b.subarray(0, Math.floor(b.length / 2))); }
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
  console.log("  sheet:", JSON.stringify(await js(`({ cls: document.querySelector(".sheet")?.className, page: document.querySelector(".sheet")?.dataset.page, kids: document.querySelector(".sheet-body")?.childElementCount, active: document.activeElement?.id || document.activeElement?.tagName })`)));
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
const key = (type) => cdp("Input.dispatchKeyEvent", { type, code: "KeyA", key: "a", windowsVirtualKeyCode: 65 });
// a file into the firmware browser's "Open a file..." input (the sheet open on its firmware page)
async function openThroughInput(file) {
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector('input[type=file][accept=".rfm1,.fwsc"]')`, 5000, "the Open a file... input");
  const { root } = (await cdp("DOM.getDocument", { depth: 1 })).result;
  const { nodeId } = (await cdp("DOM.querySelector", { nodeId: root.nodeId, selector: 'input[type=file][accept=".rfm1,.fwsc"]' })).result;
  const r = await cdp("DOM.setFileInputFiles", { nodeId, files: [file] });
  if (r.error) throw new Error(`setFileInputFiles: ${r.error.message}`);
}
const running = (fw) => `window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "rfm1" && FM1Site.current().entry.rfm1 && FM1Site.current().info.id === "${fw}"`;
// the worklet's frames, and its output's level (an AnalyserNode on the worklet node)
const WATCH = `(() => { const p = FM1Rec.o.getPlayer(), W = window.__w = { frames: 0, colours: 0, peaks: [], errors: [] };
  const ff = p.ev.frame; p.ev.frame = (px, seq) => { W.frames++; const s = new Set(); for (let i = 0; i < px.length; i += 7) { s.add(px[i]); if (s.size > 64) break; } W.colours = Math.max(W.colours, s.size); if (ff) ff(px, seq); };
  const fe = p.ev.error; p.ev.error = (m) => { W.errors.push(String(m)); if (fe) fe(m); };
  const an = p.ctx.createAnalyser(); an.fftSize = 2048; p.node.connect(an);
  const buf = new Float32Array(an.fftSize);
  W.timer = setInterval(() => { an.getFloatTimeDomainData(buf); let m = 0; for (const v of buf) m = Math.max(m, Math.abs(v)); W.peaks.push({ t: performance.now(), peak: m }); }, 40);
  return true; })()`;
// what IndexedDB holds: the library's records and the saves' keys
const STORED = `(async () => {
  const open = (n) => new Promise((res, rej) => { const r = indexedDB.open(n); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  const all = (d, s) => new Promise((res, rej) => { const r = d.transaction(s, "readonly").objectStore(s).getAll(); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  const lib = await all(await open("remotefm1-firmware"), "rfm1"), sv = await all(await open("remotefm1-player"), "saves");
  return { lib: lib.map((r) => ({ id: r.manifest.id, version: r.manifest.version, sha: r.sha256, file: r.fileName })), saves: sv.map((s) => [s.fw, s.engine, s.version, s.name, s.bytes.length]) };
})()`;

try {
  let served = false;
  for (let i = 0; i < 50 && !served; i++) { await sleep(200); try { served = "rfm1" in (await (await fetch(`http://127.0.0.1:${PORT}/web/site/catalog.json`)).json()).paths.engines; } catch {} }
  if (!served) throw new Error(`no catalog.json with paths.engines.rfm1 at http://127.0.0.1:${PORT}/web/site/`);
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
  await cdp("Runtime.enable"); await cdp("Page.enable"); await cdp("DOM.enable");
  const base = `http://127.0.0.1:${PORT}/web/site/`;
  await cdp("Page.navigate", { url: `${base}?fw=felucca&view=screen` });
  await until(`window.FM1Rec && window.FM1Site && FM1Site.current().info`, 60000, "the page running its first firmware");

  // 1. an .rfm1 through "Open a file..."
  const CLEAN = `Array.from(document.querySelector(".sheet-body").childNodes).every((n) => n.nodeType === 1)`;
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open") && document.querySelector(".get-rfm1")`, 5000, "the firmware browser");
  ok(await js(CLEAN) && !(await js(`!!document.querySelector(".mine-head")`)), "the firmware browser with no .rfm1 kept: no Your firmware group, no stray text");
  await openThroughInput(join(DIR, FIRST));
  await until(running("choralroot"), 30000, "choralroot on the rfm1 engine");
  const cur = await js(`(() => { const c = FM1Site.current(); return { entry: { id: c.entry.id, version: c.entry.version, license: c.entry.license, source: c.entry.source, local: c.entry.local },
    info: { id: c.info.id, name: c.info.name, version: c.info.version, license: c.info.license, source: c.info.source, authors: c.info.authors, credits: !!c.info.credits, lic: (c.info.licenseTexts || []).length, engine: c.info.engine } }; })()`);
  const toastText = await js(`document.getElementById("toast").textContent`);
  ok(cur.info.engine === "rfm1" && cur.info.name === "ChoralRoot" && cur.entry.version === cur.info.version && cur.info.license === "GPL-3.0-only" && /github\.com\/Quixotic7\/ChoralRootFM1@[0-9a-f]{40}$/.test(cur.info.source) && cur.info.lic > 0,
     `the .rfm1 boots on the rfm1 engine: ${cur.info.name} ${cur.info.version}, ${cur.info.license}, ${cur.info.lic} licence texts, by ${cur.info.authors.join(", ")}`);
  ok(/is in Your firmware/.test(toastText), `the page says so: "${toastText}"`);
  ok(await js(`!new URL(location.href).searchParams.has("fw")`), "no ?fw= deep link for it (a reload reopens it from the prefs)");
  await js(WATCH);
  await sleep(1500);
  let w = await js("window.__w");
  ok(w.frames > 0 && w.colours >= 2, `it draws (${w.frames} frames in 1.5 s, ${w.colours >= 64 ? "64+" : w.colours} colours)`);
  const t0 = await js("performance.now()");
  await key("keyDown"); await sleep(1500); await key("keyUp"); await sleep(300);
  w = await js("window.__w");
  const held = w.peaks.filter((p) => p.t > t0 + 200 && p.t < t0 + 1500).map((p) => p.peak || 0);
  await js("(clearInterval(window.__w.timer), true)");
  ok(held.length > 0 && Math.max(...held) > 0.005 && !w.errors.length, `it sounds with a key held (peak ${held.length ? Math.max(...held).toFixed(3) : "none"}${w.errors.length ? "; errors " + w.errors.join(" | ") : ""})`);
  await shot("rfm1-page-choralroot");
  // the firmware's own button names (the descriptor's button_names through rfm1_layout): ChoralRoot's SEL is KEY, ARP PERF
  const names = await js("window.FM1Omx.views.names.join(' ')");
  ok(names.split(" ").length === 14 && names !== "FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+", `the panel shows the firmware's button names (${names})`);
  // the header's source link and the About page
  const src = await js(`(() => { const a = document.getElementById("fw-src"); return { href: a.href, title: a.title, off: a.classList.contains("off") }; })()`);
  ok(!src.off && /^https:\/\/github\.com\/Quixotic7\/ChoralRootFM1\/tree\/[0-9a-f]{40}$/.test(src.href) && /GPL-3\.0-only/.test(src.title), `the header's Source link: ${src.href}`);
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await sleep(500);
  const mainText = await js(`document.querySelector(".sheet-body").innerText`);
  ok(/native, your file/.test(mainText) && /Native \(\.rfm1\)/.test(mainText), "the sheet's firmware card and Engine section: native, from your .rfm1 file");
  await js(`(document.querySelector(".fw-card.slim").click(), true)`);
  await sleep(500);
  const about = await js(`document.querySelector(".sheet-body").innerText`);
  ok(/GPL-3\.0-only/.test(about) && /Quixotic7\/ChoralRootFM1 @ [0-9a-f]{7}/.test(about) && /choralroot-[\d.]+\.rfm1/.test(about) && /SHA-256/.test(about) && /GPL-3\.0-only \(ChoralRoot\)/.test(about) && await js(`document.querySelectorAll(".sheet-body details.lic").length`) === cur.info.lic,
     "About: the licence, the source at its commit, the file and its SHA-256, the licence texts");
  await shot("rfm1-page-about");
  await cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
  await cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });

  // 2. the autosave: under engine "rfm1"
  await js(`(dispatchEvent(new Event("pagehide")), true)`);
  await until(`(${STORED}).then((s) => s.saves.some((x) => x[0] === "choralroot" && x[1] === "rfm1"))`, 8000, "an rfm1 autosave");
  let stored = await js(STORED);
  ok(stored.lib.length === 1 && stored.lib[0].id === "choralroot" && /^[0-9a-f]{64}$/.test(stored.lib[0].sha)
     && stored.saves.some((x) => x[0] === "choralroot" && x[1] === "rfm1" && x[3] === "Autosave" && x[4] === 1048576),
     `IndexedDB: the file by its SHA-256 (${stored.lib[0] && stored.lib[0].sha.slice(0, 12)}...), and its Autosave under engine "rfm1" (${stored.saves.map((x) => x.slice(0, 4).join("/")).join(", ")})`);

  // 3. a reload: still listed, and it opens again (the last one played)
  await js("(window.__before = true)"); await cdp("Page.reload", {});
  await until(`!window.__before && ${running("choralroot")}`, 60000, "choralroot again after the reload");
  ok(true, "after a reload the page opens the same .rfm1 again (from this browser)");
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open") && document.querySelector(".get-rfm1")`, 5000, "the firmware browser");
  const list = await js(`Array.from(document.querySelectorAll(".cards.mine .card[data-sha]"), (c) => c.innerText.replace(/\\s+/g, " "))`);
  ok(list.length === 1 && /ChoralRoot/.test(list[0]) && await js(`/Your firmware/i.test(document.querySelector(".mine-head")?.innerText || "")`),
     `the firmware browser lists it under Your firmware: ${JSON.stringify(list)}`);
  ok(await js(`!!document.querySelector('.get-rfm1 a[href="https://github.com/Quixotic7/RemoteFM1FW/releases/latest"]')`) && await js(CLEAN), "the browser links to the .rfm1 releases (Get .rfm1 firmwares), no stray text");

  // 4. a second file dropped on the page
  await js(`(async () => {
    const b = await (await fetch("../build/rfm1/${SECOND}")).arrayBuffer();
    const dt = new DataTransfer(); dt.items.add(new File([b], "${SECOND}"));
    dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true }));
    return true; })()`);
  await until(running("felucca"), 30000, "felucca (dropped) on the rfm1 engine");
  stored = await js(STORED);
  ok(stored.lib.length === 2 && stored.lib.map((r) => r.id).sort().join() === "choralroot,felucca", `a file dropped on the page opens and joins it (${stored.lib.map((r) => r.id + " " + r.version).join(", ")})`);
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await sleep(600);
  await shot("rfm1-page-browser");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(500);
  ok(await js(`document.documentElement.scrollWidth <= innerWidth + 1`), "no horizontal page scroll at 390 px with the browser open");
  await shot("rfm1-page-browser-phone");
  await cdp("Emulation.clearDeviceMetricsOverride", {});
  await sleep(300);

  // 5. a damaged file: refused, with the engine's reason; nothing added
  await cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
  await openThroughInput(damaged);
  await until(`/not a usable \\.rfm1 file/.test(document.getElementById("toast").textContent)`, 10000, "the refusal");
  const why = await js(`document.getElementById("toast").textContent`);
  stored = await js(STORED);
  ok(stored.lib.length === 2 && /damaged\.rfm1: not a usable \.rfm1 file: a section runs past the end/.test(why), `a damaged file is refused: "${why}"`);

  // 6. remove one (the inline confirmation), still gone after a reload
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await sleep(500);
  const sha = stored.lib.find((r) => r.id === "choralroot").sha;
  await js(`(document.querySelector('.cards.mine .card[data-sha="${sha}"]').parentElement.querySelector(".card-x").click(), true)`);
  await sleep(200);
  await js(`(Array.from(document.querySelectorAll(".mine-ask .pill")).find((b) => b.textContent === "Remove").click(), true)`);
  await until(`(${STORED}).then((s) => s.lib.length === 1)`, 5000, "the removal");
  await js("(window.__before = true)"); await cdp("Page.reload", {});
  await until(`!window.__before && ${running("felucca")}`, 60000, "felucca after the second reload");
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open") && document.querySelector(".get-rfm1")`, 5000, "the firmware browser");
  const left = await js(`Array.from(document.querySelectorAll(".cards.mine .card[data-sha]"), (c) => c.dataset.sha)`);
  stored = await js(STORED);
  ok(left.length === 1 && left[0] !== sha && stored.lib.length === 1 && stored.saves.some((x) => x[0] === "choralroot" && x[1] === "rfm1"),
     "one removed: gone from the browser after a reload; its saves stay");
  ok(!exceptions.length, `no exceptions in the page${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
} catch (e) {
  console.error(e.stack || e);
  fails++;
} finally {
  stop();
}
console.log(fails ? `rfm1 e2e: ${fails} of ${checks} FAILED` : `rfm1 e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
