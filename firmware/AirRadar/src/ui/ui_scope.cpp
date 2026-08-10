// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// ============================================================
//  ui_scope.cpp — AirRadar v7 radar scope (centre element of SCR_MAIN)
//  Circular clipped map + range rings + crosshair + animated blip pool + ISS.
//
//  Threading: everything here runs in loop() context (core 1) — the only
//  context allowed to touch LVGL and g_tracks (see core/state.h).
//  Each blip lives in one invisible 34x34 "holder" container; position glides
//  animate the holder only, so glow/jet/rings/label always move together.
// ============================================================
#include <math.h>
#include <string.h>
#include "ui.h"
#include "../core/tracks.h"
#include "../net/enrich.h"
#include "../net/maptiles.h"

// ---------- file-local geometry / look constants ----------
static const int SCOPE_X0     = SCOPE_CX - SCOPE_R;   // clip origin on screen
static const int SCOPE_Y0     = SCOPE_CY - SCOPE_R;
static const int SCOPE_D      = SCOPE_R * 2;
static const int RING_MID_D   = SCOPE_D * 2 / 3;
static const int RING_INNER_D = SCOPE_D / 3;
static const lv_opa_t RING_OPA_FULL  = 140;
static const lv_opa_t RING_OPA_MID   = 66;
static const lv_opa_t RING_OPA_INNER = 46;
static const lv_opa_t CROSS_OPA      = 120;
static const lv_opa_t TICK_OPA       = 200;
static const int   TICK_LEN      = 12;
static const int   NORTH_LBL_GAP = 16;          // "N" this far above the scope edge
static const float DIAG_45       = 0.70710678f; // sin/cos of 45 deg
static const int   RING_LBL_INSET = 14;         // range labels sit inside their ring
static const int   HOLDER_SZ    = 34;           // = glow / selection-ring size
static const int   HOLDER_HALF  = HOLDER_SZ / 2;
static const int   JET_SZ       = 26;           // img_jet, pivot at centre
static const int   MIL_BOX_SZ   = 30;
// ONE line, sized to a callsign. It used to be 118x28 to hold a second line
// ("FL330 · 147 km") that only the SELECTED target ever showed -- and both of
// those values sit in the Selected card in 22 px type, where they are far more
// legible than 11 px mono over a map. Dropping the line removed the redundancy
// AND the clipping: the box is now 9 monospace glyphs, which covers every ICAO
// flight id (max 7) and every registration fallback.
static const int   LBL_W        = 72;
static const int   LBL_H        = 14;           // one F_MONO11 line
static const lv_opa_t SEL_RING_OPA = 210;
static const lv_opa_t MIL_BOX_OPA  = 170;
static const int   LBL_OFF_X    = HOLDER_SZ + 4; // label beside the jet
static const int   LBL_OFF_Y    = 10;
// The label sits on whichever side keeps it inside the disc. Targets in the
// eastern third used to have their callsign shaved by the clip container --
// and because the selected target carried the widest label, the failure landed
// on the element that most needs to look authoritative.
//
// Rule 12: the holder must BOUND its children, so supporting a left-side label
// means a symmetric holder. Sized from the blip centre outward, that is
// LBL_GAP + LBL_W each way. Net area against the old asymmetric box is +14%,
// because halving the height pays back most of the extra width -- but it IS an
// increase in per-glide invalidation and wants a /api/stalls check on hardware.
static const int   LBL_GAP      = LBL_OFF_X - HOLDER_HALF;   // 21 px from centre
// The holder must BOUND every child. lv_obj_move_to() invalidates only the
// holder's own rect (lv_obj_move_children_by shifts children without
// invalidating them), so a label hanging outside it left a stale ghost at the
// old position on every glide. That ghost used to be scrubbed by an unrelated
// 4 Hz full-scope repaint; once that was fixed the ghost would have shown.
// Bounding the children also lets us drop LV_OBJ_FLAG_OVERFLOW_VISIBLE, which
// forced LVGL to draw all 13 holders even for invalid areas they don't touch.
// The jet is rotated by heading, so its drawn bounding box is the 26 px image
// diagonal (~37 px), not 26 — at 45 deg it overflows the 34 px cluster box by
// ~1.4 px on each side. With OVERFLOW_VISIBLE gone the parent clips children,
// so the cluster gets a pad and the blip centre moves to HOLDER_CX.
static const int   CLUSTER_PAD  = 3;
// The blip centre inside the holder. X and Y are NOT the same any more: the
// holder became symmetric horizontally to allow a left-side label, but its
// height is unchanged, so reusing CX for the vertical placement would hoist
// every blip by 73 px.
static const int   HOLDER_CX    = LBL_GAP + LBL_W;
static const int   HOLDER_CY    = CLUSTER_PAD + HOLDER_HALF;
static const int   HOLDER_W     = 2 * HOLDER_CX;
static const int   HOLDER_H     = (CLUSTER_PAD + LBL_OFF_Y + LBL_H
                                     > 2 * CLUSTER_PAD + HOLDER_SZ)
                                    ? (CLUSTER_PAD + LBL_OFF_Y + LBL_H)
                                    : (2 * CLUSTER_PAD + HOLDER_SZ);
