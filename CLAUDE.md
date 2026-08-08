# CLAUDE.md — engineering contract

Working notes for modifying AirRadar. **This file is deliberately not an
introduction to the project** — [`README.md`](README.md) covers what it is, the
hardware, installation, data sources and the HTTP API, and duplicating any of
that here would only create two versions to keep in sync.

What lives here is the part you cannot infer from the code: the rules that cost
a real debugging session to learn, the threading contract, and the conventions
that keep the renderer from fighting the panel DMA.

| Looking for | Go to |
|---|---|
| What the project is, install, API reference | [`README.md`](README.md) |
| Exact build, FQBN, `lv_conf.h` placement | [`firmware/BUILD.md`](firmware/BUILD.md) |
| Pin map, panel timings, GT911 reset sequence | [`docs/HARDWARE.md`](docs/HARDWARE.md) |
| v7 module tree and the port's findings | [`docs/V7_PORT.md`](docs/V7_PORT.md) |
| Version-by-version narrative | [`docs/HISTORY.md`](docs/HISTORY.md) |
| What is shipped, parked and blocked | [`docs/ROADMAP.md`](docs/ROADMAP.md) |

Current version **v7.2.0-beta.1**. The firmware is an LVGL 8.3 application under
`firmware/AirRadar/`; the root `AirRadar.ino` is the **legacy v6** single-sketch
app, kept only as the reference for proven data and bring-up logic. The rules
below apply to both.

Three things about the toolchain that bite and are easy to miss: the esp32 core
is pinned at **3.3.10**, `firmware/lv_conf.h` must be copied *beside* the lvgl
library folder or the build fails with missing symbols, and on core 3.3.x the
FQBN no longer takes `FlashFreq` — `FlashMode=qio` already means QIO 80 MHz.

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
17. **(v7.1) `vTaskDelete(NULL)` skips every C++ destructor in that scope.**
   It never returns, so a `DynamicJsonDocument` (or any RAII object) declared in
   a task function leaks its heap buffer on every single run. This was the
   entire "mysterious ~72 B/s drain": `issTask` leaked `kIssDocBytes` once per
   15 s poll — 1088 B with allocator overhead, **72.5 B/s**, against a measured
   72.7 B/s. `wxTask` and `routeTask` had it too (routeTask on four separate
   exit paths). Found with `heap_caps_walk()`, whose surviving blocks literally
   read `iss_position.latitude.51.4031`. Either scope the object in a nested
   block, or put the body in a helper function and call `vTaskDelete` in the
   wrapper. **`fetchTask` was never affected** — its document lives inside
   `fetchAircraft()`, which returns normally.
18. **Instrumentation that is never incremented reads as a measurement.**
   `g_heapDeltaIss`/`g_issRuns` were declared, published to `/metrics` and never
   written, so "ISS contributes exactly 0" was a hardcoded zero. The drain was
   blamed on the feeder for three sessions because the feeder owned the only
   working counter. Also note a fixed-cadence subsystem makes any time-linear
   drain *look* per-poll: 72.7 B/s × 2 s = 145 B "per feeder poll" is the same
   number, not evidence.
19. **(v7.2) `lv_obj_move_foreground()` invalidates the WHOLE PARENT.**
   It calls `lv_obj_move_to_index`, whose last line is
   `lv_obj_invalidate(parent)` (lvgl 8.3 `lv_obj_tree.c:216`). `blipBuild` did
   this twice per new aircraft to keep the ISS marker on top, and the parent is
   `s_clip` — the 424x424 disc. Every newly seen aircraft therefore repainted
   the whole scope plus, via the count/nearest change, the Overview card:
   ~200,000 px, 52% of the screen, ~120 ms. That was the intermittent glitch
   that survived three wrong hypotheses. Worse, the ISS is hidden except for a
   few passes a day, so almost every one of those repaints reordered an
   invisible object. Defer z-order changes out of per-item creation, and only
   perform them when the object is actually visible.
20. **(v7.2) `LV_INV_BUF_SIZE` overflow repaints the entire screen.** Exceed it
   in one refresh period and LVGL discards every pending area and invalidates
   the whole display (`lv_refr.c:256`). The default is 32; nine blips plus ~20
   card labels on a 250 ms tick sits near that. Raised to 64 in `lv_conf.h`.
   Symptom is a 384,000 px / ~230 ms flush with bbox `0,0-799,479`.
21. **Instrument before hypothesising.** Three confident explanations for the
   glitch were wrong — observer polling, TLS handshake load, logo decode — and
   each cost a round trip. `/api/stalls` (per-stage loop timing plus flushed
   pixels and their bounding box) identified it in two readings. The bbox
   `13,23-616,456` named the two regions outright, and the arithmetic then
   matched to within 2%. Same lesson as rule 18.
