// feeder.cpp — aircraft data acquisition (local feeder first, cloud fallback)
//
// Ported from the proven v6 fetch architecture (fetchAircraftData/fetchParse/
// fillFilter/fetchStats/startFetch). Runs as a short-lived task on core 0:
//   * local feeder: plain HTTP, fast connect timeout, alternate
//     /tar1090/data/ <-> /data/ path retry, seen_pos staleness skip
//   * fallback: airplanes.live point query (radius from range, NM-capped)
// Results land in g_pendingPlanes/g_pendingCount/g_pendingHeard/g_pendingLocal
// under g_dataMux with g_pendingReady set last. stats.json piggybacks after a
// successful local fetch every AR_POLL_STATS_MS -> g_feedMsgRate.
//
// THREADING: feederLoop/feederKick/feederUpdateSrcName run in loop context
// (core 1). The fetch task only reads its job snapshot (never g_set) and only
// writes the g_pending* buffers + g_feedMsgRate under g_dataMux. No LVGL here.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include "feeder.h"

// ---------- file-local constants (no magic numbers) ----------
static const uint32_t HTTP_LAN_CONNECT_MS  = 1500;   // LAN: fail fast
static const uint32_t HTTP_LAN_TIMEOUT_MS  = 4000;
static const uint32_t HTTP_CLOUD_TIMEOUT_MS = 12000; // TLS + slow API headroom
static const size_t   FEED_URL_MAX     = 160;        // job snapshot URL buffer
static const size_t   CLOUD_URL_MAX    = 128;
static const size_t   FILTER_DOC_BYTES = 1024;       // shared parse filter
static const size_t   STATS_FILTER_BYTES = 192;
static const size_t   STATS_DOC_BYTES  = 4096;
static const float    KM_PER_NM        = 1.852f;
static const char     PATH_TAR1090[]   = "/tar1090/data/";
static const char     PATH_PLAIN[]     = "/data/";
static const char     STATS_FILE[]     = "stats.json";

// ---------- job snapshot (captured in loop context before task spawn) ----------
// Tasks must never read g_set (String/double torn-read risk) — everything the
// fetch needs is copied here first. One task at a time (g_fetchInProgress),
// so a single static job slot is safe.
struct FeederJob {
  double homeLat, homeLon;
  int    rangeKm;
  char   feedUrl[FEED_URL_MAX];
};
static FeederJob s_job;

static bool      s_kick           = false;   // loop-context only
static uint32_t  s_lastFetchStart = 0;       // loop-context only
static uint32_t  s_lastStatsStart = 0;       // fetch-task only (serialized)
static ApiPlane* s_parseBuf       = nullptr; // fetch-task only, lazy PSRAM

// ============================================================
//  Small helpers (task context)
// ============================================================
static void postPendingFail() {
  portENTER_CRITICAL(&g_dataMux);
  g_pendingOk    = false;
  g_pendingReady = true;                     // ready flag last
  portEXIT_CRITICAL(&g_dataMux);
}

static ApiPlane* parseBuf() {
  if (!s_parseBuf) {
    const size_t bytes = sizeof(ApiPlane) * AR_MAX_TRACKS;
    s_parseBuf = (ApiPlane*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_parseBuf)                          // PSRAM missing? last-ditch internal
      s_parseBuf = (ApiPlane*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!s_parseBuf) Serial.println("[feeder] parse buffer alloc failed");
  }
  return s_parseBuf;
}

// Derive the alternate local path: /tar1090/data/ <-> /data/.
// Returns false if the URL has neither marker or the result would not fit.
static bool buildAltUrl(const char* url, char* out, size_t outSz) {
  const char* from = PATH_TAR1090;
  const char* to   = PATH_PLAIN;
  const char* hit  = strstr(url, from);
  if (!hit) { from = PATH_PLAIN; to = PATH_TAR1090; hit = strstr(url, from); }
  if (!hit) return false;
  size_t head = (size_t)(hit - url);
  const char* tail = hit + strlen(from);
  if (head + strlen(to) + strlen(tail) + 1 > outSz) return false;
  memcpy(out, url, head);
  out[head] = '\0';
  strlcat(out, to, outSz);
  strlcat(out, tail, outSz);
  return true;
}

