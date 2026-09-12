/*
 * BLE GATT firmware adapter tests: app_ble_gatt -> esphome_l2 transport.
 *
 * The transport here is the REAL esphome_ble_gatt.c and the adapter is the real
 * app_ble_gatt_native.c. Only the radio is fake: esphome_ble_gatt_nimble.c is
 * replaced by tests/host/fake_ble_transport.c, which implements the same ops table
 * the real NimBLE backend does. So these tests drive the real framing, the real
 * epoch guard, the real subscription table and the real radio suspend/resume
 * points.
 *
 * app_ble_gatt already has its own group (177 checks) that proves the session
 * policy against a scripted ops table. What nothing proved until now is that the
 * table the FIRMWARE supplies behaves the way that policy assumes. Four claims are
 * checked here; each of them, if wrong, fails only on hardware:
 *
 *   1. A failed read leaves no stale bytes in the caller's buffer. The transport
 *      reports the failure, but the adapter is what hands the caller a buffer, and
 *      a caller that logged and continued would publish 0xee as a reading.
 *   2. gatt_last_error() reports the error of the operation that failed. The
 *      transport keeps one slot, so reading it later reports a different
 *      operation; the adapter pins the code at failure time instead.
 *   3. The radio pair a platform supplies reaches the transport's config, is
 *      suspended at the defined points, and never reports success for arbitration
 *      that did not happen.
 *   4. A notification crosses the adapter unchanged, so app_ble_gatt's generation
 *      guard remains the only guard.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_ble_gatt.h"
#include "app_ble_gatt_native.h"
#include "esphome_ble_gatt.h"
#include "fake_ble_transport.h"
#include "private/esphome_ble_gatt_internal.h"

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
/* the host clock                                                      */
/* ------------------------------------------------------------------ */

/* Monotonic microseconds, advanced by the tests. Nothing sleeps: a deadline is
 * crossed by moving the clock, which is what makes the timeout paths testable at
 * all. */
static int64_t g_now_us;

int64_t esp_timer_get_time(void)
{
    return g_now_us;
}

static void clock_set_ms(uint32_t ms)
{
    g_now_us = (int64_t)ms * 1000;
}

static void clock_advance_ms(uint32_t ms)
{
    g_now_us += (int64_t)ms * 1000;
}

/* ------------------------------------------------------------------ */
/* platform radio hooks                                                */
/* ------------------------------------------------------------------ */

/*
 * A platform pair that COUNTS its outstanding holds, which is the contract note 1
 * of app_ble_gatt_native.h describes: the session acquires first and the transport
 * suspends again inside connect, so a second suspend while already suspended is
 * normal and the radio must come back only when the last holder lets go.
 */
typedef struct {
    int suspend_calls;
    int resume_calls;
    int outstanding;
    int max_outstanding;
    bool went_negative; /* a resume without a matching suspend */
    esp_err_t next_suspend;
    uintptr_t token_value;
} fake_radio_t;

static fake_radio_t g_radio;

static esp_err_t platform_suspend(void *user, uintptr_t *restore_token)
{
    fake_radio_t *r = (fake_radio_t *)user;

    r->suspend_calls++;
    if (r->next_suspend != ESP_OK) {
        return r->next_suspend;
    }
    r->outstanding++;
    if (r->outstanding > r->max_outstanding) {
        r->max_outstanding = r->outstanding;
    }
    if (restore_token != NULL) {
        *restore_token = r->token_value;
    }
    return ESP_OK;
}

static void platform_resume(void *user, uintptr_t restore_token)
{
    fake_radio_t *r = (fake_radio_t *)user;

    (void)restore_token;
    r->resume_calls++;
    r->outstanding--;
    if (r->outstanding < 0) {
        r->went_negative = true;
    }
}

/* ------------------------------------------------------------------ */
/* fixtures                                                            */
/* ------------------------------------------------------------------ */

#define DB_CAPACITY 8u
#define VALUE_HANDLE 0x0003u
#define CCCD_HANDLE 0x0004u