22. **(v7.2) EVERY FATFS write starves the panel DMA — the whole screen shakes.**
   Flash and PSRAM share the MSPI bus, and `CONFIG_SPI_FLASH_AUTO_SUSPEND` is
   **not set** in the prebuilt arduino-esp32 3.3.10 libraries (verified in the
   shipped sdkconfig), so a flash operation cannot be suspended to let cache
   traffic through. Measured: **~150–220 ms of blocked bus per write, almost
   independent of size** — it is sector erase plus FAT metadata, not bytes.
   A 2,592-byte logo cost 222 ms; a 9 KB route table cost 327 ms.
   Consequences: **chunking a small write makes it worse** (five 2 KB writes
   cost more than one 9 KB write — this was tried and reverted; the map chunks
   only because its write is 750 KB, where size genuinely dominates), and the
   only lever at small sizes is **frequency**. Route cache is now 15 min, logo
   saves one per 45 s. Both are mitigation, not cure.
23. **Shake vs flicker is the triage.** A *whole-screen shake* is the RGB panel's
   DMA starved of PSRAM bandwidth — look for MSPI contention: flash writes, or
   rule 2's modem sleep. A *local flicker or tear* is an oversized LVGL repaint —
   look at invalidation. Six hypotheses were spent chasing repaints before the
   owner said "the entire screen shakes", which eliminated rendering outright in
   one sentence. The measurable signature of the shake is a **long stall with
   ZERO pixels flushed** (`/api/stalls`), which every repaint metric is
   structurally blind to.
16. **All chrome is composited once at boot** (`buildChrome()` → `bg` sprite): gradient,
   decorative rings, radar rings, frosted cards. Glass = per-pixel blend of the card
   over the background; **alpha 185 in `glassRect()` is the frost-opacity knob**,
   ±noise, top highlight, bottom shade. Chrome changes require editing `buildChrome()`
   and any dependent `restore()` rectangles together.

## Threading contract

The single rule that makes the rest safe, defined in `core/state.h`:

```
core 1  loop()  ──▶ LVGL · touch · g_tracks · NVS · MQTT
                     ▲
                     │ pending buffers, guarded by g_dataMux
                     │
core 0  tasks   ──▶ feeder · routes · weather · ISS · map tiles · logos
```

- **loop() owns all LVGL, all of `g_tracks`, and all NVS access.** No exceptions.
- Network runs as **short-lived tasks on core 0**. A task never reads `g_set`
  (String/double torn-read risk) — everything it needs is snapshotted into a job
  struct in loop context before the spawn.
- Tasks write only into `g_pending*` / `g_wx` / `g_iss` / `g_routeRes*` under
  `g_dataMux`, and always set the *ready flag last*.
- Each task has its own in-progress volatile flag so only one of its kind exists
  at a time.
- Tasks take a **12 KB internal stack**. Check `tlsGateOpen()` in loop context
  *before* spawning — see rule 11.
- A task ends by clearing its flag and calling `vTaskDelete(NULL)`, which
  **never returns** — see rule 17, which is the most expensive lesson in this
  file.

## Architecture that matters when modifying

- **Rendering is event-driven.** There is no refresh loop. The RGB panel's DMA
  already consumes most PSRAM bandwidth, so never add per-frame animation
  without budgeting against it, and change-cache every dynamic write.
- **Track lifecycle:** fresh <20 s → solid glyph; 20–60 s → translucent, coasting
  on dead reckoning; >60 s → dropped.
- **Selection is by ICAO hex** (`g_selHex`), so it survives refreshes and clears
  only when the track drops. `tracksRebuildOrder()` maintains the distance-sorted
  index. The selected target draws last so its label wins overlaps.
- **Legend overlay** (`ui/ui_help.cpp`) — the `?` beside the gear. A legend, not
  a manual: it explains only what the display *encodes* (glyph colour and size,
  dimming, rings, dot states), never what it already labels. Built once at boot
  and hidden, so it costs nothing until shown. Its scrim is clickable both to
  dismiss and to stop taps falling through to the scope underneath.
- The v6 sprite/`restore()` model in the root sketch is superseded, but its
  *reason* carries forward unchanged: the panel DMA owns the PSRAM bus, so the
  renderer must repaint only what actually changed.

## Enrichment implementation

README lists the sources; these are the details that matter in the code.

- **Feeder** — readsb/tar1090 dialect, top-level `aircraft` key. Auto-retries on
  the alternate `/tar1090/data/` ↔ `/data/` path. Entries with `seen_pos > 15 s`
  are skipped so *our* coast logic owns stale positions rather than the feeder's.
  Local and cloud share one `fetchParse()` and one ArduinoJson filter.
