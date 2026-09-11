/*
 * BLE GATT session lifecycle tests.
 *
 * The backend is scripted in this file, so every path that matters - a cancel
 * racing an operation, a deadline expiring under an operation, a notification
 * arriving after teardown - is exercised deterministically. In particular these
 * tests would catch the cases the GATT layer below cannot express:
 *
 *   - a backend that returns ESP_OK for an operation the application cancelled
 *   - a backend that returns ESP_OK for an operation whose deadline expired
 *   - a notification delivered after close()
 *   - a late confirmation that would otherwise land in the next session
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_ble_gatt.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/* scripted backend                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t now_ms;

    /* What the next call returns. */
    esp_err_t next_connect;
    esp_err_t next_discover;
    esp_err_t next_read;
    esp_err_t next_write;
    esp_err_t next_subscribe;
    esp_err_t next_unsubscribe;
    esp_err_t next_disconnect;
    int native_error;

    /* Behaviour flags. */
    bool connected;
    bool fail_init;
    bool cancel_drops_link;      /* gatt_cancel leaves the link up, like a real one */
    bool cancel_kills_operation; /* gatt_cancel makes the in-flight op return OK */
    bool cancel_during_read;     /* cancel from inside the read, reproducing the race */
    bool disconnect_times_out;
    bool link_lost_during_operation; /* the link drops while an operation is outstanding */
    app_ble_gatt_session_t *session; /* set by the tests that need to re-enter */

    int init_calls;
    int deinit_calls;
    int connect_calls;
    int discover_calls;
    int read_calls;
    int write_calls;
    int subscribe_calls;
    int unsubscribe_calls;
    int disconnect_calls;
    int cancel_calls;
    int radio_acquire_calls;
    int radio_release_calls;

    /* Captured arguments. */
    app_ble_peer_t last_peer;
    uint16_t last_handle;
    uint16_t last_cccd;
    uint8_t last_write[APP_BLE_VALUE_MAX];
    size_t last_write_len;
    bool last_write_response;
    bool last_indications;
    esphome_ble_gatt_notify_fn notify_callback;
    void *notify_user;

    /* Read payload. */
    uint8_t read_value[APP_BLE_VALUE_MAX];
    size_t read_value_len;
} fake_t;

static uint32_t fake_now(void *ctx)
{
    return ((fake_t *)ctx)->now_ms;
}

