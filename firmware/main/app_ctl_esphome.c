/*
 * The ESPHome Native API control backend. See app_ctl_esphome.h for the contract and
 * for why the command needs two identifiers.
 *
 * The same shape as app_ctl_ble, for the same reasons: send() must not block the worker
 * on a network round trip, and a send must not become a state change.
 */

#include "app_ctl_esphome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_str.h"

#define APP_CTL_ESPHOME_BACKEND_ID 0x03u
#define APP_CTL_ESPHOME_BACKEND_NAME "esphome_api"

static app_ctl_esphome_op_t *slot_alloc(app_ctl_esphome_t *self)
{
    for (size_t i = 0u; i < APP_CTL_ESPHOME_MAX_INFLIGHT; ++i) {
        if (!self->ops[i].in_use) {
            return &self->ops[i];
        }
    }
    return NULL;
}

static app_ctl_esphome_op_t *slot_for_key(app_ctl_esphome_t *self, uint32_t key)
{
    for (size_t i = 0u; i < APP_CTL_ESPHOME_MAX_INFLIGHT; ++i) {
        if (self->ops[i].in_use && self->ops[i].key == key) {
            return &self->ops[i];
        }
    }
    return NULL;
}

const char *app_ctl_esphome_state_name(app_ctl_esphome_state_t state)
{
    switch (state) {
    case APP_CTL_ESPHOME_IDLE:                  return "idle";
    case APP_CTL_ESPHOME_SENDING:               return "sending";
    case APP_CTL_ESPHOME_AWAITING_CONFIRMATION: return "awaiting_confirmation";
    case APP_CTL_ESPHOME_FAILED:                return "failed";
    default:                                    return "invalid";
    }
}

size_t app_ctl_esphome_inflight_count(const app_ctl_esphome_t *self)
{
    size_t count = 0u;

    if (self == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < APP_CTL_ESPHOME_MAX_INFLIGHT; ++i) {
        if (self->ops[i].in_use) {
            count++;
        }
    }
    return count;
}

void app_ctl_esphome_reset(app_ctl_esphome_t *self)
{
    if (self == NULL) {
        return;
    }
    memset(self->ops, 0, sizeof(self->ops));
}

/* ------------------------------------------------------------------ */
/* claims                                                              */
/* ------------------------------------------------------------------ */

static bool esphome_claims(void *ctx, const app_entity_binding_t *entity)
{
    (void)ctx;

    if (entity == NULL) {
        return false;
    }
    return entity->writable && entity->backend == DEVICE_DB_BACKEND_ESPHOME_API;
}

/* ------------------------------------------------------------------ */
/* action -> command                                                   */
/* ------------------------------------------------------------------ */

/* The app's entity domain decides which ESPHome command a control becomes. The domain
 * is already resolved into the binding, so this is a mapping and not a second
 * classification of the device. */
static bool kind_for_domain(const char *domain, esphome_api_command_kind_t *out)
{
    if (domain == NULL || out == NULL) {
        return false;
    }
    if (strcmp(domain, "switch") == 0) {
        *out = ESPHOME_API_COMMAND_SWITCH;
        return true;
    }
    if (strcmp(domain, "light") == 0) {
        *out = ESPHOME_API_COMMAND_LIGHT;
        return true;
    }
    if (strcmp(domain, "button") == 0) {
        *out = ESPHOME_API_COMMAND_BUTTON;
        return true;
    }
    if (strcmp(domain, "number") == 0) {
        *out = ESPHOME_API_COMMAND_NUMBER;
        return true;
    }
    if (strcmp(domain, "select") == 0) {
        *out = ESPHOME_API_COMMAND_SELECT;
        return true;
    }
    if (strcmp(domain, "lock") == 0) {
        *out = ESPHOME_API_COMMAND_LOCK;
        return true;
    }
    return false;
}

/*
 * A scalar parsed the way the codec parses one: strtof over the whole string.
 *
 * A value the device would not read the same way must not be accepted as "close
 * enough": the confirmation below compares what the device reports with what was sent,
 * so the two have to agree on what was sent.
 */
static bool parse_scalar(const char *text, float *out)
{
    char *end = NULL;
    float parsed;

    if (text == NULL || out == NULL || text[0] == '\0') {
        return false;
    }
    parsed = strtof(text, &end);
    if (end == text || (end != NULL && *end != '\0')) {
        return false;
    }
    *out = parsed;
    return true;
}

