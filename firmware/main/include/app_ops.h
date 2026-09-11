#pragma once

/*
 * Application-level operation ownership for the headless Nearby Devices
 * runtime.
 *
 * The ESP32-C6 has one shared 2.4 GHz RF path, so competing product operations
 * must not overlap. This module owns that mutual-exclusion decision and the
 * per-stage lifecycle of a full environment scan.
 *
 * It is intentionally platform independent: no ESP-IDF or FreeRTOS headers.
 * The firmware worker and the host tests therefore run the exact same state
 * machine, which is what makes BUSY/cancel/terminal behaviour verifiable
 * without hardware.
 *
 * This is not a generic "radio runtime" framework. It is the product
 * application's own operation gate.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Product operations that compete for radio, network or storage ownership. */
typedef enum {
    APP_OP_STATE_IDLE = 0,
    APP_OP_STATE_SCANNING,
    APP_OP_STATE_COMMISSIONING,
    APP_OP_STATE_WEB_MANAGEMENT,
} app_op_state_t;

/* Scan stages in application execution order. */
typedef enum {
    APP_STAGE_WIFI_RF = 0,
    APP_STAGE_BLE_RF,
    APP_STAGE_THREAD,
    APP_STAGE_ZIGBEE,
    APP_STAGE_MDNS,
    APP_STAGE_SSDP,
    APP_STAGE_LAN_HOSTS,
    APP_STAGE_LAN_SERVICES,
    APP_STAGE_ENRICHMENT,
    APP_STAGE_MATERIALIZE,
    APP_STAGE_COUNT,
} app_scan_stage_t;

typedef enum {
    APP_STAGE_STATE_PENDING = 0,
    APP_STAGE_STATE_RUNNING,
    APP_STAGE_STATE_DONE,
    APP_STAGE_STATE_PARTIAL,
    APP_STAGE_STATE_FAILED,
    APP_STAGE_STATE_SKIPPED,
    APP_STAGE_STATE_CANCELED,
} app_stage_state_t;

typedef enum {
    APP_OPS_OK = 0,
    APP_OPS_ERR_BUSY,          /* another operation owns the hardware */
    APP_OPS_ERR_NOT_ACTIVE,    /* nothing to cancel/finish */
    APP_OPS_ERR_INVALID_STATE, /* transition not legal from the current state */
    APP_OPS_ERR_INVALID_ARGUMENT,
} app_ops_status_t;

/*
 * Persistent record of one scan generation.
 *
 * `generation` is what lets late callbacks from a cancelled or superseded scan
 * be discarded: a callback carries the generation it belongs to and the
 * application drops anything that does not match the current one.
 */
typedef struct {
    uint32_t generation;
    uint32_t request_id;
    bool canceled;
    bool partial;              /* at least one stage reported partial data */
    bool truncated;            /* a fixed table or queue hit capacity */
    app_scan_stage_t current;  /* furthest stage started */
    app_stage_state_t states[APP_STAGE_COUNT];
} app_scan_status_t;

typedef struct {
    app_op_state_t state;
    app_scan_status_t scan;
} app_ops_t;

/* Stable names used by diagnostics and tests. */
const char *app_ops_state_name(app_op_state_t state);
const char *app_scan_stage_name(app_scan_stage_t stage);
const char *app_stage_state_name(app_stage_state_t state);

void app_ops_init(app_ops_t *ops);

app_op_state_t app_ops_state(const app_ops_t *ops);

/*
 * Begin a scan generation.
 *
 * Fails with APP_OPS_ERR_BUSY when any operation is already active. On success
 * the generation counter advances, all stages become PENDING, and `out` is
 * filled so the caller can record which request owns this run.
 */
app_ops_status_t app_ops_scan_begin(app_ops_t *ops,
                                    uint32_t request_id,
                                    app_scan_status_t *out);

/* Mark a stage as running. Records the furthest stage reached. */
app_ops_status_t app_ops_stage_begin(app_ops_t *ops, app_scan_stage_t stage);

/*
 * Mark a stage terminal.
 *
 * A canceled scan forces a stage that reports DONE to be recorded as CANCELED,
 * so a canceled run can never be mistaken for a complete one.
 */
app_ops_status_t app_ops_stage_end(app_ops_t *ops,
                                   app_scan_stage_t stage,
                                   app_stage_state_t state);

/* Record that a fixed table or queue hit capacity during this generation. */
void app_ops_scan_mark_truncated(app_ops_t *ops);

/* Request cancellation of the active scan. */
app_ops_status_t app_ops_scan_cancel(app_ops_t *ops);

bool app_ops_scan_is_canceled(const app_ops_t *ops);

/*
 * Finish the active scan and return to IDLE.
 *
 * Stages still PENDING are recorded as SKIPPED, so "skipped" and "done" stay
 * distinguishable in the final report.
 */
app_ops_status_t app_ops_scan_finish(app_ops_t *ops, app_scan_status_t *out);

/* Read the current scan report without mutating it. Returns false when no scan
 * has ever run. */
bool app_ops_scan_snapshot(const app_ops_t *ops, app_scan_status_t *out);

/*
 * Take exclusive ownership for a non-scan product operation.
 * Used by commissioning and Web Management so they cannot overlap a scan.
 */
app_ops_status_t app_ops_begin_exclusive(app_ops_t *ops, app_op_state_t state);
app_ops_status_t app_ops_end_exclusive(app_ops_t *ops, app_op_state_t state);

/* Number of stages in a terminal state (DONE/PARTIAL/FAILED/SKIPPED/CANCELED).
 * Progress must be derived from real stage completion, never from a timer. */
uint32_t app_ops_stage_completed_count(const app_scan_status_t *scan);

#ifdef __cplusplus
}
#endif
