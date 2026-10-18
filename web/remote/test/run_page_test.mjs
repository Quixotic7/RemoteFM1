// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
//
// The headless-Chromium page test (page.html): serves web/ on 127.0.0.1, starts Chromium headless with its own
// fresh profile, reads the verdict over the DevTools protocol in real time, saves a screenshot to
// web/build/test/remote-page.png, and ends Chromium by its PID. Web MIDI and WebUSB are mocked in the page: no
// hardware is touched.
//   node web/remote/test/run_page_test.mjs        (after web/remote/build.sh; CHROMIUM=... to override)
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, extname, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB = normalize(join(dirname(fileURLToPath(import.meta.url)), '../..'));
const CHROMIUM = process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium';
const LIMIT_MS = +(process.env.LIMIT_MS || 60000);
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.json': 'application/json', '.wasm': 'application/wasm', '.png': 'image/png', '.css': 'text/css' };
const sleep = ms => new Promise(r => setTimeout(r, ms));

const server = createServer((req, res) => {
    const p = normalize(join(WEB, decodeURIComponent(new URL(req.url, 'http://x').pathname)));
    if (!p.startsWith(WEB) || !existsSync(p)) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': TYPES[extname(p)] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(readFileSync(p));
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/remote/test/page.html`;

const profile = mkdtempSync(join(process.env.TMPDIR || tmpdir(), 'rfm-page-'));
const chrome = spawn(CHROMIUM, ['--headless=new', '--use-mock-keychain', '--password-store=basic', '--disable-gpu',
    '--no-sandbox', '--no-first-run', '--hide-scrollbars', `--user-data-dir=${profile}`, '--window-size=640,760',
    '--remote-debugging-port=0', 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
let stderr = '';
chrome.stderr.on('data', d => { stderr += d; });

let code = 1;
try {
    const t0 = Date.now();
    let wsUrl = null;
    while (!wsUrl && Date.now() - t0 < 15000) {                                 // "DevTools listening on ws://.."
        const m = /DevTools listening on (ws:\/\/\S+)/.exec(stderr);
        if (m) wsUrl = m[1]; else await sleep(100);
    }
    if (!wsUrl) throw new Error('Chromium did not start');
    const port = new URL(wsUrl).port;
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const page = targets.find(t => t.type === 'page');
    const ws = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
    let id = 0;
    const pending = new Map();
    ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
    const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
    const evaluate = async expr => (await send('Runtime.evaluate', { expression: expr, returnByValue: true })).result.result.value;

    await send('Page.enable');
    await send('Page.navigate', { url });
    let title = '';
    while (Date.now() - t0 < LIMIT_MS) {
        await sleep(250);
        title = (await evaluate('document.title')) || '';
        if (/^(PASS|FAIL)/.test(title)) break;
    }
    console.log(await evaluate("document.getElementById('result') ? document.getElementById('result').textContent : ''"));
    const shot = await send('Page.captureScreenshot', { format: 'png' });
    mkdirSync(join(WEB, 'build/test'), { recursive: true });
    writeFileSync(join(WEB, 'build/test/remote-page.png'), Buffer.from(shot.result.data, 'base64'));
    console.log(`screenshot: ${join(WEB, 'build/test/remote-page.png')}`);
    if (!/^(PASS|FAIL)/.test(title)) console.log(`no verdict within ${LIMIT_MS} ms`);
    code = title.startsWith('PASS') ? 0 : 1;
    ws.close();
} catch (e) {
    console.log(`error: ${e.stack || e}`);
} finally {
    chrome.kill('SIGTERM');                                                       // by PID, never a pattern
    server.close();
}
process.exit(code);
