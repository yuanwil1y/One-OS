/*
 * The BLE GATT control backend. See app_ctl_ble.h for the contract.
 *
 * The shape of this file follows from one rule: send() must not block the
 * application worker on a radio, and a send must not become a state change. So
 * send() validates, encodes and queues; tick() - on the worker - performs the
 * write and enforces the confirmation deadline; a notification only records what
 * arrived. Nothing here writes the observed state.
 */

#include "app_ctl_ble.h"

#include <string.h>

#include "app_ble_gatt.h"
#include "app_str.h"

/* The backend's own id, used by app_control to route and to detect ambiguity. */
#define APP_CTL_BLE_BACKEND_ID 0x02u
#define APP_CTL_BLE_BACKEND_NAME "ble_gatt"

/* How long a written control may stay unconfirmed: see the header. */

static app_ctl_ble_op_t *slot_alloc(app_ctl_ble_t *self)
{
    for (size_t i = 0u; i < APP_CTL_BLE_MAX_INFLIGHT; ++i) {
        if (!self->ops[i].in_use) {
            return &self->ops[i];
        }
    }
    return NULL;
}

static app_ctl_ble_op_t *slot_for_handle(app_ctl_ble_t *self, uint16_t value_handle)
{
    for (size_t i = 0u; i < APP_CTL_BLE_MAX_INFLIGHT; ++i) {
        if (self->ops[i].in_use && self->ops[i].value_handle == value_handle) {
            return &self->ops[i];
        }
    }
    return NULL;
}

const char *app_ctl_ble_state_name(app_ctl_ble_state_t state)
{
    switch (state) {
    case APP_CTL_BLE_IDLE:                  return "idle";
    case APP_CTL_BLE_WRITING:               return "writing";
    case APP_CTL_BLE_AWAITING_CONFIRMATION: return "awaiting_confirmation";
    case APP_CTL_BLE_FAILED:                return "failed";
    default:                                return "invalid";
    }
}

size_t app_ctl_ble_inflight_count(const app_ctl_ble_t *self)
{
    size_t count = 0u;

    if (self == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < APP_CTL_BLE_MAX_INFLIGHT; ++i) {
        if (self->ops[i].in_use) {
            count++;
        }
    }
    return count;
}

void app_ctl_ble_reset(app_ctl_ble_t *self)
{
    if (self == NULL) {
        return;
    }
    /* The counters are deliberately kept: they describe the session that just
     * ended, and resetting them here would erase the evidence of what it did. */
    memset(self->ops, 0, sizeof(self->ops));
}

/* ------------------------------------------------------------------ */
/* claims: which entities this backend owns                            */
/* ------------------------------------------------------------------ */

static bool ble_claims(void *ctx, const app_entity_binding_t *entity)
{
    (void)ctx;

    if (entity == NULL) {
        return false;
    }
    /*
     * Only an entity that recognition resolved to a drivable BLE write target. The
     * loop has already checked writability and the device's availability; this is
     * the narrower question "is this MINE", and it must be conservative: claiming an
     * entity this backend cannot drive would make the loop route to it and fail
     * there instead of reporting the honest NO_BACKEND.
     */
    return entity->writable && entity->backend == DEVICE_DB_BACKEND_BLE_GATT;
}

/*
 * The state the device must report for this control to count as confirmed.
 *
 * Derived from the REQUEST, never from the device: a confirmation that read the
 * current device state and compared it with itself would confirm everything.
 * Returns false for actions whose success is not a state this firmware can name,
 * and such a control is reported as SENT-but-unconfirmable rather than guessed.
 */
