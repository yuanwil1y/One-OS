#pragma once

/*
 * Recognition data types: database state, an entity recipe, and the outcome of
 * one match.
 *
 * Deliberately data-only and dependency-free apart from the format and
 * HA-core size constants. Splitting these out of both the database reader and
 * the policy layer keeps the dependency graph acyclic:
 *
 *   app_recognizer.h   (types)          <- included by everyone
 *   app_device_db.h    (reader)         <- includes types
 *   app_recognition.h  (policy)         <- includes types + reader
 *   app_device.h       (device table)   <- includes types only
 *
 * The device table therefore cannot reach the filesystem, and the reader cannot
 * reach the device table.
 *
 * Platform independent: no ESP-IDF/FreeRTOS/LVGL headers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_db_format.h"
#include "ha_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_RECOGNITION_MAX_LABEL 48u
#define APP_RECOGNITION_MAX_RECIPES 8u

/*
 * How the recognition source is doing, independent of any single match.
 *
 * Reported in Settings/diagnostics, and - critically - it is what lets the
 * application distinguish "recognition is unavailable" from "nothing matched".
 */
typedef enum {
    APP_DB_STATE_CLOSED = 0,
    APP_DB_STATE_READY,
    APP_DB_STATE_SD_MISSING,
    APP_DB_STATE_FILE_MISSING,
    APP_DB_STATE_CORRUPT,
    APP_DB_STATE_INCOMPATIBLE,
    APP_DB_STATE_IO_ERROR,
} app_db_state_t;

const char *app_db_state_name(app_db_state_t state);

/* True when recognition can be attempted at all. */
bool app_db_state_is_usable(app_db_state_t state);

/* One entity recipe as the Device table needs it. Nothing here is a protocol
 * handle: it is presentation plus an already-resolved binding decision. */
typedef struct {
    uint8_t domain_id;
    uint8_t backend;
    char domain[HA_CORE_DOMAIN_LEN];
    char name[HA_CORE_NAME_LEN];
    char device_class[HA_CORE_DEVICE_CLASS_LEN];
    char unit[HA_CORE_UNIT_LEN];
    int32_t min_value;
    int32_t max_value;
    uint32_t scale;
    /* Decoder property, cluster-attribute id, or characteristic index, depending
     * on the backend. */
    uint32_t read_source_id;
    /* Set only when a control path is actually available, so the device layer can
     * build the binding without knowing which family is behind it.
     * DEVICE_DB_NO_INDEX means read-only. */
    uint32_t write_target_id;
} app_entity_recipe_t;

/*
 * The outcome of recognising one observation.
 *
 * `matched == false` leaves the device generic and read-only. `ambiguous` only
 * records why: an ambiguous match is never permission to guess and never produces
 * a writable binding.
 */
typedef struct {
    bool matched;
    bool ambiguous;
    uint32_t profile_id;
    char display_name[APP_RECOGNITION_MAX_LABEL];
    char vendor[APP_RECOGNITION_MAX_LABEL];
    char model[APP_RECOGNITION_MAX_LABEL];

    /*
     * Decoder/quirk selection, resolved by the database when it selects the
     * profile, so no family has to run a second model search at runtime.
     * DEVICE_DB_NO_INDEX means "none selected".
     */
    uint32_t theengs_decoder_id;
    uint32_t zha_quirk_id;

    /*
     * Control exposure is a three-way decision rather than a flag:
     *   - the database may declare a writable profile, AND
     *   - the recipe must name a backend this firmware can actually drive, AND
     *   - the runtime precondition for that backend must hold.
     * These are already-resolved fields, so the device layer never interprets
     * backend ids itself and a "writable" database record alone can never create
     * a live control path.
     */
    bool backend_supported;
    const char *backend_name;

    uint8_t recipe_count;
    app_entity_recipe_t recipes[APP_RECOGNITION_MAX_RECIPES];
} app_recognition_result_t;

/* Whether this firmware can drive a recipe backend today. */
bool app_backend_is_drivable(uint8_t backend);
const char *app_backend_name(uint8_t backend);

/*
 * The recognizer vtable is an opaque forward declaration here.
 *
 * Its definition lives with the SD-backed implementation (app_device_db.h), so
 * the device table can hold and call a recognizer without being able to see how
 * recognition is implemented, and without this header depending on the reader.
 */
struct app_recognizer_ops;
typedef struct app_recognizer_ops app_recognizer_ops_t;

/* A recognizer plus its context: what the device table receives. */
typedef struct {
    const app_recognizer_ops_t *ops;
    void *ctx;
} app_recognizer_ref_t;

#ifdef __cplusplus
}
#endif
