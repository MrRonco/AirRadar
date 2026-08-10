// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// bezel.cpp — a graduated bearing scale, engraved into the map.
//
// The panel could always show you that something was north-east. It could not
// tell you that it was on 038. Three concentric rings answer "how far"; until
// now nothing answered "which way" beyond a single "N" and four axis ticks.
//
// Drawn as PIXELS into the map back buffer rather than as objects, for the
// reason CLAUDE.md rule 19 exists: anything living in the clip container's
// child list costs something every time that container repaints, and 36 ticks
// plus their transforms is not something a panel already spending 25 MB/s of
// PSRAM bandwidth on DMA should be asked to carry. Engraved into the buffer it
// costs one pass at map-build time and nothing, ever, afterwards.
//
// Anti-aliasing is analytic rather than supersampled: each pixel in the
// annulus is converted to polar coordinates once, and its coverage is the
// product of how close it is to a tick's centre line (tangentially) and how
// far along the tick's length it sits (radially). One sqrtf and one atan2f per
// annulus pixel, ~19,000 of them, on core 0, once.
#include <math.h>
#include "bezel.h"
#include "theme.h"

// Tick geometry, in pixels inward from the ring.
static const int   BEZ_MINOR_LEN = 5;     // every 10 deg
static const int   BEZ_MAJOR_LEN = 9;     // every 30 deg
static const float BEZ_HALF_W    = 0.65f; // half the tick's tangential width
static const float BEZ_FEATHER   = 0.75f; // px of soft edge, all four sides

// Alpha at full coverage. Deliberately low: a bezel is read when you look for
// it and ignored when you do not, and this one sits under aircraft glyphs.
static const float BEZ_MINOR_A = 0.30f;
static const float BEZ_MAJOR_A = 0.55f;

// C_BORDER — the same hairline colour every card edge and divider uses, which
// is what makes the scale read as part of the instrument rather than as
// something drawn on the map.
static const int BEZ_R8 = 0xB4, BEZ_G8 = 0xCD, BEZ_B8 = 0xE6;

// 1 fully inside, 0 fully outside, linear across BEZ_FEATHER.
static inline float edge(float dist) {
  if (dist <= 0.0f) return 0.0f;
  if (dist >= BEZ_FEATHER) return 1.0f;
  return dist / BEZ_FEATHER;
}

void bezelRasterise(uint16_t* buf, int w, int h, int cx, int cy, int r) {
  if (!buf || r < 32) return;

  const float rOuter = (float)r;                       // ticks hang inside the ring
  const float rInner = rOuter - (float)BEZ_MAJOR_LEN - BEZ_FEATHER;
  const float rIn2   = rInner * rInner;
  const float rOut2  = (rOuter + BEZ_FEATHER) * (rOuter + BEZ_FEATHER);

  int y0 = cy - r - 2, y1 = cy + r + 2;
  int x0 = cx - r - 2, x1 = cx + r + 2;
  if (y0 < 0) y0 = 0;
  if (x0 < 0) x0 = 0;
  if (y1 > h - 1) y1 = h - 1;
  if (x1 > w - 1) x1 = w - 1;

  for (int y = y0; y <= y1; y++) {
    const float dy = (float)(y - cy);
    uint16_t* row = &buf[(size_t)y * w];
    for (int x = x0; x <= x1; x++) {
      const float dx = (float)(x - cx);
      const float d2 = dx * dx + dy * dy;
      if (d2 < rIn2 || d2 > rOut2) continue;           // cheap annulus reject

      const float d = sqrtf(d2);
      // Bearing, degrees clockwise from north. Screen y grows downward, so
      // north is -dy; atan2(dx, -dy) gives exactly that with no correction.
      float deg = atan2f(dx, -dy) * 57.2957795f;
      if (deg < 0.0f) deg += 360.0f;

      const int k = (int)lroundf(deg / 10.0f) % 36;    // nearest 10 deg mark
      if (k % 9 == 0) continue;                        // cardinals: ui_scope's

      const float len = (k % 3 == 0) ? (float)BEZ_MAJOR_LEN
                                     : (float)BEZ_MINOR_LEN;
      const float amp = (k % 3 == 0) ? BEZ_MAJOR_A : BEZ_MINOR_A;

      // Radial coverage: inside [rOuter - len, rOuter], feathered both ends.
      const float radial = edge(d - (rOuter - len)) * edge(rOuter - d);
      if (radial <= 0.0f) continue;

      // Tangential coverage: arc distance from the tick's centre line.
      float dDeg = deg - (float)(k * 10);
      if (dDeg > 180.0f) dDeg -= 360.0f;
      else if (dDeg < -180.0f) dDeg += 360.0f;
      const float tang = edge(BEZ_HALF_W + BEZ_FEATHER
                              - fabsf(dDeg * 0.01745329f * d));
      if (tang <= 0.0f) continue;

      const float a = amp * radial * tang;
      const uint16_t c = row[x];
      const int r8 = ((c >> 11) & 0x1F) << 3;
      const int g8 = ((c >> 5) & 0x3F) << 2;
      const int b8 = (c & 0x1F) << 3;
      const int nr = (int)((float)r8 + ((float)BEZ_R8 - (float)r8) * a);
      const int ng = (int)((float)g8 + ((float)BEZ_G8 - (float)g8) * a);
      const int nb = (int)((float)b8 + ((float)BEZ_B8 - (float)b8) * a);
      row[x] = (uint16_t)(((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3));
    }
  }
}
