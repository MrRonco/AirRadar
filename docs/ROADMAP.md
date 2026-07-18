# Roadmap — parked ideas (owner-approved candidates)

- **Military/interesting flag**: local feed carries `dbFlags`; draw a marker glyph on
  flagged targets and a row in the Selected Aircraft card.
- **Label decluttering**: alternate label offsets (or simple occupied-rect avoidance)
  for dense clusters; selected target already draws last/on top.
- **Dusk dimming**: backlight is on/off only, so scale the palette by local time
  (NTP already synced) — e.g. multiply all colors 60% between civil dusk and dawn.
- **Feeder stats readout**: poll tar1090 `stats.json` occasionally and show
  positions/sec + msgs/sec next to the SUDCRATER source tag.
- **Airframe names**: local feed includes `desc` ("BOEING 777-300ER") — show it in the
  Selected Aircraft card when present (local source only).
