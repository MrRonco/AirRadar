/**
 * lv_conf.h — AirRadar v7 (LVGL 8.3.x)
 *
 * INSTALL: copy this file to your Arduino libraries folder, NEXT TO the lvgl
 * library folder (not inside it):
 *     cp firmware/lv_conf.h ~/Documents/Arduino/libraries/lv_conf.h
 *
 * This is a minimal override file: every option not set here falls back to the
 * LVGL 8.3 default via lv_conf_internal.h (each default is guarded by #ifndef).
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*==== COLOR ====*/
#define LV_COLOR_DEPTH 16
/* RGB parallel panel is fed native RGB565 through LovyanGFX pushImage().
 * FIRST-FLASH CHECK: if colors come out psychedelic/wrong, flip this to 1
 * (or toggle lcd.setSwapBytes() in hal_display.cpp) — one or the other. */
#define LV_COLOR_16_SWAP 0

/*==== MEMORY ====*/
/* HARDWARE-TUNED: LVGL's heap (widget tree, styles, label texts) lives in
 * PSRAM. With plain malloc it all landed in internal SRAM and, together with
 * the internal draw buffer, starved mbedTLS (~50 KB/handshake) — every HTTPS
 * fetch (cloud aircraft, map tiles, routes, weather) failed. Widgets are cold
 * data; PSRAM is the right home. free() releases PSRAM pointers fine. */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "esp32-hal-psram.h"
#define LV_MEM_CUSTOM_ALLOC   ps_malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC ps_realloc
#define LV_MEMCPY_MEMSET_STD 1

/*==== HAL ====*/
#define LV_DISP_DEF_REFR_PERIOD 30

/* Invalidated-area slots per refresh. On overflow LVGL DISCARDS every
 * pending area and repaints the entire 800x480 screen (lv_refr.c:256).
 * Nine moving blips plus ~20 card labels on a 250 ms tick sits close to
 * the default 32; a full repaint costs ~230 ms here. 64 slots cost
 * 64 x 8 B = 512 B and move that cliff well out of reach. */
#define LV_INV_BUF_SIZE 64
#define LV_INDEV_DEF_READ_PERIOD 30
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#define LV_DPI_DEF 130

/*==== RENDERING ====*/
#define LV_DRAW_COMPLEX 1          /* anti-aliased radii, shadows, masks, img transform */
#define LV_SHADOW_CACHE_SIZE 0
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/*==== LOGGING (off in release; flip for bring-up debugging) ====*/
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/*==== FONTS ====*/
/* Montserrat built-ins double as symbol carriers (gear/home/wifi glyphs) and
 * as fallbacks if the custom Inter fonts are not compiled in. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*==== WIDGETS (lean set; unlisted ones keep their defaults) ====*/
#define LV_USE_CANVAS 1
#define LV_USE_TABLE 0
#define LV_USE_METER 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPAN 0
#define LV_USE_MENU 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_LED 0
#define LV_USE_CALENDAR 0
#define LV_USE_ANIMIMG 0

/*==== THEME ====*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0

/*==== OTHERS ====*/
#define LV_USE_PERF_MONITOR 0      /* set 1 to see FPS/CPU during bring-up */
#define LV_USE_MEM_MONITOR 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_QRCODE 0
#define LV_USE_PNG 0               /* PNG decode is done by LovyanGFX (map tiles) */
#define LV_USE_GIF 0
#define LV_USE_FS_STDIO 0

#endif /* LV_CONF_H */
