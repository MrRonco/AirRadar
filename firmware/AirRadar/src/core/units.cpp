// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// units.cpp — see units.h for the one rule that matters.
#include <math.h>
#include "units.h"
#include "state.h"

// Statute miles, not nautical. "Standard vs metric" is a consumer-facing
// choice and a statute mile is what that phrase means to a reader who is not
// a pilot. It does sit slightly oddly beside knots — a target 100 mi out doing
// 500 kt does not divide cleanly, where 100 nm would — so if this display ever
// wants to be internally consistent with its own speed unit, 0.539957 is the
// only number that has to change.
static const float KM_TO_MI  = 0.621371f;
static const float KMH_TO_MPH = 0.621371f;

bool unitsImperial() { return g_set.units == AR_UNITS_IMPERIAL; }

float unitsTemp(float degC) {
  return unitsImperial() ? (degC * 9.0f / 5.0f + 32.0f) : degC;
}
const char* unitsTempSuffix() {
  return unitsImperial() ? "\xC2\xB0" "F" : "\xC2\xB0" "C";
}

float unitsSpeed(float kmh) {
  return unitsImperial() ? (kmh * KMH_TO_MPH) : kmh;
}
const char* unitsSpeedLabel() { return unitsImperial() ? "mph" : "km/h"; }

float unitsDist(float km) { return unitsImperial() ? (km * KM_TO_MI) : km; }
int   unitsDistI(float km) { return (int)lroundf(unitsDist(km)); }
const char* unitsDistLabel()      { return unitsImperial() ? "mi" : "km"; }
const char* unitsDistLabelUpper() { return unitsImperial() ? "MI" : "KM"; }

const char* unitsName() { return unitsImperial() ? "Imperial" : "Metric"; }
