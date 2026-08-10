// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// units.h — metric / imperial display conversion.
//
// ONE rule, and it is the same rule the Fahrenheit toggle already followed:
// this is a DISPLAY layer and nothing else. Everything stored, cached,
// filtered, published or transmitted stays canonical — g_wx.tempC is Celsius,
// g_set.rangeKm is kilometres, /api/state and MQTT are metric, and the map
// cache path is still /mp/r<km>. Convert at the point of drawing, never
// before, or the range steps stop matching the cache keys and the API starts
// lying to Home Assistant.
//
// WHAT DOES NOT CONVERT: altitude (ft), ground speed (kt) and vertical rate
// (fpm). Those are ICAO standard the world over — they arrive in those units
// on the ADS-B wire, a pilot in a metric country still flies FL350 at 480 kt,
// and converting them would put this panel out of step with every other
// aviation source a reader might check it against. Only the readings a
// non-pilot expects in local units move.
#pragma once
#include <stdint.h>

// g_set.units values. Stored as a uint8 so a third system (nautical) can be
// added without another migration.
#define AR_UNITS_METRIC    0
#define AR_UNITS_IMPERIAL  1

bool unitsImperial();

// Temperature. Canonical input is always Celsius.
float       unitsTemp(float degC);
const char* unitsTempSuffix();       // "\xC2\xB0" "C" / "\xC2\xB0" "F"

// Wind and any other ground speed. Canonical input is always km/h.
float       unitsSpeed(float kmh);
const char* unitsSpeedLabel();       // "km/h" / "mph"

// Distance. Canonical input is always kilometres.
float       unitsDist(float km);
int         unitsDistI(float km);    // rounded, for range steps and ring labels
const char* unitsDistLabel();        // "km" / "mi"
const char* unitsDistLabelUpper();   // "KM" / "MI"

// For settings and the web UI.
const char* unitsName();             // "Metric" / "Imperial"
