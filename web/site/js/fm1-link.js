// SPDX-License-Identifier: GPL-3.0-only
// The page's one link to the user's FM-1: W4's RemoteFM1 (web/remote/remote.js) and what the page shows of it. The
// header's Connect FM-1 button, the start overlay's "Play on my FM-1" and the sheet's USE MY FM-1 all read view()
// and call connect() / disconnect() here, so they always agree. No DOM: web/site/test.mjs drives it.
//
//   tone  "off"   grey    not connected (also: looking quietly, by autoConnect(), for a unit to answer)
//         "busy"  amber   searching after a click, starting, reconnecting, stopping
//         "on"    green   connected; via "USB" (bulk) or "MIDI"
//         "bad"   red     an error, or nothing answered a click within searchMs; title says why; a click retries

const VIA = { bulk: "USB", midi: "MIDI" };

export function viewOf(s, notFound = false) {
  const via = VIA[s.transport] || "";
  const ver = s.unit && s.unit.fwVer ? `, RemoteFM1 ${s.unit.fwVer}` : "";
  const quietSearch = s.state === "searching" && s.quiet && !notFound;
  if (s.state === "connected")
    return { tone: "on", label: "FM-1 connected", via, live: true,
      title: `Your FM-1 is the panel${via ? " (over " + via + ")" : ""}${ver}. Click to disconnect.`,
      detail: `Connected${via ? " over " + via : ""}${ver}${s.fps ? ` · ${s.fps} fps` : ""}.` };
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
  if (/refused|denied|NotAllowed|Security/i.test(w))
    return "The browser blocked MIDI for this site: allow MIDI (the icon at the left of the address bar), then try again.";
  if (/no Web MIDI/i.test(w)) return "This browser has no Web MIDI: use Chrome or Edge on a computer.";
  return w ? w.charAt(0).toUpperCase() + w.slice(1) + (/[.!?]$/.test(w) ? "" : ".") : "Could not connect.";
}

export class Fm1Link {
  // remote: a RemoteFM1. opts: { searchMs: 10000 (a click's search with no answer turns red), setTimer, clearTimer }
  constructor(remote, opts = {}) {
    this.remote = remote;
    this.searchMs = opts.searchMs ?? 10000;
    this._set = opts.setTimer || ((f, ms) => setTimeout(f, ms));
    this._clear = opts.clearTimer || ((t) => clearTimeout(t));
    this.notFound = false;
    this._timer = null;
    this._subs = new Set();
    this.status = remote.status || { state: "idle" };
    remote.onStatus((s) => this._status(s));
  }
  on(cb) { this._subs.add(cb); return () => this._subs.delete(cb); }
  view() { return viewOf(this.status, this.notFound); }
  get connected() { return this.status.state === "connected"; }

  // at load: connect where the browser already allows it (remote.js autoConnect: no prompt)
  auto() { return typeof this.remote.autoConnect === "function" ? this.remote.autoConnect().catch(() => null) : Promise.resolve(null); }

  // from a click (call it synchronously in the handler: the WebUSB chooser needs the gesture)
  connect() {
    this.notFound = false;
    this._arm();
    const p = this.remote.connect().catch((e) => { this._status({ ...this.remote.status, state: "error", why: e.message || String(e) }); });
    this._emit();
    return p;
  }
  disconnect() { this.notFound = false; this._disarm(); return this.remote.disconnect(); }

  // a click's search that finds nothing within searchMs: red, and the search goes on quietly (2 s HELLO)
  _arm() {
    this._disarm();
    this._timer = this._set(() => {
      this._timer = null;
      const st = this.remote.status.state;
      if (st === "searching" || st === "lost") {
        this.notFound = true;
        if (this.remote.setQuiet) this.remote.setQuiet(true);
        this._emit();
      }
    }, this.searchMs);
  }
  _disarm() { if (this._timer !== null) { this._clear(this._timer); this._timer = null; } }

  _status(s) {
    const prev = this.status.state;
    this.status = s;
    if (s.state === "connected" || s.state === "starting" || s.state === "idle" || s.state === "error") { this.notFound = false; this._disarm(); }
    if (s.state === "lost" && prev === "connected" && !s.quiet) this._arm();
    this._emit(prev);
  }
  _emit(prev) { const v = this.view(); for (const cb of this._subs) { try { cb(v, this.status, prev); } catch (e) { console.error(e); } } }
}
