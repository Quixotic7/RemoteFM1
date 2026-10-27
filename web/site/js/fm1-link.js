// SPDX-License-Identifier: GPL-3.0-only
// The page's one link to the user's FM-1: W4's RemoteFM1 (web/remote/remote.js) and what the page shows of it. The
// header's Connect FM-1 button, the start overlay's "Play on my FM-1" and the sheet's USE MY FM-1 all read view()
// and call connect() / disconnect() here, so they always agree. No DOM: web/site/test.mjs drives it.
//
//   tone  "off"   grey    not connected (also: looking quietly, by autoConnect(), for a unit to answer)
//         "busy"  amber   searching after a click, starting, reconnecting, stopping
//         "on"    green   connected; via "USB" (bulk) or "MIDI"
//         "bad"   red     an error, or nothing answered a click within searchMs; title says why; a click retries
//   prompt  true while the browser's MIDI permission prompt is pending (remote.js status.midiPrompt): amber "Allow
//           MIDI...", the hint in detail; red with Try again once it has waited promptMs (30 s)

const VIA = { bulk: "USB", midi: "MIDI" };
// RemoteFM1 0.2's Bluetooth MIDI: macOS and Windows name its port after what the FM-1 advertises, "FM-1 1F2D" (four
// hex digits of its own); the USB port is "FM-1" (or a name CoreMIDI kept from another firmware)
export const BLE_PORT_RE = /\bFM-1 [0-9A-F]{4}\b/;
const viaOf = (s) => s.transport === "midi" && s.ports && BLE_PORT_RE.test(s.ports.name || "") ? "Bluetooth" : VIA[s.transport] || "";
export const MIDI_PROMPT_TEXT = "Allow MIDI: your browser is asking (the prompt by the address bar, or the site-settings icon left of the URL).";
export const MIDI_LATE_TEXT = "Still waiting for MIDI to be allowed: look for the prompt by the address bar, or the site-settings icon left of the URL. Click to try again.";
export const MIDI_DENIED_TEXT = "MIDI is blocked for this site. To allow it again: click the site-settings icon left of the URL (Site settings), set MIDI devices to Allow, then reload the page.";

export function viewOf(s, notFound = false, promptLate = false) {
  const via = viaOf(s);
  const ver = s.unit && s.unit.fwVer ? `, RemoteFM1 ${s.unit.fwVer}` : "";
  const quietSearch = s.state === "searching" && s.quiet && !notFound;
  if (s.state === "connected")
    return { tone: "on", label: "FM-1 connected", via, live: true,
      title: `Your FM-1 is the panel${via ? " (over " + via + ")" : ""}${ver}. Click to disconnect.`,
      detail: `Connected${via ? " over " + via : ""}${ver}${s.fps ? ` · ${s.fps} fps` : ""}.` };
  if (s.midiPrompt)
    return promptLate
      ? { tone: "bad", label: "Try again", via: "", live: true, prompt: true, title: MIDI_LATE_TEXT, detail: MIDI_LATE_TEXT }
      : { tone: "busy", label: "Allow MIDI...", via: "", live: true, prompt: true, title: MIDI_PROMPT_TEXT, detail: MIDI_PROMPT_TEXT };
  if (s.state === "error" || notFound) {
    const why = notFound ? "No FM-1 running RemoteFM1 answered. Is it plugged in, switched on, showing REMOTE FM1 / WAITING FOR HOST, and no other app using it?"
      : errorText(s.why);
    return { tone: "bad", label: "Connect FM-1", via: "", live: notFound, title: `${why} Click to try again.`, detail: why };
  }
  if (quietSearch || s.state === "idle" || s.state === "stopped" || !s.state)
    return { tone: "off", label: "Connect FM-1", via: "", live: quietSearch,
      title: "Play this firmware from your own FM-1 (it needs RemoteFM1).",
      detail: quietSearch ? "An FM-1 is plugged in; waiting for RemoteFM1 to answer..." : "Not connected." };
  const busy = {
    searching: ["Connecting...", "Looking for an FM-1 running RemoteFM1..."],
    starting: ["Connecting...", "Found it: starting..."],
    lost: ["Reconnecting...", "Lost the FM-1: looking again..."],
    stopping: ["Disconnecting...", "Stopping..."],
  }[s.state] || ["Connecting...", s.state];
  return { tone: "busy", label: busy[0], via: "", live: true, title: busy[1], detail: busy[1] };
}

