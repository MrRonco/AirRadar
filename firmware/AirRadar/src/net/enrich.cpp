// enrich.cpp — keyless enrichment lookups: weather (Open-Meteo), ISS
// (open-notify), route (adsbdb).
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
#include <esp_heap_caps.h>
#include <FFat.h>
#include "enrich.h"
#include "../core/tracks.h"
#include "logos.h"                             // logosIcaoFromFlight: airline test

// ---------- file-local constants ----------
static const uint32_t kHttpTimeoutMs = 8000;   // internet convention (8..12 s)
static const int      kIssFailLimit  = 3;      // consecutive fails -> invalidate
static const size_t   kWxDocBytes    = 1536;   // Open-Meteo "current" payload
static const size_t   kIssDocBytes   = 1024;   // open-notify payload
static const size_t   kRouteDocBytes = 2048;   // adsbdb filtered payload (v6-proven)
static const size_t   kUrlMax        = 240;
static const uint32_t kTlsRetryMs    = 30000;  // re-arm delay when the TLS gate is shut

// ============================================================
//  Persistent route cache
// ============================================================
// A callsign's route does not change, so asking adsbdb for it more than once
// is waste — and worse, it makes routes hostage to internal heap: once free
// heap falls under AR_TLS_HEAP_FLOOR the gate shuts and every lookup is shed,
// which is why the origin/destination row kept going blank an hour into a
// boot. Cached routes need no network, no TLS and no heap, so they survive
// both the gate closing and a reboot.
//
// One flat table in PSRAM, mirrored to /rt/tbl. 192 x 48 B = 9 KB.
struct RouteRec { char cs[10]; char org[5]; char dst[5]; char air[28]; };
static const int    kRouteMax     = 192;
static const char   kRouteFile[]  = "/rt/tbl";
static const uint32_t kRouteFlushMs = 60000;     // batch writes; flash stalls the LCD
static RouteRec*    s_rt          = nullptr;
static int          s_rtN         = 0;
static bool         s_rtDirty     = false;
static uint32_t     s_rtFlushAt   = 0;
static bool         s_rtFsOk      = false;

static int routeFind(const char* cs) {
  for (int i = 0; i < s_rtN; i++)
    if (!strcmp(s_rt[i].cs, cs)) return i;
  return -1;
}

void enrichRouteCacheBegin() {
  s_rt = (RouteRec*)heap_caps_calloc(kRouteMax, sizeof(RouteRec), MALLOC_CAP_SPIRAM);
  if (!s_rt) { Serial.println("[route] cache alloc failed"); return; }
  s_rtFsOk = FFat.begin(true);
  if (!s_rtFsOk) return;
  if (!FFat.exists("/rt")) FFat.mkdir("/rt");
  File f = FFat.open(kRouteFile, FILE_READ);
  if (!f) return;
  int n = f.size() / sizeof(RouteRec);
  if (n > kRouteMax) n = kRouteMax;
  if (n > 0 && f.read((uint8_t*)s_rt, n * sizeof(RouteRec)) == n * (int)sizeof(RouteRec))
    s_rtN = n;
  f.close();
  Serial.printf("[route] %d cached routes loaded\n", s_rtN);
}

// Loop context. Batched: a flash write stalls the LCD DMA, so we do at most
// one small write a minute rather than one per newly seen callsign.
void enrichRouteCacheFlush(uint32_t nowMs) {
  if (!s_rtDirty || !s_rtFsOk || !s_rt) return;
  if ((int32_t)(nowMs - s_rtFlushAt) < 0) return;
  File f = FFat.open(kRouteFile, FILE_WRITE);
  if (f) {
    f.write((const uint8_t*)s_rt, s_rtN * sizeof(RouteRec));
    f.close();
  }
  s_rtDirty = false;
}

static void routeStore(const char* cs, const char* org, const char* dst,
                       const char* air) {
  if (!s_rt || !cs[0]) return;
  int i = routeFind(cs);
  if (i < 0) {
    // Full: drop the oldest. Traffic turns over, and a stale entry costs one
    // needless lookup, not a wrong answer.
    if (s_rtN >= kRouteMax) { memmove(s_rt, s_rt + 1, (kRouteMax - 1) * sizeof(RouteRec)); s_rtN = kRouteMax - 1; }
    i = s_rtN++;
  }
  memset(&s_rt[i], 0, sizeof(RouteRec));
  strlcpy(s_rt[i].cs, cs, sizeof(s_rt[i].cs));
  strlcpy(s_rt[i].org, org, sizeof(s_rt[i].org));
  strlcpy(s_rt[i].dst, dst, sizeof(s_rt[i].dst));
  strlcpy(s_rt[i].air, air, sizeof(s_rt[i].air));
  s_rtDirty  = true;
  s_rtFlushAt = millis() + kRouteFlushMs;
}

