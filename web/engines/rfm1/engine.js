// SPDX-License-Identifier: GPL-3.0-only
// The .rfm1 engine (web/engine/API.md): an FM-1 firmware's host build (FM1VST's fm1core ABI 6) compiled to
// WebAssembly as an .rfm1 file (RemoteFM1IOS docs/RFM1-FORMAT.md, version 1), the file the visitor opens. This file
// and ../common/shared.js are the whole runtime: they load inside an AudioWorkletGlobalScope (no fetch, no DOM, no
// TextDecoder, no dynamic import) and in Node.
//
// THE FILE is checked before anything runs (inspect(), the format's section 7 step 1): the binary walked section by
// section with every bound checked; the manifest (the rfm1 custom section, JSON: section 4's required fields);
// rfm1.credits and rfm1.license.<n>; the imports (section 3's WASI list only, each with its signature); the exports
// (section 2's, each a function of its i32 signature, and the memory); one memory, its own, exported, fixed (initial
// = maximum), at most 32 MiB; active data segments only.
// THE INSTANCE (steps 2-3): _initialize, rfm1_version() == 1, rfm1_get(6) != 0, every rfm1_layout value against this
// engine's (fm1panel_t and fm1core_t as wasm32 lays them out), the descriptor's data read out, its id / name / version
// against the manifest's. Every call goes through the rfm1_* exports; the host passes strings, the audio block and
// MIDI words in one scratch block made with rfm1_alloc. The panel and the flash are read and written in linear memory,
// which never moves (fixed size).
// THE CLOCK is FM1VST's engine/device.cpp run_ms, as the native engine's wasm_glue.c runs it: tick(ms) once per
// device millisecond, frame() at ms 0 and every 15 ms or idle() otherwise, then audio() for each 128-frame block due
// at 44100 Hz. render() runs the device until the resampler (../common/shared.js) has the blocks it needs.
// A TRAP ends the instance: halted() is then 0x100 | code after proc_exit(code), 0x200 after any other trap, render()
// gives silence and every other call does nothing (the flash stays readable: the page boots it again).
// A FANTASY CFW (FM1VST docs/VIRTUAL-CARTS.md: a host-only .rfm1, the manifest's "virtual": true) may have up to
// 512 MiB of memory (a file under 16 MiB) and, with "fs1" in its "needs", import the file API of section 4 from
// rfm1_host_1: the RemoteFM1 folder (audio/, code/<id>/, data/<id>/), asynchronous. The engine checks every request
// (the path, the manifest's "folders", the limits) and hands it to options.fsSend(msg, transfer) (host.js posts it to
// the page, js/folder.js answers; a Node test serves a directory: ./fs-dir.mjs); the answer comes back through
// fsDone(msg) and is copied into the module's memory at the start of the next render(), never during an export call.

import { FS, BLOCK, RS_HALF, Resampler, MidiParser, packetBytes, now, LBL_N, labelSet, cleanLabel } from "../common/shared.js";

export const ENGINE = { name: "rfm1", version: "0.1" };

const ABI = 6;
const LCD_PIXELS = 240 * 240;
const NKEY = 27;
const NENC = 7;
const NB = 14;
const PAGE = 65536;
const MAX_MEMORY = 32 * 1048576;
const MAX_FILE = 64 * 1048576;
const MAX_VIRTUAL_MEMORY = 512 * 1048576;      // a Fantasy CFW (VIRTUAL-CARTS.md section 2)
const MAX_VIRTUAL_FILE = 16 * 1048576;          // (the file under it)
const STAGED = "fm1core:staged";
const HALT_TRAP = 0x200;

// fm1panel_t as wasm32 lays it out (core-api/fm1core.h): rfm1_layout 0..16
const P = { size: 115384, keys: 0, buttons: 4, keys_tap: 8, buttons_tap: 12, enc: 16, master: 44, key_vel: 48, key_lean: 75,
            led: 102, led_play_green: 146, lcd: 148, lcd_writes: 115348, btn_id: 115352, enc_id: 115366, enc_dir: 115373,
            ready: 115380 };
// fm1core_t (ABI 6), every pointer 32 bits: rfm1_layout 17..33, and the entry points between them (read for which
// optional ones the firmware has: a host reads data out of the descriptor, never calls its function pointers)
const C = { size: 192, abi_version: 0, id: 4, name: 8, version: 12, source_url: 16, family: 20, license: 24, gated: 28,
            flash_size: 32, button_names: 36, params: 92, nparams: 96, panel: 100, keymap: 104, options: 108,
            boot_options: 112, midi_in: 136, midi_out_take: 140, shutdown: 156, halted: 160, flash_stage: 164, flash: 168,
            flash_dirty: 172, flash_sync: 176, credits: 188 };
const LAYOUT = [P.size, P.keys, P.buttons, P.keys_tap, P.buttons_tap, P.enc, P.master, P.key_vel, P.key_lean, P.led,
    P.led_play_green, P.lcd, P.lcd_writes, P.btn_id, P.enc_id, P.enc_dir, P.ready,
    C.size, C.abi_version, C.id, C.name, C.version, C.source_url, C.family, C.license, C.gated, C.flash_size,
    C.button_names, C.params, C.nparams, C.panel, C.keymap, C.options, C.credits];

// the scratch block: a string, MIDI / stats words, one audio block
const STR_CAP = 1024, WORDS = 16, SCRATCH = STR_CAP + WORDS + BLOCK * 4 + 16;

