// SPDX-License-Identifier: MPL-2.0
// The OMX-27's REMOTE mode, the wire (OMX-27 firmware q7-2026-3: SYSEX_SPEC.md "REMOTE mode", omx_mode_remote.cpp,
// tools/remote_test.py, norns/omx_remote/lib/omx27.lua). No DOM: web/site/test.mjs runs it.
//   every message    F0 7D 00 00 <cmd> <payload> F7
//   host -> OMX      51 05 <MODE_REMOTE> enter REMOTE mode (the input-injection MODE command)
//                    59 idx r g b / 5A start count rgb... stage LEDs (7-bit), 5B latch them
//                    5C chunk enc7(32 bytes) stage a screen chunk (SSD1306 pages, 180 deg), 5D latch the frame
//   OMX -> host      51 00 key ev (0 up 1 down 2 hold 3 quick click, after the up), 51 01 dir amt (0 CCW 2 CW),
//                    51 02 z (encoder button), 51 03 pot v7 hi lo; 52 02 entered, 52 03 left, 52 04 frame ACK
// OmxSession keeps the OMX's LEDs and screen as the page wants them: only what changed, LEDs at most every 30 ms,
// one frame in flight (its ACK, or 250 ms), at most 15 frames a second.

export const CMD = { INPUT: 0x51, STATUS: 0x52, LED: 0x59, LED_BATCH: 0x5A, LED_SHOW: 0x5B, DRAW: 0x5C, DRAW_UPD: 0x5D };
export const STATUS = { ACTIVITY: 0x01, ON: 0x02, OFF: 0x03, ACK: 0x04 };
export const MODE_REMOTE = 9;   // config.h OMXMode: MIDI DRUM CHORDS FORM S1 S2 GRIDS EUCLID OM REMOTE CONFIG
export const EV = { UP: 0, DOWN: 1, HOLD: 2, QUICK: 3 };
export const NLED = 27, W = 128, H = 32, FB_BYTES = 512, CHUNK = 32, NCHUNK = 16;

// the port: its name says OMX
export const isOmxPortName = (name) => /omx/i.test(String(name || ""));
// an output and the input of the same name (else the first OMX input)
export function omxPair(access) {
  const up = (p) => !p.state || p.state === "connected";
  const outs = [...access.outputs.values()].filter((p) => up(p) && isOmxPortName(p.name));
  const ins = [...access.inputs.values()].filter((p) => up(p) && isOmxPortName(p.name));
  for (const o of outs) { const i = ins.find((p) => p.name === o.name) || ins[0]; if (i) return { input: i, output: o, name: o.name }; }
  return null;
}

export const sysex = (cmd, ...data) => Uint8Array.from([0xF0, 0x7D, 0x00, 0x00, cmd, ...data.flat(Infinity).map((b) => b & 0x7F), 0xF7]);
export const enterRemote = () => sysex(CMD.INPUT, 0x05, MODE_REMOTE);

// 8-bit bytes -> 7-bit groups: a byte of high bits, then up to 7 low parts (remote_test.py _enc7)
export function enc7(data) {
  const out = [];
  for (let i = 0; i < data.length; i += 7) {
    const n = Math.min(7, data.length - i);
    let hi = 0;
    for (let j = 0; j < n; j++) hi |= ((data[i + j] >> 7) & 1) << j;
    out.push(hi);
    for (let j = 0; j < n; j++) out.push(data[i + j] & 0x7F);
  }
  return out;
}
export function dec7(src, len) {
  const out = new Uint8Array(len);
  let s = 0;
  for (let i = 0; i < len; i += 7) {
    const n = Math.min(7, len - i), hi = src[s++] | 0;
    for (let j = 0; j < n; j++) out[i + j] = (src[s++] & 0x7F) | (((hi >> j) & 1) << 7);
  }
  return out;
}

// a 128 x 32 image (bits[y * 128 + x], 1 lit) -> the SSD1306 buffer, 180 deg (remote_test.py _pack)
export function pack(bits) {
  const fb = new Uint8Array(FB_BYTES);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    if (!bits[(H - 1 - y) * W + (W - 1 - x)]) continue;
    fb[x + (y >> 3) * W] |= 1 << (y & 7);
  }
  return fb;
}
export function unpack(fb) {
  const bits = new Uint8Array(W * H);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (fb[x + (y >> 3) * W] & (1 << (y & 7))) bits[(H - 1 - y) * W + (W - 1 - x)] = 1;
  return bits;
}

// a message from the OMX -> { type: "key", key, ev } | { type: "enc", delta } | { type: "encBtn", z }
//   | { type: "pot", pot, v, hires } | { type: "status", s }; anything else null
export function parse(d) {
  if (!d || d.length < 7 || d[0] !== 0xF0 || d[1] !== 0x7D || d[2] !== 0 || d[3] !== 0 || d[d.length - 1] !== 0xF7) return null;
  const cmd = d[4], a = d.subarray ? d.subarray(5, d.length - 1) : d.slice(5, -1);
  if (cmd === CMD.STATUS) return { type: "status", s: a[0] };
  if (cmd !== CMD.INPUT) return null;
  const sub = a[0];
  if (sub === 0x00 && a.length >= 3) return a[1] < NLED ? { type: "key", key: a[1], ev: a[2] } : null;
  if (sub === 0x01 && a.length >= 3) { const amt = Math.max(1, a[2]); return { type: "enc", delta: a[1] === 2 ? amt : -amt }; }
  if (sub === 0x02 && a.length >= 2) return { type: "encBtn", z: a[1] ? 1 : 0 };
  if (sub === 0x03 && a.length >= 5) return a[1] < 5 ? { type: "pot", pot: a[1], v: a[2], hires: (a[3] << 7) | a[4] } : null;
  return null;
}
// the OMX's side (the fake OMX-27 in the tests speaks it)
export const keyMsg = (k, ev) => sysex(CMD.INPUT, 0x00, k, ev);
export const encMsg = (delta) => sysex(CMD.INPUT, 0x01, delta < 0 ? 0 : 2, Math.min(127, Math.abs(delta)));
export const potMsg = (p, v, hires = v << 7) => sysex(CMD.INPUT, 0x03, p, v, (hires >> 7) & 0x7F, hires & 0x7F);
export const statusMsg = (s) => sysex(CMD.STATUS, s);

