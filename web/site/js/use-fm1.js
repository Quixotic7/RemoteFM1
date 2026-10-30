// SPDX-License-Identifier: GPL-3.0-only
// USE MY FM-1 around W4's RemoteFM1 class (web/remote/remote.js, its README "The page's wiring"): one RemoteFM1 for
// the page (fm1-link.js), shown in three places that mirror each other: the header's Connect FM-1 button, the start
// overlay's "Play on my FM-1", and the sheet's USE MY FM-1 section.
// The unit's panel goes into the page's input as source REMOTE (OR'ed with the page's own: keys and buttons held on
// either are held; encoder deltas added once; the unit's MASTER wins once it has reported it); the engine's screen
// and LEDs go out to the unit. Engine MIDI out never goes to the unit's port, and the page's own MIDI input skips the
// unit's ports (input.js). MIDI into the unit's TRS jack comes to the page (MIDI_IN) and goes to the firmware's MIDI
// in, as the computer's MIDI input does.
import { h } from "./sheet.js";
import { Fm1Link } from "./fm1-link.js";

export function createFm1(site, mod, { onUnitPorts } = {}) {
  const remote = new mod.RemoteFM1();
  const link = new Fm1Link(remote);
  let unitKeys = 0, unitButtons = 0;

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
  // the unit's TRS MIDI in (MIDI_IN) -> the firmware's MIDI in, as the computer's own MIDI input goes
  if (typeof remote.onMidiIn === "function") remote.onMidiIn((bytes) => site.midiIn(bytes));
  // the engine's screen and LEDs -> the unit; until the engine draws, a screen that says what to do
  if (!site.screen().seq) { const w = waitingFrame(); if (w) remote.setFrame(-1, w); }
  site.on("frame", (px, seq) => remote.setFrame(seq, px));
  site.on("leds", (levels, green) => remote.setLeds(levels, green));
  site.on("firmware", () => { const s = site.screen(); remote.setFrame(s.seq, s.pixels); });
  addEventListener("pagehide", () => { try { remote.stop(); } catch (_) { /* (closing) */ } });

  link.on((v, s) => {
    if (s.state !== "connected") { unitKeys = 0; unitButtons = 0; site.panel.releaseAll(); }
    if (onUnitPorts) onUnitPorts(s.ports || null);
  });
  return link;
}

// 240 x 240 RGB565 (host order): "Press Play in the browser", for the unit before the engine's first frame
function waitingFrame() {
  try {
    const c = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(240, 240) : Object.assign(document.createElement("canvas"), { width: 240, height: 240 });
    const g = c.getContext("2d");
    g.fillStyle = "#0F1113"; g.fillRect(0, 0, 240, 240);
    const grad = g.createLinearGradient(100, 54, 140, 94);
    grad.addColorStop(0, "#FFA45E"); grad.addColorStop(1, "#E07A3C");
    g.fillStyle = grad; g.beginPath(); g.roundRect ? g.roundRect(100, 54, 40, 40, 10) : g.rect(100, 54, 40, 40); g.fill();
    g.fillStyle = "#1A0E06"; g.beginPath(); g.moveTo(113, 63); g.lineTo(113, 85); g.lineTo(131, 74); g.closePath(); g.fill();
    g.textAlign = "center"; g.fillStyle = "#E8EAED";
    g.font = '700 21px "DM Sans", system-ui, sans-serif'; g.fillText("Press Play", 120, 134);
    g.font = '600 15px "DM Sans", system-ui, sans-serif'; g.fillStyle = "#8A9099"; g.fillText("in the browser", 120, 156);
    g.font = '600 12px "DM Sans", system-ui, sans-serif'; g.fillStyle = "#3DDC84"; g.fillText("FM-1 PLAYER CONNECTED", 120, 196);
    const d = g.getImageData(0, 0, 240, 240).data, px = new Uint16Array(240 * 240);
    for (let i = 0; i < px.length; i++) px[i] = ((d[4 * i] >> 3) << 11) | ((d[4 * i + 1] >> 2) << 5) | (d[4 * i + 2] >> 3);
    return px;
  } catch (_) { return null; }
}

