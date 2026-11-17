# libmoveremote (vendored)

The host side of **Move Remote**, from <https://github.com/Quixotic7/abletonmoveremote> at commit `bf01bb9`
(v0.1.0), under the Mozilla Public License 2.0 (`LICENSE`, the same text as `LICENSES/MPL-2.0.txt`).

| Here | Upstream |
|---|---|
| `include/moveremote/proto.h`, `client.h`, `gfx.h` | the same paths |
| `host/mr_client.c`, `host/mr_gfx.c` | the same paths |
| `common/font5x7.c`, `common/font5x7.h` | the same paths |

The files are byte-for-byte upstream's: no change was needed to build them with `host/CMakeLists.txt` (C99 with GNU
extensions, which `mr_client.c`'s `__thread` and `_GNU_SOURCE` need). The upstream layout is kept so that the
sources' relative includes (`../include/moveremote/...`, `../common/...`) work as they are.

`host/CMakeLists.txt` builds two libraries from them:

- `moveremote_gfx` (`mr_gfx.c`, `font5x7.c`): portable, no OS calls. `rfm_move` (`host/src/move/`) uses only this and
  the types in `proto.h` / `client.h`.
- `moveremote` (`mr_client.c`): the POSIX socket client (`mr_connect`, `mr_poll`, `mr_decode`, the LED and display
  sends). Hosts and the end-to-end test link it; `rfm_move` does not.

To update: copy the same files from a newer upstream commit and change the commit named above.
