// SPDX-License-Identifier: GPL-3.0-only
// The SMK-37 emulator as the player's unit (docs/SMK37.md "Emulator"), only with ?unit=smk37-emu in the page's URL:
// a navigator for RemoteFM1 (remote.js opts.navigator) whose Web MIDI has the browser's own ports (when this site
// already has MIDI permission; nothing is asked) and one more pair, "FM-1 (SMK-37 emulator)", carried over a
// BroadcastChannel to the emulator's tab (web/emu-smk37/smk.js, published beside the SMK-37 test installer). The pair
// is connected while the emulator says "here" (twice a second), so RemoteFM1 finds it as it finds a unit plugged in.
// Whole messages both ways.
export const EMU_CHANNEL = "remotefm1-smk37-emu";
export const EMU_PORT_NAME = "FM-1 (SMK-37 emulator)";
const GONE_MS = 2000;

export function emuNavigator(loc = globalThis.location, nav = globalThis.navigator) {
  let flag = null;
  try { flag = loc ? new URLSearchParams(loc.search).get("unit") : null; } catch (_) { flag = null; }
  if (flag !== "smk37-emu" || typeof BroadcastChannel === "undefined") return null;
  const chan = new BroadcastChannel(EMU_CHANNEL);
  const accesses = new Set();
  const now = () => (globalThis.performance ? performance.now() : Date.now());

  class Port {
    constructor(type) {
      Object.assign(this, { type, id: "smk37-emu-" + type, name: EMU_PORT_NAME, manufacturer: "RemoteFM1", version: "",
        state: "disconnected", connection: "closed", onmidimessage: null, onstatechange: null });
      this._ls = { midimessage: new Set(), statechange: new Set() };
    }
    addEventListener(t, f) { if (this._ls[t]) this._ls[t].add(f); if (t === "midimessage") this.connection = "open"; }
    removeEventListener(t, f) { if (this._ls[t]) this._ls[t].delete(f); }
    _emit(t, ev) {
      const on = this["on" + t];
      if (typeof on === "function") on.call(this, ev);
      for (const f of [...this._ls[t]]) f.call(this, ev);
    }
    open() { if (this.state === "connected") this.connection = "open"; return Promise.resolve(this); }
    close() { this.connection = "closed"; return Promise.resolve(this); }
  }
  const input = new Port("input"), output = new Port("output");
  output.send = (data) => {
    if (output.state !== "connected") throw new DOMException("the SMK-37 emulator is not there", "InvalidStateError");
    chan.postMessage({ from: "player", type: "midi", data: Uint8Array.from(data) });
  };
  output.clear = () => {};

  let seen = 0, timer = null;
  function setState(st) {
    if (input.state === st) return;
    for (const p of [input, output]) {
      p.state = st;
      p.connection = st === "connected" ? "open" : "closed";
    }
    for (const p of [input, output]) {
      const ev = { port: p, type: "statechange" };
      p._emit("statechange", ev);
      for (const a of accesses) a._emit(ev);
    }
  }
  chan.onmessage = (e) => {
    const m = e.data;
    if (!m || m.from !== "emu") return;
    if (m.type === "here") {
      if (input.state !== "connected") chan.postMessage({ from: "player", type: "here" });
      seen = now();
      setState("connected");
    } else if (m.type === "bye") {
      setState("disconnected");
    } else if (m.type === "midi" && m.data && input.state === "connected") {
      input._emit("midimessage", { data: new Uint8Array(m.data), timeStamp: now(), type: "midimessage" });
    }
  };
  timer = setInterval(() => {
    chan.postMessage({ from: "player", type: "here" });
    if (input.state === "connected" && now() - seen > GONE_MS) setState("disconnected");
  }, 500);
  chan.postMessage({ from: "player", type: "here" });

  // a port map: the real access's ports (live) and ours
  function portMap(real, ours) {
    const all = () => [...(real ? real.values() : []), ours];
    return {
      get size() { return all().length; },
      get: (id) => (id === ours.id ? ours : real ? real.get(id) : undefined),
      has: (id) => id === ours.id || (!!real && real.has(id)),
      values: () => all()[Symbol.iterator](),
      keys: () => all().map((p) => p.id)[Symbol.iterator](),
      entries: () => all().map((p) => [p.id, p])[Symbol.iterator](),
      forEach: (f) => all().forEach((p) => f(p, p.id)),
      [Symbol.iterator]() { return this.entries(); },
    };
  }
  function makeAccess(real) {
    const ls = new Set();
    const a = {
      inputs: portMap(real && real.inputs, input), outputs: portMap(real && real.outputs, output),
      sysexEnabled: true, onstatechange: null,
      addEventListener: (t, f) => { if (t === "statechange") ls.add(f); },
      removeEventListener: (t, f) => { if (t === "statechange") ls.delete(f); },
      _emit(ev) { if (typeof a.onstatechange === "function") a.onstatechange(ev); for (const f of [...ls]) f(ev); },
    };
    if (real && real.addEventListener) real.addEventListener("statechange", (ev) => a._emit(ev));
    accesses.add(a);
    return a;
  }
  async function realPermission() {
    try { return (await nav.permissions.query({ name: "midi", sysex: true })).state; } catch (_) { return "unknown"; }
  }
  return {
    async requestMIDIAccess(opts) {
      let real = null;
      if (nav && nav.requestMIDIAccess && (await realPermission()) === "granted") {
        try { real = await nav.requestMIDIAccess(opts); } catch (_) { real = null; }
      }
      return makeAccess(real);
    },
    // Web MIDI is always allowed for the emulator's pair (so RemoteFM1's autoConnect finds it without a click)
    permissions: {
      query: async (d) => (d && d.name === "midi" ? { state: "granted", addEventListener() {}, removeEventListener() {} }
        : nav && nav.permissions ? nav.permissions.query(d) : Promise.reject(new Error("no Permissions API"))),
    },
    stop() { clearInterval(timer); setState("disconnected"); chan.close(); },
  };
}
