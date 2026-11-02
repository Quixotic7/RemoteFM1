// SPDX-License-Identifier: GPL-3.0-only
// The header's Editor button and its panel: the running firmware's own manager / editor (web/managers/, hosted on this
// site) in an iframe beside the FM-1, talking to the firmware through managers.js ManagerBridge. The FM-1 keeps
// playing, with sound, while the panel is open; "Pop out" opens the manager in a tab of its own (a BroadcastChannel).
//   native firmware (or an .rfm1 file) with a hosted manager   the manager
//   emulated (or the test engine)           "Editors work with the native engine; switch Engine to Native"
//   native / .rfm1, an editor only on its site   "not hosted on this site yet" and a link to it
//   emulated only, an editor on its site    "This firmware's editor needs a real FM-1" and a link to it
//   RemoteFM1 itself on the .rfm1 engine    its DX7 cartridges (js/cart.js) through the in-page port (cartPort); on
//                                           the emulated engine "switch Engine to .rfm1" (fm1-emu's MIDI in is the
//                                           TRS jack, which takes no cartridge, and it has no MIDI out)
import { ManagerBridge, editorOf, portOf } from "./managers.js";
import { mountCart, corePort } from "./cart.js";

const $ = (id) => document.getElementById(id);
// the engines that run the firmware's own code, so its manager's SysEx gets answers: native, and an .rfm1 file
const runsOwnCode = (s) => s.engine === "native" || s.engine === "rfm1";

export class EditorPanel {
  // deps: getState() (main.js state), index (managers/index.json or null), base (the managers folder's URL),
  //       toEngine(bytes), toCart(bytes) (into RemoteFM1's own MIDI in), setEngine(name), onLayout()
  constructor(deps) {
    this.d = deps;
    this.index = deps.index || null;
    this.base = deps.base;
    this.open = false;
    this.frameFor = null;              // the manager id the iframe shows
    this.bridge = new ManagerBridge({ toEngine: deps.toEngine, onChange: () => this.renderHead() });
    this.cartPort = corePort({ name: "RemoteFM1 in this page", send: (b) => deps.toCart(b) });
    this.btn = $("ed-btn"); this.panel = $("ed-panel"); this.body = $("ed-body");
    this.btn.addEventListener("click", () => (this.open ? this.close() : this.show()));
    $("ed-close").addEventListener("click", () => this.close());
    $("ed-pop").addEventListener("click", () => this.popOut());
  }
  get editor() { const s = this.d.getState(); return editorOf(s.entry, this.index); }
  // the engine's MIDI out (any chunking): the managers' sessions and the cartridge sender
  fromEngine(bytes) { this.bridge.fromEngine(bytes); this.cartPort.feed(bytes); }
  cartReady() { const s = this.d.getState(); return s.engine === "rfm1" && s.engineState === "running" && !!s.info; }
  url(m) { return new URL(m.path, this.base).href; }

