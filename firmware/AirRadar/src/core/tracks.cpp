// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// tracks.cpp — track lifecycle: apply pending, dead-reckon, ordering, selection.
// Loop-context only (see state.h threading contract): the sole lock taken here
// is g_dataMux while draining the pending buffer, exactly like the proven v6
// applyPending(). No LVGL, no HTTP in this file.
// Logic ported 1:1 from the v6 reference (AirRadar.ino) unless noted "v7:".
#include <math.h>
#include "tracks.h"

// ---------- file-local constants ----------
static const float  KT_TO_KMH         = 1.852f;   // knots -> km/h
static const float  SEC_PER_HOUR      = 3600.0f;
static const double KM_PER_DEG_LAT    = 111.32;   // km per degree of latitude
static const float  TAP_GRAB_RADIUS_PX = 26.0f;   // scope tap pick radius
static const float  MIN_DR_SPEED_KT   = 1.0f;     // below this, hold position

static inline double d2r(double d) { return d * M_PI / 180.0; }

// Drain snapshot for the pending planes. File-scope static on purpose:
// ~6 KB would strain the loop-task stack, and internal RAM (not PSRAM) keeps
// the memcpy window inside the critical section as short as possible.
static ApiPlane s_drained[AR_MAX_TRACKS];

// ============================================================
//  Apply pending (net task -> live tracks)
// ============================================================
static void mergePlane(const ApiPlane& p, Track& t, uint32_t now) {
  // callsign changed -> cached adsbdb route is stale, allow a fresh lookup
  if (p.flight[0] && strcmp(t.flight, p.flight)) {
    t.routeTried = false;
    t.origin[0] = 0;
    t.dest[0] = 0;
  }
  strlcpy(t.flight,   p.flight,   sizeof(t.flight));
  strlcpy(t.typeCode, p.typeCode, sizeof(t.typeCode));
  strlcpy(t.category, p.category, sizeof(t.category));
  strlcpy(t.squawk,   p.squawk,   sizeof(t.squawk));
  strlcpy(t.reg,      p.reg,      sizeof(t.reg));
  // enrichment fields: keep last-known value when this poll returns blank
  if (p.desc[0])  strlcpy(t.desc,  p.desc,  sizeof(t.desc));
  if (p.ownOp[0]) strlcpy(t.ownOp, p.ownOp, sizeof(t.ownOp));
  if (p.year[0])  strlcpy(t.year,  p.year,  sizeof(t.year));
  t.mil = t.mil || p.mil;                 // sticky: once military, always military
  t.navAltFt  = p.navAltFt;
  t.lat       = p.lat;
  t.lon       = p.lon;
  t.gsKt      = p.gsKt;
  t.trackDeg  = p.trackDeg;
  t.altFt     = p.altFt;
  t.vRateFpm  = p.vRateFpm;
  t.lastApiMs = now;
}

bool tracksApplyPending() {
  int  n = 0, heard = 0;
  bool ok = false, local = true, ready = false;
  portENTER_CRITICAL(&g_dataMux);
  ready = g_pendingReady;
  if (ready) {
    ok    = g_pendingOk;
    n     = g_pendingCount;
    heard = g_pendingHeard;
    local = g_pendingLocal;
    if (n < 0) n = 0;                       // never trust the producer blindly
    if (n > AR_MAX_TRACKS) n = AR_MAX_TRACKS;
    if (ok && n > 0) memcpy(s_drained, g_pendingPlanes, sizeof(ApiPlane) * (size_t)n);
    g_pendingReady = false;
  }
  portEXIT_CRITICAL(&g_dataMux);
  if (!ready || !ok) return false;

  g_feedIsLocal = local;
  g_heardCount  = heard;
  if (!local) g_feedMsgRate = -1.0f;        // msg-rate readout is local-feeder only

  const uint32_t now = millis();
  int dropped = 0;
  for (int i = 0; i < n; i++) {
    const ApiPlane& p = s_drained[i];
    if (!p.hex[0]) continue;                // identity key required
    int slot = -1, freeSlot = -1;
    for (int t = 0; t < AR_MAX_TRACKS; t++) {
      if (g_tracks[t].valid && !strcmp(g_tracks[t].hex, p.hex)) { slot = t; break; }
      if (!g_tracks[t].valid && freeSlot < 0) freeSlot = t;
    }
    if (slot < 0) {
      if (freeSlot < 0) { dropped++; continue; }        // table full
      slot = freeSlot;
      memset(&g_tracks[slot], 0, sizeof(Track));
      g_tracks[slot].valid = true;
      strlcpy(g_tracks[slot].hex, p.hex, sizeof(g_tracks[slot].hex));
    }
    mergePlane(p, g_tracks[slot], now);
  }
  if (dropped > 0)
    Serial.printf("[trk] table full - dropped %d of %d planes\n", dropped, n);
  g_lastGoodApply = now;
  return true;
}

