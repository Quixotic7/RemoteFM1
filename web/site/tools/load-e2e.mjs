// SPDX-License-Identifier: GPL-3.0-only
// The page's first load, on a slow connection (the owner's "rendered really big, then snaps to small"): in headless
// Chromium (no window, a fresh profile, the browser and the server ended by their PIDs), the network at Slow 4G and the
// CPU throttled x4, a probe in the page samples the FM-1's canvases every frame from the first script on. Once a canvas
// is first painted and visible, its CSS box must never change size again during the load, and its bitmap must always
// be its CSS box x devicePixelRatio (never the default 300 x 150 stretched over the stage).
//   node web/site/tools/load-e2e.mjs [--base=URL] [--frames=DIR] [--fast] [--only=REGEX]
//   --base    the player's URL (default: this tree's web/site, served here); e.g. --base=https://remotefm1.com/
//   --frames  also save every screencast frame of each case to DIR/<case>/<ms>.png (for a filmstrip)
//   --fast    no network or CPU throttling
// No autoplay flag: the page waits for a click, as in a visitor's browser.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const FRAMES = arg("frames", ""), FAST = process.argv.includes("--fast"), ONLY = arg("only", "") ? new RegExp(arg("only", "")) : null;
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const CASES = [
  { name: "mac-1280", w: 1280, h: 800, dpr: 2 },
  { name: "mac-1728", w: 1728, h: 1117, dpr: 2 },
  { name: "phone-390", w: 390, h: 844, dpr: 3, mobile: true },
  { name: "screen-1280", w: 1280, h: 800, dpr: 2, query: "?view=screen" },
  { name: "embed-900", w: 900, h: 420, dpr: 1, query: "?embed=1" },
];

// the probe: every frame, each canvas's CSS box, bitmap, whether it has been drawn on and whether it can be seen
const PROBE = `(() => {
  const S = window.__loadSamples = [], mark = function () { if (this.canvas) this.canvas.__drawn = true; };
  for (const m of ["drawImage", "fillRect", "putImageData"]) {
    const f = CanvasRenderingContext2D.prototype[m];
    CanvasRenderingContext2D.prototype[m] = function (...a) { mark.call(this); return f.apply(this, a); };
  }
  const one = (id) => {
    const c = document.getElementById(id);
    if (!c) return null;
    const r = c.getBoundingClientRect();
    const seen = r.width > 0 && r.height > 0 && c.checkVisibility({ checkOpacity: true, checkVisibilityCSS: true });
    return { w: +r.width.toFixed(2), h: +r.height.toFixed(2), bw: c.width, bh: c.height, drawn: !!c.__drawn, seen };
  };
  const tick = () => { S.push({ t: Math.round(performance.now()), dpr: devicePixelRatio, device: one("device"), screen: one("screen") }); requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
})();`;

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

async function launch() {
  const prof = mkdtempSync(join(tmpdir(), "load-e2e-"));
  const p = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu", "--no-sandbox",
    "--no-first-run", `--user-data-dir=${prof}`, "--hide-scrollbars", "--remote-debugging-port=0", "--mute-audio", "about:blank"],
  { stdio: ["ignore", "ignore", "pipe"] });
  const wsUrl = await new Promise((res, rej) => {
    let buf = "";
    const t = setTimeout(() => rej(new Error("Chromium did not start")), 15000);
    p.stderr.on("data", (d) => { buf += d; const m = /DevTools listening on (ws:\S+)/.exec(buf); if (m) { clearTimeout(t); res(m[1]); } });
  });
  return { p, wsUrl };
}

// the samples from a canvas's first visible paint on: [problems]
function judge(samples, key) {
  const out = [];
  const i0 = samples.findIndex((s) => s[key] && s[key].seen && s[key].drawn);
  if (i0 < 0) return { first: null, out };
  const a = samples[i0][key];
  for (const s of samples.slice(i0)) {
    const c = s[key];
    if (!c || !c.seen) continue;
    if (Math.abs(c.w - a.w) > 0.5 || Math.abs(c.h - a.h) > 0.5) out.push(`${s.t} ms: the CSS box ${a.w}x${a.h} -> ${c.w}x${c.h}`);
    if (Math.abs(c.bw - Math.round(c.w * s.dpr)) > 1 || Math.abs(c.bh - Math.round(c.h * s.dpr)) > 1) out.push(`${s.t} ms: the bitmap ${c.bw}x${c.bh} for a CSS box ${c.w}x${c.h} at dpr ${s.dpr}`);
  }
  return { first: samples[i0].t, out };
}