// section 3: the imports a module may have, with their signatures ([params, results]; 0x7f i32, 0x7e i64)
const WASI = "wasi_snapshot_preview1";
const I32 = 0x7f, I64 = 0x7e;
const IMPORTS = {
    fd_write: [[I32, I32, I32, I32], [I32]],
    fd_read: [[I32, I32, I32, I32], [I32]],
    fd_close: [[I32], [I32]],
    fd_seek: [[I32, I64, I32, I32], [I32]],
    clock_time_get: [[I32, I64, I32], [I32]],
    proc_exit: [[I32], []],
};
// VIRTUAL-CARTS.md section 4: the file API a Fantasy CFW with "fs1" in its needs may import
const HOST = "rfm1_host_1";
const HOST_IMPORTS = {
    fs_request: [[I32, I32, I32, I64, I32, I32], [I32]],
    fs_poll: [[I32], [I32]],
    fs_cancel: [[I32], []],
    fs_changed: [[], [I32]],
};
const KNOWN_NEEDS = ["fs1"];
export const FS_OP = { STAT: 1, LIST: 2, READ: 3, WRITE: 4, DELETE: 5, MKDIR: 6 };
export const FS_ERR = { PENDING: -1, ENOENT: -2, EACCES: -3, EIO: -4, ENOSPC: -5, EBUSY: -6, ECANCELED: -7, ENOFOLDER: -8 };
const FS_MAX_INFLIGHT = 32, FS_MAX_LEN = 1048576, FS_MAX_PATH = 1024;
const FS_ROOTS = ["audio", "code", "data"];
// section 2: the exports, [name, i32 arguments, i32 results]; _initialize is optional
const EXPORTS = [["_initialize", 0, 0], ["rfm1_version", 0, 1], ["rfm1_get", 1, 1], ["rfm1_layout", 1, 1],
    ["rfm1_alloc", 1, 1], ["rfm1_free", 1, 0], ["rfm1_options", 4, 0], ["rfm1_boot_options", 3, 1], ["rfm1_init", 1, 0],
    ["rfm1_tick", 1, 0], ["rfm1_frame", 0, 0], ["rfm1_idle", 0, 0], ["rfm1_audio", 2, 0], ["rfm1_midi_in", 1, 1],
    ["rfm1_midi_out_take", 1, 1], ["rfm1_halted", 0, 1], ["rfm1_shutdown", 0, 0], ["rfm1_flash_stage", 2, 1],
    ["rfm1_flash", 0, 1], ["rfm1_flash_dirty", 0, 1], ["rfm1_flash_sync", 0, 1], ["rfm1_stats", 2, 0],
    ["rfm1_ui_info", 2, 0]];
// optional: live panel labels (FM1VST docs/DYNAMIC-LABELS.md); a module without both (or with another signature) has none
const LABEL_EXPORTS = [["rfm1_labels_epoch", 0, 1], ["rfm1_label", 1, 1]];
const REQUIRED_FIELDS = ["id", "name", "version", "license", "source_url"];

const refuse = (why) => { throw new Error(`not a usable .rfm1 file: ${why}`); };

// UTF-8 bytes -> a string (no TextDecoder here); a bad sequence becomes U+FFFD
function utf8Range(b, at, end) {
    let s = "";
    for (let i = at; i < end;) {
        const c = b[i++];
        let n = 0, u = c;
        if (c >= 0xf0 && c < 0xf8) { n = 3; u = c & 7; } else if (c >= 0xe0) { n = 2; u = c & 15; } else if (c >= 0xc0) { n = 1; u = c & 31; }
        else if (c >= 0x80) { s += "�"; continue; }
        if (c >= 0xf8) { s += "�"; continue; }
        let ok = i + n <= end;
        for (let k = 0; k < n && ok; k++) { const x = b[i + k]; if ((x & 0xc0) !== 0x80) ok = false; else u = u << 6 | (x & 63); }
        if (!ok || u > 0x10ffff) { s += "�"; continue; }
        i += n;
        s += String.fromCodePoint(u);
    }
    return s;
}

// a bounds-checked reader over one section
class Reader {
    constructor(b, at, end) { this.b = b; this.at = at; this.end = end; this.ok = true; }
    byte() { if (this.at >= this.end) { this.ok = false; return 0; } return this.b[this.at++]; }
    u32() {
        let r = 0;
        for (let sh = 0; sh < 35; sh += 7) {
            const x = this.byte();
            if (!this.ok) return 0;
            r += (x & 0x7f) * 2 ** sh;
            if (!(x & 0x80)) return r > 0xffffffff ? (this.ok = false, 0) : r;
        }
        this.ok = false;
        return 0;
    }
    sleb() { for (let i = 0; i < 10; i++) { const x = this.byte(); if (!this.ok || !(x & 0x80)) return; } this.ok = false; }
    skip(n) { if (this.at + n > this.end) this.ok = false; else this.at += n; }
    name() {
        const n = this.u32();
        if (!this.ok || this.at + n > this.end) { this.ok = false; return ""; }
        const s = utf8Range(this.b, this.at, this.at + n);
        this.at += n;
        return s;
    }
}

const TYPE_NAME = { 0x7f: "i32", 0x7e: "i64", 0x7d: "f32", 0x7c: "f64" };
const sigText = ([p, r]) => `(${p.map((t) => TYPE_NAME[t] || "?").join(", ")}) -> (${r.map((t) => TYPE_NAME[t] || "?").join(", ")})`;
const sameSig = (a, b) => a[0].length === b[0].length && a[1].length === b[1].length && a[0].every((t, i) => t === b[0][i]) && a[1].every((t, i) => t === b[1][i]);