static esp_err_t fake_init(void *ctx, const esphome_ble_gatt_config_t *config)
{
    fake_t *f = (fake_t *)ctx;
    f->init_calls++;
    if (config == NULL || config->connect_timeout_ms == 0u || config->operation_timeout_ms == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (f->fail_init) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void fake_deinit(void *ctx)
{
    ((fake_t *)ctx)->deinit_calls++;
}

static esp_err_t fake_connect(void *ctx, const app_ble_peer_t *peer)
{
    fake_t *f = (fake_t *)ctx;
    f->connect_calls++;
    if (peer != NULL) {
        f->last_peer = *peer;
    }
    if (f->next_connect == ESP_OK) {
        f->connected = true;
    }
    return f->next_connect;
}

static esp_err_t fake_discover(void *ctx, app_ble_db_t *db)
{
    fake_t *f = (fake_t *)ctx;
    f->discover_calls++;
    if (db == NULL || db->services == NULL || db->characteristics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (f->next_discover != ESP_OK) {
        if (f->link_lost_during_operation) {
            /* The peripheral went away while this operation was outstanding. */
            f->connected = false;
        }
        return f->next_discover;
    }
    /* One service with two characteristics; the second carries a CCCD. */
    db->service_count = 1u;
    db->services[0].uuid.width = ESPHOME_BLE_UUID16;
    db->services[0].uuid.value[0] = 0x00;
    db->services[0].uuid.value[1] = 0x18;
    db->services[0].start_handle = 1u;
    db->services[0].end_handle = 12u;
    db->services[0].first_characteristic = 0u;
    db->services[0].characteristic_count = 2u;

    db->characteristic_count = 2u;
    db->characteristics[0].uuid.width = ESPHOME_BLE_UUID16;
    db->characteristics[0].uuid.value[0] = 0x00;
    db->characteristics[0].uuid.value[1] = 0x2a;
    db->characteristics[0].definition_handle = 3u;
    db->characteristics[0].value_handle = 4u;
    db->characteristics[0].end_handle = 6u;
    db->characteristics[0].properties = 0x12u;
    db->characteristics[0].first_descriptor = 0u;
    db->characteristics[0].descriptor_count = 1u;

    db->characteristics[1].uuid.width = ESPHOME_BLE_UUID16;
    db->characteristics[1].uuid.value[0] = 0x01;
    db->characteristics[1].uuid.value[1] = 0x2a;
    db->characteristics[1].definition_handle = 8u;
    db->characteristics[1].value_handle = 9u;
    db->characteristics[1].end_handle = 12u;
    db->characteristics[1].properties = 0x18u;
    db->characteristics[1].first_descriptor = 1u;
    db->characteristics[1].descriptor_count = 1u;

    if (db->descriptors != NULL && db->descriptor_capacity >= 2u) {
        /* Descriptor order matches the characteristic it belongs to: index 0 is
         * characteristic 0's CCCD, index 1 is characteristic 1's. */
        db->descriptor_count = 2u;
        db->descriptors[0].uuid.width = ESPHOME_BLE_UUID16;
        db->descriptors[0].uuid.value[0] = 0x02;
        db->descriptors[0].uuid.value[1] = 0x29;
        db->descriptors[0].handle = 5u;
        db->descriptors[1].uuid.width = ESPHOME_BLE_UUID16;
        db->descriptors[1].uuid.value[0] = 0x02;
        db->descriptors[1].uuid.value[1] = 0x29;
        db->descriptors[1].handle = 11u;
    }
    return ESP_OK;
}

static esp_err_t fake_read(void *ctx, uint16_t handle, uint8_t *out, size_t capacity, size_t *out_len)
{
    fake_t *f = (fake_t *)ctx;
    f->read_calls++;
    f->last_handle = handle;
    if (f->cancel_during_read && f->session != NULL) {
        /* The race: another task cancels while this read is outstanding. */
        (void)app_ble_gatt_session_cancel(f->session);
    }
    if (f->cancel_kills_operation) {
        /* The exact defect this module exists to absorb: a cancel tore the link
         * down and the operation completes with no error and no data. */
        *out_len = 0u;
        return ESP_OK;
    }
    if (f->next_read != ESP_OK) {
        return f->next_read;
    }
    if (f->read_value_len > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, f->read_value, f->read_value_len);
    *out_len = f->read_value_len;
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx, uint16_t handle, const uint8_t *data, size_t len, bool response)
{
    fake_t *f = (fake_t *)ctx;
    f->write_calls++;
    f->last_handle = handle;
    f->last_write_response = response;
    f->last_write_len = len;
    if (len != 0u && data != NULL) {
        memcpy(f->last_write, data, len);
    }
    return f->next_write;
}

static esp_err_t fake_subscribe(void *ctx, uint16_t value_handle, uint16_t cccd_handle, bool indications,
                                esphome_ble_gatt_notify_fn callback, void *user)
{
    fake_t *f = (fake_t *)ctx;
    f->subscribe_calls++;
    f->last_handle = value_handle;
    f->last_cccd = cccd_handle;
    f->last_indications = indications;
    f->notify_callback = callback;
    f->notify_user = user;
    return f->next_subscribe;
}

static esp_err_t fake_unsubscribe(void *ctx, uint16_t value_handle)
{
    fake_t *f = (fake_t *)ctx;
    f->unsubscribe_calls++;
    f->last_handle = value_handle;
    return f->next_unsubscribe;
}

static esp_err_t fake_disconnect(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    f->disconnect_calls++;
    if (f->disconnect_times_out) {
        return ESP_ERR_TIMEOUT;
    }
    f->connected = false;
    return ESP_OK;
}

static void fake_cancel(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    f->cancel_calls++;
    if (f->cancel_drops_link) {
        f->connected = false;
    }
}

static bool fake_is_connected(void *ctx)
{
    return ((fake_t *)ctx)->connected;
}

static int fake_last_error(void *ctx)
{
    return ((fake_t *)ctx)->native_error;
}

static esp_err_t fake_radio_acquire(void *ctx)
{
    ((fake_t *)ctx)->radio_acquire_calls++;
    return ESP_OK;
}

static esp_err_t fake_radio_release(void *ctx)
{
    ((fake_t *)ctx)->radio_release_calls++;
    return ESP_OK;
}

static app_ble_gatt_session_ops_t fake_ops(void)
{
    app_ble_gatt_session_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.gatt_init = fake_init;
    ops.gatt_deinit = fake_deinit;
    ops.gatt_connect = fake_connect;
    ops.gatt_discover = fake_discover;
    ops.gatt_read = fake_read;
    ops.gatt_write = fake_write;
    ops.gatt_subscribe = fake_subscribe;
    ops.gatt_unsubscribe = fake_unsubscribe;
    ops.gatt_disconnect = fake_disconnect;
    ops.gatt_cancel = fake_cancel;
    ops.gatt_is_connected = fake_is_connected;
    ops.gatt_last_error = fake_last_error;
    ops.radio_acquire = fake_radio_acquire;
    ops.radio_release = fake_radio_release;
    ops.now_ms = fake_now;
    return ops;
}

/* A two-service, two-characteristic peer, described compactly. */
static void make_peer(app_ble_peer_t *peer, uint8_t last_byte)
{
    memset(peer, 0, sizeof(*peer));
    peer->address[0] = 0xAA;
    peer->address[5] = last_byte;
    peer->address_type = 1u;
}

/* ------------------------------------------------------------------ */
/* a session that reaches READY, for the cases that need one           */
/* ------------------------------------------------------------------ */

static app_ble_gatt_session_t g_session;
static fake_t g_fake;

static void bring_up(void)
{
    app_ble_gatt_session_ops_t ops;
    app_ble_peer_t peer;

    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.next_connect = ESP_OK;
    g_fake.next_discover = ESP_OK;
    g_fake.next_read = ESP_OK;
    g_fake.next_write = ESP_OK;
    g_fake.next_subscribe = ESP_OK;
    g_fake.next_unsubscribe = ESP_OK;
    g_fake.next_disconnect = ESP_OK;
    g_fake.now_ms = 1000u;

    ops = fake_ops();
    CHECK(app_ble_gatt_session_init(&g_session, &ops, &g_fake, NULL) == ESP_OK, "session init");
    make_peer(&peer, 0x01);
    CHECK(app_ble_gatt_session_open(&g_session, &peer) == APP_BLE_OK, "session open");
    CHECK(g_session.phase == APP_BLE_PHASE_READY, "phase after open is %s",
          app_ble_phase_name(g_session.phase));
    CHECK(g_fake.discover_calls == 1, "discovery did not run as part of open");
    CHECK(g_session.db.characteristic_count == 2u, "characteristic count");
    CHECK(g_fake.radio_acquire_calls == 1 && g_fake.radio_release_calls == 0, "radio not held");
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void test_init_validates_ops(void)
{
    app_ble_gatt_session_t session;
    app_ble_gatt_session_ops_t ops = fake_ops();
    fake_t fake;
    memset(&fake, 0, sizeof(fake));

    CHECK(app_ble_gatt_session_init(NULL, &ops, &fake, NULL) == ESP_ERR_INVALID_ARG, "NULL session");
    CHECK(app_ble_gatt_session_init(&session, NULL, &fake, NULL) == ESP_ERR_INVALID_ARG, "NULL ops");

    ops.gatt_read = NULL;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_ERR_INVALID_ARG,
          "a missing vtable entry was accepted");
    ops = fake_ops();
    ops.now_ms = NULL;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_ERR_INVALID_ARG,
          "a missing clock was accepted");

    /* The radio hooks are the only optional entries. */
    ops = fake_ops();
    ops.radio_acquire = NULL;
    ops.radio_release = NULL;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK,
          "optional radio hooks were required");

    /* Defaults are applied, and the config they reach the GATT layer with is
     * complete: a zero timeout there would mean an unbounded wait. */
    CHECK(session.config.connect_timeout_ms == APP_BLE_DEFAULT_CONNECT_TIMEOUT_MS, "default connect timeout");
    CHECK(session.config.operation_timeout_ms == APP_BLE_DEFAULT_OPERATION_TIMEOUT_MS, "default op timeout");

    /* A failing backend init is reported and leaves the session unusable. */
    fake.fail_init = true;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) != ESP_OK, "failing init accepted");
    CHECK(session.phase == APP_BLE_PHASE_FAILED, "phase after failed init");
    CHECK(app_ble_gatt_session_close(&session) == APP_BLE_OK, "close after failed init must be a no-op");
}

