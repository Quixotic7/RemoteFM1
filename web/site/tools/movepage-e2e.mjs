// SPDX-License-Identifier: MPL-2.0
// remotefm1.com/move end to end: this tree's page (the dev layout, served from the repo root) in headless Chromium
// (no window, a fresh profile, the browser and the server ended by their PIDs; --use-mock-keychain --password-store=basic).
//   - no page exceptions, no console errors
//   - the chooser: three B answers say Route B, three A answers Route A, Start again goes back to 1/3
//   - the diagram: Route B first (its links shown, Route A's hidden, "firmware runs here" over the Move), Route A
//     (its links, the badge over the computer), a node and a link tapped fill the info panel
//   - the steps: a tick counts, and is kept after a reload
//   - the embedded flasher loads (the installer's code imported): with a Wi-Fi package in releases.json (0.4.0 on) it
//     offers that release's (its file, version, identity; Flash on) and fetches and checks that very package from
//     ../../installer/firmware/ (SHA-256 and identity, nothing written); with none it says "Not released yet" and its
//     Flash button is off; "What is on my FM-1?" is there
//   - phone (390 x 844): no sideways scroll, the diagram stacked (its tall layout), the pixel FM-1 inside its node
//   node web/site/tools/movepage-e2e.mjs [--shots=web/build/test] [--prefix=movepage]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test")), PREFIX = arg("prefix", "movepage");
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const URL0 = `http://127.0.0.1:${PORT}/web/site/move/`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const tmp = mkdtempSync(join(tmpdir(), "movepage-e2e-")), profile = join(tmp, "profile");
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,900",
  "--mute-audio", `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
const stop = () => { try { chrome.kill(); } catch {} try { server.kill(); } catch {} try { rmSync(tmp, { recursive: true, force: true }); } catch {} };
process.on("exit", stop);
process.on("SIGINT", () => process.exit(130));
const hard = setTimeout(() => { console.log("FAIL movepage-e2e: timed out (120 s)"); process.exit(1); }, 120000);

let ws, id = 0;
const waiting = new Map();
const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
async function js(expression) {
  const r = await cdp("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  if (r.result?.exceptionDetails) throw new Error(`page: ${JSON.stringify(r.result.exceptionDetails).slice(0, 400)}`);
  return r.result?.result?.value;
}
async function until(expr, ms, what) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await js(`!!(${expr})`).catch(() => false)) return true; await sleep(100); }
  console.log(`  (timed out: ${what})`);
  return false;
}
const size = async (w, h, mobile) => { await cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile }); await sleep(400); };
const go = async (url) => { await cdp("Page.navigate", { url }); await sleep(300); return until(`document.readyState === "complete" && document.querySelector(".arch-svg .lk")`, 15000, "the page"); };
const click = (sel) => js(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.dispatchEvent(new MouseEvent("click", { bubbles: true })); return true; })()`);
const shot = async (name) => {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const f = join(SHOTS, `${PREFIX}-${name}.png`); writeFileSync(f, Buffer.from(r.result.data, "base64")); console.log(`  shot ${f}`);
};
const visible = (lk) => `getComputedStyle(document.querySelector('[data-lk="${lk}"]')).display !== "none"`;