// The file's own checks (the format's section 7 step 1), on its bytes; throws with the reason.
// -> { manifest, credits, licenses: [text], memoryPages }
export function inspect(bytes) {
    const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (b.length < 8 || b[0] !== 0 || b[1] !== 0x61 || b[2] !== 0x73 || b[3] !== 0x6d) refuse("not a WebAssembly module (no \\0asm magic)");
    if (b[4] !== 1 || b[5] || b[6] || b[7]) refuse("a WebAssembly module of another binary version (not 1)");
    if (b.length > MAX_FILE) refuse("larger than 64 MiB");
    const types = [], funcs = [], exports = new Map(), licenseByN = new Map();
    let importedFuncs = 0, memories = 0, memoryPages = 0, manifestText = null, credits = "";
    const hostImports = [];
    const r = new Reader(b, 8, b.length);
    while (r.at < r.end) {
        const id = r.byte(), size = r.u32();
        if (!r.ok || r.at + size > r.end) refuse("a section runs past the end of the file (a truncated or damaged file)");
        const s = new Reader(b, r.at, r.at + size);
        r.at += size;
        if (id === 0) {                                       // custom
            const nm = s.name();
            if (!s.ok) refuse("a custom section's name runs past its end");
            if (nm === "rfm1") {
                if (manifestText !== null) refuse("two rfm1 sections");
                manifestText = utf8Range(b, s.at, s.end);
            } else if (nm === "rfm1.credits") credits = utf8Range(b, s.at, s.end);
            else if (/^rfm1\.license\.\d{1,2}$/.test(nm) && +nm.slice(13) < 64) licenseByN.set(+nm.slice(13), utf8Range(b, s.at, s.end));
        } else if (id === 1) {                                // types
            for (let i = 0, n = s.u32(); i < n && s.ok; i++) {
                if (s.byte() !== 0x60) refuse("a type that is not a function type");
                const p = [], q = [];
                for (let k = 0, np = s.u32(); k < np && s.ok; k++) p.push(s.byte());
                for (let k = 0, nr = s.u32(); k < nr && s.ok; k++) q.push(s.byte());
                types.push([p, q]);
            }
        } else if (id === 2) {                                // imports: section 3's list only
            for (let i = 0, n = s.u32(); i < n && s.ok; i++) {
                const mod = s.name(), fld = s.name(), kind = s.byte();
                if (!s.ok) break;
                const what = `${mod}.${fld}`;
                if (kind !== 0) refuse(`it imports ${what} (a ${["", "table", "memory", "global"][kind] || "non-function"}): the format allows only WASI functions (section 3)`);
                const ti = s.u32();
                if (mod === HOST) {                           // a Fantasy CFW's file API: allowed by its manifest (below)
                    const hs = Object.prototype.hasOwnProperty.call(HOST_IMPORTS, fld) ? HOST_IMPORTS[fld] : null;
                    if (!hs) refuse(`it imports ${what}, which this site doesn't know (it needs a newer RemoteFM1)`);
                    if (ti >= types.length) refuse(`the import ${what} has no type`);
                    if (!sameSig(types[ti], hs)) refuse(`the import ${what} has the signature ${sigText(types[ti])}, not ${sigText(hs)}`);
                    hostImports.push(fld);
                    importedFuncs++;
                    continue;
                }
                const sig = mod === WASI && Object.prototype.hasOwnProperty.call(IMPORTS, fld) ? IMPORTS[fld] : null;
                if (!sig) refuse(`it imports ${what}, which the format doesn't allow (only ${WASI}'s ${Object.keys(IMPORTS).join(", ")})`);
                if (ti >= types.length) refuse(`the import ${what} has no type`);
                if (!sameSig(types[ti], sig)) refuse(`the import ${what} has the signature ${sigText(types[ti])}, not ${sigText(sig)}`);
                importedFuncs++;
            }
        } else if (id === 3) {                                // functions
            for (let i = 0, n = s.u32(); i < n && s.ok; i++) funcs.push(s.u32());
        } else if (id === 5) {                                // memory: one, fixed, at most 32 MiB
            for (let i = 0, n = s.u32(); i < n && s.ok; i++) {
                const flags = s.byte(), init = s.u32(), max = flags & 1 ? s.u32() : 0;
                if (!s.ok) break;
                memories++;
                if (flags & 4) refuse("a 64-bit memory (memory64): the format's memory is wasm32");
                if (flags & 2) refuse("a shared memory (threads): not in the format");
                if (!(flags & 1) || max !== init)
                    refuse(`its memory can grow (initial ${init} pages, maximum ${flags & 1 ? max : "none"}): the format wants a fixed memory, initial = maximum (section 6)`);
                if (init * PAGE > MAX_VIRTUAL_MEMORY) refuse(`its memory is ${init / 16} MiB: at most 512 MiB, for a Fantasy CFW`);
                memoryPages = init;                   // (32 MiB unless the manifest says virtual: below)
            }
        } else if (id === 7) {                                // exports
            for (let i = 0, n = s.u32(); i < n && s.ok; i++) {
                const name = s.name(), kind = s.byte(), index = s.u32();
                if (s.ok && !exports.has(name)) exports.set(name, { kind, index });
            }
        } else if (id === 11) {                               // data: active segments in memory 0
            for (let i = 0, n = s.u32(); i < n && s.ok; i++) {
                const flags = s.u32();
                if (!s.ok) break;
                if (flags !== 0) refuse(flags === 1 ? "a passive data segment (memory.init): not in the format (section 6)" : "a data segment for another memory: not in the format (section 6)");
                const op = s.byte();
                if (op === 0x41) s.sleb(); else if (op === 0x23) s.u32(); else refuse("a data segment whose offset is not a constant");
                if (s.byte() !== 0x0b) refuse("a data segment whose offset is not a constant");
                s.skip(s.u32());
            }
        }
        if (!s.ok) refuse(`a damaged section (id ${id})`);
    }
    if (manifestText === null) refuse("no rfm1 section (the manifest): a WebAssembly module, but not an .rfm1 file");
    const licenses = [];
    for (let n = 0; licenseByN.has(n); n++) licenses.push(licenseByN.get(n));

    // the manifest (section 4)
    let m;
    try { m = JSON.parse(manifestText); } catch (_) { m = null; }
    if (!m || typeof m !== "object" || Array.isArray(m)) refuse("its manifest (the rfm1 section) is not a JSON object");
    if (typeof m.rfm1 !== "number") refuse("its manifest has no \"rfm1\" (the format's version)");
    if (m.rfm1 !== 1) refuse(`an .rfm1 of format version ${m.rfm1}: this site reads version 1`);
    if (m.abi !== ABI) refuse(`its manifest's "abi" is not ${ABI}`);
    for (const k of REQUIRED_FIELDS) if (typeof m[k] !== "string" || !m[k]) refuse(`its manifest has no "${k}" (a required field, section 4)`);
    if (!/^[a-z0-9_]{1,31}$/.test(m.id)) refuse(`its manifest's id "${m.id}" is not [a-z0-9_]{1,31}`);
    if (m.authors !== undefined && !Array.isArray(m.authors)) refuse("its manifest's \"authors\" is not a list");
    if (m.glue_url !== undefined && typeof m.glue_url !== "string") refuse("its manifest's \"glue_url\" is not a string");

    // a Fantasy CFW (VIRTUAL-CARTS.md sections 1-4): what it needs first, then its limits and its file API
    if (m.virtual !== undefined && typeof m.virtual !== "boolean") refuse("its manifest's \"virtual\" is not true or false");
    const virtual = m.virtual === true;
    if (m.needs !== undefined && (!Array.isArray(m.needs) || !m.needs.every((x) => typeof x === "string"))) refuse("its manifest's \"needs\" is not a list of names");
    const needs = Array.isArray(m.needs) ? m.needs.slice() : [];
    const unknown = needs.filter((x) => !KNOWN_NEEDS.includes(x));
    if (unknown.length) refuse(`it needs a newer RemoteFM1 (it needs ${unknown.map((x) => `"${x}"`).join(", ")})`);
    if (needs.length && !virtual) refuse("its manifest lists \"needs\" but is not \"virtual\" (a Fantasy CFW)");
    const folders = {};
    if (m.folders !== undefined) {
        if (!m.folders || typeof m.folders !== "object" || Array.isArray(m.folders)) refuse("its manifest's \"folders\" is not an object");
        for (const [k, v] of Object.entries(m.folders)) {
            if (!FS_ROOTS.includes(k)) continue;                      // (a folder this site doesn't know: never granted)
            if (v !== "r" && v !== "rw") refuse(`its manifest's "folders" gives ${k} "${v}" (only "r" or "rw")`);
            folders[k] = v;
        }
    }
    if (hostImports.length && !(virtual && needs.includes("fs1")))
        refuse(`it imports ${HOST} (the RemoteFM1 folder) but its manifest doesn't say "virtual": true with "fs1" in "needs"`);
    if (!virtual && memoryPages * PAGE > MAX_MEMORY) refuse(`its memory is ${memoryPages / 16} MiB: at most 32 MiB (section 6)`);
    if (virtual && b.length >= MAX_VIRTUAL_FILE) refuse("larger than 16 MiB: a Fantasy CFW's file is under 16 MiB");

    // the memory and the exports (section 2)
    if (memories !== 1) refuse(memories ? "more than one memory" : "no memory of its own (the format wants one, exported)");
    const mem = exports.get("memory");
    if (!mem || mem.kind !== 2 || mem.index !== 0) refuse("its memory is not exported as \"memory\"");
    for (const [name, nargs, nret] of EXPORTS) {
        const e = exports.get(name);
        if (!e) { if (name === "_initialize") continue; refuse(`it has no export ${name} (section 2)`); }
        if (e.kind !== 0 || e.index < importedFuncs || e.index - importedFuncs >= funcs.length) refuse(`its export ${name} is not a function of its own`);
        const ti = funcs[e.index - importedFuncs];
        if (ti >= types.length) refuse(`its export ${name} has no type`);
        const want = [new Array(nargs).fill(I32), new Array(nret).fill(I32)];
        if (!sameSig(types[ti], want)) refuse(`its export ${name} has the signature ${sigText(types[ti])}, not ${sigText(want)}`);
    }
    const labels = LABEL_EXPORTS.every(([name, nargs, nret]) => {
        const e = exports.get(name);
        return e && e.kind === 0 && e.index >= importedFuncs && e.index - importedFuncs < funcs.length && funcs[e.index - importedFuncs] < types.length
            && sameSig(types[funcs[e.index - importedFuncs]], [new Array(nargs).fill(I32), new Array(nret).fill(I32)]);
    });
    return { manifest: m, labels, credits, licenses, memoryPages, virtual, needs, folders, fs: hostImports.length > 0 || needs.includes("fs1") };
}

