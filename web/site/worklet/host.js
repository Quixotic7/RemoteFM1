// SPDX-License-Identifier: GPL-3.0-only
// The AudioWorklet host, "fm1-host" (web/engine/API.md section 3): it runs one engine in the audio thread, feeds it
// the panel and MIDI, renders one quantum per process(), and posts the screen, LEDs, MIDI out and the flash's
// changes to the page (at most 30 Hz, only on change).
//
// Engines: a worklet can't import() at run time, so the page adds each engine with a tiny module of its own
// (js/audio.js engineShim) that imports the engine's file statically and registers it here:
//     import * as E from "<engine.js>"; globalThis.FM1_ENGINES["native"] = E;
// The registry is the worklet's one global scope (an AudioContext has a single AudioWorkletGlobalScope).

const ENGINES = (globalThis.FM1_ENGINES = globalThis.FM1_ENGINES || {});
export const registerEngine = (name, mod) => { ENGINES[name] = mod; };

const FRAME_EVERY_S = 1 / 30;

export class PanelMerge {   // the page's panel messages -> one setPanel per quantum (taps: down and up between two)
  constructor() {
    this.keys = 0; this.buttons = 0; this.master = 768; this.keyVel = new Uint8Array(27); this.enc = new Int32Array(7);
    this.everKeys = 0; this.everButtons = 0; this.sentKeys = 0; this.sentButtons = 0; this.dirty = false;
  }
  add(p) {
    this.keys = p.keys >>> 0; this.buttons = p.buttons >>> 0;
    this.everKeys = (this.everKeys | this.keys) >>> 0; this.everButtons = (this.everButtons | this.buttons) >>> 0;
    if (typeof p.master === "number") this.master = p.master;
    if (p.keyVel) this.keyVel.set(p.keyVel.subarray ? p.keyVel.subarray(0, 27) : p.keyVel.slice(0, 27));
    if (p.enc) for (let i = 0; i < 7; i++) this.enc[i] += p.enc[i] | 0;
    this.dirty = true;
  }
  take() {
    const taps = { keys: (this.everKeys & ~this.keys & ~this.sentKeys) >>> 0, buttons: (this.everButtons & ~this.buttons & ~this.sentButtons) >>> 0 };
    const p = { keys: this.keys, buttons: this.buttons, enc: this.enc.slice(), master: this.master, keyVel: this.keyVel.slice() };
    if (taps.keys || taps.buttons) p.taps = taps;
    this.enc.fill(0);
    this.sentKeys = this.keys; this.sentButtons = this.buttons;
    this.everKeys = this.keys; this.everButtons = this.buttons;
    this.dirty = false;
    return p;
  }
}

