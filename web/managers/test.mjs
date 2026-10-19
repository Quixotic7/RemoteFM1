// SPDX-License-Identifier: GPL-3.0-only
// The hosted managers' MIDI path in Node: fm1-shim.js (the fake MIDIAccess a manager page gets) against the player's
// ManagerBridge (web/site/js/managers.js) and a stub engine that answers like a firmware, both ways:
//   - in the Editor panel (an iframe: window.postMessage, same origin), and popped out (a BroadcastChannel, two player
//     tabs, one of them running a native firmware);
//   - the port's name and state, onmidimessage / addEventListener, send(data, timestamp), open() / close(), statechange
//     on a firmware switch or the emulated engine; whole messages in (SysEx split across chunks, clock dropped);
//   - no echo: nothing a manager sends comes back to a manager.
//   node web/managers/test.mjs
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { ManagerBridge, MidiAssembler, editorOf, portOf } from "../site/js/managers.js";

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
  bridge.setTarget({ port: "Melodee", firmware: "Melodee 1.4.4", engine: "native" });
  await sleep(20);
  ok(events.join() === "Felucca/input/disconnected,Felucca/output/disconnected,Melodee/input/connected,Melodee/output/connected",
     "switch: statechange disconnected (old), connected (new)");
  const ins2 = [...access.inputs.values()];
  ok(ins2.length === 1 && ins2[0].name === "Melodee" && ins2[0].id !== ins[0].id && ins[0].state === "disconnected", "switch: the maps hold the new port only, a new id");
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

console.log(`\n${checks - fails} / ${checks} checks passed`);
process.exit(fails ? 1 : 0);
