# One-OS

One-OS is the clean firmware foundation for the NearBy One NEXT handheld platform.

Target board: **Waveshare ESP32-C6-Touch-LCD-1.9**  
Framework: **ESP-IDF + FreeRTOS + LVGL**  
MCU: **ESP32-C6**  
Display: **170×320 ST7789V2**  
Touch: **CST816**

This repository starts again from the hardware and native platform layer. It intentionally does **not** import the previous scanner architecture, compatibility layers, radio orchestration, discovery pipeline, or project-specific Level-2 APIs.

## Foundation rules

1. ESP-IDF, FreeRTOS, LVGL, NimBLE, lwIP and other upstream platform APIs remain directly usable.
2. Do not add wrappers whose only purpose is renaming native APIs.
3. The BSP owns only board-specific facts: pins, shared buses, LCD/touch/SD bring-up, reset/power details and native handles.
4. The LVGL port is only a board/display/input binding. Application UI remains ordinary `lv_*` code.
5. FreeRTOS remains FreeRTOS. Tasks, queues, mutexes and timers use native FreeRTOS APIs.
6. Higher-level project APIs may be added later only when a real reusable capability exists.
7. Higher-level project API families are peers. They must not call, wrap, depend on, or expose public types from another project API family. Composition happens in applications.
8. Implement project-specific APIs according to the implementation model of their own upstream project, not by routing one project through another.
9. Keep the firmware buildable after every migration step.

## Current beta: hardware smoke test

`v0.1.0-beta.1` is intentionally a board/radio/storage validation image rather than a product firmware.

The 170×320 touch GUI exposes five tests:

| Test | What it validates |
|---|---|
| Wi-Fi | Native ESP-IDF active AP scan; reports AP count |
| BLE | Native NimBLE passive discovery for about 3 seconds; reports advertisement count |
| IEEE 802.15.4 | Native promiscuous RX sweep over channels 11–26; reports completed channels and observed frame count |
| SD R/W | Mount FATFS, write a known payload, flush/sync, reopen, read and byte-compare |
| SD Format | Explicit confirmation, FAT format (or create FAT if the card cannot mount), then automatic write/read verification |

Wireless tests are serialized on one FreeRTOS worker task. The worker never calls LVGL. Test results return through a FreeRTOS queue and the `app_main()` LVGL owner task performs all UI updates.

A zero IEEE 802.15.4 frame count is **not** treated as a failure: the hardware path passes when enable/channel/receive/sleep/disable succeeds across all 16 channels. This avoids requiring a Zigbee/Thread transmitter in the room merely to validate the radio driver.

### SD format warning

The **SD Format** action is destructive to the card's FAT filesystem. The GUI requires a second confirmation before formatting. Use **SD R/W** when you only want a non-destructive storage check.

## Source layout

```text
firmware/
├─ CMakeLists.txt
├─ sdkconfig.defaults
├─ partitions_8mb.csv
├─ components/
│  ├─ board/
│  │  ├─ include/board.h
│  │  ├─ board.c
│  │  ├─ touch.c
│  │  └─ storage.c
│  └─ lvgl_port/
│     ├─ include/lvgl_port.h
│     └─ lvgl_port.c
└─ main/
   ├─ main.c
   ├─ smoke_gui.c/.h
   └─ smoke_tests.c/.h
```

The smoke-test files are application-local validation code. They are **not** a new wireless abstraction layer or reusable Level-2 API.

## Hardware baseline

| Function | GPIO / detail |
|---|---|
| LCD resolution | 170 × 320 portrait |
| LCD controller | ST7789V2 |
| LCD MOSI / SD MOSI | GPIO4 |
| LCD SCLK / SD CLK | GPIO5 |
| LCD DC | GPIO6 |
| LCD CS | GPIO7 |
| LCD RST | GPIO14 |
| LCD backlight | GPIO15, active-low high-side switch |
| SD MISO | GPIO19 |
| SD CS | GPIO20 |
| I2C SCL | GPIO8 |
| I2C SDA | GPIO18 |
| Touch | CST816, address `0x15` |
| Shared SPI host | `SPI2_HOST` |
| Flash | 8 MB |
| PSRAM | none assumed |

LCD and SD share SPI2. The BSP initializes that bus once and individual devices attach to it; device drivers must not independently create/free the shared bus.

## Native ownership model

```text
Application
├─ ESP-IDF
├─ FreeRTOS
├─ LVGL
├─ NimBLE / lwIP / IEEE 802.15.4 when needed
├─ board BSP for board-specific hardware
└─ future independent project APIs
```

## Build

```bash
cd firmware
idf.py set-target esp32c6
idf.py build
```

Create one merged flash image:

```bash
idf.py merge-bin -o One-OS-v0.1.0-beta.1-merged.bin -f raw
```

The merged raw image contains the bootloader, partition table and application according to the project configuration and is intended to be flashed at offset `0x0`.

Normal ESP-IDF flash/monitor:

```bash
idf.py -p <PORT> flash monitor
```

LVGL is currently pinned to the proven v8.3.11 baseline while the board port is migrated. Upgrade it deliberately later rather than mixing a framework upgrade into the hardware-baseline migration.

## CI and beta release

GitHub Actions builds with **ESP-IDF v6.1** for `esp32c6`, creates the merged raw image, calculates its SHA-256 checksum and uploads both as workflow artifacts.

The beta release contains:

```text
One-OS-v0.1.0-beta.1-merged.bin
One-OS-v0.1.0-beta.1-merged.bin.sha256
```

The release image is suitable for a full-device smoke-test flash at offset `0x0`.

## Migration policy

The clean-restart whitelist currently includes:

- board pin definitions and shared-bus ownership;
- ST7789V2 initialization;
- CST816 touch input;
- SD/FATFS attachment on the shared SPI bus;
- thin LVGL display/input binding;
- minimal ESP-IDF/FreeRTOS startup;
- application-local hardware smoke tests using native wireless/storage APIs.

Still not migrated:

- scanner/discovery product applications;
- `radio_runtime`, `scan_session`, `scan_coordinator`;
- Kismet/Wireshark/Home Assistant APIs;
- old `nearby_*` compatibility APIs;
- recognition databases and product semantics;
- previous application UI screens;
- web management and scan pipelines.

Those capabilities can be reconsidered later, one independent capability at a time.
