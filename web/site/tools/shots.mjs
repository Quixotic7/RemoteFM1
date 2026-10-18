// SPDX-License-Identifier: GPL-3.0-only
// Screenshots of the site in headless Chromium, driven over the DevTools protocol (no window, a fresh profile, the
// browser ended by its own PID). Also a smoke test: console errors and page exceptions are printed, and the run fails
// on an exception.
//   node web/site/tools/shots.mjs [BASE_URL] [OUT_DIR]
//   BASE_URL  the served web/ folder (default http://127.0.0.1:8731/), e.g. `python3 -m http.server 8731` in web/
//   OUT_DIR   default web/build/shots
//   PAGES=1   BASE is the published layout (the player at the root, not site/); ONLY=<regex> picks shots
// Each shot: a viewport, a URL, then steps run in the page (open the sheet, the browser, ...).
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const BASE = process.argv[2] || "http://127.0.0.1:8731/";
const OUT = process.argv[3] || join(HERE, "..", "..", "build", "shots");
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const ONLY = process.env.ONLY ? new RegExp(process.env.ONLY) : null;
mkdirSync(OUT, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const SHOTS = [
  { name: "device", w: 1400, h: 860, url: "site/?fw=felucca", wait: 2500 },
  { name: "device-pressed", w: 1400, h: 860, url: "site/?fw=fomni", wait: 2000,
    steps: [`(() => { const k = (c, t) => dispatchEvent(new KeyboardEvent(t, { code: c, key: c, bubbles: true }));
      for (const c of ["KeyA", "KeyD", "KeyG", "F5", "Minus"]) k(c, "keydown"); })()`], after: 800 },
  { name: "device-corner", w: 1400, h: 860, url: "site/?fw=felucca", wait: 2000,
    steps: [`document.getElementById("stage").dispatchEvent(new PointerEvent("pointermove", { bubbles: true, clientX: 700, clientY: 400 }))`], after: 400 },
  { name: "sheet", w: 1400, h: 860, url: "site/?fw=felucca", wait: 2000, steps: [`document.getElementById("corner").click()`], after: 600 },
  { name: "sheet-scrolled", w: 1400, h: 860, url: "site/?fw=felucca", wait: 2000,
    steps: [`document.getElementById("corner").click()`, `document.querySelector(".sheet-body").scrollTop = 99999`], after: 600 },
  { name: "firmware-browser", w: 1400, h: 860, url: "site/?fw=felucca", wait: 2000, steps: [`document.getElementById("fw-btn").click()`], after: 600 },
  { name: "firmware-search", w: 1400, h: 860, url: "site/?fw=felucca", wait: 2000,
    steps: [`document.getElementById("fw-btn").click()`, `(() => { const s = document.querySelector(".search input"); s.value = "chord"; s.dispatchEvent(new Event("input")); })()`], after: 600 },
  { name: "about", w: 1400, h: 860, url: "site/?fw=jiant", wait: 2000,
    steps: [`document.getElementById("corner").click()`, `[...document.querySelectorAll(".fw-card")].at(-1).click()`], after: 600 },
  { name: "screen", w: 1000, h: 860, url: "site/?fw=felucca&view=screen", wait: 2500 },
  { name: "embed", w: 900, h: 560, url: "site/?fw=obervave&embed=1", wait: 2500 },
  { name: "theme-orange", w: 1400, h: 860, url: "site/?fw=coralflower", wait: 2000,
    steps: [`document.getElementById("corner").click()`, `[...document.querySelectorAll(".swatch")].find((b) => b.title === "Orange").click()`,
            `document.querySelector(".scrim").click()`], after: 600 },
  { name: "emulated", w: 1400, h: 860, url: "site/?fw=choralroot", wait: 3500 },
  { name: "mobile", w: 390, h: 844, mobile: true, url: "site/?fw=felucca", wait: 2500 },
  { name: "mobile-sheet", w: 390, h: 844, mobile: true, url: "site/?fw=felucca", wait: 2000, steps: [`document.getElementById("menu-btn").click()`], after: 600 },
  { name: "mobile-landscape", w: 844, h: 390, mobile: true, url: "site/?fw=felucca", wait: 2500 },
  { name: "nostart", w: 1400, h: 860, url: "site/?fw=felucca", wait: 1500, noAutoplay: true },
  { name: "installer", w: 1200, h: 1500, url: "installer/", wait: 1500 },
  { name: "installer-mobile", w: 390, h: 844, mobile: true, url: "installer/", wait: 1500 },
];

class Cdp {
  constructor(ws) { this.ws = ws; this.id = 0; this.pending = new Map(); this.handlers = []; }
  static async connect(url) {
    const ws = new WebSocket(url);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    const c = new Cdp(ws);
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id && c.pending.has(m.id)) { const { res, rej } = c.pending.get(m.id); c.pending.delete(m.id); m.error ? rej(new Error(m.error.message)) : res(m.result); }
      else for (const h of c.handlers) h(m);
    };
    return c;
  }
  send(method, params = {}, sessionId) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    return new Promise((res, rej) => this.pending.set(id, { res, rej }));
  }
}

