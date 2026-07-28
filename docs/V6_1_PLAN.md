# AirRadar v6.1 — integrations & polish plan

Builds on `feat/v6-redesign`. Design approved via mockup before implementation.
Prompted by comparison with the `theqkash/esp32flight` project; this pass closes the
felt polish gap with **content, not a framework rewrite** — the single Arduino sketch
stays.

## Guiding constraints (owner)
- **Aircraft data stays on the local feeder.** No external aircraft-data APIs
  (FlightAware AeroAPI declined — adsbdb already covers routes, keyless).
- **No paid keys / accounts.** Only keyless enrichment lookups: adsbdb (routes),
  Open-Meteo (weather), NOAA (METAR), wheretheiss.at (ISS), CARTO/OSM (map tiles).
- **ntfy declined.** MQTT **is** in scope (owner runs a Mosquitto broker).
- openAIP airspace, AIS ships, SondeHub balloons: **out of scope** (parked).
- All CLAUDE.md non-negotiables hold (PSRAM OPI, setSleep(false), freq_write 14MHz,
  ASCII-only fonts → icons are drawn, event-driven rendering, one-shot map decode).

## Scope, in build order

### 1. Plane glyph  *(display · low effort · done first)*
Replace the target arrow in `drawTarget()` with a top-down airplane silhouette drawn
from rotated `fillTriangle`s (reuses `rotPt()`). Filled + altitude-coloured when live,
wireframe when coasting, ringed when selected, red-flash on emergency, boxed when
military. Keeps the speed leader line and trails.

### 2. CARTO dark base map  *(display · replaces v6 Esri satellite)*
Swap the satellite base for the **CARTO `dark_all` basemap** (keyless © OSM/CARTO
tiles) — better looking, and **city labels are baked in**. On device: compute the
z/x/y tile grid (~3×3) for `homeLat/homeLon` + `rangeKm`, fetch each keyless PNG,
decode with LovyanGFX `drawPng` into the plot region, stitch, then apply a blue tint
+ vignette and re-lay the rings. One-shot on boot / range change / location change
(respects the DMA budget). This supersedes `fetchSatellite()`/`drawSatelliteBase()`.
Tint is a tunable constant (currently a blue luminance ramp matching the device).

### 3. Icon weather strip  *(display)*
Top-centre strip in the space the wordmark vacated: drawn condition icon
(sun/cloud/rain/snow) + temperature + wind (cardinal + speed). Data from Open-Meteo
`current` (keyless), polled ~15 min on the core-0 task. The ° is a drawn ring
(ASCII font). Optional: nearest-airport METAR line (NOAA aviationweather, keyless).

### 4. /screen.bmp  *(web · low effort)*
Dump the `bg`/`lcd` framebuffer as a BMP over the existing WebServer. Instantly a
Home Assistant generic-camera source and a live panel mirror on the web page.

### 5. REST /api/state + /api/config  *(web · backbone)*
Serialize `tracks[]` + overview state to JSON on the WebServer. `GET /api/state`
(flights, nearest, counts, feed rate, source, weather), `GET/POST /api/config`.
Backbone for both the web Live view and Home Assistant / n8n. Behind the existing
optional panel password.

### 6. Web Live view  *(web)*
A `/live` page served by the device in the slate/cyan look: header (location +
weather + METAR), stat tiles, flight table (flag / airline+logo / type / route /
alt / spd / dist), and a `/screen.bmp` mirror. Fetches `/api/state`; may load a
Leaflet tile layer from CDN (browser side, not CSP-restricted).

### 7. MQTT + Home Assistant discovery  *(integration)*
Publish auto-discovered HA sensors (nearest aircraft, in-range count, feed rate,
source) to the Mosquitto broker. Config: broker URI in settings. Outbound webhook
(n8n) on emergency/watchlist is a small add alongside.

### 8. Airline logos  *(display · FATFS)*
Bundle a logo pack (RGB565 or PNG) into FATFS, key by `ownOp`, blit on the Selected
card next to the callsign. Fall back to operator text when absent. (Open pack from
esp32flight is MIT — reusable with credit.)

### 9. ISS on the radar  *(display)*
Poll wheretheiss.at (keyless); when the ground track crosses the range, draw the ISS
as a distinct object with a dotted track and above-horizon indicator.

### 10. New settings  *(on-device + web)*
Favourite location slots (3), night mode (backlight off during quiet hours — fits the
no-PWM backlight), aircraft-class filter, altitude from/to filter, watchlist
highlight, plus toggles for ISS / weather / logos / MQTT. Grouped two-column device
screen; mirrored on the web Settings tab.

## Deferred to v6.2+
Aircraft photos (planespotters via adsbdb, needs a tap-to-expand view), route ETA,
ESP Web Tools one-click flasher page, session-stats view, map screensaver mode.

## Verification (owner flashes; no host toolchain)
Flash incrementally. Confirm per feature: plane glyph reads correctly at all headings
and coast/select/emergency states; CARTO base frames the right ground with city labels
and rescales with range; weather icons + wind correct; `/screen.bmp` renders in HA;
`/api/state` validates; HA auto-discovers the sensors; logos resolve by operator and
fall back cleanly; ISS appears only when overhead.
