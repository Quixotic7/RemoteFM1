// SPDX-License-Identifier: GPL-3.0-only
// Fantasy CFW in the .rfm1 engine (FM1VST docs/VIRTUAL-CARTS.md): what inspect() lets in (virtual, needs, the
// rfm1_host_1 imports and their signatures, the memory and file limits), and the file API (section 4) against a
// temporary RemoteFM1 folder served by ./fs-dir.mjs: every op, its errors, the sandbox, the limits, cancel, and that
// an answer reaches the module only between two export calls. The module: fixtures/fs_cart.c (built, committed);
// each case adds its own manifest.
//   node web/engines/rfm1/fs-test.mjs
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createEngine, inspect, FS_OP as O, FS_ERR as E } from "./engine.js";
import { dirServer } from "./fs-dir.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CART = fs.readFileSync(path.join(HERE, "fixtures/fs_cart.wasm"));
const PLAIN = fs.readFileSync(path.join(HERE, "fixtures/fs_cart_plain.wasm"));
let fails = 0, n = 0;
const ok = (c, what) => { n++; if (!c) fails++; console.log(`  ${c ? "ok  " : "FAIL"}  ${what}`); return c; };

// ---- module surgery
const leb = (v) => { const o = []; do { let c = v & 0x7f; v = Math.floor(v / 128); if (v) c |= 0x80; o.push(c); } while (v); return o; };
const custom = (name, payload) => { const nb = Buffer.from(name), body = [...leb(nb.length), ...nb, ...payload]; return [0, ...leb(body.length), ...body]; };
const withManifest = (b, m) => Uint8Array.from([...b, ...custom("rfm1", Buffer.from(JSON.stringify(m)))]);
const MAN = { rfm1: 1, abi: 6, id: "fscart", name: "FS Cart", version: "1.0", license: "MIT", source_url: "https://example.com/fscart@0" };
const FANTASY = { ...MAN, virtual: true, needs: ["fs1"], folders: { audio: "r", code: "r", data: "rw" } };
function sections(b) {
    const out = [];
    let at = 8;
    const u = () => { let r = 0, sh = 0, x; do { x = b[at++]; r += (x & 0x7f) * 2 ** sh; sh += 7; } while (x & 0x80); return r; };
    while (at < b.length) { const start = at, id = b[at++], size = u(), body = at; at = body + size; out.push({ id, start, end: at, body }); }
    return out;
}
// the memory: initial = maximum = pages
function withMemory(b, pages) {
    const mem = sections(b).find((s) => s.id === 5);
    const nb = [1, 1, ...leb(pages), ...leb(pages)];
    return Uint8Array.from([...b.subarray(0, mem.start), 5, ...leb(nb.length), ...nb, ...b.subarray(mem.end)]);
}
const rename = (b, from, to) => { const i = Buffer.from(b).indexOf(Buffer.from(from)); const c = Uint8Array.from(b); c.set(Buffer.from(to), i); return c; };
// a module that only imports rfm1_host_1.fs_request with the signature (i32) -> i32 and has a manifest
const wrongSig = Uint8Array.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 6, 1, 0x60, 1, 0x7f, 1, 0x7f,
    2, ...(() => { const m = Buffer.from("rfm1_host_1"), f = Buffer.from("fs_request"); const body = [1, m.length, ...m, f.length, ...f, 0, 0]; return [...leb(body.length), ...body]; })(),
    ...custom("rfm1", Buffer.from(JSON.stringify(FANTASY)))]);

const refusedBy = (bytes) => { try { inspect(bytes); return null; } catch (e) { return e.message; } };
async function boots(bytes, options = {}) {
    try { const e = await createEngine({ sampleRate: 44100, files: { wasm: bytes }, flash: null, options }); return e; } catch (e) { return e.message; }
}

