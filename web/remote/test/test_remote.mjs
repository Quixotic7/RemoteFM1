// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 RemoteFM1 contributors
//
// remote.js + libremotefm1 (WASM) against libremotefm1's fake unit (WASM), through a mocked Web MIDI and WebUSB,
// on a simulated millisecond clock. No hardware is touched.
//   node web/remote/test/test_remote.mjs          (after web/remote/build.sh; RFM_VERBOSE=1: the session's log)
import createRemoteFM1 from '../../build/remote/remotefm1.mjs';
import createFakeUnit from '../../build/remote/fakeunit.mjs';
import { RemoteFM1, USB_FILTER, isCandidatePortName } from '../remote.js';
import { createWorld, testFrame, beBytes, sameBytes, expectedPlanes, samePlanes } from './fakeworld.mjs';

const verbose = !!process.env.RFM_VERBOSE;
let fails = 0, passes = 0;
function check(ok, what) {
    if (ok) passes++; else fails++;
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
}
const tick = () => new Promise(r => setImmediate(r));

async function setup(opts = {}, build = () => {}, start = 3) {
    const world = await createWorld(createFakeUnit, { start });          // a page's clock starts near 0
    const u = build(world);
    const remote = new RemoteFM1({ navigator: world.navigator, now: () => world.t, timers: false, module: createRemoteFM1, ...opts });
    const ev = { status: [], panel: [], log: [] };
    remote.onStatus(s => { ev.status.push(s); if (verbose) console.log(`   ${world.t} status ${s.state} ${s.transport || ''} ${s.why}`); });
    remote.onPanel(p => ev.panel.push(p));
    remote.onLog(m => { ev.log.push(m); if (verbose) console.log(`   ${world.t} ${m}`); });
    const run = async ms => {
        for (let i = 0; i < ms; i++) {
            world.step(1);
            remote.tick();
            if ((i & 3) === 3) await tick();   // let the WebUSB promises settle
        }
        await tick();
    };
    const until = async (cond, max) => { for (let t = 0; t < max; t += 10) { await run(10); if (cond()) return t + 10; } return -1; };
    return { world, u, remote, ev, run, until };
}

