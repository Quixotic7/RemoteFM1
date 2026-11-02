<!-- SPDX-License-Identifier: MPL-2.0 -->
# The M-Vave SMK-37's patch series

`../../../build.sh` applies these only when `BOARD=smk37` (or `SMK_SOURCES=1`), after the common series in
`../fm1-base/` and `../felucca-bsp/`, so an FM-1 build's inputs are what they were (docs/SMK37.md).

- `felucca-bsp/0001-smk37-board.patch`: `boards/smk37/` (`smk_decode.h`, the panel's signals decoded, host-testable;
  `smk_input.h`, the scan; `smk_lcd_hw.h`, the panel and the pads' LEDs), the SMK's flash map and the driver's second
  net (`chip/fm1_flash.h`), the update for the SMK's map (`chip/ota.c`), the update loader (`loader/`). Everything
  behind `BOARD_SMK37`; a build without it compiles as before.
- `fm1-base/0001-smk37-board.patch`: `tools/build.py` (`BOARD`, `MODEL`), `tools/fm1pkg_make.py` (the SMK's layout,
  36 identity blocks), the platform on the SMK (`device/smk_panel.c`, `plat_smk37.c`, `smk_store.c`, `lib/touch_smk.c`;
  `unity.c`, `main_fm1.c`, `plat_fm1.c`, `panel.c`, `lcd.c`, `include/plat.h`), the simulator's SMK controls
  (`host/plat_host.c`). Behind `BOARD_SMK37`.
- `fm1-base/0002-smk37-web.patch`: the browser emulator on the SMK's panel (`tools/build.py --web` with `BOARD=smk37`;
  `web/emu/emu_web.c`'s `web_smk_*` exports, behind `BOARD_SMK37`). Its page is RemoteFM1's (`web/emu-smk37/`).
- `app/0001-smk37-app.patch`: RemoteFM1's own (`firmware/`): applied to a copy, `build/app-smk37/`, never to
  `firmware/` itself, because fm1-base's build hashes the app's sources into the image (`FM1B_BUILD`). Behind
  `BOARD_SMK37` (and the simulator's commands). Merge it into `firmware/` with the next FM-1 release, whose
  packages change anyway.

To change one: edit the patched copy (`build/fm1-base`, `build/fm1-base/bsp`, `build/app-smk37`, after a
`BOARD=smk37` build) and diff it against the same tree with only the patches before it (`diff -ruN`, paths `a/` and
`b/` from that tree's root, no timestamps).

These are RemoteFM1's own: the porting guide (genkigenki's fm1-to-smk37) holds the facts they follow, ChoralRoot's
SMK-37 port (GPL-3.0) none of their code.