// the module compiled once per file bytes (the tests boot one file many times; the page sends fresh bytes)
const compiled = new WeakMap();
async function prepare(wasm) {
    const buf = wasm instanceof ArrayBuffer ? wasm : wasm && wasm.buffer;
    if (!(buf instanceof ArrayBuffer)) throw new Error("rfm1 engine: files.wasm must be the .rfm1 file's bytes");
    const hit = compiled.get(buf);
    if (hit && hit.of === wasm) return hit;
    const bytes = wasm instanceof ArrayBuffer ? new Uint8Array(wasm) : new Uint8Array(wasm.buffer, wasm.byteOffset, wasm.byteLength);
    const file = inspect(bytes);
    let module;
    try { module = await WebAssembly.compile(bytes); } catch (e) { refuse(`the browser can't compile it (${e.message || e})`); }
    const p = { of: wasm, file, module };
    compiled.set(buf, p);
    return p;
}

// ---- a Fantasy CFW's file API (VIRTUAL-CARTS.md section 4): rfm1_host_1's four imports
// A request is checked here (the op, the path, the manifest's folders, the buffer, the limits), given an id and sent
// at once with options.fsSend; its folder path is the cart's view mapped onto the RemoteFM1 folder (/audio/x ->
// audio/x, /code/x -> code/<id>/x, /data/x -> data/<id>/x). The answer (fsDone) waits in the inbox until the start of
// the next render() (apply), which copies it into the module's memory: the module sees nothing change during a call.
// No sender (a page without a folder): every request ends in FS_ENOFOLDER, at the same point.
//   to the page:   { type: "fs", key, id, op, path, offset, len, data? }       data: a WRITE's bytes (transferred)
//   from the page: { type: "fsDone", key, id, result, data?, stat?, entries? } result < 0: an FS_ERR; data: a READ's
//                  bytes; stat: { size, mtime (ms), dir }; entries: a LIST's [[name, isDir], ...] (unsorted, all)
const utf8Bytes = (s) => {                          // (no TextEncoder in a worklet)
    const out = [];
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | c >> 6, 0x80 | c & 63);
        else if (c < 0x10000) out.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63);
        else out.push(0xf0 | c >> 18, 0x80 | c >> 12 & 63, 0x80 | c >> 6 & 63, 0x80 | c & 63);
    }
    return out;
};
const cmpBytes = (a, b) => { for (let i = 0; i < a.length && i < b.length; i++) if (a[i] !== b[i]) return a[i] - b[i]; return a.length - b.length; };

