// SPDX-License-Identifier: GPL-3.0-only
// The native engine inside a real AudioWorklet (worklet_test.html): engine.js imported statically, the core's wasm
// bytes compiled and instantiated in the AudioWorkletGlobalScope, render() in process().
import { createEngine, ENGINE } from "./engine.js";

class NativeTestProcessor extends AudioWorkletProcessor {
    constructor(opts) {
        super();
        this.engine = null;
        this.quanta = 0;
        this.port.onmessage = (m) => this.onMessage(m.data);
        const scope = {
            global: typeof AudioWorkletGlobalScope !== "undefined" && globalThis instanceof AudioWorkletGlobalScope,
            sharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
            textDecoder: typeof TextDecoder !== "undefined",
            sampleRate,
        };
        createEngine({ sampleRate, files: { wasm: opts.processorOptions.wasm }, flash: null, options: {} })
            .then((e) => {
                this.engine = e;
                this.port.postMessage({ type: "ready", engine: ENGINE, info: e.info(), scope });
            })
            .catch((err) => this.port.postMessage({ type: "error", message: String((err && err.stack) || err) }));
    }

    onMessage(d) {
        const e = this.engine;
        if (!e) return;
        if (d.type === "panel") e.setPanel(d.panel);
        const s = e.screen();
        this.port.postMessage({ type: "ack", id: d.id, quanta: this.quanta, halted: e.halted(), seq: s.seq,
                                pixels: s.pixels.slice(), leds: e.leds(), flashSeq: e.flash().seq });
    }

    process(inputs, outputs) {
        const out = outputs[0];
        if (this.engine && out.length >= 2) {
            this.engine.render(out[0], out[1]);
            this.quanta++;
        }
        return true;
    }
}

registerProcessor("fm1-native-test", NativeTestProcessor);
