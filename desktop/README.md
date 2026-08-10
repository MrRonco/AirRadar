<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# The UI harness

Runs AirRadar's screens in a window on a Mac, so a layout change can be looked
at in seconds instead of three minutes, and so rare states can be *chosen*
rather than waited for.

It is a development tool. It is **not** part of the firmware, nothing in
`firmware/` knows it exists, and the panel never needs a computer — it boots,
joins Wi-Fi and runs standalone exactly as before.

```
make          # build
./airradar-ui # a window opens
```

Only dependency is SDL2 (`brew install sdl2`). LVGL is reused from the Arduino
library folder you already build the firmware with, so there is no second copy
to keep in sync. Override with `make LVGL=/path/to/lvgl` if yours lives
elsewhere.

## Why

On the device, looking at anything costs a compile, an OTA flash, a reboot and
a `GET /screen.bmp` — call it three minutes, and it needs the panel physically
present and free. Worse, seeing a state that is not currently happening
(an emergency squawk, −40 °C, the settings screen) meant flashing firmware with
the state hardcoded. One of those probe builds put `uiShow(SCR_SETTINGS)` in
`uiInit()` ahead of `settingsBuild()`, dereferenced a screen of null widget
pointers, and boot-looped the device — it died before `webBegin()`, so there was
no OTA route back and it had to be recovered over USB.

Here a scenario is a keypress and nothing can be bricked.

## Keys

| | |
|---|---|
| `1`–`8` | pick a scenario |
| `space` | next scenario |
| `m` / `g` / `/` | main · settings · legend |
| `s` | write `shot.bmp` |
| `q` | quit |

Mouse is touch. Clicking the range chevrons, the gear and the `?` all work.

## Headless

```
./airradar-ui --shot out.bmp                       # main screen
./airradar-ui --shot out.bmp --screen settings     # or legend, or tz
./airradar-ui --shot out.bmp --scenario 3
./airradar-ui --shot out.bmp --imperial            # the other unit system
./airradar-ui --shot out.bmp --screen settings --scroll 170
make shots                                         # every scenario -> shots/
```

`--scroll` exists because the settings columns are ~630 px of content in a
352 px viewport: without it the harness can only ever review the top third of
that screen, which is how two new rows once got added with nobody able to look
at them. `--imperial` flips the whole unit system in one go, which is the only
way to check that every unit-bearing reading still fits — "155 MI" is not the
same width as "250 KM".

No display required, so a UI change can be reviewed on a machine with no
monitor attached — or by a tool. Output is a 24-bit bottom-up BMP, the same
shape `/screen.bmp` serves, so existing tooling reads both.

## Scenarios

Each one is an edge, not a pretty picture — layout bugs live at the extremes.

| # | | exercises |
|---|---|---|
| 1 | typical traffic | the ordinary case, six aircraft, one selected |
| 2 | empty sky | zero-state, which most screens are never tested in |
| 3 | squawk 7600 | red glyph, the `7600 RADIO` alert strip, red squawk value — and the sparkline yielding its slot |
| 4 | **layout extremes** | −40 °C · NW 120 km/h · −3072 fpm · FL450 · 999 kt · a 33-character operator name · a track with every field unknown |
| 5 | crowded | a full track table (`AR_MAX_TRACKS`) against 57 actually in range — decluttering under load, and the `OF 57` disclosure |
| 6 | coasting / stale | translucent glyphs, COASTING count, CLOUD fallback dot |
| 7 | no NTP | `SYNCING` in the clock's own place, rather than a plausible-looking wrong time |
| 8 | pinned target left the ring | the card and the disc disagreeing — no blip, but a populated Selected card |

Scenario 4 is the one that earns the tool. It renders in one keypress every
value that has historically broken a layout here: the `-3072` fpm that clipped
because tabular figures make the minus sign a full digit wide, and the
`-40 °C / NW 120` weather row that needed a throwaway firmware build to see.

## What it cannot tell you

Everything about *the picture* is faithful — LVGL rasterises the same
`font_*.c` data, `lv_conf.h` is the firmware's own file used verbatim, and the
real `ui_*.cpp`, `theme.cpp`, `brandcolor.cpp`, `tracks.cpp` and `state.cpp` are
compiled in. Measurements taken here match the device.

Everything about *the panel* is absent:

- **No DMA behaviour.** Wiggle, shake, MSPI contention, repaint cost — rules 8,
  19, 20, 22 and 23 in `CLAUDE.md` are hardware findings. `/api/stalls` remains
  the only instrument for them.
- **Not the panel's colour.** sRGB on a Mac display is not the IPS. The map
  tint constants in `maptiles.cpp` were calibrated on the real thing.
- **Not cartography.** The ground is synthetic. Its *statistics* are now
  calibrated against a real z8 `dark_nolabels` mosaic — water at luma 9, land
  at 38, mean 19 — because the first version was a smooth field averaging 76
  and made the map look four times brighter than the device draws it. It still
  cannot show you a label, a coastline you would recognise, or what a style
  change does. For anything about the map *itself*, use
  `firmware/tools/tintpreview.py`, which fetches the real tiles and runs the
  real `maptiles.cpp` arithmetic over them.
- **No touch reality.** Target size in millimetres, GT911 accuracy, whether a
  thumb covers the glyph it is pressing.

Treat it as a fast, safe way to get the layout and the typography right, then
confirm anything physical on the device.

## How it is wired

```
firmware/AirRadar/src/ui/*.cpp   ─┬─▶ the firmware      (unchanged)
firmware/AirRadar/src/core/*.cpp  │
firmware/lv_conf.h                └─▶ this harness      (dev only)
```

The same sources feed both. What differs is only the platform underneath:

- `shim/` — a small `Arduino.h` (String, Serial, millis, FreeRTOS types),
  `Preferences.h` (NVS as an in-memory map), `WiFi.h` (a fixed station and a
  fixed scan list), `esp_heap_caps.h`, `esp32-hal-psram.h`. Kept deliberately
  thin: if a new firmware file needs more Arduino than this, that is worth
  noticing, because the UI is supposed to talk to LVGL and `g_*` state, not to
  the platform.
- `fakes.cpp` — stubs the plumbing (network, flash, panel) and keeps the logic.
  Tracks ordering, dead reckoning, `scopeToScreen`, `altColorRGB` and the
  filters are the real firmware code, so the harness draws what the device
  draws. The logo cache always reports a miss, so the operator tile exercises
  its ICAO-in-brand-colour fallback — the path most aircraft take anyway.
- `scenarios.cpp` — the fake world.
- `main.cpp` — SDL window, LVGL display and pointer drivers, BMP writer.

Because `Preferences` is in-memory and never loaded from a device, harness
screenshots contain no SSID, no feeder host, no device IP and a round synthetic
home location. They are publishable as-is, unlike captures off the panel, which
have to be redacted by hand.