console.log("== inspect: what a Fantasy CFW may be");
{
    const plain = inspect(withManifest(PLAIN, MAN));
    ok(!plain.virtual && !plain.fs && plain.needs.length === 0, "a plain .rfm1 (no virtual, no imports): accepted, not virtual");
    const e = await boots(withManifest(PLAIN, MAN));
    ok(typeof e === "object" && e.info().virtual === false, "... and boots as today (info().virtual false)");
    const f = inspect(withManifest(CART, FANTASY));
    ok(f.virtual && f.fs && f.needs.join() === "fs1" && f.folders.data === "rw", "virtual + needs fs1 + rfm1_host_1 imports: accepted (folders read)");
    let m;
    m = refusedBy(withManifest(CART, MAN));
    ok(/imports rfm1_host_1 .*"virtual": true with "fs1"/.test(m || ""), `rfm1_host_1 imported by a non-virtual manifest: refused (${m})`);
    m = refusedBy(withManifest(CART, { ...FANTASY, needs: [] }));
    ok(/imports rfm1_host_1/.test(m || ""), "virtual, but fs1 not in needs: refused");
    m = refusedBy(withManifest(CART, { ...FANTASY, needs: ["fs1", "midi9"] }));
    ok(/needs a newer RemoteFM1 \(it needs "midi9"\)/.test(m || ""), `an unknown need: refused (${m})`);
    m = refusedBy(withManifest(PLAIN, { ...MAN, needs: ["fs1"] }));
    ok(/not "virtual"/.test(m || ""), "needs without virtual: refused");
    m = refusedBy(wrongSig);
    ok(/fs_request has the signature \(i32\) -> \(i32\), not \(i32, i32, i32, i64, i32, i32\) -> \(i32\)/.test(m || ""), `a wrong signature: refused (${m})`);
    m = refusedBy(withManifest(rename(CART, "fs_changed", "fs_chang3d"), FANTASY));
    ok(/rfm1_host_1\.fs_chang3d, which this site doesn't know/.test(m || ""), "an rfm1_host_1 function this site doesn't know: refused");
    m = refusedBy(withManifest(rename(CART, "rfm1_host_1", "rfm1_host_2"), FANTASY));
    ok(/imports rfm1_host_2\.\w+, which the format doesn't allow/.test(m || ""), "another host module (rfm1_host_2): refused as today");
    m = refusedBy(withManifest(CART, { ...FANTASY, folders: { data: "w" } }));
    ok(/folders.*"w"/.test(m || ""), "a folder mode other than r / rw: refused");
    m = refusedBy(withManifest(withMemory(PLAIN, 640), MAN));
    ok(/40 MiB: at most 32 MiB/.test(m || ""), "40 MiB of memory, not virtual: refused as today");
    const big = await boots(withManifest(withMemory(PLAIN, 640), { ...MAN, virtual: true }));
    ok(typeof big === "object" && big.info().memoryBytes === 40 * 1048576 && big.info().virtual, "40 MiB, virtual (no files): boots");
    m = refusedBy(withManifest(withMemory(CART, 8208), FANTASY));
    ok(/513 MiB: at most 512 MiB/.test(m || ""), "513 MiB, virtual: refused");
    ok(!refusedBy(withManifest(withMemory(CART, 8192), FANTASY)), "512 MiB, virtual: accepted by inspect");
    const pad = custom("pad", new Uint8Array(16 * 1048576));
    m = refusedBy(Uint8Array.from([...withManifest(CART, FANTASY), ...pad]));
    ok(/16 MiB/.test(m || ""), "a virtual file of 16 MiB: refused");
    ok(!refusedBy(Uint8Array.from([...withManifest(PLAIN, MAN), ...pad])), "the same size, not virtual: accepted (64 MiB as today)");
}

console.log("== the file API on a temporary folder");
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "fs-test-")), root = path.join(tmp, "RemoteFM1");
const put = (p, b) => { fs.mkdirSync(path.dirname(path.join(root, p)), { recursive: true }); fs.writeFileSync(path.join(root, p), b); };
const KICK = Buffer.alloc(3 * 1048576 + 17); for (let i = 0; i < KICK.length; i++) KICK[i] = (i * 7 + (i >> 9)) & 255;
put("audio/kick.wav", KICK);
put("audio/sub/x.wav", "x");
put("audio/.hidden", "h");
put("audio/B.wav", "b"); put("audio/a.wav", "a"); put("audio/Z", "z"); put("audio/é.wav", "e");
put("code/fscart/droplets/a.lua", "print('a')");
put("code/other/secret.lua", "secret");
fs.writeFileSync(path.join(tmp, "outside.txt"), "outside");
fs.symlinkSync(path.join(tmp, "outside.txt"), path.join(root, "code/fscart/escape"));

