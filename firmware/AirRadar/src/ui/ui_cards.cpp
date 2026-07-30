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

// ============================================================
//  Layout constants (content coords — st_card pad_all is 17)
// ============================================================
static const int CONTENT_W    = CARD_W - 32;   // st_card pad_all is 16

// Overview card
static const int OV_HOME_Y    = 0;             // weather row
static const int OV_DATE_Y    = 34;
static const int OV_WIND_W    = 80;   // wind glyph + "NW 13" at 18 px
static const int OV_HAIR1_Y   = 62;
static const int OV_COUNT_Y   = 90;            // hero numeral
static const int OV_INRANGE_Y = 154;           // stacked UNDER the numeral
static const int OV_COAST_Y   = 174;           // same face as IN RANGE
static const int OV_EMERG_Y   = 194;
static const int OV_EMERG_H   = 26;
static const int OV_HAIR2_Y   = 220;
static const int OV_NEAR_Y    = 230;           // "NEAREST" key
static const int OV_NEARNAME_Y= 250;           // callsign, larger
static const int OV_NEARD_Y   = 278;           // distance, left, lighter face
static const int OV_HAIR3_Y   = 310;
static const int OV_FEED_Y    = 262;
static const int OV_SRC_Y     = 322;           // single status row
static const int OV_DOT_D     = 7;             // live dot
static const int RAMP_W       = 46;            // altitude ramp bars
static const int RAMP_H       = 5;
static const int RAMP_DY      = -16;           // bar offset above its label

// Selected card
static const int SEL_TILE_Y   = 2;
static const int SEL_TILE_S   = 36;
// The cached logo bitmap is rendered at exactly this size; if they drift the
// image overflows the tile and clip_corner shaves its edges.
static_assert(SEL_TILE_S == LOGO_PX, "logo bitmap must match the tile size");
static const int SEL_TEXT_X   = 42;            // callsign/op x when logo tile shown
static const int SEL_OP_Y     = 26;
static const int SEL_ROUTE_Y  = 66;
static const int SEL_ROUTE_H  = 32;
static const int SEL_FRAME_Y  = 108;
static const int SEL_IDENT_Y  = 130;
static const int SEL_HAIR_Y   = 154;
static const int SEL_GRID_Y1  = 164;
static const int SEL_GRID_Y2  = 212;
static const int SEL_VAL_DY   = 14;            // key -> value offset in a grid cell
static const int SEL_COL2_X   = 72;            // two 64 px columns, 8 px gutter
static const int SEL_GRID_Y3  = 260;           // DIST / SQK, same grid as above
static const int SEL_HAIR2_Y  = 308;           // separates the status line
static const int SEL_LIVE_Y   = 320;
static const int SEL_DOT_D    = 7;             // LIVE dot diameter

// Semantics
static const int      FL_TRANSITION_FT = 10000;  // FLxxx above this; keeps values <=5 glyphs
static const int      CLIMB_STRONG_FPM = 300;    // colored climb/descent
static const uint32_t EMERG_BLINK_MS   = 500;

// Recolor hex strings for the range pill (mirror C_DIM / C_CY in theme.h)
static const char* RECOLOR_DIM = "8e9baa";
static const char* RECOLOR_VAL = "aab4c0";   // C_IVORY2, was cyan

// ============================================================
//  Widgets + caches
// ============================================================
static bool s_built = false;

// Overview
static lv_obj_t *s_ovCount, *s_ovInRange, *s_ovHeard;
static lv_obj_t *s_ovEmergBox, *s_ovEmergLbl;
static lv_obj_t *s_ovNear, *s_ovNearD, *s_ovFeed, *s_ovSrc, *s_ovDot;
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
static lv_color_t s_colOrigin = {}, s_colDest = {};   // dimmed while unresolved

// Time card
static lv_obj_t *s_tmTime, *s_tmDate;
static char s_bufTime[8], s_bufDate[20];

