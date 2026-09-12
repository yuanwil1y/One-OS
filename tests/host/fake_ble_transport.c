/*
 * The scripted NimBLE-level backend. See fake_ble_transport.h for why this test
 * drives the REAL transport through a fake backend rather than faking the
 * transport itself.
 */
#include "fake_ble_transport.h"

#include <stdio.h>
#include <string.h>

#include "app_ble_gatt.h"
#include "private/esphome_ble_gatt_internal.h"

/* The transport hands this backend its own storage, sized by
 * ESPHOME_BLE_GATT_BACKEND_BYTES, so the fake is a single instance living in that
 * space. One session at a time is all these tests need, so the live instance is
 * also remembered here for the few entry points that have no context argument -
 * delivering a notification is the one that matters. */
static fake_ble_backend_t *g_ctx;
static esphome_ble_gatt_backend_notify_fn g_notify;
static void *g_notify_session;

/*
 * The backend lives inside the transport's own storage, so it must fit. The
 * transport asserts this for its real backend; the fake has to assert it for
 * itself, because an oversized fake silently overwrites the transport's
 * subscription table - which is how this test first failed, with a memset of 280
 * bytes into a 224-byte slot.
 */
_Static_assert(sizeof(fake_ble_backend_t) <= ESPHOME_BLE_GATT_BACKEND_BYTES,
               "fake_ble_backend_t is larger than the transport's backend storage");

static fake_ble_backend_t *state_of(void *ctx)
{
    return (fake_ble_backend_t *)ctx;
}

void fake_ble_backend_reset_counts(void)
{
    fake_ble_backend_t *b = g_ctx;

    if (b == NULL) {
        return;
    }
    b->init_calls = 0;
    b->deinit_calls = 0;
    b->connect_calls = 0;
    b->discover_calls = 0;
    b->read_calls = 0;
    b->write_calls = 0;
    b->set_notify_calls = 0;
    b->cancel_calls = 0;
    b->disconnect_calls = 0;
    b->connected_queries = 0;
}

static esp_err_t fb_fail(fake_ble_backend_t *b, esp_err_t err, int *native)
{
    if (err != ESP_OK && native != NULL) {
        *native = b->native_error;
    }
    return err;
}

