// SPDX-License-Identifier: GPL-3.0-only
// Live panel labels on the drawn panel, end to end (FM1VST docs/DYNAMIC-LABELS.md, docs/BUTTON-LABELS.md): this
// tree's site in headless Chromium (no window, a fresh profile; the browser and the server ended by their PIDs), the
// .rfm1 test core (web/engines/rfm1/fixtures/labels_core.rfm1) opened through "Open a file...": its live labels reach
// the button names (FX DRUMS), GLO held through the computer keys (",") changes them (FX UNDO, GLO SHIFT) and the
// panel's pixels, GLO up brings them back. (The setting's merge: web/site/test.mjs.)
//   node web/site/tools/labels-e2e.mjs [--shots=web/build/test]
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test"));
const FILE = join(ROOT, "web/engines/rfm1/fixtures/labels_core.rfm1");
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const tmp = mkdtempSync(join(tmpdir(), "labels-e2e-")), profile = join(tmp, "profile");
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
  console.log("  page:", JSON.stringify(await js(`({ cur: window.FM1Site ? (({ entry, engine, info }) => ({ id: entry && entry.id, engine, info: !!info }))(FM1Site.current()) : null,
    names: window.FM1Omx && FM1Omx.views.names.join(" "), toast: document.getElementById("toast")?.textContent })`)));
  throw new Error(`timed out: ${what}`);
}
async function shot(name) {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from(r.result.data, "base64"));
  console.log(`  shot ${f}`);
}
const glo = (type) => cdp("Input.dispatchKeyEvent", { type, code: "Comma", key: ",", windowsVirtualKeyCode: 188 });
const NAMES = "window.FM1Omx.views.names.join(' ')";
// the panel canvas's pixels, hashed (the faceplate is redrawn when a label changes)
const PIXELS = `(() => { const c = Array.from(document.querySelectorAll("canvas")).sort((a, b) => b.width * b.height - a.width * a.height)[0];
  const d = c.getContext("2d").getImageData(0, 0, c.width, c.height).data; let h = 0; for (let i = 0; i < d.length; i += 4) h = (h * 31 + d[i] + d[i + 1] * 7 + d[i + 2] * 13) >>> 0; return h; })()`;

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
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=s1vave&view=panel` });   // (the site's own .rfm1 first)
  await until(`window.FM1Site && window.FM1Omx && FM1Site.current().info`, 60000, "the page running its first firmware");
  ok(await js(NAMES) === "FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+", `an .rfm1 without live labels: ${await js(NAMES)}`);

  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector('input[type=file][accept=".rfm1,.fwsc"]')`, 5000, "the Open a file... input");
  const { root } = (await cdp("DOM.getDocument", { depth: 1 })).result;
  const { nodeId } = (await cdp("DOM.querySelector", { nodeId: root.nodeId, selector: 'input[type=file][accept=".rfm1,.fwsc"]' })).result;
  const r = await cdp("DOM.setFileInputFiles", { nodeId, files: [FILE] });
  if (r.error) throw new Error(`setFileInputFiles: ${r.error.message}`);
  await until(`FM1Site.current().info && FM1Site.current().engine === "rfm1" && FM1Site.current().info.id === "labeltest"`, 30000, "labeltest on the rfm1 engine");
  for (let i = 0; i < 2; i++) await cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
  await until(`${NAMES}.startsWith("DRUMS TRK ")`, 5000, "the live labels");
  ok(await js(NAMES) === "DRUMS TRK ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+", `the live labels over button_names: ${await js(NAMES)}`);
  await sleep(300);
  const p0 = await js(PIXELS);
  await shot("labels-up");
  await glo("keyDown");
  await until(`${NAMES}.startsWith("UNDO COPY ")`, 5000, "the labels with GLO held");
  ok(await js(NAMES) === "UNDO COPY ENV LFO EDIT SHIFT HOME SAVE ARP SEQ PLAY REC OCT- OCT+", `GLO held: ${await js(NAMES)}`);
  await sleep(300);
  const p1 = await js(PIXELS);
  await shot("labels-glo-held");
  ok(p0 !== p1, "the panel is redrawn with them");
  await glo("keyUp");
  await until(`${NAMES}.startsWith("DRUMS ")`, 5000, "the labels after GLO");
  ok(true, "GLO up: DRUMS again");
  ok(!exceptions.length, `no exceptions in the page${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
} catch (e) {
  console.error(e.stack || e);
  fails++;
} finally {
  stop();
}
console.log(fails ? `labels e2e: ${fails} of ${checks} FAILED` : `labels e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
