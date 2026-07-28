// enrich.cpp — keyless enrichment lookups: weather (Open-Meteo), ISS
// (wheretheiss.at), route (adsbdb).
//
// Each lookup runs as its own short-lived task on core 0 with its own
// in-progress volatile flag and a job snapshot captured in loop context.
// Tasks only write g_wx / g_iss / g_routeRes* under g_dataMux (ready flag
// last) and end by clearing their busy flag + vTaskDelete(NULL).
// No LVGL, no g_tracks access from tasks (enrichApplyRoute is loop-context).
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include "enrich.h"
#include "../core/tracks.h"

// ---------- file-local constants ----------
static const uint32_t kHttpTimeoutMs = 8000;   // internet convention (8..12 s)
static const int      kIssFailLimit  = 3;      // consecutive fails -> invalidate
static const size_t   kWxDocBytes    = 1536;   // Open-Meteo "current" payload
static const size_t   kIssDocBytes   = 1024;   // wheretheiss.at payload
static const size_t   kRouteDocBytes = 2048;   // adsbdb filtered payload (v6-proven)
static const size_t   kUrlMax        = 240;

// ---------- weather job/flags ----------
struct WxJob { double lat, lon; };
static WxJob         s_wxJob;                  // written in loop ctx before spawn
static volatile bool s_wxBusy   = false;       // wx task alive
static bool          s_wxForce  = true;        // loop-ctx only: fetch ASAP
static uint32_t      s_wxLastKick = 0;

// ---------- ISS job/flags ----------
static volatile bool s_issBusy    = false;     // iss task alive
static bool          s_issFirst   = true;      // loop-ctx only
static uint32_t      s_issLastKick = 0;
static int           s_issFails   = 0;         // task-ctx only (tasks serialized)

// ---------- route job (in-progress flag is g_routeFetching, state.h) ----------
struct RouteJob { char hex[8]; char flight[12]; };
static RouteJob s_routeJob;                    // written in loop ctx before spawn

// ============================================================
//  Shared HTTPS GET -> JSON (task context)
// ============================================================
static volatile bool s_tlsDeferred = false;    // last httpsGetJson lost the gate
static volatile uint32_t s_routeRetryAfterMs = 0;   // request cooldown after defer

static bool httpsGetJson(const char* url, JsonDocument& doc,
                         JsonDocument* filter, const char* tag) {
  s_tlsDeferred = false;
  if (!tlsTryAcquire()) {                      // one TLS connection at a time
    s_tlsDeferred = true;
    Serial.printf("[enrich] %s: TLS busy - deferred\n", tag);
    return false;                              // all callers re-poll later
  }
  bool ok = false;
  {
    WiFiClientSecure client;
    client.setInsecure();                      // keyless public APIs, no pinning
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);                      // no chunked TE -> stream parse safe
    if (!http.begin(client, url)) {
      Serial.printf("[enrich] %s: begin failed\n", tag);
      tlsRelease();
      return false;
    }
    http.addHeader("User-Agent", AR_USER_AGENT);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      Serial.printf("[enrich] %s: http %d\n", tag, code);
      http.end();
      tlsRelease();
      return false;
    }
    DeserializationError err = filter
      ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
      : deserializeJson(doc, http.getStream());
    http.end();
    if (err) Serial.printf("[enrich] %s: json parse: %s\n", tag, err.c_str());
    ok = !err;
  }                                            // client fully destroyed here
  tlsRelease();
  return ok;
}

// ============================================================
//  Weather — Open-Meteo (AR_WX_API), cadence AR_POLL_WEATHER_MS
// ============================================================
static void wxTask(void*) {
  char url[kUrlMax];
  snprintf(url, sizeof(url),
           "%s?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,wind_speed_10m,wind_direction_10m,weather_code",
           AR_WX_API, s_wxJob.lat, s_wxJob.lon);
  DynamicJsonDocument doc(kWxDocBytes);
  if (httpsGetJson(url, doc, nullptr, "wx")) {
    JsonVariantConst cur = doc["current"];
    if (!cur.isNull() && cur["temperature_2m"].is<float>()) {
      float tempC   = cur["temperature_2m"]    | 0.0f;
      float windKmh = cur["wind_speed_10m"]    | 0.0f;  // Open-Meteo default km/h
      int   windDir = cur["wind_direction_10m"] | 0;
      int   wmo     = cur["weather_code"]       | 0;
      uint32_t nowMs = millis();
      portENTER_CRITICAL(&g_dataMux);
      g_wx.tempC      = tempC;
      g_wx.windKmh    = windKmh;
      g_wx.windDirDeg = windDir;
      g_wx.wmoCode    = wmo;
      g_wx.fetchedMs  = nowMs;
      g_wx.valid      = true;                  // validity flag last
      portEXIT_CRITICAL(&g_dataMux);
      Serial.printf("[enrich] wx %.1fC wind %.0fkm/h @%d wmo %d\n",
                    tempC, windKmh, windDir, wmo);
    } else {
      Serial.println("[enrich] wx: payload missing 'current' block");
    }
  }
  s_wxBusy = false;
  vTaskDelete(NULL);
}

