// SPDX-License-Identifier: GPL-3.0-only
// The recorder in the header: a round record button (Shift+R), while recording an elapsed time and a small level
// meter, and the Takes popover (play, save, rename, delete). The PCM comes from the worklet (worklet/host.js
// capture: the engine's own output, before the page's volume) and becomes a WAV file here (js/recorder.js).
//
// Takes are kept in this page's memory for the session (one Blob each, which the browser may page out to disk), not in
// IndexedDB: a take may be 500 MB, more than a phone's quota for a site, and the visitor saves the ones to keep. A
// page with unsaved takes asks before it closes. A take ends cleanly before anything replaces the engine (another
// firmware, engine or clock, a restart: audio.js emits beforeLoad), at the 30-minute cap, or on Stop.
//
//   const rec = new RecUI({ getPlayer, ensureRunning, nameParts, toast, settings })
//     getPlayer()      the audio.js Player (or null)
//     ensureRunning()  starts the audio (a user gesture), resolves true when it runs
//     nameParts()      { id, version } of the running firmware, for the file name
//     settings()       { bits: 24 | 16, countIn: bool }
//   rec.toggle(); rec.onRec(m) (the Player's rec event); rec.beforeLoad() (its beforeLoad event)
import { TakeWriter, chunkFrames, MAX_TAKE_S, WARN_TAKE_S, takeFileName, clock, sizeText } from "./recorder.js";

const $ = (id) => document.getElementById(id);
const COUNT_IN_S = 3;
const STOP_GRACE_MS = 4000;   // (a press this soon after a take ended by itself is taken as its Stop)
const SVG = (d, fill = false) => `<svg viewBox="0 0 16 16" aria-hidden="true" fill="${fill ? "currentColor" : "none"}" stroke="${fill ? "none" : "currentColor"}" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">${d}</svg>`;
const IC = {
  play: SVG('<path d="M5 3.2v9.6L12.6 8z"/>', true),
  pause: SVG('<rect x="4" y="3.5" width="2.8" height="9" rx=".6"/><rect x="9.2" y="3.5" width="2.8" height="9" rx=".6"/>', true),
  save: SVG('<path d="M8 2.5v7.5M4.8 7 8 10.2 11.2 7M3 12.5h10"/>'),
  rename: SVG('<path d="M10.5 3 13 5.5 6 12.5H3.5V10z"/>'),
  del: SVG('<path d="M3.5 4.5h9M6.5 4.5V3h3v1.5M5 4.5l.6 8.5h4.8l.6-8.5"/>'),
  close: SVG('<path d="M4 4l8 8M12 4l-8 8"/>'),
};
const dbfs = (p) => (p > 0 ? 20 * Math.log10(p) : -120);

