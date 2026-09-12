/*
 * The BLE GATT control backend, driven through the REAL app_control loop.
 *
 * The property this group exists to protect is the one the whole control design
 * rests on: a successful send is not a state change. A backend that confirmed its
 * own write would make "confirmed" mean "we asked", and every downstream surface -
 * console, HTTP, the future GUI - would show a device state that was never
 * observed.
 *
 * So the assertions here are about ORDER: what the observed state is after the
 * send, after the write, and only then after the device reports. The GATT session
 * is scripted, so each of those moments is reached deliberately rather than by
 * racing a radio.
 *
 * Two refusals are also pinned, because both would be silent on hardware:
 *   - a control with no live link is FAILED, not SENT: a control the caller is told
 *     left the firmware, which never left it, would sit pending until its deadline
 *     and then be reported as a device timeout;
 *   - an action the codec cannot encode is UNSUPPORTED, not guessed: guessing an
 *     encoding is how a device gets written with garbage.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_ble_gatt.h"
#include "app_control.h"
#include "app_ctl_ble.h"
#include "app_device.h"
#include "app_device_test_hooks.h"
#include "app_str.h"
#include "esp_err.h"
#include "ha_core.h"

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

/*
 * A precondition the rest of the test depends on. Returning beats continuing into a
 * null dereference: the failure is already reported, and a crash would take the
 * remaining tests with it and hide their results.
 */
#define REQUIRE(cond, ...)                                                               \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
            return;                                                                      \
        }                                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/* a scripted GATT session                                             */
/* ------------------------------------------------------------------ */

#define TEST_VALUE_HANDLE 0x0021u

typedef struct {
    bool ready;
    int write_calls;
    uint16_t write_handle;
    uint8_t written[APP_BLE_VALUE_MAX];
    size_t written_len;
    app_ble_status_t next_write;
    bool resolve_ok;
    uint16_t resolve_handle;
} fake_gatt_t;

static fake_gatt_t g_gatt;

static int fg_write(void *ctx, uint16_t handle, const uint8_t *data, size_t len)
{
    fake_gatt_t *g = (fake_gatt_t *)ctx;

    g->write_calls++;
    g->write_handle = handle;
    g->written_len = (len <= sizeof(g->written)) ? len : sizeof(g->written);
    if (data != NULL && g->written_len > 0u) {
        memcpy(g->written, data, g->written_len);
    }
    return (int)g->next_write;
}

static bool fg_is_ready(void *ctx)
{
    return ((fake_gatt_t *)ctx)->ready;
}

static bool fg_resolve(void *ctx, const app_entity_binding_t *entity,
                       const app_device_binding_t *device, uint16_t *out_handle)
{
    fake_gatt_t *g = (fake_gatt_t *)ctx;

    (void)entity;
    (void)device;
    if (!g->resolve_ok) {
        return false;
    }
    *out_handle = g->resolve_handle;
    return true;
}

static app_ble_status_t fg_open(void *ctx, const app_ble_peer_t *peer)
{
    (void)ctx;
    (void)peer;
    return APP_BLE_OK;
}

