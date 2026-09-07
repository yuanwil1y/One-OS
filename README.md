# One-OS

One-OS is the clean firmware foundation for the NearBy One NEXT handheld platform.

Target board: **Waveshare ESP32-C6-Touch-LCD-1.9**  
Framework: **ESP-IDF + FreeRTOS + LVGL**  
MCU: **ESP32-C6**  
Display: **170×320 ST7789V2**  
Touch: **CST816**

This repository starts from the hardware and native platform layer. It intentionally does **not** import the previous scanner architecture, compatibility layers, radio orchestration, discovery pipeline, or project-specific Level-2 APIs.

## Foundation rules

1. ESP-IDF, FreeRTOS, LVGL, NimBLE, lwIP and other upstream platform APIs remain directly usable.
2. Do not add wrappers whose only purpose is renaming native APIs.
3. The BSP owns only board-specific facts: pins, shared buses, LCD/touch/SD bring-up, reset/power details and native handles.
4. The LVGL port is only a board/display/input binding. Application UI remains ordinary `lv_*` code.
5. FreeRTOS remains FreeRTOS. Tasks, queues, mutexes and timers use native FreeRTOS APIs.
6. Higher-level project APIs may be added only when a real reusable capability exists.
7. Higher-level project API families are peers. They must not call, wrap, depend on, or expose public types from another project API family. Composition happens in applications.
8. Implement project-specific APIs according to the implementation model of their own upstream project, not by routing one project through another.
9. Keep the firmware buildable after every migration step.

## Nearby Devices application workflow

The product-level integration target is one generic **Nearby Devices Browser / Controller** built around Home Assistant-style **Device → Entity → State/control** semantics.

The canonical application-layer workflow is documented in:

- [`docs/application/nearby-devices-browser-controller.md`](docs/application/nearby-devices-browser-controller.md)

That document defines the complete end-to-end path from multi-protocol environment scanning, parsing and integrated Device DB matching through HA Device/Entity materialization, LVGL presentation, Entity control dispatch and confirmed state updates. It also defines the unique owner for every capability so project Level-2 API families do not duplicate one another.

Agents implementing application integration must read that document before introducing scan orchestration, recognition, Device/Entity mapping or control routing.

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
   └─ main.c
```

The released `v0.1.0-beta.1` and `v0.1.0-beta.2` hardware smoke images remain available from GitHub Releases as archived board/radio/storage validation builds. The smoke-test application is not part of the current production source baseline.

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

The current `app_main()` is intentionally small: initialize the board display, bind LVGL to LCD/touch, then run `lv_timer_handler()` from one FreeRTOS owner task.

## Build

```bash
cd firmware
idf.py set-target esp32c6
idf.py build
```

Create one merged flash image when needed:

```bash
idf.py merge-bin -o One-OS-merged.bin -f raw
```

Normal ESP-IDF flash/monitor:

```bash
idf.py -p <PORT> flash monitor
```

LVGL remains pinned to the proven v8.3.11 baseline while the platform architecture is established.

## CI and releases

`build.yml` runs on pushes and pull requests with read-only repository permissions and verifies the ESP32-C6 firmware build.

`release.yml` runs only for `v*` tags. It builds the tagged source, creates a merged raw image containing bootloader + partition table + application, publishes a SHA-256 checksum, and creates a GitHub Release using the tag name.

## Current foundation scope

The clean baseline includes:

- board pin definitions and shared-bus ownership;
- ST7789V2 initialization;
- CST816 touch input;
- SD/FATFS attachment on the shared SPI bus;
- thin LVGL display/input binding;
- minimal ESP-IDF/FreeRTOS startup;
- native Wi-Fi, NimBLE and IEEE 802.15.4 capabilities enabled in configuration for future applications/APIs.

Not part of the foundation:

- scanner/discovery product applications;
- radio lifecycle abstraction layers;
- scan/session/coordinator frameworks;
- Kismet/Wireshark/Home Assistant APIs;
- old `nearby_*` compatibility APIs;
- recognition databases and product semantics;
- previous application UI screens;
- web management and scan pipelines.

Future Level-2 APIs should be introduced one independent capability family at a time, with no cross-family dependency or type coupling.
