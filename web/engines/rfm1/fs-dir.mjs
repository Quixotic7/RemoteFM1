// SPDX-License-Identifier: GPL-3.0-only
// A Fantasy CFW's RemoteFM1 folder on a directory (Node): the page's js/folder.js for tests and tools. It answers the
// engine's file requests (engine.js FileApi: { type: "fs", key, id, op, path, offset, len, data? }) as the page does,
// on a real directory (audio/, code/<id>/, data/<id>/, created on demand); a path that leaves the directory (a
// symlink out of it) is refused.
//
//   const serve = dirServer(root);
//   let eng; eng = await createEngine({ ..., options: { fsSend: (m) => serve(m).then((r) => eng.fsDone(r)) } });
import fs from "node:fs/promises";
import path from "node:path";

const E = { ENOENT: -2, EACCES: -3, EIO: -4, ENOSPC: -5 };
const ROOTS = ["audio", "code", "data"];

// a folder path as the engine sends it ("code/<id>/droplets/x.lua") -> its segments, or null (the page checks again
// what the engine checked: nothing outside audio/, code/, data/; writes only in data/)
export function folderSegments(p, write) {
    if (typeof p !== "string" || !p) return null;
    const seg = p.split("/");
    if (!ROOTS.includes(seg[0]) || (seg[0] !== "audio" && seg.length < 2)) return null;
    if (seg.some((x) => !x || x === "." || x === ".." || x.includes("\\") || x.includes("\0"))) return null;
    if (write && (seg[0] !== "data" || seg.length < 3)) return null;
    return seg;
}

const errOf = (e) => (e && (e.code === "ENOENT" || e.code === "ENOTDIR") ? E.ENOENT : e && (e.code === "EACCES" || e.code === "EPERM") ? E.EACCES
    : e && (e.code === "ENOSPC" || e.code === "EDQUOT") ? E.ENOSPC : E.EIO);

export function dirServer(root) {
    const top = path.resolve(root);
    let real = null;
    const inside = async (p) => {                     // (a symlink out of the folder: refused)
        real = real || await fs.realpath(top);
        let r;
        // (not there yet: its folder must be; a missing folder is the op's FS_ENOENT)
        try { r = await fs.realpath(p); } catch (e) { if (e.code !== "ENOENT") throw e; r = path.join(await fs.realpath(path.dirname(p)), path.basename(p)); }
        return r === real || r.startsWith(real + path.sep);
    };
    return async function serve(m) {
        const reply = { type: "fsDone", key: m.key, id: m.id, result: E.EIO };
        const write = m.op === 4 || m.op === 5 || m.op === 6;
        const seg = folderSegments(m.path, write);
        if (!seg) { reply.result = E.EACCES; return reply; }
        try {
            // the folder's own roots, on demand: audio/, code/<id>/, data/<id>/
            await fs.mkdir(path.join(top, ...seg.slice(0, seg[0] === "audio" ? 1 : 2)), { recursive: true });
            const p = path.join(top, ...seg);
            if (!(await inside(p))) { reply.result = E.EACCES; return reply; }
            if (m.op === 1) {                                         // STAT
                const st = await fs.stat(p);
                reply.stat = { size: st.isDirectory() ? 0 : st.size, mtime: st.mtimeMs, dir: st.isDirectory() };
                reply.result = 16;
            } else if (m.op === 2) {                                  // LIST
                const ents = await fs.readdir(p, { withFileTypes: true });
                reply.entries = [];
                for (const d of ents) {
                    let dir = d.isDirectory();
                    if (d.isSymbolicLink()) { try { dir = (await fs.stat(path.join(p, d.name))).isDirectory(); } catch { continue; } }
                    reply.entries.push([d.name, dir]);
                }
                reply.result = 0;
            } else if (m.op === 3) {                                  // READ
                const h = await fs.open(p, "r");
                try {
                    const b = Buffer.alloc(m.len);
                    const { bytesRead } = await h.read(b, 0, m.len, m.offset);
                    reply.data = b.buffer.slice(b.byteOffset, b.byteOffset + bytesRead);
                    reply.result = bytesRead;
                } finally { await h.close(); }
            } else if (m.op === 4) {                                  // WRITE (offset -1: truncate and write)
                const data = new Uint8Array(m.data || new ArrayBuffer(0));
                if (m.offset < 0) await fs.writeFile(p, data);
                else {
                    const h = await fs.open(p, await fs.stat(p).then(() => "r+", () => "w"));
                    try { await h.write(data, 0, data.length, m.offset); } finally { await h.close(); }
                }
                reply.result = data.length;
            } else if (m.op === 5) {                                  // DELETE (a file or an empty folder)
                const st = await fs.lstat(p);
                if (st.isDirectory()) await fs.rmdir(p); else await fs.unlink(p);
                reply.result = 0;
            } else if (m.op === 6) {                                  // MKDIR (there already: fine)
                try { await fs.mkdir(p); } catch (e) { if (e.code !== "EEXIST" || !(await fs.stat(p)).isDirectory()) throw e; }
                reply.result = 0;
            }
        } catch (e) { reply.result = errOf(e); }
        return reply;
    };
}
