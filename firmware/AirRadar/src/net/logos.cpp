// logos.cpp — airline logo fetch/cache. See logos.h for the contract.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ctype.h>
#include <string.h>
#include "logos.h"

static const char     kUrlBase[]     = "https://raw.githubusercontent.com/theqkash/esp32flight-logos/main/logos/";
static const int      kSlots         = 8;                 // LRU-ish ring
static const size_t   kPngMax        = 96 * 1024;         // download cap
static const uint32_t kHttpTimeoutMs = 8000;
static const size_t   kPixBytes      = LOGO_PX * LOGO_PX * 2;

struct LogoSlot {
  char          key[4];       // ICAO prefix, "" = free
  LogoState     state;
  uint16_t*     pix;          // PSRAM RGB565, allocated on first use
  lv_img_dsc_t  dsc;
  uint32_t      lastUse;
};
static LogoSlot s_slots[kSlots];

static volatile bool s_fetching = false;
static char s_reqKey[4] = "";               // loop-context queue (single slot)
static char s_jobKey[4] = "";               // task-owned copy
static int  s_jobSlot   = -1;
static volatile bool     s_resReady = false;
static volatile LogoState s_resState = LOGO_MISS;
static uint8_t* s_png = nullptr;            // task-only download buffer

// ---------- slot management (loop context) ----------
static int slotFind(const char* key) {
  for (int i = 0; i < kSlots; i++)
    if (!strcmp(s_slots[i].key, key)) return i;
  return -1;
}

static int slotAlloc() {
  int best = 0;
  uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < kSlots; i++) {
    if (!s_slots[i].key[0]) return i;
    if (s_slots[i].lastUse < oldest) { oldest = s_slots[i].lastUse; best = i; }
  }
  return best;                              // evict least-recently-used
}

// ---------- fetch task (core 0) ----------
static bool fetchPng(const char* key, size_t* outLen) {
  if (!s_png) {
    s_png = (uint8_t*)heap_caps_malloc(kPngMax, MALLOC_CAP_SPIRAM);
    if (!s_png) { Serial.println("[logo] png buf alloc failed"); return false; }
  }
  char url[128];
  snprintf(url, sizeof(url), "%s%s.png", kUrlBase, key);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", AR_USER_AGENT);
  int code = http.GET();
  if (code != 200) {
    http.end();
    if (code != 404)                        // 404 = airline simply not in the pack
      Serial.printf("[logo] %s -> HTTP %d\n", key, code);
    return false;
  }
  int clen = http.getSize();
  WiFiClient* st = http.getStreamPtr();
  size_t total = 0;
  uint32_t t0 = millis();
  while (http.connected() && (clen < 0 || (int)total < clen) && total < kPngMax) {
    size_t avail = st->available();
    if (avail) {
      size_t room = kPngMax - total;
      if (avail > room) avail = room;
      int r = st->readBytes(s_png + total, avail);
      if (r > 0) { total += (size_t)r; t0 = millis(); }
    } else {
      if (millis() - t0 > 3000) break;
      delay(4);
    }
  }
  http.end();
  if (total < 100) return false;
  *outLen = total;
  return true;
}

static void logoTask(void*) {
  LogoState result = LOGO_MISS;
  size_t len = 0;
  if (WiFi.status() == WL_CONNECTED && fetchPng(s_jobKey, &len)) {
    // Decode + downscale 90 -> 46 into an offscreen sprite, then copy pixels.
    LGFX_Sprite spr(nullptr);
    spr.setColorDepth(16);
    spr.setPsram(true);
    if (spr.createSprite(LOGO_PX, LOGO_PX)) {
      spr.fillScreen(0xFFFF);               // logos ship on a white chip
      float sc = (float)LOGO_PX / 90.0f;
      spr.drawPng(s_png, len, 0, 0, LOGO_PX, LOGO_PX, 0, 0, sc, sc);
      LogoSlot& sl = s_slots[s_jobSlot];    // pix ptr prepared in loop context
      // Sprite buffers are swap565; store plain RGB565 for LVGL (swap=0).
      const uint16_t* src = (const uint16_t*)spr.getBuffer();
      for (int i = 0; i < LOGO_PX * LOGO_PX; i++)
        sl.pix[i] = __builtin_bswap16(src[i]);
      spr.deleteSprite();
      result = LOGO_OK;
    } else {
      Serial.println("[logo] sprite alloc failed");
    }
  }
  s_resState = result;
  s_resReady = true;                        // consumed by logosLoop
  s_fetching = false;
  vTaskDelete(NULL);
}

// ---------- public API (loop context) ----------
void logosRequest(const char* icao3) {
  if (!icao3 || strlen(icao3) != 3) return;
  if (slotFind(icao3) >= 0) return;         // cached (any state)
  if (s_reqKey[0] || s_fetching) return;    // one in flight; retried next tick
  strlcpy(s_reqKey, icao3, sizeof(s_reqKey));
}

LogoState logosGet(const char* icao3, const lv_img_dsc_t** out) {
  if (out) *out = nullptr;
  if (!icao3 || !icao3[0]) return LOGO_MISS;
  int i = slotFind(icao3);
  if (i < 0) return LOGO_UNKNOWN;
  s_slots[i].lastUse = millis();
  if (s_slots[i].state == LOGO_OK && out) *out = &s_slots[i].dsc;
  return s_slots[i].state;
}

void logosLoop(uint32_t nowMs) {
  (void)nowMs;
  // Land a finished fetch.
  if (s_resReady) {
    s_resReady = false;
    if (s_jobSlot >= 0) s_slots[s_jobSlot].state = s_resState;
    s_jobSlot = -1;
  }
  // Launch the queued request.
  if (s_reqKey[0] && !s_fetching && g_wifiUp) {
    int slot = slotAlloc();
    LogoSlot& sl = s_slots[slot];
    if (!sl.pix) {
      sl.pix = (uint16_t*)heap_caps_malloc(kPixBytes, MALLOC_CAP_SPIRAM);
      if (!sl.pix) { Serial.println("[logo] pix alloc failed"); s_reqKey[0] = 0; return; }
    }
    strlcpy(sl.key, s_reqKey, sizeof(sl.key));
    sl.state   = LOGO_PENDING;
    sl.lastUse = millis();
    memset(&sl.dsc, 0, sizeof(sl.dsc));
    sl.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    sl.dsc.header.w  = LOGO_PX;
    sl.dsc.header.h  = LOGO_PX;
    sl.dsc.data_size = kPixBytes;
    sl.dsc.data      = (const uint8_t*)sl.pix;

    strlcpy(s_jobKey, s_reqKey, sizeof(s_jobKey));
    s_jobSlot  = slot;
    s_reqKey[0] = 0;
    s_fetching = true;
    if (xTaskCreatePinnedToCore(logoTask, "logo", AR_NET_TASK_STACK,
                                NULL, 1, NULL, 0) != pdPASS) {
      s_fetching = false;
      sl.state = LOGO_UNKNOWN;              // retry on a later request
      Serial.println("[logo] task spawn failed");
    }
  }
}