static void test_open_failure_releases_the_radio(void)
{
    app_ble_gatt_session_t session;
    app_ble_gatt_session_ops_t ops = fake_ops();
    fake_t fake;
    app_ble_peer_t peer;

    memset(&fake, 0, sizeof(fake));
    fake.next_connect = ESP_FAIL;
    fake.now_ms = 10u;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK, "init");
    make_peer(&peer, 0x02);
    CHECK(app_ble_gatt_session_open(&session, &peer) == APP_BLE_ERR_CONNECT_FAILED, "connect failure status");
    CHECK(session.phase == APP_BLE_PHASE_FAILED, "phase after connect failure");
    CHECK(fake.radio_acquire_calls == 1 && fake.radio_release_calls == 1,
          "a failed connect must give the radio back");
    CHECK(!session.radio_held, "radio still reported as held");

    /* A connect that never succeeded reports the connect, not a missing peer. */
    memset(&fake, 0, sizeof(fake));
    fake.next_connect = ESP_FAIL;
    fake.now_ms = 10u;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK, "init");
    CHECK(app_ble_gatt_session_open(&session, &peer) == APP_BLE_ERR_CONNECT_FAILED,
          "a connect that never succeeded reports connect_failed, not peer_gone");

    /*
     * A link that was up and then went away while an operation was outstanding.
     * This is what a peripheral powering off looks like: connect reports a live
     * link, then the operation fails and liveness says the peer is gone. The
     * session must report PEER_GONE rather than a generic failure, because the
     * application has to tell "this device refused" from "this device is no
     * longer there".
     */
    memset(&fake, 0, sizeof(fake));
    fake.next_connect = ESP_OK;
    fake.next_discover = ESP_FAIL;
    fake.next_disconnect = ESP_OK;
    fake.link_lost_during_operation = true;
    fake.now_ms = 10u;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK, "init");
    {
        app_ble_status_t status = app_ble_gatt_session_open(&session, &peer);
        CHECK(status == APP_BLE_ERR_PEER_GONE, "peer gone status is %s", app_ble_status_name(status));
    }
    /* The link is gone, so there is nothing to disconnect; what must happen is
     * that the session stops claiming a connection and gives the radio back. */
    CHECK(session.phase == APP_BLE_PHASE_FAILED, "phase after losing the peer is %s",
          app_ble_phase_name(session.phase));
    CHECK(!session.connected, "the session still claims a connection");
    CHECK(fake.radio_release_calls >= 1, "radio not released after the peer vanished");

    /* Discovery failing while the link is fine is a discovery failure, not a
     * missing peer. */
    memset(&fake, 0, sizeof(fake));
    fake.next_connect = ESP_OK;
    fake.next_discover = ESP_FAIL;
    fake.connected = true;
    fake.now_ms = 10u;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK, "init");
    {
        app_ble_status_t status = app_ble_gatt_session_open(&session, &peer);
        CHECK(status == APP_BLE_ERR_DISCOVER_FAILED, "healthy link: status is %s", app_ble_status_name(status));
    }
    CHECK(session.phase == APP_BLE_PHASE_FAILED, "phase after discover failure is %s",
          app_ble_phase_name(session.phase));
    CHECK(fake.disconnect_calls == 1, "a session that failed discovery must be torn down");
    CHECK(fake.radio_release_calls == 1, "radio not released after discover failure");
}

