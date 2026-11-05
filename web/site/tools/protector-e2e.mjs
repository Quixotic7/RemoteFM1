// SPDX-License-Identifier: GPL-3.0-only
// The screen protector in the player, end to end (js/protector.js; RemoteFM1IOS ScreenProtector.swift): this tree's
// site (the dev layout, served from the repo root) in headless Chromium (no window, a fresh profile, the browser and
// the server ended by their PIDs). On a wide window the film is over the display with its red tab beside the screen;
// a short drag springs back; a long one peels it off (remembered after a reload); View › Screen protector puts it
// back. A phone upright has no film and no red square (the old indicator LED is gone). With and without a background
// image (js/skin.js), desktop and phone sizes. Screenshots in --shots.
//   node web/site/tools/protector-e2e.mjs [--shots=web/build/test]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test"));
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

// a busy background image (as skin-e2e.mjs's)
function png(w, h, f) {
  const crc = (buf) => { let c = ~0; for (const b of buf) { c ^= b; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); } return ~c >>> 0; };
  const chunk = (type, data) => { const t = Buffer.from(type), len = Buffer.alloc(4), cr = Buffer.alloc(4); len.writeUInt32BE(data.length); cr.writeUInt32BE(crc(Buffer.concat([t, data]))); return Buffer.concat([len, t, data, cr]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) raw.set(f(x, y), y * (w * 3 + 1) + 1 + x * 3);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const c8 = (v) => Math.max(0, Math.min(255, Math.round(v)));
const tmp = mkdtempSync(join(tmpdir(), "protector-e2e-")), profile = join(tmp, "profile"), photoFile = join(tmp, "my-photo.png");
writeFileSync(photoFile, png(1200, 800, (x, y) => { const p = Math.sin(x / 31) + Math.sin(y / 19) + Math.sin((x + y) / 47);
  return [c8(128 + 110 * Math.sin(p * 1.3 + x / 400)), c8(128 + 110 * Math.sin(p * 1.3 + 2.1)), c8(128 + 110 * Math.sin(p * 1.3 + 4.2 + y / 300))]; }));

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
  throw new Error(`timed out: ${what}`);
}
async function shot(name) {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from(r.result.data, "base64"));
  console.log(`  shot ${f}`);
}
const esc = () => cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
const openSheet = async () => { await js(`(document.getElementById("menu-btn").click(), true)`); await until(`document.querySelector(".sheet.open")`, 5000, "the sheet"); await sleep(300); };
const closeSheet = async () => { await esc(); await sleep(400); };
const size = async (w, h, mobile) => { if (w) await cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile }); else await cdp("Emulation.clearDeviceMetricsOverride", {}); await sleep(1200); };
// the tab's centre in page pixels (null: no film), and the indicator red (#FF3B2F) on the canvas outside the display
const TAB = `(() => { const a = FM1Device.anchor("film"); if (!a) return null; const r = document.getElementById("device").getBoundingClientRect(); return { x: r.left + a.x, y: r.top + a.y, length: a.length }; })()`;
const RED = `(() => { const c = document.getElementById("device"), g = c.getContext("2d"), d = FM1Device.L.display, k = FM1Device.dpr;
  const px = g.getImageData(0, 0, c.width, c.height).data; let n = 0;
  for (let y = 0; y < c.height; y += 2) for (let x = 0; x < c.width; x += 2) {
    if (x >= d.x * k && x < (d.x + d.size) * k && y >= d.y * k && y < (d.y + d.size) * k) continue;
    const i = (y * c.width + x) * 4; if (px[i] > 245 && Math.abs(px[i + 1] - 59) < 5 && Math.abs(px[i + 2] - 47) < 5) n++; }
  return n; })()`;
const mouse = (type, x, y) => cdp("Input.dispatchMouseEvent", { type, x, y, button: "left", buttons: type === "mouseReleased" ? 0 : 1, clickCount: 1, pointerType: "mouse" });
async function drag(from, dx, steps, release = true) {
  await mouse("mousePressed", from.x, from.y);
  for (let i = 1; i <= steps; i++) { await mouse("mouseMoved", from.x + dx * i / steps, from.y); await sleep(30); }
  if (release) { await sleep(250); await mouse("mouseReleased", from.x + dx, from.y); }   // (a pause: no fling)
}
const switchRow = (label) => `(() => { const r = [...document.querySelectorAll(".sheet-body .row")].find((x) => x.innerText.startsWith(${JSON.stringify(label)})); if (!r) return null; return r.querySelector(".switch"); })()`;

