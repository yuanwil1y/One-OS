#pragma once

/*
 * The ESPHome Native API control backend: app_control -> a discovered ESPHome entity.
 *
 * WHAT WAS ALREADY THERE, AND WHAT THIS ADDS
 *
 * The esphome_l2 component is complete on the wire: the framing, the Noise NNpsk0
 * handshake, the protobuf codec, entity discovery, state subscriptions and the command
 * encoder (`esphome_api_codec_command()`). What was missing is the controller - the
 * piece app_control talks to. That is this file, and it is the same shape as
 * app_ctl_ble: send() validates and queues, tick() performs the send on the worker and
 * enforces the confirmation deadline, and a state report only records what arrived.
 *
 * THE RULE IT MUST NOT BREAK
 *
 *   A successful send is not a state change.
 *
 * ESPHome makes this easy to get wrong, because a `SwitchStateResponse` arrives on the
 * same subscription as the state the entity already had. Confirming on "a report
 * arrived" would mark a refused command as confirmed. The comparison is therefore
 * against the state the REQUEST asked for, and the same scale trap applies as on the
 * BLE side: a number entity's value is a float, and what comes back is what was sent.
 *
 * WHY THE COMMAND NEEDS TWO IDENTIFIERS
 *
 * An ESPHome command carries the entity `key` and, optionally, the device's
 * `device_id`. The key is what the corpus can name portably, so it travels in
 * `entity->write_target_id`. The device id is a property of the connection - it is
 * discovered with the entity list - so it is looked up here through the injected
 * `find_entity` op rather than being stored in the corpus, which would duplicate a
 * fact the device itself reports.
 *
 * THREADING
 *
 * `send` and `tick` run on the application worker. `on_state` runs on the API client's
 * task, so it only records. Nothing here writes the observed state.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_control.h"
#include "app_device.h"
#include "esp_err.h"
#include "esphome_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CTL_ESPHOME_MAX_INFLIGHT 2u
/* How long a sent command may stay unconfirmed. ESPHome reports state promptly, so
 * this is generous rather than tight; a command the device obeyed must not be reported
 * as a device that never answered. */
#define APP_CTL_ESPHOME_CONFIRM_TIMEOUT_MS 15000u

typedef enum {
    APP_CTL_ESPHOME_IDLE = 0,
    APP_CTL_ESPHOME_SENDING,
    APP_CTL_ESPHOME_AWAITING_CONFIRMATION,
    APP_CTL_ESPHOME_FAILED,
} app_ctl_esphome_state_t;

typedef struct {
    bool in_use;
    app_ctl_esphome_state_t state;
    uint32_t request_id;
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char device_id[HA_CORE_ID_LEN];
    char action[HA_CORE_SERVICE_LEN];
    /* The state the device must report for this control to count as confirmed, derived
     * from the request. has_expect_state is false for an action whose success is not a
     * state this firmware can name; such a control ends at its deadline with
     * `esphome_confirm_unavailable` rather than a claim that the device failed. */
    char expect_state[HA_CORE_STATE_LEN];
    bool has_expect_state;
    uint32_t key;
    uint32_t deadline_ms;
    esphome_api_command_t command;
    bool has_reported_state;
    bool reported_boolean;
    float reported_number;
    char reported_text[ESPHOME_API_MAX_STATE_TEXT_LEN];
} app_ctl_esphome_op_t;

typedef struct {
    struct {
        /* Send one command. Returns ESP_OK or the client's error. */
        esp_err_t (*send)(void *ctx, const esphome_api_command_t *command);
        /* Is the session connected and subscribed? A command needs both. */
        bool (*is_ready)(void *ctx);
        /* Find the discovered entity with this key, so the device id the command wants
         * comes from the device rather than from the corpus. Returns NULL when the key
         * was not discovered. */
        const esphome_api_entity_t *(*find_entity)(void *ctx, uint32_t key);
    } api;
    void *api_ctx;
    uint32_t (*now_ms)(void);

    app_ctl_esphome_op_t ops[APP_CTL_ESPHOME_MAX_INFLIGHT];
    uint32_t commands_sent;
    uint32_t commands_failed;
    uint32_t confirmations;
    uint32_t states_seen;
    uint32_t deadlines_expired;
    uint32_t refused_not_ready;
    uint32_t refused_unknown_key;
    uint32_t refused_unsupported_action;
    uint32_t refused_unsupported_kind;
} app_ctl_esphome_t;

/* The vtable entry to hand app_control_register_backend(). `ctx` is an
 * app_ctl_esphome_t with its api ops and clock filled in. */
const app_control_backend_ops_t *app_ctl_esphome_backend_ops(void);

/* Advance the backend: send a queued command, expire one whose deadline has passed.
 * Returns true when anything changed. Run from the application worker. */
bool app_ctl_esphome_tick(app_ctl_esphome_t *self);

/*
 * A state report arrived. Called from the API client's task; records only.
 *
 * The report is matched to an in-flight operation by entity key and buffered. The
 * decision to confirm is taken in app_ctl_esphome_tick().
 */
void app_ctl_esphome_on_state(app_ctl_esphome_t *self, const esphome_api_state_t *state);

const char *app_ctl_esphome_state_name(app_ctl_esphome_state_t state);
size_t app_ctl_esphome_inflight_count(const app_ctl_esphome_t *self);

/* Clear the in-flight operations without confirming them. A control that was in flight
 * when the session went away is answered by app_control_reconcile(), not here. The
 * counters are kept: they describe the session that just ended. */
void app_ctl_esphome_reset(app_ctl_esphome_t *self);

#ifdef __cplusplus
}
#endif
