// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 RemoteFM1 contributors
// The SMK-37 emulator as a host's unit (docs/SMK37.md "Emulator"): RemoteFM1's protocol over its USB SysEx
// (web_usx_*), as RemoteFM1.com's player speaks it through the page's relay. HELLO -> HELLO_R (the FM-1's identity;
// byte for byte the simulator's when its dump is given), a session's TILES on the emulated screen and acknowledged
// (a flood of them too, fed as the tap has room), LEDS on the SMK's lights, an SMK button as the FM-1's panel event.
//   node web/emu-smk37/test_emu_smk37_link.mjs [app.wasm] [hello_usb.txt (firmware/tests/smk_hello.txt's dump)]
import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const path = process.argv[2] || join(here, "../../firmware/build/emu-smk37/app.wasm");
const helloDump = process.argv[3] || "";
let mem = null;
const wasi = new Proxy({
  clock_time_get: (id, prec, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; },
  fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; },
}, { get: (t, k) => (k in t ? t[k] : () => 0) });
const env = new Proxy({}, { get: () => () => 0 });
const { instance } = await WebAssembly.instantiate(readFileSync(path), { wasi_snapshot_preview1: wasi, env });
const ex = instance.exports;
mem = ex.memory;
if (ex._initialize) ex._initialize();