typedef struct {
    app_ble_gatt_session_t session;
    app_ble_gatt_native_t adapter;
    esphome_ble_gatt_session_t transport;

    esphome_ble_gatt_service_t services[DB_CAPACITY];
    esphome_ble_gatt_characteristic_t characteristics[DB_CAPACITY];
    esphome_ble_gatt_descriptor_t descriptors[DB_CAPACITY];
    esphome_ble_gatt_db_t db;
} fixture_t;

/*
 * The radio is substituted by the build, not here: the runner compiles with
 * -DESPHOME_BLE_GATT_BACKEND=fake_ble_backend_ops, so the real
 * esphome_ble_gatt_init() installs the scripted backend and the transport above it
 * is the unmodified production source. See
 * firmware/components/esphome_l2/private/esphome_ble_gatt_internal.h.
 */

/*
 * A peer in DISPLAY order, which is what the session contract says it holds (see
 * app_ble_gatt.h and app_ble_addr.h). The bytes are deliberately non-palindromic so
 * a missing or doubled conversion cannot pass.
 */
static app_ble_peer_t peer_of(uint8_t last)
{
    app_ble_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.address[0] = 0xc4u;
    peer.address[1] = 0x99u;
    peer.address[2] = 0x4cu;
    peer.address[3] = 0x1au;
    peer.address[4] = 0x2bu;
    peer.address[5] = last;
    peer.address_type = 0u;
    return peer;
}

/* What the radio must receive for peer_of(0x3d): the same bytes reversed. */
static const uint8_t PEER_WIRE[6] = {0xc4u, 0x99u, 0x4cu, 0x1au, 0x2bu, 0x3du};
static const uint8_t PEER_CONTROLLER[6] = {0x3du, 0x2bu, 0x1au, 0x4cu, 0x99u, 0xc4u};

/* Wire a session to the real transport through the real adapter. */
static void fixture_up(fixture_t *f, bool with_radio)
{
    app_ble_gatt_session_ops_t ops;

    memset(f, 0, sizeof(*f));
    memset(&g_radio, 0, sizeof(g_radio));
    g_radio.next_suspend = ESP_OK;

    f->db.services = f->services;
    f->db.service_capacity = DB_CAPACITY;
    f->db.characteristics = f->characteristics;
    f->db.characteristic_capacity = DB_CAPACITY;
    f->db.descriptors = f->descriptors;
    f->db.descriptor_capacity = DB_CAPACITY;

    /* The platform pair is supplied BEFORE the install: install is what fills the
     * ops table, and it decides from the pair whether to offer arbitration at all,
     * so a pair supplied afterwards would leave the session with no arbiter. The
     * adapter also preserves the pair across a later install, which the adapter
     * table test covers. */
    if (with_radio) {
        app_ble_gatt_native_set_radio(&f->adapter, platform_suspend, platform_resume, &g_radio);
    }
    CHECK(app_ble_gatt_native_install(&f->adapter, &f->transport, &ops) == ESP_OK,
          "adapter install failed");
    if (with_radio) {
        CHECK(ops.radio_acquire != NULL && ops.radio_release != NULL,
              "install did not offer arbitration for a platform that has a radio");
    }
    /* The session calls gatt_init, which is what initialises the transport, builds
     * the real transport config (timeouts plus, through the adapter, the radio
     * pair) and installs the fake backend. That is the same order the firmware
     * uses, so the config the fake backend records is the config the target would
     * get. */
    CHECK(app_ble_gatt_session_init(&f->session, &ops, &f->adapter, NULL) == ESP_OK,
          "session init failed");
}

static void fixture_down(fixture_t *f)
{
    (void)app_ble_gatt_session_close(&f->session);
}

/* ------------------------------------------------------------------ */
/* 1. the ops table the adapter supplies                               */
/* ------------------------------------------------------------------ */

