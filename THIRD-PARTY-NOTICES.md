# Third-party notices

AirRadar itself is licensed **GPL-3.0-or-later** (see [`LICENSE`](LICENSE)).

This file lists everything else that ships with it. It matters most for the
pre-built images in [`flasher/`](flasher/), because those **statically link** the
libraries below — distributing a binary carries the same notice obligations as
distributing the source.

Full licence texts are in [`LICENSES/`](LICENSES/).

---

## Compiled into the firmware binaries

| Component | Licence | Text |
|---|---|---|
| [LVGL](https://lvgl.io) 8.3.11 | MIT | [`MIT-LVGL.txt`](LICENSES/MIT-LVGL.txt) |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) 1.2.25 | BSD-2-Clause (FreeBSD) | [`BSD-2-Clause-LovyanGFX.txt`](LICENSES/BSD-2-Clause-LovyanGFX.txt) |
| [ArduinoJson](https://arduinojson.org) 6.21.6 | MIT | [`MIT-ArduinoJson.txt`](LICENSES/MIT-ArduinoJson.txt) |
| [PubSubClient](https://github.com/knolleary/pubsubclient) 2.8 | MIT | [`MIT-PubSubClient.txt`](LICENSES/MIT-PubSubClient.txt) |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) core 3.3.10 | LGPL-2.1-or-later | [`LGPL-2.1.txt`](LICENSES/LGPL-2.1.txt) |
| ESP-IDF, mbedTLS and related Espressif components | Apache-2.0 | [`Apache-2.0.txt`](LICENSES/Apache-2.0.txt) |

All of the above are compatible with GPL-3.0-or-later. Note that the Apache-2.0
components are compatible with GPL **version 3 only** — this project cannot be
distributed under GPL-2.0.

### LGPL note

The arduino-esp32 core is LGPL-2.1-or-later and is statically linked into the
images in `flasher/`. The corresponding source is published by Espressif at
<https://github.com/espressif/arduino-esp32>, and the full build recipe needed to
relink AirRadar against a modified core is in
[`firmware/BUILD.md`](firmware/BUILD.md).

---

## Fonts

The `font_*.c` files under `firmware/AirRadar/src/ui/` are glyph data generated
with `lv_font_conv` from the upstream fonts below. **Those generated files remain
under the SIL Open Font License 1.1, not GPL-3.0** — OFL clause 5 requires the
font software to be distributed entirely under the OFL. The GPL covers the rest of
AirRadar.

| Font | Copyright | Licence |
|---|---|---|
| [Inter](https://rsms.me/inter/) | The Inter Project Authors | [`OFL-1.1-Inter.txt`](LICENSES/OFL-1.1-Inter.txt) |
| [JetBrains Mono](https://www.jetbrains.com/lp/mono/) | The JetBrains Mono Project Authors | [`OFL-1.1-JetBrainsMono.txt`](LICENSES/OFL-1.1-JetBrainsMono.txt) |
| [Montserrat](https://github.com/JulietaUla/Montserrat) (via LVGL's built-in symbol fonts) | The Montserrat Project Authors | [`OFL-1.1-Montserrat.txt`](LICENSES/OFL-1.1-Montserrat.txt) |

Neither Inter nor JetBrains Mono declares a **Reserved Font Name**, so naming them
here, in source comments, and in symbols such as `font_hero56` is permitted. The
modified TTFs produced by `pyftfeatfreeze -f tnum` are OFL Modified Versions and
remain OFL.

---

## Map data and imagery

Base map tiles are fetched at runtime from **CARTO** and rendered on-device. Map
images committed to this repository under `docs/` are derived rasters of those
tiles.

> Base map © [CARTO](https://carto.com/attributions) ·
> Map data © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors

OpenStreetMap data is licensed under the
[Open Database License (ODbL)](https://opendatacommons.org/licenses/odbl/). If you
redistribute rendered tiles or screenshots containing them, that attribution
travels with them.

---

## Runtime data sources

These are queried over the network at runtime. No data from them is redistributed
in this repository, but each has its own terms:

| Service | Used for |
|---|---|
| Your own readsb / tar1090 feeder | Aircraft positions (primary) |
| [airplanes.live](https://airplanes.live) | Aircraft positions (fallback) |
| [adsbdb](https://www.adsbdb.com) | Airline names and routes |
| [Open-Meteo](https://open-meteo.com) | Weather |
| open-notify | ISS position |
| [esp32flight-logos](https://github.com/theqkash/esp32flight-logos) | Airline logo bitmaps, fetched on demand |

Airline names, liveries and logos are the trademarks of their respective airlines.
They are displayed for identification only; no endorsement or affiliation is
implied, and no trademark licence is granted by this project.

---

## Origin

AirRadar v1 began in 2024 as a port of Mirko Pavleski's CrowPanel ADS-B radar
project, published under GPL-3.0-or-later. The codebase has since been rewritten
several times and v7 shares no code with it, but AirRadar is released under
GPL-3.0-or-later in keeping with that origin.
