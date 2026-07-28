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
static const lv_opa_t SEL_RING_OPA = 210;
static const lv_opa_t MIL_BOX_OPA  = 170;
static const int   LBL_OFF_X    = HOLDER_SZ + 4; // label right of the jet
static const int   LBL_OFF_Y    = 10;
static const int   MOVE_EPS_PX  = 2;            // ignore sub-blip jitter
static const uint32_t BLINK_HALF_MS = 500;      // emergency blink half period
static const lv_opa_t COAST_OPA = 150;
static const lv_opa_t BLINK_HI  = 255;
static const lv_opa_t BLINK_LO  = 90;
static const int   ISS_W = 22, ISS_H = 12;      // img_iss dimensions (theme.h)
static const int   ISS_LBL_GAP  = 4;
static const uint32_t GESTURE_CLICK_GUARD_MS = 600; // swallow click after swipe
static const uint32_t POOL_WARN_MIN_MS = 5000;  // rate-limit pool-full log

// 45deg-NE centre offsets for the two range labels (from container centre)
static const int RING_LBL_FULL_OFF =
    (int)((SCOPE_R - RING_LBL_INSET) * DIAG_45);
static const int RING_LBL_MID_OFF =
    (int)((RING_MID_D / 2 - RING_LBL_INSET) * DIAG_45);

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
};
static Blip s_blips[AR_MAX_TRACKS];

