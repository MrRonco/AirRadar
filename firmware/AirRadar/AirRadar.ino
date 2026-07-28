// AirRadar v7 — LVGL port for Waveshare ESP32-S3-Touch-LCD-7 (800x480)
//
// Architecture: loop() (core 1) owns LVGL + all state mutation; network runs
// as short-lived tasks on core 0 writing into pending buffers (see state.h).
//
// Board settings (Tools):
//   ESP32S3 Dev Module · USB CDC On Boot: Enabled · Flash: QIO 80MHz, 16MB
//   Partition: 16M Flash (3MB APP/9.9MB FATFS) · PSRAM: OPI PSRAM · 921600
// Libraries (pinned): lvgl 8.3.11 · LovyanGFX 1.2.25 · ArduinoJson 6.21.6 ·
//   PubSubClient 2.8  — and lv_conf.h copied beside the lvgl library folder.

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "src/config.h"
#include "src/core/state.h"
#include "src/core/tracks.h"
#include "src/net/feeder.h"
#include "src/net/enrich.h"
#include "src/net/maptiles.h"
#include "src/net/logos.h"
#include "src/hal/hal_display.h"
#include "src/ui/theme.h"
#include "src/ui/ui.h"
#include "src/svc/web.h"
#include "src/svc/mqtt.h"

static uint32_t s_lastUiTick   = 0;
static uint32_t s_lastPosTick  = 0;
static uint32_t s_lastNightChk = 0;

static void applyStaticIpIfSet() {
  if (!g_set.netStatic) return;
  IPAddress ip, gw, mk, dn;
  if (!ip.fromString(g_set.netIp) || !gw.fromString(g_set.netGw) ||
      !mk.fromString(g_set.netMask)) {
    Serial.println("[net] static config invalid - falling back to DHCP");
    return;
  }
  if (!g_set.netDns.length() || !dn.fromString(g_set.netDns)) dn = gw;
  WiFi.config(ip, gw, mk, dn);
}

static void connectWiFiBlocking() {
  if (!g_set.wifiSsid.length()) return;      // provisioning flow handles it
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  applyStaticIpIfSet();
  WiFi.begin(g_set.wifiSsid.c_str(), g_set.wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  WiFi.setSleep(false);                       // rule #2: no modem-sleep wiggle
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiUp = true;
    Serial.printf("[net] up %s (%s)\n", WiFi.localIP().toString().c_str(),
                  g_set.netStatic ? "static" : "DHCP");
    configTzTime(g_set.tz.c_str(), "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("[net] stored WiFi failed - offline until provisioned");
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\nAirRadar %s booting\n", AR_VERSION);
  if (!psramFound())
    Serial.println("[boot] WARNING: no PSRAM - set Tools>PSRAM to OPI PSRAM");

  settingsLoad();

  if (!halDisplayInit()) {
    // Panel likely dead without PSRAM; keep serial alive for diagnosis.
    while (true) { Serial.println("[boot] display init failed"); delay(2000); }
  }
  themeInit();
  uiInit();                                   // shows SCR_MAIN (or Wi-Fi setup)
  lv_timer_handler();                         // first paint before net blocking

  connectWiFiBlocking();

  feederUpdateSrcName();
  mapBegin();
  if (g_wifiUp) {
    webBegin();
    mqttBegin();
    mapRequestRefresh();
    enrichKickWeather();
    feederKick();
  } else if (!g_set.wifiSsid.length()) {
    wifiScreenOpen();                         // first boot: scan + provision
  }

  s_lastUiTick = s_lastPosTick = millis();
  Serial.println("[boot] ready");
}

void loop() {
  uint32_t now = millis();

  lv_timer_handler();                         // LVGL render + input

  g_wifiUp = (WiFi.status() == WL_CONNECTED);

  webLoop();
  feederLoop(now);
  enrichLoop(now);
  mapLoop(now);
  mqttLoop(now);
  logosLoop(now);

  // Route result first, then (maybe) a new request — ordering avoids a
  // guaranteed duplicate adsbdb fetch right after each lookup completes.
  if (enrichApplyRoute()) cardsUpdate(now);
  if (!g_routeFetching && !g_routeResReady) {
    Track* sel = tracksSelected();
    if (sel && !sel->routeTried && sel->flight[0])
      enrichRequestRoute(sel->hex, sel->flight);
  }

  // New aircraft data. Use a FRESH timestamp for the refresh: applyPending
  // stamps tracks with a millis() taken after `now`, and an unsigned age of
  // (now - lastApiMs) would underflow into "everything is coasting".
  if (tracksApplyPending()) {
    uint32_t n2 = millis();
    tracksRebuildOrder();
    scopeUpdate(n2);
    cardsUpdate(n2);
  }

  // Dead-reckon + periodic scope refresh between polls
  if (now - s_lastPosTick >= AR_POLL_LOCAL_MS) {
    float dt = (now - s_lastPosTick) / 1000.0f;
    s_lastPosTick = now;
    tracksDeadReckon(dt);
    tracksRebuildOrder();
    scopeUpdate(now);
  }

  // Card cadence (clock, ages, emergency blink phases live inside uiTick)
  if (now - s_lastUiTick >= AR_UI_TICK_MS) {
    s_lastUiTick = now;
    uiTick(now);
  }

  // Night mode: backlight off during quiet hours, any touch wakes it
  if (now - s_lastNightChk >= 1000) {
    s_lastNightChk = now;
    if (g_set.nightEn && g_timeSynced) {
      bool night = uiNightActive();
      if (night && halBacklightState()) {
        if (!halTouchRead(nullptr, nullptr)) halBacklight(false);
      } else if (!night && !halBacklightState()) {
        halBacklight(true);
      }
    }
  }
  if (!halBacklightState() && halTouchRead(nullptr, nullptr))
    halBacklight(true);                       // tap-to-wake

  delay(2);                                   // yield; LVGL cadence is timer-driven
}
