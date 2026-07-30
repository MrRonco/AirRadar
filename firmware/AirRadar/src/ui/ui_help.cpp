// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// ui_help.cpp — the legend overlay behind the "?" button on SCR_MAIN.
//
// Deliberately a LEGEND, not a manual. Anything the display already labels
// ("ALT ft", "SPD kt") needs no entry; what needs explaining is what the
// display ENCODES — glyph colour and size, dimming, rings, dot states. This is
// also the proper home for the altitude ramp that used to sit in the Overview
// card at 1.9:1 contrast.
//
// Built once at boot and hidden. A hidden LVGL subtree is skipped entirely
// during redraw, so the overlay costs nothing until it is shown, and showing
// it is a single full-screen invalidation rather than anything continuous.
//
// Loop context only, like every other ui_* file.
#include <string.h>
#include "ui.h"

// ---------- layout ----------
static const int HLP_PAD_X   = 22;
static const int HLP_TITLE_Y = 14;
static const int HLP_TOP     = 62;
static const int HLP_COL_W   = 244;
static const int HLP_COL_GAP = 8;
static const int HLP_SAMPLE_W = 34;    // gutter reserved for the sample glyph
static const int HLP_ROW_H   = 34;     // one-line entry
static const int HLP_ROW_H2  = 48;     // two-line entry
static const lv_opa_t HLP_SCRIM_OPA = 236;

static lv_obj_t* s_overlay = nullptr;

// ============================================================
//  Small builders
// ============================================================
static lv_obj_t* hBox(lv_obj_t* p) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// A column heading ("TARGETS", "SCOPE", ...).
static void hHeading(lv_obj_t* p, int x, int y, const char* txt) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_add_style(l, &st_microlbl, 0);
  lv_obj_set_style_text_color(l, C_CY_SOFT, 0);
  lv_label_set_text(l, txt);
  lv_obj_set_pos(l, x, y);
}

// One legend row: [sample] description. Returns the height consumed so the
// caller can stack rows without hand-maintaining a running Y for each column.
static int hRow(lv_obj_t* p, int x, int y, const char* text) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, F_MONO13, 0);
  lv_obj_set_style_text_color(l, C_IVORY2, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, HLP_COL_W - HLP_SAMPLE_W);
  lv_label_set_text(l, text);
  lv_obj_set_pos(l, x + HLP_SAMPLE_W, y);
  // Two-line entries need the taller pitch; measure rather than guess.
  lv_obj_update_layout(l);
  return (lv_obj_get_height(l) > 20) ? HLP_ROW_H2 : HLP_ROW_H;
}

// Recoloured aircraft glyph used as a live sample.
static void hJet(lv_obj_t* p, int x, int y, lv_color_t c, lv_opa_t opa,
                 int angle, uint16_t zoom) {
  lv_obj_t* j = lv_img_create(p);
  lv_img_set_src(j, &img_jet);
  lv_img_set_pivot(j, 13, 13);
  lv_img_set_angle(j, angle);
  if (zoom != 256) lv_img_set_zoom(j, zoom);
  lv_obj_set_style_img_recolor(j, c, 0);
  lv_obj_set_style_img_recolor_opa(j, LV_OPA_COVER, 0);
  lv_obj_set_style_img_opa(j, opa, 0);
  lv_obj_set_pos(j, x, y);
}

static void hDot(lv_obj_t* p, int x, int y, lv_color_t c) {
  lv_obj_t* d = hBox(p);
  lv_obj_set_size(d, 9, 9);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, c, 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  lv_obj_set_pos(d, x + 8, y + 5);
}

static void hRing(lv_obj_t* p, int x, int y, lv_color_t c, int d) {
  lv_obj_t* r = hBox(p);
  lv_obj_set_size(r, d, d);
  lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  lv_obj_set_style_border_color(r, c, 0);
  lv_obj_set_style_border_opa(r, 210, 0);
  lv_obj_set_pos(r, x + (HLP_SAMPLE_W - d) / 2 - 4, y + (18 - d) / 2);
}

static void hText(lv_obj_t* p, int x, int y, const char* txt,
                  lv_color_t c, const lv_font_t* f) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, txt);
  lv_obj_set_pos(l, x, y);
}

static void onHelpClick(lv_event_t* e) { (void)e; helpToggle(); }

