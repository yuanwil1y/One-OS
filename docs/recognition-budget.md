# Buffer, cache and capacity budget (B5)

This document states the memory the recognition path is allowed to use, where each
number comes from, and which parts have **not** been measured on hardware.

Two kinds of number appear below and they are never mixed:

| Source | Meaning |
|---|---|
| **computed** | `sizeof()` of the real structs, or a product of compile-time constants. Reproducible on any machine; the same on the target for these types. |
| **measured** | Read from a running ESP32-C6. **None of these exist yet** — no board has been flashed. |

Reproduce the computed table with:

```
clang -std=c11 -O0 -Ifirmware/main/include -Ifirmware/components/ha_core/include \
      -Itests/host/stubs <program printing sizeof(...)> -o sizes && ./sizes
```

The values below were produced that way on 2026-09-11. `sizeof` of these structs is
identical on the target: they contain no platform-dependent padding beyond `bool`,
`uint32_t` and fixed-size `char` arrays, and both builds are LP32/ILP32 for these
members.

---

## 1. The reader: memory independent of corpus size

The production corpus lives on SD and is **never** loaded. What the reader holds is
fixed, whatever the database contains:

| Item | Bytes | Computed from |
|---|---|---|
| `app_device_db_t` (header staging, record scratch, diagnostics) | 568 | `sizeof(app_device_db_t)` |
| Index buffer, `app_runtime.c` budget | 8 192 | `APP_DB_INDEX_BUDGET` = 256 × `DEVICE_DB_INDEX_BUCKET_SIZE` (32) |
| Body-checksum block | 256 | `CHECKSUM_BLOCK` |
| Header probe for change detection | 128 | `DEVICE_DB_HEADER_SIZE` |
| **Reader total** | **≈ 9.1 KiB** | |

Scaling behaviour:

- the **profiles, fingerprints, recipes and strings regions are never resident**.
  Each is seeked to and read one record at a time into `db->record` or a stack
  block: profile 96 B, fingerprint 40 B, recipe 72 B;
- the **index** is the only variable-size part, and its size is a property of the
  corpus's bucket count, not of its record count. `APP_DB_MAX_INDEX_BYTES`
  (32 768 = 1024 buckets) is the hard ceiling; a corpus needing more is reported as
  `corrupt` with `ESP_ERR_NO_MEM` rather than silently truncated, because a partial
  index would silently stop matching some devices. The runtime allocates the
  smaller 8 192-byte budget, i.e. 256 buckets;
- the **body checksum** is streamed in 256-byte blocks, so verifying a 16 MiB corpus
  costs the same RAM as verifying a 2 KiB one. It costs *time*: 65 536 block reads.
  That is why the pass takes a cancellation hook (`app_device_db_set_progress`), so a
  cancelled scan stops validating instead of holding the worker for the whole file.

**Corpus size is therefore not a RAM constraint at all.** The limit that matters is
the index bucket count, and it is a quarter of the 16 MiB file cap at 1024 buckets.

## 2. Working set: recognition and device state

| Table | Per entry | Entries | Total | Constant |
|---|---|---|---|---|
| Evidence store (`wifi` 80 B, `ble` 112 B, `lan` 120 B + counters) | — | 32 / 32 / 24 | 9 080 | `app_scan_evidence_t` |
| Recognition results | 1 408 | 24 | 33 792 | `APP_RECOGNITION_TABLE_MAX` |
| Application device bindings | 136 | 24 | 3 264 | `APP_DEVICE_MAX` |
| Application entity bindings | 190 | 48 | 9 120 | `APP_ENTITY_MAX` |
| HA core devices | 499 | 8 | 3 992 | `HA_CORE_MAX_DEVICES` |
| HA core entities | 384 | 32 | 12 288 | `HA_CORE_MAX_ENTITIES` |
| HA core states | 393 | 32 | 12 576 | `HA_CORE_MAX_STATES` |
| Recognition index | — | — | 8 192 | `APP_DB_INDEX_BUDGET` |
| Reader | — | — | 568 | `sizeof(app_device_db_t)` |
| **Static total** | | | **≈ 92.9 KiB** | |

A recognition entry is dominated by its recipe storage: 1 184 of its 1 408 bytes are
`app_entity_recipe_t recipes[8]`, and 124 of each recipe's 148 bytes are strings
(domain 24, name 48, device class 32, unit 20). The table is sized for the worst
case where **every** observed device matches a profile with the maximum recipe
count, because a device that matched must keep its recipes until materialisation
runs.

### Real capacity is bounded below these limits

The tables are deliberately not all the same size, and the smallest bound wins:

| Bound | Value | Consequence |
|---|---|---|
| `HA_CORE_MAX_DEVICES` | **8** | at most 8 devices are materialised, whatever the app tables allow |
| `HA_CORE_MAX_ENTITIES` / `_STATES` | **32** | at most 32 entities exist; the 48-slot app entity table can be filled before the core refuses |
| `APP_DEVICE_MAX` | 24 | 24 bindings may exist, of which only 8 can be in `ha_core` at once |
| `APP_RECOGNITION_TABLE_MAX` | 24 | one entry per observation actually attempted |

