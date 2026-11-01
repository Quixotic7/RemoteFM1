// SPDX-License-Identifier: GPL-3.0-only
// The RemoteFM1 folder (FM1VST docs/VIRTUAL-CARTS.md section 3): the files a Fantasy CFW reads and writes, the page's
// side of its file API (engines/rfm1/engine.js FileApi; the worklet posts { type: "fs", ... }, serve() answers).
//   RemoteFM1/audio/        shared samples, every cart reads them
//   RemoteFM1/code/<id>/    a cart's scripts (ripple: code/ripple/droplets/*.lua)
//   RemoteFM1/data/<id>/    the cart's own files, the only place it writes
// Where it lives:
//   "picker" (Chrome, Edge): a folder the visitor picks (showDirectoryPicker), its handle kept in IndexedDB; on a later
//            visit the browser wants a click to allow it again (state "ask": requests end in FS_ENOFOLDER until then)
//   "opfs"   (Safari, Firefox): this site's own storage (navigator.storage.getDirectory()/RemoteFM1/); Add files copies
//            files in (their relative paths kept), Export data downloads data/ as a zip
// /audio reads are cached whole on the first READ (an LRU of 256 MB), so the rest of a sample answers at once.
//
//   const f = new Folder(); await f.ready;
//   f.state            "none" | "ask" | "ready" | "unsupported"     f.name   the folder's name
//   f.on((kind) => {})  kind: "state" (for the UI) | "changed" (the files changed: the cart's fs_changed moves)
//   await f.pick() / f.allow()                        (picker; in a click)
//   await f.addFiles([{ file, path }], cartId)        (copies in; path relative, e.g. "droplets/a.lua")
//   await f.exportData()                              (data/ as RemoteFM1-data-<date>.zip)
//   await f.serve(msg) -> { reply, transfer }

const DB = "remotefm1-folder", STORE = "handles", KEY = "root";
const E = { ENOENT: -2, EACCES: -3, EIO: -4, ENOSPC: -5, ENOFOLDER: -8 };
const ROOTS = ["audio", "code", "data"];
const CACHE_CAP = 256 * 1048576, CACHE_FILE_MAX = 64 * 1048576;
const AUDIO_EXT = /\.(wav|wave|aif|aiff|aifc)$/i;

export const pickerSupported = () => typeof window !== "undefined" && typeof window.showDirectoryPicker === "function";
const opfsSupported = () => typeof navigator !== "undefined" && !!(navigator.storage && navigator.storage.getDirectory);

