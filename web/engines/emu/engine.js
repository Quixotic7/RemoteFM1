// The emu engine: fm1-emu (github.com/joakimeriksson/fm1-emu, MIT, (c) 2026 Joakim Eriksson) behind
// RemoteFM1's web engine interface (web/engine/API.md). fm1-emu emulates the FM-1's AC791N chip and
// its two pi32v2 cores, so any FM-1 update package (.fwsc) runs unchanged.
//
// Loads inside an AudioWorkletGlobalScope (no fetch, no DOM, no dynamic import, no TextDecoder) and
// in Node. Everything is synchronous after createEngine. The module is fm1-emu's own web crate
// (crates/fm1-web, built by build.sh): plain extern "C" exports, no bindings generator.
//
// Hot code is compiled to WebAssembly here, in this scope: the crate hands a hot block over as a
// small module (env.fm1_jit_compile), it is compiled synchronously (new WebAssembly.Module, allowed
// off the main thread, so in a worklet) and installed in the crate's exported function table. That
// is what fm1-emu's own page does in its AudioWorklet (web/worklet.js); no Worker is needed.
//
// Audio: the machine produces 44.1 kHz (the firmware's I2S DMA); render() runs it until enough
// frames are there and resamples (4-point Hermite) to the context rate, a straight copy at 44.1 kHz.

export const ENGINE = { name: "emu", version: "0.1" };

const FS = 44100;
const QUANTUM = 128;
const NKEY = 27, NBTN = 14, NENC = 7;
const KEY0 = 14;                 // matrix id of note key 0 (F3): ids 0..13 buttons, 14..40 keys
const MIN_HOLD_MS = 30;          // a press is held at least this long in emulated time (debounce)
const LED_HZ = 30;
const SECTOR = 4096, RECORD = 4 + SECTOR;
const FLASH_QUIET_MS = 250;      // emulated ms without flash writes before a save is offered

// hal/fm1_input.h FM1_KEYMAP (fm1-soc periph.rs KEYMAP): key id at (row, column). LED line l of a
// column is row l + 1; fm1-emu reports one lit fraction per key id.
const KEYMAP = [
  [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
  [5, 11, 4, 10, 3, 9, 2, 8, -1, -1, -1],
  [34, 35, 36, 37, 38, 40, 39, 13, 7, 6, 12],
  [23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33],
  [0, 1, 15, 14, 17, 16, 19, 18, 20, 21, 22],
  [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
];
// levels[col*4+line] <- fm1-emu's LED of key id LED_ID[col*4+line] (-1: no key there, e.g. PLAY's
// green LED at column 8 line 0, which fm1-emu does not model)
const LED_ID = new Int8Array(44);
for (let col = 0; col < 11; col++) for (let line = 0; line < 4; line++) LED_ID[col * 4 + line] = KEYMAP[line + 1][col];
// fm1-emu's panel draws an LED fully lit from an on-fraction of 0.08 (one column of eleven: 0.091)
const LED_FULL = 0.08;

// the FM-1's printed labels, FM1P_B_* order (FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+);
// the emulator can't know a firmware's own: options.buttonNames overrides
const BUTTON_NAMES = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];

export const CREDITS = "Emulated by fm1-emu, the FM-1 emulator by Joakim Eriksson (MIT licence): " +
  "https://github.com/joakimeriksson/fm1-emu";

const modules = new WeakMap();   // compiled once per wasm ArrayBuffer

async function moduleOf(wasm) {
  if (wasm instanceof WebAssembly.Module) return wasm;
  const key = wasm instanceof ArrayBuffer ? wasm : wasm.buffer;
  let m = modules.get(key);
  if (!m) {
    m = await WebAssembly.compile(wasm);
    modules.set(key, m);
  }
  return m;
}

// The JIT's host side (fm1-emu web/jit.js, crates/fm1-core/src/jit_wasm.rs): compile a hot block's
// module and put its `run` in the function table; 0 = it stays interpreted.
function jitImports(exports, stats) {
  return {
    fm1_jit_compile(ptr, len) {
      try {
        const x = exports();
        const bytes = new Uint8Array(x.memory.buffer, ptr, len).slice();
        const table = x.__indirect_function_table;
        const run = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env: { memory: x.memory, table } }).exports.run;
        const slot = table.grow(1);
        table.set(slot, run);
        stats.regions++;
        return slot;
      } catch (e) {
        stats.failed++;
        return 0;
      }
    },
  };
}

