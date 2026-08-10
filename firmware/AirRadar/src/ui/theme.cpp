// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// theme.cpp — fonts, shared styles, weather mapping, operator monogram.
#include "theme.h"
#include <string.h>
#include <ctype.h>

#if AR_USE_INTER_FONTS
// Six faces, down from eight. The retired sizes (11/12/13/15/20) sat inside an
// 18% band and were invisible as hierarchy steps at 650 mm.
//   hero56 / clock36  InterDisplay Light, tnum frozen
//   id28              Inter Medium
//   val22             Inter Medium, tnum frozen  <- every instrument value
//   body18            Inter Regular
//   micro13           JetBrains Mono Medium (monospaced => tabular by build)
LV_FONT_DECLARE(font_hero56);
LV_FONT_DECLARE(font_clock36);
LV_FONT_DECLARE(font_id28);
LV_FONT_DECLARE(font_val22);
LV_FONT_DECLARE(font_body18);
LV_FONT_DECLARE(font_micro13);
#endif

const lv_font_t* F_NUM56;
const lv_font_t* F_NUM36;
const lv_font_t* F_L28;
const lv_font_t* F_M20;
const lv_font_t* F_UI15;
const lv_font_t* F_UI12;
const lv_font_t* F_MONO13;
const lv_font_t* F_MONO11;
const lv_font_t* F_SYM16;
const lv_font_t* F_SYM12;

lv_style_t st_card;
lv_style_t st_pill;
lv_style_t st_microlbl;
lv_style_t st_hair;

void themeInit() {
#if AR_USE_INTER_FONTS
  // The old role names survive as aliases so call sites did not all have to
  // change at once; what each one resolves to is the new scale.
  F_NUM56 = &font_hero56;    // 56  hero numeral        (tabular)
  F_NUM36 = &font_clock36;   // 36  clock               (tabular)
  F_L28   = &font_id28;      // 28  callsign / route
  F_M20   = &font_val22;     // 22  VALUES — 20 px was 14.5', under the 16' floor
  F_UI15  = &font_body18;    // 18  body / distances
  F_UI12  = &font_micro13;   // 13  keys and micro labels
  F_MONO13= &font_micro13;
  F_MONO11= &font_micro13;   // scope callsigns: 11 -> 13
#else
  F_NUM56 = &lv_font_montserrat_48;
  F_NUM36 = &lv_font_montserrat_36;
  F_L28   = &lv_font_montserrat_28;
  F_M20   = &lv_font_montserrat_20;
  F_UI15  = &lv_font_montserrat_14;
  F_UI12  = &lv_font_montserrat_12;
  F_MONO13= &lv_font_montserrat_12;
  F_MONO11= &lv_font_montserrat_12;
#endif
  F_SYM16 = &lv_font_montserrat_16;
  F_SYM12 = &lv_font_montserrat_12;      // LV_SYMBOL_* glyph carrier

  // Glass card
  lv_style_init(&st_card);
  lv_style_set_radius(&st_card, CARD_RADIUS);
  // Flat, opaque, no gradient. LV_GRAD_CACHE_DEF_SIZE is 0, so every gradient
  // was recomputed per draw; the flat fill also gives a stronger 7.4x elevation
  // step than the gradient's 4.15x-to-1.47x swing.
  lv_style_set_bg_color(&st_card, C_SURF);
  lv_style_set_bg_grad_dir(&st_card, LV_GRAD_DIR_NONE);
  lv_style_set_bg_opa(&st_card, OPA_CARD);
  lv_style_set_border_color(&st_card, C_BORDER);
  lv_style_set_border_opa(&st_card, OPA_BORDER);
  lv_style_set_border_width(&st_card, 1);
  // No shadow. Measured: it produced a 1.009:1 contrast difference in exchange
  // for a 3,362 B uncached lv_mem_buf_get plus two box-blur passes per card per
  // repaint (LV_SHADOW_CACHE_SIZE is 0). Invisible, and not cheap.
  lv_style_set_shadow_width(&st_card, 0);
  lv_style_set_pad_all(&st_card, 16);

  // Pill (weather / range)
  lv_style_init(&st_pill);
  lv_style_set_radius(&st_pill, LV_RADIUS_CIRCLE);
  lv_style_set_bg_color(&st_pill, lv_color_hex(0x090e15));
  lv_style_set_bg_opa(&st_pill, OPA_PILL);
  lv_style_set_border_color(&st_pill, C_BORDER);
  lv_style_set_border_opa(&st_pill, 22);
  lv_style_set_border_width(&st_pill, 1);
  lv_style_set_pad_hor(&st_pill, 18);
  lv_style_set_pad_ver(&st_pill, 6);

  // Micro label (tracked mono uppercase)
  lv_style_init(&st_microlbl);
  lv_style_set_text_font(&st_microlbl, F_MONO13);   // 11 px is below the ISO floor
  lv_style_set_text_color(&st_microlbl, C_DIM);
  lv_style_set_text_letter_space(&st_microlbl, 1);   // 3 px overflowed value cells

  // Hairline divider (apply to an lv_obj of height 1)
  lv_style_init(&st_hair);
  lv_style_set_bg_color(&st_hair, C_BORDER);
  lv_style_set_bg_opa(&st_hair, OPA_HAIR);
  lv_style_set_border_width(&st_hair, 0);
  lv_style_set_radius(&st_hair, 0);
  lv_style_set_pad_all(&st_hair, 0);
}

