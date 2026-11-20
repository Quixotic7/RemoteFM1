// SPDX-License-Identifier: MPL-2.0
// USE MY ABLETON MOVE: an Ableton Move running Move Remote (github.com/Quixotic7/abletonmoveremote) as the FM-1's
// controller. Its pads, steps, buttons and knobs drive the page's panel (source SRC.MOVE) or the firmware's MIDI in,
// and the page lights its pads and buttons from the FM-1's LEDs and draws its 128 x 64 screen. The mapping is the
// portable C core rfm_move (rfm1cs-abletonmove, web/vendor/rfm1cs-abletonmove: its README.md), in WebAssembly (js/move-core.js); this file is the protocol
// (Move Remote's docs/PROTOCOL.md: u16 len | u8 type | payload) and the page's side, modelled on js/omx-link.js.
//
// The link is a transport: { name, connect() -> Promise, send(Uint8Array) -> bool, onmessage(Uint8Array),
// onclose(why), close() }. Two: MoveWebSocket (ws://<move>:7720/ws, move.local over Wi-Fi or 172.16.254.1 over
// USB-C) and MoveBridge. From an https page (remotefm1.com) the ws:// is tried first, in the click: Chrome lets it
// through to a .local name or a private address (Local Network Access; it may ask once to allow local network devices,
// and the socket waits until that is answered). Browsers that refuse it throw SecurityError there, and the click opens
// Move Remote's bridge window instead (http://<move>/bridge, Move Remote 0.1.1+): a plain-http popup that opens the
// ws:// itself and relays it over postMessage (Move Remote's docs/PROTOCOL.md, "The bridge"). A direct try that fails
// or stays unanswered offers the bridge on a button (a popup needs a click). ws:// from http and localhost pages, and
// a wss:// address, go straight to the WebSocket as before.
//
// Session (rfm1cs-abletonmove's session.h, in the core's WebAssembly module: the same C the native hosts run): HELLO
// (proto 1) -> the Move's HELLO; CONFIG audio clock off (no AUDIO messages: the page plays its own sound); INPUT
// records -> the core -> actions; the core's changed LEDs out after each input and each LED change of the FM-1
// (LED_RGB / LED); the screen after a frame, an input or a label change, at most ~30 a second, and only when it
// changed (DISPLAY). PING every second; nothing heard for 3 s (STATUS comes every 500 ms) and the link is lost.
// Lost: everything the Move held is let go, and it connects again (1, 2, 4 s ...) while wanted. BYE: the exit
// gesture or another host taking the Move ends it; a power off tries again. This file moves the session's bytes
// over the transport, keeps its timer, and shows its state; the framing helpers below (frame, Deframer, helloMsg,
// parseHello, ledMsgs ..) are the protocol in JS for the tests' fake Move.
//
// The sequencer (Track 4; rfm1cs-abletonmove's README.md): js/move-seq.js's SeqClock advances the core by the audio clock
// (site.audioNow()) every SEQ_MS and before each input, its MIDI timed into the worklet (site.midiIn(bytes, at)); the
// firmware's MIDI out ("midiOut", with its frame) brings its MIDI clock in. When the sequencer stops, and on a
// disconnect, a lost link or the page going, the timed queue is cut (site.midiCut()) and every note let go. Projects:
// js/move-projects.js's store (IndexedDB) under the core's store (store.h: PROJECT_SAVE / LOAD / DELETE / CUT / COPY /
// PASTE / COLOR as slot reads, writes and deletes); at start the store's slots (projectDir) and the last project
// (prefs.moveProject) go to the core; the autosave flushed on disconnect and pagehide.
//
// Firmwares (rfm1cs-abletonmove 0.5.10, "One firmware, no FX"): the core's firmware browser on the projects view's
// Track 1 with one slot and no FX (fwCaps(0)), fed the page's own firmware list (opts.firmwares: the settings sheet's
// native tiles, A-Z) with 64 x 64 1-bit thumbnails (the catalog images, squeezed and Atkinson-dithered by the page,
// sent again when they arrive). Everything listed is on this site, so every entry is "installed" at the version the
// page would load: no download is ever offered (an FW_DOWNLOAD that comes anyway is answered as done). FW_LOAD loads
// it as the page's own picker does (progress 100 or FAILED, BUSY while another load runs), then fwSlots; the page
// always plays a firmware, so FW_UNLOAD keeps the current one (the slot shown again, a toast says so); FW_LEVEL is
// the page's volume (100 = full, the core's default); the library blob (favourites, colours, tags) is kept in
// prefs.moveLibrary (base64) and given back at start. A firmware picked on the page goes to the Move (fwSlots).
import { h } from "./sheet.js";
import { SRC, E, NB, labelToMatrix, roleToMatrix } from "./panel.js";
import { lean, MODE_NAMES, STATE, WHY, CH, STORE_PENDING, FW_FAILED, FW_BUSY } from "./move-core.js";
import { SeqClock } from "./move-seq.js";
import { MoveProjects, unpackFile, packFile, projectName, fileName, SET_NAME, firstFree, validSlot, dirBits, SLOTS, EXT_ONE, EXT_SET, MAX_FILE } from "./move-projects.js";

// ---- the protocol (include/moveremote/proto.h)
export const MR = {
  PROTO: 1, PORT: 7720, USB_ADDR: "172.16.254.1",
  D_HELLO: 0x01, D_INPUT: 0x02, D_AUDIO: 0x03, D_STATUS: 0x04, D_PONG: 0x05, D_BYE: 0x06,
  H_HELLO: 0x81, H_LED: 0x82, H_LED_RGB: 0x83, H_DISPLAY: 0x84, H_MIDI: 0x86, H_CONFIG: 0x87, H_PING: 0x88, H_QUIT: 0x89,
  CFG_AUDIO_CLOCK: 1, CAP_SIMULATOR: 0x8000, VIA: ["Wi-Fi", "USB", "this computer"],
};
export const BYE = { EXIT: 1, REPLACED: 2, SHUTDOWN: 3, QUIT: 4, PROTOCOL: 5 };
export const DEFAULT_URL = "ws://move.local:7720/ws";
export const HOST_NAME = "RemoteFM1 web";
export const SEQ_MS = 10;
export const SCREEN_MS = 33, PING_MS = 1000, SILENT_MS = 3000, HELLO_MS = 4000, RETRY_MS = [1000, 2000, 4000];
// the bridge window (Move Remote 0.1.1+): its hello within BRIDGE_HELLO_MS, its .closed polled every BRIDGE_POLL_MS
export const BRIDGE_HELLO_MS = 5000, BRIDGE_POLL_MS = 500, BRIDGE_NAME = "moveremote-bridge", BRIDGE_FEATURES = "popup,width=360,height=240";

