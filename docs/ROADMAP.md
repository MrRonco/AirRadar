# Roadmap

What is shipped, what is parked, and what is genuinely unsolved. The
version-by-version narrative lives in [`HISTORY.md`](HISTORY.md); this file is
only the forward-looking part.

## Shipped

**v7.1** — military flag · spatial label decluttering (nearest-first, 46 px
exclusion, capped at 8; "nearest N" was tried and is *worse*, because the
nearest targets are by definition clustered at the centre) · feeder msg/s in
the status line · airframe names · full-bleed base map with a coverage lens ·
FATFS caches for maps, logos and routes · legend overlay · desktop web console.

**v7.2 – v7.2.3** — the internal-heap drain, the whole-screen shake and an
oversized repaint, all three measured rather than guessed at
([`V7_PORT.md`](V7_PORT.md) notes 12–14) · weather row · clock and time zones.

**v7.2.4** — a 40-finding design review: type scale, contrast, the two-column
Selected card, label side-selection, the bearing bezel, and a card-clipping bug
that came down to LVGL's content box subtracting the border as well as the
padding.

**v7.2.5** — a second audit, 31 findings. The panel stopped asserting things it
could not know (nearest-40 selection, `OF n` disclosure, `LIVE` on a departed
pin), gained an approach readout and one hour of traffic history, and the
console became usable on a phone.

## Still parked

- **Aircraft type silhouettes** on the Selected card — the conventional
  avionics answer, self-drawn so no licensing entanglement, keyed off `t`.
- **Dusk dimming** — the backlight is on/off only, so this means scaling the
  palette by local time rather than the backlight. Night mode blanks, now with
  tap-to-wake and an emergency override.
- **Trails**, **a web `/live` page**, **ETA to destination**.
- **Session stats beyond the hour** — max range today, peak count. The v7.2.5
  sparkline holds 60 minutes in RAM and deliberately writes nothing to flash;
  anything longer has to answer CLAUDE.md rule 22 first.
- **Aircraft photos** — blocked on licensing, not code. See
  [`V7_PORT.md`](V7_PORT.md).

## Known-unsolved

- **A flash write will always shake this panel.** Flash and PSRAM share the
  MSPI bus and `CONFIG_SPI_FLASH_AUTO_SUSPEND` is not set in the prebuilt
  arduino-esp32 libraries. Write frequency is mitigation; the cure needs a core
  rebuild, which would cost the "one `arduino-cli compile` away" property the
  project is built on. Same wall as standalone heap tracing.
- **Time zones are a curated table, not tzdata**, so a DST-rule change needs a
  firmware update.

## Solved since this list was first written

Both of the original entries here are closed, and the reasons are worth keeping:

- ~~**Internal heap drain, ~72 B/s.**~~ Solved in v7.2. It was `vTaskDelete(NULL)`
  skipping a `DynamicJsonDocument`'s destructor in `issTask`. This file
  previously stated the drain "tracks feeder poll count" — that was read off a
  counter which was never incremented, and it is exactly the kind of confident
  wrong attribution the rest of these documents exist to prevent.
- ~~**The settings screen never had the geometry pass the main screen got.**~~
  Done across v7.2.4 and v7.2.5: 44 px rows, outlined chips, visible
  scrollbars, a list-based time zone picker and a metric/imperial switch.
