#pragma once

/*
 * BLE GATT session: the application side of the BLE control path.
 *
 * Owns one bounded session with one peer: connect, discover, read, write,
 * subscribe, and end - with an explicit phase, an explicit outcome for every
 * operation, a deadline, and cancellation that actually cancels.
 *
 * Platform independent. The GATT operations themselves arrive through
 * app_ble_gatt_session_ops_t, so the whole lifecycle - including cancellation,
 * deadline expiry and late callbacks - is exercised on the host against a
 * scripted backend. The firmware supplies an adapter over the esphome_l2 GATT
 * component.
 *
 * WHAT THIS MODULE EXISTS TO FIX
 *
 * The GATT layer below is a thin, synchronous wrapper: one operation at a time,
 * one shared completion slot, no notion of "which session does this callback
 * belong to". Three consequences are handled here rather than there:
 *
 *   1. CANCELLATION MUST NOT LOOK LIKE SUCCESS. If a caller cancels (or the
 *      deadline expires) while a backend operation is in flight, the backend can
 *      still return success for an operation the application no longer wants -
 *      a cancel that terminates the link makes a pending read complete with a
 *      zero-length value and no error. This module decides the outcome of every
 *      operation, not the backend: it stamps each operation with the generation
 *      it was issued in, and an operation that returns after a cancel or a close
 *      is reported as CANCELLED with its payload discarded, never as success.
 *
 *   2. A LATE CALLBACK MUST NOT LAND IN THE NEXT SESSION. The backend may deliver
 *      a notification after teardown began. Every notification carries the
 *      generation of the session it was received for; a notification whose
 *      generation does not match the live session is dropped before the caller's
 *      callback runs.
 *
 *   3. AN OPERATION TIMEOUT IS A SESSION FAILURE, NOT A RETRY. The link is torn
 *      down after a timeout, so the session moves to FAILED and reports it. The
 *      caller reconnects deliberately; nothing here retries in a loop.
 *
 * WHAT A CALLER MUST NOT DO
 *
 * Callbacks run on the GATT backend's own task. The caller's callback must not
 * call back into this module: the session is not reentrant, and an operation
 * issued from a notification callback would wait for a completion that only the
 * task it is running on could produce.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esphome_ble_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Peers are addressed in display order: address[0] is the most significant byte as
 * printed, which is the order scan evidence carries. The adapter is responsible for
 * converting to whatever byte order the radio wants. */
typedef esphome_ble_peer_t app_ble_peer_t;
typedef esphome_ble_gatt_db_t app_ble_db_t;

typedef enum {
    APP_BLE_PHASE_IDLE = 0,
    APP_BLE_PHASE_CONNECTING,
    APP_BLE_PHASE_READY,
    APP_BLE_PHASE_FAILED,
    APP_BLE_PHASE_CLOSED,
} app_ble_phase_t;

const char *app_ble_phase_name(app_ble_phase_t phase);

/*
 * The outcome of one operation. Every operation returns one of these; there is no
 * "success with an empty payload" that a caller could mistake for a real value.
 */
typedef enum {
    APP_BLE_OK = 0,
    APP_BLE_ERR_INVALID_ARGUMENT,
    APP_BLE_ERR_NOT_READY,        /* no live session */
    APP_BLE_ERR_BUSY,             /* another operation is in flight */
    APP_BLE_ERR_CONNECT_FAILED,
    APP_BLE_ERR_DISCOVER_FAILED,
    APP_BLE_ERR_READ_FAILED,
    APP_BLE_ERR_WRITE_FAILED,
    APP_BLE_ERR_SUBSCRIBE_FAILED,
    APP_BLE_ERR_NOT_FOUND,        /* no such characteristic / subscription */
    APP_BLE_ERR_TIMEOUT,          /* the operation's deadline passed */
    APP_BLE_ERR_CANCELLED,        /* cancelled or the session was closed under it */
    APP_BLE_ERR_PEER_GONE,        /* the link dropped */
    APP_BLE_ERR_TRUNCATED,        /* the value did not fit the caller's buffer */
    APP_BLE_ERR_UNSUPPORTED,      /* the backend cannot do this at all */
} app_ble_status_t;

const char *app_ble_status_name(app_ble_status_t status);

/* What the session needs from the platform. Every entry is required except the
 * two radio hooks, which may be NULL when the platform has no radio arbiter. */
