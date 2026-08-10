// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// spark.h — a one-hour traffic history, in 60 bytes.
//
// The device runs 24/7 and remembers nothing. Every reading on the panel is
// instantaneous, so after weeks of uptime it still cannot say whether right
// now is busy or quiet — and "is this normal?" is unanswerable without a
// baseline. A baseline is the one thing a 24/7 appliance is uniquely placed
// to have.
//
// Deliberately RAM-only and deliberately tiny. Nothing is written to flash:
// per CLAUDE.md rule 22 every FATFS write stalls the panel's DMA, and an hour
// of history is not worth a single shake. Losing it on reboot is correct
// behaviour, not a limitation.
#pragma once
#include <stdint.h>
#include <lvgl.h>

#define SPARK_SLOTS 60          // one sample per minute

// Feed it the current in-range count; it samples on its own schedule.
void sparkSample(uint32_t nowMs, uint8_t inRange);

// Build the canvas inside `parent`. Returns the object so the caller owns
// placement and visibility.
lv_obj_t* sparkBuild(lv_obj_t* parent, int w, int h);

// Repaint from the ring buffer. Cheap, but call it only when the data moved:
// once a minute, not once a tick.
void sparkRedraw();

// True when a fresh sample has landed since the last redraw.
bool sparkDirty();

// How many samples are held, and the newest value. Published on /metrics so
// "the sparkline is flat" can be told apart from "the sparkline is broken"
// without a reflash -- which is exactly the distinction that cost a debugging
// round here.
uint8_t sparkCount();
uint8_t sparkNewest();