// one message: u16 len (type + payload) | u8 type | payload
export function frame(type, payload = new Uint8Array(0)) {
  const n = payload.length + 1, out = new Uint8Array(n + 2);
  out[0] = n & 255; out[1] = n >> 8; out[2] = type; out.set(payload, 3);
  return out;
}
// the stream back into messages, in any chunking; push() -> [{ type, payload }], or null on a protocol error
export class Deframer {
  constructor(max = 1 << 20) { this.max = max; this.reset(); }
  reset() { this.buf = new Uint8Array(0); }
  push(bytes) {
    let b = this.buf.length ? concat(this.buf, bytes) : bytes;
    const out = [];
    let off = 0;
    while (b.length - off >= 2) {
      const len = b[off] | (b[off + 1] << 8);
      if (len === 0) { this.reset(); return null; }
      if (b.length - off < 2 + len) break;
      out.push({ type: b[off + 2], payload: b.slice(off + 3, off + 2 + len) });
      off += 2 + len;
    }
    this.buf = off < b.length ? b.slice(off) : new Uint8Array(0);
    if (this.buf.length > this.max) { this.reset(); return null; }
    return out;
  }
}
const concat = (a, b) => { const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; };
const u16 = (p, o) => p[o] | (p[o + 1] << 8);
const u32 = (p, o) => (p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24)) >>> 0;

export function helloMsg(name = HOST_NAME) {
  const p = new Uint8Array(34);
  p[0] = MR.PROTO; p[1] = 0;
  const t = new TextEncoder().encode(name).subarray(0, 31);
  p.set(t, 2);
  return frame(MR.H_HELLO, p);
}
// the Move's HELLO: "MOVR" proto16 caps16 rate32 block16 w8 h8 via8 pad8 out_cap32 name[24]; null if it is not one
export function parseHello(p) {
  if (p.length < 46 || String.fromCharCode(p[0], p[1], p[2], p[3]) !== "MOVR") return null;
  const name = new TextDecoder().decode(p.subarray(22, 46)).replace(/\0.*$/s, "");
  return { proto: u16(p, 4), caps: u16(p, 6), rate: u32(p, 8), block: u16(p, 12), w: p[14], h: p[15], via: p[16], outCap: u32(p, 18), name };
}
export function configMsg(key, value) {
  const p = new Uint8Array(5); p[0] = key;
  new DataView(p.buffer).setInt32(1, value | 0, true);
  return frame(MR.H_CONFIG, p);
}
export function pingMsg(token, ms) {
  const p = new Uint8Array(12), dv = new DataView(p.buffer), us = Math.max(0, Math.round(ms * 1000));
  dv.setUint32(0, token >>> 0, true); dv.setUint32(4, us % 0x100000000, true); dv.setUint32(8, Math.floor(us / 0x100000000), true);
  return frame(MR.H_PING, p);
}
// rfm_move's LED writes as messages: the RGB ones in one LED_RGB, the palette ones in one LED
export function ledMsgs(leds) {
  const rgb = leds.filter((l) => l.rgb), pal = leds.filter((l) => !l.rgb), out = [];
  if (rgb.length) {
    const p = new Uint8Array(rgb.length * 5);
    rgb.forEach((l, i) => p.set([l.cc ? 1 : 0, l.number & 127, l.r, l.g, l.b], i * 5));
    out.push(frame(MR.H_LED_RGB, p));
  }
  if (pal.length) {
    const p = new Uint8Array(pal.length * 3);
    pal.forEach((l, i) => p.set([l.cc ? 0xB0 : 0x90, l.number & 127, l.r & 127], i * 3));
    out.push(frame(MR.H_LED, p));
  }
  return out;
}

// ---- the firmware list for the core's browser (move.h rfm_move_fw_t)
export const FW_CATS = ["Synth", "Groovebox", "Chords", "Drums", "Other"];   // (RFM_MOVE_CAT_* bit order: steps 3..7)
// printable ASCII only (the core skips other bytes): accents dropped, typographic quotes and dashes made plain
export const asciiText = (s) => String(s == null ? "" : s).replace(/[\u2018\u2019\u2032]/g, "'").replace(/[\u201C\u201D\u2033]/g, '"')
  .replace(/[\u2010-\u2015\u2212]/g, "-").replace(/\u2026/g, "...").replace(/[\u00A0\u2009\u202F]/g, " ").replace(/\u00D7/g, "x")
  .normalize("NFKD").replace(/[^\x20-\x7e]/g, "").replace(/\s+/g, " ").trim();
// f: { id, name, version, desc, categories: [names], category, fantasy } -> core.fwList's entry (installed: the version
// the page loads; everything listed is on this site)
export function fwEntry(f, thumb) {
  const cats = (f.categories || []).reduce((b, c) => (FW_CATS.indexOf(c) >= 0 ? b | (1 << FW_CATS.indexOf(c)) : b), 0);
  const main = FW_CATS.indexOf(f.category || (f.categories || [])[0]);
  const v = asciiText(f.version) || "1";
  return { id: String(f.id), name: asciiText(f.name) || String(f.id), version: v, installed: v,
    desc: asciiText(f.desc) || null, thumb: thumb && thumb.length >= 512 ? thumb : null, cats, cat: main >= 0 ? main : 255, virt: f.fantasy ? 1 : 0 };
}
// a 64 x 64 RGBA picture -> the core's thumbnail: 1 bit, rows top first, MSB left, 1 = lit (Atkinson dithering on luma)
export function atkinson64(rgba) {
  const W = 64, l = new Float32Array(W * W), out = new Uint8Array(512);
  for (let i = 0; i < W * W; i++) l[i] = 0.299 * rgba[i * 4] + 0.587 * rgba[i * 4 + 1] + 0.114 * rgba[i * 4 + 2];
  const add = (x, y, e) => { if (x >= 0 && x < W && y < W) l[y * W + x] += e; };
  for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) {
    const v = l[y * W + x], on = v >= 128, e = (v - (on ? 255 : 0)) / 8;
    if (on) out[y * 8 + (x >> 3)] |= 0x80 >> (x & 7);
    add(x + 1, y, e); add(x + 2, y, e); add(x - 1, y + 1, e); add(x, y + 1, e); add(x + 1, y + 1, e); add(x, y + 2, e);
  }
  return out;
}