static const int   MOVE_EPS_PX  = 3;            // ignore sub-blip jitter
static const uint32_t BLINK_HALF_MS = 500;      // emergency blink half period
static const lv_opa_t COAST_OPA = 150;
static const lv_opa_t BLINK_HI  = 255;
static const lv_opa_t BLINK_LO  = 90;
static const int   ISS_W = 22, ISS_H = 12;      // img_iss dimensions (theme.h)
static const int   ISS_LBL_GAP  = 4;
static const uint32_t GESTURE_CLICK_GUARD_MS = 600; // swallow click after swipe
static const uint32_t POOL_WARN_MIN_MS = 5000;  // rate-limit pool-full log
// Label decluttering. Drawing a callsign on every blip produced an unreadable
// pile-up; "nearest N" was no better, because the nearest targets are by
// definition clustered around the centre of the scope. So the rule is spatial:
// walk targets nearest-first and grant a label only when nothing already
// labelled sits within LABEL_MIN_SEP_PX. Selection, watchlist and emergency
// are granted unconditionally and are seeded first so they win any contest.
// The clash test used to be isotropic -- dx^2 + dy^2 < 46^2 -- while the ink it
// protects is a wide, short rectangle: LBL_GAP + LBL_W across and one line
// tall. Wrong shape in BOTH directions, so two labels 60 px apart horizontally
// passed the test and overprinted, while labels that would have stacked
// perfectly well vertically were refused. The test is now the ink's own box.
static const int LABEL_CLASH_X = LBL_GAP + LBL_W;
static const int LABEL_CLASH_Y = LBL_H + 4;
static const int LABEL_MAX        = 8;    // ink ceiling regardless of spacing

// 45deg-NE centre offsets for the two range labels (from container centre)
static const int RING_LBL_FULL_OFF =
    (int)((SCOPE_R - RING_LBL_INSET) * DIAG_45);
static const int RING_LBL_MID_OFF =
    (int)((RING_MID_D / 2 - RING_LBL_INSET) * DIAG_45);

// The two range numerals, in clip coordinates: 45 deg NE of centre. They are
// ink on the same field, so they are avoided both by the declutter pass and by
// the side chooser below.
static const float kRsvX[2] = { (float)(SCOPE_R + RING_LBL_FULL_OFF),
                                (float)(SCOPE_R + RING_LBL_MID_OFF) };
static const float kRsvY[2] = { (float)(SCOPE_R - RING_LBL_FULL_OFF),
                                (float)(SCOPE_R - RING_LBL_MID_OFF) };

// ---------- blip pool ----------
struct Blip {
  bool      used;
  char      hex[8];
  lv_obj_t* holder;     // animated container; nullptr until first use
  lv_obj_t* glow;
  lv_obj_t* jet;
  lv_obj_t* lbl;
  lv_obj_t* selRing;
  lv_obj_t* milBox;
  int16_t   tgtX, tgtY; // container-local blip centre we last animated toward
  // Change caches — lv_obj_set_style_*/lv_label_set_text invalidate even when
  // the value is identical; skipping no-op writes is what stops the constant
  // scope repaints (visible as screen wiggle against the RGB-panel DMA).
  bool      cInit;      // false until the first style pass fills the caches
  uint16_t  cCol;       // recolor .full
  lv_opa_t  cOpa;
  int16_t   cAngle;
  bool      cGlowHid, cSelHid, cMilHid, cLblHid;
  bool      cLblGold;
  bool      cLblLeft;   // label mirrored to the inboard side of the disc
  lv_opa_t  cLblOpa;    // fades with the glyph while coasting
  char      cLbl[16];
};
static Blip s_blips[AR_MAX_TRACKS];

