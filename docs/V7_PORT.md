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
9. **RETRACTED — the drain is not per-TLS-connection.** This note used to
   claim ~1.5 KB leaked per secure connection. Re-measured on 2026-07-29 with
   a dedicated counter (`airradar_tls_conn`): a 90 s window in which the
   counter did **not move at all** still lost 6.8 KB. Per-subsystem
   instrumentation then attributed the whole ~72 B/s to **feeder run count**
   (~150–210 B per poll, 30 polls/min), with iss contributing exactly 0.
   Two fixes were tried against that and both measured as no-ops, so do not
   repeat them: `SO_LINGER`/abortive close (reverted — with HTTP/1.0 the
   *server* closes first, so the device never held TIME_WAIT) and HTTP
   keep-alive on the feeder (kept for hygiene — 30× fewer connections — but
   the drain stayed at 72.8 B/s). **The drain is still unexplained.** It is
   now largely neutralised rather than solved: the map and routes both cache
   to FATFS and keep working with the TLS gate shut. Weather and new logo
   fetches remain exposed. Next step is real heap tracing, not another
   hypothesis.
10. **Non-uniform font codepoint ranges.** Regenerating all faces with one
   uniform range silently dropped U+2039/203A and turned the range stepper's
   chevrons into tofu. Per-face ranges are listed in CLAUDE.md.
11. **`lv_obj_set_ext_click_area`** is how a small glyph keeps a 48 px touch
   target — used by both the gear and the "?" (26 px drawn, 48 px tappable).

## Deferred (tracked, not forgotten)

ETA on routes · session stats view · trails on the scope · web `/live` page ·
aircraft type silhouettes.

**Aircraft photos are blocked on licensing, not code.** Planespotters' photo
API is free and keyless and LovyanGFX *does* have a JPEG decoder (`drawJpg`),
so the panel could technically show them — but their Terms of Use say image
binaries "must not be downloaded, stored, or re-hosted on your infrastructure
under any circumstance … they must be loaded by the end user's browser", and
each photo must link back via a plain anchor. An embedded panel can satisfy
neither. A browser-side implementation in the web console **is** compliant
(CORS verified working from the device's own page); their terms invite a
request for advanced use once a public-API implementation exists.


## Note 12 — the ~72 B/s internal-heap drain: SOLVED

`vTaskDelete(NULL)` never returns, so C++ destructors for objects living in the
task function's scope are never run. `issTask` declared
`DynamicJsonDocument doc(kIssDocBytes)` at task scope and called
`vTaskDelete(NULL)` in that same scope, leaking the document's 1024 B heap
buffer — 1088 B with allocator overhead — on **every** poll. At the 15 s ISS
cadence that is **72.5 B/s**, against a measured drain of 72.7 B/s. That was
the entire thing.

`wxTask` had the same defect, and `routeTask` had it on four separate exit
paths. `fetchTask` never did: its document lives inside `fetchAircraft()`,
which returns normally.

**How it was found — measurement, not hypothesis.**
`heap_caps_get_info()` showed only ~0.2 new allocated blocks per feeder poll
against ~200 B lost, so the leak was one ~1.1 KB object every 10–15 s, not many
small ones. Five-second sampling put the period at ~15 s, which is not the 2 s
feeder cadence. `/api/heapwalk` (built on `heap_caps_walk`, which IS available
in the prebuilt 3.3.10 libs, unlike heap tracing) then dumped the surviving
blocks' **contents**, and they read verbatim
`timestamp.iss_position.latitude.51.4031` — one per poll, each a different
latitude. No inference required.

**Why three sessions missed it.** `g_heapDeltaIss` and `g_issRuns` were
declared, exported to `/metrics`, and never incremented. "ISS contributes
exactly 0" was a hardcoded zero being read as a measurement, so the drain was
charged to the feeder — the only subsystem with a working counter. And because
the feeder runs at a fixed 2 s cadence, any time-linear drain divides into a
convincing "145 B per feeder poll". 72.7 B/s × 2 s = 145 B is the same number
twice, not evidence of causation.

**Also falsified, so they are not retried:**
- Inbound HTTP does not leak. 100 extra `/metrics` requests over an 80 s window
  cost ~4 B each against an 83 B/s baseline (control 83.2 vs load 88.7 B/s).
- The per-task lwIP thread-semaphore + pthread TSD block (~160 B/task, a
  seductive match for the observed figure) is reclaimed —
  `vPortTCBPreDeleteHook` invokes the TLS index-0 deletion callback. Confirmed
  by disassembly of the shipped libs, and implied by the fact that the 12 KB
  task stack comes back at all.
- Standalone heap tracing is unavailable: the header declares it
  unconditionally but the symbols are absent from the prebuilt libs, so it
  compiles and fails at link. Do not spend time on it.

