// ui.h — screen construction, navigation and refresh. Loop context only.
//
// Screen inventory (Screen enum in types.h):
//   SCR_MAIN     — scope + cards (ui_scope.cpp + ui_cards.cpp)
//   SCR_SETTINGS — grouped two-column settings (ui_settings.cpp)
//   SCR_WIFI     — scan list + password keyboard (ui_settings.cpp)
//   SCR_COORDS   — lat/lon editor (ui_settings.cpp)
//   SCR_TEXTEDIT — generic single-field editor w/ keyboard (ui_settings.cpp)
//
// Pixel spec: see theme.h + config.h geometry. The build target is the
// browser-verified mock; when in doubt match the mock.
#pragma once
#include <lvgl.h>
#include "../core/state.h"
#include "theme.h"

// ---- lifecycle ----
void uiInit();                       // build all screens once (hidden), show MAIN
void uiShow(Screen s);               // switch screens (fade, 220ms)
void uiTick(uint32_t nowMs);         // called every AR_UI_TICK_MS from loop:
                                     //  refresh cards, glide blips, blink emergency
lv_obj_t* uiScreenRoot(Screen s);    // root object of a screen (built by uiInit)

// Advance the range step (dir=+1/-1), persist, kick feeder + map + UI refresh.
void uiCycleRange(int dir);

// ---- main screen (ui_scope.cpp) ----
void scopeBuild(lv_obj_t* parent);   // circular map + rings + blip pool + ISS
void scopeUpdate(uint32_t nowMs);    // sync blips to g_tracks (anim to new pos)
void scopeApplyMapImage();           // re-set map src when mapGeneration() bumps

// ---- main screen cards (ui_cards.cpp) ----
void cardsBuild(lv_obj_t* parent);   // overview, selected, time, settings btn,
                                     //  weather pill, range pill
void cardsUpdate(uint32_t nowMs);

// ---- settings + editors (ui_settings.cpp) ----
void settingsBuild();                // SCR_SETTINGS root
void settingsRefresh();              // re-read g_set into widgets
void wifiScreenBuild();              // SCR_WIFI (scan list; tap -> keyboard)
void wifiScreenOpen();               // start an async scan, then show SCR_WIFI
void coordsScreenBuild();            // SCR_COORDS
void texteditOpen(const char* title, const char* initial, bool password,
                  void (*onSave)(const char* value));   // SCR_TEXTEDIT
void settingsPoll(uint32_t nowMs);   // drives async flows (wifi connect spinner);
                                     //  called from uiTick every tick

// ---- help overlay (ui_help.cpp) ----
// A legend for what the display ENCODES (glyph colour/size, dimming, rings,
// dot states) rather than what it already labels. Built hidden at boot.
void helpBuild(lv_obj_t* parent);
void helpToggle();
bool helpVisible();

// ---- night mode ----
// Evaluate quiet hours against local time; returns true if display should be off.
bool uiNightActive();
