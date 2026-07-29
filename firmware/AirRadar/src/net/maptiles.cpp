// maptiles.cpp — CARTO dark_all slippy-tile base map for the scope.
//
// Flow: mapRequestRefresh() (loop ctx) arms a fetch. mapLoop() (loop ctx)
// snapshots {lat,lon,rangeKm} into a job struct and spawns a core-0 task.
// The task downloads the 3x3 tile grid around home, stitches the tiles into
// a 768x768 mosaic, nearest-neighbour resamples the metre-window into the
// BACK buffer at MAP_SIZE with the approved blue tint + vignette, then sets
// stitchedReady. mapLoop() swaps front/back and bumps the generation counter
// so the UI re-sets its image source. A failed fetch never touches the front
// buffer; it earns one 30 s retry, then waits for the next refresh request.
//
// Threading contract: the task touches ONLY module-private buffers and
// volatile flags — no g_set reads (job snapshot is taken in loop context),
// no g_tracks, no lv_* calls (the lv_img_dsc_t is plain data).

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include "maptiles.h"

// ---------- file-local constants ----------
static const int      TILE_PX          = 256;                  // slippy tile edge
// 5x3, not 3x3: the output is now 800x480 at the disc's metres-per-pixel, so a
// 768 px mosaic no longer spans it. Worst-case centring needs 2 tiles of margin
// horizontally and 1 vertically.
static const int      GRID_W           = 5;
static const int      GRID_H           = 3;
static const int      MOSAIC_W         = TILE_PX * GRID_W;     // 1280
static const int      MOSAIC_H         = TILE_PX * GRID_H;     // 768
static const int      ZOOM_MIN         = 3;
static const int      ZOOM_MAX         = 12;
static const size_t   TILE_BUF_BYTES   = 80 * 1024;            // PNG download buffer
static const size_t   MAP_BUF_BYTES    = (size_t)MAP_W * MAP_H * 2;   // 768,000
static const size_t   MOSAIC_BUF_BYTES = (size_t)MOSAIC_W * MOSAIC_H * 2;
static const uint32_t HTTP_TIMEOUT_MS  = 10000;
static const uint32_t STALL_TIMEOUT_MS = 4000;                 // mid-stream stall guard
static const uint32_t RETRY_DELAY_MS   = 30000;
static const double   MERC_MPP_Z0      = 156543.03;            // m/px at z0, equator
static const double   MERC_LAT_LIMIT   = 85.0;                 // usable Mercator band
static const size_t   PNG_SIG_LEN      = 8;

// Blue tint ramp (matches the approved mock) + edge vignette.
// HARDWARE-TUNED: x2.6 lift (calibrated on a dark z8 tile) rendered water
// neon-bright at the z9-z11 the scope actually uses. x1.6 + lower blue floor
// matches the approved slate-dark mock on the real panel.
static const int   TINT_LUM_NUM      = 16;   // luminance boost x1.6
static const int   TINT_LUM_DEN      = 10;
static const int   TINT_R_PCT        = 32,  TINT_R_ADD = 7;
static const int   TINT_G_PCT        = 62,  TINT_G_ADD = 14;
static const int   TINT_B_PCT        = 105, TINT_B_ADD = 26;
static const float VIGNETTE_STRENGTH = 0.55f;

// ---------- module state ----------
// Buffers: front is read by LVGL (loop ctx); back is written by the fetch
// task; mosaic + tileBuf are task-private scratch. All PSRAM.
static uint16_t* s_bufA    = nullptr;
static uint16_t* s_bufB    = nullptr;
static uint16_t* s_front   = nullptr;
static uint16_t* s_back    = nullptr;
static uint16_t* s_mosaic  = nullptr;
static uint8_t*  s_tileBuf = nullptr;

static lv_img_dsc_t s_imgDsc;                  // describes s_front for the UI
static bool          s_haveImage  = false;     // first stitch landed
static uint32_t      s_generation = 0;

struct MapJob { double lat; double lon; int rangeKm; };
static MapJob s_job;                           // written in loop ctx before spawn

static volatile bool s_fetchInFlight = false;  // task alive
static volatile bool s_stitchedReady = false;  // back buffer complete (set LAST)
static volatile bool s_fetchFailed   = false;  // task reported failure

// loop-context scheduling state
static bool     s_refreshWanted = false;
static bool     s_retryUsed     = false;       // one retry per refresh request
static bool     s_retryArmed    = false;
static uint32_t s_retryAtMs     = 0;