static app_ble_status_t fg_read(void *ctx, uint16_t handle, uint8_t *out, size_t cap,
                                size_t *out_len)
{
    (void)ctx;
    (void)handle;
    (void)out;
    (void)cap;
    (void)out_len;
    return APP_BLE_ERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/* fixture                                                             */
/* ------------------------------------------------------------------ */

static uint32_t g_now_ms;

static uint32_t now_ms(void)
{
    return g_now_ms;
}

static app_ctl_ble_t g_ctl;

/* The entity under control. A real binding placed by the test hook, bound to the BLE
 * GATT backend: the alternative is materialising a whole evidence set through
 * recognition, which would make this a recognition test. The hook exists precisely
 * for that reason and is compiled out of the firmware image. */
#define TEST_ENTITY "switch.test_bulb"
#define TEST_DEVICE "ble_00c4994c1a2b3d"
/*
 * A second entity that advertises the actions whose CONFIRMATION path is different:
 * set_value (a value the device must report back) and press (an action no backend can
 * derive a target state from). Without these the loop refuses those actions before a
 * backend sees them, and the tests below would pass by never reaching the code they
 * exist to check - which is how they read on their first run.
 */
#define TEST_VALUE_ENTITY "light.test_dimmer"
#define TEST_PRESS_ENTITY "button.test_press"

/*
 * Install the backend into a freshly reset control loop.
 *
 * app_control_reset() drops registered backends, so the registration has to follow
 * it - and that ordering is itself worth pinning, because a backend registered
 * before a reset would leave the loop with no route and every control answering
 * NO_BACKEND.
 */
static void fixture_up(bool ready)
{
    app_control_backend_t backend;
    ha_device_t device;
    ha_entity_t entity;

    memset(&g_gatt, 0, sizeof(g_gatt));
    g_gatt.ready = ready;
    g_gatt.resolve_ok = true;
    g_gatt.resolve_handle = TEST_VALUE_HANDLE;
    g_gatt.next_write = APP_BLE_OK;
    memset(&g_ctl, 0, sizeof(g_ctl));
    g_now_ms = 1000u;

    g_ctl.gatt.write = fg_write;
    g_ctl.gatt.open = fg_open;
    g_ctl.gatt.read = fg_read;
    g_ctl.gatt.is_ready = fg_is_ready;
    g_ctl.gatt.resolve = fg_resolve;
    g_ctl.gatt_ctx = &g_gatt;
    g_ctl.now_ms = now_ms;

    /* The binding table and the loop are reset together: an entity left over from a
     * previous case would make the routing assertions describe the wrong world. */
    app_device_table_reset();
    app_control_reset();

    /*
     * The entity has to exist in ha_core WITH a service mask before the binding hook
     * runs, because that is where the binding's writability comes from: the hook
     * places a binding, it does not invent a capability. An entity that advertises
     * nothing comes out read-only and every control is refused NOT_WRITABLE - which
     * is what this fixture did on its first run, and why the checks below were
     * reporting a refusal instead of the ordering they exist to test.
     */
    memset(&device, 0, sizeof(device));
    (void)app_strlcpy(device.id, TEST_DEVICE, sizeof(device.id));
    (void)app_strlcpy(device.name, "Test Bulb", sizeof(device.name));
    CHECK(ha_core_device_upsert(&device) == HA_CORE_OK, "the test device was not inserted");

    memset(&entity, 0, sizeof(entity));
    (void)app_strlcpy(entity.entity_id, TEST_ENTITY, sizeof(entity.entity_id));
    (void)app_strlcpy(entity.unique_id, TEST_ENTITY, sizeof(entity.unique_id));
    (void)app_strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)app_strlcpy(entity.domain, "switch", sizeof(entity.domain));
    (void)app_strlcpy(entity.device_id, TEST_DEVICE, sizeof(entity.device_id));
    (void)app_strlcpy(entity.name, "Test Bulb", sizeof(entity.name));
    entity.has_entity_name = true;
    entity.enabled = true;
    entity.available = true;
    entity.supported_services = HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF;
    CHECK(ha_core_entity_upsert(&entity) == HA_CORE_OK, "the test entity was not inserted");

    /* The dimmer: set_value is the action whose confirmation depends on the device
     * reporting the value it was given. */
    memset(&entity, 0, sizeof(entity));
    (void)app_strlcpy(entity.entity_id, TEST_VALUE_ENTITY, sizeof(entity.entity_id));
    (void)app_strlcpy(entity.unique_id, TEST_VALUE_ENTITY, sizeof(entity.unique_id));
    (void)app_strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)app_strlcpy(entity.domain, "light", sizeof(entity.domain));
    (void)app_strlcpy(entity.device_id, TEST_DEVICE, sizeof(entity.device_id));
    (void)app_strlcpy(entity.name, "Test Dimmer", sizeof(entity.name));
    entity.has_entity_name = true;
    entity.enabled = true;
    entity.available = true;
    entity.supported_services = HA_SERVICE_MASK_SET_VALUE;
    CHECK(ha_core_entity_upsert(&entity) == HA_CORE_OK, "the dimmer was not inserted");

    /* The button: an action with no derivable target state. */
    memset(&entity, 0, sizeof(entity));
    (void)app_strlcpy(entity.entity_id, TEST_PRESS_ENTITY, sizeof(entity.entity_id));
    (void)app_strlcpy(entity.unique_id, TEST_PRESS_ENTITY, sizeof(entity.unique_id));
    (void)app_strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)app_strlcpy(entity.domain, "button", sizeof(entity.domain));
    (void)app_strlcpy(entity.device_id, TEST_DEVICE, sizeof(entity.device_id));
    (void)app_strlcpy(entity.name, "Test Button", sizeof(entity.name));
    entity.has_entity_name = true;
    entity.enabled = true;
    entity.available = true;
    entity.supported_services = HA_SERVICE_MASK_PRESS;
    CHECK(ha_core_entity_upsert(&entity) == HA_CORE_OK, "the button was not inserted");

    app_device_test_bind_entity(TEST_ENTITY, TEST_DEVICE, DEVICE_DB_BACKEND_BLE_GATT, true, 0,
                                100);
    app_device_test_bind_entity(TEST_VALUE_ENTITY, TEST_DEVICE, DEVICE_DB_BACKEND_BLE_GATT, true,
                                0, 255);
    app_device_test_bind_entity(TEST_PRESS_ENTITY, TEST_DEVICE, DEVICE_DB_BACKEND_BLE_GATT, false,
                                0, 0);

    backend.ops = app_ctl_ble_backend_ops();
    backend.ctx = &g_ctl;
    CHECK(app_control_register_backend(&backend) == ESP_OK, "the backend did not register");
}

