// SPDX-License-Identifier: GPL-3.0-only
// Reset firmware... (the sheet's Saves section), end to end: this tree's site (the dev layout, served from the repo
// root) in headless Chromium (a fresh profile, the browser and the server ended by their PIDs). Felucca on its
// native engine: a named save made, then Reset firmware... asked inline; Cancel changes nothing; "Save a copy and
// reset" keeps the current memory as a "Before reset ..." save and restarts the firmware with no flash at all (as
// newly installed); "Reset" does the same with no copy. The named saves stay byte for byte; a reload after a reset
// does not bring the old memory back.
//   node web/site/tools/reset-e2e.mjs [--fw=felucca] [--engine=native|emu|...] [--shots=web/build/test]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const FW = arg("fw", "felucca"), ENGINE = arg("engine", ""), SHOTS = arg("shots", join(ROOT, "web/build/test"));
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const tmp = mkdtempSync(join(tmpdir(), "reset-e2e-")), profile = join(tmp, "profile");
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,900",
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
  while (Date.now() - t0 < ms) { if (await js(`(async () => !!(await (${expr})))()`)) return true; await sleep(100); }
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
const running = `window.FM1Site && FM1Site.current().info && FM1Site.current().entry && FM1Site.current().entry.id === "${FW}" && FM1Site.current().engine !== "mock"`;
// the sheet's button with this text (exact), clicked
const click = (text) => js(`(() => { const b = Array.from(document.querySelectorAll(".sheet-body button")).find((x) => x.textContent.trim() === ${JSON.stringify(text)});
  if (!b) return "none"; if (b.disabled) return "disabled"; b.click(); return "ok"; })()`);
const hasButton = (text) => js(`Array.from(document.querySelectorAll(".sheet-body button")).some((x) => x.textContent.trim() === ${JSON.stringify(text)})`);
// every save of the firmware: name -> a hash of its bytes
const SAVES = `(async () => {
  const d = await new Promise((res, rej) => { const r = indexedDB.open("remotefm1-player"); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  const all = await new Promise((res, rej) => { const r = d.transaction("saves", "readonly").objectStore("saves").getAll(); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  d.close();
  const out = {};
  for (const s of all.filter((x) => x.fw === "${FW}")) { let h = 2166136261; for (const b of s.bytes) h = Math.imul(h ^ b, 16777619); out[s.name] = (h >>> 0).toString(16) + ":" + s.bytes.length; }
  return out;
})()`;
// the player's loads: what flash each boot was given (null: none, as newly installed)
const HOOK = `(() => { const p = FM1Rec.o.getPlayer(); if (p.__hooked) return true; p.__hooked = true; window.__loads = [];
  const l = p.load.bind(p); p.load = (e, eng, flash, o) => { window.__loads.push(flash ? flash.length : null); return l(e, eng, flash, o); }; return true; })()`;
const openSaves = async () => {
  await js(`(document.querySelector(".sheet.open") || document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open") && Array.from(document.querySelectorAll(".sheet-body button")).some((x) => /^Reset firmware/.test(x.textContent.trim()))`, 8000, "the sheet's Reset firmware... button");
};

