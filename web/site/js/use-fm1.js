// SPDX-License-Identifier: GPL-3.0-only
// USE MY FM-1: the sheet's section around W4's RemoteFM1 class (web/remote/remote.js, its README "The page's
// wiring"). The unit's panel goes into the page's input as source REMOTE (OR'ed with the page's own: keys and buttons
// held on either are held; encoder deltas added once; the unit's MASTER wins once it has reported it); the engine's
// screen and LEDs go out to the unit. Engine MIDI out never goes to the unit's port, and the page's own MIDI input
// skips the unit's ports (input.js).
import { h } from "./sheet.js";

export function mountUseFm1(slot, site, mod, { onUnitPorts } = {}) {
  const remote = new mod.RemoteFM1();
  let unitKeys = 0, unitButtons = 0, status = remote.status || { state: "idle" };

  // the unit's panel -> the page's input (site.panel: source REMOTE, matrix ids; remote-hook.js maps them)
  remote.onPanel((p) => {
    for (let k = 0; k < 27; k++) {
      const was = (unitKeys >>> k) & 1, now = (p.keys >>> k) & 1, tap = (p.taps && (p.taps.keys >>> k) & 1) && !now && !was;
      if (tap) { site.panel.key(k, true); site.panel.key(k, false); }
      else if (was !== now) site.panel.key(k, !!now);
    }
    for (let m = 0; m < 14; m++) {
      const was = (unitButtons >>> m) & 1, now = (p.buttons >>> m) & 1, tap = (p.taps && (p.taps.buttons >>> m) & 1) && !now && !was;
      if (tap) { site.panel.button(m, true); site.panel.button(m, false); }
      else if (was !== now) site.panel.button(m, !!now);
    }
    unitKeys = p.keys >>> 0; unitButtons = p.buttons >>> 0;
    if (p.enc) for (let i = 0; i < 7; i++) if (p.enc[i]) site.panel.turn(i, p.enc[i]);
    if (p.master !== null && p.master !== undefined) site.panel.master(p.master);
  });
  // the engine's screen and LEDs -> the unit
  site.on("frame", (px, seq) => remote.setFrame(seq, px));
  site.on("leds", (levels, green) => remote.setLeds(levels, green));
  site.on("firmware", () => { const s = site.screen(); remote.setFrame(s.seq, s.pixels); });
  addEventListener("pagehide", () => { try { remote.stop(); } catch (_) { /* (closing) */ } });

  const btn = h("button", { class: "pill primary" }, "Connect my FM-1");
  const line = h("p", { class: "remote-status small" });
  btn.addEventListener("click", async () => {
    if (/connected|searching|starting|lost/.test(status.state)) { await remote.disconnect(); return; }
    try { await remote.connect(); } catch (e) { show({ state: "error", why: e.message || String(e) }); }
  });
  function show(s) {
    status = s;
    const live = /connected|searching|starting|lost/.test(s.state);
    btn.textContent = live ? "Stop" : "Connect my FM-1";
    btn.className = "pill " + (live ? "" : "primary");
    const via = s.transport === "bulk" ? "USB" : s.transport === "midi" ? "MIDI" : "";
    const text = {
      idle: "Not connected.", stopped: "Not connected.", stopping: "Stopping...",
      searching: "Looking for an FM-1 running RemoteFM1...", starting: "Found it: starting...",
      connected: `Connected${via ? " over " + via : ""}${s.unit && s.unit.fwVer ? ", RemoteFM1 " + s.unit.fwVer : ""}${s.fps ? ` · ${s.fps} fps` : ""}.`,
      lost: "Lost the FM-1: looking again...",
      error: s.why || "Could not connect.",
    }[s.state] || s.state;
    line.textContent = text;
    line.className = "remote-status small " + (s.state === "connected" ? "ok" : s.state === "error" || s.state === "lost" ? "bad" : "muted");
    if (s.state !== "connected") { unitKeys = 0; unitButtons = 0; site.panel.releaseAll(); }
    if (onUnitPorts) onUnitPorts(s.ports || null);
  }
  remote.onStatus(show);
  show(status);
  slot.replaceChildren(
    h("p", { class: "row-sub", style: { margin: "0 0 10px" } }, "Play this firmware from your own FM-1: its keys, knobs and screen, the sound here. The FM-1 needs RemoteFM1."),
    h("div", { class: "pills two" }, btn, h("a", { class: "pill", href: site.installerUrl }, "Install RemoteFM1")),
    line,
    h("p", { class: "note" }, "Chrome or Edge. macOS and ChromeOS get the fast USB path; Windows and Linux fall back to MIDI."));
  return { remote };
}
