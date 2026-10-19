// SPDX-License-Identifier: GPL-3.0-only
// The FM-1 Player's Web MIDI shim for a hosted manager (web/managers/build.mjs puts it first in the page's <head>).
//
// It replaces navigator.requestMIDIAccess with a fake MIDIAccess: one input and one output named exactly as the running
// firmware's USB port ("ChoralRoot FM-1", "Felucca", "Melodee", "JIANT FM" ...), sysexEnabled, wired to the firmware
// that runs in the player page instead of a USB device:
//   - in the player's Editor panel (an iframe of the same origin): window.postMessage to the parent;
//   - in its own tab ("pop out"): a BroadcastChannel ("remotefm1-managers") to the player tab.
// The player (web/site/js/managers.js ManagerBridge) feeds what the page sends into the engine's MIDI in, and the
// firmware's MIDI out (whole messages: SysEx replies, its notes) back here. The page never hears its own requests.
//
// Messages, all { __fm1mgr: 1, type, sid (this page's session), pid (the player's, once bound), ... }:
//   shim -> player   hello { manager }      who is there? (the player answers with state)
//                    midi { bytes }          one message the page sent (Uint8Array)
//                    bye                     the page goes away
//   player -> shim   state { connected, port, firmware, engine }   the port to show (connected false: none)
//                    midi { bytes }          one message from the firmware
//                    bye                     the player tab goes away
// The ports follow the player: a firmware switch, a reboot or the emulated engine take the port away (statechange
// "disconnected") and bring the new one (a new id, "connected"), as a real FM-1 does on USB.
//
// Also: links that leave the manager's folder (its installer, its home page) open on the manager's own site in a new
// tab, and images that fail to load (art its site build makes) are hidden.
(function (G) {
  "use strict";
  if (G.__fm1shim) return;
  const doc = G.document;
  const me = doc && doc.currentScript;
  const MANAGER = (me && me.dataset && me.dataset.manager) || "";
  const UPSTREAM = (me && me.dataset && me.dataset.upstream) || "";
  const CHANNEL = "remotefm1-managers";
  const HELLO_WAIT_MS = 700;
  const rnd = () => Math.random().toString(36).slice(2, 10);
  const sid = rnd();
  const now = () => (G.performance && G.performance.now ? G.performance.now() : Date.now());
  const sameOriginParent = (() => {
    try { return !!G.parent && G.parent !== G && G.parent.location.origin === G.location.origin; } catch (_) { return false; }
  })();

  // ---- the transport: the parent frame, or a BroadcastChannel
  let pid = null, bc = null, onMsg = null;
  function post(m) {
    const msg = Object.assign({ __fm1mgr: 1, sid, pid, manager: MANAGER }, m);
    if (sameOriginParent) G.parent.postMessage(msg, G.location.origin);
    else if (bc) bc.postMessage(msg);
  }
  if (sameOriginParent) {
    G.addEventListener("message", (e) => { if (e.source === G.parent && e.origin === G.location.origin) receive(e.data); });
  } else if (typeof G.BroadcastChannel === "function") {
    bc = new G.BroadcastChannel(CHANNEL);
    bc.onmessage = (e) => receive(e.data);
  }
  function receive(m) {
    if (!m || m.__fm1mgr !== 1 || (m.sid !== sid && m.sid !== "*") || m.from !== "player") return;
    if (bc) {                                        // (a tab: the first player that shows a port is ours)
      if (pid && m.pid !== pid) return;
      if (!pid) { if (m.type !== "state" || !m.connected) { if (m.type === "state") pending(m); return; } pid = m.pid; }
    }
    if (m.type === "state") setState(m);
    else if (m.type === "midi") deliver(m.bytes);
    else if (m.type === "bye") { pid = null; setState({ connected: false }); if (bc) post({ type: "hello" }); }
  }

  // ---- events: MIDIMessageEvent / MIDIConnectionEvent where the browser has them
  function messageEvent(data) {
    if (typeof G.MIDIMessageEvent === "function") { try { return new G.MIDIMessageEvent("midimessage", { data }); } catch (_) { /* below */ } }
    if (typeof G.MessageEvent === "function") return new G.MessageEvent("midimessage", { data });
    const e = new G.Event("midimessage"); Object.defineProperty(e, "data", { value: data }); return e;
  }
  function connectionEvent(port) {
    if (typeof G.MIDIConnectionEvent === "function") { try { return new G.MIDIConnectionEvent("statechange", { port }); } catch (_) { /* below */ } }
    const e = new G.Event("statechange"); Object.defineProperty(e, "port", { value: port }); return e;
  }
  const invalidState = (text) => (typeof G.DOMException === "function" ? new G.DOMException(text, "InvalidStateError") : new Error(text));

  // ---- the ports
  const accesses = new Set();
  class Port extends G.EventTarget {
    constructor(type, name, id) {
      super();
      this._type = type; this._name = name; this._id = id; this._state = "connected"; this._conn = "closed"; this._onstate = null;
      this.addEventListener("statechange", (e) => { if (typeof this._onstate === "function") this._onstate.call(this, e); });
    }
    get id() { return this._id; }
    get name() { return this._name; }
    get manufacturer() { return "FM-1 Player"; }
    get version() { return ""; }
    get type() { return this._type; }
    get state() { return this._state; }
    get connection() { return this._conn; }
    get onstatechange() { return this._onstate; }
    set onstatechange(f) { this._onstate = f; }
    open() {
      if (this._state === "connected" && this._conn !== "open") { this._conn = "open"; changed(this); }
      else if (this._state === "disconnected") this._conn = "pending";
      return Promise.resolve(this);
    }
    close() {
      if (this._conn !== "closed") { this._conn = "closed"; changed(this); }
      return Promise.resolve(this);
    }
  }
  class Input extends Port {
    constructor(name, id) {
      super("input", name, id);
      this._onmsg = null;
      super.addEventListener("midimessage", (e) => { if (typeof this._onmsg === "function") this._onmsg.call(this, e); });
    }
    get onmidimessage() { return this._onmsg; }
    set onmidimessage(f) { this._onmsg = f; if (f) this.open(); }    // (Web MIDI: setting it opens the port)
    addEventListener(type, f, o) { super.addEventListener(type, f, o); if (type === "midimessage") this.open(); }
  }
  class Output extends Port {
    constructor(name, id) { super("output", name, id); }
    send(data, timestamp) {
      if (this._state === "disconnected") throw invalidState(`the port "${this._name}" is not connected`);
      const bytes = Uint8Array.from(data || [], (v) => {
        if (!Number.isInteger(v) || v < 0 || v > 255) throw new TypeError("MIDIOutput.send: bytes must be integers 0..255");
        return v;
      });
      if (!bytes.length) return;
      if (this._conn !== "open") this.open();
      const delay = typeof timestamp === "number" && timestamp > 0 ? timestamp - now() : 0;
      const go = () => { if (this._state === "connected") post({ type: "midi", bytes }); };
      if (delay > 1) setTimeout(go, delay); else go();
    }
    clear() {}
  }
  const inputs = new Map(), outputs = new Map();
  class Access extends G.EventTarget {
    constructor() {
      super();
      this._onstate = null;
      this.addEventListener("statechange", (e) => { if (typeof this._onstate === "function") this._onstate.call(this, e); });
    }
    get inputs() { return inputs; }
    get outputs() { return outputs; }
    get sysexEnabled() { return true; }
    get onstatechange() { return this._onstate; }
    set onstatechange(f) { this._onstate = f; }
  }
  function changed(port) {
    port.dispatchEvent(connectionEvent(port));
    for (const a of accesses) a.dispatchEvent(connectionEvent(port));
  }

  // ---- the player's state -> the ports
  let pair = null, gen = 0, state = { connected: false }, firstState = null;
  const waiters = [];
  // (an unbound tab heard a player without a port: requestMIDIAccess still waits, until HELLO_WAIT_MS, for one with)
  function pending(m) { if (!firstState) firstState = m; }
  function settle() { while (waiters.length) waiters.shift()(); }
  function setState(m) {
    state = { connected: !!m.connected, port: m.port || null, firmware: m.firmware || null, engine: m.engine || null };
    firstState = firstState || m;
    const want = state.connected && state.port ? state.port : null;
    if (pair && (pair.name !== want)) {
      const old = pair; pair = null;
      for (const p of [old.input, old.output]) { p._state = "disconnected"; if (p._conn === "open") p._conn = "pending"; }
      inputs.delete(old.input.id); outputs.delete(old.output.id);
      changed(old.input); changed(old.output);
    }
    if (want && !pair) {
      gen++;
      pair = { name: want, input: new Input(want, `fm1player-${sid}-in-${gen}`), output: new Output(want, `fm1player-${sid}-out-${gen}`) };
      inputs.set(pair.input.id, pair.input); outputs.set(pair.output.id, pair.output);
      changed(pair.input); changed(pair.output);
    }
    settle();
  }
  function deliver(bytes) {
    if (!pair || !bytes || !bytes.length) return;
    const inp = pair.input;
    if (inp._conn !== "open") return;                // (a closed input hears nothing, as in Web MIDI)
    inp.dispatchEvent(messageEvent(bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes)));
  }

  // ---- navigator.requestMIDIAccess: resolves once the player answered (or after HELLO_WAIT_MS: no port yet)
  function requestMIDIAccess(/* options: sysex is always granted */) {
    const a = new Access();
    accesses.add(a);
    if (firstState) return Promise.resolve(a);
    return new Promise((resolve) => {
      waiters.push(() => resolve(a));
      setTimeout(settle, HELLO_WAIT_MS);
    });
  }
  const nav = G.navigator;
  if (nav) Object.defineProperty(nav, "requestMIDIAccess", { value: requestMIDIAccess, configurable: true, writable: true });
  post({ type: "hello" });
  if (G.addEventListener) G.addEventListener("pagehide", () => post({ type: "bye" }));

  // ---- links out of the manager's folder: on its own site; images that do not load: hidden
  if (doc && doc.addEventListener) {
    const folder = new URL(".", G.location.href);
    doc.addEventListener("click", (e) => {
      const a = e.target && e.target.closest ? e.target.closest("a[href]") : null;
      if (!a || !UPSTREAM) return;
      const raw = a.getAttribute("href");
      if (!raw || raw.startsWith("#") || /^(javascript|blob|data|mailto):/i.test(raw)) return;
      const u = new URL(raw, G.location.href);
      if (u.origin !== G.location.origin || u.pathname.startsWith(folder.pathname)) return;
      e.preventDefault();
      G.open(new URL(raw, UPSTREAM).href, "_blank", "noopener");
    }, true);
    doc.addEventListener("error", (e) => { const t = e.target; if (t && t.tagName === "IMG") t.style.visibility = "hidden"; }, true);
  }

  // (tests, and the player's diagnostics)
  G.__fm1shim = { sid, get pid() { return pid; }, get state() { return state; }, get pair() { return pair; }, manager: MANAGER, framed: sameOriginParent };
})(typeof window !== "undefined" ? window : globalThis);
