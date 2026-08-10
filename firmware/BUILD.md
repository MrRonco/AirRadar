# Building & flashing AirRadar v7 (macOS)

Two paths: **A. arduino-cli** (recommended — copy-paste, reproducible) or
**B. Arduino IDE 2.x**. Both produce identical firmware. Section **C** covers
making the merged image for the one-click web flasher, **D** covers updates
over the air, **E** first-flash checklist.

---

## 0. One-time software install

```bash
# 1) USB driver for the board's CH343 UART (needs Rosetta on Apple Silicon):
brew install --cask wch-ch34x-usb-serial-driver

# 2) The build tool:
brew install arduino-cli
```

Plug the board in with a **USB-C data cable**. The port appears as
`/dev/cu.wchusbserial*` (or `/dev/cu.usbmodem*` for the native USB port).
If upload later says "No serial data received": flip the onboard **UART slide
switch** — it gates which port is wired to the flasher.

## 1. Install the ESP32 core + libraries (exact pins — do not drift)

```bash
arduino-cli config init --overwrite
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10

arduino-cli lib install "lvgl@8.3.11" "LovyanGFX@1.2.25" \
  "ArduinoJson@6.21.6" "PubSubClient@2.8.0"
```

## 2. Place lv_conf.h  ← the step everyone gets wrong

LVGL looks for `lv_conf.h` **next to** (not inside) its library folder:

```bash
cp firmware/lv_conf.h ~/Documents/Arduino/libraries/lv_conf.h
```

If you ever see `lv_conf.h: No such file or directory` or hundreds of LVGL
config errors, this step is what's missing.

## 3. Compile & flash (path A — arduino-cli)

From the repo root:

```bash
FQBN='esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600'

arduino-cli compile --fqbn "$FQBN" --export-binaries firmware/AirRadar
arduino-cli upload  --fqbn "$FQBN" -p /dev/cu.wchusbserial* firmware/AirRadar
arduino-cli monitor -p /dev/cu.wchusbserial* -c baudrate=115200
```

`PSRAM=opi` is the setting that black-screens the device if missed.

## 3-alt. Arduino IDE 2.x (path B)

1. Boards Manager → install **esp32 by Espressif** `3.3.10`.
2. Library Manager → install the four pinned libraries from step 1.
3. Do step 2 (lv_conf.h) — the IDE uses the same `~/Documents/Arduino/libraries`.
4. Open `firmware/AirRadar/AirRadar.ino`.
5. Tools:
   | Setting | Value |
   |---|---|
   | Board | ESP32S3 Dev Module |
   | USB CDC On Boot | Enabled |
   | Flash Mode | QIO 80 MHz |
   | Flash Size | 16 MB (128 Mb) |
   | Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
   | PSRAM | **OPI PSRAM** |
   | Upload Speed | 921600 |
6. Upload.

## C. Merged image for the web flasher (ESP Web Tools)

`--export-binaries` drops artifacts into
`firmware/AirRadar/build/esp32.esp32.esp32s3/`. Merge them into the single
image the flasher page expects:

```bash
BUILD=firmware/AirRadar/build/esp32.esp32.esp32s3
BOOTAPP=$(find ~/Library/Arduino15/packages/esp32 -name boot_app0.bin | head -1)

python3 -m esptool --chip esp32s3 merge_bin -o flasher/airradar-merged.bin \
  --flash_mode keep --flash_freq keep --flash_size 16MB \
  0x0     "$BUILD/AirRadar.ino.bootloader.bin" \
  0x8000  "$BUILD/AirRadar.ino.partitions.bin" \
  0xe000  "$BOOTAPP" \
  0x10000 "$BUILD/AirRadar.ino.bin"
```

**`keep`, not `qio` — this one bricks the installer.** `--flash-mode` PATCHES
byte 2 of the bootloader header at offset 0, and the ROM reads that byte to
configure the flash interface *before* it loads anything. The Arduino core
builds the bootloader as **DIO (0x02)** on this board; forcing `qio` rewrites
it to 0x00 and the ROM then cannot read flash, so it hangs and the RTC/TG0
watchdogs reset it forever:

```
rst:0x10 (RTCWDT_RTC_RST) / rst:0x7 (TG0WDT_SYS_RST)
load:0x3fce2820,len:0x10cc
ets_loader.c 78
```

Nothing else is wrong with such an image — every offset, magic, digest and the
app itself verify fine, which is why it survived review. The app's OWN header
still says QIO and the bootloader reconfigures the interface once it is
running; only the bootloader's header has to match how the ROM must read
flash. `keep` leaves all three parameters exactly as the core built them.

Verify by comparison, not by flashing a working device — the merged image is a
FIRST-INSTALL image and installing it erases NVS (`merge_bin` pads the gaps
with 0xFF, and 0x9000-0xdfff is the NVS partition). The check that settles it
costs nothing:

```bash
# every component byte-identical to what arduino-cli flashes and boots
python3 - <<'EOF'
B='firmware/AirRadar/build/esp32.esp32.esp32s3/'
m=open('flasher/airradar-merged.bin','rb').read()
for name, off, path in (('bootloader',0x0,B+'AirRadar.ino.bootloader.bin'),
                        ('partitions',0x8000,B+'AirRadar.ino.partitions.bin'),
                        ('app',0x10000,B+'AirRadar.ino.bin')):
    b=open(path,'rb').read()
    print(name, 'OK' if m[off:off+len(b)]==b else 'DIFFERS')
EOF
```

(`esptool` ships inside the esp32 core at
`~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool`; note that 5.x
spells the subcommand `merge-bin` and the flags `--flash-mode/--flash-freq/
--flash-size`.) Also copy the app-only image for OTA:

```bash
cp "$BUILD/AirRadar.ino.bin" flasher/airradar-ota.bin
```

Then bump `flasher/manifest.json` and the footer in `flasher/index.html` to match
`AR_VERSION`, commit both bins, and enable GitHub Pages from the repository
**root** — Pages cannot publish an arbitrary `flasher/` folder, so the installer
lives at `/flasher/` underneath the site root.

## D. Updates after the first flash — no cable needed

Open `http://airradar.local/` → **Firmware** section → choose the new
`AirRadar.ino.bin` (the app binary alone, not the merged image) → Update.
The device verifies, flashes the spare OTA slot and reboots. Set a **panel
password** first (Settings → System, or the web page) — it protects OTA and
the whole API.

## E. First-flash checklist (bring-up truths)

1. **Black screen?** Tools → PSRAM must be **OPI PSRAM**. It's the #1 cause.
2. **Psychedelic / wrong colors?** Flip `LV_COLOR_16_SWAP` in `lv_conf.h`
   (0↔1) *or* toggle `lcd.setSwapBytes(...)` in `src/hal/hal_display.cpp` —
   one of the two, not both. Recompile.
3. **Touch dead?** A different board sample may latch GT911 at `0x14`:
   change `TOUCH_I2C_ADDR` in `src/hal/hal_display.cpp`.
4. **Upload fails?** Flip the UART slide switch; retry.
5. First boot with no stored Wi-Fi opens the on-screen network picker.
6. Map/weather/routes appear within ~30 s of Wi-Fi connecting; the scope shows
   the dark floor until the first CARTO stitch lands (that's by design).
