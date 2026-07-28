# AirRadar web flasher (ESP Web Tools)

One-click browser install for the ESP32-S3, hosted on GitHub Pages.

## What's here
- `index.html` — the install page (loads ESP Web Tools, styled to match AirRadar).
- `manifest.json` — points ESP Web Tools at the firmware image.
- `airradar-merged.bin` — full first-install image (bootloader+partitions+app).
- `airradar-ota.bin` — app-only image for over-the-air updates via `/update`.

## Hosting on GitHub Pages
1. Commit this `flasher/` folder.
2. Repo → Settings → Pages → Source: deploy from branch, folder `/flasher` (or move to `/docs`).
3. The installer lives at `https://mrronco.github.io/AirRadar/` (or `/flasher/`).

## Producing the flashable image
ESP Web Tools flashes a single **merged** binary at offset 0. After building the
firmware, merge the parts:

```bash
esptool.py --chip esp32s3 merge_bin -o flasher/airradar-merged.bin \
  0x0 bootloader.bin  0x8000 partitions.bin  0x10000 airradar.bin
```

(Exact offsets come from the build output / partition scheme — 3MB app / 9.9MB FATFS,
matching CLAUDE.md.) Commit the merged bin, and the install button goes live.

## Notes
- Requires desktop Chrome/Edge (Web Serial). Firefox/Safari can't flash.
- The page is static — no secrets, no backend. Safe to host publicly.
