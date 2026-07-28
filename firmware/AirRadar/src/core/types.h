// types.h — AirRadar v7 core data types (framework-agnostic, no LVGL here)
#pragma once
#include <stdint.h>
#include <string.h>
#include "../config.h"

// One tracked aircraft. Written by applyPending() (loop context) from the
// pending buffer; read by UI (same loop context) — no lock needed for reads.
struct Track {
  bool     valid;
  char     hex[8];        // ICAO id — identity key
  char     flight[12];    // callsign (may be blank)
  char     reg[12];       // tail number
  char     typeCode[8];   // e.g. A21N
  char     category[5];   // ADS-B emitter category, e.g. A3
  char     squawk[6];
  char     desc[32];      // airframe long name (aircraft DB, may be blank)
  char     ownOp[24];     // operator (aircraft DB, may be blank)
  char     year[6];
  bool     mil;           // dbFlags bit 0/1
  bool     routeTried;    // adsbdb lookup attempted for current callsign
  char     origin[5], dest[5];   // IATA route, "" until resolved
  double   lat, lon;
  float    gsKt, trackDeg;
  int      altFt;         // -1 = unknown
  int      vRateFpm;
  int      navAltFt;      // autopilot selected altitude, -1 = none
  uint32_t lastApiMs;
};

// Snapshot of one aircraft as parsed off the wire (produced on the net task,
// consumed by applyPending under the pending mutex).
struct ApiPlane {
  char   hex[8], flight[12], reg[12], typeCode[8], category[5], squawk[6];
  char   desc[32], ownOp[24], year[6];
  bool   mil;
  double lat, lon;
  float  gsKt, trackDeg;
  int    altFt, vRateFpm, navAltFt;
};

struct WeatherState {
  bool  valid;
  float tempC;
  float windKmh;
  int   windDirDeg;
  int   wmoCode;          // WMO weather code (Open-Meteo "weather_code")
  uint32_t fetchedMs;
};

struct IssState {
  bool   valid;
  double lat, lon;
  double altKm;
  uint32_t fetchedMs;
};

// Aircraft class filter bits (K_FILT_CLS)
enum : uint8_t {
  FCLS_AIRLINER = 0x01,   // category A3..A5 or has airline-style callsign
  FCLS_LIGHT    = 0x02,   // A1/A2
  FCLS_HELI     = 0x04,   // A7
  FCLS_MIL      = 0x08,   // dbFlags military
  FCLS_OTHER    = 0x10,   // everything else / unknown
};

// Screens
enum Screen : uint8_t {
  SCR_MAIN = 0,
  SCR_SETTINGS,
  SCR_WIFI,        // scan list + password keyboard
  SCR_COORDS,      // lat/lon editor
  SCR_TEXTEDIT,    // generic text editor (feeder url, watchlist, mqtt uri, ...)
  SCR_COUNT
};

// Helpers
inline bool sqIsEmergency(const char* sq) {
  return sq && (!strcmp(sq,"7500") || !strcmp(sq,"7600") || !strcmp(sq,"7700"));
}
inline const char* cardinal8(float deg) {
  static const char* c[8] = {"N","NE","E","SE","S","SW","W","NW"};
  return c[((int)((deg + 22.5f) / 45.0f)) & 7];
}
