#pragma once

/*
 * Unified control loop: Entity -> checks -> one backend -> execute -> confirmation.
 *
 * This is the single path from "a user changed an Entity" to "the Entity's state
 * changed". The serial console, the HTTP portal and the future GUI all submit here;
 * none of them may grow a second path, because the rules below are the product's and
 * they only hold if there is one place they are enforced.
 *
 * THE RULE THAT SHAPES EVERYTHING
 *
 *   A successful send is not a state change.
 *
 * `turn_on` reaching the device proves a request left this firmware and nothing more.
 * The observed state moves only when the device answers, reports, or is read back -
 * so a control is PENDING until a confirmation arrives, and a state the user asked
 * for is never published as if it were the state the device is in. A failed control
 * restores the previously confirmed state instead of leaving the Entity showing the
 * value that never took effect.
 *
 * WHAT IS CHECKED BEFORE A BACKEND IS EVER TOUCHED, in this order:
 *
 *   1. the Entity exists in the application binding table;
 *   2. its owning Device is known and ONLINE - a stale or unavailable device is not
 *      controlled, because "the device is not there" is not a reason to send a
 *      command into a void;
 *   3. the Entity is writable at all (it has a resolved, drivable backend binding);
 *   4. the requested action is one the Entity advertises;
 *   5. the value, if the action carries one, is within the recipe's declared range;
 *   6. exactly one backend claims the Entity. Zero means unsupported; more than one
 *      means the binding is ambiguous, and ambiguity is never permission to guess.
 *
 * Requests carry an identifier the caller chooses. A second request for the same
 * Entity while one is pending is REFUSED rather than queued or overwritten: the
 * caller's identifier for the first one is still outstanding, and replying to it with
 * a result produced by a different request is worse than refusing.
 *
 * Platform independent. Backends arrive through a small vtable, so the whole loop -
 * including late confirmations, device disappearance and database switches - is
 * exercised on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_device.h"
#include "esp_err.h"
#include "ha_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many entities may be controlled at once. One is the norm; a handful allows a
 * scene-like burst without unbounded bookkeeping. */
#define APP_CONTROL_MAX_PENDING 4u
/* Confirmed-state slots, one per entity the application may control. */
#define APP_CONTROL_MAX_ENTITIES 32u
#define APP_CONTROL_VALUE_MAX 40u
#define APP_CONTROL_REASON_MAX 48u

typedef enum {
    APP_CONTROL_OK = 0,
    APP_CONTROL_ERR_INVALID_ARGUMENT,
    APP_CONTROL_ERR_UNKNOWN_ENTITY,   /* not in the application binding table */
    APP_CONTROL_ERR_NOT_WRITABLE,     /* read-only: no drivable backend binding */
    APP_CONTROL_ERR_UNSUPPORTED,      /* the Entity does not advertise this action */
    APP_CONTROL_ERR_OUT_OF_RANGE,     /* the value violates the recipe's range */
    APP_CONTROL_ERR_NO_BACKEND,       /* nothing claims the Entity */
    APP_CONTROL_ERR_AMBIGUOUS_BACKEND,/* more than one backend claims it */
    APP_CONTROL_ERR_DEVICE_UNAVAILABLE,/* the owning Device is stale/unavailable */
    APP_CONTROL_ERR_BUSY,             /* a control for this Entity is already pending */
    APP_CONTROL_ERR_NO_CAPACITY,      /* too many controls in flight */
    APP_CONTROL_ERR_BACKEND_FAILED,   /* the backend refused or failed to send */
} app_control_status_t;

const char *app_control_status_name(app_control_status_t status);

/* Terminal state of one control. PENDING is not terminal. */
typedef enum {
    APP_CONTROL_STATE_PENDING = 0,
    APP_CONTROL_STATE_CONFIRMED,
    APP_CONTROL_STATE_FAILED,
    APP_CONTROL_STATE_TIMEOUT,
    APP_CONTROL_STATE_CANCELED,
} app_control_state_t;

const char *app_control_state_name(app_control_state_t state);

/*
 * How a backend reports the outcome of a sent command.
 *
 * SENT means the request was accepted for transmission and nothing more. The state
 * machine will not treat it as a state change; it waits for confirm()/report() or the
 * confirmation deadline.
 */
typedef enum {
    APP_CONTROL_BACKEND_SENT = 0,
    APP_CONTROL_BACKEND_FAILED,
    APP_CONTROL_BACKEND_UNSUPPORTED,
} app_control_backend_result_t;

