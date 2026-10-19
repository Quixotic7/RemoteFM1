// SPDX-License-Identifier: GPL-3.0-only
// Following the FM-1 Firmware Hub's releases. The deployed catalog.json is a snapshot of the Hub's catalog; the Hub
// deletes a package when its firmware has a new release, so the page fetches the Hub's catalog itself at load and
// merges it over the snapshot (mergeLive), and a package that is gone falls back (packageFor). Pure code: no DOM, so
// web/site/test.mjs runs it in Node, and web/site/tools/build_catalog.mjs builds its entries with it (siteEntry).
//
//   compareVersions(a, b)        -1 / 0 / 1, NaN when unsure (a tolerant comparator for the Hub's version strings)
//   hubNewer(hubV, nativeV)      the Hub's version is newer than the native core's, or unsure and different
//   newestWins(entry)            the default engine: emu when the native core is behind the Hub, else native
//   chooseEngine(entry, want, prefs)   the engine to start: the deep link's, the visitor's choice, the default
//   mergeLive(snapshot, hub)     the deployed catalog with the Hub's current versions, packages and firmwares
//   liveCatalog(snapshot, opts)  mergeLive over a fetched Hub catalog, within a timeout; the snapshot when it fails
//   packageFor(entry, opts)      a package's bytes; a gone one (404): the live catalog once more, then the snapshot's
//   sourceOf(entry, engine, credits)   the running firmware's source at the version that runs (the header's link)

export const HUB = "https://thefirmwarehub.com/";
export const SELF_ID = "remotefm1";
export const LIVE_TIMEOUT_MS = 4000;

// ------------------------------------------------------------------ versions ---
// The Hub's versions: "1.6.1", "0.2.20-beta", "1.0.5RC", "Beta4", "1.1.5-rb", "0.8.5-c524e3c", "2026.10.17",
// "1.5.1_and_sequences", "2.4.2+804c7785". The dotted numbers decide; then a pre-release (alpha < beta < rc) comes
// before its release; build metadata after "+" is ignored. Anything else that differs (a commit hash, a tag such as
// "-rb") is unsure: NaN.
const PRE = { dev: 0, snapshot: 0, alpha: 1, a: 1, beta: 2, b: 2, pre: 3, preview: 3, test: 3, rc: 4 };
const PRE_RE = /^(dev|snapshot|alpha|beta|preview|pre|test|rc|a|b)(?=\d|[-._]|$)[-._]?(\d*)(.*)$/;
const norm = (v) => String(v ?? "").trim().toLowerCase().replace(/^v(?=\d)/, "").replace(/\+.*$/, "");
const sign = (d) => (d > 0 ? 1 : d < 0 ? -1 : 0);

