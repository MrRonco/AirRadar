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
    src/core/  types.h state.{h,cpp} tracks.{h,cpp} units.{h,cpp}
               timezones.h                       (curated POSIX table)
               stall.{h,cpp}                     (per-stage loop timing → /api/stalls)
    src/net/   feeder.* enrich.* maptiles.* logos.*   (core-0 task modules)
    src/ui/    theme.* ui.h ui_nav.cpp ui_scope.cpp ui_cards.cpp ui_settings.cpp
               ui_help.cpp                       (legend overlay)
               bezel.*                           (bearing scale, engraved into the map)
               spark.*                           (one-hour traffic history)
               brandcolor.*                      (airline tile colours)
    src/hal/   hal_display.*                     (panel/touch/backlight/LVGL glue)
    src/svc/   web.* mqtt.* heapwalk.*
    src/assets/ img_*.c font_*.c                 (generated: glyphs + Inter/JBM fonts)
```

~10,600 lines of C++ across 42 files, assets excluded.

## Threading model (the invariant everything hangs off)

* `loop()` (core 1) owns **all** LVGL calls, `g_tracks`, settings writes, NVS.
* Network modules spawn short-lived tasks on core 0. A task gets a **snapshot**
  of the settings it needs (captured in loop context before spawn), writes its
  result into a dedicated pending structure under `g_dataMux`, sets a volatile
  ready flag **last**, clears its in-progress flag and dies.
* `loop()` drains pending → live state → targeted UI refresh. No LVGL locks
  needed anywhere because LVGL is single-threaded by construction here.

## Rendering

* LVGL draw buffer: a single 800×30 RGB565 buffer (48 KB) in **internal SRAM**;
  flush = byte-swap in SRAM, then `lcd.pushImage` into LovyanGFX's RGB-panel
  framebuffer (the panel scans that via DMA). Both halves of that sentence were
  originally wrong and each cost a session — see findings 2 and 9 below for why
  it is not in PSRAM, and CLAUDE.md rule 9 for why it is 30 lines and not 60.
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

* Web: console page + `/api/state` + `/api/config` + `/api/select` +
  `/screen.bmp` + `/metrics` + `/update` (OTA), plus three diagnostics that
  exist because serial is awkward on this board — `/api/probe?url=` (note 7),
  `/api/stalls` (note 13) and `/api/heapwalk` (note 12). HTTP Basic auth when a
  **Web & API password** is set; the name was corrected in v7.2.5 because it
  never locked the panel.
* MQTT: Home Assistant discovery (sensors + emergency binary_sensor), LWT
  availability, 5 s state publish.

## Hardware bring-up findings (first live session — each cost real debugging)

1. **RGB path wants byte-swapped 565.** LVGL stays `LV_COLOR_16_SWAP 0`.
   Symptom of getting it wrong: `#0c1119` background renders olive (128,96,64),
   AA text gets chromatic fringes. **Superseded in v7.2:** this was originally
   done with `lcd.setSwapBytes(true)`, which silently disabled LovyanGFX's fast
   path — the flag is now `false` and the swap is done by hand in `flush_cb`.
   See CLAUDE.md rule 10.
2. **LVGL draw buffer must be internal SRAM** (a single 800×60 at the time;
   **it is 800×30 now** — 96 KB starved everything else, CLAUDE.md rule 9). PSRAM draw
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
   the drain stayed at 72.8 B/s).

   **This note ends here because that is where it ended at the time. The drain
   was SOLVED in v7.2 — see note 12.** The attribution above is the part to
   distrust: "the whole ~72 B/s is feeder run count, with iss contributing
   exactly 0" was read off two counters that were declared, published to
   `/metrics` and never once incremented. It was a hardcoded zero being read as
   a measurement, and it sent three sessions after the wrong subsystem. The
   actual culprit was `issTask`.
10. **Non-uniform font codepoint ranges.** Regenerating all faces with one
   uniform range silently dropped U+2039/203A and turned the range stepper's
   chevrons into tofu. Per-face ranges are listed in CLAUDE.md.
11. **`lv_obj_set_ext_click_area`** is how a small glyph keeps a 48 px touch
   target — used by both the gear and the "?" (26 px drawn, 48 px tappable).

## Deferred (tracked, not forgotten)

Trails on the scope · web `/live` page · aircraft type silhouettes ·
**ETA to destination** (v7.2.5 shipped time-to-*you*, which is a different
question and needed no network) · **a session-statistics view** (v7.2.5 shipped
one hour of in-range history as a sparkline; max range and peak count are still
unrecorded).

