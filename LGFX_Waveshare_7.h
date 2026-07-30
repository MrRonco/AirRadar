// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// LGFX_Waveshare_7.h  (v2 - touch enabled)
// LovyanGFX config for Waveshare ESP32-S3-Touch-LCD-7 (800x480 RGB, GT911 touch,
// CH422G I/O expander).
//
// If touch does not respond, change TOUCH_I2C_ADDR from 0x5D to 0x14 below —
// the GT911 picks its address from pin states at reset and boards vary.

#pragma once

#include <Wire.h>

// ---------- CH422G I/O expander ----------
#define CH422G_REG_MODE 0x24   // write 0x01 -> IO0..IO7 push-pull output
#define CH422G_REG_OUT  0x38   // output bits
// b0=DI0  b1=TP_RST  b2=DISP/BL  b3=LCD_RST  b4=SD_CS
#define CH422G_OUT_VALUE 0x1E

#define WS_I2C_SDA 8
#define WS_I2C_SCL 9
#define TOUCH_I2C_ADDR 0x5D    // GT911: 0x5D or 0x14

inline void ch422g_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(reg);
  Wire.write(val);
  Wire.endTransmission();
}

inline void ch422g_init() {
  Wire.begin(WS_I2C_SDA, WS_I2C_SCL);

  ch422g_write(CH422G_REG_MODE, 0x01);          // IO0..IO7 push-pull output

  // --- GT911 controlled reset: pin its I2C address to 0x5D every boot ---
  // The GT911 samples its INT pin while reset releases; floating INT means a
  // random address (0x5D or 0x14) each power cycle. Hold INT low through the
  // reset window so the address is deterministic.
  pinMode(GPIO_NUM_4, OUTPUT);
  digitalWrite(GPIO_NUM_4, LOW);                // INT low -> address 0x5D

  ch422g_write(CH422G_REG_OUT, 0x1C);           // TP_RST low; DISP/LCD_RST/SD_CS high
  delay(12);
  ch422g_write(CH422G_REG_OUT, CH422G_OUT_VALUE); // TP_RST high - reset released
  delay(60);                                    // keep INT low through addr latch

  pinMode(GPIO_NUM_4, INPUT);                   // hand INT back to the GT911
  delay(60);

  Wire.end();     // release the bus - LovyanGFX's own I2C driver runs the GT911
}

// ---------- Display + touch ----------
class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB     _bus;
  lgfx::Panel_RGB   _panel;
  lgfx::Touch_GT911 _touch;

  LGFX(void) {
    {
      auto cfg = _panel.config();
      cfg.memory_width  = 800;
      cfg.memory_height = 480;
      cfg.panel_width   = 800;
      cfg.panel_height  = 480;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      _panel.config(cfg);
    }

    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;

      cfg.pin_d0  = GPIO_NUM_14;  // B0
      cfg.pin_d1  = GPIO_NUM_38;  // B1
      cfg.pin_d2  = GPIO_NUM_18;  // B2
      cfg.pin_d3  = GPIO_NUM_17;  // B3
      cfg.pin_d4  = GPIO_NUM_10;  // B4

      cfg.pin_d5  = GPIO_NUM_39;  // G0
      cfg.pin_d6  = GPIO_NUM_0;   // G1
      cfg.pin_d7  = GPIO_NUM_45;  // G2
      cfg.pin_d8  = GPIO_NUM_48;  // G3
      cfg.pin_d9  = GPIO_NUM_47;  // G4
      cfg.pin_d10 = GPIO_NUM_21;  // G5

      cfg.pin_d11 = GPIO_NUM_1;   // R0
      cfg.pin_d12 = GPIO_NUM_2;   // R1
      cfg.pin_d13 = GPIO_NUM_42;  // R2
      cfg.pin_d14 = GPIO_NUM_41;  // R3
      cfg.pin_d15 = GPIO_NUM_40;  // R4

      cfg.pin_henable = GPIO_NUM_5;
      cfg.pin_vsync   = GPIO_NUM_3;
      cfg.pin_hsync   = GPIO_NUM_46;
      cfg.pin_pclk    = GPIO_NUM_7;

      cfg.freq_write = 14000000;   // 12000000 if you ever see drift again

      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch  = 8;

      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 16;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch  = 16;

      cfg.pclk_active_neg = 1;
      cfg.de_idle_high    = 0;
      cfg.pclk_idle_high  = 0;

      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _touch.config();
      cfg.x_min = 0;   cfg.x_max = 799;
      cfg.y_min = 0;   cfg.y_max = 479;
      cfg.pin_int  = GPIO_NUM_4;
      cfg.pin_rst  = -1;              // reset lives on the CH422G, done in ch422g_init
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.pin_sda  = WS_I2C_SDA;
      cfg.pin_scl  = WS_I2C_SCL;
      cfg.freq     = 400000;
      cfg.i2c_addr = TOUCH_I2C_ADDR;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};