/*
 * A control backend.
 *
 * `send` must not block indefinitely: it runs on the application worker, and the
 * confirmation deadline is measured from the moment it returns SENT.
 *
 * `entity_id` is the HA entity id; `action` and `value` are the already-validated
 * request. A backend must not re-validate the range - the loop did that - and must
 * not mutate HA state: the loop owns the state, and a backend that wrote it directly
 * would make "confirmed" meaningless.
 */
typedef struct app_control_backend_ops {
    /* Identifies the backend in diagnostics and in the ambiguity check. */
    uint8_t backend_id;
    const char *name;
    /* Does this backend own this entity? Called during registration and routing. */
    bool (*claims)(void *ctx, const app_entity_binding_t *entity);
    app_control_backend_result_t (*send)(void *ctx, const char *entity_id,
                                         const char *action, const char *value);
} app_control_backend_ops_t;

typedef struct {
    const app_control_backend_ops_t *ops;
    void *ctx;
} app_control_backend_t;

/* One in-flight or completed control. Exposed for diagnostics and for the console. */
typedef struct {
    bool in_use;
    uint32_t request_id;
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char device_id[HA_CORE_ID_LEN];
    char action[HA_CORE_SERVICE_LEN];
    char value[APP_CONTROL_VALUE_MAX];
    app_control_state_t state;
    /* The state the entity had before this request, so a failure can restore it. */
    char previous_state[HA_CORE_STATE_LEN];
    bool has_previous_state;
    uint32_t started_ms;
    uint32_t deadline_ms;
    uint8_t backend_id;
    const char *reason; /* stable short literal, never a secret */
} app_control_slot_t;

void app_control_reset(void);

/*
 * Register a backend. Backends are fixed at start-up; registering twice with the same
 * id replaces the earlier entry, which is what a re-init would want.
 */
esp_err_t app_control_register_backend(const app_control_backend_t *backend);

/* Number of registered backends, for diagnostics. */
size_t app_control_backend_count(void);

/*
 * Submit a control request.
 *
 * `request_id` is the caller's identifier and is echoed in the result and in
 * app_control_find(). `out_slot` receives the created slot when one is created, so a
 * caller can report PENDING immediately rather than guessing.
 *
 * `now_ms` is passed in rather than read from a clock: it makes the confirmation
 * deadline testable, and this module has no clock of its own by design.
 */
app_control_status_t app_control_submit(const char *entity_id, const char *action,
                                        const char *value, uint32_t request_id,
                                        uint32_t now_ms, uint32_t timeout_ms,
                                        app_control_slot_t **out_slot);

/*
 * A backend reports that the device confirmed the new state.
 *
 * This is the ONLY way an observed state moves. Returns false when the request is no
 * longer pending - already timed out, canceled, or displaced - because a late
 * confirmation for a finished request must not resurrect it.
 */
bool app_control_confirm(uint32_t request_id);

/* A backend reports that the device is now in `state` without a request of its own:
 * a spontaneous report or a readback. This moves the observed state for that Entity
 * and fails any control pending on it, because the device's own report supersedes a
 * request that has not been confirmed. */
bool app_control_report(const char *entity_id, const char *state);

/* A backend reports that the send failed. */
bool app_control_fail(uint32_t request_id, const char *reason);

/*
 * Advance every pending control. Returns true when anything reached a terminal state.
 *
 * A control whose deadline has passed becomes TIMEOUT and its observed state is
 * restored to the pre-request value: the device never confirmed, so the value the
 * user asked for must not be left showing.
 */
bool app_control_tick(uint32_t now_ms);

/* Cancel one pending control by request id. */
bool app_control_cancel(uint32_t request_id, const char *reason);

/*
 * Fail every pending control whose owning Device is no longer controllable - swept,
 * gone, or no longer ONLINE - and every control whose Entity no longer exists.
 *
 * Called after a scan generation finishes and after a database switch. This is what
 * stops a control from being confirmed against an entity that has been renumbered or
 * removed, and what keeps a vanished device from leaving a pending spinner forever.
 */
size_t app_control_reconcile(void);

const app_control_slot_t *app_control_find(uint32_t request_id);
size_t app_control_pending_count(void);
const app_control_slot_t *app_control_pending_at(size_t index);

/* The observed (confirmed) state of an Entity, or NULL when none was ever observed. */
const char *app_control_observed_state(const char *entity_id);

/* Does this Entity advertise this action? Used by the checks and by the console. */
bool app_control_service_is_supported(const app_entity_binding_t *entity,
                                      const char *action);

/* Where the console and the portal get the entity binding for an id. */
const app_entity_binding_t *app_control_lookup(const char *entity_id);

#ifdef __cplusplus
}
#endif
