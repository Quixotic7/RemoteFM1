// SPDX-License-Identifier: MPL-2.0
// USE MY ABLETON MOVE: an Ableton Move running Move Remote (github.com/Quixotic7/abletonmoveremote) as the FM-1's
// controller. Its pads, steps, buttons and knobs drive the page's panel (source SRC.MOVE) or the firmware's MIDI in,
// and the page lights its pads and buttons from the FM-1's LEDs and draws its 128 x 64 screen. The mapping is the
// portable C core rfm_move (host/src/move/README.md), in WebAssembly (js/move-core.js); this file is the protocol
// (Move Remote's docs/PROTOCOL.md: u16 len | u8 type | payload) and the page's side, modelled on js/omx-link.js.
//
// The link is a transport: { name, connect() -> Promise, send(Uint8Array) -> bool, onmessage(Uint8Array),
// onclose(why), close() }. One now: MoveWebSocket (ws://<move>:7720/ws, move.local over Wi-Fi or 172.16.254.1 over
// USB-C). A page served over https can't open a ws:// to the Move (mixed content): the link says so instead of
// failing silently (a localhost address is allowed). Another transport (Web MIDI over the Move's USB-C, a local
// bridge) drops in through opts.transport.
//
// Session: HELLO (proto 1) -> the Move's HELLO; CONFIG audio clock off (no AUDIO messages: the page plays its own
// sound); INPUT records -> the core -> actions; the core's changed LEDs out after each input and each LED change of
// the FM-1 (LED_RGB / LED); the screen after a frame, an input or a label change, at most ~30 a second, and only when
// it changed (DISPLAY). PING every second; nothing heard for 3 s (STATUS comes every 500 ms) and the link is lost.
// Lost: everything the Move held is let go, and it connects again (1, 2, 4 s ...) while wanted. BYE: the exit
// gesture or another host taking the Move ends it; a power off tries again.
import { h } from "./sheet.js";
import { SRC, E, NB, labelToMatrix, roleToMatrix } from "./panel.js";
import { lean, MODE_NAMES } from "./move-core.js";

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
export const SCREEN_MS = 33, PING_MS = 1000, SILENT_MS = 3000, HELLO_MS = 4000, RETRY_MS = [1000, 2000, 4000];

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
// a page on https can't open ws:// (mixed content), except to this computer
export function mixedContentBlocked(url, pageProtocol) {
  if (pageProtocol !== "https:") return false;
  let u;
  try { u = new URL(url); } catch (_) { return false; }
  return u.protocol === "ws:" && !loopback(u.hostname);
}
export const MIXED_TEXT = "This page is on https, and browsers don't let an https page open a plain ws:// link to the Move. Open the player from http://localhost (web/serve.sh), or use a link over USB when there is one.";

