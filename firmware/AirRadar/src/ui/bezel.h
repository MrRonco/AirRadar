// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// bezel.h — the graduated bearing scale rasterised into the map buffer.
#pragma once
#include <stdint.h>

// Engrave a bearing scale onto a plain RGB565 buffer (LV_COLOR_16_SWAP 0):
// a tick every 10 degrees, longer every 30, skipping the four cardinals
// because ui_scope draws those as LVGL bars and owns them whether or not a
// map exists.
//
// Pure pixel arithmetic — no LVGL, no globals, no allocation — so it is safe
// to call from the core-0 map task on the back buffer it has exclusive use of.
// Cost is one pass over the annulus at map-build time and NOTHING at runtime,
// which is the whole reason it lives here rather than in 36 LVGL objects
// hanging off the clip container (CLAUDE.md rule 19).
void bezelRasterise(uint16_t* buf, int w, int h, int cx, int cy, int r);
