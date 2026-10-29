// SPDX-License-Identifier: GPL-3.0-only
// Faceplate colour themes: RemoteFM1 for iOS's (RemoteFM1IOS design/tokens.json, RFMKit Faceplate/Theme.swift). A
// theme names fourteen roles: the faceplate (body) and its printed labels, the recessed wells (bed), the key and button
// caps (key, with their lit top edge hi and hard shadow lo, and their legends keyText), the knobs and their pointer
// (mark), the display's bezel, the key LEDs (led, ledOff), and lcd / ink (the design's mock LCD: unused here, the LCD
// is the firmware's own pixels). The five presets are the app's; "My theme" is derived from five colours the visitor
// picks, by the app's rules, so a custom theme always stays legible. Colours here are [r, g, b] arrays (0..255).

export const hex = (s) => {
  const h = String(s || "").trim().replace(/^#/, "");
  if (!/^[0-9a-f]{6}$/i.test(h)) return null;
  const v = parseInt(h, 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
};
export const css = (c, a = 1) => (a >= 1 ? `rgb(${c[0]},${c[1]},${c[2]})` : `rgba(${c[0]},${c[1]},${c[2]},${a})`);
export const toHex = (c) => "#" + c.map((v) => Math.round(v).toString(16).padStart(2, "0")).join("").toUpperCase();

const lin = (c8) => { const c = c8 / 255; return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
const gam = (c) => 255 * (c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);
export const luminance = (c) => 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2]);
export const contrast = (a, b) => { const la = luminance(a), lb = luminance(b); return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05); };
export const mix = (a, b, t) => a.map((v, i) => Math.round(v + t * (b[i] - v)));

// CIE L*a*b* (D65), as Theme.swift's RGB.lab / init(l:a:b:)
const WP = [0.95047, 1, 1.08883];
export function lab(c) {
  const R = lin(c[0]), G = lin(c[1]), B = lin(c[2]);
  const x = (0.4124 * R + 0.3576 * G + 0.1805 * B) / WP[0], y = (0.2126 * R + 0.7152 * G + 0.0722 * B) / WP[1], z = (0.0193 * R + 0.1192 * G + 0.9505 * B) / WP[2];
  const f = (t) => (t > 216 / 24389 ? Math.cbrt(t) : (24389 / 27 * t + 16) / 116);
  const fx = f(x), fy = f(y), fz = f(z);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}
