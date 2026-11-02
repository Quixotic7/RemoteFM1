// SPDX-License-Identifier: GPL-3.0-only
// Live audio input in the page, for an .rfm1 cart with "ain1" in its manifest (web/engine/API.md section 6): the
// sheet's "Audio input" section while such a cart runs (Off / On, the input device, a level meter), and a prompt over
// the page as the RemoteFM1 folder's (js/folder-ui.js): "needs" (it can't do without) asks for it, "wants" offers it.
// Input starts only from a click (the browser's permission prompt), or by itself when this firmware had it on last
// time and the browser already allows the microphone. On / off and the device are kept per firmware in localStorage.
// Without getUserMedia or AudioWorklet input (or outside a secure context) the section and the prompt never show and
// the cart sees no input (state 0).
//   const ui = new AinUI({ getPlayer, ensureRunning, cart: () => ({ id, mode: "needs" | "wants" }) | null, toast, render });
//   ui.update()      after a firmware or its info changed (stops the input when the cart is gone)
//   ui.level(m)      the worklet's ainLevel
//   ui.section()     the sheet's rows ([] when no such cart runs)
import { h } from "./sheet.js";

const KEY = "remotefm1-ain";            // { fw: { [id]: { on, device } }, warned }
const store = () => { try { return typeof localStorage !== "undefined" ? localStorage : null; } catch (_) { return null; } };
function load() {
  try { const v = JSON.parse(store().getItem(KEY) || "{}"); return v && typeof v === "object" ? { fw: {}, ...v } : { fw: {} }; } catch (_) { return { fw: {} }; }
}
function save(v) { try { store().setItem(KEY, JSON.stringify(v)); } catch (_) { /* (not kept) */ } }

export function ainSupported() {
  if (typeof window === "undefined" || typeof navigator === "undefined") return false;
  if (window.isSecureContext === false) return false;
  const md = navigator.mediaDevices;
  return !!(md && typeof md.getUserMedia === "function") && typeof window.AudioWorkletNode === "function"
    && !!(window.AudioContext && "createMediaStreamSource" in window.AudioContext.prototype);
}

export class AinUI {
  constructor({ getPlayer, ensureRunning, cart, toast, render }) {
    this.getPlayer = getPlayer; this.ensureRunning = ensureRunning; this.cart = cart; this.toast = toast; this.render = render;
    this.supported = ainSupported();
    this.live = null;            // { id, deviceId, label, latency } while input is on
    this.busy = false;
    this.error = "";
    this.devices = [];
    this.tried = new Set();      // firmwares whose remembered "on" was tried this visit
    this.dismissed = new Set();
    this.peak = 0; this.told = 0;
    this.meterBar = h("i", {});
    this.meter = h("span", { class: "ain-meter", role: "meter", "aria-label": "Input level", "aria-valuemin": "0", "aria-valuemax": "1" }, this.meterBar);
    this.text = h("span", {});
    this.btn = h("button", { class: "pill primary", onclick: () => this.start() }, "Turn on");
    this.box = h("div", { class: "offer ain-ask", role: "alertdialog", "aria-label": "Audio input", hidden: true }, this.text,
      h("span", { class: "offer-btns" }, this.btn, h("button", { class: "pill ghost", onclick: () => { const c = this.cart(); if (c) this.dismissed.add(c.id); this.update(); } }, "Not now")));
    document.body.append(this.box);
    if (this.supported && navigator.mediaDevices.addEventListener)
      navigator.mediaDevices.addEventListener("devicechange", () => { if (this.live) this.listDevices(); });
  }
  pref(id) { const p = load().fw[id]; return p && typeof p === "object" ? p : { on: false, device: "" }; }
  setPref(id, p) { const v = load(); v.fw[id] = { ...this.pref(id), ...p }; save(v); }