// ---- addresses
// "move.local", "172.16.254.1", "host:port", "ws://..", "http://.." -> ws://host:port/ws
export function normalizeUrl(addr) {
  let a = String(addr || "").trim();
  if (!a) return DEFAULT_URL;
  if (/^https?:\/\//i.test(a)) a = a.replace(/^http/i, "ws");
  if (!/^wss?:\/\//i.test(a)) a = "ws://" + a;
  let u;
  try { u = new URL(a); } catch (_) { return a; }
  if (!u.port) u.port = String(MR.PORT);
  if (u.pathname === "/" || !u.pathname) u.pathname = "/ws";
  return u.href;
}
export const hostOf = (url) => { try { return new URL(url).host; } catch (_) { return String(url); } };
const loopback = (host) => host === "localhost" || host.endsWith(".localhost") || /^127\./.test(host) || host === "[::1]" || host === "::1";
// a ws:// from an https page to anything but this computer: some browsers refuse it (mixed content), Chrome lets it
// through to local network addresses; the link tries it, then the bridge
export function mixedContentBlocked(url, pageProtocol) {
  if (pageProtocol !== "https:") return false;
  let u;
  try { u = new URL(url); } catch (_) { return false; }
  return u.protocol === "ws:" && !loopback(u.hostname);
}
export const MIXED_TEXT = "On https, Chrome may ask to allow local network devices; other browsers use a small bridge window that must stay open.";
export const LNA_TEXT = "The browser may ask to allow local network devices.";
export const BLOCKED_TEXT = "The browser blocked the bridge window. Allow pop-ups for this site, then open it again.";
export const isSecurityError = (e) => !!e && (e.name === "SecurityError" || e.code === 18);
// the bridge page for a ws:// address, and the origin its messages come from
export const bridgeOrigin = (url) => "http://" + hostOf(url);
export const bridgeUrl = (url, pageOrigin) => `${bridgeOrigin(url)}/bridge?origin=${encodeURIComponent(pageOrigin || "")}`;

// ---- the WebSocket transport. start() makes the socket now (a browser that refuses ws:// throws SecurityError
// there, synchronously); connect() starts it if not yet and resolves when it opens.
export class MoveWebSocket {
  constructor(url, WS = globalThis.WebSocket) {
    this.url = url; this.WS = WS; this.ws = null; this.p = null;
    this.name = "WebSocket";
    this.onmessage = null; this.onclose = null;
  }
  start() {
    if (this.p) return;
    if (!this.WS) throw new Error("this browser has no WebSocket");
    const ws = new this.WS(this.url);
    ws.binaryType = "arraybuffer";
    this.ws = ws;
    this.p = new Promise((resolve, reject) => {
      let open = false;
      ws.onopen = () => { open = true; resolve(); };
      ws.onerror = () => { if (!open) reject(new Error(`no answer at ${hostOf(this.url)}`)); };
      ws.onclose = (e) => {
        if (!open) { reject(new Error(`no answer at ${hostOf(this.url)}`)); return; }
        if (this.ws === ws) { this.ws = null; if (this.onclose) this.onclose((e && e.reason) || "the link closed"); }
      };
      ws.onmessage = (e) => {
        if (!this.onmessage) return;
        if (e.data instanceof ArrayBuffer) this.onmessage(new Uint8Array(e.data));
        else if (ArrayBuffer.isView(e.data)) this.onmessage(new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength));
      };
    });
    this.p.catch(() => {});                   // (a socket closed before connect() was asked for)
  }
  connect() {
    try { this.start(); } catch (e) { return Promise.reject(e); }
    return this.p;
  }
  send(bytes) {
    if (!this.ws || this.ws.readyState !== 1) return false;
    try { this.ws.send(bytes); return true; } catch (_) { return false; }
  }
  close() { const ws = this.ws; this.ws = null; if (ws) { try { ws.close(); } catch (_) { /* (gone) */ } } }
}

// ---- the bridge transport: Move Remote's bridge window (bridge protocol v1). bw: { w (the window), origin
// ("http://<move host:port>"), hello (it said hello once) }, shared by the transports of one window, so a reconnect
// reuses it. Messages only from bw.w at bw.origin; posted to that origin only. close() lets go of the window without
// closing it (MoveLink closes it when the link is done).
export class MoveBridge {
  constructor(bw, opts = {}) {
    this.bw = bw; this.win = opts.window || globalThis; this.T = opts.timers || globalThis;
    this.name = "bridge window";
    this.waitMs = HELLO_MS + BRIDGE_HELLO_MS;   // (MoveLink's wait for the Move's HELLO: the bridge's hello first)
    this.isOpen = false; this.onmessage = null; this.onclose = null;
    this.listener = null; this.poll = 0; this.helloTimer = 0;
  }
  post(msg, transfer) { try { this.bw.w.postMessage(msg, this.bw.origin, transfer || []); return true; } catch (_) { return false; } }
  connect() {
    return new Promise((resolve, reject) => {
      const bw = this.bw, host = bw.origin.replace(/^http:\/\//, "");
      let settled = false;
      const fail = (msg, final) => {
        if (settled) return;
        settled = true; this.detach();
        const e = new Error(msg); e.final = !!final; reject(e);
      };
      const ended = (why) => { this.detach(); if (this.onclose) this.onclose(why); };
      this.listener = (e) => {
        if (e.source !== bw.w || e.origin !== bw.origin) return;
        const m = e.data;
        if (!m || typeof m !== "object") return;
        if (m.mr === "hello") {
          bw.hello = true;
          this.T.clearTimeout(this.helloTimer); this.helloTimer = 0;
          this.post({ mr: "open" });
        } else if (m.mr === "open") {
          if (!settled) { settled = true; this.isOpen = true; resolve(); }
        } else if (m.mr === "data") {
          if (!this.isOpen || !this.onmessage) return;
          const d = m.data;
          if (d instanceof ArrayBuffer) this.onmessage(new Uint8Array(d));
          else if (ArrayBuffer.isView(d)) this.onmessage(new Uint8Array(d.buffer, d.byteOffset, d.byteLength));
        } else if (m.mr === "close") {
          if (this.isOpen) ended(m.reason || "the link closed");
          else fail(`no answer at ${host}${m.reason ? ` (${m.reason})` : ""}`);
        } else if (m.mr === "error") {
          if (!this.isOpen) fail(`no answer at ${host}`);  // (after it opened, its close follows)
        }
      };
      this.win.addEventListener("message", this.listener);
      this.poll = this.T.setInterval(() => {
        if (!bw.w.closed) return;
        if (this.isOpen) ended("the bridge window was closed");
        else fail("The bridge window was closed.", true);
      }, BRIDGE_POLL_MS);
      if (bw.hello) this.post({ mr: "open" });
      else this.helloTimer = this.T.setTimeout(() => fail(`The bridge window did not answer. Is the Move at ${host}? Its Move Remote may be older than 0.1.1 (no bridge): update it.`, true), BRIDGE_HELLO_MS);
    });
  }
  detach() {
    this.isOpen = false;
    if (this.listener) { this.win.removeEventListener("message", this.listener); this.listener = null; }
    this.T.clearInterval(this.poll); this.poll = 0;
    this.T.clearTimeout(this.helloTimer); this.helloTimer = 0;
  }
  send(bytes) {
    if (!this.isOpen || this.bw.w.closed) return false;
    const buf = bytes.slice().buffer;
    return this.post({ mr: "send", data: buf }, [buf]);
  }
  close() { this.detach(); }
}

// matrix order (the core's) -> the panel's label / role order
const MATRIX_TO_LABEL = Array.from({ length: NB }, (_, m) => Array.from({ length: NB }, (_, b) => b).find((b) => labelToMatrix(b) === m));
const MATRIX_TO_ROLE = Array.from({ length: 7 }, (_, m) => [0, 1, 2, 3, 4, 5, 6].find((r) => roleToMatrix(r) === m));
const toB64 = (u8) => { let s = ""; for (const b of u8) s += String.fromCharCode(b); return btoa(s); };
const fromB64 = (s) => { try { return Uint8Array.from(atob(String(s)), (c) => c.charCodeAt(0)); } catch (_) { return null; } };

export class MoveLink {
  // opts: { controls, site, prefs, save(), toast(text, kind, ms), mapAct(action) (MASTER), core(settingsBytes) ->
  //         Promise<MoveCore>, transport(url) -> transport (default MoveWebSocket), now(), timers, location, onActive,
  //         projects (a MoveProjects store; default IndexedDB's), WebSocket (the constructor), window (open(),
  //         message events: the bridge window) }
  // The link's logic (HELLO, CONFIG, pings, silence, retries, BYE, the LEDs, the screen's pacing, letting go) is the
  // core's session (rfm1cs-abletonmove's session.h, in the same WebAssembly module); this class moves its bytes over
  // the transport, keeps its one timer (sessNextMs), and turns its events into the page's.
  constructor(opts) {
    this.o = opts;
    this.now = opts.now || (() => performance.now());
    this.T = opts.timers || globalThis;
    this.loc = opts.location !== undefined ? opts.location : globalThis.location;
    this.win = opts.window !== undefined ? opts.window : globalThis;
    this.makeTransport = opts.transport || ((url) => new MoveWebSocket(url, opts.WebSocket || globalThis.WebSocket));
    this.via = "direct"; this.bw = null; this.offer = false; this.gen = 0;   // (the bridge window, and its button)
    this.core = null; this.t = null; this.pre = null;
    this.want = false; this.wasLive = false; this.why = ""; this.whyCode = 0; this.closeWhy = "";
    this.tone = "off"; this.detail = "Not connected."; this.subs = new Set();
    this.timers = { sess: 0, seq: 0 };
    this.store = opts.projects || new MoveProjects();
    this.seq = new SeqClock({ advance: (n, r) => this.core.seqAdvance(n, r), midiIn: (b) => this.core.seqMidiIn(b), emit: (a, at) => this.apply(a, at) });
    this.wasPlaying = false; this.seqKey = ""; this.projSubs = new Set(); this.projList = []; this.coreP = null;
    this.px = null; this.sent = 0;
    this.labels = null; this.keymap = null;
    this.fw = { list: [], thumbs: new Map(), pending: null, asked: false };   // (the firmware browser: fwInit)
    const s = opts.site;
    s.on("leds", (levels, green) => this.onLeds(levels, green));
    s.on("frame", (px) => { this.px = px; if (this.core && this.want) this.sess((c, now) => c.sessFrame(now, px)); });
    s.on("info", (info) => this.onInfo(info));
    s.on("firmware", () => { const sc = s.screen(); this.px = sc.pixels; this.onInfo(s.current().info); if (this.core && this.want) this.sess((c, now) => c.sessFrame(now, this.px)); this.fwSync(); });
    s.on("loopLevels", (peaks) => { if (this.live && this.core.sampleLevels(peaks) > 0) this.sess((c, now) => c.sessRedraw(now)); });   // (Sample's waveform)
    s.on("midiOut", (bytes, frame) => { if (this.live) { this.seq.clock(bytes, frame); this.sess((c, now) => c.sessUpdate(now)); } });
    if (typeof addEventListener === "function") addEventListener("pagehide", () => { this.flush(); this.release(); if (this.t) this.t.close(); this.closeBridge(); });
  }
  on(cb) { this.subs.add(cb); return () => this.subs.delete(cb); }
  url() { return normalizeUrl(this.o.prefs.moveUrl || DEFAULT_URL); }
  get st() { return this.core ? this.core.sessStatus() : null; }
  get live() { return !!(this.core && this.core.sessStatus().live); }
  // what the tests and the page read: the session's counters, the Move's STATUS, the last screen sent
  get stats() {
    const st = this.st;
    return { ledWrites: st ? st.ledWrites : 0, ledMsgs: st ? st.ledMsgs : 0, screens: st ? st.screens : 0, inputs: st ? st.inputs : 0,
      pings: st ? st.pings : 0, pongs: st ? st.pongs : 0, sent: this.sent };
  }
  get status() { const st = this.st; return st && st.battery >= 0 ? { battery: st.battery, power: st.power, jacks: st.jacks, ledPending: st.ledPending } : null; }
  get shown() { return this.core ? this.core.sessShown() : null; }
  moveName() { return this.core ? this.core.sessName() : ""; }
  view() {
    const st = this.st, live = !!(st && st.live);
    return { tone: this.tone, detail: this.detail, want: this.want, active: live, url: this.url(),
      link: this.t ? this.t.name : "", via: live ? MR.VIA[st.via] || "" : "", name: live ? this.moveName() : "",
      mode: live ? MODE_NAMES[this.core.state().mode] : "",
      clock: live ? (this.core.following() ? "firmware" : "INT") : "", bpm10: live ? this.core.bpm10() : 0,
      playing: !!(live && this.core.playing()), project: this.core ? this.core.projectSlot() : -1,
      bridge: this.offer };                 // (show "Open the bridge window")
  }
  set(tone, detail) { this.tone = tone; this.detail = detail; for (const cb of this.subs) cb(this.view()); }
  changed() { if (this.o.onActive) this.o.onActive(this.live); }

  // ---- the session: a call into it, its events handled in order, its timer armed again
  sess(fn) {
    if (!this.core) return 0;
    const now = Math.max(0, Math.floor(this.now()));
    const r = fn(this.core, now);
    for (const ev of r.evs) this.event(ev);
    this.arm();
    return r.res;
  }
  arm() {
    if (!this.core) return;
    this.T.clearTimeout(this.timers.sess); this.timers.sess = 0;
    const ms = this.core.sessNextMs(Math.max(0, Math.floor(this.now())));
    if (ms >= 0) this.timers.sess = this.T.setTimeout(() => { this.timers.sess = 0; this.sess((c, now) => c.sessTick(now)); }, ms);
  }
  event(ev) {
    switch (ev.type) {
      case "send": this.send(ev.bytes); return;
      case "open": this.openTransport(ev.attempt); return;
      case "close": { const t = this.t; this.t = null; if (t) { t.onmessage = null; t.onclose = null; t.close(); } return; }
      case "action": this.apply([ev.act]); return;
      case "released":
        if (this.o.site.midiCut) this.o.site.midiCut();
        this.wasPlaying = false;
        this.seq.reset();
        this.o.controls.releaseSrc(SRC.MOVE);
        return;
      case "lost": this.onLost(ev.why, ev.wasLive); return;
      case "bye": this.onBye(ev.reason, ev.wasLive); return;
      case "status": this.onStatus(ev.changed); return;
      case "loaded": if (!ev.ok) this.o.toast(`${projectName(ev.slot)} could not be read.`, "bad"); return;
    }
  }

  // from a click (url: the address field; how.click: called in the click, so the bridge window may open), or by itself
  // at load where it was connected before (auto). Everything up to window.open() runs synchronously, in the click.
  async connect(url, how = {}) {
    if (url !== undefined) this.o.prefs.moveUrl = normalizeUrl(url);
    const u = this.url();
    this.want = true;                       // (before stop(): a bridge window still open is kept for this try)
    this.stop();
    const gen = ++this.gen;
    this.offer = false; this.via = "direct";
    if (!this.o.prefs.moveAuto) this.o.prefs.moveAuto = true;
    this.o.save();
    let pre = null;
    if (this.mayBlock(u)) {                 // https: the ws:// made now; a browser that refuses it throws here
      pre = this.makeTransport(u);
      try { if (pre.start) pre.start(); }
      catch (e) {
        if (isSecurityError(e)) return this.viaBridge(u, !!how.click, gen);
        this.want = false; this.set("bad", `${cap(e && e.message ? e.message : String(e))}.`); return;
      }
      this.closeBridge();
      this.set("busy", `Connecting to ${hostOf(u)}... ${LNA_TEXT}`);
    } else {
      this.closeBridge();
      this.set("busy", `Connecting to ${hostOf(u)}...`);
    }
    if (!(await this.coreReady(gen))) { if (pre) pre.close(); return; }
    this.begin(pre);
  }
  // from a click on "Open the bridge window"
  openBridge() {
    const u = this.url();
    this.want = true;
    this.stop();
    const gen = ++this.gen;
    this.offer = false;
    if (!this.o.prefs.moveAuto) { this.o.prefs.moveAuto = true; this.o.save(); }
    return this.viaBridge(u, true, gen);
  }
  async viaBridge(u, click, gen) {
    this.via = "bridge";
    if (!this.bridgeWindow(u, click)) {
      this.want = false; this.offer = true;
      this.set("bad", click ? BLOCKED_TEXT : "This browser reaches the Move through a small bridge window: open it.");
      return;
    }
    this.set("busy", `Connecting to ${hostOf(u)} through the bridge window (keep it open)...`);
    if (!(await this.coreReady(gen))) return;
    this.begin(null);
  }
  // the session started (its first try opens the transport: openTransport)
  begin(pre) {
    this.pre = pre;
    // tries before the first connection: from https, direct, one (then the bridge is offered); else three
    this.core.sessGiveUp(this.via === "direct" && this.mayBlock(this.url()) ? 0 : 2);
    this.sess((c, now) => c.sessStart(now));
  }
  async coreReady(gen) {
    try { await this.ensureCore(); }
    catch (e) {
      if (gen === this.gen && this.want) { this.want = false; this.stop(); this.set("bad", "The Move's module did not load (web/move/build.sh builds it)."); }
      return false;
    }
    return this.want && gen === this.gen;
  }
  mayBlock(u) { return mixedContentBlocked(u, this.loc && this.loc.protocol); }
  // the bridge window for the address: the open one, or (in a click only) a new one; null when there is none
  bridgeWindow(u, click) {
    const origin = bridgeOrigin(u);
    if (this.bw && this.bw.origin === origin && !this.bw.w.closed) return this.bw;
    this.closeBridge();
    if (!click || !this.win || typeof this.win.open !== "function") return null;
    let w = null;
    try { w = this.win.open(bridgeUrl(u, this.loc && this.loc.origin), BRIDGE_NAME, BRIDGE_FEATURES); } catch (_) { w = null; }
    if (!w) return null;
    this.bw = { w, origin, hello: false };
    return this.bw;
  }
  bridgeOpen() { return !!(this.bw && !this.bw.w.closed); }
  // the bridge window told to close its socket and itself (closed from here a second later if it never answered)
  closeBridge() {
    const bw = this.bw;
    this.bw = null;
    if (!bw || bw.w.closed) return;
    try { bw.w.postMessage({ mr: "close" }, bw.origin); } catch (_) { /* (gone) */ }
    this.T.setTimeout(() => { try { if (!bw.w.closed) bw.w.close(); } catch (_) { /* (gone) */ } }, 1000);
  }
  bridgeGone() {
    this.want = false; this.stop();
    this.offer = true;
    this.set("bad", "The bridge window was closed. Open it again to reconnect.");
  }
  // the core (once): the settings from prefs, the labels and keymap, the store's projects and the last one opened
  ensureCore() {
    if (this.core) return Promise.resolve(this.core);
    if (!this.coreP) {
      this.coreP = (async () => {
        const core = await this.o.core(fromB64(this.o.prefs.move));
        core.sessHostName(HOST_NAME);
        core.setStoreHost({ read: (slot) => this.storeRead(slot), write: (slot, bytes) => { this.projSave(slot, bytes); return 0; },
          remove: (slot) => { this.projDelete(slot); return 0; } });
        this.core = core;
        this.applyLabels(); core.setKeymap(this.keymap);
        this.fwInit();
        await this.initProjects();
        return core;
      })().catch((e) => { this.coreP = null; this.core = null; throw e; });
    }
    return this.coreP;
  }
  async initProjects() {
    const core = this.core, recs = await this.store.list(), bits = dirBits(recs);
    core.projectDir(bits); core.storeInit(bits);
    for (const r of recs) core.projectInfo(r.slot, r.bytes);   // (what the project grid draws of each)
    const last = validSlot(this.o.prefs.moveProject) ? this.o.prefs.moveProject : 0, rec = recs.find((r) => r.slot === last);
    if (this.sess((c, now) => c.sessProjectsInit(now, bits, last, rec ? rec.bytes : null)) < 0)
      this.o.toast(`${projectName(last)} could not be read: it starts empty (the stored copy is kept until it is saved over).`, "bad");
    this.projList = recs;
    this.projChanged();
  }
  auto() { if (this.o.prefs.moveAuto && !this.want) return this.connect(); return Promise.resolve(); }
  disconnect() {
    this.want = false; this.offer = false;
    this.o.prefs.moveAuto = false; this.o.save();
    this.stop();
    this.set("off", "Not connected.");
  }
  // the session stopped (the project saved first, if it changed; the transport closed; everything held let go), the
  // timers gone; no longer wanted: the bridge window closed too
  stop() {
    const was = this.wasLive;
    if (this.core) this.sess((c, now) => c.sessStop(now));
    else if (this.t) { const t = this.t; this.t = null; t.onmessage = null; t.onclose = null; t.close(); }
    this.pre = null;
    for (const k of Object.keys(this.timers)) { this.T.clearTimeout(this.timers[k]); this.T.clearInterval(this.timers[k]); this.timers[k] = 0; }
    this.wasLive = false;
    if (!this.want) this.closeBridge();
    if (was) this.changed();
  }
  release() { if (this.core) this.sess((c, now) => c.sessRelease(now)); }
  flush() { if (this.core) this.sess((c, now) => c.sessFlush(now)); }

  // the session asks for the transport (a first try or a retry: attempt, its id, goes back with the transport's calls)
  openTransport(attempt) {
    const u = this.url();
    let t;
    if (this.via === "bridge") {
      const bw = this.bridgeWindow(u, false);
      if (!bw) { this.bridgeGone(); return; }
      t = new MoveBridge(bw, { window: this.win, timers: this.T });
    } else t = this.pre || this.makeTransport(u);
    this.pre = null;
    this.t = t;
    // the FM-1's LEDs, MASTER and screen as they are now (the session paints them on the Move's HELLO)
    const lv = this.o.site.leds();
    this.core.panel(lv.levels, lv.playGreen);
    this.core.setMaster(this.o.controls.master);
    this.px = this.o.site.screen().pixels;
    this.sess((c, now) => c.sessFrame(now, this.px));
    t.onmessage = (b) => { if (this.t === t) this.rx(b, attempt); };
    t.onclose = (why) => { if (this.t === t) { this.closeWhy = why; this.sess((c, now) => c.sessClosed(now, attempt, 0)); } };
    // https, direct: a socket still opening may be the browser asking about local network devices; it is left to
    // open (patient), and the bridge offered meanwhile
    const askable = this.via === "direct" && this.mayBlock(u);
    this.core.sessWait(Math.max(0, Math.floor(this.now())), t.waitMs || HELLO_MS, askable);
    this.arm();
    Promise.resolve().then(() => t.connect()).then(() => {
      if (this.t !== t) { t.close(); return; }
      this.sess((c, now) => c.sessOpened(now, attempt));
    }, (e) => {
      if (this.t !== t) return;
      if (e && e.final) { this.want = false; this.stop(); this.offer = this.via === "bridge"; this.set("bad", e.message); return; }
      this.closeWhy = e && e.message ? e.message : "no answer";
      this.sess((c, now) => c.sessClosed(now, attempt, 0));
    });
  }
  send(bytes) { if (this.t && this.t.send(bytes)) { this.sent++; return true; } return false; }
  // the words the session's C gives every host (session.h rfm1cs_status_text ..): the address shown, the transport's
  // own words for a close, its name
  words() { return { host: hostOf(this.url()), closed: this.closeWhy, link: this.t ? this.t.name : "" }; }
  whyText(why) { return this.core.whyText(why, this.words()); }
  // the link went (the session let everything go and closed the transport); it tries again while wanted
  onLost(why, wasLive) {
    this.why = this.whyText(why); this.whyCode = why;
    this.T.clearInterval(this.timers.seq); this.timers.seq = 0;
    if (wasLive) this.o.toast(this.core.lostText(why, wasLive, this.words()), "bad");
    if (this.wasLive) { this.wasLive = false; this.changed(); }
    if (!this.want || why === WHY.PROTO) return;
    if (this.via === "bridge" && !this.bridgeOpen()) { this.bridgeGone(); return; }   // (only a click opens it again)
    if (this.via === "direct" && !this.core.sessStatus().everLive && this.mayBlock(this.url())) {
      this.want = false; this.offer = true; this.stop();
      this.set("bad", `${cap(this.why)}. From this https page, try the bridge window.`);
    }
  }
  onBye(reason, wasLive) {
    this.want = false;
    this.stop();
    const { tone, text } = this.core.byeText(reason, wasLive);
    this.set(tone, text);
    this.o.toast(text);
  }
  onStatus(changed) {
    const st = this.core.sessStatus();
    if (changed & CH.STATE) {
      if (st.state === STATE.CONNECTED) this.onConnected();
      else if (st.state === STATE.RETRYING && this.want) { const t = this.core.sessText(this.words()); this.set(t.tone, t.text); }
      else if (st.state === STATE.FAILED && this.want) {
        const t = this.core.sessText(this.words());     // (before stop(): the words are FAILED's)
        this.want = false;
        if (this.whyCode === WHY.PROTO) this.stop(); else this.closeBridge();
        this.set(t.tone, t.text);
      }
    }
    if ((changed & CH.WAITING) && st.state === STATE.CONNECTING) {
      const t = this.core.sessText(this.words());
      if (st.waiting) { this.offer = true; this.set(t.tone, `${t.text} Allow local network devices if the browser asks, or open the bridge window.`); }
      else { this.offer = false; this.set(t.tone, t.text); }
    }
    // the sequencer: a stop cuts the timed notes; the open project kept; the status follows the clock and tempo
    if (this.wasPlaying && !st.playing && this.o.site.midiCut) this.o.site.midiCut();
    this.wasPlaying = st.playing;
    const cur = st.project;
    if (validSlot(cur) && cur !== this.o.prefs.moveProject) { this.o.prefs.moveProject = cur; this.o.save(); this.projChanged(); }
    const v = this.view(), key = `${v.clock}|${v.bpm10}|${v.playing}`;
    if (key !== this.seqKey) { this.seqKey = key; for (const cb of this.subs) cb(v); }
  }
  onConnected() {
    const first = !this.wasLive, name = this.moveName();
    this.wasLive = true;
    this.T.clearInterval(this.timers.seq);
    this.seq.reset();
    this.timers.seq = this.T.setInterval(() => this.pump(), SEQ_MS);
    const t = this.core.sessText(this.words());
    this.set(t.tone, t.text);
    if (first) { this.o.toast(`${name || "The Move"} is the FM-1's controller now.`, "ok"); this.changed(); }
  }

  // ---- the Move's bytes: the sequencer brought up to the audio clock first (its input lands in the core's timeline)
  rx(bytes, attempt) {
    this.pump();
    if (this.core) this.core.setMaster(this.o.controls.master);
    this.sess((c, now) => c.sessRx(now, attempt, bytes));
  }
  // ---- the sequencer's clock: the core advanced to the audio clock (or the wall clock while no audio runs)
  audioNow() { const a = this.o.site.audioNow ? this.o.site.audioNow() : null; return a || { ms: this.now() }; }
  pump() {
    if (!this.live) return;
    this.seq.pump(this.audioNow());
    const lu = this.seq.rate ? Math.round((this.seq.lead / this.seq.rate) * 1e6) : 0;   // (recordings: where it was heard)
    if (lu !== this.leadUs) { this.leadUs = lu; this.core.setLead(lu); }
    this.sess((c, now) => c.sessPumped(now));           // (the status, the LEDs, the playhead / blinks / popups)
  }

  // ---- the core's actions onto the page; at: the absolute audio frame the sequencer's actions are due at (a.frame
  // after it), or undefined / null: now. The project actions go to the store (rfm1cs's store.h, IndexedDB's slots).
  apply(acts, at) {
    const c = this.o.controls;
    for (const a of acts) {
      if (a.kind === "midi") {
        if (typeof at === "number") this.o.site.midiIn(a.bytes, at + (a.frame || 0));
        else this.o.site.midiIn(a.bytes, undefined, { cancel: true });
        continue;
      }
      if (a.kind === "key") c.holdKey(a.k, SRC.MOVE, a.down, a.down ? a.vel : 0);
      else if (a.kind === "press") c.leanKey(a.k, lean(a.value));
      else if (a.kind === "btn") c.holdBtn(MATRIX_TO_LABEL[a.m], SRC.MOVE, a.down);
      else if (a.kind === "turn") c.turn(MATRIX_TO_ROLE[a.enc], a.steps);
      else if (a.kind === "master") this.o.mapAct({ kind: "master", value: a.value });
      else if (a.kind === "settings") this.keep();
      else if (/^fw/.test(a.kind)) this.fwAct(a);
      else if (/^project/.test(a.kind)) this.sess((core, now) => core.storeAction(now, a));
      else if (a.kind === "click") { if (this.o.site.click) this.o.site.click(typeof at === "number" ? at + (a.frame || 0) : null, a.accent); }
      else if (a.kind === "audioRec") {   // Sample: a loop of whole bars, from a bar line to a bar line (worklet frames)
        if (this.o.site.loopRec) this.o.site.loopRec(a.on, typeof at === "number" ? at + (a.frame || 0) : null, a.bars, this.core.bpm10());
      }
    }
  }

  // ---- projects: the store's slot I/O on IndexedDB (in order: its queue), the reads answered later
  storeRead(slot) {
    this.store.get(slot).then((r) => this.sess((c, now) => c.storeAnswer(now, slot, r ? r.bytes : null)));
    return STORE_PENDING;
  }
  projSave(slot, bytes) {
    return this.store.put(slot, bytes).then(() => this.refreshProjects(), (e) => { this.o.toast(`The Move's ${projectName(slot)} was not kept: ${e.message || e}.`, "bad"); });
  }
  projDelete(slot) { return this.store.remove(slot).then(() => this.refreshProjects(), () => this.refreshProjects()); }
  onProjects(cb) { this.projSubs.add(cb); return () => this.projSubs.delete(cb); }
  projChanged() { for (const cb of this.projSubs) { try { cb(this.projects()); } catch (e) { console.error(e); } } }
  async refreshProjects() {
    this.projList = await this.store.list();
    if (this.core) {
      const bits = dirBits(this.projList);
      this.core.projectDir(bits); this.core.storeInit(bits);
      for (const r of this.projList) this.core.projectInfo(r.slot, r.bytes);
    }
    this.projChanged();
    return this.projList;
  }
  // what the page lists: [{ slot, name, size, saved, current }]
  projects() {
    const cur = this.core ? this.core.projectSlot() : validSlot(this.o.prefs.moveProject) ? this.o.prefs.moveProject : -1;
    return this.projList.map((r) => ({ slot: r.slot, name: projectName(r.slot), size: r.bytes.length, saved: r.saved, current: r.slot === cur }));
  }
  // Backup: one project's file, or every project's ({ name, bytes }), null when there is none
  async backup(slot) {
    if (this.core) this.flush();
    await this.store.chain;
    const recs = await this.store.list(), pick = slot === undefined ? recs : recs.filter((r) => r.slot === slot);
    if (!pick.length) return null;
    return { name: slot === undefined ? SET_NAME : fileName(slot), bytes: packFile(await this.ensureCore(), pick, slot === undefined ? 2 : 1) };
  }
  // Import a file's bytes: a single project into `slot` (or the first free one; -1 / undefined), a set into its own
  // slots. Every project is checked by the core first; nothing is stored unless all are good. Returns the slots
  // written; throws an Error saying why it was refused
  async importFile(bytes, slot) {
    if (!bytes || bytes.length > MAX_FILE) throw new Error(`Not a Move project file: ${bytes ? `it is ${bytes.length} bytes, more than a full set` : "nothing to read"}.`);
    const core = await this.ensureCore();
    const f = unpackFile(core, bytes);
    const recs = await this.store.list(), used = dirBits(recs);
    let todo = f.entries;
    if (f.kind === 1) {
      const to = validSlot(slot) ? slot : firstFree(used);
      if (to < 0) throw new Error("All 32 project slots are used: pick one to replace, or delete one first.");
      todo = [{ ...f.entries[0], slot: to }];
    }
    for (const e of todo) await this.store.put(e.slot, e.bytes, new Date().toISOString());
    const cur = core.projectSlot(), mine = todo.find((e) => e.slot === cur);
    if (mine) this.sess((c, now) => c.sessProjectAnswer(now, cur, mine.bytes));
    await this.refreshProjects();
    return todo.map((e) => e.slot);
  }
  async deleteProject(slot) {
    await this.store.remove(slot);
    if (this.core && this.core.projectSlot() === slot) this.sess((c, now) => c.sessProjectAnswer(now, slot, null));
    await this.refreshProjects();
  }
  // ---- the firmware browser (0.5.10: one firmware, no FX): the page's firmwares, its loads, the library
  fwInit() {
    const F = this.o.firmwares, c = this.core;
    if (!F || !c) return;
    c.fwCaps(0);                                             // (before fwList: one slot, no FX setup)
    c.fwLibrary(fromB64(this.o.prefs.moveLibrary || ""));    // (-1: not a library; it starts empty)
    this.fwSend();
    if (F.thumbs && !this.fw.asked) {
      this.fw.asked = true;
      Promise.resolve().then(() => F.thumbs(this.fw.list.map((e) => e.id))).then((m) => {
        if (m && m.size) { this.fw.thumbs = m; this.fwSend(); }
      }).catch(() => {});
    }
  }
  // the list again (the array kept until the next one: the wrapper copies it into the module), and slot 0
  fwSend() {
    const F = this.o.firmwares;
    if (!F || !this.core) return;
    this.fw.list = (F.list() || []).map((f) => fwEntry(f, this.fw.thumbs.get(f.id)));
    this.fwApply(this.core.fwList(this.fw.list));
    this.fwSync();
  }
  fwCurrent() { const id = this.o.firmwares ? this.o.firmwares.current() : null; return id && this.fw.list.some((e) => e.id === id) ? id : null; }
  // slot 0 = what the page plays (after a load, an unload refused, the page's own picker)
  fwSync() { if (this.core && this.o.firmwares) this.fwApply(this.core.fwSlots([this.fwCurrent()])); }
  fwApply(acts) {
    if (acts && acts.length) this.apply(acts);
    if (this.live) { this.sess((c, now) => c.sessFlushLeds(now)); this.sess((c, now) => c.sessRedraw(now)); }
  }
  fwAct(a) {
    const F = this.o.firmwares;
    if (!F) return;
    if (a.kind === "fwLibrary") {
      const v = toB64(a.bytes);
      if (v !== this.o.prefs.moveLibrary) { this.o.prefs.moveLibrary = v; this.o.save(); }
      return;
    }
    if (a.kind === "fwLevel") { if (a.slot === 0 && F.level) F.level(a.level); return; }
    const e = this.fw.list[a.fw];
    // (answered after the session's call that brought the action)
    if (a.kind === "fwLoad" && e && a.slot === 0) {
      if (this.fw.pending) { if (this.fw.pending !== e.id) Promise.resolve().then(() => this.core && this.fwApply(this.core.fwProgress(e.id, FW_BUSY))); return; }
      this.fw.pending = e.id;
      Promise.resolve().then(() => F.load(e.id)).then((ok) => ok !== false, () => false).then((ok) => {
        this.fw.pending = null;
        if (!this.core) return;
        if (!ok) this.o.toast(`${e.name} could not be loaded.`, "bad");
        this.fwApply(this.core.fwProgress(e.id, ok ? 100 : FW_FAILED));
        this.fwSync();
      });
    } else if (a.kind === "fwUnload") {
      Promise.resolve().then(() => {
        const cur = this.fw.list.find((x) => x.id === this.fwCurrent());
        if (cur) this.o.toast(`The page always plays a firmware: ${cur.name} stays loaded.`);
        this.fwSync();
      });
    } else if (a.kind === "fwDownload" && e) {                // (never offered: everything listed is installed)
      Promise.resolve().then(() => { if (!this.core) return; this.core.fwInstalled(a.fw, e.version); this.fwApply(this.core.fwProgress(e.id, 100)); });
    }
  }
  keep() {
    if (!this.core) return;
    const v = toB64(this.core.save());
    if (v === this.o.prefs.move) return;
    this.o.prefs.move = v; this.o.save();
    if (this.live) this.set(this.tone, this.detail.replace(/: [^:]* mode\./, `: ${MODE_NAMES[this.core.state().mode]} mode.`));
  }

  // ---- what it shows (the session sends only what changed: LEDs at once, the screen at most every SCREEN_MS)
  onLeds(levels, green) {
    if (!this.core) return;
    this.sess((c, now) => c.sessPanel(now, levels, green));
  }
  onInfo(info) {
    this.keymap = info && Array.isArray(info.keymap) ? info.keymap : null;
    if (!this.core) return;
    this.core.setKeymap(this.keymap);
    this.sess((c, now) => c.sessFlushLeds(now));
  }
  // the firmware's knob labels (panel.js panelLabels().knobs: by role, 8, null = the printed name)
  setLabels(knobs) {
    this.labels = Array.isArray(knobs) ? knobs.slice(0, 8) : null;
    if (!this.core) return;
    this.applyLabels();
    this.sess((c, now) => c.sessRedraw(now));
  }
  applyLabels() {
    for (let r = 0; r < 8; r++) this.core.setLabel(r === E.MASTER ? 7 : roleToMatrix(r), this.labels && this.labels[r] ? this.labels[r] : "");
  }
}
const cap = (s) => String(s).charAt(0).toUpperCase() + String(s).slice(1);

// ---- the sheet's section
export function mountMove(slot, link) {
  const addr = h("input", { type: "text", class: "move-addr", spellcheck: "false", autocomplete: "off", "aria-label": "The Move's address",
    placeholder: "move.local", value: hostOf(link.url()).replace(/:7720$/, "") });
  const btn = h("button", { class: "pill primary" }, "Connect my Move");
  const line = h("p", { class: "remote-status small" });
  const usb = h("button", { class: "pill", title: `The Move's address over its USB-C cable: ${MR.USB_ADDR}` }, "USB-C");
  usb.addEventListener("click", () => { addr.value = MR.USB_ADDR; });
  // (connect() and openBridge() run in the click: the bridge window is a popup)
  const bridge = h("button", { class: "pill primary", hidden: true }, "Open the bridge window");
  bridge.addEventListener("click", () => link.openBridge());
  const go = () => { const v = link.view(); if (v.want && v.tone !== "bad") link.disconnect(); else link.connect(addr.value, { click: true }); };
  btn.addEventListener("click", go);
  addr.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); if (!link.view().want || link.view().tone === "bad") link.connect(addr.value, { click: true }); } });
  const show = (v) => {
    const on = v.want && v.tone !== "bad";
    btn.textContent = on ? "Disconnect" : v.tone === "bad" ? "Try again" : "Connect my Move";
    btn.className = "pill " + (on || v.bridge ? "" : "primary");
    bridge.hidden = !v.bridge;
    addr.disabled = on;
    line.replaceChildren(h("span", { class: "fm1-dot " + v.tone + (v.tone === "busy" ? " live" : "") }), v.detail);
    line.className = "remote-status small " + (v.tone === "on" ? "ok" : v.tone === "bad" ? "bad" : "muted");
  };
  link.on(show);
  show(link.view());
  // the sequencer: its clock source and tempo, and the 32 projects (Backup, Import, Delete)
  const clock = h("p", { class: "move-clock small muted" });
  const showClock = (v) => {
    clock.textContent = v.clock ? `Sequencer: ${v.playing ? "playing" : "stopped"}, clock ${v.clock === "firmware" ? "from the firmware" : "INT"} ${(v.bpm10 / 10).toFixed(1)} BPM${validSlot(v.project) ? `, ${projectName(v.project)}` : ""}.` : "";
    clock.hidden = !v.clock;
  };
  link.on(showClock);
  showClock(link.view());
  const list = h("div", { class: "move-projects" });
  const into = h("select", { class: "move-into", "aria-label": "Import into" });
  const file = h("input", { type: "file", class: "move-file", accept: `${EXT_ONE},${EXT_SET}`, hidden: true });
  const importBtn = h("button", { class: "pill" }, "Import...");
  const allBtn = h("button", { class: "pill" }, "Back up all");
  const say = (t, kind) => link.o.toast(t, kind);
  const fmtDate = (iso) => { const d = new Date(iso); return isNaN(d) ? "" : d.toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" }); };
  const showProjects = (ps) => {
    const used = new Set(ps.map((p) => p.slot)), keep = into.value;
    into.replaceChildren(h("option", { value: "-1" }, "first free slot"),
      ...Array.from({ length: SLOTS }, (_, s) => h("option", { value: String(s) }, `into ${projectName(s)}${used.has(s) ? " (replaces it)" : ""}`)));
    into.value = keep || "-1";
    allBtn.disabled = !ps.length;
    list.replaceChildren(...(ps.length ? ps.map((p) => h("div", { class: "move-proj" + (p.current ? " current" : ""), "data-slot": String(p.slot) },
      h("span", { class: "move-proj-name" }, p.name, p.current ? " (open)" : ""),
      h("span", { class: "move-proj-meta small muted" }, `${(p.size / 1024).toFixed(1)} KB, ${fmtDate(p.saved)}`),
      h("button", { class: "pill small", onclick: () => link.backup(p.slot).then((f) => f && download(f.name, f.bytes)) }, "Backup"),
      h("button", { class: "pill small", onclick: () => link.deleteProject(p.slot).then(() => say(`${p.name} deleted.`)) }, "Delete")))
      : [h("p", { class: "small muted" }, "No projects yet: Track 4 on the Move is the sequencer, Shift + step 1 its 32 projects.")]));
  };
  link.onProjects(showProjects);
  showProjects(link.projects());
  link.refreshProjects().catch(() => {});
  allBtn.addEventListener("click", () => link.backup().then((f) => f && download(f.name, f.bytes)));
  importBtn.addEventListener("click", () => file.click());
  file.addEventListener("change", async () => {
    const f = file.files && file.files[0];
    file.value = "";
    if (!f) return;
    try {
      if (f.size > MAX_FILE) throw new Error(`Not a Move project file: it is ${f.size} bytes, more than a full set.`);
      const slots = await link.importFile(new Uint8Array(await f.arrayBuffer()), Number(into.value));
      say(`Imported ${slots.map(projectName).join(", ")}.`, "ok");
    } catch (e) { say(e && e.message ? e.message : String(e), "bad"); }
  });
  slot.replaceChildren(
    h("p", { class: "row-sub", style: { margin: "0 0 10px" } }, "An Ableton Move running Move Remote becomes this FM-1's controller: its pads play the keys (or MIDI), its buttons and knobs are the FM-1's, and the page lights its pads and draws its screen. Track 1..4: FM-1, MIDI, Control, Sequencer."),
    h("div", { class: "move-row" }, addr, usb),
    h("div", { class: "pills two" }, btn, bridge),
    line,
    clock,
    h("p", { class: "row-sub", style: { margin: "12px 0 6px" } }, "The sequencer's projects, in this browser (not in the firmware's saves):"),
    list,
    h("div", { class: "move-row move-import" }, into, importBtn, allBtn, file),
    h("p", { class: "note" }, `Wi-Fi: move.local (or the Move's address); USB-C: ${MR.USB_ADDR}.${link.loc && link.loc.protocol === "https:" ? " " + MIXED_TEXT : ""} Shift + touch the volume knob + click the jog wheel on the Move to leave Move Remote.`));
}
// a file to the visitor's downloads
function download(name, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" })), a = document.createElement("a");
  a.href = url; a.download = name;
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}