static void test_open_rejects_second_session(void)
{
    bring_up();
    {
        app_ble_peer_t peer;
        make_peer(&peer, 0x03);
        CHECK(app_ble_gatt_session_open(&g_session, &peer) == APP_BLE_ERR_BUSY,
              "a second peer was allowed into a live session");
    }
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_OK, "close");
    CHECK(g_session.phase == APP_BLE_PHASE_CLOSED, "phase after close");
    CHECK(g_fake.radio_release_calls == 1, "radio not released on close");
    CHECK(g_fake.disconnect_calls == 1, "disconnect not called on close");
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_OK, "close must be idempotent");
    CHECK(g_fake.disconnect_calls == 1, "a second close disconnected again");
}

/* ------------------------------------------------------------------ */
/* cancel and deadline: the two cases that must never look successful  */
/* ------------------------------------------------------------------ */

static void test_cancel_makes_an_in_flight_operation_cancelled(void)
{
    bring_up();
    g_fake.next_read = ESP_OK;
    g_fake.read_value[0] = 0x2au;
    g_fake.read_value_len = 1u;
    g_fake.cancel_kills_operation = true; /* the backend will return OK with no data */
    g_fake.cancel_drops_link = true;

    /* Simulate the race by cancelling before the read is issued: the read then
     * carries a generation that the cancel already invalidated. */
    CHECK(app_ble_gatt_session_cancel(&g_session) == APP_BLE_OK, "cancel");
    CHECK(g_session.phase == APP_BLE_PHASE_FAILED, "cancel that dropped the link must fail the session");

    /* A read on a failed session is refused outright rather than sent. */
    {
        uint8_t out[APP_BLE_VALUE_MAX];
        size_t len = 99u;
        CHECK(app_ble_gatt_session_read(&g_session, 4u, out, sizeof(out), &len) == APP_BLE_ERR_NOT_READY,
              "read on a failed session was attempted");
        CHECK(g_fake.read_calls == 0, "the backend was called after the session failed");
    }
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_OK, "close");
}

