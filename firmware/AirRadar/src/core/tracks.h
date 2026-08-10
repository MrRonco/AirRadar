// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// tracks.h — track lifecycle: apply, dead-reckon, order, select
// All functions here run in loop context only.
#pragma once
#include "state.h"

// Drain pending buffer into g_tracks. Returns true if new data was applied.
// Also updates g_heardCount / g_feedIsLocal / g_lastGoodApply and resets a
// track's route cache when its callsign changes.
bool tracksApplyPending();

// Advance positions by dtSec using gs/track (between polls); drop tracks older
// than AR_DROP_TRACK_MS (clearing selection if it was the selected one).
void tracksDeadReckon(float dtSec);

// Rebuild g_orderIdx/g_orderN: in-range tracks passing filters, sorted by
// distance ascending.
void tracksRebuildOrder();

// Cycle selection through the current order (+1/-1). Empty order clears it.
void tracksSelectByOrder(int dir);

// Select nearest track to a scope-pixel tap (within grab radius); clears
// selection if none close. Returns true if selection changed.
bool tracksSelectAtPixel(int px, int py);

Track* tracksFindByHex(const char* hex);
Track* tracksSelected();                  // nullptr if none
Track* tracksNearest();                   // first of order, nullptr if none
Track* tracksFirstEmergency();            // in-range emergency or nullptr
// In-range tracks older than AR_STALE_TRACK_MS -- i.e. being dead-reckoned
// rather than reported. ONE definition: the Overview computed it inline and
// the web console had no notion of it at all, which is why the console still
// showed the pre-fix "heard" number the panel had already stopped trusting.
int    tracksCoastingCount(uint32_t nowMs);
