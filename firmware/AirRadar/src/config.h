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
#define AR_VERSION       "7.1.0"
// Contact URL included on purpose: several keyless APIs (Planespotters for
// one) reject a bare product token with 403.
#define AR_USER_AGENT    "ESP32-AirRadar/7.1 (+https://" AR_REPO_URL ")"
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
#define CARD_TOP_Y 28
#define CARD_BOT_Y 416
#define CARD_RADIUS 17
// Bottom band: range stepper | clock | gear. The weather pill is gone — it now
// heads the Overview card, which vacates the vertical axis so the compass N can
// never be occluded again.
#define RNG_PILL_Y  CARD_BOT_Y
#define CLOCK_X     620   // aligns under the Selected card
// Bare glyph in the very top-right corner, clear of the Selected card at
// y=28. Drawn small so it does not crowd the card, but its touch area is
// expanded to 48 px via lv_obj_set_ext_click_area.
#define GEAR_S      26
#define GEAR_X      (SCR_W - 12 - GEAR_S)
#define GEAR_Y      2
#define GEAR_TOUCH_PAD 11    // 26 + 2*11 = 48 px effective target
#define HELP_X      (GEAR_X - GEAR_S - 10)
#define HELP_Y      GEAR_Y

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
#define AR_TILE_HOST   "basemaps.cartocdn.com"                       // CARTO dark_all
#define AR_TILE_STYLE  "dark_all"
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
