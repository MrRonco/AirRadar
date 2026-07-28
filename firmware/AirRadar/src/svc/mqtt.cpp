// mqtt.cpp — Home Assistant integration via MQTT discovery (PubSubClient).
//
// Runs entirely in loop context (core 1): PubSubClient is pumped from
// mqttLoop(), no tasks are spawned, and live state (g_orderN, g_tracks via
// tracks.h helpers) is read directly, which the threading contract permits.
//
// URI format: mqtt://[user[:pass]@]host[:port]   (default port 1883)
// On connect: LWT airradar/status="offline" (retained), then "online",
// then retained HA discovery configs; states under airradar/state/* every
// AR_MQTT_PUB_MS.
#include "mqtt.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "../core/tracks.h"

// ---------- file-local constants ----------
static const uint16_t kDefaultPort       = 1883;
static const uint16_t kBufferBytes       = 1024;   // discovery JSON > 256 B default
static const uint32_t kReconnectMs       = 5000;
static const uint32_t kErrLogMs          = 60000;  // connect-failure log throttle
static const size_t   kDiscoveryJsonCap  = 768;    // StaticJsonDocument budget
static const size_t   kDiscoveryPayload  = 640;    // serialized config max

static const char* kTopicStatus    = "airradar/status";
static const char* kPayloadOnline  = "online";
static const char* kPayloadOffline = "offline";
static const char* kStateFmt       = "airradar/state/%s";

// ---------- parsed broker config ----------
static char     s_host[64]     = "";
static uint16_t s_port         = kDefaultPort;
static char     s_user[48]     = "";
static char     s_pass[48]     = "";
static char     s_clientId[20] = "";
static bool     s_cfgValid     = false;

static WiFiClient   s_net;
static PubSubClient s_mqtt(s_net);   // NB: setServer() keeps a pointer to s_host

static uint32_t s_lastTryMs    = 0;
static uint32_t s_lastErrLogMs = 0;
static uint32_t s_lastPubMs    = 0;

// ============================================================
//  URI parsing — mqtt://[user[:pass]@]host[:port]
// ============================================================
static bool mqttParseUri(const String& uri) {
  s_cfgValid = false;
  s_host[0] = s_user[0] = s_pass[0] = '\0';
  s_port = kDefaultPort;

  if (!uri.length()) {
    Serial.println("[mqtt] no broker URI configured");
    return false;
  }
  static const char* kScheme = "mqtt://";
  if (!uri.startsWith(kScheme)) {
    Serial.printf("[mqtt] unsupported scheme in \"%s\" - only mqtt:// is supported\n",
                  uri.c_str());
    return false;
  }
  String rest = uri.substring(strlen(kScheme));

  // optional user[:pass]@  (lastIndexOf so '@' inside a password still works)
  int at = rest.lastIndexOf('@');
  if (at >= 0) {
    String cred = rest.substring(0, at);
    rest = rest.substring(at + 1);
    int colon = cred.indexOf(':');
    String user = (colon >= 0) ? cred.substring(0, colon) : cred;
    String pass = (colon >= 0) ? cred.substring(colon + 1) : "";
    snprintf(s_user, sizeof(s_user), "%s", user.c_str());
    snprintf(s_pass, sizeof(s_pass), "%s", pass.c_str());
  }

  int slash = rest.indexOf('/');               // tolerate a trailing path
  if (slash >= 0) rest = rest.substring(0, slash);

  int colon = rest.indexOf(':');
  String host = (colon >= 0) ? rest.substring(0, colon) : rest;
  if (colon >= 0) {
    long p = rest.substring(colon + 1).toInt();
    if (p < 1 || p > 65535) {
      Serial.printf("[mqtt] invalid port in \"%s\"\n", uri.c_str());
      return false;
    }
    s_port = (uint16_t)p;
  }
  host.trim();
  if (!host.length()) {
    Serial.println("[mqtt] empty host in broker URI");
    return false;
  }
  snprintf(s_host, sizeof(s_host), "%s", host.c_str());
  s_cfgValid = true;
  return true;
}

