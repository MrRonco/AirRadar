// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// main.cpp — runs AirRadar's UI in a window on a Mac.
//
// Two modes:
//   ./airradar-ui                 interactive: an 800x480 window, mouse = touch
//   ./airradar-ui --shot out.bmp  headless: render one frame and write a BMP
//
// The headless mode is not an afterthought. It means a UI change can be looked
// at without a display attached at all, which is the difference between "I can
// check this" and "I need the panel back".
//
// What this is NOT: a hardware simulator. Colours are sRGB on your monitor, not
// the panel's IPS; nothing here can tell you about DMA contention, repaint cost
// or whether a touch target suits a thumb. Those live on the device and are
// measured with /api/stalls. See desktop/README.md.
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <lvgl.h>
#include "Arduino.h"
#include "scenarios.h"
#include "../firmware/AirRadar/src/config.h"
#include "../firmware/AirRadar/src/core/state.h"
#include "../firmware/AirRadar/src/core/units.h"
#include "../firmware/AirRadar/src/core/timezones.h"
#include "../firmware/AirRadar/src/ui/ui.h"
#include "../firmware/AirRadar/src/net/maptiles.h"
#include "fakemap.h"

static SDL_Window*   s_win = nullptr;
static SDL_Renderer* s_ren = nullptr;
static SDL_Texture*  s_tex = nullptr;
static uint16_t      s_fb[SCR_W * SCR_H];        // our own copy, for --shot
static int           s_scenario = SCN_TYPICAL;

// LVGL hands us RGB565 (LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 0), which is
// exactly what the panel takes, so no conversion — the same bytes the device
// would push to its framebuffer land here.
static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* a, lv_color_t* px) {
  for (int y = a->y1; y <= a->y2; y++) {
    const int w = a->x2 - a->x1 + 1;
    memcpy(&s_fb[y * SCR_W + a->x1], &px[(y - a->y1) * w], (size_t)w * 2);
  }
  lv_disp_flush_ready(drv);
}

static void mouse_cb(lv_indev_drv_t*, lv_indev_data_t* data) {
  int x = 0, y = 0;
  const Uint32 b = SDL_GetMouseState(&x, &y);
  data->point.x = (lv_coord_t)x;
  data->point.y = (lv_coord_t)y;
  data->state = (b & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED
                                                  : LV_INDEV_STATE_RELEASED;
}

static void present() {
  SDL_UpdateTexture(s_tex, nullptr, s_fb, SCR_W * 2);
  SDL_RenderClear(s_ren);
  SDL_RenderCopy(s_ren, s_tex, nullptr, nullptr);
  SDL_RenderPresent(s_ren);
}

// 24-bit BMP, bottom-up — byte-identical in shape to what /screen.bmp serves,
// so the same tooling reads both.
static bool writeBmp(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); return false; }
  const int rowBytes = SCR_W * 3, imgBytes = rowBytes * SCR_H;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  const uint32_t total = 54 + imgBytes;
  memcpy(&hdr[2], &total, 4);
  const uint32_t off = 54; memcpy(&hdr[10], &off, 4);
  const uint32_t dib = 40; memcpy(&hdr[14], &dib, 4);
  const int32_t w = SCR_W, h = SCR_H; memcpy(&hdr[18], &w, 4); memcpy(&hdr[22], &h, 4);
  const uint16_t planes = 1, bpp = 24; memcpy(&hdr[26], &planes, 2); memcpy(&hdr[28], &bpp, 2);
  memcpy(&hdr[34], &imgBytes, 4);
  fwrite(hdr, 1, 54, f);
  uint8_t* row = new uint8_t[rowBytes];
  for (int y = SCR_H - 1; y >= 0; y--) {
    for (int x = 0; x < SCR_W; x++) {
      const uint16_t c = s_fb[y * SCR_W + x];
      row[x * 3 + 0] = (uint8_t)((c & 0x001F) << 3);          // B
      row[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2);     // G
      row[x * 3 + 2] = (uint8_t)(((c >> 11) & 0x1F) << 3);    // R
    }
    fwrite(row, 1, rowBytes, f);
  }
  delete[] row;
  fclose(f);
  return true;
}

static void banner() {
  printf("\nAirRadar UI harness — %dx%d\n", SCR_W, SCR_H);
  printf("  1..%d  scenario      SPACE  next scenario\n", SCN_COUNT);
  printf("  s      save shot.bmp  m/g/?  main / settings / legend      q  quit\n");
  for (int i = 0; i < SCN_COUNT; i++) printf("    %d  %s\n", i + 1, scenarioName(i));
  printf("\nnow: %s\n", scenarioName(s_scenario));
}

