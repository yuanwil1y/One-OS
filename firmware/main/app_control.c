/*
 * Unified control loop. See app_control.h for the contract and for the checks the
 * module performs before any backend is touched.
 *
 * Platform independent: no clocks, no radios, no FreeRTOS. The caller supplies the
 * time, and backends arrive through a vtable, so every ordering this module enforces
 * is testable on the host - which matters because the failures it prevents (a state
 * that was never confirmed, a late confirmation resurrecting a finished request, a
 * device swept mid-control) are exactly the ones a board makes hard to reproduce.
 */

#include "app_control.h"

#include "app_str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- storage ---------------- */

typedef struct {
    bool in_use;
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char state[HA_CORE_STATE_LEN];
} observed_t;

static app_control_backend_t s_backends[4];
static size_t s_backend_count;

static app_control_slot_t s_pending[APP_CONTROL_MAX_PENDING];
static observed_t s_observed[APP_CONTROL_MAX_ENTITIES];

const char *app_control_status_name(app_control_status_t status)
{
    switch (status) {
    case APP_CONTROL_OK:                    return "ok";
    case APP_CONTROL_ERR_INVALID_ARGUMENT:  return "invalid_argument";
    case APP_CONTROL_ERR_UNKNOWN_ENTITY:    return "unknown_entity";
    case APP_CONTROL_ERR_NOT_WRITABLE:      return "not_writable";
    case APP_CONTROL_ERR_UNSUPPORTED:       return "unsupported_action";
    case APP_CONTROL_ERR_OUT_OF_RANGE:      return "out_of_range";
    case APP_CONTROL_ERR_NO_BACKEND:        return "no_backend";
    case APP_CONTROL_ERR_AMBIGUOUS_BACKEND: return "ambiguous_backend";
    case APP_CONTROL_ERR_DEVICE_UNAVAILABLE:return "device_unavailable";
    case APP_CONTROL_ERR_BUSY:              return "busy";
    case APP_CONTROL_ERR_NO_CAPACITY:       return "no_capacity";
    case APP_CONTROL_ERR_BACKEND_FAILED:    return "backend_failed";
    default:                                return "unknown";
    }
}

const char *app_control_state_name(app_control_state_t state)
{
    switch (state) {
    case APP_CONTROL_STATE_PENDING:   return "pending";
    case APP_CONTROL_STATE_CONFIRMED: return "confirmed";
    case APP_CONTROL_STATE_FAILED:    return "failed";
    case APP_CONTROL_STATE_TIMEOUT:   return "timeout";
    case APP_CONTROL_STATE_CANCELED:  return "canceled";
    default:                          return "invalid";
    }
}

void app_control_reset(void)
{
    memset(s_backends, 0, sizeof(s_backends));
    s_backend_count = 0u;
    memset(s_pending, 0, sizeof(s_pending));
    memset(s_observed, 0, sizeof(s_observed));
}

esp_err_t app_control_register_backend(const app_control_backend_t *backend)
{
    if (backend == NULL || backend->ops == NULL || backend->ops->send == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < s_backend_count; ++i) {
        if (s_backends[i].ops->backend_id == backend->ops->backend_id) {
            s_backends[i] = *backend;
            return ESP_OK;
        }
    }
    if (s_backend_count >= sizeof(s_backends) / sizeof(s_backends[0])) {
        return ESP_ERR_NO_MEM;
    }
    s_backends[s_backend_count++] = *backend;
    return ESP_OK;
}

size_t app_control_backend_count(void)
{
    return s_backend_count;
}

const app_entity_binding_t *app_control_lookup(const char *entity_id)
{
    if (entity_id == NULL || entity_id[0] == '\0') {
        return NULL;
    }
    return app_entity_find(entity_id);
}

/* ---------------- observed state ---------------- */

static observed_t *observed_find(const char *entity_id, bool create)
{
    observed_t *free_slot = NULL;

    if (entity_id == NULL || entity_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0u; i < APP_CONTROL_MAX_ENTITIES; ++i) {
        if (s_observed[i].in_use) {
            if (strcmp(s_observed[i].entity_id, entity_id) == 0) {
                return &s_observed[i];
            }
        } else if (free_slot == NULL) {
            free_slot = &s_observed[i];
        }
    }
    if (!create || free_slot == NULL) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = true;
    (void)app_strlcpy(free_slot->entity_id, entity_id, sizeof(free_slot->entity_id));
    return free_slot;
}

const char *app_control_observed_state(const char *entity_id)
{
    const observed_t *found = observed_find(entity_id, false);

    return found == NULL ? NULL : found->state;
}