static void mqttBuildClientId() {
  uint32_t low24 = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFULL);
  snprintf(s_clientId, sizeof(s_clientId), "airradar-%06X", (unsigned)low24);
}

// ============================================================
//  HA discovery
// ============================================================
static bool mqttPublishDiscoveryOne(const char* component, const char* key,
                                    const char* niceName, const char* unit,
                                    const char* deviceClass) {
  char uid[32], stateTopic[48], cfgTopic[80];
  snprintf(uid, sizeof(uid), "airradar_%s", key);
  snprintf(stateTopic, sizeof(stateTopic), kStateFmt, key);
  snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/%s/airradar_%s/config",
           component, key);

  StaticJsonDocument<kDiscoveryJsonCap> doc;   // char[] values are copied in
  doc["name"]               = niceName;
  doc["unique_id"]          = uid;
  doc["state_topic"]        = stateTopic;
  doc["availability_topic"] = kTopicStatus;
  if (unit)        doc["unit_of_measurement"] = unit;
  if (deviceClass) doc["device_class"]        = deviceClass;
  JsonObject dev = doc.createNestedObject("device");
  dev.createNestedArray("identifiers").add("airradar");
  dev["name"]         = "AirRadar";
  dev["manufacturer"] = "MrRonco";
  dev["model"]        = "ESP32-S3 Touch LCD 7";
  dev["sw_version"]   = AR_VERSION;

  char payload[kDiscoveryPayload];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload) - 1) {
    Serial.printf("[mqtt] discovery JSON for %s too large (%u B)\n", key, (unsigned)n);
    return false;
  }
  bool ok = s_mqtt.publish(cfgTopic, (const uint8_t*)payload, n, true);
  if (!ok) Serial.printf("[mqtt] discovery publish failed: %s\n", cfgTopic);
  return ok;
}

static void mqttPublishDiscovery() {
  struct SensorDef { const char* key; const char* name; const char* unit; };
  static const SensorDef kSensors[] = {
    { "in_range",   "Aircraft in range", nullptr },
    { "heard",      "Aircraft heard",    nullptr },
    { "feed_rate",  "Feed rate",         "msg/s" },
    { "source",     "Source",            nullptr },
    { "nearest",    "Nearest aircraft",  nullptr },
    { "nearest_km", "Nearest distance",  "km"    },
  };
  int ok = 0, total = 0;
  for (const SensorDef& s : kSensors) {
    ok += mqttPublishDiscoveryOne("sensor", s.key, s.name, s.unit, nullptr);
    total++;
  }
  ok += mqttPublishDiscoveryOne("binary_sensor", "emergency", "Emergency squawk",
                                nullptr, "safety");
  total++;
  Serial.printf("[mqtt] HA discovery published (%d/%d)\n", ok, total);
}

// ============================================================
//  State publishing
// ============================================================
static const char* mqttSourceName(uint32_t nowMs) {
  if (!g_lastGoodApply || nowMs - g_lastGoodApply > AR_STALE_FEED_MS) return "OFFLINE";
  return g_feedIsLocal ? g_localSrcName : "CLOUD";
}

static void mqttPublishState(const char* key, const char* value) {
  char topic[48];
  snprintf(topic, sizeof(topic), kStateFmt, key);
  if (!s_mqtt.publish(topic, value))
    Serial.printf("[mqtt] state publish failed: %s\n", topic);
}

static void mqttPublishStates(uint32_t nowMs) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d", g_orderN);
  mqttPublishState("in_range", buf);
  snprintf(buf, sizeof(buf), "%d", g_heardCount);
  mqttPublishState("heard", buf);
  if (g_feedMsgRate >= 0.0f) snprintf(buf, sizeof(buf), "%.1f", g_feedMsgRate);
  else                       snprintf(buf, sizeof(buf), "-1");
  mqttPublishState("feed_rate", buf);
  mqttPublishState("source", mqttSourceName(nowMs));

  Track* nr = tracksNearest();
  if (nr) {
    char name[16];
    const char* fl = nr->flight;
    while (*fl == ' ') fl++;                       // callsigns are space-padded
    snprintf(name, sizeof(name), "%s", *fl ? fl : nr->hex);
    for (int i = (int)strlen(name) - 1; i >= 0 && name[i] == ' '; i--) name[i] = '\0';
    mqttPublishState("nearest", name);
    snprintf(buf, sizeof(buf), "%.1f",
             haversineKm(g_set.homeLat, g_set.homeLon, nr->lat, nr->lon));
    mqttPublishState("nearest_km", buf);
  } else {
    mqttPublishState("nearest", "none");
    mqttPublishState("nearest_km", "-1");
  }
  mqttPublishState("emergency", tracksFirstEmergency() ? "ON" : "OFF");
}