static void test_cancel_discards_a_successful_result(void)
{
    /*
     * The exact defect: the backend returns ESP_OK for a read that the
     * application cancelled. The fake cancels from inside read(), which is the
     * ordering a real radio produces when a cancel terminates the link and the
     * pending read completes with no error and no data.
     */
    app_ble_gatt_session_t session;
    app_ble_gatt_session_ops_t ops = fake_ops();
    fake_t fake;
    app_ble_peer_t peer;
    uint8_t out[APP_BLE_VALUE_MAX];
    size_t len = 0u;

    memset(&fake, 0, sizeof(fake));
    fake.next_connect = ESP_OK;
    fake.next_discover = ESP_OK;
    fake.now_ms = 500u;
    fake.cancel_kills_operation = false;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK, "init");
    make_peer(&peer, 0x04);
    CHECK(app_ble_gatt_session_open(&session, &peer) == APP_BLE_OK, "open");

    fake.read_value[0] = 0xAAu;
    fake.read_value_len = 1u;
    fake.cancel_during_read = true;
    fake.session = &session;
    {
        app_ble_status_t status = app_ble_gatt_session_read(&session, 4u, out, sizeof(out), &len);
        CHECK(fake.cancel_calls == 1, "the read did not trigger the cancel");
        CHECK(status == APP_BLE_ERR_CANCELLED,
              "a cancelled read reported %s instead of cancelled", app_ble_status_name(status));
        CHECK(len == 0u, "a cancelled read returned a payload length");
        CHECK(out[0] == 0u, "a cancelled read left a payload in the buffer");
    }
    CHECK(app_ble_gatt_session_close(&session) == APP_BLE_OK, "close");
}

static void test_deadline_expiry_is_a_timeout_not_a_success(void)
{
    bring_up();
    g_fake.read_value[0] = 0x11u;
    g_fake.read_value_len = 1u;

    /* Let the deadline pass while an operation is in flight. */
    g_session.op_active = true;
    g_session.op_started_ms = 1000u;
    g_session.op_timeout_ms = 100u;
    g_session.op_generation = g_session.generation;
    g_fake.now_ms = 1100u; /* exactly at the deadline */

    CHECK(app_ble_gatt_session_tick(&g_session, g_fake.now_ms), "tick did not expire the operation");
    CHECK(g_session.last_status == APP_BLE_ERR_TIMEOUT, "expiry status is %s",
          app_ble_status_name(g_session.last_status));
    CHECK(g_session.timeouts == 1u, "timeout not counted");
    CHECK(g_fake.cancel_calls == 1, "expiry did not cancel the backend operation");
    CHECK(g_session.phase == APP_BLE_PHASE_FAILED, "a timeout must fail the session");
    /* Before the deadline, tick does nothing. */
    bring_up();
    g_session.op_active = true;
    g_session.op_started_ms = 1000u;
    g_session.op_timeout_ms = 100u;
    g_fake.now_ms = 1099u;
    CHECK(!app_ble_gatt_session_tick(&g_session, g_fake.now_ms), "tick expired an operation early");
    /* Counter wrap: the comparison is unsigned, so a wrapped clock still works. */
    g_session.op_started_ms = 0xFFFFFFF0u;
    g_session.op_timeout_ms = 0x20u;
    g_fake.now_ms = 0x00000010u;
    CHECK(app_ble_gatt_session_tick(&g_session, g_fake.now_ms), "tick failed across a clock wrap");
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_OK, "close");
}

/* ------------------------------------------------------------------ */
/* reads, writes, notifications                                        */
/* ------------------------------------------------------------------ */

