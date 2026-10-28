// SPDX-License-Identifier: GPL-3.0-only
// The page's audio start and output in Node, on a fake AudioContext and fake devices:
//   - Player.start() resumes the context inside the user gesture (before its first await: iOS Safari forgets the
//     gesture there), asks iOS for the "playback" audio session (the ring/silent switch), sets up the worklet once;
//   - the output: the FM-1 found among the sound outputs (sound-out.js), Chrome's microphone step to name them,
//     Firefox's chooser, the choice kept, followed when the FM-1 is unplugged and plugged in again.
//   node web/site/test-audio.mjs
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`${what.padEnd(92)} ${c ? "ok" : "FAIL"}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- a browser, faked: AudioContext (resume() only starts it inside a gesture, as WebKit), the worklet node, an iPhone
let gesture = false;
const log = [];
class FakeAC {
  constructor(o) { this.opts = o; this.state = "suspended"; this.sampleRate = 48000; this.currentTime = 0; this.sinkId = ""; this.destination = { id: "dest" }; FakeAC.made++; log.push("new"); }
  get audioWorklet() { return { addModule: async () => { log.push("addModule"); await sleep(5); } }; }
  resume() { log.push(gesture ? "resume(gesture)" : "resume"); if (gesture && this.state !== "running") { this.state = "running"; if (this.onstatechange) this.onstatechange(); } return Promise.resolve(); }
  createBufferSource() { return { connect() {}, start() { log.push("blip"); } }; }
  createBuffer() { return {}; }
  createGain() { return { gain: { value: 1, setTargetAtTime() {} }, connect(d) { log.push("out->" + d.id); }, disconnect() {} }; }
  async setSinkId(id) { log.push("setSinkId:" + id); this.sinkId = id; }
}
FakeAC.made = 0;
globalThis.window = globalThis;
globalThis.AudioContext = FakeAC;
globalThis.AudioWorkletNode = class { constructor() { this.port = { postMessage() {} }; } connect() {} };
globalThis.document = { baseURI: "https://remotefm1.com/" };
const audioTags = [];
globalThis.Audio = class { constructor(src) { this.src = src; this.paused = true; this.attrs = {}; audioTags.push(this); } setAttribute(k, v) { this.attrs[k] = v; } play() { this.paused = false; return Promise.resolve(); } };
const iphone = (session) => Object.defineProperty(globalThis, "navigator", { configurable: true, value: {
  userAgent: "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Mobile/15E148 Safari/604.1",
  platform: "iPhone", maxTouchPoints: 5, ...(session ? { audioSession: session } : {}) } });

const { Player, isIOS, sinkSupport } = await import("./js/audio.js");
const { SoundOut, isFm1Output, pickFm1, namesHidden, NO_FM1_TEXT } = await import("./js/sound-out.js");
const cat = { paths: { engines: {} } };

// ---- the start, in a gesture
{
  const session = { type: "auto" };
  iphone(session);
  ok(isIOS(), "isIOS: an iPhone's Safari");
  const p = new Player(cat);
  log.length = 0;
  gesture = true;
  const run = p.start();          // a tap's click handler: synchronous up to the first await ...
  gesture = false;                // ... and the gesture is gone after it
  const ran = await run;
  ok(ran && p.ctx.state === "running", "start(): the context runs though the gesture ended at the first await");
  ok(log.indexOf("resume(gesture)") >= 0 && log.indexOf("resume(gesture)") < log.indexOf("addModule"), "start(): resume() inside the gesture, before the worklet module loads");
  ok(log.includes("blip"), "start(): a one-sample sound started in the gesture (wakes iOS's output)");
  ok(session.type === "playback", "start(): iOS audio session 'playback' (plays with the ring/silent switch on silent)");
  ok(audioTags.length === 0, "start(): no silent <audio> where navigator.audioSession exists");
  ok(log.includes("out->dest"), "start(): the page's gain to the system output");
  const again = await p.start();
  ok(again && log.filter((l) => l === "addModule").length === 1 && FakeAC.made === 1, "start() again: the same context, the worklet module added once");
}
{
  // a touch's pointerdown is no gesture on iOS: the context stays held back until the pointerup / click
  iphone(null);
  const p = new Player(cat);
  log.length = 0;
  const first = await p.start();
  ok(!first && p.ctx.state === "suspended", "start() without a gesture: resolves false (after at most 1.5 s), the context held back");
  ok(audioTags.length === 1 && audioTags[0].attrs.playsinline === "" && !audioTags[0].paused, "iOS without audioSession: a silent looping <audio> plays (playback, not ambient)");
  gesture = true; p.unlock(); gesture = false;
  ok(p.ctx.state === "running" && FakeAC.made === 2, "unlock() in the next gesture: the same context runs");
  ok(await p.start(), "start() then: true");
}
{
  // two starts at once (the start button and a key): one worklet setup, both see it done
  iphone({ type: "auto" });
  const p = new Player(cat);
  log.length = 0;
  gesture = true;
  const a = p.start(), b = p.start();
  gesture = false;
  const [ra, rb] = await Promise.all([a, b]);
  ok(ra && rb && p.node && log.filter((l) => l === "addModule").length === 1, "two start()s at once: one worklet setup, the node there for both");
}

// ---- the output device on the Player (Chrome: AudioContext.setSinkId)
{
  Object.defineProperty(globalThis, "navigator", { configurable: true, value: { userAgent: "Mozilla/5.0 (Macintosh) Chrome/141", platform: "MacIntel", maxTouchPoints: 0 } });
  ok(sinkSupport() === "context", "sinkSupport: 'context' where AudioContext has setSinkId");
  const p = new Player(cat);
  await p.setSink("fm1-id");
  ok(!p.ctx && p.sinkId === "fm1-id", "setSink() before the start: kept for later");
  log.length = 0;
  gesture = true; const s = p.start(); gesture = false; await s;
  await p.applySink();
  ok(p.ctx.sinkId === "fm1-id" && log.includes("setSinkId:fm1-id"), "applySink() after the start: the context's output is the FM-1");
  log.length = 0;
  await p.applySink();
  ok(!log.some((l) => l.startsWith("setSinkId")), "applySink() again: nothing to do");
  await p.setSink("");
  ok(p.ctx.sinkId === "" && p.applied === "", "setSink(''): the system's output again");
}

// ---- which output is the FM-1
{
  const d = (label, deviceId = label, kind = "audiooutput") => ({ kind, deviceId, label });
  ok(isFm1Output(d("FM-1")) && isFm1Output(d("FM-1 (1209:0001)")) && isFm1Output(d("RemoteFM1")), "isFm1Output: 'FM-1', 'FM-1 (1209:0001)', 'RemoteFM1'");
  ok(!isFm1Output(d("Default - FM-1", "default")) && !isFm1Output(d("FM-1", "x", "audioinput")) && !isFm1Output(d("FM-10")) && !isFm1Output(d("MacBook Pro Speakers")) && !isFm1Output(d("FM-1", "")),
    "isFm1Output: not the default alias, an input, 'FM-10', the Mac's speakers, an unnamed one");
  ok(pickFm1([d("MacBook Pro Speakers"), d("FM-1", "abc")]).deviceId === "abc", "pickFm1: the FM-1 among the outputs");
  ok(namesHidden([{ kind: "audiooutput", deviceId: "", label: "" }]) && !namesHidden([d("FM-1")]), "namesHidden: Chrome's unnamed list before the microphone permission");
}

// ---- SoundOut: the choice, Chrome's way, Firefox's way, Safari's line, plug and unplug
function world({ fm1 = true, allow = true, perm = false } = {}) {
  const w = {
    perm, fm1, allow, gum: 0, stopped: 0, change: null, chosen: null,
    devices() {
      const all = [{ kind: "audiooutput", deviceId: "default", label: "Default - MacBook Pro Speakers" }, { kind: "audiooutput", deviceId: "spk", label: "MacBook Pro Speakers" }];
      if (w.fm1) all.push({ kind: "audiooutput", deviceId: "fm1", label: "FM-1" }, { kind: "audioinput", deviceId: "fm1in", label: "FM-1" });
      return w.perm ? all : all.map((x) => ({ ...x, deviceId: "", label: "" }));
    },
    media: {
      enumerateDevices: async () => w.devices(),
      getUserMedia: async () => { w.gum++; if (!w.allow) throw new Error("NotAllowedError"); w.perm = true; return { getTracks: () => [{ stop: () => w.stopped++ }] }; },
      selectAudioOutput: async () => { w.chosen = { kind: "audiooutput", deviceId: "fm1", label: "FM-1" }; return w.chosen; },
      addEventListener: (t, cb) => { if (t === "devicechange") w.change = cb; },
    },
    player: { ctx: {}, sinks: [], async setSink(id) { this.sinks.push(id); } },
    prefs: {}, saves: 0,
  };
  return w;
}
const mk = (w, support = "context") => new SoundOut({ player: w.player, media: w.media, prefs: w.prefs, save: () => w.saves++, support });
{
  const w = world(), so = mk(w);
  ok(so.view().want === "system" && /microphone/.test(so.view().sub), "SoundOut: the system's output by default; the row says Chrome asks for the microphone");
  await so.useFm1();
  ok(w.gum === 1 && w.stopped === 1, "useFm1 (Chrome): the microphone asked once to name the outputs, its track closed at once");
  ok(w.player.sinks.at(-1) === "fm1" && so.on && w.prefs.soundOut === "fm1" && w.saves === 1, "useFm1: the sound to the FM-1's output, the choice kept");
  ok(/FM-1: MASTER/.test(so.view().sub), "SoundOut view: 'FM-1: MASTER is its volume'");
  await so.useFm1();
  ok(w.gum === 1, "useFm1 again: no second microphone prompt (the outputs are named now)");
  w.fm1 = false; await w.change(); await sleep(0);
  ok(w.player.sinks.at(-1) === "" && !so.on && so.missing && w.prefs.soundOut === "fm1", "FM-1 unplugged: the system's output, the choice kept");
  ok(/until the FM-1 is plugged in/.test(so.view().sub), "SoundOut view: 'the computer's output until the FM-1 is plugged in'");
  w.fm1 = true; await w.change(); await sleep(0);
  ok(w.player.sinks.at(-1) === "fm1" && so.on, "FM-1 plugged in again: back to its output");
  await so.useSystem();
  ok(w.player.sinks.at(-1) === "" && !so.on && !("soundOut" in w.prefs), "useSystem: the system's output, the choice forgotten");
  w.fm1 = false; await w.change(); w.fm1 = true; await w.change(); await sleep(0);
  ok(w.player.sinks.at(-1) === "" && !so.on, "after useSystem: plugging the FM-1 in changes nothing");
}
{
  const w = world({ fm1: false }), so = mk(w);
  let err = null; try { await so.useFm1(); } catch (e) { err = e; }
  ok(err && err.message === NO_FM1_TEXT && !w.prefs.soundOut && !w.player.sinks.length, "useFm1 with no FM-1 output: says so, nothing changes");
  const w2 = world({ allow: false }), so2 = mk(w2);
  err = null; try { await so2.useFm1(); } catch (e) { err = e; }
  ok(err && /microphone/.test(err.message) && /sound output/.test(err.message) && !w2.prefs.soundOut, "useFm1, microphone refused: says why, and the system-output way");
}
{
  // a later visit: the choice kept, the permission still there: the FM-1 at once, no prompt
  const w = world({ perm: true }); w.prefs.soundOut = "fm1";
  const so = mk(w);
  await so.apply();
  ok(so.on && w.gum === 0 && w.player.sinks.at(-1) === "fm1", "apply() on a later visit: the FM-1's output, no prompt");
  // ... the permission gone: the system's output and a line asking for the click again
  const w2 = world({ perm: false }); w2.prefs.soundOut = "fm1";
  const so2 = mk(w2);
  await so2.apply();
  ok(!so2.on && so2.hidden && w2.gum === 0 && /Choose FM-1 again/.test(so2.view().sub), "apply() without the permission: no prompt, the row asks for the click again");
}
{
  const w = world({ perm: true }), so = mk(w, "element");
  await so.useFm1();
  ok(w.chosen && w.prefs.soundOutId === "fm1" && so.on && w.gum === 0, "useFm1 (Firefox): its chooser, the device kept, no microphone");
  const w2 = world({ perm: true }); w2.prefs.soundOut = "fm1"; w2.prefs.soundOutId = "fm1";
  const so2 = mk(w2, "element"); await so2.apply();
  ok(so2.on && w2.player.sinks.at(-1) === "fm1", "apply() (Firefox): the kept device");
}
{
  const w = world(), so = new SoundOut({ player: w.player, media: w.media, prefs: w.prefs, support: null });
  let err = null; try { await so.useFm1(); } catch (e) { err = e; }
  ok(so.view().support === null && /System Settings > Sound > Output|plugged in by USB/.test(so.view().sub) && err, "Safari (no setSinkId): a line on picking FM-1 as the system's output");
}

console.log(`\n${checks - fails}/${checks} checks passed`);
process.exit(fails ? 1 : 0);