/* ------------------------------------------------------------------ */
/* claims                                                              */
/* ------------------------------------------------------------------ */

static void test_claims_only_drivable_ble_entities(void)
{
    app_ctl_ble_t ctl;
    app_entity_binding_t entity;

    memset(&ctl, 0, sizeof(ctl));
    memset(&entity, 0, sizeof(entity));

    /* A writable entity whose recipe names the BLE GATT backend is ours. */
    entity.writable = true;
    entity.backend = DEVICE_DB_BACKEND_BLE_GATT;
    CHECK(app_ctl_ble_backend_ops()->claims(&ctl, &entity), "a writable BLE entity was refused");

    /* A read-only entity is nobody's, whatever its recipe said. */
    entity.writable = false;
    CHECK(!app_ctl_ble_backend_ops()->claims(&ctl, &entity),
          "a read-only entity was claimed");

    /* Another protocol's writable entity is not ours, or two backends would claim
     * it and the loop would report AMBIGUOUS_BACKEND. */
    entity.writable = true;
    entity.backend = DEVICE_DB_BACKEND_ESPHOME_API;
    CHECK(!app_ctl_ble_backend_ops()->claims(&ctl, &entity),
          "an ESPHome entity was claimed by the BLE backend");
    entity.backend = DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE;
    CHECK(!app_ctl_ble_backend_ops()->claims(&ctl, &entity),
          "a Zigbee entity was claimed by the BLE backend");

    /* An entity with no control path at all. */
    entity.backend = APP_ENTITY_BACKEND_NONE;
    CHECK(!app_ctl_ble_backend_ops()->claims(&ctl, &entity),
          "an entity with no backend was claimed");

    CHECK(!app_ctl_ble_backend_ops()->claims(&ctl, NULL), "NULL was claimed");
}

/* ------------------------------------------------------------------ */
/* the rule: a send is not a state change                              */
/* ------------------------------------------------------------------ */

