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

## Notes
- Requires desktop Chrome/Edge (Web Serial). Firefox and Safari cannot flash.
- The page is static — no secrets, no backend. Safe to host publicly.
- If the flasher reports *"No serial data received"*, flip the UART slide switch on the
  board.
