# Licensing of these packages

Source: <https://github.com/Quixotic7/RemoteFM1>, at each release's tag. The repository's own
[LICENSING.md](https://github.com/Quixotic7/RemoteFM1/blob/main/LICENSING.md) says who made what.

| Package | RemoteFM1's firmware | Also in the package |
| --- | --- | --- |
| `remote-0.2.3.fwsc` (USB + Bluetooth MIDI) | MPL-2.0 (`LICENSES/MPL-2.0.txt`) | fm1-base and felucca-bsp (MPL-2.0); JieLi's Bluetooth library, linked as a binary (Apache-2.0); the JieLi AC79 SDK's `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` (Apache-2.0); the Barlow Semi Condensed font (OFL-1.1) |
| `remote-usb-0.2.3.fwsc` (USB only) | MPL-2.0 (`LICENSES/MPL-2.0.txt`) | fm1-base and felucca-bsp (MPL-2.0); the JieLi AC79 SDK's `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` (Apache-2.0); the Barlow Semi Condensed font (OFL-1.1). No Bluetooth library |
| `remote-0.2.2.fwsc` (USB + Bluetooth MIDI) | MPL-2.0 (`LICENSES/MPL-2.0.txt`) | fm1-base and felucca-bsp (MPL-2.0); JieLi's Bluetooth library, linked as a binary (Apache-2.0); the JieLi AC79 SDK's `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` (Apache-2.0); the Barlow Semi Condensed font (OFL-1.1) |
| `remote-usb-0.2.2.fwsc` (USB only) | MPL-2.0 (`LICENSES/MPL-2.0.txt`) | fm1-base and felucca-bsp (MPL-2.0); the JieLi AC79 SDK's `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` (Apache-2.0); the Barlow Semi Condensed font (OFL-1.1). No Bluetooth library |
| `remote-0.2.1.fwsc` (USB + Bluetooth MIDI) | MPL-2.0 (`LICENSES/MPL-2.0.txt`) | fm1-base and felucca-bsp (MPL-2.0); JieLi's Bluetooth library, linked as a binary (Apache-2.0); the JieLi AC79 SDK's `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` (Apache-2.0); the Barlow Semi Condensed font (OFL-1.1) |
| `remote-usb-0.2.1.fwsc` (USB only) | MPL-2.0 (`LICENSES/MPL-2.0.txt`) | fm1-base and felucca-bsp (MPL-2.0); the JieLi AC79 SDK's `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` (Apache-2.0); the Barlow Semi Condensed font (OFL-1.1). No Bluetooth library |
| `remote-0.1.1.fwsc` | GPL-3.0-only (`LICENSE`) | TransitFM1's platform, from Dinghy and Felucca (GPL-3.0-only); the JieLi AC79 SDK's three files (Apache-2.0); the Terminus font (OFL-1.1) |

## 0.2 and later: MPL-2.0

The firmware moved onto [fm1-base](https://github.com/charlesvestal/fm1-base) (Charles Vestal) and
[felucca-bsp](https://github.com/hugelton/felucca-bsp) (Leo Kuroshita, Hügelton Instruments), both MPL-2.0. Its own
files are MPL-2.0 too, by its author's choice, so it can link JieLi's Bluetooth library, which a GPL firmware could
not be given out with. MPL-2.0 applies file by file: if you change one of these files and give the firmware to
someone, publish your change to that file.

## 0.1.1: GPL-3.0-only

0.1.1 was built on [TransitFM1](https://github.com/Quixotic7/TransitFM1)'s platform, from
[Dinghy](https://github.com/zvenson/dinghy) and [Felucca](https://github.com/hugelton/Felucca). It stays under the GNU
General Public License, version 3 only. If you give someone a firmware built from it, give them its complete source
under the same licence too.

## Third-party licences

- `LICENSES/Apache-2.0.txt`: the JieLi AC79 SDK (<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>): its Bluetooth
  libraries in the Bluetooth package, its three boot and configuration files in every package.
- `LICENSES/OFL-1.1-Barlow.txt`: Barlow Semi Condensed (0.2 and later).
- `LICENSES/OFL-1.1-Terminus.txt`: Terminus (0.1.1).

## Trademarks

"M-VAVE" and "FM-1" belong to their owners; "Felucca" and "Hügelton Instruments" are names of Hügelton Instruments.
RemoteFM1 is not made or endorsed by M-VAVE. 0.1.1 answers the update protocol with the official firmware's
identity (`FM-1_015`) so that installers accept it; 0.2 answers as itself (`FM-1_52XXYYZZ`).