try {
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const exceptions = [], errors = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") exceptions.push(JSON.stringify(d.params.exceptionDetails).slice(0, 300));
    if (d.method === "Runtime.consoleAPICalled" && d.params.type === "error") errors.push(JSON.stringify(d.params.args).slice(0, 300));
    if (d.method === "Log.entryAdded" && d.params.entry.level === "error") errors.push(`${d.params.entry.text} ${d.params.entry.url || ""}`.slice(0, 300));
  };
  await cdp("Runtime.enable"); await cdp("Page.enable"); await cdp("Log.enable");
  await size(1280, 900, false);
  ok(await go(URL0), "the page loads");

  // the chooser
  const answer = async (v) => { for (let q = 0; q < 3; q++) await click(`.ch-q[data-q="${q}"] button[data-v="${v}"]`); return js(`[document.querySelector(".ch-say").textContent, document.querySelector(".ch-count").textContent, document.querySelector(".ch-result").hidden]`); };
  let r = await answer("B");
  ok(/^Route B/.test(r[0]) && /^B/.test(r[1]) && !r[2], `chooser: three Move answers say Route B (${r[0].slice(0, 40)}…)`);
  await click("#ch-reset");
  ok(await js(`document.querySelector(".ch-count").textContent === "1/3" && !document.querySelector('.ch-q[data-q="0"]').hidden`), "chooser: Start again goes back to the first question (1/3)");
  r = await answer("A");
  ok(/^Route A/.test(r[0]) && /^A/.test(r[1]), "chooser: three computer answers say Route A");

  // the diagram, both routes
  ok(await js(`document.querySelector('.arch .seg button.on').dataset.arch === "B" && ${visible("phonemove")} && ${visible("usba")} && ${visible("wifi")} && ${visible("net")} && !(${visible("hostmove")}) && !(${visible("ble")})`),
     "diagram: Route B first (the phone page, USB-A, Wi-Fi FM-1, the catalog; no Route A links)");
  const badge = () => js(`(() => { const b = document.querySelector(".runs").getBoundingClientRect(), n = (k) => document.querySelector('[data-node="' + k + '"]').getBoundingClientRect();
    const c = b.left + b.width / 2, over = (k) => c > n(k).left && c < n(k).right && b.bottom <= n(k).top + 4 && b.bottom > n(k).top - 40;
    return over("move") ? "move" : over("host") ? "host" : JSON.stringify([b.left, b.width, b.bottom, n("move").left, n("move").right, n("move").top]); })()`);
  await sleep(600);
  const b1 = await badge();
  ok(b1 === "move" && await js(`document.querySelector('[data-node="move"]').classList.contains("here")`), `diagram B: "firmware runs here" over the Move (${b1})`);
  await click('.arch .seg button[data-arch="A"]');
  ok(await js(`${visible("hostmove")} && ${visible("usbc")} && ${visible("ble")} && ${visible("router")} && !(${visible("phonemove")}) && !(${visible("usba")})`), "diagram: Route A (Wi-Fi and USB-C to the Move, the optional FM-1, remotefm1.com)");
  await sleep(600);
  const b2 = await badge();
  ok(b2 === "host" && /Route A at a glance/.test(await js(`document.querySelector(".ai-title").textContent`)), `diagram A: the badge over the computer, the panel says Route A (${b2})`);
  await click('[data-node="move"]');
  ok(await js(`document.querySelector(".ai-title").textContent === "Ableton Move" && /Move Remote/.test(document.querySelector(".ai-body").textContent)`), "diagram: a node tapped fills the info panel");
  await click('[data-lk="usbc"]');
  ok(await js(`/USB-C/.test(document.querySelector(".ai-title").textContent)`), "diagram: a link tapped fills the info panel");
  await click('.arch .seg button[data-arch="B"]');

  // the explorer
  await click('[data-move="explore"] [data-ctl="jog"]');
  ok(await js(`document.querySelector(".ei-title").textContent === "Jog wheel" && document.querySelector(".ei-oled-txt").textContent === "SELECT"`), "explorer: a control pressed explains itself");
  ok(await js(`[...document.querySelectorAll(".m-oled b")].every((b) => b.textContent === "ChoralRoot")`), "the Move's screen shows ChoralRoot in every drawing");

  // the steps, kept
  await click('[data-steps="B"] li[data-step="schwung"] input');
  ok(await js(`document.querySelector('[data-ring="B"] b').textContent === "1"`), "steps: a tick counts (1/7)");
  ok(await go(URL0), "reloaded");
  ok(await js(`document.querySelector('[data-steps="B"] li[data-step="schwung"] input').checked && document.querySelector('[data-ring="B"] b').textContent === "1"`), "steps: the tick is kept after a reload");

  // the release states, as data/move.json says: not out yet -> "soon", no made-up number; released -> no SOON mark
  // anywhere, the hero's real count and the size unpacked
  const MJ = JSON.parse(readFileSync(new URL("../move/data/move.json", import.meta.url), "utf8")).remotefm14move;
  if (MJ.released) {
    const st = await js(`(() => ({ soon: [...document.querySelectorAll('[data-when="b-unreleased"]')].filter((e) => !e.hidden).length,
      n: document.querySelector("[data-bundled] b").textContent, lbl: document.querySelector("[data-bundled] span").textContent,
      later: document.querySelector("[data-bundled]").classList.contains("later"),
      mb: [...document.querySelectorAll("[data-bundled-mb]")].map((e) => e.textContent) }))()`);
    ok(st.soon === 0 && st.n === String(MJ.bundled) && !st.later && st.mb.length > 0 && st.mb.every((t) => t.includes(`${MJ.bundledMB} MB`)),
       `release states: RemoteFM1 for the Move ${MJ.version} released, no SOON, the hero "${st.n} ${st.lbl}"${st.mb[0] || ""}`);
  } else
    ok(await js(`!document.querySelector('[data-when="b-unreleased"]').hidden && document.querySelector("[data-bundled] b").textContent === "—"`),
       "release states: RemoteFM1 for the Move marked SOON, the bundled count not invented");

  // the embedded flasher
  await js(`document.querySelector(".flasher-frame").scrollIntoView()`);
  ok(await until(`(() => { const d = document.querySelector(".flasher-frame").contentDocument; return d && d.body && d.body.dataset.ready === "1"; })()`, 15000, "the flasher"),
     "the embedded flasher loads (the installer's code imported)");
  const fl = await js(`(() => { const d = document.querySelector(".flasher-frame").contentDocument;
    return { go: d.getElementById("f-go").disabled, t: d.getElementById("f-go-t").textContent, soon: !d.getElementById("f-soon").hidden,
             file: d.getElementById("f-file").textContent, id: d.getElementById("f-id").textContent, midi: !!navigator.requestMIDIAccess,
             state: (document.querySelector("[data-wifi-state]") || {}).textContent || "",
             check: !!d.getElementById("f-check"), h: document.querySelector(".flasher-frame").getBoundingClientRect().height }; })()`);
  const wrel = JSON.parse(readFileSync(join(ROOT, "web/installer/releases.json"), "utf8")).releases.find((r) => r.wifi);
  if (wrel) {
    ok(fl.go === !fl.midi && fl.t === `Flash RemoteFM1 ${wrel.version} Wi-Fi` && !fl.soon && fl.file === wrel.wifi.file && fl.id === wrel.wifi.identity,
       `the flasher: RemoteFM1 ${wrel.version} Wi-Fi offered (${fl.file}, ${fl.id}), Flash ${fl.go ? "off (no Web MIDI)" : "on"}`);
    ok(fl.state.includes(`RemoteFM1 ${wrel.version} Wi-Fi`), `the page: "${fl.state}"`);
    const got = await js(`(async () => { const w = document.querySelector(".flasher-frame").contentWindow;
      const I = await w.eval('import("../../../installer/installer.js")'), list = await (await w.fetch("../../../installer/releases.json")).json();
      const rel = I.wifiRelease(list.releases), p = await I.releasedPackage(rel, w.fetch.bind(w), "wifi", "../../../installer/firmware/");
      return { product: p.product, label: p.label, n: p.raw ? p.raw.length : (p.bytes ? p.bytes.length : -1) }; })()`);
    ok(got && got.product === wrel.wifi.identity, `the flasher's package: ${wrel.wifi.file} fetched from the local build, SHA-256 and identity checked (${got && got.product}, "${got && got.label}")`);
  } else ok(fl.go && fl.t === "Not released yet" && fl.soon, `the flasher: no Wi-Fi package released yet -> "${fl.t}", nothing to flash`);
  ok(fl.check && fl.h > 300, `the flasher: "What is on my FM-1?" there; the frame sized to it (${fl.h | 0} px)`);
  await shot("desktop");

  // phone
  await size(390, 844, true);
  ok(await go(URL0), "phone: the page loads");
  await sleep(500);
  const ph = await js(`(() => { const s = document.querySelector(".arch-svg"), img = document.querySelector(".arch-fm1").getBoundingClientRect(), n = document.querySelector('[data-node="fm1"]').getBoundingClientRect();
    return { scroll: document.documentElement.scrollWidth <= innerWidth + 1, vb: s.getAttribute("viewBox"),
             wide: [...document.querySelectorAll("body *")].filter((e) => e.getBoundingClientRect().right > innerWidth + 1 && getComputedStyle(e).position !== "fixed").map((e) => e.className || e.tagName).slice(0, 5),
             inside: img.left >= n.left - 1 && img.right <= n.right + 1 && img.top >= n.top - 1 && img.bottom <= n.bottom + 1, imgw: img.width,
             lbl: Math.min(...[...document.querySelectorAll(".arch-svg .lbl")].filter((t) => getComputedStyle(t).display !== "none").map((t) => t.getBoundingClientRect().height)) }; })()`);
  ok(ph.scroll, `phone: no sideways scroll${ph.wide.length ? " (" + ph.wide.join(", ") + ")" : ""}`);
  ok(ph.vb === "0 0 360 662", `phone: the diagram stacked (viewBox ${ph.vb})`);
  ok(ph.inside && (ph.imgw === 64 || ph.imgw === 128), `phone: the pixel FM-1 (${ph.imgw} px) inside its node`);
  ok(ph.lbl >= 10, `phone: the diagram's labels readable (${ph.lbl.toFixed(1)} px tall)`);
  await shot("phone");

  ok(!exceptions.length, `no page exceptions${exceptions.length ? ": " + exceptions[0] : ""}`);
  ok(!errors.length, `no console errors${errors.length ? ": " + errors[0] : ""}`);
} catch (e) {
  fails++;
  console.log(`FAIL ${e.message}`);
}
clearTimeout(hard);
console.log(`\nmovepage-e2e: ${checks - fails}/${checks} ok${fails ? `, ${fails} FAILED` : ""}`);
stop();
process.exit(fails ? 1 : 0);
