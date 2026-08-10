// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
#pragma once
#include <lvgl.h>
#include <stdint.h>
void                fakeMapBuild();
void                fakeMapSetTint(int lumNum, int lumDen);   // exercise F1's knob
uint16_t*           fakeMapBuf();                             // the bezel draws in here
const lv_img_dsc_t* fakeMapImage();
uint32_t            fakeMapGen();
