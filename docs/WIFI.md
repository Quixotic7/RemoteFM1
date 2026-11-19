# RemoteFM1 Wi-Fi (phase 1: a test build)

An FM-1 as the panel of a host on the same network (an Ableton Move first) without a USB cable: RemoteFM1's bulk
framing over TCP. This is the spike: the firmware joins a network, serves the bulk framing on TCP port 7740 and
answers discovery on UDP 7741; `tools/wifi_probe.py` puts it on a network over USB and measures it. The protocol's
part is docs/PROTOCOL.md section 10.

## Building

```sh
./build.sh --wifi            # firmware/build/remotefm1-wifi.fwsc, identity FM-1_72900 (HELLO_R "FM-1_729")
./build.sh --wifi --host     # the simulator, firmware/build/host/remotefm1-wifi_host (firmware/tests/wifi.txt)
```

`build.sh --wifi` copies `firmware/` to `build/app-wifi` with `firmware/fm1-wifi.toml` as its `fm1.toml`, so the
USB / Bluetooth build is untouched. fm1-base is the submodule's commit (317cb86: its Wi-Fi is the same as fm1-base
main's on 2026-11-19), with one more patch, `firmware/patches/fm1-base/0010`: `app_wifi.ld` gets the app's DSP code
and the second core's loop in RAM (without it both landed in the flash), a `.ram_bss` section in what the RAM code
leaves of RAMTEXT, and `fm1.toml wifi_heap2_kb`. After the build, `tools/wifi_layout.py` checks the image's pool.

## What is in it

- fm1-base's Wi-Fi (JieLi's library and the SDK's lwIP, `device/wifi.c`): a station, six networks kept in the
  store (OBJ_WIFI: a record in RemoteFM1's own sectors 0xF2000..0xF3FFF, kept compact so the other builds still read
  their own records; the password as fm1-base keeps it, in plain text there), joined at start and again after a drop;
  on at the first screen (fm1-base masks the input timer and the sound while the radio calibrates, up to 15 s).
- `net/rfm_net.c`: TCP on 7740 (one host at a time; a new one replaces the old), discovery on UDP 7741, two 8 KB
  rings to the app; a task of the shim's that runs inside the main loop's `plat_yield` (every 250 us) and waits on a
  semaphore the main loop posts. `src/rfm_wifi.c`: WIFI (0x17), the task's start, the discovery text, the big buffers.
- The standalone DX7 as in the other builds (8 ROM cartridges, the loops, the user banks); no Bluetooth, no USB
  audio, no shadow frame buffer (TILES go to the panel as they are decoded, as `RFM_SHADOW 0` has always done).
- USB: SysEx, the vendor pair (rings of 2 KB: a diagnostic here) and the updater as in the other builds.

## The memory, and the owner's question

"Can we make the Wi-Fi build have fewer DX7 carts so there is space for BLE and USB audio still?" No: the carts are
flash, and Wi-Fi's limit is the RAM. Measured with this tree (fm1-base 317cb86; bytes):

