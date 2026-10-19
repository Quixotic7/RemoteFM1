// SPDX-License-Identifier: GPL-3.0-only
// The header's Editor button and its panel: the running firmware's own manager / editor (web/managers/, hosted on this
// site) in an iframe beside the FM-1, talking to the firmware through managers.js ManagerBridge. The FM-1 keeps
// playing, with sound, while the panel is open; "Pop out" opens the manager in a tab of its own (a BroadcastChannel).
//   native firmware with a hosted manager   the manager
//   emulated (or the test engine)           "Editors work with the native engine; switch Engine to Native"
//   no native build, an editor on its site  "This firmware's editor needs a real FM-1" and a link to it
import { ManagerBridge, editorOf, portOf } from "./managers.js";

const $ = (id) => document.getElementById(id);

export class EditorPanel {
  // deps: getState() (main.js state), index (managers/index.json or null), base (the managers folder's URL),
  //       toEngine(bytes), setEngine(name), onLayout()
  constructor(deps) {
    this.d = deps;
    this.index = deps.index || null;
    this.base = deps.base;
    this.open = false;
    this.frameFor = null;              // the manager id the iframe shows
    this.bridge = new ManagerBridge({ toEngine: deps.toEngine, onChange: () => this.renderHead() });
    this.btn = $("ed-btn"); this.panel = $("ed-panel"); this.body = $("ed-body");
    this.btn.addEventListener("click", () => (this.open ? this.close() : this.show()));
    $("ed-close").addEventListener("click", () => this.close());
    $("ed-pop").addEventListener("click", () => this.popOut());
  }
  get editor() { const s = this.d.getState(); return editorOf(s.entry, this.index); }
  url(m) { return new URL(m.path, this.base).href; }

  // the firmware, its engine or its state changed: the button, the bridge's port, the panel
  update() {
    const s = this.d.getState(), e = s.entry, ed = this.editor;
    this.btn.hidden = !ed;
    if (ed) {
      this.btn.title = ed.kind === "hosted" ? `${ed.manager.title}: ${e.name}'s own editor, on the firmware running here` : `${e.name}'s editor (it needs a real FM-1)`;
      this.btn.setAttribute("aria-expanded", String(this.open));
      this.btn.classList.toggle("on", this.open);
    }
    const running = s.engine === "native" && s.engineState === "running" && !!s.info;
    const port = running ? portOf(e, this.index) : null;
    this.bridge.setTarget({ port, firmware: port ? `${e.name} ${s.info.version || ""}`.trim() : null, engine: port ? "native" : null });
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
    $("ed-title").textContent = ed ? (ed.kind === "hosted" ? ed.manager.title : `${s.entry.name}'s editor`) : "Editor";
    const dot = $("ed-dot");
    dot.className = "fm1-dot " + (t.connected ? "on" : "off");
    $("ed-sub").textContent = t.connected
      ? `port "${t.port}" · ${s.entry.name} in this page${this.bridge.count > 1 ? ` · ${this.bridge.count} editors` : ""}`
      : ed && ed.kind === "hosted" && s.engine === "native" && s.engineState !== "running" ? "starting the firmware..." : "no port";
    $("ed-pop").hidden = !ed;
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
    const native = s.engine === "native";
    if (ed.kind === "hosted" && native) {
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
    if (ed.kind === "hosted") {
      const canNative = e.engines.includes("native");
      p(s.engine === "mock" && s.wantEngine === "native"
        ? `The native engine could not start here, so ${ed.manager.title} has no firmware to talk to.`
        : "Editors work with the native engine; switch Engine to Native.", "ed-lead");
      p(`${ed.manager.title} talks to the firmware over MIDI SysEx. The emulated FM-1 (fm1-emu) has no MIDI out, so it cannot answer; the native engine runs ${e.name}'s own code and does.`, "muted");
      if (canNative && s.engine !== "native") btn(`Switch ${e.name} to Native`, () => this.d.setEngine("native"));
    } else {
      p("This firmware's editor needs a real FM-1.", "ed-lead");
      p(`${e.name} has no native build on this site, and the emulated FM-1 has no MIDI out, so its editor cannot talk to the firmware here. Open it on its own site with ${e.name} on your FM-1 over USB (Chrome or Edge).`, "muted");
      const a = document.createElement("a"); a.className = "pill primary"; a.href = ed.url; a.target = "_blank"; a.rel = "noopener"; a.textContent = `Open ${e.name}'s editor ↗`;
      box.append(a);
    }
    this.body.replaceChildren(box);
  }
}