The mismatch between 24 and 8 is intentional and reported rather than hidden:
`app_device_materialize()` sets `*truncated` when `ha_core` refuses an insert, the
stage marks the scan truncated, and the response carries `truncated=1`. A scan that
saw more than 8 devices therefore says so instead of presenting 8 as the whole
environment.

The mismatch is also a **decision to revisit**, not a settled design: raising
`HA_CORE_MAX_DEVICES` raises the HA core tables by 499 bytes per device and is
deferred until the real heap figures exist. Recorded here so the next round does not
rediscover it.

## 3. Corpus expectations

- The fixture corpus used by the tests is 2 672 bytes: 5 profiles, 7 fingerprints,
  7 recipes, 29 damaged variants. It is a **test** corpus and is never linked into
  the firmware.
- The production corpus is expected to be far larger than the index budget's 256
  buckets can cover at ~3 entries per bucket. **No production corpus has been built
  or measured**, so the bucket count needed for a real device list is unknown. The
  ceiling is stated, not projected: beyond 1024 buckets the reader reports
  `corrupt` and recognition stops.
- Corpus generation is a host-side step (`tools/device_db/build_device_db.py`) and
  its output is reproducible (SHA-256 checked in CI).

## 4. Radio and session buffers (unchanged by B5, listed for completeness)

| Item | Bytes | Source |
|---|---|---|
| Wi-Fi session frame copy | ≤ 256 per frame, ring depth 8 | `kismet_wifi_session_config_t` |
| BLE session report copy | ≤ 255 per report | `kismet_ble_session_config_t` |
| Application worker stack | 8 192 | `APP_RUNTIME_WORKER_STACK` |
| Console task stack | 6 144 | `APP_CONSOLE_STACK` |
| Diagnostic output buffers | ~4.6 KiB static | `app_diag_console.c` |

### 4b. The SD card and the screen share one SPI2 bus

B5 requires coordinating the card and the display, because on this board they are on
the same peripheral. The coordination is **structural rather than a lock**, and that
is deliberate:

| Fact | Where |
|---|---|
| One bus instance, with the LCD's MOSI/SCLK and the card's MISO | `board.c::board_spi2_init()` |
| The call is idempotent (`s_spi2_initialized` short-circuits) | same |
| The card gets its own chip select and its own VFS mount point | `storage.c::board_sd_mount()` |
| The database adapter calls `board_sd_mount()` and never touches a bus itself | `app_device_db_sd.c` |

Two consequences worth stating, because both have been the source of exactly this
class of bug on ESP32 boards:

- **A second `board_spi2_init()` from anywhere is a no-op, not a second bus.**
  Without that guard, the database adapter's lazy mount would fail with
  `ESP_ERR_INVALID_STATE` on a board where the screen came up first - and the
  symptom would be "the card is never detected", not "the bus was initialised
  twice", which is why the guard is documented here.
- **Nothing unmounts a card somebody else mounted.** `app_device_db_sd.c` opens and
  closes *files*, not the bus, so a scan and a database read can overlap without
  either one tearing down the other's transport.

*Not verified on hardware*: whether panel and card traffic actually interleave
correctly at the chosen clock, and whether a card insertion or removal mid-scan
disturbs the panel. Those need the board; they are items 2.1 and 3.x of
`docs/hardware-acceptance.md`. No throughput figure is claimed here.

## 5. GUI reservation

The task book requires the LVGL budget to be preserved, and no screen or LVGL
baseline was removed to make room. Current reservation, unchanged by B5:

| Item | Value | Source |
|---|---|---|
| LVGL heap | 32 KiB | `CONFIG_LV_MEM_SIZE_KILOBYTES=32` |
| Display colour depth | 16 bpp | `CONFIG_LV_COLOR_DEPTH_16` |
| Display buffers | owned by `lvgl_port`; **not measured** | `firmware/components/lvgl_port` |

**The static tables above do not include the LVGL heap or the display buffers.** The
recognised tables are `static`, so they are placed at link time and do not compete
with the LVGL heap or with runtime allocations. What *is* shared is total DRAM, and
that is exactly what has not been measured.

## 6. What is not measured

Everything in this section is a gap, not an estimate:

- **free heap, minimum free heap and largest free block** at any point of a scan.
  The fields exist in the resource report and have never held a value from hardware.
  A build log's `one_os.bin` size is **not** a memory measurement and is not used
  here.
- **Task stack high-water marks** for the worker and console tasks.
- **Actual static DRAM use**, i.e. `.data` + `.bss` from the linked image. This is
  obtainable from a build (`idf.py size`) but has not been captured, and it is the
  number that would validate or refute the ≈ 92.9 KiB computed above.
- **SD read throughput** and therefore the real cost of the streamed body checksum
  and of per-match record reads.
- **Behaviour of the shared SPI2 bus** when the display and the database reader are
  both active. The product rules require database reads to stay short and off the
  LVGL owner path; the design keeps them on the application worker, but coexistence
  has not been observed.
- **Any production corpus size or bucket count.**

Until those exist, no statement in this repository may claim a RAM figure for the
recognition path. The B11 acceptance work is where they are to be collected; see
[development-status.md](development-status.md).
