// hal_display.h — Waveshare ESP32-S3-Touch-LCD-7 bring-up + LVGL glue.
// Hardware ground truth carried from the proven v5/v6 code:
//   * CH422G controlled reset pins the GT911 at I2C 0x5D every boot
//   * freq_write = 14 MHz (16 produced pixel drift on real hardware)
//   * Wire.end() after CH422G writes — LovyanGFX's I2C driver owns the bus
//   * PSRAM must be OPI PSRAM or nothing allocates
#pragma once
#include <lvgl.h>

// Init order: halDisplayInit() -> themeInit() -> uiInit().
// Returns false if PSRAM buffers failed (the #1 black-screen cause).
bool halDisplayInit();

// Backlight on/off (CH422G bit b2; no PWM on this board). Runtime writes go
// through LovyanGFX's lgfx::i2c helpers so bus ownership stays coherent.
// NEEDS-HARDWARE-VERIFY: first night-mode test on the real panel.
void halBacklight(bool on);
bool halBacklightState();

// Raw touch read (used by night-mode wake without feeding LVGL).
bool halTouchRead(int32_t* x, int32_t* y);

// Read back a rect of the live panel framebuffer as RGB565 (row-major into buf).
// Used by /screen.bmp. Loop context only.
void halReadRect(int x, int y, int w, int h, uint16_t* buf);