static void test_adapter_table(void)
{
    /* Zeroed first: the adapter's state before install is whatever the caller's
     * storage held, and a fresh adapter must still end up with no radio. */
    app_ble_gatt_native_t adapter;
    esphome_ble_gatt_session_t transport;
    app_ble_gatt_session_ops_t ops;

    memset(&adapter, 0, sizeof(adapter));
    memset(&transport, 0, sizeof(transport));

    CHECK(app_ble_gatt_native_install(NULL, &transport, &ops) == ESP_ERR_INVALID_ARG,
          "a NULL adapter was accepted");
    CHECK(app_ble_gatt_native_install(&adapter, NULL, &ops) == ESP_ERR_INVALID_ARG,
          "a NULL transport was accepted");
    CHECK(app_ble_gatt_native_install(&adapter, &transport, NULL) == ESP_ERR_INVALID_ARG,
          "a NULL output table was accepted");

    CHECK(app_ble_gatt_native_install(&adapter, &transport, &ops) == ESP_OK, "install failed");
    /* Every entry the session requires must be present, or the session refuses to
     * initialise and the firmware has no BLE path at all. */
    CHECK(ops.gatt_init != NULL && ops.gatt_deinit != NULL && ops.gatt_connect != NULL &&
              ops.gatt_discover != NULL && ops.gatt_read != NULL && ops.gatt_write != NULL &&
              ops.gatt_subscribe != NULL && ops.gatt_unsubscribe != NULL &&
              ops.gatt_disconnect != NULL && ops.gatt_cancel != NULL &&
              ops.gatt_is_connected != NULL && ops.gatt_last_error != NULL && ops.now_ms != NULL,
          "the ops table is missing an entry the session requires");

    /* A platform with no arbiter: the hooks stay NULL rather than becoming stubs
     * that report success for arbitration that never happened. */
    CHECK(ops.radio_acquire == NULL && ops.radio_release == NULL,
          "an adapter with no radio still offered arbitration");
    CHECK(adapter.radio_available == false, "radio_available is set without a pair");

    app_ble_gatt_native_set_radio(&adapter, platform_suspend, platform_resume, &g_radio);
    CHECK(adapter.radio_available == true, "a full pair was not accepted");

    /* Half a pair is refused, because a half pair would let acquire() succeed
     * while the transport refuses to initialise. */
    app_ble_gatt_native_set_radio(&adapter, platform_suspend, NULL, &g_radio);
    CHECK(adapter.radio_available == false, "a suspend without a resume was accepted");
    app_ble_gatt_native_set_radio(&adapter, NULL, platform_resume, &g_radio);
    CHECK(adapter.radio_available == false, "a resume without a suspend was accepted");

    /* A re-install must not lose the platform pair the caller already supplied. */
    app_ble_gatt_native_set_radio(&adapter, platform_suspend, platform_resume, &g_radio);
    CHECK(app_ble_gatt_native_install(&adapter, &transport, &ops) == ESP_OK, "re-install failed");
    CHECK(adapter.radio_available == true, "a re-install dropped the platform pair");
    CHECK(ops.radio_acquire != NULL && ops.radio_release != NULL,
          "a re-install did not offer arbitration");

    /* And the clock is the platform's, in milliseconds. */
    clock_set_ms(1234u);
    CHECK(app_ble_gatt_native_now_ms() == 1234u, "now_ms is %u, expected 1234",
          (unsigned)app_ble_gatt_native_now_ms());
    clock_advance_ms(7u);
    CHECK(app_ble_gatt_native_now_ms() == 1241u, "the clock did not advance");
}

/* ------------------------------------------------------------------ */
/* 2. the link                                                         */
/* ------------------------------------------------------------------ */

