// hal_display.cpp — panel + touch bring-up and the LVGL display/input glue.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Wire.h>
#include "hal_display.h"
#include "../config.h"

// ============================================================
//  CH422G I/O expander (I2C "register-as-address" device)
// ============================================================
#define CH422G_REG_MODE 0x24   // write 0x01 -> IO0..IO7 push-pull output
#define CH422G_REG_OUT  0x38   // output bits: b1=TP_RST b2=DISP/BL b3=LCD_RST b4=SD_CS
#define CH422G_OUT_ALL_ON 0x1E // normal running value (backlight on)
#define CH422G_OUT_BL_OFF 0x1A // b2 cleared -> backlight off

#define WS_I2C_SDA 8
#define WS_I2C_SCL 9
#define TOUCH_I2C_ADDR 0x5D    // pinned by the controlled reset below (alt: 0x14)

static void ch422g_write_boot(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// Boot-time init using Wire, then release the bus to LovyanGFX (rule #5).
static void ch422g_init() {
  Wire.begin(WS_I2C_SDA, WS_I2C_SCL);
  ch422g_write_boot(CH422G_REG_MODE, 0x01);      // all IOs push-pull out

  // GT911 controlled reset: hold INT low through the reset window so the
  // controller latches address 0x5D deterministically every power cycle.
  pinMode(GPIO_NUM_4, OUTPUT);
  digitalWrite(GPIO_NUM_4, LOW);
  ch422g_write_boot(CH422G_REG_OUT, 0x1C);       // TP_RST low, rest high
  delay(12);
  ch422g_write_boot(CH422G_REG_OUT, CH422G_OUT_ALL_ON);
  delay(60);                                     // address latch window
  pinMode(GPIO_NUM_4, INPUT);                    // hand INT back to GT911
  delay(60);
  Wire.end();                                    // LovyanGFX owns the bus now
}

// Runtime CH422G write via LovyanGFX's I2C layer (same port the GT911 uses,
// same thread — loop context only).
static void ch422g_write_runtime(uint8_t reg, uint8_t val) {
  lgfx::i2c::transactionWrite(0 /*i2c port*/, reg, &val, 1, 400000);
}

// ============================================================
//  Panel + touch (proven config, verbatim from v5/v6)
// ============================================================
class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB     _bus;
  lgfx::Panel_RGB   _panel;
  lgfx::Touch_GT911 _touch;

  LGFX(void) {
    {
      auto cfg = _panel.config();
      cfg.memory_width  = 800;  cfg.memory_height = 480;
      cfg.panel_width   = 800;  cfg.panel_height  = 480;
      cfg.offset_x = 0;         cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;
      cfg.pin_d0  = GPIO_NUM_14; cfg.pin_d1  = GPIO_NUM_38; cfg.pin_d2  = GPIO_NUM_18;
      cfg.pin_d3  = GPIO_NUM_17; cfg.pin_d4  = GPIO_NUM_10;                       // B0-B4
      cfg.pin_d5  = GPIO_NUM_39; cfg.pin_d6  = GPIO_NUM_0;  cfg.pin_d7  = GPIO_NUM_45;
      cfg.pin_d8  = GPIO_NUM_48; cfg.pin_d9  = GPIO_NUM_47; cfg.pin_d10 = GPIO_NUM_21; // G0-G5
      cfg.pin_d11 = GPIO_NUM_1;  cfg.pin_d12 = GPIO_NUM_2;  cfg.pin_d13 = GPIO_NUM_42;
      cfg.pin_d14 = GPIO_NUM_41; cfg.pin_d15 = GPIO_NUM_40;                       // R0-R4
      cfg.pin_henable = GPIO_NUM_5;  cfg.pin_vsync = GPIO_NUM_3;
      cfg.pin_hsync   = GPIO_NUM_46; cfg.pin_pclk  = GPIO_NUM_7;
      cfg.freq_write = 14000000;     // 12 MHz if pixel drift ever returns
      cfg.hsync_polarity = 0; cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4; cfg.hsync_back_porch = 8;
      cfg.vsync_polarity = 0; cfg.vsync_front_porch = 16;
      cfg.vsync_pulse_width = 4; cfg.vsync_back_porch = 16;
      cfg.pclk_active_neg = 1; cfg.de_idle_high = 0; cfg.pclk_idle_high = 0;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 0; cfg.x_max = 799; cfg.y_min = 0; cfg.y_max = 479;
      cfg.pin_int = GPIO_NUM_4;
      cfg.pin_rst = -1;              // reset handled by CH422G in ch422g_init
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.pin_sda = WS_I2C_SDA; cfg.pin_scl = WS_I2C_SCL;
      cfg.freq = 400000;
      cfg.i2c_addr = TOUCH_I2C_ADDR;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX lcd;                                  // referenced by web.cpp (/screen.bmp)
static bool s_backlight = true;

// ============================================================
//  LVGL glue
// ============================================================
// Draw buffers live in PSRAM: two slices of 800x120 px (192 KB each). The RGB
// panel keeps its own full framebuffer inside LovyanGFX; pushImage() writes
// into it and the panel DMA-scans continuously.
#define BUF_LINES 120
static lv_disp_draw_buf_t s_drawBuf;
static lv_color_t* s_buf1 = nullptr;
static lv_color_t* s_buf2 = nullptr;

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  // FIRST-FLASH CHECK: if colors are wrong, toggle setSwapBytes in init below.
  lcd.pushImage(area->x1, area->y1, w, h, (uint16_t*)px);
  lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  uint16_t x, y;
  if (lcd.getTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

bool halDisplayInit() {
  ch422g_init();
  lcd.init();
  lcd.setSwapBytes(false);       // native RGB565 into the RGB panel FB
  lcd.fillScreen(TFT_BLACK);

  lv_init();

  size_t bufBytes = SCR_W * BUF_LINES * sizeof(lv_color_t);
  s_buf1 = (lv_color_t*)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  s_buf2 = (lv_color_t*)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  if (!s_buf1 || !s_buf2) {
    Serial.println("[hal] PSRAM draw buffer alloc FAILED - check PSRAM=OPI PSRAM");
    return false;
  }
  lv_disp_draw_buf_init(&s_drawBuf, s_buf1, s_buf2, SCR_W * BUF_LINES);

  static lv_disp_drv_t dispDrv;
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = SCR_W;
  dispDrv.ver_res = SCR_H;
  dispDrv.flush_cb = flush_cb;
  dispDrv.draw_buf = &s_drawBuf;
  lv_disp_drv_register(&dispDrv);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touch_cb;
  lv_indev_drv_register(&indevDrv);

  Serial.println("[hal] display + touch + lvgl up");
  return true;
}

void halBacklight(bool on) {
  if (on == s_backlight) return;
  s_backlight = on;
  ch422g_write_runtime(CH422G_REG_OUT, on ? CH422G_OUT_ALL_ON : CH422G_OUT_BL_OFF);
}

bool halBacklightState() { return s_backlight; }

bool halTouchRead(int32_t* x, int32_t* y) {
  uint16_t tx, ty;
  if (!lcd.getTouch(&tx, &ty)) return false;
  if (x) *x = tx;
  if (y) *y = ty;
  return true;
}

void halReadRect(int x, int y, int w, int h, uint16_t* buf) {
  lcd.readRect(x, y, w, h, buf);
}