| build | flash (of 581,564) | RAM .data+.bss (of 98,304; Wi-Fi 131,072) | pool (of 344,064; Wi-Fi 311,296) |
|---|---|---|---|
| (a) RemoteFM1 today: Bluetooth, USB audio, 8 carts | 339,216 | 97,172 | 335,320 |
| (a') the same, `--no-ble` | 203,856 | 45,916 | 310,428 |
| (b) Wi-Fi, no Bluetooth, no USB audio, the app as it is | 530,252 | 141,376 (over by 10,304) | 594,708 (over by 283,412) |
| (c) (b) with USB audio | | over by 21,056 (+10,752) | over by 283,412 |
| (d) (b) with Bluetooth (the build refuses it; forced, it links) | | over by 24,928 (+14,624) | over by 283,412 |
| (e) carts: 0 / 1 / 8, any of the above | -32,752 / -28,736 / 0 | 0 | 0 |
| the spike (this build): Wi-Fi, 8 carts | 550,840 | 128,320 | 305,988 (examples/wifi's exactly) |

- The carts (`SA_ROM`, 32,768 bytes) are in `.text`: flash only, no RAM in any build. 8 carts fit a Wi-Fi build's
  flash (31 KB left); fewer carts free flash, which is not short.
- Wi-Fi takes ~100 KB of the RAM (the driver's buffers, lwIP, 24 KB of heap), a 248 KB heap in the pool, and lwIP's
  packet buffers (28 KB) beside it: of the FM-1's ~464 KB of SRAM, ~376 KB. RemoteFM1's own buffers (the shadow 115 KB,
  the vendor rings 64 KB, the loops 32 KB, the rings and parsers 50 KB) do not fit beside it as they are.
- The spike fits by giving the app's big buffers the Wi-Fi heap (fm1-base's advice: "new buffers go on the heap"):
  86.5 KB of its 260 KB (heap2 cut from 24 KB to 12 KB to make the RAM fit), no shadow, small rings. The Wi-Fi
  library's own peak is not known until it runs (WIFI GET reports the heap's peak).
- Bluetooth on top (JieLi's stack) wants ~15 KB more RAM, three task stacks (~24 KB) and its heap from the Wi-Fi heap
  at run time; USB audio ~11 KB of RAM. Neither fits beside the spike: its RAM has 2.7 KB left, and the heap's margin
  is unmeasured.
- What could free RAM: the loops' pool in a Wi-Fi build (28 KB, but the loops kept in the flash would not all load),
  the standalone's other buffers (~20 KB), the screen reader; and the real way: fm1-base's own Wi-Fi stack (open PR
  charlesvestal/fm1-base#31, `wifi_stack = "own"`: ~3.8 KB of RAM, beside the own Bluetooth stack, nothing of
  JieLi's), which has UDP and no TCP, and has not run on an FM-1 in that form.

**Why "not with ble, yet"** (fm1-base): `tools/build.py` refuses `ble` with `wifi` ("one radio a build"). The README:
"each fills most of what the FM-1 has (the chip has no external RAM), and together they don't fit": the RAM first.
The two link (forced: only the RAM regions overflow), and the shim was made for both (its task table counts the
Bluetooth tasks with Wi-Fi's), but they share the radio: commit 0a69a99 found Bluetooth's stored radio trim made the
access point deaf to Wi-Fi (Wi-Fi now calibrates at every start), and JieLi's coexistence set-up (its `wlc.c`) is
wired only in PR #31's own stack. Untested together on an FM-1.

## The pool's layout

`app_wifi.ld` says: "keep the pool's layout as fm1-radio has it: the radio hardware writes somewhere in it (unfound),
and moving the stacks / heap / lwIP pools by even 4.6 KiB broke its reception". The spike keeps the pool before the
heap exactly as long as `examples/wifi`'s (0x5A04 bytes), so the heap and lwIP's pools sit at the example's addresses:
`src/rfm_wifi.c` `RF_POOL_PAD`, checked by `tools/wifi_layout.py` at every `--wifi` build. What else the app needs in
RAM goes to the heap (`app.h` `RF_BIG`) or to RAMTEXT's rest (`RF_LOW`: 10,244 bytes, 1,872 left).

## Putting it on a network and measuring it

On a Mac with the FM-1 on USB (python-rtmidi, as RemoteFM1's other tools):

```sh
python3 tools/wifi_probe.py setup          # asks the network's name and its password (typed, not shown); waits
                                           # for an address (the FM-1 keeps the network once it has joined)
python3 tools/wifi_probe.py status         # the state, the address, the networks kept, the heap
python3 tools/wifi_probe.py find           # discovery on the network (UDP broadcast)
python3 tools/wifi_probe.py bench [ADDR] [--sink-kb 1024 --source-kb 1024 --echo 100 --json out.json]
python3 tools/wifi_probe.py forget all     # forget the networks
```

`bench`: HELLO over TCP, ECHO round trips (16 B and 1 KB), SINK host to FM-1 (STATS confirms every byte, gaps and
checksums), SOURCE FM-1 to host (sequence and checksums checked), the heap before and after; "result: no loss" at the
end, exit 0. Against the simulator: `firmware/build/host/remotefm1-wifi_host` with a script `rf serve 30 7745`, then
`wifi_probe.py --sim bench --tcp-port 7745` (tests/run_tests.sh does it).

## What is known, and what is not

Verified by building and in the simulator: the package builds (flash, RAM, pool inside their limits; the pool as
examples/wifi's), WIFI over USB SysEx, the TCP transport through its rings, discovery and the whole bench over real
TCP on this computer's sockets (net/rfm_net.c is the same code on BSD sockets), the standalone's tests.

Needs the FM-1: that it boots and joins (JieLi's library with RemoteFM1's RAM use; the second core with Wi-Fi), the
heap's peak (WIFI GET), TCP throughput and latency on the chip (lwIP's 2.9 KB send buffer bounds SOURCE), the audio
and the screen while Wi-Fi works, discovery through the owner's router, and that USB and the updater still work.

## Phase 1: a whole panel session over TCP (2026-11-19)

- **The panel's events:** PANEL_EV (`0x56`, PROTOCOL.md section 10; caps bit13 `RFM_CAP_PANEL_EV`, caps 0x362C):
  in a session over TCP the event queue (`rf_evq`, 256) goes out as PANEL_EV messages of up to 32 events (96 bytes,
  the same MIDI bytes as section 4) when TCP's ring has room; nothing more in RAM (the message is built on the
  stack). Flash +176 B (551,016), RAM and the pool unchanged (128,320 / 305,988), `tools/wifi_layout.py` passes.
- **libremotefm1:** `RFM_LINK_TCP` (host/include/rfm/session.h): the bulk framing on a socket the back end owns, no
  pacing (`RFM_BUDGET_TCP_BPS` 300,000 B/s, 2 frames in flight), PANEL_EV into the panel decoder, preferred to a
  SysEx link (a TCP link that has not answered HELLO yet is waited for), `rfm_session_link_reset` for a reconnect,
  `rfm_parse_discovery` for the UDP answers; the fake unit's `cfg.tcp` (host/tests/test_session.c test_tcp,
  test_tcp_over_usb).
- **The simulator** answers discovery with the port it serves (`rf serve 30 7745`: port=7745) and takes the panel
  from standard input while serving (`rf serve SECONDS PORT stdin`: key, press, release, spin, master, drop, end), so
  a host's end-to-end test drives it (RemoteFM14Move's tests/fm1wifi_e2e.sh).
- **On the owner's FM-1 (2026-11-19):** linked by broadcast at once, its panel events arrived, but the session
  restarted every few seconds over a connection that stayed up: TCP's retransmission stalls (a second or more) met
  USB's timeouts (PANEL_R 1.6 s, SESSION resent after 400 ms: a second start restarted the unit's session, "shadow
  reset"). libremotefm1 now waits TCP's stalls out (PROTOCOL.md section 10, `RFM_TCP_*`); test_session test_tcp_stalls
  and the simulator's `stall MS` (RemoteFM14Move's fm1wifi_e2e.sh) reproduce it.
- **1c on the unit was worse than 1b** (2026-11-19: ack 350 ms, PANEL_R gaps of 6 s, sessions restarting): see
  "Diagnostics" below; 1d decodes a kept frame in 4 KB slices, sends 2 frames of 10 KB in flight again, never
  restarts a session over TCP for a late PANEL_R, and counts SESSION's ACKs.
- **Not done:** mDNS (`fm1-XXXX.local`, `_remotefm1._tcp`): lwIP's responder is in the SDK (`apps/mdns`) but not built
  by fm1-base (its options, its sources, its RAM: a patch to fm1-base); the UDP responder is the discovery. The
  screen's rate on the unit (no shadow: rectangles go to the panel row by row as they are decoded) is to be measured.

## Diagnostics (1d)

`tools/wifi_probe.py watch [--log trace.jsonl]` asks WIFI DIAG over USB every second while a host (the Move) drives the
unit over TCP, and prints a line a second:

- **turn**: the main loop's longest turn this second (the app's own: decoding, blitting, replies), and a histogram of
  turns (< 0.25 / 1 / 4 / 16 / 64 ms / longer); **out**: the longest time fm1-base's loop kept the app waiting; **net**:
  the longest the socket task did not run (it runs only inside plat_yield, every 250 us of the app's turns).
- **frame**: the longest a kept frame took from its END to the last slice drawn, and in how many turns; +w whole
  frames, +o frames too long to keep.
- **PANEL_R**: how many went to TCP this second and the longest gap between two in a session (500 ms when well);
  **txdrop**: messages TCP's ring had no room for.
- **rxfull**: the longest stretch TCP's input ring was (nearly) full, and how often.
- **sess**: session starts; ends by the host (h), the keepalive (k), the connection (c); **reset**: the last shadow
  reset's reason.
- **rexmit**: lwIP's retransmission episodes on the connection, the most retries in a row, the longest RTO; **wnd**:
  the smallest send / receive windows; **persist**: zero-window probes begun; **txq**: the most bytes waiting to go;
  **tcp / link**: segments out / in, drops, out-of-memory, the pbuf pool's failures. **heap**: used / peak.

`wifi_probe.py mode direct` turns whole frames off (until the unit restarts), `mode whole` on again: an A/B without a
flash. The Move's `/api/status` fm1 adds `draw_us_max` (the longest FRAME_ACK draw time), `panel_r_gap_max`,
`shadow_resets`, `reset_reason` and `wifi_rx_ms_max` (the longest silence on the connection).
