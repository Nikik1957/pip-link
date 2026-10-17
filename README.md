# PIP LINK

A companion unit for the Wand Company **Pip-Boy 3000**. A Seeed XIAO ESP32-S3 plugs into the Pip-Boy's USB-C port and turns the Pip-Boy into a field terminal:

- **MESH**: MeshCore or Meshtastic radio over Bluetooth. Channels, favorite contacts, a contacts page (message / favorite / remove), direct messages and ADVERTISE.
- **MAP**: offline street and topo maps from the Pip-Boy's SD card. Shows your GPS position, mesh contacts and saved locations, with NAV lines. Locations can be shared over the mesh.
- **RADIATION**: a RadiaCode 102 over Bluetooth, shown in rads like the game, with a graph and alarm.
- **TERM**: SSH and Telnet (BBS mode included) over Wi-Fi.
- **SETTINGS**: radio, keyboard and RadiaCode pairing, theme (follows the Pip-Boy's own color), wheels, text margin.

The Pip-Boy's side switch picks the tab. STATS, ITEMS and DATA are shortcuts, and they blink when something is waiting. A Bluetooth LE keyboard can be used for typing.

The Pip-Boy runs only a thin terminal holotape (`pipboy/TERM`). The ESP32-S3 does all the work and drives the Pip-Boy's Espruino REPL as a USB host.

## Using it

Download `PIP LINK Setup.exe` from the [Releases](../../releases) page and read **READ ME FIRST.txt**. The setup app does three things:
- flashes the XIAO,
- installs the holotape on the Pip-Boy,
- builds and sends maps.

## Project layout

| Path | What |
|---|---|
| `src/hub_main.cpp` | The PIP LINK unit's firmware (PlatformIO env `hub`) |
| `lib/` | USB host link + terminal protocol (`pipterm`), MeshCore / Meshtastic / RadiaCode / keyboard BLE drivers, SSH, VT100 |
| `pipboy/TERM/` | The Pip-Boy holotape: `APP.js` plus `MAP.js` / `KB.js`, loaded on demand |
| `app/` | The Windows setup app (Python + Tk) |
| `tools/` | Map building (`map_lib.py`), packaging (`make_update.py`, `pack_release.py`), USB test tools |
| `docs/` | Protocol notes and feature docs |

## Building

- **Firmware**: install [PlatformIO](https://platformio.org), then run `pio run -e hub`.
- **Setup app**: needs Python 3.12 with the packages the app imports. Run `python app/build_exe.py <version>`, which packages the firmware and the minified holotape too.
- **A release folder**: run `python tools/pack_release.py <version>`.

PIP LINK is MIT licensed (see `LICENSE`). Third-party code and data licenses are listed in `release/THIRD-PARTY.txt`.
