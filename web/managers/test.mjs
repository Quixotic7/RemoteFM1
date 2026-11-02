// SPDX-License-Identifier: GPL-3.0-only
// The hosted managers' MIDI path in Node: fm1-shim.js (the fake MIDIAccess a manager page gets) against the player's
// ManagerBridge (web/site/js/managers.js) and a stub engine that answers like a firmware, both ways:
//   - in the Editor panel (an iframe: window.postMessage, same origin), and popped out (a BroadcastChannel, two player
//     tabs, one of them running a native firmware);
//   - the port's name and state, onmidimessage / addEventListener, send(data, timestamp), open() / close(), statechange
//     on a firmware switch or the emulated engine; whole messages in (SysEx split across chunks, clock dropped);
//   - no echo: nothing a manager sends comes back to a manager;
//   - an author's live page (live-editor.js): the shim's sandbox transport (an opaque origin: postMessage "*" to the
//     parent, only the parent heard), its storage stand-ins kept by the player (EditorStore), history kept to the
//     srcdoc; the bridge hears origin "null" only from the live frame it was given; liveUrlOf, injectLive, the pop-out token.
//   node web/managers/test.mjs
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { ManagerBridge, MidiAssembler, editorOf, portOf, liveUrlOf } from "../site/js/managers.js";
import { injectLive, EditorStore, stashPop, takePop } from "../site/js/live-editor.js";

let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(84)} ${c ? "ok" : "FAIL"}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const eq = (a, b) => a && b && a.length === b.length && Array.from(a).every((v, i) => v === b[i]);
const SHIM = readFileSync(new URL("./fm1-shim.js", import.meta.url), "utf8");
const ORIGIN = "https://quixotic7.github.io";

// a message event as a browser makes it (data cloned; origin and source set)
function msgEvent(data, origin, source) {
  const e = new Event("message");
  Object.defineProperties(e, { data: { value: structuredClone(data) }, origin: { value: origin }, source: { value: source } });
  return e;
}
// a window: an EventTarget with what the shim and the bridge use
function fakeWindow(href) {
  const w = new EventTarget();
  Object.assign(w, { location: new URL(href), navigator: {}, performance, setTimeout, clearTimeout, URL, Event, EventTarget, MessageEvent, DOMException,
                     Uint8Array, Map, Set, Promise, Math, Date, Object, Number, TypeError, Error, structuredClone, console, open: () => null });
  w.document = { currentScript: { dataset: { manager: "felucca", upstream: "https://hugelton.github.io/Felucca/webapp/editor/" } }, addEventListener() {} };
  return w;
}
function runShim(win) {
  const ctx = vm.createContext(win);
  ctx.window = win;
  vm.runInContext(SHIM, ctx);
  return win;
}
// the player page and the manager's iframe in it
function framed(bridgeOpts = {}) {
  const player = fakeWindow(ORIGIN + "/RemoteFM1/?fw=felucca");
  const frame = fakeWindow(ORIGIN + "/RemoteFM1/managers/felucca/");
  frame.parent = player;
  // window.postMessage, as the browser does it: asynchronous, cloned, with the sender as source
  const frameProxy = { postMessage: (data, origin) => { if (origin === ORIGIN) setTimeout(() => frame.dispatchEvent(msgEvent(data, ORIGIN, player)), 0); } };
  player.postMessage = (data, origin) => { if (origin === ORIGIN) setTimeout(() => player.dispatchEvent(msgEvent(data, ORIGIN, frameProxy)), 0); };
  const engine = stubEngine();
  const bridge = new ManagerBridge({ toEngine: (b) => engine.in(b), win: player, BC: null, ...bridgeOpts });
  engine.out = (b) => bridge.fromEngine(b);
  return { player, frame, bridge, engine };
}
// a firmware's SysEx service, answered in odd chunks with a clock byte and a note inside (the engine's midiOut)
const INFO = [0xf0, 0x7d, 0x46, 0x4c, 0x01, 0xf7];
const INFO_REPLY = [0xf0, 0x7d, 0x46, 0x4c, 0x01, ...Array.from("Felucca 1.6.1", (c) => c.charCodeAt(0)), 0, 27, 91, 0xf7];
function stubEngine() {
  const e = { got: [], out: null };
  e.in = (b) => {
    e.got.push(Array.from(b));
    if (eq(b, INFO)) setTimeout(() => { const r = [0xf8, ...INFO_REPLY.slice(0, 7), 0xf8], s = [...INFO_REPLY.slice(7), 0x90, 60, 100, 0xf8]; e.out(Uint8Array.from(r)); e.out(Uint8Array.from(s)); }, 2);
    else if (b[0] === 0xf0 && b[4] === 66) setTimeout(() => e.out(Uint8Array.from([0xf0, 0x7d, 0x46, 0x4c, 66, ...Array.from({ length: 300 }, (_, i) => i & 0x7f), 0xf7])), 2);
  };
  return e;
}

