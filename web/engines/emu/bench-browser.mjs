// The emu engine's speed in a real browser: headless Chromium, a real AudioContext and AudioWorklet (muted), the
// site's own worklet host (bench-browser.html). For each package and clock: the worklet's render time per 128-frame
// quantum (mean load, p99, slowest), late audio, the engine's underruns and how often LED levels flip.
//   node web/engines/emu/bench-browser.mjs [--pkgs=melodee-1.4.4,x0x-1.1.0] [--mhz=240,320,360] [--secs=10]
//       [--opts='{"jitPerRender":16}'] [--wasm=emu/fm1_web.wasm] [--out=emu-bench] [--native=choralroot,melodee]
// --native: also each named core on the native engine (web/build/native/<core>.wasm: web/engines/native/build.sh), once
// (no clock), the same steps and measures; --pkgs= (empty) for the native ones alone.
// Packages are read from web/build/packages/<name>.fwsc (test.mjs downloads the Hub's there). Serves the worktree
// with python3 -m http.server on 127.0.0.1, drives Chromium over the DevTools protocol, stops both by PID. Writes
// web/build/test/<out>.json.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const BUILD = new URL("../../build/", import.meta.url);
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const PKGS = arg("pkgs", "melodee-1.4.4,x0x-1.1.0,felucca-1.6.1,choralroot-0.25.1").split(",").filter(Boolean);
const NATIVE = arg("native", "").split(",").filter(Boolean);
const MHZ = arg("mhz", "240,320,360").split(",").map(Number);
const SECS = +arg("secs", 10), OPTS = arg("opts", "{}"), WASM = arg("wasm", "emu/fm1_web.wasm"), OUT = arg("out", "emu-bench");
const ENGINE = arg("engine", "engine.js");
const CHROMIUM = "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

mkdirSync(new URL("test/", BUILD), { recursive: true });
const profile = mkdtempSync(fileURLToPath(new URL("chromium-profile-", BUILD)));
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=600,700",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", "--enable-blink-features=AudioContextPlayoutStats", `--remote-debugging-port=${DEBUG}`, "about:blank"],
{ stdio: "ignore" });
const stop = () => { try { chrome.kill(); } catch {} try { server.kill(); } catch {} };
process.on("exit", stop);

const results = [];
try {
  let targets;
  for (let i = 0; i < 50 && !targets; i++) {
    await sleep(200);
    try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {}
  }
  const page = targets.find((t) => t.type === "page");
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let id = 0;
  const waiting = new Map();
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") console.error("exception:", JSON.stringify(d.params.exceptionDetails).slice(0, 400));
  };
  const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  await cdp("Runtime.enable");
  await cdp("Page.enable");
  const runs = [...PKGS.flatMap((pkg) => MHZ.map((mhz) => ({ pkg, mhz }))), ...NATIVE.map((core) => ({ pkg: core, core, mhz: 0 }))];
  for (const { pkg, mhz, core } of runs) {
    {
      const qs = new URLSearchParams(core ? { core, secs: SECS } : { pkg: `${pkg}.fwsc`, mhz, secs: SECS, opts: OPTS, wasm: WASM, engine: ENGINE });
      await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/engines/emu/bench-browser.html?${qs}` });
      await sleep(1000);
      let r;
      for (let i = 0; i < 200; i++) {
        await sleep(500);
        r = await cdp("Runtime.evaluate", { expression: "window.__result", awaitPromise: true, returnByValue: true, timeout: 120000 });
        if (r.result?.result?.value) break;
      }
      const v = r.result?.result?.value ?? { pkg, mhz, error: "no result" };
      results.push(v);
      console.log(JSON.stringify(v));
    }
  }
  ws.close();
} finally {
  stop();
}
writeFileSync(new URL(`test/${OUT}.json`, BUILD), JSON.stringify(results, null, 1));
console.log("\npackage              MHz | load mean max | peak | p99 ms (worst/median) | max ms | late ms | underruns | glitches (ms) | LED flips/s");
for (const r of results) {
  console.log(`${String(r.pkg).padEnd(20)} ${String(r.mhz).padStart(4)} | ${String(r.loadMean).padStart(5)} ${String(r.loadMax).padStart(5)} | ${String(r.peak).padStart(5)} | ` +
    `${String(r.p99Ms).padStart(6)} / ${String(r.p99MedMs).padStart(6)}       | ${String(r.maxMs).padStart(6)} | ${String(r.lateMs).padStart(7)} | ` +
    `${String(r.underruns).padStart(9)} | ${String(r.glitches).padStart(4)} (${r.glitchMs}) | ${r.ledFlipsPerS}${r.error ? "  ERROR " + r.error : ""}`);
}
