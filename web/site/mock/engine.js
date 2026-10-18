// SPDX-License-Identifier: GPL-3.0-only
// A mock engine (web/engine/API.md): the page's stand-in until the native (W1) and emulated (W2) engines exist, and
// the engine the page falls back to when a real one can't load. It draws a test screen, plays a sine-ish FM tone per
// held key or MIDI note, lights the LEDs of what is held, and keeps a tiny "flash" (a counter SAVE bumps) so saves
// can be tried. Runs in an AudioWorkletGlobalScope and in Node (no DOM, no fetch).

export const ENGINE = { name: "mock", version: "0.1" };

const W = 240, NKEY = 27;
const KEYMAP = [
  [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
  [5, 11, 4, 10, 3, 9, 2, 8, -1, -1, -1],
  [34, 35, 36, 37, 38, 40, 39, 13, 7, 6, 12],
  [23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33],
  [0, 1, 15, 14, 17, 16, 19, 18, 20, 21, 22],
  [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
];
const LED_OF = new Int16Array(41).fill(-1);
for (let r = 1; r <= 4; r++) for (let c = 0; c < 11; c++) if (KEYMAP[r][c] >= 0) LED_OF[KEYMAP[r][c]] = c * 4 + r - 1;

// 5x7 font (emu.c's), ASCII 32..95, columns, bit 0 = top
const GLYPH = [
  [0x00,0x00,0x00,0x00,0x00],[0x00,0x00,0x5F,0x00,0x00],[0x00,0x07,0x00,0x07,0x00],[0x14,0x7F,0x14,0x7F,0x14],
  [0x24,0x2A,0x7F,0x2A,0x12],[0x23,0x13,0x08,0x64,0x62],[0x36,0x49,0x55,0x22,0x50],[0x00,0x05,0x03,0x00,0x00],
  [0x00,0x1C,0x22,0x41,0x00],[0x00,0x41,0x22,0x1C,0x00],[0x08,0x2A,0x1C,0x2A,0x08],[0x08,0x08,0x3E,0x08,0x08],
  [0x00,0x50,0x30,0x00,0x00],[0x08,0x08,0x08,0x08,0x08],[0x00,0x60,0x60,0x00,0x00],[0x20,0x10,0x08,0x04,0x02],
  [0x3E,0x51,0x49,0x45,0x3E],[0x00,0x42,0x7F,0x40,0x00],[0x42,0x61,0x51,0x49,0x46],[0x21,0x41,0x45,0x4B,0x31],
  [0x18,0x14,0x12,0x7F,0x10],[0x27,0x45,0x45,0x45,0x39],[0x3C,0x4A,0x49,0x49,0x30],[0x01,0x71,0x09,0x05,0x03],
  [0x36,0x49,0x49,0x49,0x36],[0x06,0x49,0x49,0x29,0x1E],[0x00,0x36,0x36,0x00,0x00],[0x00,0x56,0x36,0x00,0x00],
  [0x08,0x14,0x22,0x41,0x00],[0x14,0x14,0x14,0x14,0x14],[0x00,0x41,0x22,0x14,0x08],[0x02,0x01,0x51,0x09,0x06],
  [0x32,0x49,0x79,0x41,0x3E],[0x7E,0x11,0x11,0x11,0x7E],[0x7F,0x49,0x49,0x49,0x36],[0x3E,0x41,0x41,0x41,0x22],
  [0x7F,0x41,0x41,0x22,0x1C],[0x7F,0x49,0x49,0x49,0x41],[0x7F,0x09,0x09,0x01,0x01],[0x3E,0x41,0x41,0x51,0x32],
  [0x7F,0x08,0x08,0x08,0x7F],[0x00,0x41,0x7F,0x41,0x00],[0x20,0x40,0x41,0x3F,0x01],[0x7F,0x08,0x14,0x22,0x41],
  [0x7F,0x40,0x40,0x40,0x40],[0x7F,0x02,0x04,0x02,0x7F],[0x7F,0x04,0x08,0x10,0x7F],[0x3E,0x41,0x41,0x41,0x3E],
  [0x7F,0x09,0x09,0x09,0x06],[0x3E,0x41,0x51,0x21,0x5E],[0x7F,0x09,0x19,0x29,0x46],[0x46,0x49,0x49,0x49,0x31],
  [0x01,0x01,0x7F,0x01,0x01],[0x3F,0x40,0x40,0x40,0x3F],[0x1F,0x20,0x40,0x20,0x1F],[0x7F,0x20,0x18,0x20,0x7F],
  [0x63,0x14,0x08,0x14,0x63],[0x03,0x04,0x78,0x04,0x03],[0x61,0x51,0x49,0x45,0x43],[0x00,0x7F,0x41,0x41,0x00],
  [0x02,0x04,0x08,0x10,0x20],[0x00,0x41,0x41,0x7F,0x00],[0x04,0x02,0x01,0x02,0x04],[0x40,0x40,0x40,0x40,0x40],
];

const rgb = (r, g, b) => ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
const BARS = [rgb(232, 56, 40), rgb(247, 125, 26), rgb(246, 243, 238), rgb(84, 214, 120), rgb(80, 150, 255), rgb(170, 110, 230)];

export async function createEngine({ sampleRate = 48000, files = {}, flash = null, options = {} } = {}) {
  const px = new Uint16Array(W * W);
  const levels = new Uint8Array(44);
  let seq = 1, ledSeq = 1, flashSeq = 0, t = 0, frameAt = 0, playGreen = 0, halted = 0;
  let keys = 0, buttons = 0, master = 768, knob = new Int32Array(7), playing = false;
  const vel = new Uint8Array(NKEY);
  const midiNotes = new Map();               // note -> velocity
  const voices = new Map();                  // note -> { ph, mph, env, rel }
  let out = [];
  const fl = new Uint8Array(16);
  if (flash && flash.length >= 4) fl.set(flash.subarray(0, 16));
  else fl.set([0x4D, 0x4F, 0x43, 0x4B]);      // "MOCK", then a SAVE counter at [4]
  const name = (options && options.name) || "Mock engine";

  function rect(x, y, w, h, c) {
    for (let j = Math.max(0, y); j < Math.min(W, y + h); j++) px.fill(c, j * W + Math.max(0, x), j * W + Math.min(W, x + w));
  }
  function textAt(x, y, s, c, sc = 2) {
    s = String(s).toUpperCase();
    for (let i = 0; i < s.length; i++) {
      const g = GLYPH[s.charCodeAt(i) - 32];
      if (!g) continue;
      for (let col = 0; col < 5; col++) for (let row = 0; row < 7; row++) if ((g[col] >> row) & 1) rect(x + (i * 6 + col) * sc, y + row * sc, sc, sc, c);
    }
  }
  const centre = (y, s, c, sc = 2) => textAt(Math.round((W - (s.length * 6 - 1) * sc) / 2), y, s, c, sc);

  function drawScreen() {
    rect(0, 0, W, W, rgb(8, 8, 10));
    centre(28, name.slice(0, 18), rgb(246, 243, 238), 2);
    centre(50, "TEST SCREEN", rgb(138, 138, 146), 1);
    const held = [];
    for (let k = 0; k < NKEY; k++) if ((keys >>> k) & 1) held.push(k);
    for (const n of midiNotes.keys()) held.push(n - 53);
    for (let i = 0; i < 6; i++) {
      const w = 40 + ((Math.sin(t * 1.3 + i) + 1) * 0.5 * 160) | 0;
      rect(W - 12 - w, 76 + i * 18, w, 12, BARS[i]);
    }
    for (let k = 0; k < NKEY; k++) rect(12 + k * 8, 196, 6, 14, held.includes(k) ? rgb(84, 214, 120) : rgb(40, 40, 46));
    centre(216, `MASTER ${master}  SAVES ${fl[4]}`, rgb(138, 138, 146), 1);
    centre(228, playing ? "PLAYING" : "PRESS A KEY", playing ? rgb(255, 165, 42) : rgb(90, 90, 98), 1);
    seq++;
  }
  function updateLeds() {
    levels.fill(0);
    for (let k = 0; k < NKEY; k++) if ((keys >>> k) & 1) levels[LED_OF[14 + k]] = 255;
    for (let b = 0; b < 14; b++) if ((buttons >>> b) & 1) levels[LED_OF[b]] = 255;
    levels[LED_OF[2]] = Math.max(levels[LED_OF[2]], 12);   // FX faint, as a firmware's backlight
    if (playing) levels[LED_OF[12]] = 255;
    playGreen = playing ? 255 : 0;
    ledSeq++;
  }
  drawScreen();
  updateLeds();

  const noteHz = (n) => 440 * Math.pow(2, (n - 69) / 12);
  const blockTime = 128 / sampleRate;

  return {
    info: () => ({ id: "mock", name, version: ENGINE.version, license: "GPL-3.0-only", source: "web/site/mock/engine.js",
                   credits: "A stand-in engine of the RemoteFM1 site.", family: "mock", latencyFrames: 0, keymap: null,
                   buttonNames: null }),
    setPanel(p) {
      const prevB = buttons;
      keys = p.keys >>> 0; buttons = p.buttons >>> 0; master = p.master | 0;
      if (p.keyVel) vel.set(p.keyVel.subarray(0, NKEY));
      if (p.taps) { keys |= p.taps.keys >>> 0; buttons |= p.taps.buttons >>> 0; }
      for (let i = 0; i < 7; i++) knob[i] += p.enc ? p.enc[i] | 0 : 0;
      const down = buttons & ~prevB;
      if (down & (1 << 12)) playing = !playing;                    // PLAY
      if (down & (1 << 9)) { fl[4] = (fl[4] + 1) & 0xFF; flashSeq++; }   // SAVE
      updateLeds();
      drawScreen();
    },
    render(left, right) {
      if (halted) { left.fill(0); right.fill(0); return; }
      const gain = (master / 1023) * 0.18;
      const want = new Map();
      for (let k = 0; k < NKEY; k++) if ((keys >>> k) & 1) want.set(53 + k, vel[k] || 100);
      for (const [n, v] of midiNotes) want.set(n, v);
      if (playing) want.set(41 + ((t * 4 | 0) % 4) * 5, 90);
      for (const [n, v] of want) if (!voices.has(n) || voices.get(n).rel) voices.set(n, { ph: 0, mph: 0, env: voices.get(n)?.env || 0, rel: false, v: v / 127 });
      for (const [n, vc] of voices) if (!want.has(n)) vc.rel = true;
      left.fill(0); right.fill(0);
      for (const [n, vc] of voices) {
        const f = noteHz(n), inc = 2 * Math.PI * f / sampleRate;
        for (let i = 0; i < left.length; i++) {
          vc.env = vc.rel ? vc.env * 0.9993 : vc.env + (1 - vc.env) * 0.004;
          vc.mph += inc * 2; vc.ph += inc;
          const s = Math.sin(vc.ph + 1.2 * vc.env * Math.sin(vc.mph)) * vc.env * vc.v * gain;
          left[i] += s; right[i] += s;
        }
        if (vc.rel && vc.env < 1e-4) voices.delete(n);
      }
      t += blockTime;
      if (t - frameAt > 1 / 20) { frameAt = t; drawScreen(); }
    },
    screen: () => ({ seq, pixels: px }),
    leds: () => ({ seq: ledSeq, levels, playGreen }),
    midiIn(bytes) {
      for (let i = 0; i + 2 < bytes.length; i++) {
        const s = bytes[i] & 0xF0;
        if (s === 0x90 && bytes[i + 2] > 0) { midiNotes.set(bytes[i + 1], bytes[i + 2]); out.push(bytes[i], bytes[i + 1], bytes[i + 2]); i += 2; }
        else if (s === 0x80 || s === 0x90) { midiNotes.delete(bytes[i + 1]); i += 2; }
      }
      drawScreen();
    },
    midiOut() { const o = Uint8Array.from(out); out = []; return o; },
    flash: () => ({ seq: flashSeq, bytes: fl.slice() }),
    halted: () => halted,
    dispose() { voices.clear(); },
  };
}
