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

// MUST equal SEL_TILE_S in ui_cards.cpp — a static_assert there enforces it.
// The 90 px source is downscaled to this ONCE at fetch time, so matching the
// tile costs nothing at draw time; scaling at draw time with lv_img_set_zoom
// would put a transform on every card repaint instead.
// Changing this also changes the cache record size, and fsLoad() rejects any
// file whose size does not match, so stale blobs self-invalidate and refetch.
#define LOGO_PX 36

enum LogoState : uint8_t { LOGO_UNKNOWN = 0, LOGO_PENDING, LOGO_OK, LOGO_MISS };

void logosBegin();                           // mount FATFS cache (call in setup)
void logosLoop(uint32_t nowMs);              // loop context: land results, spawn
                                             //  queued fetch, prefetch visible
void logosRequest(const char* icao3);        // queue a lookup (no-op if cached)
// LOGO_OK: *out points at a valid image descriptor (stable until slot reuse).
LogoState logosGet(const char* icao3, const lv_img_dsc_t** out);

// "JZA238" -> "JZA" when the callsign matches the airline pattern
// (3 letters + digit); returns false for GA/reg-style callsigns.
bool logosIcaoFromFlight(const char* flight, char out[4]);
