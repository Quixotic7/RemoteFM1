// SPDX-License-Identifier: GPL-3.0-only
// USE MY OMX-27: the OMX-27 in REMOTE mode as the FM-1's controller (js/omx27.js the wire, js/omx27-views.js the
// views). Web MIDI with SysEx: the port whose name says OMX; the MODE command puts it in REMOTE mode, and while it is
// there its keys, encoder and pots drive the page's panel (source SRC.OMX) and the page lights its 27 keys and draws
// its 128 x 32 screen. Connected once, it connects by itself on the next visit (MIDI allowed) and when it is plugged
// in. Disconnect (or the page closing) clears its LEDs and screen; the OMX can't be told to leave REMOTE mode: AUX +
// encoder hold on it does. While it is in REMOTE mode the page's MIDI keyboard input and MIDI learn skip its port.
import { h } from "./sheet.js";
import { OmxSession, omxPair, STATUS, W, H, pack } from "./omx27.js";
import { OmxViews, VIEW_NAMES, THEMES } from "./omx27-views.js";
import { SRC, E, ledIndexOf, roleToMatrix } from "./panel.js";

const OFF_MS = 50;   // 52 03 then 52 02 within this: still in REMOTE mode
const LEAVE = "To leave remote mode on the OMX-27, hold AUX and the encoder.";
const MATRIX_ROLE = [0, 1, 2, 3, 4, 5, 6].map((m) => [E.SELECT, E.ALGO, E.PRESETS, E.K1, E.K2, E.K3, E.K4].find((r) => roleToMatrix(r) === m));

export class OmxLink {
  // opts: { controls, site, prefs, save(), toast(text, kind, ms), mapAct(action) (MASTER and its pickup), nav }
  constructor(opts) {
    this.o = opts;
    this.nav = opts.nav || globalThis.navigator;
    this.views = new OmxViews(opts.prefs.omx);
    this.access = null; this.pair = null; this.session = null; this.want = false;
    this.tone = "off"; this.detail = "Not connected."; this.prompt = false;
    this.subs = new Set();
    this.timer = 0; this.answerTimer = 0; this.offTimer = 0; this.ownTurn = false; this.shown = "";
    this.live = false;                   // in REMOTE mode as the page sees it (52 03 + 52 02 at once: still)
    this.pots = new Map();                // pot -> its latest message, not taken yet
    this.canvas = null;
    this.onState = () => this.scan();
    this.onMsg = (e) => this.message(e.data);
    opts.site.on("leds", () => this.refresh());
    opts.site.on("info", () => this.refresh());
    addEventListener("pagehide", () => { if (this.session && this.session.active) this.session.clear(); });
  }
  on(cb) { this.subs.add(cb); return () => this.subs.delete(cb); }
  view() { return { tone: this.tone, detail: this.detail, want: this.want, active: this.live, prompt: this.prompt, port: this.pair ? this.pair.name : "" }; }
  set(tone, detail) { this.tone = tone; this.detail = detail; for (const cb of this.subs) cb(this.view()); }
  // a port the page's MIDI input leaves alone (REMOTE mode: its events are ours)
  owns(port) { return !!(this.live && this.pair && port && (port.id === this.pair.input.id || port.name === this.pair.input.name)); }

  // from a click (it may prompt for MIDI), or by itself where MIDI is already allowed (auto)
  async connect() {
    this.want = true;
    if (!this.o.prefs.omxAuto) { this.o.prefs.omxAuto = true; this.o.save(); }
    if (this.pair) { this.session.enter(); this.set("busy", `Asking ${this.pair.name} for remote mode...`); this.expectAnswer(); return; }
    if (!this.nav || !this.nav.requestMIDIAccess) { this.set("bad", "This browser has no Web MIDI (Chrome and Edge have it)."); return; }
    if (!this.access) {
      this.prompt = true;
      this.set("busy", "Allow MIDI (with SysEx) in the browser...");
      try { this.access = await this.nav.requestMIDIAccess({ sysex: true }); }
      catch (_) { this.access = null; }
      this.prompt = false;
      if (!this.want) return;
      if (!this.access) { this.want = false; this.set("bad", "MIDI access (with SysEx) was refused: allow it for this site and try again."); return; }
      this.access.addEventListener("statechange", this.onState);
    }
    this.scan();
  }
  async auto() {
    if (!this.o.prefs.omxAuto || !this.nav || !this.nav.requestMIDIAccess || !this.nav.permissions) return;
    let st = "prompt";
    try { st = (await this.nav.permissions.query({ name: "midi", sysex: true })).state; } catch (_) { return; }
    if (st === "granted" && !this.want) this.connect();
  }
  disconnect() {
    this.want = false;
    this.o.prefs.omxAuto = false; this.o.save();
    const was = this.live;
    if (this.session) this.session.clear();
    this.drop();
    this.set("off", was ? `Disconnected. The OMX-27 stays in remote mode: ${LEAVE.charAt(0).toLowerCase() + LEAVE.slice(1)}` : "Not connected.");
  }