class FileApi {
    constructor(rt, id, folders, send) {
        this.rt = rt; this.id = id; this.folders = folders; this.send = send;
        this.key = `${id}:${Math.floor(Math.random() * 2 ** 31).toString(36)}:${Date.now().toString(36)}`;
        this.reqs = new Map();          // id -> { op, buf, len, done, canceled, result }
        this.inbox = [];
        this.next = 1;
        this.changed = 0;
        this.listCache = null;          // { path, entries }: a LIST's sorted entries while the cart pages through them
    }
    imports() {
        return {
            fs_request: (op, path, pathLen, offset, buf, len) => this.request(op, path, pathLen, offset, buf, len),
            fs_poll: (id) => this.poll(id),
            fs_cancel: (id) => this.cancel(id),
            fs_changed: () => this.changed | 0,
        };
    }
    // the cart's path (bytes in its memory) -> the folder's ("code/<id>/droplets"), or null when section 3 refuses it
    folderPath(at, n) {
        const u8 = this.rt.u8;
        if (n < 2 || n > FS_MAX_PATH || at < 0 || at + n > u8.length) return null;
        const b = u8.subarray(at, at + n);
        if (b[0] !== 0x2f) return null;
        for (const c of b) if (c === 0 || c === 0x5c) return null;     // (no NUL, no backslash)
        const s = utf8Range(b, 0, n);
        if (s.includes("�")) return null;
        const seg = s.slice(1).split("/");
        if (!FS_ROOTS.includes(seg[0])) return null;
        for (const x of seg) if (!x || x === "." || x === ".." || utf8Bytes(x).length > 255) return null;
        const root = seg[0], rest = seg.slice(1);
        return { root, top: rest.length === 0, path: (root === "audio" ? ["audio"] : [root, this.id]).concat(rest).join("/") };
    }
    request(op, pathAt, pathLen, offset, buf, len) {
        const E = FS_ERR, O = FS_OP;
        if (!(op >= 1 && op <= 6)) return E.EIO;
        let inflight = 0;
        for (const r of this.reqs.values()) if (!r.canceled) inflight++;     // (a cancelled one: no longer the host's)
        if (inflight >= FS_MAX_INFLIGHT) return E.EBUSY;
        const p = this.folderPath(pathAt >>> 0, pathLen | 0);
        if (!p) return E.EACCES;
        const write = op === O.WRITE || op === O.DELETE || op === O.MKDIR;
        const mode = this.folders[p.root] || "";
        if (write ? p.root !== "data" || mode !== "rw" || p.top : !mode.includes("r")) return E.EACCES;
        const off = typeof offset === "bigint" ? offset : BigInt(offset);
        buf >>>= 0; len |= 0;
        if (len < 0) return E.EIO;
        const size = this.rt.u8.length;
        let n = 0, o = 0;
        if (op === O.STAT) { if (len < 16 || buf + 16 > size) return E.EIO; n = 16; }
        else if (op === O.LIST || op === O.READ || op === O.WRITE) {
            n = Math.min(len, FS_MAX_LEN);
            if (buf + n > size) return E.EIO;
            if (off < (op === O.WRITE ? -1n : 0n) || off > 2n ** 53n) return E.EIO;
            o = Number(off);
            if (op === O.LIST && o > 0x7fffffff) return E.EIO;
        }
        const id = this.next;
        this.next = this.next >= 0x7ffffff0 ? 1 : this.next + 1;
        const r = { op, buf, len: n, done: false, canceled: false, result: E.PENDING, path: p.path, offset: o };
        this.reqs.set(id, r);
        if (!this.send) { this.inbox.push({ key: this.key, id, result: E.ENOFOLDER }); return id; }
        const msg = { type: "fs", key: this.key, id, op, path: p.path, offset: o, len: n };
        let transfer;
        if (op === O.WRITE) { msg.data = this.rt.u8.slice(buf, buf + n).buffer; transfer = [msg.data]; }
        if (op === O.LIST && o > 0 && this.listCache && this.listCache.path === p.path) {
            // (the next page of a listing: served from the one just read, unless the folder changed meanwhile)
            this.inbox.push({ key: this.key, id, result: 0, sorted: this.listCache.entries });
            return id;
        }
        try { this.send(msg, transfer); } catch (_) { this.inbox.push({ key: this.key, id, result: E.EIO }); }
        return id;
    }
    poll(id) {
        const r = this.reqs.get(id);
        if (!r) return FS_ERR.ENOENT;
        if (r.canceled) { this.reqs.delete(id); return FS_ERR.ECANCELED; }
        if (!r.done) return FS_ERR.PENDING;
        this.reqs.delete(id);
        return r.result;
    }
    cancel(id) {
        const r = this.reqs.get(id);
        if (r) { r.canceled = true; r.done = true; }
    }
    deliver(m) { if (m && m.key === this.key) this.inbox.push(m); }
    bump() { this.changed = (this.changed + 1) | 0; this.listCache = null; }
    // the answers, into memory (the start of render(): between export calls)
    apply() {
        if (!this.inbox.length) return;
        const box = this.inbox;
        this.inbox = [];
        for (const m of box) {
            const r = this.reqs.get(m.id);
            if (!r || r.done) continue;                    // (polled, cancelled)
            r.done = true;
            r.result = this.copy(r, m);
        }
    }
    copy(r, m) {
        const E = FS_ERR, O = FS_OP, u8 = this.rt.u8, dv = this.rt.dv;
        const res = typeof m.result === "number" ? m.result | 0 : E.EIO;
        if (res < 0) return Object.values(E).includes(res) && res !== E.PENDING ? res : E.EIO;
        if (r.op === O.READ) {
            const d = m.data ? new Uint8Array(m.data instanceof ArrayBuffer ? m.data : m.data.buffer, m.data.byteOffset || 0, m.data.byteLength) : new Uint8Array(0);
            const n = Math.min(d.length, r.len);
            u8.set(d.subarray(0, n), r.buf);
            return n;
        }
        if (r.op === O.STAT) {
            const st = m.stat;
            if (!st) return E.EIO;
            dv.setBigUint64(r.buf, BigInt(Math.max(0, Math.floor(+st.size || 0))), true);
            dv.setUint32(r.buf + 8, Math.max(0, Math.min(0xffffffff, Math.floor((+st.mtime || 0) / 1000))), true);
            dv.setUint32(r.buf + 12, st.dir ? 1 : 0, true);
            return 16;
        }
        if (r.op === O.LIST) {
            let sorted = m.sorted;
            if (!sorted) {
                // no hidden files, names of 1-255 bytes, sorted by their bytes
                sorted = (Array.isArray(m.entries) ? m.entries : [])
                    .filter((e) => Array.isArray(e) && typeof e[0] === "string" && e[0] && e[0][0] !== "." && !e[0].includes("/"))
                    .map((e) => ({ b: utf8Bytes(e[0]), dir: !!e[1] })).filter((e) => e.b.length <= 255)
                    .sort((a, b) => cmpBytes(a.b, b.b));
                this.listCache = { path: r.path, entries: sorted };
            }
            let at = r.buf, end = r.buf + r.len, k = 0;
            for (let i = r.offset; i < sorted.length; i++) {
                const e = sorted[i];
                if (at + 2 + e.b.length > end) break;
                u8[at] = e.dir ? 1 : 0; u8[at + 1] = e.b.length; u8.set(e.b, at + 2);
                at += 2 + e.b.length; k++;
            }
            if (!k && r.offset < sorted.length) return E.EIO;     // (a buffer too small for the next name)
            return k;
        }
        if (r.op === O.WRITE) return Math.min(res, r.len);
        return 0;
    }
}

class Exit extends Error { constructor(code) { super(`proc_exit(${code})`); this.code = code; } }

// section 3's WASI: fd_write to the log (stdout / stderr, rate-limited), the rest EBADF, the clock the host's, proc_exit
// halts the instance
function makeImports(rt, log, fsApi) {
    const EBADF = 8, EINVAL = 28;
    let line = [], tokens = 80, last = now(), dropped = 0;
    const emit = (fd, text) => {
        const t = now();
        tokens = Math.min(80, tokens + (t - last) * 0.04);      // 40 lines a second, bursts of 80
        last = t;
        if (tokens < 1) { dropped++; return; }
        tokens -= 1;
        if (dropped) { log(2, `(${dropped} log lines dropped)`); dropped = 0; }
        log(fd, text);
    };
    const wasi = {
        fd_write(fd, iovs, n, nwritten) {
            const u8 = rt.u8, size = u8.length, dv = rt.dv;
            if (fd !== 1 && fd !== 2) return EBADF;
            let total = 0;
            for (let i = 0; i < n; i++) {
                if (iovs + 8 * i + 8 > size) return EINVAL;
                const p = dv.getUint32(iovs + 8 * i, true), len = dv.getUint32(iovs + 8 * i + 4, true);
                if (p + len > size) return EINVAL;
                for (let k = 0; k < len; k++) {
                    const c = u8[p + k];
                    if (c === 10) { emit(fd, utf8Range(line, 0, line.length)); line = []; }
                    else if (line.length < 400) line.push(c);
                }
                total += len;
            }
            if (nwritten + 4 <= size) dv.setUint32(nwritten, total, true);
            return 0;
        },
        fd_read: () => EBADF,
        fd_close: () => EBADF,
        fd_seek: () => EBADF,
        clock_time_get(id, precision, out) {      // (diagnostics only: the firmware's time is tick's)
            if (out + 8 > rt.u8.length) return EINVAL;
            rt.dv.setBigUint64(out, BigInt(Math.round(now() * 1e6)), true);
            return 0;
        },
        proc_exit(code) { rt.exitCode = code & 0xff; throw new Exit(code & 0xff); },
    };
    return fsApi ? { [WASI]: wasi, [HOST]: fsApi.imports() } : { [WASI]: wasi };
}

