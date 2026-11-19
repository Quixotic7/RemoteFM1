// SPDX-License-Identifier: GPL-3.0-only
// The firmware browser: RemoteFM1 for iOS's Firmware panel on the web (RFMKit FirmwareTiles.swift: FirmwareThumb,
// FirmwareTileGrid, FantasyTag, ChipPicker), shared by remotefm1.com (js/sheet.js, the settings sheet's Firmware page)
// and the Ableton Move's page (RemoteFM14Move web/move/js/movebrowse.js). This file is the same in both repositories
// (RemoteFM1 web/site/js/fw-browser.js is the original; RemoteFM14Move keeps a copy in web/move/js/): each host gives
// it its items through a small adapter.
//   - a grid of square tiles (96..150 px): the thumbnail, the name and version under it (wrapped, never cut off), the
//     playing one outlined in the accent with a check; FANTASY on a Fantasy CFW's thumbnail, a heart on a favourite,
//     UPDATE on one with a newer version, a download arrow on one not installed, a ring while it downloads
//   - the thumbnail: RemoteFM1FW's catalog image (catalog/catalog.json's "image", the owner's screenshots, as the iOS
//     app shows them), else the host's 64 x 64 1-bit one (the Move catalog's "thumb": drawn crisp, pixelated, in the
//     theme's colours), else the firmware's name on a colour of its own (the iOS app's hue from its id). Drawn only as
//     a tile comes near the view (IntersectionObserver), each image once per page (the bitmaps cached as data URLs)
//   - the filters: a search field; Favourites or All (one choice; with no favourites, All shows by itself and the
//     choice is kept); Installed / Not installed where the host installs; one category at a time, and Fantasy CFW
//     The choices are kept per host in localStorage.
//   new FwBrowser({ items, onPick, ... }) -> .el (mount it), .refresh() (the items again), .filters (the choices)
//   items(): [{ id, name, version, author?, tag?, desc?, categories: [], fantasy?, fav?, installed? (a version, "" none,
//             undefined: the host has no notion of it), update? (bool), current?, image? (a URL), thumb? (base64 or
//             Uint8Array, 512 bytes), note? (a short line under the version), busy? (0..100: downloading) }]
export const FANTASY = "Fantasy CFW";
const FW_CATALOG = "https://raw.githubusercontent.com/Quixotic7/RemoteFM1FW/main/catalog/";
const IMG_KEY = "fwb.images.v1", IMG_TTL = 6 * 3600 * 1000;

// ---- the DOM (no dependency on either host's helpers)
function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === "class") e.className = v;
    else if (k === "html") e.innerHTML = v;
    else if (k === "style" && typeof v === "object") { for (const [p, x] of Object.entries(v)) if (x != null) e.style.setProperty(p, String(x)); }
    else if (k.startsWith("on") && typeof v === "function") e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v === true ? "" : String(v));
  }
  for (const c of kids.flat(Infinity)) if (c != null && c !== false) e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return e;
}
const ICON = {
  search: '<circle cx="7" cy="7" r="4.5"/><path d="M10.2 10.2 13.5 13.5"/>',
  check: '<path d="M4 8.4l2.7 2.6L12 5.6"/>', down: '<path d="M8 3v8M4.5 7.8 8 11.2l3.5-3.4M4 13.5h8"/>',
  heart: '<path d="M8 13.2S2.5 9.9 2.5 6.1A2.9 2.9 0 0 1 8 4.8a2.9 2.9 0 0 1 5.5 1.3C13.5 9.9 8 13.2 8 13.2z"/>',
  up: '<path d="M8 13V5M4.5 8.2 8 4.8l3.5 3.4M4 2.5h8"/>', close: '<path d="M4.5 4.5l7 7M11.5 4.5l-7 7"/>',
};
const icon = (name, cls = "fb-ic", fill = false) =>
  el("span", { class: cls, "aria-hidden": "true", html: `<svg viewBox="0 0 16 16" fill="${fill ? "currentColor" : "none"}" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${ICON[name]}</svg>` });

