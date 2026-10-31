# The FM-1's key velocity and pressure, for hosts (TOUCH)

For the maintainers of hosts that run a firmware with a RemoteFM1 unit as its panel: the iOS app (RemoteFM1IOS)
and FM1VST. The protocol itself is docs/PROTOCOL.md (section 2 "TOUCH", section 4 "Panel events" and "PANEL_R");
the constants are proto/rfm1.h.

## What the unit does

RemoteFM1 0.3.1 can sense its keys in panel mode as its standalone instrument does (fm1-base's lib/touch): the
velocity each key is played at, and how hard a held key is pressed. It sends them only to a host that asks:

- **Velocity**: a key's Note On carries its own velocity, 1..127, instead of the fixed 100.
- **Pressure**: a held key's pressure goes out as poly aftertouch, 0..127.

The 20 keys F3..C5 (keys 0..19) have both. The 7 top keys C#5..G5 (keys 20..26) have velocity only (no pressure
sensor on that row). With either on, a key's press is reported when the sensing has measured it, a few ms after the
contact. A host that never asks gets exactly what 0.3.0 sent: velocity 100, no aftertouch.

The standalone instrument's own VELOCITY setting (its settings page) does not apply in panel mode: there it is the
host's choice.

## The bytes

| | |
|---|---|
| caps | HELLO_R `caps` bit10, `RFM_CAP_TOUCH` (0x400). 0.3.1's full caps: 0x7AF (0x62C without the shadow) |
| opt in | **TOUCH** `RFM_TOUCH` = `0x14`, host → device, body 1 byte `flags8`: bit0 `RFM_TOUCH_F_VEL`, bit1 `RFM_TOUCH_F_PRESS`; other bits 0 |
| framing | as every RemoteFM1 message: SysEx `F0 7D 52 46 14 <body, 7-in-8 packed> F7`, or bulk `'R' 'F' 14 00 <len16> <seq32> <body>` |
| when | in a session, on the session's transport, after the SESSION start's ACK. **Every session starts with 0**: send it again after each SESSION start (a reboot, a lost session, a restart) |
| reply | **PANEL_R** (0x53). Its `flags8` (byte 24) carries what is in force: bit1 velocity, bit2 pressure (every PANEL_R, every 500 ms) |
| velocity | Note On `9F 53+k v`, v = 1..127 (Note Off unchanged: `8F 53+k 0`) |
| pressure | Poly aftertouch `AF 53+k p`, p = 0..127, on channel 16 as the keys. Sent only while the key is held, on a change of ≥ 2 or on reaching 0, at most every 10 ms per key; a press starts from 0 and the Note Off ends it (no `AF … 0` follows) |
| off | TOUCH with `flags8 = 0`: velocity 100 and no aftertouch again, at once |

Wire budget: at most 100 aftertouch messages a second per held key (3 bytes each), in practice far fewer (the
pressure moves every 5 ms, smoothed, and a still finger sends nothing).

Robustness: if a PANEL_R's flags differ from what you asked, send TOUCH again, but ignore a PANEL_R that arrives
within ~300 ms of your TOUCH (the unit sends one right after the SESSION ACK, before your TOUCH reached it; the
TOUCH's own PANEL_R follows).

## Feeding the core

The FM1VST core API (`core-api/fm1core.h` `fm1panel_t`, the website's `setPanel`, ChoralRoot's `emu_hal_t`):

- `key_vel[k]`: the key's velocity 1..127, set **before** its bit in `keys` goes down; 0 means "none given" (the
  firmware uses its own default). Without TOUCH, or from a unit before 0.3.1, give 0, not 100.
- `key_lean[k]`: the key's pressure as **0..100**: `lean = (p * 100 + 63) / 127` (rounded), 0 when the key is up.

Which firmwares use them: only those whose core reads `key_vel` / `key_lean` (in FM1VST's glue: ChoralRoot from
0.25.1 built with `FM1CORE_CRF_VEL`; the others take the default). On the website, the native (`.fm1web`) and
`.rfm1` engines pass both to the core; the emulated engine (a stock firmware on the CPU emulator) cannot: its
firmware scans the key matrix itself, which has no velocity.

