#pragma once

/*
 * The BLE GATT control backend: the link between app_control and a GATT session.
 *
 * WHAT THIS MAKES REACHABLE
 *
 * Until this module existed, `app_backend_is_drivable()` answered false for every
 * control backend, so no entity in the corpus was writable and the control loop's
 * only honest answer was NO_BACKEND. Registering this backend is what makes a
 * recognised BLE entity actually drivable - see the note on that function, which
 * must be updated in the same change or the two decisions disagree.
 *
 * THE RULE IT MUST NOT BREAK
 *
 *   A successful send is not a state change.
 *
 * `send` writes to the characteristic and returns SENT. It does not touch the
 * observed state, and it does not call app_control_confirm(): the state moves only
 * when the device answers - a notification on a subscribed characteristic, or a
 * read-back that agrees. A backend that confirmed its own write would make
 * "confirmed" mean "we asked", which is the defect app_control exists to prevent.
 *
 * WHY `send` CAN GET THE REQUEST ID
 *
 * app_control_backend_ops_t::send receives no request id, and confirm() takes
 * nothing else. The id is not actually missing, though: send() is called
 * SYNCHRONOUSLY from inside app_control_submit(), after the slot for this request
 * has been created and registered (app_control.c: slot->request_id is set before
 * the send). So at send time the pending slot for this entity IS this request, and
 * the backend can read its id from app_control_pending_at() - no vtable change and
 * no guess required. The lookup is still verified (entity_id must match) rather
 * than assumed, because a backend that confirmed the wrong request would be worse
 * than one that refuses.
 *
 * THREADING
 *
 * `send` runs on the application worker. `notify` runs on the GATT backend's own
 * task (the NimBLE host task), so it must not call back into the session: it only
 * records what arrived. `tick` runs on the application worker and is where every
 * decision is taken - the operation is started there, and the deadline is enforced
 * there.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_ble_gatt.h"
#include "app_control.h"
#include "app_device.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long a written control may stay unconfirmed. Longer than the GATT operation
 * timeout on purpose: the write may succeed and the device's report may legitimately
 * take a moment, and failing a control the device did obey would be worse than
 * waiting. Exposed so a test can cross the deadline without guessing the value. */
#define APP_CTL_BLE_CONFIRM_TIMEOUT_MS 15000u

/* One in-flight write. One is enough: app_control refuses a second control for an
 * entity while the first is pending, and a single GATT session is one peer. */
#define APP_CTL_BLE_MAX_INFLIGHT 2u

typedef enum {
    APP_CTL_BLE_IDLE = 0,
    APP_CTL_BLE_WRITING,
    APP_CTL_BLE_AWAITING_CONFIRMATION,
    APP_CTL_BLE_FAILED,
} app_ctl_ble_state_t;

typedef struct {
    bool in_use;
    app_ctl_ble_state_t state;
    uint32_t request_id;
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char device_id[HA_CORE_ID_LEN];
    char action[HA_CORE_SERVICE_LEN];
    /* The value the device must report for this control to be considered
     * confirmed. Recorded from the request, not from the device. */
    char expect_state[HA_CORE_STATE_LEN];
    bool has_expect_state;
    uint16_t value_handle;
    uint32_t deadline_ms;
    /* The bytes the write will send, and the last value the device reported for this
     * characteristic. The notification is BUFFERED rather than handed to
     * app_control_report(): that call fails any control pending on the entity by
     * design - "the device's own report supersedes a request that has not been
     * confirmed" - so using it here would destroy the very control it is meant to
     * confirm. The worker compares the buffered bytes and calls confirm(). */
    uint8_t pending_value[APP_BLE_VALUE_MAX];
    size_t pending_value_len;
    uint8_t reported_value[APP_BLE_VALUE_MAX];
    size_t reported_value_len;
    bool has_reported_value;
} app_ctl_ble_op_t;