// ---- the stand-in: the name on a colour of its own (FirmwareThumb.hue: djb2 of the id, mod 360; HSB 0.55/0.55 to
// 0.7/0.28, as HSL)
export function hue(s) {
  let h = 5381;
  for (const ch of String(s)) h = (Math.imul(h, 33) + ch.codePointAt(0)) >>> 0;
  return h % 360;
}
const hsb = (h, s, b) => { const l = b * (1 - s / 2), sl = l === 0 || l === 1 ? 0 : (b - l) / Math.min(l, 1 - l); return `hsl(${h} ${Math.round(sl * 100)}% ${Math.round(l * 100)}%)`; };
export const standIn = (id) => { const h = hue(id); return `linear-gradient(135deg, ${hsb(h, 0.55, 0.55)}, ${hsb(h, 0.7, 0.28)})`; };

// ---- the 1-bit thumbnails: 64 x 64, rows top first, MSB left (the Move catalog's "thumb", rfm1cs RFM_MOVE_FW_THUMB)
export function thumbBits(t) {
  if (!t) return null;
  if (t instanceof Uint8Array) return t.length >= 512 ? t : null;
  try { const s = atob(String(t)); if (s.length < 512) return null; const b = new Uint8Array(512); for (let i = 0; i < 512; i++) b[i] = s.charCodeAt(i); return b; }
  catch (_) { return null; }
}
const bitsCache = new Map();
export function bitsUrl(bits, fg, bg) {
  const key = `${fg}|${bg}|${Array.prototype.join.call(bits.subarray(0, 512), ",")}`;
  let u = bitsCache.get(key);
  if (u) return u;
  const c = document.createElement("canvas");
  c.width = c.height = 64;
  const g = c.getContext("2d"), img = g.createImageData(64, 64), d = img.data;
  const rgb = (css) => { g.fillStyle = "#000"; g.fillStyle = css; const m = /^#([0-9a-f]{6})$/i.exec(g.fillStyle); const n = m ? parseInt(m[1], 16) : 0; return [n >> 16, n >> 8 & 255, n & 255]; };
  const F = rgb(fg), B = rgb(bg);
  for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) {
    const on = bits[y * 8 + (x >> 3)] >> (7 - (x & 7)) & 1, o = (y * 64 + x) * 4, p = on ? F : B;
    d[o] = p[0]; d[o + 1] = p[1]; d[o + 2] = p[2]; d[o + 3] = 255;
  }
  g.putImageData(img, 0, 0);
  u = c.toDataURL("image/png");
  bitsCache.set(key, u);
  return u;
}

// ---- RemoteFM1FW's catalog images by firmware id (catalog/SCHEMA.md "image", relative to catalog/): fetched once a
// page, kept 6 hours in localStorage; a failure (offline) leaves the other thumbnails
let imagesP = null;
export function catalogImages(fetchFn = (u, o) => fetch(u, o)) {
  if (imagesP) return imagesP;
  imagesP = (async () => {
    try { const k = JSON.parse(localStorage.getItem(IMG_KEY) || "null"); if (k && Date.now() - k.t < IMG_TTL && k.map) return new Map(Object.entries(k.map)); } catch (_) {}
    try {
      const r = await fetchFn(FW_CATALOG + "catalog.json", { cache: "no-cache" });
      if (!r.ok) return new Map();
      const j = await r.json(), map = {};
      for (const [id, f] of Object.entries((j && j.firmwares) || {})) {
        if (!f || typeof f.image !== "string" || !f.image) continue;
        try { const u = new URL(f.image, FW_CATALOG); if (u.protocol === "https:") map[id] = u.href; } catch (_) {}
      }
      try { localStorage.setItem(IMG_KEY, JSON.stringify({ t: Date.now(), map })); } catch (_) {}
      return new Map(Object.entries(map));
    } catch (_) { return new Map(); }
  })();
  return imagesP;
}
const badImages = new Set();   // (an image that failed to load: not asked for again this page)

