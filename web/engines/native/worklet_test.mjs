// SPDX-License-Identifier: GPL-3.0-only
// The native engine in a real AudioWorklet, in headless Chromium:
//
//   node web/engines/native/worklet_test.mjs [id ...]      (after build.sh; default: felucca and obervave)
//
// Serves the worktree on 127.0.0.1 (a small static server here), starts headless Chromium (no window, its own
// fresh profile in web/build/test/chromium-profile, the brief's flags) with the DevTools protocol on, opens
// web/engines/native/worklet_test.html?core=<id>, waits for its window.__result, saves a screenshot to
// web/build/test/native-worklet-<id>.png, and ends Chromium by its PID.
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "../../..");
const OUT = path.join(ROOT, "web/build/test");
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const ids = process.argv.slice(2).length ? process.argv.slice(2) : ["felucca", "obervave"];
const MIME = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm",
               ".json": "application/json", ".png": "image/png", ".css": "text/css" };

const server = http.createServer((req, res) => {
    const u = decodeURIComponent(new URL(req.url, "http://x").pathname);
    const f = path.join(ROOT, path.normalize(u));
    if (!f.startsWith(ROOT) || !fs.existsSync(f) || !fs.statSync(f).isFile()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { "Content-Type": MIME[path.extname(f)] || "application/octet-stream", "Cache-Control": "no-store" });
    fs.createReadStream(f).pipe(res);
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const port = server.address().port;

fs.mkdirSync(OUT, { recursive: true });
const profile = path.join(OUT, "chromium-profile");
fs.rmSync(profile, { recursive: true, force: true });
fs.mkdirSync(profile, { recursive: true });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
    "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=640,560",
    "--remote-debugging-port=0", "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
let chromeErr = "";
chrome.stderr.on("data", (d) => { chromeErr += d; });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
try {
    let wsUrl = null;
    for (let i = 0; i < 100 && !wsUrl; i++) {
        await sleep(100);
        const f = path.join(profile, "DevToolsActivePort");
        if (fs.existsSync(f)) {
            const [p] = fs.readFileSync(f, "utf8").split("\n");
            const list = await (await fetch(`http://127.0.0.1:${p}/json/list`)).json();
            const page = list.find((t) => t.type === "page");
            if (page) wsUrl = page.webSocketDebuggerUrl;
        }
    }
    if (!wsUrl) throw new Error("Chromium's DevTools did not come up:\n" + chromeErr);
    const ws = new WebSocket(wsUrl);
    await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
    let seq = 0;
    const pending = new Map();
    ws.onmessage = (m) => {
        const d = JSON.parse(m.data);
        if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
    };
    const cdp = (method, params = {}) => new Promise((r) => { const id = ++seq; pending.set(id, r); ws.send(JSON.stringify({ id, method, params })); });
    await cdp("Page.enable");
    for (const id of ids) {
        await cdp("Page.navigate", { url: `http://127.0.0.1:${port}/web/engines/native/worklet_test.html?core=${id}` });
        let result = null;
        const t0 = Date.now();
        while (!result && Date.now() - t0 < 60000) {
            await sleep(250);
            const r = await cdp("Runtime.evaluate", { expression: "JSON.stringify(window.__result || null)", returnByValue: true });
            const v = r.result && r.result.result && r.result.result.value;
            if (v && v !== "null") result = JSON.parse(v);
        }
        const shot = await cdp("Page.captureScreenshot", { format: "png" });
        fs.writeFileSync(path.join(OUT, `native-worklet-${id}.png`), Buffer.from(shot.result.data, "base64"));
        if (!result) { console.log(`FAIL  ${id}: no result within 60 s`); fails++; continue; }
        if (result.error) { console.log(`FAIL  ${id}: ${result.error}`); fails++; continue; }
        console.log(`${result.ok ? "ok  " : "FAIL"}  ${id} in an AudioWorklet (${result.info.name} ${result.info.version}): ` +
            `AudioWorkletGlobalScope ${result.scope.global}, SharedArrayBuffer ${result.scope.sharedArrayBuffer}, ` +
            `TextDecoder ${result.scope.textDecoder}; ${result.quanta}/${result.expectedQuanta} quanta at ${result.scope.sampleRate} Hz; ` +
            `key D4 peak ${Math.round(result.during.peak * 32768)} (before ${Math.round(result.before.peak * 32768)}); ` +
            `screen ${result.colours} colours; web/build/test/native-worklet-${id}.png`);
        if (!result.ok) fails++;
    }
    ws.close();
} catch (e) {
    console.log(`FAIL  ${e.stack || e}`);
    fails++;
} finally {
    chrome.kill("SIGTERM");
    await sleep(300);
    if (chrome.exitCode === null) chrome.kill("SIGKILL");
    server.close();
    fs.rmSync(profile, { recursive: true, force: true });
}
process.exit(fails ? 1 : 0);
