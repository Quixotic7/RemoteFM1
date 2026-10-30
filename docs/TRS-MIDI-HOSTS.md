# The FM-1's TRS MIDI in, for hosts (MIDI_IN)

For the maintainers of hosts that run a firmware with a RemoteFM1 unit as its panel: the iOS app (RemoteFM1IOS)
and FM1VST. The protocol itself is docs/PROTOCOL.md (section 4, "MIDI_IN"); the constants are proto/rfm1.h.

## What the unit does

The FM-1 has a TRS MIDI input. RemoteFM1 (after 0.2.3) does this with it:

- **Standalone** (no host): the DX7 plays it, as it plays USB and Bluetooth MIDI in: notes, bend, CCs, channel
  aftertouch, program change on its channel; clock / start / continue / stop for the tempo, arp and loops. MIDI
  REMOTE ignores it, like the other MIDI in.
- **In a host's session**: it sends what comes into the jack to the host as **MIDI_IN** messages, on the session's
  transport. The host feeds it into the firmware it runs, as that firmware's own MIDI in.

## The message

| | |
|---|---|
| command | `RFM_MIDI_IN` = `0x54`, device → host |
| framing | as every RemoteFM1 message: SysEx `F0 7D 52 46 54 <body, 7-in-8 packed> F7`, or bulk `'R' 'F' 54 00 <len16> <seq32> <body>` |
| body | 1..64 bytes (`RFM_MIDI_IN_MAX`) of whole MIDI messages, each with its status byte, in arrival order |
| message lengths | `8n 9n An Bn En`: 3 bytes; `Cn Dn`: 2; `F8 FA FB FC` (clock, start, continue, stop): 1 |
| never in it | running status, SysEx, other system messages, Active Sensing |
| when | only during a session (after SESSION start=1 and its ACK, until it ends) |
| caps | HELLO_R `caps` bit9, `RFM_CAP_MIDI_IN` (0x200): the unit forwards its jack. 0.2.3 and earlier: no bit, no MIDI_IN |

The unit batches whatever came in since its last loop turn (usually one message per MIDI_IN) and sends only when the
transport has room, so frames and FRAME_ACKs are never delayed by it. A burst that finds its 64-byte batch full while
the link is busy is dropped there, not queued.

## What a host must do

1. **Parse 0x54** in its message dispatch; ignore it when not in a session. Hosts that do not know it already ignore
   it (an unknown command), so older hosts keep working with a new unit.
2. **Feed the body to the running firmware's MIDI input**, exactly where the computer's own MIDI input goes. The
   body is raw MIDI bytes, ready for a byte-stream input. For a core that takes USB-MIDI event packets, split by
   status (lengths above) and make one packet per message: cable 0, CIN = status >> 4 (`0xF` for the real-time
   bytes), then the status and up to two data bytes, zero-padded:
   `pkt = (st >> 4) | st << 8 | d1 << 16 | d2 << 24` (byte 0 = cable/CIN, as `fm1core.h`'s `midi_in(uint32_t pkt)`
   and fm1-base's `plat_midi_in` have it).
3. Deliver it on the thread that owns the core's MIDI input (the same queue as other MIDI in), in order. A clock
   from the jack is the firmware's clock: do not also send the host's own clock to the firmware unless the user
   picks it.
4. Optionally show "TRS MIDI" in the connection's details when `caps` bit9 is set.

## With libremotefm1 (host/)

- `rfm_session_cb_t` has a new last member, `midi_in(void *ctx, const uint8_t *msgs, uint32_t n)`: called once per
  valid MIDI_IN while the session is active, with 1..64 bytes of whole messages. **Zero the struct** (or set the
  member, NULL to drop) before `rfm_session_new`: a C caller that fills the struct member by member on the stack
  would otherwise leave garbage there. C++ aggregate init (`rfm_session_cb_t cb{...}`) zero-fills it.
- `rfm_midi_msg_len(status)` (1..3, 0 for a byte that starts no message) splits a body into messages;
  `rfm_parse_midi_in(b, n)` validates one (the session already does; malformed bodies are counted in
  `rfm_session_stats_t.midi_in_bad` and never delivered). `midi_in` / `midi_in_bytes` count the delivered ones.
- The fake unit forwards MIDI into its jack with `rfm_fake_trs(f, msgs, n)` (cfg `midi_in`, on by default) for tests.

Example (FM1VST's core API):

```c
static void on_trs(void *ctx, const uint8_t *b, uint32_t n)
{
    uint32_t k = 0;
    while (k < n) {
        uint32_t m = rfm_midi_msg_len(b[k]), st = b[k];
        uint32_t d1 = m > 1 ? b[k + 1] : 0, d2 = m > 2 ? b[k + 2] : 0;
        my_midi_queue_push((st >> 4) | st << 8 | d1 << 16 | d2 << 24);   /* drained into core->midi_in(pkt) */
        k += m;
    }
}
```

## In this repo

- Firmware: `firmware/src/rfm_main.c` (`rf_trs_in`, `rf_trs_flush`), `firmware/app.h` (`FM1B_TRS 1`); simulator test
  `firmware/tests/trs.txt`.
- libremotefm1: `host/src/session.c`, `host/src/codec.c`, `host/src/fake.c`; test `host/tests/test_session.c`
  (`test_midi_in`).
- The website: `web/remote/src/rfm_web.c` (`Module.rfmMidiIn`), `web/remote/remote.js` (`onMidiIn`),
  `web/site/js/use-fm1.js` (into `site.midiIn`, the firmware's MIDI in); tests `web/remote/test/test_remote.mjs`,
  `web/site/test.mjs`.
- fm1remote (macOS) does not forward it yet (it has no MIDI-in path into its core).
