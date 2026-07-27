# AirRadar v6 — redesign plan

Approved design mockup: an interactive render of the target layout was reviewed and
locked before implementation. This document is the build plan the code follows.

## Goals (owner-approved)

1. **Settings off the main screen.** Remove the on-radar Settings card (coords + labels
   toggle). Those controls already live on the gear/settings screen (`SCR_MENU`), which
   keeps every existing function: Change Wi-Fi, Network, Edit Coordinates, Labels,
   Reboot, and the browser-config URL line.
2. **Time moves left.** The clock takes the freed bottom-left slot.
3. **Symmetry + reach.** Selected Aircraft drops to align with Overview; the corner cog
   becomes a full-width SETTINGS button matching the card width. Both side columns are
   mirrored stacks (tall card over short card) and run from near the top edge to the
   floor — the old top brand strip is removed to buy that height.
4. **Satellite base map.** A real aerial still under the scope, fetched keyless from
   Esri "World Imagery" for the entered coordinates, rescaled with the range pill,
   desaturated/slate-tinted for readability, with the required attribution credit.
5. **Fuller Overview.** HOME location in the header (drawn home icon); in-range/heard,
   conditional emergency line, nearest target + bearing, feed rate, altitude colour key,
   source + age.
6. **Richer Selected Aircraft card** — three tiers (all approved):
   - **A (free):** tail number, live/coast status + age, selected altitude, always-on squawk.
   - **B (DB-dependent):** airframe name (`desc`), operator (`ownOp`), year (`year`),
     military/interesting flag (`dbFlags`). Present on the cloud feed and on local feeds
     whose tar1090 has its aircraft DB loaded; missing fields simply don't draw.
   - **C (one keyless lookup):** route origin→dest from adsbdb.com, cached by ICAO hex,
     fired lazily for the selected aircraft only, debounced against ◀ ▶ cycling.
7. **Emergency both ways.** A red line in the Overview *and* the offending target flashes
   red on the scope (toggled on the existing 2 s radar tick — no per-frame cost).
8. **About/credit.** Repo link + a short author blurb on the settings screen, plus a
   version bump to v6.0.

## Decisions locked
- Wordmark: **dropped** from the main screen (still on the boot splash).
- Satellite tint: **slate-tinted** (desaturate + cool + vignette), as rendered.
- Altitude bands: **<10k / 10–30k / >30k ft** (matches `altRGB()`).

## Non-negotiables carried forward (from CLAUDE.md)
- PSRAM = OPI; `WiFi.setSleep(false)`; `freq_write = 14 MHz`; GT911 0x5D reset;
  `Wire.end()` after CH422G; no direct LCD text inside the plot rect; FreeSans is
  7-bit ASCII (home/gear/altitude-key icons are **drawn**, not glyphs); all chrome
  composited once in `buildChrome()`.
- **No new libraries.** JPEG decode uses LovyanGFX's built-in `drawJpg`.
- Rendering stays event-driven; the satellite is a one-shot decode on boot + range
  change (respects the ~32 MB/s PSRAM DMA budget).

## Layout constants (device px, 800×480)
| Element | X | Y | W | H |
|---|---|---|---|---|
| Overview card | 12 | 14 | 168 | 360 |
| Time card | 12 | 382 | 168 | 88 |
| Selected card | 620 | 14 | 168 | 360 |
| Settings button | 620 | 382 | 168 | 88 |
| Plot / scope | 200 | 22 | 400 | 408 (RCX 400, RCY 226, R 195) |
| Range pill | 348 | 446 | 104 | 24 |
| Cycle arrows | 216 / 584 | 388 | r 22 | (inside scope) |

Removed: Settings card (`ST_*`), corner cog (`GEAR_*`), top-left brand.

## Phased delivery (each phase compiles + flashes)
- **P1 Layout reshuffle** — constants, `buildChrome()`, drawers repositioned; cog→button
  touch; HOME header; altitude-key chrome. Pure geometry milestone.
- **P2 Overview data** — in-range/heard, nearest+bearing, emergency line, source/age.
- **P3 Selected A+B** — `Track` fields + filter + parser for desc/ownOp/year/dbFlags/
  nav_altitude_mcp; re-rendered card.
- **P4 Emergency flash** — toggle target colour on the radar tick.
- **P5 Satellite** — Esri fetch task → PSRAM buffer → `drawJpg` into the `bg` plot
  region → tint + rings + credit; re-fetch on boot + range change.
- **P6 Route (Tier C)** — adsbdb lookup on selection, hex-keyed cache.
- **P7 Feed rate** — `stats.json` poll (local source), Overview row.

## Verification (owner flashes; no host toolchain)
Flash each phase to the Waveshare panel; confirm: no black screen (PSRAM), no screen
wiggle (`setSleep(false)`), touch zones align with the new layout, satellite frames the
right ground and rescales with range, DB-dependent fields blank gracefully, route
appears on selection, emergency line + blip flash together.
