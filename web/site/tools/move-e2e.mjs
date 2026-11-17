// SPDX-License-Identifier: MPL-2.0
// USE MY ABLETON MOVE (js/move-link.js) end to end in headless Chromium (no window, a fresh profile, the browser, the
// server and the simulator ended by their PIDs) on a native firmware (Felucca), against Move Remote's own simulator
// (abletonmoveremote's `make sim`: moveremote-sim serves the protocol's WebSocket on --port and takes presses through
// the mr-cmd FIFO in --dir; no Move is touched). Settings > Use my Ableton Move > the address > Connect: the HELLO, the
// LEDs and the screen; pads hold the FM-1's keys, Play presses PLAY, knob 5 turns K1, the volume knob moves MASTER;
// the simulator's screen shot is the frame the page sent, pixel for pixel; Track 2 (MIDI mode) plays a note into the
// firmware's MIDI in; Track 1 back; a reload connects by itself; Disconnect lets the Move go.
//   node web/site/tools/move-e2e.mjs [--shots=web/build/test] [--sim=.../moveremote-sim]
// Needs web/build/native/felucca.wasm (web/engines/native/build.sh felucca) and the simulator ($MOVEREMOTE_SIM, --sim,
// or an abletonmoveremote checkout beside the repository): without either it says so and skips. Builds
// web/build/move/rfm_move.mjs (web/move/build.sh) when it is missing. Writes move-sheet.png, move-page.png,
// move-screen-fm1.png and move-screen-midi.png (the simulator's screen, 4x).
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, existsSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { deflateSync } from "node:zlib";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const WEB = join(ROOT, "web");
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOTS = arg("shots", join(WEB, "build", "test"));
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const PORT = 8000 + Math.floor(Math.random() * 900), DEBUG = 9300 + Math.floor(Math.random() * 600), SIM_PORT = 17000 + Math.floor(Math.random() * 700);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const SIM = [arg("sim", ""), process.env.MOVEREMOTE_SIM || "", join(ROOT, "../abletonmoveremote/build/sim/moveremote-sim"),
  join(ROOT, "../../../../abletonmoveremote/build/sim/moveremote-sim")].find((p) => p && existsSync(p));
if (!existsSync(join(WEB, "build/native/felucca.wasm"))) { console.log("move-e2e: skipped: no web/build/native/felucca.wasm (web/engines/native/build.sh felucca)"); process.exit(0); }
if (!SIM) { console.log("move-e2e: skipped: no moveremote-sim (abletonmoveremote: make sim; then MOVEREMOTE_SIM=.../build/sim/moveremote-sim or --sim=)"); process.exit(0); }
if (!existsSync(join(WEB, "build/move/rfm_move.mjs"))) execFileSync("sh", [join(WEB, "move/build.sh")], { stdio: "inherit" });
mkdirSync(SHOTS, { recursive: true });

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); return c; };