// the LEDs as they should be (27 x [r, g, b], 0..127) against what the OMX has (null: unknown) -> the messages
export function ledMessages(want, sent) {
  const eq = (a, b) => !!a && !!b && a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
  const changed = [];
  for (let i = 0; i < NLED; i++) if (!sent || !eq(want[i], sent[i])) changed.push(i);
  if (!changed.length) return [];
  const out = changed.length > 8 ? [sysex(CMD.LED_BATCH, 0, NLED, want.map((c) => c.slice(0, 3)))] : changed.map((i) => sysex(CMD.LED, i, want[i].slice(0, 3)));
  out.push(sysex(CMD.LED_SHOW));
  return out;
}
// a frame (512 bytes) against the last one sent (null: none) -> its changed chunks and the latch, or [] when the same
export function frameMessages(fb, last) {
  const out = [];
  for (let c = 0; c < NCHUNK; c++) {
    const a = fb.subarray(c * CHUNK, (c + 1) * CHUNK);
    if (last && a.every((b, i) => b === last[c * CHUNK + i])) continue;
    out.push(sysex(CMD.DRAW, c, enc7(a)));
  }
  if (out.length) out.push(sysex(CMD.DRAW_UPD));
  return out;
}

export const LED_MS = 30, FRAME_MS = 1000 / 15, ACK_MS = 250, CHUNK_GAP_MS = 2;
const dark = () => Array.from({ length: NLED }, () => [0, 0, 0]);

export class OmxSession {
  // send(bytes, delayMs): to the OMX's output (delayMs: when, from now; the screen's chunks are paced)
  constructor({ send, now = () => Date.now() }) {
    this.send = send; this.now = now;
    this.active = false;                 // in REMOTE mode (52 02 seen, no 52 03 since)
    this.wantLeds = dark(); this.sentLeds = null; this.ledsAt = -1e9;
    this.wantFrame = new Uint8Array(FB_BYTES); this.lastFrame = null; this.frameAt = -1e9; this.pending = false;
    this.stats = { frames: 0, acks: 0, chunks: 0, ledMsgs: 0 };
  }
  // the MODE command; what the OMX shows is unknown until it says it is in REMOTE mode
  enter() { this.send(enterRemote(), 0); }
  // a message from the OMX: its event (null: none); the status ones change the session
  feed(d) {
    const m = parse(d);
    if (!m) return null;
    if (m.type === "status") {
      if (m.s === STATUS.ACK) { this.pending = false; this.stats.acks++; }
      else if (m.s === STATUS.ON) { this.active = true; this.reset(); }
      else if (m.s === STATUS.OFF) { this.active = false; this.pending = false; }
    }
    return m;
  }
  reset() { this.sentLeds = null; this.lastFrame = null; this.pending = false; this.ledsAt = this.frameAt = -1e9; }
  setLeds(colors) { this.wantLeds = colors.map((c) => [c[0] & 127, c[1] & 127, c[2] & 127]); }
  setFrame(fb) { this.wantFrame = fb; }
  // send what is due; true when something went
  tick() {
    if (!this.active) return false;
    const t = this.now();
    let sent = false;
    if (t - this.ledsAt >= LED_MS) {
      const ms = ledMessages(this.wantLeds, this.sentLeds);
      if (ms.length) { for (const m of ms) this.send(m, 0); this.sentLeds = this.wantLeds.map((c) => c.slice()); this.ledsAt = t; this.stats.ledMsgs += ms.length; sent = true; }
    }
    if (this.pending && t - this.frameAt >= ACK_MS) this.pending = false;   // (a lost ACK: no wedge)
    if (!this.pending && t - this.frameAt >= FRAME_MS) {
      const ms = frameMessages(this.wantFrame, this.lastFrame);
      if (ms.length) {
        ms.forEach((m, i) => this.send(m, i * CHUNK_GAP_MS));
        this.lastFrame = this.wantFrame.slice(); this.frameAt = t; this.pending = true;
        this.stats.frames++; this.stats.chunks += ms.length - 1; sent = true;
      }
    }
    return sent;
  }
  // every LED off and a blank screen, now (Disconnect, the page closing); the OMX stays in REMOTE mode
  clear() {
    for (const m of ledMessages(dark(), null)) this.send(m, 0);
    frameMessages(new Uint8Array(FB_BYTES), null).forEach((m, i) => this.send(m, i * CHUNK_GAP_MS));
    this.wantLeds = dark(); this.wantFrame = new Uint8Array(FB_BYTES); this.reset();
  }
}
