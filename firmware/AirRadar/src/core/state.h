// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// state.h — AirRadar v7 shared application state + threading contract
//
// THREADING MODEL (do not violate):
//   * loop() runs on core 1. ALL lv_* calls, all Track mutation, all NVS writes
//     happen there and only there.
//   * Network work runs in short-lived tasks on core 0. Those tasks may ONLY
//     write into the g_pending* structures below, guarded by g_dataMux, and
//     set volatile ready flags. They never touch tracks[], never call lv_*.
//   * applyPending() (loop context) drains pending into the live state.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "types.h"

// ---------- live state (loop-context only) ----------
extern Track    g_tracks[AR_MAX_TRACKS];
extern char     g_selHex[8];              // "" = nothing selected
extern int      g_orderIdx[AR_MAX_TRACKS];// distance-sorted indexes of in-range tracks
extern int      g_orderN;
extern int      g_heardCount;             // aircraft with positions in last poll
extern bool     g_feedIsLocal;            // current source
extern char     g_localSrcName[AR_LOCAL_SRC_NAME_MAX]; // shown on Overview ("LOCAL" or host)
extern uint32_t g_lastGoodApply;          // millis of last successful apply
extern float    g_feedMsgRate;            // msg/s from stats.json, <0 = unknown
extern WeatherState g_wx;
extern IssState     g_iss;

// ---------- settings (loaded from NVS at boot; loop-context writes) ----------
struct Settings {
  double  homeLat, homeLon;
  int     rangeKm;
  bool    showLabels;
  String  wifiSsid, wifiPass;
  String  feedUrl;
  String  tz;
  String  panelPass;          // "" = no auth
  bool    netStatic; String netIp, netGw, netMask, netDns;
  bool    mqttEn;   String mqttUri;
  bool    nightEn;  int nightFromMin, nightToMin;
  bool    wxEn, issEn, logoEn, mapEn;
  uint8_t filtCls;            // FCLS_* bitmask
  int     filtAltLo, filtAltHi;   // ft, 0 = off
  String  watchlist;          // comma-separated prefixes
  // favourites
  double  favLat[AR_MAX_FAVS], favLon[AR_MAX_FAVS];
  String  favName[AR_MAX_FAVS];
  bool    favUsed[AR_MAX_FAVS];
};
extern Settings g_set;
extern Preferences g_prefs;

void settingsLoad();                       // NVS -> g_set (call once at boot)
void settingsSaveLocation();               // lat/lon/range
void settingsSaveDisplay();                // labels/night/wx/iss/logo/map
void settingsSaveFilters();                // cls/alt/watchlist
void settingsSaveNetworkExtras();          // mqtt/panelPass/tz  (wifi & static IP have
                                           //  their own save paths in web/settings UI)
void settingsSaveFavs();

// ---------- pending buffers (net task -> loop handoff) ----------
extern portMUX_TYPE g_dataMux;
extern ApiPlane g_pendingPlanes[AR_MAX_TRACKS];
extern int      g_pendingCount;
extern int      g_pendingHeard;
extern bool     g_pendingLocal;
extern volatile bool g_pendingReady;       // set by net task, cleared by applyPending
extern volatile bool g_pendingOk;

// route lookup result (single slot)
extern char g_routeResHex[8];
extern char g_routeResOrigin[5], g_routeResDest[5];
// adsbdb also knows the operator. Feeder databases are FAA/TC-centric, so
// foreign-registered aircraft (Etihad, Royal Jordanian, ...) arrive with an
// empty ownOp; this fills that gap. "" when adsbdb has no airline either.
extern char g_routeResAirline[28];
// Definitive answer (HTTP 200 parsed, or a 404 "unknown callsign") vs a
// transient miss (timeout / TLS busy / parse error). Only a definitive answer
// may set Track::routeTried — otherwise one network hiccup blanks a flight's
// route until its callsign changes.
extern volatile bool g_routeResFinal;
extern volatile bool g_routeResReady;

// ---------- runtime flags ----------
extern volatile bool g_fetchInProgress;    // aircraft fetch task alive
extern volatile bool g_routeFetching;
extern Screen  g_screen;
extern bool    g_wifiUp;                   // WiFi connected (loop-maintained)
extern bool    g_timeSynced;               // NTP has produced a sane time

// ---------- TLS gate ----------
// mbedTLS needs ~50 KB internal heap PER handshake; two concurrent secure
// fetches exhaust the ESP32-S3's internal RAM (field-verified: selecting an
// aircraft fired logo+route+cloud TLS together and starved the feed).
// Every task that opens a WiFiClientSecure MUST tlsTryAcquire() first and
// tlsRelease() when its HTTP client is fully torn down; on failure, skip and
// retry later. Atomic under g_dataMux — safe from any context.
// `essential`: the aircraft feed passes true and is exempt from the heap
// floor; optional fetches (logos/routes/weather/map) are denied whenever free
// internal heap sits below AR_TLS_HEAP_FLOOR, so a slow leak degrades
// eye-candy first and the feed last.
bool tlsTryAcquire(bool essential = false);
void tlsRelease();
// Loop-context advisory: would an optional tlsTryAcquire() succeed right now?
// Spawning a 12 KB-stack task only for it to discover the gate is shut burns
// the very internal RAM the gate is protecting — check this BEFORE spawning.
bool tlsGateOpen();
// How many optional TLS fetches have been refused for want of internal RAM.
// Without this, "this airline has no route" and "we are out of heap" look
// identical from the outside. Exported by /metrics.
extern uint32_t g_tlsShedCount;
// Successful TLS acquisitions. heap_free delta / this delta = bytes leaked per
// secure session — the number that tells you whether a leak is TLS-driven.
extern uint32_t g_tlsConnCount;
// Cumulative internal-heap change across each subsystem's network tasks, with
// a run count, so /metrics gives bytes-per-fetch directly.
extern int32_t  g_heapDeltaFeeder;
extern int32_t  g_heapNetFeeder;   // true net loss per complete poll cycle
extern uint32_t g_heapNetSamples; extern uint32_t g_feederRuns;
extern int32_t  g_heapDeltaIss;    extern uint32_t g_issRuns;

// ---------- misc helpers (implemented in state.cpp) ----------
float haversineKm(double la1, double lo1, double la2, double lo2);
float bearingTo(double la1, double lo1, double la2, double lo2);
// Great-circle position -> scope pixel. Returns false if outside range ring.
bool  scopeToScreen(double lat, double lon, float& sx, float& sy);
uint8_t trackClass(const Track& t);        // FCLS_* single bit for a track
bool  trackPassesFilters(const Track& t);  // class + altitude filters
bool  trackOnWatchlist(const Track& t);    // prefix match reg/callsign
void  altColorRGB(int altFt, uint8_t& r, uint8_t& g, uint8_t& b); // amber/cyan/violet/red