// the simulator, its test files in a fresh folder
const DIR = mkdtempSync(join(tmpdir(), "rfm-move-e2e-"));
const sim = spawn(SIM, ["--port", String(SIM_PORT), "--dir", DIR], { stdio: ["ignore", "ignore", "pipe"] });
let simLog = "";
sim.stderr.on("data", (d) => { simLog += d; });
const FIFO = join(DIR, "mr-cmd");
const inject = (lines) => writeFileSync(FIFO, lines.endsWith("\n") ? lines : lines + "\n");
const simState = () => { try { return readFileSync(join(DIR, "mr-state"), "utf8"); } catch (_) { return ""; } };
// the simulator's screen: `shot` -> mr-screen.pbm (P1 128 x 64) -> bits, and the 1024 packed bytes as the protocol has them
async function simShot() {
  const f = join(DIR, "mr-screen.pbm"), before = existsSync(f) ? statSync(f).mtimeMs : 0;
  inject("shot");
  for (let i = 0; i < 50 && (!existsSync(f) || statSync(f).mtimeMs === before); i++) await sleep(20);
  await sleep(30);
  const rows = readFileSync(f, "utf8").trim().split("\n").slice(2), bits = rows.map((r) => Array.from(r.trim(), (c) => (c === "1" ? 1 : 0)));
  const packed = new Uint8Array(1024);
  for (let y = 0; y < 64; y++) for (let x = 0; x < 128; x++) if (bits[y][x]) packed[(y >> 3) * 128 + x] |= 1 << (y & 7);
  return { bits, packed, lit: bits.flat().reduce((a, b) => a + b, 0) };
}
// a 1-bit picture as a PNG (4x, white on dark grey, as the Move's OLED)
function png(bits, scale = 4) {
  const w = 128 * scale, h = 64 * scale, raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 3 + 1)] = 0;
    for (let x = 0; x < w; x++) { const v = bits[(y / scale) | 0][(x / scale) | 0] ? 235 : 24, o = y * (w * 3 + 1) + 1 + x * 3; raw[o] = v; raw[o + 1] = v; raw[o + 2] = v; }
  }
  const crcT = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = (b) => { let c = 0xFFFFFFFF; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
  const chunk = (t, d) => { const len = Buffer.alloc(4); len.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
// the shot is a frame the page sent (the FM-1's screen moves while a note sounds: one of the last 30 DISPLAYs, which a
// hook on the link keeps; a few tries)
const DISPLAYS = `(() => { const L = FM1Move; if (L.__hooked) return true; const f = L.send.bind(L); window.__displays = [];
  L.send = (b) => { if (b[2] === 0x84) { window.__displays.push(Array.from(b.subarray(3)).join()); if (window.__displays.length > 30) window.__displays.shift(); } return f(b); };
  return (L.__hooked = true); })()`;
async function shotMatches() {
  let s = null;
  for (let i = 0; i < 6; i++) {
    s = await simShot();
    if (await js(`window.__displays.includes(${JSON.stringify(Array.from(s.packed).join())})`)) return { ...s, same: true, tries: i + 1 };
    await sleep(60);
  }
  return { ...s, same: false, tries: 6 };
}
const savePng = (name, bits) => { const f = join(SHOTS, `${name}.png`); writeFileSync(f, png(bits)); console.log(`  picture ${f}`); };

const profile = mkdtempSync(join(tmpdir(), "chromium-profile-move-"));
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
  "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,800",
  "--mute-audio", "--autoplay-policy=no-user-gesture-required", `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
const stop = () => {
  try { inject("quit"); } catch {}
  for (const p of [chrome, server, sim]) { try { p.kill(); } catch {} }
};
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
async function untilNode(fn, ms = 5000) { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (await fn()) return true; await sleep(50); } return false; }
async function shot(name) {
  const f = join(SHOTS, `${name}.png`);
  writeFileSync(f, Buffer.from((await cdp("Page.captureScreenshot", { format: "png" })).result.data, "base64"));
  console.log(`  shot ${f}`);
}

// what the page posts to the engine: the panel (encoder detents summed, matrix order) and MIDI in
const HOOK = `(() => { const p = FM1Rec.o.getPlayer(), port = p.node.port, f = port.postMessage.bind(port);
  window.__panel = null; window.__enc = [0, 0, 0, 0, 0, 0, 0]; window.__midi = [];
  port.postMessage = (m, t) => {
    if (m && m.type === "panel") { window.__panel = { keys: m.keys, buttons: m.buttons, master: m.master }; m.enc.forEach((v, i) => { window.__enc[i] += v; }); }
    if (m && m.type === "midi") window.__midi.push(Array.from(m.bytes));
    return f(m, t); };
  return true; })()`;
const P = `(await import("/web/site/js/panel.js"))`;
const bit = (v, b) => ((v >>> b) & 1) === 1;
const running = `window.FM1Rec && window.FM1Site && window.FM1Move && FM1Site.current().info && FM1Site.current().engine === "native"`;
const press = async (lines, ms = 150) => { inject(lines); await sleep(ms); };
const panel = () => js("window.__panel || { keys: 0, buttons: 0, master: -1 }");

try {
  ok(await untilNode(() => existsSync(FIFO), 5000), `the simulator up on port ${SIM_PORT} (its FIFO in ${DIR})`);
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
  await cdp("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 2, mobile: false });
  await cdp("Page.navigate", { url: `http://127.0.0.1:${PORT}/web/site/?fw=felucca&engine=native` });
  await until(running, 90000, "native Felucca running");
  await sleep(1200);
  await js(HOOK);
  const M = await js(`(async () => { const p = ${P}; return { k1: p.roleToMatrix(p.E.K1), play: p.labelToMatrix(p.B.PLAY) }; })()`);

  // 1. the sheet's section: the address, Connect
  ok(simState().includes("host=-"), "before Connect, no host on the simulator");
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open")`, 5000, "the sheet");
  ok(await js(`Array.from(document.querySelectorAll(".sheet h3")).some((h) => h.textContent === "Use my Ableton Move")`), "the sheet has a Use my Ableton Move section");
  await js(`(document.querySelector(".move-slot").scrollIntoView({ block: "center" }), true)`);
  await js(`(() => { const a = document.querySelector(".move-slot .move-addr"); a.value = "127.0.0.1:${SIM_PORT}"; return true; })()`);
  await js(`(Array.from(document.querySelectorAll(".move-slot .pill")).find((b) => b.textContent === "Connect my Move").click(), true)`);
  await js(DISPLAYS);
  await until(`FM1Move.view().active`, 8000, "the Move connected");
  await sleep(600);
  const status = await js(`document.querySelector(".move-slot .remote-status").textContent`);
  ok(/connected over this computer \(WebSocket\): FM-1 mode/.test(status) && (await js(`Array.from(document.querySelectorAll(".move-slot .pill")).some((b) => b.textContent === "Disconnect")`)),
     `the status line says so: "${status}"`);
  await shot("move-sheet");
  ok(await untilNode(() => /host=RemoteFM1 web .*drew=1/.test(simState()), 3000), `the simulator: our host, the screen drawn (${simState().trim().split(" ").filter((s) => /^(host|drew|led_pending)=/.test(s)).join(" ")})`);
  ok(await untilNode(() => simState().includes("led_pending=0 "), 3000) && (await js("FM1Move.stats.ledWrites")) >= 76,
     `every LED written (${await js("FM1Move.stats.ledWrites")} writes in ${await js("FM1Move.stats.ledMsgs")} messages) and taken (led_pending=0)`);
  await js(`(document.querySelector(".sheet .icon-btn[aria-label=Close]").click(), true)`);
  await sleep(300);

  // 2. FM-1 mode: pads, Play, knob 5, the volume knob
  await press("note 84 100");
  ok(bit((await panel()).keys, 0) && (await js(`(async () => { const p = ${P}; return (FM1Move.o.controls.keySrc[0] & p.SRC.MOVE) !== 0; })()`)),
     "pad 16 (row 2, column 1) holds the FM-1's first F (key 0), as SRC.MOVE");
  await press("at 84 90");
  await press("note 84 0");
  ok(!bit((await panel()).keys, 0), "...and lets it go");
  await press("note 75 100");
  ok(bit((await panel()).keys, 26), "the bottom-right pad holds G5 (key 26)");
  await press("note 75 0");
  await press("note 16 127");
  ok(bit((await panel()).keys, 0), "step 1 holds key 0 too");
  await press("note 16 0");
  await press("cc 85 127");
  ok(bit((await panel()).buttons, M.play), "Play holds PLAY");
  await press("cc 85 0");
  ok(!bit((await panel()).buttons, M.play), "...and lets it go");
  await js("(window.__enc = [0, 0, 0, 0, 0, 0, 0], true)");
  await press("note 4 127\ncc 75 1\ncc 75 1\nnote 4 0");
  const enc = await js("window.__enc");
  ok(enc[M.k1] === 2 && enc.reduce((a, v) => a + Math.abs(v), 0) === 2, `knob 5 turns K1 two detents (${enc.join(" ")})`);
  const m0 = await js("FM1Rec && FM1Move.o.controls.master");
  await press("cc 79 2");
  const m1 = (await panel()).master;
  ok(m1 === Math.min(1023, m0 + 32), `the volume knob +2: MASTER ${m0} -> ${m1}`);
  await press("cc 79 126");
  await sleep(300);
  let s = await shotMatches();
  ok(s.lit > 100 && s.same, `the simulator's screen is a frame the page sent, pixel for pixel (${s.lit} pixels lit; try ${s.tries})`);
  savePng("move-screen-fm1", s.bits);
  await shot("move-page");

  // 3. Track 2: MIDI mode; a pad plays a note into the firmware's MIDI in
  await press("cc 42 127\ncc 42 0", 300);
  ok((await js("FM1Move.core.state().mode")) === 1 && /MIDI mode/.test(await js("FM1Move.view().detail")), "Track 2: MIDI mode");
  await js("(window.__midi = [], true)");
  await press("note 68 100", 250);
  ok(!bit((await panel()).keys, 0), "MIDI mode: a pad holds no FM-1 key");
  await press("note 68 0", 250);
  const midi = await js("window.__midi");
  ok(midi.length === 2 && midi[0].join() === "144,48,100" && midi[1].join() === "128,48,0", `MIDI mode: pad 0 sends C3 on and off to the firmware's MIDI in (${JSON.stringify(midi)})`);
  await sleep(200);
  s = await shotMatches();
  const midiTitle = s.bits.slice(0, 9).flat().reduce((a, b) => a + b, 0);
  ok(s.lit > 50 && s.same && midiTitle > 0, `MIDI mode: the Move's screen shows it, as the page sent it (try ${s.tries})`);
  savePng("move-screen-midi", s.bits);
  ok(typeof (await js("JSON.parse(localStorage.getItem('remotefm1-player')).move")) === "string", "the Move's settings kept in the page's prefs");
  await press("cc 43 127\ncc 43 0", 300);
  ok((await js("FM1Move.core.state().mode")) === 0, "Track 1: FM-1 mode again");

  // 4. a reload connects by itself (with the settings)
  await js("(window.__before = true)");
  await cdp("Page.reload", {});
  await until(`!window.__before && ${running}`, 90000, "Felucca again after the reload");
  await until(`FM1Move.view().active`, 8000, "the Move again after the reload");
  ok(await untilNode(() => /host=RemoteFM1 web .*drew=1/.test(simState()), 3000), "after a reload: connected by itself, the screen drawn");
  await sleep(800);
  await js(HOOK);
  await press("note 84 100");
  ok(bit((await panel()).keys, 0), "...and its pads play again");
  await press("note 84 0");

  // 5. Disconnect: the Move let go
  await js(`(document.getElementById("menu-btn").click(), true)`);
  await until(`document.querySelector(".sheet.open .move-slot .pill")`, 5000, "the sheet");
  await press("note 84 100");
  await js(`(Array.from(document.querySelectorAll(".move-slot .pill")).find((b) => b.textContent === "Disconnect").click(), true)`);
  await sleep(300);
  ok(!bit((await panel()).keys, 0) && !(await js("FM1Move.view().want")), "Disconnect: the key the Move held is let go");
  ok(await untilNode(() => simState().includes("host=-"), 3000), "Disconnect: the simulator has no host any more");
  inject("note 84 0");
  ok(!errors.length, `no page exceptions (${errors.length})`);
} catch (e) {
  console.error(e); fails++;
  if (simLog) console.error(simLog.slice(-2000));
} finally {
  try { ws && ws.close(); } catch {}
  stop();
}
console.log(`\n${checks - fails}/${checks} checks passed`);
process.exit(fails ? 1 : 0);
