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

/*
 * Ownership of a radio after a teardown that did not complete.
 *
 * When a Kismet session's bounded teardown expires, the session is deliberately
 * not freed: its task still dereferences it, and - the part that matters for the
 * rest of the application - that task still owns the underlying radio. The Wi-Fi
 * session calls esp_wifi_stop()/esp_wifi_deinit() in its own cleanup, and the BLE
 * session owns nvs_flash and the NimBLE host.
 *
 * So a timed-out session is not merely a leak. It is a live owner of hardware the
 * application must not touch, and the application records that explicitly instead
 * of assuming the radio is free.
 */
typedef enum {
    APP_SCAN_RF_QUARANTINE_NONE = 0,
    APP_SCAN_RF_QUARANTINE_HELD,      /* a session is alive and owns the radio */
    APP_SCAN_RF_QUARANTINE_RECLAIMED, /* it exited later and has been released */
} app_scan_rf_quarantine_t;

const char *app_scan_rf_quarantine_name(app_scan_rf_quarantine_t state);

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
    /*
     * The verdict each RF stage reached, computed by the host-tested policy in
     * app_scan_evaluate_rf_stage().
     *
     * The caller records this state directly. A stage must not be recorded as DONE
     * merely because its function returned ESP_OK: a session that could not be
     * shut down, or a stage that ended on its own deadline, is PARTIAL or FAILED
     * even though the call returned something the caller might read as success.
     * `*_verdict_set` is false when the stage never reached a verdict.
     */
    bool wifi_verdict_set;
    app_scan_rf_verdict_t wifi_verdict;
    bool ble_verdict_set;
    app_scan_rf_verdict_t ble_verdict;
    /* Whether the stage had to quarantine its radio, and what happened to it
     * afterwards. Reported so "this scan could not use the radio" is visible
     * rather than looking like an empty environment. */
    app_scan_rf_quarantine_t wifi_quarantine;
    bool wifi_quarantine_set;
    app_scan_rf_quarantine_t ble_quarantine;
    bool ble_quarantine_set;
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

/*
 * Service identification on hosts the host-discovery stage already found.
 *
 * This is the only stage that opens TCP connections to other devices, so its
 * scope is deliberately narrow and fixed: only already-observed "up" hosts, only
 * a short well-known port list, passive probes only (never a write, never a
 * credential attempt), one shared deadline across the port and service halves,
 * and cancellation checked between them.
 *
 * Returns ESP_OK when there was nothing to probe; that is not a failure.
 */
esp_err_t app_scan_native_lan_services(app_scan_evidence_t *ev,
                                       const app_scan_native_config_t *config,
                                       app_scan_native_stats_t *stats);

/* True when this stage was requested to stop. Set by the application worker. */
void app_scan_native_request_cancel(bool canceled);
bool app_scan_native_cancel_requested(void);

/*
 * Is the radio free to be taken?
 *
 * Reclaims every quarantined session whose task has since exited, and returns
 * true only when nothing is left holding the radio. Both RF stages call this
 * before doing anything, so no stage can initialise a driver that a previous
 * session's task is still going to deinitialise.
 *
 * The reclamation is also what makes a failed teardown recoverable rather than
 * permanent: the session, its queue, its semaphores and (for BLE) the tracker its
 * callbacks write into are all freed here, at the first moment it is safe.
 *
 * Idempotent and cheap when nothing is quarantined.
 */
bool app_scan_native_radio_available(void);

/* How the Wi-Fi and BLE radios stand, for diagnostics. The RECLAIMED value is
 * remembered until the next quarantine, so an operator can see that a failed
 * teardown did eventually recover instead of it being silently forgotten. */
app_scan_rf_quarantine_t app_scan_native_wifi_quarantine(void);
app_scan_rf_quarantine_t app_scan_native_ble_quarantine(void);

#ifdef __cplusplus
}
#endif
