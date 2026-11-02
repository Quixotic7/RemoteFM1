// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 RemoteFM1 contributors
// The SMK-37 emulator's part of fm1-base's worklet (loaded before worklet.js): the page's { type: "app" } messages
// set the SMK-37's controls (the wasm's web_smk_* exports), and about 30 times a second its lights go back to the
// page as { type: "app", op: "lights" } when they changed.
// A quick click's press and release can arrive between two audio blocks, before the firmware has run at all: each
// press is held at least HOLD_MS of the firmware's time before its release is applied.

const HOLD_MS = 30;

globalThis.fm1App = {
  load(fm1) {
    this.last = "";
    this.at = 0;
    this.down = new Map();      // control -> firmware ms its press was applied
    this.later = new Map();     // control -> { due, run }: a release waiting for its press's HOLD_MS
    this.btns = 0;              // the buttons as the firmware has them
  },
  // a press now; a release now or once the press has been held HOLD_MS
  edge(fm1, id, press, run) {
    const now = fm1.ex.web_now() >>> 0, w = this.later.get(id);
    if (w) { this.later.delete(id); w.run(); }
    if (press) { this.down.set(id, now); run(); return; }
    const t = this.down.get(id);
    this.down.delete(id);
    if (t !== undefined && now - t < HOLD_MS) this.later.set(id, { due: t + HOLD_MS, run });
    else run();
  },
  setButtons(fm1, mask) {
    const ex = fm1.ex, ch = (mask ^ this.btns) >>> 0;
    for (let b = 0; b < 32; b++) {
      const bit = 1 << b;
      if (!(ch & bit)) continue;
      const press = !!(mask & bit);
      this.edge(fm1, "b" + b, press, () => {
        this.applied = press ? (this.applied | bit) >>> 0 : (this.applied & ~bit) >>> 0;
        ex.web_smk_buttons(this.applied);
      });
    }
    this.btns = mask >>> 0;
  },
  message(fm1, m) {
    const ex = fm1.ex;
    if (this.applied === undefined) this.applied = 0;
    switch (m.op) {
      case "key": this.edge(fm1, "k" + m.k, m.vel > 0, () => ex.web_smk_key(m.k, m.vel)); break;
      case "pad": this.edge(fm1, "p" + m.p, m.vel > 0, () => ex.web_smk_pad(m.p, m.vel)); break;
      case "btns": this.setButtons(fm1, m.mask >>> 0); break;
      case "enc": ex.web_smk_enc(m.e, m.n | 0); break;
      case "fader": ex.web_smk_fader(m.f, m.v); break;
      case "bend": ex.web_smk_bend(m.v | 0); break;
      case "mod": ex.web_smk_mod(m.v); break;
      case "pedal": this.edge(fm1, "pedal", !!m.on, () => ex.web_smk_pedal(m.on ? 1 : 0)); break;
      case "midi": ex.web_midi_in(0, m.pkt >>> 0); break;      // Web MIDI in: the app's USB MIDI in
    }
  },
  process(fm1) {
    const ex = fm1.ex;
    if (this.later.size) {
      const now = ex.web_now() >>> 0;
      for (const [id, w] of this.later) if ((now - w.due) >> 0 >= 0) { this.later.delete(id); w.run(); }
    }
    if (currentTime - this.at < 1 / 30) return;
    this.at = currentTime;
    const buf = fm1.mem.buffer;
    const rgb = Array.from(new Uint32Array(buf, ex.web_smk_pad_rgb(), 16));
    const lvl = Array.from(new Uint8Array(buf, ex.web_smk_pad_lvl(), 16));
    const leds = ex.web_smk_leds() >>> 0, master = ex.web_smk_master();
    const key = leds + ":" + master + ":" + rgb.join(",") + ":" + lvl.join(",");
    if (key === this.last) return;
    this.last = key;
    fm1.port.postMessage({ type: "app", op: "lights", leds, rgb, lvl, master });
  },
};
