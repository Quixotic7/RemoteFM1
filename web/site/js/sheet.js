// SPDX-License-Identifier: GPL-3.0-only
// The settings sheet: FM1VST plugin/Sheet.cpp's Graphite sheet, as DOM. It slides in from the right over a dimmed
// scrim (180 ms, eased); a click on the scrim or Esc closes it. Pages:
//   main      FIRMWARE, SAVES, USE MY FM-1, PLAYING, LOOK, VIEW, ENGINE, ABOUT
//   firmware  the browser: a search field, category chips, a card per firmware (the loaded one outlined)
//   about     the loaded firmware (author, licence, its source at the version that runs), the engine, the credits
// app (main.js) holds the state and the actions; the sheet re-renders its page on app.changed().
import { PRESETS, accentOf, onAccent, css, hex } from "./theme.js";
import { AUTOSAVE } from "./saves.js";
import { browse, cardView, ENGINE_FILTERS } from "./live.js";
import { MHZ_CHOICES, DEVICE_MHZ, DEVICE_RANGE, OVERCLOCK, DUAL_CORE, isOverclock, inDeviceRange } from "./cpu.js";

// ---- icons: FM1VST Look::icon, 16 x 16 boxes
const ICON = {
  dots: '<circle cx="3.5" cy="8" r="1.5"/><circle cx="8" cy="8" r="1.5"/><circle cx="12.5" cy="8" r="1.5"/>',
  left: '<path d="M10 3.5 5.5 8 10 12.5"/>', right: '<path d="M6 3.5 10.5 8 6 12.5"/>', down: '<path d="M4 6l4 4 4-4"/>',
  close: '<path d="M4 4l8 8M12 4l-8 8"/>', search: '<circle cx="7" cy="7" r="4.5"/><path d="M10.2 10.2 13.5 13.5"/>',
  check: '<path d="M3.5 8.5l3 3 6-7"/>', info: '<circle cx="8" cy="8" r="6"/><path d="M8 7.2v4"/><circle cx="8" cy="4.9" r=".4"/>',
  plus: '<path d="M8 3.5v9M3.5 8h9"/>', file: '<path d="M4 2.5h5l3 3v8H4z"/><path d="M9 2.5v3h3"/>',
};
export const icon = (name, cls = "ic") =>
  `<svg class="${cls}" viewBox="0 0 16 16" aria-hidden="true" fill="${name === "dots" ? "currentColor" : "none"}" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">${ICON[name]}</svg>`;

// ---- a tiny DOM helper
export function h(tag, attrs = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === "class") el.className = v;
    else if (k === "html") el.innerHTML = v;
    else if (k === "style" && typeof v === "object") Object.assign(el.style, v);
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) if (c != null && c !== false) el.append(c.nodeType ? c : document.createTextNode(String(c)));
  return el;
}
const heading = (text, extra) => h("h3", { class: "heading" }, text, extra || null);
const row = (label, sub, control, opts = {}) =>
  h("div", { class: "row" + (opts.disabled ? " disabled" : "") }, h("div", { class: "row-label" }, h("div", {}, label), sub ? h("div", { class: "row-sub" }, sub) : null), control);
function toggle(on, onChange, disabled) {
  return h("button", { class: "switch" + (on ? " on" : ""), role: "switch", "aria-checked": String(!!on), disabled, onclick: () => onChange(!on) }, h("span"));
}
function segmented(labels, sel, onPick, disabledSet = new Set()) {
  return h("div", { class: "seg", role: "radiogroup" }, labels.map((l, i) =>
    h("button", { class: i === sel ? "on" : "", role: "radio", "aria-checked": String(i === sel), disabled: disabledSet.has(i), onclick: () => onPick(i) }, l)));
}
function stepper(value, onStep) {
  return h("div", { class: "stepper" },
    h("button", { "aria-label": "less", onclick: () => onStep(-1), html: '<svg viewBox="0 0 16 16"><path d="M3.5 8h9"/></svg>' }),
    h("span", {}, value),
    h("button", { "aria-label": "more", onclick: () => onStep(1), html: '<svg viewBox="0 0 16 16"><path d="M3.5 8h9M8 3.5v9"/></svg>' }));
}
const pill = (text, onclick, kind = "", extra = {}) => h("button", { class: "pill " + kind, onclick, ...extra }, text);
const link = (href, text) => h("a", { href, target: "_blank", rel: "noopener" }, text);

