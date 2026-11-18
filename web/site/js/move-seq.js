// SPDX-License-Identifier: MPL-2.0
// The Ableton Move's sequencer in the web player: rfm_move's clock (rfm_move_seq_advance) driven by the AUDIO clock.
// No DOM: Node runs it (web/site/test.mjs). js/move-link.js owns one.
//
// THE CLOCK. The core lives on the page's main thread (the controller's pads, LEDs and screen do), so the page
// advances it ahead of the audio: every pump() (a 10 ms timer, plus every Move input) the core is advanced, in blocks
// of BLOCK frames at the context's rate, up to the audio clock's frame (AudioContext.currentTime x sampleRate, the
// worklet's currentFrame) plus LEAD_S. Each action the core gives carries its frame in the block, so it is due at the
// absolute frame pos + frame; MIDI goes to the worklet stamped with it ({type: "midi", at}), which hands it to the
// engine before the render of the quantum nearest to it (worklet/host.js TimedMidi). The core counts the frames it
// was given and nothing else, so nothing drifts: the n-th 16th at 120 BPM and 48 kHz is due at exactly n x 6000.
// While no audio runs (no player yet, a suspended context) the wall clock stands in (microseconds, MIDI at once).
// A gap (a sleeping machine, a tab the browser froze, the context's rate changed) is not caught up: the core goes on
// from the audio clock's frame, so the sequencer pauses rather than bursting.
//
// THE FIRMWARE'S MIDI CLOCK. The worklet posts the firmware's MIDI out with the frame of the quantum that made it.
// Its real-time messages (F8 FA FB FC, F2 song position) are fed to the core at that frame plus the lead, in the
// core's own timeline (between two blocks, the block split at that point), so the core sees them evenly spaced as
// the firmware sent them: the sequencer follows the firmware's tempo and its start and stop, its notes landing the
// lead (LEAD_S) after the firmware's own beat, steadily. One that arrives later than that (a busy page) goes in at
// once.
//
//   const s = new SeqClock({ advance(frames, rate) -> acts, midiIn(bytes) -> acts, emit(acts, at | null) })
//   s.pump(now)           now: { frame, rate } of the audio clock, or { ms } (the wall clock), or null (nothing)
//   s.clock(bytes, frame) the firmware's MIDI out (any chunking; real time and song position taken out of it)
//   s.reset()             forget the position (a new link)
// emit(acts, at): actions due at absolute frame `at` (all of them stamped: at + a.frame), or at === null: now.

export const BLOCK = 128;
export const LEAD_S = 0.03;                 // how far the core runs ahead of the audio clock
export const MAX_GAP_S = 0.25;              // further behind than this: a gap, not caught up
const WALL_RATE = 1000000;                  // the wall clock's units (rfm_move_seq_advance's rate 1000000 = us)

// the real-time and song-position messages out of a MIDI byte stream (SysEx and other messages skipped), in any
// chunking: push(bytes, cb(msg Uint8Array))
export class RealtimeParser {
  constructor() { this.sx = false; this.spp = -1; this.d = []; }
  push(bytes, cb) {
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i] & 255;
      if (b >= 0xf8) { if (b === 0xf8 || b === 0xfa || b === 0xfb || b === 0xfc) cb(Uint8Array.of(b)); continue; }
      if (b === 0xf0) { this.sx = true; this.spp = -1; continue; }
      if (b >= 0x80) { this.sx = false; this.spp = b === 0xf2 ? 0 : -1; this.d = []; continue; }
      if (this.sx || this.spp < 0) continue;
      this.d.push(b);
      if (this.d.length === 2) { cb(Uint8Array.of(0xf2, this.d[0], this.d[1])); this.spp = -1; this.d = []; }
    }
  }
}

export class SeqClock {
  constructor({ advance, midiIn, emit, lead = LEAD_S, block = BLOCK }) {
    this.advance = advance; this.midiIn = midiIn; this.emit = emit;
    this.leadS = lead; this.block = block;
    this.rt = new RealtimeParser();
    this.q = [];                            // the firmware's clock messages waiting for their frame: { at, msg }
    this.stats = { blocks: 0, gaps: 0, clocks: 0, lateClocks: 0 };
    this.reset();
  }
  reset() { this.pos = null; this.rate = 0; this.wall = false; this.q = []; }
  get lead() { return this.wall ? 0 : Math.round(this.leadS * this.rate); }

  pump(now) {
    if (!now) return;
    const wall = typeof now.frame !== "number", rate = wall ? WALL_RATE : now.rate | 0;
    const t = wall ? Math.round(now.ms * 1000) : now.frame;
    if (!rate) return;
    if (this.pos === null || rate !== this.rate || wall !== this.wall || t - this.pos > MAX_GAP_S * rate || this.pos - t > rate) {
      if (this.pos !== null) this.stats.gaps++;
      this.pos = t; this.rate = rate; this.wall = wall; this.q = [];
    }
    const target = t + this.lead;
    while (this.pos < target) {
      this.feedDue();
      let n = Math.min(wall ? target - this.pos : this.block, target - this.pos);
      if (!wall && n < this.block) break;   // (whole blocks only on the audio clock: the next pump takes the rest)
      if (this.q.length && this.q[0].at > this.pos && this.q[0].at < this.pos + n) n = this.q[0].at - this.pos;
      const acts = this.advance(n, rate);
      this.stats.blocks++;
      if (acts && acts.length) this.emit(acts, wall ? null : this.pos);
      this.pos += n;
    }
    this.feedDue();
  }
  // the clock messages whose frame the core has reached
  feedDue() {
    while (this.q.length && this.q[0].at <= this.pos) this.feed(this.q.shift().msg);
  }
  feed(msg) {
    const acts = this.midiIn(msg);
    this.stats.clocks++;
    if (acts && acts.length) this.emit(acts, this.wall || this.pos === null ? null : this.pos);
  }
  // the firmware's MIDI out; frame: the quantum it came from (undefined: now)
  clock(bytes, frame) {
    this.rt.push(bytes, (msg) => {
      if (this.pos === null || this.wall || typeof frame !== "number") { this.feed(msg); return; }
      const at = frame + this.lead;
      if (at <= this.pos) { this.stats.lateClocks++; this.feed(msg); return; }
      this.q.push({ at, msg });                // (frames only grow: the queue stays in order)
    });
  }
}
