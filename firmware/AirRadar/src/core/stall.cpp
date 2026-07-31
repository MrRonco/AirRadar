// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// stall.cpp — per-stage loop timing, to identify what causes visible display
// glitches instead of guessing.
//
// WHY. The panel DMA scans the PSRAM framebuffer autonomously at ~25 MB/s, so a
// visible glitch is not "the UI stopped" — it is something else saturating the
// PSRAM bus or holding the CPU long enough that LVGL's flush collides with the
// scan. Every candidate runs in loop context (webLoop, mapLoop, logosLoop,
// lv_timer_handler...), so timing each stage separately names the culprit
// directly rather than establishing only that *something* was slow.
//
// Two prior hypotheses for this glitch were wrong — the observer's own polling,
// then TLS handshake load — both plausible, neither measured. This is the
// instrument that settles it.
//
// Cost is a millis() pair per stage per loop and a 32-entry ring in PSRAM.
// Nothing is recorded below AR_STALL_MS, so a healthy device writes nothing.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "stall.h"

static const int kRing = 32;

struct StallRec {
  uint32_t uptimeS;
  uint32_t ms;
  uint32_t px;          // pixels flushed during the stall (lvgl only)
  uint32_t n;           // number of flush calls
  int16_t  x1, y1, x2, y2;   // bounding box of everything repainted
  uint8_t  stage;
  uint8_t  busy;        // what was in flight, for correlation
};

static StallRec* s_ring = nullptr;
static int       s_head = 0;
static uint32_t  s_total = 0;
static uint32_t  s_maxMs[ST_N]   = {0};
static uint32_t  s_count[ST_N]   = {0};

static const char* kStageName[ST_N] = {
  "lvgl", "web", "feeder", "enrich", "map", "mqtt", "logos", "tracks", "uitick",
};

void stallBegin() {
  if (s_ring) return;
  s_ring = (StallRec*)heap_caps_calloc(kRing, sizeof(StallRec), MALLOC_CAP_SPIRAM);
}

void stallNote(uint8_t stage, uint32_t ms, uint8_t busy, uint32_t px,
               uint32_t n, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
  if (stage >= ST_N || ms < AR_STALL_MS) return;
  s_count[stage]++;
  if (ms > s_maxMs[stage]) s_maxMs[stage] = ms;
  s_total++;
  if (!s_ring) return;
  s_ring[s_head].uptimeS = millis() / 1000;
  s_ring[s_head].ms      = ms;
  s_ring[s_head].stage   = stage;
  s_ring[s_head].busy    = busy;
  s_ring[s_head].px      = px;
  s_ring[s_head].n       = n;
  s_ring[s_head].x1 = x1; s_ring[s_head].y1 = y1;
  s_ring[s_head].x2 = x2; s_ring[s_head].y2 = y2;
  s_head = (s_head + 1) % kRing;
}

String stallReport() {
  String r;
  r.reserve(2048);
  char l[128];
  snprintf(l, sizeof(l), "stalls over %u ms: %lu total, uptime %lus\n\n",
           (unsigned)AR_STALL_MS, (unsigned long)s_total,
           (unsigned long)(millis() / 1000));
  r += l;

  r += F("per stage        count      max ms\n");
  for (int i = 0; i < ST_N; i++) {
    if (!s_count[i]) continue;
    snprintf(l, sizeof(l), "  %-12s %8lu %10lu\n", kStageName[i],
             (unsigned long)s_count[i], (unsigned long)s_maxMs[i]);
    r += l;
  }

  if (!s_ring) return r + F("\n(ring not allocated)\n");
  r += F("\nlast events (newest first)   busy: F=feeder R=route W=wx I=iss L=logo M=map\n");
  for (int n = 0; n < kRing; n++) {
    int i = (s_head - 1 - n + kRing * 2) % kRing;
    if (!s_ring[i].ms) continue;
    char b[8];
    int k = 0;
    uint8_t f = s_ring[i].busy;
    if (f & BUSY_FEEDER) b[k++] = 'F';
    if (f & BUSY_ROUTE)  b[k++] = 'R';
    if (f & BUSY_WX)     b[k++] = 'W';
    if (f & BUSY_ISS)    b[k++] = 'I';
    if (f & BUSY_LOGO)   b[k++] = 'L';
    if (f & BUSY_MAP)    b[k++] = 'M';
    b[k] = '\0';
    snprintf(l, sizeof(l),
             "  t=%-6lus %-7s %4lums %7lupx(%2lu%%) n=%-3lu bbox %d,%d-%d,%d %s\n",
             (unsigned long)s_ring[i].uptimeS, kStageName[s_ring[i].stage],
             (unsigned long)s_ring[i].ms, (unsigned long)s_ring[i].px,
             (unsigned long)(s_ring[i].px / 3840), (unsigned long)s_ring[i].n,
             s_ring[i].x1, s_ring[i].y1, s_ring[i].x2, s_ring[i].y2, k ? b : "-");
    r += l;
  }
  return r;
}