typedef struct {
    esp_err_t (*gatt_init)(void *ctx, const esphome_ble_gatt_config_t *config);
    void (*gatt_deinit)(void *ctx);
    esp_err_t (*gatt_connect)(void *ctx, const app_ble_peer_t *peer);
    esp_err_t (*gatt_discover)(void *ctx, app_ble_db_t *db);
    esp_err_t (*gatt_read)(void *ctx, uint16_t handle, uint8_t *out, size_t capacity, size_t *out_len);
    esp_err_t (*gatt_write)(void *ctx, uint16_t handle, const uint8_t *data, size_t len, bool response);
    esp_err_t (*gatt_subscribe)(void *ctx, uint16_t value_handle, uint16_t cccd_handle, bool indications,
                                esphome_ble_gatt_notify_fn callback, void *user);
    esp_err_t (*gatt_unsubscribe)(void *ctx, uint16_t value_handle);
    esp_err_t (*gatt_disconnect)(void *ctx);
    void (*gatt_cancel)(void *ctx);
    bool (*gatt_is_connected)(void *ctx);
    int (*gatt_last_error)(void *ctx);
    /* Radio arbitration, optional. */
    esp_err_t (*radio_acquire)(void *ctx);
    esp_err_t (*radio_release)(void *ctx);
    /* Monotonic milliseconds. Required: every wait is bounded by it. */
    uint32_t (*now_ms)(void *ctx);
} app_ble_gatt_session_ops_t;

typedef struct {
    /* 0 selects the default. */
    uint32_t connect_timeout_ms;
    uint32_t operation_timeout_ms;
} app_ble_gatt_session_config_t;

#define APP_BLE_DEFAULT_CONNECT_TIMEOUT_MS 12000u
#define APP_BLE_DEFAULT_OPERATION_TIMEOUT_MS 6000u
/* Largest value the session will hand a caller or accept for a write. A GATT
 * value that does not fit is reported as truncated rather than silently cut. */
#define APP_BLE_VALUE_MAX 256u

/* Discovery is bounded in the session itself, so a peer with a huge GATT table
 * costs a fixed amount of RAM and is reported as truncated rather than
 * exhausting the heap. Descriptors are collected only so a CCCD can be located;
 * nothing else about them is retained. */
#define APP_BLE_MAX_SERVICES 8u
#define APP_BLE_MAX_CHARACTERISTICS 24u
#define APP_BLE_MAX_DESCRIPTORS 12u

#define APP_BLE_NOTIFY_SLOTS ESPHOME_BLE_GATT_MAX_SUBSCRIPTIONS

typedef void (*app_ble_notify_fn)(void *user, uint16_t value_handle, const uint8_t *data, size_t len,
                                  bool truncated);

typedef struct {
    app_ble_gatt_session_ops_t ops;
    void *ops_ctx;
    app_ble_gatt_session_config_t config;

    app_ble_phase_t phase;
    app_ble_status_t last_status; /* outcome of the most recent operation */
    int last_native_error;

    /* Incremented by every close and every cancel. An operation or callback that
     * carries a different generation belongs to a session that no longer exists. */
    uint32_t generation;
    /* The generation of the operation currently in flight. */
    uint32_t op_generation;
    bool op_active;
    bool op_cancelled;
    uint32_t op_started_ms;
    uint32_t op_timeout_ms;

    app_ble_peer_t peer;
    bool connected;
    bool radio_held;
    bool initialized;

    esphome_ble_gatt_service_t services[APP_BLE_MAX_SERVICES];
    esphome_ble_gatt_characteristic_t characteristics[APP_BLE_MAX_CHARACTERISTICS];
    esphome_ble_gatt_descriptor_t descriptors[APP_BLE_MAX_DESCRIPTORS];
    app_ble_db_t db;

    /* Subscriptions the application asked for, so close() can undo exactly what
     * open() and subscribe() created. */
    struct {
        bool in_use;
        uint16_t value_handle;
        uint16_t cccd_handle;
        app_ble_notify_fn callback;
        void *user;
        /* The generation this subscription was made in. A notification carrying
         * any other generation belongs to a session that has already ended. */
        uint32_t generation;
    } subscriptions[APP_BLE_NOTIFY_SLOTS];

    uint32_t notifications_seen;
    uint32_t notifications_dropped;
    uint32_t cancellations;
    uint32_t timeouts;
} app_ble_gatt_session_t;

void app_ble_gatt_session_config_default(app_ble_gatt_session_config_t *out);