class Rfm1Engine {
    constructor(rt, file, sampleRate, log, fsApi = null) {
        this.rt = rt;
        this.fs = fsApi;
        this.ex = rt.ex;
        this.file = file;
        this.log = log;
        this.sampleRate = sampleRate;
        this.disposed = false;
        this.dead = false;
        this.haltCode = 0;
        this.ms = 0;
        this.lastFrame = 0;
        this.framesDone = 0;
        this.haveBlk = false;
        this.lastMaster = undefined;
        this.midiPending = [];
        this.parser = new MidiParser();
        this.ledSeq = 0;
        this.ledLast = new Uint8Array(45);
        this.screenSeq = -1;
        this.screenCopy = null;
        this.blkL = new Float32Array(BLOCK);
        this.blkR = new Float32Array(BLOCK);
        this.silence = [new Float32Array(BLOCK), new Float32Array(BLOCK)];
        this.rs = sampleRate === FS ? null : new Resampler(sampleRate, () => this.pullBlock());
    }

    // ---- the instance's memory and calls
    inMem(off, n) { return off > 0 && off + n <= this.rt.u8.length; }
    cstr(off) {
        const u8 = this.rt.u8;
        if (!off || off >= u8.length) return null;
        const e = u8.indexOf(0, off);
        return e < 0 ? null : utf8Range(u8, off, e);
    }
    word(off) { return this.rt.dv.getUint32(off, true); }
    // an export called; a trap halts the instance (0 returned)
    call(name, a, b, c, d) {
        if (this.dead) return 0;
        try { return this.ex[name](a, b, c, d); } catch (e) { this.die(name, e); return 0; }
    }
    die(where, e) {
        if (this.dead) return;
        this.dead = true;
        if (this.rt.exitCode >= 0) this.haltCode = 0x100 | (this.rt.exitCode & 0xff);
        else this.haltCode = HALT_TRAP;
        const why = this.rt.exitCode >= 0 ? `exit(${this.rt.exitCode})` : `${where}: ${(e && e.message) || e}`;
        this.log(2, `${this.desc ? this.desc.id : "rfm1"}: halted (${why})`);
    }
    putStr(s) {                                    // a C string into the scratch block; its address
        const u8 = this.rt.u8;
        let n = 0;
        for (let i = 0; i < s.length && n < STR_CAP - 1; i++) u8[this.sStr + n++] = s.charCodeAt(i) & 0x7f;
        u8[this.sStr + n] = 0;
        return this.sStr;
    }

    // section 7 steps 2-3: throws with the reason
    open() {
        const ex = this.ex, rt = this.rt, m = this.file.manifest;
        if (rt.u8.length !== this.file.memoryPages * PAGE) refuse("its memory is not the size its module declares");
        const step = (what, fn) => {
            const v = fn();
            if (this.dead) refuse(`${what} trapped (${this.rt.exitCode >= 0 ? `exit(${this.rt.exitCode})` : "a trap"})`);
            return v;
        };
        if (ex._initialize) step("_initialize", () => this.call("_initialize"));
        const v = step("rfm1_version", () => this.call("rfm1_version"));
        if (v !== 1) refuse(`rfm1_version() is ${v}: this site reads version 1`);
        const d = step("rfm1_get", () => this.call("rfm1_get", ABI)) >>> 0;
        if (!d) refuse(`rfm1_get(${ABI}) is 0: the module doesn't speak ABI ${ABI}`);
        for (let i = 0; i <= LAYOUT.length; i++) {
            const got = step("rfm1_layout", () => this.call("rfm1_layout", i)) | 0, want = i < LAYOUT.length ? LAYOUT[i] : -1;
            if (got !== want) refuse(`rfm1_layout(${i}) is ${got}, this site expects ${want} (built against another fm1core.h?)`);
        }
        if (!this.inMem(d, C.size)) refuse("its descriptor is outside its memory");
        const w = (k) => this.word(d + C[k]);
        if (w("abi_version") !== ABI) refuse(`its descriptor's abi_version is ${w("abi_version")}`);
        const id = this.cstr(w("id")), name = this.cstr(w("name")), version = this.cstr(w("version"));
        if (id === null || name === null || version === null) refuse("its descriptor's id, name or version is not a string in its memory");
        if (id !== m.id || name !== m.name || version !== m.version)
            refuse(`the manifest (${m.id}, ${m.name}, ${m.version}) and the descriptor (${id}, ${name}, ${version}) disagree`);
        const panel = w("panel"), keymap = w("keymap");
        if (!this.inMem(panel, P.size)) refuse("its descriptor's panel is outside its memory");
        if (keymap && !this.inMem(keymap, 6 * 11)) refuse("its descriptor's keymap is outside its memory");
        const has = (k) => w(k) !== 0;
        this.desc = {
            id, name, version, family: this.cstr(w("family")) || "", gated: this.cstr(w("gated")),
            credits: this.cstr(w("credits")), flashSize: has("flash") ? w("flash_size") : 0,
            buttonNames: Array.from({ length: NB }, (_, i) => this.cstr(this.word(d + C.button_names + 4 * i)) || ""),
            keymap, has: { midiIn: has("midi_in"), midiOut: has("midi_out_take"), shutdown: has("shutdown"), halted: has("halted"),
                           flashStage: has("flash_stage"), flash: has("flash"), flashDirty: has("flash_dirty"), flashSync: has("flash_sync") },
        };
        this.panel = panel;
        const s = step("rfm1_alloc", () => this.call("rfm1_alloc", SCRATCH)) >>> 0;
        if (!this.inMem(s, SCRATCH)) refuse(`rfm1_alloc(${SCRATCH}) failed`);
        this.sStr = s;
        this.sWords = s + STR_CAP;
        this.sAudio = (this.sWords + WORDS + 3) & ~3;
        this.audio = new Int16Array(rt.buf, this.sAudio, 2 * BLOCK);
        this.lcd = new Uint16Array(rt.buf, panel + P.lcd, LCD_PIXELS);
    }

