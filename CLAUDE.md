# CLAUDE.md — AirRadar

Real-time ADS-B air-traffic radar display on a Waveshare 7" ESP32-S3 touchscreen.
Glassmorphism UI, touch provisioning, on-device network config, fed primarily by a
local ADS-B receiver with airplanes.live as automatic cloud fallback.

Current version: **v7.1** — an LVGL 8.3 application under `firmware/AirRadar/`
(see `docs/V7_PORT.md` for architecture and `firmware/BUILD.md` for the exact
build). The root `AirRadar.ino` is the legacy v6 single-sketch app, kept as the
reference for proven data/bring-up logic. Everything below about hardware,
toolchain pins, and the non-negotiable rules applies to BOTH; v7 additionally
pins lvgl 8.3.11 + PubSubClient 2.8 and needs `firmware/lv_conf.h` copied
beside the lvgl library folder. NOTE: on esp32 core 3.3.x the FQBN no longer
has a `FlashFreq` option — `FlashMode=qio` already means QIO 80 MHz.

## Hardware (ground truth)

- **Waveshare ESP32-S3-Touch-LCD-7, Rev 1.2** — ESP32-S3-WROOM-1-N16R8
  (16 MB QIO flash, **8 MB OPI PSRAM**)
- 800×480 RGB565 parallel panel driven by LovyanGFX `Panel_RGB`/`Bus_RGB`
- **GT911 touch** on I2C (SDA=GPIO8, SCL=GPIO9, INT=GPIO4, RST via expander).
  I2C address 0x5D — pinned deterministically by our reset sequence (see rules below)
- **CH422G I/O expander** at reg addrs 0x24 (mode) / 0x38 (output).
  Output bits: b1=TP_RST, b2=DISP/backlight, b3=LCD_RST, b4=SD_CS. Normal value `0x1E`.
  Backlight is **on/off only** — no PWM dimming on this board.
- CH343P USB-UART. The onboard **UART slide switch** must be in the correct position to
  flash — "No serial data received" during upload means flip it.
- Full RGB data-pin map and panel timings live in `LGFX_Waveshare_7.h`. Treat that file
  as the ground truth; it was debugged against real hardware.

## Toolchain

- Arduino IDE 2.3.10 or `arduino-cli`; **esp32 core 3.3.10**
- Board: **ESP32S3 Dev Module** — deliberately NOT the "Waveshare ESP32-S3-Touch-LCD-7"
  board entry (its partition menu caps the app at 2 MB; we need 3 MB)
- macOS host note: CH343 driver via `brew install --cask wch-ch34x-usb-serial-driver`
  (requires Rosetta on Apple Silicon). Port appears as `/dev/cu.wchusbserial*`.

Required Tools settings:

