# Version history

## v1 — port (from Mirko Pavleski's CrowPanel project)
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
