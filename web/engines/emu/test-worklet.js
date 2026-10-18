// The browser test's AudioWorklet host (test-browser.html): the emu engine inside a real
// AudioWorkletGlobalScope, as W3's fm1-host will run it. It boots, renders 2 s, holds a note key for
// 0.6 s, and reports the levels, the screen and how long the engine took per quantum.
import { createEngine } from "./engine.js";

const PANEL0 = { keys: 0, buttons: 0, enc: new Int32Array(7), master: 900, keyVel: new Uint8Array(27) };

class Host extends AudioWorkletProcessor {
  constructor() {
    super();
    this.e = null;
    this.port.onmessage = async (m) => {
      if (m.data.type !== "load") return;
      try {
        const t0 = Date.now();
        this.e = await createEngine({ sampleRate, files: { wasm: m.data.wasm, fwsc: new Uint8Array(m.data.fwsc) },
          flash: null, options: { mhz: m.data.mhz ?? 100, id: "test" } });
        this.e.setPanel(PANEL0);
        this.frames = 0; this.busy = 0; this.worst = 0; this.sum = [0, 0, 0]; this.n = [0, 0, 0];
        this.bootLit = null;
        this.port.postMessage({ type: "booted", ms: Date.now() - t0, info: this.e.info() });
      } catch (err) {
        this.port.postMessage({ type: "error", message: String(err) });
      }
    };
  }

  process(_in, outputs) {
    const out = outputs[0], e = this.e;
    if (!e || this.done) return true;
    const t0 = Date.now();
    e.render(out[0], out[1] || new Float32Array(out[0].length));
    const dt = Date.now() - t0;
    const n = out[0].length, s = this.frames / sampleRate;
    if (s > 0.5) { this.busy += dt; this.worst = Math.max(this.worst, dt); this.busyFrames = (this.busyFrames || 0) + n; }
    // phase 0: idle (0.5..2 s), 1: key held (2..2.6 s), 2: after
    const ph = s < 2 ? 0 : s < 2.6 ? 1 : 2;
    if (s >= 1.5 && s < 2) { for (const v of out[0]) this.sum[0] += v * v; this.n[0] += n; }
    if (s >= 2.1 && s < 2.6) { for (const v of out[0]) this.sum[1] += v * v; this.n[1] += n; }
    if (ph === 1 && !this.pressed) { this.pressed = true; e.setPanel({ ...PANEL0, keys: 1 << 7 }); }
    if (ph === 2 && !this.released) { this.released = true; e.setPanel(PANEL0); }
    if (this.bootLit === null && this.frames % 4096 < n) {
      const px = e.screen().pixels;
      let lit = 0;
      for (let i = 0; i < px.length; i++) if (px[i] !== px[0]) lit++;
      if (lit > 500) this.bootLit = s;
    }
    this.frames += n;
    if (s >= 3) {
      this.done = true;
      const scr = e.screen();
      const pixels = scr.pixels.slice();
      const rms = (k) => Math.sqrt(this.sum[k] / Math.max(1, this.n[k]));
      const audioMs = 1000 * this.busyFrames / sampleRate;
      this.port.postMessage({ type: "result", sampleRate, quiet: rms(0), key: rms(1), bootLitAt: this.bootLit,
        busyMs: this.busy, audioMs, load: this.busy / audioMs, rtf: audioMs / Math.max(1, this.busy), worstMs: this.worst,
        quantumMs: 1000 * n / sampleRate, leds: e.leds(), halted: e.halted(), stats: e._stats(), pixels }, [pixels.buffer]);
    }
    return true;
  }
}
registerProcessor("emu-test-host", Host);
