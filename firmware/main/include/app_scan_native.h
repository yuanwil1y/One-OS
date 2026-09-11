#pragma once

/*
 * Native implementation of the B2 scan stages.
 *
 * One entry point per stage. Each is finite, cancellable, repeatable, and
 * reports what it actually achieved instead of assuming success. The caller
 * (the application worker) owns the operation gate and the evidence store; these
 * functions only perform the native calls and copy bounded results into it.
 *
 * Resource handover is explicit:
 *   - APP_STAGE_WIFI_RF takes the Wi-Fi driver from the STA manager, runs the
 *     Kismet session, and gives the driver back before returning.
 *   - APP_STAGE_BLE_RF runs the Kismet NimBLE session, which owns the host
 *     lifecycle for its bounded duration.
 *   - LAN stages require a valid IPv4 address and are refused otherwise.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_scan.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Per-stage wall-clock budget. 0 selects the stage default. */
    uint32_t wifi_duration_ms;
    uint32_t ble_duration_ms;
    uint32_t mdns_timeout_ms;
    uint32_t ssdp_timeout_ms;
    uint32_t nmap_timeout_ms;
    /* Peak evidence retained. */
    uint16_t wifi_max_devices;
    uint16_t ble_max_devices;
} app_scan_native_config_t;

typedef struct {
    uint32_t frames_parsed;
    uint32_t frames_malformed;
    uint32_t frames_truncated;
    uint32_t wifi_queue_drops;
    uint32_t ble_queue_drops;
    uint32_t ble_adv_evictions;
    uint32_t lan_services;
    uint32_t lan_hosts;
    esp_err_t wifi_native_error;
    esp_err_t ble_native_error;
    bool wifi_canceled;
    bool ble_canceled;
} app_scan_native_stats_t;

void app_scan_native_config_default(app_scan_native_config_t *out);

/*
 * Wi-Fi RF stage.
 *
 * Sequence, all of which must succeed before any evidence is produced:
 *   release STA driver -> start Kismet session -> wait -> destroy session ->
 *   restore STA
 *
 * `canceled` is treated as a normal bounded outcome, not an error. A failure to
 * acquire or restore the driver is reported through the return value so the
 * caller can mark the stage FAILED and the LAN stages skipped.
 */
esp_err_t app_scan_native_wifi_rf(app_scan_evidence_t *ev,
                                  const app_scan_native_config_t *config,
                                  app_scan_native_stats_t *stats);

/* BLE RF stage: Kismet NimBLE passive scan plus Wireshark AD parsing. */
esp_err_t app_scan_native_ble_rf(app_scan_evidence_t *ev,
                                 const app_scan_native_config_t *config,
                                 app_scan_native_stats_t *stats);

/* LAN stages. Refuse (ESP_ERR_INVALID_STATE) when there is no IPv4 address. */
esp_err_t app_scan_native_mdns(app_scan_evidence_t *ev,
                               const app_scan_native_config_t *config,
                               app_scan_native_stats_t *stats);
esp_err_t app_scan_native_ssdp(app_scan_evidence_t *ev,
                               const app_scan_native_config_t *config,
                               app_scan_native_stats_t *stats);
esp_err_t app_scan_native_lan_hosts(app_scan_evidence_t *ev,
                                    const app_scan_native_config_t *config,
                                    app_scan_native_stats_t *stats);
esp_err_t app_scan_native_lan_services(app_scan_evidence_t *ev,
                                       const app_scan_native_config_t *config,
                                       app_scan_native_stats_t *stats);

/* True when this stage was requested to stop. Set by the application worker. */
void app_scan_native_request_cancel(bool canceled);
bool app_scan_native_cancel_requested(void);

#ifdef __cplusplus
}
#endif
