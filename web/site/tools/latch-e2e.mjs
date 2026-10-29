// SPDX-License-Identifier: GPL-3.0-only
// Shift-click latches on the drawn FM-1 (js/input.js), end to end in headless Chromium (no window, a fresh profile, the
// browser and the server ended by their PIDs) on a native firmware (Felucca): trusted mouse events through the DevTools
// protocol. Shift + click HOME, then SEL: both stay down in what the page posts to the engine (the worklet's "panel"
// messages) after the mouse lets go; the first Shift over the panel shows "Shift-click to hold" once; a plain click on a
// key while they are latched is a momentary note; a plain click on HOME lets it go; Escape lets the rest go.
//   node web/site/tools/latch-e2e.mjs [--shot=FILE]
// Needs web/build/native/felucca.wasm. Writes web/build/test/latch-latched.png (and --shot).
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join, dirname } from "node:path";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const WEB = join(ROOT, "web"), OUT = join(WEB, "build", "test");
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOT = arg("shot", "");
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
if (!existsSync(join(WEB, "build/native/felucca.wasm"))) { console.error("missing web/build/native/felucca.wasm"); process.exit(2); }
mkdirSync(OUT, { recursive: true });

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(96)} ${c ? "ok" : "FAIL"}`); return c; };

const profile = mkdtempSync(join(WEB, "build", "chromium-profile-latch-"));
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,800",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
const stop = () => { try { chrome.kill(); } catch {} try { server.kill(); } catch {} };
process.on("exit", stop);
process.on("SIGINT", () => process.exit(130));
process.on("SIGTERM", () => process.exit(143));

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
const SHIFT = 8;
const mouse = (type, p, modifiers = 0, button = "left") =>
  cdp("Input.dispatchMouseEvent", { type, x: p.x, y: p.y, modifiers, button: type === "mouseMoved" ? "none" : button, buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
const press = async (p, mods = 0) => { await mouse("mouseMoved", p, mods); await mouse("mousePressed", p, mods); };
const release = (p, mods = 0) => mouse("mouseReleased", p, mods);
const click = async (p, mods = 0) => { await press(p, mods); await sleep(60); await release(p, mods); await sleep(120); };
const keyEv = (type, code, key, vk, modifiers = 0) => cdp("Input.dispatchKeyEvent", { type, code, key, windowsVirtualKeyCode: vk, modifiers });

// the controls' centres in the page's CSS pixels: a DeviceView laid out as the page's (same box, same dpr), where();
// hit() confirms each point is the control meant
const WHERE = `(async () => {
  const P = await import("/web/site/js/panel.js");
  const cv = document.getElementById("device"), r = cv.getBoundingClientRect();
  const dv = new P.DeviceView({ getContext: () => null, width: 0, height: 0 }, new P.Controls(() => {}), null);
  dv.layout(cv.clientWidth, cv.clientHeight, devicePixelRatio);
  const at = (kind, idx) => { const c = dv.where(kind, idx), h = dv.hit(c.x, c.y); return { x: r.left + c.x, y: r.top + c.y, ok: !!h && h.kind === kind && h.idx === idx }; };
  const empty = { x: r.left + 3, y: r.top + 3, ok: !dv.hit(3, 3) };
  return { home: at(P.KM.BTN, P.B.HOME), sel: at(P.KM.BTN, P.B.SEL), key0: at(P.KM.KEY, 0), empty,
    homeBit: P.labelToMatrix(P.B.HOME), selBit: P.labelToMatrix(P.B.SEL) };
})()`;
// what the page posts to the engine: the worklet's last "panel" message (matrix order)
const HOOK = `(() => { const p = FM1Rec.o.getPlayer(), port = p.node.port, f = port.postMessage.bind(port);
  window.__panel = null; port.postMessage = (m, t) => { if (m && m.type === "panel") window.__panel = { keys: m.keys, buttons: m.buttons }; return f(m, t); };
  return true; })()`;
const panel = () => js("window.__panel");
const bit = (v, b) => ((v >>> b) & 1) === 1;

try {
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  const page = targets.find((t) => t.type === "page");
  ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const errors = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") { errors.push(JSON.stringify(d.params.exceptionDetails).slice(0, 400)); console.error("exception:", errors.at(-1)); }
  };
  await cdp("Runtime.enable"); await cdp("Page.enable");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 2, mobile: false });
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=felucca&engine=native` });
  try { await until(`window.FM1Rec && window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "native"`, 90000, "native Felucca running"); }
  catch (e) {
    console.log("  page:", JSON.stringify(await js(`({ cur: window.FM1Site ? (({ entry, engine, info }) => ({ id: entry && entry.id, engine, info: !!info }))(FM1Site.current()) : null,
      toast: document.getElementById("toast").textContent, status: (document.getElementById("status") || {}).textContent })`)));
    throw e;
  }
  await sleep(1500);
  await js(HOOK);
  const W = await js(WHERE);
  ok(W.home.ok && W.sel.ok && W.key0.ok, "the points: HOME, SEL and the first key where the panel draws them");

  // the hint: Shift with the mouse over the panel, once
  await mouse("mouseMoved", W.empty);
  await keyEv("rawKeyDown", "ShiftLeft", "Shift", 16, SHIFT);
  await sleep(150);
  ok(await js(`document.getElementById("toast").classList.contains("show") && document.getElementById("toast").textContent === "Shift-click to hold"`),
     "the first Shift over the panel: \"Shift-click to hold\"");
  ok(await js(`JSON.parse(localStorage.getItem("remotefm1-player")).shiftHintShown === true`), "remembered (prefs): it shows once");

  // Shift + click HOME, then SEL; the mouse lets go: both stay down in the engine's input
  await click(W.home, SHIFT);
  await click(W.sel, SHIFT);
  await keyEv("keyUp", "ShiftLeft", "Shift", 16, 0);
  await sleep(300);
  let p = await panel();
  ok(p && bit(p.buttons, W.homeBit) && bit(p.buttons, W.selBit), `Shift-click HOME and SEL, mouse up: both held in the engine's input (buttons 0x${p && p.buttons.toString(16)})`);
  await mouse("mouseMoved", W.empty);
  await sleep(2800);   // (the hint's toast gone)
  const shot = Buffer.from((await cdp("Page.captureScreenshot", { format: "png" })).result.data, "base64");
  writeFileSync(join(OUT, "latch-latched.png"), shot);
  if (SHOT) { mkdirSync(dirname(SHOT), { recursive: true }); writeFileSync(SHOT, shot); }
  console.log(`  shot ${SHOT || join(OUT, "latch-latched.png")}`);

  // a plain click on a key while latched: a momentary note, the latches stay
  await press(W.key0);
  await sleep(250);
  const during = await panel();
  await release(W.key0);
  await sleep(250);
  p = await panel();
  ok(bit(during.keys, 0) && bit(during.buttons, W.homeBit) && bit(during.buttons, W.selBit), "a plain click on a key while latched: the note held while pressed, with HOME and SEL");
  ok(!bit(p.keys, 0) && bit(p.buttons, W.homeBit) && bit(p.buttons, W.selBit), "the mouse up: the note lets go, HOME and SEL stay");

  // a plain click on HOME lets it go; Escape lets SEL go (and is not OCT-)
  await click(W.home);
  await sleep(200);
  p = await panel();
  ok(!bit(p.buttons, W.homeBit) && bit(p.buttons, W.selBit), "a plain click on latched HOME lets it go; SEL stays");
  await keyEv("rawKeyDown", "Escape", "Escape", 27); await keyEv("keyUp", "Escape", "Escape", 27);
  await sleep(200);
  p = await panel();
  ok(p.buttons === 0 && p.keys === 0, "Escape lets the rest go (nothing held, no OCT-)");
  ok(!errors.length, `no page exceptions (${errors.length})`);
} catch (e) {
  console.error(e); fails++;
} finally {
  try { ws && ws.close(); } catch {}
  stop();
}
console.log(`\n${checks - fails}/${checks} checks passed`);
process.exit(fails ? 1 : 0);
