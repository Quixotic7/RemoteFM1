// SPDX-License-Identifier: GPL-3.0-only
// Skins in the player, end to end (js/skin.js; RemoteFM1IOS docs/RFM1-FORMAT.md section 8): this tree's site (the dev
// layout, served from the repo root) in headless Chromium (no window, a fresh profile, the browser and the server
// ended by their PIDs). The faceplate with no background; the sheet's Background section; the visitor's image through
// Background image (behind the FM-1, the Dim tint over it, kept after a reload); an .rfm1 with its own skin (made here
// from fixtures/labels_core.rfm1 by tools/rfm1-skin.mjs: its image and colours win while it plays, Firmware skins off
// gives the visitor's back); a phone-sized view (the portrait image); Remove. The images are made here (no binaries
// in the tree). Screenshots in --shots.
//   node web/site/tools/skin-e2e.mjs [--shots=web/build/test]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";
import { addSkin } from "./rfm1-skin.mjs";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test"));
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

// a PNG from f(x, y) -> [r, g, b]
function png(w, h, f) {
  const crc = (buf) => { let c = ~0; for (const b of buf) { c ^= b; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); } return ~c >>> 0; };
  const chunk = (type, data) => { const t = Buffer.from(type), len = Buffer.alloc(4), cr = Buffer.alloc(4); len.writeUInt32BE(data.length); cr.writeUInt32BE(crc(Buffer.concat([t, data]))); return Buffer.concat([len, t, data, cr]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) raw.set(f(x, y), y * (w * 3 + 1) + 1 + x * 3);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const c8 = (v) => Math.max(0, Math.min(255, Math.round(v)));
// the visitor's "photo": busy, bright, many colours (the worst case for the labels)
const photo = png(1600, 1000, (x, y) => {
  const u = x / 1600, v = y / 1000, p = Math.sin(x / 37) + Math.sin(y / 23) + Math.sin((x + y) / 51) + Math.sin(Math.hypot(x - 800, y - 500) / 29);
  return [c8(128 + 110 * Math.sin(p * 1.3 + u * 3)), c8(128 + 110 * Math.sin(p * 1.3 + 2.1 + v * 2)), c8(128 + 110 * Math.sin(p * 1.3 + 4.2))];
});
// a firmware's skin: a dusk gradient with rings (portrait), a wider one (landscape), and its own colours
const dusk = (w, h) => png(w, h, (x, y) => { const v = y / h, r = Math.hypot(x - w * 0.5, y - h * 0.35); const ring = Math.sin(r / 9) > 0.85 ? 60 : 0;
  return [c8(30 + 140 * v + ring), c8(20 + 40 * v + ring * 0.5), c8(90 + 80 * (1 - v) + ring)]; });
const FW_THEME = { body: "#2A1E4A", key: "#5B3F99", knob: "#0E0A18", led: "#FFB0F0", bezel: "#05030A" };

const tmp = mkdtempSync(join(tmpdir(), "skin-e2e-")), profile = join(tmp, "profile");
const photoFile = join(tmp, "my-photo.png"), cart = join(tmp, "labels-skinned.rfm1");
writeFileSync(photoFile, photo);
writeFileSync(cart, addSkin(readFileSync(join(ROOT, "web/engines/rfm1/fixtures/labels_core.rfm1")),
  { background: new Uint8Array(dusk(900, 1600)), landscape: new Uint8Array(dusk(1600, 900)), theme: FW_THEME, dim: 0.15 }));
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
async function setFile(selector, file) {
  const { root } = (await cdp("DOM.getDocument", { depth: 1 })).result;
  const { nodeId } = (await cdp("DOM.querySelector", { nodeId: root.nodeId, selector })).result;
  const r = await cdp("DOM.setFileInputFiles", { nodeId, files: [file] });
  if (r.error) throw new Error(`setFileInputFiles: ${r.error.message}`);
}
const esc = () => cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
const openSheet = async () => { await js(`(document.getElementById("menu-btn").click(), true)`); await until(`document.querySelector(".sheet.open")`, 5000, "the sheet"); await sleep(300); };
const closeSheet = async () => { await esc(); await sleep(400); };
// the faceplate canvas: a few pixels of open faceplate (the corners and the middle of the left edge), and how many
// colours a strip of it has; the page's --body
const PLATE = `(() => { const c = document.getElementById("device"), g = c.getContext("2d"), W = c.width, H = c.height;
  const px = (x, y) => Array.from(g.getImageData(x, y, 1, 1).data.slice(0, 3));
  const strip = g.getImageData(0, Math.floor(H * 0.02), W, 1).data, s = new Set();
  for (let i = 0; i < strip.length; i += 4) s.add((strip[i] >> 3) << 10 | (strip[i + 1] >> 3) << 5 | (strip[i + 2] >> 3));
  return { tl: px(2, 2), br: px(W - 3, H - 3), colours: s.size, body: getComputedStyle(document.documentElement).getPropertyValue("--body").trim(),
    skinned: document.body.classList.contains("skinned") }; })()`;
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const rgbOf = (s) => (s.match(/\d+/g) || []).slice(0, 3).map(Number);
const sheetHas = (text) => js(`document.querySelector(".sheet-body").innerText.includes(${JSON.stringify(text)})`);
const scrollToBackground = () => js(`(() => { const hs = [...document.querySelectorAll(".sheet-body h3")]; const b = hs.find((x) => x.textContent.trim() === "Background"); if (b) b.scrollIntoView({ block: "start" }); return !!b; })()`);

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
  await until(`window.FM1Site && FM1Site.current().entry && document.getElementById("device").width > 0`, 60000, "the page");
  await sleep(1500);

  // 1. no background: the theme's colour
  let p = await js(PLATE);
  ok(!p.skinned && dist(p.tl, rgbOf(p.body)) < 4 && p.colours <= 3, `no background: the faceplate is the theme's colour (${p.body}, ${p.colours} colours along the top)`);
  await closeSheet();
  await shot("skin-1-none");

  // 2. the sheet's Background section, under the theme's colours
  await openSheet();
  const order = await js(`[...document.querySelectorAll(".sheet-body h3")].map((x) => x.childNodes[0].textContent.trim())`);
  ok(order.indexOf("Background") === order.indexOf("Look") + 1, `the sheet: Background right after Look (${order.join(", ")})`);
  ok(await sheetHas("Background image") && await sheetHas("Firmware skins") && !(await sheetHas("Remove background image")) && !(await js(`!!document.querySelector(".skin-dim")`)),
     "Background image and Firmware skins; no Remove or Dim without an image");
  await scrollToBackground(); await sleep(200);
  await shot("skin-2-sheet-empty");

  // 3. the visitor's image
  await setFile("input.skin-file", photoFile);
  await until(`document.body.classList.contains("skinned")`, 10000, "the background image");
  await sleep(300);
  ok(await sheetHas("Remove background image") && await js(`document.querySelector(".skin-dim") && document.querySelector(".skin-dim").value === "25"`), "with an image: Remove, and Dim at 25 %");
  await scrollToBackground(); await sleep(200);
  await shot("skin-3-sheet-image");
  await closeSheet();
  p = await js(PLATE);
  const tlUser = p.tl;
  ok(p.skinned && p.colours > 20, `the image behind the faceplate (${p.colours} colours along the top)`);
  await shot("skin-4-user-image");
  // Dim at most: the faceplate's colour over it, nearer the body
  await openSheet();
  await js(`(() => { const r = document.querySelector(".skin-dim"); r.value = "80"; r.dispatchEvent(new Event("input")); r.dispatchEvent(new Event("change")); return true; })()`);
  await closeSheet(); await sleep(300);
  p = await js(PLATE);
  const body = rgbOf(p.body);
  ok(dist(p.tl, body) < dist(tlUser, body) && dist(p.tl, body) < 80, `Dim 80 %: the faceplate's colour over the image (corner ${dist(tlUser, body).toFixed(0)} -> ${dist(p.tl, body).toFixed(0)} from ${p.body})`);
  await shot("skin-5-user-image-dim80");
  // back to 25 %, then a reload: still there
  await openSheet();
  await js(`(() => { const r = document.querySelector(".skin-dim"); r.value = "25"; r.dispatchEvent(new Event("change")); return true; })()`);
  await closeSheet();
  await js("(window.__before = true)"); await cdp("Page.reload", {});
  await until(`!window.__before && window.FM1Site && document.body.classList.contains("skinned")`, 60000, "the image after a reload");
  await sleep(800);
  p = await js(PLATE);
  ok(p.skinned && dist(p.tl, tlUser) < 12, "after a reload: the image again, at the dim it had (kept in this browser)");

  // 4. a firmware with its own skin: its image and colours win
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector('input[type=file][accept=".rfm1,.fwsc"]')`, 5000, "the Open a file... input");
  await setFile('input[type=file][accept=".rfm1,.fwsc"]', cart);
  await until(`FM1Site.current().engine === "rfm1" && FM1Site.current().info && FM1Site.current().info.id === "labeltest"`, 30000, "the skinned .rfm1");
  await until(`getComputedStyle(document.documentElement).getPropertyValue("--body").trim() === "rgb(42,30,74)"`, 10000, "the firmware skin's colours");
  await sleep(600);
  await closeSheet();
  p = await js(PLATE);
  ok(p.skinned && p.body === "rgb(42,30,74)" && dist(p.tl, tlUser) > 40, `the firmware's skin: its image and its colours (--body ${p.body})`);
  await shot("skin-6-firmware-skin");
  await openSheet();
  ok(await sheetHas("comes with its own skin"), "the sheet says the firmware brings its own skin");
  await scrollToBackground(); await sleep(200);
  await shot("skin-7-sheet-firmware");
  // Firmware skins off: the visitor's image and theme
  await js(`(() => { const r = [...document.querySelectorAll(".sheet-body .row")].find((x) => x.innerText.startsWith("Firmware skins")); r.querySelector(".switch").click(); return true; })()`);
  await closeSheet(); await sleep(400);
  p = await js(PLATE);
  ok(p.skinned && p.body !== "rgb(42,30,74)" && dist(p.tl, tlUser) < 12, `Firmware skins off: the visitor's image and theme again (--body ${p.body})`);
  await openSheet();
  await js(`(() => { const r = [...document.querySelectorAll(".sheet-body .row")].find((x) => x.innerText.startsWith("Firmware skins")); r.querySelector(".switch").click(); return true; })()`);
  await closeSheet(); await sleep(400);
  // a phone: the portrait image
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  p = await js(PLATE);
  ok(p.skinned && p.body === "rgb(42,30,74)", "a phone-sized view: the firmware's skin (its portrait image)");
  await shot("skin-8-firmware-skin-phone");
  await cdp("Emulation.clearDeviceMetricsOverride", {});
  await sleep(600);

  // 5. back to a firmware with none, then Remove: the theme's colour
  await openSheet();
  ok(await js(`(() => { const r = [...document.querySelectorAll(".sheet-body .row")].find((x) => x.innerText.startsWith("Remove background image")); if (!r) return false; r.querySelector("button").click(); return true; })()`), "Remove background image");
  await sleep(500);
  ok(!(await sheetHas("Remove background image")), "the Remove row goes with the image");
  await closeSheet();
  p = await js(PLATE);
  ok(p.skinned && p.body === "rgb(42,30,74)", "the firmware's own skin stays (it is not the visitor's)");
  await js("(window.__before = true)"); await cdp("Page.reload", {});
  await until(`!window.__before && window.FM1Site && FM1Site.current().info`, 60000, "the page after a reload");
  await sleep(1500);
  const stored = await js(`new Promise((res) => { const r = indexedDB.open("remotefm1-skin"); r.onsuccess = () => { const q = r.result.transaction("skin").objectStore("skin").get("background"); q.onsuccess = () => res(!!q.result); q.onerror = () => res("err"); }; r.onerror = () => res("err"); })`);
  ok(stored === false, "removed: nothing kept in this browser");
  ok(!exceptions.length, `no page exceptions${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
} catch (e) {
  fails++;
  console.error(e.stack || String(e));
  try { await shot("skin-failed"); } catch {}
} finally {
  stop();
}
console.log(fails ? `skin e2e: ${fails} of ${checks} FAILED` : `skin e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