static bool expected_state_for(const char *action, char *out, size_t out_size)
{
    if (action == NULL || out == NULL || out_size == 0u) {
        return false;
    }
    if (strcmp(action, "turn_on") == 0) {
        (void)app_strlcpy(out, HA_STATE_ON, out_size);
        return true;
    }
    if (strcmp(action, "turn_off") == 0) {
        (void)app_strlcpy(out, HA_STATE_OFF, out_size);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* send                                                                */
/* ------------------------------------------------------------------ */

/*
 * Find the request id for the control being routed.
 *
 * send() is called synchronously from app_control_submit() after the slot for this
 * request exists, so the pending slot for this entity is this request. It is still
 * verified rather than assumed: the entity id must match, and a mismatch means the
 * assumption does not hold and the control must be refused rather than answered
 * with another request's id.
 */
static bool current_request_id(const char *entity_id, uint32_t *out)
{
    for (size_t i = 0u; i < app_control_pending_count(); ++i) {
        const app_control_slot_t *slot = app_control_pending_at(i);

        if (slot == NULL || slot->state != APP_CONTROL_STATE_PENDING) {
            continue;
        }
        if (strcmp(slot->entity_id, entity_id) == 0) {
            *out = slot->request_id;
            return true;
        }
    }
    return false;
}

static app_control_backend_result_t ble_send(void *ctx, const char *entity_id, const char *action,
                                             const char *value)
{
    app_ctl_ble_t *self = (app_ctl_ble_t *)ctx;
    const app_entity_binding_t *entity;
    const app_device_binding_t *device;
    app_ctl_ble_op_t *op;
    uint8_t encoded[APP_BLE_VALUE_MAX];
    size_t encoded_len = 0u;
    uint16_t handle = 0u;
    app_ble_status_t status;
    uint32_t request_id = 0u;

    if (self == NULL || self->gatt.write == NULL || self->gatt.resolve == NULL ||
        self->gatt.is_ready == NULL || self->now_ms == NULL) {
        return APP_CONTROL_BACKEND_FAILED;
    }
    if (entity_id == NULL || action == NULL) {
        return APP_CONTROL_BACKEND_FAILED;
    }

    entity = app_control_lookup(entity_id);
    if (entity == NULL || !ble_claims(self, entity)) {
        return APP_CONTROL_BACKEND_UNSUPPORTED;
    }

    /*
     * No live link means no control. Reported as FAILED rather than SENT: a control
     * the caller is told left the firmware, which never left it, would sit pending
     * until its deadline and then be reported as a device timeout.
     */
    if (!self->gatt.is_ready(self->gatt_ctx)) {
        self->refused_not_ready++;
        return APP_CONTROL_BACKEND_FAILED;
    }

    device = app_device_find(entity->device_id);
    if (device == NULL || !self->gatt.resolve(self->gatt_ctx, entity, device, &handle)) {
        self->refused_no_handle++;
        return APP_CONTROL_BACKEND_FAILED;
    }
    if (handle == 0u) {
        self->refused_no_handle++;
        return APP_CONTROL_BACKEND_FAILED;
    }

    /*
     * The codec refuses a shape it does not implement, and a refusal here is the
     * honest answer: guessing an encoding is how a device gets written with garbage.
     * scale is passed through because a recipe expresses a tenths-of-a-degree
     * register that way.
     */
    status = app_ble_gatt_encode_action(action, value, (float)entity->scale, encoded,
                                        sizeof(encoded), &encoded_len);
    if (status != APP_BLE_OK) {
        return APP_CONTROL_BACKEND_UNSUPPORTED;
    }

    if (!current_request_id(entity_id, &request_id)) {
        /* The routing assumption did not hold. Refusing is the only safe answer: the
         * alternative is confirming whichever request happens to match. */
        self->refused_no_slot++;
        return APP_CONTROL_BACKEND_FAILED;
    }

    op = slot_alloc(self);
    if (op == NULL) {
        self->refused_no_slot++;
        return APP_CONTROL_BACKEND_FAILED;
    }

    memset(op, 0, sizeof(*op));
    op->in_use = true;
    op->state = APP_CTL_BLE_WRITING;
    op->request_id = request_id;
    (void)app_strlcpy(op->entity_id, entity_id, sizeof(op->entity_id));
    (void)app_strlcpy(op->device_id, entity->device_id, sizeof(op->device_id));
    (void)app_strlcpy(op->action, action, sizeof(op->action));
    op->value_handle = handle;
    op->deadline_ms = 0u; /* set by tick, once the write has actually happened */
    op->has_expect_state = expected_state_for(action, op->expect_state,
                                              sizeof(op->expect_state));

    /*
     * The bytes are handed to tick through the operation rather than written here:
     * a GATT write blocks for up to its operation timeout, and send() runs on the
     * application worker. Storing up to APP_BLE_VALUE_MAX bytes per slot is cheap
     * and keeps every decision on one task.
     */
    memcpy(op->pending_value, encoded, encoded_len);
    op->pending_value_len = encoded_len;

    return APP_CONTROL_BACKEND_SENT;
}

/* ------------------------------------------------------------------ */
/* tick: perform the write, then enforce the deadline                  */
/* ------------------------------------------------------------------ */

/*
 * Translate a decoded characteristic value into the vocabulary app_control uses.
 *
 * app_ble_gatt_decode_state() is a value codec: it renders a one-byte 0x01 as "1",
 * which for a numeric register is exactly right. app_control publishes HA_STATE_ON /
 * HA_STATE_OFF for a boolean, so the two vocabularies have to be mapped here rather
 * than compared directly. Comparing "1" with "on" is what this function exists to
 * prevent, and it was the first thing this test caught: the control stayed PENDING
 * while the device was reporting that it had obeyed.
 *
 * A value the mapping does not recognise is passed through unchanged, so a numeric
 * entity's reported value still matches a numeric expectation.
 */
static void normalise_reported_state(const char *decoded, char *out, size_t out_size)
{
    if (strcmp(decoded, "1") == 0) {
        (void)app_strlcpy(out, HA_STATE_ON, out_size);
    } else if (strcmp(decoded, "0") == 0) {
        (void)app_strlcpy(out, HA_STATE_OFF, out_size);
    } else {
        (void)app_strlcpy(out, decoded, out_size);
    }
}

/*
 * Does the buffered device report agree with what this control asked for?
 *
 * The bytes are decoded and compared with the state derived from the REQUEST, never
 * with the device's own previous state: a confirmation that compared the device
 * with itself would confirm everything. An action whose success is not a state this
 * firmware can name is never confirmed here, and its deadline is what ends it.
 */
static bool device_reported_expected(const app_ctl_ble_op_t *op)
{
    char decoded[APP_CONTROL_VALUE_MAX];
    char normalised[APP_CONTROL_VALUE_MAX];

    if (!op->has_reported_value || !op->has_expect_state) {
        return false;
    }
    if (app_ble_gatt_decode_state(op->reported_value, op->reported_value_len, 1.0f, decoded,
                                  sizeof(decoded)) != APP_BLE_OK) {
        return false;
    }
    normalise_reported_state(decoded, normalised, sizeof(normalised));
    return strcmp(normalised, op->expect_state) == 0;
}

bool app_ctl_ble_tick(app_ctl_ble_t *self)
{
    bool changed = false;
    uint32_t now;

    if (self == NULL || self->now_ms == NULL) {
        return false;
    }
    now = self->now_ms();

    for (size_t i = 0u; i < APP_CTL_BLE_MAX_INFLIGHT; ++i) {
        app_ctl_ble_op_t *op = &self->ops[i];

        if (!op->in_use) {
            continue;
        }

        if (op->state == APP_CTL_BLE_WRITING) {
            app_ble_status_t status;

            /* One write at a time: the transport is not a queue. The next slot is
             * picked up on the following tick. */
            for (size_t j = 0u; j < i; ++j) {
                if (self->ops[j].in_use && self->ops[j].state == APP_CTL_BLE_WRITING) {
                    goto next_slot;
                }
            }

            status = self->gatt.write(self->gatt_ctx, op->value_handle, op->pending_value,
                                      op->pending_value_len);
            if (status != APP_BLE_OK) {
                op->state = APP_CTL_BLE_FAILED;
                self->writes_failed++;
                (void)app_control_fail(op->request_id, "ble_write_failed");
                op->in_use = false;
                changed = true;
                continue;
            }

            self->writes_sent++;
            op->state = APP_CTL_BLE_AWAITING_CONFIRMATION;
            op->deadline_ms = now + APP_CTL_BLE_CONFIRM_TIMEOUT_MS;
            changed = true;

            /*
             * A control whose confirmation could arrive DURING the write is not
             * covered: the notification handler only records, and the check below
             * runs on the next tick. That is deliberate - confirming from the
             * notification task would move the observed state on a task that does
             * not own it.
             */
            continue;
        }

        if (op->state == APP_CTL_BLE_AWAITING_CONFIRMATION) {
            if (device_reported_expected(op)) {
                /* The device said so. This is the only path that moves the state,
                 * and it goes through app_control so the loop owns the decision. */
                if (app_control_confirm(op->request_id)) {
                    self->confirmations++;
                    op->in_use = false;
                    changed = true;
                }
                continue;
            }
            if ((int32_t)(now - op->deadline_ms) >= 0) {
                op->state = APP_CTL_BLE_FAILED;
                self->deadlines_expired++;
                (void)app_control_fail(op->request_id, "ble_confirm_timeout");
                op->in_use = false;
                changed = true;
            }
        }
next_slot:
        ;
    }

    return changed;
}

/* ------------------------------------------------------------------ */
/* notifications                                                       */
/* ------------------------------------------------------------------ */

void app_ctl_ble_on_notify(app_ctl_ble_t *self, uint16_t value_handle, const uint8_t *data,
                           size_t len, bool truncated)
{
    app_ctl_ble_op_t *op;

    if (self == NULL) {
        return;
    }
    /*
     * A truncated value is not a value: the device sent more than the session's
     * buffer holds, so the one field this backend reads cannot be trusted. Recording
     * it would confirm a control from half a reading.
     */
    if (truncated) {
        return;
    }
    if (data == NULL || len == 0u) {
        return;
    }

    op = slot_for_handle(self, value_handle);
    if (op == NULL) {
        /* A notification nobody is waiting for. Not an error: a subscribed
         * characteristic reports whenever the device feels like it, and this backend
         * only answers the control it issued. A spontaneous report is the caller's
         * business, through app_control_report(). */
        return;
    }

    /*
     * Buffered, not acted on. Two reasons: the decision belongs to tick() on the
     * worker, and app_control_report() would FAIL this pending control by design
     * ("the device's own report supersedes a request that has not been confirmed"),
     * which would destroy the control this notification is meant to confirm.
     */
    if (len > sizeof(op->reported_value)) {
        return;
    }
    memcpy(op->reported_value, data, len);
    op->reported_value_len = len;
    op->has_reported_value = true;
    self->notifications_seen++;
}

/* ------------------------------------------------------------------ */
/* the vtable                                                          */
/* ------------------------------------------------------------------ */

static const app_control_backend_ops_t s_ble_ops = {
    .backend_id = APP_CTL_BLE_BACKEND_ID,
    .name = APP_CTL_BLE_BACKEND_NAME,
    .claims = ble_claims,
    .send = ble_send,
};

const app_control_backend_ops_t *app_ctl_ble_backend_ops(void)
{
    return &s_ble_ops;
}
