// ui_nav.cpp — screen roots, navigation, tick fan-out, range cycling, night calc.
#include <time.h>
#include "ui.h"
#include "../core/tracks.h"
#include "../net/feeder.h"
#include "../net/enrich.h"
#include "../net/maptiles.h"

static lv_obj_t* s_roots[SCR_COUNT] = {nullptr};

lv_obj_t* uiScreenRoot(Screen s) { return s_roots[s]; }

static lv_obj_t* makeRoot() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, SCR_W, SCR_H);
  // Flat ink. The vertical gradient banded visibly in RGB565 and was recomputed
  // on every screen-root repaint (LV_GRAD_CACHE_DEF_SIZE 0).
  lv_obj_set_style_bg_color(scr, C_INK, 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

void uiInit() {
  for (int i = 0; i < SCR_COUNT; i++) s_roots[i] = makeRoot();

  scopeBuild(s_roots[SCR_MAIN]);      // scope first: cards float above it
  cardsBuild(s_roots[SCR_MAIN]);
  helpBuild(s_roots[SCR_MAIN]);   // hidden overlay, must be built last
  settingsBuild();
  wifiScreenBuild();
  coordsScreenBuild();
  // SCR_TEXTEDIT content is (re)built by texteditOpen()

  g_screen = SCR_MAIN;
  lv_scr_load(s_roots[SCR_MAIN]);
}

void uiShow(Screen s) {
  if (s >= SCR_COUNT || !s_roots[s]) return;
  g_screen = s;
  if (s == SCR_SETTINGS) settingsRefresh();
  // No fade: a 220 ms cross-fade of two full 800x480 screens is the single most
  // expensive thing this UI can ask the renderer to do, against a panel DMA that
  // already needs 25 MB/s.
  lv_scr_load_anim(s_roots[s], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

void uiTick(uint32_t nowMs) {
  settingsPoll(nowMs);                 // async flows (wifi connect, etc.)
  if (g_screen == SCR_MAIN) {
    cardsUpdate(nowMs);
    scopeUpdate(nowMs);                // blip glide targets + emergency blink phase
    scopeApplyMapImage();              // picks up newly stitched map images
  }
}

void uiCycleRange(int dir) {
  static const int steps[] = AR_RANGE_STEPS;
  const int n = sizeof(steps) / sizeof(steps[0]);
  int idx = 0;
  for (int i = 0; i < n; i++) if (steps[i] == g_set.rangeKm) idx = i;
  idx = (idx + dir + n) % n;
  g_set.rangeKm = steps[idx];
  settingsSaveLocation();
  tracksRebuildOrder();
  feederKick();
  mapRequestRefresh();
  scopeUpdate(millis());
  cardsUpdate(millis());
}

bool uiNightActive() {
  struct tm ti;
  if (!getLocalTime(&ti, 10)) return false;
  int nowMin = ti.tm_hour * 60 + ti.tm_min;
  int from = g_set.nightFromMin, to = g_set.nightToMin;
  if (from == to) return false;
  if (from < to) return nowMin >= from && nowMin < to;      // e.g. 01:00-05:00
  return nowMin >= from || nowMin < to;                     // e.g. 23:00-06:00
}