const serve = dirServer(root);
let pending = [];
async function cart(manifest = FANTASY, withSend = true) {
    let eng;
    eng = await createEngine({ sampleRate: 44100, files: { wasm: withManifest(CART, manifest) }, flash: null,
        options: withSend ? { fsSend: (m) => { pending.push(serve(m).then((r) => eng.fsDone(r))); } } : {} });
    return eng;
}
const L = new Float32Array(128), R = new Float32Array(128);
const settle = async (e) => { while (pending.length) { const p = pending; pending = []; await Promise.all(p); } e.render(L, R); };
const enc = new TextEncoder(), dec = new TextDecoder();
function api(e) {
    const ex = e.ex, u8 = () => e.rt.u8, buf = ex.t_buf(), P = buf + 1536 * 1024;
    ex.t_demo(0);                                                // (the cart's own demo off: these calls alone)
    const call = (op, p, off = 0n, len = 0, at = buf) => { const b = typeof p === "string" ? enc.encode(p) : p; u8().set(b, P); return ex.t_request(op, P, b.length, BigInt(off), at, len); };
    const run = async (op, p, off, len, at) => { const id = call(op, p, off, len, at); if (id <= 0) return { id, r: id }; await settle(e); return { id, r: ex.t_poll(id) }; };
    const bytes = (n, at = buf) => u8().slice(at, at + n);
    const list = async (p, len = 4096, off = 0) => {
        const { r } = await run(O.LIST, p, off, len);
        const out = []; let at = buf;
        for (let i = 0; i < r; i++) { const d = u8()[at], nl = u8()[at + 1]; out.push((d ? "/" : "") + dec.decode(u8().slice(at + 2, at + 2 + nl))); at += 2 + nl; }
        return { r, out };
    };
    return { ex, buf, call, run, bytes, list, u8 };
}