**Aircraft photos are blocked on licensing, not code.** Planespotters' photo
API is free and keyless and LovyanGFX *does* have a JPEG decoder (`drawJpg`),
so the panel could technically show them — but their Terms of Use say image
binaries "must not be downloaded, stored, or re-hosted on your infrastructure
under any circumstance … they must be loaded by the end user's browser", and
each photo must link back via a plain anchor. An embedded panel can satisfy
neither. A browser-side implementation in the web console **is** compliant
(CORS verified working from the device's own page); their terms invite a
request for advanced use once a public-API implementation exists.


## Note 14 — the whole-screen shake: every FATFS write starves the panel DMA

The symptom that mattered was one sentence from the owner: **"the entire screen
shakes."** A repaint tears locally; a whole-screen shake is the RGB panel's DMA
being starved of PSRAM bandwidth. That eliminated rendering outright, after six
hypotheses had been spent inside it.

`/api/stalls` then showed the signature immediately — **a long stall with zero
pixels flushed**, which every repaint metric is structurally blind to:

```
t=2884  logos    222 ms   0 px (0%)
t=223   rtcache  327 ms   0 px (0%)
```

Both are FATFS writes. Flash and PSRAM share the MSPI bus, so a flash operation
blocks cache traffic and the panel starves for its duration.

**It cannot be fixed properly on this platform.** `CONFIG_SPI_FLASH_AUTO_SUSPEND`
— the ESP-IDF option that suspends a flash operation so cache access continues —
is **not set** in the prebuilt arduino-esp32 3.3.10 libraries, and enabling it
needs a core rebuild. Same wall as standalone heap tracing (note 12).

**Cost is fixed overhead, not bytes.** ~150–220 ms per write regardless of size:
sector erase plus FAT metadata. This has a counter-intuitive consequence that
was learned the hard way — **chunking a small write makes it worse.** Copying
the map's chunked pattern to the 9 KB route table turned one 327 ms stall into
four of 146–158 ms. The map chunks correctly only because its write is 750 KB,
where size genuinely dominates. That commit was reverted.

**The only lever at small sizes is frequency:**

| source | before | after |
|---|---|---|
| route cache | every 60 s | every 15 min |
| logo cache | every new airline | one per 45 s, oldest-first |
| map cache | per range change | already chunked |

**Result, 7.6 hours on the fixed build:**

| | before | after |
|---|---|---|
| `logos` stalls | 4 in ~5 min | **6 in 7.6 h** |
| `rtcache` stalls | recurring | **0** |

Roughly a 350x reduction — one every ~75 s to one every ~76 min. The rate keeps
decaying because an ICAO is written once, ever.

These are **mitigations, not cures**, and the source comments say so. A flash
write will always shake this panel.

### Hypotheses falsified on the way, so they are not retried

Observer `/metrics` polling · TLS handshake load · logo fetch/decode (the *fetch*
was innocent; the *write* was not) · `lv_obj_move_foreground` invalidating the
whole disc (a real bug, fixed, 53% → 11% repaints — but not this symptom) ·
modem sleep returning after a reconnect (`wifi_reconnects` stayed 0; the rule-2
re-assertion was a real latent defect and was fixed anyway) · periodic map cache
writes (they are per range change only).

## Note 13 — the intermittent display glitch: SOLVED

`blipBuild` ended with two `lv_obj_move_foreground()` calls, keeping the ISS
marker on top after a late holder was created. That resolves to
`lv_obj_move_to_index`, whose final statement is `lv_obj_invalidate(parent)`
(lvgl 8.3 `lv_obj_tree.c:216`) — and the parent is `s_clip`, the 424x424 disc.

Every newly seen aircraft therefore repainted the whole scope, and since a new
aircraft also changes the Overview count and nearest, the left card repainted
with it. 179,776 px of disc plus ~20,000 px of card = ~200,000 px, 52% of the
screen, ~120 ms. Irregular and clustered because aircraft arrive that way.

The ISS is hidden except for a few passes a day, so nearly every one of those
repaints was reordering an invisible object.

**Result.** Steady-state LVGL stalls, boot excluded, same traffic level:

| | before | after |
|---|---|---|
| worst repaint | 203,118 px (53%) | 41,676 px (11%) |
| worst duration | 129 ms | 60 ms |
| mean repaint | 60,120 px | 26,447 px |
| events >= 11% of screen | 2 | **0** |

**How it was found.** Three hypotheses were wrong first: the observer's own
`/metrics` polling (falsified — the glitch recurred with nothing polling), TLS
handshake load (falsified — zero `enrich` stalls), and logo decode (falsified —
zero `logos` stalls). `/api/stalls` settled it in two readings: per-stage loop
timing eliminated every non-LVGL stage, then flushed-pixel counts with their
bounding box gave `13,23-616,456` — the left card's edge through the scope's
edge — and the arithmetic matched to within 2%.

Also fixed alongside: `LV_INV_BUF_SIZE` raised 32 → 64. On overflow LVGL
discards every pending area and invalidates the whole display
(`lv_refr.c:256`), which is the 384,000 px / 224 ms signature.

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

