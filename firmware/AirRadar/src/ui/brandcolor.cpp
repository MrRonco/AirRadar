// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// brandcolor.cpp — ICAO operator code to brand colour.
//
// WHY THIS EXISTS. The operator tile shows a real airline logo when one is
// cached (g_set.logoEn, default on). This is what it falls back to, and that
// is the common case rather than the rare one: general aviation, cargo,
// private operators and any carrier missing from the logo pack have no logo at
// all. The old fallback was two initials on a coloured chip, which was
// ambiguous — AC is both Air Canada and Air China — and duplicated the
// operator name sitting directly beside it.
//
// The ICAO code in the airline's colour identifies the operator without
// reproducing any artwork: a colour is a fact, not a copyrightable work. That
// matters because every available logo pack traces back to a scraper with no
// permission at any link, and Canada has no fair-use doctrine — only the
// closed list of fair-dealing purposes in Copyright Act s.29, which does not
// include "identification in a gadget". Keeping the logos is a deliberate,
// informed choice by the owner; this at least means the DEFAULT path for an
// aircraft with no cached logo reproduces nothing.
//
// It is also better typography at this size. At 36x36 roughly 71% of airline
// logos are wordmarks wider than 2.5:1 — letterboxed to about 36x7 they are an
// illegible smear.
//
// The colour is drawn as TEXT on the known dark tile rather than as a tile
// background, which sidesteps the contrast problem entirely: white on Lufthansa
// yellow or Ryanair yellow is unreadable, and solving that per airline would
// mean a second luminance table.
#include <ctype.h>
#include <math.h>
#include <string.h>
#include "brandcolor.h"

// ---------- known carriers ----------
// Primary identity colour, adjusted where the true brand colour is too dark to
// read on C_SURF (#0d131d). A navy carrier becomes a bright cornflower: still
// recognisably "their blue", but legible. Sorted by ICAO for bsearch.
struct Brand { char icao[4]; uint32_t rgb; };

static const Brand kBrands[] = {
  // Verified against published brand guidelines; values lightened where the
  // true colour is too dark to read on C_SURF. Sorted for binary search.
  {"AAL", 0x9aa8b5}, {"ACA", 0xf01428}, {"AFR", 0x5b8fd6}, {"AIC", 0xda0e29},
  {"ALK", 0x1e93bc}, {"AMX", 0x7272d6}, {"ANA", 0x527ce3}, {"ANZ", 0xc8d2dc},
  {"ARG", 0x0484cc}, {"ASA", 0x4fb3a0}, {"ATN", 0x7fa3c4}, {"AUA", 0xe24a4f},
  {"AVA", 0xda291c}, {"AZA", 0xdf4f3b}, {"BAW", 0xd44a5c}, {"CCA", 0xe24a50},
  {"CES", 0x547be3}, {"CKS", 0x9fb4c6}, {"CMP", 0x2a82c5}, {"CPA", 0x16a09e},
  {"CSN", 0x5aa9e0}, {"DAL", 0xe0556a}, {"DLH", 0xf5c04a}, {"EIN", 0x59c39a},
  {"ELY", 0x7290ee}, {"ETD", 0xc4921b}, {"ETH", 0x5e8f4d}, {"EVA", 0x00a64f},
  {"FDX", 0xb07fd6}, {"FFT", 0x66c39d}, {"FIN", 0x806ee7}, {"GTI", 0x8fb0cc},
  {"IBE", 0xef5a68}, {"ICE", 0x587ae4}, {"ITY", 0x4b87dc}, {"JAL", 0xe14a55},
  {"JBU", 0x5b9ae8}, {"JZA", 0xf0505f}, {"KAL", 0x4e86d6}, {"KLM", 0x63a8e6},
  {"LAN", 0x8b6ad4}, {"LOT", 0x5580d8}, {"MSR", 0x3f7dd6}, {"NAX", 0xe1495e},
  {"NKS", 0xf5d24a}, {"POE", 0x5ec4d6}, {"QFA", 0xe40000}, {"QTR", 0xc15c8f},
  {"ROU", 0xf05a4a}, {"SAS", 0x6ea3dc}, {"SIA", 0xfcb130}, {"SVA", 0x46bc80},
  {"SWA", 0xf0a24a}, {"SWR", 0xe24a4d}, {"TAM", 0x8b6ad4}, {"TAP", 0x71bf44},
  {"THA", 0x9b62ce}, {"THY", 0xd55752}, {"TSC", 0x4fc0d8}, {"UAE", 0xd71921},
  {"UAL", 0x5b9ae0}, {"UPS", 0xc9954f}, {"VIR", 0xe8506e}, {"VOI", 0xc24fb0},
  {"WJA", 0x4fb8e0},
};
static const int kBrandCount = (int)(sizeof(kBrands) / sizeof(kBrands[0]));

// ---------- deterministic fallback ----------
// An unknown carrier still gets a stable, distinct colour rather than a flat
// grey, so two operators on screen never look alike. FNV-1a over the code keeps
// it stable across reboots and identical on every device.
static uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
  return h;
}

// HSV -> RGB at fixed S/V chosen for legibility on C_SURF. Hue skips the green
// band (70..170 deg): the palette is deliberately green-free (theme.h), and
// green also collides with nothing else on this display so it reads as a status
// colour rather than an identity one.
static uint32_t hueToRgb(int hue) {
  const float s = 0.55f, v = 0.90f;
  float c = v * s;
  float hp = hue / 60.0f;
  float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
  float r = 0, g = 0, b = 0;
  if      (hp < 1) { r = c; g = x; }
  else if (hp < 2) { r = x; g = c; }
  else if (hp < 3) { g = c; b = x; }
  else if (hp < 4) { g = x; b = c; }
  else if (hp < 5) { r = x; b = c; }
  else             { r = c; b = x; }
  float m = v - c;
  uint32_t R = (uint32_t)((r + m) * 255.0f + 0.5f);
  uint32_t G = (uint32_t)((g + m) * 255.0f + 0.5f);
  uint32_t B = (uint32_t)((b + m) * 255.0f + 0.5f);
  return (R << 16) | (G << 8) | B;
}

uint32_t brandRgbFor(const char* icao3) {
  if (!icao3 || !icao3[0]) return 0xaab4c0;          // C_IVORY2: unknown operator

  char key[4] = {0, 0, 0, 0};
  for (int i = 0; i < 3 && icao3[i]; i++) key[i] = (char)toupper((unsigned char)icao3[i]);

  int lo = 0, hi = kBrandCount - 1;                   // table is sorted
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int cmp = strcmp(key, kBrands[mid].icao);
    if (cmp == 0) return kBrands[mid].rgb;
    if (cmp < 0) hi = mid - 1; else lo = mid + 1;
  }

  // Unknown: 250 degrees of hue split into two arcs that skip green.
  uint32_t h = fnv1a(key) % 250u;
  int hue = (h < 70u) ? (int)h : (int)(h + 100u);     // 0..69 then 170..349
  return hueToRgb(hue);
}

lv_color_t brandColorFor(const char* icao3) {
  return lv_color_hex(brandRgbFor(icao3));
}
