// SPDX-License-Identifier: MPL-2.0
// The Ableton Move sequencer's 32 projects in this browser (host/src/move/README.md, "Projects"), kept apart from the
// firmwares' saves (js/saves.js): IndexedDB "remotefm1-move", store "projects", one record a slot
// { slot 0..31, bytes: the core's blob ("RMSQ"), saved: ISO date }. Not per firmware or device: a project is MIDI.
// Every write goes through one queue, so a save, a load and a delete of the same slot happen in the order asked.
// No DOM: Node runs it against a fake IndexedDB (web/site/test.mjs).
//
// THE FILE (Backup / Import): a small container, little-endian,
//   "RMVP"  magic (4 bytes)
//   u8      version (1)
//   u8      kind: 1 one project (.rfmmove), 2 a set (.rfmmoveset)
//   u16     count: 1..32 entries
//   then each entry: u8 slot (0..31, unique), u8 flags (0), u32 saved (Unix seconds, 0 unknown), u32 len, len bytes:
//   the core's project blob as rfm_move_project_save writes it (checked with rfm_move_project_check on import)
// and nothing after the last entry. A file over MAX_FILE bytes is not read.
export const DB = "remotefm1-move", STORE = "projects", VERSION = 1;
export const SLOTS = 32, BLOB_MAX = 39072;   // (RFM_MOVE_PROJECT_MAX)
export const MAGIC = [0x52, 0x4d, 0x56, 0x50], FILE_VERSION = 1, KIND_ONE = 1, KIND_SET = 2;
export const HEAD = 8, ENTRY = 10;
export const MAX_FILE = HEAD + SLOTS * (ENTRY + BLOB_MAX);
export const EXT_ONE = ".rfmmove", EXT_SET = ".rfmmoveset";

export class MoveProjects {
  constructor(idb = globalThis.indexedDB) { this.idb = idb; this.dbp = null; this.chain = Promise.resolve(); }
  db() {
    if (this.dbp) return this.dbp;
    this.dbp = new Promise((resolve, reject) => {
      if (!this.idb) return reject(new Error("no IndexedDB"));
      const r = this.idb.open(DB, VERSION);
      r.onupgradeneeded = () => r.result.createObjectStore(STORE, { keyPath: "slot" });
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => reject(r.error);
    });
    this.dbp.catch(() => { this.dbp = null; });
    return this.dbp;
  }
  // one at a time, in order
  queue(fn) { const p = this.chain.then(fn, fn); this.chain = p.catch(() => {}); return p; }
  async tx(mode, fn) {
    const d = await this.db(), t = d.transaction(STORE, mode), s = t.objectStore(STORE);
    const r = fn(s);
    return new Promise((resolve, reject) => {
      t.oncomplete = () => resolve(r && "result" in r ? r.result : undefined);
      t.onerror = () => reject(t.error); t.onabort = () => reject(t.error || new Error("aborted"));
    });
  }
  put(slot, bytes, saved = new Date().toISOString()) {
    if (!validSlot(slot)) return Promise.reject(new Error(`no slot ${slot}`));
    const rec = { slot, bytes: new Uint8Array(bytes), saved };
    return this.queue(() => this.tx("readwrite", (s) => s.put(rec))).then(() => rec);
  }
  get(slot) { return this.queue(() => this.tx("readonly", (s) => s.get(slot))).then((r) => r || null, () => null); }
  remove(slot) { return this.queue(() => this.tx("readwrite", (s) => s.delete(slot))); }
  // every project, by slot ([] when the store can't be opened)
  list() { return this.queue(() => this.tx("readonly", (s) => s.getAll())).then((a) => (a || []).sort((x, y) => x.slot - y.slot), () => []); }
  async dir() { return dirBits(await this.list()); }
}
export const validSlot = (s) => Number.isInteger(s) && s >= 0 && s < SLOTS;
export const dirBits = (recs) => recs.reduce((b, r) => (validSlot(r.slot) ? (b | (1 << r.slot)) >>> 0 : b), 0);
export const projectName = (slot) => `Project ${slot + 1}`;
// a project blob with its colour (byte 18, 0..16) set and its FNV-1a check (the last 4 bytes) made again
export function withColor(bytes, color) {
  const b = Uint8Array.from(bytes), n = b.length - 4;
  b[18] = color & 255;
  let h = 2166136261;
  for (let i = 0; i < n; i++) h = Math.imul(h ^ b[i], 16777619) >>> 0;
  b[n] = h & 255; b[n + 1] = (h >>> 8) & 255; b[n + 2] = (h >>> 16) & 255; b[n + 3] = (h >>> 24) & 255;
  return b;
}
export const fileName = (slot) => `move-project-${String(slot + 1).padStart(2, "0")}${EXT_ONE}`;
export const SET_NAME = `move-projects${EXT_SET}`;
export const firstFree = (bits) => { for (let s = 0; s < SLOTS; s++) if (!(bits >>> s & 1)) return s; return -1; };

