// SPDX-License-Identifier: GPL-3.0-only
// The OMX-27 in REMOTE mode (js/omx-link.js) end to end in headless Chromium (no window, a fresh profile, the browser
// and the server ended by their PIDs) on a native firmware (Felucca), against a fake OMX-27 (tools/omx-fake.js: a
// mocked Web MIDI port that speaks the protocol; no real MIDI device is touched). Settings > Use my OMX-27 > Connect:
// the MODE command, 52 02, a toast; Base: a key plays the FM-1's, 13 held + 7 picks KN2 and the encoder turns it, 14
// held + PLAY presses it; K1 to Seq: the mirrored LEDs follow the FM-1's; Keyboard: notes reach the firmware's MIDI
// in; K5 sets MASTER after its pickup; the port skipped by the MIDI keyboard input; 52 03 and back; a reload connects
// by itself; unplugged and back; Disconnect clears the OMX. Pictures of the fake OMX's LEDs and screen per view.
//   node web/site/tools/omx-e2e.mjs [--shots=web/build/test]
// Needs web/build/native/felucca.wasm. Writes omx-base.png, omx-knobs.png, omx-buttons.png, omx-seq.png,
// omx-keyboard.png, omx-quick.png and omx-sheet.png.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, existsSync } from "node:fs";
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
const FAKE = readFileSync(new URL("./omx-fake.js", import.meta.url), "utf8");

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

const profile = mkdtempSync(join(WEB, "build", "chromium-profile-omx-"));
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
async function shot(name) {
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from((await cdp("Page.captureScreenshot", { format: "png" })).result.data, "base64"));
  console.log(`  shot ${f}`);
}
// the fake OMX as a picture: its latched LEDs and screen, each key named by what it does now
async function picture(name, title) {
  await sleep(250);   // (the LEDs' 30 ms, a frame and its ACK)
  const url = await js(`__omx.picture({ title: ${JSON.stringify(title)}, labels: FM1Omx.views.labels() })`);
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from(url.split(",")[1], "base64"));
  console.log(`  picture ${f}`);
}

// what the page posts to the engine: the panel (encoder detents summed, matrix order) and MIDI in
const HOOK = `(() => { const p = FM1Rec.o.getPlayer(), port = p.node.port, f = port.postMessage.bind(port);
  window.__panel = null; window.__enc = [0, 0, 0, 0, 0, 0, 0]; window.__midi = []; window.__posts = 0;
  port.postMessage = (m, t) => {
    if (m && m.type === "panel") { window.__posts++; window.__panel = { keys: m.keys, buttons: m.buttons, master: m.master }; m.enc.forEach((v, i) => { window.__enc[i] += v; }); }
    if (m && m.type === "midi") window.__midi.push(Array.from(m.bytes));
    return f(m, t); };
  return true; })()`;
