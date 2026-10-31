// SPDX-License-Identifier: GPL-3.0-only
// The hook for "Use my FM-1" (WEB-PLAN.md section 2, agent W4, web/remote/). The sheet's USE MY FM-1 section is a
// stub until web/remote/remote.js exists; then the page imports it and calls
//
//     mount(sectionElement, site)        // may return { unmount() }
//
// `site` is the page's whole surface for it (version 1):
//     site.version                         1
//     site.current()                       { entry, engine, info } of the running firmware (entry: a catalog entry)
//     site.screen()                        { seq, pixels: Uint16Array(240*240) } the latest frame, RGB565 host order
//     site.leds()                          { levels: Uint8Array(44), playGreen } levels[col * 4 + line]
//     site.on(event, cb) -> off()          events: "frame" (pixels, seq), "leds" (levels, playGreen), "info" (info),
//                                          "midiOut" (bytes), "firmware" ({ entry, engine })
//     site.panel.key(k, down, vel?)        note key 0..26 (F3..G5), from the user's FM-1
//     site.panel.button(matrixId, down)    matrix button 0 OCT- 1 OCT+ 2 FX .. 13 REC
//     site.panel.turn(matrixEnc, detents)  matrix encoder 0 SELECT 1 ALGO 2..5 KNOB1..4 6 PRESETS, + = clockwise
//     site.panel.master(v)                 0..1023
//     site.panel.lean(k, v)                a held key's pressure 0..100 (aftertouch; fm1core's key_lean)
//     site.panel.releaseAll()              (the unit went away: nothing stays held)
//     site.midiIn(bytes)                   raw MIDI to the firmware's MIDI in
//     site.setStatus(text, kind)           the line under the section's title ("", "ok", "warn", "bad")
//     site.installerUrl                    the installer page, for "Install RemoteFM1 first"
// The FM-1's own input arrives as source REMOTE, OR'ed with the page's own (a key held on either is held).
import { SRC, labelToMatrix, roleToMatrix, NB } from "./panel.js";

const MATRIX_TO_LABEL = Array.from({ length: NB }, (_, m) => (m >= 2 ? m - 2 : m + 12));
const MATRIX_TO_ROLE = [0, 1, 3, 4, 5, 6, 2];

export function makeSite({ controls, getState, installerUrl, setStatus, midiIn }) {
  const subs = new Map();
  const site = {
    version: 1,
    installerUrl,
    current: () => { const s = getState(); return { entry: s.entry, engine: s.engine, info: s.info }; },
    screen: () => { const s = getState(); return { seq: s.frameSeq, pixels: s.pixels ? s.pixels.slice() : new Uint16Array(240 * 240) }; },
    leds: () => { const s = getState(); return { levels: s.levels.slice(), playGreen: s.playGreen }; },
    on(ev, cb) {
      if (!subs.has(ev)) subs.set(ev, new Set());
      subs.get(ev).add(cb);
      return () => subs.get(ev).delete(cb);
    },
    panel: {
      key: (k, down, vel = 0) => controls.holdKey(k, SRC.REMOTE, !!down, vel),
      button: (m, down) => controls.holdBtn(MATRIX_TO_LABEL[m], SRC.REMOTE, !!down),
      turn: (enc, detents) => controls.turn(MATRIX_TO_ROLE[enc], detents | 0),
      master: (v) => controls.setMaster(v),
      lean: (k, v) => controls.leanKey(k, v),
      releaseAll: () => controls.releaseSrc(SRC.REMOTE),
    },
    midiIn: (bytes) => midiIn(bytes),
    setStatus: (text, kind = "") => setStatus(text, kind),
  };
  const emit = (ev, ...a) => { for (const cb of subs.get(ev) || []) { try { cb(...a); } catch (e) { console.error(e); } } };
  return { site, emit };
}
// (the inverse maps, checked against panel.js's)
for (let b = 0; b < NB; b++) if (MATRIX_TO_LABEL[labelToMatrix(b)] !== b) throw new Error("remote-hook: button map");
for (let r = 0; r < 7; r++) if (MATRIX_TO_ROLE[roleToMatrix(r)] !== r) throw new Error("remote-hook: encoder map");