// ------------------------------------------------------------------ 1. MIDI only, among other devices
{
    console.log('-- Web MIDI: discovery among other ports, session, frames, LEDs, panel, keepalive, recovery, stop');
    const { world, remote, ev, run, until } = await setup({ usb: 'off' }, w => {
        w.other1 = w.addOther('IAC Driver Bus 1');
        w.other2 = w.addOther('Arturia KeyStep 37');
        w.v15 = w.addUnit({ midiName: 'FM-1', name: 'FM-1_015', proto: 1 });          // a stock-like firmware
        w.mute = w.addUnit({ midiName: 'FM-1 #2', silent: true });                    // one that ignores us
        w.updater = w.addUnit({ midiName: 'FM-1 Update' });                           // never to be touched
        w.unit = w.addUnit({ midiName: 'ChoralRoot FM-1' });                          // RemoteFM1, stale port name
    });
    const u = world.unit;
    check(isCandidatePortName('ChoralRoot FM-1') && isCandidatePortName('FM-1') && !isCandidatePortName('IAC Driver Bus 1')
        && !isCandidatePortName('FM-1 Update'), 'port names: FM-1 candidates, others and updater ports skipped');
    check(isCandidatePortName('FM-1 1F2D') && isCandidatePortName('FM-1 00AF Bluetooth'),
        'port names: RemoteFM1 0.2 over Bluetooth MIDI ("FM-1 1F2D", its advertised name) is a candidate');
    await remote.connect();
    check(world.midiRequests.length === 1 && world.midiRequests[0].sysex === true, 'requestMIDIAccess({sysex: true})');
    const tc = await until(() => remote.status.state === 'connected', 3000);
    check(tc >= 0 && remote.status.transport === 'midi', `connected via midi in ${tc} ms (unit fw "${remote.status.unit && remote.status.unit.fwVer}")`);
    check(world.session(u) === 1 && world.stats(u).sessions_started === 1, 'the RemoteFM1 unit has a session on SysEx');
    check(world.stats(world.v15).sessions_started === 0 && world.v15.cmds.length > 0 && world.v15.cmds.every(c => c === 0x01),
        `another firmware on "FM-1" got only HELLO (${world.v15.cmds.length})`);
    check(world.mute.cmds.every(c => c === 0x01), 'a silent "FM-1" got only HELLO');
    check(world.updater.sent.length === 0, 'the updater port got nothing');
    check(world.other1.bytes === 0 && world.other2.bytes === 0, 'other MIDI devices got nothing');
    const up = remote.unitPorts();
    check(up && up.inputId === u.input.id && up.outputId === u.output.id, `unitPorts() names the unit's ports (${up && up.name})`);

    // frames converge to the shadow (host-order pixels in, the unit's BE shadow out)
    const f1 = testFrame(0);
    remote.setFrame(1, f1);
    let t = await until(() => sameBytes(world.shadow(u), beBytes(f1)), 4000);
    check(t >= 0, `frame 1 converged on the unit's shadow in ${t} ms`);
    const f2 = testFrame(5);
    remote.setFrame(2, f2);
    t = await until(() => sameBytes(world.shadow(u), beBytes(f2)), 4000);
    check(t >= 0, `frame 2 (a change) converged in ${t} ms`);

    // animation: fps reported
    for (let i = 0; i < 40; i++) { remote.setFrame(100 + i, testFrame(i)); await run(50); }
    const s = remote.stats();
    check(remote.status.fps >= 10, `animating: ${remote.status.fps} fps acknowledged (budget ${s.budget} B/frame, ack max ${s.ack_ms_max} ms)`);
    check(s.decode_errs === 0 && s.ack_timeouts === 0 && s.shadow_resets === 0, 'no decode error, no ack timeout, no shadow reset');
    t = await until(() => sameBytes(world.shadow(u), beBytes(testFrame(39))), 4000);
    check(t >= 0, 'the last animation frame converged');

    // LEDs
    const lv = new Uint8Array(44);
    lv[0] = 255; lv[5] = 120; lv[10] = 50; lv[15] = 10; lv[43] = 200; lv[21] = 176; lv[22] = 95;
    remote.setLeds(lv, 255);
    await run(100);
    check(samePlanes(world.leds(u), expectedPlanes(lv, 255)), 'LED levels -> the unit\'s lit / mid / dim / faint planes, PLAY green');
    remote.setLeds(new Uint8Array(44), 0);
    await run(100);
    check(samePlanes(world.leds(u), expectedPlanes(new Uint8Array(44), 0)), 'LEDs off again');

    // panel events
    ev.panel.length = 0;
    world.key(u, 5, 1); await run(5);
    let last = ev.panel.at(-1);
    check(last && last.keys === (1 << 5) && last.keyVel.length === 27 && last.enc.length === 7, 'key 5 down -> panel.keys bit 5');
    world.key(u, 5, 0); world.key(u, 26, 1); await run(5);
    last = ev.panel.at(-1);
    check(last && last.keys === (1 << 26), 'key 5 up, key 26 (G5) down');
    world.key(u, 26, 0);
    world.button(u, 12, 1); await run(5);
    check(ev.panel.at(-1).buttons === (1 << 12), 'PLAY (matrix button 12) down');
    world.button(u, 12, 0); await run(5);
    check(ev.panel.at(-1).buttons === 0, 'PLAY up');
    ev.panel.length = 0;
    world.enc(u, 2, 3); world.enc(u, 2, -1); world.enc(u, 6, 70); await run(5);
    const encSum = new Int32Array(7);
    for (const p of ev.panel) for (let i = 0; i < 7; i++) encSum[i] += p.enc[i];
    check(encSum[2] === 2 && encSum[6] === 70 && encSum[0] === 0, `encoder deltas: enc[2] ${encSum[2]}, enc[6] ${encSum[6]} (a 70-detent turn split over MIDI)`);
    world.master(u, 700); await run(10);
    check(ev.panel.at(-1).master === 700, `MASTER 700 (got ${ev.panel.at(-1).master})`);
    world.master(u, 701); await run(700);                      // a 1-count move: only PANEL_R carries it
    check(ev.panel.at(-1).master === 701, 'MASTER 701 (1 count, from PANEL_R)');
    // a lost key event is repaired by PANEL_R
    world.dropMidi(u, 1); world.key(u, 3, 1); await run(700);
    check(ev.panel.at(-1).keys === (1 << 3), 'a lost key event repaired by PANEL_R');
    world.key(u, 3, 0); await run(10);

    // keepalive: 10 s idle
    const before = world.stats(u);
    await run(10000);
    const after = world.stats(u);
    check(world.session(u) === 1 && after.keepalive_expired === 0 && remote.status.state === 'connected',
        `10 s idle: session kept (${after.leds - before.leds} LEDS, ${after.panel_r - before.panel_r} PANEL_R)`);

    // the unit reboots: a new session, the screen sent again
    world.reboot(u);
    t = await until(() => remote.status.state === 'connected' && world.session(u) === 1 && sameBytes(world.shadow(u), beBytes(testFrame(39))), 5000);
    check(t >= 0, `unit rebooted: session restarted and the screen resent in ${t} ms`);

    // unplugged: lost, panel released; plugged back: connected again
    world.key(u, 7, 1); await run(5);
    const nStatus = ev.status.length;
    world.unplug(u);
    await run(300);
    check(ev.status.slice(nStatus).some(s => s.state === 'lost'), 'unplugged: status "lost"');
    check(ev.panel.at(-1).keys === 0, 'unplugged: every key released');
    await run(2000);
    check(world.other1.bytes === 0 && world.updater.sent.length === 0, 'still nothing to other devices while searching');
    world.replug(u);
    t = await until(() => remote.status.state === 'connected' && sameBytes(world.shadow(u), beBytes(testFrame(39))), 5000);
    check(t >= 0, `plugged back: connected and the screen resent in ${t} ms`);

    // stop: SESSION 0
    const p = remote.stop();
    await run(400);
    let resolved = false;
    await Promise.race([p.then(() => { resolved = true; }), tick()]);
    check(resolved && world.session(u) === 0 && world.stats(u).sessions_ended >= 1 && remote.status.state === 'stopped',
        'stop(): SESSION 0 acknowledged, the unit back on its waiting screen');
    const np = ev.panel.length;
    world.key(u, 4, 1); await run(10); world.key(u, 4, 0); await run(10);
    check(ev.panel.length === np, 'after stop(): the unit\'s keys no longer reach the page');
    await remote.connect();                                     // resume
    t = await until(() => remote.status.state === 'connected', 3000);
    check(t >= 0, 'connect() after stop() resumes');
    const pd = remote.disconnect();
    await run(400);
    await pd;
    check(world.session(u) === 0 && remote.status.state === 'idle', 'disconnect(): session ended, idle');
    check(world.other1.bytes === 0 && world.other2.bytes === 0 && world.updater.sent.length === 0, 'no byte ever went to a non-FM-1 port');
}