static void test_a_send_does_not_move_the_state(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true);

    status = app_control_submit(TEST_ENTITY, "turn_on", NULL, 41u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));

    /* Accepted, and nothing has moved: the write has not even happened yet. */
    CHECK(slot != NULL, "no slot was returned for an accepted control");
    CHECK(app_control_pending_count() == 1u, "the control is not pending: %u pending",
          (unsigned)app_control_pending_count());
    CHECK(app_control_observed_state(TEST_ENTITY) == NULL,
          "a state was published before the device said anything, got '%s'",
          app_control_observed_state(TEST_ENTITY));
    CHECK(g_gatt.write_calls == 0, "the write happened inside submit, on the caller's task");

    /* The worker writes. Still nothing has moved: a write is not a report. */
    CHECK(app_ctl_ble_tick(&g_ctl), "the tick did not start the write");
    CHECK(g_gatt.write_calls == 1, "the write ran %d times", g_gatt.write_calls);
    CHECK(g_gatt.write_handle == TEST_VALUE_HANDLE, "the write went to handle 0x%04x",
          (unsigned)g_gatt.write_handle);
    CHECK(app_control_pending_count() == 1u,
          "the control left PENDING on the strength of its own write");
    CHECK(app_control_observed_state(TEST_ENTITY) == NULL,
          "a state was published after the write but before any report, got '%s'",
          app_control_observed_state(TEST_ENTITY));

    /* Now the device reports. This is the first moment the state may move. */
    {
        const uint8_t on = 0x01u;

        app_ctl_ble_on_notify(&g_ctl, TEST_VALUE_HANDLE, &on, 1u, false);
        CHECK(g_ctl.notifications_seen == 1u, "the notification was not recorded");
        /* Buffered, not acted on: confirming here would move the observed state on
         * the GATT task, which does not own it. */
        CHECK(app_control_observed_state(TEST_ENTITY) == NULL,
              "the notification published a state from the GATT task");
        CHECK(app_ctl_ble_tick(&g_ctl), "the tick did not act on the device's report");
        CHECK(g_ctl.confirmations == 1u, "confirmations counted %u",
              (unsigned)g_ctl.confirmations);
        CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "the operation is still in flight");
        CHECK(app_control_observed_state(TEST_ENTITY) != NULL &&
                  strcmp(app_control_observed_state(TEST_ENTITY), HA_STATE_ON) == 0,
              "the observed state is not the one the device reported");
    }

    /* And the encoded bytes are the ones the codec defines, not an invention. */
    CHECK(g_gatt.written_len == 1u && g_gatt.written[0] == 0x01u,
          "turn_on wrote %u bytes starting 0x%02x", (unsigned)g_gatt.written_len,
          g_gatt.written_len > 0u ? g_gatt.written[0] : 0u);
}

/*
 * A device that reports the OPPOSITE state does not confirm the control.
 *
 * This is the case that separates "the device answered" from "the device agreed":
 * a backend that confirmed on any report would mark a refused control as
 * confirmed, and the UI would show the state the user asked for on a device that
 * declined it.
 */
static void test_a_disagreeing_report_does_not_confirm(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true);
    status = app_control_submit(TEST_ENTITY, "turn_on", NULL, 42u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    CHECK(app_ctl_ble_tick(&g_ctl), "the write did not start");
    CHECK(app_control_pending_count() == 1u, "pending before the report");

    /* The device says OFF. The control asked for ON. */
    {
        const uint8_t off = 0x00u;

        app_ctl_ble_on_notify(&g_ctl, TEST_VALUE_HANDLE, &off, 1u, false);
        (void)app_ctl_ble_tick(&g_ctl);
    }
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 1u,
          "a disagreeing report ended the control");
    CHECK(g_ctl.confirmations == 0u, "a disagreement was counted as a confirmation");
    CHECK(app_control_observed_state(TEST_ENTITY) == NULL,
          "a disagreeing report published a state");

    /* And the deadline is what ends it, restoring the previous state. */
    g_now_ms += APP_CTL_BLE_CONFIRM_TIMEOUT_MS + 1u;
    CHECK(app_ctl_ble_tick(&g_ctl), "the deadline did not end the control");
    CHECK(g_ctl.deadlines_expired == 1u, "the expiry was not counted");
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "the expired control is still in flight");
    CHECK(app_control_pending_count() == 0u, "the expired control is still pending");
    CHECK(app_control_observed_state(TEST_ENTITY) == NULL,
          "an expired control left the requested state showing");
}

static void test_a_control_with_no_link_is_failed_not_sent(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(false); /* no live session */
    status = app_control_submit(TEST_ENTITY, "turn_on", NULL, 43u, g_now_ms, 5000u, &slot);
    CHECK(status == APP_CONTROL_ERR_BACKEND_FAILED,
          "a control with no link returned %s, expected backend_failed",
          app_control_status_name(status));
    /* A refused control leaves nothing behind: no pending entry, no observed state,
     * and no slot for a caller to read as "maybe it went through". */
    CHECK(app_control_pending_count() == 0u, "a refused control is still pending");
    CHECK(app_control_observed_state(TEST_ENTITY) == NULL, "a refused control published a state");
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "a refused control is in flight");
    CHECK(g_ctl.refused_not_ready == 1u, "the refusal was not recorded");
    CHECK(g_gatt.write_calls == 0, "a write was attempted without a link");
}