// ---- the assembler
{
  const got = [], a = new MidiAssembler();
  a.push([0xf0, 1, 2], (m) => got.push(Array.from(m)));
  a.push([0xf8, 3, 0xf7, 0x90, 60, 100, 61, 0, 0xc0, 5, 0xfe, 0xf0, 9, 0x80, 1, 2], (m) => got.push(Array.from(m)));
  ok(JSON.stringify(got) === JSON.stringify([[0xf0, 1, 2, 3, 0xf7], [0x90, 60, 100], [0x90, 61, 0], [0xc0, 5], [0x80, 1, 2]]),
     "assembler: SysEx across chunks whole, running status expanded, real time dropped, a cut SysEx dropped");
}

// ---- in the Editor panel (iframe)
{
  const { frame, bridge, engine } = framed();
  runShim(frame);
  ok(typeof frame.navigator.requestMIDIAccess === "function", "shim: navigator.requestMIDIAccess replaced");
  bridge.setTarget({ port: "Felucca", firmware: "Felucca 1.6.1", engine: "native" });
  const access = await frame.navigator.requestMIDIAccess({ sysex: true });
  const ins = [...access.inputs.values()], outs = [...access.outputs.values()];
  ok(access.sysexEnabled === true && ins.length === 1 && outs.length === 1, "iframe: one input, one output, sysexEnabled");
  ok(ins[0].name === "Felucca" && outs[0].name === "Felucca" && ins[0].type === "input" && outs[0].type === "output"
     && ins[0].state === "connected" && outs[0].state === "connected" && ins[0].connection === "closed", "iframe: named as the firmware's USB port, connected, closed until used");
  ok(frame.__fm1shim.framed && bridge.count === 1 && [...bridge.sessions.values()][0].via === "frame", "iframe: the bridge has the session (postMessage)");
  const heard = [], heard2 = [];
  ins[0].onmidimessage = (e) => heard.push(Array.from(e.data));
  ins[0].addEventListener("midimessage", (e) => heard2.push(e.data));
  ok(ins[0].connection === "open", "iframe: setting onmidimessage opens the input");
  outs[0].send(INFO);
  await sleep(40);
  ok(eq(engine.got[0], INFO) && engine.got.length === 1, "iframe: the manager's INFO reaches the engine, once, intact");
  ok(heard.length === 2 && eq(heard[0], INFO_REPLY) && eq(heard[1], [0x90, 60, 100]), "iframe: the reply comes back whole (split chunks, clock dropped), then the note");
  ok(heard2.length === 2 && heard2[0] instanceof Uint8Array, "iframe: addEventListener('midimessage') hears the same, data a Uint8Array");
  ok(!heard.some((m) => eq(m, INFO)), "iframe: no echo of the request");
  // a GET of 300 bytes, sent with a timestamp 30 ms ahead
  const t0 = performance.now();
  outs[0].send([0xf0, 0x7d, 0x46, 0x4c, 66, 1, 0, 0, 0, 0, 0, 0, 2, 0xf7], performance.now() + 30);
  await sleep(10);
  ok(engine.got.length === 1, "iframe: send(data, timestamp) waits for its time");
  await sleep(60);
  ok(engine.got.length === 2 && heard.length === 3 && heard[2].length === 306 && heard[2][4] === 66, `iframe: a 306-byte reply arrives as one message (${Math.round(performance.now() - t0)} ms)`);
  let threw = null;
  try { outs[0].send([256]); } catch (e) { threw = e; }
  ok(threw instanceof TypeError, "iframe: send() refuses a value above 255 (TypeError)");
  // the firmware switches: the old port goes, the new one comes (new ids), as on USB
  const events = [];
  access.onstatechange = (e) => events.push(`${e.port.name}/${e.port.type}/${e.port.state}`);
  bridge.setTarget({ port: "Melodee FM-1", firmware: "Melodee 1.4.5", engine: "native" });
  await sleep(20);
  ok(events.join() === "Felucca/input/disconnected,Felucca/output/disconnected,Melodee FM-1/input/connected,Melodee FM-1/output/connected",
     "switch: statechange disconnected (old), connected (new)");
  const ins2 = [...access.inputs.values()];
  ok(ins2.length === 1 && ins2[0].name === "Melodee FM-1" && ins2[0].id !== ins[0].id && ins[0].state === "disconnected", "switch: the maps hold the new port only, a new id");
  threw = null;
  try { outs[0].send(INFO); } catch (e) { threw = e; }
  ok(threw && threw.name === "InvalidStateError", "switch: sending on the old port throws InvalidStateError");
  // the emulated engine (or loading): no port; bytes sent meanwhile reach nothing
  bridge.setTarget({ port: null });
  await sleep(20);
  ok(access.inputs.size === 0 && access.outputs.size === 0, "emulated / loading: no port at all");
  const before = engine.got.length;
  bridge.receive({ __fm1mgr: 1, sid: frame.__fm1shim.sid, type: "midi", bytes: Uint8Array.from(INFO) }, "frame", () => {});
  ok(engine.got.length === before, "emulated / loading: the bridge passes nothing to the engine");
  // a message from another origin is ignored
  const n = bridge.count;
  bridge.win.dispatchEvent(msgEvent({ __fm1mgr: 1, sid: "evil", type: "hello" }, "https://example.com", { postMessage() {} }));
  ok(bridge.count === n, "iframe: a message from another origin is ignored");
  bridge.dispose();
}