export class RecUI {
  constructor(o) {
    this.o = o;
    this.phase = "idle";          // idle | starting (the audio) | count (the count-in) | rec | stopping
    this.cur = null;              // the take being recorded: { id, writer, why, startFrame, endFrame, frames, warned, ... }
    this.seq = 0;
    this.takes = [];              // newest first: { key, name, blob, url, frames, sampleRate, bits, clips, size, saved, startFrame, endFrame }
    this.audio = new Audio();
    this.audio.preload = "none";
    this.playing = null;
    this.audio.addEventListener("ended", () => { this.playing = null; this.renderTakes(); });
    this.audio.addEventListener("pause", () => { if (this.playing && this.audio.paused) { this.playing = null; this.renderTakes(); } });
    this.meter = [0, 0];
    this.btn = $("rec-btn"); this.time = $("rec-time"); this.meterEl = $("rec-meter");
    this.takesBtn = $("takes-btn"); this.pop = $("takes"); this.live = $("rec-live");
    this.btn.addEventListener("click", () => this.toggle());
    this.takesBtn.addEventListener("click", (e) => { e.stopPropagation(); this.pop.hidden ? this.openTakes() : this.closeTakes(); });
    addEventListener("keydown", (e) => {
      if (e.code !== "KeyR" || !e.shiftKey || e.metaKey || e.ctrlKey || e.altKey || e.repeat) return;
      const t = e.target;
      if (t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
      e.preventDefault();
      this.toggle();
    });
    addEventListener("keydown", (e) => { if (e.key === "Escape" && !this.pop.hidden) { e.stopPropagation(); this.closeTakes(); this.takesBtn.focus(); } });
    document.addEventListener("pointerdown", (e) => { if (!this.pop.hidden && !this.pop.contains(e.target) && !this.takesBtn.contains(e.target)) this.closeTakes(); });
    addEventListener("resize", () => { if (!this.pop.hidden) this.place(); });
    addEventListener("beforeunload", (e) => { if (this.phase === "rec" || this.takes.some((t) => !t.saved)) { e.preventDefault(); e.returnValue = ""; } });
    $("rec").hidden = false;
    this.render();
  }
  get recording() { return this.phase === "rec"; }
  get busy() { return this.phase !== "idle"; }   // (a take is starting, counting in, recording or ending)
  idle() { if (this.o.onIdle) setTimeout(() => this.o.onIdle(), 0); }
  say(text) { this.live.textContent = ""; setTimeout(() => { this.live.textContent = text; }, 30); }

  // ------------------------------------------------------------ take ---
  async toggle() {
    if (this.phase === "count") return this.cancelCount();
    if (this.phase === "rec") return this.stop("stop");
    if (this.phase !== "idle") return;
    // a take that ended by itself (the engine restarted, the cap) a moment ago: this press meant Stop, not a new take
    if (this.endedAt && performance.now() - this.endedAt < STOP_GRACE_MS) {
      this.endedAt = 0;
      this.say("The take had already stopped");
      this.o.toast("That take had already stopped: it is in Takes. Press again to record a new one.", "ok");
      return;
    }
    this.phase = "starting";   // (the audio may start now: this click is the gesture)
    this.render();
    let ok = false;
    try { ok = await this.o.ensureRunning(); } catch (_) { ok = false; }
    if (this.phase !== "starting") return;
    if (!ok || !this.o.getPlayer() || !this.o.getPlayer().sampleRate) {
      this.phase = "idle"; this.render(); this.idle();
      this.o.toast("The sound could not start, so there is nothing to record yet.", "bad");
      return;
    }
    if (this.o.settings().countIn) {
      this.phase = "count";
      this.count = COUNT_IN_S;
      this.countTimer = setInterval(() => {
        if (--this.count > 0) return this.render();
        clearInterval(this.countTimer); this.countTimer = 0;
        this.begin();
      }, 1000);
      this.render();
      this.say("Recording in 3 seconds");
    } else this.begin();
  }
  cancelCount() { clearInterval(this.countTimer); this.countTimer = 0; this.phase = "idle"; this.render(); this.say("Recording cancelled"); this.idle(); }
  begin() {
    const p = this.o.getPlayer(), sr = p.sampleRate, s = this.o.settings(), np = this.o.nameParts();
    const id = ++this.seq;
    this.cur = { id, writer: new TakeWriter({ sampleRate: sr, bits: s.bits === 16 ? 16 : 24 }), why: null, frames: 0, warned: false,
                 name: takeFileName(np.id, np.version).replace(/\.wav$/, ""), startFrame: -1, endFrame: -1, ctxStart: p.ctx.currentTime };
    this.meter = [0, 0];
    p.recStart({ id, chunkFrames: chunkFrames(sr), maxFrames: Math.round(MAX_TAKE_S * sr) });
    this.phase = "rec";
    this.render();
    this.say("Recording");
  }
  // why: "stop" (the visitor), "switch" (the engine is being replaced)
  stop(why) {
    if (this.phase === "count") return this.cancelCount();
    if (this.phase !== "rec") return;
    this.cur.why = why;
    this.cur.ctxStop = this.o.getPlayer().ctx.currentTime;
    this.phase = "stopping";
    this.o.getPlayer().recStop();
    this.render();
    const id = this.cur.id;
    clearTimeout(this.stopTimer);   // (a worklet that never answers: the take ends with what came)
    this.stopTimer = setTimeout(() => { if (this.cur && this.cur.id === id) this.finish("lost"); }, 2500);
  }
  beforeLoad() { if (this.phase === "rec") this.stop("switch"); else if (this.phase === "count") this.cancelCount(); }

  onRec(m) {
    const p = this.o.getPlayer(), cur = this.cur;
    if (m.type === "recChunk") {
      if (cur && m.id === cur.id) {
        cur.writer.push(m.l, m.r, m.frames);
        cur.frames = m.total;
        if (m.startFrame >= 0) { cur.startFrame = m.startFrame; cur.endFrame = m.endFrame; }
      }
      if (p) p.recBuf(m.l, m.r);   // (the buffers go back to the worklet for its next chunk)
      if (cur && m.id === cur.id && m.final) this.finish(m.reason);
      return;
    }
    if (m.type === "recLevel" && cur && m.id === cur.id && this.phase === "rec") {
      cur.frames = m.frames;
      // the meter: the peak, falling back at about 24 dB a second
      for (let c = 0; c < 2; c++) this.meter[c] = Math.max(m.peak[c], this.meter[c] * 0.82);
      if (!cur.warned && cur.frames >= WARN_TAKE_S * cur.writer.sampleRate) {
        cur.warned = true;
        this.o.toast(`This take stops by itself at ${MAX_TAKE_S / 60} minutes (${clock((MAX_TAKE_S - WARN_TAKE_S))} to go).`, "bad", 8000);
      }
      this.renderLive();
    }
  }
  // the final chunk is in: the WAV, a take
  finish(reason) {
    const cur = this.cur;
    clearTimeout(this.stopTimer);
    this.cur = null; this.phase = "idle";
    const w = cur.writer;
    this.idle();
    if (!w.frames) { this.render(); this.say("Stopped, nothing recorded"); return; }
    const blob = new Blob(w.parts(), { type: "audio/wav" });
    w.chunks = [];   // (the Blob holds the PCM now)
    const take = { key: cur.id, name: cur.name, blob, url: URL.createObjectURL(blob), frames: w.frames, sampleRate: w.sampleRate, bits: w.bits,
                   clips: w.clips, size: blob.size, saved: false, startFrame: cur.startFrame, endFrame: cur.endFrame,
                   ctxSpan: (cur.ctxStop ?? this.o.getPlayer()?.ctx.currentTime ?? 0) - cur.ctxStart };
    this.takes.unshift(take);
    this.render();
    this.say("Stopped, take saved");
    const why = cur.why || reason;
    this.endedAt = why === "stop" ? 0 : performance.now();
    if (why === "cap") this.o.toast(`The take stopped at the ${MAX_TAKE_S / 60}-minute cap: it is in Takes.`, "bad", 8000);
    else if (why === "switch" || why === "engine") this.o.toast("The take ended before the firmware restarted: it is in Takes.", "ok");
    else if (why === "error" || why === "lost") this.o.toast("The engine stopped, so the take ended: it is in Takes.", "bad");
    if (why === "stop") this.openTakes();
    else if (!this.pop.hidden) this.renderTakes();
  }

  // ------------------------------------------------------------ header ---
  render() {
    const ph = this.phase, on = ph === "rec" || ph === "stopping";
    this.btn.disabled = ph === "starting" || ph === "stopping";
    document.body.classList.toggle("recording", on);
    document.body.classList.toggle("has-takes", this.takes.length > 0);
    this.btn.classList.toggle("on", on);
    this.btn.classList.toggle("count", ph === "count");
    this.btn.setAttribute("aria-pressed", String(on || ph === "count"));
    const label = ph === "count" ? "Cancel the count-in" : on ? "Stop recording" : "Record";
    this.btn.setAttribute("aria-label", label);
    this.btn.title = `${label} (Shift+R)`;
    this.time.hidden = !(on || (ph === "count" && this.countTimer));
    this.meterEl.hidden = !on;
    if (ph === "count") this.time.textContent = this.countTimer ? `-${this.count}` : "";
    this.time.classList.toggle("count", ph === "count");
    this.takesBtn.hidden = !this.takes.length;
    $("takes-n").textContent = String(this.takes.length);
    if (on) this.renderLive();
  }
  renderLive() {
    const cur = this.cur;
    if (!cur) return;
    this.time.textContent = clock(cur.frames / cur.writer.sampleRate);
    const bars = this.meterEl.children;
    for (let c = 0; c < 2; c++) {
      const db = dbfs(this.meter[c]), pct = Math.max(0, Math.min(100, ((db + 48) / 48) * 100));
      bars[c].style.setProperty("--lvl", pct.toFixed(1) + "%");
      bars[c].className = this.meter[c] >= 1 ? "clip" : db > -6 ? "hot" : "";
    }
  }

  // ------------------------------------------------------------ takes ---
  openTakes() {
    this.pop.hidden = false;
    this.takesBtn.setAttribute("aria-expanded", "true");
    this.renderTakes();
    this.place();
  }
  closeTakes() {
    this.pop.hidden = true;
    this.takesBtn.setAttribute("aria-expanded", "false");
    this.renaming = null; this.confirmDel = null;
  }
  place() {
    const b = this.takesBtn.getBoundingClientRect(), w = Math.min(360, innerWidth - 16);
    const left = Math.max(8, Math.min(innerWidth - w - 8, b.left + b.width / 2 - w / 2));
    Object.assign(this.pop.style, { left: left + "px", top: b.bottom + 8 + "px", width: w + "px" });
  }
  renderTakes() {
    if (this.pop.hidden) return;
    const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
    const btn = (cls, html, label, fn) => { const b = el("button", "take-btn " + cls); b.innerHTML = html; b.setAttribute("aria-label", label); b.title = label; b.addEventListener("click", fn); return b; };
    const head = el("div", "takes-head");
    head.append(el("b", null, "Takes"), el("span", "takes-sub", "kept until you close this page"),
      btn("ghost", IC.close, "Close", () => { this.closeTakes(); this.takesBtn.focus(); }));
    const list = el("ul", "takes-list");
    for (const t of this.takes) {
      const li = el("li", "take" + (this.playing === t.key ? " playing" : ""));
      const top = el("div", "take-top");
      top.append(btn("play", this.playing === t.key ? IC.pause : IC.play, this.playing === t.key ? `Pause ${t.name}` : `Play ${t.name}`, () => this.play(t)));
      if (this.renaming === t.key) {
        const inp = el("input", "text take-name-in");
        inp.value = t.name; inp.setAttribute("aria-label", "Take name"); inp.maxLength = 120;
        const done = (keep) => { if (keep) { const v = inp.value.trim().replace(/[\\/:*?"<>|]+/g, "_"); if (v) t.name = v; } this.renaming = null; this.renderTakes(); };
        inp.addEventListener("keydown", (e) => { if (e.key === "Enter") done(true); else if (e.key === "Escape") { e.stopPropagation(); done(false); } });
        inp.addEventListener("blur", () => { if (this.renaming === t.key) done(true); });
        top.append(inp);
        setTimeout(() => { inp.focus(); inp.select(); }, 0);
      } else top.append(el("span", "take-name", t.name));
      const meta = el("div", "take-meta");
      meta.append(el("span", null, `${clock(t.frames / t.sampleRate)} · ${sizeText(t.size)} · ${t.bits}-bit WAV`));
      if (t.clips) meta.append(el("span", "take-clips", `${t.clips.toLocaleString()} clipped sample${t.clips === 1 ? "" : "s"}`));
      const acts = el("div", "take-acts");
      const save = btn("pill primary", IC.save + "<span>Save</span>", `Save ${t.name}.wav`, () => this.save(t));
      acts.append(save,
        btn("pill", IC.rename + "<span>Rename</span>", `Rename ${t.name}`, () => { this.renaming = t.key; this.renderTakes(); }),
        this.confirmDel === t.key
          ? btn("pill danger-fill", "<span>Delete it</span>", `Delete ${t.name} for good`, () => this.remove(t))
          : btn("pill danger", IC.del + "<span>Delete</span>", `Delete ${t.name}`, () => { this.confirmDel = t.key; this.renderTakes(); clearTimeout(this.delTimer); this.delTimer = setTimeout(() => { this.confirmDel = null; this.renderTakes(); }, 3000); }));
      if (t.saved) meta.append(el("span", "take-saved", "saved"));
      li.append(top, meta, acts);
      list.append(li);
    }
    this.pop.replaceChildren(head, list);
  }
  play(t) {
    if (this.playing === t.key) { this.audio.pause(); return; }
    this.playing = t.key;
    if (this.audio.src !== t.url) this.audio.src = t.url;
    else this.audio.currentTime = this.audio.ended ? 0 : this.audio.currentTime;
    this.audio.play().catch(() => { this.playing = null; this.renderTakes(); });
    this.renderTakes();
  }
  // a download; on a touch device the share sheet where it takes files (iOS Safari: Save to Files, AirDrop...)
  save(t) {
    const name = t.name + ".wav";
    const done = () => { t.saved = true; this.renderTakes(); };
    const download = () => {
      const a = Object.assign(document.createElement("a"), { href: t.url, download: name });
      document.body.append(a); a.click(); a.remove();
      done();
    };
    let file = null;
    try { file = new File([t.blob], name, { type: "audio/wav" }); } catch (_) { file = null; }
    const coarse = typeof matchMedia === "function" && matchMedia("(pointer: coarse)").matches;
    if (file && coarse && navigator.canShare && navigator.share) {
      let can = false;
      try { can = navigator.canShare({ files: [file] }); } catch (_) { can = false; }
      if (can) { navigator.share({ files: [file], title: t.name }).then(done, (e) => { if (!e || e.name !== "AbortError") download(); }); return; }
    }
    download();
  }
  remove(t) {
    if (this.playing === t.key) { this.audio.pause(); this.audio.removeAttribute("src"); this.playing = null; }
    URL.revokeObjectURL(t.url);
    this.takes = this.takes.filter((x) => x !== t);
    this.confirmDel = null;
    this.render();
    this.say(`Deleted ${t.name}`);
    if (!this.takes.length) { this.closeTakes(); return; }
    this.renderTakes();
  }
}
