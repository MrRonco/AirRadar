// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// scenarios.cpp — the fake world.
//
// This is the reason the harness exists. On the device, seeing a squawk 7600 or
// a -40 degree reading means waiting for one to happen or flashing firmware
// that hardcodes it — and it was exactly that kind of probe build that
// boot-looped the panel. Here a scenario is a keypress.
//
// Every scenario is a WORST CASE or an EDGE, not a pretty picture. Layout bugs
// live at the extremes: "-3072" fpm is 5 tabular digits, "-40" plus a unit is
// the widest the weather row ever gets, and an empty sky is the state most
// screens are never tested in.
#include <cstring>
#include <cmath>
#include "Arduino.h"
#include "scenarios.h"
#include "../firmware/AirRadar/src/core/state.h"
#include "../firmware/AirRadar/src/core/tracks.h"

static void clearTracks() {
  for (int i = 0; i < AR_MAX_TRACKS; i++) g_tracks[i].valid = false;
  g_selHex[0] = 0;
  g_orderN = 0;
  g_heardCount = 0;
  g_inRangeTotal = 0;
}

// Place a track at a bearing/distance from home so it lands where you expect.
static Track* put(int slot, const char* hex, const char* flight, const char* op,
                  const char* type, const char* reg, int altFt, float gs,
                  float trk, int vs, float bearingDeg, float km,
                  const char* squawk = "1200",
                  const char* origin = "", const char* dest = "") {
  Track& t = g_tracks[slot];
  memset(&t, 0, sizeof(t));
  t.valid = true;
  strlcpy(t.hex, hex, sizeof(t.hex));
  strlcpy(t.flight, flight, sizeof(t.flight));
  strlcpy(t.ownOp, op, sizeof(t.ownOp));
  strlcpy(t.typeCode, type, sizeof(t.typeCode));
  strlcpy(t.reg, reg, sizeof(t.reg));
  strlcpy(t.squawk, squawk, sizeof(t.squawk));
  strlcpy(t.year, "2019", sizeof(t.year));
  strlcpy(t.origin, origin, sizeof(t.origin));
  strlcpy(t.dest,   dest,   sizeof(t.dest));
  t.routeTried = true;
  t.altFt = altFt; t.gsKt = gs; t.trackDeg = trk; t.vRateFpm = vs;
  t.navAltFt = -1;
  t.lastApiMs = millis();
  const double R = 6371.0, br = bearingDeg * M_PI / 180.0, d = km / R;
  const double la1 = g_set.homeLat * M_PI / 180.0, lo1 = g_set.homeLon * M_PI / 180.0;
  const double la2 = asin(sin(la1) * cos(d) + cos(la1) * sin(d) * cos(br));
  const double lo2 = lo1 + atan2(sin(br) * sin(d) * cos(la1), cos(d) - sin(la1) * sin(la2));
  t.lat = la2 * 180.0 / M_PI;
  t.lon = lo2 * 180.0 / M_PI;
  return &t;
}

const char* scenarioName(int n) {
  switch (n) {
    case SCN_TYPICAL:   return "typical traffic";
    case SCN_EMPTY:     return "empty sky";
    case SCN_EMERGENCY: return "squawk 7600 emergency";
    case SCN_EXTREMES:  return "layout extremes (-40 C, NW 120, -3072 fpm, FL450)";
    case SCN_CROWDED:   return "crowded (30 aircraft)";
    case SCN_COASTING:  return "all coasting / stale feed";
    case SCN_NOTIME:    return "no NTP yet";
    case SCN_LEFTRING:  return "pinned target has left the range ring";
    default:            return "?";
  }
}

