// Where the emu engine's firmware packages come from: the FM-1 Firmware Hub's catalog
// (thefirmwarehub.com, zvenson's). RemoteFM1's site hosts no one's firmware; the page fetches a
// package from the Hub when a visitor starts it, as fm1-emu's own page does (its
// tools/hub_catalog.mjs).
//
// Runtime fetch path, checked 2026-10-18:
//   https://thefirmwarehub.com/catalog.json         Access-Control-Allow-Origin: *, max-age=600
//   https://thefirmwarehub.com/<pkg>, pkg = "fw/<id>-<version>.fwsc"   Access-Control-Allow-Origin: *
// GitHub release downloads can't be fetched from a page: github.com/.../releases/download/... answers
// 302 to release-assets.githubusercontent.com, and neither sends Access-Control-Allow-Origin. So the
// Hub's mirror is the only browser path; `release` (the author's release page) is for links only.
//
// M-VAVE's stock firmware only ever comes from the visitor's own file (stockFromFile). The Hub's
// catalog lists a copy of it (`stock`); this module reads only its hash, to recognise the file.
//
// Works in the browser, the AudioWorklet-free page and Node 18+ (fetch, crypto.subtle).

export const HUB = "https://thefirmwarehub.com/";
export const HUB_CATALOG = HUB + "catalog.json";

// Hub firmwares known not to run in fm1-emu at its pinned commit (fm1-emu tools/hub_catalog.mjs SKIP)
export const EMU_SKIP = { bubba: "boots to a black screen, no sound (needs M-VAVE's own loader)" };

export const STOCK_ID = "mvave";

// Fetch the Hub's catalog (one request; callers cache it: the Hub refreshes it every few hours and
// sends max-age=600).
export async function fetchHubCatalog(fetchFn = fetch, url = HUB_CATALOG) {
  const r = await fetchFn(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return r.json();
}

// The emu engine's entries from the Hub's catalog JSON. One per FM-1 firmware with a package:
//   { id, name, version, author, kind, about, source, release, product, ident,
//     package: { url, sha256, size } | null, byo, emu: true | "<why not>" }
// id is the Hub's own. The stock firmware comes last, with byo: true and no package URL.
export function entriesFromHub(hub) {
  const out = [];
  for (const h of hub.firmwares || []) {
    if (!h.pkg) continue;
    out.push({
      id: h.id,
      name: h.name,
      version: String(h.version ?? ""),
      author: h.author ?? null,
      kind: h.kind ?? null,
      about: h.what ?? "",
      source: h.source || (h.repo ? `https://github.com/${h.repo}` : h.site || null),
      release: h.release ?? null,
      product: h.product ?? null,
      ident: h.ident ?? null,
      package: { url: new URL(h.pkg, HUB).href, sha256: h.sha256 ?? null, size: h.size ?? null },
      byo: false,
      emu: EMU_SKIP[h.id] ?? true,
    });
  }
  out.push(stockEntry(hub));
  return out;
}

export function stockEntry(hub = {}) {
  const s = hub.stock || {};
  return {
    id: STOCK_ID,
    name: "M-VAVE stock firmware",
    version: s.product ? String(s.product).replace(/^FM-1_0*/, "V") : "",
    author: "M-VAVE",
    kind: "synth",
    about: "The firmware the FM-1 ships with: a DX7-style FM synth. Bring your own FM-1.fwsc.",
    source: "https://github.com/joakimeriksson/fm1-emu/blob/master/docs/stock-firmware.md",
    release: null,
    product: s.product ?? null,
    ident: null,
    package: null,                      // never fetched: the visitor opens their own file
    knownSha256: s.sha256 ?? null,      // to recognise the visitor's file as V15
    byo: true,
    emu: true,
  };
}

async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// The package bytes for an entry, fetched at play time from the Hub's mirror and checked against
// the catalog's SHA-256. Returns { bytes: Uint8Array, url }. The stock firmware throws: use
// stockFromFile with the visitor's own file.
export async function fetchPackage(entry, fetchFn = fetch) {
  if (entry.byo || !entry.package?.url) {
    throw new Error(`${entry.name}: not fetched; open your own ${entry.id === STOCK_ID ? "FM-1.fwsc" : ".fwsc"}`);
  }
  const { url, sha256 } = entry.package;
  const r = await fetchFn(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  const bytes = new Uint8Array(await r.arrayBuffer());
  if (sha256) {
    const got = await sha256hex(bytes);
    if (got !== sha256) throw new Error(`${url}: SHA-256 ${got}, the Hub's catalog says ${sha256}`);
  }
  return { bytes, url };
}

// The visitor's own package (a File / Blob / ArrayBuffer / Uint8Array they opened). Says whether
// it is M-VAVE's V15 (by the hash the Hub's catalog gives; null when no catalog was given). Any FM-1
// package boots the same way, so another file still runs. Returns { bytes, sha256, stock }.
export async function stockFromFile(file, hub = null) {
  const bytes = file instanceof Uint8Array ? file
    : new Uint8Array(file.arrayBuffer ? await file.arrayBuffer() : file);
  const sha256 = await sha256hex(bytes);
  const known = hub?.stock?.sha256 ?? null;
  return { bytes, sha256, stock: known ? sha256 === known : null };
}