void enrichKickWeather() {
  s_wxForce = true;                            // next enrichLoop pass spawns it
}

// ============================================================
//  ISS — wheretheiss.at (AR_ISS_API), cadence AR_POLL_ISS_MS
// ============================================================
static void issTask(void*) {
  DynamicJsonDocument doc(kIssDocBytes);
  bool ok = false;
  if (httpsGetJson(AR_ISS_API, doc, nullptr, "iss")) {
    double lat = doc["latitude"]  | 999.0;     // sentinel = missing key
    double lon = doc["longitude"] | 999.0;
    double alt = doc["altitude"]  | 0.0;
    if (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
      uint32_t nowMs = millis();
      portENTER_CRITICAL(&g_dataMux);
      g_iss.lat = lat; g_iss.lon = lon; g_iss.altKm = alt;
      g_iss.fetchedMs = nowMs;
      g_iss.valid = true;
      portEXIT_CRITICAL(&g_dataMux);
      ok = true;
    } else {
      Serial.println("[enrich] iss: coords missing/out of range");
    }
  }
  if (ok) {
    s_issFails = 0;
  } else if (++s_issFails >= kIssFailLimit) {
    portENTER_CRITICAL(&g_dataMux);
    g_iss.valid = false;                       // stale ISS is worse than none
    portEXIT_CRITICAL(&g_dataMux);
    Serial.printf("[enrich] iss: %d consecutive fails - invalidated\n", s_issFails);
  }
  s_issBusy = false;
  vTaskDelete(NULL);
}

// ============================================================
//  Route — adsbdb (AR_ROUTE_API + callsign), single-slot result
// ============================================================
static void routeTask(void*) {
  char org[5] = "", dst[5] = "";
  // Callsign comes straight off the ADS-B wire — allow only [A-Za-z0-9] into
  // the URL so a hostile broadcast can't inject path/query segments.
  char safe[sizeof(s_routeJob.flight)];
  int si = 0;
  for (const char* p = s_routeJob.flight; *p && si < (int)sizeof(safe) - 1; p++)
    if (isalnum((unsigned char)*p)) safe[si++] = *p;
  safe[si] = '\0';
  if (!si) {                                   // nothing valid to look up
    portENTER_CRITICAL(&g_dataMux);
    strlcpy(g_routeResHex, s_routeJob.hex, sizeof(g_routeResHex));
    g_routeResOrigin[0] = 0;
    g_routeResDest[0] = 0;
    g_routeResReady = true;
    portEXIT_CRITICAL(&g_dataMux);
    g_routeFetching = false;
    vTaskDelete(NULL);
    return;
  }
  char url[kUrlMax];
  snprintf(url, sizeof(url), "%s%s", AR_ROUTE_API, safe);
  StaticJsonDocument<512> filt;                // v6-proven filter + doc sizes
  filt["response"]["flightroute"]["origin"]["iata_code"]      = true;
  filt["response"]["flightroute"]["destination"]["iata_code"] = true;
  DynamicJsonDocument doc(kRouteDocBytes);
  if (httpsGetJson(url, doc, &filt, "route")) {
    JsonVariantConst fr = doc["response"]["flightroute"];
    strlcpy(org, fr["origin"]["iata_code"]      | "", sizeof(org));
    strlcpy(dst, fr["destination"]["iata_code"] | "", sizeof(dst));
  } else if (s_tlsDeferred) {
    // Lost the TLS gate — NOT an answer. Don't mark tried; the main loop
    // re-requests once the cooldown passes.
    s_routeRetryAfterMs = millis() + 2500;
    g_routeFetching = false;
    vTaskDelete(NULL);
    return;
  }
  // Post the result even when blank/failed — that is what marks the callsign
  // as tried (v6 semantics; callsign change resets routeTried for a retry).
  portENTER_CRITICAL(&g_dataMux);
  strlcpy(g_routeResHex,    s_routeJob.hex, sizeof(g_routeResHex));
  strlcpy(g_routeResOrigin, org,            sizeof(g_routeResOrigin));
  strlcpy(g_routeResDest,   dst,            sizeof(g_routeResDest));
  g_routeResReady = true;                      // ready flag LAST
  portEXIT_CRITICAL(&g_dataMux);
  Serial.printf("[enrich] route %s: %s -> %s\n", s_routeJob.flight,
                org[0] ? org : "?", dst[0] ? dst : "?");
  g_routeFetching = false;
  vTaskDelete(NULL);
}

