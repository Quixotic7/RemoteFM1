// SPDX-License-Identifier: GPL-3.0-only
// Panel colour themes: FM1VST plugin/Theme.cpp ported. A theme is three colours (the body plate, the silicone
// membrane of the keys and buttons, the knob caps) plus two optional overrides (the bed, the printed labels);
// derive() computes every colour the panel draws from them, so a theme always stays legible. The presets are
// FM1VST themes/presets.json (copied to data/themes.json). Colours here are [r, g, b] arrays (0..255).

export const hex = (s) => {
  const h = String(s || "").trim().replace(/^#/, "");
  if (!/^[0-9a-f]{6}$/i.test(h)) return null;
  const v = parseInt(h, 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
};
export const css = (c, a = 1) => (a >= 1 ? `rgb(${c[0]},${c[1]},${c[2]})` : `rgba(${c[0]},${c[1]},${c[2]},${a})`);
export const toHex = (c) => "#" + c.map((v) => v.toString(16).padStart(2, "0")).join("").toUpperCase();

const lin = (c8) => { const c = c8 / 255; return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
export const luminance = (c) => 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2]);
export const contrast = (a, b) => { const la = luminance(a), lb = luminance(b); return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05); };
// juce::Colour::interpolatedWith
export const mix = (a, b, t) => a.map((v, i) => v + Math.round(t * (b[i] - v)));
const WHITE = [255, 255, 255], BLACK = [0, 0, 0];
const isDark = (c) => luminance(c) < 0.179;

function ensureContrast(c, against, floor, lighterFirst) {
  if (contrast(c, against) >= floor) return c;
  for (let pass = 0; pass < 2; pass++) {
    const lighter = pass === 0 ? lighterFirst : !lighterFirst;
    for (let i = 1; i <= 64; i++) {
      const cand = mix(c, lighter ? WHITE : BLACK, i / 64);
      if (contrast(cand, against) >= floor) return cand;
    }
  }
  return contrast(WHITE, against) >= contrast(BLACK, against) ? WHITE : BLACK;
}
const autoText = (bg, light, dark) => (contrast(light, bg) >= contrast(dark, bg) ? light : dark);

// theme: { name, base, membrane, knob, bed?, label? } with "#RRGGBB" strings
export function derive(t) {
  const p = {};
  p.backdrop = hex("#0E0E10");
  p.plate = hex(t.base);
  p.outline = BLACK;
  p.edge = isDark(p.plate) ? mix(p.plate, WHITE, 0.06) : mix(p.plate, BLACK, 0.08);
  const m = hex(t.membrane), bed = hex(t.bed), label = hex(t.label);
  p.bed = bed || mix(m, BLACK, 0.15);
  p.bedEdge = isDark(p.bed) ? mix(p.bed, WHITE, 0.09) : mix(p.bed, BLACK, 0.12);
  p.cap = m;
  p.pressed = ensureContrast(luminance(m) < 0.6 ? mix(m, WHITE, 0.12) : mix(m, BLACK, 0.12), m, 1.3, luminance(m) < 0.6);
  p.ledOff = ensureContrast(mix(m, BLACK, 0.4), m, 1.3, false);
  p.capOutline = isDark(m) ? mix(m, BLACK, 0.78) : mix(m, BLACK, 0.55);
  p.capText = ensureContrast(autoText(m, hex("#C8C8D0"), hex("#1A1A1E")), m, 3.0, isDark(m));
  p.noteText = mix(p.capText, m, 0.4);
  p.litText = hex("#111114");
  const light = hex("#E8E8EC"), dark = hex("#202024");
  p.label = label || autoText(p.plate, light, dark);
  p.bedLabel = label && contrast(label, p.bed) >= 3.0 ? label : autoText(p.bed, light, dark);
  p.hint = ensureContrast(hex("#54D678"), m, 2.0, isDark(m));
  p.hintLit = hex("#1E3A26");
  p.capDim = mix(m, hex("#F4F4FF"), 0.38);
  p.capTextDim = ensureContrast(autoText(p.capDim, hex("#C8C8D0"), hex("#1A1A1E")), p.capDim, 3.0, isDark(p.capDim));
  p.hintDim = ensureContrast(hex("#54D678"), p.capDim, 2.0, isDark(p.capDim));
  p.hintBed = ensureContrast(hex("#54D678"), p.bed, 2.0, isDark(p.bed));
  const k = hex(t.knob);
  p.knobCap = k;
  p.knobBody = isDark(k) ? mix(k, BLACK, 0.28) : mix(k, BLACK, 0.2);
  p.knobRing = ensureContrast(mix(k, WHITE, 0.03), p.knobBody, 1.25, true);
  p.pointer = ensureContrast(isDark(k) ? hex("#C8C8D0") : hex("#1A1A1E"), k, 2.0, isDark(k));
  p.masterPointer = isDark(k) ? WHITE : BLACK;
  p.screenBezel = hex("#0B0B0E");
  p.white = hex("#F4F4FF");
  p.red = hex("#FF4242");
  p.orange = hex("#FFA52A");
  p.green = hex("#3DDC5A");
  return p;
}

// FM1VST Look::setAccentFrom: the membrane, lifted to read on the Graphite sheet (a grey one: the fallback orange)
const SURFACE = hex("#1D2024");
function saturation(c) {   // juce::Colour::getSaturation (HSB)
  const hi = Math.max(...c), lo = Math.min(...c);
  return hi > 0 ? (hi - lo) / hi : 0;
}
export function accentOf(t) {
  let a = hex(t.membrane);
  if (saturation(a) < 0.18) a = hex("#FF8A3D");
  for (let i = 0; i < 40 && contrast(a, SURFACE) < 3.0; i++) a = mix(a, WHITE, 0.06);
  return a;
}
export const onAccent = (a) => (contrast(a, hex("#1A0E06")) >= contrast(a, WHITE) ? hex("#1A0E06") : WHITE);

// the presets (FM1VST themes/presets.json), in its order; "Emulator" is the default
export const PRESETS = [
  { name: "Black", base: "#3A3A3C", membrane: "#2E2E30", bed: "#2C2C2C", knob: "#2A2A2A" },
  { name: "Black/Green", base: "#4C4B4E", membrane: "#6FCCBD", bed: "#65CDBC", knob: "#424344" },
  { name: "Cool Gray", base: "#E3D9CF", membrane: "#3A3C3D", bed: "#1A1E1F", knob: "#1C1C1C" },
  { name: "Orange", base: "#C4734F", membrane: "#92594D", bed: "#834C41", knob: "#171717", label: "#FFF8F2" },
  { name: "Purple", base: "#C19AD5", membrane: "#7D72B5", bed: "#6B61AD", knob: "#1C1C1E" },
  { name: "White/Blue", base: "#F2ECE7", membrane: "#587592", bed: "#466484", knob: "#181918" },
  { name: "Emulator", base: "#1C1C20", membrane: "#2B2B31", bed: "#141417", knob: "#35353C", label: "#8A8A92" },
];
export const DEFAULT_THEME = "Emulator";
export const presetNamed = (n) => PRESETS.find((p) => p.name.toLowerCase() === String(n || "").toLowerCase()) || null;