/*
 * Publish a state to ha_core and to the observed table.
 *
 * One function, so "what the entity shows" and "what the loop believes" cannot drift.
 */
static void observed_set(const char *entity_id, const char *state)
{
    observed_t *slot = observed_find(entity_id, true);

    if (state == NULL) {
        return;
    }
    if (slot != NULL) {
        (void)app_strlcpy(slot->state, state, sizeof(slot->state));
    }
    (void)ha_core_state_set(entity_id, state, NULL, 0u);
}

/* ---------------- service checks ---------------- */

bool app_control_service_is_supported(const app_entity_binding_t *entity,
                                      const char *action)
{
    const ha_entity_t *ha;

    if (entity == NULL || action == NULL) {
        return false;
    }
    /*
     * The authority is the HA entity's own service mask, not the binding: the mask is
     * what a dispatcher elsewhere in the system would consult, and a second list here
     * would be a second answer to the same question.
     */
    ha = ha_core_entity_get(entity->entity_id);
    if (ha == NULL) {
        return false;
    }
    if (strcmp(action, HA_SERVICE_TURN_ON) == 0) {
        return (ha->supported_services & HA_SERVICE_MASK_TURN_ON) != 0u;
    }
    if (strcmp(action, HA_SERVICE_TURN_OFF) == 0) {
        return (ha->supported_services & HA_SERVICE_MASK_TURN_OFF) != 0u;
    }
    if (strcmp(action, HA_SERVICE_PRESS) == 0) {
        return (ha->supported_services & HA_SERVICE_MASK_PRESS) != 0u;
    }
    if (strcmp(action, HA_SERVICE_SET_VALUE) == 0) {
        return (ha->supported_services & HA_SERVICE_MASK_SET_VALUE) != 0u;
    }
    if (strcmp(action, HA_SERVICE_SELECT_OPTION) == 0) {
        return (ha->supported_services & HA_SERVICE_MASK_SELECT_OPTION) != 0u;
    }
    if (strcmp(action, HA_SERVICE_SET_TEMPERATURE) == 0) {
        return (ha->supported_services & HA_SERVICE_MASK_SET_TEMPERATURE) != 0u;
    }
    return false;
}

/*
 * Does this action carry a value?
 *
 * Stated once. `turn_on`/`turn_off`/`press` do not; the rest do, and a request that
 * omits a required value is refused rather than sent with an empty one.
 */
static bool action_takes_value(const char *action)
{
    return strcmp(action, HA_SERVICE_SET_VALUE) == 0 ||
           strcmp(action, HA_SERVICE_SELECT_OPTION) == 0 ||
           strcmp(action, HA_SERVICE_SET_TEMPERATURE) == 0;
}

/*
 * Parse a numeric value strictly.
 *
 * strtol with end-pointer checking rather than atoi: atoi maps every unparseable
 * string to zero, so "abc" would silently become a request to set zero - which on a
 * light is "off" and on a thermostat is a real temperature.
 */
static bool parse_i32(const char *text, int32_t *out)
{
    char *end = NULL;
    long parsed;

    if (text == NULL || text[0] == '\0' || out == NULL) {
        return false;
    }
    parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    if (parsed < (long)INT32_MIN || parsed > (long)INT32_MAX) {
        return false;
    }
    *out = (int32_t)parsed;
    return true;
}

/* ---------------- routing ---------------- */

/*
 * Find the single backend that owns this entity.
 *
 * Returns APP_CONTROL_OK and fills `*out` when exactly one claims it. Zero owners is
 * NO_BACKEND and more than one is AMBIGUOUS_BACKEND: a second claim means the binding
 * is not unique, and dispatching anyway would be guessing which protocol stack should
 * carry the command.
 */
static app_control_status_t route_backend(const app_entity_binding_t *entity,
                                          app_control_backend_t *out)
{
    size_t claims = 0u;

    for (size_t i = 0u; i < s_backend_count; ++i) {
        const app_control_backend_ops_t *ops = s_backends[i].ops;

        if (ops->claims != NULL && !ops->claims(s_backends[i].ctx, entity)) {
            continue;
        }
        if (ops->claims == NULL) {
            /* A backend with no claims() claims nothing. Treating "no predicate" as
             * "everything" would make every entity ambiguous the moment a second
             * backend registered. */
            continue;
        }
        claims++;
        if (claims == 1u) {
            *out = s_backends[i];
        }
    }
    if (claims == 0u) {
        return APP_CONTROL_ERR_NO_BACKEND;
    }
    if (claims > 1u) {
        return APP_CONTROL_ERR_AMBIGUOUS_BACKEND;
    }
    return APP_CONTROL_OK;
}