// ---- popped out: a tab, the BroadcastChannel, two player tabs (one emulated, one native)
{
  const emuPlayer = new ManagerBridge({ toEngine: () => { throw new Error("the emulated tab got bytes"); }, win: null, BC: BroadcastChannel, pid: "emu-tab" });
  emuPlayer.setTarget({ port: null });
  const engine = stubEngine();
  const nativePlayer = new ManagerBridge({ toEngine: (b) => engine.in(b), win: null, BC: BroadcastChannel, pid: "native-tab" });
  engine.out = (b) => nativePlayer.fromEngine(b);
  nativePlayer.setTarget({ port: "JIANT FM", firmware: "JIANT FM 0.8.5", engine: "native" });
  const tab = fakeWindow(ORIGIN + "/RemoteFM1/managers/jiant/");
  tab.parent = tab;
  tab.BroadcastChannel = BroadcastChannel;
  runShim(tab);
  const access = await tab.navigator.requestMIDIAccess({ sysex: true });
  ok(!tab.__fm1shim.framed && tab.__fm1shim.pid === "native-tab", "tab: bound to the player tab that runs a native firmware");
  const inp = [...access.inputs.values()][0], out = [...access.outputs.values()][0];
  ok(inp && out && inp.name === "JIANT FM" && out.name === "JIANT FM", "tab: the port is named as JIANT's USB port");
  const heard = [];
  inp.onmidimessage = (e) => heard.push(Array.from(e.data));
  out.send(INFO);
  await sleep(60);
  ok(engine.got.length === 1 && heard.length === 2 && eq(heard[0], INFO_REPLY), "tab: INFO there and back over the BroadcastChannel");
  ok(!heard.some((m) => eq(m, INFO)) && emuPlayer.stats.toEngine === 0, "tab: no echo; the other player tab gets nothing");
  // the player tab goes away: the port goes
  nativePlayer.dispose();
  await sleep(40);
  ok(access.inputs.size === 0 && inp.state === "disconnected" && tab.__fm1shim.pid === null, "tab: the player tab closed: the port is gone");
  emuPlayer.dispose();
  tab.dispatchEvent(new Event("pagehide"));
}

