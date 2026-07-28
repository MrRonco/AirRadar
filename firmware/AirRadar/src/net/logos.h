// logos.h — runtime airline logo fetch (theqkash/esp32flight-logos, 90x90 PNG
// keyed by 3-letter ICAO callsign prefix), scaled to the 46px Selected tile.
//
// Flow: UI calls logosRequest(icao3) for the selected aircraft; a single
// core-0 task fetches + decodes into a PSRAM RGB565 cache slot; UI polls
// logosGet() each tick and swaps the tile image in when it lands. Misses are
// cached too, so a 404 never refetches. Logos are trademarks of their owners,
// used solely for identification (see the source repo's provenance note).
#pragma once
#include <lvgl.h>
#include "../core/state.h"

#define LOGO_PX 46                 // rendered tile size (SEL_TILE_S)

enum LogoState : uint8_t { LOGO_UNKNOWN = 0, LOGO_PENDING, LOGO_OK, LOGO_MISS };

void logosLoop(uint32_t nowMs);              // loop context: spawn queued fetch
void logosRequest(const char* icao3);        // queue a lookup (no-op if cached)
// LOGO_OK: *out points at a valid image descriptor (stable until slot reuse).
LogoState logosGet(const char* icao3, const lv_img_dsc_t** out);