// word and number tokens compared in order; the words must be the same ones
function tokenCompare(x, y) {
  const tx = x.match(/\d+|[a-z]+/g) || [], ty = y.match(/\d+|[a-z]+/g) || [];
  if (tx.length !== ty.length) return NaN;
  for (let i = 0; i < tx.length; i++) {
    const nx = /^\d/.test(tx[i]), ny = /^\d/.test(ty[i]);
    if (nx !== ny) return NaN;
    if (nx) { const d = Number(tx[i]) - Number(ty[i]); if (d) return sign(d); }
    else if (tx[i] !== ty[i]) return NaN;
  }
  return 0;
}
function suffixKind(s) {
  if (!s) return { k: "release" };
  if (/^[0-9a-f]{6,40}$/.test(s) && /\d/.test(s) && /[a-f]/.test(s)) return { k: "hash", s };
  const m = PRE_RE.exec(s);
  if (m) return { k: "pre", rank: PRE[m[1]], n: m[2] ? Number(m[2]) : 0, rest: m[3].replace(/^[-._]+/, "") };
  return { k: "tag", s };
}
function suffixCompare(a, b) {
  const x = suffixKind(a.replace(/^[-._ ]+/, "")), y = suffixKind(b.replace(/^[-._ ]+/, ""));
  if (x.k === "release" && y.k === "release") return 0;
  if (x.k === "pre" && y.k === "release") return -1;
  if (x.k === "release" && y.k === "pre") return 1;
  if (x.k === "pre" && y.k === "pre") {
    const d = x.rank - y.rank || x.n - y.n;
    if (d) return sign(d);
    return x.rest === y.rest ? 0 : NaN;
  }
  if (x.k === "tag" && y.k === "tag") return tokenCompare(x.s, y.s);
  return x.k === y.k && x.s === y.s ? 0 : NaN;
}
export function compareVersions(a, b) {
  const x = norm(a), y = norm(b);
  if (x === y) return 0;
  if (!x || !y) return NaN;
  const mx = /^(\d+(?:\.\d+)*)(.*)$/.exec(x), my = /^(\d+(?:\.\d+)*)(.*)$/.exec(y);
  if (!mx || !my) return mx || my ? NaN : tokenCompare(x, y);     // "Beta4" against "Beta5": the numbers decide
  const cx = mx[1].split(".").map(Number), cy = my[1].split(".").map(Number);
  for (let i = 0; i < Math.max(cx.length, cy.length); i++) {
    const d = (cx[i] || 0) - (cy[i] || 0);
    if (d) return sign(d);
  }
  return suffixCompare(mx[2], my[2]);
}
// the Hub's version is newer than the native core's; unsure counts as newer (prefer the Hub's)
export function hubNewer(hubV, nativeV) {
  const c = compareVersions(hubV, nativeV);
  return c > 0 || Number.isNaN(c);
}

// ------------------------------------------------------------------ engines ---
// Newest wins: the native core is the default only while it is the Hub's version (or newer).
export function newestWins(e) {
  if (!e.native || !e.engines.includes("native")) return e;
  e.nativeBehind = e.engines.includes("emu") && hubNewer(e.version, e.native.version);
  e.defaultEngine = e.nativeBehind ? "emu" : "native";
  return e;
}
// the engine to start: the deep link's (?engine=), else the visitor's own choice while the firmware's version is the
// one they chose it at (a new release resets it), else the default. prefs: { engine: {id: name}, engineAt: {id: ver} }
export function chooseEngine(e, want, prefs = {}) {
  if (want && e.engines.includes(want)) return want;
  const pref = (prefs.engine || {})[e.id], at = (prefs.engineAt || {})[e.id];
  if (pref && e.engines.includes(pref) && (at === undefined ? !e.nativeBehind : at === e.version)) return pref;
  return e.defaultEngine;
}

// ------------------------------------------------------------------ entries ---
export const CAT = { synth: "Synth", groove: "Groovebox", chords: "Chords", drums: "Drums", other: "Other", dev: "Other" };
export const CATEGORIES = ["All", "Synth", "Groovebox", "Chords", "Drums", "Other"];