int main(int argc, char** argv) {
  const char* shot = nullptr;
  const char* screen = "main";
  int tintNum = 0;
  bool imperial = false;
  int  scrollY = 0;              // 0 = leave fakemap.cpp's own default alone
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) s_scenario = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--screen") && i + 1 < argc) screen = argv[++i];
    // --tint N exercises F1(b): maptiles.cpp's TINT_LUM_NUM over a fixed /10.
    // The harness can show the RELATIVE effect of this knob, never the correct
    // absolute value -- the synthetic map's base brightness is invented.
    else if (!strcmp(argv[i], "--tint") && i + 1 < argc) tintNum = atoi(argv[++i]);
    // --imperial renders the whole UI in the other unit system. Worth a flag
    // rather than a keypress: every unit-bearing reading has to be checked for
    // width at once, and "155 MI" is not the same size as "250 KM".
    else if (!strcmp(argv[i], "--imperial")) imperial = true;
    // --scroll N pushes the settings columns down before the shot. Without it
    // the harness can only ever review the top 352 px of a screen that is
    // ~630 px tall, which is how two new rows got added without anyone being
    // able to look at them.
    else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) scrollY = atoi(argv[++i]);
  }

  if (SDL_Init(shot ? 0 : SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  lv_init();
  static lv_disp_draw_buf_t db;
  static lv_color_t buf[SCR_W * 60];
  lv_disp_draw_buf_init(&db, buf, nullptr, SCR_W * 60);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.draw_buf = &db; dd.flush_cb = flush_cb;
  dd.hor_res = SCR_W; dd.ver_res = SCR_H;
  lv_disp_drv_register(&dd);

  if (!shot) {
    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER; id.read_cb = mouse_cb;
    lv_indev_drv_register(&id);
  }

  // Real firmware, from here down.
  settingsLoad();
  // Deliberately a round, obviously-synthetic location. Preferences is an
  // in-memory map here, so nothing real is ever loaded: no SSID, no feeder
  // host, no device IP. A harness screenshot is publishable as-is, which the
  // device's own /screen.bmp is not — every settings capture off the panel has
  // to be redacted by hand.
  g_set.homeLat = 46.5000; g_set.homeLon = -81.0000;
  g_set.rangeKm = 250;
  g_wifiUp = true;
  themeInit();
  mapBegin();          // same order as the firmware's setup()
  if (tintNum > 0) fakeMapSetTint(tintNum, 10);
  if (imperial) g_set.units = AR_UNITS_IMPERIAL;
  uiInit();
  scenarioApply(s_scenario);
  uiTick(millis());

  if (shot) {
    if (!strcmp(screen, "settings")) uiShow(SCR_SETTINGS);
    else if (!strcmp(screen, "legend")) helpToggle();
    else if (!strcmp(screen, "tz")) {
      // Reaches SCR_PICKER through the real settings code path rather than a
      // harness-only shortcut, so what is rendered is what a tap produces.
      static const char* names[kTimezoneCount];
      for (int i = 0; i < kTimezoneCount; i++) names[i] = kTimezones[i].name;
      pickerOpen("TIME ZONE", names, kTimezoneCount,
                 tzIndexOf(g_set.tz.c_str()), nullptr);
    }
    for (int i = 0; i < 8; i++) { lv_timer_handler(); SDL_Delay(20); }
    if (scrollY) {
      lv_obj_t* root = uiScreenRoot(g_screen);
      for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); i++) {
        lv_obj_t* c = lv_obj_get_child(root, i);
        // Only things with content BELOW the fold. LVGL flags plain labels
        // scrollable too, and scrolling those just walked the page title off
        // the top of the diagnostic.
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE) &&
            lv_obj_get_scroll_bottom(c) > 0)
          lv_obj_scroll_by(c, 0, -scrollY, LV_ANIM_OFF);
      }
      for (int i = 0; i < 4; i++) { lv_timer_handler(); SDL_Delay(10); }
    }
    uiTick(millis());
    lv_timer_handler();
    const bool ok = writeBmp(shot);
    printf("%s %s (%s / %s)\n", ok ? "wrote" : "FAILED", shot, screen, scenarioName(s_scenario));
    return ok ? 0 : 1;
  }

  s_win = SDL_CreateWindow("AirRadar UI harness", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, SCR_W, SCR_H, SDL_WINDOW_SHOWN);
  s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
  s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                            SDL_TEXTUREACCESS_STREAMING, SCR_W, SCR_H);
  banner();

  bool run = true;
  uint32_t lastTick = 0;
  while (run) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) run = false;
      if (e.type == SDL_KEYDOWN) {
        const SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_q || k == SDLK_ESCAPE) run = false;
        else if (k >= SDLK_1 && k < SDLK_1 + SCN_COUNT) {
          s_scenario = k - SDLK_1;
          scenarioApply(s_scenario);
          printf("scenario: %s\n", scenarioName(s_scenario));
        } else if (k == SDLK_SPACE) {
          s_scenario = (s_scenario + 1) % SCN_COUNT;
          scenarioApply(s_scenario);
          printf("scenario: %s\n", scenarioName(s_scenario));
        } else if (k == SDLK_s) {
          writeBmp("shot.bmp");
          printf("wrote shot.bmp\n");
        } else if (k == SDLK_m) uiShow(SCR_MAIN);
        else if (k == SDLK_g)   uiShow(SCR_SETTINGS);
        else if (k == SDLK_SLASH) helpToggle();
      }
    }
    const uint32_t now = millis();
    if (now - lastTick >= AR_UI_TICK_MS) { uiTick(now); lastTick = now; }
    lv_timer_handler();
    present();
    SDL_Delay(5);
  }
  SDL_Quit();
  return 0;
}