// ---- the handle in IndexedDB
function idb() {
  return new Promise((resolve, reject) => {
    const r = indexedDB.open(DB, 1);
    r.onupgradeneeded = () => r.result.createObjectStore(STORE);
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}
async function idbDo(mode, fn) {
  const d = await idb();
  try {
    return await new Promise((resolve, reject) => {
      const t = d.transaction(STORE, mode), r = fn(t.objectStore(STORE));
      t.oncomplete = () => resolve(r && r.result); t.onerror = () => reject(t.error);
    });
  } finally { d.close(); }
}

// the engine's folder path, checked again here: nothing outside audio/, code/<id>/, data/<id>/; writes in data/ only
export function folderSegments(p, write) {
  if (typeof p !== "string" || !p) return null;
  const seg = p.split("/");
  if (!ROOTS.includes(seg[0]) || (seg[0] !== "audio" && seg.length < 2)) return null;
  if (seg.some((x) => !x || x === "." || x === ".." || x.includes("\\") || x.includes("\0"))) return null;
  if (write && (seg[0] !== "data" || seg.length < 3)) return null;
  return seg;
}

const errOf = (e) => {
  const n = e && e.name;
  return n === "NotFoundError" ? E.ENOENT : n === "NotAllowedError" || n === "SecurityError" ? E.ENOFOLDER : n === "QuotaExceededError" ? E.ENOSPC : E.EIO;
};

export class Folder {
  constructor() {
    this.mode = pickerSupported() ? "picker" : opfsSupported() ? "opfs" : "none";
    this.root = null; this.state = "none"; this.name = "";
    this.subs = new Set();
    this.cache = new Map();     // audio path -> { stamp, buf } (Map order: least recently used first)
    this.cacheBytes = 0;
    this.loading = new Map();   // audio path + stamp -> Promise<ArrayBuffer> (one read per file at a time)
    this.ready = this.init().catch((e) => { console.warn("folder:", e); this.setState("none"); });
  }
  on(fn) { this.subs.add(fn); return () => this.subs.delete(fn); }
  emit(kind) { for (const f of this.subs) { try { f(kind); } catch (e) { console.warn(e); } } }
  setState(s) { this.state = s; this.emit("state"); }
  changed() { this.dropCache(); this.emit("changed"); }

  async init() {
    if (this.mode === "opfs") return this.useOpfs();
    if (this.mode !== "picker") return this.setState("unsupported");
    const h = await idbDo("readonly", (s) => s.get(KEY)).catch(() => null);
    if (!h) return this.setState("none");
    this.root = h; this.name = h.name;
    const p = await h.queryPermission({ mode: "readwrite" }).catch(() => "prompt");
    this.setState(p === "granted" ? "ready" : "ask");
  }
  // this site's own storage (Safari, Firefox; and a test in Chromium)
  async useOpfs() {
    this.mode = "opfs";
    this.root = await (await navigator.storage.getDirectory()).getDirectoryHandle("RemoteFM1", { create: true });
    this.name = "RemoteFM1";
    this.setState("ready");
    this.changed();
  }
  // (picker) in a click: the visitor's folder
  async pick() {
    let h;
    try { h = await window.showDirectoryPicker({ id: "remotefm1", mode: "readwrite" }); }
    catch (e) { if (e && e.name === "AbortError") return false; throw e; }
    await idbDo("readwrite", (s) => s.put(h, KEY)).catch(() => { /* (kept for this visit only) */ });
    this.root = h; this.name = h.name;
    const p = await h.queryPermission({ mode: "readwrite" }).catch(() => "granted");
    this.setState(p === "granted" ? "ready" : "ask");
    this.changed();
    return true;
  }
  // (picker) in a click: the folder picked on an earlier visit, allowed again
  async allow() {
    if (!this.root) return this.pick();
    const p = await this.root.requestPermission({ mode: "readwrite" }).catch(() => "denied");
    this.setState(p === "granted" ? "ready" : "ask");
    if (p === "granted") this.changed();
    return p === "granted";
  }

  // ---- the file API's requests
  async dirAt(seg, create = false) {
    let d = this.root;
    for (const s of seg) d = await d.getDirectoryHandle(s, { create });
    return d;
  }
  async serve(m) {
    const reply = { type: "fsDone", key: m.key, id: m.id, result: E.EIO }, transfer = [];
    const out = () => ({ reply, transfer });
    if (this.state !== "ready" || !this.root) { reply.result = E.ENOFOLDER; return out(); }
    const write = m.op === 4 || m.op === 5 || m.op === 6;
    const seg = folderSegments(m.path, write);
    if (!seg) { reply.result = E.EACCES; return out(); }
    try {
      const base = seg[0] === "audio" ? 1 : 2;
      await this.dirAt(seg.slice(0, base), true);                  // audio/, code/<id>/, data/<id>/: on demand
      const isRoot = seg.length === base, name = seg[seg.length - 1];
      const parent = isRoot ? null : await this.dirAt(seg.slice(0, -1));
      const entry = async () => {                                  // the file or folder at the path
        if (isRoot) return this.dirAt(seg);
        try { return await parent.getFileHandle(name); }
        catch (e) { if (e && e.name === "TypeMismatchError") return parent.getDirectoryHandle(name); throw e; }
      };
      if (m.op === 1) {                                            // STAT
        const h = await entry();
        if (h.kind === "directory") reply.stat = { size: 0, mtime: 0, dir: true };
        else { const f = await h.getFile(); reply.stat = { size: f.size, mtime: f.lastModified, dir: false }; }
        reply.result = 16;
      } else if (m.op === 2) {                                     // LIST (the engine sorts and pages)
        const d = isRoot ? await this.dirAt(seg) : await parent.getDirectoryHandle(name);
        reply.entries = [];
        for await (const [n, h] of d.entries()) reply.entries.push([n, h.kind === "directory"]);
        reply.result = 0;
      } else if (m.op === 3) {                                     // READ
        if (isRoot) throw Object.assign(new Error("a folder"), { name: "TypeMismatchError" });
        const f = await (await parent.getFileHandle(name)).getFile();
        const end = Math.min(f.size, m.offset + m.len);
        let data;
        if (m.offset >= f.size) data = new ArrayBuffer(0);
        else if (seg[0] === "audio" && f.size <= CACHE_FILE_MAX) data = (await this.cached(m.path, f)).slice(m.offset, end);
        else data = await f.slice(m.offset, end).arrayBuffer();
        reply.data = data; transfer.push(data);
        reply.result = data.byteLength;
      } else if (m.op === 4) {                                     // WRITE (offset -1: truncate and write)
        const fh = await parent.getFileHandle(name, { create: true });
        const w = await fh.createWritable({ keepExistingData: m.offset >= 0 });
        try { await w.write(m.offset >= 0 ? { type: "write", position: m.offset, data: m.data || new ArrayBuffer(0) } : (m.data || new ArrayBuffer(0))); }
        catch (e) { await w.abort().catch(() => {}); throw e; }
        await w.close();
        reply.result = m.data ? m.data.byteLength : 0;
      } else if (m.op === 5) {                                     // DELETE (a file or an empty folder)
        await entry();
        await parent.removeEntry(name);
        reply.result = 0;
      } else if (m.op === 6) {                                     // MKDIR (there already: fine)
        await parent.getDirectoryHandle(name, { create: true });
        reply.result = 0;
      }
    } catch (e) {
      reply.result = errOf(e);
      if (reply.result === E.ENOFOLDER && this.mode === "picker") this.setState("ask");   // (the permission went)
    }
    return out();
  }

  // ---- /audio: whole files, least recently used out first
  async cached(path, file) {
    const stamp = `${file.size}:${file.lastModified}`, hit = this.cache.get(path);
    if (hit && hit.stamp === stamp) { this.cache.delete(path); this.cache.set(path, hit); return hit.buf; }
    const k = path + "|" + stamp;
    if (!this.loading.has(k)) {
      this.loading.set(k, file.arrayBuffer().then((buf) => {
        const old = this.cache.get(path);
        if (old) { this.cache.delete(path); this.cacheBytes -= old.buf.byteLength; }
        this.cache.set(path, { stamp, buf });
        this.cacheBytes += buf.byteLength;
        for (const [p, v] of this.cache) { if (this.cacheBytes <= CACHE_CAP || p === path) break; this.cache.delete(p); this.cacheBytes -= v.buf.byteLength; }
        return buf;
      }).finally(() => this.loading.delete(k)));
    }
    return this.loading.get(k);
  }
  dropCache() { this.cache.clear(); this.cacheBytes = 0; }

  // ---- Add files (this site's storage; a picked folder too): each { file, path } copied in, its path kept. A path
  // already under audio/, code/ or data/ (or RemoteFM1/...) stays; otherwise samples go to audio/, the rest to the
  // running cart's code/<id>/ (a dropped "droplets" folder: code/ripple/droplets/). Hidden files are skipped.
  async addFiles(items, cartId) {
    if (this.state !== "ready") throw new Error("No RemoteFM1 folder yet.");
    let n = 0;
    for (const { file, path } of items) {
      let seg = String(path || file.name).split("/").filter(Boolean);
      if (seg[0] === "RemoteFM1") seg = seg.slice(1);
      if (!seg.length || seg.some((s) => s.startsWith(".") || s === "..")) continue;
      if (!ROOTS.includes(seg[0])) seg = AUDIO_EXT.test(file.name) || !cartId ? ["audio", ...seg] : ["code", cartId, ...seg];
      if (seg.length < 2) continue;
      const d = await this.dirAt(seg.slice(0, -1), true);
      const w = await (await d.getFileHandle(seg[seg.length - 1], { create: true })).createWritable();
      await w.write(file);
      await w.close();
      n++;
    }
    if (n) this.changed();
    return n;
  }

  // ---- Export data: data/ (every cart's) as a zip, stored (no compression)
  async exportData() {
    if (this.state !== "ready") throw new Error("No RemoteFM1 folder yet.");
    const files = [];
    const walk = async (d, prefix) => {
      for await (const [n, h] of d.entries()) {
        if (h.kind === "directory") await walk(h, `${prefix}${n}/`);
        else { const f = await h.getFile(); files.push({ path: prefix + n, bytes: new Uint8Array(await f.arrayBuffer()), time: f.lastModified }); }
      }
    };
    await walk(await this.dirAt(["data"], true), "data/");
    const blob = zip(files);
    const a = Object.assign(document.createElement("a"), { href: URL.createObjectURL(blob), download: `RemoteFM1-data-${new Date().toISOString().slice(0, 10)}.zip` });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    return files.length;
  }
}

// files dropped on the page (folders walked: webkitGetAsEntry) -> [{ file, path }]
export async function filesFromDrop(dt) {
  const out = [], walks = [];
  const walk = (entry, prefix) => new Promise((resolve) => {
    if (entry.isFile) entry.file((file) => { out.push({ file, path: prefix + entry.name }); resolve(); }, () => resolve());
    else if (entry.isDirectory) {
      const r = entry.createReader(), kids = [];
      const more = () => r.readEntries(async (list) => {
        if (list.length) { kids.push(...list); more(); return; }
        for (const k of kids) await walk(k, `${prefix}${entry.name}/`);
        resolve();
      }, () => resolve());
      more();
    } else resolve();
  });
  const items = Array.from(dt.items || []);
  for (const it of items) { const en = it.webkitGetAsEntry && it.webkitGetAsEntry(); if (en) walks.push(walk(en, "")); }
  if (!walks.length) for (const f of Array.from(dt.files || [])) out.push({ file: f, path: f.name });
  await Promise.all(walks);
  return out;
}

// ---- a stored zip (PKZIP, method 0)
const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = (b) => { let c = 0xffffffff; for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
export function zip(files) {
  const enc = new TextEncoder(), parts = [], central = [];
  let off = 0;
  for (const f of files) {
    const name = enc.encode(f.path), crc = crc32(f.bytes), d = new Date(f.time || Date.now());
    const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
    const date = ((Math.max(1980, d.getFullYear()) - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
    const head = (sig, central) => {
      const h = new DataView(new ArrayBuffer(central ? 46 : 30));
      h.setUint32(0, sig, true);
      let o = 4;
      if (central) { h.setUint16(o, 20, true); o += 2; }
      h.setUint16(o, 20, true); h.setUint16(o + 2, 0x800, true); h.setUint16(o + 4, 0, true);   // version, UTF-8 names, stored
      h.setUint16(o + 6, time, true); h.setUint16(o + 8, date, true); h.setUint32(o + 10, crc, true);
      h.setUint32(o + 14, f.bytes.length, true); h.setUint32(o + 18, f.bytes.length, true); h.setUint16(o + 22, name.length, true);
      if (central) h.setUint32(42, off, true);
      return new Uint8Array(h.buffer);
    };
    const local = head(0x04034b50, false);
    parts.push(local, name, f.bytes);
    central.push(head(0x02014b50, true), name);
    off += local.length + name.length + f.bytes.length;
  }
  const size = central.reduce((s, b) => s + b.length, 0), end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, files.length, true); end.setUint16(10, files.length, true);
  end.setUint32(12, size, true); end.setUint32(16, off, true);
  return new Blob([...parts, ...central, new Uint8Array(end.buffer)], { type: "application/zip" });
}
