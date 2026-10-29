// SPDX-License-Identifier: GPL-3.0-only
// The page's side of the audio: one AudioContext, the "fm1-host" AudioWorkletNode (worklet/host.js), the engines
// added to its worklet, and the files each engine needs, fetched here (an engine can't fetch: API.md section 1).
//
//   const p = new Player(catalog, events);   events: { frame, leds, info, flash, halted, midiOut, error, status }
//   await p.start();                         // in a user gesture (the autoplay policy); p.unlock() is its synchronous part
//   await p.setSink(id)                       the output device: "" the system's, else an enumerateDevices() id (sinkSupport())
//   await p.load(entry, "native" | "emu" | "rfm1" | "mock", flashBytes | null)
//   p.panel(msg); p.midi(bytes); p.flashReq();
//   p.setVolume(0..1)                         the page's volume: a GainNode after the worklet (recordings are taken before it)
//   p.recStart({ id, chunkFrames, maxFrames }); p.recStop(); p.recBuf(l, r)   the recorder (js/rec-ui.js); events: rec(m)
//
// The emulated engine's packages come from where the Hub points, at play time (thefirmwarehub.com's mirror: CORS *;
// GitHub release downloads send no CORS header). W2's web/engines/emu/catalog.mjs is the hook: its fetchPackage()
// fetches and checks a package; a hook may also export engineFiles({ entry, fetch, paths }) to take the whole job.
// Without it the Hub's package URL from catalog.json is fetched and checked here. RemoteFM1's own package (emu.from
// "site") is the one exception: a path relative to the page, the installer's firmware/ folder on this same site.
// The rfm1 engine runs the visitor's own .rfm1 files (their bytes, from the page) and this site's (FIZZ: rfm1.file,
// fetched from fw/ and checked against the catalog's SHA-256).

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

// iPhone, iPad, iPod touch (every browser there is WebKit; an iPad asks for the desktop page as a Mac with touch)
export const isIOS = () => typeof navigator !== "undefined" &&
  (/iP(hone|ad|od)/.test(navigator.userAgent || "") || (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1));

