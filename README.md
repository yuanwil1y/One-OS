# One-OS

One-OS is an ESP32-C6 firmware foundation with independent protocol components for the NearBy One NEXT handheld platform. The Nearby Devices product application is not yet integrated.

Target board: **Waveshare ESP32-C6-Touch-LCD-1.9**  
Framework: **ESP-IDF + FreeRTOS + LVGL**  
MCU: **ESP32-C6**  
Display: **170×320 ST7789V2**  
Touch: **CST816**

The current main branch contains the board/LVGL foundation and eight non-Matter Level-2 capability families (ten components). It does not yet connect these components into a scanner/controller application. See [the development status and cleanup audit](docs/development-status.md) for implemented capabilities, gaps, validation evidence and branch disposition.

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

## Current development order

Build and validate the headless runtime first; add GUI last. The actionable task list, dependencies and acceptance criteria are in [Pre-GUI development tasks (Chinese)](docs/pre-ui-development.md). Serial diagnostics must exercise the same application commands and state snapshots that the later GUI will use.

## Nearby Devices application workflow

The product-level integration target is one generic **Nearby Devices Browser / Controller** built around Home Assistant-style **Device → Entity → State/control** semantics.

The canonical application-layer workflow is documented in:

- [`docs/application/nearby-devices-browser-controller.md`](docs/application/nearby-devices-browser-controller.md)
- [`docs/application/nearby-devices-product-rules.md`](docs/application/nearby-devices-product-rules.md)
- [`docs/application/provisioning-web-management.md`](docs/application/provisioning-web-management.md)

All three documents are required reading for Agents implementing the Nearby Devices application. The first defines the complete end-to-end path from multi-protocol environment scanning, parsing and integrated Device DB matching through HA Device/Entity materialization, LVGL presentation, Entity control dispatch and confirmed state updates. The second freezes the production storage and UX rules: the full Device DB lives on SD, unknown/unmatched devices remain visible with generic information, and the UI follows the Home Assistant-like blue/white card language established by the previous NearBy One NEXT UI. The third defines persistent Wi-Fi provisioning and Device DB import through the temporary SoftAP/Web Management portal, including the old NearBy One NEXT code that should be directly reused or adapted.

If the main workflow guide contains an older recommendation that conflicts with `nearby-devices-product-rules.md` in storage/unknown-device/UI areas, the product-rules document wins. For provisioning, SoftAP/Web Management and database-upload behavior, `provisioning-web-management.md` is authoritative.

Agents implementing application integration must read all three documents before introducing scan orchestration, recognition, Device/Entity mapping, database storage, provisioning, Web Management, UI behavior or control routing.

## Source layout

```text
firmware/
  main/                   Minimal display/touch startup; no product UI yet
  components/
    board/                Board pins, shared SPI, LCD, touch, SD
    lvgl_port/            LVGL display/input binding
    ha_core/              Device / Entity / State model
    ha_discovery_l2/       mDNS / SSDP
    esphome_l2/            Native API subset / BLE GATT
    theengs_l2/            Selected passive BLE decoders
    zha_l2/               Selected quirks and capability transforms
    zigpy_l2/             Backend-driven Zigbee transactions
    openthread_l2/         Native OpenThread network operations
    kismet_l2/            Wi-Fi / BLE scan sessions and trackers
    nmap_l2/              Bounded LAN discovery
    wireshark_l2/         Wi-Fi / BLE bounded parsers
docs/application/         Product requirements, not implementation evidence
docs/research/            Capability scope and provenance
tests/, tools/            Host regression tests
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
└─ independent project APIs (present; application composition pending)
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

`build.yml` runs on pushes to main and pull requests with read-only repository permissions. It runs eight host-test groups and an ESP-IDF v6.1 ESP32-C6 firmware build.

`release.yml` runs only for `v*` tags. It builds the tagged source, creates a merged raw image containing bootloader + partition table + application, publishes a SHA-256 checksum, and creates a GitHub Release using the tag name.

## Current implementation scope

The board/LVGL foundation and non-Matter components listed above are present in main.
The released beta images preserve earlier hardware smoke applications; current
`app_main()` only initializes LCD/touch and services LVGL. SD mounting, radio
initialization, discovery scheduling and product UI are not wired into startup.

Still pending: application composition, persistent Wi-Fi provisioning, SoftAP/Web
Management, SD Device DB reader/importer/matcher, Device/Entity UI and confirmed
control/state updates. ESPHome authenticated Native API control and a native
Zigbee backend also remain gaps. Matter is isolated on its research branch and
has not passed its latest firmware build.

Preserve peer-family independence; compose capabilities in the application.
See [development status](docs/development-status.md) before continuing development.
