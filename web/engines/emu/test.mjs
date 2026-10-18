// The emu engine's headless test (web/engine/API.md section 4), in Node, no AudioWorklet:
// createEngine + render in a loop, at 48 kHz (so the resampler runs). For each firmware it boots,
// checks the screen isn't blank within 3 s of rendered audio, presses a note key (then PLAY, for a
// groovebox that is silent until it plays) and checks the audio isn't silent, writes
// web/build/test/emu-<id>.png and measures the real-time factor.
//
//   web/engines/emu/build.sh && node web/engines/emu/test.mjs [--only=id,id] [--mhz=100] [--stock=path]
//
// Packages are downloaded once into web/build/packages/ (gitignored, never committed) from the Hub's
// mirror, the same URLs the page fetches at play time; the Hub's catalog is cached in
// web/build/hub-catalog.json for six hours. Stock V15 is read from a local file only.
import { readFileSync, writeFileSync, mkdirSync, existsSync, statSync } from "node:fs";
import { deflateSync, crc32 } from "node:zlib";
import { createEngine } from "./engine.js";
import { fetchHubCatalog, entriesFromHub, fetchPackage, stockFromFile } from "./catalog.mjs";

const WEB = new URL("../../", import.meta.url);
const BUILD = new URL("build/", WEB);
const arg = (k, d) => process.argv.find((a) => a.startsWith(`--${k}=`))?.split("=")[1] ?? d;
const MHZ = +arg("mhz", 100);
const RATE = 48000;
const STOCK = arg("stock", "/Volumes/Q7Media-2025/Projects/Github/ChoralRootFM1/MvaveFM1Unbricker/FM-1.fwsc");
// the firmwares to test: Hub id and, where pinned, version
const WANT = [["felucca", "1.6.1"], ["x0x", "1.1.0"], ["sloop", "2.7.1"], ["sloopdx"], ["orbit"], ["choralroot", "0.25.1"], ["mvave"]];
const only = arg("only", "").split(",").filter(Boolean);

mkdirSync(new URL("test/", BUILD), { recursive: true });
mkdirSync(new URL("packages/", BUILD), { recursive: true });

const wasmPath = new URL("emu/fm1_web.wasm", BUILD);
if (!existsSync(wasmPath)) throw new Error("no web/build/emu/fm1_web.wasm: run web/engines/emu/build.sh");
const wasm = readFileSync(wasmPath);
const wasmBuf = wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength);

async function hubCatalog() {
  const p = new URL("hub-catalog.json", BUILD);
  if (existsSync(p) && Date.now() - statSync(p).mtimeMs < 6 * 3600e3) return JSON.parse(readFileSync(p));
  const hub = await fetchHubCatalog();
  writeFileSync(p, JSON.stringify(hub));
  return hub;
}

async function packageFor(entry) {
  const p = new URL(`packages/${entry.id}-${entry.version}.fwsc`, BUILD);
  if (existsSync(p)) return new Uint8Array(readFileSync(p));
  const { bytes } = await fetchPackage(entry);   // checks the catalog's SHA-256
  writeFileSync(p, bytes);
  return bytes;
}

function png(path, pixels) {
  const W = 240, H = 240, raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) {
    raw[y * (W * 3 + 1)] = 0;
    for (let x = 0; x < W; x++) {
      const v = pixels[y * W + x], o = y * (W * 3 + 1) + 1 + x * 3;
      raw[o] = ((v >> 11) & 31) * 255 / 31;
      raw[o + 1] = ((v >> 5) & 63) * 255 / 63;
      raw[o + 2] = (v & 31) * 255 / 31;
    }
  }
  const chunk = (type, data) => {
    const b = Buffer.alloc(12 + data.length);
    b.writeUInt32BE(data.length, 0);
    b.write(type, 4, "latin1");
    data.copy(b, 8);
    b.writeUInt32BE(crc32(b.subarray(4, 8 + data.length)), 8 + data.length);
    return b;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8; ihdr[9] = 2;
  writeFileSync(path, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]));
}

// a screen is "not blank" with at least 500 pixels off the most common colour
function lit(pixels) {
  const count = new Map();
  for (const v of pixels) count.set(v, (count.get(v) || 0) + 1);
  return pixels.length - Math.max(...count.values());
}

const PANEL0 = { keys: 0, buttons: 0, enc: new Int32Array(7), master: 900, keyVel: new Uint8Array(27) };