  // the firmware, its engine or its state changed: the button, the bridge's port, the panel
  update() {
    const s = this.d.getState(), e = s.entry, ed = this.editor;
    this.btn.hidden = !ed;
    if (ed) {
      this.btn.title = ed.kind === "hosted" ? `${ed.manager.title}: ${e.name}'s own editor, on the firmware running here`
        : ed.kind === "cart" ? `DX7 cartridges: send a .syx to ${e.name} running here` : `${e.name}'s editor (on its own site, for an FM-1 on USB)`;
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
    if (!this.frameFor) return;
    this.body.replaceChildren(); this.frameFor = null;
    this.bridge.forgetFrames();
  }
  popOut() {
    const ed = this.editor;
    if (!ed) return;
    const href = ed.kind === "hosted" ? this.url(ed.manager) : ed.url;
    window.open(href, "_blank", "noopener");
    if (ed.kind === "hosted") this.close();
  }

  renderHead() {
    if (!this.open) return;
    const s = this.d.getState(), ed = this.editor, t = this.bridge.target;
    $("ed-title").textContent = ed ? (ed.kind === "hosted" ? ed.manager.title : ed.kind === "cart" ? "DX7 cartridges" : `${s.entry.name}'s editor`) : "Editor";
    const dot = $("ed-dot"), cart = ed && ed.kind === "cart";
    dot.className = "fm1-dot " + ((cart ? this.cartReady() : t.connected) ? "on" : "off");
    $("ed-sub").textContent = cart ? (this.cartReady() ? `${s.entry.name} ${s.info.version || ""} in this page`.trim() : s.engine === "rfm1" ? "starting the firmware..." : "no port")
      : t.connected
      ? `port "${t.port}" · ${s.entry.name} in this page${this.bridge.count > 1 ? ` · ${this.bridge.count} editors` : ""}`
      : ed && ed.kind === "hosted" && runsOwnCode(s) && s.engineState !== "running" ? "starting the firmware..." : "no port";
    $("ed-pop").hidden = !ed || cart;
    $("ed-pop").textContent = ed && ed.kind === "real" ? "Open its site ↗" : "Pop out ↗";
    const m = ed && ed.kind === "hosted" ? ed.manager : null;
    const src = $("ed-src");
    src.hidden = !m;
    if (m) { src.href = m.source; src.title = `${m.title} by ${m.author} (${m.license}): its source at the commit hosted here`; }
  }

  render() {
    const s = this.d.getState(), e = s.entry, ed = this.editor;
    this.renderHead();
    if (!ed) { this.dropFrame(); return; }
    if (ed.kind === "cart" && s.engine === "rfm1") {
      if (this.frameFor === "cart") return;               // (kept through a reboot: the port follows the engine)
      this.dropFrame();
      const box = document.createElement("div");
      box.className = "ed-note ed-cart";
      this.cartPort.reset();
      mountCart(box, null, null, { inPage: { port: this.cartPort, ready: () => this.cartReady() } });
      this.body.replaceChildren(box);
      this.frameFor = "cart";
      return;
    }
    if (ed.kind === "hosted" && runsOwnCode(s)) {
      if (this.frameFor === ed.manager.id) return;          // (the same manager stays: a reboot only moves its port)
      this.dropFrame();
      const f = document.createElement("iframe");
      f.className = "ed-frame"; f.title = ed.manager.title; f.src = this.url(ed.manager);
      this.body.replaceChildren(f);
      this.frameFor = ed.manager.id;
      return;
    }
    this.dropFrame();
    const box = document.createElement("div");
    box.className = "ed-note";
    const p = (text, cls = "") => { const el = document.createElement("p"); el.textContent = text; if (cls) el.className = cls; box.append(el); return el; };
    const btn = (text, onclick, cls = "pill primary") => { const b = document.createElement("button"); b.className = cls; b.textContent = text; b.onclick = onclick; box.append(b); return b; };
    if (ed.kind === "cart") {
      const own = e.engines.includes("rfm1");
      p(`The DX7 cartridge sender works with ${e.name}'s .rfm1 engine${own ? "; switch Engine to .rfm1" : ", which this site does not have for this version"}.`, "ed-lead");
      p(`${e.name} takes cartridges as SysEx on its USB MIDI. The emulated FM-1 (fm1-emu) has MIDI in only on the TRS jack, which takes no cartridge, and no MIDI out for the replies.`, "muted");
      if (own && s.engine !== "rfm1") btn(`Switch ${e.name} to .rfm1`, () => this.d.setEngine("rfm1"));
    } else if (ed.kind === "hosted") {
      // the engine to offer: the firmware's own code, native or its .rfm1 (whichever the catalog lists first)
      const own = e.engines.find((k) => k === "native" || k === "rfm1") || null;
      const ownName = own === "rfm1" ? ".rfm1" : "Native";
      p(s.engine === "mock" && (s.wantEngine === "native" || s.wantEngine === "rfm1")
        ? `The ${s.wantEngine === "rfm1" ? ".rfm1" : "native"} engine could not start here, so ${ed.manager.title} has no firmware to talk to.`
        : `Editors work with the firmware's own code; switch Engine to ${own ? ownName : "Native"}.`, "ed-lead");
      p(`${ed.manager.title} talks to the firmware over MIDI SysEx. The emulated FM-1 (fm1-emu) has no MIDI out, so it cannot answer; the native and .rfm1 engines run ${e.name}'s own code and do.`, "muted");
      if (own && s.engine !== own) btn(`Switch ${e.name} to ${ownName}`, () => this.d.setEngine(own));
    } else if (e.engines.some((k) => k === "native" || k === "rfm1")) {
      // the firmware runs its own code here, but its editor is not hosted on this site (yet): only its own site has it
      p(`${e.name}'s editor is not hosted on this site yet.`, "ed-lead");
      p(`It lives on ${e.name}'s own site, which cannot reach the firmware running in this page. Open it there with ${e.name} on your FM-1 over USB (Chrome or Edge).`, "muted");
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
}