/* ---------------- admission ---------------- */

static app_control_slot_t *pending_find(uint32_t request_id)
{
    for (size_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        if (s_pending[i].in_use && s_pending[i].request_id == request_id) {
            return &s_pending[i];
        }
    }
    return NULL;
}

static app_control_slot_t *pending_for_entity(const char *entity_id)
{
    for (size_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        if (s_pending[i].in_use &&
            strcmp(s_pending[i].entity_id, entity_id) == 0) {
            return &s_pending[i];
        }
    }
    return NULL;
}

static app_control_slot_t *pending_alloc(void)
{
    for (size_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        if (!s_pending[i].in_use) {
            memset(&s_pending[i], 0, sizeof(s_pending[i]));
            s_pending[i].in_use = true;
            return &s_pending[i];
        }
    }
    return NULL;
}

static void pending_release(app_control_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
}

app_control_status_t app_control_submit(const char *entity_id, const char *action,
                                        const char *value, uint32_t request_id,
                                        uint32_t now_ms, uint32_t timeout_ms,
                                        app_control_slot_t **out_slot)
{
    const app_entity_binding_t *entity;
    const app_device_binding_t *device;
    app_control_backend_t backend;
    app_control_status_t route;
    app_control_slot_t *slot;
    int32_t numeric = 0;

    if (out_slot != NULL) {
        *out_slot = NULL;
    }
    if (entity_id == NULL || entity_id[0] == '\0' || action == NULL ||
        action[0] == '\0') {
        return APP_CONTROL_ERR_INVALID_ARGUMENT;
    }

    /* 1. the entity exists */
    entity = app_control_lookup(entity_id);
    if (entity == NULL) {
        return APP_CONTROL_ERR_UNKNOWN_ENTITY;
    }

    /* 2. the owning device is known and online */
    device = app_device_find(entity->device_id);
    if (device == NULL) {
        /* The binding exists but its device does not: the tables disagree, which is a
         * defect, and sending a command into that state is not a repair. */
        return APP_CONTROL_ERR_DEVICE_UNAVAILABLE;
    }
    if (device->availability != APP_AVAILABILITY_ONLINE) {
        return APP_CONTROL_ERR_DEVICE_UNAVAILABLE;
    }

    /* 3. it is writable at all */
    if (!entity->writable || entity->backend == APP_ENTITY_BACKEND_NONE) {
        return APP_CONTROL_ERR_NOT_WRITABLE;
    }

    /* 4. the action is advertised */
    if (!app_control_service_is_supported(entity, action)) {
        return APP_CONTROL_ERR_UNSUPPORTED;
    }

    /* 5. the value is present when required and inside the declared range */
    if (action_takes_value(action)) {
        if (value == NULL || value[0] == '\0') {
            return APP_CONTROL_ERR_INVALID_ARGUMENT;
        }
        if (entity->has_range) {
            if (!parse_i32(value, &numeric)) {
                /* A required numeric value that does not parse is refused, never
                 * coerced: zero is a meaningful value for most of these domains. */
                return APP_CONTROL_ERR_OUT_OF_RANGE;
            }
            if (numeric < entity->min_value || numeric > entity->max_value) {
                return APP_CONTROL_ERR_OUT_OF_RANGE;
            }
        }
    } else if (value != NULL && value[0] != '\0') {
        /* A value on an action that takes none is a caller mistake, and silently
         * dropping it would make the request mean something the caller did not ask. */
        return APP_CONTROL_ERR_INVALID_ARGUMENT;
    }

    /* 6. exactly one backend owns it */
    route = route_backend(entity, &backend);
    if (route != APP_CONTROL_OK) {
        return route;
    }

    /*
     * One control per entity at a time.
     *
     * The earlier request's identifier is still outstanding with its caller, so a
     * second request is refused rather than queued or allowed to overwrite: answering
     * the first identifier with a result produced by the second request is worse than
     * telling this caller to come back.
     */
    if (pending_for_entity(entity_id) != NULL) {
        return APP_CONTROL_ERR_BUSY;
    }

    slot = pending_alloc();
    if (slot == NULL) {
        return APP_CONTROL_ERR_NO_CAPACITY;
    }

    slot->request_id = request_id;
    (void)app_strlcpy(slot->entity_id, entity_id, sizeof(slot->entity_id));
    (void)app_strlcpy(slot->device_id, entity->device_id, sizeof(slot->device_id));
    (void)app_strlcpy(slot->action, action, sizeof(slot->action));
    if (value != NULL) {
        (void)app_strlcpy(slot->value, value, sizeof(slot->value));
    }
    slot->state = APP_CONTROL_STATE_PENDING;
    slot->started_ms = now_ms;
    /*
     * A zero timeout means "no deadline yet": the caller is expected to tick with a
     * real timeout later, or the control stays pending until a confirmation arrives.
     * Treating zero as "expire immediately" would fail every control submitted this
     * way, so it is deliberately not given a default here - the runtime supplies one.
     */
    slot->deadline_ms = timeout_ms;
    slot->backend_id = backend.ops->backend_id;

    /* The state to restore if this fails. Recorded before the send, because after it
     * the device may already be changing. */
    {
        const char *previous = app_control_observed_state(entity_id);

        if (previous != NULL) {
            (void)app_strlcpy(slot->previous_state, previous,
                              sizeof(slot->previous_state));
            slot->has_previous_state = true;
        } else {
            const ha_state_t *ha = ha_core_state_get(entity_id);

            if (ha != NULL) {
                (void)app_strlcpy(slot->previous_state, ha->state,
                                  sizeof(slot->previous_state));
                slot->has_previous_state = true;
            }
        }
    }

    if (out_slot != NULL) {
        *out_slot = slot;
    }

    /*
     * Send. A SENT result keeps the slot PENDING: the request left the firmware and
     * nothing more is known, so no state moves until confirm() arrives.
     */
    switch (backend.ops->send(backend.ctx, entity_id, action, value)) {
    case APP_CONTROL_BACKEND_SENT:
        return APP_CONTROL_OK;
    case APP_CONTROL_BACKEND_UNSUPPORTED:
        slot->state = APP_CONTROL_STATE_FAILED;
        slot->reason = "backend_unsupported";
        pending_release(slot);
        return APP_CONTROL_ERR_UNSUPPORTED;
    case APP_CONTROL_BACKEND_FAILED:
    default:
        slot->state = APP_CONTROL_STATE_FAILED;
        slot->reason = "backend_failed";
        pending_release(slot);
        return APP_CONTROL_ERR_BACKEND_FAILED;
    }
}

