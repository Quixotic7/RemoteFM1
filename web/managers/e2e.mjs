// SPDX-License-Identifier: GPL-3.0-only
// The hosted managers end to end in headless Chromium (no window, a fresh profile per run, ended by its PID; no real
// Web MIDI: the managers only ever see the shim). Per manager:
//   engine  the player with the firmware's native core, the Editor panel open, the manager's own connect (Connect /
//           its auto-connect): its INFO must reach the engine; with a core that answers (FM1VST sysex-glue), INFO, a
//           backup GET (Felucca editors: Save backup; ChoralRoot: the device read) and a put + get-back (Felucca
//           editors: Restore backup, then Save backup again: the same archive; ChoralRoot: Place a pack sound, read back)
//   double  the same through the shim and the bridge, with the manager's own simulator standing in for the core
//           (Felucca editors: their makeMockDevice; ChoralRoot: fm1manager.js makeSimFM1): the whole UI path, today
//   git     a manager build.mjs fetched from its repository (index.json "git"): S1Vave's editor (READ SYNTH, its CC 119
//           dump; a slider's CC read back by the next dump), Mutable Dreams' (#device-connect; the tempo slider read
//           back by its live-state poll; Back up all device banks: the .mddb; its restore asks for SEL on the unit: not
//           tried)
//   PIGEON (engine only: its loader has no simulator): a fresh PIGEON is silent; the sample loader connects (negotiate,
//           INFO), opens FM1VST's test bank (tests/fixtures/pigeon/bank.kob), sends it (Send bank, verified by its
//           read-back), and then the keys sound (the player's output, through an AnalyserNode)
//   --live  the author's live editor page instead of the hosted copy (engine mode; web/site/js/live-editor.js): fetched
//           from the author's site (the network), in the panel's sandboxed iframe (an opaque origin, reached here through
//           its own execution context), then the same checks; ids without a hosted manager too (rainbow, fizz: their
//           catalog editor, on --engine rfm1; FIZZ's manager: its port scan, then #fm1); popped out: managers/live.html relays it to the player tab. A page that
//           falls back to the hosted copy is reported (its reason), not failed
//           Without --live the hosted copies are tested: the player is told to prefer them (its "Use hosted copy").
// Screenshots: web/build/test/editor-<id>-<mode>.png (and SHOTS_DIR when set).
//   node web/managers/e2e.mjs [id ...] [--mode engine|double|notes|both] [--engine native|rfm1] [--live]     (after node web/managers/build.mjs; the native
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
const ei = argv.indexOf("--engine"), ENGINE = ei >= 0 ? argv[ei + 1] : "native";     // rfm1: the firmware's .rfm1 file
const IDS = argv.filter((a, i) => !a.startsWith("--") && (mi < 0 || i !== mi + 1) && (ei < 0 || i !== ei + 1));
const LIVE = argv.includes("--live");
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
const PREF = (id) => `remotefm1-editor-src:${id}`;
// (a known reason a manager gets no answer yet, said instead of the generic one)
const BLOCKED = { mutabledreams: "blocked on the .rfm1 identity fix: mutabledreams-1.4.rfm1 answers the M-UPGRADE handshake with \"FM-1_7\" and fill, not its package identity, and does not answer the remote panel (7D 4C 43)" };
const GIT = new Set(INDEX.managers.filter((m) => m.git).map((m) => m.id));     // (build.mjs: from a repository at a commit)