// Parse a saved flash: fm1-emu's sector records (u32 LE offset + 4096 bytes, its .fm1save sectors
// packed, what flash() returns), or a whole 1 MB image (diffed against the package's own flash).
function sectorsFrom(saved, base) {
  if (saved.length === base.length) return packChanged(base, saved);
  if (saved.length % RECORD) throw new Error("emu: saved flash is neither sector records nor a whole flash image");
  return saved;
}

function packChanged(base, flash) {
  const at = [];
  for (let s = 0; s < flash.length; s += SECTOR) {
    for (let i = s; i < s + SECTOR; i++) if (base[i] !== flash[i]) { at.push(s); break; }
  }
  const out = new Uint8Array(at.length * RECORD);
  const dv = new DataView(out.buffer);
  at.forEach((s, k) => {
    dv.setUint32(k * RECORD, s, true);
    out.set(flash.subarray(s, s + SECTOR), k * RECORD + 4);
  });
  return out;
}

// 4-point, 3rd-order Hermite
function hermite(xm1, x0, x1, x2, t) {
  const c = (x1 - xm1) * 0.5, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5, b = w + a;
  return ((a * t - b) * t + c) * t + x0;
}

export async function createEngine({ sampleRate, files, flash = null, options = {} }) {
  if (!files?.wasm || !files?.fwsc) throw new Error("emu: files.wasm and files.fwsc are needed");
  const mhz = options.mhz ?? 100;
  const module = await moduleOf(files.wasm);
  const jit = { regions: 0, failed: 0 };
  let x = null;
  const instance = await WebAssembly.instantiate(module, { env: jitImports(() => x, jit) });
  x = instance.exports;
  x.fm1_jit(options.jit === false ? 0 : 1, options.jitPerRender ?? 16);

  const put = (bytes) => {
    const p = x.fm1_alloc(bytes.byteLength);
    new Uint8Array(x.memory.buffer, p, bytes.byteLength).set(bytes);
    return p;
  };
  const pkg = files.fwsc instanceof Uint8Array ? files.fwsc : new Uint8Array(files.fwsc);
  if (x.fm1_boot_fwsc(put(pkg), pkg.byteLength, 0, 0, mhz) !== 0) throw new Error("emu: not an FM-1 update package");
  if (x.fm1_board() !== 0) throw new Error("emu: an SMK-37 package; this engine runs FM-1 packages");
  const flashLen = x.fm1_flash_len();
  const base = new Uint8Array(x.memory.buffer, x.fm1_flash_base(), flashLen).slice();
  if (flash?.byteLength) {
    const rec = sectorsFrom(flash instanceof Uint8Array ? flash : new Uint8Array(flash), base);
    if (rec.length && x.fm1_flash_restore(put(rec), rec.length) !== 0) throw new Error("emu: invalid saved flash");
  }
  if (options.syx?.byteLength && x.fm1_load_syx(put(options.syx), options.syx.byteLength) === 1) {
    throw new Error("emu: this firmware cannot load that DX7 bank");
  }

  const budget = Math.ceil(mhz * 1e6 * 0.02);   // at most 20 ms of emulated time per render (fm1-emu's)
  const step = FS / sampleRate;
  const direct = Math.abs(step - 1) < 1e-9;
  // source FIFO (44.1 kHz), in frames
  const CAP = 8192;
  const fl = new Float32Array(CAP), fr = new Float32Array(CAP);
  let rd = 0, avail = 0, frac = 0;
  // history for the interpolator: the frame before the read position
  let hl = 0, hr = 0;
  // frames run ahead: the FIFO's residue (< one 128-frame chunk) and the interpolator's 2 frames
  const latencyFrames = direct ? QUANTUM : Math.ceil((QUANTUM + 2) / step);

  let outL = null, outR = null, outBuf = null;
  const views = () => {
    if (outBuf !== x.memory.buffer) {
      outBuf = x.memory.buffer;
      outL = new Float32Array(outBuf, x.fm1_out_l(), 1024);
      outR = new Float32Array(outBuf, x.fm1_out_r(), 1024);
      lcd = new Uint16Array(outBuf, x.fm1_lcd(), 240 * 240);
    }
  };
  let lcd = null;

  function pull() {
    const have = x.fm1_render(QUANTUM, budget);
    views();
    for (let i = 0; i < have; i++) {
      const w = (rd + avail + i) % CAP;
      fl[w] = outL[i];
      fr[w] = outR[i];
    }
    avail += have;
    return have;
  }
  const at = (buf, k) => buf[(rd + k) % CAP];

  // ---- panel state
  const emuDown = new Uint8Array(41), wantDown = new Uint8Array(41);
  const pressedAt = new Float64Array(41), releaseAt = new Float64Array(41);   // releaseAt 0: none pending
  let master = -1;
  function press(id, now) {
    x.fm1_key(id, 1);
    emuDown[id] = 1;
    pressedAt[id] = now;
    releaseAt[id] = 0;
  }
  function release(id, now) {
    if (now - pressedAt[id] >= MIN_HOLD_MS) {
      x.fm1_key(id, 0);
      emuDown[id] = 0;
      releaseAt[id] = 0;
    } else releaseAt[id] = pressedAt[id] + MIN_HOLD_MS;
  }
  function pendingReleases() {
    const now = x.fm1_ms();
    for (let id = 0; id < 41; id++) {
      if (releaseAt[id] && !wantDown[id] && now >= releaseAt[id]) {
        x.fm1_key(id, 0);
        emuDown[id] = 0;
        releaseAt[id] = 0;
      }
    }
  }

  // ---- LEDs, sampled at 30 Hz of rendered audio
  const levels = new Uint8Array(44);
  let ledSeq = 0, ledFrames = 0;
  function sampleLeds() {
    const f = new Float32Array(x.memory.buffer, x.fm1_leds(), 41);
    let changed = false;
    for (let k = 0; k < 44; k++) {
      const id = LED_ID[k];
      const v = id < 0 ? 0 : Math.round(Math.min(1, f[id] / LED_FULL) * 255);
      if (v !== levels[k]) { levels[k] = v; changed = true; }
    }
    if (changed) ledSeq++;
  }

  // ---- flash: offered once quiet (not busy, no write for 250 emulated ms)
  let flashRev = x.fm1_flash_revision(), flashChangedMs = 0, flashSeq = 0, flashPublishedRev = flashRev;
  let flashBytes = null;   // the sector records at flashSeq (null: unchanged since boot)
  function watchFlash() {
    const rev = x.fm1_flash_revision(), now = x.fm1_ms();
    if (rev !== flashRev) { flashRev = rev; flashChangedMs = now; }
    if (rev !== flashPublishedRev && now - flashChangedMs >= FLASH_QUIET_MS && !x.fm1_flash_busy()) {
      flashPublishedRev = rev;
      flashBytes = packChanged(base, new Uint8Array(x.memory.buffer, x.fm1_flash(), flashLen));
      flashSeq++;
    }
  }

  let disposed = false, short = 0;
  const engine = {
    info() {
      return {
        id: options.id ?? null,
        name: options.name ?? null,
        version: options.version ?? null,
        license: options.license ?? null,
        source: options.source ?? null,
        credits: [options.author ? `${options.name ?? "Firmware"} by ${options.author}` : null, CREDITS].filter(Boolean),
        family: options.family ?? "fm1",
        latencyFrames,
        keymap: null,
        buttonNames: options.buttonNames ?? BUTTON_NAMES.slice(),
        engine: { ...ENGINE, emulator: "fm1-emu", mhz, jit: { ...jit } },
      };
    },

    setPanel(p) {
      if (disposed) return;
      const now = x.fm1_ms();
      const keys = p.keys >>> 0, buttons = p.buttons >>> 0;
      const tk = (p.taps?.keys ?? 0) >>> 0, tb = (p.taps?.buttons ?? 0) >>> 0;
      for (let id = 0; id < 41; id++) {
        const isKey = id >= KEY0, bit = isKey ? id - KEY0 : id;
        const want = ((isKey ? keys : buttons) >>> bit) & 1;
        const tap = ((isKey ? tk : tb) >>> bit) & 1;
        wantDown[id] = want;
        if (want) {
          if (!emuDown[id]) press(id, now);
          else releaseAt[id] = 0;
        } else if (emuDown[id]) {
          if (!releaseAt[id]) release(id, now);
        } else if (tap) {
          press(id, now);
          releaseAt[id] = now + MIN_HOLD_MS;
        }
      }
      if (p.enc) for (let e = 0; e < NENC; e++) if (p.enc[e]) x.fm1_enc(e, p.enc[e] | 0);
      if (p.master !== undefined && p.master !== master) {
        master = Math.max(0, Math.min(1023, p.master | 0));
        x.fm1_pot(master);
      }
      // keyVel: the FM-1's keys have no velocity, nor has the emulated matrix
    },

    render(left, right) {
      const n = left.length;
      if (disposed || x.fm1_halted()) { left.fill(0); right.fill(0); return; }   // (after dispose: silence)
      pendingReleases();
      // frames needed past the read position: n outputs at `step`, plus the interpolator's two ahead
      const need = direct ? n : Math.floor(frac + (n - 1) * step) + 3;
      while (avail < need) if (pull() < QUANTUM) { short++; break; }   // the machine ran out of budget
      if (direct) {
        const k = Math.min(n, avail);
        for (let i = 0; i < k; i++) { left[i] = at(fl, i); right[i] = at(fr, i); }
        for (let i = k; i < n; i++) { left[i] = 0; right[i] = 0; }
        rd = (rd + k) % CAP;
        avail -= k;
      } else {
        let pos = frac, i = 0;
        for (; i < n; i++) {
          const k = Math.floor(pos);
          if (k + 2 >= avail) break;
          const t = pos - k;
          const l0 = k ? at(fl, k - 1) : hl, r0 = k ? at(fr, k - 1) : hr;
          left[i] = hermite(l0, at(fl, k), at(fl, k + 1), at(fl, k + 2), t);
          right[i] = hermite(r0, at(fr, k), at(fr, k + 1), at(fr, k + 2), t);
          pos += step;
        }
        for (let j = i; j < n; j++) { left[j] = 0; right[j] = 0; }
        const used = Math.min(Math.floor(pos), avail);
        if (used > 0) { hl = at(fl, used - 1); hr = at(fr, used - 1); }
        rd = (rd + used) % CAP;
        avail -= used;
        frac = i < n ? 0 : pos - Math.floor(pos);
      }
      ledFrames += n;
      if (ledFrames >= sampleRate / LED_HZ) {
        ledFrames = 0;
        sampleLeds();
        watchFlash();
      }
    },

    screen() {
      if (disposed) return { seq: 0, pixels: new Uint16Array(240 * 240) };
      views();
      return { seq: x.fm1_lcd_revision(), pixels: lcd };
    },

    leds() {
      return { seq: ledSeq, levels, playGreen: 0 };
    },

    midiIn(bytes) {
      if (disposed) return;
      for (let i = 0; i < bytes.length; i++) x.fm1_midi(bytes[i]);
    },

    // fm1-emu has no MIDI out: its UART TX "has no pin" and USB MIDI isn't modelled
    midiOut() {
      return new Uint8Array(0);
    },

    flash() {
      return {
        seq: flashSeq,
        get bytes() { return flashBytes ? flashBytes.slice() : null; },
      };
    },

    halted() {
      return disposed || x.fm1_halted() ? 1 : 0;
    },

    dispose() {
      disposed = true;
      x = null;                  // drop the instance (and its memory) for the GC
      outBuf = outL = outR = lcd = null;
    },

    // not in API.md: test and diagnostics only
    _stats() {
      if (disposed) return null;
      return { ms: x.fm1_ms(), minsns: x.fm1_minsns(), jit: { ...jit }, avail, short };
    },
  };
  return engine;
}
