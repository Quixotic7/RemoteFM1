// SPDX-License-Identifier: GPL-3.0-only
// The hosted managers end to end in headless Chromium (no window, a fresh profile per run, ended by its PID; no real
// Web MIDI: the managers only ever see the shim). Per manager:
//   engine  the player with the firmware's native core, the Editor panel open, the manager's own connect (Connect /
//           its auto-connect): its INFO must reach the engine; with a core that answers (FM1VST sysex-glue), INFO, a
//           backup GET (Felucca editors: Save backup; ChoralRoot: the device read) and a put + get-back (Felucca
//           editors: Restore backup, then Save backup again: the same archive; ChoralRoot: Place a pack sound, read back)
//   double  the same through the shim and the bridge, with the manager's own simulator standing in for the core
//           (Felucca editors: their makeMockDevice; ChoralRoot: fm1manager.js makeSimFM1): the whole UI path, today
// Screenshots: web/build/test/editor-<id>-<mode>.png (and SHOTS_DIR when set).
//   node web/managers/e2e.mjs [id ...] [--mode engine|double|notes|both]     (after node web/managers/build.mjs; the native
//   cores in web/build/native: web/engines/native/build.sh)
// Exit 1 when a check fails. "engine" without an answering core reports "no reply" (expected until sysex-glue lands)
// and fails only if the manager's bytes did not reach the engine.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, existsSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import net from "node:net";

const HERE = dirname(fileURLToPath(import.meta.url)), WEB = join(HERE, "..");
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const OUT = join(WEB, "build", "test"), SHOTS = process.env.SHOTS_DIR || null;
const argv = process.argv.slice(2), mi = argv.indexOf("--mode");
const MODE = mi >= 0 ? argv[mi + 1] : "both";
const IDS = argv.filter((a, i) => !a.startsWith("--") && i !== mi + 1);
const INDEX = JSON.parse(readFileSync(join(WEB, "build", "managers", "index.json"), "utf8"));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
mkdirSync(OUT, { recursive: true });
if (SHOTS) mkdirSync(SHOTS, { recursive: true });

// ---- a static server for web/ (python3 -m http.server), ended by its PID
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
const PORT = await freePort();
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1"], { cwd: WEB, stdio: "ignore" });
const BASE = `http://127.0.0.1:${PORT}/`;
for (let i = 0; i < 50; i++) { try { await fetch(BASE + "site/index.html"); break; } catch (_) { await sleep(100); } }

class Cdp {
  constructor(ws) { this.ws = ws; this.id = 0; this.pending = new Map(); this.handlers = []; }
  static async connect(url) {
    const ws = new WebSocket(url);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    const c = new Cdp(ws);
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id && c.pending.has(m.id)) { const { res, rej } = c.pending.get(m.id); c.pending.delete(m.id); m.error ? rej(new Error(m.error.message)) : res(m.result); }
      else for (const h of c.handlers) h(m);
    };
    return c;
  }
  send(method, params = {}, sessionId) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    return new Promise((res, rej) => this.pending.set(id, { res, rej }));
  }
}
async function launch() {
  const prof = mkdtempSync(join(tmpdir(), "mgr-e2e-"));
  const args = ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu", "--no-sandbox", "--no-first-run",
    `--user-data-dir=${prof}`, "--hide-scrollbars", "--remote-debugging-port=0", "--mute-audio", "--autoplay-policy=no-user-gesture-required", "about:blank"];
  const p = spawn(CHROMIUM, args, { stdio: ["ignore", "ignore", "pipe"] });
  const wsUrl = await new Promise((res, rej) => {
    let buf = "";
    const t = setTimeout(() => rej(new Error("Chromium did not start")), 15000);
    p.stderr.on("data", (d) => { buf += d; const m = /DevTools listening on (ws:\S+)/.exec(buf); if (m) { clearTimeout(t); res(m[1]); } });
  });
  return { p, wsUrl };
}