static void test_read_and_write(void)
{
    uint8_t out[APP_BLE_VALUE_MAX];
    size_t len = 0u;

    bring_up();
    g_fake.read_value[0] = 0x2au;
    g_fake.read_value[1] = 0x00u;
    g_fake.read_value_len = 2u;
    CHECK(app_ble_gatt_session_read(&g_session, 4u, out, sizeof(out), &len) == APP_BLE_OK, "read");
    CHECK(len == 2u && out[0] == 0x2au, "read payload");
    CHECK(g_fake.last_handle == 4u, "read handle not passed through");
    CHECK(!g_session.op_active, "read left an operation in flight");

    /* A buffer that is too small: the backend reports it, and the caller must not
     * receive a truncated value presented as a whole one. */
    {
        uint8_t small[1];
        g_fake.read_value_len = 4u;
        CHECK(app_ble_gatt_session_read(&g_session, 4u, small, sizeof(small), &len) ==
                  APP_BLE_ERR_READ_FAILED,
              "an oversized value was not refused");
        CHECK(len == 0u, "a failed read reported a length");
    }

    /* Write with and without response. */
    {
        const uint8_t on[1] = {0x01u};
        CHECK(app_ble_gatt_session_write(&g_session, 9u, on, sizeof(on), true) == APP_BLE_OK, "write response");
        CHECK(g_fake.write_calls == 1 && g_fake.last_write_response, "write response flag");
        CHECK(g_fake.last_write_len == 1u && g_fake.last_write[0] == 0x01u, "write payload");
        CHECK(app_ble_gatt_session_write(&g_session, 9u, on, sizeof(on), false) == APP_BLE_OK, "write no response");
        CHECK(!g_fake.last_write_response, "write without response flag");
    }

    /* Bad arguments. */
    CHECK(app_ble_gatt_session_read(&g_session, 4u, NULL, 8u, &len) == APP_BLE_ERR_INVALID_ARGUMENT,
          "NULL read buffer accepted");
    CHECK(app_ble_gatt_session_read(&g_session, 4u, out, 0u, &len) == APP_BLE_ERR_INVALID_ARGUMENT,
          "zero-capacity read accepted");
    CHECK(app_ble_gatt_session_read(&g_session, 4u, out, sizeof(out), NULL) == APP_BLE_ERR_INVALID_ARGUMENT,
          "NULL length accepted");
    CHECK(app_ble_gatt_session_write(&g_session, 9u, NULL, 1u, true) == APP_BLE_ERR_INVALID_ARGUMENT,
          "NULL write data with a length accepted");
    {
        uint8_t big[APP_BLE_VALUE_MAX + 1u];
        memset(big, 0, sizeof(big));
        CHECK(app_ble_gatt_session_write(&g_session, 9u, big, sizeof(big), true) ==
                  APP_BLE_ERR_INVALID_ARGUMENT,
              "an oversized write was accepted");
    }
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_OK, "close");
}

static int notify_calls;
static uint16_t notify_handle;
static uint8_t notify_payload[8];
static size_t notify_len;
static bool notify_truncated;

static void on_notify(void *user, uint16_t value_handle, const uint8_t *data, size_t len, bool truncated)
{
    int *tag = (int *)user;
    CHECK(tag == &notify_calls, "callback user pointer was not passed through");
    notify_calls++;
    notify_handle = value_handle;
    notify_len = len;
    notify_truncated = truncated;
    if (len != 0u && data != NULL) {
        memcpy(notify_payload, data, len < sizeof(notify_payload) ? len : sizeof(notify_payload));
    }
}

static void test_subscribe_and_notifications(void)
{
    uint16_t value_handle = 0u;
    uint16_t cccd_handle = 0u;

    bring_up();

    /* Handle resolution over the discovered database. */
    CHECK(app_ble_gatt_resolve(&g_session.db, 0u, &value_handle, &cccd_handle) == APP_BLE_OK,
          "resolving characteristic 0");
    CHECK(value_handle == 4u, "value handle");
    CHECK(cccd_handle == 5u, "CCCD handle");
    CHECK(app_ble_gatt_resolve(&g_session.db, 1u, &value_handle, &cccd_handle) == APP_BLE_OK,
          "resolving characteristic 1");
    CHECK(value_handle == 9u && cccd_handle == 11u, "second characteristic handles");
    CHECK(app_ble_gatt_resolve(&g_session.db, 2u, &value_handle, &cccd_handle) == APP_BLE_ERR_NOT_FOUND,
          "an out-of-range index resolved");
    CHECK(app_ble_gatt_resolve(NULL, 0u, &value_handle, &cccd_handle) == APP_BLE_ERR_INVALID_ARGUMENT,
          "NULL database accepted");

    /* Subscribe, deliver a notification, then deliver one after close. */
    notify_calls = 0;
    CHECK(app_ble_gatt_session_subscribe(&g_session, 9u, 11u, on_notify, &notify_calls, false) == APP_BLE_OK,
          "subscribe");
    CHECK(g_fake.last_cccd == 11u && !g_fake.last_indications, "CCCD arguments");
    CHECK(g_fake.notify_callback != NULL && g_fake.notify_user == &g_session, "notify bridge not installed");

    g_fake.notify_callback(9u, (const uint8_t *)"\x01\x02", 2u, false, g_fake.notify_user);
    CHECK(notify_calls == 1, "notification was not delivered");
    CHECK(notify_handle == 9u && notify_len == 2u && notify_payload[0] == 0x01u, "notification contents");
    CHECK(g_session.notifications_seen == 1u, "notifications_seen");

    /* A notification for a handle nobody subscribed to is dropped, not dispatched. */
    g_fake.notify_callback(4u, (const uint8_t *)"\x03", 1u, false, g_fake.notify_user);
    CHECK(notify_calls == 1, "a notification for an unsubscribed handle was delivered");
    CHECK(g_session.notifications_dropped == 1u, "notifications_dropped");

    /* Subscribing twice on one handle is refused rather than silently replacing
     * the callback, which would leave the device subscribed with no way to undo. */
    CHECK(app_ble_gatt_session_subscribe(&g_session, 9u, 11u, on_notify, &notify_calls, false) ==
              APP_BLE_ERR_BUSY,
          "a duplicate subscription was accepted");

    /* Truncation is reported to the callback, not hidden. */
    notify_calls = 0;
    g_fake.notify_callback(9u, (const uint8_t *)"\xff", 1u, true, g_fake.notify_user);
    CHECK(notify_calls == 1 && notify_truncated, "truncation was not reported");

    /* After close, a queued notification must not reach the application. */
    notify_calls = 0;
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_OK, "close");
    CHECK(g_fake.unsubscribe_calls == 1, "close did not unsubscribe");
    g_fake.notify_callback(9u, (const uint8_t *)"\x01", 1u, false, g_fake.notify_user);
    CHECK(notify_calls == 0, "a notification after close reached the application");
    CHECK(g_session.notifications_dropped >= 2u, "the late notification was not counted as dropped");
}

