// SPDX-License-Identifier: GPL-3.0-only
// For shots.mjs only (not published): injected before the page's own scripts (Page.addScriptToEvaluateOnNewDocument),
// it replaces navigator.requestMIDIAccess, navigator.usb and the 'midi' permission with web/remote/test/fakeworld.mjs
// around WASM fake units, so the player's Connect FM-1 can be shown in every state. No real MIDI or USB device is
// touched. window.__FM1_MOCK = { perm: 'granted' | 'prompt' | 'denied', units: [{ midiName, bulk, paired, silent,
// name, proto }] } comes first. Needs web/ served at the origin's root and web/remote/build.sh run.
(() => {
  const cfg = window.__FM1_MOCK || {};
  const ready = (async () => {
    const [{ createWorld }, { default: createFakeUnit }] = await Promise.all([
      import("/remote/test/fakeworld.mjs"), import("/build/remote/fakeunit.mjs")]);
    const w = await createWorld(createFakeUnit, { start: Math.floor(performance.now()) });
    w.midiPerm = cfg.perm || "prompt";
    for (const u of cfg.units || []) {
      const x = w.addUnit(u);
      if (u.paired && x.usb) w.permitted.add(x.usb);
    }
    setInterval(() => { const now = Math.floor(performance.now()); while (w.t < now) { w.t++; w.pump(); } }, 4);
    window.__fm1World = w;
    return w;
  })();
  const def = (o, k, v) => Object.defineProperty(o, k, { value: v, configurable: true });
  def(navigator, "requestMIDIAccess", async (o) => (await ready).navigator.requestMIDIAccess(o));
  const usb = new EventTarget();
  usb.getDevices = async () => (await ready).navigator.usb.getDevices();
  usb.requestDevice = async (o) => (await ready).navigator.usb.requestDevice(o);
  ready.then((w) => { for (const t of ["connect", "disconnect"]) w.navigator.usb.addEventListener(t, (e) => { const f = new Event(t); f.device = e.device; usb.dispatchEvent(f); }); });
  def(navigator, "usb", usb);
  const real = navigator.permissions;
  def(navigator, "permissions", { query: async (d) => (d && d.name === "midi" ? (await ready).navigator.permissions.query(d) : real.query(d)) });
})();