  // the ports now: the OMX's pair attached, or let go when it went
  scan() {
    if (!this.want || !this.access) return;
    const p = omxPair(this.access);
    if (this.pair && (!p || p.input.id !== this.pair.input.id || p.output.id !== this.pair.output.id)) {
      const name = this.pair.name;
      this.drop();
      this.set("busy", `${name} unplugged: it connects again when it is back.`);
      if (!p) return;
    }
    if (!this.pair && p) this.attach(p);
    else if (!this.pair) this.set("busy", "Plug in your OMX-27: it connects when it appears.");
  }
  attach(p) {
    this.pair = p;
    p.input.addEventListener("midimessage", this.onMsg);
    for (const port of [p.input, p.output]) if (port.open) Promise.resolve(port.open()).catch(() => {});
    const now = () => performance.now();
    this.session = new OmxSession({ send: (b, d) => { try { p.output.send(b, d ? now() + d : undefined); } catch (_) { /* (gone) */ } }, now });
    this.shown = "";
    this.session.enter();
    this.set("busy", `Asking ${p.name} for remote mode...`);
    this.expectAnswer();
    clearInterval(this.timer);
    this.timer = setInterval(() => this.tick(), 10);
  }
  expectAnswer() {
    clearTimeout(this.answerTimer);
    this.answerTimer = setTimeout(() => {
      if (this.session && !this.session.active && this.want) this.set("bad", `${this.pair.name} did not answer: it needs the OMX-27 firmware with REMOTE mode. Connect asks again.`);
    }, 2500);
  }
  drop() {
    clearInterval(this.timer); clearTimeout(this.answerTimer); clearTimeout(this.offTimer); this.offTimer = 0; this.pots.clear();
    const was = this.live;
    this.live = false;
    this.release();
    if (this.pair) this.pair.input.removeEventListener("midimessage", this.onMsg);
    this.pair = null; this.session = null;
    if (!this.want && this.access) { this.access.removeEventListener("statechange", this.onState); this.access = null; }
    if (was) this.changed();
  }
  release() { this.apply(this.views.releaseAll()); this.o.controls.releaseSrc(SRC.OMX); }
  changed() { if (this.o.onActive) this.o.onActive(this.live); }

  // ---- the OMX's messages
  message(d) {
    if (!this.session) return;
    const m = this.session.feed(d);
    if (!m) return;
    const now = performance.now();
    if (m.type === "status") {
      if (m.s === STATUS.ON) {
        clearTimeout(this.answerTimer);
        this.shown = "";
        // (MODE while already in REMOTE mode: the OMX answers 52 03 and 52 02 within a few ms: still connected)
        clearTimeout(this.offTimer); this.offTimer = 0;
        this.set("on", `${this.pair.name} in remote mode: ${VIEW_NAMES[this.views.view]} view. ${LEAVE}`);
        if (!this.live) { this.live = true; this.o.toast(`${this.pair.name} in remote mode: its keys, knobs and screen play the FM-1.`, "ok"); this.changed(); }
        this.refresh();
      } else if (m.s === STATUS.OFF) {
        clearTimeout(this.offTimer);
        this.offTimer = setTimeout(() => {
          this.offTimer = 0;
          if (!this.session || this.session.active) return;
          this.live = false;
          this.release();
          this.set("busy", `${this.pair.name} left remote mode: pick REMOTE on it again, or Connect.`);
          this.o.toast(`${this.pair.name} left remote mode.`);
          this.changed();
        }, OFF_MS);
      }
      return;
    }
    if (!this.session.active) return;
    // (pots stream every 2-3 ms while they move: the latest of each, taken on the next tick)
    if (m.type === "pot") { this.pots.set(m.pot, m); return; }
    if (m.type === "key") this.apply(this.views.key(m.key, m.ev, now));
    else if (m.type === "enc") this.apply(this.views.enc(m.delta));
    this.refresh();
  }
  takePots() {
    if (!this.pots.size) return false;
    const now = performance.now();
    for (const m of this.pots.values()) this.apply(this.views.pot(m.pot, m.v, m.hires, now, this.o.controls.master));
    this.pots.clear();
    return true;
  }
  apply(acts) {
    const c = this.o.controls;
    for (const a of acts) {
      if (a.kind === "key") c.holdKey(a.k, SRC.OMX, a.down, a.down ? 100 : 0);
      else if (a.kind === "btn") c.holdBtn(a.b, SRC.OMX, a.down);
      else if (a.kind === "turn") { this.ownTurn = true; try { c.turn(a.role, a.steps); } finally { this.ownTurn = false; } }
      else if (a.kind === "select") c.select(a.role);
      else if (a.kind === "midi") this.o.site.midiIn(a.bytes);
      else if (a.kind === "master" || a.kind === "pickup") this.o.mapAct(a);
      else if (a.kind === "view") this.set(this.tone, `${this.pair ? this.pair.name : "The OMX-27"} in remote mode: ${VIEW_NAMES[a.view]} view. ${LEAVE}`);
      if (a.kind === "theme" || a.kind === "select" || a.kind === "btn" || a.kind === "view") this.keep();
    }
  }
  keep() {
    const v = this.views.saved();
    if (JSON.stringify(v) === JSON.stringify(this.o.prefs.omx)) return;
    this.o.prefs.omx = v; this.o.save();
  }
  // the page's panel turned a knob (not the OMX): the encoder turns that one next (enc: matrix detents)
  panelTurned(enc) {
    if (this.ownTurn || !enc) return;
    for (let m = 0; m < 7; m++) if (enc[m]) this.views.knobUsed(MATRIX_ROLE[m]);
  }

