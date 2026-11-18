// SPDX-License-Identifier: MPL-2.0
// The Ableton Move sequencer's 32 projects in this browser (rfm1cs-abletonmove's README.md, "Projects"), kept apart from the
// firmwares' saves (js/saves.js): IndexedDB "remotefm1-move", store "projects", one record a slot
// { slot 0..31, bytes: the core's blob ("RMSQ"), saved: ISO date }. Not per firmware or device: a project is MIDI.
// Every write goes through one queue, so a save, a load and a delete of the same slot happen in the order asked.
// No DOM: Node runs it against a fake IndexedDB (web/site/test.mjs).
//
// THE FILE (Backup / Import): rfm1cs-abletonmove's project file (store.h, "The project file": "RMVP", version 1, kind 1 one
// project .rfmmove / 2 a set .rfmmoveset, u16 count, then per entry u8 slot, u8 flags, u32 saved (Unix s), u32 len,
// the blob), packed and read by its C in the core's module (MoveCore filePack / fileUnpack), as the iOS and Android
// apps do: one file format, one reader. A file over MAX_FILE bytes is not read.
import { PROJECTS, PROJECT_MAX, FILE_MAX, FILE_ONE, FILE_SET } from "./move-core.js";
export const DB = "remotefm1-move", STORE = "projects", VERSION = 1;
export const SLOTS = PROJECTS, BLOB_MAX = PROJECT_MAX;   // (RFM_MOVE_PROJECT_MAX)
export const KIND_ONE = FILE_ONE, KIND_SET = FILE_SET, MAX_FILE = FILE_MAX;
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

// the file's bytes for these records ({ slot, bytes, saved }); core: a MoveCore (its module packs it)
export function packFile(core, recs, kind = recs.length === 1 ? KIND_ONE : KIND_SET) {
  return core.filePack(recs.map((r) => {
    const t = Date.parse(r.saved || "");
    return { slot: r.slot, bytes: r.bytes, saved: Number.isFinite(t) ? Math.max(0, Math.floor(t / 1000)) : 0 };
  }), kind);
}
// a file's bytes -> { kind, entries: [{ slot, bytes, saved }] }, every blob checked by the core; throws an Error that
// says what is wrong ("Not a Move project file: ...")
export function unpackFile(core, bytes) {
  const f = core.fileUnpack(bytes instanceof Uint8Array ? bytes : null, "page");
  if (f.error) throw new Error(f.error);
  return { kind: f.kind, entries: f.entries.map((e) => ({ slot: e.slot, bytes: e.bytes, saved: e.saved ? new Date(e.saved * 1000).toISOString() : new Date().toISOString() })) };
}