export class Sheet {
  constructor(app) {
    this.app = app;
    this.page = "main";
    this.search = "";
    this.cat = 0;
    this.inline = null;          // { kind: "saveas" | "rename" | "delete", rec }
    this.scrim = h("div", { class: "scrim", onclick: () => this.close() });
    this.head = h("header", { class: "sheet-head" });
    this.body = h("div", { class: "sheet-body" });
    this.el = h("aside", { class: "sheet", "aria-label": "Settings", "aria-hidden": "true", tabindex: "-1" }, this.head, this.body);
    document.body.append(this.scrim, this.el);
    this.remoteSlot = h("div", { class: "remote-slot" });
  }
  get isOpen() { return this.el.classList.contains("open"); }
  open(page = "main") {
    this.show(page);
    this.el.classList.add("open"); this.scrim.classList.add("open");
    this.el.setAttribute("aria-hidden", "false");
    this.el.focus({ preventScroll: true });
    this.app.sheetChanged(true);
  }
  close() {
    this.el.classList.remove("open"); this.scrim.classList.remove("open");
    this.el.setAttribute("aria-hidden", "true");
    this.inline = null;
    this.app.sheetChanged(false);
  }
  show(page) {
    this.page = page;
    this.el.dataset.page = page;
    this.render();
    this.body.scrollTop = 0;
    if (page === "firmware") setTimeout(() => { const s = this.body.querySelector("input[type=search]"); if (s && matchMedia("(pointer: fine)").matches) s.focus(); }, 200);
  }
  render() {
    const a = this.app, st = a.state;
    const accent = accentOf(st.theme);
    this.el.style.setProperty("--accent", css(accent));
    this.el.style.setProperty("--on-accent", css(onAccent(accent)));
    this.el.style.setProperty("--accent-soft", css(accent, 0.12));
    // the header: the mark (main) or the page's title
    this.head.replaceChildren(
      this.page === "main"
        ? h("div", { class: "brand" }, h("span", { class: "mark" }), h("b", {}, "FM-1 Player"), h("span", { class: "ver" }, "RemoteFM1"))
        : h("div", { class: "brand" }, h("button", { class: "icon-btn", "aria-label": "Back", onclick: () => this.show("main"), html: icon("left") }),
            h("b", {}, this.page === "firmware" ? "Firmware" : "About")),
      h("button", { class: "icon-btn", "aria-label": "Close", onclick: () => this.close(), html: icon("close") }));
    const keep = this.page === this.lastPage ? this.body.scrollTop : 0;
    this.body.replaceChildren(...(this.page === "firmware" ? this.firmwarePage() : this.page === "about" ? this.aboutPage() : this.mainPage()));
    this.body.scrollTop = keep;
    this.lastPage = this.page;
  }

