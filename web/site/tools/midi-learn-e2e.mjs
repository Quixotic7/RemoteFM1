// SPDX-License-Identifier: GPL-3.0-only
// MIDI learn (js/midi-map.js) end to end in headless Chromium (no window, a fresh profile, the browser and the server
// ended by their PIDs) on a native firmware (Felucca), with a mocked Web MIDI: one input named "OMX-27" whose messages
// the test sends (no real MIDI device is touched). The sheet's Enable MIDI, then Learn: KN1 clicked on the canvas and
// CC 21 moved binds it (absolute); after Done CC 21 turns KN1 in what the page posts to the engine (the worklet's
// "panel" messages). MASTER from CC 22 with its pickup (a CC far from MASTER moves nothing), HOME from a note; a
// learned note plays no key while another still does; the mappings and MIDI itself come back after a reload.
//   node web/site/tools/midi-learn-e2e.mjs [--shots=web/build/test]
// Needs web/build/native/felucca.wasm. Writes midi-learn-desktop.png, midi-learn-phone.png, midi-learn-sheet.png.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join } from "node:path";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const WEB = join(ROOT, "web");
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(WEB, "build", "test"));
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
if (!existsSync(join(WEB, "build/native/felucca.wasm"))) { console.error("missing web/build/native/felucca.wasm"); process.exit(2); }
mkdirSync(SHOTS, { recursive: true });

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const profile = mkdtempSync(join(WEB, "build", "chromium-profile-learn-"));
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
const mouse = (type, p) => cdp("Input.dispatchMouseEvent", { type, x: p.x, y: p.y, button: type === "mouseMoved" ? "none" : "left", buttons: type === "mousePressed" ? 1 : 0, clickCount: 1 });
const click = async (p) => { await mouse("mouseMoved", p); await mouse("mousePressed", p); await sleep(50); await mouse("mouseReleased", p); await sleep(150); };
const keyEv = (type, code, key, vk) => cdp("Input.dispatchKeyEvent", { type, code, key, windowsVirtualKeyCode: vk });
async function shot(name) {
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from((await cdp("Page.captureScreenshot", { format: "png" })).result.data, "base64"));
  console.log(`  shot ${f}`);
}

// Web MIDI, mocked before the page's scripts: one input, "OMX-27"; window.__send(bytes) is a message from it. The
// 'midi' permission reads granted (as after a first allow), so a reload turns MIDI on by itself.
const MIDI_MOCK = `(() => {
  const input = { id: "omx27-in", name: "OMX-27", manufacturer: "Denki-Oto", type: "input", state: "connected", connection: "open",
    onmidimessage: null, addEventListener() {}, removeEventListener() {}, open: async () => input, close: async () => input };
  const access = { inputs: new Map([[input.id, input]]), outputs: new Map(), sysexEnabled: false, onstatechange: null, addEventListener() {}, removeEventListener() {} };
  window.__send = (b) => { if (input.onmidimessage) input.onmidimessage({ data: new Uint8Array(b), timeStamp: performance.now() }); return !!input.onmidimessage; };
  Object.defineProperty(navigator, "requestMIDIAccess", { value: async () => access, configurable: true });
  const real = navigator.permissions;
  Object.defineProperty(navigator, "permissions", { value: { query: async (d) => (d && d.name === "midi" ? { state: "granted", onchange: null, addEventListener() {} } : real.query(d)) }, configurable: true });
})();`;
// the controls' centres in the page's CSS pixels (a DeviceView laid out as the page's; hit() confirms each)
const WHERE = `(async () => {
  const P = await import("/web/site/js/panel.js");
  const cv = document.getElementById("device"), r = cv.getBoundingClientRect();
  const dv = new P.DeviceView({ getContext: () => null, width: 0, height: 0 }, new P.Controls(() => {}), null);
  dv.layout(cv.clientWidth, cv.clientHeight, devicePixelRatio);
  const at = (kind, idx) => { const c = dv.where(kind, idx), h = dv.hit(c.x, c.y); return { x: r.left + c.x, y: r.top + c.y, ok: !!h && h.kind === kind && h.idx === idx }; };
  return { kn1: at(P.KM.SELECT, P.E.K1), master: at(P.KM.SELECT, P.E.MASTER), home: at(P.KM.BTN, P.B.HOME),
    kn1Enc: P.roleToMatrix(P.E.K1), homeBit: P.labelToMatrix(P.B.HOME) };
})()`;
// what the page posts to the engine: the panel messages, their encoder detents summed (matrix order)
const HOOK = `(() => { const p = FM1Rec.o.getPlayer(), port = p.node.port, f = port.postMessage.bind(port);
  window.__panel = null; window.__enc = [0, 0, 0, 0, 0, 0, 0];
  port.postMessage = (m, t) => { if (m && m.type === "panel") { window.__panel = { keys: m.keys, buttons: m.buttons, master: m.master }; m.enc.forEach((v, i) => { window.__enc[i] += v; }); } return f(m, t); };
  return true; })()`;