// ============================================================
//  Dead reckoning + drop
// ============================================================
void tracksDeadReckon(float dtSec) {
  const uint32_t now = millis();
  for (int i = 0; i < AR_MAX_TRACKS; i++) {
    Track& t = g_tracks[i];
    if (!t.valid) continue;
    if (now - t.lastApiMs > AR_DROP_TRACK_MS) {
      if (g_selHex[0] && !strcmp(g_selHex, t.hex)) g_selHex[0] = 0;
      t.valid = false;
      continue;
    }
    if (t.gsKt < MIN_DR_SPEED_KT) continue;
    float dKm = t.gsKt * KT_TO_KMH / SEC_PER_HOUR * dtSec;
    float rad = (float)d2r(t.trackDeg);
    t.lat += (dKm * cosf(rad)) / KM_PER_DEG_LAT;
    t.lon += (dKm * sinf(rad)) / (KM_PER_DEG_LAT * cos(d2r(t.lat)));
  }
}

// ============================================================
//  Distance order (in-range, filter-passing, ascending)
// ============================================================
void tracksRebuildOrder() {
  float dist[AR_MAX_TRACKS];
  g_orderN = 0;
  for (int i = 0; i < AR_MAX_TRACKS; i++) {
    const Track& t = g_tracks[i];
    if (!t.valid) continue;
    if (!trackPassesFilters(t)) continue;   // v7: user class/altitude filters
    dist[i] = haversineKm(g_set.homeLat, g_set.homeLon, t.lat, t.lon);
    if (dist[i] > (float)g_set.rangeKm) continue;
    int j = g_orderN;                       // insertion sort by distance
    while (j > 0 && dist[g_orderIdx[j - 1]] > dist[i]) {
      g_orderIdx[j] = g_orderIdx[j - 1];
      j--;
    }
    g_orderIdx[j] = i;
    g_orderN++;
  }
}

// ============================================================
//  Selection
// ============================================================
void tracksSelectByOrder(int dir) {
  tracksRebuildOrder();
  if (!g_orderN) { g_selHex[0] = 0; return; }
  int cur = -1;
  for (int i = 0; i < g_orderN; i++)
    if (!strcmp(g_tracks[g_orderIdx[i]].hex, g_selHex)) { cur = i; break; }
  cur = (cur < 0) ? (dir > 0 ? 0 : g_orderN - 1) : (cur + dir + g_orderN) % g_orderN;
  strlcpy(g_selHex, g_tracks[g_orderIdx[cur]].hex, sizeof(g_selHex));
}

bool tracksSelectAtPixel(int px, int py) {
  char prev[sizeof(g_selHex)];
  strlcpy(prev, g_selHex, sizeof(prev));
  float bestD2 = TAP_GRAB_RADIUS_PX * TAP_GRAB_RADIUS_PX;
  int   best   = -1;
  for (int i = 0; i < AR_MAX_TRACKS; i++) {
    const Track& t = g_tracks[i];
    if (!t.valid) continue;
    if (!trackPassesFilters(t)) continue;   // only what the scope draws is tappable
    float sx, sy;
    if (!scopeToScreen(t.lat, t.lon, sx, sy)) continue;   // outside range ring
    float dx = sx - (float)px, dy = sy - (float)py;
    float d2 = dx * dx + dy * dy;
    if (d2 < bestD2) { bestD2 = d2; best = i; }
  }
  if (best >= 0) strlcpy(g_selHex, g_tracks[best].hex, sizeof(g_selHex));
  else           g_selHex[0] = 0;
  return strcmp(prev, g_selHex) != 0;
}

// ============================================================
//  Lookups
// ============================================================
Track* tracksFindByHex(const char* hex) {
  if (!hex || !hex[0]) return nullptr;
  for (int i = 0; i < AR_MAX_TRACKS; i++)
    if (g_tracks[i].valid && !strcmp(g_tracks[i].hex, hex)) return &g_tracks[i];
  return nullptr;
}

Track* tracksSelected() {
  return tracksFindByHex(g_selHex);
}

Track* tracksNearest() {
  return (g_orderN > 0) ? &g_tracks[g_orderIdx[0]] : nullptr;
}

Track* tracksFirstEmergency() {
  // Nearest visible emergency first (matches v6 overview-line behaviour) ...
  for (int i = 0; i < g_orderN; i++) {
    Track& t = g_tracks[g_orderIdx[i]];
    if (sqIsEmergency(t.squawk)) return &t;
  }
  // ... but never let user filters mask a 7500/7600/7700 that is in range.
  for (int i = 0; i < AR_MAX_TRACKS; i++) {
    Track& t = g_tracks[i];
    if (!t.valid || !sqIsEmergency(t.squawk)) continue;
    if (haversineKm(g_set.homeLat, g_set.homeLon, t.lat, t.lon) >
        (float)g_set.rangeKm) continue;
    return &t;
  }
  return nullptr;
}