// ============================================================
//  Parsing (shared by local + cloud) — v6 fetchParse semantics
// ============================================================
static void fillFilter(JsonObject f) {
  f["hex"] = true; f["flight"] = true; f["lat"] = true; f["lon"] = true;
  f["alt_baro"] = true; f["alt_geom"] = true; f["gs"] = true; f["track"] = true;
  f["t"] = true; f["category"] = true; f["baro_rate"] = true; f["geom_rate"] = true;
  f["squawk"] = true; f["r"] = true; f["seen_pos"] = true;
  // v7 enrichment (aircraft DB fields; absent on some feeds — parsed defensively)
  f["desc"] = true; f["ownOp"] = true; f["year"] = true; f["dbFlags"] = true;
  f["nav_altitude_mcp"] = true;
}

static void parsePlane(JsonObject ac, ApiPlane& p) {
  memset(&p, 0, sizeof(ApiPlane));
  strlcpy(p.hex,      ac["hex"]      | "", sizeof(p.hex));
  strlcpy(p.flight,   ac["flight"]   | "", sizeof(p.flight));
  strlcpy(p.typeCode, ac["t"]        | "", sizeof(p.typeCode));
  strlcpy(p.category, ac["category"] | "", sizeof(p.category));
  strlcpy(p.squawk,   ac["squawk"]   | "", sizeof(p.squawk));
  strlcpy(p.reg,      ac["r"]        | "", sizeof(p.reg));
  strlcpy(p.desc,     ac["desc"]     | "", sizeof(p.desc));
  strlcpy(p.ownOp,    ac["ownOp"]    | "", sizeof(p.ownOp));
  if (ac["year"].is<const char*>())
    strlcpy(p.year, ac["year"] | "", sizeof(p.year));
  else if (ac["year"].is<int>())
    snprintf(p.year, sizeof(p.year), "%d", ac["year"].as<int>());
  int flags = ac["dbFlags"] | 0;
  p.mil = ((flags & 3) != 0);
  p.lat = ac["lat"];
  p.lon = ac["lon"];
  p.gsKt     = ac["gs"]    | 0.0f;
  p.trackDeg = ac["track"] | 0.0f;
  if      (ac["alt_baro"].is<int>())         p.altFt = ac["alt_baro"];
  else if (ac["alt_baro"].is<const char*>()) p.altFt = 0;      // "ground"
  else if (ac["alt_geom"].is<int>())         p.altFt = ac["alt_geom"];
  else                                       p.altFt = -1;
  if      (ac["baro_rate"].is<int>()) p.vRateFpm = ac["baro_rate"];
  else if (ac["geom_rate"].is<int>()) p.vRateFpm = ac["geom_rate"];
  else                                p.vRateFpm = 0;
  p.navAltFt = ac["nav_altitude_mcp"].is<int>() ? ac["nav_altitude_mcp"].as<int>() : -1;
}

static bool fetchParse(Stream& s, bool local, const FeederJob& job) {
  StaticJsonDocument<FILTER_DOC_BYTES> filter;
  fillFilter(filter["ac"].createNestedObject());        // airplanes.live dialect
  fillFilter(filter["aircraft"].createNestedObject());  // readsb/tar1090 dialect

  DynamicJsonDocument doc(AR_JSON_DOC_BYTES);
  DeserializationError err = deserializeJson(doc, s, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[feeder] JSON(%s): %s\n", local ? "local" : "cloud", err.c_str());
    return false;
  }
  JsonArray arr = doc["aircraft"].as<JsonArray>();
  if (arr.isNull()) arr = doc["ac"].as<JsonArray>();
  if (arr.isNull()) {
    Serial.printf("[feeder] %s: no aircraft array\n", local ? "local" : "cloud");
    return false;
  }
  ApiPlane* tmp = parseBuf();
  if (!tmp) return false;

  int n = 0, heard = 0;
  for (JsonObject ac : arr) {
    if (!ac["lat"].is<float>() || !ac["lon"].is<float>()) continue;
    if (local) {                            // our coast logic owns stale positions
      float sp = ac["seen_pos"] | 999.0f;
      if (sp > AR_SEENPOS_SKIP_S) continue;
    }
    heard++;                                // aircraft with a live position
    double lat = ac["lat"], lon = ac["lon"];
    if (haversineKm(job.homeLat, job.homeLon, lat, lon) > (float)job.rangeKm) continue;
    if (n >= AR_MAX_TRACKS) continue;       // in range but table full
    parsePlane(ac, tmp[n]);
    n++;
  }

  portENTER_CRITICAL(&g_dataMux);
  g_pendingCount = n;
  g_pendingHeard = heard;
  memcpy(g_pendingPlanes, tmp, sizeof(ApiPlane) * n);
  g_pendingOk    = true;
  g_pendingLocal = local;
  g_pendingReady = true;                    // ready flag last
  portEXIT_CRITICAL(&g_dataMux);
  Serial.printf("[feeder] fetch(%s): %d in range, %d heard\n",
                local ? "local" : "cloud", n, heard);
  return true;
}