typedef struct {
    /* Ops the caller supplies. `gatt` is the session's public API, so the host can
     * drive this with a scripted one and the firmware with app_ble_gatt_native. */
    struct {
        int (*write)(void *ctx, uint16_t handle, const uint8_t *data, size_t len);
        app_ble_status_t (*open)(void *ctx, const app_ble_peer_t *peer);
        app_ble_status_t (*read)(void *ctx, uint16_t handle, uint8_t *out, size_t cap,
                                 size_t *out_len);
        bool (*is_ready)(void *ctx);
        /*
         * Resolve an entity's recipe to the GATT value handle to write.
         *
         * The firmware supplies this and it is the last piece of the B7 chain that
         * is not written yet. What it has to do, from the fields that already exist:
         *
         *   - the device binding carries the BLE display-order address, which must
         *     equal the address of the OPEN peer, or this control is for a different
         *     device than the session is connected to (and writing it would drive the
         *     wrong peripheral);
         *   - `entity->write_target_id` is the recipe's GATT characteristic index
         *     (`device_db_recipe_t::read_source_id` is the same index for the read
         *     side), and `app_ble_gatt_resolve()` turns a characteristic index into a
         *     value handle over the session's discovered database - so the
         *     subscription/write handle is a lookup, not a second discovery;
         *   - the characteristic must be writable, or the control is refused here
         *     rather than at the radio.
         *
         * It returns false rather than guessing: a wrong handle writes to whatever
         * characteristic happens to sit at that offset.
         */
        bool (*resolve)(void *ctx, const app_entity_binding_t *entity,
                        const app_device_binding_t *device, uint16_t *out_value_handle);
    } gatt;
    void *gatt_ctx;
    uint32_t (*now_ms)(void);

    app_ctl_ble_op_t ops[APP_CTL_BLE_MAX_INFLIGHT];
    uint32_t writes_sent;
    uint32_t writes_failed;
    uint32_t confirmations;
    uint32_t notifications_seen;
    uint32_t deadlines_expired;
    /* Refusals by reason, for diagnostics. */
    uint32_t refused_not_ready;
    uint32_t refused_no_handle;
    uint32_t refused_no_slot;
} app_ctl_ble_t;

typedef struct {
    void *ctx;
    uint32_t (*now_ms)(void);
} app_ctl_ble_config_t;

/*
 * The vtable entry to hand app_control_register_backend().
 *
 * `ctx` must be an app_ctl_ble_t whose `gatt` ops and `now_ms` are filled in.
 */
const app_control_backend_ops_t *app_ctl_ble_backend_ops(void);

/*
 * Advance the backend: start a queued write, and expire one whose confirmation
 * deadline has passed. Run from the application worker.
 *
 * An expired operation is reported through app_control_fail(), which restores the
 * previously confirmed state. It is NOT silently dropped, because a control the
 * caller was told is pending must always reach a terminal answer.
 *
 * Returns true when anything changed, so the caller logs it once.
 */
bool app_ctl_ble_tick(app_ctl_ble_t *self);

/*
 * A notification arrived for `value_handle` with these bytes.
 *
 * Called from the GATT task. Records the value; the decision to confirm is taken in
 * app_ctl_ble_tick(), because that is the worker's job and because confirming from
 * the notification task would make the observed state move on a task that does not
 * own it.
 */
void app_ctl_ble_on_notify(app_ctl_ble_t *self, uint16_t value_handle, const uint8_t *data,
                           size_t len, bool truncated);

/* Diagnostics. Never includes a secret: these are counters and a state name. */
const char *app_ctl_ble_state_name(app_ctl_ble_state_t state);
size_t app_ctl_ble_inflight_count(const app_ctl_ble_t *self);

/*
 * Reset for a test or a session switch. Clears the in-flight operations WITHOUT
 * confirming them: a control that was in flight when the session went away is
 * reported through app_control_reconcile(), not answered here.
 */
void app_ctl_ble_reset(app_ctl_ble_t *self);

#ifdef __cplusplus
}
#endif
