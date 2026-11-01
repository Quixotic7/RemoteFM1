// SPDX-License-Identifier: GPL-3.0-only
// Fantasy CFW in the player, end to end (FM1VST docs/VIRTUAL-CARTS.md): this tree's site (the dev layout, served from
// the repo root) in headless Chromium (no window, a fresh profile, the browser and the server ended by their PIDs; a
// real AudioContext and AudioWorklet, muted). The cart: web/engines/rfm1/fixtures/fs_cart.wasm with a Fantasy CFW
// manifest, listed in the catalog as one of this site's .rfm1 files (catalog.json and the file answered through the
// DevTools protocol). Its demo READs /code/hello.txt and WRITEs "got:<text>" to /data/out.txt: red screen and silence
// without the file, green and a square wave once written.
//   1. no folder: the prompt "Pick your RemoteFM1 folder", the cart red (FS_ENOFOLDER)
//   2. Pick... (showDirectoryPicker answered with a handle made here: the picker can't be clicked headless): the cart
//      reads the picked folder's code/fscart/hello.txt and writes its data/fscart/out.txt; green, sound
//   3. the badge (the catalog card, the sheet's firmware card, About) and the sheet's RemoteFM1 folder section
//   4. a reload: the picked folder comes back from IndexedDB
//   5. this site's storage (OPFS, what Safari and Firefox use): Add files, a drop on the folder box, Export data (a zip)
//   6. the cart opened from a file (Your firmware): the badge there too
//   7. fm1-ripple (when its .rfm1 is built): a droplet from the folder (code/ripple/droplets/) in its list, and sound
//   node web/site/tools/fantasy-e2e.mjs [--shots=web/build/test] [--ripple=<ripple .rfm1>]
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, readdirSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, basename } from "node:path";
import { fileURLToPath } from "node:url";
import { rfm1Entry } from "../js/live.js";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(ROOT, "web/build/test"));
const RIPPLE = arg("ripple", "/Volumes/Q7Media-2025/Projects/Github/ChoralRootFM1/FM1VST/.claude/worktrees/virtual-carts/build-rfm1/rfm1/ripple-0.4.rfm1");
const DROPLETS = arg("droplets", "/Volumes/Q7Media-2025/Projects/Github/ChoralRootFM1/FM1VST/.claude/worktrees/virtual-carts/cores/fm1-ripple/builtin/droplets");
mkdirSync(SHOTS, { recursive: true });
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(110)} ${c ? "ok" : "FAIL"}`); return c; };

// the cart: the fixture with its manifest
const leb = (v) => { const o = []; do { let c = v & 0x7f; v = Math.floor(v / 128); if (v) c |= 0x80; o.push(c); } while (v); return o; };
const MAN = { rfm1: 1, abi: 6, id: "fscart", name: "FS Cart", version: "1.0", license: "MIT", source_url: "https://github.com/Quixotic7/RemoteFM1@0000000000000000000000000000000000000000",
  authors: ["RemoteFM1 tests"], virtual: true, needs: ["fs1"], folders: { audio: "r", code: "r", data: "rw" } };
const CART = (() => {
  const b = readFileSync(join(ROOT, "web/engines/rfm1/fixtures/fs_cart.wasm")), nm = Buffer.from("rfm1"), js = Buffer.from(JSON.stringify(MAN));
  const body = [...leb(nm.length), ...nm, ...js];
  return Buffer.from([...b, 0, ...leb(body.length), ...body]);
})();
const SHA = createHash("sha256").update(CART).digest("hex");
const ENTRY = rfm1Entry({ file: "fw/fscart-1.0.rfm1", author: "tests", category: "Other", tag: "test cart", what: "A Fantasy CFW for the tests: it reads and writes its folder." },
  MAN, { sha256: SHA, size: CART.length });

const tmp = mkdtempSync(join(tmpdir(), "fantasy-e2e-")), profile = join(tmp, "profile");
const cartFile = join(tmp, "fscart-1.0.rfm1");
writeFileSync(cartFile, CART);
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
  while (Date.now() - t0 < ms) { if (await js(`(async () => !!(await (${expr})))()`)) return true; await sleep(100); }
  console.log("  page:", JSON.stringify(await js(`({ cur: window.FM1Site ? (({ entry, engine, info }) => ({ id: entry && entry.id, engine, info: !!info }))(FM1Site.current()) : null,
    folder: window.FM1Folder && { mode: FM1Folder.mode, state: FM1Folder.state, name: FM1Folder.name },
    toast: document.getElementById("toast")?.textContent, status: document.getElementById("status")?.textContent })`)));
  throw new Error(`timed out: ${what}`);
}
async function shot(name) {
  const r = await cdp("Page.captureScreenshot", { format: "png" });
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from(r.result.data, "base64"));
  console.log(`  shot ${f}`);
}
const esc = () => cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Escape", key: "Escape", windowsVirtualKeyCode: 27 });
const running = (fw) => `window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "rfm1" && FM1Site.current().info.id === "${fw}"`;
// the frames (the first pixel) and the output's level (an AnalyserNode on the worklet node)
const WATCH = `(() => { const p = FM1Rec.o.getPlayer(); if (window.__w) clearInterval(window.__w.timer); const W = window.__w = { frames: 0, px0: -1, peaks: [], errors: [] };
  const ff = p.ev.__frame || p.ev.frame; p.ev.__frame = ff; p.ev.frame = (px, seq) => { W.frames++; W.px0 = px[0]; if (ff) ff(px, seq); };
  const fe = p.ev.__error || p.ev.error; p.ev.__error = fe; p.ev.error = (m) => { W.errors.push(String(m)); if (fe) fe(m); };
  const an = p.ctx.createAnalyser(); an.fftSize = 2048; p.node.connect(an);
  const buf = new Float32Array(an.fftSize);
  W.timer = setInterval(() => { an.getFloatTimeDomainData(buf); let m = 0; for (const v of buf) m = Math.max(m, Math.abs(v)); W.peaks.push({ t: performance.now(), peak: m }); }, 40);
  return true; })()`;
const peakSince = async (ms) => js(`(() => { const t = performance.now() - ${ms}; return Math.max(0, ...window.__w.peaks.filter((p) => p.t > t).map((p) => p.peak)); })()`);
// a file in this site's storage (OPFS), by its path from the storage's root; null if it isn't there
const opfsText = (p) => `(async () => { try { let d = await navigator.storage.getDirectory(); const s = ${JSON.stringify(p)}.split("/");
  for (const x of s.slice(0, -1)) d = await d.getDirectoryHandle(x); return await (await (await d.getFileHandle(s[s.length - 1])).getFile()).text(); } catch (e) { return null; } })()`;
const opfsPut = (p, text) => `(async () => { let d = await navigator.storage.getDirectory(); const s = ${JSON.stringify(p)}.split("/");
  for (const x of s.slice(0, -1)) d = await d.getDirectoryHandle(x, { create: true });
  const w = await (await d.getFileHandle(s[s.length - 1], { create: true })).createWritable(); await w.write(${JSON.stringify(text)}); await w.close(); return true; })()`;
const sheetText = () => js(`document.querySelector(".sheet-body").innerText`);

try {
  let served = false;
  for (let i = 0; i < 50 && !served; i++) { await sleep(200); try { served = "rfm1" in (await (await fetch(`http://127.0.0.1:${PORT}/web/site/catalog.json`)).json()).paths.engines; } catch {} }
  if (!served) throw new Error(`no catalog.json with paths.engines.rfm1 at http://127.0.0.1:${PORT}/web/site/`);
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const exceptions = [];
  ws.onmessage = async (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") { exceptions.push(JSON.stringify(d.params.exceptionDetails).slice(0, 400)); console.error("exception:", exceptions.at(-1)); }
    // the catalog with the cart in it, and the cart's file
    if (d.method === "Fetch.requestPaused") {
      const { requestId, request } = d.params;
      let body = null;
      if (/\/catalog\.json(\?|$)/.test(request.url)) {
        const c = await (await fetch(request.url)).json();
        c.firmwares.unshift(ENTRY);
        body = Buffer.from(JSON.stringify(c));
      } else if (/\/fw\/fscart-1\.0\.rfm1$/.test(request.url)) body = CART;
      if (body) cdp("Fetch.fulfillRequest", { requestId, responseCode: 200, responseHeaders: [{ name: "Content-Type", value: /json/.test(request.url) ? "application/json" : "application/wasm" }], body: body.toString("base64") });
      else cdp("Fetch.continueRequest", { requestId });
    }
  };
  await cdp("Runtime.enable"); await cdp("Page.enable"); await cdp("DOM.enable");
  await cdp("Fetch.enable", { patterns: [{ urlPattern: "*catalog.json*" }, { urlPattern: "*/fw/fscart-1.0.rfm1" }] });
  const base = `http://127.0.0.1:${PORT}/web/site/`;
  await cdp("Page.navigate", { url: `${base}?fw=fscart&view=screen` });
  await until(`window.FM1Rec && window.FM1Folder && ${running("fscart")}`, 60000, "the cart running");
  const info = await js(`(({ info }) => ({ virtual: info.virtual, needs: info.needs, folders: info.folders, mem: info.memoryBytes }))(FM1Site.current())`);
  ok(info.virtual === true && info.needs.join() === "fs1" && info.folders.data === "rw", `the cart boots on the rfm1 engine as a Fantasy CFW (needs ${info.needs}, folders ${JSON.stringify(info.folders)})`);
  await js(WATCH);

  // 1. no folder yet
  ok(await js(`FM1Folder.mode === "picker" && FM1Folder.state === "none"`), "Chromium: the folder is a picked one (showDirectoryPicker), none picked yet");
  await until(`FM1Site.screen().pixels[0] === 0xf800`, 5000, "the cart's red screen");
  await until(`!document.querySelector(".folder-ask").hidden`, 5000, "the prompt");
  const ask = await js(`document.querySelector(".folder-ask").innerText.replace(/\\s+/g, " ")`);
  ok(/Pick your RemoteFM1 folder/.test(ask) && /Pick\.\.\./.test(ask) && (await peakSince(800)) === 0, `without a folder: red, silent, and the prompt "${ask}"`);
  await shot("fantasy-prompt");

  // 2. Pick... (the picker answered with a folder made here, in this site's storage)
  await js(opfsPut("picked/code/fscart/hello.txt", "picked"));
  await js(`(window.showDirectoryPicker = async () => (await navigator.storage.getDirectory()).getDirectoryHandle("picked"), true)`);
  await js(`(document.querySelector(".folder-ask .pill.primary").click(), true)`);
  await until(`FM1Folder.state === "ready" && FM1Folder.name === "picked"`, 5000, "the folder picked");
  await until(opfsText("picked/data/fscart/out.txt"), 8000, "the cart's write");
  const out1 = await js(opfsText("picked/data/fscart/out.txt"));
  await until(`FM1Site.screen().pixels[0] === 0x07e0`, 5000, "the cart's green screen");
  await sleep(500);
  const pk1 = await peakSince(400);
  ok(out1 === "got:picked" && pk1 > 0.02 && await js(`document.querySelector(".folder-ask").hidden`),
     `picked: the cart read code/fscart/hello.txt and wrote data/fscart/out.txt "${out1}"; green, sound (peak ${pk1.toFixed(3)}); the prompt gone`);
  await shot("fantasy-picked");

  // 3. the badge and the sheet's folder section
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await sleep(500);
  const main = await sheetText();
  ok(await js(`!!document.querySelector(".sheet-body .fw-card .fantasy-badge")`) && /RemoteFM1 folder/i.test(main) && /picked/.test(main) && /reads audio, code, data · writes data/.test(main) && /Change\.\.\./.test(main),
     "the sheet: the firmware card's Fantasy CFW badge; RemoteFM1 folder: picked, ready, what it reads and writes, Change...");
  await shot("fantasy-sheet");
  await js(`(document.querySelector(".fw-card").click(), true)`);
  await sleep(500);
  ok(await js(`!!document.querySelector('.card[data-id="fscart"] .fantasy-badge') && document.querySelectorAll(".card .fantasy-badge").length === 1`), "the firmware browser: the catalog card has the badge (and no other card)");
  await shot("fantasy-browser");
  await esc(); await sleep(300);
  await js(`(document.querySelector(".fw-card.slim").click(), true)`);
  await sleep(500);
  ok(await js(`!!document.querySelector(".sheet-body h3 .fantasy-badge")`) && /in RemoteFM1 only, not on an FM-1/.test(await sheetText()), "About: the badge by its name, and where it runs");
  await esc(); await esc(); await sleep(300);

  // 4. a reload: the picked folder from IndexedDB
  await js("(window.__before = true)"); await cdp("Page.reload", {});
  await until(`!window.__before && window.FM1Folder && ${running("fscart")}`, 60000, "the cart after the reload");
  await js("FM1Folder.ready");
  const after = await js(`({ state: FM1Folder.state, name: FM1Folder.name, mode: FM1Folder.mode })`);
  ok(after.mode === "picker" && after.name === "picked" && (after.state === "ready" || after.state === "ask"), `after a reload the picked folder is back from IndexedDB (state ${after.state})`);

  // 5. this site's storage (Safari, Firefox): Add files, a drop, Export data
  await js(WATCH);
  await js(`FM1Folder.useOpfs()`);
  ok(await js(`FM1Folder.mode === "opfs" && FM1Folder.state === "ready" && FM1Folder.name === "RemoteFM1"`), "this site's storage (OPFS): RemoteFM1/, ready at once");
  const n = await js(`FM1Folder.addFiles([{ file: new File(["opfs"], "hello.txt"), path: "hello.txt" }, { file: new File([new Uint8Array(64)], "kick.wav"), path: "kit/kick.wav" },
    { file: new File(["x"], ".DS_Store"), path: ".DS_Store" }, { file: new File(["s"], "s.wav"), path: "RemoteFM1/audio/s.wav" }], "fscart")`);
  await until(`(await ${opfsText("RemoteFM1/data/fscart/out.txt")}) === "got:opfs"`, 8000, "the cart's write into this site's storage");
  ok(n === 3 && await js(`(async () => (await ${opfsText("RemoteFM1/audio/kit/kick.wav")}) !== null && (await ${opfsText("RemoteFM1/audio/s.wav")}) === "s" && (await ${opfsText("RemoteFM1/code/fscart/hello.txt")}) === "opfs" && (await ${opfsText("RemoteFM1/code/fscart/.DS_Store")}) === null)()`),
     "Add files: 3 copied (a sample to audio/kit/, a RemoteFM1/audio/ path kept, the rest to code/fscart/; no hidden file); the cart wrote got:opfs");
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await sleep(500);
  const opfsMain = await sheetText();
  ok(/in this browser/.test(opfsMain) && /Add folder\.\.\./.test(opfsMain) && /Add files\.\.\./.test(opfsMain) && /Export data/.test(opfsMain), "the sheet: in this browser, Add folder..., Add files..., Export data");
  await shot("fantasy-sheet-opfs");
  await js(`(() => { const dt = new DataTransfer(); dt.items.add(new File(["dropped"], "hello.txt"));
    document.querySelector(".folder-box").dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true })); return true; })()`);
  await until(`(await ${opfsText("RemoteFM1/data/fscart/out.txt")}) === "got:dropped"`, 8000, "the dropped file read");
  ok(true, "a file dropped on the folder box: copied to code/fscart/, the cart read it again (fs_changed) and wrote got:dropped");
  const zip = await js(`(async () => { const made = []; const o = URL.createObjectURL; URL.createObjectURL = (b) => { made.push(b); return o.call(URL, b); };
    document.querySelector(".folder-box .pill.ghost").click(); for (let i = 0; i < 50 && !made.length; i++) await new Promise((r) => setTimeout(r, 50));
    URL.createObjectURL = o; if (!made.length) return null; const b = new Uint8Array(await made[0].arrayBuffer()); const t = new TextDecoder().decode(b);
    return { pk: b[0] === 0x50 && b[1] === 0x4b && b[2] === 3 && b[3] === 4, name: t.includes("data/fscart/out.txt"), body: t.includes("got:dropped"), size: b.length }; })()`);
  ok(zip && zip.pk && zip.name && zip.body, `Export data: a zip (${zip && zip.size} bytes) with data/fscart/out.txt`);
  await esc(); await sleep(300);

  // 6. the cart from a file (Your firmware)
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await until(`document.querySelector('input[type=file][accept=".rfm1,.fwsc"]')`, 5000, "the Open a file... input");
  const { nodeId } = (await cdp("DOM.querySelector", { nodeId: (await cdp("DOM.getDocument", { depth: 1 })).result.root.nodeId, selector: 'input[type=file][accept=".rfm1,.fwsc"]' })).result;
  await cdp("DOM.setFileInputFiles", { nodeId, files: [cartFile] });
  await until(`${running("fscart")} && FM1Site.current().entry.local`, 30000, "the cart from the file");
  await js(`(document.getElementById("fw-btn").click(), true)`);
  await sleep(500);
  ok(await js(`!!document.querySelector(".cards.mine .card[data-sha] .fantasy-badge")`), "opened from a file: Your firmware's card has the badge");
  await shot("fantasy-mine");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(400);
  await esc(); await sleep(200);
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await sleep(500);
  ok(await js(`document.documentElement.scrollWidth <= innerWidth + 1`), "390 px wide, the sheet with the folder section open: no horizontal page scroll");
  await shot("fantasy-sheet-phone");
  await cdp("Emulation.clearDeviceMetricsOverride", {});
  await esc(); await sleep(300);

  // 7. fm1-ripple, with a droplet from the folder
  if (existsSync(RIPPLE)) {
    const drops = existsSync(DROPLETS) ? readdirSync(DROPLETS).filter((f) => f.endsWith(".lua")) : [];
    const src = drops.includes("drone.lua") ? "drone.lua" : drops[0];
    if (!src) ok(false, `ripple: no droplet to copy in ${DROPLETS}`);
    else {
      await js(opfsPut("RemoteFM1/code/ripple/droplets/aaweb.lua", readFileSync(join(DROPLETS, src), "utf8")));
      await js(`(FM1Folder.changed(), true)`);
      await js(`(document.getElementById("fw-btn").click(), true)`);
      await until(`document.querySelector('input[type=file][accept=".rfm1,.fwsc"]')`, 5000, "the Open a file... input");
      const nid = (await cdp("DOM.querySelector", { nodeId: (await cdp("DOM.getDocument", { depth: 1 })).result.root.nodeId, selector: 'input[type=file][accept=".rfm1,.fwsc"]' })).result.nodeId;
      await cdp("DOM.setFileInputFiles", { nodeId: nid, files: [RIPPLE] });
      await until(running("ripple"), 60000, "ripple running");
      await js(WATCH);
      await sleep(4000);
      await shot("fantasy-ripple");
      const rp = await peakSince(3000);
      console.log(`  ripple: ${basename(RIPPLE)} running; the droplet aaweb.lua (a copy of ${src}) is in RemoteFM1/code/ripple/droplets/; peak ${rp.toFixed(3)}: look at fantasy-ripple.png for the list`);
      ok(!(await js(`window.__w.errors.length`)), `ripple boots with the folder, no worklet errors (peak ${rp.toFixed(3)}; the droplet list: see the screenshot)`);
      // the folder's droplet sorts first ("aaweb"): PLAY (the "-" key) starts it, and it sounds
      await cdp("Input.dispatchKeyEvent", { type: "keyDown", code: "Minus", key: "-", windowsVirtualKeyCode: 189 });
      await sleep(150);
      await cdp("Input.dispatchKeyEvent", { type: "keyUp", code: "Minus", key: "-", windowsVirtualKeyCode: 189 });
      await sleep(5000);
      await shot("fantasy-ripple-playing");
      const pp = await peakSince(3000);
      ok(pp > 0.001 && !(await js(`window.__w.errors.length`)), `ripple plays the folder's droplet (a copy of ${src}): peak ${pp.toFixed(3)}`);
    }
  } else console.log(`  ripple: ${RIPPLE} is not built yet (step 7 skipped)`);

  ok(!exceptions.length, `no exceptions in the page${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
} catch (e) {
  console.error(e.stack || e);
  fails++;
} finally {
  stop();
}
console.log(fails ? `fantasy e2e: ${fails} of ${checks} FAILED` : `fantasy e2e passed (${checks})`);
process.exit(fails ? 1 : 0);