// ---- which editor a firmware has
{
  const index = { managers: [{ id: "felucca", firmwares: ["felucca"], path: "felucca/" }], ports: { felucca: "Felucca", ghoulbox: "Felucca" } };
  ok(editorOf({ id: "felucca" }, index).kind === "hosted" && editorOf({ id: "sloop", editor: "https://x/" }, index).kind === "real"
     && editorOf({ id: "amb" }, index) === null && portOf({ id: "ghoulbox" }, index) === "Felucca" && portOf({ id: "sloop" }, index) === null,
     "editorOf / portOf: hosted, on its site (a real FM-1), none; the port names");
}

// ---- an author's live page: the sandbox transport (srcdoc, an opaque origin: messages come with origin "null")
// a Storage as the browser has it (the player's localStorage)
function fakeStorage() {
  const m = new Map();
  return { get length() { return m.size; }, key: (i) => [...m.keys()][i] ?? null, getItem: (k) => (m.has(k) ? m.get(k) : null),
           setItem: (k, v) => m.set(k, String(v)), removeItem: (k) => m.delete(k), _m: m };
}
{
  const ls = fakeStorage();
  const URL1 = "https://hugelton.github.io/Felucca/webapp/editor/";
  const store = new EditorStore(URL1, ls);
  ls.setItem(store.prefix + "felucca-tab", "seq");
  ls.setItem("remotefm1-ed:https://other.example/|felucca-tab", "other");
  const player = fakeWindow(ORIGIN + "/RemoteFM1/?fw=felucca");
  const frame = fakeWindow("about:srcdoc");
  frame.document.currentScript.dataset = { manager: "felucca", transport: "sandbox", storage: JSON.stringify(store.seed()) };
  frame.parent = player;
  // history as in a srcdoc with <base>: a URL that resolves to the author's site is another origin than about:srcdoc
  frame.history = { url: null, replaceState(st, title, url) {
    if (!String(url).startsWith("about:srcdoc")) throw new DOMException("cannot be created in a document with origin 'null'", "SecurityError");
    this.url = url; } };
  const sent = [];
  // the browser: the frame posts with target "*" (an opaque origin has no name); the player's replies arrive from it
  const frameProxy = { postMessage: (data, origin) => { sent.push(origin); setTimeout(() => frame.dispatchEvent(msgEvent(data, ORIGIN, player)), 0); } };
  player.postMessage = (data, origin) => { if (origin === "*") setTimeout(() => player.dispatchEvent(msgEvent(data, "null", frameProxy)), 0); };
  const engine = stubEngine();
  const stored = [];
  const bridge = new ManagerBridge({ toEngine: (b) => engine.in(b), win: player, BC: null, onStorage: (m, w) => { stored.push(w); store.apply(m); } });
  engine.out = (b) => bridge.fromEngine(b);
  bridge.setTarget({ port: "Felucca", firmware: "Felucca 1.8.1", engine: "rfm1" });
  // not yet allowed: the bridge does not hear the sandbox (origin "null" from a window it was not given)
  runShim(frame);
  await sleep(20);
  ok(bridge.count === 0 && frame.__fm1shim.transport === "sandbox" && frame.__fm1shim.framed, "sandbox: origin \"null\" from a window the panel did not name: not heard");
  bridge.allowFrame(frameProxy);
  // (the shim said hello at load, unheard; in the panel the frame is allowed before its page loads: its hello, again)
  bridge.win.dispatchEvent(msgEvent({ __fm1mgr: 1, sid: frame.__fm1shim.sid, manager: "felucca", type: "hello" }, "null", frameProxy));
  const access = await frame.navigator.requestMIDIAccess({ sysex: true });
  await sleep(20);
  const inp = [...access.inputs.values()][0], out = [...access.outputs.values()][0];
  ok(bridge.hasFrame(frameProxy) && inp && inp.name === "Felucca" && frame.__fm1shim.pid === bridge.pid, "sandbox: allowed, the live page's shim binds to the player and gets the port");
  const heard = [];
  inp.onmidimessage = (e) => heard.push(Array.from(e.data));
  out.send(INFO);
  await sleep(60);
  ok(engine.got.length === 1 && heard.length === 2 && eq(heard[0], INFO_REPLY) && sent.length > 0 && sent.every((o) => o === "*"),
     "sandbox: INFO there and back; the replies go to that window with target \"*\"");
  // another window says it is the parent: the shim hears only its parent
  frame.dispatchEvent(msgEvent({ __fm1mgr: 1, from: "player", pid: bridge.pid, sid: frame.__fm1shim.sid, type: "midi", bytes: Uint8Array.of(0x90, 1, 2) }, ORIGIN, { postMessage() {} }));
  await sleep(10);
  ok(heard.length === 2, "sandbox: a message from a window other than the parent is not heard");
  // storage: the stand-in, seeded with this editor's keys only, writes kept by the player under its prefix
  ok(frame.localStorage.getItem("felucca-tab") === "seq" && frame.localStorage.length === 1 && frame.indexedDB === undefined && typeof frame.sessionStorage.setItem === "function",
     "sandbox: localStorage seeded with this editor's keys only; indexedDB taken away; a sessionStorage");
  frame.localStorage.setItem("felucca-tab", "mix");
  frame.localStorage.setItem("big", "x".repeat(70000));
  await sleep(20);
  ok(ls.getItem(store.prefix + "felucca-tab") === "mix" && ls.getItem(store.prefix + "big") === null && frame.localStorage.getItem("big").length === 70000
     && ls.getItem("remotefm1-ed:https://other.example/|felucca-tab") === "other" && stored.every((w) => w === frameProxy),
     "sandbox: a write reaches the player's storage under the editor's prefix; an oversized one stays in the page");
  // a storage message from a window not allowed: dropped
  bridge.win.dispatchEvent(msgEvent({ __fm1mgr: 1, sid: "x", type: "storage", op: "set", key: "felucca-tab", value: "evil" }, "null", { postMessage() {} }));
  await sleep(10);
  ok(ls.getItem(store.prefix + "felucca-tab") === "mix", "sandbox: a storage write from another opaque window: dropped");
  frame.history.replaceState(null, "", "srcdoc#seq");
  ok(frame.history.url === "about:srcdoc#seq", "sandbox: history.replaceState kept to the srcdoc's URL (the fragment)");
  // the panel drops the frame: the window is no longer heard
  bridge.disallowFrame(frameProxy);
  const n = engine.got.length;
  bridge.win.dispatchEvent(msgEvent({ __fm1mgr: 1, sid: frame.__fm1shim.sid, type: "midi", bytes: Uint8Array.from(INFO) }, "null", frameProxy));
  await sleep(20);
  ok(bridge.count === 0 && engine.got.length === n, "sandbox: disallowed (the panel closed): its session gone, its bytes reach nothing");
  bridge.dispose();
}

