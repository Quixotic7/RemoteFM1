// SPDX-License-Identifier: GPL-3.0-only
// The header's Editor button and its panel: the running firmware's own manager / editor in an iframe beside the FM-1,
// talking to the firmware through managers.js ManagerBridge. The FM-1 keeps playing, with sound, while the panel is open.
//   native firmware (or an .rfm1 file) whose author's editor page is readable from here (live-editor.js)
//                                           the author's live page, in a sandbox ("live · host"); "Use hosted copy"
//                                           when this site hosts one too (remembered per firmware)
//     ... the page not readable, no answer in 5 s, or its shim silent for 4 s: the hosted copy, else the link below
//   native / .rfm1 with a hosted manager    the hosted copy (web/managers/, the commit FM1VST pins)
//   emulated (or the test engine)           "Editors work with the native engine; switch Engine to Native"
//   native / .rfm1, an editor only on its site   "not hosted on this site yet" and a link to it
//   emulated only, an editor on its site    "This firmware's editor needs a real FM-1" and a link to it
// "Pop out": a hosted copy in a tab of its own (the BroadcastChannel); a live page in managers/live.html, which loads
// it the same way and relays between its sandbox and this tab over the BroadcastChannel.
import { ManagerBridge, editorOf, portOf } from "./managers.js";
import { LIVE_SANDBOX, HELLO_MS, fetchLive, injectLive, EditorStore, stashPop } from "./live-editor.js";