// ============================================================
//  Connection management
// ============================================================
static void mqttTryConnect(uint32_t nowMs) {
  s_lastTryMs = nowMs;
  bool ok = s_mqtt.connect(s_clientId,
                           s_user[0] ? s_user : nullptr,
                           s_pass[0] ? s_pass : nullptr,
                           kTopicStatus, 0, true, kPayloadOffline);
  if (ok) {
    Serial.printf("[mqtt] connected %s:%u as %s\n", s_host, (unsigned)s_port, s_clientId);
    s_lastErrLogMs = 0;                            // next failure logs at once
    if (!s_mqtt.publish(kTopicStatus, kPayloadOnline, true))
      Serial.println("[mqtt] status publish failed");
    mqttPublishDiscovery();
    s_lastPubMs = 0;                               // publish state promptly
    return;
  }
  if (s_lastErrLogMs == 0 || nowMs - s_lastErrLogMs >= kErrLogMs) {
    s_lastErrLogMs = nowMs;
    Serial.printf("[mqtt] connect %s:%u failed, state=%d\n",
                  s_host, (unsigned)s_port, s_mqtt.state());
  }
}

static void mqttDisconnectGraceful(const char* why) {
  // LWT only fires on ungraceful loss — publish "offline" ourselves first so
  // HA availability stays truthful.
  s_mqtt.publish(kTopicStatus, kPayloadOffline, true);
  s_mqtt.disconnect();
  Serial.printf("[mqtt] disconnected (%s)\n", why);
}

// ============================================================
//  Public API
// ============================================================
void mqttBegin() {
  mqttBuildClientId();
  if (!s_mqtt.setBufferSize(kBufferBytes))
    Serial.println("[mqtt] WARNING: buffer alloc failed - discovery may truncate");
  if (!g_set.mqttEn) return;                       // no-op if disabled
  if (!mqttParseUri(g_set.mqttUri)) return;
  s_mqtt.setServer(s_host, s_port);
  s_lastTryMs = 0;                                 // connect on first loop pass
  Serial.printf("[mqtt] armed for %s:%u (client %s)\n",
                s_host, (unsigned)s_port, s_clientId);
}

void mqttLoop(uint32_t nowMs) {
  if (!g_set.mqttEn || !g_wifiUp) {
    if (s_mqtt.connected())
      mqttDisconnectGraceful(g_set.mqttEn ? "wifi down" : "disabled");
    return;
  }
  if (!s_cfgValid) return;

  if (!s_mqtt.connected() &&
      (s_lastTryMs == 0 || nowMs - s_lastTryMs >= kReconnectMs))
    mqttTryConnect(nowMs);

  s_mqtt.loop();
  if (!s_mqtt.connected()) return;

  if (s_lastPubMs == 0 || nowMs - s_lastPubMs >= AR_MQTT_PUB_MS) {
    s_lastPubMs = nowMs;
    mqttPublishStates(nowMs);
  }
}

void mqttRestart() {
  if (s_mqtt.connected()) mqttDisconnectGraceful("settings change");
  s_cfgValid     = false;
  s_lastTryMs    = 0;
  s_lastErrLogMs = 0;
  s_lastPubMs    = 0;
  if (!g_set.mqttEn) {
    Serial.println("[mqtt] disabled");
    return;
  }
  if (!mqttParseUri(g_set.mqttUri)) return;
  s_mqtt.setServer(s_host, s_port);                // discovery re-sent on connect
  Serial.printf("[mqtt] restart armed for %s:%u\n", s_host, (unsigned)s_port);
}