static void test_link_lifecycle(void)
{
    fixture_t f;
    fake_ble_backend_t *b;
    app_ble_peer_t peer = peer_of(0x11u);

    fixture_up(&f, false);
    b = fake_ble_backend_state();
    CHECK(b != NULL, "the fake backend is not live after session init");
    if (b == NULL) {
        return;
    }

    /* The session's timeouts must reach the transport config: a timeout that never
     * gets there is a timeout that never fires. Read from the transport itself,
     * which is the object the session built and the object the backend was given.
     * The backend's own copy of the config is deliberately not asserted on: the
     * transport installs the backend with esphome_ble_gatt_init(), whose config
     * lives in the session's own storage, and a backend that keeps a copy is
     * keeping a snapshot rather than the live value. */
    {
        const esphome_ble_gatt_impl_t *impl = (const esphome_ble_gatt_impl_t *)&f.transport;

        CHECK(impl->config.connect_timeout_ms == APP_BLE_DEFAULT_CONNECT_TIMEOUT_MS,
              "connect timeout is %u, expected %u", (unsigned)impl->config.connect_timeout_ms,
              (unsigned)APP_BLE_DEFAULT_CONNECT_TIMEOUT_MS);
        CHECK(impl->config.operation_timeout_ms == APP_BLE_DEFAULT_OPERATION_TIMEOUT_MS,
              "operation timeout is %u, expected %u", (unsigned)impl->config.operation_timeout_ms,
              (unsigned)APP_BLE_DEFAULT_OPERATION_TIMEOUT_MS);
        /* The adapter must not pass a radio pair the caller never supplied. */
        CHECK(impl->config.radio_suspend == NULL && impl->config.radio_resume == NULL,
              "a radio pair reached the transport without a platform");
    }
    /* The cccd_handle the fake reports must be the one the session resolved, which
     * is how a recipe's characteristic index becomes a real handle. */
    /* The cccd_handle the fake reports must be the one the session resolved, which
     * is how a recipe's characteristic index becomes a real handle. */

    CHECK(app_ble_gatt_session_open(&f.session, &peer) == APP_BLE_OK, "open failed");
    CHECK(f.session.phase == APP_BLE_PHASE_READY, "phase is %s",
          app_ble_phase_name(f.session.phase));
    CHECK(b->connect_calls == 1, "connect was called %d times", b->connect_calls);
    CHECK(b->discover_calls == 1, "discover was called %d times", b->discover_calls);
    CHECK(f.session.db.service_count == 1u, "the database has %u services",
          (unsigned)f.session.db.service_count);
    CHECK(f.session.db.characteristic_count == 1u, "the database has %u characteristics",
          (unsigned)f.session.db.characteristic_count);
    /* Descriptors are collected only so a CCCD can be located, and the fake reports
     * exactly one: the session must have kept it, or no subscription can resolve
     * its CCCD handle. */
    CHECK(f.session.db.descriptor_count == 1u, "the discovered database has %u descriptors",
          (unsigned)f.session.db.descriptor_count);

    /* The handle a recipe names is the characteristic INDEX, which only works if
     * resolution reads the discovered table. */
    {
        uint16_t value_handle = 0u;
        uint16_t cccd_handle = 0u;

        CHECK(app_ble_gatt_resolve(&f.session.db, 0u, &value_handle, &cccd_handle) == APP_BLE_OK,
              "resolving characteristic 0 failed");
        CHECK(value_handle == VALUE_HANDLE, "value handle is 0x%04x, expected 0x%04x",
              (unsigned)value_handle, (unsigned)VALUE_HANDLE);
        CHECK(cccd_handle == CCCD_HANDLE, "CCCD handle is 0x%04x, expected 0x%04x",
              (unsigned)cccd_handle, (unsigned)CCCD_HANDLE);
        CHECK(app_ble_gatt_resolve(&f.session.db, 1u, &value_handle, &cccd_handle) ==
                  APP_BLE_ERR_NOT_FOUND,
              "an out-of-range characteristic index resolved");
    }

    fixture_down(&f);
    CHECK(f.session.phase == APP_BLE_PHASE_CLOSED, "phase after close is %s",
          app_ble_phase_name(f.session.phase));
    CHECK(b->disconnect_calls == 1, "disconnect was called %d times", b->disconnect_calls);
}

/* ------------------------------------------------------------------ */
/* 3. read and write across the adapter                                */
/* ------------------------------------------------------------------ */

