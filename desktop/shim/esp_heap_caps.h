// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// esp_heap_caps.h — desktop shim. Reports a healthy heap so the TLS gate stays
// open and nothing gets shed. The harness cannot say anything true about the
// real heap; that measurement only exists on the device (/metrics).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 0x800
#define MALLOC_CAP_SPIRAM   0x400
#define MALLOC_CAP_8BIT     0x004
inline size_t heap_caps_get_free_size(uint32_t)          { return 160 * 1024; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return  90 * 1024; }
inline void*  heap_caps_malloc(size_t n, uint32_t)       { return malloc(n); }
inline void   heap_caps_free(void* p)                    { free(p); }