{
    const e = await cart(), a = api(e), dv = () => new DataView(e.rt.buf);
    // STAT
    let x = await a.run(O.STAT, "/audio/kick.wav", 0, 16);
    ok(x.r === 16 && dv().getBigUint64(a.buf, true) === BigInt(KICK.length) && dv().getUint32(a.buf + 12, true) === 0
       && Math.abs(dv().getUint32(a.buf + 8, true) - Date.now() / 1000) < 600, `STAT /audio/kick.wav: 16, size ${KICK.length}, a file, mtime now`);
    x = await a.run(O.STAT, "/audio", 0, 16);
    ok(x.r === 16 && dv().getUint32(a.buf + 12, true) === 1, "STAT /audio: a folder");
    x = await a.run(O.STAT, "/audio/kick.wav", 0, 15);
    ok(x.r === E.EIO, "STAT into 15 bytes: FS_EIO");
    x = await a.run(O.STAT, "/audio/none.wav", 0, 16);
    ok(x.id > 0 && x.r === E.ENOENT, "STAT of a missing file: an id, then FS_ENOENT");

    // completion only between export calls
    const id = a.call(O.READ, "/audio/kick.wav", 1000, 4096);
    for (let i = 0; i < 4; i++) await Promise.all(pending);     // (the answer is back: in the engine's inbox)
    const spun = a.ex.t_spin(id, 100000);
    a.u8().fill(0xee, a.buf, a.buf + 4096);
    e.runMs(0);                                                  // (no export call in between: only apply())
    const after = a.ex.t_poll(id);
    pending = [];
    ok(spun === E.PENDING && after === 4096 && Buffer.from(a.bytes(4096)).equals(KICK.subarray(1000, 5096)),
       `the answer, back before an export call, stays out of the module during it (polled 100000 times: PENDING), then READ 4096 at offset 1000: ${after}, the bytes match`);

    // READ
    x = await a.run(O.READ, "/audio/kick.wav", 0, 2 * 1048576);
    ok(x.r === 1048576 && Buffer.from(a.bytes(1048576)).equals(KICK.subarray(0, 1048576)), "READ of 2 MiB: 1 MiB (the limit per request)");
    x = await a.run(O.READ, "/audio/kick.wav", KICK.length - 10, 100);
    ok(x.r === 10, "READ past the end: the bytes left (10)");
    x = await a.run(O.READ, "/audio/kick.wav", KICK.length, 100);
    ok(x.r === 0, "READ at the end: 0");
    x = await a.run(O.READ, "/audio/kick.wav", 2n ** 40n, 100);
    ok(x.r === 0, "READ at offset 2^40 (an i64): 0");
    ok(a.call(O.READ, "/audio/kick.wav", -5n, 100) === E.EIO && a.call(O.READ, "/audio/kick.wav", 2n ** 60n, 100) === E.EIO, "READ at a negative offset or beyond 2^53: FS_EIO at once");
    ok(a.call(O.READ, "/audio/kick.wav", 0, 64, e.rt.u8.length - 10) === E.EIO, "READ into a buffer past the memory's end: FS_EIO at once");
    x = await a.run(O.READ, "/code/droplets/a.lua", 0, 100);
    ok(x.r === 10 && dec.decode(a.bytes(10)) === "print('a')", "READ /code/droplets/a.lua: its own code/fscart/droplets/a.lua");

    // LIST
    let l = await a.list("/audio");
    ok(l.out.join(" ") === "B.wav Z a.wav kick.wav /sub é.wav", `LIST /audio: sorted by name bytes, folders marked, no hidden files (${l.out.join(" ")})`);
    const pages = [];
    for (let off = 0; ; ) { const p = await a.list("/audio", 12, off); if (p.r <= 0) { pages.push(p.r); break; } pages.push(p.out.join(",")); off += p.r; }
    ok(pages.join(" | ") === "B.wav,Z | a.wav | kick.wav | /sub | é.wav | 0", `LIST in 12-byte pages from entry offset n, never splitting an entry, 0 at the end (${pages.join(" | ")})`);
    l = await a.list("/audio", 4);
    ok(l.r === E.EIO, "LIST into 4 bytes (no room for the first name): FS_EIO");
    l = await a.list("/code");
    ok(l.out.join(" ") === "/droplets escape", `LIST /code: its own code/fscart/ (${l.out.join(" ")})`);
    l = await a.list("/data");
    ok(l.r === 0 && fs.statSync(path.join(root, "data/fscart")).isDirectory(), "LIST /data: empty, data/fscart/ made on demand");

    // WRITE, MKDIR, DELETE
    const w = async (p, s, off) => { a.u8().set(enc.encode(s), a.buf); return (await a.run(O.WRITE, p, off, enc.encode(s).length)).r; };
    const disk = (p) => fs.readFileSync(path.join(root, "data/fscart", p), "utf8");
    ok(await w("/data/save.txt", "hello", -1) === 5 && disk("save.txt") === "hello", "WRITE /data/save.txt at -1: 5, data/fscart/save.txt");
    ok(await w("/data/save.txt", " world", 5) === 6 && disk("save.txt") === "hello world", "WRITE at offset 5: appended");
    ok(await w("/data/save.txt", "HE", 0) === 2 && disk("save.txt") === "HEllo world", "WRITE at offset 0: overwrites, keeps the rest");
    ok(await w("/data/save.txt", "x", -1) === 1 && disk("save.txt") === "x", "WRITE at -1: truncates");
    ok((await a.run(O.MKDIR, "/data/sub")).r === 0 && (await a.run(O.MKDIR, "/data/sub")).r === 0, "MKDIR /data/sub: 0, and again 0");
    ok(await w("/data/sub/f.bin", "f", -1) === 1, "WRITE /data/sub/f.bin");
    { const r = await w("/data/nofolder/f.bin", "f", -1); ok(r === E.ENOENT, `WRITE into a folder that isn't there: FS_ENOENT (${r})`); }
    ok((await a.run(O.DELETE, "/data/sub")).r < 0, `DELETE of a folder with a file in it: refused (${(await a.run(O.DELETE, "/data/sub")).r})`);
    ok((await a.run(O.DELETE, "/data/sub/f.bin")).r === 0 && (await a.run(O.DELETE, "/data/sub")).r === 0 && !fs.existsSync(path.join(root, "data/fscart/sub")), "DELETE the file, then the empty folder: 0, gone");
    ok((await a.run(O.STAT, "/data/sub", 0, 16)).r === E.ENOENT, "STAT of what was deleted: FS_ENOENT");

    // access (section 3) and the sandbox: refused at once, FS_EACCES
    const acc = [[O.WRITE, "/audio/x.wav"], [O.WRITE, "/code/x.lua"], [O.MKDIR, "/audio/new"], [O.DELETE, "/code/droplets/a.lua"], [O.DELETE, "/data"], [O.MKDIR, "/data"],
        [O.READ, "/audio/../code/other/secret.lua"], [O.READ, "/code/../other/secret.lua"], [O.READ, "/code/./droplets/a.lua"], [O.READ, "//audio/kick.wav"],
        [O.READ, "/audio//kick.wav"], [O.LIST, "/audio/"], [O.READ, "audio/kick.wav"], [O.READ, "/etc/passwd"], [O.READ, "/"], [O.READ, "/Audio/kick.wav"],
        [O.READ, "/audio/kick.wav\0"], [O.READ, "/audio\\kick.wav"], [O.READ, Uint8Array.from([0x2f, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x2f, 0xff])], [O.READ, "/audio/" + "x".repeat(1100)]];
    const bad = acc.filter(([op, p]) => a.call(op, p, 0, 16) !== E.EACCES);
    ok(!bad.length, `${acc.length} accesses outside section 3 (writes outside /data, .., ., //, a trailing /, relative, /etc, NUL, \\, bad UTF-8, a 1 KiB path): FS_EACCES at once${bad.length ? "; not: " + bad.map((b) => String(b[1]).slice(0, 40)).join(", ") : ""}`);
    x = await a.run(O.READ, "/code/escape", 0, 16);
    ok(x.id > 0 && x.r === E.EACCES, "a symlink out of the folder (code/fscart/escape): FS_EACCES from the folder");
    ok(a.call(9, "/audio/kick.wav", 0, 16) === E.EIO && a.call(0, "/audio/kick.wav", 0, 16) === E.EIO, "an op that isn't 1-6: FS_EIO");

    // 32 in flight
    const ids = [];
    for (let i = 0; i < 32; i++) ids.push(a.call(O.STAT, "/audio/kick.wav", 0, 16, a.buf + 16 * i));
    const over = a.call(O.STAT, "/audio/kick.wav", 0, 16);
    await settle(e);
    const res = ids.map((i) => a.ex.t_poll(i));
    ok(ids.every((i) => i > 0) && new Set(ids).size === 32 && over === E.EBUSY && res.every((r) => r === 16) && a.call(O.STAT, "/audio", 0, 16) > 0,
       "32 requests in flight; the 33rd: FS_EBUSY; once polled, room again");
    await settle(e); pending = [];

    // cancel
    a.u8().fill(0x55, a.buf, a.buf + 64);
    const c = a.call(O.READ, "/audio/kick.wav", 0, 64);
    a.ex.t_cancel(c);
    await settle(e);
    ok(a.bytes(64).every((v) => v === 0x55) && a.ex.t_poll(c) === E.ECANCELED && a.ex.t_poll(c) === E.ENOENT, "cancel: the buffer untouched, poll FS_ECANCELED once, then the id is free (FS_ENOENT)");
    ok(a.ex.t_poll(123456) === E.ENOENT, "poll of an id never given: FS_ENOENT");

    // fs_changed
    const c0 = a.ex.t_changed();
    e.fsChanged(); e.fsChanged();
    ok(a.ex.t_changed() === c0 + 2, `fs_changed: ${c0} -> ${a.ex.t_changed()} after two folder changes`);
    // the answer for another engine's request (a stale key): ignored
    const s = a.call(O.STAT, "/audio", 0, 16);
    e.fsDone({ type: "fsDone", key: "someone-else", id: s, result: 16, stat: { size: 1, mtime: 0, dir: false } });
    e.render(L, R);
    ok(a.ex.t_poll(s) === E.PENDING, "an answer with another engine's key: ignored");
    await settle(e);
    e.dispose();
}
{
    // folders: what the manifest lists only
    const e = await cart({ ...FANTASY, folders: { audio: "r" } }), a = api(e);
    ok(a.call(O.READ, "/code/droplets/a.lua", 0, 16) === E.EACCES && a.call(O.LIST, "/data", 0, 64) === E.EACCES && a.call(O.READ, "/audio/kick.wav", 0, 16) > 0,
       "a manifest listing only audio: /code and /data refused, /audio read");
    await settle(e);
    const e2 = await cart({ ...FANTASY, folders: { audio: "r", data: "r" } }), a2 = api(e2);
    ok(a2.call(O.WRITE, "/data/x", -1, 1) === E.EACCES && a2.call(O.LIST, "/data", 0, 64) > 0, "data \"r\": no writes, reads fine");
    await settle(e2);
    // no folder (no sender): FS_ENOFOLDER, at the next render
    const e3 = await cart(FANTASY, false), a3 = api(e3);
    const i3 = a3.call(O.READ, "/audio/kick.wav", 0, 16);
    const before = a3.ex.t_poll(i3);
    e3.render(L, R);
    ok(i3 > 0 && before === E.PENDING && a3.ex.t_poll(i3) === E.ENOFOLDER, "no folder: an id, PENDING, then FS_ENOFOLDER after a render");
    e.dispose(); e2.dispose(); e3.dispose();
}