static esp_err_t fb_init(void *ctx, void *session, esphome_ble_gatt_backend_notify_fn fn,
                         int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    memset(b, 0, sizeof(*b));
    b->next_init = ESP_OK;
    b->next_connect = ESP_OK;
    b->next_discover = ESP_OK;
    b->next_read = ESP_OK;
    b->next_write = ESP_OK;
    b->next_set_notify = ESP_OK;
    b->next_cancel = ESP_OK;
    b->next_disconnect = ESP_OK;
    b->session = session;
    b->init_calls = 1;

    g_ctx = b;
    g_notify = fn;
    g_notify_session = session;

    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static void fb_deinit(void *ctx)
{
    fake_ble_backend_t *b = state_of(ctx);

    b->deinit_calls++;
    b->connected = false;
    b->notify_active = false;
    /* The transport wipes its own storage after calling this, so the callback it
     * registered must not be invoked again. */
    g_notify = NULL;
    g_notify_session = NULL;
    g_ctx = NULL;
}

static esp_err_t fb_connect(void *ctx, const esphome_ble_peer_t *peer, uint32_t timeout_ms,
                           int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    (void)peer;
    b->connect_calls++;
    b->connect_timeout_seen = timeout_ms;
    if (peer != NULL) {
        /* Recorded before the result check, so a test can see the byte order even on
         * a connect that is scripted to fail. */
        memcpy(b->connected_address, peer->address, sizeof(b->connected_address));
        b->connected_address_type = peer->address_type;
    }
    if (b->next_connect != ESP_OK) {
        return fb_fail(b, b->next_connect, native);
    }
    b->connected = true;
    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static esp_err_t fb_discover(void *ctx, esphome_ble_gatt_db_t *db, uint32_t timeout_ms, int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    (void)timeout_ms;
    b->discover_calls++;
    if (b->next_discover != ESP_OK) {
        return fb_fail(b, b->next_discover, native);
    }

    /* One service holding one characteristic with one descriptor: a CCCD. Enough
     * for a subscription, and small enough that the handle numbers in a test read
     * as themselves. Any table smaller than that is left empty rather than
     * half-filled, so a test that under-sizes its buffers sees an empty database
     * instead of a corrupt one. */
    if (db != NULL && db->services != NULL && db->service_capacity > 0u &&
        db->characteristics != NULL && db->characteristic_capacity > 0u &&
        db->descriptors != NULL && db->descriptor_capacity > 0u) {
        memset(&db->services[0], 0, sizeof(db->services[0]));
        db->service_count = 1u;
        db->services[0].start_handle = 0x0001u;
        db->services[0].end_handle = 0x0005u;
        db->services[0].first_characteristic = 0u;
        db->services[0].characteristic_count = 1u;
        db->services[0].uuid.width = ESPHOME_BLE_UUID16;
        db->services[0].uuid.value[0] = 0x0fu;
        db->services[0].uuid.value[1] = 0x18u;

        memset(&db->characteristics[0], 0, sizeof(db->characteristics[0]));
        db->characteristic_count = 1u;
        db->characteristics[0].definition_handle = 0x0002u;
        db->characteristics[0].value_handle = 0x0003u;
        db->characteristics[0].end_handle = 0x0004u;
        db->characteristics[0].properties = 0x12u; /* read | notify */
        db->characteristics[0].first_descriptor = 0u;
        db->characteristics[0].descriptor_count = 1u;
        db->characteristics[0].uuid.width = ESPHOME_BLE_UUID16;
        db->characteristics[0].uuid.value[0] = 0x19u;
        db->characteristics[0].uuid.value[1] = 0x2au;

        memset(&db->descriptors[0], 0, sizeof(db->descriptors[0]));
        db->descriptor_count = 1u;
        db->descriptors[0].handle = 0x0004u;
        db->descriptors[0].uuid.width = ESPHOME_BLE_UUID16;
        db->descriptors[0].uuid.value[0] = 0x02u;
        db->descriptors[0].uuid.value[1] = 0x29u;
        db->truncated = false;
    }
    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static esp_err_t fb_read(void *ctx, uint16_t handle, uint8_t *out, size_t capacity, size_t *out_len,
                         uint32_t timeout_ms, int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    (void)handle;
    (void)timeout_ms;
    b->read_calls++;

    if (b->cancel_session != NULL) {
        /* The race: another task cancels while this read is outstanding. The
         * session's cancel reaches the transport's cancel, which is the one call
         * the transport allows while an operation is in flight. */
        (void)app_ble_gatt_session_cancel((app_ble_gatt_session_t *)b->cancel_session);
    }

    if (b->next_read != ESP_OK) {
        /* A real read that fails may still have touched the buffer. That is
         * exactly what the adapter must defend against, so the fake does it. */
        if (out != NULL && capacity > 0u) {
            out[0] = 0xeeu;
        }
        if (out_len != NULL) {
            *out_len = capacity;
        }
        return fb_fail(b, b->next_read, native);
    }
    if (out != NULL && b->read_value_len <= capacity) {
        memcpy(out, b->read_value, b->read_value_len);
    }
    if (out_len != NULL) {
        *out_len = b->read_reply_len;
    }
    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static esp_err_t fb_write(void *ctx, uint16_t handle, const uint8_t *data, size_t len, bool response,
                          uint32_t timeout_ms, int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    (void)timeout_ms;
    b->write_calls++;
    if (b->next_write != ESP_OK) {
        return fb_fail(b, b->next_write, native);
    }
    b->written_handle = handle;
    b->written_response = response;
    b->written_len = (len <= sizeof(b->written)) ? len : sizeof(b->written);
    if (data != NULL && b->written_len > 0u) {
        memcpy(b->written, data, b->written_len);
    }
    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static esp_err_t fb_set_notify(void *ctx, uint16_t cccd_handle, bool on, bool indications,
                               uint32_t timeout_ms, int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    (void)timeout_ms;
    b->set_notify_calls++;
    if (b->next_set_notify != ESP_OK) {
        return fb_fail(b, b->next_set_notify, native);
    }
    b->last_cccd_handle = cccd_handle;
    b->last_notify_on = on;
    b->last_indications = indications;
    b->notify_active = on;
    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static esp_err_t fb_cancel(void *ctx, uint32_t timeout_ms, int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    b->cancel_calls++;
    b->disconnect_timeout_seen = timeout_ms;
    if (b->next_cancel != ESP_OK) {
        return fb_fail(b, b->next_cancel, native);
    }
    /* A cancel abandons the operation but leaves the link up, which is what a real
     * cancel does and what makes the transport's epoch guard necessary. */
    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static esp_err_t fb_disconnect(void *ctx, uint32_t timeout_ms, int *native)
{
    fake_ble_backend_t *b = state_of(ctx);

    b->disconnect_calls++;
    b->disconnect_timeout_seen = timeout_ms;
    if (b->next_disconnect != ESP_OK) {
        return fb_fail(b, b->next_disconnect, native);
    }
    b->connected = false;
    b->notify_active = false;
    if (native != NULL) {
        *native = 0;
    }
    return ESP_OK;
}

static bool fb_connected(const void *ctx)
{
    const fake_ble_backend_t *b = (const fake_ble_backend_t *)ctx;

    /* Widened deliberately: the counter is diagnostic only. */
    ((fake_ble_backend_t *)b)->connected_queries++;
    return b->connected;
}

const esphome_ble_gatt_backend_ops_t fake_ble_backend_ops = {
    .init = fb_init,
    .deinit = fb_deinit,
    .connect = fb_connect,
    .discover = fb_discover,
    .read = fb_read,
    .write = fb_write,
    .set_notify = fb_set_notify,
    .cancel = fb_cancel,
    .disconnect = fb_disconnect,
    .connected = fb_connected,
};

fake_ble_backend_t *fake_ble_backend_state(void)
{
    return g_ctx;
}

void fake_ble_backend_notify(uint16_t value_handle, const uint8_t *data, size_t len)
{
    if (g_notify != NULL) {
        g_notify(g_notify_session, value_handle, data, len, false);
    }
}

void fake_ble_backend_notify_truncated(uint16_t value_handle, const uint8_t *data, size_t len)
{
    if (g_notify != NULL) {
        g_notify(g_notify_session, value_handle, data, len, true);
    }
}