// a short tag from the Hub's description: its first clause, without the article, at most ~30 characters
export function tagOf(what) {
  let s = String(what || "").split(/[:;(]|\.(?:\s|$)| - | — /)[0].trim().replace(/^(an?|the)\s+/i, "");
  if (!s) return "Firmware";
  const comma = s.split(",")[0].trim();
  if (s.length > 30 && comma.length >= 12) s = comma;
  s = s[0].toUpperCase() + s.slice(1);
  if (s.length <= 30) return s;
  const cut = s.slice(0, 29), sp = cut.lastIndexOf(" ");
  return (sp > 12 ? cut.slice(0, sp) : cut).replace(/[\s,-]+$/, "") + "…";
}
export const shortAuthor = (a) => String(a || "").replace(/\s*\(.*\)\s*/g, "").trim() || "unknown";
const emuOf = (f) => ({ pkg: f.pkg ? new URL(f.pkg, HUB).href : null, sha256: f.sha256 || null, size: f.size || 0, product: f.product || null });

// the site's entry for one of the Hub's firmwares (f: the Hub's own JSON). core: FM1VST's (data/cores.json) or null;
// native: { core, version, wasm } when its native build is on the site
export function siteEntry(f, { core = null, native = null, emuSkip = {} } = {}) {
  const kinds = (Array.isArray(f.kind) ? f.kind : [f.kind]).filter(Boolean);
  const cats = [...new Set(kinds.map((k) => CAT[k] || "Other"))];
  const e = {
    id: f.id,
    name: f.name,
    version: String(f.version ?? ""),
    author: core ? core.author : shortAuthor(f.author),
    authorFull: f.author,
    category: core ? core.category : cats[0] || "Other",
    categories: core ? [...new Set([core.category, ...cats])] : cats,
    tag: core ? core.tag : tagOf(f.what),
    what: f.what,
    license: core ? core.license : null,
    source: f.source || (f.repo ? `https://github.com/${f.repo}` : null),
    repo: f.repo || null,
    release: f.release || null,
    site: f.site || null,
    play: f.play || null,
    editor: f.editor || null,                       // the firmware's own editor on its site (for an FM-1 on USB)
    beta: !!f.beta,
    engines: native ? ["native", "emu"] : ["emu"],
    defaultEngine: native ? "native" : "emu",
    native,
    emu: emuOf(f),
  };
  if (core && core.gated) e.nativeGated = core.gated;
  if (emuSkip[f.id]) e.emuNote = emuSkip[f.id];
  return newestWins(e);
}

const byName = (a, b) => a.name.localeCompare(b.name, "en", { sensitivity: "base" });

// The deployed catalog (snapshot) with the Hub's catalog (hub) over it:
//   - a firmware on both: the Hub's version, package URL, SHA-256 and links; the snapshot's own fields otherwise
//     (category, tag, the native core: its wasm and version). Its snapshot version is kept as .snapshot for packageFor.
//   - a firmware only on the Hub: a new emulated entry.
//   - a firmware that left the Hub: gone, unless it has a native core (then native only) or is RemoteFM1's own.
//   - RemoteFM1's entry (a same-origin package) stays as it is.
export function mergeLive(snapshot, hub, { emuSkip = {} } = {}) {
  const byId = new Map(snapshot.firmwares.map((f) => [f.id, f]));
  const seen = new Set(), out = [];
  for (const h of hub.firmwares || []) {
    if (!h || !h.id || !h.pkg || h.id === SELF_ID || seen.has(h.id)) continue;
    const old = byId.get(h.id);
    if (old && old.emu && old.emu.from === "site") continue;
    seen.add(h.id);
    if (!old) { out.push({ ...siteEntry(h, { emuSkip }), live: true, added: true }); continue; }
    const emu = { ...old.emu, ...emuOf(h) };
    const e = { ...old, version: String(h.version ?? old.version), emu, live: true,
      what: h.what || old.what, source: h.source || old.source, release: h.release || old.release, play: h.play || old.play, beta: !!h.beta };
    if (!old.engines.includes("emu")) e.engines = [...old.engines, "emu"];
    delete e.snapshot;
    if (old.emu && old.emu.pkg && (old.emu.pkg !== emu.pkg || old.version !== e.version)) e.snapshot = { version: old.version, emu: old.emu };
    out.push(newestWins(e));
  }
  for (const f of snapshot.firmwares) {
    if (seen.has(f.id)) continue;
    if (f.emu && f.emu.from === "site") out.push(f);
    else if (f.native) out.push({ ...f, engines: ["native"], defaultEngine: "native", nativeBehind: false, offHub: true });
  }
  out.sort(byName);
  return { ...snapshot, firmwares: out, hubUpdated: hub.updated || snapshot.hubUpdated, live: true };
}

// fetchHub(signal) -> the Hub's catalog JSON. The snapshot (live: false) when it fails or takes longer than timeoutMs.
export async function liveCatalog(snapshot, { fetchHub, timeoutMs = LIVE_TIMEOUT_MS, emuSkip = {} } = {}) {
  const ctl = typeof AbortController === "function" ? new AbortController() : null;
  let timer = 0;
  try {
    const hub = await Promise.race([fetchHub(ctl && ctl.signal), new Promise((_, rej) => { timer = setTimeout(() => { rej(new Error(`the Hub: no answer in ${timeoutMs} ms`)); if (ctl) ctl.abort(); }, timeoutMs); })]);
    if (!hub || !Array.isArray(hub.firmwares)) throw new Error("the Hub's catalog has no firmwares");
    return mergeLive(snapshot, hub, { emuSkip });
  } catch (e) {
    return { ...snapshot, live: false, liveError: String((e && e.message) || e) };
  } finally { clearTimeout(timer); }
}

// ------------------------------------------------------------------ packages ---
// A package that is not there (any more): HTTP 404 / 410 from web/engines/emu/catalog.mjs fetchPackage ("<url>: 404"),
// or a network failure. A wrong SHA-256 is not: that one is reported as it is.
export const isGone = (err) => /: (404|410)$/.test(String((err && err.message) || err)) || (err && err.name === "TypeError");

// The package of an emulated entry: { bytes, entry } where entry is the one that played (its version keys the saves).
//   fetchPackage(pkg) -> { bytes }: catalog.mjs's (checks the SHA-256); pkg = { id, name, byo, package: { url, sha256 } }
//   recheck(id) -> the entry from the Hub's catalog fetched once more (null: none), for a package that is gone
// Order: the entry's package; if it is gone, the live catalog once more (the Hub may have released since the page
// loaded); then the snapshot's package (the version this site was deployed with).
export async function packageFor(entry, { fetchPackage, recheck = null }) {
  const tried = new Set();
  const attempt = async (e) => {
    tried.add(e.emu.pkg);
    const r = await fetchPackage({ id: e.id, name: e.name, byo: false, package: { url: e.emu.pkg, sha256: e.emu.sha256 } });
    return { bytes: r.bytes, entry: e };
  };
  let first;
  try { return await attempt(entry); } catch (err) { if (!isGone(err)) throw err; first = err; }
  let fresh = null;
  if (recheck) { try { fresh = await recheck(entry.id); } catch (_) { fresh = null; } }
  if (fresh && fresh.emu && fresh.emu.pkg && !tried.has(fresh.emu.pkg)) {
    try { return await attempt(fresh); } catch (err) { if (!isGone(err)) throw err; }
  }
  const base = fresh || entry, snap = base.snapshot || entry.snapshot;
  if (snap && snap.emu && snap.emu.pkg && !tried.has(snap.emu.pkg)) {
    const e = { ...base, version: snap.version, emu: snap.emu, fromSnapshot: true };
    delete e.snapshot;
    return attempt(newestWins(e));
  }
  throw first;
}

// ------------------------------------------------------------------ source ---
// The source of the firmware that runs, at the version that runs: { url, repo, ref, refKind, title, glue } or null.
//   native     the commit FM1VST pins for the core (its submodule: build_catalog.mjs), and FM1VST's glue folder for it
//   emulated   the Hub's source: github.com/<owner>/<repo>/tree/<tag> as it is; a bare repo with a GitHub release tag
//              gets /tree/<tag>; a bare repo with no tag stays bare (its default branch)
//   RemoteFM1  the same rule: this repo, its release's tag
//   a file     (M-VAVE's stock firmware or any .fwsc from the visitor's computer): null, no public source
const GH_REPO = /^https?:\/\/(?:www\.)?github\.com\/([^/\s?#]+)\/([^/\s?#]+?)(?:\.git)?\/?$/;
const GH_TREE = /^https?:\/\/(?:www\.)?github\.com\/([^/\s?#]+)\/([^/\s?#]+)\/(?:tree|commit|blob)\/([^?#]+?)\/?$/;
const GH_TAG = /^https?:\/\/(?:www\.)?github\.com\/([^/\s?#]+)\/([^/\s?#]+)\/releases\/tag\/([^/?#]+)/;
const isHash = (r) => /^[0-9a-f]{7,40}$/.test(r) && /[a-f]/.test(r) && /\d/.test(r);
const dec = (r) => { try { return decodeURIComponent(r); } catch (_) { return r; } };
const refLabel = (ref, kind) => !ref ? "its repository (no release tag)" : kind === "commit" ? `commit ${ref.slice(0, 7)}` : ref;
function hubSource(e) {
  const src = e.source || (e.repo ? `https://github.com/${e.repo}` : null);
  if (!src) return null;
  let m = GH_TREE.exec(src);
  if (m) { const ref = dec(m[3]); return { url: src, repo: `${m[1]}/${m[2]}`, ref, refKind: isHash(ref) ? "commit" : "ref" }; }
  m = GH_REPO.exec(src);
  if (m) {
    const t = GH_TAG.exec(e.release || "");
    if (t && t[1].toLowerCase() === m[1].toLowerCase() && t[2].toLowerCase() === m[2].toLowerCase()) {
      const ref = dec(t[3]);
      return { url: `https://github.com/${m[1]}/${m[2]}/tree/${t[3]}`, repo: `${m[1]}/${m[2]}`, ref, refKind: "tag" };
    }
    return { url: `https://github.com/${m[1]}/${m[2]}`, repo: `${m[1]}/${m[2]}`, ref: null, refKind: null };
  }
  return { url: src, repo: src.replace(/^https?:\/\/(www\.)?/, ""), ref: null, refKind: null };   // (not GitHub: as the Hub gives it)
}
export function sourceOf(e, engine, credits = {}) {
  if (!e || e.local) return null;
  const nat = engine === "native" && e.native;
  let s = null, glue = null;
  if (nat && e.native.commit && e.native.repo) {
    const m = GH_REPO.exec(e.native.repo);
    s = { url: `${e.native.repo.replace(/\/$/, "")}/tree/${e.native.commit}`, repo: m ? `${m[1]}/${m[2]}` : e.native.repo, ref: e.native.commit, refKind: "commit" };
  } else s = hubSource(e);
  if (!s) return null;
  const v = nat ? e.native.version : e.version, fv = credits.fm1vst;
  if (nat && e.native.glue && fv && fv.source)
    glue = { url: `${fv.source.replace(/\/$/, "")}/tree/${fv.commit || "main"}/core-glue/${e.native.glue}`, path: `core-glue/${e.native.glue}` };
  const at = refLabel(s.ref, s.refKind) + (nat && s.refKind === "commit" ? ", the commit FM1VST builds" : "");
  const title = `${e.name} ${v} by ${e.authorFull || e.author} · ${e.license || "licence: see its source"} · ${s.ref ? "source at " : "source: "}${at}`;
  return { ...s, title, glue, native: !!nat };
}

// ------------------------------------------------------------------ the browser ---
// The firmware browser's engine filter: "all", "native" (firmwares with a native core) or "emu" (every firmware the
// emulator runs: not the ones known not to, nor a native-only one that left the Hub). Combined with a category chip
// and a search.
export const ENGINE_FILTERS = [["all", "All"], ["native", "Native"], ["emu", "Emulated"]];
export function browse(list, { engine = "all", cat = "All", q = "" } = {}) {
  const s = String(q || "").trim().toLowerCase();
  return list.filter((f) =>
    (engine === "all" || (engine === "native" ? !!f.native && f.engines.includes("native") : f.engines.includes("emu") && !f.emuNote)) &&
    (cat === "All" || (f.categories || []).includes(cat)) &&
    (!s || [f.name, f.author, f.authorFull, f.tag, f.what, f.id].some((x) => x && String(x).toLowerCase().includes(s))));
}
// what a card shows under a filter: { version, note, engine } (engine: the one a click starts; null: the default)
export function cardView(f, engine = "all") {
  if (engine === "native" && f.native) return { version: f.native.version, note: f.nativeBehind ? `Hub: ${f.version}` : null, engine: "native" };
  if (engine === "emu") return { version: f.version, note: null, engine: "emu" };
  return { version: f.version, note: f.nativeBehind ? `native: ${f.native.version}` : null, engine: null };
}
