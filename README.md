# AirRadar

Real-time ADS-B air-traffic radar on a Waveshare ESP32-S3-Touch-LCD-7 (800×480).
**v7** is a full LVGL 8.3 application: anti-aliased Inter typography, a CARTO
dark base map with city labels under the scope, animated aircraft glyphs
coloured by altitude, airline/operator tiles, adsbdb routes, live weather, the
ISS overhead, Home Assistant (MQTT discovery), a JSON API + live screenshot,
and OTA updates — fed by a local adsb.im/tar1090 receiver with airplanes.live
as automatic cloud fallback. All data sources are keyless; aircraft data never
leaves your own antenna path.

## Install

- **One-click (Chrome/Edge):** open the web flasher (GitHub Pages from
  `flasher/`), connect USB-C, Install. Done.
- **From source:** see `firmware/BUILD.md` — exact macOS steps, pinned
  versions, and the `lv_conf.h` gotcha.
- **Updates:** `http://airradar.local/` → Firmware → upload
  `airradar-ota.bin`. No cable.

## Repo layout

- `firmware/` — **v7** LVGL application (`AirRadar/` sketch + `BUILD.md` + tools)
- `flasher/` — ESP Web Tools one-click installer (host on GitHub Pages)
- `AirRadar.ino` + `LGFX_Waveshare_7.h` — legacy v6 single-sketch app (kept as
  reference; the hardware bring-up ground truth lives on in `firmware/.../hal/`)
- `CLAUDE.md` — project context and hard-won rules for AI-assisted development
- `docs/` — hardware notes, version history, plans (`V7_PORT.md` is current)

Built iteratively on real hardware; see `docs/HISTORY.md` for the journey.