static lv_obj_t* s_clip         = nullptr;   // circular clip container
static lv_obj_t* s_mapImg       = nullptr;
static bool      s_issRaisePending = false;  // z-order fix deferred out of blipBuild
static lv_obj_t* s_rangeLblMid  = nullptr;
static lv_obj_t* s_rangeLblFull = nullptr;
static lv_obj_t* s_issImg       = nullptr;
static lv_obj_t* s_issLbl       = nullptr;
static uint32_t  s_lastMapGen   = 0;
static int       s_lastRangeShown = -1;
static uint32_t  s_lastGestureMs  = 0;

// ============================================================
//  Small helpers
// ============================================================
// Change-cached on purpose. lv_obj_add_flag/clear_flag INVALIDATE the object
// unconditionally in LVGL 8.3 — they do not check whether the flag already had
// that value. scopeApplyMapImage() and scopeUpdateIss() call this every tick,
// so the unguarded version repainted the whole 392x392 map image ~4x/second
// (614 kpx/s, ~2.5 MB/s of extra PSRAM traffic) against a panel DMA that needs
// 25 MB/s continuous. That is the "wiggle", and it explains why it only ever
// appeared once a map image existed.
static inline void setHidden(lv_obj_t* o, bool hide) {
  if (hide == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;   // no-op write
  if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Plain lv_objs are clickable+scrollable by default; decorations must not
// steal taps from the clip container.
static void makeInert(lv_obj_t* o) {
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* makeRing(lv_obj_t* parent, int d, lv_color_t col,
                          lv_opa_t opa) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, col, 0);
  lv_obj_set_style_border_opa(o, opa, 0);
  makeInert(o);
  lv_obj_center(o);
  return o;
}

static lv_obj_t* makeBar(lv_obj_t* parent, int x, int y, int w, int h,
                         lv_color_t col, lv_opa_t opa) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, col, 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  makeInert(o);
  return o;
}

static lv_obj_t* makeMicroLabel(lv_obj_t* parent, const char* txt,
                                lv_color_t col) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, F_MONO11, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_label_set_text(l, txt);
  return l;
}

