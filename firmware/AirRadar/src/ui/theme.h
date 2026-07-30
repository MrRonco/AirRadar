// theme.h — AirRadar v7 design tokens: colors, fonts, shared styles, assets.
// The pixel spec is the browser-verified 800x480 mock; treat these values as
// the single source of truth for look & feel.
#pragma once
#include <lvgl.h>
#include "../config.h"

// ---------- palette ----------
#define C_INK      lv_color_hex(0x05080d)   // page ink (deep background)
#define C_INK_HI   lv_color_hex(0x0c1119)   // background gradient top
// Card body — FLAT and opaque. This is the exact colour the settings groups
// were already producing (C_SURF #182231 at opa 110 over C_INK), so both
// screens now match; stating it directly also lets the settings groups go
// opaque and regain the LV_COVER_RES_COVER fast path.
#define C_SURF     lv_color_hex(0x0d131d)
#define C_SURF_HI  lv_color_hex(0x26344a)   // inset tile (logo chip)
// Kept so nothing that still references the old gradient fails to build.
#define C_CARD_HI  C_SURF
#define C_CARD_LO  C_SURF
#define C_IVORY    lv_color_hex(0xeef1f4)   // primary text
#define C_IVORY2   lv_color_hex(0xaab4c0)   // secondary text
#define C_DIM      lv_color_hex(0x8e9baa)   // keys/labels — 5.65:1 on the card
#define C_MUTE     lv_color_hex(0x75828f)   // genuinely optional text only, 4.07:1
#define C_FAINT    lv_color_hex(0x39434e)   // DECORATION ONLY — never text (1.9:1)
#define C_CY       lv_color_hex(0x54dcee)   // live / interactive accent
#define C_CY_SOFT  lv_color_hex(0x3fb6c8)
#define C_AMBER    lv_color_hex(0xf6b24a)   // altitude < 10k
#define C_VIOLET   lv_color_hex(0xa98cff)   // altitude > 30k
#define C_RED      lv_color_hex(0xff6472)   // emergency RING + error states
#define C_ALERT    lv_color_hex(0xff8a94)   // emergency GLYPH (brighter than C_RED)
// Altitude ramp, luminance now descending 0.599 -> 0.491 -> 0.321 so the band
// reads low/near = loud, high/far = quiet without consulting a legend. Cyan is
// no longer in this ramp: it means "live" and nothing else.
#define C_ALT_LOW  lv_color_hex(0xffc061)
#define C_ALT_MID  lv_color_hex(0x6fc7d8)
#define C_ALT_HIGH lv_color_hex(0x9b8ce0)
#define C_BORDER   lv_color_hex(0xb4cde6)   // card hairline (use with low opa)
#define C_RING     lv_color_hex(0x2b4d5d)   // scope ring
#define C_RING_DIM lv_color_hex(0x193040)   // crosshair
#define C_GOLD     lv_color_hex(0xffd77a)   // watchlist highlight

// LV_OPA_COVER on purpose: at 216 every card returned LV_COVER_RES_NOT_COVER,
// so each 1 Hz label repaint recomposited the whole screen root — to buy a
// measured 1.057:1 difference. Opaque restores the LV_COVER_RES_COVER path.
#define OPA_CARD     LV_OPA_COVER
#define OPA_BORDER    40    // the only elevation cue now that shadows are gone
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
extern const lv_font_t* F_MONO13;  // mono values           (IPs, feed rate)
extern const lv_font_t* F_MONO11;  // micro labels (tracked) ("OVERVIEW")
extern const lv_font_t* F_SYM16;   // symbol carrier (LV_SYMBOL_*)
extern const lv_font_t* F_SYM12;   // small symbols (route arrow)

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