/* Install the ops. Validates every required entry and calls gatt_init once. */
esp_err_t app_ble_gatt_session_init(app_ble_gatt_session_t *session,
                                    const app_ble_gatt_session_ops_t *ops, void *ops_ctx,
                                    const app_ble_gatt_session_config_t *config);

/*
 * Open a session with `peer`. Takes the radio if the platform offers one, calls
 * connect, then runs service discovery into the session's own database - a
 * session without a discovered database can neither resolve handles nor
 * subscribe, so a caller should never have to remember to ask for it.
 */
app_ble_status_t app_ble_gatt_session_open(app_ble_gatt_session_t *session, const app_ble_peer_t *peer);

/*
 * End the session. Idempotent. Cancels anything in flight first, so the caller
 * cannot leave an operation waiting on a link that is being torn down, then
 * unsubscribes, disconnects, releases the radio, and moves to CLOSED.
 *
 * A clean end returns APP_BLE_OK. A backend that fails or times out during
 * teardown is reported, but the session still ends: a session that cannot be
 * ended is worse than one that ends with a diagnostic.
 */
app_ble_status_t app_ble_gatt_session_close(app_ble_gatt_session_t *session);

/*
 * Cancel whatever is in flight and drop the link, without ending the session.
 * The in-flight operation reports CANCELLED, never the backend's own result.
 */
app_ble_status_t app_ble_gatt_session_cancel(app_ble_gatt_session_t *session);

app_ble_status_t app_ble_gatt_session_read(app_ble_gatt_session_t *session, uint16_t handle,
                                           uint8_t *out, size_t capacity, size_t *out_len);

app_ble_status_t app_ble_gatt_session_write(app_ble_gatt_session_t *session, uint16_t handle,
                                            const uint8_t *data, size_t len, bool response);

app_ble_status_t app_ble_gatt_session_subscribe(app_ble_gatt_session_t *session, uint16_t value_handle,
                                                uint16_t cccd_handle, app_ble_notify_fn callback,
                                                void *user, bool indications);

app_ble_status_t app_ble_gatt_session_unsubscribe(app_ble_gatt_session_t *session, uint16_t value_handle);

/*
 * Expire an in-flight operation whose deadline has passed. Called from the
 * application worker, not from a callback.
 *
 * Returns true when something ended, so the caller logs it once.
 */
bool app_ble_gatt_session_tick(app_ble_gatt_session_t *session, uint32_t now_ms);

/*
 * Notification sink. The backend calls this; the session checks the generation
 * before invoking the caller's callback, so a notification from a dead session
 * never reaches the application.
 *
 * `generation` is the value of session->generation at the time the subscription
 * was made. A backend adapter passes it through; anything else is a backend bug
 * and is treated as a stale notification rather than trusted.
 */
void app_ble_gatt_session_on_notify(app_ble_gatt_session_t *session, uint32_t generation,
                                    uint16_t value_handle, const uint8_t *data, size_t len,
                                    bool truncated);

/*
 * Handle resolution over the discovered database.
 *
 * `characteristic_index` is the index into the database's characteristic array,
 * which is what a recognition recipe can name portably. Returns NOT_FOUND rather
 * than guessing when the index is out of range or the characteristic carries no
 * CCCD and the caller asked for one.
 */
app_ble_status_t app_ble_gatt_resolve(const app_ble_db_t *db, uint32_t characteristic_index,
                                      uint16_t *out_value_handle, uint16_t *out_cccd_handle);

/*
 * Value codec, so a control action becomes bytes without every caller inventing
 * its own encoding.
 *
 * `action` is the HA service name ("turn_on", "turn_off", "set_value", "set_text").
 * Booleans encode as one byte, 0x00 or 0x01, little-endian integers for numeric
 * values, and a UTF-8 string truncated at APP_BLE_VALUE_MAX for text. `scale`
 * multiplies a numeric value before conversion, which is how a recipe expresses a
 * tenths-of-a-degree register.
 *
 * Returns APP_BLE_ERR_UNSUPPORTED for an action or payload shape the codec does
 * not implement: guessing an encoding is how a device gets written with garbage.
 */
app_ble_status_t app_ble_gatt_encode_action(const char *action, const char *value, float scale,
                                            uint8_t *out, size_t capacity, size_t *out_len);

/* The inverse, for interpreting a read or a notification. */
app_ble_status_t app_ble_gatt_decode_state(const uint8_t *data, size_t len, float scale,
                                           char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
