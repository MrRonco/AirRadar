# AirRadar web flasher (ESP Web Tools)

One-click browser install for the ESP32-S3, hosted on GitHub Pages.

## What's here
- `index.html` — the install page (loads ESP Web Tools, styled to match AirRadar).
- `manifest.json` — points ESP Web Tools at the firmware image.
- `airradar-merged.bin` — full first-install image (bootloader+partitions+boot_app0+app).
- `airradar-ota.bin` — app-only image for over-the-air updates via `/update`.

## Hosting on GitHub Pages

GitHub Pages can only publish from the repository **root** or from `/docs` — there is no
option to publish an arbitrary folder such as `/flasher`. Publish from root; the
installer then lands at the `/flasher/` path underneath it.

1. The repository must be **public** — Pages on a private repo requires a paid plan.
2. Repo → Settings → Pages → Source: **Deploy from a branch**, branch `main`,
   folder **`/ (root)`**.
3. The installer is then live at
   **https://mrronco.github.io/AirRadar/flasher/**

Or from the CLI:

```bash
gh api --method POST repos/MrRonco/AirRadar/pages \
  -f 'source[branch]=main' -f 'source[path]=/'
```

## Producing the flashable image

ESP Web Tools flashes a single **merged** binary at offset 0. The authoritative build and
merge recipe is [`../firmware/BUILD.md`](../firmware/BUILD.md) section C — follow that
one, so there is only ever a single source of truth.

The merge must include `boot_app0.bin` at `0xe000` and the correct flash flags
(`--flash-mode qio --flash-freq 80m --flash-size 16MB`). Omit either and you get an image
that appears to flash but does not boot correctly.

Verify before committing:

```bash
python3 -c "print('7.1.0' in open('flasher/airradar-ota.bin','rb').read().decode('latin1'))"
```

Keep `manifest.json`'s `version` and the footer in `index.html` in step with `AR_VERSION`
in `firmware/AirRadar/src/config.h`, or the install page advertises a version it is not
shipping.

## Licences

The images here are **object code** and carry notice obligations that the source tree
alone does not.

AirRadar is **GPL-3.0-or-later**. The Corresponding Source for these binaries is this
repository at the commit that produced them, and the build recipe is
[`../firmware/BUILD.md`](../firmware/BUILD.md) — that satisfies GPL §6.

They also statically link:

- **arduino-esp32** — LGPL-2.1-or-later. Source: <https://github.com/espressif/arduino-esp32>
- **ESP-IDF / mbedTLS** — Apache-2.0
- **LovyanGFX** — BSD-2-Clause (its clause 2 requires the notice travel with binary distributions)
- **LVGL, ArduinoJson, PubSubClient** — MIT
- **Inter, JetBrains Mono, Montserrat** glyph data — SIL OFL 1.1

Full texts are in [`../LICENSES/`](../LICENSES/); the summary is
[`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md). If you redistribute these
`.bin` files anywhere other than this repository, carry those notices with them.

## Notes
- Requires desktop Chrome/Edge (Web Serial). Firefox and Safari cannot flash.
- The page is static — no secrets, no backend. Safe to host publicly.
- If the flasher reports *"No serial data received"*, flip the UART slide switch on the
  board.
