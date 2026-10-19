// SPDX-License-Identifier: GPL-3.0-only
// The player's side of the hosted managers (web/managers/): the firmwares' own preset managers / editors, each with the
// Web MIDI shim (web/managers/fm1-shim.js) in place of a USB port.
//
//   const bridge = new ManagerBridge({ toEngine: (bytes) => player.midi(bytes) });
//   bridge.setTarget({ port: "Felucca", firmware: "Felucca 1.6.1", engine: "native" })   // or { port: null }: no port
//   bridge.fromEngine(bytes)        // the engine's MIDI out (worklet "midiOut"), any chunking
//
// Sessions: a manager in the Editor panel's iframe talks by window.postMessage (same origin, checked); a popped-out one
// in its own tab by the BroadcastChannel "remotefm1-managers". Every session gets the same port (the running
// firmware's USB name) and the firmware's whole messages (SysEx replies and pushes, its notes; MIDI clock and the other
// real-time bytes are dropped: a manager never needs them). A manager's bytes go to the engine only; nothing a manager
// sends is ever passed back to a manager (no echo, unlike a loopback bus).
export const CHANNEL = "remotefm1-managers";

// raw MIDI bytes in any chunking -> whole messages (a SysEx F0..F7 as one; running status expanded)
export class MidiAssembler {
  constructor({ maxSysex = 1 << 16 } = {}) { this.max = maxSysex; this.sx = null; this.status = 0; this.data = []; }
  push(bytes, emit) {
    for (const b0 of bytes) {
      const b = b0 & 0xff;
      if (b >= 0xf8) continue;                                   // real time: dropped (see above)
      if (b === 0xf0) { this.sx = [b]; this.status = 0; continue; }
      if (b === 0xf7) {
        if (this.sx) { this.sx.push(b); emit(Uint8Array.from(this.sx)); this.sx = null; }
        continue;
      }
      if (this.sx) {
        if (b < 0x80) { if (this.sx.length < this.max) this.sx.push(b); else this.sx = null; continue; }
        this.sx = null;                                          // (a status byte cut the SysEx off: dropped)
      }
      if (b >= 0x80) {
        this.status = b; this.data = [];
        if (b === 0xf6) { emit(Uint8Array.of(b)); this.status = 0; }
        else if (b >= 0xf4) this.status = 0;
        continue;
      }
      const st = this.status;
      if (!st) continue;
      this.data.push(b);
      const need = (st >= 0xc0 && st < 0xe0) || st === 0xf1 || st === 0xf3 ? 1 : 2;
      if (this.data.length < need) continue;
      emit(Uint8Array.of(st, ...this.data));
      this.data = [];
      if (st >= 0xf0) this.status = 0;
    }
  }
}

const isBytes = (b) => (b instanceof Uint8Array || Array.isArray(b)) && b.length > 0 && b.length <= 1 << 16;

export class ManagerBridge {
  // toEngine(bytes): one message into the firmware. win: the page's window (the iframe sessions); BC: BroadcastChannel
  constructor({ toEngine, win = typeof window !== "undefined" ? window : null, BC = typeof BroadcastChannel === "function" ? BroadcastChannel : null,
                pid = Math.random().toString(36).slice(2, 10), onChange = () => {} } = {}) {
    this.toEngine = toEngine;
    this.pid = pid;
    this.onChange = onChange;
    this.target = { connected: false, port: null, firmware: null, engine: null };
    this.sessions = new Map();          // sid -> { via: "frame" | "tab", manager, send(msg), seen }
    this.asm = new MidiAssembler();
    this.stats = { toEngine: 0, toManagers: 0, lastIn: null, lastOut: null };
    this.win = win;
    if (win && win.addEventListener) {
      this.onWindow = (e) => {
        const m = e.data;
        if (!m || m.__fm1mgr !== 1 || m.from === "player" || e.origin !== win.location.origin || !e.source) return;
        const src = e.source;
        this.receive(m, "frame", (msg) => { try { src.postMessage(msg, e.origin); } catch (_) { this.sessions.delete(m.sid); } });
      };
      win.addEventListener("message", this.onWindow);
    }
    if (BC) {
      this.bc = new BC(CHANNEL);
      this.bc.onmessage = (e) => {
        const m = e.data;
        if (!m || m.__fm1mgr !== 1 || m.from === "player" || (m.pid && m.pid !== this.pid)) return;
        this.receive(m, "tab", (msg) => this.bc.postMessage(msg));
      };
    }
  }

