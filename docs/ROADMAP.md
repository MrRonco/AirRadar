# Roadmap

## Shipped since this list was written

- **Military flag** — `dbFlags` drives a box glyph on the target (legend documents it).
- **Label decluttering** — spatial rule, not alternating offsets: walk targets
  nearest-first and grant a label only when nothing already labelled is within
  46 px, capped at 8. Selection/watchlist/emergency are seeded first so they
  always win. ("Nearest N" was tried and is *worse* — the nearest targets are
  by definition clustered at the centre, so they pile up on each other.)
- **Feeder stats readout** — msg/s sits in the Overview status line.
- **Airframe names** — `desc` renders in the Selected card.
- **Full-bleed base map** with a coverage-lens dim outside the range circle.
- **Persistent caches on FATFS** — stitched maps per range, airline logos,
  adsbdb routes. All three survive reboots and the TLS gate closing.
- **Legend overlay** behind the "?" button.
- **Desktop web console** — live status strip, traffic table, panel mirror.

## Still parked

- **Aircraft type silhouettes** on the Selected card — the conventional
  avionics answer, self-drawn so no licensing entanglement, keyed off `t`.
- **Dusk dimming** — backlight is on/off only, so scale the palette by local
  time rather than the backlight. Night mode currently just blanks.
- **Route ETA**, **session stats** (max range today, peak count), **trails**,
  **web `/live` page**.
- **Aircraft photos** — blocked on licensing, not code. See `V7_PORT.md`.

## Known-unsolved

- **Internal heap drain, ~72 B/s.** Tracks feeder poll count; not TLS, not
  TIME_WAIT (both measured and ruled out — see `V7_PORT.md` note 9). Largely
  neutralised by the FATFS caches, but weather and new logo fetches still stop
  once the gate shuts, roughly 25–30 minutes into a boot. Needs heap tracing.
- **Settings screen** never had the geometry pass the main screen got; it works
  but was laid out for the old 184 px card widths.
