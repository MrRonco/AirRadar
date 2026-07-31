// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// logos.cpp — airline logo fetch/cache. See logos.h for the contract.
//
// Three tiers: RAM slots (fast) -> FATFS /lg/<ICAO> (persistent, 4.2 KB per
// rendered logo against a 9.9 MB partition) -> network fetch (TLS, gated).
// After the first encounter an airline's logo loads from flash in
// milliseconds, forever, with zero network. A gentle prefetcher walks the
// currently visible aircraft so the cache converges on the local traffic mix.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <FFat.h>
#include <ctype.h>
#include <string.h>
#include "logos.h"
#include "../core/tracks.h"

static const char     kUrlBase[]     = "https://raw.githubusercontent.com/theqkash/esp32flight-logos/main/logos/";
static const char     kFsDir[]       = "/lg";
// Must comfortably exceed the number of distinct airlines visible at once, or
// live entries get evicted and re-fetched. Only OK entries own a 4.2 KB PSRAM
// pixel buffer, so an unused slot costs ~40 bytes of .bss.
static const int      kSlots         = 24;                // LRU-ish ring
static const uint32_t kPrefetchMs    = 6000;              // visible-aircraft walk
static const size_t   kPngMax        = 96 * 1024;         // download cap
static const uint32_t kHttpTimeoutMs = 8000;
static const uint32_t kTlsShutRetryMs = 30000;            // gate shut: back right off
static const size_t   kPixBytes      = LOGO_PX * LOGO_PX * 2;

