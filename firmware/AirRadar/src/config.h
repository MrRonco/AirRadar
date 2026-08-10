// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// config.h — AirRadar v7 global configuration
// All tunables, defaults, layout geometry and NVS keys live here.
// Rule: no magic numbers in module code — put them here and name them.
#pragma once
#include <stdint.h>

// ============================================================
//  Identity
// ============================================================
#define AR_VERSION       "7.2.4"
// Contact URL included on purpose: several keyless APIs (Planespotters for
// one) reject a bare product token with 403.
#define AR_USER_AGENT    "ESP32-AirRadar/7.2 (+https://" AR_REPO_URL ")"
#define AR_REPO_URL      "github.com/MrRonco/AirRadar"
#define AR_AUTHOR_LINE   "A hobby ADS-B radar by Franco Raso"
#define AR_MDNS_NAME     "airradar"

// ============================================================
//  Defaults (all owner-specific values are set at runtime; repo ships neutral)
// ============================================================
#define AR_DEFAULT_LAT   0.0
#define AR_DEFAULT_LON   0.0
#define AR_DEFAULT_TZ    "UTC0"           // POSIX TZ, e.g. "EST5EDT,M3.2.0,M11.1.0"
#define AR_DEFAULT_FEED  "http://adsb.local:8080/data/aircraft.json"
#define AR_LOCAL_SRC_NAME_MAX 12          // display name derived from feeder host

// ============================================================
//  Timing (ms unless noted)
// ============================================================
#define AR_POLL_LOCAL_MS     2000         // own feeder: no rate limit
#define AR_POLL_CLOUD_MS     8000         // airplanes.live courtesy — keep it
#define AR_POLL_STATS_MS    15000         // feeder stats.json (msg rate)
#define AR_POLL_WEATHER_MS  (15UL*60UL*1000UL)   // Open-Meteo
#define AR_POLL_ISS_MS      15000         // open-notify
// Background route walk: one adsbdb lookup per interval for a visible aircraft
// that hasn't been tried yet. Routes used to resolve for the SELECTED aircraft
// only, so at most one flight on screen could ever show origin/dest. Keep this
// interval polite — adsbdb is a free keyless API run by one person.
#define AR_POLL_ROUTE_MS     9000
#define AR_STALE_TRACK_MS   20000         // fresh -> coasting
#define AR_DROP_TRACK_MS    60000         // coasting -> dropped
#define AR_STALE_FEED_MS    30000         // "STALE" banner threshold
#define AR_UI_TICK_MS         250         // card refresh cadence
#define AR_BLIP_GLIDE_MS      900         // target position ease duration
#define AR_MQTT_PUB_MS       5000         // state publish cadence
#define AR_SEENPOS_SKIP_S    15.0f        // local feed: skip stale positions

// ============================================================
//  Capacities
// ============================================================
#define AR_MAX_TRACKS        40
#define AR_MAX_SCAN_APS       7
#define AR_MAX_FAVS           3
#define AR_WATCHLIST_LEN     48           // comma-separated prefixes
#define AR_JSON_DOC_BYTES  65536          // aircraft.json parse budget
#define AR_NET_TASK_STACK  12288          // TLS needs headroom

