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
      this.port.onmessage = (e) => this.onMessage(e.data);
    }
    post(m, transfer) { try { this.port.postMessage(m, transfer || []); } catch (_) { /* (the page went away) */ } }
    fail(where, e) { this.post({ type: "error", message: `${where}: ${(e && e.message) || e}` }); }
    async load(m) {
      const ticket = ++this.loading;
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
      if (m.type === "unload") { this.loading++; this.drop(); return; }
      if (m.type === "panel") return this.panel.add(m);
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
        this.measure(clock(), t0, L.length);
        this.since += L.length / sampleRate;
        if (this.since >= FRAME_EVERY_S) { this.since = 0; this.publish(eng); }
      } catch (e) {
        this.fail("render", e);
        this.drop();
      }
      return true;
    }
    // the share of the audio time spent rendering, and how far the wall clock ran ahead of the audio clock (late
    // quanta: dropouts). A window whose wall time is over twice its audio time is a pause (a sleeping machine), not load.
    measure(t1, t0, n) {
      const m = this.mon || (this.mon = { busy: 0, frames: 0, wall0: t0 });
      m.busy += t1 - t0; m.frames += n;
      if (m.frames < sampleRate / 2) return;
      const audioMs = (m.frames / sampleRate) * 1000, wallMs = t1 - m.wall0;
      if (wallMs < 2 * audioMs) this.post({ type: "load", load: m.busy / audioMs, lateMs: Math.max(0, wallMs - audioMs - 20) });
      this.mon = { busy: 0, frames: 0, wall0: t1 };
    }
    publish(eng) {
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
      const mo = eng.midiOut();
      if (mo && mo.length) this.post({ type: "midiOut", bytes: Uint8Array.from(mo) });
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