// ------------------------------------------------------------------ 2. WebUSB bulk for frames, MIDI for the panel
{
    console.log('-- WebUSB: the chooser, interface 2, bulk frames, MIDI panel, losing USB');
    const { world, remote, ev, run, until } = await setup({ usb: 'ask' }, w => {
        w.other = w.addOther('IAC Driver Bus 1');
        w.unit = w.addUnit({ midiName: 'FM-1', bulk: true });
    });
    const u = world.unit;
    await remote.connect();
    const f = world.requestDeviceFilters[0];
    check(f && f.filters.length === 1 && f.filters[0].vendorId === 0x1209 && f.filters[0].productId === 1 && f.filters[0].classCode === 0xff,
        'requestDevice({filters: [{vendorId: 0x1209, productId: 0x0001, classCode: 0xFF}]})');
    check(world.claims.length === 1 && world.claims[0] === 2, 'only interface 2 claimed');
    let t = await until(() => remote.status.state === 'connected', 3000);
    check(t >= 0 && remote.status.transport === 'bulk' && world.session(u) === 2, `connected via bulk in ${t} ms`);
    const f1 = testFrame(3);
    remote.setFrame(1, f1);
    t = await until(() => sameBytes(world.shadow(u), beBytes(f1)), 3000);
    const st = world.stats(u);
    check(t >= 0 && world.usbOut > 20000 && st.bk_msgs > st.sx_msgs, `frame converged over bulk in ${t} ms (${world.usbOut} bytes on EP3)`);
    for (let i = 0; i < 40; i++) { remote.setFrame(100 + i, testFrame(i)); await run(50); }
    check(remote.status.fps >= 15, `animating over bulk: ${remote.status.fps} fps`);
    ev.panel.length = 0;
    world.key(u, 0, 1); world.enc(u, 1, -4); await run(5);
    check(ev.panel.some(p => p.keys & 1) && ev.panel.reduce((a, p) => a + p.enc[1], 0) === -4, 'panel events over MIDI while frames use bulk');
    world.unplugUsb(u);
    t = await until(() => remote.status.state === 'connected' && remote.status.transport === 'midi', 5000);
    check(t >= 0, `USB gone: back on MIDI in ${t} ms`);
    t = await until(() => sameBytes(world.shadow(u), beBytes(testFrame(39))), 4000);
    check(t >= 0, 'the screen resent over MIDI');
    await Promise.all([remote.disconnect(), run(400)]);
    check(world.other.bytes === 0, 'nothing to the other device');
}

