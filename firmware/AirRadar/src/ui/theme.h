// theme.h — AirRadar v7 design tokens: colors, fonts, shared styles, assets.
// The pixel spec is the browser-verified 800x480 mock; treat these values as
// the single source of truth for look & feel.
#pragma once
#include <lvgl.h>
#include "../config.h"

// ---------- palette ----------
#define C_INK      lv_color_hex(0x05080d)   // page ink (deep background)
#define C_INK_HI   lv_color_hex(0x0c1119)   // background gradient top
#define C_CARD_HI  lv_color_hex(0x1c2838)   // card gradient top
#define C_CARD_LO  lv_color_hex(0x0a1018)   // card gradient bottom
#define C_IVORY    lv_color_hex(0xeef1f4)   // primary text
#define C_IVORY2   lv_color_hex(0xaab4c0)   // secondary text
#define C_DIM      lv_color_hex(0x69757f)   // labels / muted
#define C_FAINT    lv_color_hex(0x39434e)   // faintest text
#define C_CY       lv_color_hex(0x54dcee)   // live / interactive accent
#define C_CY_SOFT  lv_color_hex(0x3fb6c8)
#define C_AMBER    lv_color_hex(0xf6b24a)   // altitude < 10k
#define C_VIOLET   lv_color_hex(0xa98cff)   // altitude > 30k
#define C_RED      lv_color_hex(0xff6472)   // emergency / error
#define C_BORDER   lv_color_hex(0xb4cde6)   // card hairline (use with low opa)
#define C_RING     lv_color_hex(0x2b4d5d)   // scope ring
#define C_RING_DIM lv_color_hex(0x193040)   // crosshair
#define C_GOLD     lv_color_hex(0xffd77a)   // watchlist highlight

#define OPA_CARD     216    // card body opacity (over the map edge)
#define OPA_BORDER    26    // 10% hairline
#define OPA_PILL     168    // weather/range pill body

// ---------- fonts ----------
// Indirection so the whole UI survives with Montserrat if the custom Inter
// fonts aren't generated. Always use these, never lv_font_montserrat_* direct.
extern const lv_font_t* F_NUM56;   // huge light numerals   ("12")
extern const lv_font_t* F_NUM36;   // clock                 ("2:47")
extern const lv_font_t* F_L28;     // callsign / route      ("ACA123", "YYZ")
extern const lv_font_t* F_M20;     // medium titles         ("Home", "20°")
extern const lv_font_t* F_UI15;    // body                  (keys, values)
extern const lv_font_t* F_UI12;    // small body
extern const lv_font_t* F_MONO13;  // mono values           ("adsb.local")
extern const lv_font_t* F_MONO11;  // micro labels (tracked) ("OVERVIEW")
extern const lv_font_t* F_SYM16;   // symbol carrier (LV_SYMBOL_*)

// ---------- shared styles (initialised once in themeInit) ----------
extern lv_style_t st_card;         // glass card: gradient, radius 17, hairline
extern lv_style_t st_pill;         // rounded pill (weather / range)
extern lv_style_t st_microlbl;     // mono micro label, tracked, C_DIM
extern lv_style_t st_hair;         // 1px divider line style (use on lv_obj h=1)

// ---------- image assets (generated, see tools/genassets.py) ----------
LV_IMG_DECLARE(img_jet);           // 26x26 white jet, alpha — recolor per altitude
LV_IMG_DECLARE(img_glow);          // 34x34 soft radial glow, alpha — recolor
LV_IMG_DECLARE(img_iss);           // 22x12 ISS glyph
LV_IMG_DECLARE(img_wx_sun);
LV_IMG_DECLARE(img_wx_cloud);
LV_IMG_DECLARE(img_wx_rain);
LV_IMG_DECLARE(img_wx_snow);
LV_IMG_DECLARE(img_wx_fog);
LV_IMG_DECLARE(img_wx_storm);
LV_IMG_DECLARE(img_wx_wind);       // wind lines glyph

void themeInit();                  // resolve fonts, init styles. Call before any UI build.

// Map a WMO weather code to an icon + short word ("Overcast", "Rain", ...).
const lv_img_dsc_t* wxIconFor(int wmoCode);
const char*         wxWordFor(int wmoCode);

// Operator monogram: derive up to 2 initials + a stable brand-ish colour from
// an operator name ("Air Canada" -> "AC", red-ish). Used by the Selected card.
void monogramFor(const char* ownOp, char out[3], lv_color_t* bg);