// ---- the live page's address, the srcdoc, the pop-out token
{
  const up = "https://hugelton.github.io/Felucca/webapp/editor/";
  ok(liveUrlOf({ id: "felucca", editor: "https://x.example/e/" }, { upstream: up }) === up
     && liveUrlOf({ id: "ghoulbox" }, { upstream: "https://a.example/e/", live: "https://b.example/e/" }) === "https://b.example/e/"
     && liveUrlOf({ id: "pigeon", editor: "https://x.example/" }, { upstream: "https://a.example/", live: false }) === null
     && liveUrlOf({ id: "rainbow", editor: "https://r.example/webapp/editor/" }, null) === "https://r.example/webapp/editor/"
     && liveUrlOf({ id: "mutable", editor: "https://m.example/#editor" }, null) === null
     && liveUrlOf({ id: "x", editor: "http://x.example/" }, null) === null && liveUrlOf({ id: "y" }, null) === null,
     "liveUrlOf: managers.json live, its upstream, the catalog's editor; never false, a fragment, plain http");
  const index = { managers: [{ id: "felucca", firmwares: ["felucca"], path: "felucca/", upstream: up }], ports: {} };
  ok(editorOf({ id: "felucca" }, index).live === up && editorOf({ id: "rainbow", editor: "https://r.example/e/" }, index).live === "https://r.example/e/",
     "editorOf: the live page with the hosted manager, and with a catalog-only editor");
  const ports = { ports: { jiant: "JIANT FM", karma: "Felucca", felucca: "Felucca", hortator: "Felucca" }, managers: [{ id: "jiant", firmwares: ["jiant"] }] };
  ok(portOf({ id: "jiant", port: "FM-1" }, ports) === "JIANT FM" && portOf({ id: "hortator", port: "Hortator" }, ports) === "Hortator"
     && portOf({ id: "caravel", port: "Caravel" }, ports) === "Caravel" && portOf({ id: "felucca" }, ports) === "Felucca"
     && portOf({ id: "caravel", port: "Caravel" }, null) === "Caravel" && portOf({ id: "nope" }, ports) === null,
     "portOf: a hosted manager's tested port first, else the catalog's port, either where the other has none");
  ok(liveUrlOf({ id: "skiff", editor: "https://s.example/", liveEditor: "https://s.example/webapp/editor/" }, null) === "https://s.example/webapp/editor/",
     "liveUrlOf: the catalog's liveEditor over its editor");
  const html = `<!doctype html><html><head><meta charset="utf-8"><script>window.page = 1</script></head><body></body></html>`;
  const doc = injectLive(html, { base: up, shim: "/* shim */ if (a</script>) {}", manager: "felucca", storage: { k: `"<v>"` } });
  const i0 = doc.indexOf("<head>") + 6;
  ok(doc.slice(i0).startsWith(`<base href="${up}"><script data-manager="felucca" data-transport="sandbox" data-storage="{&quot;k&quot;:&quot;\\&quot;&lt;v&gt;\\&quot;&quot;}">`)
     && doc.indexOf("<\\/script>) {}") > 0 && doc.indexOf("window.page") > doc.indexOf("data-transport"),
     "injectLive: <base> and the shim first in <head>, attributes escaped, </script in the shim broken");
  ok(injectLive(`<!doctype html><meta charset="utf-8"><p>x`, { base: up, shim: "" }).startsWith(`<!doctype html><base href=`), "injectLive: a page without <head>: after the doctype");
  const ls = fakeStorage();
  const tok = stashPop({ url: up, manager: "felucca", title: "Felucca editor" }, ls);
  const got = takePop(tok, ls);
  ok(tok && got && got.url === up && takePop(tok, ls) === null && takePop("../evil", ls) === null, "pop out: the token hands the page over once; nothing else opens");
}

