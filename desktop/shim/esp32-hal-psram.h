// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// esp32-hal-psram.h — desktop shim.
//
// This exists so the harness can use the firmware's lv_conf.h VERBATIM. That
// file routes LVGL's allocator at ps_malloc and its tick at Arduino's millis();
// providing those two symbols is cheaper than maintaining a second lv_conf,
// which would drift and quietly stop showing what the panel shows.
#pragma once
#include <stdlib.h>
static inline void* ps_malloc(size_t n)            { return malloc(n); }
static inline void* ps_realloc(void* p, size_t n)  { return realloc(p, n); }
static inline void* ps_calloc(size_t n, size_t s)  { return calloc(n, s); }