const P = `(await import("/web/site/js/panel.js"))`;
const bit = (v, b) => ((v >>> b) & 1) === 1;
const running = `window.FM1Rec && window.FM1Site && window.FM1Omx && FM1Site.current().info && FM1Site.current().engine === "native"`;
const omxDo = async (expr, ms = 120) => { await js(`(${expr}, true)`); await sleep(ms); };
const panel = () => js("window.__panel");
const toastText = () => js(`document.getElementById("toast").textContent`);
const lit = (c) => c.some((v) => v > 0);

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
  await cdp("Page.addScriptToEvaluateOnNewDocument", { source: FAKE });
  await cdp("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 2, mobile: false });
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=felucca&engine=native` });
  await until(running, 90000, "native Felucca running");
  await sleep(1200);
  await js(HOOK);
  const M = await js(`(async () => { const p = ${P}; return { kn2: p.roleToMatrix(p.E.K2), play: p.labelToMatrix(p.B.PLAY) }; })()`);

  // 1. the sheet's section; MIDI on as a keyboard first (the OMX-27 is listed), then Connect
  ok(!(await js("__omx.got.length")), "before Connect, nothing goes to the OMX-27");
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open")`, 5000, "the sheet");
  await js(`(Array.from(document.querySelectorAll(".sheet .pill")).find((b) => b.textContent === "Enable MIDI").click(), true)`);
  await until(`document.querySelector('.sheet select[aria-label="MIDI input"]')`, 5000, "the MIDI input picker");
  const names = () => js(`Array.from(document.querySelector('.sheet select[aria-label="MIDI input"]').options, (o) => o.textContent)`);
  ok((await names()).includes("OMX-27"), "MIDI on: the OMX-27 is a MIDI keyboard input while not in remote mode");
  await omxDo(`__omx.note([0x90, 62, 100])`);
  ok(bit((await panel()).keys, 62 - 53), "...and its notes play the keys");
  await omxDo(`__omx.note([0x80, 62, 0])`);
  ok(await js(`Array.from(document.querySelectorAll(".sheet h3")).some((h) => h.textContent === "Use my OMX-27")`), "the sheet has a Use my OMX-27 section");
  await js(`(document.querySelector(".omx-slot").scrollIntoView({ block: "center" }), true)`);
  await js(`(Array.from(document.querySelectorAll(".omx-slot .pill")).find((b) => b.textContent === "Connect my OMX-27").click(), true)`);
  await until(`__omx.remote && FM1Omx.view().active`, 8000, "REMOTE mode");
  const mode = await js(`__omx.got.find((m) => m[4] === 0x51)`);
  ok(mode && mode.join() === [0xF0, 0x7D, 0, 0, 0x51, 0x05, 9, 0xF7].join(), "Connect sends the MODE command (51 05 09)");
  await sleep(300);
  ok(/remote mode/.test(await toastText()), `52 02: a toast: "${await toastText()}"`);
  const status = await js(`document.querySelector(".omx-slot .remote-status").textContent`);
  ok(/in remote mode/.test(status) && /hold AUX and the encoder/.test(status) && (await js(`document.querySelector(".omx-slot .pill").textContent`)) === "Disconnect",
     `the status line says so, and how to leave it: "${status}"`);
  await sleep(500);
  await shot("omx-sheet");
  await until(`__omx.frames > 0 && __omx.leds[0][0] > 0`, 5000, "the first frame and LEDs");
  ok(!(await names()).includes("OMX-27"), "in remote mode the OMX-27 is no MIDI keyboard input (the list leaves it out)");
  await omxDo(`__omx.note([0x90, 64, 100])`);
  ok(!bit((await panel()).keys, 64 - 53), "...and a note from it plays nothing (nothing twice)");
  await omxDo(`__omx.note([0x80, 64, 0])`);
  await js(`(document.querySelector(".sheet .icon-btn[aria-label=Close]").click(), true)`);
  await sleep(300);

  // 2. Base: a key plays the FM-1's
  ok(await js(`__omx.leds[11].join() !== "0,0,0" && __omx.leds[0][0] > 0 && __omx.leds[0][0] < 100`), "Base: Range 1 lit, AUX at half");
  const shownText = await js(`(() => { const b = __omx.screenBits(); return b.reduce((s, v) => s + v, 0); })()`);
  ok(shownText > 100, `Base: the screen shows something (${shownText} pixels lit)`);
  await picture("omx-base", "Base view (FM-1)");
  await omxDo(`__omx.press(15)`);
  ok(bit((await panel()).keys, 0), "Base: OMX key 15 holds the FM-1's first F (key 0) in Range 1");
  await omxDo(`__omx.release(15)`);
  ok(!bit((await panel()).keys, 0), "...and lets it go");
  await omxDo(`__omx.click(12)`);
  await omxDo(`__omx.press(23)`);
  ok(bit((await panel()).keys, 26), "Range 2: key 23 holds the FM-1's top key (G5)");
  await omxDo(`__omx.release(23)`);
  await omxDo(`__omx.click(11)`);

  // 3. 13 held, 7 picks KN2, the encoder turns it
  await omxDo(`__omx.press(13)`);
  await picture("omx-knobs", "Base view, knob quick keys (key 13 held)");
  await omxDo(`__omx.press(7)`);
  await omxDo(`(__omx.release(7), __omx.release(13))`);
  await js("(window.__enc = [0, 0, 0, 0, 0, 0, 0], true)");
  await omxDo(`(__omx.enc(1), __omx.enc(2))`);
  let enc = await js("window.__enc");
  ok(enc[M.kn2] === 3 && enc.reduce((a, v) => a + Math.abs(v), 0) === 3, `13 + 7 picks KN2; the encoder (1, then 2 with acceleration) turns it 3 detents (${enc.join(" ")})`);

  // 4. 14 held, PLAY down and up
  await omxDo(`__omx.press(14)`);
  await picture("omx-buttons", "Base view, button quick keys (key 14 held)");
  await omxDo(`__omx.press(25)`);
  ok(bit((await panel()).buttons, M.play), "14 held: key 25 holds PLAY");
  await omxDo(`__omx.release(25)`);
  ok(!bit((await panel()).buttons, M.play), "...and lets it go");
  await omxDo(`__omx.release(14)`);
  await omxDo(`__omx.press(0)`);
  ok(bit((await panel()).buttons, M.play), "AUX then presses the selected button (PLAY)");
  await omxDo(`__omx.release(0)`);

  // 5. Seq: the mirrored LEDs follow the FM-1's
  await omxDo(`__omx.pot(0, 40)`, 250);
  ok(await js(`FM1Omx.views.view === 1`), "K1 at 40: the Seq view");
  const mirror = await js(`(async () => { const p = ${P}, V = await import("/web/site/js/omx27-views.js"), idx = p.ledIndexOf(FM1Site.current().info.keymap || null), lv = FM1Site.leds().levels;
    const fm1 = (k) => (k <= 10 ? V.FM1_SHARPS[k - 1] : V.FM1_WHITES[k - 11]);
    return Array.from({ length: 26 }, (_, i) => (lv[idx[14 + fm1(i + 1)]] > 0) === __omx.leds[i + 1].some((v) => v > 0)).every(Boolean) + ":" + Array.from({ length: 27 }, (_, k) => lv[idx[14 + k]]).join(" "); })()`);
  ok(mirror.startsWith("true"), `Seq: each mirrored key lit exactly when its FM-1 key is (FM-1 levels ${mirror.split(":")[1]})`);
  await omxDo(`__omx.press(16)`, 400);   // OMX white 16 = the FM-1's sixth white (D4, key 9)
  const natural = await js(`(async () => { const p = ${P}, idx = p.ledIndexOf(FM1Site.current().info.keymap || null); return FM1Site.leds().levels[idx[14 + 9]]; })()`);
  let leds = await js("__omx.leds");
  if (natural > 0) ok(lit(leds[16]) && leds.filter(lit).length >= 2, `Seq: Felucca lights its held key (level ${natural}); the OMX's key 16 lights with it`);
  else console.log(`  (Felucca does not light a held key: level ${natural}; the levels are forced below)`);
  await omxDo(`__omx.release(16)`, 200);
  // the levels forced (as an engine's leds event), held while checked
  const FORCE = `(async (keys) => { const p = ${P}, idx = p.ledIndexOf(FM1Site.current().info.keymap || null), lv = new Uint8Array(44);
    for (const [k, v] of keys) lv[idx[14 + k]] = v;
    const pl = FM1Rec.o.getPlayer(), emit = pl.emit.bind(pl);
    window.__forced = lv; pl.emit = (n, ...a) => (n === "leds" ? emit(n, window.__forced, 0) : emit(n, ...a));
    emit("leds", lv, 0); return true; })`;
  await js(`${FORCE}([[0, 255], [1, 255], [12, 96], [22, 40], [25, 255], [26, 255]])`);
  await sleep(200);
  leds = await js("__omx.leds");
  ok(lit(leds[11]) && lit(leds[1]) && lit(leds[18]) && lit(leds[26]) && lit(leds[10]) && !lit(leds[12]) && !lit(leds[2]),
     "Seq: OMX 11 (F3), 1 (F#3), 18 (F4), 10 (D#5), 26 (G5) light with their FM-1 keys, the rest dark");
  ok(leds[11][0] > leds[18][0] && leds[18][0] > 0, `Seq: the FM-1's level sets the brightness (255 -> ${leds[11][0]}, 96 -> ${leds[18][0]})`);
  await picture("omx-seq", "Seq view (FM-1 F3, F#3, F4, D#5, F#5, G5 lit)");
  await omxDo(`__omx.pot(3, 100)`, 200);
  leds = await js("__omx.leds");
  ok(lit(leds[0]) && leds[0][0] === leds[0][1] && /AUX F#5/.test(await js(`FM1Omx.views.screen(1e12).line`)), "Seq: K4 up, AUX is F#5 and mirrors it (lit white)");
  await omxDo(`__omx.pot(3, 10)`);
  await js(`(() => { const pl = FM1Rec.o.getPlayer(); delete pl.emit; return true; })()`);

  // 6. Keyboard: notes to the firmware's MIDI in
  await omxDo(`__omx.pot(0, 70)`, 200);
  await js("(window.__midi = [], true)");
  await omxDo(`__omx.press(12)`, 250);
  await picture("omx-keyboard", "Keyboard view (key 12, C4, held)");
  await omxDo(`__omx.release(12)`);
  const midi = await js("window.__midi");
  ok(midi.length === 2 && midi[0].join() === "144,60,100" && midi[1].join() === "128,60,0" && !bit((await panel()).keys, 60 - 53),
     `Keyboard: key 12 sends C4 on and off to the firmware's MIDI in, and holds no panel key (${JSON.stringify(midi)})`);

  // 7. Quick Keys
  await omxDo(`__omx.pot(0, 120)`, 200);
  await picture("omx-quick", "Quick Keys view");
  await omxDo(`__omx.press(23)`);
  ok(bit((await panel()).buttons, M.play) && (await js("FM1Omx.views.button")) === 10, "Quick Keys: key 23 selects PLAY and presses it");
  await omxDo(`__omx.release(23)`);

  // 8. K5: MASTER after its pickup
  const m0 = (await panel()).master;
  await omxDo(`__omx.pot(4, 10)`);
  ok((await panel()).master === m0 && /MASTER: turn the pot to/.test(await toastText()), `K5 far from MASTER (${m0}): MASTER stays, "${await toastText()}"`);
  const at = Math.round(m0 * 127 / 1023);
  await omxDo(`(__omx.pot(4, ${at - 5}), __omx.pot(4, ${at + 1}))`);
  await omxDo(`__omx.pot(4, 64)`);
  const mv = (await panel()).master;
  ok(mv === Math.round((64 << 7) * 1023 / 16383), `K5 picks MASTER up, then sets it: K5 at 64 -> MASTER ${mv}`);
  ok(/^MASTER 50%$/.test(await js(`FM1Omx.views.screen(performance.now()).line`)), "K5: the screen says MASTER 50%");
  // a real pot's stream: a message every 2-3 ms, hi-res steps of 32
  await js("(window.__posts = 0, window.__msgs0 = __omx.potMsgs, window.__t0 = performance.now(), true)");
  await js("__omx.sweep(4, 64, 100)");
  await sleep(100);
  const sw = await js("({ posts: window.__posts, msgs: __omx.potMsgs - window.__msgs0, ms: Math.round(performance.now() - window.__t0 - 100), master: window.__panel.master })");
  ok(sw.master === Math.round(12800 * 1023 / 16383) && sw.posts < sw.msgs && sw.posts <= sw.ms / 10 + 2,
     `K5 swept as the device streams it (${sw.msgs} messages in ${sw.ms} ms): at most one panel post per 10 ms tick (${sw.posts}), MASTER ${sw.master}`);
  // MODE again while in REMOTE mode: the OMX answers 52 03 then 52 02: still connected, no "left"
  await js("(document.getElementById('toast').textContent = '', true)");
  await omxDo(`FM1Omx.connect()`, 300);
  ok((await js("FM1Omx.view().active && FM1Omx.view().tone === 'on'")) && !/left/.test(await toastText()) && (await js("__omx.modes")) >= 2,
     `MODE again: 52 03 + 52 02 is still connected (no "left" toast: "${await toastText()}")`);

  // 9. 52 03, and REMOTE picked again on the device
  await omxDo(`__omx.leave()`, 300);
  ok(!(await js("FM1Omx.view().active")) && /left remote mode/.test(await toastText()), `52 03: a toast: "${await toastText()}"`);
  await omxDo(`__omx.pickRemote()`, 400);
  ok(await js("FM1Omx.view().active && __omx.frames > 0"), "52 02 again: remote mode is back, the screen drawn again");

  // 10. a reload connects by itself (MIDI allowed, connected before)
  await js("(window.__before = true)");
  await cdp("Page.reload", {});
  await until(`!window.__before && ${running}`, 90000, "Felucca again after the reload");
  await until(`FM1Omx.view().active && __omx.remote`, 8000, "remote mode by itself after the reload");
  await sleep(200);
  ok((await js("FM1Omx.view().active")) && /in remote mode/.test(await toastText()), `after a reload (the OMX still in REMOTE mode: 52 03 + 52 02): connected by itself ("${await toastText()}")`);
  await sleep(800);
  await js(HOOK);
  await omxDo(`__omx.note([0x90, 64, 100])`);
  ok(!(await panel()) || !bit((await panel()).keys, 64 - 53), "after a reload: its port still skipped by the MIDI keyboard input");
  await omxDo(`__omx.note([0x80, 64, 0])`);

  // 11. unplugged and back
  await omxDo(`__omx.unplug()`, 300);
  ok(!(await js("FM1Omx.view().active")) && /unplugged/.test(await js("FM1Omx.view().detail")), `unplugged: "${await js("FM1Omx.view().detail")}"`);
  await omxDo(`__omx.plug()`, 600);
  ok(await js("FM1Omx.view().active && __omx.remote"), "plugged in again: remote mode by itself");
  await omxDo(`__omx.press(15)`);
  ok(bit((await panel()).keys, 0), "...and its keys play again");
  await omxDo(`__omx.release(15)`);

  // 12. Disconnect: the OMX's LEDs and screen cleared
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open .omx-slot .pill")`, 5000, "the sheet");
  await js(`(document.querySelector(".omx-slot .pill").click(), true)`);
  await sleep(400);
  ok(await js(`__omx.leds.every((c) => c.join() === "0,0,0") && __omx.frame.every((b) => b === 0)`), "Disconnect: every OMX LED off, its screen blank");
  const st2 = await js(`document.querySelector(".omx-slot .remote-status").textContent`);
  ok(/stays in remote mode/.test(st2) && /hold AUX and the encoder/.test(st2) && !(await js("FM1Omx.view().want")), `Disconnect: the status says how to leave remote mode: "${st2}"`);
  ok((await names()).includes("OMX-27"), "Disconnect: the OMX-27 is a MIDI keyboard input again");
  await omxDo(`__omx.note([0x90, 64, 100])`);
  ok(bit((await panel()).keys, 64 - 53), "...and its notes play the keys again");
  await omxDo(`__omx.note([0x80, 64, 0])`);
  ok(!errors.length, `no page exceptions (${errors.length})`);
} catch (e) {
  console.error(e); fails++;
} finally {
  try { ws && ws.close(); } catch {}
  stop();
}
console.log(`\n${checks - fails}/${checks} checks passed`);
process.exit(fails ? 1 : 0);