// Copy `in` to `out` with leading/trailing spaces removed.
static void trimCopy(char* out, size_t outLen, const char* in) {
  while (*in == ' ') in++;
  strlcpy(out, in, outLen);
  size_t n = strlen(out);
  while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
}

void enrichRequestRoute(const char* hex, const char* flight) {
  if (g_routeFetching) return;                 // dedupe: one lookup in flight
  if ((int32_t)(millis() - s_routeRetryAfterMs) < 0) return;   // TLS-busy cooldown
  if (!hex || !hex[0] || !flight) return;
  char fl[sizeof(s_routeJob.flight)];
  trimCopy(fl, sizeof(fl), flight);
  if (!fl[0]) return;                          // blank callsign -> nothing to ask
  strlcpy(s_routeJob.hex, hex, sizeof(s_routeJob.hex));       // loop-ctx snapshot
  strlcpy(s_routeJob.flight, fl, sizeof(s_routeJob.flight));
  g_routeFetching = true;
  if (xTaskCreatePinnedToCore(routeTask, "route", AR_NET_TASK_STACK,
                              NULL, 1, NULL, 0) != pdPASS) {
    g_routeFetching = false;
    Serial.println("[enrich] route: task spawn failed");
  }
}

bool enrichApplyRoute() {
  if (!g_routeResReady) return false;
  char hex[8], org[5], dst[5];
  portENTER_CRITICAL(&g_dataMux);
  strlcpy(hex, g_routeResHex,    sizeof(hex));
  strlcpy(org, g_routeResOrigin, sizeof(org));
  strlcpy(dst, g_routeResDest,   sizeof(dst));
  g_routeResReady = false;
  portEXIT_CRITICAL(&g_dataMux);
  Track* t = tracksFindByHex(hex);
  if (!t) return false;                        // track dropped while fetching
  strlcpy(t->origin, org, sizeof(t->origin));
  strlcpy(t->dest,   dst, sizeof(t->dest));
  t->routeTried = true;
  return g_selHex[0] && !strcmp(g_selHex, hex);   // true only for selected a/c
}

// ============================================================
//  Scheduler (loop context)
// ============================================================
void enrichLoop(uint32_t nowMs) {
  if (!g_wifiUp) return;

  // Weather: cadence AR_POLL_WEATHER_MS, or forced by enrichKickWeather().
  if (g_set.wxEn && !s_wxBusy &&
      (s_wxForce || nowMs - s_wxLastKick >= AR_POLL_WEATHER_MS)) {
    s_wxForce = false;
    s_wxLastKick = nowMs;
    s_wxJob.lat = g_set.homeLat;               // snapshot in loop ctx (torn-read rule)
    s_wxJob.lon = g_set.homeLon;
    s_wxBusy = true;
    if (xTaskCreatePinnedToCore(wxTask, "wx", AR_NET_TASK_STACK,
                                NULL, 1, NULL, 0) != pdPASS) {
      s_wxBusy = false;
      Serial.println("[enrich] wx: task spawn failed");
    }
  }

  // ISS: cadence AR_POLL_ISS_MS.
  if (g_set.issEn && !s_issBusy &&
      (s_issFirst || nowMs - s_issLastKick >= AR_POLL_ISS_MS)) {
    s_issFirst = false;
    s_issLastKick = nowMs;
    s_issBusy = true;
    if (xTaskCreatePinnedToCore(issTask, "iss", AR_NET_TASK_STACK,
                                NULL, 1, NULL, 0) != pdPASS) {
      s_issBusy = false;
      Serial.println("[enrich] iss: task spawn failed");
    }
  }
}