export function fromLab(l, a, b) {
  const fy = (l + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
  const fi = (t) => (t * t * t > 216 / 24389 ? t * t * t : (116 * t - 16) / (24389 / 27));
  const x = fi(fx) * WP[0], y = fi(fy) * WP[1], z = fi(fz) * WP[2];
  const R = 3.2406 * x - 1.5372 * y - 0.4986 * z, G = -0.9689 * x + 1.8758 * y + 0.0415 * z, B = 0.0557 * x - 0.2040 * y + 1.0570 * z;
  return [R, G, B].map((v) => Math.max(0, Math.min(255, Math.round(gam(Math.max(0, v))))));
}
// the same colour with its L* moved by dl (+ lighter)
export const lightness = (c, dl) => { const [l, a, b] = lab(c); return fromLab(Math.max(0, Math.min(100, l + dl)), a, b); };

export const ROLES = ["body", "label", "bed", "key", "hi", "lo", "keyText", "knob", "mark", "bezel", "lcd", "ink", "led", "ledOff"];
const NEAR_WHITE = "#F6F0E8", NEAR_BLACK = "#1B1714";

// the app's presets, in its order; Tangerine (the orange FM-1) is the default
export const PRESETS = [
  { name: "Tangerine", body: "#EE7A3C", label: "#3A1606", bed: "#874332", key: "#924E3A", hi: "#A9614B", lo: "#64301F", keyText: "#FBEDE3", knob: "#1B1715", mark: "#F4EDE6", bezel: "#141210", lcd: "#C9D6EA", ink: "#13203A", led: "#FFE6A8", ledOff: "#6F6668" },
  { name: "Midnight", body: "#1E2230", label: "#C9D2E6", bed: "#12151D", key: "#2B3244", hi: "#3B4359", lo: "#0A0C12", keyText: "#E6ECF8", knob: "#0A0B0F", mark: "#6CF0C2", bezel: "#05060A", lcd: "#0D1F1A", ink: "#7CF5C8", led: "#6CF0C2", ledOff: "#454B5C" },
  { name: "Mint", body: "#A8DCC8", label: "#143329", bed: "#2F5B52", key: "#376A60", hi: "#4A7F74", lo: "#1E3F38", keyText: "#EAF7F1", knob: "#1A2421", mark: "#F2FFF9", bezel: "#121816", lcd: "#E9F0D8", ink: "#1E2A17", led: "#FFF3B0", ledOff: "#6E8580" },
  { name: "Lilac", body: "#C9AEE6", label: "#2A1642", bed: "#573B77", key: "#634486", hi: "#77589B", lo: "#3B2655", keyText: "#F6EFFC", knob: "#1C1624", mark: "#F7F0FF", bezel: "#120E18", lcd: "#D7E3F2", ink: "#1A2236", led: "#FFD6F0", ledOff: "#8A7A9A" },
  { name: "Cream", body: "#EFE6D2", label: "#2A231C", bed: "#2A2520", key: "#3A332C", hi: "#4B433A", lo: "#141110", keyText: "#F3EBDC", knob: "#E8622A", mark: "#FFF4E8", bezel: "#15120F", lcd: "#F6C34A", ink: "#2A1A05", led: "#FF7A3D", ledOff: "#6A6158" },
];
export const DEFAULT_THEME = "Tangerine";
export const CUSTOM_NAME = "My theme";

// the five colours a visitor picks (the app's theme editor): Faceplate, Keys and buttons, Knobs, LEDs, Display (bezel)
export const CHOICE_ROLES = [["body", "Faceplate"], ["key", "Keys and buttons"], ["knob", "Knobs"], ["led", "LEDs"], ["bezel", "Display"]];
export const choiceOf = (t) => Object.fromEntries(CHOICE_ROLES.map(([k]) => [k, t[k]]));

// the text colour, near-white or near-black, with the better contrast on bg
const textOn = (bg) => (contrast(bg, hex(NEAR_WHITE)) >= contrast(bg, hex(NEAR_BLACK)) ? NEAR_WHITE : NEAR_BLACK);

// a custom theme from its five colours (Theme.swift derived): hi = key + 8 L*, lo = key - 18 L*, bed = key - 6 L*,
// keyText / label = near-white or near-black for contrast on key / body, mark = near-white on a dark knob and
// near-black on a light one, ledOff = a grey 20 L* lighter than bed. Missing or bad colours come from Tangerine.
export function derived(choice, name = CUSTOM_NAME) {
  const base = PRESETS[0], c = {};
  for (const [k] of CHOICE_ROLES) c[k] = hex(choice && choice[k]) || hex(base[k]);
  const bed = lightness(c.key, -6);
  return {
    name, body: toHex(c.body), label: textOn(c.body), bed: toHex(bed), key: toHex(c.key), hi: toHex(lightness(c.key, 8)),
    lo: toHex(lightness(c.key, -18)), keyText: textOn(c.key), knob: toHex(c.knob), mark: lab(c.knob)[0] < 55 ? NEAR_WHITE : NEAR_BLACK,
    bezel: toHex(c.bezel), lcd: base.lcd, ink: base.ink, led: toHex(c.led), ledOff: toHex(fromLab(Math.min(lab(bed)[0] + 20, 100), 0, 0)),
  };
}

// a preset by name ("My theme": the custom one, from the stored choice); null when unknown
export function presetNamed(n, custom) {
  const s = String(n || "").toLowerCase();
  if (s === CUSTOM_NAME.toLowerCase()) return derived(custom);
  return PRESETS.find((p) => p.name.toLowerCase() === s) || null;
}

// every role as [r, g, b], plus the fixed colours (design/tokens.json fixedColors)
export function resolve(t) {
  const p = {};
  for (const r of ROLES) p[r] = hex(t[r]) || hex(PRESETS[0][r]);
  p.indicator = hex("#FF3B2F"); p.play = hex("#4FE08A"); p.rec = hex("#FF4436"); p.linked = hex("#3BD47A");
  p.black = [0, 0, 0]; p.white = [255, 255, 255];
  return p;
}
// (the old name, for callers that still say derive)
export const derive = resolve;

// the sheet's accent: the theme's own colour where it reads on the sheet (the faceplate, else the LEDs), else the
// app's sheet accent
const SHEET_SURFACE = hex("#211E19");
const saturation = (c) => { const hi = Math.max(...c), lo = Math.min(...c); return hi > 0 ? (hi - lo) / hi : 0; };
export function accentOf(t) {
  for (const k of ["body", "led", "hi"]) {
    const c = hex(t[k]);
    if (c && saturation(c) >= 0.25 && contrast(c, SHEET_SURFACE) >= 3) return c;
  }
  return hex("#FF8A4C");
}
export const onAccent = (a) => (contrast(a, hex("#17140F")) >= contrast(a, [255, 255, 255]) ? hex("#17140F") : [255, 255, 255]);