let fails = 0, checks = 0;
const rows = [];
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`  ${c ? "ok  " : "FAIL"}  ${what}`); return !!c; };
const note = (c, what) => { console.log(`  ${c ? "ok  " : "--  "}  ${what}${c ? "" : " (the double's limit, not checked)"}`); return !!c; };
const hex = (a) => (a ? Array.from(a.slice ? a.slice(0, 12) : a, (v) => v.toString(16).padStart(2, "0")).join(" ") + (a.length > 12 ? " ..." : "") : "-");

// expressions run in the player page; F = the manager's window (the panel's iframe, same origin)
const FR = `var F = document.querySelector(".ed-frame") && document.querySelector(".ed-frame").contentWindow;`;
const felucca = (id) => id !== "choralroot";

async function session(id, mode) {
  const { p, wsUrl } = await launch();
  const logs = [];
  try {
    const cdp = await Cdp.connect(wsUrl);
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const S = (m, a) => cdp.send(m, a, sessionId);
    cdp.handlers.push((m) => {
      if (m.sessionId !== sessionId) return;
      if (m.method === "Runtime.exceptionThrown") logs.push("EXCEPTION: " + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
      if (m.method === "Runtime.consoleAPICalled" && m.params.type === "error") logs.push("console.error: " + m.params.args.map((a) => a.value ?? a.description).join(" ").slice(0, 200));
    });
    await S("Runtime.enable"); await S("Page.enable");
    await cdp.send("Browser.setDownloadBehavior", { behavior: "deny" }).catch(() => {});
    await S("Emulation.setDeviceMetricsOverride", { width: 1600, height: 900, deviceScaleFactor: 1, mobile: false });
    const ev = async (expr) => {
      const r = await S("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true });
      if (r.exceptionDetails) throw new Error((r.exceptionDetails.exception?.description || r.exceptionDetails.text).split("\n")[0]);
      return r.result.value;
    };
    const until = async (expr, ms, step = 100) => { const t0 = Date.now(); for (;;) { let v; try { v = await ev(expr); } catch (_) { v = null; } if (v) return v; if (Date.now() - t0 > ms) return null; await sleep(step); } };
    const shot = async (name) => {
      const s = await S("Page.captureScreenshot", { format: "png" });
      const b = Buffer.from(s.data, "base64");
      writeFileSync(join(OUT, name), b);
      if (SHOTS) writeFileSync(join(SHOTS, name), b);
      return join(OUT, name);
    };

    // the player's Autosave of this firmware (IndexedDB "remotefm1-player"): when it was saved, and a hash of its bytes
    const autosave = () => ev(`(async () => { const d = await new Promise((res) => { const q = indexedDB.open("remotefm1-player"); q.onsuccess = () => res(q.result); });
      const all = await new Promise((res) => { const q = d.transaction("saves").objectStore("saves").getAll(); q.onsuccess = () => res(q.result); });
      const a = all.find((x) => x.fw === "${id}" && x.engine === "native" && x.name === "Autosave"); if (!a) return null;
      let h = 2166136261; for (let i = 0; i < a.bytes.length; i++) h = Math.imul(h ^ a.bytes[i], 16777619);
      return a.saved + " " + (h >>> 0).toString(16); })()`);
    // a flash write the manager made must reach the Autosave as the firmware's own saves do (main.js: the flash's seq
    // moves, 1.5 s later the page reads the flash and keeps it)
    const autosaveMoved = async (before, what) => {
      const t0 = Date.now();
      let now = null;
      while (Date.now() - t0 < 12000) { now = await autosave(); if (now && now !== before && now.split(" ")[1] !== (before || " ").split(" ")[1]) break; await sleep(300); }
      return ok(now && now !== before && now.split(" ")[1] !== (before || " ").split(" ")[1], `${id}: ${what}'s flash writes reached the site's Autosave (${before || "none"} -> ${now || "none"})`);
    };
    await S("Page.navigate", { url: `${BASE}site/?fw=${id}&engine=native` });
    const up = await until(`window.FM1Editor && FM1Editor.bridge.target.connected && FM1Editor.bridge.target.port`, 25000);
    ok(up, `${id}: the native engine runs, the in-page port is "${up}"`);
    if (!up) return;
    const r = { id, mode, port: up };
    // the double: the manager's own simulator in place of the core, behind the same bridge (installed before the
    // panel opens: ChoralRoot's manager connects by itself)
    if (mode === "double" && !felucca(id)) {
      await ev(`(async () => {
        const m = await import("/vendor/FM1VST/cores/ChoralRootFM1/web/fm1manager.js");
        const sim = window.__sim = m.makeSimFM1(m.simObjects(12), { version: "ChoralRoot 0.25.1 (e2e double)" });
        window.__double = (b) => {
          if (b[0] !== 0xf0 || b[1] !== 0x7d || b[2] !== 0x46 || b[3] !== 0x4c) return;
          const a = sim.handle(b[4], Array.from(b.slice(5, -1)));
          if (a) setTimeout(() => FM1Editor.bridge.fromEngine(Uint8Array.from([0xf0, 0x7d, 0x46, 0x4c, b[4], ...a, 0xf7])), 2);
        };
        return true; })()`);
    }
    await ev(`FM1Editor.bridge.stats.toEngine = 0; FM1Editor.bridge.stats.toManagers = 0; true`);
    // every request the manager makes, by command (F0 7D 46 4C cmd ...), whatever answers it
    await ev(`(() => { window.__cmds = {}; const t = FM1Editor.bridge.toEngine; FM1Editor.bridge.toEngine = (b) => {
      if (b[0] === 0xf0 && b[1] === 0x7d) { const k = b[1] === 0x7d && b[2] === 0x46 ? b[4] : "7d-" + b[2]; __cmds[k] = (__cmds[k] || 0) + 1; } return (window.__double || t)(b); }; return true; })()`);
    await ev(`document.getElementById("ed-btn").click()`);
    const loaded = await until(`${FR} !!(F && F.__fm1shim && F.document.readyState === "complete")`, 15000);
    ok(loaded, `${id}: the Editor panel shows ${INDEX.managers.find((m) => m.id === id).title}, the shim in it`);
    if (!loaded) {
      console.log("    panel:", await ev(`${FR} JSON.stringify({ shim: typeof (F && F.__fm1shim), ready: F && F.document.readyState, nav: F && String(F.navigator.requestMIDIAccess).slice(0, 60), index: !!FM1Editor.index, btn: !document.getElementById("ed-btn").hidden, open: FM1Editor.open, body: document.getElementById("ed-body").innerHTML.slice(0, 300) })`));
      await shot(`editor-${id}-${mode}-fail.png`);
      return;
    }
    await sleep(600);
    const names = await ev(`${FR} F.navigator.requestMIDIAccess().then((a) => [...a.inputs.values(), ...a.outputs.values()].map((p) => p.type + ":" + p.name + ":" + p.state))`);
    ok(names.length === 2 && names.every((n) => n.endsWith(":" + up + ":connected")), `${id}: the manager sees ${names.join(", ")}`);
    if (mode === "double" && felucca(id)) {
      await ev(`${FR} (() => { const mock = F.__mock = F.makeMockDevice({ auto: false });
        const mi = [...mock.access.inputs.values()][0], mo = [...mock.access.outputs.values()][0];
        mi.onmidimessage = (e) => FM1Editor.bridge.fromEngine(e.data);
        window.__double = (b) => mo.send(Array.from(b));
        return true; })()`);
    }

    // ---- connect + INFO
    let answered = false;
    if (felucca(id)) {
      await ev(`${FR} F.document.getElementById("connect").click(), true`);
      // (the editor's state lives in a closure: its own page tells, #version filled and the status "Connected")
      answered = !!(await until(`${FR} F.document.getElementById("version").textContent && /^(Connected|接続しました)$/.test(F.document.getElementById("status").textContent) || null`, mode === "double" ? 15000 : 9000));
      if (!answered) await sleep(500);
      r.status = await ev(`${FR} F.document.getElementById("status").textContent`);
      r.version = await ev(`${FR} F.document.getElementById("version").textContent`);
    } else {
      answered = !!(await until(`${FR} /read:/.test(F.document.getElementById("status").textContent)`, mode === "double" ? 15000 : 9000));
      r.status = await ev(`${FR} F.document.getElementById("conn").textContent + " | " + F.document.getElementById("status").textContent`);
      r.version = await ev(`${FR} F.document.getElementById("fw").textContent`);
    }
    const st = await ev(`({ toEngine: FM1Editor.bridge.stats.toEngine, toManagers: FM1Editor.bridge.stats.toManagers, first: FM1Editor.bridge.stats.lastIn && Array.from(FM1Editor.bridge.stats.lastIn) })`);
    r.toEngine = st.toEngine; r.toManagers = st.toManagers;
    ok(st.toEngine > 0, `${id}: the manager's requests reached the ${mode === "double" ? "double" : "engine"} (${st.toEngine} messages, last ${hex(st.first)})`);
    r.info = answered;
    if (mode === "double") ok(answered, `${id}: connected through the shim: "${r.version}" (${r.status})`);
    else console.log(`  ${answered ? "ok  " : "--  "}  ${id}: INFO ${answered ? `answered: "${r.version}"` : "not answered (the core has no SysEx service yet: FM1VST sysex-glue)"}; status "${r.status}"`);

    // ---- a backup get, a put and the get-back
    if (answered && felucca(id)) {
      const caps = await ev(`${FR} (F.document.getElementById("backup-export").disabled ? 0 : 1) | (F.document.getElementById("backup-import").disabled ? 0 : 2)`);
      if (caps & 1) {
        const save = async () => {
          await ev(`${FR} (() => { F.__blobs = []; if (!F.__hooked) { F.__hooked = 1; const o = F.URL.createObjectURL.bind(F.URL); F.URL.createObjectURL = (b) => { F.__blobs.push(b); return o(b); }; } return true; })()`);
          await ev(`${FR} F.document.getElementById("backup-export").click(), true`);
          const done = await until(`${FR} F.__blobs.length ? true : null`, 60000, 200);
          return done ? ev(`${FR} F.__blobs[0].text()`) : null;
        };
        const a1 = await save();
        r.get = !!a1;
        // (the double is the editor's own mock: its backup, where it has one, is partial; the engine's must work)
        (mode === "engine" ? ok : note)(a1 && a1.length > 100, `${id}: backup GET (Save backup): an archive of ${a1 ? a1.length : 0} bytes`);
        if (a1 && caps & 2) {
          const flashBefore = await autosave();
          // (the status line says "restored" until the next push overwrites it: watched, not polled)
          await ev(`${FR} (() => { F.__said = []; new F.MutationObserver(() => F.__said.push(F.document.getElementById("status").textContent))
            .observe(F.document.getElementById("status"), { childList: true, characterData: true, subtree: true }); return true; })()`);
          await ev(`${FR} (() => { F.confirm = () => true; const dt = new F.DataTransfer(); dt.items.add(new F.File([${JSON.stringify(a1)}], "e2e-backup.json", { type: "application/json" }));
            const i = F.document.getElementById("backup-file"); i.files = dt.files; i.dispatchEvent(new F.Event("change")); return true; })()`);
          const restored = await until(`${FR} F.__said.find((t) => /restored|復元しました/i.test(t)) || null`, 90000, 250);
          r.said = await ev(`${FR} [...new Set(F.__said.filter((t) => !/%$|^Reading|^読み/.test(t)))].join(" / ")`);
          ok(restored, `${id}: backup PUT (Restore backup): the editor said "${r.said}"`);
          await sleep(1500);
          const a2 = await save();
          const strip = (t) => { try { const j = JSON.parse(t); delete j.created; delete j.date; delete j.saved; return JSON.stringify(j); } catch (_) { return t; } };
          r.put = !!restored && a2 && strip(a2) === strip(a1);
          if (!r.put && a2) {
            const o1 = JSON.parse(a1).objects, o2 = JSON.parse(a2).objects;
            console.log("        objects that differ (id size crc):", o1.filter((o, i) => o.crc !== o2[i].crc).map((o, i) => `${o.id} ${o.size}/${o2[o1.indexOf(o)].size}`).join(", "));
          }
          ok(r.put, `${id}: get-back: the archive saved again is the one restored`);
          if (mode === "engine") await autosaveMoved(flashBefore, "the restore");
        }
      } else console.log(`  --    ${id}: the firmware reports no backup support (backupCaps ${caps})`);
    }
    if (answered && !felucca(id)) {
      r.get = true;
      ok(true, `${id}: backup GET: the device read (LIST + GET of every object): "${r.status}"`);
      const before = await ev(`${FR} F.document.querySelectorAll("#dev-rows li.user").length`);
      const flashBefore = await autosave();
      await ev(`${FR} (() => { const s = F.document.getElementById("lib-src"); s.value = "factory-fm6.json"; s.dispatchEvent(new F.Event("change")); return true; })()`);
      const libOk = await until(`${FR} F.document.querySelector("#lib-rows li.user") ? true : null`, 5000);
      if (libOk) {
        await ev(`${FR} F.document.querySelector("#lib-rows li.user").click(), true`);
        await until(`${FR} !F.document.getElementById("place").disabled || null`, 3000);
        await ev(`${FR} F.document.getElementById("place").click(), true`);
        const placed = await until(`${FR} /^Placed/.test(F.document.getElementById("status").textContent) ? F.document.getElementById("status").textContent : null`, 30000, 200);
        const after = await ev(`${FR} F.document.querySelectorAll("#dev-rows li.user").length`);
        r.put = !!placed && after === before + 1;
        ok(r.put, `${id}: PUT (Place a pack sound) and the read-back: ${before} -> ${after} user sounds ("${placed}")`);
        if (mode === "engine") await autosaveMoved(flashBefore, "the Place");
      } else ok(false, `${id}: the library shows the FM6 pack`);
    }
    // the firmware keeps running beside the editor: a key played on the page's FM-1 moves its screen (frames come from
    // the worklet's render loop, the one that makes the sound)
    {
      const f0 = await ev(`FM1Editor.d.getState().frameSeq`);
      await ev(`dispatchEvent(new KeyboardEvent("keydown", { code: "KeyD", key: "d", bubbles: true })), true`);
      await sleep(400);
      await ev(`dispatchEvent(new KeyboardEvent("keyup", { code: "KeyD", key: "d", bubbles: true })), true`);
      const f1 = await until(`FM1Editor.d.getState().frameSeq !== ${f0} ? FM1Editor.d.getState().frameSeq : null`, 3000);
      const ctx = await ev(`FM1Editor.d.getState().running && FM1Editor.d.getState().engineState`);
      ok(f1 && ctx === "running", `${id}: the firmware keeps running with the editor open (screen frame ${f0} -> ${f1}, engine ${ctx})`);
    }
    r.cmds = await ev(`JSON.stringify(window.__cmds)`);
    Object.assign(r, await ev(`({ toEngine: FM1Editor.bridge.stats.toEngine, toManagers: FM1Editor.bridge.stats.toManagers })`));
    console.log(`        requests by command: ${r.cmds} (1 INFO, 65 LIST, 66 GET, 67 PUT)`);
    if (r.put) ok(/"66":/.test(r.cmds) && /"67":/.test(r.cmds), `${id}: the put went as BACKUP_PUT (67) and came back by BACKUP_GET (66)`);
    await sleep(300);
    r.shot = await shot(`editor-${id}-${mode}.png`);
    console.log(`  shot  ${r.shot}`);
    rows.push(r);

    if (mode === "engine" && answered) {
      // ---- RESTART (72) from the manager's port: the firmware halts, the page boots it again from its flash as it
      // stands (main.js halted), the port goes and comes back, the manager finds it again
      const users = !felucca(id) ? await ev(`${FR} F.document.querySelectorAll("#dev-rows li.user").length`) : null;
      await ev(`window.__halts = 0; new MutationObserver(() => { if (/restarted/.test(document.getElementById("toast").textContent)) window.__halts++; })
        .observe(document.getElementById("toast"), { childList: true, characterData: true, subtree: true }); true`);
      await ev(`${FR} F.navigator.requestMIDIAccess().then((a) => { [...a.outputs.values()][0].send([0xf0, 0x7d, 0x46, 0x4c, 72, 0xf7]); return true; })`);
      const halted = await until(`window.__halts > 0 || null`, 5000);
      if (!halted) console.log(`  --    ${id}: RESTART (72): the firmware did not halt (its glue does not map it)`);
      else {
        const back = await until(`FM1Editor.bridge.target.connected && FM1Editor.bridge.target.port || null`, 20000);
        ok(back, `${id}: RESTART (72): the firmware halted, booted again from its flash; the port "${back}" is back`);
        if (!felucca(id)) {
          const again = await until(`${FR} /read:/.test(F.document.getElementById("status").textContent) && F.document.querySelectorAll("#dev-rows li.user").length === ${users} ? F.document.getElementById("status").textContent : null`, 20000, 250);
          ok(again, `${id}: after the reboot the manager found the port again and read ${users} user sound(s) back from the flash ("${again}")`);
        }
      }
      // ---- popped out: the manager in a tab of its own, over the BroadcastChannel to this player tab
      await ev(`document.getElementById("ed-close").click(), true`);
      const pid = await ev(`FM1Editor.bridge.pid`);
      const { targetId: t2 } = await cdp.send("Target.createTarget", { url: `${BASE}build/managers/${id}/` });
      const { sessionId: s2 } = await cdp.send("Target.attachToTarget", { targetId: t2, flatten: true });
      await cdp.send("Runtime.enable", {}, s2);
      await cdp.send("Emulation.setDeviceMetricsOverride", { width: 1200, height: 900, deviceScaleFactor: 1, mobile: false }, s2);
      const ev2 = async (expr) => { const x = await cdp.send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true }, s2); return x.exceptionDetails ? null : x.result.value; };
      const until2 = async (expr, ms) => { const t0 = Date.now(); for (;;) { const v = await ev2(expr).catch(() => null); if (v) return v; if (Date.now() - t0 > ms) return null; await sleep(150); } };
      const bound = await until2(`window.__fm1shim && __fm1shim.pid`, 8000);
      ok(bound === pid, `${id}: popped out: the tab's shim is bound to the player tab (${bound})`);
      let tabOk;
      if (felucca(id)) {
        await until2(`document.readyState === "complete" || null`, 5000);
        await ev2(`document.getElementById("connect").click(), true`);
        tabOk = await until2(`document.getElementById("version").textContent && /^(Connected|接続しました)$/.test(document.getElementById("status").textContent) ? document.getElementById("version").textContent : null`, 12000);
      } else tabOk = await until2(`/read:/.test(document.getElementById("status").textContent) ? document.getElementById("status").textContent : null`, 15000);
      ok(tabOk, `${id}: popped out: the manager talks to the firmware from its own tab ("${tabOk}")`);
      const s = await cdp.send("Page.captureScreenshot", { format: "png" }, s2);
      writeFileSync(join(OUT, `editor-${id}-tab.png`), Buffer.from(s.data, "base64"));
      if (SHOTS) writeFileSync(join(SHOTS, `editor-${id}-tab.png`), Buffer.from(s.data, "base64"));
    }
    cdp.ws.close();
  } finally {
    for (const l of logs.slice(0, 8)) console.log("    " + l);
    p.kill("SIGTERM");
    await new Promise((res) => { p.once("exit", res); setTimeout(res, 3000); });
  }
}

