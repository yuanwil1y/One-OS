#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KISMET_BLE_DEFAULT_MAX_DEVICES 64u
#define KISMET_BLE_DEFAULT_DEVICE_IDLE_MS 120000u
#define KISMET_BLE_HARD_MAX_DEVICES 128u
#define KISMET_BLE_MAX_ADV_COPY 255u

#define KISMET_BLE_DEFAULT_DURATION_MS 5000u
#define KISMET_BLE_DEFAULT_REPORT_COPY_LEN 255u
#define KISMET_BLE_DEFAULT_REPORT_QUEUE_DEPTH 8u
#define KISMET_BLE_HARD_MAX_REPORT_QUEUE_DEPTH 16u
#define KISMET_BLE_HARD_MAX_DURATION_MS 600000u

#define KISMET_BLE_PARTIAL_DEVICE_EVICTION (1u << 0)
#define KISMET_BLE_PARTIAL_REPORT_DROP (1u << 1)
#define KISMET_BLE_PARTIAL_TRUNCATED_REPORT (1u << 2)

typedef struct kismet_ble_tracker kismet_ble_tracker_t;
typedef struct kismet_ble_session kismet_ble_session_t;

typedef struct {
    uint16_t max_devices;
    uint32_t device_idle_ms;
} kismet_ble_tracker_config_t;

typedef struct {
    uint64_t seen_ms;
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    bool connectable;
} kismet_ble_observation_t;

typedef struct {
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi_last;
    int8_t rssi_min;
    int8_t rssi_max;
    bool connectable;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t seen_count;
} kismet_ble_device_t;

typedef struct {
    uint32_t device_evictions;
    uint32_t report_queue_drops;
    uint32_t truncated_reports;
    uint32_t partial_flags;
} kismet_ble_tracker_stats_t;

esp_err_t kismet_ble_tracker_create(const kismet_ble_tracker_config_t *config,
                                    kismet_ble_tracker_t **out_tracker);
void kismet_ble_tracker_destroy(kismet_ble_tracker_t *tracker);
void kismet_ble_tracker_reset(kismet_ble_tracker_t *tracker);
esp_err_t kismet_ble_tracker_ingest(kismet_ble_tracker_t *tracker,
                                    const kismet_ble_observation_t *observation);
void kismet_ble_tracker_expire(kismet_ble_tracker_t *tracker, uint64_t now_ms);
size_t kismet_ble_tracker_count(const kismet_ble_tracker_t *tracker);
esp_err_t kismet_ble_tracker_get(const kismet_ble_tracker_t *tracker,
                                 size_t index,
                                 kismet_ble_device_t *out_device);
esp_err_t kismet_ble_tracker_get_stats(const kismet_ble_tracker_t *tracker,
                                       kismet_ble_tracker_stats_t *out_stats);

typedef struct {
    uint64_t seen_ms;
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    bool connectable;
    uint16_t original_len;
    uint16_t captured_len;
    bool truncated;
    uint8_t bytes[KISMET_BLE_MAX_ADV_COPY];
} kismet_ble_report_t;

typedef void (*kismet_ble_report_callback_t)(const kismet_ble_report_t *report,
                                              void *user_ctx);

typedef struct {
    uint32_t duration_ms;      /* 0 = KISMET_BLE_DEFAULT_DURATION_MS. */
    uint16_t report_copy_len;  /* 0 = KISMET_BLE_DEFAULT_REPORT_COPY_LEN. */
    uint8_t report_queue_depth;/* 0 = KISMET_BLE_DEFAULT_REPORT_QUEUE_DEPTH. */
} kismet_ble_session_config_t;

typedef struct {
    esp_err_t native_error;
    bool canceled;
    uint32_t elapsed_ms;
    uint32_t reports_received;
    uint32_t reports_delivered;
    uint32_t report_queue_drops;
    uint32_t truncated_reports;
    uint32_t partial_flags;
} kismet_ble_session_result_t;

/* v1 owns one NimBLE host lifecycle for the bounded passive scan. The
 * application serializes this stage with other BLE owners. No GATT, AD
 * structure parsing, service parsing, manufacturer parsing, or recognition is
 * performed here. */
esp_err_t kismet_ble_session_start(const kismet_ble_session_config_t *config,
                                   kismet_ble_tracker_t *tracker,
                                   kismet_ble_report_callback_t report_cb,
                                   void *report_cb_ctx,
                                   kismet_ble_session_t **out_session);
esp_err_t kismet_ble_session_cancel(kismet_ble_session_t *session);
esp_err_t kismet_ble_session_wait(kismet_ble_session_t *session, uint32_t timeout_ms);
esp_err_t kismet_ble_session_get_result(const kismet_ble_session_t *session,
                                        kismet_ble_session_result_t *out_result);

/*
 * Release a session.
 *
 * Teardown is bounded: it waits a finite time for the session task to exit. If the
 * task does not exit in time the session is deliberately NOT freed, because that
 * task still references it. Use the _checked form to learn whether teardown
 * completed.
 */
void kismet_ble_session_destroy(kismet_ble_session_t *session);

/*
 * As kismet_ble_session_destroy(), but reports whether teardown completed.
 *
 * Returns ESP_ERR_TIMEOUT when the session task did not exit within the bound. In
 * that case nothing is freed and the NimBLE host may still be running; the caller
 * must not treat the scan as cleanly finished or publish its evidence.
 *
 * A timed-out session is NOT lost. The task sets `finished` as its last action
 * before deleting itself, so the handle stays valid and the caller can:
 *
 *   1. keep the pointer and call kismet_ble_session_task_alive() later;
 *   2. once that returns false, call destroy_checked() again - it then frees the
 *      session immediately, because `finished` is already set.
 *
 * Until then the session owns nvs_flash and the NimBLE host, so nothing may call
 * nvs_flash_init() or nimble_port_init() and no new session may be started.
 */
esp_err_t kismet_ble_session_destroy_checked(kismet_ble_session_t *session);

/*
 * Is the session's task still running?
 *
 * Safe to call on a session whose teardown timed out, and the only supported way
 * to decide whether the NimBLE host lifecycle has been handed back. Returns false
 * for a NULL session and for one whose task has reached its completion point.
 */
bool kismet_ble_session_task_alive(const kismet_ble_session_t *session);

/* Tracker enumeration/get calls are intended after the mutating session has
 * completed (or otherwise under application-owned serialization). */

#ifdef __cplusplus
}
#endif