- **Routes** — callsign is sanitized to `[A-Za-z0-9]` before it reaches the URL;
  it comes straight off the ADS-B wire and would otherwise be injectable. Only a
  *definitive* answer (HTTP 200 or 404) sets `routeTried` — a shed fetch must
  stay retryable, or one hiccup blanks a route for the rest of the boot.
- **ISS** — plain HTTP on purpose: one less ~35 KB mbedTLS handshake, and one
  less competitor for the single-slot TLS gate at a 15 s cadence. (The original
  "esp-tls leaks ~1.5 KB per connection" rationale is **retracted** — see
  `docs/V7_PORT.md` note 9.)
- **Base map** — 5×3 CARTO stitch → blue-tint → coverage-lens dim → 800×480
  RGB565. The transient 1.9 MB mosaic is freed immediately after resampling.
  Cached to FATFS `/mp/r<km>` keyed by `/mp/key`, so it depends only on
  {lat, lon, range}. **Flash writes are chunked at 32 KB** — one large write
  stalls the LCD DMA.
- **Logos** — three tiers: RAM (24 slots) → FATFS `/lg/<ICAO>` → network. The
  prefetch pass touches `lastUse` for every *visible* airline, otherwise live
  entries become the LRU victim and a evicted `LOGO_MISS` causes repeat 404s
  over TLS.
- **Route cache** — 192 entries mirrored to FATFS `/rt/tbl`, flushed every 60 s.
- **TLS discipline (non-negotiable):** one secure connection at a time via
  `tlsTryAcquire()`; the aircraft feed has priority (`essential=true`); optional
  fetches are shed below the internal-heap floor. Concurrent TLS starves mbedTLS.

## NVS schema (Preferences namespace `"radar"`)

v5/v6 keys kept for in-place upgrade: `ssid pass lat lon lbl rng feed nstat nip
ngw nmask ndns`. v7 adds `tz` (POSIX TZ), `ppass` (panel/API password),
`mqtten mqtturi`, `nighten nightfr nightto` (quiet-hours minutes),
`wxen issen logoen mapen` (layer toggles), `tempf` (Fahrenheit — display only;
`g_wx.tempC`, `/api/state`'s `temp_c` and MQTT stay Celsius),
`fcls` (class filter bitmask),
`faltlo falthi` (altitude filter), `watch` (watchlist prefixes) and
`fav{0..2}{lat,lon,nam}` (favourite locations).

## Conventions

- All geometry, timings and NVS keys are named in `config.h` — no magic numbers
  in modules.
- Palette and fonts are tokens in `ui/theme.h`. `altColorRGB()` is
  amber/cyan/violet/red. **No greens** — owner preference.
- Every dynamic LVGL write must be change-cached. Unchanged writes still
  invalidate and fight the panel DMA; this was the "wiggle" (rule 8).
- Regenerate images with `firmware/tools/genassets.py`.
- **Fonts: six faces.** `font_hero56` / `font_clock36` (InterDisplay Light,
  **tnum frozen**), `font_id28` (Inter Medium), `font_val22` (Inter Medium,
  **tnum frozen** — every instrument value), `font_body18` (Inter Regular),
  `font_micro13` (JetBrains Mono Medium). Regenerate with
  `lv_font_conv --bpp 4`, and freeze tabular figures first with
  `pyftfeatfreeze -f tnum` or digits visibly jitter as they change.
  **Codepoint ranges are not uniform and dropping one breaks glyphs silently:**
  hero `0x30-0x39`, clock `0x30-0x3A`,
  id28 `0x20,0x2D,0x2E,0x30-0x39,0x41-0x5A,0xB0,0xB7`,
  micro13 `0x20-0x7E,0xB0,0xB7,0x2039,0x203A` (the last two are the range
  stepper's chevrons), val22/body18 `0x20-0x7E,0xB0,0xB7`.
  The old `F_*` names survive in `theme.cpp` as aliases onto this scale.
- Generated `font_*.c` files are **OFL-1.1, not GPL** — see
  [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## Network environment (troubleshooting)

The feeder typically runs on a separate host; adapt to your own setup.

- Give the ESP32 a stable address — a DHCP reservation, or the on-device
  static-IP screen.
- On an adsb.im Raspberry Pi, port 80 is the Flask config app (a 404 there is
  *expected*) and tar1090 is on **port 8080** at `/data/aircraft.json`.
- If the feeder is on a different VLAN/subnet than the display, the firewall
  needs a pass rule for **TCP → &lt;feeder-ip&gt;:8080**. If the panel shows CLOUD
  instead of your local source, suspect that rule or a wrong feeder URL first —
  `GET /api/probe?url=` runs the fetch from the device and settles it.