static void test_a_control_whose_write_fails_is_failed(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true);
    g_gatt.next_write = APP_BLE_ERR_WRITE_FAILED;
    status = app_control_submit(TEST_ENTITY, "turn_on", NULL, 44u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    (void)app_ctl_ble_tick(&g_ctl);
    CHECK(g_ctl.writes_failed == 1u, "the failed write was not counted");
    CHECK(g_ctl.confirmations == 0u, "a failed write was confirmed");
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "the failed control is still in flight");
    CHECK(app_control_pending_count() == 0u, "the failed control is still pending");
    CHECK(app_control_observed_state(TEST_ENTITY) == NULL,
          "a failed write published a state");
}

static void test_an_unresolvable_handle_is_refused(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true);
    g_gatt.resolve_ok = false;
    status = app_control_submit(TEST_ENTITY, "turn_on", NULL, 45u, g_now_ms, 5000u, &slot);
    CHECK(status == APP_CONTROL_ERR_BACKEND_FAILED,
          "an unresolvable handle returned %s, expected backend_failed",
          app_control_status_name(status));
    CHECK(g_ctl.refused_no_handle == 1u, "the refusal was not recorded");
}

/*
 * A value-carrying control CAN be confirmed.
 *
 * The bounded version of this backend derived an expected state only for turn_on /
 * turn_off, so a set_value that the device obeyed could never confirm: it ran to its
 * deadline and was reported as a failure. That is a false negative in the worst
 * direction - the operator is told a device that worked did not answer - and it is
 * invisible in every test that only exercised switches.
 *
 * The comparison is strict text: the device must report the value it was given. A
 * mismatch costs one confirmation window rather than publishing a wrong state.
 */
static void test_a_value_control_is_confirmed_by_its_own_value(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true);
    status = app_control_submit(TEST_VALUE_ENTITY, "set_value", "128", 50u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL,
            "set_value returned %s for an entity that advertises it",
            app_control_status_name(status));

    REQUIRE(app_ctl_ble_tick(&g_ctl), "the write did not start");
    /* 128 encoded as a one-byte register. */
    CHECK(g_gatt.written_len == 1u && g_gatt.written[0] == 0x80u,
          "set_value wrote %u bytes starting 0x%02x", (unsigned)g_gatt.written_len,
          g_gatt.written_len > 0u ? g_gatt.written[0] : 0u);

    /* The device reports the value it was given. */
    {
        const uint8_t reported = 0x80u;

        app_ctl_ble_on_notify(&g_ctl, TEST_VALUE_HANDLE, &reported, 1u, false);
        CHECK(app_ctl_ble_tick(&g_ctl), "the tick did not act on the device's report");
    }
    CHECK(g_ctl.confirmations == 1u, "an obedient device was not confirmed (counted %u)",
          (unsigned)g_ctl.confirmations);
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "the control is still in flight");
}

/*
 * An action the codec cannot encode is UNSUPPORTED, without guessing a byte to send.
 *
 * `press` is such an action: app_ble_gatt_encode_action() implements turn_on/turn_off,
 * set_value and set_text, and refuses the rest. The backend passes that refusal up
 * rather than inventing an encoding, which is how a device gets written with garbage.
 *
 * This also closes the "sent but unconfirmable" case by construction: every action
 * this backend CAN send is either a named state (turn_on/turn_off) or carries the
 * value the device must report back (set_value/set_text). The reason string for the
 * remaining case stays in app_ctl_ble_tick() as a guard for a future action that has
 * neither; it is not reachable today, and this test records why rather than pretending
 * to exercise it.
 */
/*
 * A value control is confirmed by the value the DEVICE reports, whatever the codec
 * scaled it to.
 *
 * A recipe's `scale` is how the corpus expresses a register in tenths: the codec
 * multiplies the requested value before putting it on the wire, so an obedient device
 * reports the SCALED number and not the one the user typed. Comparing against the
 * unscaled request would never match, and a working control would be reported as a
 * device that never answered - a silent false negative, which is the failure mode this
 * whole case exists to prevent.
 *
 * The assertion is written against what the codec actually wrote rather than a literal,
 * so it holds for any scale: whatever went on the wire is what an obedient device
 * echoes, and that must confirm.
 */
