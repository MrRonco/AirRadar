// ui_cards.cpp — SCR_MAIN cards: Overview, Selected, Time, Settings button,
// weather pill, range pill. Realizes the browser-verified 800x480 mock.
// Loop context only (state.h threading contract): reads g_tracks helpers,
// g_set, g_wx; writes g_timeSynced. All texts cached in static bufs so
// lv_label_set_text is skipped when unchanged (250 ms refresh cadence).
#include <time.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include "ui.h"
#include "../core/tracks.h"
#include "../net/logos.h"

LV_IMG_DECLARE(img_arrowhead);          // 8x10 chevron; not exported via theme.h

// ============================================================
//  Layout constants (content coords — st_card pad_all is 17)
// ============================================================
static const int CONTENT_W    = CARD_W - 34;   // 150 inside a card

// Overview card
static const int OV_HOME_Y    = 20;
static const int OV_HAIR1_Y   = 50;
static const int OV_COUNT_Y   = 56;
static const int OV_INRANGE_DX= 8;             // gap count -> "IN RANGE"
static const int OV_INRANGE_DY= -12;           // lift toward the numeral baseline
static const int OV_HEARD_Y   = 124;
static const int OV_EMERG_Y   = 148;
static const int OV_EMERG_H   = 26;
static const int OV_HAIR2_Y   = 182;
static const int OV_NEAR_Y    = 192;
static const int OV_NEARD_Y   = 212;
static const int OV_FEED_Y    = 236;
static const int OV_SRC_Y     = 262;           // SOURCE row (name · age)
static const int RAMP_W       = 46;            // altitude ramp bars
static const int RAMP_H       = 5;
static const int RAMP_DY      = -16;           // bar offset above its label

// Selected card
static const int SEL_TILE_Y   = 20;
static const int SEL_TILE_S   = 46;
static const int SEL_TEXT_X   = 56;            // callsign/op x when logo tile shown
static const int SEL_OP_Y     = 52;
static const int SEL_ROUTE_Y  = 74;
static const int SEL_ROUTE_H  = 34;
static const int SEL_LINE_X   = 54;            // route connector geometry
static const int SEL_LINE_W   = 28;
static const int SEL_LINE_Y   = 15;
static const int SEL_ARROW_X  = 82;
static const int SEL_ARROW_Y  = 10;
static const int SEL_FRAME_Y  = 112;
static const int SEL_IDENT_Y  = 132;
static const int SEL_HAIR_Y   = 150;
static const int SEL_GRID_Y1  = 158;
static const int SEL_GRID_Y2  = 204;
static const int SEL_VAL_DY   = 14;            // key -> value offset in a grid cell
static const int SEL_COL2_X   = 78;            // second grid column (~(CARD_W-34)/2)
static const int SEL_DIST_Y   = 250;
static const int SEL_SQK_Y    = 274;
static const int SEL_DOT_D    = 7;             // LIVE dot diameter

// Semantics
static const int      FL_TRANSITION_FT = 18000;  // FLxxx display threshold
static const int      CLIMB_STRONG_FPM = 300;    // colored climb/descent
static const uint32_t EMERG_BLINK_MS   = 500;

// Recolor hex strings for the range pill (mirror C_DIM / C_CY in theme.h)
static const char* RECOLOR_DIM = "69757f";
static const char* RECOLOR_CY  = "54dcee";

// ============================================================
//  Widgets + caches
// ============================================================
static bool s_built = false;

// Overview
static lv_obj_t *s_ovCount, *s_ovInRange, *s_ovHeard;
static lv_obj_t *s_ovEmergBox, *s_ovEmergLbl;
static lv_obj_t *s_ovNear, *s_ovNearD, *s_ovFeed, *s_ovSrc;
static char s_bufCount[8], s_bufHeard[24], s_bufEmerg[24];
static char s_bufNear[12], s_bufNearD[24], s_bufFeed[12], s_bufSrc[24];
static lv_color_t s_colSrc = {};
static lv_color_t s_colEmerg = {};
static lv_color_t s_emergDim;                  // computed at build

// Selected
static lv_obj_t *s_selCont, *s_selEmpty;
static lv_obj_t *s_selTile, *s_selIni, *s_selLogoImg, *s_selCallsign, *s_selOp;
static const lv_img_dsc_t* s_selLogoShown = nullptr;   // change cache for the tile
static lv_obj_t *s_selRoute, *s_selOrigin, *s_selDest;
static lv_obj_t *s_selFrame, *s_selIdent, *s_selMil;
static lv_obj_t *s_selAlt, *s_selSpd, *s_selHdg, *s_selClimb;
static lv_obj_t *s_selDist, *s_selSqk, *s_selDot, *s_selLive;
static char s_bufCall[12], s_bufOwnOp[24] = "\x01";   // sentinel forces 1st pass
static char s_bufOrigin[8], s_bufDest[8], s_bufFrame[36], s_bufIdent[28];
static char s_bufAlt[16], s_bufSpd[16], s_bufHdg[16], s_bufClimb[16];
static char s_bufDist[24], s_bufSqk[8], s_bufLive[20];
static lv_color_t s_colAlt = {}, s_colClimb = {}, s_colSqk = {};
static lv_color_t s_colLive = {}, s_colLiveDot = {};