// Weather pill
static lv_obj_t *s_wxIcon, *s_wxTemp, *s_wxWind, *s_wxWindBox;
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
  lv_obj_set_style_bg_opa(o, OPA_BORDER, 0);          // matches the card hairline
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
static void buildOverview(lv_obj_t* parent) {
  lv_obj_t* card = mkBox(parent);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_pos(card, CARD_L_X, CARD_TOP_Y);
  lv_obj_set_size(card, CARD_W, CARD_TALL_H);

  // Weather heads the card. It used to float in a pill on the vertical axis,
  // directly over the map and on top of the compass N. "Home" is gone: it once
  // meant "local feed vs cloud", and that lives in the SOURCE row below.
  s_wxIcon = lv_img_create(card);
  lv_img_set_src(s_wxIcon, wxIconFor(0));
  lv_obj_set_pos(s_wxIcon, 0, OV_HOME_Y + 2);
  s_wxTemp = mkLbl(card, F_M20, C_IVORY);
  lv_label_set_text(s_wxTemp, "--");
  lv_obj_set_pos(s_wxTemp, 30, OV_HOME_Y);
  // Fixed width, not SIZE_CONTENT: a content-sized flex box under-measures
  // here and clips from the left (the old "DHCP" -> "CP" bug in settings).
  s_wxWindBox = mkBox(card);
  lv_obj_set_size(s_wxWindBox, OV_WIND_W, 22);   // the glyph is 22 px tall
  lv_obj_set_pos(s_wxWindBox, CONTENT_W - OV_WIND_W, OV_HOME_Y + 2);
  lv_obj_set_flex_flow(s_wxWindBox, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_wxWindBox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_wxWindBox, 4, 0);
  lv_obj_t* wico = lv_img_create(s_wxWindBox);
  lv_img_set_src(wico, &img_wx_wind);
  lv_obj_set_style_img_recolor(wico, C_IVORY2, 0);
  lv_obj_set_style_img_recolor_opa(wico, LV_OPA_COVER, 0);
  s_wxWind = mkLbl(s_wxWindBox, F_UI15, C_IVORY2);
  lv_label_set_text(s_wxWind, "");
  s_tmDate = mkLbl(card, F_MONO13, C_DIM);
  lv_obj_set_style_text_letter_space(s_tmDate, 1, 0);
  lv_obj_align(s_tmDate, LV_ALIGN_TOP_MID, 0, OV_DATE_Y);
  mkHair(card, OV_HAIR1_Y, CONTENT_W);

  s_ovCount = mkLbl(card, F_NUM56, C_IVORY);
  lv_obj_set_pos(s_ovCount, 0, OV_COUNT_Y);
  s_ovInRange = mkMicro(card, "IN RANGE", 0, OV_INRANGE_Y);
  // Same face as IN RANGE — they are two readings of one count, so they should
  // not look like different kinds of thing.
  s_ovHeard = mkMicro(card, "", 0, OV_COAST_Y);
  lv_obj_set_style_text_color(s_ovHeard, C_MUTE, 0);

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
  s_ovNear = mkLbl(card, F_M20, C_IVORY);          // identifier, larger
  lv_obj_set_pos(s_ovNear, 0, OV_NEARNAME_Y);
  s_ovNearD = mkLbl(card, F_UI15, C_IVORY2);       // distance, lighter face
  lv_obj_set_pos(s_ovNearD, 0, OV_NEARD_Y);
  mkHair(card, OV_HAIR3_Y, CONTENT_W);             // separates the status line
  // One status line: a live dot, the source and its age on the left, message
  // rate on the right. Two labelled rows for what is really one fact.
  s_ovDot = mkBox(card);
  lv_obj_set_size(s_ovDot, OV_DOT_D, OV_DOT_D);
  lv_obj_set_style_radius(s_ovDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_ovDot, C_CY, 0);
  lv_obj_set_style_bg_opa(s_ovDot, LV_OPA_COVER, 0);
  s_ovSrc = mkLbl(card, F_MONO13, C_DIM);   // matches the rate on the right
  lv_obj_set_pos(s_ovSrc, OV_DOT_D + 6, OV_SRC_Y);
  lv_obj_align_to(s_ovDot, s_ovSrc, LV_ALIGN_OUT_LEFT_MID, -6, 0);
  s_ovFeed = mkLbl(card, F_MONO13, C_DIM);
  lv_obj_align(s_ovFeed, LV_ALIGN_TOP_RIGHT, 0, OV_SRC_Y);

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

  // F_L28 needed ~102 px for a 6-glyph callsign in an 88 px slot, so "ACA306"
  // lost its last character. F_M20 fits 7 glyphs with room to spare.
  s_selCallsign = mkLbl(cont, F_M20, C_IVORY);
  lv_label_set_long_mode(s_selCallsign, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(s_selCallsign, SEL_TEXT_X, SEL_TILE_Y - 2);
  lv_obj_set_width(s_selCallsign, CONTENT_W - SEL_TEXT_X);
  s_selOp = mkLbl(cont, F_UI12, C_DIM);
  lv_label_set_long_mode(s_selOp, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_selOp, SEL_TEXT_X, SEL_OP_Y);
  lv_obj_set_width(s_selOp, CONTENT_W - SEL_TEXT_X);

  s_selRoute = mkBox(cont);
  lv_obj_set_pos(s_selRoute, 0, SEL_ROUTE_Y);
  lv_obj_set_size(s_selRoute, CONTENT_W, SEL_ROUTE_H);
  // Centred flex row. Pinning origin hard-left and destination hard-right left
  // the arrow floating with a much bigger gap on one side than the other.
  lv_obj_set_flex_flow(s_selRoute, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_selRoute, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_selRoute, 7, 0);
  s_selOrigin = mkLbl(s_selRoute, F_M20, C_IVORY);
  lv_obj_t* arrow = mkLbl(s_selRoute, F_SYM12, C_DIM);   // smaller triangle
  lv_label_set_text(arrow, LV_SYMBOL_PLAY);
  s_selDest = mkLbl(s_selRoute, F_M20, C_IVORY);

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
  s_selAlt   = mkGridCell(cont, 0,          SEL_GRID_Y1, "ALT ft");
  s_selSpd   = mkGridCell(cont, SEL_COL2_X, SEL_GRID_Y1, "SPD kt");
  s_selHdg   = mkGridCell(cont, 0,          SEL_GRID_Y2, "HDG");
  s_selClimb = mkGridCell(cont, SEL_COL2_X, SEL_GRID_Y2, "V/S fpm");

  // Same two-column grid as ALT/SPD and HDG/V-S, then a rule before the status
  // line — previously these were full-width label/value rows in a different
  // style and the status line ran straight on with nothing separating it.
  s_selDist = mkGridCell(cont, 0,          SEL_GRID_Y3, "DIST km");
  s_selSqk  = mkGridCell(cont, SEL_COL2_X, SEL_GRID_Y3, "SQK");
  mkHair(cont, SEL_HAIR2_Y, CONTENT_W);

  s_selDot = mkBox(cont);
  lv_obj_set_size(s_selDot, SEL_DOT_D, SEL_DOT_D);
  lv_obj_set_style_radius(s_selDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_selDot, C_CY, 0);
  lv_obj_set_style_bg_opa(s_selDot, LV_OPA_COVER, 0);
  s_selLive = mkLbl(cont, F_MONO13, C_DIM);
  lv_obj_set_pos(s_selLive, 14, SEL_LIVE_Y);
  lv_obj_align_to(s_selDot, s_selLive, LV_ALIGN_OUT_LEFT_MID, -6, 0);
}

static void buildSelected(lv_obj_t* parent) {
  lv_obj_t* card = mkBox(parent);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_pos(card, CARD_R_X, CARD_TOP_Y);
  lv_obj_set_size(card, CARD_W, CARD_TALL_H);

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
  lv_obj_set_pos(card, CLOCK_X, CARD_BOT_Y);
  lv_obj_set_size(card, CARD_W, CARD_SHORT_H);
  s_tmTime = mkLbl(card, F_NUM36, C_IVORY);
  lv_obj_center(s_tmTime);      // the date moved into the Overview header
}

static void buildSettingsBtn(lv_obj_t* parent) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  // No plate at all now — just the glyph, top-right of the screen. The old
  // 184x66 lit slab measured 7.26x denser and 14.7x brighter than the entire
  // radar disc; even the 52 px boxed version still read as a panel.
  lv_obj_set_pos(btn, GEAR_X, GEAR_Y);
  lv_obj_set_size(btn, GEAR_S, GEAR_S);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  // Small glyph, full-size target: the drawn icon is 26 px but the hit area is
  // 48 px, which is the ~9 mm floor for this panel.
  lv_obj_set_ext_click_area(btn, GEAR_TOUCH_PAD);
  lv_obj_add_event_cb(btn, onSettingsClicked, LV_EVENT_CLICKED, NULL);
  lv_obj_t* ico = mkLbl(btn, F_SYM16, C_DIM);
  lv_label_set_text(ico, LV_SYMBOL_SETTINGS);
  lv_obj_center(ico);
  lv_obj_set_style_text_color(ico, C_CY, LV_STATE_PRESSED);
}

