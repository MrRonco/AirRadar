// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// brandcolor.h — ICAO operator code to brand colour. A colour is a fact, not a
// copyrightable work, which is why the operator tile draws the code in the
// airline's colour instead of reproducing its logo. See brandcolor.cpp.
#pragma once
#include <lvgl.h>
#include <stdint.h>

// Primary identity colour for an ICAO operator code ("ACA", "UAL", ...).
// Known carriers come from a table; anything else gets a stable generated hue
// derived from the code, so two operators never collide visually.
uint32_t   brandRgbFor(const char* icao3);
lv_color_t brandColorFor(const char* icao3);
