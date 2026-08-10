// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// fakemap.cpp — a stand-in base map for the harness.
//
// Why this exists: without a map the harness renders the scope over flat ink,
// and both design reviews had to caveat every composition judgement because of
// it. Worse, anything that DRAWS INTO the map buffer — the graduated bezel — is
// invisible without one.
//
// This is not a map. It is a ground with the right statistical character, run
// through the REAL tint and coverage-lens formulas copied from maptiles.cpp so
// the disc edge, the vignette and the 30% outside dim all behave exactly as
// they do on the device. Judge composition and contrast against it; do not
// judge cartography.
//
// CALIBRATED, not invented -- and this cost a wrong answer to find. The first
// version was a smooth value-noise field scaled to 0..150, which put its mean
// input luminance at 76. Real CARTO dark_nolabels tiles measure a mean of
// **19**, with a strongly BIMODAL distribution: water at 9, land at 38, and a
// maximum of 41 across a whole z8 mosaic. The harness was therefore showing a
// ground four times brighter than the panel's, and a smooth gradient where the
// real thing is two flat tones with a fine feature network on top. Anything
// judged against it -- "is the map too loud", "can I still see it" -- was
// being judged against the wrong picture.
//
// The constants below are measured off a 5x3 z8 dark_nolabels mosaic at the
// synthetic home. Re-measure if AR_TILE_STYLE ever changes again.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "Arduino.h"
#include "fakemap.h"
#include "../firmware/AirRadar/src/config.h"
// The real thing, compiled in: the bezel is pure pixel arithmetic on an RGB565
// buffer, so the harness engraves it exactly as the map task does. This is the
// reason fakemap.cpp exists -- anything that draws INTO the map is invisible
// without a map to draw into.
#include "../firmware/AirRadar/src/ui/bezel.h"

// Mirrors maptiles.cpp so the harness dims identically. If those change there,
// change them here — the point of this file is fidelity to that pipeline.
static const float VIGNETTE_STRENGTH = 0.55f;
static const int   TINT_R_PCT = 32,  TINT_R_ADD = 7;
static const int   TINT_G_PCT = 62,  TINT_G_ADD = 14;
static const int   TINT_B_PCT = 105, TINT_B_ADD = 26;

// Measured off a 5x3 z8 dark_nolabels mosaic: mean 19, median 13, p25 9,
// p75 38, max 41.
static const float LUM_WATER   = 9.0f;
static const float LUM_LAND    = 38.0f;
static const float LUM_FEATURE = 5.0f;
static const float LAND_EDGE0  = 0.47f;   // noise value where the coast starts
static const float LAND_EDGE1  = 0.55f;   //  ... and where it is fully land

static uint16_t*    s_buf = nullptr;
static lv_img_dsc_t s_dsc;
static uint32_t     s_gen = 0;
static int          s_lumNum = 11, s_lumDen = 10;   // TINT_LUM_NUM/DEN under test

// Deterministic value noise — same field every run, so a layout change is the
// only thing that can differ between two screenshots.
static float vnoise(float x, float y, uint32_t seed) {
  const int xi = (int)floorf(x), yi = (int)floorf(y);
  const float xf = x - xi, yf = y - yi;
  auto h = [seed](int a, int b) {
    uint32_t n = (uint32_t)(a * 374761393 + b * 668265263 + seed * 2246822519u);
    n = (n ^ (n >> 13)) * 1274126177u;
    return (float)((n ^ (n >> 16)) & 0xFFFF) / 65535.0f;
  };
  const float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
  return (h(xi, yi)     * (1 - u) + h(xi + 1, yi)     * u) * (1 - v) +
         (h(xi, yi + 1) * (1 - u) + h(xi + 1, yi + 1) * u) * v;
}

void fakeMapSetTint(int lumNum, int lumDen) {
  s_lumNum = lumNum; s_lumDen = lumDen;
  fakeMapBuild();
}

void fakeMapBuild() {
  if (!s_buf) {
    s_buf = (uint16_t*)malloc((size_t)MAP_W * MAP_H * 2);
    if (!s_buf) return;
    s_dsc.header.always_zero = 0;
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.header.w  = MAP_W;
    s_dsc.header.h  = MAP_H;
    s_dsc.data_size = (size_t)MAP_W * MAP_H * 2;
    s_dsc.data      = (const uint8_t*)s_buf;
  }
  const int   cx = MAP_W / 2, cy = MAP_H / 2;
  const float rIn = (float)(SCOPE_R - MAP_DIM_FEATHER);
  const float rOut = (float)(SCOPE_R + MAP_DIM_FEATHER);

  for (int y = 0; y < MAP_H; y++) {
    for (int x = 0; x < MAP_W; x++) {
      // Two octaves decide WHERE the coastline is; they do not set brightness.
      // CARTO's dark tiles are not a continuous field, they are two flat tones
      // with a thin feature network drawn over them, so the noise is
      // thresholded into water/land with a soft edge rather than used directly.
      float n = vnoise(x / 96.0f, y / 96.0f, 7u) * 0.68f +
                vnoise(x / 31.0f, y / 31.0f, 19u) * 0.32f;
      float land = (n - LAND_EDGE0) / (LAND_EDGE1 - LAND_EDGE0);
      if (land < 0.0f) land = 0.0f; else if (land > 1.0f) land = 1.0f;
      land = land * land * (3.0f - 2.0f * land);            // smoothstep
      // Roads and built-up areas: a fine, low-amplitude layer. This is what
      // the resample aliases against on the device, so it has to be present.
      const float feat = vnoise(x / 6.5f, y / 6.5f, 31u);
      int lum = (int)(LUM_WATER + (LUM_LAND - LUM_WATER) * land
                      + feat * LUM_FEATURE);
      lum = lum * s_lumNum / s_lumDen;             // the knob under test
      if (lum > 255) lum = 255;

      int r = lum * TINT_R_PCT / 100 + TINT_R_ADD;
      int g = lum * TINT_G_PCT / 100 + TINT_G_ADD;
      int b = lum * TINT_B_PCT / 100 + TINT_B_ADD;
      if (b > 255) b = 255;

      const float dx = (float)(x - cx), dy = (float)(y - cy);
      const float d = sqrtf(dx * dx + dy * dy);
      float k;
      if (d <= rIn)       k = 1.0f - VIGNETTE_STRENGTH * (d * d) / (float)(SCOPE_R * SCOPE_R);
      else if (d >= rOut) k = MAP_DIM_PCT / 100.0f;
      else {
        const float t  = (d - rIn) / (rOut - rIn);
        const float in = 1.0f - VIGNETTE_STRENGTH;
        k = in + (MAP_DIM_PCT / 100.0f - in) * t;
      }
      r = (int)(r * k); g = (int)(g * k); b = (int)(b * k);
      s_buf[y * MAP_W + x] =
          (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  bezelRasterise(s_buf, MAP_W, MAP_H, cx, cy, SCOPE_R);
  s_gen++;
}

uint16_t*           fakeMapBuf()   { return s_buf; }
const lv_img_dsc_t* fakeMapImage() { return s_buf ? &s_dsc : nullptr; }
uint32_t            fakeMapGen()   { return s_gen; }