// ---- the browser
export class FwBrowser {
  // o: { items(), onPick(item), onFav?(item) (the heart can be tapped: else favourites only show), favourites (bool:
  //      the Favourites / All choice), install (bool: the Installed / Not installed choices), categories ([names],
  //      "All" first), fantasy (bool: the Fantasy CFW choice), key (localStorage name), images (bool: RemoteFM1FW's
  //      catalog images), fetch, label (what the tile's tap does: (item) => "Load", for its title), extra (() =>
  //      [elements] after the tiles), empty ("No firmware matches"), searchFocus (bool) }
  constructor(o) {
    this.o = o;
    this.key = o.key || "fwb";
    const kept = (() => { try { return JSON.parse(localStorage.getItem(this.key) || "{}") || {}; } catch (_) { return {}; } })();
    this.filters = { q: "", scope: kept.scope === "fav" ? "fav" : "all", inst: ["in", "out"].includes(kept.inst) ? kept.inst : "any", cat: kept.cat || "All" };
    this.images = new Map();
    this.io = typeof IntersectionObserver === "function"
      ? new IntersectionObserver((es) => { for (const e of es) if (e.isIntersecting) { this.io.unobserve(e.target); this.paint(e.target); } }, { rootMargin: "240px 0px" })
      : null;
    this.search = el("input", { type: "search", class: "fb-q", "aria-label": "Search firmware", autocomplete: "off", spellcheck: "false",
      oninput: () => { this.filters.q = this.search.value; this.fill(); } });
    this.bar = el("div", { class: "fb-bar" });
    this.grid = el("div", { class: "fb-grid", role: "list" });
    this.more = el("div", { class: "fb-more" });
    this.el = el("div", { class: "fb" }, el("label", { class: "fb-search" }, icon("search"), this.search), this.bar, this.grid, this.more);
    if (o.images !== false) catalogImages(o.fetch).then((m) => { this.images = m; if (m.size) this.repaint(); });
    this.refresh();
  }
  keep() { try { localStorage.setItem(this.key, JSON.stringify({ scope: this.filters.scope, inst: this.filters.inst, cat: this.filters.cat })); } catch (_) {} }
  refresh() { this.all = (this.o.items() || []).filter(Boolean); this.fill(); }