// ---------- weather job/flags ----------
struct WxJob { double lat, lon; };
static WxJob         s_wxJob;                  // written in loop ctx before spawn
static volatile bool s_wxBusy   = false;       // wx task alive
static bool          s_wxForce  = true;        // loop-ctx only: fetch ASAP
static uint32_t      s_wxLastKick = 0;
// Set by wxTask when it loses the TLS gate after being spawned. Without it the
// task silently consumed its 15-minute slot, so one lost race meant no weather
// for the rest of the boot (field-observed: wx.valid stayed false all session).
static volatile bool s_wxRetrySoon = false;

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
static volatile uint32_t s_routeRetryAfterMs = 0;   // request cooldown after defer

// outCode (optional) receives the HTTP status, or 0 when the request never got
// far enough to have one (TLS gate shut / begin failed). Callers use it to tell
// "the server answered: no such callsign" (404 — cache it) from "we never got
// an answer" (timeout/-1 — retry later).
// outDeferred must be a CALLER-LOCAL bool, not a file static: wxTask and
// routeTask have independent busy flags and both run on core 0, so a shared
// flag could be cleared by one task inside the other's read window — which
// silently converted a deferral into a permanent "no route".
static bool httpsGetJson(const char* url, JsonDocument& doc,
                         JsonDocument* filter, const char* tag,
                         int* outCode = nullptr, bool* outDeferred = nullptr) {
  if (outCode) *outCode = 0;
  if (outDeferred) *outDeferred = false;
  if (!tlsTryAcquire()) {                      // one TLS connection at a time
    if (outDeferred) *outDeferred = true;
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
    if (outCode) *outCode = code;
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
  bool deferred = false;
  if (httpsGetJson(url, doc, nullptr, "wx", nullptr, &deferred)) {
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
  } else if (deferred) {
    s_wxRetrySoon = true;   // don't swallow the 15-minute slot over a lost race
  }
  s_wxBusy = false;
  vTaskDelete(NULL);
}

void enrichKickWeather() {
  s_wxForce = true;                            // next enrichLoop pass spawns it
}

// ============================================================
//  ISS — open-notify (AR_ISS_API), cadence AR_POLL_ISS_MS
// ============================================================
static void issTask(void*) {
  uint32_t issH0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  // Plain HTTP by design (see AR_ISS_API in config.h): one less ~35 KB mbedTLS
  // handshake, and at a 15 s cadence one less competitor for the single-slot
  // TLS gate. The old "esp-tls leaks 1.5 KB per connection" rationale is
  // retracted -- see V7_PORT.md note 9 -- but the choice stands on its own.
  // open-notify returns iss_position lat/lon as STRINGS.
  DynamicJsonDocument doc(kIssDocBytes);
  bool ok = false;
  bool fetched = false;
  {
    WiFiClient net;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);
    if (http.begin(net, AR_ISS_API)) {
      http.addHeader("User-Agent", AR_USER_AGENT);
      int code = http.GET();
      if (code == HTTP_CODE_OK)
        fetched = !deserializeJson(doc, http.getStream());
      else
        Serial.printf("[enrich] iss: http %d\n", code);
      http.end();
    }
  }
  if (fetched) {
    double lat = atof(doc["iss_position"]["latitude"]  | "999");
    double lon = atof(doc["iss_position"]["longitude"] | "999");
    double alt = 420.0;                        // open-notify has no altitude
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
  char org[5] = "", dst[5] = "", airline[sizeof(g_routeResAirline)] = "";
  bool final = false;                          // may we mark the callsign tried?
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
    g_routeResAirline[0] = 0;
    g_routeResFinal = true;                    // a blank callsign never resolves
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
  // The same response carries the operator. Feeder aircraft databases are
  // FAA/Transport-Canada centric, so foreign registrations arrive with an
  // empty ownOp and used to show only callsign initials.
  filt["response"]["flightroute"]["airline"]["name"]          = true;
  DynamicJsonDocument doc(kRouteDocBytes);
  int  code = 0;
  bool deferred = false;
  if (httpsGetJson(url, doc, &filt, "route", &code, &deferred)) {
    JsonVariantConst fr = doc["response"]["flightroute"];
    strlcpy(org, fr["origin"]["iata_code"]      | "", sizeof(org));
    strlcpy(dst, fr["destination"]["iata_code"] | "", sizeof(dst));
    strlcpy(airline, fr["airline"]["name"]      | "", sizeof(airline));
    final = true;                              // the server answered
  } else if (deferred) {
    // Lost the TLS gate — NOT an answer. Don't mark tried; the main loop
    // re-requests once the cooldown passes.
    s_routeRetryAfterMs = millis() + 2500;
    g_routeFetching = false;
    vTaskDelete(NULL);
    return;
  } else if (code == 404) {
    final = true;    // adsbdb genuinely has no route for this callsign; cache it
  } else {
    // Timeout, DNS failure, 5xx, truncated JSON — we learned nothing. Posting
    // this as "tried" is what made routes vanish permanently after one hiccup.
    s_routeRetryAfterMs = millis() + 10000;
    g_routeFetching = false;
    vTaskDelete(NULL);
    return;
  }
  portENTER_CRITICAL(&g_dataMux);
  strlcpy(g_routeResHex,     s_routeJob.hex, sizeof(g_routeResHex));
  strlcpy(g_routeResOrigin,  org,            sizeof(g_routeResOrigin));
  strlcpy(g_routeResDest,    dst,            sizeof(g_routeResDest));
  strlcpy(g_routeResAirline, airline,        sizeof(g_routeResAirline));
  g_routeResFinal = final;
  g_routeResReady = true;                      // ready flag LAST
  portEXIT_CRITICAL(&g_dataMux);
  if (final) routeStore(safe, org, dst, airline);
  Serial.printf("[enrich] route %s: %s -> %s (%s)\n", s_routeJob.flight,
                org[0] ? org : "?", dst[0] ? dst : "?",
                airline[0] ? airline : "no airline");
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
  // Check the gate HERE, not inside the task: loop() re-requests every pass
  // while the selected aircraft has no route, and each spawn costs a 12 KB
  // internal stack. Setting the cooldown also throttles the heap query.
  // Cache first: a hit needs no task, no TLS and no heap, so routes keep
  // working with the gate shut and straight after a reboot.
  {
    char fl2[sizeof(s_routeJob.flight)];
    trimCopy(fl2, sizeof(fl2), flight);
    int i = routeFind(fl2);
    if (i >= 0) {
      portENTER_CRITICAL(&g_dataMux);
      strlcpy(g_routeResHex,     hex,           sizeof(g_routeResHex));
      strlcpy(g_routeResOrigin,  s_rt[i].org,   sizeof(g_routeResOrigin));
      strlcpy(g_routeResDest,    s_rt[i].dst,   sizeof(g_routeResDest));
      strlcpy(g_routeResAirline, s_rt[i].air,   sizeof(g_routeResAirline));
      g_routeResFinal = true;
      g_routeResReady = true;
      portEXIT_CRITICAL(&g_dataMux);
      return;
    }
  }
  if (!tlsGateOpen()) { s_routeRetryAfterMs = millis() + 3000; return; }
  char fl[sizeof(s_routeJob.flight)];
  trimCopy(fl, sizeof(fl), flight);
  if (!fl[0]) return;                          // blank callsign -> nothing to ask
  strlcpy(s_routeJob.hex, hex, sizeof(s_routeJob.hex));       // loop-ctx snapshot
  strlcpy(s_routeJob.flight, fl, sizeof(s_routeJob.flight));
  g_routeFetching = true;
  if (xTaskCreatePinnedToCore(routeTask, "route", AR_NET_TASK_STACK,
                              NULL, 1, NULL, 0) != pdPASS) {
    g_routeFetching = false;
    // Cooldown is essential here: loop() re-requests every pass, so without it
    // a failing spawn becomes a failed 12 KB malloc plus a Serial line at loop
    // rate — which blocks loop() on the UART and stalls LVGL entirely.
    s_routeRetryAfterMs = millis() + 5000;
    Serial.println("[enrich] route: task spawn failed");
  }
}

bool enrichApplyRoute() {
  if (!g_routeResReady) return false;
  char hex[8], org[5], dst[5], airline[sizeof(g_routeResAirline)];
  bool final;
  portENTER_CRITICAL(&g_dataMux);
  strlcpy(hex,     g_routeResHex,     sizeof(hex));
  strlcpy(org,     g_routeResOrigin,  sizeof(org));
  strlcpy(dst,     g_routeResDest,    sizeof(dst));
  strlcpy(airline, g_routeResAirline, sizeof(airline));
  final = g_routeResFinal;
  g_routeResReady = false;
  portEXIT_CRITICAL(&g_dataMux);
  Track* t = tracksFindByHex(hex);
  if (!t) return false;                        // track dropped while fetching
  if (org[0]) strlcpy(t->origin, org, sizeof(t->origin));
  if (dst[0]) strlcpy(t->dest,   dst, sizeof(t->dest));
  // Fill the operator only when the feeder didn't know it — a local aircraft
  // database entry is more specific than adsbdb's airline name. mergePlane()
  // never overwrites ownOp with a blank, so this survives later polls.
  if (airline[0] && !t->ownOp[0]) strlcpy(t->ownOp, airline, sizeof(t->ownOp));
  t->routeTried = final;                       // transient failure -> retry later
  return g_selHex[0] && !strcmp(g_selHex, hex);   // true only for selected a/c
}

// ============================================================
//  Scheduler (loop context)
// ============================================================
// Walk the distance-sorted in-range list and give ONE untried aircraft a route
// lookup per interval. Nearest first, so the aircraft the user is most likely
// to care about resolve soonest. enrichRequestRoute() does the deduping, the
// cooldown and the TLS gating, so this stays a simple picker.
void enrichRouteWalk(uint32_t nowMs) {
  static uint32_t lastWalkMs = 0;
  if (!g_wifiUp || g_routeFetching || g_routeResReady) return;
  if ((int32_t)(nowMs - lastWalkMs) < (int32_t)AR_POLL_ROUTE_MS) return;
  if (!tlsGateOpen()) { lastWalkMs = nowMs; return; }   // throttle the heap query
  for (int i = 0; i < g_orderN; i++) {
    Track& t = g_tracks[g_orderIdx[i]];
    if (t.routeTried || !t.flight[0]) continue;
    // Only airline-style callsigns have routes; skip GA tails so we don't burn
    // the interval on lookups adsbdb will 404 anyway.
    char icao[4];
    if (!logosIcaoFromFlight(t.flight, icao)) continue;
    lastWalkMs = nowMs;
    enrichRequestRoute(t.hex, t.flight);
    return;                                    // one per interval
  }
  lastWalkMs = nowMs;                          // nothing to do; re-check later
}

void enrichLoop(uint32_t nowMs) {
  if (!g_wifiUp) return;

  // Weather: cadence AR_POLL_WEATHER_MS, or forced by enrichKickWeather().
  if (s_wxRetrySoon && !s_wxBusy) {            // task lost the gate: re-arm short
    s_wxRetrySoon = false;
    s_wxLastKick = nowMs - AR_POLL_WEATHER_MS + kTlsRetryMs;
  }
  if (g_set.wxEn && !s_wxBusy &&
      (s_wxForce || nowMs - s_wxLastKick >= AR_POLL_WEATHER_MS)) {
    // Gate in loop context — spawning a 12 KB-stack task only for it to find
    // the TLS gate shut wastes the internal RAM the gate exists to protect.
    // Re-arm on a short retry instead of re-testing the heap every loop pass.
    if (!tlsGateOpen()) {
      s_wxForce = false;
      s_wxLastKick = nowMs - AR_POLL_WEATHER_MS + kTlsRetryMs;
    } else {
      s_wxForce = false;
      s_wxLastKick = nowMs;
      s_wxJob.lat = g_set.homeLat;             // snapshot in loop ctx (torn-read rule)
      s_wxJob.lon = g_set.homeLon;
      s_wxBusy = true;
      if (xTaskCreatePinnedToCore(wxTask, "wx", AR_NET_TASK_STACK,
                                  NULL, 1, NULL, 0) != pdPASS) {
        s_wxBusy = false;
        Serial.println("[enrich] wx: task spawn failed");
      }
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