/*
 * The state the device must report for this control to count as confirmed, derived from
 * the REQUEST.
 *
 * A number entity's state is a float, and ESPHome reports the value it was given, so the
 * expectation is that value in the same decimal form this firmware sends. Again the
 * comparison is text, so both sides have to be canonicalised the same way - getting this
 * wrong is a false negative in which a device that obeyed is reported as silent.
 */
static void expected_state_for(const char *action, const char *value, char *out, size_t out_size)
{
    if (action == NULL || out == NULL || out_size == 0u) {
        return;
    }
    out[0] = '\0';
    if (strcmp(action, "turn_on") == 0) {
        (void)app_strlcpy(out, HA_STATE_ON, out_size);
        return;
    }
    if (strcmp(action, "turn_off") == 0) {
        (void)app_strlcpy(out, HA_STATE_OFF, out_size);
        return;
    }
    if (value == NULL || value[0] == '\0') {
        /* press, and anything else without a value: no state to expect. */
        return;
    }
    if (strcmp(action, "set_text") == 0 || strcmp(action, "select_option") == 0) {
        (void)app_strlcpy(out, value, out_size);
        return;
    }
    if (strcmp(action, "set_value") == 0) {
        float number;

        if (!parse_scalar(value, &number)) {
            return;
        }
        (void)snprintf(out, out_size, "%g", (double)number);
    }
}

/* ------------------------------------------------------------------ */
/* send                                                                */
/* ------------------------------------------------------------------ */

/*
 * The request id for the control being routed.
 *
 * send() is called synchronously from app_control_submit() after the slot for this
 * request exists, so the pending slot for this entity IS this request. Verified rather
 * than assumed: a mismatch means the assumption does not hold, and answering with
 * another request's id would be worse than refusing.
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

static app_control_backend_result_t esphome_send(void *ctx, const char *entity_id,
                                                 const char *action, const char *value)
{
    app_ctl_esphome_t *self = (app_ctl_esphome_t *)ctx;
    const app_entity_binding_t *entity;
    const esphome_api_entity_t *discovered;
    esphome_api_command_kind_t kind;
    app_ctl_esphome_op_t *op;
    uint32_t request_id = 0u;

    if (self == NULL || self->api.send == NULL || self->api.is_ready == NULL ||
        self->api.find_entity == NULL || self->now_ms == NULL) {
        return APP_CONTROL_BACKEND_FAILED;
    }
    if (entity_id == NULL || action == NULL) {
        return APP_CONTROL_BACKEND_FAILED;
    }

    entity = app_control_lookup(entity_id);
    if (entity == NULL || !esphome_claims(self, entity)) {
        return APP_CONTROL_BACKEND_UNSUPPORTED;
    }

    if (!self->api.is_ready(self->api_ctx)) {
        /* No live session. FAILED, not SENT: a control the caller is told left the
         * firmware which never left it sits pending until its deadline and is then
         * reported as a device timeout. */
        self->refused_not_ready++;
        return APP_CONTROL_BACKEND_FAILED;
    }

    /*
     * The key is the corpus's portable name for the entity; the device id comes back
     * with the discovered entity, because it is a fact the device reports about itself.
     */
    discovered = self->api.find_entity(self->api_ctx, entity->write_target_id);
    if (discovered == NULL) {
        /* Either the device does not expose this entity any more, or it was never
         * discovered. Both mean there is nothing to command. */
        self->refused_unknown_key++;
        return APP_CONTROL_BACKEND_FAILED;
    }

    if (!kind_for_domain(entity->domain, &kind)) {
        self->refused_unsupported_kind++;
        return APP_CONTROL_BACKEND_UNSUPPORTED;
    }

    op = slot_alloc(self);
    if (op == NULL) {
        return APP_CONTROL_BACKEND_FAILED;
    }
    if (!current_request_id(entity_id, &request_id)) {
        self->refused_unknown_key++;
        return APP_CONTROL_BACKEND_FAILED;
    }

    memset(op, 0, sizeof(*op));
    op->in_use = true;
    op->state = APP_CTL_ESPHOME_SENDING;
    op->request_id = request_id;
    op->key = entity->write_target_id;
    (void)app_strlcpy(op->entity_id, entity_id, sizeof(op->entity_id));
    (void)app_strlcpy(op->device_id, entity->device_id, sizeof(op->device_id));
    (void)app_strlcpy(op->action, action, sizeof(op->action));
    expected_state_for(action, value, op->expect_state, sizeof(op->expect_state));
    op->has_expect_state = op->expect_state[0] != '\0';

    op->command.kind = kind;
    op->command.key = discovered->key;
    op->command.device_id = discovered->device_id;

    /*
     * The action-to-field mapping, and every refusal is deliberate: a command built
     * from a guess writes the wrong field of the right entity.
     */
    if (strcmp(action, "turn_on") == 0 || strcmp(action, "turn_off") == 0) {
        if (kind != ESPHOME_API_COMMAND_SWITCH && kind != ESPHOME_API_COMMAND_LIGHT) {
            self->refused_unsupported_action++;
            op->in_use = false;
            return APP_CONTROL_BACKEND_UNSUPPORTED;
        }
        if (kind == ESPHOME_API_COMMAND_SWITCH) {
            op->command.value.switch_.state = strcmp(action, "turn_on") == 0;
        } else {
            op->command.value.light.has_state = true;
            op->command.value.light.state = strcmp(action, "turn_on") == 0;
        }
    } else if (strcmp(action, "press") == 0) {
        if (kind != ESPHOME_API_COMMAND_BUTTON) {
            self->refused_unsupported_action++;
            op->in_use = false;
            return APP_CONTROL_BACKEND_UNSUPPORTED;
        }
        /* A button command carries only the key: the press IS the command. */
    } else if (strcmp(action, "set_value") == 0) {
        float number;

        if (kind != ESPHOME_API_COMMAND_NUMBER || !parse_scalar(value, &number)) {
            self->refused_unsupported_action++;
            op->in_use = false;
            return APP_CONTROL_BACKEND_UNSUPPORTED;
        }
        op->command.value.number.state = number;
    } else if (strcmp(action, "set_text") == 0 || strcmp(action, "select_option") == 0) {
        if (kind != ESPHOME_API_COMMAND_SELECT || value == NULL || value[0] == '\0') {
            self->refused_unsupported_action++;
            op->in_use = false;
            return APP_CONTROL_BACKEND_UNSUPPORTED;
        }
        op->command.value.select.state = value;
    } else {
        /* An action with no ESPHome equivalent in this firmware. Guessing one is how a
         * device gets commanded to do something nobody asked for. */
        self->refused_unsupported_action++;
        op->in_use = false;
        return APP_CONTROL_BACKEND_UNSUPPORTED;
    }

    return APP_CONTROL_BACKEND_SENT;
}