let fails = 0;
function check(what, ok, detail = "") {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`);
  if (!ok) fails++;
}
for (const f of ["web_usx_in_buf", "web_usx_room", "web_usx_in", "web_usx_out_buf", "web_usx_out_len", "web_usx_out_clear"])
  if (typeof ex[f] !== "function") { console.log(`FAIL no export ${f} (not an SMK-37 build?)`); process.exit(1); }

const run = (ms) => { for (let f = Math.round(ms * 44.1); f > 0; f -= 128) ex.web_render(Math.min(128, f)); };

// 7-in-8 and the framing (docs/PROTOCOL.md section 1)
function pack7(body) {
  const out = [];
  for (let i = 0; i < body.length; i += 7) {
    const g = body.slice(i, i + 7);
    let msb = 0;
    g.forEach((b, k) => { msb |= (b >> 7) << k; });
    out.push(msb, ...g.map((b) => b & 0x7f));
  }
  return out;
}
function unpack7(p) {
  const out = [];
  for (let i = 0; i < p.length; i += 8) for (let k = 1; k < 8 && i + k < p.length; k++) out.push(p[i + k] | ((p[i] >> (k - 1)) & 1) << 7);
  return out;
}
const sx = (cmd, body = []) => Uint8Array.from([0xf0, 0x7d, 0x52, 0x46, cmd, ...pack7(body), 0xf7]);

// the host's bytes in, as the tap has room (the page's worklet does the same); the device runs meanwhile
let fedBlocks = 0;
function send(bytes) {
  let i = 0;
  while (i < bytes.length) {
    const n = Math.min(ex.web_usx_room(), bytes.length - i);
    if (n) {
      new Uint8Array(mem.buffer, ex.web_usx_in_buf(), n).set(bytes.subarray(i, i + n));
      ex.web_usx_in(n);
      i += n;
    }
    ex.web_render(128);
    fedBlocks++;
  }
}
// the device's SysEx out since the last call: [{ cmd, body }] (RemoteFM1's)
const replies = [];
function drain() {
  const n = ex.web_usx_out_len();
  const b = new Uint8Array(mem.buffer, ex.web_usx_out_buf(), n).slice();
  ex.web_usx_out_clear();
  let s = -1;
  for (let i = 0; i < b.length; i++) {
    if (b[i] === 0xf0) s = i;
    else if (b[i] === 0xf7 && s >= 0) {
      const m = b.subarray(s, i + 1);
      if (m[1] === 0x7d && m[2] === 0x52 && m[3] === 0x46) replies.push({ cmd: m[4], body: unpack7([...m.subarray(5, m.length - 1)]) });
      s = -1;
    }
  }
  return replies.splice(0);
}
// USB-MIDI packets out (the panel events): [status, d1, d2]
function pkts() {
  const n = ex.web_out_n(), p = new Uint32Array(mem.buffer, ex.web_out_pkts(), n);
  const r = [...p].map((x) => [(x >>> 8) & 255, (x >>> 16) & 255, x >>> 24]).filter((m) => m[0] < 0xf8);
  ex.web_out_clear();
  return r;
}
const hex = (a) => [...a].map((v) => v.toString(16).toUpperCase().padStart(2, "0")).join("");

ex.web_master(2048);
ex.web_boot();
run(1100);
drain(); pkts();

// HELLO -> HELLO_R
send(sx(0x01));
run(50);
let r = drain();
const hr = r.find((m) => m.cmd === 0x41);
check("HELLO answered with HELLO_R", !!hr, r.map((m) => m.cmd.toString(16)).join(" "));
if (hr) {
  const h = hr.body, str = (o, n) => String.fromCharCode(...h.slice(o, o + n)).replace(/\0.*$/, "");
  check("HELLO_R: RemoteFM1, protocol 2", str(0, 16) === "RemoteFM1" && h[16] === 2, `${str(0, 16)} ${h[16]}`);
  check("HELLO_R: the FM-1's identity", str(43, 8) === "FM-1_720", str(43, 8));
  check("HELLO_R: USB configured, SysEx heard", (h[25] & 5) === 5, h[25].toString(2));
  check("HELLO_R: caps 4015", (h[55] | h[56] << 8) === 4015, String(h[55] | h[56] << 8));
  if (helloDump && existsSync(helloDump)) {
    const want = readFileSync(helloDump, "utf8").trim();
    check("HELLO_R byte for byte the simulator's (smk_hello.txt)", hex(h) === want, hex(h));
  }
}

// SESSION start: PANEL_R with the session flag
send(sx(0x10, [1, 3000 & 255, 3000 >> 8]));
run(100);
r = drain();
const pr = r.filter((m) => m.cmd === 0x53).pop();
check("a session starts: PANEL_R with its flag", !!pr && (pr.body[24] & 1) === 1, pr ? hex(pr.body) : "none");
pkts();

// TILES: the whole screen filled, acknowledged and on the emulated screen
const fb = () => new Uint16Array(mem.buffer, ex.web_fb(), 240 * 240);
const before = ex.web_blits();
send(sx(0x12, [1, 0, 1, 1, 0, 0, 240, 240, 1, 0xf8, 0x00]));
run(100);
r = drain();
let ack = r.find((m) => m.cmd === 0x52);
check("TILES (a fill) acknowledged", !!ack && (ack.body[0] | ack.body[1] << 8) === 1 && !(ack.body[2] & 1), ack ? hex(ack.body) : "none");
{
  const f = fb(), s = new Set(f);
  check("the screen is the host's (all red)", ex.web_blits() > before && s.size === 1 && (s.has(0x00f8) || s.has(0xf800)),
        [...s].slice(0, 4).map((v) => v.toString(16)).join(" "));
}

// a flood: a frame of raw 16 x 16 tiles (225 messages, ~120 KB of SysEx), fed only as the tap has room
{
  const px = (x, y) => (x * 31 + y * 7) & 0xffff;
  let bytes = [], k = 0;
  for (let ty = 0; ty < 15; ty++) for (let tx = 0; tx < 15; tx++, k++) {
    const body = [2, 0, k === 224 ? 1 : 0, 1, tx * 16, ty * 16, 16, 16, 0];
    for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) { const c = px(tx * 16 + x, ty * 16 + y); body.push(c >> 8, c & 255); }
    bytes.push(...sx(0x12, body));
  }
  bytes = Uint8Array.from(bytes);
  fedBlocks = 0;
  send(bytes);
  run(300);
  r = drain();
  ack = r.find((m) => m.cmd === 0x52 && (m.body[0] | m.body[1] << 8) === 2);
  check(`a flood of ${bytes.length} bytes fed in ${fedBlocks} audio blocks, acknowledged clean`, !!ack && ack.body[2] === 0,
        ack ? hex(ack.body) : r.map((m) => m.cmd.toString(16)).join(" "));
  const f = fb();
  let bad = 0;
  for (let y = 0; y < 240; y++) for (let x = 0; x < 240; x++) {
    const v = f[y * 240 + x], want = px(x, y);
    if (v !== want && v !== (((want >> 8) | (want << 8)) & 0xffff)) bad++;
  }
  check("the flood's frame on the screen, every pixel", bad === 0, `${bad} wrong`);
  check("nothing dropped on the way out", (ex.web_usx_drops ? ex.web_usx_drops() : 0) === 0);
}

// LEDS: OCT+ (matrix button 1: column 1, row bit 4) lit -> the SMK's OCTAVE+ LED; then all off
const SMK_OCTUP = 5;
const leds = (col, bit) => { const L = new Array(45).fill(0); if (bit) L[col] = bit; return L; };
send(sx(0x11, leds(1, 0x10)));
run(100);
check("LEDS: OCT+ lit on the SMK's OCTAVE+", (ex.web_smk_leds() >>> SMK_OCTUP & 1) === 1, ex.web_smk_leds().toString(2));
send(sx(0x11, leds(0, 0)));
run(100);
check("LEDS: all off, OCTAVE+ dark", (ex.web_smk_leds() >>> SMK_OCTUP & 1) === 0, ex.web_smk_leds().toString(2));

// an SMK button in the session: OCTAVE+ is the FM-1's OCT+ (matrix 1): 9E 01 7F, then 8E 01
pkts();
ex.web_smk_buttons(1 << SMK_OCTUP); run(60); ex.web_smk_buttons(0); run(60);
const o = pkts();
check("OCTAVE+ in a session: the FM-1's OCT+ panel event", o.some((m) => m[0] === 0x9e && m[1] === 1 && m[2] === 127) &&
      o.some((m) => m[0] === 0x8e && m[1] === 1), JSON.stringify(o));

// SESSION stop
send(sx(0x10, [0, 0, 0]));
run(100);
drain();
check("the session over (PANEL_R without its flag on PANEL_REQ)", (() => {
  send(sx(0x13)); run(50);
  const p = drain().filter((m) => m.cmd === 0x53).pop();
  return !!p && (p.body[24] & 1) === 0;
})());

console.log(fails ? `${fails} FAILED` : "all passed");
process.exit(fails ? 1 : 0);
