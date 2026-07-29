// config.h — AirRadar v7 global configuration
// All tunables, defaults, layout geometry and NVS keys live here.
// Rule: no magic numbers in module code — put them here and name them.
#pragma once
#include <stdint.h>

// ============================================================
//  Identity
// ============================================================
#define AR_VERSION       "7.0.0"
#define AR_USER_AGENT    "ESP32-AirRadar/7.0"
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
#define AR_POLL_ISS_MS      15000         // wheretheiss.at
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
#define SCOPE_CY 238
#define SCOPE_R  196
#define MAP_SIZE (SCOPE_R*2)              // 392x392 map image under the rings
// Cards
#define CARD_W   184
#define CARD_TALL_H 346
#define CARD_SHORT_H 66
#define CARD_L_X  14
#define CARD_R_X  (SCR_W - 14 - CARD_W)   // 602
#define CARD_TOP_Y 46
#define CARD_BOT_Y 400
#define CARD_RADIUS 17
// Weather pill (top centre) / range pill (bottom centre)
#define WX_PILL_Y   12
#define RNG_PILL_Y  446

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
// Plain HTTP on purpose: the 15s ISS poll over TLS leaked ~1.5KB/connection
// in the esp-tls layer (field-measured); open-notify needs no TLS at all.
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