// ============================================================
//  Build
// ============================================================
void helpBuild(lv_obj_t* parent) {
  s_overlay = hBox(parent);
  lv_obj_set_pos(s_overlay, 0, 0);
  lv_obj_set_size(s_overlay, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(s_overlay, C_INK, 0);
  lv_obj_set_style_bg_opa(s_overlay, HLP_SCRIM_OPA, 0);
  // Clickable so a tap anywhere dismisses it AND so taps cannot fall through
  // to the scope underneath and change the selection while help is open.
  lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_overlay, onHelpClick, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

  hText(s_overlay, HLP_PAD_X, HLP_TITLE_Y, "LEGEND", C_IVORY, F_L28);
  hText(s_overlay, HLP_PAD_X + 108, HLP_TITLE_Y + 12,
        "tap anywhere to close", C_MUTE, F_MONO13);

  const int c1 = HLP_PAD_X;
  const int c2 = c1 + HLP_COL_W + HLP_COL_GAP;
  const int c3 = c2 + HLP_COL_W + HLP_COL_GAP;
  int y;

  // ---------- targets ----------
  hHeading(s_overlay, c1, HLP_TOP - 22, "TARGETS");
  y = HLP_TOP;
  hJet(s_overlay, c1, y - 4, C_ALT_LOW, LV_OPA_COVER, 900, 300);
  y += hRow(s_overlay, c1, y, "Below 10,000 ft");
  hJet(s_overlay, c1, y - 4, C_ALT_MID, LV_OPA_COVER, 900, 256);
  y += hRow(s_overlay, c1, y, "10,000 to 30,000 ft");
  hJet(s_overlay, c1, y - 4, C_ALT_HIGH, LV_OPA_COVER, 900, 220);
  y += hRow(s_overlay, c1, y, "Above 30,000 ft. Bigger glyph = lower and nearer");
  hJet(s_overlay, c1, y - 4, C_IVORY2, LV_OPA_COVER, 900, 256);
  y += hRow(s_overlay, c1, y, "Altitude not reported");
  hJet(s_overlay, c1, y - 4, C_ALT_MID, 150, 900, 256);
  y += hRow(s_overlay, c1, y, "Faded: coasting. Position is estimated, no recent report");
  hJet(s_overlay, c1, y - 4, C_ALERT, LV_OPA_COVER, 900, 256);
  y += hRow(s_overlay, c1, y, "Emergency squawk 7500 / 7600 / 7700");

  // ---------- scope ----------
  hHeading(s_overlay, c2, HLP_TOP - 22, "SCOPE");
  y = HLP_TOP;
  hRing(s_overlay, c2, y, C_IVORY, 18);
  y += hRow(s_overlay, c2, y, "White ring marks the selected aircraft");
  {  // military box sample
    lv_obj_t* b = hBox(s_overlay);
    lv_obj_set_size(b, 17, 17);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, C_IVORY, 0);
    lv_obj_set_style_border_opa(b, 180, 0);
    lv_obj_set_pos(b, c2 + 4, y);
  }
  y += hRow(s_overlay, c2, y, "Square box marks a military aircraft");
  hText(s_overlay, c2 + 2, y, "ABC", C_GOLD, F_MONO13);
  y += hRow(s_overlay, c2, y, "Gold callsign is on your watchlist");
  hRing(s_overlay, c2, y, C_RING, 18);
  y += hRow(s_overlay, c2, y, "Rings are one third, two thirds and full range");
  y += hRow(s_overlay, c2, y, "The circle edge is your receiver's range limit. Map beyond it is dimmed");
  y += hRow(s_overlay, c2, y, "North is always up");
  y += hRow(s_overlay, c2, y, "Tap a target to select it, swipe to cycle through them");

  // ---------- panels ----------
  hHeading(s_overlay, c3, HLP_TOP - 22, "PANELS");
  y = HLP_TOP;
  hDot(s_overlay, c3, y, C_CY);
  y += hRow(s_overlay, c3, y, "Live from your own receiver");
  hDot(s_overlay, c3, y, C_IVORY2);
  y += hRow(s_overlay, c3, y, "Falling back to the cloud feed");
  hDot(s_overlay, c3, y, C_AMBER);
  y += hRow(s_overlay, c3, y, "Feed has gone stale");
  y += hRow(s_overlay, c3, y, "COASTING counts aircraft still tracked but with no recent position");
  hText(s_overlay, c3, y, "FL350", C_IVORY, F_MONO13);
  y += hRow(s_overlay, c3, y, "Flight level: hundreds of feet, used above 10,000");
  hText(s_overlay, c3, y, "--", C_DIM, F_MONO13);
  y += hRow(s_overlay, c3, y, "Route not resolved yet, or none published");
  hText(s_overlay, c3, y, "0s", C_DIM, F_MONO13);
  y += hRow(s_overlay, c3, y, "Seconds since that aircraft last reported");
}

void helpToggle() {
  if (!s_overlay) return;
  bool hidden = lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
  if (hidden) {
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
  } else {
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
  }
}

bool helpVisible() {
  return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}