  // ================================================================ main ===
  mainPage() {
    const a = this.app, st = a.state, e = st.entry;
    const out = [];
    // FIRMWARE
    out.push(heading("Firmware"));
    out.push(h("button", { class: "fw-card", onclick: () => this.show("firmware") },
      h("span", { class: "led" }),
      h("span", { class: "fw-text" },
        h("span", { class: "fw-top" }, h("b", {}, e ? e.name : "No firmware"), h("span", { class: "muted" }, e ? a.versionOf(e, st.wantEngine) : "")),
        h("span", { class: "fw-meta" + (st.engineState === "mock" ? " warn" : "") }, a.metaLine())),
      h("span", { class: "chev", html: icon("right") })));

    // SAVES
    out.push(heading("Saves"));
    out.push(...this.savesSection());

    // USE MY FM-1 (W4)
    out.push(heading("Use my FM-1"));
    out.push(this.remoteSection());

    // PLAYING
    out.push(heading("Playing"));
    out.push(row("Transpose", "octaves, for MIDI notes", stepper(String(st.transpose), (d) => a.set("transpose", Math.max(-3, Math.min(3, st.transpose + d))))));
    const m = a.midi;
    let midiCtl;
    if (!m.supported) midiCtl = h("span", { class: "muted small" }, "no Web MIDI here");
    else if (!m.access) midiCtl = pill("Enable MIDI", () => a.enableMidi());
    else {
      const sel = h("select", { class: "select", "aria-label": "MIDI input", onchange: (ev) => a.selectMidi(ev.target.value) },
        h("option", { value: "" }, "None"), h("option", { value: "*" }, "All inputs"),
        m.inputs().map((i) => h("option", { value: i.id }, i.name + (i.state === "disconnected" ? " (gone)" : ""))));
      sel.value = m.selected;
      midiCtl = sel;
    }
    out.push(row("MIDI input", m.error || (m.access ? `${m.inputs().length} found` : "a MIDI keyboard plays the keys"), midiCtl));
    out.push(row("MIDI notes play keys", null, toggle(st.notesPlayKeys, (v) => a.set("notesPlayKeys", v))));
    out.push(row("...and reach the firmware's MIDI in", null, toggle(st.reachFirmware, (v) => a.set("reachFirmware", v))));
    out.push(row("Show computer-key hints", "the keyboard letters on the panel's keys and buttons", toggle(st.keyHints, (v) => a.set("keyHints", v))));
    out.push(h("p", { class: "note" }, "Keys: A S D F G H J K L ; ' ] white, W E T Y U O P black, 2 3 4 5 Tab F1-F4 the left block. Buttons: F5-F10 or C V B N M , and 7 8 9 0 - =. Z X octave. Page Up/Down pick a knob, arrows turn it. ` the screen view."));

    // LOOK
    out.push(heading("Look", h("span", { class: "heading-val" }, st.theme.name)));
    out.push(h("div", { class: "swatches" }, PRESETS.map((t) => {
      const b = h("button", { class: "swatch" + (t.name === st.theme.name ? " on" : ""), title: t.name, "aria-label": t.name, onclick: () => a.set("theme", t.name) });
      b.style.background = `linear-gradient(135deg, ${t.base} 0 50%, ${t.membrane} 50% 100%)`;
      return b;
    })));

    // VIEW
    out.push(heading("View"));
    out.push(row("Show", null, segmented(["Device", "Screen"], st.view === "screen" ? 1 : 0, (i) => a.set("view", i ? "screen" : "device"))));

    // ENGINE
    out.push(heading("Engine"));
    if (e) {
      const both = e.engines.length > 1;
      const shown = st.engine === "mock" ? st.wantEngine : st.engine;
      if (both && e.nativeBehind) {
        // two versions: the Hub's latest (emulated, the default) and FM1VST's older core (native), one per line
        out.push(row("Runs on", `the Hub has ${e.version}; the native core is ${e.native.version}`, null));
        out.push(h("div", { class: "seg stack", role: "radiogroup" }, [["emu", "Emulated", e.version, "latest"], ["native", "Native", e.native.version, "older, faster"]].map(([k, label, v, why]) =>
          h("button", { class: shown === k ? "on" : "", role: "radio", "aria-checked": String(shown === k), onclick: () => a.setEngine(k) }, `${label} ${v}`, h("small", {}, `(${why})`)))));
      } else {
        out.push(row("Runs on", both ? null : e.nativeGated ? "native build not public: " + e.nativeGated : e.native ? "native only (no longer on the Hub)" : "emulated only",
          segmented(["Native", "Emulated"], shown === "native" ? 0 : 1, (i) => a.setEngine(i ? "emu" : "native"),
            new Set(both ? [] : e.native ? [1] : [0]))));
      }
      // the emulated CPU clock (emu only): Auto, the device's range (320-360), an overclock (480, 640)
      if (shown === "emu") {
        const c = st.mhzChoice;
        const sub = c === "auto" ? (st.mhz === DEVICE_MHZ ? `Auto: ${DEVICE_MHZ} MHz, the FM-1's own clock; lower if this computer can't keep up` : `Auto: stepped down to ${st.mhz} MHz for this computer`)
          : isOverclock(c) ? `${c} MHz, ${OVERCLOCK}` : inDeviceRange(c) ? `${c} MHz, the FM-1's own range` : `${c} MHz, under the FM-1's clock: lighter, may stutter`;
        out.push(row("Emulated CPU", sub, null));
        out.push(h("div", { class: "seg wide", role: "radiogroup", "aria-label": "Emulated CPU clock" }, MHZ_CHOICES.map((m) =>
          h("button", { class: (m === c ? "on" : "") + (isOverclock(m) ? " oc" : inDeviceRange(m) ? " dev" : ""), role: "radio", "aria-checked": String(m === c),
            title: m === "auto" ? `${DEVICE_MHZ} MHz, stepping down if this computer can't keep up` : isOverclock(m) ? OVERCLOCK : inDeviceRange(m) ? "the FM-1's own range" : "under the FM-1's clock",
            onclick: () => a.setMhz(m) }, m === "auto" ? "Auto" : `${m}`))));
        out.push(h("div", { class: "mhz-scale" }, h("span", { class: "dev" }, "the FM-1"), h("span", { class: "oc" }, "overclock")));
        out.push(h("p", { class: "note" }, `MHz. The FM-1 runs at about ${DEVICE_RANGE[0]}-${DEVICE_RANGE[1]} MHz; 480 and 640 are an overclock: more headroom for heavy firmwares, more work for this computer. A change restarts the firmware from its running flash.`));
      }
    }

    // ABOUT
    out.push(heading("About"));
    out.push(h("button", { class: "fw-card slim", onclick: () => this.show("about") },
      h("span", { class: "ic-wrap", html: icon("info") }),
      h("span", { class: "fw-text" }, h("span", { class: "fw-top" }, h("b", {}, e ? `${e.name} and credits` : "Credits")),
        h("span", { class: "fw-meta" }, e ? `${e.author}${e.license ? " · " + e.license : ""}` : "")),
      h("span", { class: "chev", html: icon("right") })));
    if (e) out.push(this.sourceLines(true));
    out.push(h("div", { class: "links" }, h("a", { href: a.installerUrl }, "Install RemoteFM1 on your FM-1"), " · ",
      link("https://github.com/Quixotic7/RemoteFM1", "RemoteFM1 (this site) on GitHub")));
    return out;
  }
  // the running firmware's source at the version that runs (the header's link), and a native core's FM1VST glue
  sourceLines(compact) {
    const a = this.app, e = a.state.entry, s = a.sourceNow();
    const short = (r, kind) => kind === "commit" ? r.slice(0, 7) : r;
    const srcLink = s ? h("a", { class: "src-inline", href: s.url, target: "_blank", rel: "noopener", title: s.title },
      `${s.repo}${s.ref ? " @ " + short(s.ref, s.refKind) : ""} ↗`) : h("span", { class: "muted" }, "no public source (a package from your computer)");
    const glue = s && s.glue ? link(s.glue.url, `FM1VST ${s.glue.path}`) : null;
    if (!compact) return [srcLink, glue];
    return h("div", { class: "src-lines" },
      h("div", {}, h("span", { class: "muted" }, `${e.name}'s source: `), srcLink),
      glue ? h("div", {}, h("span", { class: "muted" }, "built with FM1VST's glue: "), glue) : null);
  }