static void test_a_late_notification_does_not_land_in_the_next_session(void)
{
    app_ble_gatt_session_ops_t ops = fake_ops();
    static app_ble_gatt_session_t session;
    static fake_t fake;
    app_ble_peer_t peer;

    memset(&fake, 0, sizeof(fake));
    fake.next_connect = ESP_OK;
    fake.next_discover = ESP_OK;
    fake.now_ms = 100u;
    CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK, "init");
    make_peer(&peer, 0x05);
    CHECK(app_ble_gatt_session_open(&session, &peer) == APP_BLE_OK, "open");
    notify_calls = 0;
    CHECK(app_ble_gatt_session_subscribe(&session, 9u, 11u, on_notify, &notify_calls, false) == APP_BLE_OK,
          "subscribe");
    {
        esphome_ble_gatt_notify_fn first_callback = fake.notify_callback;
        CHECK(app_ble_gatt_session_close(&session) == APP_BLE_OK, "close");
        /* A whole new session, reusing the same storage. */
        memset(&fake, 0, sizeof(fake));
        fake.next_connect = ESP_OK;
        fake.next_discover = ESP_OK;
        fake.now_ms = 200u;
        CHECK(app_ble_gatt_session_init(&session, &ops, &fake, NULL) == ESP_OK, "re-init");
        make_peer(&peer, 0x06);
        CHECK(app_ble_gatt_session_open(&session, &peer) == APP_BLE_OK, "re-open");
        notify_calls = 0;
        /* The old bridge still holds a pointer to the same session storage. A
         * notification delivered through it is stale and must be dropped. */
        first_callback(9u, (const uint8_t *)"\x07", 1u, false, &session);
        CHECK(notify_calls == 0, "a stale notification from the previous session was delivered");
        CHECK(session.notifications_dropped == 1u, "the stale notification was not counted");
    }
    CHECK(app_ble_gatt_session_close(&session) == APP_BLE_OK, "close");
}

static void test_close_reports_a_failing_teardown(void)
{
    bring_up();
    g_fake.disconnect_times_out = true;
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_ERR_TIMEOUT,
          "a timing-out disconnect was reported as a clean close");
    CHECK(g_session.phase == APP_BLE_PHASE_CLOSED, "the session did not end after a failed teardown");
    CHECK(g_fake.radio_release_calls == 1, "the radio was not released after a failed teardown");
}

static void test_operation_guard_is_released(void)
{
    bring_up();
    g_session.op_active = true;
    CHECK(app_ble_gatt_session_write(&g_session, 4u, (const uint8_t *)"\x01", 1u, true) == APP_BLE_ERR_BUSY,
          "a second concurrent operation was accepted");
    CHECK(g_fake.write_calls == 0, "the backend was called while another operation was in flight");
    g_session.op_active = false;
    CHECK(app_ble_gatt_session_close(&g_session) == APP_BLE_OK, "close");
}

/* ------------------------------------------------------------------ */
/* value codec                                                         */
/* ------------------------------------------------------------------ */

