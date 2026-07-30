// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// maptiles.h — CARTO dark_all base map: fetch slippy tiles for the current
// view, stitch + blue-tint into a PSRAM RGB565 buffer sized MAP_SIZE².
//
// Ownership: the module owns two buffers (front shown by LVGL, back being
// written by the fetch task). When a stitch completes the task sets a ready
// flag; mapLoop() (loop context) swaps front/back and tells the UI.
#pragma once
#include "../core/state.h"
#include <lvgl.h>

void mapBegin();                       // allocate buffers (PSRAM); safe if PSRAM absent
void mapLoop(uint32_t nowMs);          // swap-in finished images; retry failed fetches
void mapRequestRefresh();              // call on boot / range / location change

// Front image for the UI (stable between mapLoop calls). NULL until first
// stitch lands or if map layer disabled/unavailable — UI shows the dark floor.
const lv_img_dsc_t* mapImage();
uint32_t mapGeneration();              // bumps on every swap so UI knows to re-set src
