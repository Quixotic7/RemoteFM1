// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 RemoteFM1 contributors
// The SMK-37 emulator's wasm without a browser (BOARD=smk37 ./build.sh --web; tests/run_tests.sh): boot, a key's
// sound, a pad as its white key, the buttons as the FM-1's by place, an outer key as MIDI out, a fader as KNOB 1 and
// > as SELECT (no CC of their own), the pads' colours read back.
//   node web/emu-smk37/test_emu_smk37.mjs [firmware/build/emu-smk37/app.wasm]
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const path = process.argv[2] || join(here, "../../firmware/build/emu-smk37/app.wasm");
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
// run the device ms milliseconds; the audio's peak meanwhile
function run(ms) {
  let peak = 0;
  for (let f = Math.round(ms * 44.1); f > 0; f -= 128) {
    const n = Math.min(128, f);
    ex.web_render(n);
    const l = new Float32Array(mem.buffer, ex.web_out_l(), n);
    for (const v of l) peak = Math.max(peak, Math.abs(v));
  }
  return peak;
}
// what went out on USB since the last call: [status, d1, d2]
function out() {
  const n = ex.web_out_n(), p = new Uint32Array(mem.buffer, ex.web_out_pkts(), n);
  const r = [...p].map((x) => [(x >>> 8) & 255, (x >>> 16) & 255, x >>> 24]).filter((m) => m[0] !== 0xf8);
  ex.web_out_clear();
  return r;
}
const has = (msgs, s, d1, d2) => msgs.some((m) => m[0] === s && m[1] === d1 && (d2 === undefined || m[2] === d2));
const B = { ">": 3, "OCT-": 4, "OCT+": 5, ARP: 6, GLOBE: 10 };
const pads = () => [...new Uint8Array(mem.buffer, ex.web_smk_pad_lvl(), 16)];
const rgb = () => [...new Uint32Array(mem.buffer, ex.web_smk_pad_rgb(), 16)];

for (const f of ["web_smk_key", "web_smk_pad", "web_smk_buttons", "web_smk_enc", "web_smk_fader", "web_smk_bend",
                 "web_smk_mod", "web_smk_pedal", "web_smk_leds", "web_smk_pad_rgb", "web_smk_pad_lvl"])
  if (typeof ex[f] !== "function") { console.log(`FAIL no export ${f} (not an SMK-37 build?)`); process.exit(1); }

ex.web_master(2896);
ex.web_boot();
run(1500);
{
  const fb = new Uint16Array(mem.buffer, ex.web_fb(), 240 * 240);
  const colours = new Set(fb);
  check("boot: the screen drawn", ex.web_blits() > 0 && colours.size > 2, `${colours.size} colours, ${ex.web_blits()} blits`);
}
check("silent at rest", run(300) < 0.001);
out();

// keybed key 12 (C4, the FM-1's key 7) with velocity: the DX7 sounds, note on and off out
ex.web_smk_key(12, 90);
const peak = run(300);
check("an FM-1 key (keybed 12) sounds", peak > 0.01, `peak ${peak.toFixed(3)}`);
ex.web_smk_key(12, 0);
run(50);
let o = out();
check("its note out with its velocity", has(o, 0x90, 0x3c, 90) && has(o, 0x80, 0x3c), JSON.stringify(o));
run(1500);

// pad 1 is the FM-1's white key 1 (F3): its note at the pad's velocity, and the pad lit as that key's LED
ex.web_smk_pad(0, 70);
run(40);
o = out();
check("pad 1 holds white key F3", has(o, 0x90, 0x35, 70), JSON.stringify(o));
check("pad 1 lit (its key sounds)", pads()[0] === 255 && rgb()[0] !== 0, `level ${pads()[0]}, rgb ${rgb()[0].toString(16)}`);
check("the other pads dark", pads().slice(1).every((v) => v === 0), pads().join(" "));
ex.web_smk_pad(0, 0);
run(40);
o = out();
check("pad 1 let go", has(o, 0x80, 0x35), JSON.stringify(o));
run(800);
check("pad 1 dark again", pads()[0] === 0, `level ${pads()[0]}`);

// OCTAVE+ is OCT+: the next key an octave up, its LED lit; GLOBE is EDIT: CC bank E, GLOBE lit, ARP (FX) not
function tap(b) { ex.web_smk_buttons(1 << b); run(60); ex.web_smk_buttons(0); run(60); }
tap(B["OCT+"]);
check("OCTAVE+ lights", (ex.web_smk_leds() >>> B["OCT+"] & 1) === 1);
out();
ex.web_smk_key(12, 64); run(20); ex.web_smk_key(12, 0); run(20);
o = out();
check("OCTAVE+ is OCT+ (key 12 now 72)", has(o, 0x90, 0x48, 64), JSON.stringify(o));
tap(B["OCT-"]);
check("ARP (FX) lit at the start", (ex.web_smk_leds() >>> B.ARP & 1) === 1);
tap(B.GLOBE);
check("GLOBE is EDIT (its LED lit, ARP not)", (ex.web_smk_leds() >>> B.GLOBE & 1) === 1 && (ex.web_smk_leds() >>> B.ARP & 1) === 0,
      ex.web_smk_leds().toString(2));
tap(B.ARP);

// an outer key (C3, keybed 0): MIDI out (note 48); fader 1 is KNOB 1 (bank A's first CC), not CC 16; > no CC 113
out();
ex.web_smk_key(0, 70); run(20); ex.web_smk_key(0, 0); run(20);
o = out();
check("outer key C3 plays note 48", has(o, 0x90, 0x30, 70), JSON.stringify(o));
ex.web_smk_fader(0, 100); run(20);
o = out();
check("fader 1 turns KNOB 1 (a CC out), no CC 16 = 100", o.some((m) => m[0] === 0xb0) && !has(o, 0xb0, 0x10, 100),
      JSON.stringify(o));
tap(B[">"]);
o = out();
check("> sends no CC 113", !has(o, 0xb0, 0x71), JSON.stringify(o));

// K1 is MASTER (96 a detent)
const m0 = ex.web_smk_master();
ex.web_smk_enc(0, 2);
check("K1 turns MASTER", ex.web_smk_master() === m0 + 192, `${m0} -> ${ex.web_smk_master()}`);

// a quick click through app_worklet.js: press and release in one gap between audio blocks still reach the firmware
{
  globalThis.currentTime = 0;
  await import(join(here, "app_worklet.js"));
  const app = globalThis.fm1App, fm1 = { ex, mem, port: { postMessage() {} } };
  app.load(fm1);
  const block = () => { app.process(fm1); run(128 / 44.1); };
  app.message(fm1, { type: "app", op: "btns", mask: 1 << B.GLOBE });
  app.message(fm1, { type: "app", op: "btns", mask: 0 });
  for (let i = 0; i < 40; i++) block();
  check("a quick click on GLOBE (via the worklet) lights it", (ex.web_smk_leds() >>> B.GLOBE & 1) === 1, ex.web_smk_leds().toString(2));
  out();
  app.message(fm1, { type: "app", op: "key", k: 12, vel: 80 });
  app.message(fm1, { type: "app", op: "key", k: 12, vel: 0 });
  for (let i = 0; i < 40; i++) block();
  o = out();
  check("a quick tap on a key (via the worklet) plays it", has(o, 0x90, 0x3c, 80) && has(o, 0x80, 0x3c), JSON.stringify(o));
}

console.log(fails ? `${fails} FAILED` : "all passed");
process.exit(fails ? 1 : 0);
