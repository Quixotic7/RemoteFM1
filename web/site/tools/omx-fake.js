// SPDX-License-Identifier: GPL-3.0-only
// A fake OMX-27 behind a mocked Web MIDI, injected before the page's scripts (tools/omx-e2e.mjs): one input and one
// output named "OMX-27" that speak REMOTE mode as the firmware does (omx_mode_remote.cpp), decoded here on their own
// (not with js/omx27.js). The MODE command (51 05 09) enters REMOTE mode and answers 52 02; 59 / 5A stage LEDs and
// 5B latches them; 5C stages a screen chunk and 5D latches the frame and ACKs it (52 04). Sysex reaches only a
// MIDIAccess asked with { sysex: true }, as in Chrome. As the owner's unit (omx-27-v3): MODE while already in REMOTE
// mode answers 52 03 then 52 02; pots stream a message every 2-3 ms while they move. window.__omx:
//   key(k, ev) / press(k) / release(k) / click(k)  enc(delta)  pot(p, v) / sweep(p, from, to)  leave() / pickRemote()  unplug() / plug()  note(bytes)
//   leds (latched, 27 x [r, g, b] 7-bit), frame (latched 512 bytes), remote, frames, acks, got (every message)
//   picture({ title, labels }) -> a PNG data URL of the OMX-27 with its LEDs and screen as latched
(() => {
  const ledStage = Array.from({ length: 27 }, () => [0, 0, 0]);
  const omx = window.__omx = {
    connected: true, leds: Array.from({ length: 27 }, () => [0, 0, 0]),
    frame: new Uint8Array(512), stage: new Uint8Array(512), frames: 0, acks: 0, got: [], pots: [0, 0, 0, 0, 0], ackDelay: 3, potMsgs: 0, modes: 0,
  };
  // REMOTE mode survives a page reload, as on the device
  let remote = false;
  try { remote = sessionStorage.getItem("__omxRemote") === "1"; } catch (_) { /* */ }
  Object.defineProperty(omx, "remote", { get: () => remote, set: (v) => { remote = !!v; try { sessionStorage.setItem("__omxRemote", remote ? "1" : "0"); } catch (_) { /* */ } } });
  const accesses = [];
  const port = (type) => ({ id: "omx27-" + type, name: "OMX-27", manufacturer: "Denki-Oto", type, version: "1",
    get state() { return omx.connected ? "connected" : "disconnected"; }, connection: "open" });
  // per MIDIAccess: its own input and output objects (as in Chrome), over the one device
  function makeAccess(sysex) {
    const lis = new Set();
    const input = Object.assign(port("input"), {
      onmidimessage: null, onstatechange: null,
      addEventListener(t, f) { if (t === "midimessage") lis.add(f); }, removeEventListener(t, f) { if (t === "midimessage") lis.delete(f); },
      open: async () => input, close: async () => input,
    });
    const output = Object.assign(port("output"), {
      onstatechange: null, addEventListener() {}, removeEventListener() {}, open: async () => output, close: async () => output,
      send(data) { if (!omx.connected) throw new Error("disconnected"); receive(Uint8Array.from(data)); }, clear() {},
    });
    const alis = new Set();
    const access = {
      sysexEnabled: sysex, onstatechange: null,
      get inputs() { return new Map(omx.connected ? [[input.id, input]] : []); },
      get outputs() { return new Map(omx.connected ? [[output.id, output]] : []); },
      addEventListener(t, f) { if (t === "statechange") alis.add(f); }, removeEventListener(t, f) { if (t === "statechange") alis.delete(f); },
      deliver(d) {
        if (!omx.connected || (d[0] === 0xF0 && !sysex)) return;
        const ev = { data: Uint8Array.from(d), timeStamp: performance.now(), target: input };
        if (input.onmidimessage) input.onmidimessage(ev);
        for (const f of lis) f(ev);
      },
      changed() { const ev = { port: input }; if (access.onstatechange) access.onstatechange(ev); for (const f of alis) f(ev); },
    };
    accesses.push(access);
    return access;
  }
  const out = (d, ms = 0) => setTimeout(() => { for (const a of accesses) a.deliver(d); }, ms);
  const sx = (...b) => [0xF0, 0x7D, 0, 0, ...b, 0xF7];
  // the host's messages, as omx_mode_remote.cpp takes them
  function receive(d) {
    omx.got.push(Array.from(d));
    if (d[0] !== 0xF0 || d[1] !== 0x7D || d[2] !== 0 || d[3] !== 0) return;
    const cmd = d[4], p = d.subarray(5, d.length - 1);
    if (cmd === 0x51 && p[0] === 0x05) {
      // (already in REMOTE mode: the real one leaves and re-enters, 52 03 then 52 02 within 3 ms)
      if (p[1] === 9) { const was = omx.remote; if (was) out(sx(0x52, 0x03)); omx.remote = true; out(sx(0x52, 0x02), was ? 3 : 0); omx.modes++; }
      else if (omx.remote) { omx.remote = false; out(sx(0x52, 0x03)); }
      return;
    }
    if (!omx.remote) return;
    if (cmd === 0x59 && p.length >= 4 && p[0] < 27) ledStage[p[0]] = [p[1], p[2], p[3]];
    else if (cmd === 0x5A && p.length >= 2) { for (let i = 0; i < p[1] && p[0] + i < 27; i++) ledStage[p[0] + i] = [p[2 + 3 * i], p[3 + 3 * i], p[4 + 3 * i]]; }
    else if (cmd === 0x5B) omx.leds = ledStage.map((c) => c.slice());
    else if (cmd === 0x5C && p.length >= 1 && p[0] < 16) {
      let s = 1;
      for (let i = 0; i < 32; i += 7) {
        const n = Math.min(7, 32 - i), hi = p[s++];
        for (let j = 0; j < n; j++) omx.stage[p[0] * 32 + i + j] = (p[s++] & 0x7F) | (((hi >> j) & 1) << 7);
      }
    } else if (cmd === 0x5D) {
      omx.frame = omx.stage.slice(); omx.frames++;
      setTimeout(() => { omx.acks++; out(sx(0x52, 0x04)); }, omx.ackDelay);
    }
  }
  // the OMX's own events
  omx.key = (k, ev) => out(sx(0x51, 0x00, k, ev));
  omx.press = (k) => omx.key(k, 1);
  omx.release = (k) => { omx.key(k, 0); omx.key(k, 3); };
  omx.click = (k) => { omx.press(k); omx.release(k); };
  omx.enc = (delta) => out(sx(0x51, 0x01, delta < 0 ? 0 : 2, Math.min(127, Math.abs(delta))));
  omx.pot = (p, v) => { omx.pots[p] = v; const hi = v << 7; out(sx(0x51, 0x03, p, v, (hi >> 7) & 0x7F, hi & 0x7F)); omx.potMsgs++; };
  // a pot turned as the real one streams it: a message every 2-3 ms, hi-res steps of 32 (v7 every ~4); resolves when done
  omx.sweep = (p, from, to) => new Promise((done) => {
    let hi = from << 7;
    const end = to << 7, step = end > hi ? 32 : -32;
    const next = () => {
      hi = step > 0 ? Math.min(end, hi + step) : Math.max(end, hi + step);
      const v = hi >> 7; omx.pots[p] = v; omx.potMsgs++;
      out(sx(0x51, 0x03, p, v, (hi >> 7) & 0x7F, hi & 0x7F));
      if (hi === end) setTimeout(done, 20); else setTimeout(next, 2 + (omx.potMsgs & 1));
    };
    next();
  });
  omx.leave = () => { omx.remote = false; out(sx(0x52, 0x03)); };
  omx.pickRemote = () => { omx.remote = true; out(sx(0x52, 0x02)); };   // REMOTE picked on the device's mode select
  omx.unplug = () => { omx.connected = false; omx.remote = false; for (const a of accesses) a.changed(); };
  omx.plug = () => { omx.connected = true; for (const a of accesses) a.changed(); };
  omx.note = (b) => out(b);   // a plain message (an OMX outside REMOTE mode playing notes)
  omx.screenBits = () => {   // the panel's pixels (the buffer is 180 deg)
    const bits = new Uint8Array(128 * 32);
    for (let y = 0; y < 32; y++) for (let x = 0; x < 128; x++) if (omx.frame[x + (y >> 3) * 128] & (1 << (y & 7))) bits[(31 - y) * 128 + (127 - x)] = 1;
    return bits;
  };

  // the picture: the OLED and the pots on top, AUX and the 10 black keys over the 16 white ones (their piano places)
  omx.picture = ({ title = "", labels = [] } = {}) => {
    const KW = 60, X0 = 30, cv = document.createElement("canvas"), Wd = X0 * 2 + 16 * KW, Hd = 470;
    cv.width = Wd; cv.height = Hd;
    const g = cv.getContext("2d");
    g.fillStyle = "#16181b"; g.fillRect(0, 0, Wd, Hd);
    g.fillStyle = "#24272c"; g.beginPath(); g.roundRect(12, 40, Wd - 24, Hd - 52, 18); g.fill();
    g.fillStyle = "#e8eaed"; g.font = "700 18px 'DM Sans', sans-serif"; g.fillText(title, 16, 28);
    // the OLED, x3
    g.fillStyle = "#000"; g.fillRect(X0, 60, 128 * 3 + 12, 32 * 3 + 12);
    const bits = omx.screenBits();
    g.fillStyle = "#cfe8ff";
    for (let y = 0; y < 32; y++) for (let x = 0; x < 128; x++) if (bits[y * 128 + x]) g.fillRect(X0 + 6 + x * 3, 66 + y * 3, 3, 3);
    // the encoder and the pots
    const knob = (cx, cy, r, label, v) => {
      g.fillStyle = "#3a3e45"; g.beginPath(); g.arc(cx, cy, r, 0, 7); g.fill();
      g.strokeStyle = "#111"; g.lineWidth = 2; g.stroke();
      if (v !== null) {
        const a = (v / 127 - 0.5) * 1.5 * Math.PI - Math.PI / 2;
        g.strokeStyle = "#e8eaed"; g.lineWidth = 3; g.beginPath(); g.moveTo(cx, cy); g.lineTo(cx + Math.cos(a) * r * 0.8, cy + Math.sin(a) * r * 0.8); g.stroke();
      }
      g.fillStyle = "#9aa0a8"; g.font = "600 12px 'DM Sans', sans-serif"; g.textAlign = "center"; g.fillText(label, cx, cy + r + 15); g.textAlign = "left";
    };
    knob(X0 + 128 * 3 + 60, 112, 26, "ENC", null);
    for (let i = 0; i < 5; i++) knob(X0 + 128 * 3 + 150 + i * 92, 112, 30, "K" + (i + 1), omx.pots[i]);
    // keys
    const cap = (k, x, y, w, hgt) => {
      const c = omx.leds[k], rgb = c.map((v) => Math.min(255, v * 2)), lit = rgb.some((v) => v > 0);
      g.save();
      if (lit) { g.shadowColor = `rgb(${rgb})`; g.shadowBlur = 22; }
      g.fillStyle = lit ? `rgb(${rgb})` : "#33373d";
      g.beginPath(); g.roundRect(x, y, w, hgt, 8); g.fill();
      g.restore();
      g.strokeStyle = "rgba(0,0,0,.6)"; g.lineWidth = 2; g.beginPath(); g.roundRect(x, y, w, hgt, 8); g.stroke();
      const lum = 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2];
      g.fillStyle = lum > 140 ? "#111" : "#e8eaed"; g.textAlign = "center";
      g.font = "700 11px 'DM Sans', sans-serif"; g.fillText(labels[k] || "", x + w / 2, y + hgt / 2 + 4);
      g.fillStyle = lum > 140 ? "rgba(0,0,0,.55)" : "rgba(255,255,255,.4)"; g.font = "600 9px 'DM Sans', sans-serif"; g.fillText(String(k), x + w / 2, y + hgt - 6);
      g.textAlign = "left";
    };
    const WY = 330, BY = 230, S = 52;
    for (let i = 0; i < 16; i++) cap(11 + i, X0 + i * KW + 4, WY, S, S + 40);
    const BLACK_AFTER = [1, 2, 4, 5, 6, 8, 9, 11, 12, 13];   // the white key each black one follows (B3 = 0)
    BLACK_AFTER.forEach((wi, j) => cap(1 + j, X0 + (wi + 1) * KW - S / 2 + 2, BY, S, S + 30));
    cap(0, X0 + 4, BY, S, S + 30);
    return cv.toDataURL("image/png");
  };

  // Web MIDI, mocked; the 'midi' permission reads granted (as after a first allow)
  Object.defineProperty(navigator, "requestMIDIAccess", { value: async (o) => makeAccess(!!(o && o.sysex)), configurable: true });
  const real = navigator.permissions;
  Object.defineProperty(navigator, "permissions", { value: { query: async (d) => (d && d.name === "midi" ? { state: "granted", onchange: null, addEventListener() {} } : real.query(d)) }, configurable: true });
})();
