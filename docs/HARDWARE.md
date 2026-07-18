# Hardware notes — Waveshare ESP32-S3-Touch-LCD-7 (Rev 1.2)

## Module
ESP32-S3-WROOM-1-N16R8: 16 MB QIO flash, 8 MB OPI PSRAM. CH343P USB-UART
(macOS: WCH CH34x driver cask). Onboard UART slide switch gates flashing.

## RGB565 panel pins (see LGFX_Waveshare_7.h for the authoritative config)
- Blue  B0–B4: 14, 38, 18, 17, 10
- Green G0–G5: 39, 0, 45, 48, 47, 21
- Red   R0–R4: 1, 2, 42, 41, 40
- DE=5, VSYNC=3, HSYNC=46, PCLK=7
- Timings: hsync 8/4/8, vsync 16/4/16, pclk_active_neg=1, freq_write=14 MHz

## I2C bus (SDA=8, SCL=9)
- CH422G expander: mode reg 0x24 (write 0x01 = push-pull out), output reg 0x38.
  Bits: b1 TP_RST, b2 DISP/backlight (on/off only), b3 LCD_RST, b4 SD_CS. Normal 0x1E.
- GT911 touch: addr 0x5D (pinned by reset sequence; 0x14 is the alternate),
  INT = GPIO4, RST on expander b1. LovyanGFX Touch_GT911, i2c_port 0, 400 kHz.

## GT911 reset sequence (in ch422g_init)
Drive GPIO4 (INT) low as output → TP_RST low (out reg 0x1C) → 12 ms → TP_RST high
(0x1E) → 60 ms (address latch window with INT held low ⇒ 0x5D) → release INT to
input → Wire.end() so LovyanGFX's I2C driver owns the bus.

## Network (example setup — adapt to yours)
Give the ESP32 a stable address (DHCP reservation or the on-device static-IP screen).
Feeder: adsb.im on a Raspberry Pi — port 80 = Flask config app, port 8080 = tar1090
(`/data/aircraft.json`). If the feeder is on a different VLAN/subnet than the display,
add a firewall pass rule TCP → <feeder-ip>:8080 from the display's network.
