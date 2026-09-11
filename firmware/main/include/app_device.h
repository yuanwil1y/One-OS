#pragma once

/*
 * Application Device/Entity binding table.
 *
 * ha_core owns the Home Assistant semantic model (Device / Entity / State) but
 * deliberately knows nothing about scanning. This module is the application side
 * that:
 *
 *   - gives every observation a stable application Device identity;
 *   - records which protocols observed a Device and in which scan generation;
 *   - holds recognition state (unknown / ambiguous / db-unavailable / matched)
 *     separately from availability;
 *   - materialises generic read-only diagnostic Entities that have an exact
 *     protocol meaning.
 *
 * Deliberate non-goals:
 *   - No invented sensors or control capabilities. A device that only
 *     advertises "manufacturer id 76" gets an identity and generic metadata, not
 *     a fabricated temperature reading.
 *   - No cross-protocol merging on unsafe keys. BLE address, Wi-Fi BSSID and IP
 *     address are *namespaced per protocol*; the same bytes in two protocols are
 *     never the same physical device.
 *   - No Device DB here. Recognition runs on SD in a later task; until then every
 *     device is APP_RECOGNITION_DB_UNAVAILABLE or UNKNOWN and stays read-only.
 *
 * Platform independent: no ESP-IDF/FreeRTOS/LVGL dependency, so the host tests
 * exercise the same identity, dedup, generation and availability rules as the
 * firmware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_scan.h"
#include "ha_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded application binding capacity. BLE/Wi-Fi/LAN evidence can be larger
 * than this: overflow is counted and reported, never silently dropped. */
#define APP_DEVICE_MAX 24u
#define APP_ENTITY_MAX 48u

typedef enum {
    APP_RECOGNITION_UNKNOWN = 0,
    APP_RECOGNITION_AMBIGUOUS,
    APP_RECOGNITION_DB_UNAVAILABLE,
    APP_RECOGNITION_MATCHED,
} app_recognition_state_t;

/* Source protocol bitset. */
#define APP_SOURCE_BLE  (1u << 0)
#define APP_SOURCE_WIFI (1u << 1)
#define APP_SOURCE_LAN  (1u << 2)

typedef enum {
    APP_AVAILABILITY_UNKNOWN = 0,
    APP_AVAILABILITY_ONLINE,
    APP_AVAILABILITY_STALE,
    APP_AVAILABILITY_UNAVAILABLE,
} app_availability_t;

typedef struct {
    char device_id[HA_CORE_ID_LEN];
    char ha_device_id[HA_CORE_ID_LEN];
    uint32_t sources;
    app_recognition_state_t recognition;
    app_availability_t availability;
    bool ephemeral;              /* observation-derived, may be swept */
    bool read_only;              /* no writable Entity may be attached */
    uint32_t first_generation;
    uint32_t last_generation;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    int8_t signal_dbm;
    bool has_signal;
    char protocol_label[16];     /* stable short label for the UI/console */
} app_device_binding_t;

typedef struct {
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char device_id[HA_CORE_ID_LEN];
    char domain[HA_CORE_DOMAIN_LEN];
    char name[HA_CORE_NAME_LEN];
    char unit[HA_CORE_UNIT_LEN];
    bool writable;
} app_entity_binding_t;

const char *app_recognition_name(app_recognition_state_t state);
const char *app_availability_name(app_availability_t availability);

void app_device_table_reset(void);

/* Number of application Device bindings currently registered. */
size_t app_device_count(void);
const app_device_binding_t *app_device_at(size_t index);
const app_device_binding_t *app_device_find(const char *device_id);

size_t app_entity_count(void);
const app_entity_binding_t *app_entity_at(size_t index);
const app_entity_binding_t *app_entity_find(const char *entity_id);
/* Number of entities bound to one application device. */
size_t app_entity_count_for_device(const char *device_id);

/*
 * Generation lifecycle.
 *
 * begin() advances the application generation and marks every ephemeral binding
 * as not-yet-seen. finish() sweeps ephemeral bindings that were not seen again
 * and marks everything else stale rather than deleting it.
 */
void app_device_generation_begin(uint32_t generation);
void app_device_generation_finish(void);

/* Materialise every observation in `ev` into HA Device/Entity/State.
 *
 * Returns the number of devices created or refreshed. Sets `*truncated` when a
 * bounded table was full, so the caller can report a partial scan instead of
 * pretending the environment was fully covered.
 */
size_t app_device_materialize(const app_scan_evidence_t *ev, bool *truncated);

/* Current generation. */
uint32_t app_device_generation(void);

#ifdef __cplusplus
}
#endif