  // ---- what it shows
  refresh() {
    if (!this.session || !this.session.active) return;
    const { levels } = this.o.site.leds(), info = this.o.site.current().info;
    const idx = ledIndexOf(info && info.keymap ? info.keymap : null);
    this.session.setLeds(this.views.leds((k) => { const i = idx[14 + k]; return i >= 0 ? levels[i] : 0; }));
    this.draw();
    this.session.tick();
  }
  tick() {
    if (!this.session || !this.session.active) return;
    if (this.takePots()) return this.refresh();
    this.draw(); this.session.tick();
  }
  draw() {
    const s = this.views.screen(performance.now()), key = s.title + "|" + s.line + "|" + s.view;
    if (key === this.shown) return;
    this.shown = key;
    this.session.setFrame(render(this.canvas || (this.canvas = Object.assign(document.createElement("canvas"), { width: W, height: H })), s));
  }
}

// the screen: the view's name large, its context small, four marks for K1's views; 1-bit, packed for the OMX
export function render(cv, { title, line, view }) {
  const g = cv.getContext("2d", { willReadFrequently: true });
  g.fillStyle = "#000"; g.fillRect(0, 0, W, H);
  g.fillStyle = "#fff"; g.textBaseline = "alphabetic";
  for (let i = 0; i < 4; i++) { if (i === view) g.fillRect(104 + i * 6, 2, 4, 6); else g.fillRect(104 + i * 6, 7, 4, 1); }
  let px = 19;
  do { g.font = `700 ${px}px "Chakra Petch", "DM Sans", sans-serif`; } while (g.measureText(title).width > 100 && --px > 10);
  g.fillText(title, 1, 17);
  px = 11;
  do { g.font = `700 ${px}px "Chakra Petch", "DM Sans", sans-serif`; } while (g.measureText(line).width > 126 && --px > 7);
  g.fillText(line, 1, 30);
  const d = g.getImageData(0, 0, W, H).data, bits = new Uint8Array(W * H);
  for (let i = 0; i < bits.length; i++) bits[i] = d[i * 4] >= 128 ? 1 : 0;
  return pack(bits);
}

// ---- the sheet's section
export function mountOmx(slot, link) {
  const btn = h("button", { class: "pill primary" }, "Connect my OMX-27");
  const line = h("p", { class: "remote-status small" });
  btn.addEventListener("click", () => { const v = link.view(); if (v.want && v.tone !== "bad") link.disconnect(); else link.connect(); });
  const show = (v) => {
    btn.textContent = v.want && v.tone !== "bad" ? "Disconnect" : v.tone === "bad" ? "Try again" : "Connect my OMX-27";
    btn.className = "pill " + (v.want && v.tone !== "bad" ? "" : "primary");
    line.replaceChildren(h("span", { class: "fm1-dot " + v.tone + (v.tone === "busy" ? " live" : "") }), v.detail);
    line.className = "remote-status small " + (v.tone === "on" ? "ok" : v.tone === "bad" ? "bad" : "muted");
  };
  link.on(show);
  show(link.view());
  slot.replaceChildren(
    h("p", { class: "row-sub", style: { margin: "0 0 10px" } }, "An OMX-27 becomes this FM-1's controller: its keys, encoder and pots play it, and the page lights its keys and draws its screen. K1 picks the view (Base, Seq, Keyboard, Quick Keys), K2 the colours, K5 is MASTER."),
    h("div", { class: "pills two" }, btn),
    line,
    h("p", { class: "note" }, `Chrome or Edge, and the OMX-27 firmware with REMOTE mode. Once connected, it connects by itself when the page opens or the OMX-27 is plugged in. ${THEMES.length} colour themes on K2. ${LEAVE}`));
}