  // ---- the filters: the chips (one choice per row), then the tiles that pass
  passes(f, skip) {
    const F = this.filters, q = F.q.trim().toLowerCase();
    if (q && ![f.name, f.id, f.author, f.tag, ...(f.categories || [])].some((x) => x && String(x).toLowerCase().includes(q))) return false;
    if (skip !== "scope" && this.o.favourites && this.scopeNow() === "fav" && !f.fav) return false;
    if (skip !== "inst" && this.o.install) {
      if (F.inst === "in" && !f.installed) return false;
      if (F.inst === "out" && f.installed) return false;
    }
    if (skip !== "cat" && F.cat !== "All") {
      if (F.cat === FANTASY ? !f.fantasy : !(f.categories || []).includes(F.cat)) return false;
    }
    return true;
  }
  // Favourites as chosen, All while there are none (the choice kept: the Move's all_auto)
  scopeNow() { return this.filters.scope === "fav" && this.all.some((f) => f.fav) ? "fav" : "all"; }
  chips(label, opts, cur, pick) {
    return el("div", { class: "fb-chips", role: "radiogroup", "aria-label": label }, opts.map((c) =>
      el("button", { class: `fb-chip${c.v === cur ? " on" : ""}${c.cls ? " " + c.cls : ""}`, role: "radio", "aria-checked": String(c.v === cur), disabled: c.off,
        "data-v": c.v, title: c.title || null, onclick: () => { pick(c.v); this.keep(); this.fill(); } }, c.icon || null, c.t, c.n != null ? el("b", { class: "fb-n" }, c.n) : null)));
  }
  bars() {
    const F = this.filters, rows = [], all = this.all;
    const count = (skip, test) => all.filter((f) => this.passes(f, skip) && test(f)).length;
    if (this.o.favourites || this.o.install) {
      const left = [];
      if (this.o.favourites) {
        const favs = all.filter((f) => f.fav).length;
        left.push(this.chips("Favourites or all", [
          { v: "fav", t: "Favourites", icon: icon("heart", "fb-ic", true), n: favs, off: !favs, cls: "fav", title: favs ? null : "No favourites yet" },
          { v: "all", t: "All", n: count("scope", () => true) }], this.scopeNow(), (v) => { F.scope = v; }));
      }
      if (this.o.install) {
        const ups = all.filter((f) => f.update && this.passes(f, "inst")).length;
        left.push(this.chips("Installed or not", [
          { v: "any", t: "Any" },
          { v: "in", t: "Installed", n: ups ? `${ups} update${ups === 1 ? "" : "s"}` : null, cls: ups ? "ups" : "" },
          { v: "out", t: "Not installed", n: count("inst", (f) => !f.installed) }], F.inst, (v) => { F.inst = v; }));
      }
      rows.push(el("div", { class: "fb-row" }, left));
    }
    const cats = (this.o.categories || ["All"]).filter((c) => c === "All" || all.some((f) => (f.categories || []).includes(c)));
    if (this.o.fantasy && all.some((f) => f.fantasy)) cats.push(FANTASY);
    if (!cats.includes(F.cat)) F.cat = "All";
    if (cats.length > 1) rows.push(this.chips("Category", cats.map((c) => ({ v: c, t: c, cls: c === FANTASY ? "fan" : "" })), F.cat, (v) => { F.cat = v; }));
    return rows;
  }
  fill() {
    this.bar.replaceChildren(...this.bars());
    this.search.placeholder = `Search ${this.all.length} firmware${this.all.length === 1 ? "" : "s"}`;
    const list = this.all.filter((f) => this.passes(f));
    // (a tile is kept while its item is the same: no image drawn again)
    const old = new Map([...this.grid.children].map((t) => [t.dataset.key, t]));
    const tiles = list.map((f) => { const k = tileKey(f), t = old.get(k); return t || this.tile(f, k); });
    if (!list.length) tiles.push(el("p", { class: "fb-empty" }, this.all.length ? (this.o.empty || "No firmware matches") : (this.o.none || "No firmware yet")));
    this.grid.replaceChildren(...tiles);
    this.more.replaceChildren(...((this.o.extra && this.o.extra()) || []));
  }

