// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// stall.h — per-stage loop timing for diagnosing display glitches. See stall.cpp.
#pragma once
#include <Arduino.h>

// Anything in loop() taking longer than this is worth recording. Normal
// iteration is ~2 ms (the delay(2) yield), so 40 ms is 20x nominal and well
// past the point where LVGL's flush can collide with the panel scan.
#define AR_STALL_MS 40

enum StallStage : uint8_t {
  ST_LVGL = 0, ST_WEB, ST_FEEDER, ST_ENRICH, ST_MAP, ST_MQTT, ST_LOGOS,
  ST_TRACKS, ST_UITICK, ST_N
};

// Bitmask of what was in flight when the stall happened, for correlation.
#define BUSY_FEEDER 0x01
#define BUSY_ROUTE  0x02
#define BUSY_WX     0x04
#define BUSY_ISS    0x08
#define BUSY_LOGO   0x10
#define BUSY_MAP    0x20

void   stallBegin();                                        // allocate the ring (setup)
void   stallNote(uint8_t stage, uint32_t ms, uint8_t busy, uint32_t px = 0);
String stallReport();                                       // human-readable dump
