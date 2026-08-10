// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// spark.cpp — see spark.h for why this exists at all.
//
// Cost, because on this panel that is the whole argument:
//   history   SPARK_SLOTS bytes of .bss (60)
//   canvas    LV_IMG_CF_ALPHA_1BIT, w*h/8 bytes -- 134x20 is ~340
//   repaint   ONCE A MINUTE, ~2,700 px
//
// For scale, the v7.2 display glitch was a 200,000 px repaint four times a
// second. This is three orders of magnitude below that and fires 1/240th as
// often. An alpha canvas rather than true colour keeps the buffer at an
// eighth of the size and lets the colour come from the style, so the sparkline
// follows the theme instead of baking a value in.
#include <string.h>
#include "spark.h"
#include "theme.h"

static uint8_t   s_hist[SPARK_SLOTS];
static uint8_t   s_count = 0;          // how many slots are real yet
static uint8_t   s_head  = 0;          // next slot to write
static uint32_t  s_lastSampleMs = 0;
static bool      s_dirty = false;

static lv_obj_t* s_canvas = nullptr;
static uint8_t*  s_buf    = nullptr;
static int       s_w = 0, s_h = 0;

static const uint32_t SAMPLE_MS = 60UL * 1000UL;

void sparkSample(uint32_t nowMs, uint8_t inRange) {
  // Signed compare: millis() wraps every 49 days and this device is expected
  // to run through that.
  if (s_lastSampleMs && (int32_t)(nowMs - s_lastSampleMs) < (int32_t)SAMPLE_MS) return;
  s_lastSampleMs = nowMs;
  s_hist[s_head] = inRange;
  s_head = (uint8_t)((s_head + 1) % SPARK_SLOTS);
  if (s_count < SPARK_SLOTS) s_count++;
  s_dirty = true;
}

bool sparkDirty() { return s_dirty; }

lv_obj_t* sparkBuild(lv_obj_t* parent, int w, int h) {
  static uint8_t buf[LV_CANVAS_BUF_SIZE_ALPHA_1BIT(SPARK_SLOTS * 2, 24)];
  s_w = w; s_h = h; s_buf = buf;
  s_canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(s_canvas, buf, w, h, LV_IMG_CF_ALPHA_1BIT);
  // Alpha format: the ink comes from the style, so the sparkline tracks the
  // palette and costs an eighth of a true-colour buffer.
  lv_obj_set_style_img_recolor(s_canvas, C_CY_SOFT, 0);
  lv_obj_set_style_img_recolor_opa(s_canvas, LV_OPA_COVER, 0);
  lv_obj_set_style_img_opa(s_canvas, 190, 0);
  lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_TRANSP);
  return s_canvas;
}

void sparkRedraw() {
  if (!s_canvas) return;
  s_dirty = false;
  lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_TRANSP);
  if (!s_count) return;

  uint8_t peak = 1;
  for (int i = 0; i < s_count; i++) if (s_hist[i] > peak) peak = s_hist[i];

  // One column per slot, oldest at the left. Columns are two px wide with the
  // right one left blank, which reads as a bar chart rather than a smear at
  // this size and halves the pixels touched.
  const int cols = (s_w / 2) < SPARK_SLOTS ? (s_w / 2) : SPARK_SLOTS;
  for (int c = 0; c < cols; c++) {
    // Walk back from the newest so a partly-filled history hugs the right.
    const int age = cols - 1 - c;
    if (age >= s_count) continue;
    const int slot = (s_head - 1 - age + SPARK_SLOTS * 2) % SPARK_SLOTS;
    int bar = (int)((long)s_hist[slot] * (s_h - 1) / peak);
    if (bar < 1 && s_hist[slot] > 0) bar = 1;   // a lone aircraft still shows
    const int x = c * 2;
    for (int y = s_h - bar; y < s_h; y++) lv_canvas_set_px_opa(s_canvas, x, y, LV_OPA_COVER);
  }
  // A baseline, so an empty hour reads as "nothing flew" rather than as a
  // component that failed to render.
  for (int x = 0; x < cols * 2; x += 2)
    lv_canvas_set_px_opa(s_canvas, x, s_h - 1, LV_OPA_COVER);
}