// the wall clock in ms (an AudioWorkletGlobalScope may have no performance object)
const clock = () => (globalThis.performance && typeof globalThis.performance.now === "function" ? globalThis.performance.now() : Date.now());
if (typeof AudioWorkletProcessor !== "undefined") {
  class Host extends AudioWorkletProcessor {
    constructor() {
      super();
      this.engine = null; this.loading = 0; this.panel = new PanelMerge();
      this.since = 0; this.seen = { screen: -1, leds: -1, flash: -1 }; this.haltSent = false;
      this.mon = null;   // the render load: { busy, frames, wall0 } (a report every half second: page js/cpu.js)
      this.rec = null; this.recFree = [];   // the recorder's take, and its spare chunk buffers (see recStart)
      this.port.onmessage = (e) => this.onMessage(e.data);
    }
    post(m, transfer) { try { this.port.postMessage(m, transfer || []); } catch (_) { /* (the page went away) */ } }
    fail(where, e) { this.post({ type: "error", message: `${where}: ${(e && e.message) || e}` }); }
    async load(m) {
      const ticket = ++this.loading;
      this.recStop("engine");   // (the page ends a take before it loads: this is the backstop)
      this.drop();
      const mod = ENGINES[m.engine];
      if (!mod || typeof mod.createEngine !== "function") return this.fail("load", `no engine "${m.engine}" registered`);
      try {
        const eng = await mod.createEngine({ sampleRate, files: m.files || {}, flash: m.flash || null, options: m.options || {} });
        if (ticket !== this.loading) { try { eng.dispose(); } catch (_) {} return; }
        this.engine = eng;
        this.seen = { screen: -1, leds: -1, flash: -1 };
        this.haltSent = false;
        this.panel.dirty = true;
        this.mon = null;
        this.post({ type: "info", engine: m.engine, engineVersion: mod.ENGINE && mod.ENGINE.version, ...eng.info() });
      } catch (e) { this.fail("load", e); }
    }
    drop() { if (this.engine) { try { this.engine.dispose(); } catch (_) {} this.engine = null; } }
    onMessage(m) {
      if (!m || !m.type) return;
      if (m.type === "load") return this.load(m);
      if (m.type === "unload") { this.loading++; this.recStop("engine"); this.drop(); return; }
      if (m.type === "panel") return this.panel.add(m);
      if (m.type === "rec") return m.on ? this.recStart(m) : this.recStop("stop");
      if (m.type === "recBuf") { if (this.rec && m.l && m.l.length === this.rec.n) this.recFree.push([m.l, m.r]); return; }
      if (!this.engine) return;
      try {
        if (m.type === "midi") this.engine.midiIn(m.bytes instanceof Uint8Array ? m.bytes : Uint8Array.from(m.bytes));
        else if (m.type === "flashReq") {
          const f = this.engine.flash(true);
          this.post({ type: "flash", seq: f.seq, bytes: f.bytes ? f.bytes.slice() : null, requested: true });
        }
      } catch (e) { this.fail(m.type, e); }
    }
    process(inputs, outputs) {
      const out = outputs[0], L = out[0], R = out[1] || out[0];
      const eng = this.engine;
      if (!eng) { if (L) L.fill(0); if (R) R.fill(0); return true; }
      try {
        if (this.panel.dirty) eng.setPanel(this.panel.take());
        const t0 = clock();
        eng.render(L, R);
        if (this.rec) this.capture(L, R);   // (inside the measured time: the load reports include it)
        this.measure(clock(), t0, L.length);
        // the firmware's MIDI out every quantum, posted at once: a manager's reply (SysEx) waits ~3 ms, not a screen
        // frame, and the core's own out queue never has to hold more than a quantum's worth
        const mo = eng.midiOut();
        if (mo && mo.length) this.post({ type: "midiOut", bytes: Uint8Array.from(mo) });
        this.since += L.length / sampleRate;
        if (this.since >= FRAME_EVERY_S) { this.since = 0; this.publish(eng); }
      } catch (e) {
        this.fail("render", e);
        this.recStop("error");
        this.drop();
      }
      return true;
    }
    // The recorder (page js/rec-ui.js, js/recorder.js): the exact PCM the engine rendered, before the page's volume,
    // copied into chunk buffers of m.n frames (about 0.25 s); a full chunk is posted with its buffers transferred, and
    // the page sends them back (recBuf) for the next one, so nothing is allocated while rendering (two spare pairs up
    // front; a missing one is made). The take starts at the next quantum and stops at m.max frames (the cap) or at a
    // stop, when the part-filled chunk goes as the final one. The meter: each channel's peak, with publish() (30 Hz).
    recStart(m) {
      if (this.rec) this.recStop("restart");
      const n = m.chunkFrames | 0;
      this.recFree = this.recFree.filter((p) => p[0].length === n);
      while (this.recFree.length < 2) this.recFree.push([new Float32Array(n), new Float32Array(n)]);
      const [l, r] = this.recFree.pop();
      this.rec = { id: m.id, n, max: m.maxFrames > 0 ? m.maxFrames : Infinity, frames: 0, at: 0, l, r, start: -1, pl: 0, pr: 0 };
    }
    recChunk(final, reason) {
      const r = this.rec;
      this.post({ type: "recChunk", id: r.id, l: r.l, r: r.r, frames: r.at, total: r.frames, final, reason: reason || null,
                  startFrame: r.start, endFrame: r.start < 0 ? -1 : r.start + r.frames }, [r.l.buffer, r.r.buffer]);
      if (final) { this.rec = null; return; }
      const p = this.recFree.pop() || [new Float32Array(r.n), new Float32Array(r.n)];
      r.l = p[0]; r.r = p[1]; r.at = 0;
    }
    recStop(reason) { if (this.rec) this.recChunk(true, reason); }
    capture(L, R) {
      const r = this.rec, n = L.length;
      if (r.start < 0) r.start = currentFrame;
      let off = 0, pl = r.pl, pr = r.pr;
      for (let i = 0; i < n; i++) { const a = L[i] < 0 ? -L[i] : L[i], b = R[i] < 0 ? -R[i] : R[i]; if (a > pl) pl = a; if (b > pr) pr = b; }
      r.pl = pl; r.pr = pr;
      while (off < n) {
        const k = Math.min(n - off, r.n - r.at, r.max - r.frames);
        if (k === n) { r.l.set(L, r.at); r.r.set(R, r.at); }
        else { r.l.set(L.subarray(off, off + k), r.at); r.r.set(R.subarray(off, off + k), r.at); }
        r.at += k; r.frames += k; off += k;
        if (r.frames >= r.max) return this.recStop("cap");
        if (r.at === r.n) this.recChunk(false);
      }
    }
    // the share of the audio time spent rendering, and how far the wall clock ran ahead of the audio clock (late
    // quanta: dropouts). A window whose wall time is over twice its audio time is a pause (a sleeping machine), not load.
    // peak: the busiest stretch of eight quanta (21 ms), its render time over its audio time (above 1: those quanta
    // were late unless the device's buffer hid it; eight, as the worklet's clock may tick in whole milliseconds);
    // p99Ms / maxMs the 99th-percentile and the slowest quantum; short the engine's own underruns (when it counts them).
    measure(t1, t0, n) {
      const m = this.mon || (this.mon = { busy: 0, frames: 0, wall0: t0, q: 0, g: 0, gn: 0, peak: 0 });
      const dt = t1 - t0;
      m.busy += dt; m.frames += n;
      if (!this.qt) this.qt = new Float32Array(512);
      if (m.q < this.qt.length) this.qt[m.q++] = dt;
      m.g += dt;
      if (++m.gn === 8) { m.peak = Math.max(m.peak, m.g / ((8000 * n) / sampleRate)); m.g = 0; m.gn = 0; }
      if (m.frames < sampleRate / 2) return;
      const audioMs = (m.frames / sampleRate) * 1000, wallMs = t1 - m.wall0;
      const q = this.qt.subarray(0, m.q).sort(), p99 = q[Math.min(q.length - 1, Math.floor(q.length * 0.99))];
      const st = typeof this.engine?._stats === "function" ? this.engine._stats() : null;
      const short = st && typeof st.short === "number" ? st.short : null;
      if (wallMs < 2 * audioMs) this.post({ type: "load", load: m.busy / audioMs, lateMs: Math.max(0, wallMs - audioMs - 20),
        peak: m.peak, p99Ms: p99, maxMs: q[q.length - 1], short });
      this.mon = { busy: 0, frames: 0, wall0: t1, q: 0, g: 0, gn: 0, peak: 0 };
    }
    publish(eng) {
      const rc = this.rec;
      if (rc && rc.start >= 0) { this.post({ type: "recLevel", id: rc.id, peak: [rc.pl, rc.pr], frames: rc.frames }); rc.pl = 0; rc.pr = 0; }
      const s = eng.screen();
      if (s && s.seq !== this.seen.screen) {
        this.seen.screen = s.seq;
        const px = new Uint16Array(s.pixels);
        this.post({ type: "frame", seq: s.seq, pixels: px }, [px.buffer]);
      }
      const l = eng.leds();
      if (l && l.seq !== this.seen.leds) {
        this.seen.leds = l.seq;
        this.post({ type: "leds", levels: Uint8Array.from(l.levels), playGreen: l.playGreen | 0 });
      }
      const f = eng.flash();
      if (f && f.seq !== this.seen.flash) {
        const first = this.seen.flash === -1;
        this.seen.flash = f.seq;
        if (!first) this.post({ type: "flash", seq: f.seq, bytes: null });
      }
      const h = eng.halted();
      if (h && !this.haltSent) { this.haltSent = true; this.post({ type: "halted", code: h }); }
    }
  }
  registerProcessor("fm1-host", Host);
}
