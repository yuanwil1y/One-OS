#pragma once

/*
 * Firmware binding: app_ble_gatt session -> esphome_l2 GATT transport.
 *
 * app_ble_gatt owns the session policy (phases, deadlines, cancellation, the
 * generation guard on notifications) and takes the platform from an ops table.
 * This file is that table for the real firmware, so the policy module stays
 * platform independent and host-testable.
 *
 * WHERE THE TWO INTERFACES DO NOT LINE UP
 *
 *   1. The radio is reached twice. app_ble_gatt takes a radio with
 *      acquire()/release(); the transport suspends and resumes it itself, through
 *      the suspend/resume pair in its own config, at defined points (a connect
 *      suspends and a failed connect resumes). Both calls reach the platform's
 *      pair, and the order is fixed: the session acquires first, the transport
 *      connects and suspends second. That fixes the contract on the platform's
 *      side - its pair must tolerate a second suspend while already suspended for
 *      the same session and resume only when the last holder lets go. A pair that
 *      is not idempotent would put the radio back while the session still believes
 *      it holds it, which is the failure mode that corrupts a scan. A platform
 *      with no arbiter supplies no pair at all, and this adapter then reports
 *      arbitration as unsupported rather than pretending the radio is free.
 *
 *   2. The radio pair is reached twice, through two different pointers. The
 *      transport hands `radio_user` to the suspend/resume hooks; app_ble_gatt
 *      hands its own ops_ctx to acquire/release. The adapter is the ops_ctx, and
 *      it forwards acquire/release to the platform pair with the caller's
 *      `radio_ctx`. Both paths must therefore carry `radio_ctx` as the platform's
 *      `user`, which is what gatt_init sets radio_user to - not the adapter. A
 *      platform hook given the adapter on one path and its own context on the
 *      other reads its state out of the wrong struct, which is a defect this test
 *      group caught.
 *
 *   3. A notification carries no session identity. The transport hands the
 *      subscriber's own `user` pointer back untouched, so app_ble_gatt's
 *      notify_bridge - which is what maps a value handle to the generation the
 *      subscription was made in - is the `user` the adapter registers. The adapter
 *      therefore knows nothing about generations, and the guard stays in exactly
 *      one place.
 *
 *   4. "No such feature" versus "the call failed". The transport has no way to say
 *      it cannot arbitrate the radio, so radio_acquire() returns
 *      ESP_ERR_NOT_SUPPORTED when no pair was supplied, rather than ESP_OK.
 *      Reporting success for arbitration that never happened is the one outcome
 *      the session must never see.
 *
 * The clock is the platform's, in milliseconds. Every wait in app_ble_gatt is
 * bounded by it, so it must be monotonic; esp_timer_get_time() is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ble_gatt.h"
#include "esp_err.h"
#include "esphome_ble_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One adapter instance. Lives as long as the session that uses it; the caller owns
 * it because app_ble_gatt does not allocate.
 *
 * `last_native_error` is the transport's own error code from the most recent
 * failed operation, kept here so gatt_last_error() reports that operation rather
 * than whatever happened to run last.
 */
typedef struct {
    /* The transport session this adapter drives. */
    esphome_ble_gatt_session_t *transport;
    /* Passed to the platform's suspend/resume hooks; may be NULL. */
    void *radio_ctx;
    /* The platform's pair, or NULL when it has no arbiter. */
    esphome_ble_gatt_radio_suspend_fn radio_suspend;
    esphome_ble_gatt_radio_resume_fn radio_resume;
    /* True when the platform supplied both hooks. */
    bool radio_available;
    /* Transport error from the most recent failure; 0 when the last operation
     * succeeded or none has run. */
    int last_native_error;
    /* Every transport call whose result was not ESP_OK, for diagnostics.
     * Successes are not counted. */
    uint32_t failed_operations;
} app_ble_gatt_native_t;

/*
 * Fill `out` with the ops that bind a session to `transport`.
 *
 * Returns ESP_ERR_INVALID_ARG for a NULL argument. Call
 * app_ble_gatt_native_set_radio() first if the platform has an arbiter; without it
 * the session still works and simply never takes a radio.
 *
 * The ops call the transport synchronously and are safe to call from the
 * application worker. gatt_cancel is the one entry the transport documents as safe
 * while an operation is outstanding, which is what lets the session's cancel path
 * work at all.
 */
esp_err_t app_ble_gatt_native_install(app_ble_gatt_native_t *adapter,
                                      esphome_ble_gatt_session_t *transport,
                                      app_ble_gatt_session_ops_t *out);

/*
 * Tell the adapter which platform pair to arbitrate with. Both hooks or neither: a
 * half-supplied pair would make acquire() succeed while the transport refuses to
 * initialise, and the pair must be idempotent for the same session (note 1 in this
 * header).
 *
 * This is the only place the platform hooks are supplied. The adapter adds them to
 * the transport config itself, in gatt_init (note 2).
 */
void app_ble_gatt_native_set_radio(app_ble_gatt_native_t *adapter,
                                   esphome_ble_gatt_radio_suspend_fn suspend,
                                   esphome_ble_gatt_radio_resume_fn resume, void *radio_ctx);

/*
 * The platform's monotonic millisecond clock. Exposed so a test and the adapter
 * cannot disagree about what "now" means.
 */
uint32_t app_ble_gatt_native_now_ms(void);

#ifdef __cplusplus
}
#endif