// ============================================================
//  Sources (task context)
// ============================================================
static bool tryLocal(const FeederJob& job) {
  char alt[FEED_URL_MAX];
  const char* urls[2] = { job.feedUrl, nullptr };
  if (buildAltUrl(job.feedUrl, alt, sizeof(alt))) urls[1] = alt;

  for (int u = 0; u < 2 && urls[u]; u++) {
    WiFiClient net;
    HTTPClient http;
    http.setConnectTimeout(HTTP_LAN_CONNECT_MS);
    http.setTimeout(HTTP_LAN_TIMEOUT_MS);
    http.useHTTP10(true);
    if (!http.begin(net, urls[u])) {
      Serial.printf("[feeder] bad local url %s\n", urls[u]);
      continue;
    }
    int code = http.GET();
    bool ok = (code == 200) && fetchParse(http.getStream(), true, job);
    http.end();
    if (ok) return true;
    Serial.printf("[feeder] local %s -> HTTP %d\n", urls[u], code);
  }
  return false;
}

static bool tryCloud(const FeederJob& job) {
  if (!tlsTryAcquire()) {                   // another TLS fetch is running —
    Serial.println("[feeder] TLS busy - cloud pass skipped");
    return false;                           // next poll retries in 8s
  }
  int radiusNm = (int)ceilf(job.rangeKm / KM_PER_NM);
  if (radiusNm > AR_CLOUD_RADIUS_NM_CAP) radiusNm = AR_CLOUD_RADIUS_NM_CAP;
  char url[CLOUD_URL_MAX];
  snprintf(url, sizeof(url), "%s%.4f/%.4f/%d",
           AR_CLOUD_API, job.homeLat, job.homeLon, radiusNm);

  WiFiClientSecure client;
  client.setInsecure();                     // keyless public API, no cert store
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(HTTP_CLOUD_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("[feeder] cloud begin failed");
    tlsRelease();
    return false;
  }
  http.addHeader("User-Agent", AR_USER_AGENT);
  int code = http.GET();
  bool ok = (code == 200) && fetchParse(http.getStream(), false, job);
  http.end();
  tlsRelease();
  if (!ok) Serial.printf("[feeder] cloud -> HTTP %d\n", code);
  return ok;
}

// ---- Feed rate: tar1090/readsb stats.json (local feeder only) ----
static void fetchStats(const FeederJob& job) {
  const char* slash = strrchr(job.feedUrl, '/');
  if (!slash) return;
  char url[FEED_URL_MAX + sizeof(STATS_FILE)];
  size_t dirLen = (size_t)(slash - job.feedUrl) + 1;
  if (dirLen + strlen(STATS_FILE) + 1 > sizeof(url)) return;
  memcpy(url, job.feedUrl, dirLen);
  url[dirLen] = '\0';
  strlcat(url, STATS_FILE, sizeof(url));

  WiFiClient net;
  HTTPClient http;
  http.setConnectTimeout(HTTP_LAN_CONNECT_MS);
  http.setTimeout(HTTP_LAN_TIMEOUT_MS);
  http.useHTTP10(true);
  if (!http.begin(net, url)) return;
  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<STATS_FILTER_BYTES> filt;
    filt["last1min"]["messages"] = true;
    DynamicJsonDocument doc(STATS_DOC_BYTES);
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filt));
    if (!err) {
      long m = doc["last1min"]["messages"] | (long)-1;
      if (m >= 0) {
        float rate = (float)m / 60.0f;
        portENTER_CRITICAL(&g_dataMux);
        g_feedMsgRate = rate;
        portEXIT_CRITICAL(&g_dataMux);
      }
    } else {
      Serial.printf("[feeder] stats JSON: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[feeder] stats -> HTTP %d\n", code);
  }
  http.end();
}

