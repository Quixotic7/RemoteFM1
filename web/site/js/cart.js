// SPDX-License-Identifier: GPL-3.0-only
// SEND A DX7 CARTRIDGE: a DX7 32-voice bulk dump (.syx) checked here and sent to the user's FM-1 over Web MIDI, where
// RemoteFM1's standalone instrument keeps it as a user bank (firmware/sa/sa_ubank.c; docs/CART-UPLOAD.md): USER1..USER3
// on 0.3.0, which sends no reply (the unit's screen is the confirmation); USER1..USER16 from 0.3.1, which lists them,
// answers each dump (STORED), takes one into a chosen slot (Replace) and removes one (Delete).
// The unit keeps a bank only as an instrument (outside a session): the page's own link is ended first.
// Everything above mountCart has no DOM: web/site/test.mjs drives it with a fake MIDI port.
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

// What the unit says of its user banks, and the ops on them (docs/CART-UPLOAD.md, PROTOCOL.md section 8). RemoteFM1
// 0.3.0 answers nothing (3 banks, the oldest replaced): every call here returns null then and the page says so.
export const BANKS_DEFAULT = "up to 3 on RemoteFM1 0.3.0, 16 from 0.3.1";
// SA_INFO (0x0B) -> SA_INFO_R (0x4B), the body 7-in-8 packed as every RemoteFM1 SysEx (PROTOCOL.md 2, 8):
//   op 0 BANKS  "00"       -> "00 max count busy next loops", then per stored bank "slot8 arrival32 crc32 name[10]"
//   op 2 TARGET "02 slot"  -> "02 slot status": the next dump (within 10 s) goes to slot, replacing it
//   op 3 DELETE "03 slot"  -> "03 slot status" once the directory is written
//   op 4 STORED (no request) "04 slot status": after every DX7 dump, once it is written (or refused)
const SX = [0xF0, 0x7D, 0x52, 0x46];
export const pack7 = (b) => { const o = []; for (let i = 0; i < b.length; i += 7) { const g = b.slice(i, i + 7); o.push(g.reduce((m, x, k) => m | ((x >> 7) & 1) << k, 0), ...g.map((x) => x & 0x7F)); } return o; };
export const unpack7 = (b) => { const o = []; for (let i = 0; i < b.length; i += 8) { const g = b.slice(i, i + 8); for (let k = 1; k < g.length; k++) o.push(g[k] | (((g[0] >> (k - 1)) & 1) << 7)); } return o; };
export const infoMsg = (body) => [...SX, 0x0B, ...pack7(body), 0xF7];
export const STATUS = ["stored", "refused: a bad checksum or size", "a bad request", "busy: try again in a second",
  "refused: the FM-1 is in a host session (end it first)", "the flash write failed"];