  savesSection() {
    const a = this.app, st = a.state, saves = st.saves || [];
    const out = [];
    // a save of an older version, offered once (main.js offerCarry)
    if (st.carryOffer) out.push(h("div", { class: "carry" }, a.carryText(st.carryOffer),
      h("div", { class: "pills two" }, pill("Bring it", () => a.answerCarry(true), "primary"), pill("Start fresh", () => a.answerCarry(false), "ghost"))));
    const mine = saves.filter((r) => r.engine === st.engine && r.version === a.versionOf(st.entry, st.engine));
    const named = mine.filter((r) => r.name !== AUTOSAVE);
    const sel = named.find((r) => r.name === st.selectedSave) || null;
    const auto = mine.find((r) => r.name === AUTOSAVE);
    // the nav: ‹ name ›
    const names = named.map((r) => r.name);
    const i = sel ? names.indexOf(sel.name) : -1;
    const step = (d) => { if (!names.length) return; const j = ((i < 0 ? (d > 0 ? -1 : 0) : i) + d + names.length) % names.length; a.loadSave(named[j]); };
    const picker = h("select", { class: "nav-select", "aria-label": "Saves", onchange: (ev) => { const r = named.find((x) => x.name === ev.target.value); if (r) a.loadSave(r); } },
      h("option", { value: "" }, auto ? "Autosave (running)" : "No save yet"), names.map((n) => h("option", { value: n }, n)));
    picker.value = sel ? sel.name : "";
    out.push(h("div", { class: "nav" },
      h("button", { class: "nav-arrow", "aria-label": "Previous save", onclick: () => step(-1), html: icon("left") }),
      h("div", { class: "nav-mid" }, picker, h("span", { class: "nav-chev", html: icon("down") })),
      h("button", { class: "nav-arrow", "aria-label": "Next save", onclick: () => step(1), html: icon("right") })));
    // inline: Save as / Rename (a name), Delete (confirm)
    if (this.inline && (this.inline.kind === "saveas" || this.inline.kind === "rename")) {
      const input = h("input", { class: "text", type: "text", maxlength: "40", placeholder: "Name", value: this.inline.kind === "rename" && sel ? sel.name : "" });
      const ok = async () => {
        const n = input.value.trim();
        if (!n || n === AUTOSAVE) return input.focus();
        const kind = this.inline.kind;
        this.inline = null;
        if (kind === "saveas") await a.saveAs(n); else if (sel) await a.renameSave(sel, n);
      };
      input.addEventListener("keydown", (ev) => { if (ev.key === "Enter") ok(); });
      out.push(h("div", { class: "inline" }, input, pill(this.inline.kind === "saveas" ? "Save" : "Rename", ok, "primary"), pill("Cancel", () => { this.inline = null; this.render(); }, "ghost")));
      setTimeout(() => input.focus(), 0);
    } else if (this.inline && this.inline.kind === "delete" && sel) {
      out.push(h("div", { class: "inline" }, h("span", { class: "grow" }, `Delete "${sel.name}"?`),
        pill("Delete", async () => { this.inline = null; await a.deleteSave(sel); }, "danger-fill"), pill("Cancel", () => { this.inline = null; this.render(); }, "ghost")));
    }
    const fileIn = h("input", { type: "file", accept: ".json,.bin,.fm1save,application/json,application/octet-stream", hidden: true,
      onchange: (ev) => { const f = ev.target.files[0]; ev.target.value = ""; if (f) a.importSaves(f); } });
    out.push(h("div", { class: "pills" },
      pill("Save", () => a.saveOver(sel), "", { disabled: !sel || !st.running, title: sel ? `Write the running flash over "${sel.name}"` : "Pick a save first" }),
      pill("Save as", () => { this.inline = { kind: "saveas" }; this.render(); }, "", { disabled: !st.running }),
      pill("Rename", () => { this.inline = { kind: "rename" }; this.render(); }, "", { disabled: !sel }),
      pill("Delete", () => { this.inline = { kind: "delete" }; this.render(); }, "danger", { disabled: !sel }),
      pill("Export...", () => a.exportSaves(), "", { disabled: !saves.length, title: "A backup file of every save of this firmware" }),
      pill("Import...", () => fileIn.click()), fileIn));
    out.push(h("p", { class: "note" }, auto
      ? `The firmware's flash is kept in this browser as it changes (${new Date(auto.saved).toLocaleString()}). A save is a copy you keep; picking one restarts the firmware from it.`
      : "The firmware's flash is kept in this browser as it changes. A save is a copy you keep; picking one restarts the firmware from it."));
    return out;
  }

