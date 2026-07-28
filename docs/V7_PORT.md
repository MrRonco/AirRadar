# AirRadar v7 — LVGL port architecture

v7 is the full UI rewrite: the hand-composited LovyanGFX sketch (v5/v6) becomes
an LVGL 8.3 application with the browser-verified "North Star" design, while
LovyanGFX drops down to what it is best at — the proven RGB-panel/GT911 driver
and PNG decoding. The Arduino toolchain stays (deliberately: one `arduino-cli
compile` away from a flashable image, no ESP-IDF).

## Layout

```
firmware/
  lv_conf.h                LVGL config — copy beside the lvgl library folder
  BUILD.md                 macOS build / flash / OTA instructions
  tools/genassets.py       regenerates the image assets (Pillow)
  AirRadar/
    AirRadar.ino           boot + main loop (thread owner)
    src/config.h           every constant: geometry, timing, NVS keys, URLs
    src/core/  types.h state.{h,cpp} tracks.{h,cpp}
    src/net/   feeder.* enrich.* maptiles.*      (core-0 task modules)
    src/ui/    theme.* ui.h ui_nav.cpp ui_scope.cpp ui_cards.cpp ui_settings.cpp
    src/hal/   hal_display.*                     (panel/touch/backlight/LVGL glue)
    src/svc/   web.* mqtt.*
    src/assets/ img_*.c font_*.c                 (generated: glyphs + Inter/JBM fonts)
```

## Threading model (the invariant everything hangs off)

* `loop()` (core 1) owns **all** LVGL calls, `g_tracks`, settings writes, NVS.
* Network modules spawn short-lived tasks on core 0. A task gets a **snapshot**
  of the settings it needs (captured in loop context before spawn), writes its
  result into a dedicated pending structure under `g_dataMux`, sets a volatile
  ready flag **last**, clears its in-progress flag and dies.
* `loop()` drains pending → live state → targeted UI refresh. No LVGL locks
  needed anywhere because LVGL is single-threaded by construction here.

## Rendering

* LVGL draw buffers: 2 × 800×120 RGB565 in PSRAM; flush = `lcd.pushImage`
  into LovyanGFX's RGB-panel framebuffer (panel scans it via DMA).
* The scope is a circle-clipped container: CARTO base map image (fetched,
  stitched, blue-tinted in `maptiles.cpp`, double-buffered), hairline rings,
  and a pooled set of blip widgets (glow + recolorable jet + label). Target
  moves **animate** (ease-out glide) instead of teleporting each poll.
* All hardware bring-up rules from CLAUDE.md carry over (OPI PSRAM, 14 MHz
  pclk, GT911@0x5D controlled reset, `Wire.end()` handoff, `WiFi.setSleep(false)`).

## Data sources (unchanged philosophy: local feeder is the aircraft truth)

readsb/tar1090 primary → airplanes.live fallback · adsbdb routes ·
Open-Meteo weather · wheretheiss ISS · CARTO dark tiles. All keyless.

## Services

* Web: settings page + `/api/state` + `/api/config` + `/screen.bmp` +
  `/metrics` + `/update` (OTA), HTTP Basic auth when a panel password is set.
* MQTT: Home Assistant discovery (sensors + emergency binary_sensor), LWT
  availability, 5 s state publish.

## Deferred (tracked, not forgotten)

Real airline logo pack in FATFS (monogram tile ships now) · aircraft photos ·
ETA on routes · session stats view · trails on the scope · web `/live` page.