    // section 7 step 4: power-on from the flash given (null: a fresh, erased flash); throws when it can't
    boot(flash) {
        const u8 = this.rt.u8, ds = this.desc;
        if (ds.has.flashStage) {
            if (flash && flash.length) {
                const n = flash.length, p = this.call("rfm1_alloc", n) >>> 0;
                if (!this.dead && !this.inMem(p, n)) refuse(`rfm1_alloc(${n}) for the saved flash failed`);
                if (!this.dead) { u8.set(flash, p); this.call("rfm1_flash_stage", p, n); this.call("rfm1_free", p); }
            } else this.call("rfm1_flash_stage", 0, 0);
        }
        this.call("rfm1_options", this.putStr(STAGED), 0, 0, 1);
        this.call("rfm1_boot_options", -1, 0, -1);
        u8[this.panel + P.ready] = 2;
        this.call("rfm1_init", 0);
        if (this.dead) refuse(`the firmware stopped during its power-on (${this.haltCode >= 0x200 ? "a trap" : `exit(${this.haltCode & 0xff})`})`);
        if (ds.flashSize) {
            const p = this.call("rfm1_flash") >>> 0;
            this.flashAt = this.inMem(p, ds.flashSize) ? p : 0;
        }
        this._info = this.readInfo();
    }

    readInfo() {
        const u8 = this.rt.u8, ds = this.desc, m = this.file.manifest, pn = this.panel;
        let keymap = null;
        if (ds.keymap) {
            keymap = [];
            for (let r = 0; r < 6; r++) {
                const row = [];
                for (let c = 0; c < 11; c++) row.push((u8[ds.keymap + r * 11 + c] << 24) >> 24);
                keymap.push(row);
            }
        }
        return {
            id: ds.id,
            name: ds.name,
            version: ds.version,
            license: m.license,
            source: m.source_url,
            credits: this.file.credits || ds.credits || null,
            family: ds.family,
            latencyFrames: this.rs ? Math.round(RS_HALF * this.sampleRate / FS) : 0,
            keymap,
            buttonNames: ds.buttonNames,
            // (beyond API.md, as the native engine's) the panel table after power-on: btnId[label], encId[role], encDir[role]
            panelTable: {
                btnId: Array.from(u8.subarray(pn + P.btn_id, pn + P.btn_id + 14)),
                encId: Array.from(u8.subarray(pn + P.enc_id, pn + P.enc_id + 7)),
                encDir: Array.from(u8.subarray(pn + P.enc_dir, pn + P.enc_dir + 7), (v) => (v << 24) >> 24),
            },
            // (beyond API.md) the file's own: its manifest, the build glue's source, the licence texts
            authors: Array.isArray(m.authors) ? m.authors.map(String) : [],
            glueUrl: typeof m.glue_url === "string" ? m.glue_url : null,
            builtBy: typeof m.built_by === "string" ? m.built_by : null,
            licenseTexts: this.file.licenses.slice(),
            flashSize: ds.flashSize,
            memoryBytes: this.rt.u8.length,
            // (beyond API.md) a Fantasy CFW (VIRTUAL-CARTS.md): the manifest's flag, what it needs, its folders
            virtual: this.file.virtual, needs: this.file.needs.slice(), folders: { ...this.file.folders },
        };
    }

    info() { return this._info; }

    // the live panel labels (API.md): null without the exports or at epoch 0; re-read when the epoch moves
    labels() {
        if (!this.file.labels || this.disposed || this.dead) return null;
        const seq = this.call("rfm1_labels_epoch") >>> 0;
        if (!seq || this.dead) return null;
        if (this.lbl && this.lbl.seq === seq) return this.lbl;
        const list = [];
        for (let i = 0; i < LBL_N; i++) list.push(this.label(this.call("rfm1_label", i) >>> 0));
        return (this.lbl = labelSet(seq, list));
    }
    // one label: a C string in memory (read up to 64 bytes), cleanLabel'd (null: none given)
    label(p) {
        const u8 = this.rt.u8;
        if (!p || p >= u8.length) return null;
        let s = "";
        for (let i = p; i < u8.length && i < p + 64 && u8[i]; i++) s += String.fromCharCode(u8[i]);
        return cleanLabel(s);
    }

    // ---- the clock: one device millisecond (wasm_glue.c run_ms)
    runMs1() {
        const ex = this.ex, ms = this.ms++;
        const frame = ms === 0 || ms - this.lastFrame >= 15;
        try {
            ex.rfm1_tick(ms);
            if (frame) { this.lastFrame = ms; ex.rfm1_frame(); } else ex.rfm1_idle();
            while (this.framesDone + BLOCK <= Math.floor(ms * FS / 1000)) {
                ex.rfm1_audio(this.sAudio, BLOCK);             // (at most one a ms: 44.1 frames < 128)
                this.framesDone += BLOCK;
                this.haveBlk = true;
            }
        } catch (e) { this.die(`device ms ${ms}`, e); }
    }

    pullBlock() {
        if (this.dead || this.disposed) return this.silence;
        this.haveBlk = false;
        while (!this.haveBlk && !this.dead) this.runMs1();
        if (this.dead) return this.silence;
        const s = this.audio, L = this.blkL, R = this.blkR;
        for (let i = 0; i < BLOCK; i++) {
            L[i] = s[2 * i] * (1 / 32768);
            R[i] = s[2 * i + 1] * (1 / 32768);
        }
        return [L, R];
    }

    setPanel(p) {
        if (this.disposed || this.dead) return;
        const dv = this.rt.dv, u8 = this.rt.u8, h = this.panel;
        const KM = (1 << NKEY) - 1;
        const keys = (p.keys >>> 0) & KM;
        if (p.keyVel) {                          // velocities before the keys' bits (fm1core.h)
            const lean = p.keyLean;
            for (let k = 0; k < NKEY; k++) {
                u8[h + P.key_vel + k] = Math.min(127, (p.keyVel[k] | 0) >>> 0);
                u8[h + P.key_lean + k] = Math.min(100, (lean ? lean[k] | 0 : 0) >>> 0);
            }
        }
        if (p.enc) for (let i = 0; i < NENC; i++) if (p.enc[i]) {
            const at = h + P.enc + 4 * i;
            dv.setInt32(at, (dv.getInt32(at, true) + (p.enc[i] | 0)) | 0, true);
        }
        const t = p.taps || {};
        // MASTER: the firmware's own default level stands until the pot moves (as the native engine)
        if (typeof p.master === "number") {
            const m = Math.max(0, Math.min(1023, p.master | 0));
            if (this.lastMaster !== undefined && m !== this.lastMaster) dv.setInt32(h + P.master, m, true);
            this.lastMaster = m;
        }
        const buttons = (p.buttons >>> 0) & 0x3fff;
        const oldK = dv.getUint32(h + P.keys, true), oldB = dv.getUint32(h + P.buttons, true);
        const tk = ((t.keys >>> 0) || 0) & KM, tb = ((t.buttons >>> 0) || 0) & 0x3fff;
        dv.setUint32(h + P.keys_tap, (dv.getUint32(h + P.keys_tap, true) | (keys & ~oldK) | tk) >>> 0, true);
        dv.setUint32(h + P.keys, keys, true);
        dv.setUint32(h + P.buttons_tap, (dv.getUint32(h + P.buttons_tap, true) | (buttons & ~oldB) | tb) >>> 0, true);
        dv.setUint32(h + P.buttons, buttons, true);
    }