// the execution contexts of a page and of its frames (a sandboxed srcdoc iframe may be a process of its own, an OOPIF:
// auto-attached; or a context of the page's own session): evaluate in the newest frame context of an opaque origin
function frameContexts(cdp, rootSession) {
  const ctx = [];                                      // { session, id, frameId, origin, n }
  let n = 0;
  const sessions = new Set([rootSession]);
  cdp.handlers.push((m) => {
    if (!sessions.has(m.sessionId)) return;
    if (m.method === "Runtime.executionContextCreated") {
      const c = m.params.context;
      if (c.auxData && c.auxData.isDefault) ctx.push({ session: m.sessionId, id: c.id, frameId: c.auxData.frameId, origin: c.origin, n: ++n });
    } else if (m.method === "Runtime.executionContextDestroyed") {
      const i = ctx.findIndex((c) => c.session === m.sessionId && c.id === m.params.executionContextId); if (i >= 0) ctx.splice(i, 1);
    } else if (m.method === "Runtime.executionContextsCleared") {
      for (let i = ctx.length - 1; i >= 0; i--) if (ctx[i].session === m.sessionId) ctx.splice(i, 1);
    } else if (process.env.E2E_DEBUG && m.sessionId !== rootSession && (m.method === "Runtime.exceptionThrown" || (m.method === "Runtime.consoleAPICalled" && /error|warn/.test(m.params.type)))) {
      console.log("    [frame]", m.method === "Runtime.exceptionThrown" ? (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text).slice(0, 300) : m.params.args.map((a) => a.value ?? a.description).join(" ").slice(0, 300));
    } else if (m.method === "Target.attachedToTarget" && m.params.targetInfo.type === "iframe") {
      const s = m.params.sessionId; sessions.add(s);
      cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, s).catch(() => {});
      cdp.send("Runtime.enable", {}, s).catch(() => {});
    }
  });
  // (the newest opaque-origin frame context that has the shim: the editor's, not a frame of its own, a challenge's)
  let chosen = null;
  const pick = async () => {
    if (chosen && ctx.includes(chosen)) return chosen;
    chosen = null;
    for (const c of ctx.filter((x) => x.origin === "null" || x.origin === "" || x.origin === "://").sort((a, b) => b.n - a.n)) {
      const r = await cdp.send("Runtime.evaluate", { expression: "!!window.__fm1shim", contextId: c.id, returnByValue: true }, c.session).catch(() => null);
      if (r && r.result && r.result.value === true) return (chosen = c);
    }
    return null;
  };
  return {
    start: () => cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, rootSession),
    pick,
    async ev(expr) {
      const c = await pick();
      if (!c) throw new Error("no live frame context");
      if (process.env.E2E_DEBUG) console.log("    [ctx]", JSON.stringify(ctx));
      const r = await cdp.send("Runtime.evaluate", { expression: expr, contextId: c.id, awaitPromise: true, returnByValue: true, userGesture: true }, c.session);
      if (r.exceptionDetails) throw new Error((r.exceptionDetails.exception?.description || r.exceptionDetails.text).split("\n")[0]);
      return r.result.value;
    },
  };
}

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
    // F: the manager's window. Hosted: the panel's iframe, same origin, from the player page; live: the sandbox's own context
    const fc = frameContexts(cdp, sessionId);
    if (LIVE) await fc.start();
    const evF = (expr) => (LIVE ? fc.ev("((F) => (" + expr + "))(window)") : ev(FR + " " + expr));
    const untilF = async (expr, ms, step = 100) => { const t0 = Date.now(); for (;;) { let v; try { v = await evF(expr); } catch (_) { v = null; } if (v) return v; if (Date.now() - t0 > ms) return null; await sleep(step); } };
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
      const a = all.find((x) => x.fw === "${id}" && x.engine === "${ENGINE}" && x.name === "Autosave"); if (!a) return null;
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
    await S("Page.navigate", { url: `${BASE}site/?fw=${id}&engine=${ENGINE}` });
    const up = await until(`window.FM1Editor && FM1Editor.bridge.target.connected && FM1Editor.bridge.target.port`, 25000);
    ok(up, `${id}: the ${ENGINE} engine runs, the in-page port is "${up}"`);
    if (!up) { console.log("    state:", await ev(`(() => { const s = FM1Editor.d.getState(); return JSON.stringify({ entry: s.entry && s.entry.id, engine: s.engine, want: s.wantEngine, st: s.engineState, info: s.info, port: s.entry && s.entry.port }); })()`).catch((e) => e.message)); for (const l of logs.slice(0, 6)) console.log("    " + l); return; }
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
    await ev(`(() => { try { ${LIVE ? `localStorage.removeItem("${PREF(id)}")` : `localStorage.setItem("${PREF(id)}", "hosted")`}; } catch (_) {} return true; })()`);
    await ev(`document.getElementById("ed-btn").click()`);
    const title = (INDEX.managers.find((m) => m.id === id) || {}).title || `${id}'s editor`;
    if (LIVE) {
      const want = await ev(`FM1Editor.editor && FM1Editor.editor.live`);
      const got = await until(`FM1Editor.showingLive && FM1Editor.live.ready ? "live" : FM1Editor.editor && FM1Editor.liveFail.has(FM1Editor.editor.live) ? "fail" : null`, 15000);
      r.live = got === "live" ? `live (${await ev(`FM1Editor.live.host`)})` : `fell back: ${got ? await ev(`FM1Editor.liveFail.get(FM1Editor.editor.live)`) : "no hello, no failure in 15 s"}`;
      if (!want) r.live = "no live page (not https, a fragment, or none)";
      ok(got === "live" || got === "fail", `${id}: the author's live page ${want || "-"}: ${r.live}`);
      if (got !== "live") {
        const fb = await ev(`FM1Editor.frameFor`);
        console.log(`  --    ${id}: the panel shows ${fb || "a note"} instead`);
        await shot(`editor-${id}-live-fallback.png`);
        rows.push(r);
        return;
      }
      ok(await ev(`(() => { const f = document.querySelector(".ed-frame"); return f.getAttribute("sandbox").split(" ").indexOf("allow-same-origin") < 0 && f.hasAttribute("srcdoc"); })()`),
         `${id}: the live page runs in a sandbox without allow-same-origin (srcdoc)`);
      ok(await ev(`(() => { try { return !document.querySelector(".ed-frame").contentWindow.document; } catch (_) { return true; } })()`),
         `${id}: the player cannot reach into the live page (an opaque origin)`);
    }
    const loaded = await untilF(`!!(F && F.__fm1shim && F.document.readyState === "complete")`, 15000);
    ok(loaded, `${id}: the Editor panel shows ${title}${LIVE ? " (live)" : ""}, the shim in it${LIVE ? ` (transport ${loaded ? await evF("F.__fm1shim.transport") : "-"})` : ""}`);
    if (LIVE && loaded) {
      const st = await evF(`(() => { const o = {}; try { localStorage.setItem("e2e-k", "v1"); o.ls = localStorage.getItem("e2e-k"); } catch (e) { o.ls = "threw " + e.name; }
        o.idb = typeof indexedDB; try { history.replaceState(null, "", location.pathname + location.search + "#e2e"); o.hist = location.hash; } catch (e) { o.hist = "threw " + e.name; }
        o.base = document.baseURI; return o; })()`);
      await sleep(200);
      const kept = await ev(`Object.keys(localStorage).filter((k) => k.startsWith("remotefm1-ed:") && k.endsWith("|e2e-k")).map((k) => localStorage.getItem(k)).join()`);
      ok(st.ls === "v1" && kept === "v1" && st.hist === "#e2e", `${id}: in the sandbox: localStorage a stand-in kept by the player (${st.ls}/${kept}), indexedDB ${st.idb}, replaceState ${st.hist}, base ${st.base}`);
    }
    if (!loaded) {
      if (LIVE) console.log("    live frame:", await evF(`JSON.stringify({ shim: typeof F.__fm1shim, ready: F.document.readyState, href: location.href, title: document.title, body: document.body && document.body.innerText.slice(0, 200) })`).catch((e) => e.message));
      if (!LIVE) console.log("    panel:", await evF(`JSON.stringify({ shim: typeof (F && F.__fm1shim), ready: F && F.document.readyState, nav: F && String(F.navigator.requestMIDIAccess).slice(0, 60), index: !!FM1Editor.index, btn: !document.getElementById("ed-btn").hidden, open: FM1Editor.open, body: document.getElementById("ed-body").innerHTML.slice(0, 300) })`));
      await shot(`editor-${id}-${mode}-fail.png`);
      return;
    }
    await sleep(600);
    const names = await evF(`F.navigator.requestMIDIAccess().then((a) => [...a.inputs.values(), ...a.outputs.values()].map((p) => p.type + ":" + p.name + ":" + p.state))`);
    ok(names.length === 2 && names.every((n) => n.endsWith(":" + up + ":connected")), `${id}: the manager sees ${names.join(", ")}`);
    if (mode === "double" && felucca(id)) {
      await evF(`(() => { const mock = F.__mock = F.makeMockDevice({ auto: false });
        const mi = [...mock.access.inputs.values()][0], mo = [...mock.access.outputs.values()][0];
        mi.onmidimessage = (e) => FM1Editor.bridge.fromEngine(e.data);
        window.__double = (b) => mo.send(Array.from(b));
        return true; })()`);
    }

    // ---- connect + INFO
    let answered = false;
    // (Felucca's own editor since 1.8: a header chip, #state-t "live · 1.8.1" once it read the firmware; no #version)
    const felucca2 = felucca(id) && await evF(`!F.document.getElementById("version") && !!F.document.getElementById("state-t")`);
    // (an editor this test does not know (a live page only: GR!MM, Caravel, DUELL ...): its own Connect button, if it has
    // one; it answered when the firmware's replies reached it and its page shows the firmware's version)
    // (hosted, a git-sourced one: S1Vave's editor (no Connect: it finds the port; READ SYNTH asks for a CC dump, CC 119),
    // Mutable Dreams' (#device-connect, then "Connected to <id>" in #device-status))
    const generic = (LIVE || GIT.has(id)) && felucca(id) && !felucca2 && await evF(`!F.document.getElementById("version") || !F.document.getElementById("status")`);
    const fizz = generic && await evF(`!!(F.document.getElementById("unit") && F.document.getElementById("fm1"))`);
    if (fizz) {
      // (FIZZ's manager: its port scan finds the FM-1 running FIZZ (the M-UPGRADE identity, then FIZZ's IDENTITY) and
      // offers it on #fm1; a click makes it the unit: the pill #unit turns to "fm1", "on")
      const found = await untilF(`F.document.getElementById("fm1").dataset.state === "found" || null`, 10000);
      if (found) await evF(`F.document.getElementById("fm1").click(), true`);
      else await evF(`(() => { F.document.getElementById("unit").click(); const b = F.document.getElementById("up-connect"); if (b) b.click(); return !!b; })()`);
      answered = !!(await untilF(`F.document.getElementById("unit").dataset.u === "fm1" && F.document.getElementById("unit").dataset.state === "on" || null`, 15000));
      await sleep(1500);
      r.status = r.version = await evF(`F.document.getElementById("unit").textContent.trim().slice(0, 60)`);
    } else if (generic) {
      await evF(`(() => { const b = F.document.getElementById("device-connect") || F.document.getElementById("btnReadSynth") || F.document.getElementById("connect") || [...F.document.querySelectorAll("button")].find((x) => /^\\s*connect/i.test(x.textContent)); if (b) b.click(); return !!b; })()`);
      answered = !!(await until(`FM1Editor.bridge.stats.toManagers > 0 || null`, 9000));
      if (await evF(`!!F.document.getElementById("device-status")`))
        answered = answered && !!(await untilF(`/^Connected to /.test(F.document.getElementById("device-status").textContent) || null`, 9000));
      await sleep(1500);
      r.status = r.version = await evF(`F.document.getElementById("device-status") ? F.document.getElementById("device-status").textContent.slice(0, 60)
        : ((F.document.body.innerText.match(/v?\\d+\\.\\d+(\\.\\d+)?[\\w.-]*/) || [""])[0])`);
    } else if (felucca2) {
      await evF(`F.document.getElementById("connect").click(), true`);
      answered = !!(await untilF(`!F.document.getElementById("state-dot").classList.contains("off") && /·/.test(F.document.getElementById("state-t").textContent) || null`, 9000));
      r.status = r.version = await evF(`F.document.getElementById("state-t").textContent`);
    } else if (felucca(id)) {
      await evF(`F.document.getElementById("connect").click(), true`);
      // (the editor's state lives in a closure: its own page tells, #version filled and the status "Connected")
      answered = !!(await untilF(`F.document.getElementById("version").textContent && /^(Connected|接続しました)$/.test(F.document.getElementById("status").textContent) || null`, mode === "double" ? 15000 : 9000));
      if (!answered) await sleep(500);
      r.status = await evF(`F.document.getElementById("status").textContent`);
      r.version = await evF(`F.document.getElementById("version").textContent`);
    } else {
      answered = !!(await untilF(`/read:/.test(F.document.getElementById("status").textContent)`, mode === "double" ? 15000 : 9000));
      r.status = await evF(`F.document.getElementById("conn").textContent + " | " + F.document.getElementById("status").textContent`);
      r.version = await evF(`F.document.getElementById("fw").textContent`);
    }
    const st = await ev(`({ toEngine: FM1Editor.bridge.stats.toEngine, toManagers: FM1Editor.bridge.stats.toManagers, first: FM1Editor.bridge.stats.lastIn && Array.from(FM1Editor.bridge.stats.lastIn) })`);
    r.toEngine = st.toEngine; r.toManagers = st.toManagers;
    ok(st.toEngine > 0, `${id}: the manager's requests reached the ${mode === "double" ? "double" : "engine"} (${st.toEngine} messages, last ${hex(st.first)})`);
    r.info = answered;
    if (mode === "double") ok(answered, `${id}: connected through the shim: "${r.version}" (${r.status})`);
    else console.log(`  ${answered ? "ok  " : "--  "}  ${id}: INFO ${answered ? `answered: "${r.version}"` : `not answered (${BLOCKED[id] || "the core has no SysEx service yet: FM1VST sysex-glue"})`}; status "${r.status}"`);

    // ---- a backup get, a put and the get-back
    if (answered && felucca(id) && !felucca2 && !generic && await evF(`!!F.document.getElementById("backup-export")`)) {
      const caps = await evF(`(F.document.getElementById("backup-export").disabled ? 0 : 1) | (F.document.getElementById("backup-import").disabled ? 0 : 2)`);
      if (caps & 1) {
        const save = async () => {
          await evF(`(() => { F.__blobs = []; if (!F.__hooked) { F.__hooked = 1; const o = F.URL.createObjectURL.bind(F.URL); F.URL.createObjectURL = (b) => { F.__blobs.push(b); return o(b); }; } return true; })()`);
          await evF(`F.document.getElementById("backup-export").click(), true`);
          const done = await untilF(`F.__blobs.length ? true : null`, 60000, 200);
          return done ? evF(`F.__blobs[0].text()`) : null;
        };
        const a1 = await save();
        r.get = !!a1;
        // (the double is the editor's own mock: its backup, where it has one, is partial; the engine's must work)
        (mode === "engine" ? ok : note)(a1 && a1.length > 100, `${id}: backup GET (Save backup): an archive of ${a1 ? a1.length : 0} bytes`);
        if (a1 && caps & 2) {
          const flashBefore = await autosave();
          // (the status line says "restored" until the next push overwrites it: watched, not polled)
          await evF(`(() => { F.__said = []; new F.MutationObserver(() => F.__said.push(F.document.getElementById("status").textContent))
            .observe(F.document.getElementById("status"), { childList: true, characterData: true, subtree: true }); return true; })()`);
          await evF(`(() => { F.confirm = () => true; const dt = new F.DataTransfer(); dt.items.add(new F.File([${JSON.stringify(a1)}], "e2e-backup.json", { type: "application/json" }));
            const i = F.document.getElementById("backup-file"); i.files = dt.files; i.dispatchEvent(new F.Event("change")); return true; })()`);
          const restored = await untilF(`F.__said.find((t) => /restored|復元しました/i.test(t)) || null`, 90000, 250);
          r.said = await evF(`[...new Set(F.__said.filter((t) => !/%$|^Reading|^読み/.test(t)))].join(" / ")`);
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
    if (answered && id === "s1vave") {
      // a slider of the editor (SAW, CC 20) to a value; the editor's own dump request (CC 119): the firmware's reply
      // carries the value back
      const v = await ev(`(() => { const v = 37 + (Date.now() % 50); window.__fromEng = []; const f = FM1Editor.bridge.fromEngine.bind(FM1Editor.bridge);
        FM1Editor.bridge.fromEngine = (b) => { __fromEng.push(Array.from(b)); return f(b); }; return v; })()`);
      await evF(`(() => { const s = F.document.getElementById("slSaw"); s.value = ${v}; s.dispatchEvent(new F.Event("input", { bubbles: true })); return true; })()`);
      await sleep(600);
      await evF(`F.document.getElementById("btnReadSynth").click(), true`);
      const back = await until(`__fromEng.some((b) => (b[0] & 0xf0) === 0xb0 && b[1] === 20 && b[2] === ${v}) || null`, 5000);
      r.slider = !!back;
      ok(back, `${id}: slider SAW to ${v} (CC 20), the editor's dump request (CC 119): the firmware sends CC 20 = ${v} back`);
    }
    if (answered && id === "mutabledreams") {
      // the tempo slider (command 15) to a value; the editor's poll (command 9, its live state) reads it back
      const v = 60 + (Date.now() % 90);
      await evF(`(() => { const s = F.document.getElementById("tempo"); s.value = ${v}; s.dispatchEvent(new F.Event("input", { bubbles: true })); s.dispatchEvent(new F.Event("change", { bubbles: true })); return true; })()`);
      const bpm = await untilF(`F.document.getElementById("bpm").textContent === "${v} BPM" || null`, 6000);
      r.slider = !!bpm;
      ok(bpm, `${id}: the tempo slider to ${v}: the firmware's live state reads back "${await evF(`F.document.getElementById("bpm").textContent`)}"`);
      // backup GET: Back up all device banks (the restore asks for SEL on the unit: not tried)
      await evF(`(() => { F.__blobs = []; const o = F.URL.createObjectURL.bind(F.URL); F.URL.createObjectURL = (b) => { F.__blobs.push(b); return o(b); }; F.document.getElementById("device-backup").click(); return true; })()`);
      const size = await untilF(`F.__blobs.length ? F.__blobs[0].size : null`, 60000, 250);
      r.get = !!size;
      ok(size > 1000, `${id}: backup GET (Back up all device banks): an .mddb of ${size || 0} bytes ("${await evF(`F.document.getElementById("device-status").textContent.slice(0, 80)`)}")`);
    }
    if (answered && !felucca(id)) {
      r.get = true;
      ok(true, `${id}: backup GET: the device read (LIST + GET of every object): "${r.status}"`);
      const before = await evF(`F.document.querySelectorAll("#dev-rows li.user").length`);
      const flashBefore = await autosave();
      await evF(`(() => { const s = F.document.getElementById("lib-src"); s.value = "factory-fm6.json"; s.dispatchEvent(new F.Event("change")); return true; })()`);
      const libOk = await untilF(`F.document.querySelector("#lib-rows li.user") ? true : null`, 5000);
      if (libOk) {
        await evF(`F.document.querySelector("#lib-rows li.user").click(), true`);
        await untilF(`!F.document.getElementById("place").disabled || null`, 3000);
        await evF(`F.document.getElementById("place").click(), true`);
        const placed = await untilF(`/^Placed/.test(F.document.getElementById("status").textContent) ? F.document.getElementById("status").textContent : null`, 30000, 200);
        const after = await evF(`F.document.querySelectorAll("#dev-rows li.user").length`);
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
      // (SLOOP's, Skiff's and DUELL's home screens do not redraw for that key, with or without the editor: there the
      // engine's state tells)
      const still = ["sloop", "skiff", "duell"].includes(id) ? ctx === "running" : f1 && ctx === "running";
      ok(still, `${id}: the firmware keeps running with the editor open (screen frame ${f0} -> ${f1}, engine ${ctx})`);
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
      const users = !felucca(id) ? await evF(`F.document.querySelectorAll("#dev-rows li.user").length`) : null;
      await ev(`window.__halts = 0; new MutationObserver(() => { if (/restarted/.test(document.getElementById("toast").textContent)) window.__halts++; })
        .observe(document.getElementById("toast"), { childList: true, characterData: true, subtree: true }); true`);
      await evF(`F.navigator.requestMIDIAccess().then((a) => { [...a.outputs.values()][0].send([0xf0, 0x7d, 0x46, 0x4c, 72, 0xf7]); return true; })`);
      const halted = await until(`window.__halts > 0 || null`, 5000);
      if (!halted) console.log(`  --    ${id}: RESTART (72): the firmware did not halt (its glue does not map it)`);
      else {
        const back = await until(`FM1Editor.bridge.target.connected && FM1Editor.bridge.target.port || null`, 20000);
        ok(back, `${id}: RESTART (72): the firmware halted, booted again from its flash; the port "${back}" is back`);
        if (!felucca(id)) {
          const again = await untilF(`/read:/.test(F.document.getElementById("status").textContent) && F.document.querySelectorAll("#dev-rows li.user").length === ${users} ? F.document.getElementById("status").textContent : null`, 20000, 250);
          ok(again, `${id}: after the reboot the manager found the port again and read ${users} user sound(s) back from the flash ("${again}")`);
        }
      }
      // ---- popped out: the manager in a tab of its own, over the BroadcastChannel to this player tab (live: managers/
      // live.html, opened by the panel's Pop out with its token, relays its sandbox)
      if (LIVE) { await livePopOut(cdp, ev, id); cdp.ws.close(); return; }
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
      if (generic) {
        // (its own read button in the tab; the firmware's replies reach the tab's manager through the player)
        await until2(`document.readyState === "complete" || null`, 5000);
        const n0 = await ev(`FM1Editor.bridge.stats.toManagers`);
        await ev2(`(() => { const b = document.getElementById("device-connect") || document.getElementById("btnReadSynth"); if (b) b.click(); return !!b; })()`);
        tabOk = await until(`FM1Editor.bridge.stats.toManagers > ${n0} ? (FM1Editor.bridge.stats.toManagers - ${n0}) + " replies" : null`, 12000);
      } else if (felucca(id)) {
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

// ---- a live editor popped out: the panel's Pop out opens managers/live.html (its token), which loads the author's page
// in its own sandbox and relays it to this player tab over the BroadcastChannel; the editor connects from there
async function livePopOut(cdp, ev, id) {
  const r = rows.find((x) => x.id === id && x.mode === "engine");
  await ev(`document.getElementById("ed-pop").click(), true`);
  let tid = null;
  for (let i = 0; i < 80 && !tid; i++) {
    const { targetInfos } = await cdp.send("Target.getTargets");
    const t = targetInfos.find((x) => x.type === "page" && /\/managers\/live\.html\?t=/.test(x.url));
    if (t) tid = t.targetId; else await sleep(100);
  }
  if (!ok(tid, `${id}: Pop out opened managers/live.html`)) { if (r) r.pop = "no tab"; return; }
  const { sessionId: s2 } = await cdp.send("Target.attachToTarget", { targetId: tid, flatten: true });
  const fc2 = frameContexts(cdp, s2);
  await fc2.start();
  await cdp.send("Runtime.enable", {}, s2);
  await cdp.send("Emulation.setDeviceMetricsOverride", { width: 1200, height: 900, deviceScaleFactor: 1, mobile: false }, s2).catch(() => {});
  const ev2 = async (expr) => { const x = await cdp.send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true }, s2); return x.exceptionDetails ? null : x.result.value; };
  const until2 = async (f, ms) => { const t0 = Date.now(); for (;;) { const v = await f().catch(() => null); if (v) return v; if (Date.now() - t0 > ms) return null; await sleep(150); } };
  const port = await until2(() => ev2(`window.__fm1live && __fm1live.port`), 15000);
  ok(port, `${id}: popped out live: the relay's page is bound to the player tab, port "${port}" (${await ev2(`document.getElementById("sub").textContent`)})`);
  const fe = (expr) => fc2.ev("((F) => (" + expr + "))(window)");
  let said;
  if (felucca(id)) {
    await until2(() => fe(`F.document.readyState === "complete" || null`), 8000);
    const out0 = await ev(`FM1Editor.bridge.stats.toManagers`);
    await fe(`(() => { const b = F.document.getElementById("connect") || [...F.document.querySelectorAll("button")].find((x) => /^\\s*connect/i.test(x.textContent)); if (b) b.click(); return true; })()`).catch(() => {});
    const gen = await fe(`!F.document.getElementById("state-t") && (!F.document.getElementById("version") || !F.document.getElementById("status"))`).catch(() => false);
    if (await fe(`!!(F.document.getElementById("unit") && F.document.getElementById("fm1"))`).catch(() => false)) {      // (FIZZ's manager)
      if (await until2(() => fe(`F.document.getElementById("fm1").dataset.state === "found" || null`), 10000)) await fe(`F.document.getElementById("fm1").click(), true`);
      else await fe(`(() => { F.document.getElementById("unit").click(); const b = F.document.getElementById("up-connect"); if (b) b.click(); return !!b; })()`);
      said = await until2(() => fe(`F.document.getElementById("unit").dataset.u === "fm1" && F.document.getElementById("unit").dataset.state === "on" ? F.document.getElementById("unit").textContent.trim() : null`), 15000);
      // (the player tab is in the background here and its timers slow: the scan's quiet probe answers, connect's may
      // time out; the scan's answer in the manager's log is the round trip through the relay)
      if (!said) said = await fe(`((F.document.getElementById("log") || {}).textContent || "").match(/answered FM-1_\\d+/)?.[0] || null`).catch(() => null);
    } else if (gen) said = await until2(() => ev(`FM1Editor.bridge.stats.toManagers > ${out0} ? "replies: " + (FM1Editor.bridge.stats.toManagers - ${out0}) : null`), 15000);
    else if (await fe(`!F.document.getElementById("version")`).catch(() => false))
      said = await until2(() => fe(`!F.document.getElementById("state-dot").classList.contains("off") && /·/.test(F.document.getElementById("state-t").textContent) ? F.document.getElementById("state-t").textContent : null`), 15000);
    else said = await until2(() => fe(`F.document.getElementById("version").textContent && /^(Connected|接続しました)$/.test(F.document.getElementById("status").textContent) ? F.document.getElementById("version").textContent : null`), 15000);
  } else said = await until2(() => fe(`/read:/.test(F.document.getElementById("status").textContent) ? F.document.getElementById("status").textContent : null`), 20000);
  const tabs = await ev(`[...FM1Editor.bridge.sessions.values()].filter((s) => s.via === "tab").length`);
  ok(said && tabs >= 1, `${id}: popped out live: the editor talks to the firmware from managers/live.html ("${said}", ${tabs} tab session(s))`);
  if (r) r.pop = said ? "ok" : "FAIL";
  const s = await cdp.send("Page.captureScreenshot", { format: "png" }, s2);
  writeFileSync(join(OUT, `editor-${id}-live-tab.png`), Buffer.from(s.data, "base64"));
  if (SHOTS) writeFileSync(join(SHOTS, `editor-${id}-live-tab.png`), Buffer.from(s.data, "base64"));
}

// the site's catalog lists the firmware on an .rfm1 (the dev catalog built with --fw-catalog)
const rfm1Of = (id) => { try { return JSON.parse(readFileSync(join(WEB, "site", "catalog.json"), "utf8")).firmwares.some((f) => f.id === id && f.engines.includes("rfm1")); } catch (_) { return false; } };
// the panel without a manager to show: an emulated firmware ("switch Engine to Native"), an emulated-only
// firmware whose editor is on its own site ("needs a real FM-1"); a phone upright (the panel under the FM-1)
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
      { name: "emu", url: "site/?fw=felucca&engine=emu", w: 1600, h: 900, want: /switch Engine to (Native|\.rfm1)/ },
      { name: "real", url: "site/?fw=zp12", w: 1600, h: 900, want: /needs a real FM-1/ },
      // with the .rfm1 files (build_catalog --fw-catalog): SLOOP's editor on its .rfm1, Mutable Dreams' only on its own
      // site (its editor is an app of its own, a #fragment: never loaded live)
      ...(rfm1Of("sloop") ? [{ name: "rfm1", url: "site/?fw=sloop", w: 1600, h: 900, want: /SLOOP editor/, frame: true }] : []),
      ...(rfm1Of("mutabledreams") ? [{ name: "rfm1-link", url: "site/?fw=mutabledreams&engine=rfm1", w: 1600, h: 900, want: /not hosted on this site yet/ }] : []),
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

// ---- PIGEON's sample loader (managers.json "pigeon"): a fresh PIGEON has no samples and is silent; the loader is how a
// visitor gives it some. Its own flow end to end: Connect FM1, open a bank, Send bank to FM1 (written and read back);
// then the keys play the bank's chops.
const PIGEON_BANK = "/vendor/FM1VST/tests/fixtures/pigeon/bank.kob";
async function pigeonSession() {
  const id = "pigeon";
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
    const fc = frameContexts(cdp, sessionId);
    if (LIVE) await fc.start();
    let liveF = LIVE;
    const evF = (expr) => (liveF ? fc.ev("((F) => (" + expr + "))(window)") : ev(FR + " " + expr));
    const untilF = async (expr, ms, step = 100) => { const t0 = Date.now(); for (;;) { let v; try { v = await evF(expr); } catch (_) { v = null; } if (v) return v; if (Date.now() - t0 > ms) return null; await sleep(step); } };
    const status = () => evF(`F.document.getElementById("status").textContent`);
    await S("Page.navigate", { url: `${BASE}site/?fw=${id}&engine=${ENGINE}` });
    const up = await until(`window.FM1Editor && FM1Editor.bridge.target.connected && FM1Editor.bridge.target.port`, 25000);
    ok(up === "PIGEON", `${id}: the ${ENGINE} engine runs, the in-page port is "${up}"`);
    if (!up) return;
    // the player's output, measured: the loudest sample over a window, through an AnalyserNode on the output
    await ev(`(() => { const pl = FM1Rec.o.getPlayer(), an = pl.ctx.createAnalyser(); an.fftSize = 2048; pl.out.connect(an);
      const b = new Float32Array(2048); window.__peak = 0;
      setInterval(() => { an.getFloatTimeDomainData(b); for (const v of b) window.__peak = Math.max(window.__peak, Math.abs(v)); }, 10); return true; })()`);
    const play = async () => {
      await ev(`window.__peak = 0, true`);
      for (const code of ["KeyA", "KeyS", "KeyD", "KeyF", "KeyG"]) {
        await ev(`dispatchEvent(new KeyboardEvent("keydown", { code: "${code}", bubbles: true })), true`);
        await sleep(350);
        await ev(`dispatchEvent(new KeyboardEvent("keyup", { code: "${code}", bubbles: true })), true`);
        await sleep(100);
      }
      await sleep(300);
      return ev(`window.__peak`);
    };
    await sleep(3500);                                   // (PIGEON's splash)
    const before = await play();
    ok(before < 0.002, `${id}: a fresh PIGEON is silent: no samples (keys A..G, peak ${before.toFixed(4)})`);

    await ev(`(() => { try { ${LIVE ? `localStorage.removeItem("${PREF(id)}")` : `localStorage.setItem("${PREF(id)}", "hosted")`}; } catch (_) {} return true; })()`);
    await ev(`document.getElementById("ed-btn").click()`);
    let liveSaid = null;
    if (LIVE && !(await ev(`FM1Editor.editor && FM1Editor.editor.live`))) {
      liveSaid = "no live page (managers.json live: false): the hosted copy";
      liveF = false;
      console.log(`  --    ${id}: ${liveSaid}`);
    } else if (LIVE) {
      const got = await until(`FM1Editor.showingLive && FM1Editor.live.ready ? "live" : FM1Editor.editor && FM1Editor.liveFail.has(FM1Editor.editor.live) ? "fail" : null`, 15000);
      liveSaid = got === "live" ? `live (${await ev(`FM1Editor.live.host`)})` : `fell back: ${got ? await ev(`FM1Editor.liveFail.get(FM1Editor.editor.live)`) : "no hello in 15 s"}`;
      ok(got === "live" || got === "fail", `${id}: the author's live page: ${liveSaid}`);
      if (got !== "live") { rows.push({ id, mode: "engine", live: liveSaid, port: up }); return; }
    }
    const loaded = await untilF(`!!(F && F.__fm1shim && F.document.readyState === "complete" && F.document.getElementById("send-help")
      && F.document.getElementById("send-help").textContent !== "Preparing the tool…")`, 30000);
    if (!loaded && LIVE) console.log("    live frame:", await evF(`JSON.stringify({ shim: typeof F.__fm1shim, ready: F.document.readyState, help: (F.document.getElementById("send-help") || {}).textContent, offline: (F.document.getElementById("offline") || {}).textContent, status: (F.document.getElementById("status") || {}).textContent })`).catch((e) => e.message));
    ok(loaded, `${id}: the Editor panel shows the PIGEON sample loader, the shim in it, ready (${loaded ? await evF(`F.document.getElementById("offline").textContent`) : "not ready"})`);
    if (!loaded) return;
    await evF(`(F.confirm = () => true, F.document.getElementById("connect").click(), true)`);
    const conn = await untilF(`/PIGEON connected/.test(F.document.getElementById("status").textContent) ? F.document.getElementById("firmware-version").textContent : null`, 20000);
    ok(conn, `${id}: Connect FM1: negotiated through the shim, "${conn || await status()}" on "${await evF(`F.document.getElementById("connection").textContent`)}"`);
    if (!conn) return;
    // (the bank: read by the player page, this site; the live page's own fetch would be cross-origin to this test server)
    const bank64 = await ev(`(async () => { const b = new Uint8Array(await (await fetch("${PIGEON_BANK}")).arrayBuffer()); let s = ""; for (const v of b) s += String.fromCharCode(v); return btoa(s); })()`);
    await evF(`(async () => { const s = atob("${bank64}"), b = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i); const dt = new F.DataTransfer();
      dt.items.add(new F.File([b], "bank.kob")); const i = F.document.getElementById("open"); i.files = dt.files; i.dispatchEvent(new F.Event("change")); return true; })()`);
    const opened = await untilF(`!F.document.getElementById("upload").disabled || null`, 20000);
    ok(opened, `${id}: FM1VST's test bank opened in the loader ("${await status()}")`);
    if (!opened) return;
    const t0 = Date.now();
    await evF(`(F.document.getElementById("upload").click(), true)`);
    await untilF(`!F.document.getElementById("send-review").hidden || null`, 5000);
    await evF(`(F.document.getElementById("send-confirm").click(), true)`);
    const sent = await untilF(`/Bank sent and verified/.test(F.document.getElementById("status").textContent) || F.document.getElementById("status").parentElement.classList.contains("error") || null`, 180000, 250);
    const said = await status();
    ok(sent && /Bank sent and verified/.test(said), `${id}: Send bank to FM1: "${said}" (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
    const after = await play();
    ok(after > 0.01, `${id}: the keys play the bank's chops now (peak ${after.toFixed(4)})`);
    const s = await S("Page.captureScreenshot", { format: "png" });
    writeFileSync(join(OUT, `editor-${id}-engine.png`), Buffer.from(s.data, "base64"));
    if (SHOTS) writeFileSync(join(SHOTS, `editor-${id}-engine.png`), Buffer.from(s.data, "base64"));
    console.log(`  shot  ${join(OUT, `editor-${id}-engine.png`)}`);
    rows.push({ id, mode: "engine", live: liveSaid, port: up, info: !!conn, get: !!sent, put: after > 0.01, ...(await ev(`({ toEngine: FM1Editor.bridge.stats.toEngine, toManagers: FM1Editor.bridge.stats.toManagers })`)) });
    cdp.ws.close();
  } finally {
    for (const l of logs.slice(0, 8)) console.log("    " + l);
    p.kill("SIGTERM");
    await new Promise((res) => { p.once("exit", res); setTimeout(res, 3000); });
  }
}

const ids = MODE === "notes" ? [] : IDS.length ? IDS : INDEX.managers.map((m) => m.id);
if (MODE === "notes" || (MODE === "both" && !IDS.length && !LIVE)) { console.log("== the panel's notes"); try { await notes(); } catch (e) { ok(false, `notes: ${e.message}`); } }
for (const id of ids) {
  const core = INDEX.managers.find((m) => m.id === id);
  if (!core && !LIVE) { ok(false, `${id}: no such hosted manager`); continue; }
  if (ENGINE === "native" && !existsSync(join(WEB, "build", "native", `${id}.wasm`))) { ok(false, `${id}: no web/build/native/${id}.wasm (web/engines/native/build.sh)`); continue; }
  for (const mode of LIVE ? ["engine"] : MODE === "both" ? ["double", "engine"] : [MODE]) {
    console.log(`== ${id} (${mode})`);
    if (id === "pigeon" && mode === "double") { console.log("  --    pigeon: no double (the loader has no simulator of its own)"); continue; }
    try { await (id === "pigeon" ? pigeonSession() : session(id, mode)); } catch (e) { ok(false, `${id} (${mode}): ${e.message}`); }
  }
}
server.kill("SIGTERM");
console.log(`\n| manager | mode |${LIVE ? " live page |" : ""} port | INFO | backup GET | PUT + get-back | pop out | messages in / out |\n|---|---|${LIVE ? "---|" : ""}---|---|---|---|---|---|`);
for (const r of rows) console.log(`| ${r.id} | ${r.mode} |${LIVE ? ` ${r.live} |` : ""} ${r.port} | ${r.info ? "answered" : "no reply"} | ${r.get ? "ok" : "-"} | ${r.put ? "ok" : r.slider ? "slider read-back" : "-"} | ${r.pop || "-"} | ${r.toEngine ?? "-"} / ${r.toManagers ?? "-"} |`);
console.log(`\n${checks - fails} / ${checks} checks passed`);
process.exit(fails ? 1 : 0);