try {
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
  for (let i = 0; i < 50; i++) { try { if ((await fetch(base)).ok) break; } catch {} await sleep(200); }
  await cdp("Page.navigate", { url: `${base}?view=device` });
  await until(`window.FM1Site && window.FM1Device && FM1Site.current().entry && FM1Device.L`, 60000, "the page");
  await sleep(1500);
  await js(`(document.getElementById("toast").classList.remove("show"), true)`);

  // 1. a wide window: the film and its tab
  let tab = await js(TAB);
  ok(tab && await js(`FM1Device.kind`) === "padLandscape", `1280 x 800: the film is on, its tab at ${tab && tab.x.toFixed(0)}, ${tab && tab.y.toFixed(0)}`);
  ok(await js(RED) > 20, "the red square is the tab, right of the screen");
  await shot("protector-1-desktop-on");
  // 2. a short drag springs back
  await drag(tab, -tab.length * 0.18, 6);
  await sleep(700);
  ok(!!(await js(TAB)) && (await js(`FM1Device.film.t`)) < 1, "a short drag: the film springs back, still on");
  // 3. a long one peels it (a shot halfway)
  await drag(tab, -tab.length * 0.45, 12, false);
  await sleep(200);
  ok((await js(`FM1Device.film.t`)) > tab.length * 0.3, "dragging: the film folds back with the pointer");
  await shot("protector-2-peeling");
  await mouse("mouseReleased", tab.x - tab.length * 0.45, tab.y);
  await sleep(900);
  { const n = await js(RED); ok(!(await js(TAB)) && n === 0, `let go past a third: it peels off, the red square with it (${n} red pixels left)`); }
  ok(await js(`JSON.parse(localStorage.getItem("remotefm1-player")).screenProtector === false`), "remembered (prefs.screenProtector false)");
  await shot("protector-3-desktop-peeled");
  await js("(window.__before = true)"); await cdp("Page.reload", {});
  await until(`!window.__before && window.FM1Device && FM1Device.L && FM1Site.current().entry`, 60000, "the page after a reload");
  await sleep(1000);
  ok(!(await js(TAB)) && await js(RED) === 0, "after a reload: still off");
  // 4. View › Screen protector puts it back
  await openSheet();
  ok(await js(`!!${switchRow("Screen protector")} && ${switchRow("Screen protector")}.getAttribute("aria-checked") === "false"`), "the sheet's View: Screen protector, off");
  await js(`(${switchRow("Screen protector")}.click(), true)`);
  await closeSheet();
  ok(!!(await js(TAB)), "turned on again: the film is back");

  // 5. a phone upright: no film, no red square; sideways: the film
  await size(390, 844, true);
  ok(await js(`FM1Device.kind`) === "phonePortrait" && !(await js(TAB)) && await js(RED) === 0, "a phone upright: no film and no red square (the indicator LED is gone)");
  await shot("protector-4-phone-portrait");
  await size(844, 390, false);
  ok(await js(`FM1Device.kind`) === "phoneLandscape" && !!(await js(TAB)) && await js(RED) > 5, "a phone sideways: the film, its tab beside the screen");
  await shot("protector-5-phone-landscape");

  // 6. with a background image
  await size(0);
  await openSheet();
  const { root } = (await cdp("DOM.getDocument", { depth: 1 })).result;
  const { nodeId } = (await cdp("DOM.querySelector", { nodeId: root.nodeId, selector: "input.skin-file" })).result;
  await cdp("DOM.setFileInputFiles", { nodeId, files: [photoFile] });
  await until(`document.body.classList.contains("skinned")`, 10000, "the background image");
  await closeSheet();
  ok(!!(await js(TAB)) && await js(RED) > 20, "with a background image: the film and its tab over it");
  await shot("protector-6-desktop-skin");
  await size(390, 844, true);
  ok(!(await js(TAB)) && await js(RED) === 0, "a phone upright with the image: no red square");
  await shot("protector-7-phone-portrait-skin");
  await size(844, 390, false);
  await shot("protector-8-phone-landscape-skin");
  await size(0);
  ok(!exceptions.length, `no page exceptions${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
} catch (e) {
  fails++;
  console.error(e.stack || String(e));
  try { await shot("protector-failed"); } catch {}
} finally {
  stop();
}
console.log(fails ? `protector e2e: ${fails} of ${checks} FAILED` : `protector e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