// iOS plays Web Audio as "ambient" sound, which the ring/silent switch mutes. "playback" is what a music app uses: it
// plays with the switch on silent (navigator.audioSession, Safari 16.4+). Without it, a silent <audio> kept playing
// does the same (a playing media element moves the page's audio to playback).
function playbackSession() {
  try { if (navigator.audioSession && navigator.audioSession.type !== "playback") navigator.audioSession.type = "playback"; } catch (_) { /* (not settable here) */ }
}
let silentUrl = null;
function silentWav() {   // 0.5 s of 8 kHz 8-bit mono silence
  if (silentUrl) return silentUrl;
  const n = 4000, b = new ArrayBuffer(44 + n), v = new DataView(b);
  const str = (o, t) => { for (let i = 0; i < t.length; i++) v.setUint8(o + i, t.charCodeAt(i)); };
  str(0, "RIFF"); v.setUint32(4, 36 + n, true); str(8, "WAVEfmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, 8000, true); v.setUint32(28, 8000, true); v.setUint16(32, 1, true); v.setUint16(34, 8, true); str(36, "data"); v.setUint32(40, n, true);
  new Uint8Array(b, 44).fill(128);
  return (silentUrl = URL.createObjectURL(new Blob([b], { type: "audio/wav" })));
}

// how this browser can pick the page's output device: "context" (AudioContext.setSinkId: Chrome, Edge 110+),
// "element" (an <audio> element's setSinkId with selectAudioOutput(): Firefox), or null (Safari: the system's output)
export function sinkSupport() {
  if (typeof window === "undefined") return null;
  const AC = window.AudioContext;
  if (AC && "setSinkId" in AC.prototype) return "context";
  if (AC && "createMediaStreamDestination" in AC.prototype && typeof HTMLMediaElement !== "undefined" && "setSinkId" in HTMLMediaElement.prototype
      && navigator.mediaDevices && typeof navigator.mediaDevices.selectAudioOutput === "function") return "element";
  return null;
}

export class Player {
  constructor(catalog, events = {}) {
    this.catalog = catalog;
    this.ev = events;
    this.ctx = null; this.node = null; this.out = null; this.volume = 1;
    this.setup = null;           // Promise: the worklet module and the host node
    this.sinkId = "";            // the output device asked for ("" the system's)
    this.applied = "";           // ... and the one the sound goes to
    this.tag = null;             // iOS without navigator.audioSession: the silent <audio> that keeps playback on
    this.route = null;           // { dest, el }: the "element" output route (Firefox)
    this.added = new Map();      // engine name -> Promise (its module added to the worklet)
    this.hook = undefined;       // W2's catalog.mjs (null: none)
    this.current = null;         // { entry, engine }
  }
  emit(name, ...a) { if (this.ev[name]) this.ev[name](...a); }
  get running() { return !!this.ctx && this.ctx.state === "running"; }

  // the part of start() that must run inside the user gesture, so synchronous: iOS Safari forgets the gesture at the
  // first await (and a touch's pointerdown is no gesture there at all). The audio session, the context, resume(),
  // and a one-sample sound started at once (what wakes iOS's output). Safe to call from any click, key or touch.
  unlock() {
    playbackSession();
    if (!this.ctx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC || !("audioWorklet" in AC.prototype)) throw new Error("This browser has no AudioWorklet: use a current Chrome, Edge, Firefox or Safari.");
      this.ctx = new AC({ latencyHint: "interactive" });
      this.ctx.onstatechange = () => this.emit("audioState", this.ctx.state);
    }
    if (this.ctx.state !== "running") {
      this.ctx.resume().catch(() => { /* (no gesture: a later one resumes) */ });
      try {
        const b = this.ctx.createBufferSource();
        b.buffer = this.ctx.createBuffer(1, 1, this.ctx.sampleRate);
        b.connect(this.ctx.destination); b.start(0);
      } catch (_) { /* (closed) */ }
    }
    if (isIOS() && !navigator.audioSession) {
      if (!this.tag) { this.tag = new Audio(silentWav()); this.tag.loop = true; this.tag.setAttribute("playsinline", ""); this.tag.setAttribute("x-webkit-airplay", "deny"); }
      if (this.tag.paused) this.tag.play().catch(() => { /* (no gesture yet) */ });
    }
  }

  // the context and the host node; resolves once audio runs (call it from a user gesture; resolves false if the
  // browser still holds the audio back)
  async start() {
    this.unlock();
    if (!this.setup) {
      this.setup = (async () => {
        await this.ctx.audioWorklet.addModule(abs("worklet/host.js"));
        this.node = new AudioWorkletNode(this.ctx, "fm1-host", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
        this.node.port.onmessage = (e) => this.onMessage(e.data);
        this.out = this.ctx.createGain();
        this.out.gain.value = this.volume;
        this.node.connect(this.out);
        this.out.connect(this.route && this.sinkId ? this.route.dest : this.ctx.destination);
      })();
      this.setup.catch(() => { this.setup = null; this.node = null; });
    }
    await this.setup;
    // (Chrome keeps resume() pending until a gesture: don't wait for it forever)
    if (this.ctx.state !== "running") await Promise.race([this.ctx.resume().catch(() => {}), new Promise((r) => setTimeout(r, 1500))]);
    return this.ctx.state === "running";
  }
  get audioState() { return this.ctx ? this.ctx.state : "none"; }

  // the output device: "" the system's output, else a deviceId from enumerateDevices() / selectAudioOutput().
  // Resolves once the sound goes there; throws when the browser refuses (the device gone, no permission).
  async setSink(id = "") {
    id = id || "";
    const how = sinkSupport();
    if (!how) { if (id) throw new Error("This browser plays through the system's sound output only."); return; }
    if (!this.ctx) { this.sinkId = id; return; }   // (start() comes first; applySink() then)
    if (this.applied === id) { this.sinkId = id; return; }
    if (how === "context") {
      if ((this.ctx.sinkId || "") !== id) await this.ctx.setSinkId(id);
    } else {
      // Firefox: the sound through a MediaStream into an <audio> element, whose output can be picked
      if (id && !this.route) {
        const dest = this.ctx.createMediaStreamDestination(), el = new Audio();
        el.srcObject = dest.stream;
        this.route = { dest, el };
      }
      if (this.route) {
        await this.route.el.setSinkId(id);
        if (this.out) {
          try { this.out.disconnect(); } catch (_) { /* (not connected) */ }
          this.out.connect(id ? this.route.dest : this.ctx.destination);
        }
        if (id) await this.route.el.play(); else this.route.el.pause();
      }
    }
    this.sinkId = this.applied = id;
  }
  // after start(): the device asked for before the context existed
  async applySink() { if (this.ctx && this.sinkId !== this.applied) await this.setSink(this.sinkId); }
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
    else if (m.type === "recChunk" || m.type === "recLevel") this.emit("rec", m);
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
      const wasm = (await fetchBytes(abs(entry.native.wasm), `The native core of ${entry.name}`)).buffer;
      // an author's own build (engines/native/ext/): its manifest's flags, which the engine checks against the module
      return entry.native.by === "author" ? { wasm, flags: { asyncify: !!entry.native.asyncify, abi: entry.native.abi || 6 } } : { wasm };
    }
    // an .rfm1 file the visitor opened (js/library.js): its bytes, which the page read from the file or this browser;
    // this site's own (rfm1.from "site"): fetched from the site and checked
    if (engine === "rfm1") {
      if (entry.localBytes) return { wasm: entry.localBytes.slice().buffer };
      if (!entry.rfm1 || !entry.rfm1.file) throw new Error(`${entry.name}'s .rfm1 file is not in this browser any more.`);
      const b = await fetchBytes(abs(entry.rfm1.file), `The .rfm1 file of ${entry.name}`);
      if (entry.rfm1.sha256 && (await sha256hex(b)) !== entry.rfm1.sha256)
        throw new Error(`The .rfm1 file of ${entry.name} is not the one the catalog lists (its SHA-256 differs): reload the page.`);
      return { wasm: b.buffer };
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
    this.emit("beforeLoad");   // (the recorder ends a take: the engine is about to be replaced)
    this.emit("status", { state: "loading", entry, engine });
    await this.addEngine(engine);
    const files = await this.files(entry, engine);
    this.current = { entry, engine };
    this.node.port.postMessage({ type: "load", engine, files, flash: flash ? new Uint8Array(flash) : null,
                                 options: { id: entry.id, name: entry.name, version: entry.version, author: entry.authorFull || entry.author,
                                            license: entry.license || null, source: entry.source || null, family: entry.category, ...extra } });   // (copied, not transferred: a hook may cache its buffers)
  }
  unload() { this.emit("beforeLoad"); if (this.node) this.node.port.postMessage({ type: "unload" }); this.current = null; }
  setVolume(v) {
    this.volume = Math.max(0, Math.min(1, +v || 0));
    if (this.out) this.out.gain.setTargetAtTime(this.volume, this.ctx.currentTime, 0.015);
  }
  get sampleRate() { return this.ctx ? this.ctx.sampleRate : 0; }
  recStart(o) { if (this.node) this.node.port.postMessage({ type: "rec", on: true, ...o }); }
  recStop() { if (this.node) this.node.port.postMessage({ type: "rec", on: false }); }
  recBuf(l, r) { if (this.node && l && l.buffer.byteLength) this.node.port.postMessage({ type: "recBuf", l, r }, [l.buffer, r.buffer]); }
  panel(p) { if (this.node) this.node.port.postMessage({ type: "panel", ...p }); }
  midi(bytes) { if (this.node) this.node.port.postMessage({ type: "midi", bytes: Uint8Array.from(bytes) }); }
  flashReq() { if (this.node) this.node.port.postMessage({ type: "flashReq" }); }
}