  // after a firmware change or its info: the input stopped when its cart is gone; the prompt; a remembered "on"
  update() {
    const c = this.supported ? this.cart() : null;
    if (this.live && (!c || c.id !== this.live.id)) this.stop(false);
    if (c && !this.live && !this.busy && !this.tried.has(c.id) && this.pref(c.id).on) {
      this.tried.add(c.id);
      this.resume(c);
    }
    const show = !!c && !this.live && !this.busy && !this.dismissed.has(c.id);
    this.text.textContent = c && c.mode === "needs" ? "This firmware uses audio input" : "This firmware can use audio input";
    this.btn.textContent = c && c.mode === "needs" ? "Turn on input" : "Turn on";
    if (show && this.box.hidden) { this.box.hidden = false; requestAnimationFrame(() => this.box.classList.add("show")); }
    else if (!show && !this.box.hidden) { this.box.classList.remove("show"); this.box.hidden = true; }
  }
  // on again without a click only when the browser already allows the microphone (no prompt then)
  async resume(c) {
    let granted = false;
    try { granted = (await navigator.permissions.query({ name: "microphone" })).state === "granted"; } catch (_) { granted = false; }
    const now = this.cart();
    if (granted && now && now.id === c.id && !this.live) await this.start(true);
  }
  // in a click (or resume()): input on, with this firmware's device
  async start(quiet = false) {
    const c = this.cart();
    if (!c || this.busy || !this.supported) return;
    this.busy = true; this.error = ""; this.update(); this.render();
    try {
      if (this.ensureRunning) await this.ensureRunning();
      const p = this.getPlayer();
      if (!p) throw new Error("the audio has not started");
      const got = await p.ainStart(this.pref(c.id).device || "");
      const now = this.cart();
      if (!now || now.id !== c.id) { p.ainStop(); return; }       // (another firmware meanwhile)
      this.live = { id: c.id, ...got };
      this.setPref(c.id, { on: true, device: got.deviceId || this.pref(c.id).device || "" });
      this.dismissed.delete(c.id);
      await this.listDevices();
      const v = load();
      if (!v.warned) { v.warned = true; save(v); this.toast("Speakers and a microphone can feed back: headphones are best.", "", 6500); }
      else if (!quiet) this.toast(`Audio input on: ${got.label || "your input"}.`, "ok");
    } catch (e) {
      const why = e && e.name === "NotAllowedError" ? "the browser was not allowed to use it" : e && e.name === "NotFoundError" ? "no input device found" : (e && e.message) || String(e);
      this.error = why;
      if (!quiet) this.toast(`Audio input could not start (${why}).`, "bad");
    } finally {
      this.busy = false;
      this.update(); this.render();
    }
  }
  // input off (keep: the firmware's "on" kept for next time, as when it is switched away from)
  stop(byVisitor = true) {
    const id = this.live && this.live.id, p = this.getPlayer();
    if (p) p.ainStop();
    this.live = null; this.peak = 0; this.setLevel(0);
    if (byVisitor && id) { this.setPref(id, { on: false }); this.dismissed.add(id); }
    this.update(); this.render();
  }
  // the browser ended the track (unplugged, permission withdrawn)
  ended() { this.live = null; this.setLevel(0); this.error = "the input went away"; this.update(); this.render(); }
  async pickDevice(id) {
    const c = this.cart();
    if (!c) return;
    this.setPref(c.id, { device: id });
    if (this.live) { const p = this.getPlayer(); if (p) p.ainStop(); this.live = null; await this.start(true); }
  }
  async listDevices() {
    try { this.devices = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === "audioinput" && d.deviceId); }
    catch (_) { this.devices = []; }
    this.render();
  }
  level(m) {
    if (!this.live) return;
    this.peak = Math.max(m.peak || 0, this.peak * 0.8);       // (a meter that falls back, as a peak meter does)
    this.setLevel(this.peak);
    this.live.missing = m.missing || 0;
    if (this.live.told !== m.latency) { this.live.told = m.latency; this.render(); }
  }
  setLevel(pk) {
    const db = pk > 0 ? 20 * Math.log10(pk) : -90, w = Math.max(0, Math.min(1, (db + 60) / 60));
    this.meterBar.style.width = `${Math.round(w * 100)}%`;
    this.meterBar.classList.toggle("hot", pk >= 0.98);
    this.meter.setAttribute("aria-valuenow", pk.toFixed(2));
  }

  section() {
    const c = this.supported ? this.cart() : null;
    if (!c) return [];
    const on = !!this.live;
    const state = this.busy ? "starting..." : on ? "on" : c.mode === "needs" ? "off: it needs it" : "off";
    const out = [h("h3", { class: "heading" }, "Audio input", h("span", { class: "heading-val" }, state))];
    const seg = h("div", { class: "seg", role: "radiogroup", "aria-label": "Audio input" },
      ["Off", "On"].map((l, i) => h("button", { class: (i === 1) === on ? "on" : "", role: "radio", "aria-checked": String((i === 1) === on), disabled: this.busy,
        onclick: () => { if (i === 1 && !on) this.start(); else if (i === 0 && on) this.stop(true); } }, l)));
    const sub = this.error ? `not on: ${this.error}` : on ? "headphones are best: speakers can feed back" : "your microphone or audio interface, into the firmware";
    out.push(h("div", { class: "row" }, h("div", { class: "row-label" }, h("div", {}, "Input"), h("div", { class: "row-sub" }, sub)), seg));
    if (on) {
      const p = this.pref(c.id), cur = this.live.deviceId || p.device || "";
      const sel = h("select", { class: "select", "aria-label": "Input device", onchange: (ev) => this.pickDevice(ev.target.value) },
        this.devices.length ? this.devices.map((d, i) => h("option", { value: d.deviceId }, d.label || `Input ${i + 1}`)) : h("option", { value: "" }, this.live.label || "Default"));
      if (this.devices.some((d) => d.deviceId === cur)) sel.value = cur;
      const ms = this.live.told ? `${(this.live.told / 44.1).toFixed(1)} ms` : "";
      out.push(h("div", { class: "row" }, h("div", { class: "row-label" }, h("div", {}, "Device"), h("div", { class: "row-sub ain-level" }, this.meter,
        h("span", { title: "how long the input takes to reach the firmware (the browser's capture and this page's buffer)" }, ms))), sel));
    }
    return out;
  }
}
