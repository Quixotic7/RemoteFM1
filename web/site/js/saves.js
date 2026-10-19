// SPDX-License-Identifier: GPL-3.0-only
// Saves: each firmware's flash, per firmware, engine and version, in IndexedDB. "Autosave" is the running flash
// (written when the firmware changes its flash); named saves are copies the visitor keeps. A backup file holds any
// number of saves (JSON, the bytes in base64): exportBackup / importBackup. A raw flash image (.bin, .fm1save) is
// imported as one named save of the current firmware.

const DB = "remotefm1-player", STORE = "saves", VERSION = 1;
export const AUTOSAVE = "Autosave";
let dbp = null;

function db() {
  if (dbp) return dbp;
  dbp = new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") return reject(new Error("no IndexedDB"));
    const r = indexedDB.open(DB, VERSION);
    r.onupgradeneeded = () => {
      const s = r.result.createObjectStore(STORE, { keyPath: "key" });
      s.createIndex("fw", "fw");
    };
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  return dbp;
}
const req = (r) => new Promise((resolve, reject) => { r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error); });
async function read(fn) { const d = await db(); return req(fn(d.transaction(STORE, "readonly").objectStore(STORE))); }
async function write(fn) {
  const d = await db(), t = d.transaction(STORE, "readwrite");
  fn(t.objectStore(STORE));
  return new Promise((resolve, reject) => { t.oncomplete = () => resolve(); t.onerror = () => reject(t.error); t.onabort = () => reject(t.error); });
}

export const keyOf = (fw, engine, version, name) => `${fw}\u0001${engine}\u0001${version}\u0001${name}`;

// a save: { key, fw, engine, version, name, bytes: Uint8Array, saved: ISO date }
export async function put(fw, engine, version, name, bytes) {
  const rec = { key: keyOf(fw, engine, version, name), fw, engine, version, name, bytes: new Uint8Array(bytes), saved: new Date().toISOString() };
  await write((s) => s.put(rec));
  return rec;
}
export async function get(fw, engine, version, name) {
  try { return (await read((s) => s.get(keyOf(fw, engine, version, name)))) || null; } catch (_) { return null; }
}
export async function remove(fw, engine, version, name) { await write((s) => s.delete(keyOf(fw, engine, version, name))); }
export async function rename(fw, engine, version, from, to) {
  const r = await get(fw, engine, version, from);
  if (!r) return null;
  await remove(fw, engine, version, from);
  return put(fw, engine, version, to, r.bytes);
}
// every save of a firmware (all engines and versions), autosaves first, then by name
export async function list(fw) {
  try {
    const all = await read((s) => s.index("fw").getAll(fw));
    return (all || []).sort((a, b) => (b.name === AUTOSAVE) - (a.name === AUTOSAVE) || a.name.localeCompare(b.name));
  } catch (_) { return []; }
}
export async function listAll() {
  try { return (await read((s) => s.getAll())) || []; } catch (_) { return []; }
}

// ---- backup files
const b64 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000)); return btoa(s); };
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
export function backupJson(recs) {
  return JSON.stringify({ format: "remotefm1-saves", version: 1, exported: new Date().toISOString(),
    saves: recs.map((r) => ({ fw: r.fw, engine: r.engine, version: r.version, name: r.name, saved: r.saved, bytes: b64(r.bytes) })) }, null, 1);
}
// a backup file's text -> the saves it holds (throws if it is not one)
export function parseBackup(text) {
  const j = JSON.parse(text);
  if (!j || j.format !== "remotefm1-saves" || !Array.isArray(j.saves)) throw new Error("This is not a saves backup of this site.");
  return j.saves.map((s) => ({ fw: String(s.fw), engine: String(s.engine), version: String(s.version), name: String(s.name), bytes: unb64(s.bytes) }));
}
export async function importBackup(text) {
  const saves = parseBackup(text);
  for (const s of saves) await put(s.fw, s.engine, s.version, s.name, s.bytes);
  return saves.length;
}

// ---- a save carried to a new version
// A firmware loaded at a version with no save, when an older version of it has one on the same engine kind, is
// offered that save once: "Bring your Felucca 1.6.1 save to 1.6.2?". Yes copies the older version's flash into this
// version's Autosave and leaves the older save as it is (the backup); no starts fresh and is not asked again. Saves
// are only carried within an engine kind: a native save is an fm1core flash image (FM1VST's core, 1 MiB), an emulated
// one fm1-emu's sector records (the sectors that differ from the package's own flash); neither engine reads the
// other's, and no conversion between them is checked.
//   new Carry({ list, put, decisions: { get(key), set(key, value) }, compare })
//   await carry.check(fw, engine, version)  -> an offer { fw, engine, from, to, rec } or null
//   await carry.answer(offer, yes)          -> the new Autosave (yes) or null
// A decision per (firmware, engine, version): "offered" (shown, not answered: shown again at the next load), "yes",
// "no". A version that already had saves before any offer is never asked about.
export const carryKey = (fw, engine, version) => `${fw}|${engine}|${version}`;

// the offer from a firmware's saves (every engine and version), or null; compare: live.js compareVersions
export function carryOffer(recs, fw, engine, version, decision, compare) {
  if (decision === "yes" || decision === "no" || engine === "mock" || !version) return null;
  const mine = recs.filter((r) => r.fw === fw && r.engine === engine);
  if (mine.some((r) => r.version === version) && decision !== "offered") return null;
  // older versions (or unsure ones: a build hash), never a newer one
  const older = mine.filter((r) => r.version !== version && !(compare(r.version, version) > 0));
  if (!older.length) return null;
  // the newest of them (unsure ones: the one saved last), then its Autosave (else its last named save)
  const last = older.reduce((a, b) => { const c = compare(b.version, a.version); return c > 0 ? b : c < 0 ? a : b.saved > a.saved ? b : a; });
  const same = older.filter((r) => r.version === last.version);
  const rec = same.find((r) => r.name === AUTOSAVE) || same.reduce((a, b) => (b.saved > a.saved ? b : a));
  return { fw, engine, from: rec.version, to: version, rec };
}

export class Carry {
  constructor({ list: listFn = list, put: putFn = put, decisions, compare }) {
    this.list = listFn; this.put = putFn; this.decisions = decisions; this.compare = compare;
    this.shown = new Set();     // offered in this page already
  }
  async check(fw, engine, version) {
    const key = carryKey(fw, engine, version);
    if (this.shown.has(key)) return null;
    const o = carryOffer(await this.list(fw), fw, engine, version, this.decisions.get(key), this.compare);
    if (!o) return null;
    this.shown.add(key);
    this.decisions.set(key, "offered");
    return o;
  }
  async answer(o, yes) {
    this.decisions.set(carryKey(o.fw, o.engine, o.to), yes ? "yes" : "no");
    return yes ? this.put(o.fw, o.engine, o.to, AUTOSAVE, o.rec.bytes) : null;
  }
}
