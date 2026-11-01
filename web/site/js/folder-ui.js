// SPDX-License-Identifier: GPL-3.0-only
// The RemoteFM1 folder in the page (js/folder.js): its section in the sheet while a Fantasy CFW runs (the folder's
// name and state; Pick / Allow / Change in Chrome and Edge; Add files, a drop zone and Export data with this site's
// storage), and a prompt over the page when a running Fantasy CFW has no folder yet.
//   const ui = new FolderUI({ folder, cart: () => ({ id, folders, fs }) | null, toast, render });
//   ui.update()      after a firmware, its info or the folder changed
//   ui.section()     the sheet's rows ([] when no Fantasy CFW runs)
import { h } from "./sheet.js";
import { filesFromDrop } from "./folder.js";

// the manifest's folders, short: "reads audio, code · writes data"
export function accessLine(folders = {}) {
  const r = ["audio", "code", "data"].filter((k) => folders[k]), w = folders.data === "rw" ? ["data"] : [];
  return [r.length ? `reads ${r.join(", ")}` : "", w.length ? `writes ${w.join(", ")}` : ""].filter(Boolean).join(" · ") || "no files";
}

export class FolderUI {
  constructor({ folder, cart, toast, render }) {
    this.folder = folder; this.cart = cart; this.toast = toast; this.render = render;
    this.dismissed = new Set();
    this.text = h("span", {});
    this.btn = h("button", { class: "pill primary", onclick: () => this.grant() });
    this.box = h("div", { class: "offer folder-ask", role: "alertdialog", "aria-label": "RemoteFM1 folder", hidden: true }, this.text,
      h("span", { class: "offer-btns" }, this.btn, h("button", { class: "pill ghost", onclick: () => { const c = this.cart(); if (c) this.dismissed.add(c.id); this.update(); } }, "Not now")));
    document.body.append(this.box);
  }
  // the prompt: a running Fantasy CFW that reads files, and no folder it may use yet
  update() {
    const c = this.cart(), f = this.folder;
    const show = !!c && c.fs && (f.state === "none" || f.state === "ask") && !this.dismissed.has(c.id);
    this.text.textContent = f.state === "ask" ? `Allow your RemoteFM1 folder (${f.name})` : "Pick your RemoteFM1 folder";
    this.btn.textContent = f.state === "ask" ? "Allow" : "Pick...";
    if (show && this.box.hidden) { this.box.hidden = false; requestAnimationFrame(() => this.box.classList.add("show")); }
    else if (!show && !this.box.hidden) { this.box.classList.remove("show"); this.box.hidden = true; }
  }
  // in a click: the folder picked, or allowed again
  async grant(change = false) {
    try { if (this.folder.state === "ask" && !change) await this.folder.allow(); else await this.folder.pick(); }
    catch (e) { this.toast(`The folder could not be opened (${e.message || e}).`, "bad"); }
    this.update(); this.render();
  }
  async add(items) {
    const c = this.cart();
    try {
      const n = await this.folder.addFiles(items, c && c.id);
      this.toast(n ? `Added ${n} file${n === 1 ? "" : "s"} to the RemoteFM1 folder.` : "No files to add.", n ? "ok" : "");
    } catch (e) { this.toast(`The files could not be added (${e.message || e}).`, "bad"); }
    this.render();
  }
  async exportData() {
    try { const n = await this.folder.exportData(); if (!n) this.toast("Nothing in data/ yet."); }
    catch (e) { this.toast(`Export failed (${e.message || e}).`, "bad"); }
  }

  section() {
    const c = this.cart(), f = this.folder;
    if (!c) return [];
    const state = f.state === "ready" ? "ready" : f.state === "ask" ? "needs your OK" : f.state === "unsupported" ? "not in this browser" : "not picked";
    const out = [h("h3", { class: "heading" }, "RemoteFM1 folder", h("span", { class: "heading-val" }, state))];
    const opfs = f.mode === "opfs";
    const pills = [];
    if (f.mode === "picker") {
      if (f.state === "ask") pills.push(h("button", { class: "pill primary", onclick: () => this.grant() }, "Allow"));
      pills.push(h("button", { class: "pill" + (f.state === "none" ? " primary" : ""), onclick: () => this.grant(true) }, f.state === "none" ? "Pick..." : "Change..."));
    }
    if (f.state === "ready") {
      const input = (dir) => h("input", { type: "file", multiple: true, hidden: true, ...(dir ? { webkitdirectory: true } : {}),
        onchange: (ev) => { const items = Array.from(ev.target.files || [], (file) => ({ file, path: file.webkitRelativePath || file.name })); ev.target.value = ""; if (items.length) this.add(items); } });
      if (opfs) {
        pills.push(h("label", { class: "pill" }, "Add folder...", input(true)));
        pills.push(h("label", { class: "pill" }, "Add files...", input(false)));
      }
      pills.push(h("button", { class: "pill ghost", onclick: () => this.exportData() }, "Export data"));
    }
    const zone = h("div", { class: "folder-box" + (f.state === "ready" ? "" : " off") },
      h("div", { class: "folder-top" }, h("b", {}, f.name || "No folder"), opfs ? h("span", { class: "muted small" }, "in this browser") : null),
      h("div", { class: "row-sub" }, accessLine(c.folders)),
      h("div", { class: "pills folder-pills" }, pills));
    if (f.state === "ready") {
      zone.addEventListener("dragover", (ev) => { ev.preventDefault(); ev.stopPropagation(); zone.classList.add("over"); });
      zone.addEventListener("dragleave", () => zone.classList.remove("over"));
      zone.addEventListener("drop", async (ev) => {
        ev.preventDefault(); ev.stopPropagation(); zone.classList.remove("over");
        const items = await filesFromDrop(ev.dataTransfer);
        if (items.length) this.add(items);
      });
    }
    out.push(zone);
    return out;
  }
}
