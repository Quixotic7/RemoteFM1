// SPDX-License-Identifier: GPL-3.0-only
// RemoteFM1's own packages in the emulated engine: does a key make a sound? The latest release's two packages
// (web/installer/releases.json, the files in web/installer/firmware) boot in fm1_web.wasm and a held C4 must sound,
// with no network (test.mjs, the wider check, fetches the Hub's firmwares).
// 2026-11-08: RemoteFM1 0.3.4..0.3.6 were silent in the player's emulator. Their felucca-bsp checks the audio PLL
// before it starts the I2S, and the emulator's PLL registers read 0 (patches/0005). The page drew the screen and
// played nothing; native cores were fine. The Pages workflow runs this after it builds the emulator.
//   node web/engines/emu/own-test.mjs [--wasm=path/fm1_web.wasm]
import { readFileSync, existsSync } from "node:fs";
import { createEngine } from "./engine.js";

const WEB = new URL("../../", import.meta.url);
const arg = (k, d) => process.argv.find((a) => a.startsWith(`--${k}=`))?.slice(k.length + 3) ?? d;
const wasmPath = arg("wasm", null) ?? new URL("build/emu/fm1_web.wasm", WEB);
if (!existsSync(wasmPath)) { console.error(`no ${wasmPath}: run web/engines/emu/build.sh`); process.exit(2); }
const wasm = readFileSync(wasmPath);
const wasmBuf = wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength);
const RATE = 48000;
const PANEL0 = { keys: 0, buttons: 0, enc: new Int32Array(7), master: 900, keyVel: new Uint8Array(27) };

const rel = JSON.parse(readFileSync(new URL("installer/releases.json", WEB), "utf8"));
const latest = (rel.releases || rel)[0];
const files = [`remote-${latest.version}.fwsc`, latest.usbOnly && latest.usbOnly.file].filter(Boolean);

let fails = 0;
for (const f of files) {
  const fwsc = new Uint8Array(readFileSync(new URL(`installer/firmware/${f}`, WEB)));
  const e = await createEngine({ sampleRate: RATE, files: { wasm: wasmBuf, fwsc }, flash: null,
    options: { id: "remotefm1", name: "RemoteFM1", version: latest.version, mhz: 100 } });
  const L = new Float32Array(128), R = new Float32Array(128);
  const rms = (secs) => {
    let s = 0, n = 0;
    for (let i = 0; i < secs * RATE; i += 128) { e.render(L, R); for (let k = 0; k < 128; k++) s += L[k] * L[k] + R[k] * R[k]; n += 256; }
    return Math.sqrt(s / n);
  };
  e.setPanel(PANEL0);
  rms(2.5);                                         // boot and settle
  const quiet = rms(0.4);
  e.setPanel({ ...PANEL0, keys: 1 << 7 });          // C4 held
  const held = rms(0.6);
  e.setPanel(PANEL0);
  const ok = held > Math.max(4 * quiet, 2e-3) && !e.halted();
  if (!ok) fails++;
  console.log(`${`${f}: a held key sounds in the emulator (rms ${held.toFixed(4)}, quiet ${quiet.toFixed(4)})`.padEnd(92)} ${ok ? "ok" : "FAIL"}`);
}
console.log(fails ? `own-test: ${fails} of ${files.length} FAILED` : `own-test passed (${files.length})`);
process.exit(fails ? 1 : 0);
