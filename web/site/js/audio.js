// SPDX-License-Identifier: GPL-3.0-only
// The page's side of the audio: one AudioContext, the "fm1-host" AudioWorkletNode (worklet/host.js), the engines
// added to its worklet, and the files each engine needs, fetched here (an engine can't fetch: API.md section 1).
//
//   const p = new Player(catalog, events);   events: { frame, leds, info, flash, halted, midiOut, error, status }
//   await p.start();                         // in a user gesture (the autoplay policy)
//   await p.load(entry, "native" | "emu" | "mock", flashBytes | null)
//   p.panel(msg); p.midi(bytes); p.flashReq();
//
// The emulated engine's packages come from where the Hub points, at play time (thefirmwarehub.com's mirror: CORS *;
// GitHub release downloads send no CORS header). W2's web/engines/emu/catalog.mjs is the hook: its fetchPackage()
// fetches and checks a package; a hook may also export engineFiles({ entry, fetch, paths }) to take the whole job.
// Without it the Hub's package URL from catalog.json is fetched and checked here. RemoteFM1's own package (emu.from
// "site") is the one exception: a path relative to the page, the installer's firmware/ folder on this same site.

const abs = (p) => new URL(p, document.baseURI).href;

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return Array.from(new Uint8Array(d), (b) => b.toString(16).padStart(2, "0")).join("");
}
async function fetchBytes(url, what) {
  let r;
  try { r = await fetch(url); } catch (e) { throw new Error(`${what} could not be downloaded (${e.message || e}).`); }
  if (!r.ok) throw new Error(`${what} could not be downloaded (HTTP ${r.status}).`);
  return new Uint8Array(await r.arrayBuffer());
}

export class Player {
  constructor(catalog, events = {}) {
    this.catalog = catalog;
    this.ev = events;
    this.ctx = null; this.node = null;
    this.added = new Map();      // engine name -> Promise (its module added to the worklet)
    this.hook = undefined;       // W2's catalog.mjs (null: none)
    this.current = null;         // { entry, engine }
  }
  emit(name, ...a) { if (this.ev[name]) this.ev[name](...a); }
  get running() { return !!this.ctx && this.ctx.state === "running"; }

  // the context and the host node; resolves once audio runs (call it from a user gesture; resolves false if the
  // browser still holds the audio back)
  async start() {
    if (!this.ctx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC || !("audioWorklet" in AC.prototype)) throw new Error("This browser has no AudioWorklet: use a current Chrome, Edge, Firefox or Safari.");
      this.ctx = new AC({ latencyHint: "interactive" });
      await this.ctx.audioWorklet.addModule(abs("worklet/host.js"));
      this.node = new AudioWorkletNode(this.ctx, "fm1-host", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
      this.node.port.onmessage = (e) => this.onMessage(e.data);
      this.node.connect(this.ctx.destination);
    }
    if (this.ctx.state !== "running") { try { await this.ctx.resume(); } catch (_) { /* (no gesture yet) */ } }
    return this.ctx.state === "running";
  }
  // a context that runs without a gesture (an embed in a page that already has one, a test browser): try quietly
  async tryAutostart() {
    try {
      const AC = window.AudioContext || window.webkitAudioContext;
      const probe = new AC();
      const ok = probe.state === "running";
      await probe.close();
      return ok ? this.start() : false;
    } catch (_) { return false; }
  }

  onMessage(m) {
    if (!m || !m.type) return;
    if (m.type === "frame") this.emit("frame", m.pixels, m.seq);
    else if (m.type === "leds") this.emit("leds", m.levels, m.playGreen);
    else if (m.type === "info") this.emit("info", m);
    else if (m.type === "flash") this.emit("flash", m);
    else if (m.type === "halted") this.emit("halted", m.code);
    else if (m.type === "midiOut") this.emit("midiOut", m.bytes);
    else if (m.type === "error") this.emit("error", m.message);
    else if (m.type === "load") this.emit("load", { ...m, glitches: this.glitches() });
  }

  // the browser's running count of audio glitches (frames the device got no audio for), where it has one: Chrome's
  // AudioContext.playoutStats; null elsewhere
  glitches() {
    try {
      const p = this.ctx && (this.ctx.playoutStats || this.ctx.playbackStats);
      const n = p && (p.fallbackFramesEvents ?? p.underrunEvents);
      return typeof n === "number" ? n : null;
    } catch (_) { return null; }
  }

