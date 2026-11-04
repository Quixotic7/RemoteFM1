// SPDX-License-Identifier: GPL-3.0-only
// Skins: an image behind the faceplate, as RemoteFM1 for iOS's (RemoteFM1IOS RFMKit Faceplate/Skin.swift, PLAN.md
// section 23).
//   the visitor's   the sheet's Background: one image for every firmware, kept in this browser's IndexedDB (scaled to
//                   at most 2048 px, a JPEG); Dim lays the faceplate's colour over it (0..0.8, default 0.25), so the
//                   labels, drawn for contrast on that colour, stay legible on a busy image
//   a firmware's    an .rfm1 may carry its own skin (RemoteFM1IOS docs/RFM1-FORMAT.md section 8): the custom sections
//                   rfm1.skin (JSON: {"skin": 1, "dim", "theme"}), rfm1.skin.background and .landscape (PNG or JPEG,
//                   at most 4 MiB each); the rfm1 engine hands them over in info().skin. Shown while that firmware
//                   plays, its colours in place of the theme, unless "Firmware skins" is off
// The firmware's skin wins over the visitor's image (it was made for that firmware); with neither, the theme's colour.
// The parsing and the choice need no DOM (web/site/test.mjs runs them); the decoding and the store need a browser.
import { ROLES, hex, derived } from "./theme.js";

export const SECTION = "rfm1.skin", BACKGROUND = "rfm1.skin.background", LANDSCAPE = "rfm1.skin.background.landscape";
export const MAX_IMAGE_BYTES = 4 << 20, MAX_PIXELS = 2048, MAX_DIM = 0.8, DEFAULT_DIM = 0.25;
export const SKIN_THEME_NAME = "Firmware skin";

export const clampDim = (d) => (typeof d === "number" && isFinite(d) ? Math.min(Math.max(d, 0), MAX_DIM) : 0);

