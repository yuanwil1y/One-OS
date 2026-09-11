/*
 * Application operation gate and scan lifecycle.
 *
 * Platform independent: compiled into both the ESP32-C6 firmware and the host
 * regression tests. No ESP-IDF/FreeRTOS/LVGL headers may be included here.
 */

#include "app_ops.h"

#include <string.h>

const char *app_ops_state_name(app_op_state_t state)
{
    switch (state) {
    case APP_OP_STATE_IDLE:           return "idle";
    case APP_OP_STATE_SCANNING:       return "scanning";
    case APP_OP_STATE_COMMISSIONING:  return "commissioning";
    case APP_OP_STATE_WEB_MANAGEMENT: return "web_management";
    default:                          return "invalid";
    }
}

const char *app_scan_stage_name(app_scan_stage_t stage)
{
    switch (stage) {
    case APP_STAGE_WIFI_RF:      return "wifi_rf";
    case APP_STAGE_BLE_RF:       return "ble_rf";
    case APP_STAGE_THREAD:       return "thread";
    case APP_STAGE_ZIGBEE:       return "zigbee";
    case APP_STAGE_MDNS:         return "mdns";
    case APP_STAGE_SSDP:         return "ssdp";
    case APP_STAGE_LAN_HOSTS:    return "lan_hosts";
    case APP_STAGE_LAN_SERVICES: return "lan_services";
    case APP_STAGE_ENRICHMENT:   return "enrichment";
    case APP_STAGE_MATERIALIZE:  return "materialize";
    default:                     return "invalid";
    }
}

const char *app_stage_state_name(app_stage_state_t state)
{
    switch (state) {
    case APP_STAGE_STATE_PENDING:  return "pending";
    case APP_STAGE_STATE_RUNNING:  return "running";
    case APP_STAGE_STATE_DONE:     return "done";
    case APP_STAGE_STATE_PARTIAL:  return "partial";
    case APP_STAGE_STATE_FAILED:   return "failed";
    case APP_STAGE_STATE_SKIPPED:  return "skipped";
    case APP_STAGE_STATE_CANCELED: return "canceled";
    default:                       return "invalid";
    }
}

static bool stage_is_terminal(app_stage_state_t state)
{
    return state != APP_STAGE_STATE_PENDING && state != APP_STAGE_STATE_RUNNING;
}

static bool stage_index_valid(app_scan_stage_t stage)
{
    return (int)stage >= 0 && (int)stage < (int)APP_STAGE_COUNT;
}

void app_ops_init(app_ops_t *ops)
{
    size_t i;

    if (ops == NULL) {
        return;
    }
    memset(ops, 0, sizeof(*ops));
    ops->state = APP_OP_STATE_IDLE;
    for (i = 0u; i < (size_t)APP_STAGE_COUNT; ++i) {
        ops->scan.states[i] = APP_STAGE_STATE_PENDING;
    }
}

app_op_state_t app_ops_state(const app_ops_t *ops)
{
    if (ops == NULL) {
        return APP_OP_STATE_IDLE;
    }
    return ops->state;
}

app_ops_status_t app_ops_scan_begin(app_ops_t *ops,
                                    uint32_t request_id,
                                    app_scan_status_t *out)
{
    uint32_t next_generation;
    size_t i;

    if (ops == NULL) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (ops->state != APP_OP_STATE_IDLE) {
        return APP_OPS_ERR_BUSY;
    }

    /* Advance the generation first so every event from the previous run is
     * stale before any new stage starts. */
    next_generation = ops->scan.generation + 1u;

    memset(&ops->scan, 0, sizeof(ops->scan));
    ops->scan.generation = next_generation;
    ops->scan.request_id = request_id;
    ops->scan.current = APP_STAGE_WIFI_RF;
    for (i = 0u; i < (size_t)APP_STAGE_COUNT; ++i) {
        ops->scan.states[i] = APP_STAGE_STATE_PENDING;
    }

    ops->state = APP_OP_STATE_SCANNING;

    if (out != NULL) {
        *out = ops->scan;
    }
    return APP_OPS_OK;
}

app_ops_status_t app_ops_stage_begin(app_ops_t *ops, app_scan_stage_t stage)
{
    if (ops == NULL || !stage_index_valid(stage)) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (ops->state != APP_OP_STATE_SCANNING) {
        return APP_OPS_ERR_INVALID_STATE;
    }
    if (ops->scan.states[stage] != APP_STAGE_STATE_PENDING) {
        return APP_OPS_ERR_INVALID_STATE;
    }

    /* A canceled scan must not start further work. */
    if (ops->scan.canceled) {
        return APP_OPS_ERR_INVALID_STATE;
    }

    ops->scan.states[stage] = APP_STAGE_STATE_RUNNING;
    ops->scan.current = stage;
    return APP_OPS_OK;
}

