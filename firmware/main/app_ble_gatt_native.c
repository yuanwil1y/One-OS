/*
 * Firmware binding: app_ble_gatt session -> esphome_l2 GATT transport.
 *
 * Why every hook is written out rather than macro-generated: the interesting
 * behaviour of this file is the four conversions listed in the header (the radio
 * pair, the shared context pointer, the notification's opaque user pointer, and
 * the difference between "unsupported" and "failed"), and a macro table would hide
 * exactly the lines a reader needs to check.
 *
 * The adapter is a pure translator. It makes no policy decision: it does not decide
 * whether a native error is fatal, because the session owns the phase machine and a
 * transport error the session treats as recoverable must not become a session
 * failure here.
 */
#include "app_ble_gatt_native.h"

#include <string.h>

#include "app_ble_addr.h"
#include "esp_timer.h"

/* Records the transport's own error code for a failed call. A success clears it,
 * so the session always reads the code belonging to the operation it just saw. */
static esp_err_t note(app_ble_gatt_native_t *self, esp_err_t err)
{
    if (err != ESP_OK) {
        self->last_native_error = esphome_ble_gatt_last_native_error(self->transport);
        self->failed_operations++;
    } else {
        self->last_native_error = 0;
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* radio arbitration                                                   */
/* ------------------------------------------------------------------ */

/*
 * The session's acquire/release and the transport's own suspend/resume are the
 * same intent - "this session wants the radio to itself" - reached through two
 * interfaces, and the order is fixed: esphome_ble_gatt_connect() suspends the
 * radio itself, after the session has already acquired it. Both calls therefore
 * reach the platform's pair.
 *
 * That defines the contract on the platform's side: its suspend/resume must
 * tolerate a second suspend while already suspended for the same session and
 * resume only when the last holder lets go. A pair that is not idempotent - a
 * plain "turn the radio off" without a count - would put the radio back while the
 * session still believes it holds it, which is the failure mode that corrupts a
 * scan. The platform states which of the two it is by supplying the pair at all:
 * a platform with no arbiter supplies none, and this adapter then reports
 * arbitration as unsupported rather than pretending the radio is free.
 */
static esp_err_t nat_radio_acquire(void *ctx)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    if (!self->radio_available) {
        /* Not ESP_OK: reporting success for arbitration that never happened is
         * the one outcome the session must never see. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* The token belongs to the transport for this call and is discarded: the
     * transport keeps the one it needs for the suspend it performs inside
     * connect(), and the platform must not be handed two tokens for one hold. */
    {
        uintptr_t token = 0u;
        return self->radio_suspend(self->radio_ctx, &token);
    }
}

static esp_err_t nat_radio_release(void *ctx)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    if (!self->radio_available) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Resume with the token of the most recent suspend, which the platform
     * understands as "the holder is done" and not as "restore this exact token":
     * the transport may have resumed already on its own path, so the release has
     * to be a no-op when nothing is left to restore. */
    self->radio_resume(self->radio_ctx, 0u);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* transport operations                                                */
/* ------------------------------------------------------------------ */

static esp_err_t nat_gatt_init(void *ctx, const esphome_ble_gatt_config_t *config)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;
    esphome_ble_gatt_config_t effective;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* app_ble_gatt builds this config from the session config and its own
     * defaults and knows nothing about radio arbitration, so the pair and the
     * pointer the transport will hand it are added here (note 2 in the header).
     * The caller's config is copied rather than modified: it belongs to the
     * session, and a session that re-inits must not accumulate state from the
     * previous attempt. */
    effective = *config;
    effective.radio_suspend = self->radio_suspend;
    effective.radio_resume = self->radio_resume;
    /*
     * radio_user is the platform's own context, not the adapter.
     *
     * The pair is reached twice - once by the session's acquire/release, which the
     * adapter forwards with self->radio_ctx, and once by the transport itself
     * inside connect, which passes whatever radio_user says. Both calls land in
     * the same platform function, so both must carry the same context; handing the
     * transport the adapter instead would give one platform hook two different
     * `user` pointers and make it read its own state out of the wrong struct.
     */
    effective.radio_user = self->radio_ctx;

    return note(self, esphome_ble_gatt_init(self->transport, &effective));
}

static void nat_gatt_deinit(void *ctx)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    /* The transport's deinit returns nothing: it cannot fail. */
    esphome_ble_gatt_deinit(self->transport);
    self->last_native_error = 0;
}

