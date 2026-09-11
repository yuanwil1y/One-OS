#pragma once

/*
 * Application scan policy and evidence store.
 *
 * This header and app_scan.c are platform independent: no ESP-IDF, FreeRTOS or
 * LVGL dependency, so the firmware and the host tests run the exact same stage
 * decisions and the same bounded evidence tables.
 *
 * Split of responsibility:
 *   - app_scan.c decides *what should happen* to each stage from bounded
 *     observations (unprovisioned, no IP, protocol not wired, capacity).
 *   - app_scan_native.c performs the actual native calls (Kismet sessions,
 *     Wireshark parsing, mDNS/SSDP/Nmap) and feeds this layer.
 *
 * Keeping the decision here is what makes "stage skipped because there is no
 * IP" or "stage failed because the handover failed" testable without hardware,
 * and it is what stops a partial scan from being reported as a complete one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ops.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded application evidence sizes. Chosen for the C6's memory budget; the
 * Device DB (SD) is the production recognition source, this is a working set. */
#define APP_SCAN_MAX_WIFI 32u
#define APP_SCAN_MAX_BLE 32u
#define APP_SCAN_MAX_BLE_ADV 16u
#define APP_SCAN_MAX_LAN 24u
#define APP_SCAN_MAX_SERVICE_NAME 24u
#define APP_SCAN_MAX_SSID 32u
#define APP_SCAN_MAX_HOSTNAME 48u
#define APP_SCAN_MAX_IPV4 16u
#define APP_SCAN_MAX_ADV_BYTES 62u
#define APP_SCAN_MAX_UUID16 8u

/* One bounded copy of Wi-Fi RF evidence. Never contains raw frame buffers. */
typedef struct {
    uint32_t generation;
    uint8_t bssid[6];
    uint8_t role;              /* kismet_wifi_role_t as a plain byte */
    int8_t rssi_last;
    uint8_t channel;
    uint16_t channel_bitmap;
    bool has_ssid;
    bool ssid_hidden;
    uint8_t ssid[APP_SCAN_MAX_SSID];
    uint8_t ssid_len;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t seen_count;
} app_scan_wifi_t;

/* Bounded BLE advertisement structure parsed by the Wireshark family. */
typedef struct {
    bool valid;
    uint8_t address[6];
    uint8_t address_type;
    bool name_present;
    char name[APP_SCAN_MAX_SERVICE_NAME];
    bool flags_present;
    uint8_t flags;
    bool tx_power_present;
    int8_t tx_power_dbm;
    bool appearance_present;
    uint16_t appearance;
    uint8_t uuid16_count;
    uint16_t uuid16[APP_SCAN_MAX_UUID16];
    uint8_t service_data_count;
    uint8_t manufacturer_data_count;
    uint16_t first_company_id;
    bool truncated;
} app_scan_ble_adv_t;

/* One bounded copy of BLE RF evidence plus its parsed advertisement. */
typedef struct {
    uint32_t generation;
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi_last;
    bool connectable;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t seen_count;
    uint16_t original_len;
    uint16_t captured_len;
    bool truncated;
    bool has_parsed_adv;
    app_scan_ble_adv_t adv;
} app_scan_ble_t;

/* One bounded LAN observation, merged from mDNS/SSDP/Nmap evidence. */
typedef struct {
    uint32_t generation;
    char ipv4[APP_SCAN_MAX_IPV4];
    char hostname[APP_SCAN_MAX_HOSTNAME];
    bool from_mdns;
    bool from_ssdp;
    bool from_nmap;
    bool up;
    uint16_t service_count;
    char service[APP_SCAN_MAX_SERVICE_NAME];
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
} app_scan_lan_t;

typedef struct {
    uint32_t device_evictions;
    uint32_t ble_adv_evictions;
    uint32_t lan_evictions;
    uint32_t rej_malformed;
    uint32_t rej_duplicate;
    uint32_t rej_truncated;
    uint32_t queue_drops;
} app_scan_drop_stats_t;

/* Everything the stage planner needs to decide a stage's fate. */
typedef struct {
    bool wifi_configured;      /* stored STA credentials exist */
    bool wifi_connected;       /* STA currently holds an IP */
    bool wifi_driver_acquired; /* handover for this scan succeeded */
    bool ble_available;        /* NimBLE session ran for this generation */
    bool has_ip;               /* an IPv4 address is available for LAN stages */
    bool canceled;
} app_scan_inputs_t;

typedef enum {
    APP_STAGE_ACTION_RUN = 0,      /* execute the stage body */
    APP_STAGE_ACTION_SKIP,         /* not applicable now, e.g. no IP */
    APP_STAGE_ACTION_UNAVAILABLE,  /* dependency missing, e.g. no credentials */
    APP_STAGE_ACTION_REJECTED,     /* handover failed; nothing was attempted */
} app_scan_stage_action_t;