// ============================================================
//  Screen geometry (800x480) — matches the browser-verified mock 1:1
// ============================================================
#define SCR_W 800
#define SCR_H 480
// Scope (radar circle)
#define SCOPE_CX 400
#define SCOPE_CY 240   // true centre
#define SCOPE_R  212   // disc 120,687 -> 141,196 px (+17%)
#define MAP_SIZE (SCOPE_R*2)              // disc diameter — the map's SCALE reference
// The base map is full-bleed: it fills the panel and is dimmed outside the
// coverage circle, so the disc reads as a lens over live receiver coverage.
// Dimming is not decoration — it is what lets the cards stay LV_OPA_COVER over
// the map instead of needing translucency (and losing LV_COVER_RES_COVER).
#define MAP_W 800
#define MAP_H 480
#define MAP_DIM_PCT   30                  // brightness outside the circle, %
#define MAP_DIM_FEATHER 10                // px of soft falloff at the edge
// Cards
#define CARD_W   168   // tangent to the disc, 8 px gutter
#define CARD_TALL_H 376
#define CARD_SHORT_H 52
#define CARD_L_X  12
#define CARD_R_X  (SCR_W - 12 - CARD_W)   // 620
// 24/412, not 28/416. The stack ran 28..468 -- optical centre 248 against the
// disc's 240, with a 28 px top margin and a 12 px bottom one. The 28 was
// reserved for the corner controls, but that is a RIGHT-EDGE reservation, not a
// full-width one. Now centre 244, margins 24/16.
#define CARD_TOP_Y 24
#define CARD_BOT_Y 412
#define CARD_RADIUS 17
// Bottom band: range stepper | clock | gear. The weather pill is gone — it now
// heads the Overview card, which vacates the vertical axis so the compass N can
// never be occluded again.
#define RNG_PILL_Y  CARD_BOT_Y
#define CLOCK_X     620   // aligns under the Selected card
// Bare glyph in the very top-right corner, clear of the Selected card at
// y=28. Drawn small so it does not crowd the card, but its touch area is
// expanded to 48 px via lv_obj_set_ext_click_area.
// Both marks are centred in a notional GEAR_S cell, and both float inside it:
// "?" sets 12 px of the 26 and the cog 16. So the 10 px gap between the CELLS
// rendered as a 24 px gap between the MARKS, and the "?" read as marooned
// rather than paired with the cog. HELP_CELL_X is therefore derived from the
// gap we actually want to see — 12 px of clear air between the two glyphs.
#define GEAR_S      26
#define GEAR_X      (SCR_W - 12 - GEAR_S)   // 762; the cog's ink lands 768..781
#define GEAR_Y      2
#define HELP_X      (GEAR_X - 24)           // 738; the "?" ink lands 745..755
#define HELP_Y      GEAR_Y
// Overlay screens (settings, legend) share one margin. Main keeps CARD_L_X 12
// because that is a TANGENCY constraint -- the cards touch the disc -- not a
// margin, and changing it would break the composition's best decision.
#define PAGE_PAD    20
// Touch is a SEPARATE pair of plates, deliberately not centred on the glyphs:
// centring a 12 px mark in a 48 px target is what forced them apart to begin
// with. The plates TILE the corner and meet at CORNER_SPLIT, the middle of the
// visual gap, so every tap up here resolves to exactly one control. The old
// symmetric ext_click_area overlapped its neighbour by 11 px — and reached
// 10 px down into the Selected card, which starts at CARD_TOP_Y.
#define CORNER_SPLIT   761
#define CORNER_LEFT    702
// Deliberately tied to CARD_TOP_Y: the plates must stop where the Selected card
// begins or taps on its corner open Settings. Centring the card stack (24/412)
// therefore costs these plates 4 px of height, 28 -> 24. Accepted: they are
// 59 and 39 px WIDE, they sit against the physical top edge so you cannot
// overshoot upward, and the glyph ink (y 7..22) stays comfortably inside.
#define CORNER_TOUCH_H CARD_TOP_Y

// ============================================================
//  Altitude ramp — ONE definition
// ============================================================
// These were duplicated: theme.h held C_ALT_* for the legend and the cards,
// while core/state.cpp's altColorRGB() -- the path that actually colours every
// glyph on the scope -- carried its own hardcoded bytes. Both comments claimed
// "cyan left the ramp"; only one of them was changed when it did, so the tokens
// and the glyphs disagreed. config.h is the one header both layers already
// include, so the ramp lives here and theme.h/state.cpp derive from it.
//
// Luminance descends 0.599 -> 0.49 -> 0.321 so low/near reads loud without
// consulting the legend. No cyan: that means "live" and nothing else.
#define AR_ALT_LOW_RGB   0xffc061   // below 10,000 ft  — amber
#define AR_ALT_MID_RGB   0xa8bfc9   // 10k–30k          — ice/steel
#define AR_ALT_HIGH_RGB  0x9b8ce0   // above 30,000 ft  — violet
#define AR_ALT_NONE_RGB  0xaab4c0   // altitude unknown — ivory2

// ============================================================
//  Range steps (km)
// ============================================================
#define AR_RANGE_STEPS {50, 100, 150, 250}
#define AR_RANGE_DEFAULT 100
#define AR_CLOUD_RADIUS_NM_CAP 250

