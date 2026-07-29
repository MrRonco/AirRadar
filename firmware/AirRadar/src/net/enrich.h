// enrich.h — keyless enrichment lookups: weather (Open-Meteo), ISS
// (wheretheiss.at), route (adsbdb). Tasks on core 0; results into g_wx /
// g_iss (small structs written under g_dataMux) and g_routeRes* slot.
#pragma once
#include "../core/state.h"

// Call from loop(): schedules weather + ISS polls per config cadence and
// honours g_set.wxEn / g_set.issEn.
void enrichLoop(uint32_t nowMs);

// Request an adsbdb route lookup for the given hex/callsign (deduped: no-op
// while a lookup is in flight). Result appears in g_routeRes* with
// g_routeResReady=true; loop applies it via enrichApplyRoute().
void enrichRequestRoute(const char* hex, const char* flight);

// Background walk: one adsbdb lookup per AR_POLL_ROUTE_MS for a visible
// aircraft that has not been tried yet, nearest first. Without it only the
// SELECTED aircraft ever gets a route. Loop context.
void enrichRouteWalk(uint32_t nowMs);

// Persistent route cache (PSRAM table mirrored to FATFS). Routes are static
// per callsign, so caching them makes origin/destination survive both a reboot
// and the TLS gate closing under heap pressure.
void enrichRouteCacheBegin();
void enrichRouteCacheFlush(uint32_t nowMs);

// Loop-context: if a route result is ready, write it into the matching track
// (marking routeTried) and return true so the UI can refresh.
bool enrichApplyRoute();

// Force an immediate weather refetch (after location change).
void enrichKickWeather();
