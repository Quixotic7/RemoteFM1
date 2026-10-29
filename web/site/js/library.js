// SPDX-License-Identifier: GPL-3.0-only
// Your firmware: the .rfm1 files the visitor opened (RemoteFM1IOS docs/RFM1-FORMAT.md), kept in this browser's
// IndexedDB by their SHA-256, so they stay in the firmware browser after a reload. They run on the rfm1 engine
// (web/engines/rfm1); their saves are the firmware's under engine "rfm1" (saves.js), apart from its native and
// emulated ones. The page can't download them itself (GitHub release downloads send no CORS header): the visitor
// downloads a file and opens it (or drops it on the page).
//
//   await add({ sha256, fileName, manifest, credits, bytes })   -> the record (bytes kept apart from the list)
//   await list()                                                -> records without their bytes, oldest first
//   await bytes(sha256)                                         -> Uint8Array | null
//   await remove(sha256)
//   entryOf(record)                                             -> the page's firmware entry for it

const DB = "remotefm1-firmware", META = "rfm1", BYTES = "rfm1-bytes", VERSION = 1;
export const RELEASES = "https://github.com/Quixotic7/RemoteFM1FW/releases/latest";
let dbp = null;

function db() {
  if (dbp) return dbp;
  dbp = new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") return reject(new Error("no IndexedDB"));
    const r = indexedDB.open(DB, VERSION);
    r.onupgradeneeded = () => {
      r.result.createObjectStore(META, { keyPath: "sha256" });
      r.result.createObjectStore(BYTES);
    };
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  dbp.catch(() => { dbp = null; });
  return dbp;
}
const req = (r) => new Promise((resolve, reject) => { r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error); });
async function tx(stores, mode, fn) {
  const d = await db(), t = d.transaction(stores, mode);
  const out = fn(t);
  await new Promise((resolve, reject) => { t.oncomplete = () => resolve(); t.onerror = () => reject(t.error); t.onabort = () => reject(t.error || new Error("aborted")); });
  return out;
}

export async function add({ sha256, fileName, manifest, credits = "", bytes }) {
  const rec = { sha256, fileName, size: bytes.length, manifest, credits, added: new Date().toISOString() };
  await tx([META, BYTES], "readwrite", (t) => { t.objectStore(META).put(rec); t.objectStore(BYTES).put(new Uint8Array(bytes), sha256); });
  return rec;
}
export async function list() {
  try {
    const all = await tx([META], "readonly", (t) => req(t.objectStore(META).getAll()));
    return ((await all) || []).sort((a, b) => (a.added < b.added ? -1 : a.added > b.added ? 1 : 0));
  } catch (_) { return []; }
}
export async function bytes(sha256) {
  const r = await tx([BYTES], "readonly", (t) => req(t.objectStore(BYTES).get(sha256)));
  const b = await r;
  return b ? new Uint8Array(b) : null;
}
export async function remove(sha256) {
  await tx([META, BYTES], "readwrite", (t) => { t.objectStore(META).delete(sha256); t.objectStore(BYTES).delete(sha256); });
}

// a firmware entry as the catalog's look (live.js siteEntry), for the browser, the header and the about page
export function entryOf(rec) {
  const m = rec.manifest || {}, authors = Array.isArray(m.authors) ? m.authors.map(String).filter(Boolean) : [];
  const by = authors.join(", ");
  return {
    id: m.id, name: m.name, version: String(m.version), author: by || "unknown", authorFull: by || "unknown",
    category: "Other", categories: ["Other"], tag: ".rfm1 file",
    what: `${m.name} ${m.version} from your .rfm1 file (${rec.fileName}): the firmware's own code, built to WebAssembly${m.built_by ? ` by ${m.built_by}` : ""}.`,
    license: m.license || null, source: m.source_url || null,
    engines: ["rfm1"], defaultEngine: "rfm1", native: null, emu: null, local: true,
    rfm1: { sha256: rec.sha256, fileName: rec.fileName, size: rec.size, glueUrl: typeof m.glue_url === "string" ? m.glue_url : null,
            builtBy: m.built_by || null, authors },
  };
}