void scenarioApply(int n) {
  clearTracks();
  // Sane baseline; individual scenarios override.
  g_wx.valid = true; g_wx.tempC = 21.0f; g_wx.windKmh = 12.0f;
  g_wx.windDirDeg = 315; g_wx.wmoCode = 0;
  g_iss.valid = false;
  g_feedIsLocal = true; g_feedMsgRate = 24.0f;
  g_lastGoodApply = millis();
  g_timeSynced = true;
  g_fakeNtpSynced = true;

  switch (n) {
    case SCN_EMPTY:
      g_heardCount = 0;
      break;

    case SCN_EMERGENCY:
      put(0, "c04a11", "ACA337", "AIR CANADA", "A320", "C-FCQX", 34000, 431, 273, 64,  70, 109, "7600", "YUL", "YEG");
      put(1, "a1b2c3", "UAL770", "UNITED AIRLINES INC", "B739", "N37502", 40000, 452, 190, 0, 200, 178);
      put(2, "c01234", "JZA434", "Jazz Aviation LP", "CRJ9", "C-GJZQ", 21000, 388, 95, -900, 320, 88);
      strlcpy(g_selHex, "c04a11", sizeof(g_selHex));
      g_heardCount = 3;
      break;

    case SCN_EXTREMES:
      // Every value at the width the layout has to survive.
      g_wx.tempC = -40.0f; g_wx.windKmh = 120.0f; g_wx.windDirDeg = 315; g_wx.wmoCode = 75;
      put(0, "abcdef", "SWR9999", "SWISS INTERNATIONAL AIR LINES LTD", "B77W",
          "HB-JNA", 45000, 999, 359, -3072, 45, 248, "7700", "ZRH", "YYZ");
      put(1, "000001", "", "", "", "", -1, 0, 0, 0, 180, 12);   // unknown everything
      strlcpy(g_selHex, "abcdef", sizeof(g_selHex));
      g_heardCount = 2;
      break;

    case SCN_CROWDED: {
      static const char* ops[6] = {"AIR CANADA", "WESTJET", "Porter Airlines",
                                   "UNITED AIRLINES INC", "Jazz Aviation LP", ""};
      // AR_MAX_TRACKS aircraft, not 30 -- the interesting case is the table
      // exactly full, because that is when the feeder starts discarding and
      // the CAPPED disclosure has to appear.
      for (int i = 0; i < AR_MAX_TRACKS; i++) {
        char hex[8], fl[12];
        snprintf(hex, sizeof(hex), "c0%04x", i);
        snprintf(fl, sizeof(fl), "%s%03d", (i % 3 == 0) ? "ACA" : (i % 3 == 1) ? "WJA" : "POE", i);
        put(i, hex, fl, ops[i % 6], "A320", "C-FABC",
            3000 + i * 1400, 260 + i * 6, (float)(i * 12 % 360),
            (i % 5 - 2) * 700, (float)(i * 12 % 360), 15.0f + i * 7.5f);
      }
      g_heardCount = 61;
      g_inRangeTotal = 57;      // 57 were in range; the table holds 40
      break;
    }

    case SCN_COASTING:
      put(0, "c04a11", "ACA184", "AIR CANADA", "A321", "C-GJWO", 33000, 472, 137, 0, 60, 147);
      put(1, "a1b2c3", "DAL298", "DELTA AIR LINES INC", "B738", "N3746H", 37000, 445, 250, 0, 240, 96);
      // Older than AR_STALE_TRACK_MS: both should render translucent and the
      // Overview should show a COASTING count.
      g_tracks[0].lastApiMs = millis() - 40000;
      g_tracks[1].lastApiMs = millis() - 52000;
      g_lastGoodApply = millis() - 45000;      // drives the STALE feed state
      g_feedIsLocal = false;                   // and the CLOUD fallback dot
      g_heardCount = 2;
      break;

    case SCN_LEFTRING:
      // The pinned aircraft is OUTSIDE the ring. tracksFindByHex() has no
      // range test but the scope loop does, so the card stays populated while
      // the disc shows no blip -- the two halves of the instrument disagreeing.
      put(0, "c04a11", "ACA184", "AIR CANADA", "A321", "C-GJWO", 33000, 472, 137, 0,
          60, 268, "1200", "YVR", "YYZ");                    // 268 km, ring is 250
      put(1, "a1b2c3", "DAL298", "DELTA AIR LINES INC", "B738", "N3746H",
          37000, 445, 250, -640, 240, 96, "1200", "ATL", "SEA");
      strlcpy(g_selHex, "c04a11", sizeof(g_selHex));         // pinned, out of range
      g_heardCount = 2;
      break;

    case SCN_NOTIME:
      g_fakeNtpSynced = false;      // reaches the real tm_year <= 120 branch
      g_timeSynced = false;
      g_wx.valid = false;
      g_heardCount = 0;
      break;

    case SCN_TYPICAL:
    default:
      put(0, "c04a11", "ACA184", "AIR CANADA", "A321", "C-GJWO", 33000, 472, 137, 0,   60, 147, "1200", "YVR", "YYZ");
      put(1, "a1b2c3", "DAL298", "DELTA AIR LINES INC", "B738", "N3746H", 37000, 445, 250, -640, 240, 96, "1200", "ATL", "SEA");
      put(2, "c01234", "JZA239", "Jazz Aviation LP", "CRJ9", "C-GJZQ", 21000, 388, 95, 1200, 320, 52);
      put(3, "4ca7b1", "EIN122", "Aer Lingus", "A333", "EI-EDY", 37000, 505, 275, 0,  15, 205);
      put(4, "c06f2a", "POE264", "Porter Airlines Inc.", "E195", "C-GKQN", 39000, 484, 110, -64, 285, 168);
      // Inbound: track 290 against a bearing of 130 is closing at cos(160) of
      // its ground speed, so this exercises the approach readout.
      put(5, "a99887", "N512BA", "", "C172", "N512BA", 4200, 104, 290, 300, 130, 23);
      strlcpy(g_selHex, "c04a11", sizeof(g_selHex));
      g_heardCount = 8;
      break;
  }
  tracksRebuildOrder();
}