// ============================================================
//  Blip pool: create / acquire / place / style / release
// ============================================================
static void blipCreateObjects(Blip& b) {
  b.holder = lv_obj_create(s_clip);
  lv_obj_remove_style_all(b.holder);
  lv_obj_set_size(b.holder, HOLDER_W, HOLDER_H);
  lv_obj_add_flag(b.holder, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(b.holder, LV_OBJ_FLAG_HIDDEN);
  makeInert(b.holder);

  // Every child is placed from the holder's top-left so the 34x34 glyph
  // cluster keeps occupying 0..HOLDER_SZ regardless of the holder's real size
  // — blipPlace() centres the blip by offsetting HOLDER_CX.
  b.glow = lv_img_create(b.holder);
  lv_img_set_src(b.glow, &img_glow);
  lv_obj_set_pos(b.glow, HOLDER_CX - HOLDER_HALF, CLUSTER_PAD);
  lv_obj_set_style_img_recolor_opa(b.glow, LV_OPA_COVER, 0);

  b.selRing = makeRing(b.holder, HOLDER_SZ, C_IVORY, SEL_RING_OPA);
  lv_obj_align(b.selRing, LV_ALIGN_TOP_LEFT,           // makeRing centres by default
               HOLDER_CX - HOLDER_HALF, CLUSTER_PAD);
  lv_obj_add_flag(b.selRing, LV_OBJ_FLAG_HIDDEN);

  b.milBox = lv_obj_create(b.holder);
  lv_obj_remove_style_all(b.milBox);
  lv_obj_set_size(b.milBox, MIL_BOX_SZ, MIL_BOX_SZ);
  lv_obj_set_style_bg_opa(b.milBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(b.milBox, 1, 0);
  lv_obj_set_style_border_color(b.milBox, C_IVORY, 0);
  lv_obj_set_style_border_opa(b.milBox, MIL_BOX_OPA, 0);
  makeInert(b.milBox);
  lv_obj_align(b.milBox, LV_ALIGN_TOP_LEFT,
               HOLDER_CX - MIL_BOX_SZ / 2,
               CLUSTER_PAD + (HOLDER_SZ - MIL_BOX_SZ) / 2);
  lv_obj_add_flag(b.milBox, LV_OBJ_FLAG_HIDDEN);

  b.jet = lv_img_create(b.holder);
  lv_img_set_src(b.jet, &img_jet);
  lv_obj_set_pos(b.jet, HOLDER_CX - JET_SZ / 2,
                        CLUSTER_PAD + (HOLDER_SZ - JET_SZ) / 2);
  lv_img_set_pivot(b.jet, JET_SZ / 2, JET_SZ / 2);
  lv_obj_set_style_img_recolor_opa(b.jet, LV_OPA_COVER, 0);

  b.lbl = makeMicroLabel(b.holder, "", C_IVORY2);
  lv_obj_set_pos(b.lbl, HOLDER_CX + LBL_GAP, CLUSTER_PAD + LBL_OFF_Y);
  lv_obj_set_size(b.lbl, LBL_W, LBL_H);        // fixed box: no SIZE_CONTENT growth
  lv_label_set_long_mode(b.lbl, LV_LABEL_LONG_CLIP);

  // ISS must stay the topmost scope layer even after a late holder is created,
  // but DO NOT reorder here. lv_obj_move_foreground -> lv_obj_move_to_index
  // ends with lv_obj_invalidate(PARENT), and the parent is s_clip -- the whole
  // 424x424 disc. Doing it per blip meant every newly seen aircraft repainted
  // the entire scope plus, via the count/nearest change, the Overview card:
  // ~200,000 px, 52% of the screen, ~120 ms. That was the intermittent glitch.
  // Defer it instead; scopeUpdateIss performs one raise, and only when the ISS
  // is actually on screen, which is a few passes a day rather than every time
  // a new aircraft appears.
  s_issRaisePending = true;
}

// Find the slot already bound to hex, else bind the first free one.
// Returns slot index or -1 when the pool is full. isNew = freshly bound.
static int blipAcquire(const char* hex, bool& isNew) {
  int freeSlot = -1;
  for (int i = 0; i < AR_MAX_TRACKS; i++) {
    if (s_blips[i].used) {
      if (!strcmp(s_blips[i].hex, hex)) { isNew = false; return i; }
    } else if (freeSlot < 0) {
      freeSlot = i;
    }
  }
  if (freeSlot < 0) return -1;
  Blip& b = s_blips[freeSlot];
  if (!b.holder) blipCreateObjects(b);
  b.used = true;
  snprintf(b.hex, sizeof(b.hex), "%s", hex);
  isNew = true;
  return freeSlot;
}

static void blipAnimX(void* var, int32_t v) { lv_obj_set_x((lv_obj_t*)var, v); }
static void blipAnimY(void* var, int32_t v) { lv_obj_set_y((lv_obj_t*)var, v); }

static void blipGlide(lv_obj_t* o, lv_anim_exec_xcb_t cb, int32_t from,
                      int32_t to) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, AR_BLIP_GLIDE_MS);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// Put the label on whichever side keeps it inside the disc. Called from
// blipPlace because that is where the screen position is known.
static void blipLabelSide(Blip& b, int cx, int cy) {
  // Prefer the right, flip for the disc edge, and flip again if the right side
  // would land on a range numeral. A must-have label (selected, watchlist,
  // emergency) skips the declutter pass entirely, so without this the SELECTED
  // target -- the one that most needs to be readable -- was the one printing
  // over "250".
  bool left = (cx + LBL_GAP + LBL_W) > (SCOPE_D - 2);
  if (!left) {
    for (int k = 0; k < 2; k++) {
      if (fabsf(kRsvX[k] - (float)cx) < (float)LABEL_CLASH_X &&
          fabsf(kRsvY[k] - (float)cy) < (float)LABEL_CLASH_Y &&
          kRsvX[k] >= (float)cx) { left = true; break; }
    }
  }
  if (b.cInit && b.cLblLeft == left) return;      // change-cached: a no-op
  b.cLblLeft = left;                              //  set_pos still invalidates
  lv_obj_set_x(b.lbl, left ? (HOLDER_CX - LBL_GAP - LBL_W)
                           : (HOLDER_CX + LBL_GAP));
  lv_obj_set_style_text_align(b.lbl, left ? LV_TEXT_ALIGN_RIGHT
                                          : LV_TEXT_ALIGN_LEFT, 0);
}

// cx/cy = desired blip centre in clip-container coordinates.
static void blipPlace(Blip& b, int cx, int cy, bool isNew) {
  blipLabelSide(b, cx, cy);
  if (isNew) {
    lv_anim_del(b.holder, blipAnimX);
    lv_anim_del(b.holder, blipAnimY);
    lv_obj_set_pos(b.holder, cx - HOLDER_CX, cy - HOLDER_CY);
    lv_obj_clear_flag(b.holder, LV_OBJ_FLAG_HIDDEN);
    b.tgtX = (int16_t)cx;
    b.tgtY = (int16_t)cy;
    return;
  }
  int dx = cx - b.tgtX, dy = cy - b.tgtY;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  if (dx <= MOVE_EPS_PX && dy <= MOVE_EPS_PX) return;
  b.tgtX = (int16_t)cx;
  b.tgtY = (int16_t)cy;
  lv_anim_del(b.holder, blipAnimX);
  lv_anim_del(b.holder, blipAnimY);
  blipGlide(b.holder, blipAnimX, lv_obj_get_x(b.holder), cx - HOLDER_CX);
  blipGlide(b.holder, blipAnimY, lv_obj_get_y(b.holder), cy - HOLDER_CY);
}

static void blipSetLabel(Blip& b, const Track& t, bool selected, bool ranked) {
  bool show = selected || (g_set.showLabels &&
                           (ranked || trackOnWatchlist(t) || sqIsEmergency(t.squawk)));
  if (!b.cInit || b.cLblHid != !show) {
    setHidden(b.lbl, !show);
    b.cLblHid = !show;
  }
  if (!show) return;
  // One line, always. The selected target used to get a second line carrying
  // its flight level and distance -- both of which the Selected card already
  // shows in 22 px tabular type a few hundred pixels away. On the scope they
  // were 11 px mono over a map, and they were what pushed the label past the
  // clip edge. The white selection ring is the marker; the card is the readout.
  char txt[16];
  strlcpy(txt, t.flight[0] ? t.flight : t.hex, sizeof(txt));
  if (!b.cInit || strcmp(txt, b.cLbl) != 0) {
    strlcpy(b.cLbl, txt, sizeof(b.cLbl));
    lv_label_set_text(b.lbl, txt);
  }
  bool gold = trackOnWatchlist(t);
  if (!b.cInit || gold != b.cLblGold) {
    b.cLblGold = gold;
    lv_obj_set_style_text_color(b.lbl, gold ? C_GOLD : C_IVORY2, 0);
  }
}

static void blipStyle(Blip& b, const Track& t, uint32_t nowMs, bool ranked,
                      bool nearest) {
  bool selected  = g_selHex[0] && !strcmp(t.hex, g_selHex);
  bool emergency = sqIsEmergency(t.squawk);
  // Signed: applyPending can stamp lastApiMs a hair AFTER our caller's nowMs;
  // unsigned math would underflow into "coasting" for a frame.
  bool coasting  = (int32_t)(nowMs - t.lastApiMs) > (int32_t)AR_STALE_TRACK_MS;

  uint8_t r, g, bl;
  altColorRGB(t.altFt, r, g, bl);
  lv_color_t col = emergency ? C_RED : lv_color_make(r, g, bl);
  if (!b.cInit || col.full != b.cCol) {
    b.cCol = col.full;
    lv_obj_set_style_img_recolor(b.jet, col, 0);
    lv_obj_set_style_img_recolor(b.glow, col, 0);
  }

  int angle = ((int)(t.trackDeg * 10.0f)) % 3600;
  if (angle < 0) angle += 3600;
  if (!b.cInit || (int16_t)angle != b.cAngle) {
    b.cAngle = (int16_t)angle;
    lv_img_set_angle(b.jet, angle);
  }

  // The label fades WITH the glyph. Only the glyph used to dim, so a coasting
  // target's callsign rendered byte-identical to a live one's -- the legend
  // promises "faded = position estimated" and half the mark was not keeping it.
  lv_opa_t jetOpa = coasting ? COAST_OPA : LV_OPA_COVER;
  if (!b.cInit || b.cLblOpa != jetOpa) {
    b.cLblOpa = jetOpa;
    lv_obj_set_style_text_opa(b.lbl, jetOpa, 0);
  }
  if (emergency) jetOpa = ((nowMs / BLINK_HALF_MS) & 1) ? BLINK_HI : BLINK_LO;
  if (!b.cInit || jetOpa != b.cOpa) {
    b.cOpa = jetOpa;
    lv_obj_set_style_img_opa(b.jet, jetOpa, 0);
  }

  // Emphasis only. A 34 px halo on every blip merges into a smear once a dozen
  // targets are up -- and when everything is emphasised nothing is. Above ~12
  // tracks this also SHRINKS per-tick invalidation, so it pays for itself.
  const bool glowOn = !coasting && (selected || trackOnWatchlist(t) ||
                                    sqIsEmergency(t.squawk) || nearest);
  if (!b.cInit || b.cGlowHid != !glowOn) {
    b.cGlowHid = !glowOn;
    setHidden(b.glow, !glowOn);
  }
  if (!b.cInit || b.cSelHid != !selected) {
    b.cSelHid = !selected;
    setHidden(b.selRing, !selected);
  }
  if (!b.cInit || b.cMilHid != !t.mil) {
    b.cMilHid = !t.mil;
    setHidden(b.milBox, !t.mil);
  }
  blipSetLabel(b, t, selected, ranked);
  b.cInit = true;
}

static void blipRelease(Blip& b) {
  lv_anim_del(b.holder, blipAnimX);
  lv_anim_del(b.holder, blipAnimY);
  lv_obj_add_flag(b.holder, LV_OBJ_FLAG_HIDDEN);
  b.used = false;
  b.hex[0] = '\0';
  b.cInit = false;      // caches are stale for the next occupant of this slot
}

static void warnPoolExhausted(uint32_t nowMs) {
  static uint32_t lastWarn = 0;
  if (lastWarn && nowMs - lastWarn < POOL_WARN_MIN_MS) return;
  lastWarn = nowMs;
  Serial.println("[scope] blip pool exhausted - some targets not drawn");
}

// ============================================================
//  Events: tap-select + swipe-cycle
// ============================================================
static void refreshAfterSelect() {
  scopeUpdate(millis());
  cardsUpdate(millis());
  Track* sel = tracksSelected();
  if (sel) enrichRequestRoute(sel->hex, sel->flight);
}

static void scopeEventCb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;

  if (code == LV_EVENT_GESTURE) {
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT)       tracksSelectByOrder(+1);
    else if (dir == LV_DIR_RIGHT) tracksSelectByOrder(-1);
    else return;
    s_lastGestureMs = millis();
    refreshAfterSelect();
    return;
  }

  if (code == LV_EVENT_CLICKED) {
    // A swipe releases as a click too — don't let it clobber the selection.
    if (s_lastGestureMs && millis() - s_lastGestureMs < GESTURE_CLICK_GUARD_MS)
      return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (tracksSelectAtPixel(p.x, p.y)) refreshAfterSelect();
  }
}

