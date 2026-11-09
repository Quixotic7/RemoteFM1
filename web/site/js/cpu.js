// SPDX-License-Identifier: GPL-3.0-only
// The emulated CPU clock (the emu engine's options.mhz: fm1-emu's instructions per emulated second, fixed at boot by
// fm1_boot_fwsc, so a change reloads the firmware with its flash kept). The FM-1 runs at about 320-360 MHz.
// One setting for every emulated firmware (the owner, 2026-11-09: a pick stays as firmwares change): Auto, 80, 120,
// 160, 240, 320 or 360 MHz; no overclock (no computer of theirs kept up with one). Auto starts at 320 and steps down
// (320 -> 240 -> 160 -> 120 -> 80, the floor) when this computer can't keep up, and the step is kept for every later
// firmware too: a computer that needs 120 starts the next one at 120. It never steps up by itself (picking Auto
// again starts at 320). A manual pick never changes; it only warns.
// Pure code (no DOM): web/site/test.mjs runs it with fake timing.
//
// What "can't keep up" is: the worklet (worklet/host.js) reports every half second the share of the audio time it
// spent rendering (load), its busiest stretch of eight quanta (peak: 21 ms; above 1 those quanta were rendered late),
// and how far the wall clock ran ahead of the audio clock (lateMs: quanta rendered late, heard as dropouts); the page
// adds the browser's own count of glitches where it has one (AudioContext.playoutStats: frames the device got no
// audio for). Too slow, before the audio breaks up: load above 80 % for 2 s, or a peak above 95 % in three of four
// reports (2 s), or 3 glitches within 3 s, or 250 ms late within 5 s. The first 4 s after a boot don't count (the
// boot and fm1-emu's JIT compiling the hot code).
//
// A weak host (four or fewer cores: phones, old laptops) starts Auto one step lower (240 MHz) for the dual-core
// firmwares, unless Auto has already stepped lower than that.

export const MHZ_CHOICES = ["auto", 80, 120, 160, 240, 320, 360];
export const DEVICE_MHZ = 360;
export const DEVICE_RANGE = [320, 360];
export const AUTO_START = 320;
export const AUTO_STEPS = [320, 240, 160, 120, 80];
export const inDeviceRange = (m) => typeof m === "number" && m >= DEVICE_RANGE[0] && m <= DEVICE_RANGE[1];

// The FM-1 firmwares that run both cores (the owner's list): the heaviest ones to emulate.
export const DUAL_CORE = new Set(["choralroot", "melodee", "sloopdx", "fm106", "coralflower"]);

// a weak host: four or fewer cores (navigator.hardwareConcurrency; unknown counts as strong)
export const weakHost = (host = {}) => typeof host.cores === "number" && host.cores > 0 && host.cores <= 4;
// the visitor's choice and the clock it gives a firmware. prefs: { cpuMhz: choice, cpuAutoMhz: mhz } (one for every
// firmware); host: { cores } (Auto starts one step lower for a dual-core firmware on a weak host)
export function mhzFor(id, prefs = {}, host = {}) {
  const choice = MHZ_CHOICES.includes(prefs.cpuMhz) ? prefs.cpuMhz : "auto";
  if (choice !== "auto") return { choice, mhz: choice };
  const at = AUTO_STEPS.includes(prefs.cpuAutoMhz) ? prefs.cpuAutoMhz : AUTO_START;
  return { choice, mhz: weakHost(host) && DUAL_CORE.has(id) && at === AUTO_START ? nextStep(at) : at };
}
// a new choice (picking Auto again forgets an earlier step down); returns the new prefs fields
export function pickMhz(choice) {
  if (!MHZ_CHOICES.includes(choice)) throw new Error(`no clock choice ${choice}`);
  return { cpuMhz: choice, cpuAutoMhz: null };
}
// Auto's step down, for this firmware and the ones after it; returns the new prefs field
export const stepDown = (to) => ({ cpuAutoMhz: to });
export const nextStep = (mhz) => { const i = AUTO_STEPS.indexOf(mhz); return i >= 0 && i < AUTO_STEPS.length - 1 ? AUTO_STEPS[i + 1] : null; };

export const GRACE_MS = 4000, HIGH_LOAD = 0.8, HIGH_FOR_MS = 2000, LATE_MS = 250, LATE_WINDOW_MS = 5000;
export const HIGH_PEAK = 0.95, PEAK_OF = 4, PEAK_NEED = 3, GLITCHES = 3, GLITCH_WINDOW_MS = 3000;

// One per boot. feed() a worklet report ({ load, peak, lateMs, glitches }: glitches the browser's running count, when
// it has one) at time now (ms); it answers null, { step: mhz } (Auto: reload at that clock) or { warn: true } (a
// manual pick, or Auto at its lowest: tell the visitor, once).
export class Governor {
  constructor({ choice, mhz, now }) {
    this.choice = choice; this.mhz = mhz; this.t0 = now;
    this.highSince = null; this.late = []; this.peaks = []; this.glitch = []; this.glitches = null; this.done = false;
  }
  feed({ load = 0, lateMs = 0, peak = null, glitches = null } = {}, now) {
    // (the glitch count runs on through the grace: only those after it count)
    const newGlitches = typeof glitches === "number" && this.glitches !== null ? Math.max(0, glitches - this.glitches) : 0;
    if (typeof glitches === "number") this.glitches = glitches;
    if (this.done || now - this.t0 < GRACE_MS) return null;
    if (load > HIGH_LOAD) { if (this.highSince === null) this.highSince = now; } else this.highSince = null;
    if (lateMs > 0) this.late.push([now, lateMs]);
    while (this.late.length && now - this.late[0][0] > LATE_WINDOW_MS) this.late.shift();
    const lateSum = this.late.reduce((s, [, ms]) => s + ms, 0);
    if (typeof peak === "number") { this.peaks.push(peak > HIGH_PEAK); if (this.peaks.length > PEAK_OF) this.peaks.shift(); }
    const peaky = this.peaks.filter(Boolean).length >= PEAK_NEED;
    if (newGlitches) this.glitch.push([now, newGlitches]);
    while (this.glitch.length && now - this.glitch[0][0] > GLITCH_WINDOW_MS) this.glitch.shift();
    const glitchy = this.glitch.reduce((s, [, k]) => s + k, 0) >= GLITCHES;
    const slow = (this.highSince !== null && now - this.highSince >= HIGH_FOR_MS) || lateSum >= LATE_MS || peaky || glitchy;
    if (!slow) return null;
    this.done = true;
    const next = this.choice === "auto" ? nextStep(this.mhz) : null;
    return next ? { step: next } : { warn: true };
  }
}
