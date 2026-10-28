# ESP32-S3 Teacher Recorder

Firmware for an ESP32-S3-based classroom audio recorder. A teacher taps an
enrolled NFC card to start a recording; audio from a USB microphone is
processed and saved to an SD card. The device also provides a small TFT user
interface, BLE administration, Wi-Fi time synchronization, and background
upload of saved recordings.

## Features

- USB Audio Class microphone capture with filtering and conversion to 16 kHz
  audio.
- WAV recordings written to an SD card, with recovery of unfinished recording
  files after a restart.
- PN532 NFC reader for teacher-card identification.
- ST7735 TFT interface for device status and recording progress.
- BLE administration for Wi-Fi, class/section, clock, and teacher-card setup.
- Wi-Fi/NTP synchronization and automatic retry of pending recording uploads.
- Diagnostic firmware stages for checking the display, microphone, USB host,
  SD card, and board pin wiring.

## Hardware

The firmware is configured for an **ESP32-S3 N16R8** board: 16 MB QIO flash
and 8 MB octal PSRAM. The USB host, display, SD card, and PN532 reader use the
following default pin assignments.

| Peripheral | ESP32-S3 pins | Notes |
|---|---|---|
| USB microphone | GPIO 19 (D-), GPIO 20 (D+) | Supply the microphone's VBUS from a suitable 5 V source; share ground. |
| TFT + SD SPI bus | MOSI GPIO 11, SCLK GPIO 12, MISO GPIO 13 | Display and SD card share the SPI bus. |
| ST7735 display | CS GPIO 10, reset GPIO 9, DC GPIO 8, backlight GPIO 17 | 128x160 panel, configured for landscape orientation. |
| SD card | CS GPIO 15 | Use a compatible 3.3 V SPI SD-card module. |
| PN532 NFC reader | SDA GPIO 4, SCL GPIO 6, IRQ GPIO 5 | I2C connection. |
| On-board BOOT button | GPIO 0 | Used by the development button configuration. |

Check the board schematic and module voltage requirements before wiring.
GPIO assignments and display settings are defined in
[`main/board_pins.h`](main/board_pins.h) and
[`main/app_config.h`](main/app_config.h).

## Build and flash

### Requirements

- PlatformIO Core or the PlatformIO extension for VS Code.
- An ESP32-S3 toolchain and ESP-IDF **5.5.3**, matching the version pinned in
  [`dependencies.lock`](dependencies.lock).
- A USB data connection to the ESP32-S3 for flashing and serial monitoring.

The configured PlatformIO environment is `s3_n16r8`:

```sh
pio run -e s3_n16r8
pio run -e s3_n16r8 -t upload
pio device monitor -b 115200
```

Alternatively, from an ESP-IDF command prompt:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the serial port for the board. The project uses
[`partitions.csv`](partitions.csv), [`sdkconfig.defaults`](sdkconfig.defaults),
and the board-specific [`sdkconfig.s3_n16r8`](sdkconfig.s3_n16r8).

## First-time setup

1. Insert an SD card and connect the TFT, PN532 reader, and USB microphone.
2. Flash the firmware and open the serial monitor. Check the display and logs
   for peripheral initialization status.
3. Hold the device button for about two seconds to enter BLE administration.
   The device advertises with a name beginning `TCHR-REC-`.
4. On a computer with Python 3.9 or later, install the BLE client dependency
   and launch the setup utility:

   ```sh
   python -m pip install bleak
   python tools/admin.py
   ```

5. Use the utility to set Wi-Fi credentials and class/section details, then
   enroll teachers by tapping their NFC cards. The utility also supports CSV
   imports; see [`tools/teachers_example.csv`](tools/teachers_example.csv).

Wi-Fi credentials and teacher settings are stored on the device in NVS, not in
the source tree. Recordings remain on the SD card if network upload is
unavailable; pending uploads are retried when connectivity is restored.

## Firmware stages

The default build selects the complete recorder application
(`APP_STAGE_FULL`). For hardware bring-up, change `APP_STAGE` in
[`main/app_config.h`](main/app_config.h) to one of the diagnostic stages:

| Stage | Purpose |
|---|---|
| `APP_STAGE_FULL` | NFC-triggered recording, display, storage, administration, and upload flow. |
| `APP_STAGE_MIC` | USB microphone recording test. |
| `APP_STAGE_DISPLAY` | Display demonstration. |
| `APP_STAGE_PINTEST` | Slow pin-state test for wiring checks. |
| `APP_STAGE_SDTEST` | SD card initialization diagnostic. |
| `APP_STAGE_USBTEST` | USB host device and audio-class diagnostic. |

Rebuild and flash after changing the selected stage.

## Repository layout

```text
main/                 Firmware sources and ESP-IDF component manifest
managed_components/   Locked ESP Component Manager dependencies
pc/                   Host-side test support and platform stubs
tools/                BLE administration and font-generation utilities
platformio.ini        PlatformIO build environment
dependencies.lock     Locked ESP-IDF component versions
partitions.csv        Custom flash partition table
sdkconfig.defaults    Default ESP-IDF configuration
sdkconfig.s3_n16r8    Board-specific ESP-IDF configuration
```

## License

Released under the MIT License; see [`LICENSE`](LICENSE).