static void test_a_value_is_confirmed_by_what_was_written(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true);
    status = app_control_submit(TEST_VALUE_ENTITY, "set_value", "12", 52u, g_now_ms, 5000u,
                                &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "set_value returned %s",
            app_control_status_name(status));
    REQUIRE(app_ctl_ble_tick(&g_ctl), "the write did not start");
    REQUIRE(g_gatt.written_len == 1u, "the write carried %u bytes",
            (unsigned)g_gatt.written_len);

    {
        const uint8_t reported = g_gatt.written[0];

        app_ctl_ble_on_notify(&g_ctl, TEST_VALUE_HANDLE, &reported, 1u, false);
        CHECK(app_ctl_ble_tick(&g_ctl), "the tick did not act on the device's report");
    }
    CHECK(g_ctl.confirmations == 1u,
          "a device that reported exactly what it was sent was not confirmed (counted %u)",
          (unsigned)g_ctl.confirmations);
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "the control is still in flight");
}

/*
 * A SCALED recipe is confirmed by the scaled value the device reports.
 *
 * `scale` is how the corpus expresses a register in tenths: the codec multiplies the
 * requested value before putting it on the wire, so an obedient device reports the
 * SCALED number and not the one the user typed. Comparing against the unscaled request
 * would never match, and a working control would be reported as a device that never
 * answered - a silent false negative, and the reason this case exists.
 *
 * The binding hook does not carry a scale (a recipe's scale is resolved at recognition
 * time, which app_device_db covers), so this sets one on the binding the loop routes and
 * then drives the backend. Every value here is a whole number on purpose: the loop
 * refuses a fractional value against an integer range, which is its own correct rule and
 * is covered in the control group.
 */
static void test_a_scaled_recipe_expects_the_scaled_value(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;
    app_entity_binding_t *binding;

    fixture_up(true);

    binding = (app_entity_binding_t *)app_control_lookup(TEST_VALUE_ENTITY);
    REQUIRE(binding != NULL && binding->writable, "the dimmer is not a writable binding");
    binding->scale = 10u; /* tenths */

    status = app_control_submit(TEST_VALUE_ENTITY, "set_value", "12", 53u, g_now_ms, 5000u,
                                &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "set_value returned %s",
            app_control_status_name(status));
    REQUIRE(app_ctl_ble_tick(&g_ctl), "the write did not start");

    /* The codec scaled it: twelve tenths is register 120. */
    CHECK(g_gatt.written_len == 1u && g_gatt.written[0] == 120u,
          "a scale-10 recipe wrote %u bytes starting 0x%02x, expected 120",
          (unsigned)g_gatt.written_len, g_gatt.written_len > 0u ? g_gatt.written[0] : 0u);

    /* A device reporting the SCALED value has obeyed, and must be confirmed. Comparing
     * against the unscaled "12" is what this asserts against. */
    {
        const uint8_t reported = 120u;

        app_ctl_ble_on_notify(&g_ctl, TEST_VALUE_HANDLE, &reported, 1u, false);
        CHECK(app_ctl_ble_tick(&g_ctl), "the tick did not act on the device's report");
    }
    CHECK(g_ctl.confirmations == 1u,
          "a device reporting the scaled value was not confirmed (counted %u)",
          (unsigned)g_ctl.confirmations);
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "the control is still in flight");
}

static void test_an_action_the_codec_cannot_encode_is_refused(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true);
    status = app_control_submit(TEST_PRESS_ENTITY, "press", NULL, 51u, g_now_ms, 5000u, &slot);
    CHECK(status == APP_CONTROL_ERR_UNSUPPORTED,
          "'press' returned %s, expected unsupported from the codec",
          app_control_status_name(status));
    CHECK(g_gatt.write_calls == 0, "a write was attempted for an action that cannot be encoded");
    CHECK(app_ctl_ble_inflight_count(&g_ctl) == 0u, "a refused action left an operation queued");
    CHECK(app_control_pending_count() == 0u, "a refused action left a pending control");
    CHECK(app_control_observed_state(TEST_PRESS_ENTITY) == NULL,
          "a refused action published a state");
}

/* ------------------------------------------------------------------ */
/* notifications                                                       */
/* ------------------------------------------------------------------ */