// ============================================================
//  Build
// ============================================================
static void scopeBuildRingsAndCross() {
  makeRing(s_clip, SCOPE_D, C_RING, RING_OPA_FULL);
  makeRing(s_clip, RING_MID_D, C_RING, RING_OPA_MID);
  makeRing(s_clip, RING_INNER_D, C_RING, RING_OPA_INNER);
  // crosshair
  makeBar(s_clip, 0, SCOPE_R, SCOPE_D, 1, C_RING_DIM, CROSS_OPA);
  makeBar(s_clip, SCOPE_R, 0, 1, SCOPE_D, C_RING_DIM, CROSS_OPA);
  // N/E/S/W inner-edge ticks
  makeBar(s_clip, SCOPE_R, 0, 1, TICK_LEN, C_RING, TICK_OPA);
  makeBar(s_clip, SCOPE_D - TICK_LEN, SCOPE_R, TICK_LEN, 1, C_RING, TICK_OPA);
  makeBar(s_clip, SCOPE_R, SCOPE_D - TICK_LEN, 1, TICK_LEN, C_RING, TICK_OPA);
  makeBar(s_clip, 0, SCOPE_R, TICK_LEN, 1, C_RING, TICK_OPA);
}

static void scopeBuildRangeLabels() {
  // C_DIM, not C_FAINT. theme.h annotates C_FAINT "DECORATION ONLY -- never
  // text (1.9:1)", and these two numerals are the ONLY thing telling you whether
  // a blip 100 px out is 40 km or 200 km away. Over the map they had effectively
  // vanished. 1.99:1 -> 5.65:1.
  s_rangeLblFull = makeMicroLabel(s_clip, "", C_DIM);
  lv_obj_align(s_rangeLblFull, LV_ALIGN_CENTER, RING_LBL_FULL_OFF,
               -RING_LBL_FULL_OFF);
  s_rangeLblMid = makeMicroLabel(s_clip, "", C_DIM);
  lv_obj_align(s_rangeLblMid, LV_ALIGN_CENTER, RING_LBL_MID_OFF,
               -RING_LBL_MID_OFF);
}