// ---- the sheet's section
export function mountUseFm1(slot, link, site) {
  const btn = h("button", { class: "pill primary" }, "Connect my FM-1");
  const line = h("p", { class: "remote-status small" });
  btn.addEventListener("click", () => { const v = link.view(); if (v.tone === "on" || v.tone === "busy") link.disconnect(); else link.connect(); });
  const show = (v) => {
    const live = v.tone === "on" || v.tone === "busy";
    btn.textContent = v.tone === "on" ? "Disconnect" : v.tone === "busy" ? "Stop" : v.tone === "bad" ? "Try again" : "Connect my FM-1";
    btn.className = "pill " + (live ? "" : "primary");
    line.replaceChildren(h("span", { class: "fm1-dot " + v.tone + (v.live ? " live" : "") }), v.detail);
    line.className = "remote-status small " + (v.tone === "on" ? "ok" : v.tone === "bad" ? "bad" : "muted");
  };
  link.on(show);
  show(link.view());
  slot.replaceChildren(
    h("p", { class: "row-sub", style: { margin: "0 0 10px" } }, "Play this firmware from your own FM-1: its keys, knobs and screen, the sound here. The FM-1 needs RemoteFM1."),
    h("div", { class: "pills two" }, btn, h("a", { class: "pill", href: site.installerUrl }, "Install RemoteFM1")),
    line,
    h("p", { class: "note" }, "Chrome or Edge. Once allowed, it connects by itself when the page opens or the FM-1 is plugged in. macOS and ChromeOS get the fast USB path; Windows and Linux fall back to MIDI."));
}

// ---- the header's Connect FM-1 button, and its menu while connected or connecting
export function mountHeader(btn, link, { openSheet } = {}) {
  const label = btn.querySelector(".fm1-label"), via = btn.querySelector(".fm1-via"), dot = btn.querySelector(".fm1-dot");
  const menu = h("div", { class: "fm1-menu", role: "dialog", "aria-label": "Your FM-1", hidden: true });
  document.body.append(menu);
  const close = () => { menu.hidden = true; btn.setAttribute("aria-expanded", "false"); };
  const mDot = h("span"), mText = h("span"), mStop = h("button", { class: "pill danger", onclick: () => { close(); link.disconnect(); } });
  menu.append(h("div", { class: "fm1-menu-line" }, mDot, mText),
    h("div", { class: "fm1-menu-row" }, mStop, openSheet ? h("button", { class: "pill", onclick: () => { close(); openSheet(); } }, "Settings") : null));
  const fill = (v) => { mDot.className = "fm1-dot " + v.tone + (v.live ? " live" : ""); mText.textContent = v.detail; mStop.textContent = v.tone === "on" ? "Disconnect" : "Stop"; };
  const open = () => {
    const r = btn.getBoundingClientRect();
    fill(link.view());
    menu.style.top = r.bottom + 6 + "px";
    menu.style.right = Math.max(8, innerWidth - r.right) + "px";
    menu.hidden = false;
    btn.setAttribute("aria-expanded", "true");
  };
  btn.addEventListener("click", (e) => {
    e.stopPropagation();
    if (!menu.hidden) { close(); return; }
    const v = link.view();
    if (v.tone === "on" || v.tone === "busy") open(); else link.connect();
  });
  addEventListener("pointerdown", (e) => { if (!menu.hidden && !menu.contains(e.target) && !btn.contains(e.target)) close(); }, true);
  addEventListener("keydown", (e) => { if (e.key === "Escape" && !menu.hidden) close(); });
  const show = (v) => {
    btn.dataset.tone = v.tone;
    dot.className = "fm1-dot " + v.tone + (v.live && v.tone !== "off" ? " live" : "");
    label.textContent = v.label;
    via.textContent = v.via; via.hidden = !v.via;
    btn.title = v.title;
    btn.setAttribute("aria-label", v.via ? `${v.label} over ${v.via}` : v.label);
    if (!menu.hidden) { if (v.tone === "on" || v.tone === "busy") fill(v); else close(); }
  };
  link.on(show);
  show(link.view());
  btn.hidden = false;
}