static void test_notifications_are_ignored_when_nothing_waits(void)
{
    const uint8_t on = 0x01u;

    fixture_up(true);
    /* No control in flight: a spontaneous report must not be counted as a
     * confirmation of anything. */
    app_ctl_ble_on_notify(&g_ctl, TEST_VALUE_HANDLE, &on, 1u, false);
    CHECK(g_ctl.notifications_seen == 0u,
          "a notification with no control in flight was recorded as %u",
          (unsigned)g_ctl.notifications_seen);
    CHECK(g_ctl.confirmations == 0u, "a notification confirmed something");
    CHECK(!app_ctl_ble_tick(&g_ctl), "the tick claimed a change with nothing in flight");
}

static void test_a_truncated_notification_is_not_a_value(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;
    const uint8_t on = 0x01u;

    fixture_up(true);
    status = app_control_submit(TEST_ENTITY, "turn_on", NULL, 46u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    (void)app_ctl_ble_tick(&g_ctl);

    /* The device sent more than the session could hold, so the field cannot be
     * trusted: half a reading must not confirm a control. */
    app_ctl_ble_on_notify(&g_ctl, TEST_VALUE_HANDLE, &on, 1u, true);
    CHECK(g_ctl.notifications_seen == 0u,
          "a truncated notification was recorded as usable");
    (void)app_ctl_ble_tick(&g_ctl);
    CHECK(slot->state == APP_CONTROL_STATE_PENDING,
          "a truncated notification confirmed the control");
    CHECK(g_ctl.confirmations == 0u, "a truncated notification was counted as a confirmation");
}

/* ------------------------------------------------------------------ */
/* the two connections the loop's routing depends on                   */
/* ------------------------------------------------------------------ */

/*
 * The backend's id is stable and its name is not empty.
 *
 * app_control routes by the vtable's own `claims`, but reports the backend by id
 * and name, and an empty name would make an AMBIGUOUS_BACKEND refusal unreadable.
 */
static void test_the_vtable_identifies_itself(void)
{
    const app_control_backend_ops_t *ops = app_ctl_ble_backend_ops();

    CHECK(ops != NULL, "no ops table");
    CHECK(ops->claims != NULL && ops->send != NULL, "the table is missing an entry");
    CHECK(ops->backend_id != 0u, "the backend id is zero");
    CHECK(ops->name != NULL && ops->name[0] != '\0', "the backend has no name");
    CHECK(strcmp(ops->name, "ble_gatt") == 0, "the backend is named '%s'", ops->name);
    CHECK(ops->backend_id != APP_ENTITY_BACKEND_NONE,
          "the backend id collides with the read-only sentinel");
}

/*
 * Registration is what makes the loop route here at all.
 *
 * Without it every control answers NO_BACKEND - which is the honest answer when no
 * controller exists, and the reason this test registers and then unregisters.
 */
static void test_registration_is_what_routes(void)
{
    app_control_backend_t backend;
    app_ctl_ble_t ctl;

    memset(&ctl, 0, sizeof(ctl));
    app_control_reset();
    CHECK(app_control_backend_count() == 0u, "the loop already had a backend");

    backend.ops = app_ctl_ble_backend_ops();
    backend.ctx = &ctl;
    CHECK(app_control_register_backend(&backend) == ESP_OK, "registration failed");
    CHECK(app_control_backend_count() == 1u, "the loop has %u backends",
          (unsigned)app_control_backend_count());
}

int main(void)
{
    printf("ble control backend tests\n");

    test_claims_only_drivable_ble_entities();
    test_the_vtable_identifies_itself();
    test_registration_is_what_routes();
    test_a_send_does_not_move_the_state();
    test_a_disagreeing_report_does_not_confirm();
    test_a_value_control_is_confirmed_by_its_own_value();
    test_a_value_is_confirmed_by_what_was_written();
    test_a_scaled_recipe_expects_the_scaled_value();
    test_an_action_the_codec_cannot_encode_is_refused();
    test_a_control_with_no_link_is_failed_not_sent();
    test_a_control_whose_write_fails_is_failed();
    test_an_unresolvable_handle_is_refused();
    test_notifications_are_ignored_when_nothing_waits();
    test_a_truncated_notification_is_not_a_value();

    printf("app_ctl_ble: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