const $ = (id) => document.getElementById(id);
// the engines that run the firmware's own code, so its manager's SysEx gets answers: native, and an .rfm1 file
const runsOwnCode = (s) => s.engine === "native" || s.engine === "rfm1";
const hostOf = (u) => { try { return new URL(u).host; } catch (_) { return u; } };
// the catalog's repo ("owner/name": GitHub, or an address) as a link, or null
const repoUrl = (r) => (!r ? null : /^https:\/\//.test(r) ? r : /^[\w.-]+\/[\w.-]+$/.test(r) ? `https://github.com/${r}` : null);
const PREF = "remotefm1-editor-src:";            // + firmware id: "hosted" (the hosted copy) / absent (the live page)

export class EditorPanel {
  // deps: getState() (main.js state), index (managers/index.json or null), base (the managers folder's URL),
  //       toEngine(bytes), setEngine(name), onLayout(); fetchFn (tests)
  constructor(deps) {
    this.d = deps;
    this.index = deps.index || null;
    this.base = deps.base;
    this.open = false;
    this.frameFor = null;              // what the iframe shows: "hosted:<manager id>" / "live:<url>"
    this.live = null;                  // the live page shown or loading: { url, final, host, store, win, frame, ready }
    this.liveFail = new Map();         // live url -> why it fell back (this page's life)
    this.gen = 0;
    this.bridge = new ManagerBridge({ toEngine: deps.toEngine, onChange: () => this.renderHead(),
      onStorage: (m, w) => { if (this.live && this.live.win === w) this.live.store.apply(m); } });
    this.btn = $("ed-btn"); this.panel = $("ed-panel"); this.body = $("ed-body");
    this.btn.addEventListener("click", () => (this.open ? this.close() : this.show()));
    $("ed-close").addEventListener("click", () => this.close());
    $("ed-pop").addEventListener("click", () => this.popOut());
    const mode = $("ed-mode");
    if (mode) mode.addEventListener("click", () => this.switchSource());
  }
  get editor() { const s = this.d.getState(); return editorOf(s.entry, this.index); }
  url(m) { return new URL(m.path, this.base).href; }
  pref(e) { try { return e ? localStorage.getItem(PREF + e.id) : null; } catch (_) { return null; } }
  setPref(e, v) { try { if (v) localStorage.setItem(PREF + e.id, v); else localStorage.removeItem(PREF + e.id); } catch (_) { /* (not kept) */ } }
  // the live page can be tried: an address, the firmware's own code running here with its port name, not failed, and
  // the hosted copy not chosen
  liveWanted(s = this.d.getState(), ed = this.editor) {
    return !!(ed && ed.live && runsOwnCode(s) && portOf(s.entry, this.index) && !this.liveFail.has(ed.live)
      && !(ed.kind === "hosted" && this.pref(s.entry) === "hosted"));
  }
  get showingLive() { return !!(this.live && this.frameFor === "live:" + this.live.url); }

  // the firmware, its engine or its state changed: the button, the bridge's port, the panel
  update() {
    const s = this.d.getState(), e = s.entry, ed = this.editor;
    this.btn.hidden = !ed;
    if (ed) {
      this.btn.title = ed.kind === "hosted" || this.liveWanted(s, ed) ? `${ed.manager ? ed.manager.title : e.name + "'s editor"}: on the firmware running here` : `${e.name}'s editor (on its own site, for an FM-1 on USB)`;
      this.btn.setAttribute("aria-expanded", String(this.open));
      this.btn.classList.toggle("on", this.open);
    }
    const running = runsOwnCode(s) && s.engineState === "running" && !!s.info;
    const port = running ? portOf(e, this.index) : null;
    this.bridge.setTarget({ port, firmware: port ? `${e.name} ${s.info.version || ""}`.trim() : null, engine: port ? s.engine : null });
    if (this.open && !ed) this.close();
    else if (this.open) this.render();
  }

  show() { this.open = true; document.body.classList.add("ed-open"); this.panel.hidden = false; this.render(); this.update(); this.d.onLayout(); }
  close() {
    this.open = false; document.body.classList.remove("ed-open"); this.panel.hidden = true;
    this.dropFrame(); this.update(); this.d.onLayout();
  }
  dropFrame() {
    this.gen++;
    if (this.live) { this.bridge.disallowFrame(this.live.win); this.live = null; }
    if (!this.frameFor) return;
    this.body.replaceChildren(); this.frameFor = null;
    this.bridge.forgetFrames();
  }
  // "Use hosted copy" / "Use live page"
  switchSource() {
    const s = this.d.getState(), ed = this.editor;
    if (!ed || !ed.live || ed.kind !== "hosted") return;
    const toHosted = this.showingLive;
    this.setPref(s.entry, toHosted ? "hosted" : null);
    if (!toHosted) this.liveFail.delete(ed.live);
    this.dropFrame(); this.render(); this.update();
  }
  popOut() {
    const ed = this.editor, s = this.d.getState();
    if (!ed) return;
    if (this.showingLive) {
      const token = stashPop({ url: this.live.url, manager: ed.kind === "hosted" ? ed.manager.id : s.entry.id, title: this.title(s, ed),
                               hosted: ed.kind === "hosted" ? this.url(ed.manager) : null });
      if (token) { window.open(new URL(`live.html?t=${token}`, this.base).href, "_blank", "noopener"); this.close(); return; }
      window.open(this.live.url, "_blank", "noopener");     // (no storage to hand it over: the author's own page)
      return;
    }
    const href = ed.kind === "hosted" ? this.url(ed.manager) : ed.url;
    window.open(href, "_blank", "noopener");
    if (ed.kind === "hosted") this.close();
  }
  title(s, ed) { return ed.kind === "hosted" ? ed.manager.title : `${s.entry.name}'s editor`; }

  renderHead() {
    if (!this.open) return;
    const s = this.d.getState(), ed = this.editor, t = this.bridge.target, live = this.showingLive ? this.live : null;
    $("ed-title").textContent = ed ? this.title(s, ed) : "Editor";
    const dot = $("ed-dot");
    dot.className = "fm1-dot " + (t.connected && (!live || live.ready) ? "on" : "off");
    const port = t.connected
      ? `port "${t.port}"${live ? "" : ` · ${s.entry.name} in this page`}${this.bridge.count > 1 ? ` · ${this.bridge.count} editors` : ""}`
      : ed && (ed.kind === "hosted" || live) && runsOwnCode(s) && s.engineState !== "running" ? "starting the firmware..." : "no port";
    $("ed-sub").textContent = live ? `live · ${live.host}${live.ready ? "" : " · loading"} · ${port}` : port;
    $("ed-pop").hidden = !ed;
    $("ed-pop").textContent = ed && ed.kind === "real" && !live ? "Open its site ↗" : "Pop out ↗";
    const src = $("ed-src");
    if (live) {
      const repo = repoUrl(s.entry.repo);
      src.hidden = false; src.href = repo || live.url;
      src.title = `${this.title(s, ed)}: its author's own page, live from ${live.host} (${live.final})${repo ? "; the link: its source" : ""}`;
    } else {
      const m = ed && ed.kind === "hosted" && this.frameFor ? ed.manager : null;
      src.hidden = !m;
      if (m) { src.href = m.source; src.title = `${m.title} by ${m.author} (${m.license}): its source at the commit hosted here`; }
    }
    const mode = $("ed-mode");
    if (mode) {
      const can = !!(ed && ed.kind === "hosted" && ed.live && runsOwnCode(s) && portOf(s.entry, this.index) && this.frameFor);
      mode.hidden = !can;
      if (can) {
        mode.textContent = live ? "Use hosted copy" : "Use live page";
        mode.title = live ? `the copy this site hosts (${ed.manager.source})` : this.liveFail.has(ed.live)
          ? `try the author's live page again (${hostOf(ed.live)}: ${this.liveFail.get(ed.live)})` : `the author's live page, from ${hostOf(ed.live)}`;
      }
    }
  }

  render() {
    const s = this.d.getState(), e = s.entry, ed = this.editor;
    this.renderHead();
    if (!ed) { this.dropFrame(); return; }
    if (this.liveWanted(s, ed)) {
      if (this.frameFor === "live:" + ed.live) return;       // (the same page stays: a reboot only moves its port)
      this.dropFrame();
      this.loadLive(ed);
      this.renderHead();
      return;
    }
    if (ed.kind === "hosted" && runsOwnCode(s)) {
      if (this.frameFor === "hosted:" + ed.manager.id) return;
      this.dropFrame();
      const f = document.createElement("iframe");
      f.className = "ed-frame"; f.title = ed.manager.title; f.src = this.url(ed.manager);
      this.body.replaceChildren(f);
      this.frameFor = "hosted:" + ed.manager.id;
      this.renderHead();
      return;
    }
    this.dropFrame();
    const box = document.createElement("div");
    box.className = "ed-note";
    const p = (text, cls = "") => { const el = document.createElement("p"); el.textContent = text; if (cls) el.className = cls; box.append(el); return el; };
    const btn = (text, onclick, cls = "pill primary") => { const b = document.createElement("button"); b.className = cls; b.textContent = text; b.onclick = onclick; box.append(b); return b; };
    const own = e.engines.find((k) => k === "native" || k === "rfm1") || null;
    if (ed.kind === "hosted" || (ed.live && own && portOf(e, this.index) && !this.liveFail.has(ed.live))) {
      // the engine to offer: the firmware's own code, native or its .rfm1 (whichever the catalog lists first)
      const title = this.title(s, ed);
      const ownName = own === "rfm1" ? ".rfm1" : "Native";
      p(s.engine === "mock" && (s.wantEngine === "native" || s.wantEngine === "rfm1")
        ? `The ${s.wantEngine === "rfm1" ? ".rfm1" : "native"} engine could not start here, so ${title} has no firmware to talk to.`
        : `Editors work with the firmware's own code; switch Engine to ${own ? ownName : "Native"}.`, "ed-lead");
      p(`${title} talks to the firmware over MIDI SysEx. The emulated FM-1 (fm1-emu) has no MIDI out, so it cannot answer; the native and .rfm1 engines run ${e.name}'s own code and do.`, "muted");
      if (own && s.engine !== own) btn(`Switch ${e.name} to ${ownName}`, () => this.d.setEngine(own));
    } else if (own) {
      // the firmware runs its own code here, but its editor is not hosted on this site (yet), and its page could not be
      // loaded live: only its own site has it
      p(`${e.name}'s editor is not hosted on this site yet.`, "ed-lead");
      p(`It lives on ${e.name}'s own site, which cannot reach the firmware running in this page. Open it there with ${e.name} on your FM-1 over USB (Chrome or Edge).`, "muted");
      if (ed.live && this.liveFail.has(ed.live)) p(`(Its live page from ${hostOf(ed.live)} did not load here: ${this.liveFail.get(ed.live)}.)`, "muted");
    } else {
      p("This firmware's editor needs a real FM-1.", "ed-lead");
      p(`${e.name} has no native or .rfm1 build on this site, and the emulated FM-1 has no MIDI out, so its editor cannot talk to the firmware here. Open it on its own site with ${e.name} on your FM-1 over USB (Chrome or Edge).`, "muted");
    }
    if (ed.kind === "real") {
      const a = document.createElement("a"); a.className = "pill primary"; a.href = ed.url; a.target = "_blank"; a.rel = "noopener"; a.textContent = `Open ${e.name}'s editor ↗`;
      box.append(a);
    }
    this.body.replaceChildren(box);
  }

  // ---- the author's live page: fetched, the shim put in, in a sandbox; it falls back when it cannot be read, does
  // not answer in time, or its shim does not say hello
  shimText() {
    if (!this.shimP) this.shimP = fetch(new URL("fm1-shim.js", this.base), { cache: "no-cache" }).then((r) => { if (!r.ok) throw new Error(`the shim: HTTP ${r.status}`); return r.text(); });
    this.shimP.catch(() => { this.shimP = null; });
    return this.shimP;
  }
  loadLive(ed) {
    const s = this.d.getState(), e = s.entry, url = ed.live, g = ++this.gen;
    this.frameFor = "live:" + url;
    this.live = { url, final: url, host: hostOf(url), store: new EditorStore(url), win: null, frame: null, ready: false };
    const box = document.createElement("div");
    box.className = "ed-note";
    const p = document.createElement("p"); p.className = "muted"; p.textContent = `Loading ${this.title(s, ed)} from ${hostOf(url)}…`;
    box.append(p);
    this.body.replaceChildren(box);
    const fail = (why) => {
      if (g !== this.gen) return;
      console.warn(`the Editor panel: ${url}: ${why}; ${ed.kind === "hosted" ? "the hosted copy" : "the link"} instead`);
      this.liveFail.set(url, why);
      this.dropFrame(); this.render(); this.update();
    };
    Promise.all([fetchLive(url, { fetchFn: this.d.fetchFn }), this.shimText()]).then(([page, shim]) => {
      if (g !== this.gen) return;
      const live = this.live;
      live.final = page.url; live.host = hostOf(page.url);
      const f = document.createElement("iframe");
      f.className = "ed-frame"; f.title = `${this.title(s, ed)} (live from ${live.host})`;
      f.setAttribute("sandbox", LIVE_SANDBOX);
      f.setAttribute("referrerpolicy", "no-referrer");
      f.srcdoc = injectLive(page.html, { base: page.url, shim, manager: ed.kind === "hosted" ? ed.manager.id : e.id, storage: live.store.seed() });
      let loads = 0, t0 = Date.now();
      const check = () => {
        if (g !== this.gen) return;
        if (this.bridge.hasFrame(live.win)) { live.ready = true; this.renderHead(); return; }
        if (Date.now() - t0 > HELLO_MS) fail(loads > 1 ? "its page went to another page, without the player's MIDI" : `its page did not start the player's MIDI in ${HELLO_MS / 1000} s`);
        else setTimeout(check, 100);
      };
      f.addEventListener("load", () => {
        // (the frame loaded again: the page reloaded itself (its shim said bye, and hello again) or went to another page
        // (a link the shim did not catch, a form: bye, and no hello); no session from it in time: the panel falls back)
        if (++loads > 1 && g === this.gen && this.live) { live.ready = false; t0 = Date.now(); this.renderHead(); check(); }
      });
      this.body.replaceChildren(f);
      live.frame = f; live.win = f.contentWindow;
      this.bridge.allowFrame(live.win);
      check();
    }, (err) => fail(err && err.message ? err.message : String(err)));
  }
}
