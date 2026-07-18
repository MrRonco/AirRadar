# AirRadar

Real-time ADS-B air-traffic radar on a Waveshare ESP32-S3-Touch-LCD-7 (800×480),
with a glassmorphism UI, on-screen WiFi provisioning, touch aircraft selection,
adjustable range (50–250 km), on-device DHCP/static network config, and a browser
settings page — fed by a local adsb.im/tar1090 receiver with airplanes.live as
automatic cloud fallback.

## Quick start

1. Open `AirRadar.ino` in Arduino IDE 2.x with the esp32 core (3.3.x).
2. Board: **ESP32S3 Dev Module** with the exact Tools settings in `CLAUDE.md`
   (PSRAM = OPI PSRAM is the one that black-screens the device if missed).
3. Libraries: LovyanGFX 1.2.x, ArduinoJson **6.x** (not 7).
4. Flash. First boot opens the WiFi picker; everything after that is configured
   on the touchscreen or at `http://<device-ip>/`.

## Repo layout

- `AirRadar.ino` — the whole application (rendering, touch UI, data, web server)
- `LGFX_Waveshare_7.h` — display/touch/expander config; hardware ground truth
- `CLAUDE.md` — project context and hard-won rules for AI-assisted development
- `docs/` — hardware notes, version history, roadmap

Built iteratively on real hardware; see `docs/HISTORY.md` for the journey.