    render(left, right) {
        if (this.disposed || this.dead) { left.fill(0); right.fill(0); return; }
        if (this.fs) this.fs.apply();
        this.feedMidi();
        if (!this.rs) {
            let o = 0;
            while (o < left.length) {
                const [L, R] = this.pullBlock();
                const n = Math.min(BLOCK, left.length - o);
                left.set(n === BLOCK ? L : L.subarray(0, n), o);
                right.set(n === BLOCK ? R : R.subarray(0, n), o);
                o += n;
            }
            return;
        }
        this.rs.render(left, right);
    }

    feedMidi() {
        if (!this.desc.has.midiIn) { this.midiPending.length = 0; return; }
        while (this.midiPending.length && !this.dead) {
            if (!this.call("rfm1_midi_in", this.midiPending[0])) break;   // (no room, or a trap: next quantum)
            this.midiPending.shift();
        }
    }

    screen() {
        if (this.disposed) return { seq: this.screenSeq, pixels: this.screenCopy || new Uint16Array(LCD_PIXELS) };
        const seq = this.rt.dv.getUint32(this.panel + P.lcd_writes, true);
        if (seq !== this.screenSeq || !this.screenCopy) {
            // the panel's RAM is big-endian RGB565 as sent: swapped to the value
            const src = this.lcd, out = new Uint16Array(LCD_PIXELS);
            for (let i = 0; i < LCD_PIXELS; i++) { const v = src[i]; out[i] = (v >> 8) | ((v & 0xff) << 8); }
            this.screenCopy = out;
            this.screenSeq = seq;
        }
        return { seq, pixels: this.screenCopy };
    }

    leds() {
        const u8 = this.rt.u8, p = this.panel + P.led;
        const levels = u8.slice(p, p + 44);
        const playGreen = u8[this.panel + P.led_play_green];
        let changed = playGreen !== this.ledLast[44];
        for (let i = 0; i < 44 && !changed; i++) changed = levels[i] !== this.ledLast[i];
        if (changed) {
            this.ledSeq++;
            this.ledLast.set(levels);
            this.ledLast[44] = playGreen;
        }
        return { seq: this.ledSeq, levels, playGreen };
    }

    midiIn(bytes) {
        for (const b of bytes) this.parser.push(b & 0xff, this.midiPending);
        if (this.midiPending.length > 4096) this.midiPending.splice(0, this.midiPending.length - 4096);
    }

    midiOut() {
        if (this.disposed || this.dead || !this.desc.has.midiOut) return new Uint8Array(0);
        const out = [], dv = this.rt.dv;
        for (let i = 0; i < 4096 && this.call("rfm1_midi_out_take", this.sWords); i++) packetBytes(dv.getUint32(this.sWords, true), out);
        return Uint8Array.from(out);
    }

    flash() {
        const seq = this.disposed || this.dead || !this.desc.has.flashDirty ? this.lastFlashSeq || 0 : this.call("rfm1_flash_dirty") >>> 0;
        this.lastFlashSeq = seq;
        const self = this;
        // the image on request (a flash_sync and a 1 MiB copy): the firmware's deferred saves written first
        return { seq, get bytes() { return self.flashBytes(); } };
    }

    flashBytes() {
        if (this.disposed || !this.flashAt) return null;
        if (!this.dead && this.desc.has.flashSync && !this.halted()) this.call("rfm1_flash_sync");
        return this.rt.u8.slice(this.flashAt, this.flashAt + this.desc.flashSize);
    }

    halted() {
        if (this.disposed) return 0;
        if (this.dead) return this.haltCode;
        const h = this.desc.has.halted ? this.call("rfm1_halted") >>> 0 : 0;
        return this.dead ? this.haltCode : h;
    }

    dispose() {
        if (this.disposed) return;
        if (!this.dead && this.desc && this.desc.has.shutdown) this.call("rfm1_shutdown");
        this.disposed = true;
        this.ex = null;
    }

    // (beyond API.md, for tests) the device's simulated milliseconds since power-on
    deviceMs() { return this.ms; }
    // (beyond API.md, for tests) n device milliseconds with their audio dropped
    runMs(n) { if (this.fs && !this.dead && !this.disposed) this.fs.apply(); for (let i = 0; i < n && !this.dead && !this.disposed; i++) this.runMs1(); }

    // (beyond API.md) a Fantasy CFW's file API: the page's answer to one of its requests (applied at the next render),
    // and "the folder changed" (fs_changed() moves)
    fsDone(m) { if (this.fs && !this.disposed) this.fs.deliver(m); }
    fsChanged() { if (this.fs) this.fs.bump(); }
}

// files.wasm: the .rfm1 file's bytes (ArrayBuffer or typed array). options.log(fd, line): the firmware's printf
// (default: dropped). options.fsSend(msg, transfer): a Fantasy CFW's file requests (none: they end in FS_ENOFOLDER).
// Throws with the reason when the file is not one this engine runs (section 7).
export async function createEngine({ sampleRate, files, flash = null, options = {} }) {
    if (!files || !files.wasm) throw new Error("rfm1 engine: files.wasm (the .rfm1 file) is required");
    const { file, module } = await prepare(files.wasm);
    const log = options.log || (() => {});
    const rt = { ex: null, exitCode: -1, buf: null, u8: null, dv: null };
    const fsApi = file.fs ? new FileApi(rt, file.manifest.id, file.folders, typeof options.fsSend === "function" ? options.fsSend : null) : null;
    let inst;
    try { inst = await WebAssembly.instantiate(module, makeImports(rt, log, fsApi)); }
    catch (e) { refuse(`it can't be instantiated (${e.message || e})`); }
    rt.ex = inst.exports;
    rt.buf = rt.ex.memory.buffer;
    rt.u8 = new Uint8Array(rt.buf);
    rt.dv = new DataView(rt.buf);
    const eng = new Rfm1Engine(rt, file, sampleRate || FS, log, fsApi);
    eng.open();
    if (flash instanceof ArrayBuffer) flash = new Uint8Array(flash);
    eng.boot(flash && flash.length ? flash : null);
    return eng;
}