  remoteSection() {
    // W4 mounts into remoteSlot (kept across renders); until then, the stub
    const a = this.app;
    if (!a.remoteMounted) {
      this.remoteSlot.replaceChildren(
        h("p", { class: "row-sub", style: { margin: "0 0 10px" } }, "Play this firmware from your own FM-1: its keys, knobs and screen, the sound here. The FM-1 needs RemoteFM1."),
        h("div", { class: "pills two" },
          pill("Connect my FM-1", null, "", { disabled: true, title: "Coming soon" }),
          h("a", { class: "pill", href: a.installerUrl }, "Install RemoteFM1")),
        h("p", { class: "remote-status muted small" }, a.remoteStatus || "Coming soon."));
    }
    return this.remoteSlot;
  }

  // ============================================================ firmware ===
  firmwarePage() {
    const a = this.app, cat = a.catalog, st = a.state;
    const cats = cat.categories;
    const grid = h("div", { class: "cards" });
    const fill = () => {
      const eng = st.browseEngine || "all";
      const list = browse(cat.firmwares, { engine: eng, cat: cats[this.cat], q: this.search });
      grid.replaceChildren(...list.map((f) => {
        // a card with a badge (RemoteFM1's "meta") is wider and shows its blurb in place of the tag
        const v = cardView(f, eng), meta = f.blurb || `${f.tag} · ${f.author}`;
        // a dual-core firmware, when it would run emulated: CPU-heavy
        const runsEmu = v.engine ? v.engine === "emu" : f.defaultEngine === "emu";
        const note = v.note || (runsEmu && DUAL_CORE.has(f.id) ? "CPU-heavy" : null);
        const card = h("button", { class: "card" + (f.badge ? " badged" : "") + (st.entry && f.id === st.entry.id ? " current" : ""), title: `${f.name} ${v.version}\n${f.what || ""}`,
          "data-id": f.id, onclick: () => { a.openFirmware(f.id, v.engine); this.close(); } },
          h("span", { class: "card-top" }, h("b", {}, f.name), f.badge ? h("span", { class: "card-tag" }, f.badge) : null, h("span", { class: "card-ver" }, v.version)),
          note
            ? h("span", { class: "card-meta split" }, h("span", {}, meta), h("span", { class: "card-note" + (v.note ? "" : " heavy"),
                title: v.note ? (eng === "native" ? `The Hub has ${f.version}; FM1VST's native core is ${f.native.version}` : `FM1VST's native core is ${f.native.version}; the Hub's latest ${f.version} runs emulated`)
                  : "Runs both of the FM-1's cores: heavy to emulate" }, note))
            : h("span", { class: "card-meta" }, meta),
          f.native ? h("span", { class: "card-badge", title: "Runs natively (WebAssembly)" }) : null);
        return card;
      }), h("label", { class: "card file", title: "Run a package from your computer, emulated (for example M-VAVE's FM-1.fwsc)" },
        h("span", { class: "card-top" }, h("b", {}, "Open a file..."), h("span", { class: "card-ver", html: icon("file") })),
        h("span", { class: "card-meta" }, "a .fwsc package, emulated"),
        h("input", { type: "file", accept: ".fwsc", hidden: true, onchange: (ev) => { const f = ev.target.files[0]; ev.target.value = ""; if (f) { a.openLocalFile(f); this.close(); } } })));
      if (!list.length) grid.prepend(h("p", { class: "empty" }, "No firmware matches"));
    };
    const search = h("div", { class: "search" }, h("span", { html: icon("search") }),
      h("input", { type: "search", placeholder: `Search ${cat.firmwares.length} firmwares`, value: this.search, oninput: (ev) => { this.search = ev.target.value; fill(); } }));
    const engines = h("div", { class: "seg engine-filter", role: "radiogroup", "aria-label": "Engine" }, ENGINE_FILTERS.map(([k, label]) =>
      h("button", { class: (st.browseEngine || "all") === k ? "on" : "", role: "radio", "aria-checked": String((st.browseEngine || "all") === k), "data-engine": k,
        title: k === "native" ? "Firmwares with a native core (FM1VST's), started native" : k === "emu" ? "Every firmware the emulator runs, at the Hub's latest, started emulated" : "Every firmware, on its default engine",
        onclick: (ev) => { a.set("browseEngine", k); } }, label)));
    const chips = h("div", { class: "chips" }, cats.map((c, i) => h("button", { class: "chip" + (i === this.cat ? " on" : ""), onclick: (ev) => {
      this.cat = i; for (const b of chips.children) b.classList.toggle("on", b === ev.currentTarget); fill();
    } }, c)));
    fill();
    return [search, engines, chips, grid,
      h("p", { class: "note" }, "From ", link(cat.hub, "the FM-1 Firmware Hub"), ` (${cat.live ? "live, " : "as of "}${cat.hubUpdated || "snapshot"}). Firmwares with a dot run natively, compiled from FM1VST's cores (where the Hub has a newer release, it runs emulated unless you pick native); every firmware also runs emulated on `,
        link(cat.credits.fm1emu.source, "fm1-emu"), ", its package fetched from where the Hub points (RemoteFM1's from this site).")];
  }

