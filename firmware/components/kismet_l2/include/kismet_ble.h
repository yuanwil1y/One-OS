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
    uint32_t duration_ms;
    uint16_t report_copy_len;
    uint8_t report_queue_depth;
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

/* v1 owns one NimBLE host lifecycle for the bounded passive scan. The application
 * serializes this stage with other BLE owners; no GATT or AD parsing is performed. */
esp_err_t kismet_ble_session_start(const kismet_ble_session_config_t *config,
                                   kismet_ble_tracker_t *tracker,
                                   kismet_ble_report_callback_t report_cb,
                                   void *report_cb_ctx,
                                   kismet_ble_session_t **out_session);
esp_err_t kismet_ble_session_cancel(kismet_ble_session_t *session);
esp_err_t kismet_ble_session_wait(kismet_ble_session_t *session, uint32_t timeout_ms);
esp_err_t kismet_ble_session_get_result(const kismet_ble_session_t *session,
                                        kismet_ble_session_result_t *out_result);
void kismet_ble_session_destroy(kismet_ble_session_t *session);

#ifdef __cplusplus
}
#endif