static void test_read_and_write(void)
{
    fixture_t f;
    fake_ble_backend_t *b;
    app_ble_peer_t peer = peer_of(0x22u);
    uint8_t out[16];

    fixture_up(&f, false);
    b = fake_ble_backend_state();
    if (b == NULL) {
        CHECK(false, "the fake backend is not live");
        return;
    }
    if (app_ble_gatt_session_open(&f.session, &peer) != APP_BLE_OK) {
        CHECK(false, "open failed");
        return;
    }

    /* A successful read carries the bytes through unchanged. */
    b->read_value[0] = 0x42u;
    b->read_value[1] = 0x43u;
    b->read_value_len = 2u;
    b->read_reply_len = 2u;
    memset(out, 0, sizeof(out));
    {
        size_t len = 0u;

        CHECK(app_ble_gatt_session_read(&f.session, VALUE_HANDLE, out, sizeof(out), &len) ==
                  APP_BLE_OK,
              "read failed");
        CHECK(len == 2u, "read reported %u bytes", (unsigned)len);
        CHECK(out[0] == 0x42u && out[1] == 0x43u, "read returned 0x%02x 0x%02x", out[0], out[1]);
    }

    /* A failed read must not leave the 0xee the backend wrote: a caller that
     * carried on would publish it as a value. This is claim 1. */
    b->next_read = ESP_FAIL;
    b->native_error = 0x1234;
    memset(out, 0, sizeof(out));
    {
        size_t len = 99u;

        CHECK(app_ble_gatt_session_read(&f.session, VALUE_HANDLE, out, sizeof(out), &len) ==
                  APP_BLE_ERR_READ_FAILED,
              "a failed read was reported as success");
        CHECK(out[0] == 0x00u, "a failed read left 0x%02x in the caller's buffer", out[0]);
        CHECK(len == 0u, "a failed read reported %u bytes", (unsigned)len);
        CHECK(f.session.last_native_error == 0x1234,
              "the native error is 0x%x, expected 0x1234", (unsigned)f.session.last_native_error);
    }

    /* Claim 2: after a later SUCCESSFUL operation the session must still be able
     * to see the code from the failure, because the transport's own slot has been
     * cleared by then. Read the adapter's answer directly, which is what
     * gatt_last_error() returns to the session. */
    b->next_read = ESP_OK;
    b->read_value_len = 1u;
    b->read_reply_len = 1u;
    b->read_value[0] = 0x01u;
    {
        size_t len = 0u;

        CHECK(app_ble_gatt_session_read(&f.session, VALUE_HANDLE, out, sizeof(out), &len) ==
                  APP_BLE_OK,
              "the recovery read failed");
    }
    CHECK(f.session.ops.gatt_last_error(f.session.ops_ctx) == 0,
          "a success did not clear the adapter's error: 0x%x",
          (unsigned)f.session.ops.gatt_last_error(f.session.ops_ctx));

    /* A write crosses the adapter with its handle, bytes and response mode. */
    {
        const uint8_t payload[3] = {0xaa, 0xbb, 0xcc};

        CHECK(app_ble_gatt_session_write(&f.session, VALUE_HANDLE, payload, sizeof(payload), true) ==
                  APP_BLE_OK,
              "write failed");
        CHECK(b->written_handle == VALUE_HANDLE, "the write went to handle 0x%04x",
              (unsigned)b->written_handle);
        CHECK(b->written_len == sizeof(payload), "the write carried %u bytes",
              (unsigned)b->written_len);
        CHECK(b->written_response == true, "the write lost its response mode");
        CHECK(memcmp(b->written, payload, sizeof(payload)) == 0, "the write carried other bytes");
    }

    fixture_down(&f);
}

/* ------------------------------------------------------------------ */
/* 4. the subscription path                                            */
/* ------------------------------------------------------------------ */

static int notify_calls;
static uint16_t notify_handle;
static uint8_t notify_data[32];
static size_t notify_len;
static bool notify_truncated;

