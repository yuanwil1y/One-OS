#pragma once

/*
 * Firmware binding: app_ctl_ble (the control backend) -> app_ble_gatt (the session).
 *
 * app_ctl_ble owns the control policy - queue, write on the worker, confirm only
 * from the device's own report, fail on the deadline - and takes the session from an
 * ops table. This file is that table for the real firmware, so the policy module
 * stays platform independent and host-testable.
 *
 * WHAT ONLY THE FIRMWARE CAN ANSWER: WHICH CHARACTERISTIC
 *
 * A recognition recipe names a portable GATT characteristic INDEX
 * (`entity->write_target_id`, the same index `device_db_recipe_t::read_source_id`
 * carries for the read side). Turning that into a value handle needs the session's
 * discovered database and needs to be sure the control is meant for the peer that is
 * actually connected - writing characteristic 3 of the wrong device is how a bulb
 * gets driven instead of a lock. Both facts live in the session, so the resolution
 * belongs here rather than in the backend.
 *
 * WHY IT REFUSES RATHER THAN GUESSES
 *
 * Every failure below returns false, and the backend turns that into
 * APP_CONTROL_BACKEND_FAILED, which the loop reports as a failed control:
 *
 *   - no live session (nothing to write to);
 *   - the entity's device is not the open peer (this control is for another device);
 *   - the characteristic index is out of range or its value handle is zero;
 *   - the characteristic is not writable, which is checked here rather than letting
 *     the radio refuse, so the caller learns why.
 *
 * A wrong handle does not fail - it writes successfully to the wrong attribute.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ble_gatt.h"
#include "app_ctl_ble.h"
#include "app_device.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One adapter instance. The caller owns it: app_ctl_ble does not allocate, and the
 * session it points at must outlive it.
 */
typedef struct {
    app_ble_gatt_session_t *session;
    /* Every write this adapter attempted, and every resolution it refused, so a
     * failure on hardware can be told from a failure in the backend. */
    uint32_t writes_attempted;
    uint32_t resolve_refusals_not_open;
    uint32_t resolve_refusals_wrong_peer;
    uint32_t resolve_refusals_bad_index;
    uint32_t resolve_refusals_not_writable;
} app_ctl_ble_gatt_t;

/*
 * Fill the ops an app_ctl_ble_t needs, pointed at `session`.
 *
 * `out` is fully written on success. Returns ESP_ERR_INVALID_ARG for a NULL
 * argument; the session itself may be uninitialised, in which case is_ready() answers
 * false until it is.
 */
esp_err_t app_ctl_ble_gatt_install(app_ctl_ble_gatt_t *adapter, app_ble_gatt_session_t *session,
                                   app_ctl_ble_t *out_backend);

/*
 * Resolve a binding to a GATT value handle. Exposed for its own tests: the four
 * refusal reasons are the part of this adapter that must never be wrong, and they are
 * testable without a radio.
 */
bool app_ctl_ble_gatt_resolve(void *ctx, const app_entity_binding_t *entity,
                              const app_device_binding_t *device, uint16_t *out_value_handle);

/* Does this characteristic's property byte permit a write? Bit 2 is Write, bit 3 is
 * Write Without Response (Bluetooth GATT specification, Characteristic Properties). */
bool app_ctl_ble_gatt_property_is_writable(uint8_t properties);

#ifdef __cplusplus
}
#endif