// the panel without a manager to show: an emulated firmware ("switch Engine to Native"), a firmware with no native
// build whose editor is on its own site ("needs a real FM-1"); a phone upright (the panel under the FM-1)
async function notes() {
  const { p, wsUrl } = await launch();
  try {
    const cdp = await Cdp.connect(wsUrl);
    const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
    const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
    const S = (m, a) => cdp.send(m, a, sessionId);
    await S("Runtime.enable"); await S("Page.enable");
    const ev = async (expr) => { const r = await S("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true }); return r.exceptionDetails ? null : r.result.value; };
    const until = async (expr, ms) => { const t0 = Date.now(); for (;;) { const v = await ev(expr); if (v) return v; if (Date.now() - t0 > ms) return null; await sleep(150); } };
    const cases = [
      { name: "emu", url: "site/?fw=felucca&engine=emu", w: 1600, h: 900, want: /switch Engine to Native/ },
      { name: "real", url: "site/?fw=sloop", w: 1600, h: 900, want: /needs a real FM-1/ },
      { name: "phone", url: "site/?fw=jiant&engine=native", w: 390, h: 844, mobile: true, want: /JIANT FM editor/, frame: true },
    ];
    for (const c of cases) {
      await S("Emulation.setDeviceMetricsOverride", { width: c.w, height: c.h, deviceScaleFactor: c.mobile ? 2 : 1, mobile: !!c.mobile });
      await S("Page.navigate", { url: BASE + c.url });
      const btn = await until(`window.FM1Editor && !document.getElementById("ed-btn").hidden || null`, 20000);
      if (c.frame) await until(`FM1Editor.bridge.target.connected || null`, 20000);
      await sleep(1500);
      await ev(`document.getElementById("ed-btn").click(), true`);
      await sleep(c.frame ? 2500 : 800);
      const text = await ev(`document.getElementById("ed-panel").innerText`);
      ok(btn && c.want.test(text || ""), `notes: ${c.url}: the Editor panel says "${(text || "").split("\n").filter(Boolean).slice(1, 2).join(" ").slice(0, 90)}"`);
      const s = await S("Page.captureScreenshot", { format: "png" });
      writeFileSync(join(OUT, `editor-note-${c.name}.png`), Buffer.from(s.data, "base64"));
      if (SHOTS) writeFileSync(join(SHOTS, `editor-note-${c.name}.png`), Buffer.from(s.data, "base64"));
    }
    cdp.ws.close();
  } finally { p.kill("SIGTERM"); await new Promise((res) => { p.once("exit", res); setTimeout(res, 3000); }); }
}

const ids = MODE === "notes" ? [] : IDS.length ? IDS : INDEX.managers.map((m) => m.id);
if (MODE === "notes" || (MODE === "both" && !IDS.length)) { console.log("== the panel's notes"); try { await notes(); } catch (e) { ok(false, `notes: ${e.message}`); } }
for (const id of ids) {
  const core = INDEX.managers.find((m) => m.id === id);
  if (!core) { ok(false, `${id}: no such hosted manager`); continue; }
  if (!existsSync(join(WEB, "build", "native", `${id}.wasm`))) { ok(false, `${id}: no web/build/native/${id}.wasm (web/engines/native/build.sh)`); continue; }
  for (const mode of MODE === "both" ? ["double", "engine"] : [MODE]) {
    console.log(`== ${id} (${mode})`);
    try { await session(id, mode); } catch (e) { ok(false, `${id} (${mode}): ${e.message}`); }
  }
}
server.kill("SIGTERM");
console.log("\n| manager | mode | port | INFO | backup GET | PUT + get-back | messages in / out |\n|---|---|---|---|---|---|---|");
for (const r of rows) console.log(`| ${r.id} | ${r.mode} | ${r.port} | ${r.info ? "answered" : "no reply"} | ${r.get ? "ok" : "-"} | ${r.put ? "ok" : "-"} | ${r.toEngine} / ${r.toManagers} |`);
console.log(`\n${checks - fails} / ${checks} checks passed`);
process.exit(fails ? 1 : 0);
