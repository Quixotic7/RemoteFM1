// SPDX-License-Identifier: GPL-3.0-only
// The screen protector: the clear film the FM-1 ships with, as RemoteFM1 for iOS's (RemoteFM1IOS RFMKit
// Faceplate/ScreenProtector.swift). A faint sheen over the display, a strip out to its solid red tab (about a knob
// cap's size) right of the screen, level with its top, square to the panel; on the wide (landscape) layouts only, as
// the FM-1 looks. Dragging the tab across the screen folds the film back (the fold line halfway between the tab's
// place and the pointer); let go early and it springs back, past a third of the way it peels off and is gone, the red
// square with it. Remembered (prefs.screenProtector); the sheet's View › Screen protector puts it back.
// The geometry needs no DOM (web/site/test.mjs runs it). Points are [x, y] in the canvas's CSS pixels.

export const PEEL_AT = 0.33;
// let go at this fold (0..1): peeled off (true) or back on
export const peels = (fold) => fold > PEEL_AT;
// the layouts that carry the film (panel.js LAYOUTS): the wide ones
export const filmOn = (kind) => kind === "phoneLandscape" || kind === "padLandscape";

// the tab's square beside a display {x, y, w, h}: right of it, level with its top, overlapping the bezel a little
export function tabRect(d) {
  const t = Math.round(d.w * 0.13), o = Math.round(t * 0.15);
  return { x: d.x + d.w - o, y: d.y + o, w: t, h: t };
}

const area = (p) => p.reduce((s, a, i) => { const b = p[(i + 1) % p.length]; return s + a[0] * b[1] - b[0] * a[1]; }, 0) / 2;
const oriented = (p) => (area(p) < 0 ? p.slice().reverse() : p);
// a polygon's part where f >= 0 (f linear: one half-plane)
function clip(poly, f) {
  const out = [];
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length], fa = f(a), fb = f(b);
    if (fa >= 0) out.push(a);
    if ((fa >= 0) !== (fb >= 0)) { const k = fa / (fa - fb); out.push([a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k]); }
  }
  return out;
}

// the film's outline: the display's rounded square and a strip out to the tab, and its fold
export class FilmGeometry {
  constructor(display, radius, tab = tabRect(display)) {
    this.display = display; this.radius = radius; this.tab = tab;
    const d = display, c = [tab.x + tab.w / 2, tab.y + tab.h / 2];
    // the peel runs straight across the screen, away from the tab (the tab is right of the screen: leftward)
    const dx = c[0] >= d.x + d.w / 2 ? -1 : 1, dy = 0;
    this.direction = [dx, dy];
    const r = Math.min(radius, d.w / 2, d.h / 2), sq = [];
    for (const [x, y, a0] of [[d.x + d.w - r, d.y + d.h - r, 0], [d.x + r, d.y + d.h - r, 90], [d.x + r, d.y + r, 180], [d.x + d.w - r, d.y + r, 270]])
      for (let i = 0; i <= 6; i++) { const a = (a0 + i * 15) * Math.PI / 180; sq.push([x + r * Math.cos(a), y + r * Math.sin(a)]); }
    // the strip: a straight band from inside the display's edge out to the tab, as tall as the tab
    const x0 = dx < 0 ? d.x + d.w - r : d.x + r;
    const strip = [[x0, tab.y], [c[0], tab.y], [c[0], tab.y + tab.h], [x0, tab.y + tab.h]];
    this.pieces = [oriented(sq), oriented(strip)];
    const proj = this.pieces.flat().map((p) => (p[0] - c[0]) * dx + (p[1] - c[1]) * dy);
    const reach = Math.max(...proj);
    this.back = Math.min(0, Math.min(...proj));   // the film's back edge: the fold starts there, nothing folds at rest
    this.length = 2 * Math.max(reach - this.back, 1);   // the tab's travel that folds the whole film over
  }
  // the fold (0..1) from the pointer's movement: its travel along the peel's direction over the film's length
  fold(dx, dy) { return Math.min(1, Math.max(0, (dx * this.direction[0] + dy * this.direction[1]) / this.length)); }
  // the film folded by travel t: what's left on, the folded-back flap (mirrored across the fold line) and the tab's centre
  folded(t) {
    const c = [this.tab.x + this.tab.w / 2, this.tab.y + this.tab.h / 2], u = this.direction, h = Math.max(0, t) / 2;
    const along = (p) => (p[0] - c[0]) * u[0] + (p[1] - c[1]) * u[1] - this.back - h;
    const left = this.pieces.map((p) => clip(p, along)).filter((p) => p.length > 2);
    const flap = h <= 0 ? [] : this.pieces.map((p) => clip(p, (q) => -along(q))).filter((p) => p.length > 2 && Math.abs(area(p)) > 0.5)
      .map((p) => p.map((q) => { const k = along(q); return [q[0] - 2 * k * u[0], q[1] - 2 * k * u[1]]; }));
    return { left, flap, tab: [c[0] + u[0] * 2 * h, c[1] + u[1] * 2 * h] };
  }
  // is (x, y) on the tab (a hit area of at least 44 px, as the app's)
  onTab(x, y, t = 0) {
    const [cx, cy] = this.folded(t).tab, half = Math.max(22, this.tab.w / 2);
    return Math.abs(x - cx) <= half && Math.abs(y - cy) <= half;
  }
}