async function run(entry, fwsc) {
  const e = await createEngine({ sampleRate: RATE, files: { wasm: wasmBuf, fwsc }, flash: null,
    options: { id: entry.id, name: entry.name, version: entry.version, author: entry.author, mhz: MHZ } });
  const L = new Float32Array(128), R = new Float32Array(128);
  let frames = 0, wall = 0;
  const render = (secs, fn) => {
    const t0 = performance.now();
    let sum = 0, n = 0;
    for (let i = 0; i < secs * RATE; i += 128) {
      e.render(L, R);
      frames += 128;
      for (let k = 0; k < 128; k++) sum += L[k] * L[k] + R[k] * R[k];
      n += 256;
      if (fn) fn();
    }
    wall += performance.now() - t0;
    return Math.sqrt(sum / n);
  };
  e.setPanel(PANEL0);
  // boot: the first non-blank screen within 3 s of audio
  let bootAt = null;
  const tBoot = performance.now();
  for (let q = 0; q < 3 * RATE / 128 && bootAt === null; q += 16) {
    render(16 * 128 / RATE);
    if (lit(e.screen().pixels) >= 500) bootAt = +(e._stats().ms / 1000).toFixed(2);   // emulated seconds (boot runs ahead of the audio)
  }
  const bootWall = (performance.now() - tBoot) / 1000;
  render(1.5);   // settle (boot animations, the first screen)
  const fw0 = frames, w0 = wall, short0 = e._stats().short;
  const quiet = render(0.5);
  // a note key: C4 (key 7), held 0.6 s
  e.setPanel({ ...PANEL0, keys: 1 << 7 });
  const keyRms = render(0.6);
  e.setPanel(PANEL0);
  render(0.3);
  let how = "key", level = keyRms;
  const sounds = (v) => v > Math.max(4 * quiet, 2e-3);
  if (!sounds(keyRms)) {
    // a groovebox's prelude: PLAY (matrix button 12) starts its pattern
    e.setPanel({ ...PANEL0, taps: { keys: 0, buttons: 1 << 12 } });
    e.setPanel(PANEL0);
    level = render(1.5);
    how = "PLAY";
    if (!sounds(level)) {
      // and a key again while it plays (a sequencer track's note)
      e.setPanel({ ...PANEL0, keys: 1 << 7 });
      level = render(0.6);
      e.setPanel(PANEL0);
      how = "PLAY+key";
    }
  }
  // an encoder: KNOB1 (matrix encoder 2) five detents clockwise, then back; does the screen change?
  const seqBefore = e.screen().seq;
  const enc = new Int32Array(7); enc[2] = 5;
  e.setPanel({ ...PANEL0, enc });
  render(0.3);
  const encChanged = e.screen().seq !== seqBefore;
  enc[2] = -5;
  e.setPanel({ ...PANEL0, enc });
  render(0.3);
  // the steady speed: 3 s with a three-note chord held
  e.setPanel({ ...PANEL0, keys: (1 << 7) | (1 << 11) | (1 << 14) });
  const fs = frames, ws = wall;
  render(3);
  const steady = (frames - fs) / RATE / ((wall - ws) / 1000);
  e.setPanel(PANEL0);
  render(0.2);
  const scr = e.screen();
  png(new URL(`test/emu-${entry.id}.png`, BUILD), scr.pixels);
  const info = e.info(), stats = e._stats();
  const ok = bootAt !== null && sounds(level) && !e.halted();
  const r = {
    id: entry.id, version: entry.version, ok, bootAt, bootWall: +bootWall.toFixed(2), lit: lit(scr.pixels),
    quiet: +quiet.toFixed(5), level: +level.toFixed(5), how, rtf: +((frames - fw0) / RATE / ((wall - w0) / 1000)).toFixed(2),
    rtfChord: +steady.toFixed(2), ledSeq: e.leds().seq, encChanged, flashSeq: e.flash().seq, halted: e.halted(),
    emulatedMs: stats.ms, shortBoot: short0, shortAfter: stats.short - short0, jitRegions: stats.jit.regions, jitFailed: stats.jit.failed, latencyFrames: info.latencyFrames,
  };
  // the flash round trip: when the firmware saved something, a second engine boots from it
  const fl = e.flash();
  if (fl.seq && fl.bytes) {
    const e2 = await createEngine({ sampleRate: RATE, files: { wasm: wasmBuf, fwsc }, flash: fl.bytes, options: { mhz: MHZ } });
    for (let i = 0; i < RATE; i += 128) e2.render(L, R);
    r.flashRestore = `${fl.bytes.length / 4100} sectors, reboot ${e2.halted() ? "halted" : "ok"}`;
    e2.dispose();
  }
  e.dispose();
  return r;
}

const hub = await hubCatalog();
const entries = entriesFromHub(hub);
const results = [];
for (const [id, version] of WANT) {
  if (only.length && !only.includes(id)) continue;
  const entry = entries.find((f) => f.id === id);
  if (!entry) { results.push({ id, ok: false, error: "not in the Hub's catalog" }); continue; }
  if (version && entry.version !== version) {
    results.push({ id, ok: false, error: `the Hub has ${entry.version}, not ${version}` });
    continue;
  }
  try {
    let fwsc;
    if (entry.byo) {
      if (!existsSync(STOCK)) { results.push({ id, ok: false, error: `no local ${STOCK}` }); continue; }
      const own = await stockFromFile(new Uint8Array(readFileSync(STOCK)), hub);
      fwsc = own.bytes;
      entry.version = own.stock ? "V15" : own.product || "?";
    } else fwsc = await packageFor(entry);
    const r = await run(entry, fwsc);
    results.push(r);
    console.log(JSON.stringify(r));
  } catch (err) {
    results.push({ id, ok: false, error: String(err?.stack || err) });
    console.log(id, "ERROR", err);
  }
}
writeFileSync(new URL("test/emu-results.json", BUILD), JSON.stringify(results, null, 1));
console.log("\nfirmware            | boot s (emulated) | sound    | RTF   | RTF chord | result");
for (const r of results) {
  console.log(`${`${r.id} ${r.version ?? ""}`.padEnd(20)}| ${String(r.bootAt ?? "-").padEnd(6)} | ${String(r.how ?? "-").padEnd(8)} | ` +
    `${String(r.rtf ?? "-").padEnd(5)} | ${String(r.rtfChord ?? "-").padEnd(9)} | ${r.ok ? "PASS" : "FAIL " + (r.error ?? "")}`);
}
process.exit(results.every((r) => r.ok) ? 0 : 1);