async function launch(noAutoplay) {
  const prof = mkdtempSync(join(tmpdir(), "shots-"));
  const args = ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu", "--no-sandbox", "--no-first-run",
    `--user-data-dir=${prof}`, "--hide-scrollbars", "--remote-debugging-port=0", "--mute-audio",
    ...(noAutoplay ? [] : ["--autoplay-policy=no-user-gesture-required"]), "about:blank"];
  const p = spawn(CHROMIUM, args, { stdio: ["ignore", "ignore", "pipe"] });
  const wsUrl = await new Promise((res, rej) => {
    let buf = "";
    const t = setTimeout(() => rej(new Error("Chromium did not start")), 15000);
    p.stderr.on("data", (d) => { buf += d; const m = /DevTools listening on (ws:\S+)/.exec(buf); if (m) { clearTimeout(t); res(m[1]); } });
  });
  return { p, wsUrl };
}

let failed = 0;
async function shoot(s) {
  const { p, wsUrl } = await launch(s.noAutoplay);
  try {
    const cdp = await Cdp.connect(wsUrl);
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const S = (m, a) => cdp.send(m, a, sessionId);
    const logs = [];
    cdp.handlers.push((m) => {
      if (m.sessionId !== sessionId) return;
      if (m.method === "Runtime.consoleAPICalled" && /error|warning/.test(m.params.type)) logs.push(`console.${m.params.type}: ${m.params.args.map((a) => a.value ?? a.description).join(" ")}`);
      if (m.method === "Runtime.exceptionThrown") { logs.push("EXCEPTION: " + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text)); failed++; }
      if (m.method === "Log.entryAdded" && m.params.entry.level === "error") logs.push(`log: ${m.params.entry.text} ${m.params.entry.url || ""}`);
    });
    await S("Runtime.enable"); await S("Log.enable"); await S("Page.enable");
    await S("Emulation.setDeviceMetricsOverride", { width: s.w, height: s.h, deviceScaleFactor: s.dpr || (s.mobile ? 2 : 1), mobile: !!s.mobile });
    if (s.mobile) await S("Emulation.setTouchEmulationEnabled", { enabled: true, maxTouchPoints: 5 });
    await S("Page.navigate", { url: new URL(process.env.PAGES ? s.url.replace(/^site\//, "") : s.url, BASE).href });
    await sleep(s.wait || 2000);
    for (const js of s.steps || []) {
      const r = await S("Runtime.evaluate", { expression: js, awaitPromise: true, userGesture: true });
      if (r.exceptionDetails) logs.push("step failed: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
      await sleep(250);
    }
    await sleep(s.after || 0);
    const shot = await S("Page.captureScreenshot", { format: "png" });
    const file = join(OUT, `${s.name}.png`);
    writeFileSync(file, Buffer.from(shot.data, "base64"));
    console.log(`${s.name.padEnd(18)} ${file}`);
    for (const l of logs) console.log("    " + l);
    cdp.ws.close();
  } finally {
    p.kill("SIGTERM");
    await new Promise((r) => { p.once("exit", r); setTimeout(r, 3000); });
  }
}

for (const s of SHOTS) if (!ONLY || ONLY.test(s.name)) {
  try { await shoot(s); } catch (e) { failed++; console.log(`${s.name}: ${e.message}`); }
}
process.exit(failed ? 1 : 0);