try {
  let served = false;
  for (let i = 0; i < 50 && !served; i++) { await sleep(200); try { served = (await fetch(`http://127.0.0.1:${PORT}/web/site/catalog.json`)).ok; } catch {} }
  if (!served) throw new Error(`no catalog.json at http://127.0.0.1:${PORT}/web/site/`);
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
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=${FW}${ENGINE ? `&engine=${ENGINE}` : ""}&view=screen` });
  await until(running, 60000, `${FW} running on a real engine`);
  const engine = await js("FM1Site.current().engine");
  ok(true, `${FW} runs on its ${engine} engine`);

  // the autosave, and a named save of it (fm1-emu keeps none while the flash is still the package's own)
  await js(`(dispatchEvent(new Event("pagehide")), true)`);
  let hasAuto = true;
  try { await until(`(${SAVES}).then((s) => "Autosave" in s)`, 8000, "the autosave"); }
  catch (_) { hasAuto = false; console.log(`  (no autosave on ${engine}: the flash is still the firmware's own)`); }
  await openSaves();
  if (hasAuto) {
    ok(await click("Save as") === "ok", "Save as");
    await sleep(200);
    await js(`(() => { const i = document.querySelector(".sheet-body .inline input"); i.value = "Mine"; i.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" })); return true; })()`);
    await until(`(${SAVES}).then((s) => "Mine" in s)`, 8000, "the named save");
  }
  const before = await js(SAVES);
  ok(Object.keys(before).sort().join() === (hasAuto ? "Autosave,Mine" : ""), `saves before: ${JSON.stringify(before)}`);

  // the question, Cancel
  await js(HOOK);
  ok(await click("Reset firmware...") === "ok", "Reset firmware... asks");
  await sleep(200);
  const ask = await js(`document.querySelector(".sheet-body .ask")?.innerText.replace(/\\s+/g, " ")`);
  ok(/Start .+ with a clean memory, as if newly installed\? Your saves stay as they are\./.test(ask || "") && await hasButton("Save a copy and reset") && await hasButton("Reset") && await hasButton("Cancel"),
     `the question: "${ask}"`);
  await js(`document.querySelector(".sheet-body .ask").scrollIntoView({ block: "center" }), true`);
  await shot("reset-ask");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(400);
  await js(`document.querySelector(".sheet-body .ask").scrollIntoView({ block: "center" }), true`);
  ok(await js(`document.documentElement.scrollWidth <= innerWidth + 1`), "no horizontal page scroll at 390 px with the question open");
  await shot("reset-ask-phone");
  await cdp("Emulation.clearDeviceMetricsOverride", {});
  await sleep(300);
  ok(await click("Cancel") === "ok", "Cancel");
  await sleep(300);
  ok(!(await js(`!!document.querySelector(".sheet-body .ask")`)) && (await js("__loads")).length === 0 && JSON.stringify(await js(SAVES)) === JSON.stringify(before),
     "Cancel: no reset, no restart, the saves as they were");

  // Save a copy and reset
  await click("Reset firmware...");
  await sleep(200);
  ok(await click("Save a copy and reset") === "ok", "Save a copy and reset");
  await until(`__loads.length >= 1 && ${running}`, 30000, "the firmware restarted");
  await sleep(500);
  let loads = await js("__loads"), after = await js(SAVES);
  const copy = Object.keys(after).find((n) => /^Before reset \d{4}-\d\d-\d\d \d\d:\d\d/.test(n));
  ok(loads.length === 1 && loads[0] === null, `it restarts with no flash at all, as newly installed (loads: ${JSON.stringify(loads)})`);
  if (hasAuto) ok(!!copy && after[copy].split(":")[1] === before.Autosave.split(":")[1], `the current memory is kept as "${copy}" (${copy && after[copy]})`);
  else ok(!copy, "nothing to keep (the flash was the firmware's own): no copy");
  ok(after.Mine === before.Mine, "the named save stays byte for byte");
  const toast1 = await js(`document.getElementById("toast").textContent`);
  ok(hasAuto ? /started with a clean memory\. The old one is saved as "Before reset/.test(toast1) : /started with a clean memory\.$/.test(toast1), `the page says so: "${toast1}"`);
  await shot("reset-done");

  // Reset (no copy)
  await js(`(dispatchEvent(new Event("pagehide")), true)`);
  await sleep(1500);
  const mid = await js(SAVES);
  await openSaves();
  await click("Reset firmware...");
  await sleep(200);
  ok(await click("Reset") === "ok", "Reset");
  await until(`__loads.length >= 2 && ${running}`, 30000, "the firmware restarted again");
  await sleep(500);
  loads = await js("__loads"); after = await js(SAVES);
  const named = (s) => Object.keys(s).filter((n) => n !== "Autosave").sort().join();
  ok(loads[1] === null && named(after) === named(mid), `Reset: no flash, no new save (${named(after)})`);
  ok(after.Mine === before.Mine && (!copy || after[copy] === mid[copy]), "the named saves stay byte for byte");

  // a reload: the firmware comes back from its own (fresh) autosave or none, never the old memory
  const autoNow = after.Autosave || null;
  await js("(window.__before = true)"); await cdp("Page.reload", {});
  await until(`!window.__before && ${running}`, 60000, `${FW} again after the reload`);
  ok(true, `after a reload ${FW} runs (autosave ${autoNow ? "the fresh one" : "none yet"})`);
  ok(!exceptions.length, `no page exceptions (${exceptions.length})`);
} catch (err) {
  fails++;
  console.error(err.stack || String(err));
}
stop();
console.log(fails ? `reset e2e FAILED (${fails} of ${checks})` : `reset e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