  msg(sid, type, extra = {}) { return { __fm1mgr: 1, from: "player", pid: this.pid, sid, type, ...extra }; }
  stateMsg(sid) { const t = this.target; return this.msg(sid, "state", { connected: t.connected, port: t.port, firmware: t.firmware, engine: t.engine }); }

  receive(m, via, send) {
    if (typeof m.sid !== "string") return;
    if (m.type === "hello") {
      this.sessions.set(m.sid, { via, manager: m.manager || "", send, seen: Date.now() });
      send(this.stateMsg(m.sid));
      this.onChange();
      return;
    }
    if (m.type === "bye") { if (this.sessions.delete(m.sid)) this.onChange(); return; }
    let s = this.sessions.get(m.sid);
    if (!s) { s = { via, manager: m.manager || "", send, seen: Date.now() }; this.sessions.set(m.sid, s); send(this.stateMsg(m.sid)); this.onChange(); }
    s.seen = Date.now();
    if (m.type === "midi") {
      // a manager's message: into the firmware, only while a port is shown (and never back to any manager)
      if (!this.target.connected || !isBytes(m.bytes)) return;
      const bytes = m.bytes instanceof Uint8Array ? m.bytes : Uint8Array.from(m.bytes);
      if (bytes.some((v) => v > 255)) return;
      this.stats.toEngine++; this.stats.lastIn = bytes;
      this.toEngine(bytes);
    }
  }

  // the port the managers see: { port, firmware, engine } (port null: none; the firmware is emulated, loading, gone)
  setTarget({ port = null, firmware = null, engine = null } = {}) {
    const t = { connected: !!port, port: port || null, firmware, engine };
    const o = this.target;
    if (o.connected === t.connected && o.port === t.port && o.firmware === t.firmware && o.engine === t.engine) return;
    this.target = t;
    if (!t.connected) this.asm = new MidiAssembler();       // (a half message of the old firmware goes with it)
    for (const [sid, s] of this.sessions) s.send(this.stateMsg(sid));
    this.onChange();
  }

  // the engine's MIDI out -> every session, as whole messages
  fromEngine(bytes) {
    if (!bytes || !bytes.length || !this.target.connected) return;
    this.asm.push(bytes, (m) => {
      this.stats.lastOut = m;
      for (const [sid, s] of this.sessions) { this.stats.toManagers++; s.send(this.msg(sid, "midi", { bytes: m })); }
    });
  }

  get count() { return this.sessions.size; }
  // a frame session whose window went away (the panel closed, the iframe navigated): forgotten
  forgetFrames() { for (const [sid, s] of this.sessions) if (s.via === "frame") this.sessions.delete(sid); this.onChange(); }

  dispose() {
    for (const [sid, s] of this.sessions) { try { s.send(this.msg(sid, "bye")); } catch (_) { /* (gone) */ } }
    this.sessions.clear();
    if (this.win && this.onWindow) this.win.removeEventListener("message", this.onWindow);
    if (this.bc) { try { this.bc.postMessage(this.msg("*", "bye")); } catch (_) {} this.bc.close(); }
  }
}

// which editor a firmware has: { kind: "hosted", manager } (on this site), { kind: "real", url } (its own site's, for
// an FM-1 on USB), or null. index: managers/index.json (null: none hosted here)
export function editorOf(entry, index) {
  if (!entry) return null;
  const m = index && index.managers ? index.managers.find((x) => x.firmwares.includes(entry.id)) : null;
  if (m) return { kind: "hosted", manager: m };
  if (entry.editor) return { kind: "real", url: entry.editor };
  return null;
}
// the port name the in-page port takes for a firmware (its real USB product string), or null
export const portOf = (entry, index) => (entry && index && index.ports ? index.ports[entry.id] || null : null);