struct LogoSlot {
  char          key[4];       // ICAO prefix, "" = free
  LogoState     state;
  uint16_t*     pix;          // PSRAM RGB565, allocated on first use
  lv_img_dsc_t  dsc;
  uint32_t      lastUse;
  bool          needsSave;    // fetched, not yet persisted (see kFsWriteGapMs)
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

static volatile uint32_t s_retryAfterMs = 0;   // cooldown after transient failure
static bool     s_fsOk = false;                // FATFS cache mounted
static uint32_t s_lastPrefetchMs = 0;

// ---------- persistent tier (loop context only) ----------
void logosBegin() {
  // format_if_failed=true: first boot on a fresh FATFS partition formats it
  // (takes a few seconds, once ever).
  s_fsOk = FFat.begin(true);
  if (!s_fsOk) { Serial.println("[logo] FATFS mount failed - no persistent cache"); return; }
  if (!FFat.exists(kFsDir)) FFat.mkdir(kFsDir);
  Serial.printf("[logo] FATFS cache up (%u KB free)\n",
                (unsigned)(FFat.freeBytes() / 1024));
}

static void fsPathFor(const char* key, char* out, size_t cap) {
  snprintf(out, cap, "%s/%s", kFsDir, key);
}

// Try to fill a slot's pixels from flash. Loop context; ~ms for 4.2 KB.
static bool fsLoad(const char* key, uint16_t* pix) {
  if (!s_fsOk) return false;
  char path[16];
  fsPathFor(key, path, sizeof(path));
  File f = FFat.open(path, FILE_READ);
  if (!f) return false;
  bool ok = (f.size() == kPixBytes) &&
            (f.read((uint8_t*)pix, kPixBytes) == kPixBytes);
  f.close();
  if (!ok) FFat.remove(path);          // corrupt/short file: refetch later
  return ok;
}

// Flash writes starve the panel DMA. Flash and PSRAM share the MSPI bus, and
// CONFIG_SPI_FLASH_AUTO_SUSPEND is NOT enabled in the prebuilt arduino-esp32
// libraries (verified in the shipped sdkconfig), so a write cannot be suspended
// to let cache traffic through. /api/stalls measured 222 ms of blocked bus for
// one 2,592-byte logo, with zero pixels flushed -- the whole screen shakes and
// no repaint metric can see it, which is why this survived six hypotheses.
//
// Nothing here can make a write cheap, so the only lever is how OFTEN. Saves are
// marked on the slot and drained at most one per kFsWriteGapMs, turning a burst
// of new airlines into occasional isolated blips instead of a run of shakes.
// The rate decays naturally: an ICAO is written once, ever.
static const uint32_t kFsWriteGapMs = 45000;
static uint32_t s_lastFsWrite = 0;

// Persist freshly fetched pixels. Loop context (keeps FATFS single-threaded).
static void fsSave(const char* key, const uint16_t* pix) {
  if (!s_fsOk) return;
  char path[16];
  fsPathFor(key, path, sizeof(path));
  File f = FFat.open(path, FILE_WRITE);
  if (!f) { Serial.printf("[logo] fs write open failed %s\n", path); return; }
  size_t w = f.write((const uint8_t*)pix, kPixBytes);
  f.close();
  if (w != kPixBytes) { FFat.remove(path); Serial.printf("[logo] fs write short %s\n", path); }
  else Serial.printf("[logo] cached %s to flash\n", key);
}

bool logosIcaoFromFlight(const char* flight, char out[4]) {
  out[0] = 0;
  if (!flight) return false;
  if (!isalpha((unsigned char)flight[0]) || !isalpha((unsigned char)flight[1]) ||
      !isalpha((unsigned char)flight[2]) || !isdigit((unsigned char)flight[3]))
    return false;
  for (int i = 0; i < 3; i++) out[i] = (char)toupper((unsigned char)flight[i]);
  out[3] = 0;
  return true;
}

// ---------- fetch task (core 0) ----------
// Returns HTTP code (200/404/...) or negative on transport failure.
static int fetchPngCode(const char* key, size_t* outLen) {
  if (!s_png) {
    s_png = (uint8_t*)heap_caps_malloc(kPngMax, MALLOC_CAP_SPIRAM);
    if (!s_png) { Serial.println("[logo] png buf alloc failed"); return -1; }
  }
  char url[128];
  snprintf(url, sizeof(url), "%s%s.png", kUrlBase, key);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, url)) return -1;
  http.addHeader("User-Agent", AR_USER_AGENT);
  int code = http.GET();
  if (code != 200) {
    http.end();
    if (code != 404)                        // 404 = airline simply not in the pack
      Serial.printf("[logo] %s -> HTTP %d\n", key, code);
    return code;
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
  if (total < 100) return -2;               // truncated transfer: transient
  *outLen = total;
  return 200;
}

static void logoTask(void*) {
  // TLS gate: never contend with the aircraft feed / map / route fetches —
  // that contention starved mbedTLS and stalled the whole data pipeline.
  if (!tlsTryAcquire()) {
    s_resState = LOGO_UNKNOWN;              // not an answer: retry later
    s_retryAfterMs = millis() + 3000;
    s_resReady = true;
    s_fetching = false;
    vTaskDelete(NULL);
    return;
  }
  LogoState result = LOGO_MISS;
  int code = -1;
  size_t len = 0;
  if (WiFi.status() == WL_CONNECTED) code = fetchPngCode(s_jobKey, &len);
  tlsRelease();
  if (code == 200) {
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
  } else if (code != 404) {
    // Transient (timeout/TLS/heap): DON'T negative-cache — an American
    // Airlines logo shouldn't vanish forever because one fetch hiccuped.
    result = LOGO_UNKNOWN;
    s_retryAfterMs = millis() + 5000;
  }                                          // 404 stays LOGO_MISS (permanent)
  s_resState = result;
  s_resReady = true;                        // consumed by logosLoop
  s_fetching = false;
  vTaskDelete(NULL);
}