let fails = 0;
async function run(base, k) {
  const { p, wsUrl } = await launch();
  try {
    const cdp = await Cdp.connect(wsUrl);
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const S = (m, a) => cdp.send(m, a, sessionId);
    const frames = [];
    let t0 = 0;
    cdp.handlers.push((m) => {
      if (m.sessionId !== sessionId) return;
      if (m.method === "Page.screencastFrame") {
        S("Page.screencastFrameAck", { sessionId: m.params.sessionId }).catch(() => {});
        frames.push({ t: Math.round(m.params.metadata.timestamp * 1000), data: m.params.data });
      }
      if (m.method === "Runtime.exceptionThrown") { console.log("  EXCEPTION: " + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text)); fails++; }
    });
    await S("Runtime.enable"); await S("Page.enable"); await S("Network.enable");
    await S("Network.setCacheDisabled", { cacheDisabled: true });
    await S("Emulation.setDeviceMetricsOverride", { width: k.w, height: k.h, deviceScaleFactor: k.dpr, mobile: !!k.mobile });
    if (!FAST) {   // DevTools' "Slow 4G", and a CPU four times slower
      await S("Network.emulateNetworkConditions", { offline: false, latency: 562.5, downloadThroughput: 180000, uploadThroughput: 84375 });
      await S("Emulation.setCPUThrottlingRate", { rate: 4 });
    }
    await S("Page.addScriptToEvaluateOnNewDocument", { source: PROBE });
    if (FRAMES) await S("Page.startScreencast", { format: "png", everyNthFrame: 1, maxWidth: k.w, maxHeight: k.h });
    t0 = Date.now();
    await S("Page.navigate", { url: new URL(k.query || "", base).href });
    // until the page has settled (the start overlay up, or the player running), then a second more; at most 20 s
    const settled = `(() => { const s = document.getElementById("start"); return !!s && (!s.hidden || document.body.classList.contains("running")); })()`;
    while (Date.now() - t0 < 20000) {
      const r = await S("Runtime.evaluate", { expression: settled, returnByValue: true }).catch(() => null);
      if (r && r.result && r.result.value) break;
      await sleep(100);
    }
    await sleep(1000);
    if (FRAMES) await S("Page.stopScreencast");
    const { result } = await S("Runtime.evaluate", { expression: "JSON.stringify(window.__loadSamples || [])", returnByValue: true });
    const samples = JSON.parse(result.value);
    const dev = judge(samples, "device"), scr = judge(samples, "screen");
    const bad = [...dev.out.map((x) => "#device " + x), ...scr.out.map((x) => "#screen " + x)];
    const painted = dev.first ?? scr.first;
    const good = painted != null && bad.length === 0;
    if (!good) fails++;
    console.log(`${k.name.padEnd(12)} ${String(samples.length).padStart(4)} frames, first paint ${painted ?? "-"} ms (device ${dev.first ?? "-"}, screen ${scr.first ?? "-"})  ${good ? "ok" : "FAIL"}`);
    for (const b of bad.slice(0, 6)) console.log("    " + b);
    if (bad.length > 6) console.log(`    (${bad.length - 6} more)`);
    if (painted == null) console.log("    no canvas was painted and visible");
    if (FRAMES) {
      const dir = join(FRAMES, k.name);
      mkdirSync(dir, { recursive: true });
      const f0 = frames.length ? frames[0].t : 0;
      for (const f of frames) writeFileSync(join(dir, `${String(f.t - f0).padStart(6, "0")}.png`), Buffer.from(f.data, "base64"));
      writeFileSync(join(dir, "samples.json"), JSON.stringify(samples));
    }
    cdp.ws.close();
  } finally {
    p.kill("SIGTERM");
    await new Promise((r) => { p.once("exit", r); setTimeout(r, 3000); });
  }
}

let server = null, base = arg("base", "");
if (!base) {
  server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
  base = `http://127.0.0.1:${PORT}/web/site/`;
  for (let i = 0; i < 50; i++) { try { await fetch(base); break; } catch { await sleep(100); } }
}
try {
  console.log(`load: ${base}${FAST ? "" : " (Slow 4G, CPU x4)"}`);
  for (const k of CASES) if (!ONLY || ONLY.test(k.name)) {
    try { await run(base, k); } catch (e) { fails++; console.log(`${k.name}: ${e.message}`); }
  }
} finally { if (server) server.kill(); }
console.log(fails ? `load-e2e: ${fails} failed` : "load-e2e: all ok");
process.exit(fails ? 1 : 0);