// Time card
static lv_obj_t *s_tmTime, *s_tmDate;
static char s_bufTime[8], s_bufDate[20];

// Weather pill
static lv_obj_t *s_wxPill, *s_wxIcon, *s_wxTemp, *s_wxWind;
static const lv_img_dsc_t* s_wxIconSrc = nullptr;
static char s_bufTemp[8], s_bufWind[32];

// Range pill
static lv_obj_t *s_rngLbl;
static char s_bufRange[48];

// ============================================================
//  Small helpers
// ============================================================
static lv_obj_t* mkLbl(lv_obj_t* p, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

static lv_obj_t* mkMicro(lv_obj_t* p, const char* txt, int x, int y) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_add_style(l, &st_microlbl, 0);
  lv_label_set_text(l, txt);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t* mkBox(lv_obj_t* p) {                 // bare container
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* mkHair(lv_obj_t* p, int y, int w) {
  lv_obj_t* o = mkBox(p);
  lv_obj_add_style(o, &st_hair, 0);
  lv_obj_set_style_bg_opa(o, 22, 0);                  // st_hair opa (bg part)
  lv_obj_set_size(o, w, 1);
  lv_obj_set_pos(o, 0, y);
  return o;
}

// Set label text only when it differs from the cached copy. Returns true if set.
static bool setTextCached(lv_obj_t* lbl, char* cache, size_t cap, const char* txt) {
  if (strcmp(cache, txt) == 0) return false;
  snprintf(cache, cap, "%s", txt);
  lv_label_set_text(lbl, cache);
  return true;
}

static void setColorCached(lv_obj_t* lbl, lv_color_t* cache, lv_color_t c) {
  if (cache->full == c.full) return;
  *cache = c;
  lv_obj_set_style_text_color(lbl, c, 0);
}

static void setHiddenCached(lv_obj_t* o, bool hide) {
  bool cur = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
  if (cur == hide) return;
  if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Trimmed callsign, hex fallback (Overview / emergency strip — full ASCII font).
static void trackDisplayName(const Track* t, char* out, size_t cap) {
  size_t n = 0;
  for (const char* p = t->flight; *p && n + 1 < cap; p++)
    if (*p != ' ') out[n++] = *p;
  if (!n)
    for (const char* p = t->hex; *p && n + 1 < cap; p++)
      out[n++] = (char)toupper((unsigned char)*p);
  out[n] = 0;
}

// F_L28 carries only A-Z 0-9 '-' '.' — sanitize hard, hex fallback.
static void sanitizeCallsign(const Track* t, char* out, size_t cap) {
  size_t n = 0;
  for (const char* p = t->flight; *p && n + 1 < cap; p++) {
    char c = (char)toupper((unsigned char)*p);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')
      out[n++] = c;
  }
  if (!n)
    for (const char* p = t->hex; *p && n + 1 < cap; p++)
      out[n++] = (char)toupper((unsigned char)*p);
  out[n] = 0;
}

static void upCopy(char* dst, size_t cap, const char* src) {
  size_t n = 0;
  for (const char* p = src; *p && n + 1 < cap; p++)
    dst[n++] = (char)toupper((unsigned char)*p);
  dst[n] = 0;
}

// ============================================================
//  Event callbacks
// ============================================================
static void onSettingsClicked(lv_event_t* e) { (void)e; uiShow(SCR_SETTINGS); }
static void onRangeClicked(lv_event_t* e)    { (void)e; uiCycleRange(+1); }

// ============================================================
//  Build — Overview card
// ============================================================
static void mkRampBar(lv_obj_t* card, lv_align_t a, lv_color_t c, const char* txt) {
  lv_obj_t* bar = mkBox(card);
  lv_obj_set_size(bar, RAMP_W, RAMP_H);
  lv_obj_set_style_radius(bar, 3, 0);
  lv_obj_set_style_bg_color(bar, c, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_align(bar, a, 0, RAMP_DY);
  lv_obj_t* l = mkLbl(card, F_MONO11, C_FAINT);
  lv_label_set_text(l, txt);
  lv_obj_align(l, a, 0, 0);
}

static void buildOverview(lv_obj_t* parent) {
  lv_obj_t* card = mkBox(parent);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_pos(card, CARD_L_X, CARD_TOP_Y);
  lv_obj_set_size(card, CARD_W, CARD_TALL_H);

  mkMicro(card, "OVERVIEW", 0, 0);
  lv_obj_t* homeIco = mkLbl(card, F_SYM16, C_CY);
  lv_label_set_text(homeIco, LV_SYMBOL_HOME);
  lv_obj_set_pos(homeIco, 0, OV_HOME_Y);
  lv_obj_t* homeLbl = mkLbl(card, F_M20, C_IVORY);
  lv_label_set_text(homeLbl, "Home");
  lv_obj_set_pos(homeLbl, 24, OV_HOME_Y - 2);
  mkHair(card, OV_HAIR1_Y, CONTENT_W);

  s_ovCount = mkLbl(card, F_NUM56, C_IVORY);
  lv_obj_set_pos(s_ovCount, 0, OV_COUNT_Y);
  s_ovInRange = mkMicro(card, "IN RANGE", 64, OV_COUNT_Y + 38);   // realigned on update
  s_ovHeard = mkLbl(card, F_UI15, C_IVORY2);
  lv_obj_set_pos(s_ovHeard, 0, OV_HEARD_Y);

  s_ovEmergBox = mkBox(card);
  lv_obj_set_pos(s_ovEmergBox, 0, OV_EMERG_Y);
  lv_obj_set_size(s_ovEmergBox, CONTENT_W, OV_EMERG_H);
  lv_obj_set_style_bg_color(s_ovEmergBox, C_RED, 0);
  lv_obj_set_style_bg_opa(s_ovEmergBox, 38, 0);
  lv_obj_set_style_border_color(s_ovEmergBox, C_RED, 0);
  lv_obj_set_style_border_opa(s_ovEmergBox, 128, 0);
  lv_obj_set_style_border_width(s_ovEmergBox, 1, 0);
  lv_obj_set_style_radius(s_ovEmergBox, 6, 0);
  lv_obj_set_style_pad_ver(s_ovEmergBox, 4, 0);
  lv_obj_set_style_pad_hor(s_ovEmergBox, 8, 0);
  s_ovEmergLbl = mkLbl(s_ovEmergBox, F_MONO11, C_RED);
  lv_obj_align(s_ovEmergLbl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_add_flag(s_ovEmergBox, LV_OBJ_FLAG_HIDDEN);
  s_emergDim = lv_color_mix(C_RED, C_CARD_LO, 140);

  mkHair(card, OV_HAIR2_Y, CONTENT_W);
  mkMicro(card, "NEAREST", 0, OV_NEAR_Y);
  s_ovNear = mkLbl(card, F_UI15, C_IVORY);
  lv_obj_align(s_ovNear, LV_ALIGN_TOP_RIGHT, 0, OV_NEAR_Y - 2);
  s_ovNearD = mkLbl(card, F_UI15, C_CY);
  lv_obj_align(s_ovNearD, LV_ALIGN_TOP_RIGHT, 0, OV_NEARD_Y);
  mkMicro(card, "FEED", 0, OV_FEED_Y);
  s_ovFeed = mkLbl(card, F_UI15, C_IVORY);
  lv_obj_align(s_ovFeed, LV_ALIGN_TOP_RIGHT, 0, OV_FEED_Y - 2);
  mkMicro(card, "SOURCE", 0, OV_SRC_Y);
  s_ovSrc = mkLbl(card, F_UI15, C_IVORY2);
  lv_obj_align(s_ovSrc, LV_ALIGN_TOP_RIGHT, 0, OV_SRC_Y - 2);

  mkRampBar(card, LV_ALIGN_BOTTOM_LEFT,  C_AMBER,  "<10K");
  mkRampBar(card, LV_ALIGN_BOTTOM_MID,   C_CY,     "10-30K");
  mkRampBar(card, LV_ALIGN_BOTTOM_RIGHT, C_VIOLET, ">30K");
}

// ============================================================
//  Build — Selected card
// ============================================================
static lv_obj_t* mkGridCell(lv_obj_t* p, int x, int y, const char* key) {
  mkMicro(p, key, x, y);
  lv_obj_t* v = mkLbl(p, F_M20, C_IVORY);
  lv_obj_set_pos(v, x, y + SEL_VAL_DY);
  return v;
}

static void buildSelectedTop(lv_obj_t* cont) {
  s_selTile = mkBox(cont);
  lv_obj_set_pos(s_selTile, 0, SEL_TILE_Y);
  lv_obj_set_size(s_selTile, SEL_TILE_S, SEL_TILE_S);
  lv_obj_set_style_radius(s_selTile, 12, 0);
  lv_obj_set_style_bg_color(s_selTile, C_CARD_HI, 0);
  lv_obj_set_style_bg_opa(s_selTile, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_selTile, lv_color_white(), 0);
  lv_obj_set_style_border_opa(s_selTile, 46, 0);
  lv_obj_set_style_border_width(s_selTile, 1, 0);
  lv_obj_set_style_clip_corner(s_selTile, true, 0);   // logo respects radius 12
  s_selIni = mkLbl(s_selTile, F_M20, lv_color_white());
  lv_obj_align(s_selIni, LV_ALIGN_CENTER, 0, 0);
  s_selLogoImg = lv_img_create(s_selTile);            // real airline logo when cached
  lv_obj_align(s_selLogoImg, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_selLogoImg, LV_OBJ_FLAG_HIDDEN);

  s_selCallsign = mkLbl(cont, F_L28, C_IVORY);
  lv_label_set_long_mode(s_selCallsign, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(s_selCallsign, SEL_TEXT_X, SEL_TILE_Y - 2);
  lv_obj_set_width(s_selCallsign, CONTENT_W - SEL_TEXT_X);
  s_selOp = mkLbl(cont, F_UI12, C_CY);
  lv_label_set_long_mode(s_selOp, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_selOp, SEL_TEXT_X, SEL_OP_Y);
  lv_obj_set_width(s_selOp, CONTENT_W - SEL_TEXT_X);

  s_selRoute = mkBox(cont);
  lv_obj_set_pos(s_selRoute, 0, SEL_ROUTE_Y);
  lv_obj_set_size(s_selRoute, CONTENT_W, SEL_ROUTE_H);
  lv_obj_add_flag(s_selRoute, LV_OBJ_FLAG_HIDDEN);
  s_selOrigin = mkLbl(s_selRoute, F_L28, C_IVORY);
  lv_obj_set_pos(s_selOrigin, 0, 0);
  s_selDest = mkLbl(s_selRoute, F_L28, C_IVORY);
  lv_obj_align(s_selDest, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_t* line = mkBox(s_selRoute);                 // LVGL8 can't fade a grad's
  lv_obj_set_size(line, SEL_LINE_W, 2);               //  opa — solid per spec
  lv_obj_set_pos(line, SEL_LINE_X, SEL_LINE_Y);
  lv_obj_set_style_radius(line, 1, 0);
  lv_obj_set_style_bg_color(line, C_CY_SOFT, 0);
  lv_obj_set_style_bg_opa(line, 140, 0);
  lv_obj_t* arrow = lv_img_create(s_selRoute);
  lv_img_set_src(arrow, &img_arrowhead);
  lv_obj_set_pos(arrow, SEL_ARROW_X, SEL_ARROW_Y);
  lv_obj_set_style_img_recolor(arrow, C_CY_SOFT, 0);
  lv_obj_set_style_img_recolor_opa(arrow, LV_OPA_COVER, 0);
  lv_obj_set_style_img_opa(arrow, 140, 0);

  s_selFrame = mkLbl(cont, F_UI12, C_IVORY2);
  lv_label_set_long_mode(s_selFrame, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_selFrame, 0, SEL_FRAME_Y);
  lv_obj_set_width(s_selFrame, CONTENT_W);
  s_selIdent = mkLbl(cont, F_MONO11, C_DIM);
  lv_obj_set_pos(s_selIdent, 0, SEL_IDENT_Y);
  s_selMil = mkLbl(cont, F_MONO11, C_CY);
  lv_label_set_text(s_selMil, "MIL");
  lv_obj_align(s_selMil, LV_ALIGN_TOP_RIGHT, 0, SEL_IDENT_Y);
  lv_obj_add_flag(s_selMil, LV_OBJ_FLAG_HIDDEN);
}

static void buildSelectedBottom(lv_obj_t* cont) {
  mkHair(cont, SEL_HAIR_Y, CONTENT_W);
  s_selAlt   = mkGridCell(cont, 0,          SEL_GRID_Y1, "ALT");
  s_selSpd   = mkGridCell(cont, SEL_COL2_X, SEL_GRID_Y1, "SPEED");
  s_selHdg   = mkGridCell(cont, 0,          SEL_GRID_Y2, "HEADING");
  s_selClimb = mkGridCell(cont, SEL_COL2_X, SEL_GRID_Y2, "CLIMB");

  mkMicro(cont, "DIST", 0, SEL_DIST_Y);
  s_selDist = mkLbl(cont, F_UI15, C_IVORY);
  lv_obj_align(s_selDist, LV_ALIGN_TOP_RIGHT, 0, SEL_DIST_Y - 2);
  mkMicro(cont, "SQK", 0, SEL_SQK_Y);
  s_selSqk = mkLbl(cont, F_MONO13, C_IVORY2);
  lv_obj_align(s_selSqk, LV_ALIGN_TOP_RIGHT, 0, SEL_SQK_Y - 1);

  s_selDot = mkBox(cont);
  lv_obj_set_size(s_selDot, SEL_DOT_D, SEL_DOT_D);
  lv_obj_set_style_radius(s_selDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_selDot, C_CY, 0);
  lv_obj_set_style_bg_opa(s_selDot, LV_OPA_COVER, 0);
  lv_obj_align(s_selDot, LV_ALIGN_BOTTOM_LEFT, 1, -4);
  s_selLive = mkLbl(cont, F_MONO11, C_CY);
  lv_obj_align(s_selLive, LV_ALIGN_BOTTOM_LEFT, 14, -1);
}

static void buildSelected(lv_obj_t* parent) {
  lv_obj_t* card = mkBox(parent);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_pos(card, CARD_R_X, CARD_TOP_Y);
  lv_obj_set_size(card, CARD_W, CARD_TALL_H);

  mkMicro(card, "SELECTED", 0, 0);
  s_selCont = mkBox(card);
  lv_obj_set_pos(s_selCont, 0, 0);
  lv_obj_set_size(s_selCont, LV_PCT(100), LV_PCT(100));
  buildSelectedTop(s_selCont);
  buildSelectedBottom(s_selCont);
  lv_obj_add_flag(s_selCont, LV_OBJ_FLAG_HIDDEN);

  s_selEmpty = mkLbl(card, F_UI15, C_DIM);
  lv_label_set_text(s_selEmpty, "Tap a target or\nswipe to cycle");
  lv_obj_set_style_text_align(s_selEmpty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_selEmpty, LV_ALIGN_CENTER, 0, 0);
}

// ============================================================
//  Build — bottom row cards + pills
// ============================================================
static void buildTimeCard(lv_obj_t* parent) {
  lv_obj_t* card = mkBox(parent);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_pos(card, CARD_L_X, CARD_BOT_Y);
  lv_obj_set_size(card, CARD_W, CARD_SHORT_H);
  s_tmTime = mkLbl(card, F_NUM36, C_IVORY);
  lv_obj_align(s_tmTime, LV_ALIGN_CENTER, 0, -10);   // breathing room over the date
  s_tmDate = mkLbl(card, F_MONO11, C_DIM);
  lv_obj_set_style_text_letter_space(s_tmDate, 2, 0);
  lv_obj_align(s_tmDate, LV_ALIGN_CENTER, 0, 17);
}

static void buildSettingsBtn(lv_obj_t* parent) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_add_style(btn, &st_card, 0);
  lv_obj_set_pos(btn, CARD_R_X, CARD_BOT_Y);
  lv_obj_set_size(btn, CARD_W, CARD_SHORT_H);
  lv_obj_set_style_bg_color(btn, lv_color_mix(C_CY, C_CARD_HI, 46), 0);
  lv_obj_set_style_bg_grad_color(btn, C_CARD_LO, 0);
  lv_obj_set_style_border_color(btn, C_CY, 0);
  lv_obj_set_style_border_opa(btn, 100, 0);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn, onSettingsClicked, LV_EVENT_CLICKED, NULL);

  lv_obj_t* ico = mkLbl(btn, F_SYM16, C_CY);
  lv_label_set_text(ico, LV_SYMBOL_SETTINGS);
  lv_obj_align(ico, LV_ALIGN_LEFT_MID, 6, 0);
  lv_obj_t* lbl = mkLbl(btn, F_M20, C_IVORY);
  lv_obj_set_style_text_letter_space(lbl, 2, 0);
  lv_label_set_text(lbl, "SETTINGS");
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 34, 0);
}

static void buildWeatherPill(lv_obj_t* parent) {
  s_wxPill = mkBox(parent);
  lv_obj_add_style(s_wxPill, &st_pill, 0);
  lv_obj_set_size(s_wxPill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_wxPill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_wxPill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_wxPill, 10, 0);
  lv_obj_align(s_wxPill, LV_ALIGN_TOP_MID, 0, WX_PILL_Y);

  s_wxIcon = lv_img_create(s_wxPill);
  lv_img_set_src(s_wxIcon, &img_wx_cloud);
  lv_obj_set_style_img_recolor(s_wxIcon, C_IVORY2, 0);
  lv_obj_set_style_img_recolor_opa(s_wxIcon, LV_OPA_COVER, 0);
  s_wxIconSrc = &img_wx_cloud;
  s_wxTemp = mkLbl(s_wxPill, F_M20, C_IVORY);
  lv_obj_t* div = mkBox(s_wxPill);
  lv_obj_set_size(div, 1, 18);
  lv_obj_set_style_bg_color(div, C_BORDER, 0);
  lv_obj_set_style_bg_opa(div, 60, 0);
  lv_obj_t* wind = lv_img_create(s_wxPill);
  lv_img_set_src(wind, &img_wx_wind);
  lv_obj_set_style_img_recolor(wind, C_CY, 0);
  lv_obj_set_style_img_recolor_opa(wind, LV_OPA_COVER, 0);
  s_wxWind = mkLbl(s_wxPill, F_MONO13, C_IVORY2);
  lv_obj_add_flag(s_wxPill, LV_OBJ_FLAG_HIDDEN);
}

static void buildRangePill(lv_obj_t* parent) {
  lv_obj_t* pill = mkBox(parent);
  lv_obj_add_style(pill, &st_pill, 0);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -(SCR_H - RNG_PILL_Y - 24));
  lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(pill, onRangeClicked, LV_EVENT_CLICKED, NULL);
  s_rngLbl = mkLbl(pill, F_MONO13, C_DIM);
  lv_label_set_recolor(s_rngLbl, true);
}

// ============================================================
//  Update — Overview
// ============================================================
static void updateEmergencyStrip(uint32_t nowMs) {
  Track* e = tracksFirstEmergency();
  setHiddenCached(s_ovEmergBox, e == nullptr);
  if (!e) return;
  char nm[12];
  trackDisplayName(e, nm, sizeof(nm));
  char b[24];
  snprintf(b, sizeof(b), "! %s  %.7s", e->squawk, nm);
  setTextCached(s_ovEmergLbl, s_bufEmerg, sizeof(s_bufEmerg), b);
  bool on = (nowMs / EMERG_BLINK_MS) & 1;
  setColorCached(s_ovEmergLbl, &s_colEmerg, on ? C_RED : s_emergDim);
}

static void updateOverview(uint32_t nowMs) {
  char b[24];
  snprintf(b, sizeof(b), "%d", g_orderN);
  if (setTextCached(s_ovCount, s_bufCount, sizeof(s_bufCount), b)) {
    lv_obj_update_layout(s_ovCount);       // fresh width before manual re-align
    lv_obj_align_to(s_ovInRange, s_ovCount, LV_ALIGN_OUT_RIGHT_BOTTOM,
                    OV_INRANGE_DX, OV_INRANGE_DY);
  }
  snprintf(b, sizeof(b), "of %d heard", g_heardCount);
  setTextCached(s_ovHeard, s_bufHeard, sizeof(s_bufHeard), b);

  updateEmergencyStrip(nowMs);

  Track* n = tracksNearest();
  if (n) {
    char nm[12];
    trackDisplayName(n, nm, sizeof(nm));
    setTextCached(s_ovNear, s_bufNear, sizeof(s_bufNear), nm);
    float d = haversineKm(g_set.homeLat, g_set.homeLon, n->lat, n->lon);
    float brg = bearingTo(g_set.homeLat, g_set.homeLon, n->lat, n->lon);
    snprintf(b, sizeof(b), "%.1f km %s", (double)d, cardinal8(brg));
    setTextCached(s_ovNearD, s_bufNearD, sizeof(s_bufNearD), b);
  } else {
    setTextCached(s_ovNear, s_bufNear, sizeof(s_bufNear), "--");
    setTextCached(s_ovNearD, s_bufNearD, sizeof(s_bufNearD), "");
  }

  if (g_feedIsLocal && g_feedMsgRate >= 0.0f)
    snprintf(b, sizeof(b), "%d/s", (int)(g_feedMsgRate + 0.5f));
  else
    snprintf(b, sizeof(b), "--");
  setTextCached(s_ovFeed, s_bufFeed, sizeof(s_bufFeed), b);

  // SOURCE row: where the data is coming from + how fresh it is.
  lv_color_t sc;
  int32_t ageS = g_lastGoodApply
                     ? (int32_t)(nowMs - g_lastGoodApply) / 1000 : -1;
  if (ageS < 0) ageS = 0;
  if (!g_wifiUp) {
    snprintf(b, sizeof(b), "OFFLINE");
    sc = C_RED;
  } else if (!g_lastGoodApply ||
             (int32_t)(nowMs - g_lastGoodApply) > (int32_t)AR_STALE_FEED_MS) {
    snprintf(b, sizeof(b), "STALE");
    sc = C_AMBER;
  } else if (g_feedIsLocal) {
    snprintf(b, sizeof(b), "%s · %lds", g_localSrcName, (long)ageS);
    sc = C_CY;
  } else {
    snprintf(b, sizeof(b), "CLOUD · %lds", (long)ageS);
    sc = C_IVORY2;
  }
  setTextCached(s_ovSrc, s_bufSrc, sizeof(s_bufSrc), b);
  if (s_colSrc.full != sc.full) {
    s_colSrc = sc;
    lv_obj_set_style_text_color(s_ovSrc, sc, 0);
  }
}

// ============================================================
//  Update — Selected
// ============================================================
static void selApplyLogoLayout(bool logoEn) {
  static int applied = -1;
  if (applied == (int)logoEn) return;
  applied = (int)logoEn;
  int x = logoEn ? SEL_TEXT_X : 0;
  setHiddenCached(s_selTile, !logoEn);
  lv_obj_set_pos(s_selCallsign, x, SEL_TILE_Y - 2);
  lv_obj_set_width(s_selCallsign, CONTENT_W - x);
  lv_obj_set_pos(s_selOp, x, SEL_OP_Y);
  lv_obj_set_width(s_selOp, CONTENT_W - x);
}

static void updateSelectedIdentity(const Track* t) {
  char cs[12];
  sanitizeCallsign(t, cs, sizeof(cs));
  // Adaptive size: >=6 chars overflow the tile row at 28px ("HUSK28" showed
  // as "HUSK2"), so long callsigns drop to the 20px face and stay whole.
  static const lv_font_t* csFont = nullptr;
  const lv_font_t* want = (strlen(cs) >= 6) ? F_M20 : F_L28;
  if (want != csFont) {
    csFont = want;
    lv_obj_set_style_text_font(s_selCallsign, want, 0);
  }
  setTextCached(s_selCallsign, s_bufCall, sizeof(s_bufCall), cs);

  // Cache key covers ownOp AND callsign — with no operator the initials come
  // from the callsign, which changes per selection.
  char opKey[24];
  snprintf(opKey, sizeof(opKey), "%.12s|%.8s", t->ownOp, t->flight);
  if (strncmp(s_bufOwnOp, opKey, sizeof(s_bufOwnOp)) != 0) {
    snprintf(s_bufOwnOp, sizeof(s_bufOwnOp), "%s", opKey);
    lv_label_set_text(s_selOp, t->ownOp);
    char ini[3];
    lv_color_t bg;
    if (t->ownOp[0]) {
      monogramFor(t->ownOp, ini, &bg);
    } else {
      // No operator in this feed: first two letters of the callsign on a
      // neutral slate tile (never the red "--" tombstone).
      int n = 0;
      ini[0] = ini[1] = 0; ini[2] = 0;
      for (const char* p = t->flight; *p && n < 2; p++)
        if (isalpha((unsigned char)*p)) ini[n++] = toupper((unsigned char)*p);
      if (!n) { ini[0] = 'A'; ini[1] = 'C'; }   // last resort: AirCraft
      bg = lv_color_hex(0x33475c);
    }
    lv_label_set_text(s_selIni, ini);
    lv_obj_set_style_bg_color(s_selTile, bg, 0);
  }

  bool hasRoute = t->origin[0] && t->dest[0];
  setHiddenCached(s_selRoute, !hasRoute);
  if (hasRoute) {
    char code[8];
    upCopy(code, sizeof(code), t->origin);
    setTextCached(s_selOrigin, s_bufOrigin, sizeof(s_bufOrigin), code);
    upCopy(code, sizeof(code), t->dest);
    setTextCached(s_selDest, s_bufDest, sizeof(s_bufDest), code);
  }

  setTextCached(s_selFrame, s_bufFrame, sizeof(s_bufFrame),
                t->desc[0] ? t->desc : t->typeCode);

  char b[28];
  if (t->reg[0] && t->year[0])
    snprintf(b, sizeof(b), "%s \xC2\xB7 %s", t->reg, t->year);
  else if (t->reg[0])
    snprintf(b, sizeof(b), "%s", t->reg);
  else if (t->year[0])
    snprintf(b, sizeof(b), "%s", t->year);
  else
    b[0] = 0;
  setTextCached(s_selIdent, s_bufIdent, sizeof(s_bufIdent), b);
  setHiddenCached(s_selMil, !t->mil);
}

static void updateSelectedGrid(const Track* t) {
  char b[16];
  if (t->altFt >= FL_TRANSITION_FT)
    snprintf(b, sizeof(b), "FL%03d", t->altFt / 100);
  else if (t->altFt >= 0)
    snprintf(b, sizeof(b), "%d ft", t->altFt);
  else
    snprintf(b, sizeof(b), "---");
  setTextCached(s_selAlt, s_bufAlt, sizeof(s_bufAlt), b);
  uint8_t r, g, bl;
  altColorRGB(t->altFt, r, g, bl);
  setColorCached(s_selAlt, &s_colAlt, lv_color_make(r, g, bl));

  snprintf(b, sizeof(b), "%d kt", (int)t->gsKt);
  setTextCached(s_selSpd, s_bufSpd, sizeof(s_bufSpd), b);

  int hd = (((int)t->trackDeg) % 360 + 360) % 360;
  snprintf(b, sizeof(b), "%03d\xC2\xB0 %s", hd, cardinal8(t->trackDeg));
  setTextCached(s_selHdg, s_bufHdg, sizeof(s_bufHdg), b);

  snprintf(b, sizeof(b), "%+d", t->vRateFpm);
  setTextCached(s_selClimb, s_bufClimb, sizeof(s_bufClimb), b);
  lv_color_t cc = (t->vRateFpm > CLIMB_STRONG_FPM)    ? C_CY
                  : (t->vRateFpm < -CLIMB_STRONG_FPM) ? C_AMBER
                                                      : C_IVORY;
  setColorCached(s_selClimb, &s_colClimb, cc);
}

static void updateSelectedStatus(const Track* t, uint32_t nowMs) {
  char b[24];
  float d = haversineKm(g_set.homeLat, g_set.homeLon, t->lat, t->lon);
  float brg = bearingTo(g_set.homeLat, g_set.homeLon, t->lat, t->lon);
  snprintf(b, sizeof(b), "%.1f km %s", (double)d, cardinal8(brg));
  setTextCached(s_selDist, s_bufDist, sizeof(s_bufDist), b);

  bool emerg = sqIsEmergency(t->squawk);
  setTextCached(s_selSqk, s_bufSqk, sizeof(s_bufSqk),
                t->squawk[0] ? t->squawk : "----");
  setColorCached(s_selSqk, &s_colSqk, emerg ? C_RED : C_IVORY2);

  // Signed: applyPending may stamp lastApiMs a hair after our nowMs — the
  // unsigned difference would underflow into COAST + a 4-billion-second age.
  int32_t ageMs = (int32_t)(nowMs - t->lastApiMs);
  if (ageMs < 0) ageMs = 0;
  bool coasting = ageMs > (int32_t)AR_STALE_TRACK_MS;
  uint32_t age = (uint32_t)ageMs / 1000U;
  snprintf(b, sizeof(b), "%s \xC2\xB7 %lus", coasting ? "COAST" : "LIVE",
           (unsigned long)age);
  setTextCached(s_selLive, s_bufLive, sizeof(s_bufLive), b);
  lv_color_t lc = coasting ? C_AMBER : C_CY;
  setColorCached(s_selLive, &s_colLive, lc);
  if (s_colLiveDot.full != lc.full) {
    s_colLiveDot = lc;
    lv_obj_set_style_bg_color(s_selDot, lc, 0);
  }
}

// Real airline logo: 3-letter ICAO callsign prefix (letters + digit pattern,
// e.g. JZA238) drives a lazy CDN fetch; the monogram stays until/unless it lands.
static void updateSelLogo(const Track* t) {
  const lv_img_dsc_t* want = nullptr;
  if (g_set.logoEn && t) {
    char icao[4];
    if (logosIcaoFromFlight(t->flight, icao)) {
      const lv_img_dsc_t* d = nullptr;
      LogoState st = logosGet(icao, &d);
      if (st == LOGO_UNKNOWN) logosRequest(icao);
      else if (st == LOGO_OK) want = d;
    }
  }
  if (want != s_selLogoShown) {
    s_selLogoShown = want;
    if (want) {
      lv_img_set_src(s_selLogoImg, want);
      lv_obj_clear_flag(s_selLogoImg, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_selIni, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_selLogoImg, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_selIni, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void updateSelected(uint32_t nowMs) {
  Track* t = tracksSelected();
  setHiddenCached(s_selCont, t == nullptr);
  setHiddenCached(s_selEmpty, t != nullptr);
  if (!t) return;
  selApplyLogoLayout(g_set.logoEn);
  updateSelectedIdentity(t);
  updateSelLogo(t);
  updateSelectedGrid(t);
  updateSelectedStatus(t, nowMs);
}

// ============================================================
//  Update — time card, weather pill, range pill
// ============================================================
static void updateTimeCard() {
  // Non-blocking clock: time(nullptr) is free, unlike getLocalTime()'s wait
  // loop which would burn its full timeout every 250 ms tick before NTP sync.
  time_t tt = time(nullptr);
  struct tm ti;
  localtime_r(&tt, &ti);
  if (ti.tm_year <= 120) {                     // not synced yet (or offline)
    // F_NUM36 carries only digits+':' — "0:00" stays inside the subset;
    // the sync hint lives on the mono date label instead.
    setTextCached(s_tmTime, s_bufTime, sizeof(s_bufTime), "0:00");
    setTextCached(s_tmDate, s_bufDate, sizeof(s_bufDate), "WAITING FOR TIME");
    return;
  }
  if (!g_timeSynced) g_timeSynced = true;      // loop context: allowed write

  int h = ti.tm_hour % 12;
  if (h == 0) h = 12;
  char b[8];
  snprintf(b, sizeof(b), "%d:%02d", h, ti.tm_min);
  setTextCached(s_tmTime, s_bufTime, sizeof(s_bufTime), b);

  char d[20];
  strftime(d, sizeof(d), "%a \xC2\xB7 %b %d", &ti);
  for (char* p = d; *p; p++)
    if ((unsigned char)*p < 0x80) *p = (char)toupper((unsigned char)*p);
  setTextCached(s_tmDate, s_bufDate, sizeof(s_bufDate), d);
}

static void updateWeatherPill() {
  bool show = g_set.wxEn && g_wx.valid;
  setHiddenCached(s_wxPill, !show);
  if (!show) return;

  const lv_img_dsc_t* ic = wxIconFor(g_wx.wmoCode);
  if (ic != s_wxIconSrc) {
    s_wxIconSrc = ic;
    lv_img_set_src(s_wxIcon, ic);
  }
  char b[32];
  snprintf(b, sizeof(b), "%d\xC2\xB0", (int)lroundf(g_wx.tempC));
  setTextCached(s_wxTemp, s_bufTemp, sizeof(s_bufTemp), b);
  snprintf(b, sizeof(b), "%s %d \xC2\xB7 %s", cardinal8((float)g_wx.windDirDeg),
           (int)lroundf(g_wx.windKmh), wxWordFor(g_wx.wmoCode));
  setTextCached(s_wxWind, s_bufWind, sizeof(s_bufWind), b);
}

static void updateRangePill() {
  char b[48];
  snprintf(b, sizeof(b),
           "#%s \xE2\x80\xB9#  #%s %d KM#  #%s \xE2\x80\xBA#",
           RECOLOR_DIM, RECOLOR_CY, g_set.rangeKm, RECOLOR_DIM);
  setTextCached(s_rngLbl, s_bufRange, sizeof(s_bufRange), b);
}

// ============================================================
//  Public API
// ============================================================
void cardsBuild(lv_obj_t* parent) {
  if (s_built) {
    Serial.println("[cards] cardsBuild called twice - ignored");
    return;
  }
  if (!parent) {
    Serial.println("[cards] cardsBuild: null parent");
    return;
  }
  buildOverview(parent);
  buildSelected(parent);
  buildTimeCard(parent);
  buildSettingsBtn(parent);
  buildWeatherPill(parent);
  buildRangePill(parent);
  s_built = true;
  Serial.println("[cards] built");
  cardsUpdate(millis());                       // populate initial texts
}

void cardsUpdate(uint32_t nowMs) {
  if (!s_built) return;
  updateOverview(nowMs);
  updateSelected(nowMs);
  updateTimeCard();
  updateWeatherPill();
  updateRangePill();
}