app_ops_status_t app_ops_stage_end(app_ops_t *ops,
                                   app_scan_stage_t stage,
                                   app_stage_state_t state)
{
    if (ops == NULL || !stage_index_valid(stage)) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (ops->state != APP_OP_STATE_SCANNING) {
        return APP_OPS_ERR_INVALID_STATE;
    }
    if (ops->scan.states[stage] != APP_STAGE_STATE_RUNNING) {
        return APP_OPS_ERR_INVALID_STATE;
    }
    if (!stage_is_terminal(state)) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }

    /* Cancellation wins over whatever the stage wanted to report: a canceled
     * stage is never recorded as cleanly done. */
    if (ops->scan.canceled && state == APP_STAGE_STATE_DONE) {
        state = APP_STAGE_STATE_CANCELED;
    }

    ops->scan.states[stage] = state;
    if (state == APP_STAGE_STATE_PARTIAL || state == APP_STAGE_STATE_FAILED) {
        ops->scan.partial = true;
    }
    return APP_OPS_OK;
}

void app_ops_scan_mark_truncated(app_ops_t *ops)
{
    if (ops == NULL) {
        return;
    }
    ops->scan.truncated = true;
    ops->scan.partial = true;
}

app_ops_status_t app_ops_scan_cancel(app_ops_t *ops)
{
    if (ops == NULL) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (ops->state != APP_OP_STATE_SCANNING) {
        return APP_OPS_ERR_NOT_ACTIVE;
    }
    ops->scan.canceled = true;
    return APP_OPS_OK;
}

bool app_ops_scan_is_canceled(const app_ops_t *ops)
{
    if (ops == NULL) {
        return false;
    }
    return ops->scan.canceled;
}

app_ops_status_t app_ops_scan_finish(app_ops_t *ops, app_scan_status_t *out)
{
    size_t i;

    if (ops == NULL) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (ops->state != APP_OP_STATE_SCANNING) {
        return APP_OPS_ERR_NOT_ACTIVE;
    }

    /* Any stage never reached is explicitly SKIPPED: it is a statement about
     * what this generation actually covered, not a silent omission. */
    for (i = 0u; i < (size_t)APP_STAGE_COUNT; ++i) {
        if (ops->scan.states[i] == APP_STAGE_STATE_PENDING) {
            ops->scan.states[i] = APP_STAGE_STATE_SKIPPED;
        } else if (ops->scan.states[i] == APP_STAGE_STATE_RUNNING) {
            /* Defensive: a stage left RUNNING at finish did not complete. */
            ops->scan.states[i] = APP_STAGE_STATE_FAILED;
            ops->scan.partial = true;
        }
    }

    if (out != NULL) {
        *out = ops->scan;
    }
    ops->state = APP_OP_STATE_IDLE;
    return APP_OPS_OK;
}

bool app_ops_scan_snapshot(const app_ops_t *ops, app_scan_status_t *out)
{
    if (ops == NULL || out == NULL) {
        return false;
    }
    if (ops->scan.generation == 0u) {
        return false;
    }
    *out = ops->scan;
    return true;
}

app_ops_status_t app_ops_begin_exclusive(app_ops_t *ops, app_op_state_t state)
{
    if (ops == NULL) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (state != APP_OP_STATE_COMMISSIONING && state != APP_OP_STATE_WEB_MANAGEMENT) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (ops->state != APP_OP_STATE_IDLE) {
        return APP_OPS_ERR_BUSY;
    }
    ops->state = state;
    return APP_OPS_OK;
}

app_ops_status_t app_ops_end_exclusive(app_ops_t *ops, app_op_state_t state)
{
    if (ops == NULL) {
        return APP_OPS_ERR_INVALID_ARGUMENT;
    }
    if (ops->state != state) {
        return APP_OPS_ERR_NOT_ACTIVE;
    }
    ops->state = APP_OP_STATE_IDLE;
    return APP_OPS_OK;
}

uint32_t app_ops_stage_completed_count(const app_scan_status_t *scan)
{
    uint32_t count = 0u;
    size_t i;

    if (scan == NULL) {
        return 0u;
    }
    for (i = 0u; i < (size_t)APP_STAGE_COUNT; ++i) {
        if (stage_is_terminal(scan->states[i])) {
            ++count;
        }
    }
    return count;
}