// ------------------------------------------------------------------ 3. the chooser cancelled, sysex refused
{
    console.log('-- refusals');
    const { world, remote, run, until } = await setup({ usb: 'ask' }, w => { w.usbChooserCancels = true; w.unit = w.addUnit({ midiName: 'FM-1', bulk: true }); });
    await remote.connect();
    const t = await until(() => remote.status.state === 'connected', 3000);
    check(t >= 0 && remote.status.transport === 'midi', 'WebUSB chooser cancelled: frames over MIDI');
    await Promise.all([remote.disconnect(), run(400)]);
    const w2 = await createWorld(createFakeUnit);
    w2.navigator.requestMIDIAccess = async () => { throw new DOMException('denied', 'SecurityError'); };
    delete w2.navigator.usb;
    const r2 = new RemoteFM1({ navigator: w2.navigator, now: () => w2.t, timers: false, module: createRemoteFM1 });
    let err = null;
    try { await r2.connect(); } catch (e) { err = e; }
    check(err && r2.status.state === 'error', `MIDI refused, no WebUSB: connect() rejects ("${err && err.message}")`);
}

// ------------------------------------------------------------------ 4. autoConnect(): no gesture, no prompt
{
    console.log('-- autoConnect: permission granted / prompt / denied, plug-in later, quiet HELLO, disconnect pauses');
    const hellos = u => u.cmds.filter(c => c === 0x01).length;

    // granted: connects by itself, MIDI only from what was granted, no chooser, nothing to other devices
    {
        const { world, remote, run, until } = await setup({ usb: 'ask' }, w => {
            w.other = w.addOther('IAC Driver Bus 1');
            w.updater = w.addUnit({ midiName: 'FM-1 Update' });
            w.unit = w.addUnit({ midiName: 'FM-1', bulk: true });
        });
        const info = await remote.autoConnect();
        check(info.midi === 'granted' && world.permQueries[0].name === 'midi' && world.permQueries[0].sysex === true,
            'granted: permissions.query({name: "midi", sysex: true})');
        const t = await until(() => remote.status.state === 'connected', 4000);
        check(t >= 0 && remote.status.quiet === true && remote.status.transport === 'midi', `granted: connected by itself in ${t} ms (quiet, MIDI)`);
        check(world.requestDeviceFilters.length === 0, 'granted: no requestDevice without a gesture (the unpaired USB unit stays on MIDI)');
        check(world.other.bytes === 0 && world.updater.sent.length === 0, 'granted: no byte to the other device or the updater port');
        await Promise.all([remote.disconnect(), run(400)]);
        check(remote.status.state === 'idle', 'disconnect(): idle');
        await run(5000);
        check(remote.status.state === 'idle' && world.session(world.unit) === 0, 'after disconnect(): no auto-connect while the unit stays plugged in');
        world.unplug(world.unit); await run(300);
        world.replug(world.unit);
        const t2 = await until(() => remote.status.state === 'connected', 5000);
        check(t2 >= 0, `after disconnect(): plugged in again, connected by itself in ${t2} ms`);
        await Promise.all([remote.disconnect(), run(400)]);
    }

    // prompt / denied: nothing asked, nothing sent
    for (const perm of ['prompt', 'denied']) {
        const { world, remote, run } = await setup({ usb: 'ask' }, w => {
            w.midiPerm = perm;
            w.unit = w.addUnit({ midiName: 'FM-1', bulk: true });
            w.permitted.add(w.unit.usb);                           // even a paired USB unit: no MIDI, no auto session
        });
        const info = await remote.autoConnect();
        await run(5000);
        check(info.midi === perm && world.midiRequests.length === 0 && world.requestDeviceFilters.length === 0
            && world.unit.sent.length === 0 && world.usbOut === 0 && remote.status.state === 'idle',
            `${perm}: no requestMIDIAccess, no requestDevice, nothing sent, idle (${world.unit.sent.length} msgs)`);
        let err = null;
        try { await remote.connect(); } catch (e) { err = e; }      // then a click
        if (perm === 'prompt') {
            const t = await (async () => { for (let i = 0; i < 300; i++) { await run(10); if (remote.status.state === 'connected') return i * 10; } return -1; })();
            check(world.midiRequests.length === 1 && world.requestDeviceFilters.length === 0 && t >= 0 && remote.status.transport === 'bulk'
                && remote.status.quiet === false, `prompt: a click asks for MIDI and connects (bulk: the paired unit) in ${t} ms`);
        } else {
            // denied MIDI, but a paired USB unit: a click still connects (bulk), as before
            check(world.midiRequests.length === 1 && !err, 'denied: a click asks once more (refused); the paired USB unit is used');
        }
        await Promise.all([remote.disconnect(), run(400)]);
    }

    // a permission granted later (the installer, in another tab) starts it
    {
        const { world, remote, run, until } = await setup({ usb: 'off' }, w => { w.midiPerm = 'prompt'; w.unit = w.addUnit({ midiName: 'FM-1' }); });
        await remote.autoConnect();
        await run(500);
        world.setMidiPerm('granted');
        const t = await until(() => remote.status.state === 'connected', 4000);
        check(t >= 0 && t < 1000 && world.midiRequests.length === 1, `permission granted later: connected by itself in ${t} ms`);
        await Promise.all([remote.disconnect(), run(400)]);
    }

    // plug-in later; quiet HELLO only to FM-1 ports, at most every 2 s; unplugged: the quiet session ends
    {
        const { world, remote, run, until } = await setup({ usb: 'ask' }, w => { w.other = w.addOther('Arturia KeyStep 37'); });
        await remote.autoConnect();
        await run(3000);
        check(remote.status.state === 'idle' && world.midiRequests.length === 1 && world.other.bytes === 0,
            'granted, no FM-1 plugged in: no session, nothing sent');
        const v15 = world.addUnit({ midiName: 'FM-1', name: 'FM-1_015', proto: 1 });   // the stock firmware: answers, not RemoteFM1
        world.plugIn(v15);
        await run(1000);
        check(remote.status.state === 'searching' && remote.status.quiet && hellos(v15) === 1, 'a stock FM-1 plugged in: one HELLO, a quiet search');
        await run(10000);
        const n = hellos(v15);
        check(n >= 5 && n <= 7 && v15.cmds.every(c => c === 0x01), `quiet: ${n} HELLOs in 11 s to the stock FM-1 (at most every 2 s), nothing else`);
        world.unplug(v15);
        await run(3000);
        check(remote.status.state === 'idle', 'its ports gone: the quiet session ends (idle)');
        const unit = world.addUnit({ midiName: 'ChoralRoot FM-1', bulk: true });
        world.plugIn(unit, { paired: true });
        const t = await until(() => remote.status.state === 'connected', 5000);
        check(t >= 0 && remote.status.transport === 'bulk', `a RemoteFM1 unit plugged in later (paired before): connected over bulk in ${t} ms`);
        check(world.other.bytes === 0 && world.requestDeviceFilters.length === 0, 'still nothing to the KeyStep, no requestDevice');
        // a click while connected quietly: a normal session (the chooser is not needed: a USB link is there)
        await remote.connect();
        check(remote.status.quiet === false && world.requestDeviceFilters.length === 0, 'a click on the quiet session: normal, no chooser (USB already linked)');
        world.unplug(unit); await run(3000);
        check(remote.status.state === 'searching', 'a normal session keeps looking after an unplug (as before)');
        await Promise.all([remote.disconnect(), run(400)]);
    }

    // a click on a quiet search (nothing answering): the chooser, now that there is a gesture
    {
        const { world, remote, run, until } = await setup({ usb: 'ask' }, w => { w.unit = w.addUnit({ midiName: 'FM-1', silent: true, bulk: true }); });
        await remote.autoConnect();
        await until(() => remote.status.state === 'searching', 2000);
        check(remote.status.quiet && world.requestDeviceFilters.length === 0, 'a silent FM-1: a quiet search, no chooser');
        await remote.connect();
        check(!remote.status.quiet && world.requestDeviceFilters.length === 1, 'a click on it: no longer quiet, the WebUSB chooser opened once');
        await Promise.all([remote.disconnect(), run(400)]);
    }
}

console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