## With libremotefm1 (host/)

- `rfm_session_cfg_t.key_touch` (new last member): `RFM_TOUCH_F_VEL | RFM_TOUCH_F_PRESS` by default
  (`rfm_session_cfg_default`), 0 off. The session sends TOUCH after every SESSION ACK when the unit's caps have
  bit10, and again when PANEL_R says otherwise. `rfm_session_set_key_touch(s, flags)` changes it at once (a
  setting "Velocity & pressure from my FM-1"). A C caller that fills the cfg by hand must set it (or start from
  `rfm_session_cfg_default`).
- `rfm_event_t` has a new last member `vel`: on an `RFM_EV_KEY` down event, the key's velocity 1..127, or 0 (none
  given: TOUCH velocity off, or an older unit's 100).
- `RFM_EV_PRESS` (5): `index` the key, `value` its pressure 0..127. A key let go gets an `RFM_EV_PRESS` 0 after its
  `RFM_EV_KEY` up if its pressure was not 0.
- `rfm_panel_t` has `touch` (the flags in force), `key_vel[27]` (each key's last press's velocity) and
  `key_press[27]` (0..127, 0 up); `rfm_panel_lean(p, k)` gives `key_lean` 0..100.
- `rfm_session_stats_t`: `touch_sent` (TOUCH messages sent) and `touch` (the flags in force now).
- The fake unit: cfg `touch` (on by default), `rfm_fake_key_vel(f, k, down, vel)`, `rfm_fake_press(f, k, p)`,
  `rfm_fake_touch(f)`.

Example (FM1VST, events into `fm1panel_t` on the thread that owns the panel):

```c
static void on_event(void *ctx, const rfm_event_t *e)
{
    fm1panel_t *p = my_panel(ctx);
    switch (e->type) {
    case RFM_EV_KEY:
        if (e->value) {
            p->key_vel[e->index] = e->vel;               /* before the key's bit (0: the firmware's default) */
            p->keys |= 1u << e->index;
        } else {
            p->keys &= ~(1u << e->index);
            p->key_lean[e->index] = 0;
        }
        break;
    case RFM_EV_PRESS:
        p->key_lean[e->index] = (uint8_t)((e->value * 100 + 63) / 127);
        break;
    /* buttons, encoders, MASTER as before */
    }
}
```

Without libremotefm1 (the iOS app's own Swift session): parse `AF` on channel 16 like `9F` / `8F`, keep the Note
On's third byte as the velocity once TOUCH velocity is in force, send `F0 7D 52 46 14 <packed 03> F7` after each
SESSION ACK when `caps & 0x400`, and watch PANEL_R byte 24 bits 1 and 2.

## In this repo

- Firmware: `firmware/src/rfm_app.c` (`RFM_TOUCH`, `rfm_app_panel`'s velocity and aftertouch), `firmware/src/rfm_main.c`
  (the keys sensed in panel mode while TOUCH is on), `firmware/sa/sa_main.c` (`sa_panel_touch`; the standalone's
  VELOCITY setting); simulator test `firmware/tests/standalone_velocity.txt`.
- libremotefm1: `host/src/session.c`, `host/src/panel.c`, `host/src/codec.c`, `host/src/fake.c`; test
  `host/tests/test_session.c` (`test_touch`).
- The website: `web/remote/src/rfm_web.c` (`rw_set_key_touch`, `rw_key_vel`, `rw_key_press`), `web/remote/remote.js`
  (`keyTouch` / `setKeyTouch`, the panel's `keyVel` / `keyLean`, `status.unit.touch`), `web/site/js/use-fm1.js`
  (the sheet's "Velocity & pressure from my FM-1", on by default), `web/site/js/panel.js` (`leanKey`),
  `web/site/worklet/host.js` (`keyLean` to the engine); tests `web/remote/test/test_remote.mjs`, `web/site/test.mjs`.
- fm1remote (macOS) does not feed them yet (its build needs FM1VST's engine sources).
