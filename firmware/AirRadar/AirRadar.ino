// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
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
#include "src/core/stall.h"
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
static uint32_t s_wakeUntilMs  = 0;   // quiet-hours reprieve; 0 = none pending
static char     s_nightAlertHex[8] = "";   // emergency already woken for

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

  stallBegin();                               // glitch instrumentation ring
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
  logosBegin();                               // FATFS logo cache (formats once)
  enrichRouteCacheBegin();                    // FATFS route cache
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

  // Every stage below runs in loop context, so any one of them running long
  // both delays the UI and competes with the panel DMA for the PSRAM bus.
  // Timing them individually names the culprit instead of merely proving that
  // something was slow. STAGE() is free when nothing exceeds AR_STALL_MS.
  uint8_t busy = (g_fetchInProgress ? BUSY_FEEDER : 0) |
                 (g_routeFetching   ? BUSY_ROUTE  : 0);
  #define STAGE(id, call) do { uint32_t _t0 = millis(); call; \
                               stallNote((id), millis() - _t0, busy); } while (0)

  // Reset the flush counter around LVGL only, so a recorded stall says how much
  // of the screen it actually repainted.
  extern volatile uint32_t g_flushPx, g_flushN;
  extern volatile int16_t  g_flushX1, g_flushY1, g_flushX2, g_flushY2;
  g_flushPx = 0; g_flushN = 0;
  uint32_t _lv0 = millis();
  lv_timer_handler();                         // LVGL render + input
  stallNote(ST_LVGL, millis() - _lv0, busy, g_flushPx, g_flushN,
            g_flushX1, g_flushY1, g_flushX2, g_flushY2);

  // Rule 2 is not a one-time setting. setAutoReconnect(true) means the stack
  // silently re-associates after any dropout, and the reconnect does NOT
  // preserve setSleep(false) -- modem sleep comes back, its wake bursts contend
  // with the panel's continuous PSRAM DMA, and the screen wiggles. That is
  // invisible to loop-stall timing because the contention is the radio, not the
  // CPU. Re-assert it on every transition into CONNECTED, and count them.
  {
    bool up = (WiFi.status() == WL_CONNECTED);
    if (up && !g_wifiUp) {
      WiFi.setSleep(false);
      g_wifiReconnects++;
      Serial.printf("[net] reconnected (#%lu) - setSleep(false) re-applied\n",
                    (unsigned long)g_wifiReconnects);
    }
    g_wifiUp = up;
  }

  STAGE(ST_WEB,    webLoop());
  STAGE(ST_FEEDER, feederLoop(now));
  STAGE(ST_ENRICH, enrichLoop(now));
  STAGE(ST_MAP,    mapLoop(now));
  STAGE(ST_MQTT,   mqttLoop(now));
  STAGE(ST_LOGOS,  logosLoop(now));
  STAGE(ST_RTCACHE, enrichRouteCacheFlush(now));   // FATFS write: stalls LCD DMA

  // Route result first, then (maybe) a new request — ordering avoids a
  // guaranteed duplicate adsbdb fetch right after each lookup completes.
  if (enrichApplyRoute()) cardsUpdate(now);
  if (!g_routeFetching && !g_routeResReady) {
    Track* sel = tracksSelected();
    if (sel && !sel->routeTried && sel->flight[0])
      enrichRequestRoute(sel->hex, sel->flight);   // selection always wins
    else
      enrichRouteWalk(now);                        // then fill in the rest
  }

  // New aircraft data. Use a FRESH timestamp for the refresh: applyPending
  // stamps tracks with a millis() taken after `now`, and an unsigned age of
  // (now - lastApiMs) would underflow into "everything is coasting".
  if (tracksApplyPending()) {
    uint32_t n2 = millis();
    uint32_t _t0 = millis();
    tracksRebuildOrder();
    scopeUpdate(n2);
    cardsUpdate(n2);
    stallNote(ST_TRACKS, millis() - _t0, busy);
  }

  // Dead-reckon + periodic scope refresh between polls
  if (now - s_lastPosTick >= AR_POLL_LOCAL_MS) {
    float dt = (now - s_lastPosTick) / 1000.0f;
    s_lastPosTick = now;
    uint32_t _dr0 = millis();
    tracksDeadReckon(dt);
    tracksRebuildOrder();
    scopeUpdate(now);
    stallNote(ST_DEADRECKON, millis() - _dr0, busy);
  }

  // Card cadence (clock, ages, emergency blink phases live inside uiTick)
  if (now - s_lastUiTick >= AR_UI_TICK_MS) {
    s_lastUiTick = now;
    STAGE(ST_UITICK, uiTick(now));
  }
  #undef STAGE

  // ---- Night mode ----------------------------------------------------
  // Tap-to-wake used to last less than a second. The wake below turned the
  // backlight on; the check above turned it straight back off on its next
  // 1 s tick unless a finger was STILL on the glass, because the only thing
  // keeping it lit was halTouchRead() returning true in that instant. There
  // was no timer. Quiet hours default to seven hours a day, so for seven
  // hours the only interaction this device offers responded and then took
  // itself away -- which reads as a hardware fault, not a policy.
  //
  // s_wakeUntilMs is that missing timer: a tap buys AR_NIGHT_WAKE_MS and the
  // panel goes dark again on its own.
  const bool quiet = g_set.nightEn && g_timeSynced && uiNightActive();

  if (!halBacklightState() && halTouchRead(nullptr, nullptr)) {
    halBacklight(true);                       // tap-to-wake
    if (quiet) s_wakeUntilMs = now + AR_NIGHT_WAKE_MS;
  }

  if (now - s_lastNightChk >= 1000) {
    s_lastNightChk = now;

    // A 7700 overhead at 03:00 was invisible by design. Light the panel for a
    // NEWLY seen emergency -- keyed on the hex so one aircraft holding a
    // squawk for an hour wakes the room once, not every second. Checked on
    // the 1 s tick, not in the loop body: the loop runs ~500 times a second
    // and this walks the whole track table.
    if (quiet) {
      Track* e = tracksFirstEmergency();
      if (e && strcmp(e->hex, s_nightAlertHex) != 0) {
        strlcpy(s_nightAlertHex, e->hex, sizeof(s_nightAlertHex));
        s_wakeUntilMs = now + AR_NIGHT_ALERT_MS;
        halBacklight(true);                   // the timer alone does not light it
        Serial.printf("[night] emergency %s -- waking panel\n", e->hex);
      } else if (!e) {
        s_nightAlertHex[0] = 0;               // rearm once the sky is clear
      }
    } else {
      s_nightAlertHex[0] = 0;
    }

    if (g_set.nightEn && g_timeSynced) {
      // Signed compare: s_wakeUntilMs wraps with millis() every 49 days.
      const bool reprieve = s_wakeUntilMs && (int32_t)(s_wakeUntilMs - now) > 0;
      if (quiet && halBacklightState()) {
        if (!reprieve && !halTouchRead(nullptr, nullptr)) {
          halBacklight(false);
          s_wakeUntilMs = 0;
        }
      } else if (!quiet && !halBacklightState()) {
        halBacklight(true);
        s_wakeUntilMs = 0;                    // daylight owns it again
      }
    }
  }

  delay(2);                                   // yield; LVGL cadence is timer-driven
}
