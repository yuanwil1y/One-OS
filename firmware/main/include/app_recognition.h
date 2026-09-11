#pragma once

/*
 * Device recognition: the one production matcher.
 *
 * This is application infrastructure. It is not a Level-2 family and not a second
 * recognition engine: the SD .nbdb is the single corpus, and every protocol's
 * evidence goes through device_db_match_*() below. No component may add its own
 * fingerprint table beside it.
 *
 * Dependency direction is deliberately one-way. The device table (app_device.c)
 * must not know how a device is recognised, so it receives a `recognizer` vtable
 * and calls it. That keeps the recognition path replaceable and, more
 * importantly, keeps the device logic host-testable without an SD card: the tests
 * supply their own recognizer and assert what the device layer does with each
 * possible outcome.
 *
 * Storage rules this layer exists to enforce:
 *   - the corpus is read from SD on demand; the whole database is never loaded
 *     into RAM, only the header/index plus one record at a time;
 *   - a missing card, a corrupt file, an incompatible version or an I/O error all
 *     degrade to "no recognition" while observed devices stay visible;
 *   - a statically linked fixture is NOT provided. Test fixtures live in host
 *     tests only and are never a firmware fallback.
 *
 * Platform independent: no ESP-IDF/FreeRTOS/LVGL headers here, so the same source
 * is compiled for the target and for the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_device.h"
#include "app_scan.h"
#include "device_db_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds on what one recognition may carry back. */
#define APP_RECOGNITION_MAX_LABEL 48u
#define APP_RECOGNITION_MAX_RECIPES 8u

/*
 * How the recognition source is doing, independent of any single match.
 *
 * This is reported in Settings/diagnostics and, critically, it is what lets the
 * application say "recognition unavailable" rather than "no devices found".
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

/* One entity recipe as the device table needs it: presentation plus the binding
 * decision. Nothing here is a protocol handle. */
typedef struct {
    uint8_t domain_id;
    uint8_t backend;
    char domain[HA_CORE_DOMAIN_LOCAL];
    char name[HA_CORE_NAME_LOCAL];
    char device_class[HA_CORE_DEVICE_CLASS_LOCAL];
    char unit[HA_CORE_UNIT_LOCAL];
    int32_t min_value;
    int32_t max_value;
    uint32_t scale;
    uint32_t read_source_id;
} app_entity_recipe_t;

/*
 * The outcome of recognising one observation.
 *
 * `matched` false with `ambiguous` or `not_found` both leave the device generic
 * and read-only; the difference is only what the UI says about why.
 */
typedef struct {
    bool matched;
    bool ambiguous;
    uint32_t profile_id;
    char display_name[APP_RECOGNITION_MAX_LABEL];
    char vendor[APP_RECOGNITION_MAX_LABEL];
    char model[APP_RECOGNITION_MAX_LABEL];

    /* Decoder/quirk selection, resolved by the database rather than searched for
     * at runtime by each family. DEVICE_DB_NO_INDEX means "none selected". */
    uint32_t theengs_decoder_id;
    uint32_t zha_quirk_id;

    /*
     * Control exposure is a three-way decision, not a flag:
     *   - the database may declare a writable profile, AND
     *   - the recipe must name a backend this firmware can actually drive, AND
     *   - the runtime precondition for that backend must hold.
     * Only when all three agree does the device table attach a writable entity.
     * Exposed here as already-resolved fields so the device layer never has to
     * interpret backend ids itself.
     */
    bool backend_supported;
    const char *backend_name;

    uint8_t recipe_count;
    app_entity_recipe_t recipes[APP_RECOGNITION_MAX_RECIPES];
} app_recognition_result_t;

/*
 * A recognizer. Implemented by the SD-backed device database and, in tests, by a
 * stub. `ctx` is the recognizer's own state.
 */
typedef struct {
    /*
     * Recognise one observation. Returns false when recognition could not be
     * attempted at all (no database); `*out` is zeroed first, so a recognizer
     * only fills what it actually resolved.
     */
    bool (*recognize)(void *ctx, uint32_t sources,
                      const app_scan_wifi_t *wifi,
                      const app_scan_ble_t *ble,
                      const app_scan_lan_t *lan,
                      app_recognition_result_t *out);

    /* Current state of the recognition source. */
    app_db_state_t (*state)(void *ctx);

    /* Short status for diagnostics: version when ready, reason when not. Never
     * contains a path secret or a credential. */
    void (*describe)(void *ctx, char *out, size_t out_size);
} app_recognizer_t;

/* A recognizer plus its context, which is what the device layer receives. */
typedef struct {
    const app_recognizer_t *ops;
    void *ctx;
} app_recognizer_ref_t;

/*
 * Materialise an observation with recognition applied.
 *
 * The single production path from evidence to Device/Entity: the caller passes
 * evidence and a recognizer, and this is the only place that decides identity,
 * recognition state and read-only-ness.
 *
 * `wifi`, `ble` and `lan` are mutually exclusive: exactly one is non-NULL and
 * `sources` names which. A NULL recognizer is valid and means recognition is
 * unavailable, which must still produce a generic Device.
 */
size_t app_device_materialize_with_recognition(const app_scan_evidence_t *ev,
                                               const app_recognizer_ref_t *recognizer,
                                               bool *out_truncated);

#ifdef __cplusplus
}
#endif