static lv_obj_t* s_clip         = nullptr;   // circular clip container
static lv_obj_t* s_mapImg       = nullptr;
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
static inline void setHidden(lv_obj_t* o, bool hide) {
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
  lv_obj_set_size(b.holder, HOLDER_SZ, HOLDER_SZ);
  lv_obj_add_flag(b.holder, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(b.holder, LV_OBJ_FLAG_OVERFLOW_VISIBLE); // label pokes out
  lv_obj_add_flag(b.holder, LV_OBJ_FLAG_HIDDEN);
  makeInert(b.holder);

  b.glow = lv_img_create(b.holder);
  lv_img_set_src(b.glow, &img_glow);
  lv_obj_set_pos(b.glow, 0, 0);
  lv_obj_set_style_img_recolor_opa(b.glow, LV_OPA_COVER, 0);

  b.selRing = makeRing(b.holder, HOLDER_SZ, C_IVORY, SEL_RING_OPA);
  lv_obj_add_flag(b.selRing, LV_OBJ_FLAG_HIDDEN);

  b.milBox = lv_obj_create(b.holder);
  lv_obj_remove_style_all(b.milBox);
  lv_obj_set_size(b.milBox, MIL_BOX_SZ, MIL_BOX_SZ);
  lv_obj_set_style_bg_opa(b.milBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(b.milBox, 1, 0);
  lv_obj_set_style_border_color(b.milBox, C_IVORY, 0);
  lv_obj_set_style_border_opa(b.milBox, MIL_BOX_OPA, 0);
  makeInert(b.milBox);
  lv_obj_center(b.milBox);
  lv_obj_add_flag(b.milBox, LV_OBJ_FLAG_HIDDEN);

  b.jet = lv_img_create(b.holder);
  lv_img_set_src(b.jet, &img_jet);
  lv_obj_set_pos(b.jet, (HOLDER_SZ - JET_SZ) / 2, (HOLDER_SZ - JET_SZ) / 2);
  lv_img_set_pivot(b.jet, JET_SZ / 2, JET_SZ / 2);
  lv_obj_set_style_img_recolor_opa(b.jet, LV_OPA_COVER, 0);

  b.lbl = makeMicroLabel(b.holder, "", C_IVORY2);
  lv_obj_set_pos(b.lbl, LBL_OFF_X, LBL_OFF_Y);

  // ISS stays the topmost scope layer even after late holder creation.
  if (s_issImg) lv_obj_move_foreground(s_issImg);
  if (s_issLbl) lv_obj_move_foreground(s_issLbl);
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

// cx/cy = desired blip centre in clip-container coordinates.
static void blipPlace(Blip& b, int cx, int cy, bool isNew) {
  if (isNew) {
    lv_anim_del(b.holder, blipAnimX);
    lv_anim_del(b.holder, blipAnimY);
    lv_obj_set_pos(b.holder, cx - HOLDER_HALF, cy - HOLDER_HALF);
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
  blipGlide(b.holder, blipAnimX, lv_obj_get_x(b.holder), cx - HOLDER_HALF);
  blipGlide(b.holder, blipAnimY, lv_obj_get_y(b.holder), cy - HOLDER_HALF);
}

static void blipSetLabel(Blip& b, const Track& t, bool selected) {
  bool show = g_set.showLabels || selected;
  setHidden(b.lbl, !show);
  if (!show) return;
  const char* name = t.flight[0] ? t.flight : t.hex;
  if (selected) {
    float dKm = haversineKm(g_set.homeLat, g_set.homeLon, t.lat, t.lon);
    char dist[12];
    if (dKm < 10.0f) snprintf(dist, sizeof(dist), "%.1f", dKm);
    else             snprintf(dist, sizeof(dist), "%.0f", dKm);
    char txt[64];
    if (t.altFt >= 0)
      snprintf(txt, sizeof(txt), "%s\nFL%03d · %s km", name, t.altFt / 100,
               dist);
    else
      snprintf(txt, sizeof(txt), "%s\n--- · %s km", name, dist);
    lv_label_set_text(b.lbl, txt);
  } else {
    lv_label_set_text(b.lbl, name);
  }
  lv_obj_set_style_text_color(b.lbl, trackOnWatchlist(t) ? C_GOLD : C_IVORY2,
                              0);
}

static void blipStyle(Blip& b, const Track& t, uint32_t nowMs) {
  bool selected  = g_selHex[0] && !strcmp(t.hex, g_selHex);
  bool emergency = sqIsEmergency(t.squawk);
  // Signed: applyPending can stamp lastApiMs a hair AFTER our caller's nowMs;
  // unsigned math would underflow into "coasting" for a frame.
  bool coasting  = (int32_t)(nowMs - t.lastApiMs) > (int32_t)AR_STALE_TRACK_MS;

  uint8_t r, g, bl;
  altColorRGB(t.altFt, r, g, bl);
  lv_color_t col = emergency ? C_RED : lv_color_make(r, g, bl);
  lv_obj_set_style_img_recolor(b.jet, col, 0);
  lv_obj_set_style_img_recolor(b.glow, col, 0);

  int angle = ((int)(t.trackDeg * 10.0f)) % 3600;
  if (angle < 0) angle += 3600;
  lv_img_set_angle(b.jet, angle);

  lv_opa_t jetOpa = coasting ? COAST_OPA : LV_OPA_COVER;
  if (emergency) jetOpa = ((nowMs / BLINK_HALF_MS) & 1) ? BLINK_HI : BLINK_LO;
  lv_obj_set_style_img_opa(b.jet, jetOpa, 0);

  setHidden(b.glow, coasting);
  setHidden(b.selRing, !selected);
  setHidden(b.milBox, !t.mil);
  blipSetLabel(b, t, selected);
}

static void blipRelease(Blip& b) {
  lv_anim_del(b.holder, blipAnimX);
  lv_anim_del(b.holder, blipAnimY);
  lv_obj_add_flag(b.holder, LV_OBJ_FLAG_HIDDEN);
  b.used = false;
  b.hex[0] = '\0';
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
  s_rangeLblFull = makeMicroLabel(s_clip, "", C_FAINT);
  lv_obj_align(s_rangeLblFull, LV_ALIGN_CENTER, RING_LBL_FULL_OFF,
               -RING_LBL_FULL_OFF);
  s_rangeLblMid = makeMicroLabel(s_clip, "", C_FAINT);
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

  s_clip = lv_obj_create(parent);
  lv_obj_remove_style_all(s_clip);
  lv_obj_set_pos(s_clip, SCOPE_X0, SCOPE_Y0);
  lv_obj_set_size(s_clip, SCOPE_D, SCOPE_D);
  lv_obj_set_style_radius(s_clip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(s_clip, true, 0);
  lv_obj_set_style_bg_color(s_clip, C_INK, 0);   // dark floor when no map
  lv_obj_set_style_bg_opa(s_clip, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_clip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_clip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_clip, scopeEventCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_clip, scopeEventCb, LV_EVENT_GESTURE, nullptr);

  s_mapImg = lv_img_create(s_clip);            // src set by scopeApplyMapImage
  lv_obj_set_pos(s_mapImg, 0, 0);
  lv_obj_add_flag(s_mapImg, LV_OBJ_FLAG_HIDDEN);

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
}

void scopeUpdate(uint32_t nowMs) {
  if (!s_clip) return;
  scopeUpdateRangeLabels();

  bool seen[AR_MAX_TRACKS] = {false};
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
    blipStyle(b, t, nowMs);
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