const le32 = (b, p) => (b[p] | b[p + 1] << 8 | b[p + 2] << 16 | b[p + 3] << 24) >>> 0;
export function parseBanksReply(body) {
  if (body.length < 6 || body[0] !== 0) return null;
  const [, max, count, busy, next, loops] = body, banks = [];
  for (let i = 0, p = 6; i < count && p + 19 <= body.length; i++, p += 19)
    banks.push({ slot: body[p], arrival: le32(body, p + 1), crc: le32(body, p + 5), name: String.fromCharCode(...body.slice(p + 9, p + 19)).replace(/[\0 ]+$/, "") });
  return { max, count, busy: !!busy, next, loops, banks, names: banks.map((b) => b.name) };
}
const inputOf = (access, out) => access && out && [...access.inputs.values()].find((i) => i.name === out.name);
// send `msgs` in turn, then wait up to `ms` for a SA_INFO_R body that `want` accepts: that body, or null
async function exchange(access, out, msgs, want, ms) {
  const inp = inputOf(access, out);
  if (!inp) return null;
  let done;
  const got = new Promise((r) => { done = r; }), on = (e) => {
    const d = [...e.data];
    if (d.length > 6 && SX.every((x, i) => d[i] === x) && d[4] === 0x4B) { const b = unpack7(d.slice(5, -1)); if (want(b)) done(b); }
  };
  inp.addEventListener("midimessage", on);
  try {
    for (const m of msgs) out.send(m);
    return await Promise.race([got, new Promise((r) => setTimeout(() => r(null), ms))]);
  } finally { inp.removeEventListener("midimessage", on); }
}
// BANKS, polled while busy (a bank still being written); null when the unit doesn't answer (0.3.0)
export async function queryUserBanks(access, out, { tries = 15, wait = 200 } = {}) {
  let last = null;
  for (let t = 0; t < tries; t++) {
    const b = await exchange(access, out, [infoMsg([0])], (x) => x[0] === 0, wait);
    if (!b) return last;
    last = parseBanksReply(b);
    if (last && !last.busy) return last;
    await new Promise((r) => setTimeout(r, wait));
  }
  return last;
}
// a bank sent, into `slot` (0..15, replacing it: TARGET first) or where the unit puts it (slot null). The unit's
// STORED: { slot, status } (status 0 stored; STATUS says the rest); null: no reply (0.3.0; a bank without a slot is
// sent anyway, the screen confirms). Throws when a slot was asked of a unit that cannot take one.
export async function sendCartTo(access, out, bytes, slot = null, { ms = 3000 } = {}) {
  sendCart({ send() {} }, bytes);                  // (checked before anything goes out)
  if (slot != null) {
    const r = await exchange(access, out, [infoMsg([2, slot])], (x) => x[0] === 2 && x[1] === slot, 1000);
    if (!r) throw new Error("This FM-1 cannot replace a chosen bank: it needs RemoteFM1 0.3.1 or newer.");
    if (r[2]) return { slot, status: r[2] };
  }
  const r = await exchange(access, out, [bytes], (x) => x[0] === 4, ms);
  return r ? { slot: r[1], status: r[2] } : null;
}
// a bank removed: { slot, status } or null (no reply: 0.3.0 has no DELETE)
export async function deleteBank(access, out, slot, { ms = 3000 } = {}) {
  const r = await exchange(access, out, [infoMsg([3, slot])], (x) => x[0] === 3 && x[1] === slot, ms);
  return r ? { slot: r[1], status: r[2] } : null;
}
export const banksText = (info) => info && info.max
  ? `${info.names ? info.names.length : "?"} of ${info.max} USER banks stored; a new one goes to the first free slot, else replaces the oldest.`
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
  const stored = h("div", { class: "cart-list cart-stored" });
  let busy = false, chosen = null;

  async function port() {                          // the FM-1's output (USB first), the player's link ended
    if (!nav || !nav.requestMIDIAccess) throw new Error("This browser has no Web MIDI: use Chrome or Edge on a computer.");
    if (link && link.view().tone !== "off" && link.view().tone !== "bad") {
      say("Ending the player's link first: the FM-1 keeps banks only as an instrument...");
      await link.disconnect();
      await new Promise((r) => setTimeout(r, 400));
    }
    const access = await nav.requestMIDIAccess({ sysex: true });
    const outs = fm1Outputs(access);
    if (!outs.length) throw new Error("No FM-1 MIDI port: plug it in (USB) with RemoteFM1 0.3 running.");
    return { access, out: outs[0] };
  }
  async function run(job) {
    if (busy) return;
    busy = true;
    try { await job(await port()); } catch (e) {
      say(/denied|NotAllowed|Security/i.test(String(e && (e.name || e.message))) ? "The browser blocked MIDI with SysEx for this site: allow it (the icon left of the address bar), then try again."
        : String(e && e.message || e), "bad");
    } finally { busy = false; }
  }
  // the stored banks (BANKS), each with Replace (the chosen file bank into its slot) and Delete
  async function refresh({ access, out }) {
    const info = await queryUserBanks(access, out).catch(() => null);
    limits.textContent = notes(info);
    if (!info) { stored.replaceChildren(h("p", { class: "row-sub" }, "This FM-1 does not list its banks (RemoteFM1 0.3.0): Replace and Delete need 0.3.1.")); return info; }
    stored.replaceChildren(h("div", { class: "row-sub" }, `On your FM-1: ${info.banks.length} of ${info.max} USER banks`),
      ...info.banks.map((b) => h("div", { class: "cart-slot" },
        h("span", {}, `USER${b.slot + 1} · ${b.name || "-"}`),
        h("button", { class: "pill", disabled: !chosen, title: chosen ? `Send ${chosen.label} into USER${b.slot + 1}` : "Choose a .syx file first", onclick: () => replace(b.slot) }, "Replace"),
        h("button", { class: "pill", onclick: () => remove(b.slot, b.name) }, "Delete"))));
    return info;
  }
  const result = (r, what) => !r ? null : r.status === 0 ? `${what}: done (USER${r.slot + 1}).` : `${what}: ${STATUS[r.status] || `status ${r.status}`}.`;
  const send = (bank, slot = null) => run(async (p) => {
    say(slot == null ? "Sending..." : `Sending into USER${slot + 1}...`);
    const r = await sendCartTo(p.access, p.out, bank.bytes, slot);
    if (r) say(result(r, `Sent to ${p.out.name}`) + (r.status ? "" : ` The FM-1 plays its first voice (${bank.names[0] || "voice 1"}).`), r.status ? "bad" : "ok");
    else say(`Sent to ${p.out.name}. The FM-1 shows the new USER bank with its first voice selected (${bank.names[0] || "voice 1"}). Nothing on the screen? It needs RemoteFM1 0.3 or newer.`, "ok");
    await refresh(p);
  });
  const replace = (slot) => chosen && send(chosen, slot);
  const remove = (slot, name) => {
    if (globalThis.confirm && !globalThis.confirm(`Delete USER${slot + 1} (${name || "no name"}) from the FM-1?`)) return;
    run(async (p) => {
      const r = await deleteBank(p.access, p.out, slot);
      say(result(r, `USER${slot + 1} deleted`) || "No reply: this FM-1 cannot delete banks (it needs RemoteFM1 0.3.1).", r && !r.status ? "ok" : "bad");
      await refresh(p);
    });
  };
  const listBtn = h("button", { class: "pill" }, "Show the banks on my FM-1");
  listBtn.addEventListener("click", () => run(async (p) => { say("Asking the FM-1..."); const i = await refresh(p); say(i ? "" : "No answer to the bank list.", i ? "muted" : "bad"); }));

  function show(name, buf) {
    const banks = parseCarts(new Uint8Array(buf)), good = banks.filter((b) => b.ok);
    banks.forEach((b, i) => { b.label = `${name}${banks.length > 1 ? ` · bank ${i + 1}` : ""}`; });
    chosen = good[0] || null;
    const pickBtns = [];
    list.replaceChildren(...banks.map((b, i) => b.ok
      ? h("div", { class: "cart-bank" },
          h("div", { class: "row-sub" }, `${b.label}${b.fixed ? " · checksum corrected" : ""}`),
          h("ol", { class: "cart-names" }, b.names.map((n) => h("li", {}, n || "-"))),
          h("button", { class: "pill primary", onclick: () => send(b) }, "Send to my FM-1"),
          good.length > 1 ? (pickBtns[i] = h("button", { class: "pill", disabled: b === chosen, onclick: () => {
            chosen = b; pickBtns.forEach((x, j) => x && (x.disabled = banks[j] === chosen));
            stored.querySelectorAll("button").forEach((x) => { if (x.textContent === "Replace") x.disabled = false; });
          } }, "Use for Replace")) : null)
      : h("p", { class: "row-sub" }, `${name}${banks.length > 1 ? ` · message ${i + 1}` : ""}: ${b.why}.`)));
    stored.querySelectorAll("button").forEach((x) => { if (x.textContent === "Replace") x.disabled = !chosen; });
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
    drop, input, list, line, listBtn, stored,
    limits));
}
