// SPDX-License-Identifier: GPL-3.0-only
// SEND A DX7 CARTRIDGE: a DX7 32-voice bulk dump (.syx) checked here and sent to the user's FM-1 over Web MIDI, where
// RemoteFM1's standalone instrument keeps it as a user bank, USER1..USER3 (firmware/sa/sa_ubank.c; docs/CART-UPLOAD.md).
// The unit keeps a bank only as an instrument (outside a session): the page's own link is ended first. The firmware
// sends no reply: the unit shows the new bank (USERn, its first voice selected), which is the confirmation.
// parseCarts / sendCart have no DOM: web/site/test.mjs drives them with a fake MIDI port.
import { h } from "./sheet.js";

export const CART_LEN = 4104;                      // F0 43 0n 09 20 00, 4096 bytes of voices, the checksum, F7
const FM1_OUT = /FM-1|RemoteFM1/i;

const nameOf = (v, k) => String.fromCharCode(...v.subarray(128 * k + 118, 128 * k + 128).map((c) => (c >= 32 && c < 127 ? c : 32))).trimEnd();
const sumOf = (v) => (128 - (v.reduce((a, b) => a + b, 0) & 0x7F)) & 0x7F;

// every SysEx message in the file: [{ ok, why, names[32], bytes (4104, the channel 0, the checksum as sent), fixed }]
// (a librarian's bundle may hold several banks; other SysEx in the file is listed as not a bank)
export function parseCarts(file) {
  const b = file instanceof Uint8Array ? file : new Uint8Array(file);
  const out = [];
  for (let i = 0; i < b.length; i++) {
    if (b[i] !== 0xF0) continue;
    let j = i + 1;
    while (j < b.length && b[j] < 0x80) j++;
    const m = b.subarray(i, j + 1), end = j < b.length && b[j] === 0xF7;
    i = j - (j < b.length && b[j] === 0xF0 ? 1 : 0);
    const isBank = m.length > 5 && m[1] === 0x43 && (m[2] & 0xF0) === 0 && m[3] === 0x09;
    if (!isBank) { out.push({ ok: false, why: m[1] === 0x43 ? "a Yamaha message, not a 32-voice bank" : "not a DX7 bank" }); continue; }
    if (!end || m.length !== CART_LEN || m[4] !== 0x20 || m[5] !== 0x00) { out.push({ ok: false, why: `a DX7 dump of ${m.length} bytes, not a 32-voice bank (${CART_LEN})` }); continue; }
    const v = m.subarray(6, 4102), bytes = new Uint8Array(m);
    bytes[2] = 0x00;
    const sum = sumOf(v), fixed = m[4102] !== sum;
    bytes[4102] = sum;                             // (many files online carry a wrong one; the unit refuses those)
    out.push({ ok: true, names: Array.from({ length: 32 }, (_, k) => nameOf(v, k)), bytes, fixed });
  }
  if (!out.length) out.push({ ok: false, why: b.length ? "no SysEx in this file" : "an empty file" });
  return out;
}

// the FM-1 outputs of a MIDIAccess: the USB port first, a Bluetooth one ("FM-1 1F2D") after, never the updater's
export function fm1Outputs(access) {
  const outs = [...access.outputs.values()].filter((o) => FM1_OUT.test(o.name || "") && !/update/i.test(o.name || ""));
  return outs.sort((a, b) => (/FM-1 [0-9A-F]{4}\b/.test(a.name) ? 1 : 0) - (/FM-1 [0-9A-F]{4}\b/.test(b.name) ? 1 : 0));
}

// one bank to one output (the whole message in one send: Web MIDI splits it into packets)
export function sendCart(out, bytes) {
  if (!bytes || bytes.length !== CART_LEN || bytes[0] !== 0xF0 || bytes[CART_LEN - 1] !== 0xF7) throw new Error("not a 32-voice bank");
  out.send(bytes);
}

// What the unit says of its user banks. RemoteFM1 0.3.0 answers nothing (3 banks); 0.3.1 adds a query of the capacity
// and the stored banks' names (docs/PROTOCOL.md section 8 on that branch): plug it in here, returning
// { max, names: [...] } or null when the unit does not answer. The UI then shows the real numbers.
export const BANKS_DEFAULT = "up to 3 on RemoteFM1 0.3.0, 16 from 0.3.1";
export async function queryUserBanks(/* access, out */) { return null; }
export const banksText = (info) => info && info.max
  ? `${info.names ? info.names.length : "?"} of ${info.max} USER banks stored; one more replaces the oldest.`
  : `USER banks: ${BANKS_DEFAULT}; one more replaces the oldest.`;