// ---- build.mjs: a manager from a GitHub repository at a pinned commit (managers.json "git"): its page, files and
// licence files fetched from --git-raw (here a local server standing in for raw.githubusercontent.com), the shim's
// data-local, "open" in index.json's path, NOTICE.txt; an unreachable source is a warning and the build goes on
{
  const { mkdtempSync, writeFileSync, readFileSync: rd, existsSync } = await import("node:fs");
  const { tmpdir } = await import("node:os");
  const { join } = await import("node:path");
  const { execFile } = await import("node:child_process");
  const http = await import("node:http");
  const C = "0123456789abcdef0123456789abcdef01234567";
  const tree = { [`/own/repo/${C}/site/Page.html`]: `<!doctype html><html><meta charset="utf-8"><title>p</title><a href="Install.html">i</a>`,
                 [`/own/repo/${C}/site/app.js`]: "app()", [`/own/repo/${C}/LICENSE`]: "GPL text", [`/own/repo/${C}/LICENSE-MIT.txt`]: "MIT text" };
  const asked = [];
  const srv = http.createServer((q, r) => { asked.push(q.url); const b = tree[q.url]; r.writeHead(b ? 200 : 404); r.end(b || ""); });
  await new Promise((res) => srv.listen(0, "127.0.0.1", res));
  const tmp = mkdtempSync(join(tmpdir(), "mgr-git-")), spec = join(tmp, "managers.json");
  const entry = (id, more = {}) => ({ id, title: "T", firmwares: [id], git: { repo: "https://github.com/own/repo", commit: C }, page: "site/Page.html",
    files: ["site/app.js"], licenceFiles: ["LICENSE", "LICENSE-MIT.txt", "LICENSING.md"], licence: "GPL-3.0-only", author: "A", repo: "https://github.com/own/repo",
    upstream: "https://own.example/", live: false, ...more });
  writeFileSync(spec, JSON.stringify({ ports: {}, managers: [entry("gm", { open: "#ed" }), entry("bad", { git: { repo: "https://github.com/own/repo", commit: "main" } })] }));
  const build = (raw, out) => new Promise((res) => execFile(process.execPath, [new URL("./build.mjs", import.meta.url).pathname, "--spec", spec, "--out", out,
    "--git-cache", join(out, "cache"), "--git-raw", raw], (e, so, se) => res({ code: e ? e.code : 0, log: so + se })));
  const r1 = await build(`http://127.0.0.1:${srv.address().port}`, join(tmp, "a"));
  srv.close();
  const dir = join(tmp, "a", "gm"), idx = JSON.parse(rd(join(tmp, "a", "index.json"), "utf8")), m = idx.managers.find((x) => x.id === "gm");
  const page = existsSync(join(dir, "index.html")) ? rd(join(dir, "index.html"), "utf8") : "";
  ok(r1.code === 0 && m && m.path === "gm/#ed" && m.commit === C && m.git === "https://github.com/own/repo" && m.source === `https://github.com/own/repo/tree/${C}`
     && page.includes(`<meta charset="utf-8">\n<script src="../fm1-shim.js" data-manager="gm" data-upstream="https://own.example/" data-local="index.html app.js LICENSE LICENSE-MIT.txt NOTICE.txt"></script>`)
     && rd(join(dir, "app.js"), "utf8") === "app()" && rd(join(dir, "LICENSE-MIT.txt"), "utf8") === "MIT text" && !existsSync(join(dir, "LICENSING.md"))
     && /Source: https:\/\/github.com\/own\/repo\/tree\/0123456/.test(rd(join(dir, "NOTICE.txt"), "utf8")) && asked.every((u) => u.includes(`/own/repo/${C}/`)),
     "build.mjs git: page, files and licences fetched at the commit, shim with data-local, open fragment in the path, NOTICE");
  ok(!idx.managers.some((x) => x.id === "bad") && /bad: its source .*not a GitHub repository and a full commit.*skipped/.test(r1.log), "build.mjs git: a commit that is not 40 hex: skipped, a warning");
  const r2 = await build("http://127.0.0.1:1", join(tmp, "b"));
  ok(r2.code === 0 && JSON.parse(rd(join(tmp, "b", "index.json"), "utf8")).managers.length === 0 && /gm: its source https:\/\/github.com\/own\/repo@0123456 could not be fetched/.test(r2.log),
     "build.mjs git: an unreachable source is a warning; the build goes on (exit 0)");
}

console.log(`\n${checks - fails} / ${checks} checks passed`);
process.exit(fails ? 1 : 0);