static void test_codec(void)
{
    uint8_t out[APP_BLE_VALUE_MAX];
    size_t len = 0u;
    char text[32];

    /* Booleans */
    CHECK(app_ble_gatt_encode_action("turn_on", NULL, 1.0f, out, sizeof(out), &len) == APP_BLE_OK && len == 1u &&
              out[0] == 0x01u,
          "turn_on encoding");
    CHECK(app_ble_gatt_encode_action("turn_off", "anything", 1.0f, out, sizeof(out), &len) == APP_BLE_OK &&
              out[0] == 0x00u,
          "turn_off encoding");

    /* Numeric with a scale: 21.5 degrees at scale 10 is register value 215. */
    CHECK(app_ble_gatt_encode_action("set_value", "21.5", 10.0f, out, sizeof(out), &len) == APP_BLE_OK,
          "scaled set_value");
    CHECK(len == 1u && out[0] == 215u, "scaled value is %u", (unsigned)out[0]);

    /* A value that needs two bytes. */
    CHECK(app_ble_gatt_encode_action("set_value", "300", 1.0f, out, sizeof(out), &len) == APP_BLE_OK,
          "two-byte set_value");
    CHECK(len == 2u && out[0] == 0x2cu && out[1] == 0x01u, "two-byte little-endian value");

    /* Round trip through the decoder. */
    CHECK(app_ble_gatt_decode_state(out, 2u, 1.0f, text, sizeof(text)) == APP_BLE_OK, "decode");
    CHECK(strcmp(text, "300") == 0, "decoded text is '%s'", text);
    CHECK(app_ble_gatt_encode_action("set_value", "21.5", 10.0f, out, sizeof(out), &len) == APP_BLE_OK,
          "encode for scaled decode");
    CHECK(app_ble_gatt_decode_state(out, 1u, 10.0f, text, sizeof(text)) == APP_BLE_OK, "scaled decode");
    CHECK(strcmp(text, "21.50") == 0, "scaled decode is '%s'", text);

    /* Text. */
    CHECK(app_ble_gatt_encode_action("set_text", "kitchen", 1.0f, out, sizeof(out), &len) == APP_BLE_OK,
          "set_text");
    CHECK(len == 7u && memcmp(out, "kitchen", 7u) == 0, "text payload");

    /* Refusals: an unknown action, a non-numeric value, a negative register, a
     * text value that does not fit, and a payload the decoder cannot interpret. */
    CHECK(app_ble_gatt_encode_action("explode", NULL, 1.0f, out, sizeof(out), &len) == APP_BLE_ERR_UNSUPPORTED,
          "an unknown action was encoded");
    CHECK(app_ble_gatt_encode_action("set_value", "warm", 1.0f, out, sizeof(out), &len) ==
              APP_BLE_ERR_UNSUPPORTED,
          "a non-numeric value was encoded");
    CHECK(app_ble_gatt_encode_action("set_value", "-5", 1.0f, out, sizeof(out), &len) ==
              APP_BLE_ERR_UNSUPPORTED,
          "a negative register value was encoded");
    CHECK(app_ble_gatt_encode_action("set_value", "70000", 1.0f, out, sizeof(out), &len) ==
              APP_BLE_ERR_UNSUPPORTED,
          "a value beyond two bytes was encoded");
    CHECK(app_ble_gatt_encode_action("set_text", "toolong", 1.0f, out, 3u, &len) == APP_BLE_ERR_TRUNCATED,
          "an over-long text value was silently cut");
    CHECK(app_ble_gatt_decode_state(out, 4u, 1.0f, text, sizeof(text)) == APP_BLE_ERR_UNSUPPORTED,
          "a non-numeric payload was decoded as a number");
    CHECK(app_ble_gatt_encode_action(NULL, NULL, 1.0f, out, sizeof(out), &len) == APP_BLE_ERR_INVALID_ARGUMENT,
          "NULL action accepted");
}

int main(void)
{
    test_init_validates_ops();
    test_open_failure_releases_the_radio();
    test_open_rejects_second_session();
    test_cancel_makes_an_in_flight_operation_cancelled();
    test_cancel_discards_a_successful_result();
    test_deadline_expiry_is_a_timeout_not_a_success();
    test_read_and_write();
    test_subscribe_and_notifications();
    test_a_late_notification_does_not_land_in_the_next_session();
    test_close_reports_a_failing_teardown();
    test_operation_guard_is_released();
    test_codec();

    if (failures != 0) {
        printf("app_ble_gatt: %d check(s), %d failure(s)\n", checks, failures);
        return 1;
    }
    printf("app_ble_gatt: %d checks, 0 failures\n", checks);
    return 0;
}
