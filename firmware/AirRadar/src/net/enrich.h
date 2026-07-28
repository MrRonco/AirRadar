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

// Loop-context: if a route result is ready, write it into the matching track
// (marking routeTried) and return true so the UI can refresh.
bool enrichApplyRoute();

// Force an immediate weather refetch (after location change).
void enrichKickWeather();
