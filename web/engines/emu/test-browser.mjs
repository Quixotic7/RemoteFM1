// The emu engine in headless Chromium, in a real AudioWorklet (test-browser.html, test-worklet.js):
// the page fetches the package from the Hub's mirror (the runtime path, CORS included), boots it,
// presses a key, and reports levels and the engine's load. Audio is muted (--mute-audio).
//   node web/engines/emu/test-browser.mjs [--fw=felucca] [--mhz=100]
// Serves the worktree with python3 -m http.server on 127.0.0.1, drives Chromium over the DevTools
// protocol, stops both by PID. Writes web/build/test/emu-browser-<fw>.png and .json.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const BUILD = new URL("../../build/", import.meta.url);
const arg = (k, d) => process.argv.find((a) => a.startsWith(`--${k}=`))?.split("=")[1] ?? d;
const FW = arg("fw", "felucca"), MHZ = arg("mhz", "100");
const CHROMIUM = "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

mkdirSync(new URL("test/", BUILD), { recursive: true });
const profile = mkdtempSync(fileURLToPath(new URL("chromium-profile-", BUILD)));
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=600,700",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", `--remote-debugging-port=${DEBUG}`, "about:blank"],
{ stdio: "ignore" });
const stop = () => { try { chrome.kill(); } catch {} try { server.kill(); } catch {} };
process.on("exit", stop);

let result;
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
  const console_ = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.consoleAPICalled") console_.push(d.params.args.map((a) => a.value ?? a.description).join(" "));
    if (d.method === "Runtime.exceptionThrown") console_.push("exception: " + JSON.stringify(d.params.exceptionDetails).slice(0, 400));
  };
  const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  await cdp("Runtime.enable");
  await cdp("Page.enable");
  const url = `http://127.0.0.1:${PORT}/web/engines/emu/test-browser.html?fw=${FW}&mhz=${MHZ}`;
  await cdp("Page.navigate", { url });
  let r;
  for (let i = 0; i < 100; i++) {
    await sleep(300);
    r = await cdp("Runtime.evaluate", { expression: "window.__result", awaitPromise: true, returnByValue: true, timeout: 60000 });
    if (r.result?.result?.value) break;
  }
  result = r.result?.result?.value ?? { error: "no result", raw: r };
  const shot = await cdp("Page.captureScreenshot", { format: "png" });
  if (shot.result?.data) writeFileSync(new URL(`test/emu-browser-${FW}.png`, BUILD), Buffer.from(shot.result.data, "base64"));
  result.console = console_;
  ws.close();
} finally {
  stop();
}
writeFileSync(new URL(`test/emu-browser-${FW}.json`, BUILD), JSON.stringify(result, null, 1));
console.log(JSON.stringify(result, null, 1));
const ok = !result.error && result.bootLitAt !== null && result.key > Math.max(4 * result.quiet, 2e-3) && !result.halted;
console.log(ok ? "PASS" : "FAIL");
process.exit(ok ? 0 : 1);
