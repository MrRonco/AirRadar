// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// timezones.h -- a curated POSIX TZ list, so nobody has to type
// "EST5EDT,M3.2.0,M11.1.0" from memory.
//
// Why a table and not the IANA database: newlib on ESP32 has no tzdata. It
// takes a POSIX TZ string and applies the DST rule encoded in it literally,
// which is exactly what these strings are. The cost is that a government
// changing its DST dates needs a firmware update -- accepted, because the
// alternative is ~450 KB of tzdata in a 3 MB app partition for a clock.
//
// The rules below are the 2007-onward US/Canada convention (Mar 2nd Sun ->
// Nov 1st Sun), the EU convention (Mar last Sun -> Oct last Sun) and the
// southern-hemisphere equivalents. Zones that do not observe DST carry no
// rule at all, which is why Arizona and Saskatchewan are separate entries
// rather than aliases of Mountain and Central.
//
// Ordering is west-to-east within a region, regions roughly by where this
// project's users are. A "Custom" escape hatch lives in the UI, not here, so
// a hand-written POSIX string is never lost by a firmware update.
//
// NAMES ARE LIMITED TO font_body18's SUBSET: 0x20-0x7E plus 0xB0 and 0xB7.
// The separator is a MIDDLE DOT (0xB7), not an em dash -- an em dash is
// U+2014, outside the range, and LVGL renders it as a tofu box on the panel
// while looking perfectly fine in the web UI. Anything added here has to be
// checked against the font, not against a browser.
#pragma once

struct TzEntry {
  const char* name;    // what a person recognises
  const char* posix;   // what newlib needs
};

static const TzEntry kTimezones[] = {
  // --- North America ---
  {"Hawaii",                          "HST10"},
  {"Alaska",                          "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Pacific · Vancouver, Los Angeles","PST8PDT,M3.2.0,M11.1.0"},
  {"Arizona (no DST)",                "MST7"},
  {"Mountain · Calgary, Denver",      "MST7MDT,M3.2.0,M11.1.0"},
  {"Saskatchewan (no DST)",           "CST6"},
  {"Central · Winnipeg, Chicago",     "CST6CDT,M3.2.0,M11.1.0"},
  {"Eastern · Toronto, New York",     "EST5EDT,M3.2.0,M11.1.0"},
  {"Atlantic · Halifax",              "AST4ADT,M3.2.0,M11.1.0"},
  {"Newfoundland · St. John's",       "NST3:30NDT,M3.2.0,M11.1.0"},
  // --- South America ---
  {"Sao Paulo (no DST)",              "BRT3"},
  {"Buenos Aires",                    "ART3"},
  // --- Europe / Africa ---
  {"UTC",                             "UTC0"},
  {"UK / Ireland / Portugal",         "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Central Europe · Paris, Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Eastern Europe · Athens, Kyiv",   "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Johannesburg",                    "SAST-2"},
  {"Moscow",                          "MSK-3"},
  // --- Asia / Pacific ---
  {"Dubai",                           "GST-4"},
  {"India",                           "IST-5:30"},
  {"Bangkok / Jakarta",               "WIB-7"},
  {"China / Singapore / Hong Kong",   "CST-8"},
  {"Japan / Korea",                   "JST-9"},
  {"Brisbane (no DST)",               "AEST-10"},
  {"Sydney / Melbourne",              "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Auckland",                        "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
static const int kTimezoneCount = (int)(sizeof(kTimezones) / sizeof(kTimezones[0]));

// Index of the entry whose POSIX string matches, or -1 for a custom value.
// Callers use -1 to decide whether to show the raw string to the user.
inline int tzIndexOf(const char* posix) {
  if (!posix) return -1;
  for (int i = 0; i < kTimezoneCount; i++) {
    const char* a = kTimezones[i].posix;
    const char* b = posix;
    while (*a && *a == *b) { a++; b++; }
    if (!*a && !*b) return i;
  }
  return -1;
}

// The friendly name for a stored value, or the raw string when it is custom.
inline const char* tzNameOf(const char* posix) {
  const int i = tzIndexOf(posix);
  return (i >= 0) ? kTimezones[i].name : (posix && *posix ? posix : "unset");
}