// ---- the WebSocket transport
export class MoveWebSocket {
  constructor(url, WS = globalThis.WebSocket) {
    this.url = url; this.WS = WS; this.ws = null;
    this.name = "WebSocket";
    this.onmessage = null; this.onclose = null;
  }
  connect() {
    return new Promise((resolve, reject) => {
      if (!this.WS) { reject(new Error("this browser has no WebSocket")); return; }
      let ws, open = false;
      try { ws = new this.WS(this.url); } catch (e) { reject(e); return; }
      ws.binaryType = "arraybuffer";
      this.ws = ws;
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
  }
  send(bytes) {
    if (!this.ws || this.ws.readyState !== 1) return false;
    try { this.ws.send(bytes); return true; } catch (_) { return false; }
  }
  close() { const ws = this.ws; this.ws = null; if (ws) { try { ws.close(); } catch (_) { /* (gone) */ } } }
}

// matrix order (the core's) -> the panel's label / role order
const MATRIX_TO_LABEL = Array.from({ length: NB }, (_, m) => Array.from({ length: NB }, (_, b) => b).find((b) => labelToMatrix(b) === m));
const MATRIX_TO_ROLE = Array.from({ length: 7 }, (_, m) => [0, 1, 2, 3, 4, 5, 6].find((r) => roleToMatrix(r) === m));
const toB64 = (u8) => { let s = ""; for (const b of u8) s += String.fromCharCode(b); return btoa(s); };
const fromB64 = (s) => { try { return Uint8Array.from(atob(String(s)), (c) => c.charCodeAt(0)); } catch (_) { return null; } };

export class MoveLink {
  // opts: { controls, site, prefs, save(), toast(text, kind, ms), mapAct(action) (MASTER), core(settingsBytes) ->
  //         Promise<MoveCore>, transport(url) -> transport (default MoveWebSocket), now(), timers, location, onActive }
  constructor(opts) {
    this.o = opts;
    this.now = opts.now || (() => performance.now());
    this.T = opts.timers || globalThis;
    this.loc = opts.location !== undefined ? opts.location : globalThis.location;
    this.makeTransport = opts.transport || ((url) => new MoveWebSocket(url));
    this.core = null; this.t = null; this.hello = null; this.status = null;
    this.want = false; this.live = false; this.everLive = false; this.retries = 0;
    this.tone = "off"; this.detail = "Not connected."; this.subs = new Set();
    this.deframer = new Deframer();
    this.timers = { hello: 0, ping: 0, retry: 0, screen: 0 };
    this.lastRx = 0; this.token = 0;
    this.px = null; this.dirty = false; this.lastShot = -1e9; this.shown = null;
    this.labels = null; this.keymap = null;
    this.stats = { ledWrites: 0, ledMsgs: 0, screens: 0, inputs: 0, pings: 0, pongs: 0, sent: 0 };
    const s = opts.site;
    s.on("leds", (levels, green) => this.onLeds(levels, green));
    s.on("frame", (px) => { this.px = px; this.screenSoon(); });
    s.on("info", (info) => this.onInfo(info));
    s.on("firmware", () => { const sc = s.screen(); this.px = sc.pixels; this.onInfo(s.current().info); this.screenSoon(); });
    if (typeof addEventListener === "function") addEventListener("pagehide", () => { if (this.t) this.t.close(); });
  }
  on(cb) { this.subs.add(cb); return () => this.subs.delete(cb); }
  url() { return normalizeUrl(this.o.prefs.moveUrl || DEFAULT_URL); }
  view() {
    return { tone: this.tone, detail: this.detail, want: this.want, active: this.live, url: this.url(),
      link: this.t ? this.t.name : "", via: this.hello ? MR.VIA[this.hello.via] || "" : "", name: this.hello ? this.hello.name : "",
      mode: this.core && this.live ? MODE_NAMES[this.core.state().mode] : "" };
  }
  set(tone, detail) { this.tone = tone; this.detail = detail; for (const cb of this.subs) cb(this.view()); }
  changed() { if (this.o.onActive) this.o.onActive(this.live); }

  // from a click (url: the address field), or by itself at load where it was connected before (auto)
  async connect(url) {
    if (url !== undefined) this.o.prefs.moveUrl = normalizeUrl(url);
    const u = this.url();
    this.stop();
    this.want = true; this.everLive = false; this.retries = 0;
    if (!this.o.prefs.moveAuto) this.o.prefs.moveAuto = true;
    this.o.save();
    if (mixedContentBlocked(u, this.loc && this.loc.protocol)) { this.want = false; this.set("bad", MIXED_TEXT); return; }
    this.set("busy", `Connecting to ${hostOf(u)}...`);
    if (!this.core) {
      try { this.core = await this.o.core(fromB64(this.o.prefs.move)); }
      catch (e) { this.want = false; this.set("bad", "The Move's module did not load (web/move/build.sh builds it)."); return; }
      this.applyLabels(); this.core.setKeymap(this.keymap);
    }
    if (!this.want) return;
    this.open(u);
  }
  auto() { if (this.o.prefs.moveAuto && !this.want) return this.connect(); return Promise.resolve(); }
  disconnect() {
    this.want = false;
    this.o.prefs.moveAuto = false; this.o.save();
    this.stop();
    this.set("off", "Not connected.");
  }
  // the transport and timers gone, everything held let go
  stop() {
    for (const k of Object.keys(this.timers)) { this.T.clearTimeout(this.timers[k]); this.T.clearInterval(this.timers[k]); this.timers[k] = 0; }
    const t = this.t;
    this.t = null;
    if (t) { t.onmessage = null; t.onclose = null; t.close(); }
    const was = this.live;
    this.live = false; this.hello = null;
    this.release();
    if (was) this.changed();
  }
  release() {
    if (this.core) this.apply(this.core.releaseAll());
    this.o.controls.releaseSrc(SRC.MOVE);
  }

  open(u) {
    const t = this.makeTransport(u);
    this.t = t;
    this.deframer.reset();
    t.onmessage = (b) => { if (this.t === t) this.rx(b); };
    t.onclose = (why) => { if (this.t === t) this.lost(why); };
    this.timers.hello = this.T.setTimeout(() => { if (this.t === t && !this.live) this.lost(`no answer from ${hostOf(u)}`); }, HELLO_MS);
    Promise.resolve().then(() => t.connect()).then(() => {
      if (this.t !== t) { t.close(); return; }
      this.lastRx = this.now();
      this.send(helloMsg());
    }, (e) => { if (this.t === t) this.lost(e && e.message ? e.message : "no answer"); });
  }
  send(bytes) { if (this.t && this.t.send(bytes)) { this.stats.sent++; return true; } return false; }

  // the link went: let go, and try again while wanted
  lost(why) {
    const was = this.live, name = this.hello ? this.hello.name || "The Move" : "The Move";
    this.stop();
    if (was) this.o.toast(`${name}: link lost (${why}).`, "bad");
    if (!this.want) return;
    if (!this.everLive && this.retries >= 2) {
      this.want = false;
      this.set("bad", `${cap(why)}. Is Move Remote running on the Move, and is ${hostOf(this.url())} its address? Over USB-C it is ${MR.USB_ADDR}.`);
      return;
    }
    const ms = RETRY_MS[Math.min(this.retries, RETRY_MS.length - 1)];
    this.retries++;
    this.set("busy", `${cap(why)}: trying again in ${Math.round(ms / 1000)} s...`);
    this.timers.retry = this.T.setTimeout(() => { this.timers.retry = 0; if (this.want && !this.t) this.open(this.url()); }, ms);
  }

  // ---- the Move's messages
  rx(bytes) {
    this.lastRx = this.now();
    const msgs = this.deframer.push(bytes);
    if (!msgs) { this.lost("a message the page doesn't understand"); return; }
    for (const m of msgs) {
      if (!this.t) return;
      this.message(m.type, m.payload);
    }
  }
  message(type, p) {
    if (type === MR.D_HELLO) return this.onHello(parseHello(p));
    if (type === MR.D_BYE) return this.onBye(p.length ? p[0] : 0);
    if (type === MR.D_PONG) { this.stats.pongs++; return; }
    if (type === MR.D_STATUS) { if (p.length >= 32) this.status = { battery: p[4], jacks: p[6], ledPending: u16(p, 24) }; return; }
    if (type !== MR.D_INPUT || !this.live) return;
    const c = this.o.controls, now = this.now() >>> 0;
    for (let i = 0; i + 8 <= p.length; i += 8) {
      this.stats.inputs++;
      this.core.setMaster(c.master);
      this.apply(this.core.input(u32(p, i + 4), u32(p, i), now));
    }
    this.flushLeds();
    this.dirty = true;
    this.screenSoon();
  }
  onHello(hl) {
    if (!hl) return this.lost("not a Move Remote answer");
    if (hl.proto !== MR.PROTO) { this.want = false; this.stop(); this.set("bad", `The Move speaks Move Remote protocol ${hl.proto}; this page speaks ${MR.PROTO}. Update one of them.`); return; }
    this.T.clearTimeout(this.timers.hello); this.timers.hello = 0;
    this.hello = hl;
    const first = !this.live;
    this.live = true; this.everLive = true; this.retries = 0;
    this.send(configMsg(MR.CFG_AUDIO_CLOCK, 0));          // (no AUDIO every 6 ms: the page plays its own sound)
    this.core.ledsInvalidate();
    this.shown = null;
    const lv = this.o.site.leds();
    this.core.panel(lv.levels, lv.playGreen);
    this.core.setMaster(this.o.controls.master);
    this.flushLeds();
    const sc = this.o.site.screen();
    this.px = sc.pixels; this.dirty = true; this.lastShot = -1e9;
    this.screenSoon();
    this.T.clearInterval(this.timers.ping);
    this.timers.ping = this.T.setInterval(() => this.heartbeat(), PING_MS);
    this.set("on", `${hl.name || "Move"} connected${hl.via in MR.VIA ? " over " + MR.VIA[hl.via] : ""} (${this.t.name}): ${MODE_NAMES[this.core.state().mode]} mode. Track 1..4 pick the mode.`);
    if (first) { this.o.toast(`${hl.name || "The Move"} is the FM-1's controller now.`, "ok"); this.changed(); }
  }
  onBye(reason) {
    const name = this.hello ? this.hello.name || "The Move" : "The Move";
    if (reason === BYE.SHUTDOWN) { this.lost("the Move is powering off"); return; }
    this.want = false;
    this.stop();
    const why = reason === BYE.EXIT ? `${name} left Move Remote (its exit gesture).` : reason === BYE.REPLACED ? `Another host took ${name} over.`
      : reason === BYE.PROTOCOL ? `${name} closed the link: a protocol error.` : `${name} closed the link.`;
    this.set(reason === BYE.PROTOCOL ? "bad" : "off", why);
    this.o.toast(why);
  }
  heartbeat() {
    if (!this.live) return;
    if (this.now() - this.lastRx > SILENT_MS) { this.lost("the Move went quiet"); return; }
    this.stats.pings++;
    this.send(pingMsg(++this.token, this.now()));
  }

  // ---- the core's actions onto the page
  apply(acts) {
    const c = this.o.controls;
    for (const a of acts) {
      if (a.kind === "key") c.holdKey(a.k, SRC.MOVE, a.down, a.down ? a.vel : 0);
      else if (a.kind === "press") c.leanKey(a.k, lean(a.value));
      else if (a.kind === "btn") c.holdBtn(MATRIX_TO_LABEL[a.m], SRC.MOVE, a.down);
      else if (a.kind === "turn") c.turn(MATRIX_TO_ROLE[a.enc], a.steps);
      else if (a.kind === "master") this.o.mapAct({ kind: "master", value: a.value });
      else if (a.kind === "midi") this.o.site.midiIn(a.bytes);
      else if (a.kind === "settings") this.keep();
    }
  }
  keep() {
    if (!this.core) return;
    const v = toB64(this.core.save());
    if (v === this.o.prefs.move) return;
    this.o.prefs.move = v; this.o.save();
    if (this.live) this.set(this.tone, this.detail.replace(/: [^:]* mode\./, `: ${MODE_NAMES[this.core.state().mode]} mode.`));
  }

  // ---- what it shows
  onLeds(levels, green) {
    if (!this.core) return;
    this.core.panel(levels, green);
    this.flushLeds();
  }
  onInfo(info) {
    this.keymap = info && Array.isArray(info.keymap) ? info.keymap : null;
    if (!this.core) return;
    this.core.setKeymap(this.keymap);
    this.flushLeds();
  }
  flushLeds() {
    if (!this.live) return;
    for (let i = 0; i < 4; i++) {           // (rfm_move_leds gives every due one: one pass is enough, a few at most)
      const leds = this.core.leds();
      if (!leds.length) return;
      this.stats.ledWrites += leds.length;
      for (const m of ledMsgs(leds)) { this.stats.ledMsgs++; this.send(m); }
    }
  }
  // the firmware's knob labels (panel.js panelLabels().knobs: by role, 8, null = the printed name)
  setLabels(knobs) {
    this.labels = Array.isArray(knobs) ? knobs.slice(0, 8) : null;
    if (!this.core) return;
    this.applyLabels();
    this.dirty = true;
    this.screenSoon();
  }
  applyLabels() {
    for (let r = 0; r < 8; r++) this.core.setLabel(r === E.MASTER ? 7 : roleToMatrix(r), this.labels && this.labels[r] ? this.labels[r] : "");
  }
  // the screen, at most every SCREEN_MS, only when it changed
  screenSoon() {
    if (!this.live) return;
    this.dirty = true;
    if (this.timers.screen) return;
    const wait = SCREEN_MS - (this.now() - this.lastShot);
    if (wait <= 0) this.draw();
    else this.timers.screen = this.T.setTimeout(() => { this.timers.screen = 0; this.draw(); }, wait);
  }
  draw() {
    if (!this.live || !this.dirty) return;
    this.dirty = false;
    this.lastShot = this.now();
    const out = this.core.screen(this.px);
    this.px = null;                         // (the core keeps the last FM-1 screen)
    if (this.shown && this.shown.length === out.length && this.shown.every((b, i) => b === out[i])) return;
    this.shown = out;
    this.stats.screens++;
    this.send(frame(MR.H_DISPLAY, out));
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
  const go = () => { const v = link.view(); if (v.want && v.tone !== "bad") link.disconnect(); else link.connect(addr.value); };
  btn.addEventListener("click", go);
  addr.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); if (!link.view().want || link.view().tone === "bad") link.connect(addr.value); } });
  const show = (v) => {
    const on = v.want && v.tone !== "bad";
    btn.textContent = on ? "Disconnect" : v.tone === "bad" ? "Try again" : "Connect my Move";
    btn.className = "pill " + (on ? "" : "primary");
    addr.disabled = on;
    line.replaceChildren(h("span", { class: "fm1-dot " + v.tone + (v.tone === "busy" ? " live" : "") }), v.detail);
    line.className = "remote-status small " + (v.tone === "on" ? "ok" : v.tone === "bad" ? "bad" : "muted");
  };
  link.on(show);
  show(link.view());
  slot.replaceChildren(
    h("p", { class: "row-sub", style: { margin: "0 0 10px" } }, "An Ableton Move running Move Remote becomes this FM-1's controller: its pads play the keys (or MIDI), its buttons and knobs are the FM-1's, and the page lights its pads and draws its screen. Track 1..4: FM-1, MIDI, Control, Sequencer."),
    h("div", { class: "move-row" }, addr, usb),
    h("div", { class: "pills two" }, btn),
    line,
    h("p", { class: "note" }, `Wi-Fi: move.local (or the Move's address); USB-C: ${MR.USB_ADDR}. The page must be on http (web/serve.sh) or localhost: browsers block ws:// from an https page. Shift + touch the volume knob + click the jog wheel on the Move to leave Move Remote.`));
}