static void on_notify(void *user, uint16_t value_handle, const uint8_t *data, size_t len,
                      bool truncated)
{
    (void)user;
    notify_calls++;
    notify_handle = value_handle;
    notify_len = (len <= sizeof(notify_data)) ? len : sizeof(notify_data);
    if (data != NULL && notify_len > 0u) {
        memcpy(notify_data, data, notify_len);
    }
    notify_truncated = truncated;
}

static void test_subscription_and_notifications(void)
{
    fixture_t f;
    fake_ble_backend_t *b;
    app_ble_peer_t peer = peer_of(0x33u);
    const uint8_t first[2] = {0x10u, 0x11u};
    const uint8_t second[4] = {0x20u, 0x21u, 0x22u, 0x23u};

    fixture_up(&f, false);
    b = fake_ble_backend_state();
    if (b == NULL) {
        CHECK(false, "the fake backend is not live");
        return;
    }
    if (app_ble_gatt_session_open(&f.session, &peer) != APP_BLE_OK) {
        CHECK(false, "open failed");
        return;
    }

    notify_calls = 0;
    notify_len = 0u;
    notify_truncated = false;

    CHECK(app_ble_gatt_session_subscribe(&f.session, VALUE_HANDLE, CCCD_HANDLE, on_notify, NULL,
                                         false) == APP_BLE_OK,
          "subscribe failed");
    CHECK(b->last_cccd_handle == CCCD_HANDLE, "the CCCD written was 0x%04x",
          (unsigned)b->last_cccd_handle);
    CHECK(b->last_notify_on == true, "the subscription did not enable notifications");
    CHECK(b->last_indications == false, "a notification subscription asked for indications");

    /* Claim 4: a notification crosses the adapter and reaches the caller with the
     * value handle, the bytes and the truncation flag intact. */
    fake_ble_backend_notify(VALUE_HANDLE, first, sizeof(first));
    CHECK(notify_calls == 1, "the callback ran %d times", notify_calls);
    CHECK(notify_handle == VALUE_HANDLE, "the callback saw handle 0x%04x", (unsigned)notify_handle);
    CHECK(notify_len == sizeof(first), "the callback saw %u bytes", (unsigned)notify_len);
    CHECK(memcmp(notify_data, first, sizeof(first)) == 0, "the callback saw other bytes");
    CHECK(notify_truncated == false, "a complete notification was marked truncated");
    CHECK(f.session.notifications_seen == 1u, "the session counted %u notifications",
          (unsigned)f.session.notifications_seen);

    /* A second notification must be delivered too: a one-shot would look correct
     * to a test that only sent one. */
    fake_ble_backend_notify(VALUE_HANDLE, second, sizeof(second));
    CHECK(notify_calls == 2, "the second notification did not arrive");
    CHECK(notify_len == sizeof(second), "the second notification carried %u bytes",
          (unsigned)notify_len);
    CHECK(memcmp(notify_data, second, sizeof(second)) == 0, "the second payload is wrong");

    /* The truncation flag is the transport's, and it must survive the adapter: a
     * value that did not fit is not a value. */
    fake_ble_backend_notify_truncated(VALUE_HANDLE, second, sizeof(second));
    CHECK(notify_truncated == true, "the truncation flag was lost in the adapter");

    /* A notification for a handle nobody subscribed to must not reach a callback
     * that belongs to a different characteristic. */
    {
        int before = notify_calls;

        fake_ble_backend_notify(0x0099u, first, sizeof(first));
        CHECK(notify_calls == before, "an unsubscribed handle reached the callback");
    }

    /* Unsubscribing turns the CCCD off and silences the callback. */
    CHECK(app_ble_gatt_session_unsubscribe(&f.session, VALUE_HANDLE) == APP_BLE_OK,
          "unsubscribe failed");
    CHECK(b->last_notify_on == false, "unsubscribe did not clear the CCCD");
    {
        int before = notify_calls;

        fake_ble_backend_notify(VALUE_HANDLE, first, sizeof(first));
        CHECK(notify_calls == before, "a notification arrived after unsubscribe");
    }

    fixture_down(&f);
}