// ============================================================
//  Weather (WMO code -> icon + word)
// ============================================================
const lv_img_dsc_t* wxIconFor(int c) {
  if (c == 0 || c == 1)           return &img_wx_sun;
  if (c == 2 || c == 3)           return &img_wx_cloud;
  if (c == 45 || c == 48)         return &img_wx_fog;
  if (c >= 51 && c <= 67)         return &img_wx_rain;
  if (c >= 71 && c <= 77)         return &img_wx_snow;
  if (c >= 80 && c <= 82)         return &img_wx_rain;
  if (c == 85 || c == 86)         return &img_wx_snow;
  if (c >= 95)                    return &img_wx_storm;
  return &img_wx_cloud;
}
const char* wxWordFor(int c) {
  if (c == 0)                     return "Clear";
  if (c == 1)                     return "Mostly clear";
  if (c == 2)                     return "Partly cloudy";
  if (c == 3)                     return "Overcast";
  if (c == 45 || c == 48)         return "Fog";
  if (c >= 51 && c <= 57)         return "Drizzle";
  if (c >= 61 && c <= 67)         return "Rain";
  if (c >= 71 && c <= 77)         return "Snow";
  if (c >= 80 && c <= 82)         return "Showers";
  if (c == 85 || c == 86)         return "Snow showers";
  if (c >= 95)                    return "Storm";
  return "--";
}

// ============================================================
//  Operator monogram (initials + stable brand-ish colour, no greens)
// ============================================================
static const uint32_t MONO_PAL[] = {
  0xe23744,  // red
  0x1c4fa0,  // blue
  0x0a7d8c,  // teal
  0xd97a26,  // orange
  0x7a4fd0,  // violet
  0x9c1520,  // dark red
  0x2b6cb8,  // sky blue
  0xb02a6e,  // magenta
};

void monogramFor(const char* ownOp, char out[3], lv_color_t* bg) {
  out[0] = out[1] = 0; out[2] = 0;
  uint32_t h = 5381;
  if (ownOp && ownOp[0]) {
    // initials of the first two words
    int oi = 0;
    bool newWord = true;
    for (const char* p = ownOp; *p && oi < 2; p++) {
      if (isspace((unsigned char)*p)) { newWord = true; continue; }
      if (newWord && isalpha((unsigned char)*p)) {
        out[oi++] = toupper((unsigned char)*p);
        newWord = false;
      }
    }
    if (oi == 0) { out[0] = '?'; }
    else if (oi == 1 && strlen(ownOp) > 1) out[1] = toupper((unsigned char)ownOp[1]);
    for (const char* p = ownOp; *p; p++) h = h * 33u + (uint8_t)*p;
  } else {
    out[0] = '-'; out[1] = '-';
  }
  if (bg) *bg = lv_color_hex(MONO_PAL[h % (sizeof(MONO_PAL) / sizeof(MONO_PAL[0]))]);
}
