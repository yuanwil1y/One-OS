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

## Initial source layout

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

The initial `app_main()` is intentionally small: initialize the board display, bind LVGL to LCD/touch, then run `lv_timer_handler()` from one FreeRTOS owner task. Future applications can replace that smoke-test runtime without changing the BSP or LVGL binding.

## Build

```bash
cd firmware
idf.py set-target esp32c6
idf.py build
```

Flash and monitor:

```bash
idf.py -p <PORT> flash monitor
```

LVGL is currently pinned to the proven v8.3.11 baseline while the board port is migrated. Upgrade it deliberately later rather than mixing a framework upgrade into the hardware-baseline migration.

## Migration policy

The first migration wave is a whitelist only:

- board pin definitions and shared-bus ownership;
- ST7789V2 initialization;
- CST816 touch input;
- SD/FATFS attachment on the shared SPI bus;
- thin LVGL display/input binding;
- minimal ESP-IDF/FreeRTOS startup and build configuration.

Not migrated in this wave:

- scanner/discovery applications;
- `radio_runtime`, `scan_session`, `scan_coordinator`;
- Kismet/Wireshark/Home Assistant APIs;
- old `nearby_*` compatibility APIs;
- recognition databases and product semantics;
- application UI screens;
- web management and scan pipelines.

Those capabilities can be reconsidered later, one independent capability at a time.
