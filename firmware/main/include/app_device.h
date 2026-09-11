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

#include "app_recognizer.h"
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
 * as not-yet-seen.
 *
 * finish() is source-aware. A device is only swept when at least one of the
 * protocols that observed it actually ran in this generation and did not report
 * it again. A device whose protocols were all skipped (for example "no IP"),
 * canceled or failed is kept and marked STALE instead: "this protocol never got
 * a chance to look" is not evidence that the device disappeared, and treating it
 * as such would mass-delete devices on any partial scan.
 *
 * `scan` may be NULL, in which case nothing is swept and every unseen ephemeral
 * device is marked stale - the conservative choice.
 */
void app_device_generation_begin(uint32_t generation);
void app_device_generation_finish(const app_scan_status_t *scan);

/* True when a stage ran far enough to have observed its protocol this
 * generation. Used to decide whether absence of evidence means anything. */
bool app_scan_stage_was_observed(const app_scan_status_t *scan,
                                 app_scan_stage_t stage);

/* Materialise every observation in `ev` into HA Device/Entity/State.
 *
 * This is the single production path from evidence to Device/Entity. There is no
 * separate "without recognition" variant on purpose: one function means a caller
 * cannot accidentally take the unrecognised path and quietly lose recognition.
 *
 * `recognizer` may be NULL, and the database behind it may be unopenable; both
 * mean "recognition unavailable", which still produces a generic read-only Device.
 * An unmatched or ambiguous result likewise keeps the Device generic. A matched
 * result may add profile-defined entities, but a writable binding is attached only
 * when recognition reports the backend as actually drivable - so a database record
 * claiming `writable` cannot conjure a control path on its own.
 *
 * Returns the number of devices created or refreshed. Sets `*truncated` when a
 * bounded table was full, so the caller reports a partial scan rather than
 * pretending the environment was fully covered.
 */
size_t app_device_materialize(const app_scan_evidence_t *ev,
                              const app_recognizer_ref_t *recognizer,
                              bool *truncated);

/* Current generation. */
uint32_t app_device_generation(void);

/*
 * Number of application bindings removed so far - swept as no longer observed,
 * or evicted to make room. Reported by diagnostics: a device that disappears
 * from the list must be accounted for, not silently dropped.
 */
uint32_t app_device_swept_count(void);

/*
 * Per-observation recognition results, filled by the enrichment stage.
 *
 * WHY THIS EXISTS
 * ---------------
 * The product workflow recognises the whole accumulated evidence set in one
 * place and only then materialises Devices. Keeping that order matters:
 *
 *   - "one App Device DB is responsible for all fingerprint matching", so
 *     matching happens once per observation per generation, in one stage that
 *     the scan report accounts for, instead of being re-derived per protocol
 *     while the device table is being written;
 *   - the device table still receives recognition only through
 *     app_recognizer_ref_t, so it cannot reach the SD database itself;
 *   - the result is inspectable, so a test can assert what recognition decided
 *     without going through materialisation.
 *
 * Entries are keyed by the same identity the device table uses (protocol +
 * observation bytes), so a lookup cannot cross protocols: the same bytes in
 * Wi-Fi and BLE stay two devices with two independent results.
 */
#define APP_RECOGNITION_TABLE_MAX APP_DEVICE_MAX

typedef enum {
    APP_RECOGNITION_ENTRY_EMPTY = 0,
    APP_RECOGNITION_ENTRY_PRESENT,
} app_recognition_entry_state_t;

typedef struct {
    app_recognition_entry_state_t state;
    uint32_t sources;
    char identity[HA_CORE_ID_LEN];   /* the same key the device table builds */
    bool attempted;                  /* false: recognition could not run at all */
    app_recognition_result_t result;
} app_recognition_entry_t;

typedef struct {
    app_recognition_entry_t entries[APP_RECOGNITION_TABLE_MAX];
    size_t count;
    /*
     * True when an observation could not be stored because the table was full.
     * The caller reports a partial scan; it never means "nothing matched".
     */
    bool truncated;
} app_recognition_table_t;

void app_recognition_table_reset(app_recognition_table_t *table);

/* Number of recorded outcomes. */
size_t app_recognition_table_count(const app_recognition_table_t *table);

/* True when an observation could not be recorded because the table was full. The
 * caller reports a partial scan; it never means "nothing matched". */
bool app_recognition_table_truncated(const app_recognition_table_t *table);

/* Identity key for one observation, exactly as the device table builds it.
 * `out` receives a NUL-terminated key and the return value is the key length,
 * or 0 when the observation has no usable identity. */
size_t app_device_identity_of_wifi(const app_scan_wifi_t *obs, char *out, size_t out_size);
size_t app_device_identity_of_ble(const app_scan_ble_t *obs, char *out, size_t out_size);
size_t app_device_identity_of_lan(const app_scan_lan_t *obs, char *out, size_t out_size);

const app_recognition_entry_t *app_recognition_table_find(
    const app_recognition_table_t *table, const char *identity);

/*
 * Recognise every observation in `ev` and record the outcome.
 *
 * This is the enrichment stage body. It performs the matching, resolves the
 * decoder/quirk selection and prepares the entity recipes, but it writes no
 * Device or Entity: materialisation applies them, so a failure here leaves the
 * previous generation's Devices intact rather than half-updated.
 *
 * Returns the number of observations recorded. `recognizer` may be NULL or point
 * at a closed database; every entry is then recorded as "not attempted" and the
 * caller keeps generic Devices.
 */
size_t app_recognition_enrich(const app_scan_evidence_t *ev,
                              const app_recognizer_ref_t *recognizer,
                              app_recognition_table_t *table);

/*
 * A recognizer view over an already-enriched table.
 *
 * Materialisation receives this instead of the database, so each Device adopts
 * the result enrichment decided for its own observation. A device with no entry
 * is reported as not attemptable, which keeps it generic.
 */
app_recognizer_ref_t app_recognition_table_recognizer(app_recognition_table_t *table);

#ifdef __cplusplus
}
#endif