{
    // the cart's demo, in its own ticks: READ /code/hello.txt (none yet: it retries), WRITE "got:..." to /data/out.txt
    const e = await cart();
    const run = async (ms) => { for (let t = 0; t < ms; t += 3) await settle(e); };
    await run(300);
    const red = e.screen().pixels[0];
    put("code/fscart/hello.txt", "hi");
    e.fsChanged();
    await run(300);
    const out = fs.existsSync(path.join(root, "data/fscart/out.txt")) ? fs.readFileSync(path.join(root, "data/fscart/out.txt"), "utf8") : null;
    let pk = 0; e.render(L, R); for (const v of L) pk = Math.max(pk, Math.abs(v));
    ok(red === 0xf800 && out === "got:hi" && e.screen().pixels[0] === 0x07e0 && pk > 0.05,
       `the cart in its own ticks: red without the file, then (fs_changed) reads it and writes data/fscart/out.txt "${out}", green, sound ${pk.toFixed(2)}`);
    e.dispose();
}

console.log("== the flag in the page's entries");
{
    const { rfm1Entry, fantasyCfw } = await import("../../site/js/live.js");
    const { entryOf } = await import("../../site/js/library.js");
    const f = { sha256: "0".repeat(64), size: 1 };
    ok(fantasyCfw(rfm1Entry({ file: "fw/x.rfm1" }, FANTASY, f)) && fantasyCfw(rfm1Entry({ file: "fw/x.rfm1", virtual: true }, MAN, f)) && !fantasyCfw(rfm1Entry({ file: "fw/x.rfm1" }, MAN, f)),
       "live.js rfm1Entry: virtual from the manifest or fw-catalog.json's record (build_catalog.mjs); not otherwise");
    ok(fantasyCfw(entryOf({ sha256: f.sha256, fileName: "x.rfm1", size: 1, manifest: FANTASY })) && !fantasyCfw(entryOf({ sha256: f.sha256, fileName: "x.rfm1", size: 1, manifest: MAN })),
       "library.js entryOf: virtual from the visitor's file's manifest");
}

fs.renameSync(tmp, tmp + "-done");   // (left for a look; the OS clears tmp)
console.log(fails ? `\n${fails} of ${n} checks failed` : `\nall ${n} checks passed`);
process.exit(fails ? 1 : 0);