// ============================================================
//  Task body
// ============================================================
static void fetchAircraft(const FeederJob& job) {
  if (WiFi.status() != WL_CONNECTED) { postPendingFail(); return; }

  if (tryLocal(job)) {                      // 1) local feeder — LAN, plain HTTP
    if (millis() - s_lastStatsStart >= AR_POLL_STATS_MS) {
      s_lastStatsStart = millis();
      fetchStats(job);                      // piggyback on the good local pass
    }
    return;
  }
  if (tryCloud(job)) return;                // 2) airplanes.live fallback
  postPendingFail();
}

static void fetchTask(void* param) {
  const FeederJob* job = (const FeederJob*)param;
  fetchAircraft(*job);
  g_fetchInProgress = false;                // clear in-progress flag, then die
  vTaskDelete(NULL);
}

// ============================================================
//  Public API (loop context)
// ============================================================
void feederLoop(uint32_t nowMs) {
  if (g_fetchInProgress || !g_wifiUp) return;
  uint32_t cadence = g_feedIsLocal ? AR_POLL_LOCAL_MS : AR_POLL_CLOUD_MS;
  if (!s_kick && (nowMs - s_lastFetchStart) < cadence) return;
  s_kick = false;
  s_lastFetchStart = nowMs;

  // Snapshot everything the task needs — tasks never read g_set.
  s_job.homeLat = g_set.homeLat;
  s_job.homeLon = g_set.homeLon;
  s_job.rangeKm = g_set.rangeKm;
  if (g_set.feedUrl.length() >= sizeof(s_job.feedUrl))
    Serial.println("[feeder] feed url too long - truncated");
  strlcpy(s_job.feedUrl, g_set.feedUrl.c_str(), sizeof(s_job.feedUrl));

  g_fetchInProgress = true;
  BaseType_t ok = xTaskCreatePinnedToCore(fetchTask, "feeder", AR_NET_TASK_STACK,
                                          &s_job, 1, NULL, 0);
  if (ok != pdPASS) {
    g_fetchInProgress = false;
    Serial.println("[feeder] task spawn failed");
  }
}

void feederKick() {
  s_kick = true;
}

void feederUpdateSrcName() {
  // Extract host from g_set.feedUrl: after "://", up to first '/' or ':'.
  String host = g_set.feedUrl;
  int schemeEnd = host.indexOf("://");
  if (schemeEnd >= 0) host = host.substring(schemeEnd + 3);
  int hostEnd = 0;
  while (hostEnd < (int)host.length() && host[hostEnd] != '/' && host[hostEnd] != ':')
    hostEnd++;
  host = host.substring(0, hostEnd);
  host.trim();

  String lower = host;
  lower.toLowerCase();
  IPAddress ipProbe;
  bool isIpLiteral = host.length() && ipProbe.fromString(host.c_str());
  if (!host.length() || isIpLiteral || lower.endsWith(".local")) {
    strlcpy(g_localSrcName, "LOCAL", sizeof(g_localSrcName));
    return;
  }
  // Hostname: first DNS label, uppercased, truncated to fit.
  int dot = host.indexOf('.');
  String label = (dot >= 0) ? host.substring(0, dot) : host;
  label.trim();
  label.toUpperCase();
  if (!label.length()) {
    strlcpy(g_localSrcName, "LOCAL", sizeof(g_localSrcName));
    return;
  }
  strlcpy(g_localSrcName, label.c_str(), sizeof(g_localSrcName));
}
