// SPDX-License-Identifier: GPL-3.0-only
// An author's live editor page, in the player: the page is fetched from its author's site at run time (their sites send
// Access-Control-Allow-Origin: *), given <base href> its own address and the player's Web MIDI shim (fm1-shim.js,
// inline, data-transport="sandbox") first in <head>, and shown in a sandboxed iframe (srcdoc) without
// allow-same-origin: the author's code, unreviewed, runs as an opaque origin, never as this site. It talks to the
// firmware running in the player through the shim and managers.js ManagerBridge (allowFrame), as the hosted copies do.
// Used by editor-panel.js (the Editor panel) and managers/live.html (pop out; web/managers/build.mjs copies this file
// next to it). Nothing of the author's is copied into the site: the visitor's browser loads it from the author's host.
//
//   fetchLive(url, { timeout })      -> { html, url (after redirects) }; throws (HTTP, not CORS-readable, timeout)
//   injectLive(html, { base, shim, manager, storage })   -> the srcdoc
//   new EditorStore(key)             the live page's localStorage, kept here under a prefix (seed(), apply(msg))
//   stashPop(info) / takePop(token)  hands a live editor to managers/live.html (pop out) through this site's storage
export const LIVE_SANDBOX = "allow-scripts allow-downloads allow-popups allow-popups-to-escape-sandbox allow-forms allow-modals";
export const FETCH_MS = 5000, HELLO_MS = 4000;

export async function fetchLive(url, { timeout = FETCH_MS, fetchFn = (u, o) => fetch(u, o) } = {}) {
  const ctl = typeof AbortController === "function" ? new AbortController() : null;
  const t = setTimeout(() => ctl && ctl.abort(), timeout);
  try {
    let r;
    try { r = await fetchFn(url, { mode: "cors", credentials: "omit", cache: "no-cache", redirect: "follow", signal: ctl ? ctl.signal : undefined }); }
    catch (e) { throw new Error(ctl && ctl.signal.aborted ? `no answer in ${timeout / 1000} s` : "not readable from here (CORS or offline)"); }
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const ct = (r.headers && r.headers.get && r.headers.get("content-type")) || "";
    if (ct && !/html/i.test(ct)) throw new Error(`not a page (${ct})`);
    const html = await r.text();
    if (!/<head[\s>]/i.test(html) && !/<html[\s>]/i.test(html)) throw new Error("not a page");
    // (a page that forwards at once, a meta refresh: not the editor itself; Skiff's catalog editor is its installer's door)
    if (/<meta[^>]+http-equiv\s*=\s*["']?refresh/i.test(html)) throw new Error("the page forwards to another (meta refresh)");
    return { html, url: r.url || url };
  } finally { clearTimeout(t); }
}

const attr = (s) => String(s).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
// <base href> and the shim's <script> first in <head> (before any script of the page: navigator.requestMIDIAccess is the
// shim's when the page asks for it; the first <base> is the one that counts)
export function injectLive(html, { base, shim, manager = "", storage = {} }) {
  const tag = `<base href="${attr(base)}"><script data-manager="${attr(manager)}" data-transport="sandbox" data-storage="${attr(JSON.stringify(storage || {}))}">`
    + String(shim).replace(/<\/script/gi, "<\\/script") + "</script>";
  const hd = /<head(\s[^>]*)?>/i.exec(html);
  if (hd) return html.slice(0, hd.index + hd[0].length) + tag + html.slice(hd.index + hd[0].length);
  const ht = /<html(\s[^>]*)?>/i.exec(html);
  if (ht) return html.slice(0, ht.index + ht[0].length) + "<head>" + tag + "</head>" + html.slice(ht.index + ht[0].length);
  const dt = /^\s*<!doctype[^>]*>/i.exec(html);
  return dt ? dt[0] + tag + html.slice(dt[0].length) : tag + html;
}

const lsOf = () => { try { return typeof localStorage !== "undefined" ? localStorage : null; } catch (_) { return null; } };
// the live page's localStorage: this site's, under "remotefm1-ed:<its address>|"; bounded (an author's page can not fill
// the site's storage): 64 keys, 64 KiB a value, 512 KiB in all
export class EditorStore {
  constructor(key, ls = lsOf()) { this.prefix = `remotefm1-ed:${key}|`; this.ls = ls; }
  keys() { const out = []; try { for (let i = 0; i < this.ls.length; i++) { const k = this.ls.key(i); if (k && k.startsWith(this.prefix)) out.push(k); } } catch (_) {} return out; }
  seed() {
    const o = {};
    if (!this.ls) return o;
    try { for (const k of this.keys()) o[k.slice(this.prefix.length)] = this.ls.getItem(k); } catch (_) {}
    return o;
  }
  apply(m) {
    if (!this.ls || !m) return false;
    try {
      if (m.op === "clear") { for (const k of this.keys()) this.ls.removeItem(k); return true; }
      if (typeof m.key !== "string" || m.key.length > 256) return false;
      if (m.op === "remove") { this.ls.removeItem(this.prefix + m.key); return true; }
      if (m.op !== "set" || typeof m.value !== "string" || m.value.length > 65536) return false;
      const keys = this.keys(), k = this.prefix + m.key;
      let total = m.value.length + m.key.length;
      for (const x of keys) if (x !== k) total += x.length - this.prefix.length + (this.ls.getItem(x) || "").length;
      if ((!keys.includes(k) && keys.length >= 64) || total > 512 * 1024) return false;
      this.ls.setItem(k, m.value);
      return true;
    } catch (_) { return false; }
  }
}

// pop out: the live editor to open is handed to managers/live.html under a random token (live.html opens only what this
// site put there, never an address from its own URL)
const POP = "remotefm1-live-pop:";
export function stashPop(info, ls = lsOf()) {
  const token = Math.random().toString(36).slice(2, 12) + Math.random().toString(36).slice(2, 8);
  try {
    for (let i = ls.length - 1; i >= 0; i--) { const k = ls.key(i); if (k && k.startsWith(POP)) { const v = JSON.parse(ls.getItem(k) || "{}"); if (!(Date.now() - v.at < 120000)) ls.removeItem(k); } }
    ls.setItem(POP + token, JSON.stringify({ ...info, at: Date.now() }));
  } catch (_) { return null; }
  return token;
}
export function takePop(token, ls = lsOf()) {
  if (!token || !/^[a-z0-9]{6,24}$/.test(token)) return null;
  try {
    const v = JSON.parse(ls.getItem(POP + token) || "null");
    ls.removeItem(POP + token);
    return v && Date.now() - v.at < 120000 ? v : null;
  } catch (_) { return null; }
}
