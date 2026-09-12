#pragma once

/*
 * Headless application runtime for the Nearby Devices product.
 *
 * This is the single business-logic entry point of the application. The serial
 * diagnostic console uses it today; the LVGL GUI will submit the exact same
 * requests later. No other layer may implement scanning, recognition or control
 * behind its back.
 *
 * Threading contract:
 *   - one application worker task owns every state mutation;
 *   - callers submit requests and read completed reports through this API;
 *   - protocol/timer callbacks must only copy bounded data into the runtime
 *     queue, never mutate HA state or LVGL directly.
 *
 * Resource ownership follows the reported component contracts: for example
 * kismet_ble_session_start() owns one NimBLE host lifecycle for the duration of
 * its bounded scan, so the runtime must not initialise NimBLE itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_diag_protocol.h"
#include "app_ops.h"
#include "app_recognizer.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware identity reported by diagnostics. */
esp_err_t app_runtime_version(char *out, size_t out_size);

/* Resource report. Never contains credentials or protocol keys. */
typedef struct {
    uint32_t free_heap_bytes;
    uint32_t min_free_heap_bytes;
    uint32_t largest_free_block_bytes;
    uint32_t worker_stack_high_water_bytes;
    uint32_t console_stack_high_water_bytes;
    uint32_t queue_drops;
    uint32_t generation;
    uint32_t stage_completed;
    uint32_t stage_total;
    const char *op_state;
    /* Recognition source state and the path it was looked for at, so a missing
     * card, a missing corpus and a corrupt corpus are distinguishable from the
     * console without guessing. */
    const char *db_state;
    const char *db_path;
} app_runtime_resources_t;

esp_err_t app_runtime_get_resources(app_runtime_resources_t *out);

/* Application-owned snapshot of one device/entity for serialisation. Raw
 * pointers and protocol handles never appear here. */
typedef struct {
    char id[HA_CORE_ID_LEN];
    char name[HA_CORE_NAME_LEN];
    char manufacturer[HA_CORE_NAME_LEN];
    char model[HA_CORE_NAME_LEN];
    uint8_t entity_count;
    bool available;
} app_runtime_device_t;

esp_err_t app_runtime_write_devices(char *out, size_t out_size,
                                    size_t *out_written, bool *out_truncated);

esp_err_t app_runtime_write_entities(const char *device_id, char *out,
                                     size_t out_size, size_t *out_written,
                                     bool *out_truncated);

/*
 * Start the runtime: create the operation gate, worker task and request queue.
 * Safe to call once from app_main.
 */
esp_err_t app_runtime_start(void);

/*
 * Submit a request. Returns immediately; the worker processes it in order.
 * `error` reports only submission failures (BUSY queue, bad argument), not the
 * outcome of the operation itself, which is delivered in the response.
 */
esp_err_t app_runtime_submit(const app_diag_request_t *request,
                             app_diag_response_t *out_response,
                             uint32_t timeout_ms);

/* Convenience wrappers used by the console and, later, the GUI. */
esp_err_t app_runtime_request_scan(uint32_t request_id,
                                   app_diag_scan_kind_t kind,
                                   uint32_t timeout_ms,
                                   app_diag_response_t *out_response);
esp_err_t app_runtime_request_cancel(uint32_t target_request_id,
                                     app_diag_response_t *out_response);
esp_err_t app_runtime_request_status(uint32_t request_id,
                                     app_diag_response_t *out_response);
esp_err_t app_runtime_request_control(uint32_t request_id,
                                      const char *entity_id,
                                      const char *action,
                                      const char *value,
                                      app_diag_response_t *out_response);

/* Current operation state, readable from any task. */
app_op_state_t app_runtime_op_state(void);

/*
 * Recognition database state, readable from any task.
 *
 * Separate from the scan report on purpose: the database has a state whether or
 * not a scan has ever run, and Settings has to show it. `READY` means the corpus
 * on the card was validated and can be matched against; every other value is a
 * distinct, reportable reason why recognition is unavailable.
 */
app_db_state_t app_runtime_db_state(void);

/* One-line description of the corpus for Settings/diagnostics, e.g.
 * "ready v20260911 profiles=5". Never contains credentials. */
esp_err_t app_runtime_db_describe(char *out, size_t out_size);

/*
 * Corpus metadata as separate values.
 *
 * /api/status and the console both need the version and the profile count as their own
 * fields rather than as one sentence, and neither may reach into the reader - the
 * runtime is what owns it. Returns false when no corpus is open, in which case the
 * outputs are zeroed rather than left holding a previous corpus's numbers, which would
 * be a status document describing a database that is no longer there.
 */
bool app_runtime_db_info(uint32_t *out_content_version, uint32_t *out_profile_count);

/* Absolute path the corpus is read from, so the operator can compare it against
 * what they wrote to the card. */
const char *app_runtime_db_path(void);

/* ---------------- provisioning portal ---------------- */

/*
 * Present the active session's credentials, ONCE, to the local caller.
 *
 * Deliberately named "present" rather than "get". The AP password and the session token
 * are shown to the operator at the device and are not readable again: a second call
 * reports that they were already shown instead of repeating them, so a terminal
 * scrollback captured later does not contain the password. The product rules put these
 * values on the device's own presentation and a local output, never in a log or an HTTP
 * status response - which is why this is a presentation and not a getter.
 *
 * Returns false when no session is active.
 */
bool app_runtime_portal_present(char *out, size_t out_size);

/* One-line portal report: phase, whether the AP is really up, whether the credentials
 * were presented, and the upload phase. Never the credentials. */
esp_err_t app_runtime_portal_status(char *out, size_t out_size);

/*
 * Register the console task so the resource report can include its stack
 * high-water mark. Purely diagnostic; passing NULL simply omits the value.
 */
void app_runtime_set_console_task(TaskHandle_t task);

#ifdef __cplusplus
}
#endif
