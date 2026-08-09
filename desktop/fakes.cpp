// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// fakes.cpp — everything the UI links against that is not the UI.
//
// The rule this file follows: stub the PLUMBING, keep the LOGIC. Anything that
// decides how a pixel looks — tracks.cpp's ordering and dead reckoning,
// state.cpp's scopeToScreen/altColorRGB/filters, brandcolor's table — is
// compiled from the real firmware source, so the harness renders what the
// device renders. Only the things that touch a network, a flash chip or a
// panel are faked here.
//
// The logo cache is the one deliberate exception: it always reports a miss, so
// the operator tile always draws its ICAO-in-brand-colour fallback. That is
// the path most aircraft take on the real device anyway, and it needs no
// 24-slot RAM cache to exercise.
#include <cstdarg>
#include <chrono>
#include "Arduino.h"
#include "WiFi.h"
#include "../firmware/AirRadar/src/core/state.h"
#include "../firmware/AirRadar/src/net/logos.h"
#include "../firmware/AirRadar/src/net/feeder.h"
#include "../firmware/AirRadar/src/net/enrich.h"
#include "../firmware/AirRadar/src/net/maptiles.h"
#include "../firmware/AirRadar/src/svc/web.h"
#include "../firmware/AirRadar/src/svc/mqtt.h"
#include "../firmware/AirRadar/src/hal/hal_display.h"

// ---------- Arduino runtime ----------
SerialShim Serial;
WiFiShim   WiFi;
EspShim    ESP;

static const std::chrono::steady_clock::time_point kT0 = std::chrono::steady_clock::now();
extern "C" uint32_t millis(void) {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - kT0).count();
}

int SerialShim::printf(const char* f, ...) {
  va_list a; va_start(a, f);
  int n = vfprintf(stdout, f, a);
  va_end(a);
  return n;
}

size_t strlcpy(char* dst, const char* src, size_t cap) {
  size_t n = strlen(src);
  if (cap) {
    size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, c);
    dst[c] = 0;
  }
  return n;
}

// ---------- network + flash modules: no-ops ----------
void logosBegin() {}
void logosLoop(uint32_t) {}
void logosRequest(const char*) {}
LogoState logosGet(const char*, const lv_img_dsc_t** out) { if (out) *out = nullptr; return LOGO_MISS; }
bool logosIcaoFromFlight(const char* flight, char out[4]) {
  if (!flight) return false;
  int n = 0;
  for (const char* p = flight; *p && n < 3; p++) {
    if (*p >= 'A' && *p <= 'Z') out[n++] = *p;
    else if (*p >= 'a' && *p <= 'z') out[n++] = *p - 32;
    else break;
  }
  out[n] = 0;
  return n == 3;
}

void feederLoop(uint32_t) {}
void feederKick() {}
void feederUpdateSrcName() {}

void enrichLoop(uint32_t) {}
void enrichRequestRoute(const char*, const char*) {}
void enrichRouteWalk(uint32_t) {}
void enrichRouteCacheBegin() {}
void enrichRouteCacheFlush(uint32_t) {}
bool enrichApplyRoute() { return false; }
void enrichKickWeather() {}

// No base map. The scope draws its rings and blips over the flat ink floor,
// which is also what the device shows before the first stitch lands.
void mapBegin() {}
void mapLoop(uint32_t) {}
void mapRequestRefresh() {}
const lv_img_dsc_t* mapImage() { return nullptr; }
uint32_t mapGeneration() { return 0; }
uint16_t* mapCleanBuf() { return nullptr; }
uint16_t* mapShowBuf(bool) { return nullptr; }
void mapPublishShow(const lv_area_t*) {}
const lv_area_t* mapDirtyArea() { return nullptr; }
void mapPublishClean() {}
size_t mapBufBytes() { return 0; }

void webBegin() {}
void webLoop() {}
void mqttBegin() {}
void mqttLoop(uint32_t) {}
void mqttRestart() {}

// ---------- panel ----------
// halBacklight is the only one the UI calls (night mode); the rest exist so the
// link succeeds if a future UI file reaches for them.
static bool s_bl = true;
bool halDisplayInit() { return true; }
void halBacklight(bool on) { s_bl = on; }
bool halBacklightState() { return s_bl; }
bool halTouchRead(int32_t*, int32_t*) { return false; }
void halReadRect(int, int, int, int, uint16_t*) {}