/* ---------------- completion ---------------- */

bool app_control_confirm(uint32_t request_id)
{
    app_control_slot_t *slot = pending_find(request_id);

    /*
     * Only a request that is still PENDING can be confirmed. A late confirmation for a
     * request that already timed out or was canceled must not resurrect it: the caller
     * has been told the control failed, and moving the state now would contradict that
     * answer and let a stale device response overwrite a newer request's result.
     */
    if (slot == NULL || slot->state != APP_CONTROL_STATE_PENDING) {
        return false;
    }

    /*
     * The state the user asked for is published only here, and only because the device
     * said so. This is the whole point of the module.
     */
    if (strcmp(slot->action, HA_SERVICE_TURN_ON) == 0) {
        observed_set(slot->entity_id, HA_STATE_ON);
    } else if (strcmp(slot->action, HA_SERVICE_TURN_OFF) == 0) {
        observed_set(slot->entity_id, HA_STATE_OFF);
    } else if (slot->value[0] != '\0') {
        observed_set(slot->entity_id, slot->value);
    } else {
        /* A value-less action whose result carries no state (a button press) leaves
         * the observed state alone; inventing one would be a fabricated reading. */
    }

    slot->state = APP_CONTROL_STATE_CONFIRMED;
    slot->reason = "confirmed";
    pending_release(slot);
    return true;
}

bool app_control_report(const char *entity_id, const char *state)
{
    app_control_slot_t *pending;

    if (entity_id == NULL || state == NULL || state[0] == '\0') {
        return false;
    }
    if (app_control_lookup(entity_id) == NULL) {
        return false;
    }

    /*
     * A device's own report supersedes a request that has not been confirmed. The
     * device is the authority on its own state, so the pending control is failed and
     * the reported state published - leaving the request pending would mean waiting
     * for a confirmation that the device has already contradicted.
     */
    pending = pending_for_entity(entity_id);
    if (pending != NULL && pending->state == APP_CONTROL_STATE_PENDING) {
        pending->state = APP_CONTROL_STATE_FAILED;
        pending->reason = "superseded_by_report";
        pending_release(pending);
    }

    observed_set(entity_id, state);
    return true;
}