static esp_err_t nat_gatt_connect(void *ctx, const app_ble_peer_t *peer)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;
    app_ble_peer_t controller_peer;

    if (peer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * This is the radio boundary: the session holds display order (see
     * app_ble_addr.h) and the transport hands the address to the controller, which
     * wants it least significant byte first. The conversion belongs here and
     * nowhere else in this path - the address arrived from scan evidence, which
     * already converted it once, and converting it twice is byte-for-byte the same
     * as not converting at all.
     *
     * address_type is not part of the address and is passed through unchanged.
     */
    controller_peer = *peer;
    (void)app_ble_addr_to_controller(peer->address, controller_peer.address);
    controller_peer.address_type = peer->address_type;

    return note(self, esphome_ble_gatt_connect(self->transport, &controller_peer));
}

static esp_err_t nat_gatt_discover(void *ctx, app_ble_db_t *db)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    if (db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return note(self, esphome_ble_gatt_discover(self->transport, db));
}

static esp_err_t nat_gatt_read(void *ctx, uint16_t handle, uint8_t *out, size_t capacity,
                               size_t *out_len)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    if (out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * The caller's buffer is cleared AFTER the call, not before.
     *
     * A read that fails may still have written part of the buffer - the real
     * backend does - so clearing first only guarantees that the last thing to touch
     * the buffer is the failure. A caller that logged the error and carried on
     * would then publish those bytes as a reading. Clearing once the call has
     * returned makes "the read failed" and "the value is empty" the same observable
     * state, whatever the transport left behind.
     *
     * A successful read is untouched: the value the transport wrote is the value
     * the caller gets.
     */
    {
        esp_err_t err = esphome_ble_gatt_read(self->transport, handle, out, capacity, out_len);
        if (err != ESP_OK) {
            memset(out, 0, capacity);
            if (out_len != NULL) {
                *out_len = 0u;
            }
        }
        return note(self, err);
    }
}

static esp_err_t nat_gatt_write(void *ctx, uint16_t handle, const uint8_t *data, size_t len,
                                bool response)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    if (data == NULL && len != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return note(self, esphome_ble_gatt_write(self->transport, handle, data, len, response));
}

static esp_err_t nat_gatt_subscribe(void *ctx, uint16_t value_handle, uint16_t cccd_handle,
                                    bool indications, esphome_ble_gatt_notify_fn callback, void *user)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    /* `user` is app_ble_gatt's notify_bridge, which is how a value handle becomes
     * the generation the subscription was made in (note 3 in the header). Nothing
     * is converted here: the two callback signatures already agree, and the
     * adapter passing the pointer straight through is what keeps the generation
     * guard in one place instead of two. */
    return note(self, esphome_ble_gatt_subscribe(self->transport, value_handle, cccd_handle,
                                                 indications, callback, user));
}

static esp_err_t nat_gatt_unsubscribe(void *ctx, uint16_t value_handle)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    return note(self, esphome_ble_gatt_unsubscribe(self->transport, value_handle));
}

static esp_err_t nat_gatt_disconnect(void *ctx)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    return note(self, esphome_ble_gatt_disconnect(self->transport));
}

static void nat_gatt_cancel(void *ctx)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    /* The one transport call documented as safe while an operation is
     * outstanding. There is no error to record: the abandoned operation reports
     * ESP_ERR_INVALID_STATE to whoever waits on it and the session decides what
     * that means (it reports CANCELLED). Recording a failure here would blame the
     * cancel for the operation it cancelled. */
    (void)esphome_ble_gatt_cancel(self->transport);
}