// ---------- public API (loop context) ----------
void logosRequest(const char* icao3) {
  if (!icao3 || strlen(icao3) != 3) return;
  if ((int32_t)(millis() - s_retryAfterMs) < 0) return;   // transient cooldown
  int i = slotFind(icao3);
  if (i >= 0 && s_slots[i].state != LOGO_UNKNOWN) return; // OK/MISS/PENDING: done
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
  // Land a finished fetch (and persist wins to flash — loop context keeps
  // FATFS access single-threaded).
  if (s_resReady) {
    s_resReady = false;
    if (s_jobSlot >= 0) {
      s_slots[s_jobSlot].state = s_resState;
      // Do NOT write here -- mark it, and let the gate below pick the moment.
      if (s_resState == LOGO_OK) s_slots[s_jobSlot].needsSave = true;
    }
    s_jobSlot = -1;
  }

  // Drain at most one pending save per gap, oldest-used first so a slot cannot
  // be evicted before it is ever persisted.
  if (s_fsOk && (uint32_t)(nowMs - s_lastFsWrite) >= kFsWriteGapMs) {
    int pick = -1;
    for (int i = 0; i < kSlots; i++)
      if (s_slots[i].needsSave && s_slots[i].key[0] && s_slots[i].pix &&
          (pick < 0 || s_slots[i].lastUse < s_slots[pick].lastUse))
        pick = i;
    if (pick >= 0) {
      fsSave(s_slots[pick].key, s_slots[pick].pix);
      s_slots[pick].needsSave = false;
      s_lastFsWrite = nowMs;
    }
  }

  // Prefetch: every kPrefetchMs, queue one visible airline whose logo we
  // don't have yet. With the flash tier this converges to "everything the
  // local sky ever shows loads instantly".
  if (!s_reqKey[0] && !s_fetching && g_wifiUp &&
      (int32_t)(nowMs - s_lastPrefetchMs) > (int32_t)kPrefetchMs) {
    s_lastPrefetchMs = nowMs;
    // Pass 1: mark every visible airline as recently used. lastUse was only
    // ever touched for the SELECTED aircraft, so prefetched entries were always
    // the LRU victim — and evicting a LOGO_MISS throws away the "this airline
    // has no logo" record, which means the next prefetch re-fetches it and
    // takes another 404 over TLS. That churn is pure leaked heap.
    for (int i = 0; i < g_orderN; i++) {
      char icao[4];
      if (!logosIcaoFromFlight(g_tracks[g_orderIdx[i]].flight, icao)) continue;
      int s = slotFind(icao);
      if (s >= 0) s_slots[s].lastUse = nowMs;
    }
    // Pass 2: request the first visible airline we still know nothing about.
    for (int i = 0; i < g_orderN; i++) {
      char icao[4];
      if (!logosIcaoFromFlight(g_tracks[g_orderIdx[i]].flight, icao)) continue;
      int s = slotFind(icao);
      if (s >= 0 && s_slots[s].state != LOGO_UNKNOWN) continue;
      logosRequest(icao);
      break;                            // one per interval — stay gentle
    }
  }
  // Launch the queued request. Reuse the key's existing slot (UNKNOWN retry)
  // so a retried airline never occupies two cache entries.
  if (s_reqKey[0] && !s_fetching && g_wifiUp) {
    int slot = slotFind(s_reqKey);
    if (slot < 0) slot = slotAlloc();
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

    // Persistent tier: flash hit means no task, no network, ~instant. This
    // still works with the TLS gate shut — which is why logos keep appearing
    // when routes and weather do not.
    if (fsLoad(sl.key, sl.pix)) {
      sl.state = LOGO_OK;
      s_reqKey[0] = 0;
      return;
    }

    // Network tier: check the gate HERE. logoTask used to spawn a 12 KB
    // internal stack, discover the gate was shut, and die — several times a
    // minute, burning the very heap the gate protects.
    if (!tlsGateOpen()) {
      sl.state = LOGO_UNKNOWN;                 // not an answer: retry later
      s_retryAfterMs = millis() + kTlsShutRetryMs;
      s_reqKey[0] = 0;
      return;
    }

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