export function errorText(why) {
  const w = String(why || "");
  if (/blocked for this site/i.test(w)) return MIDI_DENIED_TEXT;
  if (/refused|denied|NotAllowed|Security/i.test(w))
    return "The browser blocked MIDI for this site: allow MIDI (the icon at the left of the address bar), then try again.";
  if (/no Web MIDI/i.test(w)) return "This browser has no Web MIDI: use Chrome or Edge on a computer.";
  return w ? w.charAt(0).toUpperCase() + w.slice(1) + (/[.!?]$/.test(w) ? "" : ".") : "Could not connect.";
}

export class Fm1Link {
  // remote: a RemoteFM1. opts: { searchMs: 10000 (a click's search with no answer turns red), promptMs: 30000 (the
  // browser's MIDI prompt unanswered: red, Try again), setTimer, clearTimer }
  constructor(remote, opts = {}) {
    this.remote = remote;
    this.searchMs = opts.searchMs ?? 10000;
    this.promptMs = opts.promptMs ?? 30000;
    this.promptLate = false;
    this._promptTimer = null;
    this._set = opts.setTimer || ((f, ms) => setTimeout(f, ms));
    this._clear = opts.clearTimer || ((t) => clearTimeout(t));
    this.notFound = false;
    this._timer = null;
    this._subs = new Set();
    this.status = remote.status || { state: "idle" };
    remote.onStatus((s) => this._status(s));
  }
  on(cb) { this._subs.add(cb); return () => this._subs.delete(cb); }
  view() { return viewOf(this.status, this.notFound, this.promptLate); }
  get connected() { return this.status.state === "connected"; }

  // at load: connect where the browser already allows it (remote.js autoConnect: no prompt)
  auto() { return typeof this.remote.autoConnect === "function" ? this.remote.autoConnect().catch(() => null) : Promise.resolve(null); }

  // from a click (call it synchronously in the handler: the WebUSB chooser needs the gesture)
  connect() {
    this.notFound = false;
    if (this.status.midiPrompt) { this.promptLate = false; this._armPrompt(); }   // Try again: the request goes out again
    this._arm();
    const p = this.remote.connect().catch((e) => { this._status({ ...this.remote.status, state: "error", why: e.message || String(e) }); });
    this._emit();
    return p;
  }
  disconnect() { this.notFound = false; this.promptLate = false; this._disarm(); this._disarmPrompt(); return this.remote.disconnect(); }

  // a click's search that finds nothing within searchMs: red, and the search goes on quietly (2 s HELLO)
  _arm() {
    this._disarm();
    this._timer = this._set(() => {
      this._timer = null;
      if (this.status.midiPrompt) { this._arm(); return; }   // (waiting on the browser's prompt, not on the FM-1)
      const st = this.remote.status.state;
      if (st === "searching" || st === "lost") {
        this.notFound = true;
        if (this.remote.setQuiet) this.remote.setQuiet(true);
        this._emit();
      }
    }, this.searchMs);
  }
  _disarm() { if (this._timer !== null) { this._clear(this._timer); this._timer = null; } }
  _armPrompt() {
    this._disarmPrompt();
    this._promptTimer = this._set(() => { this._promptTimer = null; if (this.status.midiPrompt) { this.promptLate = true; this._emit(); } }, this.promptMs);
  }
  _disarmPrompt() { if (this._promptTimer !== null) { this._clear(this._promptTimer); this._promptTimer = null; } }

  _status(s) {
    const prev = this.status.state, prompted = !!this.status.midiPrompt;
    this.status = s;
    if (s.midiPrompt && !prompted) { this.promptLate = false; this._armPrompt(); }
    if (!s.midiPrompt && prompted) { this.promptLate = false; this._disarmPrompt(); if (s.state === "searching" && !s.quiet) this._arm(); }
    if (s.state === "connected" || s.state === "starting" || s.state === "idle" || s.state === "error") { this.notFound = false; this._disarm(); }
    if (s.state === "lost" && prev === "connected" && !s.quiet) this._arm();
    this._emit(prev);
  }
  _emit(prev) { const v = this.view(); for (const cb of this._subs) { try { cb(v, this.status, prev); } catch (e) { console.error(e); } } }
}
