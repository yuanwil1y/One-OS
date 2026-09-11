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
#include "esp_err.h"
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

#ifdef __cplusplus
}
#endif
