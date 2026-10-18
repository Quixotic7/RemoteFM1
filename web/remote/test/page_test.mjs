// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
//
// The page test: remote.js in a real browser (headless Chromium), with its default module URL, timers and clock,
// and navigator.requestMIDIAccess / navigator.usb replaced by the fake world, so no hardware is touched. The
// verdict goes to #result and document.title ("PASS n" / "FAIL ..."). run_page_test.sh drives it.
import createFakeUnit from '../../build/remote/fakeunit.mjs';
import { RemoteFM1 } from '../remote.js';
import { createWorld, testFrame, beBytes, sameBytes, expectedPlanes, samePlanes } from './fakeworld.mjs';

const out = document.getElementById('result');
const lines = [];
let fails = 0;
function check(ok, what) { if (!ok) fails++; lines.push(`${ok ? 'PASS' : 'FAIL'}  ${what}`); out.textContent = lines.join('\n'); }
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(cond, max) { const t0 = performance.now(); while (performance.now() - t0 < max) { if (cond()) return Math.round(performance.now() - t0); await sleep(10); } return -1; }

function draw(canvas, be) {
    const g = canvas.getContext('2d'), img = g.createImageData(240, 240);
    for (let i = 0; i < 240 * 240; i++) {
        const v = (be[2 * i] << 8) | be[2 * i + 1];
        img.data[4 * i] = (v >> 11) << 3; img.data[4 * i + 1] = ((v >> 5) & 63) << 2; img.data[4 * i + 2] = (v & 31) << 3; img.data[4 * i + 3] = 255;
    }
    g.putImageData(img, 0, 0);
}

try {
    const world = await createWorld(createFakeUnit, { start: Math.floor(performance.now()) });
    world.other = world.addOther('IAC Driver Bus 1');
    world.unit = world.addUnit({ midiName: 'ChoralRoot FM-1', bulk: true });
    const u = world.unit;
    // the mocks replace the browser's own: the real Web MIDI and WebUSB are never called
    Object.defineProperty(navigator, 'requestMIDIAccess', { value: world.navigator.requestMIDIAccess, configurable: true });
    Object.defineProperty(navigator, 'usb', { value: world.navigator.usb, configurable: true });
    setInterval(() => { const now = Math.floor(performance.now()); while (world.t < now) { world.t++; world.pump(); } }, 4);

    const remote = new RemoteFM1();         // every default: navigator, timers, clock, module URL
    const st = document.getElementById('status');
    const panels = [];
    remote.onStatus(s => { st.textContent = `status: ${s.state}${s.transport ? ' via ' + s.transport : ''}${s.fps ? ' ' + s.fps + ' fps' : ''} (${s.why})`; });
    remote.onPanel(p => panels.push(p));
    await remote.connect();
    let t = await until(() => remote.status.state === 'connected', 3000);
    check(t >= 0 && remote.status.transport === 'bulk', `connected via bulk in ${t} ms (fw "${remote.status.unit && remote.status.unit.fwVer}")`);

    const f = testFrame(7);
    remote.setFrame(1, f);
    draw(document.getElementById('engine'), beBytes(f));
    t = await until(() => sameBytes(world.shadow(u), beBytes(f)), 3000);
    check(t >= 0, `frame converged on the unit's shadow in ${t} ms`);
    for (let i = 0; i < 30; i++) { remote.setFrame(10 + i, testFrame(i)); await sleep(50); }
    const fps = remote.status.fps;                  // (measured each second; 0 again once the screen is still)
    t = await until(() => sameBytes(world.shadow(u), beBytes(testFrame(29))), 3000);
    check(t >= 0 && fps >= 10, `animation converged; ${fps} fps while animating`);
    draw(document.getElementById('engine'), beBytes(testFrame(29)));
    draw(document.getElementById('unit'), world.shadow(u));

    const lv = new Uint8Array(44).map((_, i) => [0, 12, 40, 96, 255][i % 5]);
    remote.setLeds(lv, 255);
    await sleep(100);
    check(samePlanes(world.leds(u), expectedPlanes(lv, 255)), 'LEDs on the unit');
    const ledsEl = document.getElementById('leds');
    for (let i = 0; i < 44; i++) { const s = document.createElement('span'); s.style.background = `rgb(${lv[i]},${lv[i] >> 2},0)`; ledsEl.appendChild(s); }

    world.key(u, 9, 1); world.enc(u, 4, 5); world.master(u, 512);
    await sleep(30);
    const enc4 = panels.reduce((a, p) => a + p.enc[4], 0);
    check(panels.at(-1).keys === (1 << 9) && enc4 === 5 && panels.at(-1).master === 512, 'panel: key 9, enc[4] +5, MASTER 512');
    world.key(u, 9, 0);

    await sleep(3500);
    check(world.session(u) === 2 && remote.status.state === 'connected' && world.stats(u).keepalive_expired === 0, '3.5 s idle: session kept');

    const sp = remote.stop();
    await sp;
    check(world.session(u) === 0 && remote.status.state === 'stopped', 'stop(): SESSION 0');
    await remote.disconnect();
    check(world.other.bytes === 0, 'the other MIDI device got nothing');
} catch (e) {
    check(false, `exception: ${e && e.stack || e}`);
}
document.title = fails ? `FAIL ${fails}` : `PASS ${lines.length}`;
out.className = fails ? 'fail' : 'pass';
out.textContent = lines.join('\n') + `\n\n${document.title}`;
