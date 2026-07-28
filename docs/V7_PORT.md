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

## Hardware bring-up findings (first live session — each cost real debugging)

1. **RGB path wants byte-swapped 565.** `lcd.setSwapBytes(true)` in the flush;
   LVGL stays `LV_COLOR_16_SWAP 0`. Symptom of getting it wrong: `#0c1119`
   background renders olive (128,96,64), AA text gets chromatic fringes.
2. **LVGL draw buffer must be internal SRAM** (single 800×60). PSRAM draw
   buffers put render-writes + flush-reads on the bus the panel DMA scans —
   visible wiggle whenever the map forced real compositing.
3. **…but then LVGL's heap must move to PSRAM** (`ps_malloc` in lv_conf.h),
   or widgets + draw buffer together starve internal RAM below the ~50 KB
   mbedTLS needs per handshake and every HTTPS fetch dies (STALE + no map).
4. **One TLS connection at a time** (`tlsTryAcquire` in state.h). Selecting an
   airliner fired logo+route+cloud TLS simultaneously — 3×50 KB — and stalled
   the whole pipeline. Feed has priority; everything else defers and retries.
5. **Style writes invalidate even when values are unchanged.** Every dynamic
   widget needs change-caching (see the Blip caches / setTextCached) or the
   scope repaints constantly and fights the panel DMA.
6. **LVGL flex under-measures SIZE_CONTENT boxes** in these row layouts and
   clips children from the *left* ("DHCP" → "CP"). Fixed-width value labels,
   right-aligned, `LONG_DOT`.
7. **Screen diagnostics beat serial**: with CDC-on-boot the runtime log needs
   the UART switch flipped, but `/metrics` (heap!), `/api/state`,
   `/api/probe?url=` (the device fetches a URL and reports code+ms) and
   `/screen.bmp` (literal framebuffer) diagnose almost everything over LAN.
   The 4 ms `-1` probe result is how the "firewall vs firmware" feeder
   question was settled (instant REJECT vs 2 s timeout for a DROP).
8. CARTO tint: ×1.6 luminance lift (×2.6, tuned on a z8 tile, is neon at the
   z9–z11 the scope really uses).
9. **Repeated TLS connections leak ~1.5 KB each** somewhere in esp-tls/mbedTLS
   (core 3.3.10) — measured via A/B: heap drifted −120 B/s with the 15 s
   wheretheiss.at poll and recovered flat with it off. Consequences baked in:
   ISS uses open-notify over **plain HTTP**; and `tlsTryAcquire(essential)`
   has a 45 KB heap floor so a residual drift sheds logos/routes/weather
   before it can ever starve the aircraft feed. Rare TLS (weather 15 min,
   logos/routes once per selection) leaks negligibly; a device parked on the
   **cloud fallback** (TLS every 8 s) will still drift — fix the local feeder
   reachability rather than living on cloud long-term.

## Deferred (tracked, not forgotten)

Real airline logo pack in FATFS (monogram tile ships now) · aircraft photos ·
ETA on routes · session stats view · trails on the scope · web `/live` page.
