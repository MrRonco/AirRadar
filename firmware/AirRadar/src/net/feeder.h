// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// feeder.h — aircraft data acquisition (local feeder first, cloud fallback)
// Spawns short-lived tasks on core 0; results land in g_pending* (state.h).
#pragma once
#include "../core/state.h"

// Call from loop(): kicks a fetch task when due (local vs cloud cadence),
// respecting g_fetchInProgress. Also schedules the stats.json poll piggyback.
void feederLoop(uint32_t nowMs);

// Force an immediate fetch on next loop pass (after settings changes).
void feederKick();

// Derive the short display name for the local source from g_set.feedUrl host
// (an IPv4 literal or *.local host -> "LOCAL"; a named host -> first DNS label
// uppercased, e.g. "myfeeder.lan" -> "MYFEEDER").
// Writes into g_localSrcName. Call at boot and when feedUrl changes.
void feederUpdateSrcName();