  // =============================================================== about ===
  aboutPage() {
    const a = this.app, st = a.state, e = st.entry, cat = a.catalog, info = st.info || {};
    const out = [];
    const kv = (k, v) => h("div", { class: "kv" }, h("span", { class: "k" }, k), h("span", { class: "v" }, v));
    const shown = st.engine === "mock" ? st.wantEngine : st.engine;
    if (e) {
      out.push(heading(e.name));
      out.push(h("p", { class: "about-what" }, e.what || ""));
      out.push(kv("Version", a.versionOf(e, shown) + (shown === "native" && e.native.version !== e.version ? ` (the Hub's: ${e.version})` : "")));
      out.push(kv("Author", e.authorFull || e.author));
      out.push(kv("Licence", e.license || info.license || "see its source"));
      const [srcLink, glue] = this.sourceLines(false);
      out.push(kv("Source", srcLink));
      if (glue) out.push(kv("Glue", h("span", {}, "built with FM1VST's glue: ", glue)));
      if (e.release) out.push(kv("Release", link(e.release, "release page")));
      if (e.play) out.push(kv("Its own page", link(e.play, e.play.replace(/^https?:\/\//, ""))));
      if (info.credits && st.engine !== "mock") out.push(h("pre", { class: "credits" }, String(info.credits)));
      out.push(heading("Engine"));
      out.push(h("p", { class: "note" }, a.engineNote()));
      if (shown === "emu")
        out.push(h("p", { class: "note" }, "Emulated by ", link(cat.credits.fm1emu.source, "fm1-emu"), ` by ${cat.credits.fm1emu.author} (${cat.credits.fm1emu.license}): the FM-1's chip, so the firmware's own package runs unchanged. ${e.emu && e.emu.from === "site" ? "The package is this site's own, the one its installer writes." : "The package comes from where the FM-1 Firmware Hub points; this site hosts no firmware but its own."}`));
      if (shown === "native")
        out.push(h("p", { class: "note" }, "Built from ", link(cat.credits.fm1vst.source, "FM1VST"), "'s core of this firmware (GPL-3.0), compiled to WebAssembly from the firmware's own source."));
    }
    out.push(heading("Credits and notices"));
    out.push(h("ul", { class: "credit-list" },
      h("li", {}, "The panel and the sheet: ", link(cat.credits.fm1vst.source, "FM1VST"), "'s design (GPL-3.0), after ChoralRoot FM-1's emulator."),
      h("li", {}, "Firmware catalog: ", link(cat.hub, "the FM-1 Firmware Hub"), ". Every firmware belongs to its author; see each one's licence."),
      h("li", {}, link(cat.credits.fm1emu.source, "fm1-emu"), ` by ${cat.credits.fm1emu.author}, MIT: the emulated engine.`),
      h("li", {}, "Inter Tight by the Inter Project Authors, SIL Open Font License 1.1 (", h("a", { href: "fonts/OFL.txt" }, "OFL.txt"), ")."),
      h("li", {}, "This site: ", link("https://github.com/Quixotic7/RemoteFM1", "RemoteFM1"), ", GPL-3.0-only."),
      h("li", {}, "\"M-VAVE\" and \"FM-1\" belong to their owners. Not made or endorsed by M-VAVE.")));
    return out;
  }
}
export { hex };
