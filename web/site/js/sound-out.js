// SPDX-License-Identifier: GPL-3.0-only
// Where the page's sound goes: the system's output, or the FM-1 itself (RemoteFM1 0.2.1+ is a USB audio device,
// "FM-1" among the computer's sound outputs, MASTER its volume). The choice is kept in prefs.soundOut ("fm1" or none)
// and follows the FM-1: on its output while it is plugged in, the system's when it goes, back when it returns.
//   const out = new SoundOut({ player, media: navigator.mediaDevices, prefs, save, onChange });
//   out.view()       { support, want, on, label, sub }   for the sheet's Sound output row
//   await out.useFm1()   (a click) Chrome: asks for the microphone once, the only way it names the outputs; Firefox:
//                        its output chooser. Throws a sentence when there is no FM-1 output.
//   await out.useSystem(); await out.apply()   (after the player started, and on every device change)
// Safari has no way to pick a page's output (support null): the sheet says to pick FM-1 as the system's output.
// No DOM here beyond what is passed in: web/site/test.mjs runs it with fakes.
import { sinkSupport, isIOS } from "./audio.js";

// an FM-1's output as the browser names it ("FM-1", "FM-1 (1209:0001)"), not "Default - FM-1" (the system default's alias)
export const isFm1Output = (d) => !!d && d.kind === "audiooutput" && !!d.deviceId && d.deviceId !== "default" && d.deviceId !== "communications"
  && /(^|[^A-Za-z0-9])FM-?1([^0-9]|$)|RemoteFM1/i.test(d.label || "");
export const pickFm1 = (devices) => (devices || []).find(isFm1Output) || null;
// outputs listed without names: Chrome before the page may use the microphone
export const namesHidden = (devices) => (devices || []).filter((d) => d.kind === "audiooutput").every((d) => !d.label);

export const NO_PICK_TEXT = () => isIOS()
  ? "This browser plays through the iPhone's or iPad's own output: an FM-1 plugged in by USB takes it over by itself."
  : "This browser can't pick a page's output: choose FM-1 as the computer's sound output (on a Mac: System Settings > Sound > Output).";
export const NO_FM1_TEXT = "No FM-1 among the sound outputs. It needs RemoteFM1 0.2.1 or later and a USB cable (not Bluetooth).";

export class SoundOut {
  constructor({ player, media, prefs, save, onChange, support = sinkSupport() }) {
    this.player = player; this.media = media || null; this.prefs = prefs; this.save = save || (() => {}); this.onChange = onChange || (() => {});
    this.support = media ? support : null;
    this.device = null;        // the FM-1's output while the sound goes there
    this.missing = false;      // FM-1 wanted, not plugged in
    this.hidden = false;       // FM-1 wanted, Chrome won't name the outputs (no microphone permission any more)
    this.error = "";
    this.busy = false;
    if (this.media && this.media.addEventListener) this.media.addEventListener("devicechange", () => { this.apply().catch(() => {}); });
  }
  get want() { return this.prefs.soundOut === "fm1" ? "fm1" : "system"; }
  get on() { return !!this.device; }

  view() {
    const s = this.support;
    let sub;
    if (!s) sub = NO_PICK_TEXT();
    else if (this.error) sub = this.error;
    else if (this.want === "system") sub = s === "context" ? "the computer's output. FM-1: Chrome asks for the microphone once, only to list the outputs by name" : "the computer's output";
    else if (this.device) sub = `${this.device.label}: MASTER is its volume`;
    else if (this.hidden) sub = "Choose FM-1 again to let the browser list the outputs";
    else if (this.missing) sub = "the computer's output until the FM-1 is plugged in by USB";
    else sub = "the FM-1, once the sound starts";
    return { support: s, want: this.want, on: this.on, label: this.device ? this.device.label : "", sub, busy: this.busy };
  }

  async list() { try { return await this.media.enumerateDevices(); } catch (_) { return []; } }

  // a click: the FM-1 from now on
  async useFm1() {
    if (!this.support) throw new Error(NO_PICK_TEXT());
    this.busy = true; this.error = ""; this.onChange();
    try {
      let dev = null;
      if (this.support === "element") {
        // Firefox: its own chooser (the visitor picks; the FM-1 is what they're asked for)
        dev = await this.media.selectAudioOutput();
        this.prefs.soundOutId = dev.deviceId;
      } else {
        let all = await this.list();
        if (!pickFm1(all) && namesHidden(all)) {
          // Chrome names the outputs only to a page that may use the microphone: ask, and close it at once
          let stream;
          try { stream = await this.media.getUserMedia({ audio: true }); }
          catch (_) { throw new Error("Without the microphone permission Chrome won't name the sound outputs. Or choose FM-1 as the computer's sound output."); }
          stream.getTracks().forEach((t) => t.stop());
          all = await this.list();
        }
        dev = pickFm1(all);
        if (!dev) throw new Error(NO_FM1_TEXT);
      }
      await this.player.setSink(dev.deviceId);
      this.device = { id: dev.deviceId, label: dev.label || "FM-1" };
      this.missing = this.hidden = false;
      this.prefs.soundOut = "fm1"; this.save();
    } catch (e) {
      this.error = (e && e.message) || String(e);
      throw e;
    } finally { this.busy = false; this.onChange(); }
  }

  async useSystem() {
    this.error = "";
    delete this.prefs.soundOut; delete this.prefs.soundOutId; this.save();
    try { await this.player.setSink(""); } catch (_) { /* (the system's output can't be refused) */ }
    this.device = null; this.missing = this.hidden = false;
    this.onChange();
  }

  // the kept choice, now: after the player started, and when a device comes or goes (no prompt ever)
  async apply() {
    if (!this.support || !this.player.ctx) return;
    if (this.want !== "fm1") { if (this.device) await this.useSystem(); return; }
    const all = await this.list();
    let dev = null;
    if (this.support === "element") dev = all.find((d) => d.kind === "audiooutput" && d.deviceId === this.prefs.soundOutId) || null;
    else dev = pickFm1(all);
    this.hidden = !dev && this.support === "context" && namesHidden(all);
    this.missing = !dev && !this.hidden;
    const id = dev ? dev.deviceId : "";
    if (this.device && this.device.id === id) return;
    try { await this.player.setSink(id); this.device = dev ? { id, label: dev.label || "FM-1" } : null; this.error = ""; }
    catch (e) { this.device = null; this.error = (e && e.message) || String(e); try { await this.player.setSink(""); } catch (_) { /* (as is) */ } }
    this.onChange();
  }
}