  // add an engine's module to the worklet: a shim that imports it statically and registers it with host.js
  addEngine(name) {
    if (this.added.has(name)) return this.added.get(name);
    const spec = this.catalog.paths.engines[name];
    if (!spec) return Promise.reject(new Error(`no engine "${name}" in the catalog`));
    const src = `import * as E from ${JSON.stringify(abs(spec.module))};\n(globalThis.FM1_ENGINES = globalThis.FM1_ENGINES || {})[${JSON.stringify(name)}] = E;\n`;
    const url = URL.createObjectURL(new Blob([src], { type: "text/javascript" }));
    const p = this.ctx.audioWorklet.addModule(url).finally(() => URL.revokeObjectURL(url));
    this.added.set(name, p);
    p.catch(() => this.added.delete(name));
    return p;
  }

  async emuHook() {
    if (this.hook !== undefined) return this.hook;
    const spec = this.catalog.paths.engines.emu;
    try { this.hook = spec && spec.hook ? await import(abs(spec.hook)) : null; } catch (_) { this.hook = null; }
    return this.hook;
  }

  // the files an engine needs for a firmware (API.md section 1)
  async files(entry, engine) {
    const eng = this.catalog.paths.engines;
    if (engine === "mock") return {};
    if (engine === "native") {
      if (!entry.native) throw new Error(`${entry.name} has no native core.`);
      return { wasm: (await fetchBytes(abs(entry.native.wasm), `The native core of ${entry.name}`)).buffer };
    }
    if (engine === "emu") {
      const hook = await this.emuHook();
      // the hook may take the whole job ...
      if (hook && typeof hook.engineFiles === "function") return hook.engineFiles({ entry, fetch: (u) => fetch(u), paths: eng.emu, baseURI: document.baseURI });
      const wasm = (await fetchBytes(abs(eng.emu.wasm), "The emulator")).buffer;
      let fwsc = entry.localBytes ? entry.localBytes.slice() : null;
      if (!fwsc) {
        if (!entry.emu || !entry.emu.pkg) throw new Error(`${entry.name} has no package to emulate.`);
        // the Hub's mirror (an absolute URL), or this site's own package (RemoteFM1's, from: "site", relative to the page)
        const own = entry.emu.from === "site", url = abs(entry.emu.pkg), where = own ? "this site" : "the FM-1 Firmware Hub";
        if (hook && typeof hook.fetchPackage === "function") {
          // W2's web/engines/emu/catalog.mjs: fetched and checked against the catalog's SHA-256
          try { fwsc = (await hook.fetchPackage({ id: entry.id, name: entry.name, byo: false, package: { url, sha256: entry.emu.sha256 } })).bytes; }
          catch (e) { throw new Error(`The package of ${entry.name} could not be downloaded from ${where} (${e.message || e}).`); }
        } else {
          fwsc = await fetchBytes(url, `The package of ${entry.name}`);
          if (entry.emu.sha256 && (await sha256hex(fwsc)) !== entry.emu.sha256)
            throw new Error(`The package of ${entry.name} is not the one ${own ? "the catalog" : "the Hub"} lists (its SHA-256 differs).`);
        }
      }
      return { wasm, fwsc };
    }
    throw new Error(`unknown engine ${engine}`);
  }

  // load a firmware on an engine; resolves once the worklet has it (info arrives as an event)
  // extra: engine options (the emu engine's mhz)
  async load(entry, engine, flash = null, extra = {}) {
    if (!this.ctx) throw new Error("start() first");
    this.emit("status", { state: "loading", entry, engine });
    await this.addEngine(engine);
    const files = await this.files(entry, engine);
    this.current = { entry, engine };
    this.node.port.postMessage({ type: "load", engine, files, flash: flash ? new Uint8Array(flash) : null,
                                 options: { id: entry.id, name: entry.name, version: entry.version, author: entry.authorFull || entry.author,
                                            license: entry.license || null, source: entry.source || null, family: entry.category, ...extra } });   // (copied, not transferred: a hook may cache its buffers)
  }
  unload() { if (this.node) this.node.port.postMessage({ type: "unload" }); this.current = null; }
  panel(p) { if (this.node) this.node.port.postMessage({ type: "panel", ...p }); }
  midi(bytes) { if (this.node) this.node.port.postMessage({ type: "midi", bytes: Uint8Array.from(bytes) }); }
  flashReq() { if (this.node) this.node.port.postMessage({ type: "flashReq" }); }
}
