// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 RemoteFM1 contributors
// The SMK-37 emulator's part of fm1-base's worklet (loaded before worklet.js): the page's { type: "app" } messages
// set the SMK-37's controls (the wasm's web_smk_* exports), and about 30 times a second its lights go back to the
// page as { type: "app", op: "lights" } when they changed.

globalThis.fm1App = {
  load(fm1) {
    this.last = "";
    this.at = 0;
  },
  message(fm1, m) {
    const ex = fm1.ex;
    switch (m.op) {
      case "key": ex.web_smk_key(m.k, m.vel); break;
      case "pad": ex.web_smk_pad(m.p, m.vel); break;
      case "btns": ex.web_smk_buttons(m.mask >>> 0); break;
      case "enc": ex.web_smk_enc(m.e, m.n | 0); break;
      case "fader": ex.web_smk_fader(m.f, m.v); break;
      case "bend": ex.web_smk_bend(m.v | 0); break;
      case "mod": ex.web_smk_mod(m.v); break;
      case "pedal": ex.web_smk_pedal(m.on ? 1 : 0); break;
      case "midi": ex.web_midi_in(0, m.pkt >>> 0); break;      // Web MIDI in: the app's USB MIDI in
    }
  },
  process(fm1) {
    if (currentTime - this.at < 1 / 30) return;
    this.at = currentTime;
    const ex = fm1.ex, buf = fm1.mem.buffer;
    const rgb = Array.from(new Uint32Array(buf, ex.web_smk_pad_rgb(), 16));
    const lvl = Array.from(new Uint8Array(buf, ex.web_smk_pad_lvl(), 16));
    const leds = ex.web_smk_leds() >>> 0, master = ex.web_smk_master();
    const key = leds + ":" + master + ":" + rgb.join(",") + ":" + lvl.join(",");
    if (key === this.last) return;
    this.last = key;
    fm1.port.postMessage({ type: "app", op: "lights", leds, rgb, lvl, master });
  },
};