| Setting | Value |
|---|---|
| USB CDC On Boot | Enabled |
| Flash Mode | QIO 80 MHz |
| Flash Size | 16 MB (128 Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| PSRAM | **OPI PSRAM** |
| Upload Speed | 921600 |

arduino-cli equivalents (verify option ids with `arduino-cli board details -b esp32:esp32:esp32s3` — they occasionally change between core releases):

```bash
FQBN='esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashFreq=80,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600'
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p /dev/cu.wchusbserial* .
arduino-cli monitor -p /dev/cu.wchusbserial* -c baudrate=115200
```

## Libraries (pinned — do not casually upgrade)

- **LovyanGFX 1.2.25** (`LGFX_USE_V1` API; RGB panel + Touch_GT911)
- **ArduinoJson 6.21.6** — code uses `DynamicJsonDocument`/`StaticJsonDocument`,
  which v7 deprecates. Stay on 6.x unless doing a deliberate migration.
- Everything else ships with the esp32 core: WiFi, HTTPClient, WiFiClientSecure,
  WebServer, ESPmDNS, Preferences.

## Non-negotiable rules (each one was a real debugging session)

1. **PSRAM must be OPI PSRAM.** Wrong/missing → the 800×480 background sprite fails to
   allocate → black screen or boot loop. This is the first thing to check on any
   "screen is black" report.
2. **`WiFi.setSleep(false)` after every connect.** Modem-sleep wake bursts contend with
   the RGB panel's continuous PSRAM DMA (~32 MB/s) and cause visible screen wiggle.
3. **`freq_write = 14 MHz`** in the panel config. 16 MHz produced pixel drift on this
   unit; drop to 12 MHz if drift ever returns.
4. **GT911 address is latched from the INT pin at reset.** Left floating it's a coin
   flip (0x5D/0x14) per power cycle. `ch422g_init()` performs a controlled reset —
   INT driven low while TP_RST pulses — pinning **0x5D** every boot. On a different
   board sample with dead touch, change `TOUCH_I2C_ADDR` to 0x14 in the header.
5. **`Wire.end()` after the CH422G writes.** LovyanGFX's own I2C driver owns the bus
   afterward for the GT911. Do not reintroduce `Wire` at runtime (e.g., for backlight
   tricks) without rethinking bus ownership.
6. **No direct-drawn LCD text inside the plot rectangle** (`PLOT_X/Y/W/H`,
   currently 200,22,400,408). The radar sprite is pushed every ~2 s and erases it.
   Dynamic text either goes *into* the plot sprite or lives outside the rect.
   (This was the disappearing range-pill bug.)
7. **FreeSans GFX fonts are 7-bit ASCII.** No degree symbol (0xF8) or other high
   glyphs — they render as garbage boxes.
8. **(v7) LVGL flag writes invalidate unconditionally.** `lv_obj_add_flag`/
   `clear_flag` repaint the object even when the flag already had that value.
   `setHidden()` was called every tick on the map image, so the whole 392×392
   map repainted ~4×/s (~2.5 MB/s of PSRAM traffic against a panel DMA that
   needs 25 MB/s) — that was the "wiggle", and it's why it only ever appeared
   once a map image existed. Every dynamic LVGL write must be change-cached,
   flags included.
9. **(v7) The LVGL draw buffer is 800×30 (48 KB) internal SRAM — not 60 lines.**
   96 KB starved everything else: internal heap settled ~17 KB, permanently
   below `AR_TLS_HEAP_FLOOR`, so routes/weather/logos were shed forever and the
   device eventually rebooted. It must stay in internal SRAM (PSRAM buffers
   contend with the panel DMA), just not that large.
10. **(v7) `lcd.setSwapBytes(false)` — we byte-swap in `flush_cb` ourselves.**
   With the flag on, LovyanGFX resolves `pushImage` through the `rgb565_t`
   pixelcopy specialisation, which sets `no_convert=false` and skips
   `Panel_FrameBufferBase::writeImage`'s per-row `memcpy` — every flushed pixel
   became an individual convert-and-store into PSRAM. Swapping in internal SRAM
   first keeps the bulk `memcpy`. `halReadRect()` undoes it for `/screen.bmp`.
11. **(v7) Check the TLS gate in LOOP context before spawning a task.**
   Network tasks take a 12 KB *internal* stack; spawning one only to discover
   the gate is shut burns the very RAM the gate protects (~80 no-op
   create/destroy cycles a minute — the fragmentation engine that pinned
   `heap_largest` at 10 KB). Use `tlsGateOpen()`. The gate tests free size *and*
   largest free block, because mbedTLS needs a ~16.4 KB contiguous buffer.
12. **(v7) A blip holder must bound its children.** `lv_obj_move_to()`
   invalidates only the holder's own rect — `lv_obj_move_children_by()` shifts
   children without invalidating them — so a label hanging outside leaves a
   ghost at its old position.
13. **(v7) LVGL flex overflows; it does not shrink.** A child wider than its
   parent is not compressed, it spills — and a `SIZE_CONTENT` parent can
   *under-measure* and clip a child that was itself sized correctly. When a value
   looks truncated, measure the label with `lv_obj_update_layout()` before
   blaming the font: twice now the label was fine and the parent was the bug.
   Fixed-width keys (`SET_KEY_W`) plus `LV_LABEL_LONG_DOT` beat hoping.
14. **(v7.1) A blank secret field must never overwrite the stored secret.**
   `handleWifi` wrote `server.arg("pass")` unconditionally, and the field renders
   blank *by design* so the password is not served back — so saving any unrelated
   Wi-Fi setting silently erased the key. Guard every write with `if (pw.length())`.
15. **(v7.1) The CSRF guard must compare origins exactly.** A substring/`indexOf`
   check on the Host header passes `http://airradar.local.evil.com`. Build the
   two acceptable origins (`http://` + Host, `https://` + Host) and compare whole.
16. **All chrome is composited once at boot** (`buildChrome()` → `bg` sprite): gradient,
   decorative rings, radar rings, frosted cards. Glass = per-pixel blend of the card
   over the background; **alpha 185 in `glassRect()` is the frost-opacity knob**,
   ±noise, top highlight, bottom shade. Chrome changes require editing `buildChrome()`
   and any dependent `restore()` rectangles together.

## Architecture

The sprite/`restore()` model below is **v6** (root `AirRadar.ino`). v7 replaces it
with LVGL objects + change-caching, but the *reason* for both is identical and is
the one thing to carry forward: the panel DMA owns the PSRAM bus, so the renderer
must repaint only what actually changed. See `docs/V7_PORT.md` for the v7 tree.

- **Rendering is event-driven** — no continuous refresh loop. The RGB panel's DMA
  already consumes most PSRAM bandwidth; never add per-frame animation without
  budgeting for that.
- Two PSRAM sprites: `bg` (800×480 static chrome) and `plot` (400×408 dynamic radar).
  Radar tick: copy the bg slice under the plot (`bg.pushSprite(&plot,-PLOT_X,-PLOT_Y)`),
  draw ring labels + targets, push to LCD.
- Card updates: `restore(x,y,w,h)` clips-and-repaints chrome from `bg`, then transparent
  text on top. Every drawer sets its own font *and* datum explicitly.
- **Fetch runs as a task on core 0** (`startFetch` → `fetchTask`). Results land in a
  `pending*` buffer guarded by `portMUX`; `applyPending()` merges into `tracks[]` on
  the main loop. Never touch `tracks[]` from the fetch task.
- Track lifecycle: fresh <20 s → filled glyph; 20–60 s → hollow "COAST" glyph with dead
  reckoning; >60 s → dropped. Trails record every 10 s (5 dots), stored in *screen*
  coordinates — they are cleared on range change (`clearTrails()`) because screen
  coords don't survive rescaling.
- Selection is by ICAO hex (`selHex`), survives refreshes, cleared when the track drops.
  `rebuildOrder()` maintains a distance-sorted index used by the ◀ ▶ cycle buttons.
  Selected target draws last so its label wins overlaps.
- Screens: `SCR_MAIN`, `SCR_WIFI_SCAN`, `SCR_WIFI_PASS` (3-layer QWERTY),
  `SCR_COORDS` (numpad), `SCR_MENU` (gear), `SCR_NET` (DHCP/static editor).
  Touch is edge-triggered in `loop()` and dispatched per screen.
- **Legend overlay (v7.1, `ui/ui_help.cpp`)** — the `?` left of the gear. It is a
  legend, not a manual: it explains only what the display *encodes* (glyph colour
  and size, dimming, rings, dot states), never what it already labels. Built once
  at boot and hidden; a hidden LVGL subtree is skipped entirely during redraw, so
  it costs nothing until shown. The scrim is clickable both to dismiss and to stop
  taps falling through and changing the selection underneath.

## Data sources

- **Primary — local feeder:** `feedUrl` (NVS, default
  `http://adsb.local:8080/data/aircraft.json` — set yours on first boot). readsb/tar1090
  dialect: top-level key `aircraft`, includes `r`/`t`/`desc`/`seen_pos`/`r_dst`. Polled
  every **2 s**, plain HTTP, 1.5 s connect + 4 s read timeout, auto-retry on the
  alternate `/tar1090/data/` path.
  Entries with `seen_pos > 15 s` are skipped so our coast logic owns stale positions.
- **Fallback — airplanes.live** `/v2/point/lat/lon/radiusNM` (key `ac`), polled every
  **8 s** — that interval is API courtesy; keep it. Radius derives from `rangeKm`
  (capped 250 NM). Snaps back to local automatically on the next successful poll.
- Both parse through `fetchParse()` with one shared ArduinoJson filter.
- The Overview card shows the live source: LOCAL (cyan) / CLOUD (grey) / OFFLINE.

## Network environment (example setup)

The author runs the feeder on a segmented network; adapt these to yours.

- The ESP32 typically gets a DHCP reservation so its IP is stable (or use the
  on-device static-IP screen).
- Feeder: a Raspberry Pi running the adsb.im image — port 80 is the Flask config app
  (a 404 there is *expected*), tar1090 is on **port 8080** (`/data/aircraft.json`).
- If your feeder is on a different VLAN/subnet than the display, the firewall needs a
  pass rule for **TCP → <feeder-ip>:8080**. If the display shows CLOUD instead of the
  local source, suspect that rule (or a wrong feeder URL) first.

## Enrichment sources (v7, all keyless)

- **Routes** — adsbdb.com `/v0/callsign/<CS>`, TLS, looked up lazily per selected
  aircraft, cached by hex. Callsign is sanitized to `[A-Za-z0-9]` before the URL.
- **Weather** — Open-Meteo `current=` query, TLS, ~15 min.
- **ISS** — open-notify `iss-now.json` over **plain HTTP on purpose**: one less
  ~35 KB mbedTLS handshake, and at a 15 s cadence one less competitor for the
  single-slot TLS gate. (The original "esp-tls leaks ~1.5 KB per connection"
  rationale is **retracted** — see `docs/V7_PORT.md` note 9 — but plain HTTP is
  still correct here on handshake cost alone.)
- **Base map (v7.1: full-bleed)** — CARTO `dark_all` slippy tiles, TLS, a **5×3**
  stitch → blue-tint → coverage-lens dim → **800×480** RGB565 behind the whole
  screen, not just the disc. The transient 1.9 MB mosaic is freed right after the
  resample. Persisted to FATFS `/mp/r<km>` keyed by `/mp/key` (lat/lon), so it
  only depends on {lat, lon, range} and is re-fetched **only** when one changes.
  Flash writes are chunked at 32 KB — a single large write stalls the LCD DMA.
- **Airline logos** — theqkash/esp32flight-logos (90×90 ICAO PNG), TLS, three-tier
  cache: RAM (24 slots) → FATFS `/lg/<ICAO>` (persistent) → network;
  visible-aircraft prefetch, and the prefetch pass touches `lastUse` for every
  visible airline so live entries can't become the LRU victim.
- **Route cache (v7.1)** — routes are static per callsign, so the 192-entry table
  mirrors to FATFS `/rt/tbl` (flushed every 60 s). Origin/destination survive both
  a reboot and the TLS gate closing under heap pressure. Only a *definitive*
  answer (HTTP 200 or 404) sets `routeTried`; a shed fetch must stay retryable.
- **TLS discipline (non-negotiable):** one secure connection at a time
  (`tlsTryAcquire`), aircraft feed has priority (`essential=true`), optional fetches
  are shed below a 45 KB internal-heap floor. Concurrent TLS starves mbedTLS.

## NVS schema (Preferences namespace `"radar"`)

v5/v6 keys kept for in-place upgrade: `ssid pass lat lon lbl rng feed nstat nip ngw
nmask ndns`. v7 adds: `tz` (POSIX TZ), `ppass` (panel/API password), `mqtten mqtturi`,
`nighten nightfr nightto` (quiet-hours minutes), `wxen issen logoen mapen` (layer
toggles), `fcls` (class filter bitmask), `faltlo falthi` (altitude filter),
`watch` (watchlist prefixes), `fav{0..2}{lat,lon,nam}` (favourite locations).

## Web interface & API

`http://<ip>/` (also `http://airradar.local/`). **v7.1 rebuilt this as a desktop
console**, not a phone settings form: a fixed 1240 px grid with no viewport meta
(it is only ever opened from a computer), an 8-tile live status strip off
`/metrics` at 10 s, and a traffic table off `/api/state` at 15 s — both gated on
`document.visibilityState` so a background tab stops polling the device. The
panel mirror is manual-only (1.1 MB, and it blocks the panel ~2 s). Below that
sit the Radar/Feed, Network/Wi-Fi and Integrations/Firmware **OTA** forms plus a
danger zone. Static/Wi-Fi changes reboot by design. v7 API (all behind HTTP Basic auth
`admin`/panel-password when set, with an Origin/Host CSRF guard):
`GET /api/state` · `GET|POST /api/config` · `GET /screen.bmp` (live 800×480 BMP) ·
`GET /metrics` (Prometheus incl. `heap_free/heap_min/heap_largest`) ·
`GET /api/probe?url=` (device-side LAN fetch test — settles firewall-vs-firmware) ·
`POST /update` (OTA). MQTT publishes Home-Assistant discovery sensors.

## Conventions (v7)

- Firmware is now a multi-file LVGL 8.3 app under `firmware/AirRadar/` — see
  `docs/V7_PORT.md`. The root `AirRadar.ino` is the **legacy v6** reference.
- **Threading contract (core/state.h):** loop() owns all LVGL + `g_tracks` + NVS;
  network runs as short-lived core-0 tasks writing pending buffers under `g_dataMux`.
- All geometry/timing/keys are named in `config.h` — no magic numbers in modules.
- Palette + fonts in `ui/theme.h` tokens; `altColorRGB()` = amber/cyan/violet/red.
  No greens — owner preference.
- Every dynamic LVGL write must be change-cached (unchanged writes still invalidate
  and fight the panel DMA — this was the "wiggle").
- Regenerate images with `firmware/tools/genassets.py`; `firmware/lv_conf.h` must
  sit beside the lvgl library.
- **Fonts (v7.1): six faces, not eight.** `font_hero56` / `font_clock36`
  (InterDisplay Light, **tnum frozen**), `font_id28` (Inter Medium),
  `font_val22` (Inter Medium, **tnum frozen** — every instrument value),
  `font_body18` (Inter Regular), `font_micro13` (JetBrains Mono Medium).
  Regenerate with `lv_font_conv --bpp 4`; freeze tabular figures first with
  `pyftfeatfreeze -f tnum` or digits will visibly jitter as they change.
  Codepoint ranges are NOT uniform and dropping one breaks glyphs silently:
  hero `0x30-0x39`, clock `0x30-0x3A`, id28 `0x20,0x2D,0x2E,0x30-0x39,0x41-0x5A,0xB0,0xB7`,
  micro13 `0x20-0x7E,0xB0,0xB7,0x2039,0x203A` (the last two are the range
  stepper's chevrons), val22/body18 `0x20-0x7E,0xB0,0xB7`.
  The old F_* names survive in theme.cpp as aliases onto this scale.

## Roadmap (owner-approved parked ideas)

See `docs/ROADMAP.md` for the current list. Done in v7: military `dbFlags` glyph ·
`stats.json` msg-rate · `desc` airframe name · airline logos · routes · weather ·
ISS · night mode · Home Assistant. Done in v7.1: label decluttering (spatial, not
nearest-N — the nearest targets are by definition the most clustered) · full-bleed
map · FATFS map/route caches · legend overlay · desktop web console. Still parked:

- Route ETA; session-stats screen; a served `/live` web page
- Ship the logo pack pre-loaded to FATFS at flash time (vs fetch-on-demand)
- **Aircraft photos (Planespotters) — blocked, not deferred.** Technically viable
  (LovyanGFX has `drawJpg`), but their Photo API terms forbid downloading or
  storing the image binary on your own infrastructure: it "must be loaded by the
  end user's browser". A panel fetching the JPEG itself cannot comply, and the
  required plain-anchor attribution link has nowhere to go on a 7" display.
  Rendering it **browser-side in the web console is compliant** — CORS was
  verified working from the device's own origin — so that is the only open path.
- **The ~72 B/s internal-heap drain.** Not TLS-per-connection, not TIME_WAIT, not
  feeder keep-alive — all three measured and falsified (see `docs/V7_PORT.md`
  note 9 so they are not retried). It tracks feeder run count at ~150–210 B per
  poll. Largely *neutralised* by the FATFS caches, not fixed. Next step is real
  heap tracing, not a fourth hypothesis.
