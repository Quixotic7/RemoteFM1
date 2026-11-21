// SPDX-License-Identifier: GPL-3.0-only
// The firmware browser (js/fw-browser.js in the settings sheet's Firmware page), end to end: this tree's site (the dev
// layout, served from the repo root) in headless Chromium (no window, a fresh profile, the browser and the server
// ended by their PIDs). RemoteFM1FW's catalog (raw.githubusercontent.com) is served by the test (--live: the real one).
//   - a tile per native firmware, in the iOS app's grid; the catalog images drawn (lazily: the ones near the view),
//     the others their name on a colour of their own; the loaded firmware outlined with a check
//   - the filters: a category (one at a time), Fantasy CFW, the search; a heart makes a favourite, Favourites shows
//     only those; the last one gone, All shows by itself (the choice kept); the choices kept after a reload
//   - a tap opens that firmware; phone (390 x 844): no horizontal scroll, every tile's name whole (never cut off)
//   node web/site/tools/fw-browser-e2e.mjs [--shots=web/build/test] [--live] [--prefix=fw-browser]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test")), PREFIX = arg("prefix", "fw-browser"), LIVE = process.argv.includes("--live");
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(110)} ${c ? "ok" : "FAIL"}`); return c; };

// RemoteFM1FW's catalog, as the test serves it: an image for some of the site's native firmwares (PNG and SVG)
function png(w, h, f) {
  const crc = (buf) => { let c = ~0; for (const b of buf) { c ^= b; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); } return ~c >>> 0; };
  const chunk = (type, data) => { const t = Buffer.from(type), len = Buffer.alloc(4), cr = Buffer.alloc(4); len.writeUInt32BE(data.length); cr.writeUInt32BE(crc(Buffer.concat([t, data]))); return Buffer.concat([len, t, data, cr]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) raw.set(f(x, y), y * (w * 3 + 1) + 1 + x * 3);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const site = JSON.parse(readFileSync(join(ROOT, "web/site/catalog.json"), "utf8"));
const native = site.firmwares.filter((f) => (f.native && f.engines.includes("native")) || f.engines.includes("rfm1"));
const withImg = native.slice(0, 6).map((f) => f.id);
const FW = "https://raw.githubusercontent.com/Quixotic7/RemoteFM1FW/main/catalog/";
const FIX = { [FW + "catalog.json"]: ["application/json", Buffer.from(JSON.stringify({ schema: 1, firmwares: Object.fromEntries(withImg.map((id, i) => [id, { image: `images/${id}.png`, category: "Synth" }])) }))] };
withImg.forEach((id, i) => { FIX[`${FW}images/${id}.png`] = ["image/png", png(96, 96, (x, y) => [(x * 3 + i * 40) & 255, (y * 2 + i * 70) & 255, ((x ^ y) * 4) & 255])]; });

const tmp = mkdtempSync(join(tmpdir(), "fw-browser-e2e-")), profile = join(tmp, "profile");
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,860",
  "--mute-audio", `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
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
  while (Date.now() - t0 < ms) { if (await js(`!!(${expr})`).catch(() => false)) return true; await sleep(100); }
  console.log(`  (timed out: ${what})`);
  return false;
}
async function shot(name) {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const f = join(SHOTS, `${PREFIX}-${name}.png`);
  writeFileSync(f, Buffer.from(r.result.data, "base64"));
  console.log(`  shot ${f}`);
}
const size = async (w, h, mobile) => { await cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile }); await sleep(600); };
const tap = (sel) => js(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true; })()`);
const tiles = () => js(`[...document.querySelectorAll(".sheet .fb-tile")].map((t) => t.dataset.id)`);
const openBrowser = async () => {
  await tap("#fw-btn");
  return until(`document.querySelector(".sheet.open") && document.querySelectorAll(".sheet .fb-tile").length`, 8000, "the firmware page");
};

try {
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const exceptions = [], asked = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") exceptions.push(JSON.stringify(d.params.exceptionDetails).slice(0, 300));
    if (d.method === "Fetch.requestPaused") {
      const u = d.params.request.url, f = FIX[u];
      asked.push(u);
      cdp("Fetch.fulfillRequest", { requestId: d.params.requestId, responseCode: f ? 200 : 404,
        responseHeaders: [{ name: "Content-Type", value: f ? f[0] : "text/plain" }, { name: "Access-Control-Allow-Origin", value: "*" }],
        body: (f ? f[1] : Buffer.from("not here")).toString("base64") });
    }
  };
  if (!LIVE) await cdp("Fetch.enable", { patterns: [{ urlPattern: "https://raw.githubusercontent.com/*" }] });
  await cdp("Runtime.enable"); await cdp("Page.enable");
  await size(1280, 860, false);
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/` });
  ok(await until(`window.FM1Site && FM1Site.current && FM1Site.current().entry`, 30000, "the page"), "the page loads (its first firmware)");
  await sleep(800);

  // ---- the grid
  ok(await openBrowser(), "the header's firmware picker opens the Firmware page");
  const ids = await tiles();
  ok(ids.length === native.length, `a tile per native firmware: ${ids.length} of ${native.length}`);
  const cur = await js(`(() => { const c = document.querySelectorAll(".sheet .fb-tile.current"); return c.length === 1 && c[0].querySelector(".fb-cur") ? c[0].dataset.id : null; })()`);
  ok(cur && cur === (await js(`FM1Site.current().entry.id`)), `the loaded firmware's tile outlined with a check (${cur})`);
  const drawn = await until(`[...document.querySelectorAll(".sheet .fb-art img.in")].length >= ${LIVE ? 4 : withImg.length}`, 10000, "the catalog images");
  const paint = await js(`(() => { const t = [...document.querySelectorAll(".sheet .fb-tile")];
    return { imgs: t.filter((x) => x.querySelector(".fb-art img.in")).map((x) => x.dataset.id), painted: t.filter((x) => x.querySelector(".fb-art").dataset.painted).length,
      stand: t.filter((x) => !x.querySelector(".fb-art img") && getComputedStyle(x.querySelector(".fb-stand")).backgroundImage.includes("gradient")).length,
      pix: (() => { const i = document.querySelector(".sheet .fb-art img.in"); return i ? [i.naturalWidth, i.getBoundingClientRect().width | 0] : null; })() }; })()`);
  ok(drawn && (LIVE ? paint.imgs.length >= 4 : withImg.every((x) => paint.imgs.includes(x))), `the catalog images drawn: ${paint.imgs.length} (${paint.imgs.slice(0, 6).join(", ")}${LIVE ? ", live" : ""})`);
  ok(paint.stand > 0, `firmwares without an image: their name on a colour of their own (${paint.stand} tiles)`);
  ok(LIVE || paint.painted < ids.length, `lazily: ${paint.painted} of ${ids.length} tiles painted before scrolling`);
  if (!LIVE) ok(asked.filter((u) => u.endsWith("catalog.json")).length === 1, "RemoteFM1FW's catalog asked for once");
  await sleep(LIVE ? 2000 : 400);   // (the images fade in)
  await shot("site-desktop");

  // ---- the filters
  const cats = await js(`[...document.querySelectorAll('.sheet .fb-chips[aria-label="Category"] .fb-chip')].map((c) => c.dataset.v)`);
  const pick = cats.find((c) => c !== "All" && c !== "Fantasy CFW");
  await tap(`.sheet .fb-chips[aria-label="Category"] .fb-chip[data-v="${pick}"]`);
  await sleep(200);
  const inCat = await tiles(), want = native.filter((f) => f.categories.includes(pick)).map((f) => f.id);
  ok(inCat.length === want.length && inCat.every((x) => want.includes(x)), `one category at a time: ${pick} shows its ${want.length}`);
  ok(await js(`document.querySelectorAll('.sheet .fb-chips[aria-label="Category"] .fb-chip.on').length === 1`), "a single choice (one chip on)");
  if (cats.includes("Fantasy CFW")) {
    await tap(`.sheet .fb-chip[data-v="Fantasy CFW"]`); await sleep(200);
    ok(await js(`[...document.querySelectorAll(".sheet .fb-tile")].every((t) => t.querySelector(".fb-fan-tag"))`), "Fantasy CFW: only those, each with its tag");
  }
  await tap(`.sheet .fb-chip[data-v="All"]`); await sleep(200);
  const q = native[3].name.slice(0, 4);
  await js(`(() => { const i = document.querySelector(".sheet .fb-q"); i.value = ${JSON.stringify(q)}; i.dispatchEvent(new Event("input")); return true; })()`);
  await sleep(200);
  const found = await tiles();
  ok(found.includes(native[3].id) && found.length < ids.length, `the search "${q}" narrows it (${found.length})`);
  await js(`(() => { const i = document.querySelector(".sheet .fb-q"); i.value = ""; i.dispatchEvent(new Event("input")); return true; })()`);
  await sleep(200);

  // ---- favourites: a heart, Favourites, the last one gone: All by itself
  ok(await js(`document.querySelector('.sheet .fb-chip[data-v="fav"]').disabled`), "no favourites yet: the Favourites chip is off, All shows");
  await tap(`.sheet .fb-tile[data-id="${native[2].id}"] .fb-fav`); await sleep(150);
  await tap(`.sheet .fb-tile[data-id="${native[4].id}"] .fb-fav`); await sleep(150);
  await tap(`.sheet .fb-chip[data-v="fav"]`); await sleep(200);
  const favs = await tiles();
  ok(favs.length === 2 && favs.includes(native[2].id) && favs.includes(native[4].id), `Favourites: the two hearted (${favs.join(", ")})`);
  await shot("site-favourites");
  await js(`location.reload()`); await sleep(500);
  ok(await until(`window.FM1Site && FM1Site.current && FM1Site.current().entry`, 30000, "the page again"), "reloaded");
  await openBrowser();
  ok((await tiles()).length === 2, "after a reload: still Favourites, still two (kept in this browser)");
  await tap(`.sheet .fb-tile[data-id="${native[2].id}"] .fb-fav`); await sleep(150);
  await tap(`.sheet .fb-tile[data-id="${native[4].id}"] .fb-fav`); await sleep(200);
  ok((await tiles()).length === ids.length && await js(`localStorage.getItem("fm1player.fwbrowser").includes('"fav"')`),
    "the last favourite gone: All shows by itself, the Favourites choice kept");

  // ---- a tap opens the firmware
  const other = native.find((f) => f.id !== cur && f.engines.includes("native")) || native.find((f) => f.id !== cur);
  await tap(`.sheet .fb-tile[data-id="${other.id}"] .fb-main`);
  ok(await until(`!document.querySelector(".sheet.open") && FM1Site.current().entry && FM1Site.current().entry.id === ${JSON.stringify(other.id)}`, 15000, "the pick"),
    `a tap opens ${other.name} and closes the sheet`);

  // ---- phone
  await size(390, 844, true);
  await openBrowser();
  await sleep(LIVE ? 2500 : 800);
  const phone = await js(`({ scroll: document.documentElement.scrollWidth <= innerWidth + 1,
    cut: [...document.querySelectorAll(".sheet .fb-name, .sheet .fb-ver, .sheet .fb-chip")].filter((e) => e.scrollWidth > e.clientWidth + 1 || e.getBoundingClientRect().right > innerWidth + 1).map((e) => e.textContent),
    cols: getComputedStyle(document.querySelector(".sheet .fb-grid")).gridTemplateColumns.split(" ").length })`);
  ok(phone.scroll, "phone: no horizontal page scroll");
  ok(!phone.cut.length, `phone: names, versions and chips whole, never cut off (${phone.cut.slice(0, 4).join(", ") || "none cut"})`);
  ok(phone.cols >= 3, `phone: ${phone.cols} tiles a row`);
  await shot("site-phone");
  await js(`(() => { const b = document.querySelector(".sheet-body"); b.scrollTop = b.scrollHeight / 3; return true; })()`);
  await sleep(LIVE ? 2500 : 600);
  ok(await js(`document.querySelectorAll(".sheet .fb-art[data-painted]").length >= ${LIVE ? 4 : withImg.length}`), "scrolled: the tiles coming into view get drawn");
  await shot("site-phone-scrolled");

  ok(!exceptions.length, `no page exceptions${exceptions.length ? ": " + exceptions[0] : ""}`);
} catch (e) {
  fails++;
  console.log(`FAIL ${e.message}`);
}
console.log(`\nfw-browser-e2e: ${checks - fails}/${checks} ok${fails ? `, ${fails} FAILED` : ""}`);
stop();
process.exit(fails ? 1 : 0);
