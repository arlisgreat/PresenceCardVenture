# ESP32-8048S043C display client

Minimal PlatformIO/Arduino display endpoint for the 4.3-inch Sunton
ESP32-8048S043C. It is separate from the existing CoreS3 firmware.

## What it does

- Initializes the 800×480 direct-RGB panel through LovyanGFX. Touch is not
  initialized in this first build.
- Briefly shows red, green and blue blocks and prints `COLOR_CHECK` so the
  physical PCB's RGB bit order can be checked before network testing.
- Derives `dvc_<12 hex>` from `ESP_MAC_WIFI_STA` in network byte order, matching
  the existing CoreS3 firmware convention.
- Requests and displays a six-digit pair code, then stores `device_token` in
  NVS after `/pair/status` returns `bound`.
- Every 10 seconds requests `/device/state` and `/feed?limit=1`. Feed polling
  does not depend on `unseen_count`.
- Resolves the feed `image_url`, adds `?size=320`, and downloads at most
  160 KiB. It requires JPEG SOI/EOI markers and a 320×240 SOF before decode.
- Renders the JPEG full-screen with an aspect-preserving center crop. Runtime
  polling leaves the full-screen photo intact. `latest_photo` is saved in NVS
  only after the frame is drawn.
- A missing configuration is a supported build: the device displays
  `CONFIG REQUIRED` instead of attempting a connection.

The implementation keeps the existing `/v1` device protocol unchanged. The
CoreS3 capture board and this 8048 display board each bind to the same user with
their own device IDs; no new endpoint is needed.

## Pinned build

- PlatformIO `espressif32@6.12.0` (Arduino core 2.x line)
- `LovyanGFX@1.2.28`
- `ArduinoJson@7.4.2`