static bool nat_gatt_is_connected(void *ctx)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    return esphome_ble_gatt_is_connected(self->transport);
}

static int nat_gatt_last_error(void *ctx)
{
    app_ble_gatt_native_t *self = (app_ble_gatt_native_t *)ctx;

    /* Read from the adapter, not from the transport. By the time the session asks
     * - after a cancel, say - another operation may have run and replaced the
     * transport's slot, and the code the session wants is the one from the
     * operation that just failed. */
    return self->last_native_error;
}

uint32_t app_ble_gatt_native_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t nat_now_ms(void *ctx)
{
    (void)ctx;
    return app_ble_gatt_native_now_ms();
}

esp_err_t app_ble_gatt_native_install(app_ble_gatt_native_t *adapter,
                                      esphome_ble_gatt_session_t *transport,
                                      app_ble_gatt_session_ops_t *out)
{
    if (adapter == NULL || transport == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * The platform pair survives an install, so the caller may call set_radio()
     * before or after this. Reading the pair is therefore what decides
     * `radio_available` - deriving the flag from the previous flag would read a
     * byte this function has not written yet on a fresh adapter, and the pair and
     * the flag could disagree. A half-supplied pair is dropped entirely rather
     * than reported as available: acquire() would otherwise succeed while the
     * transport refuses to initialise.
     */
    {
        esphome_ble_gatt_radio_suspend_fn suspend = adapter->radio_suspend;
        esphome_ble_gatt_radio_resume_fn resume = adapter->radio_resume;
        void *radio_ctx = adapter->radio_ctx;

        memset(adapter, 0, sizeof(*adapter));
        if (suspend != NULL && resume != NULL) {
            adapter->radio_suspend = suspend;
            adapter->radio_resume = resume;
            adapter->radio_ctx = radio_ctx;
            adapter->radio_available = true;
        }
    }
    adapter->transport = transport;

    memset(out, 0, sizeof(*out));
    out->gatt_init = nat_gatt_init;
    out->gatt_deinit = nat_gatt_deinit;
    out->gatt_connect = nat_gatt_connect;
    out->gatt_discover = nat_gatt_discover;
    out->gatt_read = nat_gatt_read;
    out->gatt_write = nat_gatt_write;
    out->gatt_subscribe = nat_gatt_subscribe;
    out->gatt_unsubscribe = nat_gatt_unsubscribe;
    out->gatt_disconnect = nat_gatt_disconnect;
    out->gatt_cancel = nat_gatt_cancel;
    out->gatt_is_connected = nat_gatt_is_connected;
    out->gatt_last_error = nat_gatt_last_error;
    /* NULL rather than a stub that returns ESP_OK: the session treats a missing
     * arbiter as "no radio to take" and skips arbitration entirely, which is the
     * truth on a platform that has none. */
    out->radio_acquire = adapter->radio_available ? nat_radio_acquire : NULL;
    out->radio_release = adapter->radio_available ? nat_radio_release : NULL;
    out->now_ms = nat_now_ms;

    return ESP_OK;
}

void app_ble_gatt_native_set_radio(app_ble_gatt_native_t *adapter,
                                   esphome_ble_gatt_radio_suspend_fn suspend,
                                   esphome_ble_gatt_radio_resume_fn resume, void *radio_ctx)
{
    if (adapter == NULL) {
        return;
    }
    adapter->radio_ctx = radio_ctx;
    /* Both hooks or neither: the transport rejects a half-supplied pair, and a
     * half-supplied pair here would make acquire() succeed while the transport
     * refuses to initialise. */
    adapter->radio_available = (suspend != NULL && resume != NULL);
    adapter->radio_suspend = adapter->radio_available ? suspend : NULL;
    adapter->radio_resume = adapter->radio_available ? resume : NULL;
}
