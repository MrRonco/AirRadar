# Version history

## v1 — first light (inspired by Mirko Pavleski's CrowPanel ADS-B project)
Ported to the Waveshare panel: new RGB pin map, CH422G expander init, board settings
(16 MB / OPI PSRAM), UART-switch flashing fix. Worked, but the whole screen wiggled.

## v2 — ATC scope rewrite
Root-caused the wiggle: 40 fps sprite pushes + RGB DMA = PSRAM bandwidth contention,
plus WiFi modem-sleep bursts. Moved to event-driven rendering (persistent targets,
dead reckoning between polls, trails, coast/drop lifecycle) and `WiFi.setSleep(false)`;
`freq_write` 16→14 MHz. Rock solid. Green phosphor aesthetic, big 12h clock.

## v3 — touch edition
GT911 enabled (with controlled reset pinning I2C 0x5D — address otherwise randomizes
per power cycle). WiFi provisioning with scan list + 3-layer keyboard, NVS persistence,
tap/arrow aircraft selection with detail panel, coordinate numpad, gear menu, web
config page + mDNS. Deliberately no OpenSky credential screens (airplanes.live is
keyless; OpenSky OAuth2 + credit limits would be strictly worse).

## v4 — glass edition
Tech Talkies card layout, slate/cyan glassmorphism (per-pixel frost compositing into a
full-screen chrome sprite, built once at boot), FreeSans typography, amber/cyan/violet
altitude ramp. v4.1 added the local feeder: local-feeder-first polling at 2 s with
airplanes.live fallback at 8 s, `seen_pos` staleness guard, live source tag.

## v5
Symmetric card layout; age + source on separate lines; tappable range pill
(50/100/150/250 km, dynamic ring labels); local-time-only clock; NETWORK screen
(DHCP/static IP/GW/mask/DNS on-device, reboot-to-apply); bottom bar removed; web page
gained Wi-Fi and Network sections; stacked SELECTED/AIRCRAFT card title; centered
headers; numpad recentered; `<` backspace.

## v6 — layout redesign (immediate-mode, root AirRadar.ino)
Settings card removed from the radar; both columns mirrored top-to-floor; time moved
bottom-left, cog → full-width SETTINGS button; HOME in the Overview header; airplane
glyphs replacing arrows; emergency flash; the first satellite/CARTO base experiments.
Kept as the reference for proven data + hardware logic once v7 took over the UI.

## v7 — current: the LVGL rewrite
Full port to an LVGL 8.3 application (`firmware/AirRadar/`) chasing "past esp32flight":
anti-aliased Inter/JetBrains-Mono type, CARTO dark base map with city labels, animated
altitude-coloured aircraft glyphs, operator monogram + real airline logos (FATFS-cached),
adsbdb routes, Open-Meteo weather, ISS overhead, night mode, filters/favourites/watchlist,
Home-Assistant MQTT discovery, JSON API + `/screen.bmp` + `/metrics` + `/api/probe` + OTA.
Data core (feeder-first, dead-reckon, coast/drop) ported intact from v6.

First live hardware session settled a chain of physics: RGB panel wants byte-swapped 565;
LVGL draw buffer in internal SRAM but its heap in PSRAM; one TLS connection at a time or
mbedTLS starves; change-cache every widget write to stop redraw-vs-DMA wiggle; ISS on
plain HTTP to dodge an esp-tls leak. See `docs/V7_PORT.md` for the full findings, and
`firmware/BUILD.md` to build/flash. Installs one-click via `flasher/` (ESP Web Tools).

### v7.0.1 — the memory-starvation round
A second live session found the wiggle had returned *and* the device was silently
rebooting itself. Metrics forensics (plus a controlled quiet-vs-busy experiment, because
polling `/metrics` perturbs the very heap it reports) showed internal SRAM falling ~66 B/s
from boot, hitting a fragmentation cliff — 30 KB free but only a 10 KB largest block —
and latching at ~17 KB, permanently below the 45 KB TLS floor. Everything optional was
shed forever: no routes, no weather, no new logos. Airline logos still appeared, which is
exactly why the symptom looked like a data bug rather than a RAM bug — they load from the
FATFS cache with no TLS.

Four root causes, all distinct: (1) `setHidden()` wasn't change-cached and LVGL flag
writes invalidate unconditionally, repainting the whole map 4×/s — the wiggle;
(2) the 96 KB internal draw buffer was starving mbedTLS, and below the floor every
optional fetch still spawned a 12 KB-stack task just to find the gate shut, which was
itself the fragmentation engine; (3) `setSwapBytes(true)` silently disabled LovyanGFX's
per-row `memcpy`, making every flushed pixel an individual store into the PSRAM
framebuffer; (4) carrier names came only from the feeder's FAA/Transport-Canada database,
so foreign registrations had none — while adsbdb was returning `airline.name` in a
response we were already fetching and filtering out.

Result on hardware: free internal heap 17 KB → 159 KB, largest block 10 KB → 65 KB,
minimum-ever 124 B → 51 KB, weather and routes alive again. Routes also now walk the
visible list instead of resolving for the selected aircraft only.

### v7.1 — the design pass
Acted on `UI_UX_REVIEW.md`, which scored the panel 3.9/10 against the owner's
brief. Headline finding: the radar was not the hero of its own display — the
SETTINGS button measured 7.26× denser and 14.7× brighter than the entire scope,
and chrome outweighed the disc 1.26 : 1 by area. It is now 0.82 : 1.

Geometry and type. Disc 196 → 212 px (+17% area); cards 184 → 168 px, tangent
with an 8 px gutter so they no longer overlap the circle and can stay opaque;
weather moved off its floating pill into the Overview card, which vacated the
vertical axis and made the compass-ghost bug structurally impossible; clock,
range stepper and a bare 26 px gear share one bottom band. Eight type faces
became six, values went 20 → 22 px to clear the 16-arcmin ISO 9241-303 floor at
650 mm, and tabular figures were frozen into Inter with `pyftfeatfreeze` so live
numbers stop shimmying as digits change.

Measured, not stylistic. Card shadows bought 1.009:1 of contrast for a 3,362 B
uncached buffer plus two blur passes per card per repaint — deleted. `OPA_CARD
216` forced `LV_COVER_RES_NOT_COVER`, recompositing the screen root under every
1 Hz label, for a 1.057:1 difference — cards are opaque. Dropping `clip_corner`
from the scope removed the same penalty again.

Safety. `altColorRGB`'s unknown-altitude branch returned `0xff6472` — byte
identical to `C_RED`, so an aircraft with no altitude was indistinguishable from
a 7700 squawk. Unknown is now ivory; red belongs to emergency alone, and the
ramp is luminance-ordered so it reads without a legend.

Caching, on the owner's suggestion. The stitched map only depends on
{lat, lon, range}, yet was re-fetched from CARTO every boot — 15 TLS handshakes
on the one subsystem that fails under heap pressure. Maps, logos and routes now
all persist to FATFS. The map is on screen 5 s after boot, before Wi-Fi
associates.

Web console. Rebuilt from a 452 px mobile settings form into a desktop console
with a live status strip and traffic table. Two defects found during the review
and fixed: `handleWifi` wrote an empty password over the stored one because the
field renders blank by design, and the CSRF guard was a substring match that
`http://airradar.local.evil.com` would pass.