// a firmware skin's colours: every role given, those; else the five a visitor picks (body, key, knob, led, bezel),
// the rest derived from them (theme.js derived, as Skin.swift's theme(_:)); null otherwise
export function skinTheme(t) {
  if (!t || typeof t !== "object") return null;
  if (ROLES.every((r) => hex(t[r]))) {
    const full = { name: SKIN_THEME_NAME };
    for (const r of ROLES) full[r] = "#" + String(t[r]).trim().replace(/^#/, "").toUpperCase();
    return full;
  }
  if (!["body", "key", "knob", "led", "bezel"].every((r) => hex(t[r]))) return null;
  return derived(t, SKIN_THEME_NAME);
}

const isImage = (b) => b instanceof Uint8Array && b.length > 0 && b.length <= MAX_IMAGE_BYTES;

// the skin in an .rfm1's sections ({ name: Uint8Array }, the engine's info().skin): { dim, theme, background,
// landscape } (the images still bytes), or null when it has none (no rfm1.skin, skin != 1, no image and no theme)
export function parseSkin(sections) {
  if (!sections || !(sections[SECTION] instanceof Uint8Array)) return null;
  let o;
  try { o = JSON.parse(new TextDecoder().decode(sections[SECTION])); } catch (_) { return null; }
  if (!o || typeof o !== "object" || o.skin !== 1) return null;
  const background = isImage(sections[BACKGROUND]) ? sections[BACKGROUND] : null;
  const landscape = isImage(sections[LANDSCAPE]) ? sections[LANDSCAPE] : null;
  const theme = skinTheme(o.theme);
  if (!background && !landscape && !theme) return null;
  return { dim: clampDim(o.dim), theme, background, landscape };
}

// what the faceplate shows: the playing firmware's skin (when Firmware skins is on), else the visitor's image, else
// null. fw / user: decoded skins ({ id, portrait, landscape, dim, theme }); user's dim is the setting's
export function pickSkin({ firmwareSkins = true, fw = null, user = null, dim = DEFAULT_DIM } = {}) {
  if (firmwareSkins && fw) return fw;
  if (user && user.portrait) return { ...user, dim: clampDim(dim), theme: null };
  return null;
}

// the image for a canvas of this size: a wide one gets the landscape image, if there is one
export const imageFor = (skin, wide) => (skin ? (wide ? skin.landscape || skin.portrait : skin.portrait || skin.landscape) || null : null);

// the source rectangle that fills (w, h) from an image of (iw, ih), cropped about its centre ("cover")
export function coverRect(iw, ih, w, h) {
  const s = Math.max(w / iw, h / ih), sw = w / s, sh = h / s;
  return [(iw - sw) / 2, (ih - sh) / 2, sw, sh];
}

// ---------------------------------------------------------------- the browser's part ---

// an image's bytes (or a Blob) to an ImageBitmap at most MAX_PIXELS on its longer side; null when it isn't one
export async function decodeImage(data, maxBytes = MAX_IMAGE_BYTES) {
  try {
    const blob = data instanceof Blob ? data : new Blob([data]);
    if (blob.size > maxBytes || typeof createImageBitmap !== "function") return null;
    const full = await createImageBitmap(blob);
    const s = Math.min(1, MAX_PIXELS / Math.max(full.width, full.height));
    if (s >= 1) return full;
    const out = await createImageBitmap(full, { resizeWidth: Math.round(full.width * s), resizeHeight: Math.round(full.height * s), resizeQuality: "high" });
    full.close();
    return out;
  } catch (_) { return null; }
}

// a firmware's skin, decoded: { id, portrait, landscape, dim, theme } or null (id: "rfm1:<sha256>")
export async function firmwareSkin(sections, id) {
  const p = parseSkin(sections);
  if (!p) return null;
  const [portrait, landscape] = await Promise.all([p.background ? decodeImage(p.background) : null, p.landscape ? decodeImage(p.landscape) : null]);
  if (!portrait && !landscape && !p.theme) return null;
  return { id, portrait, landscape, dim: p.dim, theme: p.theme };
}

// the image as stored: a JPEG of the ImageBitmap (quality 0.85, as the app's)
async function jpegOf(bmp) {
  const c = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(bmp.width, bmp.height) : Object.assign(document.createElement("canvas"), { width: bmp.width, height: bmp.height });
  c.getContext("2d").drawImage(bmp, 0, 0);
  if (c.convertToBlob) return c.convertToBlob({ type: "image/jpeg", quality: 0.85 });
  return new Promise((res) => c.toBlob(res, "image/jpeg", 0.85));
}

// The visitor's background image in IndexedDB (its own database: one record, the JPEG and a stamp)
const DB = "remotefm1-skin", STORE = "skin", KEY = "background";
let dbp = null;
function db() {
  if (dbp) return dbp;
  dbp = new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") return reject(new Error("no IndexedDB"));
    const r = indexedDB.open(DB, 1);
    r.onupgradeneeded = () => r.result.createObjectStore(STORE);
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  dbp.catch(() => { dbp = null; });
  return dbp;
}
async function op(mode, fn) {
  const d = await db(), t = d.transaction(STORE, mode), r = fn(t.objectStore(STORE));
  return new Promise((resolve, reject) => {
    t.oncomplete = () => resolve(r && "result" in r ? r.result : undefined);
    t.onerror = () => reject(t.error); t.onabort = () => reject(t.error || new Error("aborted"));
  });
}

// -> { id, portrait } | null: the stored image, decoded
export async function loadUserSkin() {
  try {
    const rec = await op("readonly", (s) => s.get(KEY));
    if (!rec || !rec.jpeg) return null;
    const bmp = await decodeImage(rec.jpeg, Infinity);
    return bmp ? { id: `user:${rec.stamp}`, portrait: bmp, landscape: null } : null;
  } catch (_) { return null; }
}
// a file the visitor picked: scaled, stored, decoded. -> { skin, kept } ({ id, portrait }; kept: false when this
// browser can't keep it, so it shows until the page closes); null when it isn't an image this browser can read
export async function setUserImage(file) {
  const bmp = await decodeImage(file, Infinity);
  if (!bmp) return null;
  const stamp = Date.now();
  let kept = false;
  try {
    const jpeg = await jpegOf(bmp);   // (made before the transaction: one closes on an await)
    await op("readwrite", (s) => s.put({ jpeg, stamp }, KEY));
    kept = true;
  } catch (_) { kept = false; }
  return { skin: { id: `user:${stamp}`, portrait: bmp, landscape: null }, kept };
}
export async function removeUserImage() { try { await op("readwrite", (s) => s.delete(KEY)); } catch (_) { /* (nothing kept) */ } }