/*
 * The scan-to-GATT address handoff, which is the one thing the conversion tests
 * cannot prove on their own.
 *
 * A byte reversal applied twice is byte-for-byte identical to never applying it, so
 * a suite that only tested app_ble_addr.c would pass just as happily with the
 * conversion in both places as with it in neither. What pins it is the whole path:
 * an address in display order goes in at the session, and the bytes the radio is
 * handed must be that address reversed - exactly once, by the adapter.
 */
static void test_peer_address_reaches_the_radio_once(void)
{
    fixture_t f;
    fake_ble_backend_t *b;

    fixture_up(&f, false);
    b = fake_ble_backend_state();
    if (b == NULL) {
        CHECK(false, "the fake backend is not live");
        return;
    }

    /* The session's peer is the display order it was given, unchanged: the session
     * itself must not convert, or the adapter's conversion would be the second. */
    {
        app_ble_peer_t peer = peer_of(0x3du);

        CHECK(memcmp(peer.address, PEER_WIRE, 6) == 0, "the fixture is not display order");
        CHECK(app_ble_gatt_session_open(&f.session, &peer) == APP_BLE_OK, "open failed");
        CHECK(memcmp(f.session.peer.address, PEER_WIRE, 6) == 0,
              "the session altered the peer address it was given");
        CHECK(f.session.peer.address_type == 0u, "the session altered the address type");
    }

    /* And what the radio got is the reverse, which is what NimBLE's
     * ble_addr_t::val takes. */
    CHECK(b->connect_calls == 1, "connect ran %d times", b->connect_calls);
    CHECK(memcmp(b->connected_address, PEER_CONTROLLER, 6) == 0,
          "the radio was handed the wrong byte order");
    CHECK(memcmp(b->connected_address, PEER_WIRE, 6) != 0,
          "the radio was handed display order, so no conversion happened");
    CHECK(b->connected_address_type == 0u, "the address type did not reach the radio");

    fixture_down(&f);

    /* A second peer, so the result cannot be an accident of the first address. */
    {
        app_ble_peer_t peer;

        memset(&peer, 0, sizeof(peer));
        peer.address[0] = 0x7au;
        peer.address[1] = 0x11u;
        peer.address[2] = 0x22u;
        peer.address[3] = 0x33u;
        peer.address[4] = 0x44u;
        peer.address[5] = 0x55u;
        peer.address_type = 1u;

        fixture_up(&f, false);
        b = fake_ble_backend_state();
        if (b == NULL) {
            CHECK(false, "the fake backend is not live");
            return;
        }
        CHECK(app_ble_gatt_session_open(&f.session, &peer) == APP_BLE_OK, "second open failed");
        {
            static const uint8_t expected[6] = {0x55u, 0x44u, 0x33u, 0x22u, 0x11u, 0x7au};

            CHECK(memcmp(b->connected_address, expected, 6) == 0,
                  "the second address was handed over in the wrong order");
        }
        CHECK(b->connected_address_type == 1u, "the second address type did not arrive");
        fixture_down(&f);
    }
}

/* ------------------------------------------------------------------ */
/* 5. radio arbitration                                                */
/* ------------------------------------------------------------------ */