/* ------------------------------------------------------------------ */
/* tick                                                                */
/* ------------------------------------------------------------------ */

/*
 * Normalise a reported state into the vocabulary app_control publishes.
 *
 * A binary sensor or switch reports a boolean, which app_control names on/off; a number
 * reports a float, which it publishes as the number. The two have to be compared in one
 * vocabulary, and which one applies follows from what arrived.
 */
static void normalise_reported(const app_ctl_esphome_op_t *op, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    out[0] = '\0';
    if (op->command.kind == ESPHOME_API_COMMAND_SWITCH ||
        op->command.kind == ESPHOME_API_COMMAND_BUTTON) {
        (void)app_strlcpy(out, op->reported_boolean ? HA_STATE_ON : HA_STATE_OFF, out_size);
        return;
    }
    if (op->command.kind == ESPHOME_API_COMMAND_NUMBER) {
        (void)snprintf(out, out_size, "%g", (double)op->reported_number);
        return;
    }
    (void)app_strlcpy(out, op->reported_text, out_size);
}

static bool device_reported_expected(const app_ctl_esphome_op_t *op)
{
    char observed[HA_CORE_STATE_LEN];

    if (!op->has_reported_state || !op->has_expect_state) {
        return false;
    }
    normalise_reported(op, observed, sizeof(observed));
    return strcmp(observed, op->expect_state) == 0;
}