bool app_control_fail(uint32_t request_id, const char *reason)
{
    app_control_slot_t *slot = pending_find(request_id);

    if (slot == NULL || slot->state != APP_CONTROL_STATE_PENDING) {
        return false;
    }
    /*
     * A failure restores the previously confirmed state. The device never confirmed
     * the requested value, so leaving it showing would publish a state the device is
     * not in - and the operator would have no way to tell.
     */
    if (slot->has_previous_state) {
        observed_set(slot->entity_id, slot->previous_state);
    }
    slot->state = APP_CONTROL_STATE_FAILED;
    slot->reason = reason != NULL ? reason : "failed";
    pending_release(slot);
    return true;
}

bool app_control_cancel(uint32_t request_id, const char *reason)
{
    app_control_slot_t *slot = pending_find(request_id);

    if (slot == NULL || slot->state != APP_CONTROL_STATE_PENDING) {
        return false;
    }
    if (slot->has_previous_state) {
        observed_set(slot->entity_id, slot->previous_state);
    }
    slot->state = APP_CONTROL_STATE_CANCELED;
    slot->reason = reason != NULL ? reason : "canceled";
    pending_release(slot);
    return true;
}

/* Deadline arithmetic that cannot be fooled by a wrapping millisecond counter. */
static bool deadline_passed(uint32_t now_ms, uint32_t started_ms, uint32_t timeout_ms)
{
    if (timeout_ms == 0u) {
        return false;
    }
    return (uint32_t)(now_ms - started_ms) >= timeout_ms;
}

bool app_control_tick(uint32_t now_ms)
{
    bool changed = false;

    for (size_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        app_control_slot_t *slot = &s_pending[i];

        if (!slot->in_use || slot->state != APP_CONTROL_STATE_PENDING) {
            continue;
        }
        if (!deadline_passed(now_ms, slot->started_ms, slot->deadline_ms)) {
            continue;
        }
        /*
         * Timed out. The device never confirmed, so the requested value must not be
         * left showing: the previous confirmed state is restored and the caller is
         * told TIMEOUT rather than FAILED, because "we do not know" and "the device
         * said no" are different answers.
         */
        if (slot->has_previous_state) {
            observed_set(slot->entity_id, slot->previous_state);
        }
        slot->state = APP_CONTROL_STATE_TIMEOUT;
        slot->reason = "confirmation_timeout";
        pending_release(slot);
        changed = true;
    }
    return changed;
}

size_t app_control_reconcile(void)
{
    size_t failed = 0u;

    for (size_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        app_control_slot_t *slot = &s_pending[i];
        const app_entity_binding_t *entity;
        const app_device_binding_t *device;
        const char *reason = NULL;

        if (!slot->in_use || slot->state != APP_CONTROL_STATE_PENDING) {
            continue;
        }

        entity = app_control_lookup(slot->entity_id);
        device = entity != NULL ? app_device_find(entity->device_id) : NULL;

        if (entity == NULL) {
            /*
             * The entity is gone - swept, renumbered by a database switch, or evicted
             * for capacity. Its state cannot be restored, because there is no entity
             * to restore it onto; the control is failed and nothing is written.
             */
            slot->state = APP_CONTROL_STATE_FAILED;
            slot->reason = "entity_gone";
            pending_release(slot);
            failed++;
            continue;
        }
        if (device == NULL) {
            reason = "device_gone";
        } else if (device->availability != APP_AVAILABILITY_ONLINE) {
            /*
             * The device went stale or unavailable while the command was in flight. It
             * is not going to confirm, and continuing to wait would leave a spinner
             * that never resolves.
             */
            reason = "device_unavailable";
        } else if (!entity->writable || entity->backend == APP_ENTITY_BACKEND_NONE) {
            /* A database switch made the entity read-only. The control that was
             * accepted under the old corpus is no longer valid. */
            reason = "no_longer_writable";
        }

        if (reason == NULL) {
            continue;
        }
        if (slot->has_previous_state) {
            observed_set(slot->entity_id, slot->previous_state);
        }
        slot->state = APP_CONTROL_STATE_FAILED;
        slot->reason = reason;
        pending_release(slot);
        failed++;
    }
    return failed;
}

/* ---------------- enumeration ---------------- */

const app_control_slot_t *app_control_find(uint32_t request_id)
{
    return pending_find(request_id);
}

size_t app_control_pending_count(void)
{
    size_t count = 0u;

    for (size_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        if (s_pending[i].in_use && s_pending[i].state == APP_CONTROL_STATE_PENDING) {
            count++;
        }
    }
    return count;
}

const app_control_slot_t *app_control_pending_at(size_t index)
{
    size_t seen = 0u;

    for (size_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        if (!s_pending[i].in_use || s_pending[i].state != APP_CONTROL_STATE_PENDING) {
            continue;
        }
        if (seen++ == index) {
            return &s_pending[i];
        }
    }
    return NULL;
}