// ============================================================
//  Task-side helpers (core 0) — module buffers only
// ============================================================

// Pick the integer zoom whose metres/pixel best matches the scope scale.
static int chooseZoom(double latDeg, int rangeKm) {
  const double target = (2.0 * (double)rangeKm * 1000.0) / (double)MAP_SIZE;
  const double cosLat = cos(latDeg * M_PI / 180.0);
  int best = ZOOM_MIN;
  double bestErr = 1e30;
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    const double mpp = MERC_MPP_Z0 * cosLat / (double)(1 << z);
    const double err = fabs(mpp - target);
    if (err < bestErr) { bestErr = err; best = z; }
  }
  return best;
}

// GET one tile PNG into s_tileBuf (readBytes loop honouring Content-Length,
// same pattern as the proven v6 Esri fetch). Validates the PNG signature.
static bool fetchTileToBuf(WiFiClientSecure& client, int z, int x, int y,
                           size_t& outLen) {
  outLen = 0;
  char url[128];
  snprintf(url, sizeof(url), "https://%s/%s/%d/%d/%d.png",
           AR_TILE_HOST, AR_TILE_STYLE, z, x, y);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent(AR_USER_AGENT);
  if (!http.begin(client, url)) {
    Serial.printf("[map] http.begin failed: %s\n", url);
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    Serial.printf("[map] tile %d/%d/%d -> HTTP %d\n", z, x, y, code);
    http.end();
    return false;
  }
  const int clen = http.getSize();
  if (clen > (int)TILE_BUF_BYTES) {
    Serial.printf("[map] tile %d/%d/%d too big (%d B)\n", z, x, y, clen);
    http.end();
    return false;
  }
  WiFiClient* st = http.getStreamPtr();
  size_t total = 0;
  uint32_t t0 = millis();
  while (http.connected() && (clen < 0 || (int)total < clen) &&
         total < TILE_BUF_BYTES) {
    const size_t avail = st->available();
    if (avail) {
      size_t chunk = TILE_BUF_BYTES - total;
      if (avail < chunk) chunk = avail;
      const int r = st->readBytes(s_tileBuf + total, chunk);
      if (r > 0) { total += (size_t)r; t0 = millis(); }
    } else {
      if (millis() - t0 > STALL_TIMEOUT_MS) break;
      delay(4);
    }
  }
  http.end();
  static const uint8_t PNG_SIG[PNG_SIG_LEN] =
      {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (clen >= 0 && total < (size_t)clen) {
    Serial.printf("[map] tile %d/%d/%d short read (%u/%d B)\n",
                  z, x, y, (unsigned)total, clen);
    return false;
  }
  if (total < PNG_SIG_LEN || memcmp(s_tileBuf, PNG_SIG, PNG_SIG_LEN) != 0) {
    Serial.printf("[map] tile %d/%d/%d not a PNG (%u B)\n",
                  z, x, y, (unsigned)total);
    return false;
  }
  outLen = total;
  return true;
}

// Decode s_tileBuf into the reusable sprite, then blit into the mosaic at
// grid slot (gx,gy). Sprite raw buffer is swap565 — handled at tint time.
static bool decodeTileToMosaic(LGFX_Sprite& spr, size_t pngLen, int gx, int gy) {
  spr.fillScreen(0);
  if (!spr.drawPng(s_tileBuf, (uint32_t)pngLen, 0, 0)) {
    Serial.printf("[map] PNG decode failed (grid %d,%d)\n", gx, gy);
    return false;
  }
  const uint16_t* src = (const uint16_t*)spr.getBuffer();
  if (!src) return false;
  for (int row = 0; row < TILE_PX; row++) {
    memcpy(&s_mosaic[(size_t)(gy * TILE_PX + row) * MOSAIC_W + gx * TILE_PX],
           &src[(size_t)row * TILE_PX], (size_t)TILE_PX * 2);
  }
  return true;
}

// Fetch + stitch the 3x3 grid around home into s_mosaic. Outputs the home
// position in mosaic pixels (fractional tile coords). False on any tile
// failure — out-of-bounds y rows are skipped (left black), not failures.
static bool buildMosaic(const MapJob& job, int z, double& cxPx, double& cyPx) {
  const int    n      = 1 << z;
  const double latRad = job.lat * M_PI / 180.0;
  const double xtF    = (job.lon + 180.0) / 360.0 * (double)n;
  const double ytF    = (1.0 - asinh(tan(latRad)) / M_PI) / 2.0 * (double)n;
  const int    xt     = (int)floor(xtF);
  const int    yt     = (int)floor(ytF);
  cxPx = (xtF - (double)(xt - GRID_W / 2)) * TILE_PX;
  cyPx = (ytF - (double)(yt - GRID_H / 2)) * TILE_PX;

  memset(s_mosaic, 0, MOSAIC_BUF_BYTES);

  LGFX_Sprite spr(nullptr);
  spr.setColorDepth(16);
  spr.setPsram(true);
  if (!spr.createSprite(TILE_PX, TILE_PX)) {
    Serial.println("[map] tile sprite alloc failed");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();                 // keyless public CDN, no cert pinning
  bool ok = true;
  for (int gy = 0; gy < GRID_H && ok; gy++) {
    const int ty = yt - GRID_H / 2 + gy;
    if (ty < 0 || ty >= n) continue;    // beyond the poles: leave black
    for (int gx = 0; gx < GRID_W && ok; gx++) {
      int tx = (xt - GRID_W / 2 + gx) % n;   // wrap across the antimeridian
      if (tx < 0) tx += n;
      size_t len = 0;
      if (!fetchTileToBuf(client, z, tx, ty, len) ||
          !decodeTileToMosaic(spr, len, gx, gy)) {
        ok = false;
      }
    }
  }
  spr.deleteSprite();
  return ok;
}

// Nearest-neighbour resample of the metre-window around home from the mosaic
// into s_back, applying the blue tint + vignette. Outside the scope circle
// pixels are written 0x0000. Output is plain RGB565 (LV_COLOR_16_SWAP 0);
// the mosaic holds LovyanGFX swap565, hence the bswap on read.
static void resampleAndTint(const MapJob& job, int z, double cxPx, double cyPx) {
  const double latRad = job.lat * M_PI / 180.0;
  const double mpp    = MERC_MPP_Z0 * cos(latRad) / (double)(1 << z);
  const double halfPx = ((double)job.rangeKm * 1000.0) / mpp; // scope radius, mosaic px
  const double step   = (halfPx * 2.0) / (double)MAP_SIZE;    // mosaic px per out px
  const int    ocx    = MAP_W / 2;
  const int    ocy    = MAP_H / 2;
  const float  rIn    = (float)(SCOPE_R - MAP_DIM_FEATHER);
  const float  rOut   = (float)(SCOPE_R + MAP_DIM_FEATHER);

  for (int oy = 0; oy < MAP_H; oy++) {
    const double my = cyPx + ((double)oy + 0.5 - ocy) * step;
    int syi = (int)my;
    if (syi < 0) syi = 0; else if (syi >= MOSAIC_H) syi = MOSAIC_H - 1;
    const uint16_t* srcRow = &s_mosaic[(size_t)syi * MOSAIC_W];
    uint16_t*       dstRow = &s_back[(size_t)oy * MAP_W];
    const int dy = oy - ocy;

    for (int ox = 0; ox < MAP_W; ox++) {
      const int   dx = ox - ocx;
      const float d  = sqrtf((float)(dx * dx + dy * dy));
      const double mx = cxPx + ((double)ox + 0.5 - ocx) * step;
      int sxi = (int)mx;
      if (sxi < 0) sxi = 0; else if (sxi >= MOSAIC_W) sxi = MOSAIC_W - 1;

      const uint16_t c = __builtin_bswap16(srcRow[sxi]);      // swap565 -> rgb565
      const int r8 = ((c >> 11) & 0x1F) << 3;
      const int g8 = ((c >> 5) & 0x3F) << 2;
      const int b8 = (c & 0x1F) << 3;
      int lum = (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
      lum = (lum * TINT_LUM_NUM) / TINT_LUM_DEN;
      if (lum > 255) lum = 255;
      int r = (lum * TINT_R_PCT) / 100 + TINT_R_ADD;
      int g = (lum * TINT_G_PCT) / 100 + TINT_G_ADD;
      int b = (lum * TINT_B_PCT) / 100 + TINT_B_ADD;
      if (b > 255) b = 255;

      // Coverage lens: full brightness inside the range circle, MAP_DIM_PCT
      // outside it, with a short feather across the boundary. The circle then
      // means something — it is the edge of what the receiver can see — and the
      // darkened field is what lets the cards stay opaque on top of the map.
      float k;
      if (d <= rIn)       k = 1.0f - VIGNETTE_STRENGTH * (d * d) / (float)(SCOPE_R * SCOPE_R);
      else if (d >= rOut) k = MAP_DIM_PCT / 100.0f;
      else {
        const float t  = (d - rIn) / (rOut - rIn);           // 0..1 across the edge
        const float in = 1.0f - VIGNETTE_STRENGTH;
        k = in + (MAP_DIM_PCT / 100.0f - in) * t;
      }
      r = (int)((float)r * k);
      g = (int)((float)g * k);
      b = (int)((float)b * k);
      dstRow[ox] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
}

// Core-0 task: mosaic -> back buffer -> ready flag. Flag order matters:
// stitchedReady is set only after s_back is fully written; the in-flight
// flag is cleared last so mapLoop never swaps under a live task.
static void mapFetchTask(void*) {
  if (!tlsTryAcquire()) {              // one TLS user at a time (heap budget)
    Serial.println("[map] TLS busy - fetch deferred");
    s_fetchFailed = true;              // existing retry path re-requests soon
    s_fetchInFlight = false;
    vTaskDelete(NULL);
    return;
  }
  const MapJob job = s_job;            // snapshot was taken in loop context
  const int z = chooseZoom(job.lat, job.rangeKm);
  s_mosaic = (uint16_t*)heap_caps_malloc(MOSAIC_BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_mosaic) {
    Serial.println("[map] mosaic alloc failed - skipping this refresh");
    tlsRelease();
    s_fetchFailed = true;
    s_fetchInFlight = false;
    vTaskDelete(NULL);
    return;
  }
  Serial.printf("[map] fetch lat=%.4f lon=%.4f range=%dkm -> z%d\n",
                job.lat, job.lon, job.rangeKm, z);
  double cxPx = 0.0, cyPx = 0.0;
  bool ok = buildMosaic(job, z, cxPx, cyPx);
  tlsRelease();
  if (ok) {
    resampleAndTint(job, z, cxPx, cyPx);
  }
  heap_caps_free(s_mosaic);            // 1.9 MB back to PSRAM immediately
  s_mosaic = nullptr;
  if (ok) {
    Serial.println("[map] stitch complete");
    s_stitchedReady = true;
  } else {
    Serial.println("[map] fetch failed - previous image kept");
    s_fetchFailed = true;
  }
  s_fetchInFlight = false;
  vTaskDelete(NULL);
}

// ============================================================
//  Loop-context API
// ============================================================

void mapBegin() {
  s_bufA    = (uint16_t*)heap_caps_malloc(MAP_BUF_BYTES, MALLOC_CAP_SPIRAM);
  s_bufB    = (uint16_t*)heap_caps_malloc(MAP_BUF_BYTES, MALLOC_CAP_SPIRAM);
  // s_mosaic is allocated per fetch and freed again — at 1.9 MB it is not worth
  // holding for the whole session just to re-stitch on a range change.
  s_tileBuf = (uint8_t*)heap_caps_malloc(TILE_BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_bufA || !s_bufB || !s_tileBuf) {
    Serial.println("[map] PSRAM alloc failed - map layer disabled");
    if (s_bufA)    { heap_caps_free(s_bufA);    s_bufA = nullptr; }
    if (s_bufB)    { heap_caps_free(s_bufB);    s_bufB = nullptr; }
    if (s_tileBuf) { heap_caps_free(s_tileBuf); s_tileBuf = nullptr; }
    return;
  }
  s_front = s_bufA;
  s_back  = s_bufB;
  memset(s_front, 0, MAP_BUF_BYTES);

  s_imgDsc.header.always_zero = 0;
  s_imgDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  s_imgDsc.header.w  = MAP_W;
  s_imgDsc.header.h  = MAP_H;
  s_imgDsc.data_size = MAP_BUF_BYTES;
  s_imgDsc.data      = (const uint8_t*)s_front;
  Serial.printf("[map] buffers ready (%u KB PSRAM resident, +%u KB per fetch)\n",
                (unsigned)((MAP_BUF_BYTES * 2 + TILE_BUF_BYTES) / 1024),
                (unsigned)(MOSAIC_BUF_BYTES / 1024));
}

void mapLoop(uint32_t nowMs) {
  if (!s_bufA) return;                          // boot allocation failed

  // 1) Publish a finished stitch (task done: ready set, in-flight cleared).
  static int      s_appliedRange = -1;     // what the FRONT image was built for
  static double   s_appliedLat = 999, s_appliedLon = 999;
  static uint32_t s_lastHealMs = 0;
  if (s_stitchedReady && !s_fetchInFlight) {
    s_stitchedReady = false;
    uint16_t* t = s_front;
    s_front = s_back;
    s_back  = t;
    s_imgDsc.data = (const uint8_t*)s_front;
    s_haveImage = true;
    s_generation++;
    s_appliedRange = s_job.rangeKm;        // one fetch at a time: s_job is stable
    s_appliedLat   = s_job.lat;
    s_appliedLon   = s_job.lon;
  }

  // 1b) Self-heal: if the shown map doesn't match the current settings (range
  // cycled, location moved, or an earlier fetch died), quietly re-request.
  // Covers "range button shows more aircraft but the map never rescales".
  if (g_set.mapEn && g_wifiUp && !s_fetchInFlight && !s_stitchedReady &&
      !s_refreshWanted && (int32_t)(nowMs - s_lastHealMs) > 15000) {
    bool mismatch = !s_haveImage ||
                    s_appliedRange != g_set.rangeKm ||
                    fabs(s_appliedLat - g_set.homeLat) > 0.01 ||
                    fabs(s_appliedLon - g_set.homeLon) > 0.01;
    if (mismatch) {
      s_lastHealMs = nowMs;
      s_retryUsed  = false;                // fresh attempt gets a retry again
      s_refreshWanted = true;
      Serial.println("[map] self-heal refetch (range/location/image mismatch)");
    }
  }

  // 2) Failure: one delayed retry per refresh request, then give up.
  if (s_fetchFailed && !s_fetchInFlight) {
    s_fetchFailed = false;
    if (!s_retryUsed) {
      s_retryUsed  = true;
      s_retryArmed = true;
      s_retryAtMs  = nowMs + RETRY_DELAY_MS;
      Serial.printf("[map] retry in %lu s\n",
                    (unsigned long)(RETRY_DELAY_MS / 1000));
    } else {
      Serial.println("[map] retry exhausted - awaiting next refresh request");
    }
  }
  if (s_retryArmed && (int32_t)(nowMs - s_retryAtMs) >= 0) {
    s_retryArmed    = false;
    s_refreshWanted = true;
  }

  // 3) Spawn at most one fetch; snapshot settings here (loop context only).
  //    !s_stitchedReady: never start a new task while a finished stitch is
  //    still waiting to be published (it owns s_back until the swap).
  if (s_refreshWanted && !s_fetchInFlight && !s_stitchedReady &&
      g_set.mapEn && g_wifiUp) {
    // Gate in loop context. mapFetchTask used to spawn a 12 KB internal stack
    // just to find the gate shut, and the 15 s self-heal re-armed it forever.
    // Clearing the want flag first is what stops this from re-testing the heap
    // on every single loop pass; the armed retry brings us back.
    if (!tlsGateOpen()) {
      s_refreshWanted = false;
      s_retryArmed    = true;                  // quiet retry, no task, no churn
      s_retryAtMs     = nowMs + RETRY_DELAY_MS;
      return;
    }
    s_refreshWanted = false;
    double lat = g_set.homeLat;
    if (lat >  MERC_LAT_LIMIT) lat =  MERC_LAT_LIMIT;
    if (lat < -MERC_LAT_LIMIT) lat = -MERC_LAT_LIMIT;
    s_job.lat     = lat;
    s_job.lon     = g_set.homeLon;
    s_job.rangeKm = g_set.rangeKm;
    s_fetchInFlight = true;
    if (xTaskCreatePinnedToCore(mapFetchTask, "mapfetch", AR_NET_TASK_STACK,
                                NULL, 1, NULL, 0) != pdPASS) {
      s_fetchInFlight = false;
      // Arm the delayed retry rather than re-setting s_refreshWanted: the
      // latter retried on the very next loop pass, so a heap too fragmented
      // for a 12 KB stack produced a spawn-fail storm at loop rate.
      s_retryArmed = true;
      s_retryAtMs  = nowMs + RETRY_DELAY_MS;
      Serial.println("[map] task spawn failed");
    }
  }
}

void mapRequestRefresh() {
  if (!s_bufA) return;
  s_refreshWanted = true;                       // coalesces if a fetch is live
  s_retryUsed     = false;
  s_retryArmed    = false;
}

const lv_img_dsc_t* mapImage() {
  if (!s_haveImage || !g_set.mapEn) return NULL;
  return &s_imgDsc;
}

uint32_t mapGeneration() {
  return s_generation;
}