// ============================================================
//  Data sources (all keyless)
// ============================================================
#define AR_CLOUD_API   "https://api.airplanes.live/v2/point/"        // lat/lon/radiusNM
#define AR_ROUTE_API   "https://api.adsbdb.com/v0/callsign/"         // + CALLSIGN
#define AR_WX_API      "https://api.open-meteo.com/v1/forecast"      // ?lat&lon&current=...
// Plain HTTP on purpose: open-notify needs no TLS at all, and one less secure
// fetch is one less ~35 KB mbedTLS handshake against a tight internal heap.
// NOTE: the older "esp-tls leaks ~1.5 KB per connection" rationale recorded
// here did NOT survive re-measurement on 2026-07-29 — a 90 s window in which
// the TLS connection counter never moved still lost 6.8 KB of internal heap.
// Treat that figure as unproven; the live drain tracks feeder poll count.
#define AR_ISS_API     "http://api.open-notify.org/iss-now.json"
#define AR_TLS_HEAP_FLOOR (45 * 1024)   // below this, optional TLS is shed
// Free SIZE alone is the wrong test: mbedTLS wants a ~16.4 KB CONTIGUOUS
// record buffer, so a fragmented 60 KB heap still fails the handshake while
// reporting plenty free. Field-measured: heap_largest sat at 11 KB while
// heap_free was 30 KB. Both tests must pass before an optional TLS fetch runs.
#define AR_TLS_BLOCK_FLOOR (20 * 1024)
#define AR_TILE_HOST   "basemaps.cartocdn.com"
// F1: dark_nolabels, not dark_all. CARTO's labelled tiles put place names at
// z9-z11 under a 424 px disc that is already carrying aircraft glyphs, their
// callsigns, three range numerals and a crosshair -- and the panel is read
// from across a room, where a 7 px town name is not legible, only textured.
// The scope wants a GROUND, not a map. Same tile server, same cache path.
#define AR_TILE_STYLE  "dark_nolabels"
#define AR_TILE_ATTRIB "(C) OSM - CARTO"

// ============================================================
//  NVS (Preferences) — namespace + keys.  v5/v6 keys kept for upgrades.
// ============================================================
#define AR_NVS_NS "radar"
// existing since v5/v6:
//   ssid,pass (wifi) · lat,lon (home) · lbl (labels) · rng (range km)
//   feed (feeder url) · nstat,nip,ngw,nmask,ndns (static ip)
// new in v7:
#define K_TZ        "tz"        // POSIX TZ string
#define K_PPASS     "ppass"     // panel/API password ("" = open)
#define K_MQTT_URI  "mqtturi"   // mqtt://user:pass@host:port
#define K_MQTT_EN   "mqtten"    // bool
#define K_NIGHT_EN  "nighten"   // bool
#define K_NIGHT_FROM "nightfr"  // minutes since midnight (e.g. 1380 = 23:00)
#define K_NIGHT_TO  "nightto"   // minutes since midnight (e.g. 360 = 06:00)
#define K_WX_EN     "wxen"      // weather strip on/off
// Night mode: how long a tap keeps the backlight up, and how long a newly
// seen emergency squawk does. Both are one-shots, not toggles -- the panel
// returns to dark on its own.
// Unit conversions used in more than one translation unit. ONE definition:
// the altitude ramp was duplicated across two files with the same wrong
// comment on both copies, and that is how it stayed wrong (C2).
// A knot is one nautical mile per hour, so both readings share the number.
#define AR_KM_PER_NM   1.852f     // also knots -> km/h
#define AR_DEG2RAD     0.01745329f

#define AR_NIGHT_WAKE_MS   (30 * 1000)
#define AR_NIGHT_ALERT_MS  (60 * 1000)

#define K_UNITS     "units"     // 0 = metric, 1 = imperial. DISPLAY ONLY --
                                //  g_wx.tempC, g_set.rangeKm, /api/state and
                                //  MQTT all stay metric. See core/units.h.
#define K_TEMP_F    "tempf"     // v7.2.3 and earlier. Read once to migrate a
                                //  Fahrenheit user onto K_UNITS, then ignored.
#define K_CLOCK24   "clk24"     // true = 24-hour clock, false = 12-hour + AM/PM
#define K_ISS_EN    "issen"     // ISS layer on/off
#define K_LOGO_EN   "logoen"    // operator monogram/logo tile on/off
#define K_MAP_EN    "mapen"     // map base layer on/off
#define K_FILT_CLS  "fcls"      // bitmask: 1=airliner 2=light 4=heli 8=mil 16=other
#define K_FILT_ALT_LO "faltlo"  // ft, 0 = off
#define K_FILT_ALT_HI "falthi"  // ft, 0 = off
#define K_WATCH     "watch"     // comma-separated reg/callsign prefixes
#define K_FAV_BASE  "fav"       // fav0lat fav0lon fav0name ... fav2*
#define AR_FILT_CLS_ALL 0x1F

// ============================================================
//  Behaviour switches
// ============================================================
#define AR_USE_INTER_FONTS 1    // 0 = fall back to built-in Montserrat everywhere