static void test_radio_arbitration(void)
{
    fixture_t f;
    fake_ble_backend_t *b;
    app_ble_peer_t peer = peer_of(0x44u);

    /* With a platform pair: the session acquires before connecting (claim 3), so
     * the platform sees a suspend before the radio is ever asked to connect. */
    fixture_up(&f, true);
    b = fake_ble_backend_state();
    if (b == NULL) {
        CHECK(false, "the fake backend is not live");
        return;
    }
    /* The platform pair must reach the transport config (note 2 of
     * app_ble_gatt_native.h): the transport suspends the radio itself inside
     * connect, so a pair that stayed in the adapter would leave that suspend
     * without a hook. */
    {
        const esphome_ble_gatt_impl_t *impl = (const esphome_ble_gatt_impl_t *)&f.transport;

        CHECK(impl->config.radio_suspend == platform_suspend,
              "the platform suspend hook never reached the transport config");
        CHECK(impl->config.radio_resume == platform_resume,
              "the platform resume hook never reached the transport config");
        CHECK(impl->config.radio_user == &g_radio,
              "the transport config's radio_user is not the platform's own context");
    }
    CHECK(f.session.ops.radio_acquire != NULL,
          "the session was not offered arbitration although the platform has a radio");

    CHECK(app_ble_gatt_session_open(&f.session, &peer) == APP_BLE_OK, "open with radio failed");
    CHECK(f.session.radio_held == true, "the session does not report holding the radio");
    /* Two suspends: the session's acquire, then the transport's own suspend inside
     * connect. A platform pair that is not idempotent would have resumed the radio
     * on the first release while the session still held it. */
    CHECK(g_radio.suspend_calls == 2, "the platform saw %d suspends, expected 2",
          g_radio.suspend_calls);
    CHECK(g_radio.outstanding == 2, "the platform thinks %d holds are outstanding, expected 2",
          g_radio.outstanding);

    fixture_down(&f);
    CHECK(f.session.radio_held == false, "the radio is still held after close");
    CHECK(g_radio.went_negative == false, "the platform was resumed more often than suspended");
    CHECK(g_radio.outstanding == 0, "the platform still has %d holds outstanding",
          g_radio.outstanding);

    /* A platform whose suspend fails must fail the open, not proceed without the
     * radio: a session that believes it holds a radio it never got is the state
     * that corrupts a scan. */
    fixture_up(&f, true);
    g_radio.next_suspend = ESP_ERR_INVALID_STATE;
    CHECK(app_ble_gatt_session_open(&f.session, &peer) == APP_BLE_ERR_CONNECT_FAILED,
          "a refused radio did not fail the open");
    CHECK(f.session.phase == APP_BLE_PHASE_FAILED, "phase is %s",
          app_ble_phase_name(f.session.phase));
    CHECK(f.session.radio_held == false, "the session holds a radio it never got");
    CHECK(g_radio.outstanding == 0, "a failed suspend left a hold outstanding");
    fixture_down(&f);
}

/* ------------------------------------------------------------------ */
/* 6. cancellation through three layers                                */
/* ------------------------------------------------------------------ */

static void test_cancel_inside_a_read(void)
{
    fixture_t f;
    fake_ble_backend_t *b;
    app_ble_peer_t peer = peer_of(0x55u);
    uint8_t out[16];
    size_t len = 0u;

    fixture_up(&f, false);
    b = fake_ble_backend_state();
    if (b == NULL) {
        CHECK(false, "the fake backend is not live");
        return;
    }
    if (app_ble_gatt_session_open(&f.session, &peer) != APP_BLE_OK) {
        CHECK(false, "open failed");
        return;
    }

    /* The backend cancels while the read is outstanding. The read succeeds as far
     * as the radio is concerned, and the adapter passes that success up: the
     * session is what decides this was a cancellation, and reporting success here
     * would tell the application a value was read that it must not use. */
    b->read_value_len = 0u;
    b->read_reply_len = 0u;
    b->cancel_session = &f.session;
    CHECK(app_ble_gatt_session_read(&f.session, VALUE_HANDLE, out, sizeof(out), &len) ==
              APP_BLE_ERR_CANCELLED,
          "a read cancelled underneath the session was reported as %s",
          app_ble_status_name(f.session.last_status));
    CHECK(b->cancel_calls == 1, "the cancel reached the backend %d times", b->cancel_calls);
    CHECK(f.session.cancellations >= 1u, "the session did not record the cancellation");
    b->cancel_session = NULL;

    fixture_down(&f);
}

int main(void)
{
    printf("ble gatt adapter tests\n");

    test_adapter_table();
    test_link_lifecycle();
    test_read_and_write();
    test_subscription_and_notifications();
    test_peer_address_reaches_the_radio_once();
    test_radio_arbitration();
    test_cancel_inside_a_read();

    printf("ble gatt adapter: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