The custom board file selects ESP32-S3, 16 MB QIO flash and 8 MB OPI PSRAM.
The first-build display timing is deliberately conservative: 12.5 MHz PCLK,
H/V pulse 4 and front/back porch 8/8. Pins and timing follow the exact
[`esp32-8048S043C.json`](https://github.com/rzeldent/platformio-espressif32-sunton/blob/main/esp32-8048S043C.json).
The data-pin ordering is also consistent with LovyanGFX's upstream
[`LGFX_ESP32S3_RGB_ESP32-8048S043.h`](https://github.com/lovyan03/LovyanGFX/blob/master/src/lgfx_user/LGFX_ESP32S3_RGB_ESP32-8048S043.h),
but PCLK/porches intentionally use the more conservative board JSON values.
Both remain physical-board assumptions until the startup color test is seen.

## Configure

`include/device_config.h` is gitignored and written with mode `0600`.
Interactive use asks for the Wi-Fi password twice without putting it in shell
history. Config metadata is tracked so each change rebuilds the firmware; the
secret itself remains ignored. The ESP32-S3 must use a 2.4 GHz network; for an iPhone hotspot, enable
**Settings → Personal Hotspot → Maximize Compatibility**, then reconnect the
laptop to the hotspot:

```sh
python3 tools/configure.py \
  --ssid 'your-wifi' \
  --api-base 'https://api.example.com/v1' \
  --ca-file /path/to/api-root-ca.pem
```

For non-interactive use, place the password in an environment variable and
name that variable with `--password-env`. An open network must be explicitly
selected with `--open-network`. Existing configuration is not overwritten
without an interactive confirmation or `--force`.

The current laptop-side development API can be reached over the LAN with an
explicit local-only HTTP switch (example address from this test session):

```sh
python3 tools/configure.py \
  --ssid 'your-wifi' \
  --api-base 'http://192.168.1.10:3000/v1' \
  --allow-local-http
```

Plain HTTP is rejected unless the compile-time switch is enabled **and** the
host is loopback, `.local`, or a private IPv4 address. HTTPS calls use the
configured root CA and hostname verification; the firmware never calls
`setInsecure()`. Before the first HTTPS request it obtains UTC through SNTP so
certificate validity dates can be checked. Image downloads are additionally
restricted to the API's same origin so the bearer token is not sent to an
arbitrary feed URL.

## Build

Configuration is not required for compilation:

```sh
/private/tmp/pvc-platformio-venv/bin/pio run
```

Run this command from `firmware/display/`.

## Safe first flash

Do not power the board from P1 while USB-C is connected. For first bring-up,
use USB-C as the only power source and confirm that the CH340 port is the
intended board. The currently observed port is `/dev/cu.usbserial-10`; always
pass the port explicitly rather than relying on auto-selection.

Before the first write, make a read-only, segmented backup with the
PlatformIO-installed `esptool.py`. This CH340 board was observed dropping data
with the default stub, including at 115200/230400, while ROM mode with
`--no-stub` at 115200 was stable. The observed factory partition table occupies
`0x000000`–`0x400000`; four smaller reads avoid losing all progress if USB
disconnects:

```sh
PVC_BACKUP_DIR='/Users/airulan/Documents/New project/PresenceCardVenture-hardware-backups'
PVC_PIO_PYTHON='/private/tmp/pvc-platformio-venv/bin/python'
PVC_ESPTOOL_PY='/Users/airulan/.platformio/packages/tool-esptoolpy/esptool.py'
mkdir -p "$PVC_BACKUP_DIR"
"$PVC_PIO_PYTHON" "$PVC_ESPTOOL_PY" \
  --chip esp32s3 --port /dev/cu.usbserial-10 --baud 115200 --no-stub \
  read_flash 0x000000 0x010000 \
  "$PVC_BACKUP_DIR/00-bootloader-0x000000-0x010000.bin"
"$PVC_PIO_PYTHON" "$PVC_ESPTOOL_PY" \
  --chip esp32s3 --port /dev/cu.usbserial-10 --baud 115200 --no-stub \
  read_flash 0x010000 0x140000 \
  "$PVC_BACKUP_DIR/01-app0-0x010000-0x150000.bin"
"$PVC_PIO_PYTHON" "$PVC_ESPTOOL_PY" \
  --chip esp32s3 --port /dev/cu.usbserial-10 --baud 115200 --no-stub \
  read_flash 0x150000 0x140000 \
  "$PVC_BACKUP_DIR/02-app1-0x150000-0x290000.bin"
"$PVC_PIO_PYTHON" "$PVC_ESPTOOL_PY" \
  --chip esp32s3 --port /dev/cu.usbserial-10 --baud 115200 --no-stub \
  read_flash 0x290000 0x170000 \
  "$PVC_BACKUP_DIR/03-spiffs-0x290000-0x400000.bin"
stat -f '%z bytes %N' \
  "$PVC_BACKUP_DIR/00-bootloader-0x000000-0x010000.bin" \
  "$PVC_BACKUP_DIR/01-app0-0x010000-0x150000.bin" \
  "$PVC_BACKUP_DIR/02-app1-0x150000-0x290000.bin" \
  "$PVC_BACKUP_DIR/03-spiffs-0x290000-0x400000.bin"
shasum -a 256 \
  "$PVC_BACKUP_DIR/00-bootloader-0x000000-0x010000.bin" \
  "$PVC_BACKUP_DIR/01-app0-0x010000-0x150000.bin" \
  "$PVC_BACKUP_DIR/02-app1-0x150000-0x290000.bin" \
  "$PVC_BACKUP_DIR/03-spiffs-0x290000-0x400000.bin"
```

Only after the four sizes are exactly 65,536; 1,310,720; 1,310,720; and
1,507,328 bytes, and all checksums are recorded, upload explicitly:

```sh
/private/tmp/pvc-platformio-venv/bin/pio run \
  --target upload --upload-port /dev/cu.usbserial-10
/private/tmp/pvc-platformio-venv/bin/pio device monitor \
  --port /dev/cu.usbserial-10 --baud 115200
```

The project also pins upload to 115200 because this physical CH340 link was
not reliable at 230400 or 460800 during first bring-up.

This project performs no automatic erase, backup, upload, or serial-port
operation. If auto-download fails, hold BOOT, tap RESET, then release BOOT;
do not erase the whole flash as a troubleshooting shortcut.

## Physical acceptance

1. The startup screen must show left red, center green, right blue. Confirm the
   serial line `COLOR_CHECK left=RED center=GREEN right=BLUE`.
2. With no config, `CONFIG REQUIRED` remains visible.
3. With config, verify Wi-Fi, six-digit pairing, then `PAIRED`.
4. Bind the CoreS3 capture board and this 8048 display board to the same test
   user. Upload one 320×240 JPEG from CoreS3 and verify that the display board
   shows it within one 10-second poll.
5. Disconnect the API: the last drawn frame and NVS binding must remain; no
   token value should appear in serial output.

Verified on the backed-up physical board: RGB order, backlight, 8 MB PSRAM,
2.4 GHz Wi-Fi, local API pairing, 320×240 JPEG download, full-screen rendering,
and repeated 10-second polling. Production TLS and long-duration stability
remain deployment acceptance items.