typedef struct {
    app_scan_stage_action_t action;
    app_stage_state_t terminal_state; /* PENDING means "run and decide later" */
    const char *reason;               /* stable short literal, never a secret */
} app_scan_stage_plan_t;

/*
 * How an RF stage actually ended.
 *
 * `stop_confirmed` is the important one: it means session teardown really
 * completed, so the session task can no longer be writing into the evidence
 * store. A stage can hit its own bounded wait and still have stopped cleanly; a
 * stage can also look finished while its session refuses to shut down. Those are
 * different situations and must be reported differently.
 */
typedef struct {
    bool stop_confirmed;
    bool canceled;
    bool timed_out;         /* the stage's own bounded wait expired */
    esp_err_t native_error; /* ESP_OK when no native call failed */
    uint32_t collected;     /* bounded evidence items ingested */
} app_scan_rf_outcome_t;

typedef struct {
    app_stage_state_t terminal_state;
    /* False means the evidence must NOT be published: the session could not be
     * shut down, so its callbacks may still be mutating the store. */
    bool evidence_usable;
    const char *reason;
} app_scan_rf_verdict_t;

/*
 * Decide the recorded state of an RF stage and whether its evidence may be used.
 *
 * Rules, in order:
 *   - teardown not confirmed -> FAILED and evidence unusable. Reporting anything
 *     else would publish data from a session we could not stop.
 *   - canceled               -> CANCELED (a normal bounded outcome, not an error).
 *   - native error           -> FAILED.
 *   - timed out              -> PARTIAL when something was collected, else FAILED.
 *     A timeout means coverage was incomplete, so it must never read as DONE.
 *   - otherwise              -> DONE.
 */
app_scan_rf_verdict_t app_scan_evaluate_rf_stage(const app_scan_rf_outcome_t *outcome);

/*
 * Does an observed stage state mean the protocol was covered well enough that a
 * device's absence is evidence it is gone?
 *
 * Only DONE qualifies. PARTIAL explicitly does not: a partially covered protocol
 * may simply have missed a device, so absence proves nothing. This is what keeps
 * a partial scan from mass-marking devices as disappeared, and it is deliberately
 * stricter than "the stage reported success".
 */
bool app_scan_state_covers_protocol(app_stage_state_t state);

typedef struct {
    app_scan_wifi_t wifi[APP_SCAN_MAX_WIFI];
    size_t wifi_count;
    app_scan_ble_t ble[APP_SCAN_MAX_BLE];
    size_t ble_count;
    app_scan_lan_t lan[APP_SCAN_MAX_LAN];
    size_t lan_count;
    app_scan_drop_stats_t drops;
    uint32_t generation;
} app_scan_evidence_t;

void app_scan_evidence_reset(app_scan_evidence_t *ev, uint32_t generation);

/*
 * Decide what a stage should do. `terminal_state` is meaningful only when the
 * action is not RUN.
 *
 * This is the single place that encodes "LAN stages need an IP" and "an
 * unavailable protocol must be reported as skipped, not silently done".
 */
app_scan_stage_plan_t app_scan_plan_stage(const app_scan_inputs_t *inputs,
                                          app_scan_stage_t stage);

/*
 * Bounded evidence ingestion. Each returns false when the observation was
 * rejected (malformed, duplicate with no new information, or table full and no
 * evictable entry), and updates the drop counters so a caller can report
 * partial/truncated honestly.
 */
bool app_scan_ingest_wifi(app_scan_evidence_t *ev, const app_scan_wifi_t *obs);
bool app_scan_ingest_ble(app_scan_evidence_t *ev, const app_scan_ble_t *obs);
bool app_scan_ingest_lan(app_scan_evidence_t *ev, const app_scan_lan_t *obs);

/* True when any table hit capacity during this generation. */
bool app_scan_evidence_truncated(const app_scan_evidence_t *ev);

/* Find helpers used when materialising devices. Return NULL when absent. */
app_scan_wifi_t *app_scan_find_wifi(app_scan_evidence_t *ev, const uint8_t bssid[6]);
app_scan_ble_t *app_scan_find_ble(app_scan_evidence_t *ev, const uint8_t address[6],
                                  uint8_t address_type);
app_scan_lan_t *app_scan_find_lan(app_scan_evidence_t *ev, const char *ipv4);

/*
 * Compact the parsed advertisement into the bounded representation. When the
 * caller's own copy table is full the advertisement is dropped and counted
 * rather than silently discarded.
 */
bool app_scan_store_ble_adv(app_scan_ble_adv_t *slots, size_t capacity,
                            size_t *used, const app_scan_ble_adv_t *adv,
                            uint32_t *out_evictions);

#ifdef __cplusplus
}
#endif