static void scopeUpdateRangeLabels() {
  if (g_set.rangeKm == s_lastRangeShown) return;
  s_lastRangeShown = g_set.rangeKm;
  lv_label_set_text_fmt(s_rangeLblFull, "%d", g_set.rangeKm);
  lv_label_set_text_fmt(s_rangeLblMid, "%d",
                        (int)(g_set.rangeKm * 2.0f / 3.0f + 0.5f));
}

static void scopeBuildIss() {
  s_issImg = lv_img_create(s_clip);
  lv_img_set_src(s_issImg, &img_iss);
  lv_obj_add_flag(s_issImg, LV_OBJ_FLAG_HIDDEN);
  s_issLbl = makeMicroLabel(s_clip, "ISS", C_IVORY2);
  lv_obj_add_flag(s_issLbl, LV_OBJ_FLAG_HIDDEN);
}

void scopeBuild(lv_obj_t* parent) {
  memset(s_blips, 0, sizeof(s_blips));
  s_lastMapGen = 0;
  s_lastRangeShown = -1;

  // Full-bleed base map on the screen root, behind everything. It is dimmed to
  // MAP_DIM_PCT outside the range circle by the resampler, so the disc still
  // reads as the edge of receiver coverage and the cards can stay opaque.
  s_mapImg = lv_img_create(parent);
  lv_obj_set_pos(s_mapImg, 0, 0);
  lv_obj_add_flag(s_mapImg, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_background(s_mapImg);

  s_clip = lv_obj_create(parent);
  lv_obj_remove_style_all(s_clip);
  lv_obj_set_pos(s_clip, SCOPE_X0, SCOPE_Y0);
  lv_obj_set_size(s_clip, SCOPE_D, SCOPE_D);
  // Transparent, and NO clip_corner. Nothing needs clipping — scopeToScreen()
  // rejects anything beyond the range ring, so a blip can never leave the
  // circle — and clip_corner made LVGL answer COVER_CHECK with MASKED, which
  // forced every invalidation inside the scope to redraw from the screen root.
  lv_obj_set_style_bg_opa(s_clip, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(s_clip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_clip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_clip, scopeEventCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_clip, scopeEventCb, LV_EVENT_GESTURE, nullptr);

  scopeBuildRingsAndCross();
  scopeBuildRangeLabels();
  scopeUpdateRangeLabels();
  scopeBuildIss();

  // "N" above the top tick — on the screen root so the circle doesn't clip it.
  lv_obj_t* north = makeMicroLabel(parent, "N", C_DIM);
  lv_obj_align(north, LV_ALIGN_TOP_MID, SCOPE_CX - SCR_W / 2,
               SCOPE_Y0 - NORTH_LBL_GAP);
}

// ============================================================
//  Update
// ============================================================
static void scopeUpdateIss() {
  if (!s_issImg) return;
  // Snapshot under the mux: issTask rewrites these doubles on core 0 and a
  // torn 64-bit read here would place the glyph at a bogus position.
  IssState iss;
  portENTER_CRITICAL(&g_dataMux);
  iss = g_iss;
  portEXIT_CRITICAL(&g_dataMux);
  float sx = 0, sy = 0;
  bool show = g_set.issEn && iss.valid &&
              scopeToScreen(iss.lat, iss.lon, sx, sy);
  if (show) {
    int lx = (int)lroundf(sx) - SCOPE_X0;
    int ly = (int)lroundf(sy) - SCOPE_Y0;
    lv_obj_set_pos(s_issImg, lx - ISS_W / 2, ly - ISS_H / 2);
    lv_obj_set_pos(s_issLbl, lx + ISS_W / 2 + ISS_LBL_GAP, ly - ISS_H / 2);
  }
  setHidden(s_issImg, !show);
  setHidden(s_issLbl, !show);
  // Restore z-order at most once per update, and only while visible -- raising
  // a hidden object costs a full-disc invalidate and buys nothing.
  if (show && s_issRaisePending) {
    lv_obj_move_foreground(s_issImg);
    lv_obj_move_foreground(s_issLbl);
    s_issRaisePending = false;
  }
}

void scopeUpdate(uint32_t nowMs) {
  if (!s_clip) return;
  scopeUpdateRangeLabels();

  bool seen[AR_MAX_TRACKS] = {false};

  // ---- decide which targets get a callsign (spatial declutter) ----
  bool  ranked[AR_MAX_TRACKS] = {false};
  float lx[LABEL_MAX], ly[LABEL_MAX];
  int   nLbl = 0;
  // The two range numerals are ink on the same field and were never in the
  // clash test, so a callsign could land straight on top of one -- which only
  // became obvious once they stopped being C_FAINT and could actually be seen.
  // They sit at 45 deg NE of centre, and they are RESERVED rather than counted:
  // LABEL_MAX is a budget for callsigns, not for chrome.
  // kRsv* are CLIP coordinates; this loop works in SCREEN coordinates, so the
  // comparison has to be made in one space. Getting this wrong produced a
  // spurious clash that silently cost a label.
  for (int pass = 0; pass < 2; pass++) {
    for (int oi = 0; oi < g_orderN && nLbl < LABEL_MAX; oi++) {
      int idx = g_orderIdx[oi];
      const Track& t = g_tracks[idx];
      if (ranked[idx]) continue;
      bool must = (g_selHex[0] && !strcmp(t.hex, g_selHex)) ||
                  trackOnWatchlist(t) || sqIsEmergency(t.squawk);
      if (pass == 0 && !must) continue;        // pass 0 seeds the must-haves
      float sx = 0, sy = 0;
      if (!scopeToScreen(t.lat, t.lon, sx, sy)) continue;
      if (!must) {
        bool clash = false;
        const float cxL = sx - SCOPE_X0, cyL = sy - SCOPE_Y0;
        for (int k = 0; k < 2 && !clash; k++) {
          if (fabsf(kRsvX[k] - cxL) < LABEL_CLASH_X &&
              fabsf(kRsvY[k] - cyL) < LABEL_CLASH_Y) clash = true;
        }
        for (int k = 0; k < nLbl && !clash; k++) {
          if (fabsf(lx[k] - sx) < LABEL_CLASH_X &&
              fabsf(ly[k] - sy) < LABEL_CLASH_Y) clash = true;
        }
        if (clash) continue;
      }
      ranked[idx] = true;
      lx[nLbl] = sx; ly[nLbl] = sy; nLbl++;
    }
  }

  for (int i = 0; i < AR_MAX_TRACKS; i++) {
    const Track& t = g_tracks[i];
    if (!t.valid || !trackPassesFilters(t)) continue;
    float sx = 0, sy = 0;
    if (!scopeToScreen(t.lat, t.lon, sx, sy)) continue;

    bool isNew = false;
    int slot = blipAcquire(t.hex, isNew);
    if (slot < 0) { warnPoolExhausted(nowMs); continue; }
    seen[slot] = true;

    Blip& b = s_blips[slot];
    blipPlace(b, (int)lroundf(sx) - SCOPE_X0, (int)lroundf(sy) - SCOPE_Y0,
              isNew);
    // This loop walks track SLOTS, so "nearest" is the slot g_orderIdx[0]
    // names -- g_orderIdx is the distance-sorted index, not this loop's cursor.
    const bool isNear = (g_orderN > 0 && g_orderIdx[0] == i);
    blipStyle(b, t, nowMs, ranked[i], isNear);
  }

  for (int i = 0; i < AR_MAX_TRACKS; i++)
    if (s_blips[i].used && !seen[i]) blipRelease(s_blips[i]);

  scopeUpdateIss();
}

// ============================================================
//  Map image swap-in
// ============================================================
void scopeApplyMapImage() {
  if (!s_mapImg) return;
  uint32_t gen = mapGeneration();
  const lv_img_dsc_t* img = mapImage();
  if (gen != s_lastMapGen && img != nullptr) {
    lv_img_set_src(s_mapImg, img);
    lv_obj_invalidate(s_mapImg);   // same dsc pointer may carry fresh pixels
    s_lastMapGen = gen;
    Serial.printf("[scope] map gen %lu applied\n", (unsigned long)gen);
  }
  bool haveImg = (lv_img_get_src(s_mapImg) != nullptr);
  setHidden(s_mapImg, !g_set.mapEn || !haveImg);
}