static void buildRangePill(lv_obj_t* parent) {
  lv_obj_t* pill = mkBox(parent);
  lv_obj_add_style(pill, &st_card, 0);      // same fill/hairline as the panels
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_set_size(pill, CARD_W, CARD_SHORT_H);
  lv_obj_set_pos(pill, CARD_L_X, RNG_PILL_Y);
  lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(pill, C_SURF_HI, LV_STATE_PRESSED);
  lv_obj_add_event_cb(pill, onRangeClicked, LV_EVENT_CLICKED, NULL);
  s_rngLbl = mkLbl(pill, F_MONO13, C_DIM);
  lv_label_set_recolor(s_rngLbl, true);
  lv_obj_center(s_rngLbl);
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
  setTextCached(s_ovCount, s_bufCount, sizeof(s_bufCount), b);
  // "N IN RANGE / of M heard" compared a 60 s coast window against a single
  // poll, so the two numbers openly disagreed. Name the difference instead:
  // the gap IS the coasting set. Row collapses when nothing is coasting.
  int coasting = 0;
  for (int i = 0; i < g_orderN; i++) {
    const Track& t = g_tracks[g_orderIdx[i]];
    if ((int32_t)(nowMs - t.lastApiMs) > (int32_t)AR_STALE_TRACK_MS) coasting++;
  }
  setHiddenCached(s_ovHeard, coasting == 0);
  if (coasting > 0) {
    snprintf(b, sizeof(b), "%d COASTING", coasting);
    setTextCached(s_ovHeard, s_bufHeard, sizeof(s_bufHeard), b);
  }

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
    snprintf(b, sizeof(b), "");
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
    sc = C_DIM;      // quiet: the dot carries the state
  } else {
    snprintf(b, sizeof(b), "CLOUD · %lds", (long)ageS);
    sc = C_DIM;
  }
  setTextCached(s_ovSrc, s_bufSrc, sizeof(s_bufSrc), b);
  if (s_colSrc.full != sc.full) {
    s_colSrc = sc;
    lv_obj_set_style_text_color(s_ovSrc, sc, 0);
    // Dot keeps full state colour even when the text has gone quiet.
    lv_color_t dc = (sc.full == C_DIM.full)
                        ? (g_feedIsLocal ? C_CY : C_IVORY2) : sc;
    lv_obj_set_style_bg_color(s_ovDot, dc, 0);
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
  const lv_font_t* want = (strlen(cs) >= 7) ? F_UI15 : F_M20;
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

  // The row stays visible with "--" placeholders rather than vanishing: a row
  // that appears and disappears shifted everything below it, and a blank
  // reads as "no data" when the truth is usually "lookup still pending".
  char code[8];
  upCopy(code, sizeof(code), t->origin[0] ? t->origin : "--");
  setTextCached(s_selOrigin, s_bufOrigin, sizeof(s_bufOrigin), code);
  upCopy(code, sizeof(code), t->dest[0] ? t->dest : "--");
  setTextCached(s_selDest, s_bufDest, sizeof(s_bufDest), code);
  setColorCached(s_selOrigin, &s_colOrigin, t->origin[0] ? C_IVORY : C_DIM);
  setColorCached(s_selDest,   &s_colDest,   t->dest[0]   ? C_IVORY : C_DIM);

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
  // Units live in the key. "489 kt" was 82.6 px in a 64 px cell and clipped to
  // "489 k"; "216° SW" ran into the next column's "+64".
  if (t->altFt >= FL_TRANSITION_FT)
    snprintf(b, sizeof(b), "FL%03d", t->altFt / 100);
  else if (t->altFt >= 0)
    snprintf(b, sizeof(b), "%d", t->altFt);
  else
    snprintf(b, sizeof(b), "---");
  setTextCached(s_selAlt, s_bufAlt, sizeof(s_bufAlt), b);
  uint8_t r, g, bl;
  altColorRGB(t->altFt, r, g, bl);
  setColorCached(s_selAlt, &s_colAlt, lv_color_make(r, g, bl));

  snprintf(b, sizeof(b), "%d", (int)t->gsKt);
  setTextCached(s_selSpd, s_bufSpd, sizeof(s_bufSpd), b);

  int hd = (((int)t->trackDeg) % 360 + 360) % 360;
  snprintf(b, sizeof(b), "%03d\xC2\xB0", hd);   // cardinal dropped: it never fit
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
  // 22 px tabular digits are 14.25 px, so five glyphs ("119.9") would fill the
  // 64 px cell exactly. Past 100 km a tenth of a kilometre is noise anyway.
  if (d >= 100.0f) snprintf(b, sizeof(b), "%d", (int)(d + 0.5f));
  else             snprintf(b, sizeof(b), "%.1f", (double)d);
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
  lv_color_t lc = coasting ? C_AMBER : C_DIM;   // text matches the keys
  lv_color_t ld = coasting ? C_AMBER : C_CY;    // dot keeps the state
  setColorCached(s_selLive, &s_colLive, lc);
  if (s_colLiveDot.full != ld.full) {
    s_colLiveDot = ld;
    lv_obj_set_style_bg_color(s_selDot, ld, 0);
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
  // The pill is gone; these three widgets are rows in the Overview card now, so
  // hide them individually. (s_wxPill no longer exists — calling
  // setHiddenCached on it would dereference nullptr.)
  bool show = g_set.wxEn && g_wx.valid;
  setHiddenCached(s_wxIcon, !show);
  setHiddenCached(s_wxTemp, !show);
  setHiddenCached(s_wxWindBox, !show);
  if (!show) return;

  const lv_img_dsc_t* ic = wxIconFor(g_wx.wmoCode);
  if (ic != s_wxIconSrc) {
    s_wxIconSrc = ic;
    lv_img_set_src(s_wxIcon, ic);
  }
  char b[32];
  snprintf(b, sizeof(b), "%d\xC2\xB0", (int)lroundf(g_wx.tempC));
  setTextCached(s_wxTemp, s_bufTemp, sizeof(s_bufTemp), b);
  snprintf(b, sizeof(b), "%s %d", cardinal8((float)g_wx.windDirDeg),
           (int)lroundf(g_wx.windKmh));
  setTextCached(s_wxWind, s_bufWind, sizeof(s_bufWind), b);
}

static void updateRangePill() {
  char b[48];
  snprintf(b, sizeof(b),
           "#%s \xE2\x80\xB9#  #%s %d KM#  #%s \xE2\x80\xBA#",
           RECOLOR_DIM, RECOLOR_VAL, g_set.rangeKm, RECOLOR_DIM);
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