// the file's bytes for these records ({ slot, bytes, saved })
export function packFile(recs, kind = recs.length === 1 ? KIND_ONE : KIND_SET) {
  const n = recs.reduce((a, r) => a + ENTRY + r.bytes.length, HEAD), out = new Uint8Array(n), dv = new DataView(out.buffer);
  out.set(MAGIC, 0); out[4] = FILE_VERSION; out[5] = kind; dv.setUint16(6, recs.length, true);
  let o = HEAD;
  for (const r of recs) {
    const t = Date.parse(r.saved || "");
    out[o] = r.slot; out[o + 1] = 0;
    dv.setUint32(o + 2, Number.isFinite(t) ? Math.max(0, Math.floor(t / 1000)) : 0, true);
    dv.setUint32(o + 6, r.bytes.length, true);
    out.set(r.bytes, o + ENTRY);
    o += ENTRY + r.bytes.length;
  }
  return out;
}
// a file's bytes -> { kind, entries: [{ slot, bytes, saved }] }; throws an Error that says what is wrong.
// check(blob) -> 0 for a valid project (the core's rfm_move_project_check)
export function unpackFile(bytes, check) {
  const bad = (why) => { throw new Error(`Not a Move project file: ${why}.`); };
  if (!bytes || !(bytes instanceof Uint8Array)) bad("nothing to read");
  if (bytes.length > MAX_FILE) bad(`it is ${bytes.length} bytes, more than a full set (${MAX_FILE})`);
  if (bytes.length < HEAD || MAGIC.some((b, i) => bytes[i] !== b)) bad("it does not start with RMVP");
  if (bytes[4] !== FILE_VERSION) bad(`version ${bytes[4]}; this page reads version ${FILE_VERSION}`);
  const kind = bytes[5], dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), count = dv.getUint16(6, true);
  if (kind !== KIND_ONE && kind !== KIND_SET) bad(`kind ${kind}`);
  if (count < 1 || count > SLOTS || (kind === KIND_ONE && count !== 1)) bad(`${count} projects`);
  const entries = [], seen = new Set();
  let o = HEAD;
  for (let i = 0; i < count; i++) {
    if (o + ENTRY > bytes.length) bad(`project ${i + 1} is cut off`);
    const slot = bytes[o], t = dv.getUint32(o + 2, true), len = dv.getUint32(o + 6, true);
    if (!validSlot(slot) || seen.has(slot)) bad(`slot ${slot + 1} ${seen.has(slot) ? "twice" : "out of range"}`);
    if (len < 1 || len > BLOB_MAX) bad(`project ${i + 1} is ${len} bytes`);
    if (o + ENTRY + len > bytes.length) bad(`project ${i + 1} is cut off`);
    const blob = bytes.slice(o + ENTRY, o + ENTRY + len);
    let r;
    try { r = check(blob); } catch (_) { r = -1; }
    if (r !== 0) bad(`project ${i + 1} (slot ${slot + 1}) is damaged`);
    seen.add(slot);
    entries.push({ slot, bytes: blob, saved: t ? new Date(t * 1000).toISOString() : new Date().toISOString() });
    o += ENTRY + len;
  }
  if (o !== bytes.length) bad(`${bytes.length - o} bytes after the last project`);
  return { kind, entries };
}
