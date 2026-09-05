# PhotoPainter TRMNL Firmware

Custom TRMNL BYOD client firmware for the Waveshare ESP32-S3 PhotoPainter.

This project ports the TRMNL client flow from `trmnl-m5paper` to the Waveshare
7.3-inch Spectra 6 e-paper photo frame.

Browser flasher: https://paveldn.github.io/trmnl-PhotoPainter/

## Hardware

- Waveshare ESP32-S3 PhotoPainter, SKU 32408
- ESP32-S3-WROOM-1-N16R8, 16 MB flash, 8 MB PSRAM
- 7.3-inch 800x480 six-color e-paper panel
- AXP2101 PMIC
- BOOT button on GPIO0

Display pins:

| Function | GPIO |
| --- | --- |
| MOSI | 11 |
| SCK | 10 |
| CS | 9 |
| DC | 8 |
| RST | 12 |
| BUSY | 13 |

## Features

- WiFi captive portal setup, no hardcoded credentials
- Official TRMNL and custom/local TRMNL-compatible servers
- MAC-based setup registration
- TRMNL `/api/display` polling with BYOD headers
- PNG, JPEG, and BMP download with six-color palette rendering
- Image caching through filename plus HTTP validators
- Deep sleep with timer and BOOT-button wake
- AXP2101 battery and USB telemetry
- Server-driven OTA plus GitHub release fallback

## Current Limitations

- Images are centered without scaling. Configure the TRMNL server for 800x480 output.
- The display and power paths compile against Waveshare's reference pinout and
  init sequence, but they still need testing on the actual PhotoPainter hardware.
- PNG transparency is rendered against a white background.

## Build

Requires PlatformIO.

```sh
pio run
pio run -t upload
```

The build target is `esp32-s3-devkitc1-n16r8`, matching the PhotoPainter's
16 MB flash and 8 MB PSRAM configuration.

## Setup

1. Flash the firmware.
2. On first boot, connect to the open `PhotoPainter-TRMNL` WiFi network.
3. Open `http://192.168.4.1`.
4. Configure WiFi and, optionally, a custom TRMNL server/API key.
5. The device restarts, registers if needed, fetches the current display image,
   refreshes the panel, and sleeps.

Hold BOOT for more than 5 seconds after wake to clear WiFi credentials. Hold for
about 16 seconds to factory reset.