const resetEnc = () => js("(window.__enc = [0, 0, 0, 0, 0, 0, 0], true)");
const send = async (...msgs) => { for (const m of msgs) { await js(`window.__send(${JSON.stringify(m)})`); await sleep(15); } await sleep(120); };
const cc = (n, v) => [0xB0, n, v], on = (n, v = 100) => [0x90, n, v], off = (n) => [0x80, n, 0];
const toastText = () => js(`document.getElementById("toast").textContent`);
const stored = () => js(`(JSON.parse(localStorage.getItem("remotefm1-player") || "{}").midiMap || [])`);
const bit = (v, b) => ((v >>> b) & 1) === 1;
const running = `window.FM1Rec && window.FM1Site && FM1Site.current().info && FM1Site.current().engine === "native"`;

try {
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const errors = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") { errors.push(JSON.stringify(d.params.exceptionDetails).slice(0, 400)); console.error("exception:", errors.at(-1)); }
  };
  await cdp("Runtime.enable"); await cdp("Page.enable");
  await cdp("Page.addScriptToEvaluateOnNewDocument", { source: MIDI_MOCK });
  await cdp("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 2, mobile: false });
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=felucca&engine=native` });
  await until(running, 90000, "native Felucca running");
  await sleep(1200);
  await js(HOOK);
  let W = await js(WHERE);
  ok(W.kn1.ok && W.master.ok && W.home.ok, "the points: KN1, MASTER and HOME where the panel draws them");

  // 1. MIDI on from the sheet
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open")`, 5000, "the sheet");
  ok(await js(`Array.from(document.querySelectorAll(".sheet .pill")).some((b) => b.textContent === "Enable MIDI")`), "a first visit: MIDI input is off until Enable MIDI");
  await js(`(Array.from(document.querySelectorAll(".sheet .pill")).find((b) => b.textContent === "Enable MIDI").click(), true)`);
  await until(`document.querySelector('.sheet select[aria-label="MIDI input"]')`, 5000, "the MIDI input picker");
  const picked = await js(`(() => { const s = document.querySelector('.sheet select[aria-label="MIDI input"]'); return { v: s.value, names: Array.from(s.options, (o) => o.textContent) }; })()`);
  ok(picked.v === "*" && picked.names.includes("OMX-27"), `Enable MIDI: the OMX-27 is listed, all inputs on (${picked.names.join(", ")})`);
  ok(await js(`!!document.querySelector(".sheet .learn-pill")`) && !(await js(`!!document.querySelector(".sheet .map-list")`)), "the Playing section has Knobs from MIDI with a Learn pill, no mappings yet");
  // an unlearned CC reaches nothing new; an unlearned note plays its key (as before)
  await send(on(62));
  let p = await js("window.__panel");
  ok(bit(p.keys, 62 - 53), "before learning: a note from the OMX-27 plays its key");
  await send(off(62));

  // 2. learn KN1 from CC 21
  await js(`(document.querySelector(".sheet .learn-pill").click(), true)`);
  await until(`!document.getElementById("learn").hidden && !document.querySelector(".sheet.open")`, 5000, "learn mode");
  ok(/Click a knob or button, then move a control on your MIDI device/.test(await js(`document.getElementById("learn").innerText`)), "Learn: the sheet closes, the bar says what to do");
  await resetEnc();
  await click(W.kn1);
  await sleep(200);
  ok(/^KN1: now turn a knob/.test(await js(`document.getElementById("learn-text").textContent`)), "clicking KN1 picks it");
  let enc = await js("window.__enc");
  ok(enc.every((v) => v === 0), "the click turns nothing");
  await send(cc(21, 40), cc(21, 41), cc(21, 42), cc(21, 43));
  const t1 = await toastText();
  let maps = await stored();
  enc = await js("window.__enc");
  ok(maps.length === 1 && maps[0].src === "cc:1:21" && maps[0].target === "KN1" && maps[0].mode === "abs" && /^KN1 ← CC 21,\schannel\s1 \(absolute\)$/.test(t1),
     `CC 21 binds to KN1, absolute: "${t1}"`);
  ok(enc.every((v) => v === 0), "while learning, CC 21 turns nothing");
  await sleep(500);
  await shot("midi-learn-desktop");

  // 3. learn MASTER from CC 22, HOME from a note
  await click(W.master);
  await send(cc(22, 90));
  await click(W.home);
  await send(on(60), off(60));
  maps = await stored();
  ok(maps.length === 3 && maps.some((m) => m.src === "cc:1:22" && m.target === "MASTER" && !m.mode) && maps.some((m) => m.src === "note:1:60" && m.target === "HOME"),
     `MASTER from CC 22 and HOME from note 60 (${maps.map((m) => `${m.target}<-${m.src}`).join(", ")})`);
  p = await js("window.__panel");
  ok(!bit(p.buttons, W.homeBit) && !bit(p.keys, 60 - 53), "the learning note pressed nothing and played no key");
  // phone, still learning
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(600);
  const fit = await js(`(() => { const b = document.getElementById("learn").getBoundingClientRect(); return { l: b.left, r: b.right, w: innerWidth, scroll: document.documentElement.scrollWidth }; })()`);
  ok(fit.l >= 8 && fit.r <= fit.w - 8 && fit.scroll <= fit.w + 1, `at 390 px the bar fits (${Math.round(fit.l)}..${Math.round(fit.r)} of ${fit.w}), no page scroll`);
  await shot("midi-learn-phone");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 2, mobile: false });
  await sleep(500);
  W = await js(WHERE);

  // 4. Done; CC 21 turns KN1
  await js(`(document.getElementById("learn-done").click(), true)`);
  await sleep(150);
  ok(await js(`document.getElementById("learn").hidden`), "Done ends learn mode");
  await resetEnc();
  await send(...[44, 45, 46, 47, 48, 49, 50, 51, 52, 53].map((v) => cc(21, v)));
  enc = await js("window.__enc");
  ok(enc[W.kn1Enc] === 10 && enc.reduce((a, v) => a + Math.abs(v), 0) === 10, `CC 21 rising 44..53 turns KN1 by 10 detents in the engine's input (${enc.join(" ")})`);
  await send(cc(21, 50));
  enc = await js("window.__enc");
  ok(enc[W.kn1Enc] === 7, "and back down 3");

  // 5. MASTER: a CC far from it moves nothing (pickup), then it follows
  const m0 = (await js("window.__panel")).master;
  await send(cc(22, 10));
  p = await js("window.__panel");
  const t2 = await toastText();
  ok(p.master === m0 && /^MASTER: turn the pot to \d+% to pick it up$/.test(t2), `CC 22 at 10, far from MASTER (${m0}): MASTER stays, "${t2}"`);
  const at = Math.round(m0 * 127 / 1023);
  await send(cc(22, at - 6), cc(22, at - 3), cc(22, at + 1));
  await send(cc(22, 64));
  p = await js("window.__panel");
  ok(p.master === Math.round(64 * 1023 / 127), `once it reaches MASTER (${at}) it picks it up, then follows: CC 64 -> MASTER ${p.master}`);

  // 6. HOME from the note; the note plays no key, another note still does
  await send(on(60));
  p = await js("window.__panel");
  ok(bit(p.buttons, W.homeBit) && !bit(p.keys, 60 - 53), "note 60 presses HOME and plays no key");
  await send(off(60));
  p = await js("window.__panel");
  ok(!bit(p.buttons, W.homeBit), "its note-off lets HOME go");
  await send(on(62));
  p = await js("window.__panel");
  ok(bit(p.keys, 62 - 53) && !bit(p.buttons, W.homeBit), "an unlearned note still plays its key");
  await send(off(62));

  // 7. the sheet's list
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open .map-list")`, 5000, "the mapping list");
  const rows = await js(`Array.from(document.querySelectorAll(".map-row"), (r) => ({ t: r.querySelector(".map-what").textContent, mode: r.querySelector("select") ? r.querySelector("select").value : null }))`);
  ok(rows.length === 3 && rows.some((r) => r.t === "KN1 ← CC 21 ch 1" && r.mode === "abs") && rows.some((r) => r.t === "MASTER ← CC 22 ch 1" && r.mode === null)
     && rows.some((r) => r.t === "HOME ← note 60 ch 1"), `the sheet lists them: ${rows.map((r) => r.t + (r.mode ? " · " + r.mode : "")).join(" | ")}`);
  await js(`(document.querySelector(".map-list").scrollIntoView({ block: "center" }), true)`);
  await sleep(400);
  await shot("midi-learn-sheet");
  // the mode select
  await js(`(() => { const s = document.querySelector('.map-row[data-src="cc:1:21"] select'); s.value = "rel64"; s.dispatchEvent(new Event("change")); return true; })()`);
  maps = await stored();
  ok(maps.find((m) => m.src === "cc:1:21").mode === "rel64", "the mode select changes the mapping (relative 64 ± n)");
  await js(`(() => { const s = document.querySelector('.map-row[data-src="cc:1:21"] select'); s.value = "abs"; s.dispatchEvent(new Event("change")); return true; })()`);
  await keyEv("keyDown", "Escape", "Escape", 27);
  await sleep(300);

  // 8. a reload: the mappings stay, MIDI comes back by itself (already allowed), CC 21 turns KN1 again
  await js("(window.__before = true)");
  await cdp("Page.reload", {});
  await until(`!window.__before && ${running}`, 90000, "Felucca again after the reload");
  await until(`window.__send([0xB0, 99, 0])`, 8000, "MIDI back on after the reload");
  await sleep(1200);
  await js(HOOK);
  W = await js(WHERE);
  maps = await stored();
  ok(maps.length === 3, "after a reload: the three mappings are still there");
  await send(cc(21, 30), cc(21, 31), cc(21, 32), cc(21, 33));
  enc = await js("window.__enc");
  ok(enc[W.kn1Enc] === 3, `after a reload: MIDI is on by itself and CC 21 turns KN1 (${enc.join(" ")})`);
  await send(on(60));
  p = await js("window.__panel");
  ok(bit(p.buttons, W.homeBit) && !bit(p.keys, 60 - 53), "after a reload: note 60 is still HOME");
  await send(off(60));

  // 9. learn again: Forget with Delete, Esc ends it; Clear all
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open .learn-pill")`, 5000, "the sheet");
  await js(`(document.querySelector(".sheet .learn-pill").click(), true)`);
  await until(`!document.getElementById("learn").hidden`, 5000, "learn mode");
  await click(W.home);
  ok(!(await js(`document.getElementById("learn-forget").hidden`)) && /^HOME ← note 60 ch 1$/.test(await js(`document.getElementById("learn-text").textContent`)), "picking a learned control shows its binding and Forget");
  await keyEv("rawKeyDown", "Delete", "Delete", 46); await keyEv("keyUp", "Delete", "Delete", 46);
  await sleep(150);
  maps = await stored();
  ok(maps.length === 2 && !maps.some((m) => m.target === "HOME"), "Delete forgets HOME's binding");
  await keyEv("rawKeyDown", "Escape", "Escape", 27);
  await sleep(150);
  p = await js("window.__panel");
  await keyEv("keyUp", "Escape", "Escape", 27);
  ok(await js(`document.getElementById("learn").hidden`) && !bit(p.buttons, 0), "Esc ends learn mode (and is not OCT-, matrix button 0)");
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open .map-foot")`, 5000, "the list");
  await js(`(document.querySelector(".map-foot .pill").click(), true)`);
  await sleep(150);
  ok((await stored()).length === 0 && !(await js(`!!document.querySelector(".map-list")`)), "Clear all empties the list");
  ok(!errors.length, `no page exceptions (${errors.length})`);
} catch (e) {
  console.error(e); fails++;
} finally {
  try { ws && ws.close(); } catch {}
  stop();
}
console.log(`\n${checks - fails}/${checks} checks passed`);
process.exit(fails ? 1 : 0);