// ---- the sheet's part, under USE MY FM-1
export function mountCart(slot, link, nav = globalThis.navigator) {
  const input = h("input", { type: "file", accept: ".syx,application/octet-stream", hidden: true });
  const pick = h("button", { class: "pill" }, "Choose a .syx file");
  const drop = h("div", { class: "cart-drop" }, pick, h("span", { class: "row-sub" }, " or drop it here"));
  const list = h("div", { class: "cart-list" });
  const line = h("p", { class: "remote-status small muted" });
  const say = (t, tone = "muted") => { line.textContent = t; line.className = "remote-status small " + tone; };
  const notes = (info) => `${banksText(info)} Installing ChoralRoot or the stock firmware erases them. Sending ends the player's link (the FM-1 keeps banks only as an instrument); a file with several banks: send each.`;
  const limits = h("p", { class: "note" }, notes(null));
  let busy = false;

  async function send(bank) {
    if (busy) return;
    busy = true;
    try {
      if (!nav || !nav.requestMIDIAccess) throw new Error("This browser has no Web MIDI: use Chrome or Edge on a computer.");
      if (link && link.view().tone !== "off" && link.view().tone !== "bad") {
        say("Ending the player's link first: the FM-1 keeps banks only as an instrument...");
        await link.disconnect();
        await new Promise((r) => setTimeout(r, 400));
      }
      const access = await nav.requestMIDIAccess({ sysex: true });
      const outs = fm1Outputs(access);
      if (!outs.length) throw new Error("No FM-1 MIDI port: plug it in (USB) with RemoteFM1 0.3 running.");
      sendCart(outs[0], bank.bytes);
      const info = await queryUserBanks(access, outs[0]).catch(() => null);
      limits.textContent = notes(info);
      say(`Sent to ${outs[0].name}. The FM-1 shows the new USER bank with its first voice selected (${bank.names[0] || "voice 1"}). Nothing on the screen? It needs RemoteFM1 0.3 or newer.`, "ok");
    } catch (e) {
      say(/denied|NotAllowed|Security/i.test(String(e && (e.name || e.message))) ? "The browser blocked MIDI with SysEx for this site: allow it (the icon left of the address bar), then try again."
        : String(e && e.message || e), "bad");
    } finally { busy = false; }
  }

  function show(name, buf) {
    const banks = parseCarts(new Uint8Array(buf)), good = banks.filter((b) => b.ok);
    list.replaceChildren(...banks.map((b, i) => b.ok
      ? h("div", { class: "cart-bank" },
          h("div", { class: "row-sub" }, `${name}${banks.length > 1 ? ` · bank ${i + 1}` : ""}${b.fixed ? " · checksum corrected" : ""}`),
          h("ol", { class: "cart-names" }, b.names.map((n) => h("li", {}, n || "-"))),
          h("button", { class: "pill primary", onclick: () => send(b) }, "Send to my FM-1"))
      : h("p", { class: "row-sub" }, `${name}${banks.length > 1 ? ` · message ${i + 1}` : ""}: ${b.why}.`)));
    say(good.length ? `${good.length} bank${good.length > 1 ? "s" : ""} of 32 voices found.` : "No DX7 32-voice bank in this file.", good.length ? "muted" : "bad");
  }
  const load = (f) => { if (!f) return; if (f.size > 1 << 20) { say("That file is too big for a DX7 cartridge.", "bad"); return; } f.arrayBuffer().then((b) => show(f.name, b)); };
  pick.addEventListener("click", () => input.click());
  input.addEventListener("change", () => { load(input.files[0]); input.value = ""; });
  drop.addEventListener("dragover", (e) => { e.preventDefault(); drop.classList.add("over"); });
  drop.addEventListener("dragleave", () => drop.classList.remove("over"));
  drop.addEventListener("drop", (e) => { e.preventDefault(); drop.classList.remove("over"); load(e.dataTransfer.files[0]); });

  slot.append(h("div", { class: "cart" },
    h("p", { class: "row-sub", style: { margin: "14px 0 6px" } }, "Send a DX7 cartridge (a 32-voice .syx) to your FM-1 running RemoteFM1 0.3: it becomes a USER bank, after the ROM cartridges (GLO + SELECT)."),
    drop, input, list, line,
    limits));
}
