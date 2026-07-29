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
// HARDWARE-TUNED: the draw buffer lives in INTERNAL SRAM (single 800x60 slice,
// 96 KB). With PSRAM draw buffers, every redraw was render-write + flush-read
// on the same PSRAM bus the panel DMA scans at ~32 MB/s — visible as screen
// wiggle whenever the map image forced real compositing. Internal SRAM keeps
// rendering off that bus; only the final pushImage writes PSRAM. Falls back
// to PSRAM if the heap can't give 96 KB (then expect the wiggle, and consider
// dropping freq_write to 12 MHz as documented in CLAUDE.md).
// 30 lines (800x30 = 48 KB), NOT 60. The 96 KB version was the single largest
// discretionary claim on internal SRAM and it starved everything else: free
// internal heap settled around 17 KB, permanently below AR_TLS_HEAP_FLOOR, so
// routes/weather/logo fetches were shed forever and the device eventually
// rebooted. Halving it returns 48 KB — and a 48 KB CONTIGUOUS region, which
// also un-breaks the 12 KB network task stacks (heap_largest was 10 KB).
// Safe for rendering: LVGL sizes each flush chunk as buffer_px / AREA width,
// not screen width, so small invalidations still flush in one pass; only
// full-screen repaints cost an extra chunk or two.
#define BUF_LINES 30
static lv_disp_draw_buf_t s_drawBuf;
static lv_color_t* s_buf1 = nullptr;

// Swap the 16-bit halves of every pixel, 2 pixels per 32-bit word.
static inline void swapPixels565(uint16_t* p, uint32_t n) {
  if (((uintptr_t)p & 3u) == 0) {              // buffer base is always aligned
    uint32_t* q = (uint32_t*)p;
    for (uint32_t i = 0, pairs = n >> 1; i < pairs; i++) {
      uint32_t v = q[i];
      q[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
    if (n & 1u) p[n - 1] = (uint16_t)((p[n - 1] << 8) | (p[n - 1] >> 8));
    return;
  }
  for (uint32_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] << 8) | (p[i] >> 8));
}

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  // The panel framebuffer holds byte-swapped 565. We used to get that by
  // leaving setSwapBytes(true), but that makes LovyanGFX pick the rgb565_t
  // pixelcopy specialisation, which sets no_convert=false and therefore skips
  // Panel_FrameBufferBase::writeImage's per-row memcpy fast path — every
  // flushed pixel became an individual read-convert-write into PSRAM, on the
  // same bus the panel DMA is scanning at ~25 MB/s.
  // Swapping here instead keeps that work in INTERNAL SRAM and lets pushImage
  // bulk-memcpy whole rows into PSRAM. Colours are identical either way.
  swapPixels565((uint16_t*)px, (uint32_t)w * (uint32_t)h);
  lcd.pushImage(area->x1, area->y1, w, h, (uint16_t*)px);
  lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  uint16_t x, y;
  // Panel dark (night mode): swallow input so the wake-up tap can't also
  // click whatever invisible widget sits under the finger.
  if (s_backlight && lcd.getTouch(&x, &y)) {
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
  // HARDWARE-VERIFIED on the real panel: the RGB path wants byte-swapped 565
  // (with plain RGB565, #0c1119 rendered as olive (128,96,64)).
  // We produce that swap ourselves in flush_cb / halReadRect and leave
  // LovyanGFX's own flag OFF, because turning it on costs the row-memcpy fast
  // path — see the comment in flush_cb. LVGL stays LV_COLOR_16_SWAP=0, so the
  // map buffer and baked image assets remain plain RGB565.
  lcd.setSwapBytes(false);
  lcd.fillScreen(TFT_BLACK);

  lv_init();

  size_t bufBytes = SCR_W * BUF_LINES * sizeof(lv_color_t);
  s_buf1 = (lv_color_t*)heap_caps_malloc(bufBytes,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (s_buf1) {
    Serial.printf("[hal] draw buffer: %u KB internal SRAM\n",
                  (unsigned)(bufBytes / 1024));
  } else {
    s_buf1 = (lv_color_t*)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
    Serial.println("[hal] WARNING: internal draw buffer failed - using PSRAM "
                   "(screen wiggle possible)");
  }
  if (!s_buf1) {
    Serial.println("[hal] draw buffer alloc FAILED - check PSRAM=OPI PSRAM");
    return false;
  }
  lv_disp_draw_buf_init(&s_drawBuf, s_buf1, NULL, SCR_W * BUF_LINES);

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
  // readRect honours the same _swapBytes flag as pushImage. With it off we get
  // the framebuffer's raw swapped 565, so undo it here — callers (/screen.bmp)
  // expect plain RGB565 with red in bits 11..15.
  swapPixels565(buf, (uint32_t)w * (uint32_t)h);
}