  // ---- a tile
  tile(f, key) {
    // (the stand-in's name sized to its longest word: a word is not broken while it can fit)
    const word = Math.max(4, ...String(f.name || "?").split(/\s+/).map((w) => w.length));
    const art = el("span", { class: "fb-art", style: { "--fb-stand": standIn(f.id || f.name), "--fb-word": word } }, el("span", { class: "fb-stand" }, f.name || "?"));
    const marks = [];
    if (f.fantasy) marks.push(el("span", { class: "fb-fan-tag", title: "Fantasy CFW: runs in RemoteFM1 hosts only, not on an FM-1" }, "Fantasy"));
    if (f.current) marks.push(el("span", { class: "fb-mark fb-cur", title: "Playing" }, icon("check")));
    else if (f.busy != null) marks.push(el("span", { class: "fb-ring", style: { "--p": `${Math.max(2, Math.min(100, f.busy))}%` }, title: `Downloading ${f.busy | 0} %` }));
    else if (f.update) marks.push(el("span", { class: "fb-mark fb-up", title: `Update: ${f.installed} here, ${f.version} in the catalog` }, icon("up"), "Update"));
    else if (f.installed === "") marks.push(el("span", { class: "fb-mark fb-get", title: "Not installed: a tap downloads it" }, icon("down")));
    if (f.fav && !this.o.onFav) marks.push(el("span", { class: "fb-heart", title: "A favourite" }, icon("heart", "fb-ic", true)));
    const ver = [f.update ? `${f.installed} → ${f.version}` : f.version, f.note].filter(Boolean).join(" · ");
    const what = this.o.label ? this.o.label(f) : "";
    const main = el("button", { class: "fb-main", "data-id": f.id, onclick: () => this.o.onPick && this.o.onPick(f),
      title: [`${f.name} ${f.version}${f.author ? ` · ${f.author}` : ""}`, f.tag, f.desc, what].filter(Boolean).join("\n"),
      "aria-label": `${f.name} ${f.version}${f.current ? ", playing" : ""}${f.fantasy ? ", Fantasy CFW" : ""}${f.fav ? ", favourite" : ""}${f.update ? ", update available" : f.installed === "" ? ", not installed" : ""}` },
      el("span", { class: "fb-thumb" }, art, marks),
      el("span", { class: "fb-name" }, f.name),
      ver ? el("span", { class: "fb-ver" }, ver) : null);
    const t = el("div", { class: `fb-tile${f.current ? " current" : ""}${f.installed === "" ? " missing" : ""}${f.busy != null ? " busy" : ""}`, role: "listitem", "data-key": key, "data-id": f.id }, main);
    if (this.o.onFav) t.append(el("button", { class: `fb-fav${f.fav ? " on" : ""}`, "aria-pressed": String(!!f.fav), "aria-label": `${f.fav ? "Remove" : "Add"} ${f.name} ${f.fav ? "from" : "to"} favourites`,
      title: f.fav ? "A favourite (tap to remove)" : "Add to favourites", onclick: (e) => { e.stopPropagation(); this.o.onFav(f); } }, icon("heart", "fb-ic", true)));
    t._item = f;
    if (this.io) this.io.observe(t); else this.paint(t);
    return t;
  }
  // the thumbnail, once the tile is near the view: the catalog image, else the 1-bit one, else the stand-in stays
  paint(t) {
    const f = t._item, art = t.querySelector(".fb-art");
    if (!f || !art || art.dataset.painted) return;
    const url = f.image || this.images.get(f.id), bits = thumbBits(f.thumb);
    const put = (src, cls) => {
      const img = el("img", { class: cls, alt: "", decoding: "async", draggable: "false" });
      img.onload = () => { art.classList.add("drawn"); img.classList.add("in"); };
      img.onerror = () => { badImages.add(src); img.remove(); if (cls === "fb-img" && bits) put(this.bitsSrc(bits), "fb-img fb-bits"); };
      img.src = src;
      art.append(img);
    };
    if (url && !badImages.has(url)) { art.dataset.painted = "img"; put(url, "fb-img"); }
    else if (bits) { art.dataset.painted = "bits"; put(this.bitsSrc(bits), "fb-img fb-bits"); }
  }
  bitsSrc(bits) {
    const cs = getComputedStyle(this.el);
    return bitsUrl(bits, cs.getPropertyValue("--fb-px").trim() || "#F3EBDC", cs.getPropertyValue("--fb-px-bg").trim() || "#141210");
  }
  // the catalog images came: tiles drawn without one get it
  repaint() {
    for (const t of this.grid.children) {
      const f = t._item, art = t.querySelector && t.querySelector(".fb-art");
      if (!f || !art || art.dataset.painted === "img" || f.image || !this.images.get(f.id)) continue;
      art.querySelectorAll("img").forEach((i) => i.remove());
      art.classList.remove("drawn");
      delete art.dataset.painted;
      if (this.io) { this.io.unobserve(t); this.io.observe(t); } else this.paint(t);
    }
  }
  focus() { if (matchMedia("(pointer: fine)").matches) this.search.focus({ preventScroll: true }); }
}
// a tile is drawn again when what it shows changed
const tileKey = (f) => JSON.stringify([f.id, f.name, f.version, f.installed, !!f.update, !!f.current, !!f.fav, f.busy ?? null, !!f.fantasy, f.note || "", f.image || ""]);
