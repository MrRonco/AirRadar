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

The merge must include `boot_app0.bin` at `0xe000` and must pass
**`--flash-mode keep --flash-freq keep`**. Forcing `qio` rewrites byte 2 of the
bootloader header, which the ROM reads before it can load anything — the board builds its
bootloader as DIO, and a QIO header there produces a watchdog boot loop from an image
whose offsets, magics and digests all verify. v7.2.3's published installer shipped with
exactly that fault.

Verify before committing — **by comparison, never by installing it on a configured
device.** The merged image is a first-install image: `merge_bin` pads the gaps with
0xFF and `0x9000-0xdfff` is the NVS partition, so installing it wipes every setting,
including the Wi-Fi password, which is never recoverable by design. The component
check in [`../firmware/BUILD.md`](../firmware/BUILD.md) section C costs nothing and
proves more. A quick version sanity check on the OTA image:

```bash
python3 -c "import re;print(sorted(set(re.findall(rb'7\\.\\d+\\.\\d+', open('flasher/airradar-ota.bin','rb').read()))))"
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
