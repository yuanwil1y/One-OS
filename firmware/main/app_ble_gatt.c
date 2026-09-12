/*
 * BLE GATT session. See include/app_ble_gatt.h for the contract and the three
 * properties this module exists to guarantee.
 *
 * Platform independent: every GATT call arrives through app_ble_gatt_session_ops_t.
 */

#include "app_ble_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* names                                                               */
/* ------------------------------------------------------------------ */

const char *app_ble_phase_name(app_ble_phase_t phase)
{
    switch (phase) {
    case APP_BLE_PHASE_IDLE: return "idle";
    case APP_BLE_PHASE_CONNECTING: return "connecting";
    case APP_BLE_PHASE_READY: return "ready";
    case APP_BLE_PHASE_FAILED: return "failed";
    case APP_BLE_PHASE_CLOSED: return "closed";
    default: return "unknown";
    }
}

const char *app_ble_status_name(app_ble_status_t status)
{
    switch (status) {
    case APP_BLE_OK: return "ok";
    case APP_BLE_ERR_INVALID_ARGUMENT: return "invalid_argument";
    case APP_BLE_ERR_NOT_READY: return "not_ready";
    case APP_BLE_ERR_BUSY: return "busy";
    case APP_BLE_ERR_CONNECT_FAILED: return "connect_failed";
    case APP_BLE_ERR_DISCOVER_FAILED: return "discover_failed";
    case APP_BLE_ERR_READ_FAILED: return "read_failed";
    case APP_BLE_ERR_WRITE_FAILED: return "write_failed";
    case APP_BLE_ERR_SUBSCRIBE_FAILED: return "subscribe_failed";
    case APP_BLE_ERR_NOT_FOUND: return "not_found";
    case APP_BLE_ERR_TIMEOUT: return "timeout";
    case APP_BLE_ERR_CANCELLED: return "cancelled";
    case APP_BLE_ERR_PEER_GONE: return "peer_gone";
    case APP_BLE_ERR_TRUNCATED: return "truncated";
    case APP_BLE_ERR_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}

void app_ble_gatt_session_config_default(app_ble_gatt_session_config_t *out)
{
    if (out == NULL) {
        return;
    }
    out->connect_timeout_ms = APP_BLE_DEFAULT_CONNECT_TIMEOUT_MS;
    out->operation_timeout_ms = APP_BLE_DEFAULT_OPERATION_TIMEOUT_MS;
}

/* ------------------------------------------------------------------ */
/* operation bookkeeping                                               */
/* ------------------------------------------------------------------ */

static bool ops_complete(const app_ble_gatt_session_ops_t *ops)
{
    return ops->gatt_init != NULL && ops->gatt_deinit != NULL && ops->gatt_connect != NULL &&
           ops->gatt_discover != NULL && ops->gatt_read != NULL && ops->gatt_write != NULL &&
           ops->gatt_subscribe != NULL && ops->gatt_unsubscribe != NULL &&
           ops->gatt_disconnect != NULL && ops->gatt_cancel != NULL &&
           ops->gatt_is_connected != NULL && ops->gatt_last_error != NULL && ops->now_ms != NULL;
}

static uint32_t now(app_ble_gatt_session_t *s)
{
    return s->ops.now_ms(s->ops_ctx);
}

static void db_reset(app_ble_gatt_session_t *s)
{
    memset(s->services, 0, sizeof(s->services));
    memset(s->characteristics, 0, sizeof(s->characteristics));
    memset(s->descriptors, 0, sizeof(s->descriptors));
    memset(&s->db, 0, sizeof(s->db));
    s->db.services = s->services;
    s->db.service_capacity = APP_BLE_MAX_SERVICES;
    s->db.characteristics = s->characteristics;
    s->db.characteristic_capacity = APP_BLE_MAX_CHARACTERISTICS;
    /* Descriptors exist only so a CCCD can be located. */
    s->db.descriptors = s->descriptors;
    s->db.descriptor_capacity = APP_BLE_MAX_DESCRIPTORS;
}

static esp_err_t begin(app_ble_gatt_session_t *s, uint32_t timeout_ms)
{
    if (s->op_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s->op_active = true;
    s->op_cancelled = false;
    s->op_generation = s->generation;
    s->op_started_ms = now(s);
    s->op_timeout_ms = timeout_ms;
    return ESP_OK;
}

static void finish(app_ble_gatt_session_t *s)
{
    s->op_active = false;
    s->op_cancelled = false;
}

/*
 * Decide the outcome of an operation that has returned.
 *
 * This function is the reason the module exists. A backend call that returns
 * ESP_OK after the session was cancelled or closed did not succeed: the result
 * belongs to a session that no longer exists, and reporting it as success would
 * tell the application that a device was read or written when nothing of the
 * sort happened.
 */
static app_ble_status_t outcome(app_ble_gatt_session_t *s, esp_err_t err, app_ble_status_t on_failure)
{
    s->last_native_error = s->ops.gatt_last_error(s->ops_ctx);
    if (s->op_cancelled || s->generation != s->op_generation) {
        s->last_status = APP_BLE_ERR_CANCELLED;
        return s->last_status;
    }
    if (err == ESP_ERR_TIMEOUT) {
        s->timeouts++;
        s->last_status = APP_BLE_ERR_TIMEOUT;
        return s->last_status;
    }
    if (err != ESP_OK) {
        /* A link that was up and is now gone is reported as such: the
         * application has to distinguish "this device refused" from "this device
         * is no longer there". Before the session ever connected there is no
         * link to lose, so the operation's own failure is reported instead. */
        if (s->connected && !s->ops.gatt_is_connected(s->ops_ctx)) {
            s->connected = false;
            s->phase = APP_BLE_PHASE_FAILED;
            s->last_status = APP_BLE_ERR_PEER_GONE;
            return s->last_status;
        }
        s->last_status = on_failure;
        return s->last_status;
    }
    s->last_status = APP_BLE_OK;
    return s->last_status;
}

/* ------------------------------------------------------------------ */
/* notifications                                                       */
/* ------------------------------------------------------------------ */

/*
 * The subscription stores the generation it was made in. The backend adapter
 * passes that value back on every notification, which is what lets a late
 * callback be recognised and dropped instead of landing in the next session.
 */
static void notify_bridge(uint16_t value_handle, const uint8_t *data, size_t data_len, bool truncated,
                          void *user)
{
    app_ble_gatt_session_t *s = (app_ble_gatt_session_t *)user;
    uint32_t generation = 0u;

    if (s == NULL) {
        return;
    }
    /* The generation is read from the slot for this handle, not from the live
     * session: a notification is only trustworthy if it belongs to the session
     * the subscription was made in. */
    for (size_t i = 0u; i < APP_BLE_NOTIFY_SLOTS; i++) {
        if (s->subscriptions[i].in_use && s->subscriptions[i].value_handle == value_handle) {
            generation = s->subscriptions[i].generation;
            break;
        }
    }
    /* generation == 0 means no subscription is armed on this handle, or it was
     * disarmed while the notification was queued: stale either way. */
    app_ble_gatt_session_on_notify(s, generation, value_handle, data, data_len, truncated);
}

void app_ble_gatt_session_on_notify(app_ble_gatt_session_t *session, uint32_t generation,
                                    uint16_t value_handle, const uint8_t *data, size_t len,
                                    bool truncated)
{
    if (session == NULL || !session->initialized) {
        return;
    }
    if (generation != session->generation || session->phase != APP_BLE_PHASE_READY) {
        session->notifications_dropped++;
        return;
    }
    if (data == NULL && len != 0u) {
        session->notifications_dropped++;
        return;
    }
    for (size_t i = 0u; i < APP_BLE_NOTIFY_SLOTS; i++) {
        if (!session->subscriptions[i].in_use) {
            continue;
        }
        if (session->subscriptions[i].value_handle != value_handle) {
            continue;
        }
        session->notifications_seen++;
        if (session->subscriptions[i].callback != NULL) {
            session->subscriptions[i].callback(session->subscriptions[i].user, value_handle, data, len,
                                               truncated);
        }
        return;
    }
    /* Nothing subscribed on that handle: dropped, not dispatched. */
    session->notifications_dropped++;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

esp_err_t app_ble_gatt_session_init(app_ble_gatt_session_t *session,
                                    const app_ble_gatt_session_ops_t *ops, void *ops_ctx,
                                    const app_ble_gatt_session_config_t *config)
{
    esphome_ble_gatt_config_t gatt_config;
    esp_err_t err;

    if (session == NULL || ops == NULL || !ops_complete(ops)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(session, 0, sizeof(*session));
    session->ops = *ops;
    session->ops_ctx = ops_ctx;
    app_ble_gatt_session_config_default(&session->config);
    if (config != NULL) {
        session->config = *config;
        if (session->config.connect_timeout_ms == 0u) {
            session->config.connect_timeout_ms = APP_BLE_DEFAULT_CONNECT_TIMEOUT_MS;
        }
        if (session->config.operation_timeout_ms == 0u) {
            session->config.operation_timeout_ms = APP_BLE_DEFAULT_OPERATION_TIMEOUT_MS;
        }
    }
    db_reset(session);

    memset(&gatt_config, 0, sizeof(gatt_config));
    gatt_config.connect_timeout_ms = session->config.connect_timeout_ms;
    gatt_config.operation_timeout_ms = session->config.operation_timeout_ms;
    gatt_config.disconnect_timeout_ms = session->config.operation_timeout_ms;

    err = session->ops.gatt_init(session->ops_ctx, &gatt_config);
    if (err != ESP_OK) {
        session->phase = APP_BLE_PHASE_FAILED;
        session->last_status = APP_BLE_ERR_CONNECT_FAILED;
        session->last_native_error = err;
        return err;
    }
    session->initialized = true;
    session->phase = APP_BLE_PHASE_IDLE;
    session->generation = 1u; /* 0 is reserved for "no session has ever existed" */
    session->last_status = APP_BLE_OK;
    return ESP_OK;
}

app_ble_status_t app_ble_gatt_session_open(app_ble_gatt_session_t *session, const app_ble_peer_t *peer)
{
    esp_err_t err;

    if (session == NULL || peer == NULL) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (!session->initialized) {
        return APP_BLE_ERR_NOT_READY;
    }
    if (session->op_active) {
        return APP_BLE_ERR_BUSY;
    }
    if (session->phase == APP_BLE_PHASE_READY) {
        /* One session, one peer. Reopening means closing first, because a
         * second link would leave two peers claiming the same subscriptions. */
        return APP_BLE_ERR_BUSY;
    }

    db_reset(session);
    session->peer = *peer;
    session->phase = APP_BLE_PHASE_CONNECTING;

    if (session->ops.radio_acquire != NULL) {
        err = session->ops.radio_acquire(session->ops_ctx);
        if (err != ESP_OK) {
            session->phase = APP_BLE_PHASE_FAILED;
            session->last_status = APP_BLE_ERR_CONNECT_FAILED;
            session->last_native_error = err;
            return session->last_status;
        }
        session->radio_held = true;
    }

    if (begin(session, session->config.connect_timeout_ms) != ESP_OK) {
        app_ble_status_t status = APP_BLE_ERR_BUSY;
        session->last_status = status;
        return status;
    }
    err = session->ops.gatt_connect(session->ops_ctx, peer);
    {
        app_ble_status_t status = outcome(session, err, APP_BLE_ERR_CONNECT_FAILED);
        finish(session);
        if (status != APP_BLE_OK) {
            session->phase = APP_BLE_PHASE_FAILED;
            if (session->ops.radio_release != NULL && session->radio_held) {
                session->ops.radio_release(session->ops_ctx);
                session->radio_held = false;
            }
            return status;
        }
    }
    session->connected = true;

    /* Discovery is part of opening: a session without a database cannot resolve a
     * handle, and a caller that forgets to ask would get NOT_FOUND forever. */
    if (begin(session, session->config.operation_timeout_ms) != ESP_OK) {
        app_ble_gatt_session_close(session);
        return APP_BLE_ERR_BUSY;
    }
    err = session->ops.gatt_discover(session->ops_ctx, &session->db);
    {
        app_ble_status_t status = outcome(session, err, APP_BLE_ERR_DISCOVER_FAILED);
        finish(session);
        if (status != APP_BLE_OK) {
            session->phase = APP_BLE_PHASE_FAILED;
            (void)app_ble_gatt_session_close(session);
            return status;
        }
    }

    session->phase = APP_BLE_PHASE_READY;
    session->last_status = APP_BLE_OK;
    return APP_BLE_OK;
}

app_ble_status_t app_ble_gatt_session_cancel(app_ble_gatt_session_t *session)
{
    if (session == NULL) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (!session->initialized) {
        return APP_BLE_ERR_NOT_READY;
    }

    /* Bumping the generation first is what makes a cancel authoritative: any
     * operation still in flight will now report CANCELLED whatever the backend
     * returns, and any notification still queued is dropped. */
    session->generation++;
    session->cancellations++;
    if (session->op_active) {
        session->op_cancelled = true;
    }
    session->ops.gatt_cancel(session->ops_ctx);
    session->last_native_error = session->ops.gatt_last_error(session->ops_ctx);

    session->connected = session->ops.gatt_is_connected(session->ops_ctx);
    if (!session->connected && session->phase == APP_BLE_PHASE_READY) {
        session->phase = APP_BLE_PHASE_FAILED;
    }
    session->last_status = APP_BLE_ERR_CANCELLED;
    return APP_BLE_OK;
}

app_ble_status_t app_ble_gatt_session_close(app_ble_gatt_session_t *session)
{
    app_ble_status_t result = APP_BLE_OK;
    esp_err_t err;

    if (session == NULL) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (!session->initialized || session->phase == APP_BLE_PHASE_CLOSED) {
        return APP_BLE_OK; /* idempotent */
    }

    /* Anything in flight is cancelled before the link goes away, so the caller
     * cannot be left waiting on a completion that will never be meaningful. */
    if (session->op_active) {
        (void)app_ble_gatt_session_cancel(session);
    }

    for (size_t i = 0u; i < APP_BLE_NOTIFY_SLOTS; i++) {
        if (!session->subscriptions[i].in_use) {
            continue;
        }
        err = session->ops.gatt_unsubscribe(session->ops_ctx, session->subscriptions[i].value_handle);
        if (err != ESP_OK && result == APP_BLE_OK) {
            result = APP_BLE_ERR_UNSUPPORTED;
        }
        session->subscriptions[i].in_use = false;
        session->subscriptions[i].callback = NULL;
        session->subscriptions[i].user = NULL;
    }

    if (session->connected) {
        err = session->ops.gatt_disconnect(session->ops_ctx);
        if (err != ESP_OK && result == APP_BLE_OK) {
            /* The teardown did not complete cleanly. Report it, but still end the
             * session: a session that cannot be ended is worse than one that ends
             * with a diagnostic on the record. */
            session->last_native_error = session->ops.gatt_last_error(session->ops_ctx);
            result = (err == ESP_ERR_TIMEOUT) ? APP_BLE_ERR_TIMEOUT : APP_BLE_ERR_PEER_GONE;
        }
    }
    session->connected = session->ops.gatt_is_connected(session->ops_ctx);

    if (session->ops.radio_release != NULL && session->radio_held) {
        session->ops.radio_release(session->ops_ctx);
        session->radio_held = false;
    }

    /* Invalidate the session before declaring it closed, so a callback that
     * arrives during this function's tail is already stale. */
    session->generation++;
    /* A session that failed stays failed: "closed" must not launder a failure
     * into a clean end. */
    if (session->phase != APP_BLE_PHASE_FAILED) {
        session->phase = APP_BLE_PHASE_CLOSED;
    }
    if (session->op_active) {
        session->op_cancelled = true;
    }
    session->last_status = result;
    return result;
}

bool app_ble_gatt_session_tick(app_ble_gatt_session_t *session, uint32_t now_ms)
{
    if (session == NULL || !session->initialized) {
        return false;
    }
    if (!session->op_active) {
        return false;
    }
    if ((uint32_t)(now_ms - session->op_started_ms) < session->op_timeout_ms) {
        return false;
    }

    /* The deadline is authoritative for the same reason cancel is: the backend
     * cannot be asked politely, and its eventual return value is not the
     * application's outcome. */
    session->timeouts++;
    session->op_cancelled = true;
    session->generation++;
    session->ops.gatt_cancel(session->ops_ctx);
    session->last_native_error = session->ops.gatt_last_error(session->ops_ctx);
    session->connected = session->ops.gatt_is_connected(session->ops_ctx);
    /* The operation was abandoned because the session could not be trusted to
     * finish it, so the session is failed whether or not the link still answers
     * liveness. A caller reconnects deliberately. */
    session->phase = APP_BLE_PHASE_FAILED;
    session->last_status = APP_BLE_ERR_TIMEOUT;
    return true;
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

app_ble_status_t app_ble_gatt_session_read(app_ble_gatt_session_t *session, uint16_t handle,
                                           uint8_t *out, size_t capacity, size_t *out_len)
{
    esp_err_t err;
    app_ble_status_t status;

    if (session == NULL || out == NULL || out_len == NULL || capacity == 0u) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    *out_len = 0u;
    if (!session->initialized) {
        return APP_BLE_ERR_NOT_READY;
    }
    if (session->phase != APP_BLE_PHASE_READY || !session->connected) {
        return APP_BLE_ERR_NOT_READY;
    }
    if (begin(session, session->config.operation_timeout_ms) != ESP_OK) {
        return APP_BLE_ERR_BUSY;
    }

    err = session->ops.gatt_read(session->ops_ctx, handle, out, capacity, out_len);
    status = outcome(session, err, APP_BLE_ERR_READ_FAILED);
    if (status == APP_BLE_OK) {
        /* A backend that reports success with a length beyond the buffer is
         * lying about the buffer; clamping is safer than trusting it. */
        if (*out_len > capacity) {
            *out_len = capacity;
            status = APP_BLE_ERR_TRUNCATED;
        }
    } else if (status == APP_BLE_ERR_CANCELLED) {
        /* Never hand back a payload produced after the caller gave up. */
        memset(out, 0, capacity);
        *out_len = 0u;
    }
    finish(session);
    session->last_status = status;
    return status;
}

app_ble_status_t app_ble_gatt_session_write(app_ble_gatt_session_t *session, uint16_t handle,
                                            const uint8_t *data, size_t len, bool response)
{
    esp_err_t err;
    app_ble_status_t status;

    if (session == NULL || (data == NULL && len != 0u)) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (!session->initialized) {
        return APP_BLE_ERR_NOT_READY;
    }
    if (session->phase != APP_BLE_PHASE_READY || !session->connected) {
        return APP_BLE_ERR_NOT_READY;
    }
    if (len > APP_BLE_VALUE_MAX) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (begin(session, session->config.operation_timeout_ms) != ESP_OK) {
        return APP_BLE_ERR_BUSY;
    }

    err = session->ops.gatt_write(session->ops_ctx, handle, data, len, response);
    status = outcome(session, err, APP_BLE_ERR_WRITE_FAILED);
    finish(session);
    session->last_status = status;
    return status;
}

app_ble_status_t app_ble_gatt_session_subscribe(app_ble_gatt_session_t *session, uint16_t value_handle,
                                               uint16_t cccd_handle, app_ble_notify_fn callback,
                                               void *user, bool indications)
{
    esp_err_t err;
    app_ble_status_t status;
    size_t slot = APP_BLE_NOTIFY_SLOTS;

    if (session == NULL || value_handle == 0u || cccd_handle == 0u || callback == NULL) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (!session->initialized) {
        return APP_BLE_ERR_NOT_READY;
    }
    if (session->phase != APP_BLE_PHASE_READY || !session->connected) {
        return APP_BLE_ERR_NOT_READY;
    }
    for (size_t i = 0u; i < APP_BLE_NOTIFY_SLOTS; i++) {
        if (!session->subscriptions[i].in_use) {
            if (slot == APP_BLE_NOTIFY_SLOTS) {
                slot = i;
            }
            continue;
        }
        if (session->subscriptions[i].value_handle == value_handle) {
            /* Subscribing twice on one handle is a caller bug, and overwriting
             * the slot would leave the device subscribed with no way to undo it. */
            return APP_BLE_ERR_BUSY;
        }
    }
    if (slot == APP_BLE_NOTIFY_SLOTS) {
        return APP_BLE_ERR_UNSUPPORTED;
    }
    if (begin(session, session->config.operation_timeout_ms) != ESP_OK) {
        return APP_BLE_ERR_BUSY;
    }

    err = session->ops.gatt_subscribe(session->ops_ctx, value_handle, cccd_handle, indications,
                                      notify_bridge, session);
    status = outcome(session, err, APP_BLE_ERR_SUBSCRIBE_FAILED);
    if (status == APP_BLE_OK) {
        session->subscriptions[slot].in_use = true;
        session->subscriptions[slot].value_handle = value_handle;
        session->subscriptions[slot].cccd_handle = cccd_handle;
        session->subscriptions[slot].callback = callback;
        session->subscriptions[slot].user = user;
        session->subscriptions[slot].generation = session->generation;
    }
    finish(session);
    session->last_status = status;
    return status;
}

app_ble_status_t app_ble_gatt_session_unsubscribe(app_ble_gatt_session_t *session, uint16_t value_handle)
{
    esp_err_t err;
    app_ble_status_t status;
    size_t slot = APP_BLE_NOTIFY_SLOTS;

    if (session == NULL || value_handle == 0u) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (!session->initialized) {
        return APP_BLE_ERR_NOT_READY;
    }
    for (size_t i = 0u; i < APP_BLE_NOTIFY_SLOTS; i++) {
        if (session->subscriptions[i].in_use && session->subscriptions[i].value_handle == value_handle) {
            slot = i;
            break;
        }
    }
    if (slot == APP_BLE_NOTIFY_SLOTS) {
        return APP_BLE_ERR_NOT_FOUND;
    }

    /* Disarm before the backend call: from here on a notification for this handle
     * has no callback to reach, even if one is already in flight. */
    session->subscriptions[slot].in_use = false;
    if (begin(session, session->config.operation_timeout_ms) != ESP_OK) {
        session->subscriptions[slot].in_use = true;
        return APP_BLE_ERR_BUSY;
    }
    err = session->ops.gatt_unsubscribe(session->ops_ctx, value_handle);
    status = outcome(session, err, APP_BLE_ERR_UNSUPPORTED);
    finish(session);
    /* The slot stays disarmed either way: after an unsubscribe attempt the
     * application must not be handed notifications it can no longer stop. */
    session->subscriptions[slot].callback = NULL;
    session->subscriptions[slot].user = NULL;
    session->subscriptions[slot].generation = 0u;
    session->last_status = status;
    return status;
}

/* ------------------------------------------------------------------ */
/* handle resolution and the value codec                               */
/* ------------------------------------------------------------------ */

app_ble_status_t app_ble_gatt_resolve(const app_ble_db_t *db, uint32_t characteristic_index,
                                      uint16_t *out_value_handle, uint16_t *out_cccd_handle)
{
    const esphome_ble_gatt_characteristic_t *ch;

    if (db == NULL || db->characteristics == NULL || out_value_handle == NULL) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (characteristic_index >= db->characteristic_count) {
        return APP_BLE_ERR_NOT_FOUND;
    }
    ch = &db->characteristics[characteristic_index];
    if (ch->value_handle == 0u) {
        return APP_BLE_ERR_NOT_FOUND;
    }
    *out_value_handle = ch->value_handle;

    if (out_cccd_handle == NULL) {
        return APP_BLE_OK;
    }
    *out_cccd_handle = 0u;
    /* The CCCD lives in this characteristic's own descriptor range. It is found by
     * walking that range rather than by trusting an adjacent handle, because the
     * gap between the value handle and the CCCD is not fixed. */
    if (db->descriptors != NULL && ch->descriptor_count != 0u) {
        for (uint16_t i = 0u; i < ch->descriptor_count; i++) {
            uint16_t index = (uint16_t)(ch->first_descriptor + i);
            if (index >= db->descriptor_count) {
                break;
            }
            if (db->descriptors[index].uuid.width == ESPHOME_BLE_UUID16) {
                uint16_t uuid = (uint16_t)(db->descriptors[index].uuid.value[0] |
                                           ((uint16_t)db->descriptors[index].uuid.value[1] << 8));
                if (uuid == 0x2902u) { /* Client Characteristic Configuration */
                    *out_cccd_handle = db->descriptors[index].handle;
                    return APP_BLE_OK;
                }
            }
        }
    }
    return APP_BLE_ERR_NOT_FOUND;
}

static bool parse_float(const char *text, float *out)
{
    char *end = NULL;
    double value;

    if (text == NULL || *text == '\0') {
        return false;
    }
    value = strtod(text, &end);
    if (end == text) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = (float)value;
    return true;
}

app_ble_status_t app_ble_gatt_encode_action(const char *action, const char *value, float scale,
                                            uint8_t *out, size_t capacity, size_t *out_len)
{
    float scaled;

    if (action == NULL || out == NULL || out_len == NULL) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    *out_len = 0u;
    if (scale == 0.0f) {
        scale = 1.0f;
    }

    if (strcmp(action, "turn_on") == 0 || strcmp(action, "turn_off") == 0) {
        if (capacity < 1u) {
            return APP_BLE_ERR_UNSUPPORTED;
        }
        out[0] = (strcmp(action, "turn_on") == 0) ? 0x01u : 0x00u;
        *out_len = 1u;
        return APP_BLE_OK;
    }
    if (strcmp(action, "set_value") == 0) {
        if (!parse_float(value, &scaled)) {
            return APP_BLE_ERR_UNSUPPORTED;
        }
        scaled *= scale;
        {
            long rounded = (long)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
            if (rounded < 0) {
                /* A negative register value is not something the corpus can express
                 * today; refuse instead of wrapping into a huge unsigned. */
                return APP_BLE_ERR_UNSUPPORTED;
            }
            if (rounded <= 0xFFL) {
                if (capacity < 1u) {
                    return APP_BLE_ERR_UNSUPPORTED;
                }
                out[0] = (uint8_t)rounded;
                *out_len = 1u;
                return APP_BLE_OK;
            }
            if (rounded <= 0xFFFFL) {
                if (capacity < 2u) {
                    return APP_BLE_ERR_UNSUPPORTED;
                }
                out[0] = (uint8_t)(rounded & 0xFFL);
                out[1] = (uint8_t)((rounded >> 8) & 0xFFL);
                *out_len = 2u;
                return APP_BLE_OK;
            }
        }
        return APP_BLE_ERR_UNSUPPORTED;
    }
    if (strcmp(action, "set_text") == 0) {
        size_t len;
        if (value == NULL) {
            return APP_BLE_ERR_UNSUPPORTED;
        }
        len = strlen(value);
        if (len > capacity) {
            return APP_BLE_ERR_TRUNCATED;
        }
        memcpy(out, value, len);
        *out_len = len;
        return APP_BLE_OK;
    }
    return APP_BLE_ERR_UNSUPPORTED;
}

app_ble_status_t app_ble_gatt_decode_state(const uint8_t *data, size_t len, float scale,
                                           char *out, size_t capacity)
{
    long value;

    if (data == NULL || out == NULL || capacity == 0u) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    if (scale == 0.0f) {
        scale = 1.0f;
    }
    if (len == 1u) {
        value = (long)data[0];
    } else if (len == 2u) {
        value = (long)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    } else {
        /* Anything else is not a numeric register. It is reported as unsupported
         * rather than rendered as a number the user would believe. */
        return APP_BLE_ERR_UNSUPPORTED;
    }
    {
        float scaled = (float)value / scale;
        if (scaled < 0.0f) {
            return APP_BLE_ERR_UNSUPPORTED;
        }
        if (scaled == (float)(long)scaled) {
            (void)snprintf(out, capacity, "%ld", (long)scaled);
        } else {
            (void)snprintf(out, capacity, "%.2f", (double)scaled);
        }
    }
    return APP_BLE_OK;
}