bool app_ctl_esphome_tick(app_ctl_esphome_t *self)
{
    bool changed = false;
    uint32_t now;

    if (self == NULL || self->now_ms == NULL) {
        return false;
    }
    now = self->now_ms();

    for (size_t i = 0u; i < APP_CTL_ESPHOME_MAX_INFLIGHT; ++i) {
        app_ctl_esphome_op_t *op = &self->ops[i];

        if (!op->in_use) {
            continue;
        }

        if (op->state == APP_CTL_ESPHOME_SENDING) {
            esp_err_t err;

            /* One command at a time: the client is not a queue. The next slot is picked
             * up on the following tick. */
            {
                bool busy = false;

                for (size_t j = 0u; j < i; ++j) {
                    if (self->ops[j].in_use && self->ops[j].state == APP_CTL_ESPHOME_SENDING) {
                        busy = true;
                        break;
                    }
                }
                if (busy) {
                    continue;
                }
            }

            err = self->api.send(self->api_ctx, &op->command);
            if (err != ESP_OK) {
                op->state = APP_CTL_ESPHOME_FAILED;
                self->commands_failed++;
                (void)app_control_fail(op->request_id, "esphome_send_failed");
                op->in_use = false;
                changed = true;
                continue;
            }

            self->commands_sent++;
            op->state = APP_CTL_ESPHOME_AWAITING_CONFIRMATION;
            op->deadline_ms = now + APP_CTL_ESPHOME_CONFIRM_TIMEOUT_MS;
            changed = true;
            continue;
        }

        if (op->state == APP_CTL_ESPHOME_AWAITING_CONFIRMATION) {
            if (device_reported_expected(op)) {
                if (app_control_confirm(op->request_id)) {
                    self->confirmations++;
                    op->in_use = false;
                    changed = true;
                }
                continue;
            }
            if ((int32_t)(now - op->deadline_ms) >= 0) {
                op->state = APP_CTL_ESPHOME_FAILED;
                self->deadlines_expired++;
                /* Two endings the caller must be able to tell apart: an action with no
                 * derivable state was never going to confirm, so blaming the device
                 * would report the firmware's own limit as the device's fault. */
                (void)app_control_fail(op->request_id, op->has_expect_state
                                                          ? "esphome_confirm_timeout"
                                                          : "esphome_confirm_unavailable");
                op->in_use = false;
                changed = true;
            }
        }
    }

    return changed;
}

/* ------------------------------------------------------------------ */
/* state reports                                                       */
/* ------------------------------------------------------------------ */

void app_ctl_esphome_on_state(app_ctl_esphome_t *self, const esphome_api_state_t *state)
{
    app_ctl_esphome_op_t *op;

    if (self == NULL || state == NULL) {
        return;
    }
    op = slot_for_key(self, state->key);
    if (op == NULL) {
        /* A report for an entity nobody is controlling. Not an error: a subscribed
         * entity reports whenever it changes, and the observed state for a passive
         * entity is the application's business through app_control_report(). */
        return;
    }

    /*
     * `missing` means the device does not have a value for the entity right now. That is
     * not a state, so it must not confirm anything: treating "no value" as a value would
     * confirm a control from an absence, and treating the LAST value as current would
     * confirm from stale data.
     */
    if (state->missing) {
        return;
    }

    /* Buffered, not acted on: the decision belongs to tick() on the worker, and
     * app_control_report() would fail this pending control by design. */
    switch (state->kind) {
    case ESPHOME_API_ENTITY_BINARY_SENSOR:
    case ESPHOME_API_ENTITY_SWITCH:
        op->reported_boolean = state->value.boolean;
        break;
    case ESPHOME_API_ENTITY_SENSOR:
    case ESPHOME_API_ENTITY_NUMBER:
        op->reported_number = state->value.number;
        break;
    case ESPHOME_API_ENTITY_TEXT_SENSOR:
    case ESPHOME_API_ENTITY_SELECT:
        (void)app_strlcpy(op->reported_text, state->value.text, sizeof(op->reported_text));
        break;
    default:
        /* A kind this backend cannot interpret. Recording it would compare a value it
         * did not read. */
        return;
    }
    op->has_reported_state = true;
    self->states_seen++;
}

/* ------------------------------------------------------------------ */
/* the vtable                                                          */
/* ------------------------------------------------------------------ */

static const app_control_backend_ops_t s_esphome_ops = {
    .backend_id = APP_CTL_ESPHOME_BACKEND_ID,
    .name = APP_CTL_ESPHOME_BACKEND_NAME,
    .claims = esphome_claims,
    .send = esphome_send,
};

const app_control_backend_ops_t *app_ctl_esphome_backend_ops(void)
{
    return &s_esphome_ops;
}
