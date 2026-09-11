/*
 * Host regression tests for the unified control loop.
 *
 * The property under test is the one the product rules name explicitly: a successful
 * send is not a state change. Everything here follows from taking that seriously:
 *
 *   - an observed state moves only on confirm() or report(), never on submit();
 *   - a failed, timed-out or canceled control restores the previously confirmed state
 *     rather than leaving the requested value showing;
 *   - a late confirmation for a finished request cannot resurrect it;
 *   - admission refuses what it cannot do honestly: a stale device, a read-only
 *     entity, an unadvertised action, an out-of-range or unparseable value, no
 *     backend, two backends, a second control for the same entity;
 *   - a device that disappears mid-control is failed and cleaned up, not left
 *     pending forever.
 *
 * The backends are injected, so a duplicate, a late or a missing confirmation is a
 * one-line change here instead of something that needs a misbehaving device.
 */

#include <stdio.h>
#include <string.h>

#include "app_control.h"
#include "app_device.h"
#include "app_scan.h"
#include "app_str.h"
#include "app_device_test_hooks.h"
#include "ha_core.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) {                                                        \
            ++failures;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* ---------------- fake backends ---------------- */

#define BACKEND_ID_GENERIC 1u
#define BACKEND_ID_SECOND  2u

typedef struct {
    /* Everything the backend was asked to send, in order. */
    struct {
        char entity_id[HA_CORE_ENTITY_ID_LEN];
        char action[HA_CORE_SERVICE_LEN];
        char value[APP_CONTROL_VALUE_MAX];
    } sent[16];
    size_t sent_count;

    /* Behaviour. */
    app_control_backend_result_t result;
    uint32_t claim_mask;      /* which entities this backend claims */
    bool claim_everything;
    bool has_claims;
} fake_backend_t;

static fake_backend_t g_generic;
static fake_backend_t g_second;

/* Entity ids this fake claims, by ordinal. A bitmask keeps the predicate trivial. */
static bool fake_claims(void *ctx, const app_entity_binding_t *entity)
{
    fake_backend_t *backend = (fake_backend_t *)ctx;

    if (!backend->has_claims) {
        return false;
    }
    if (backend->claim_everything) {
        return true;
    }
    for (size_t i = 0u; i < 32u; ++i) {
        char expected[HA_CORE_ENTITY_ID_LEN];

        (void)snprintf(expected, sizeof(expected), "switch.dev%02u_outlet",
                       (unsigned)i);
        if ((backend->claim_mask & (1u << i)) != 0u &&
            strcmp(entity->entity_id, expected) == 0) {
            return true;
        }
    }
    return false;
}

static app_control_backend_result_t fake_send(void *ctx, const char *entity_id,
                                              const char *action, const char *value)
{
    fake_backend_t *backend = (fake_backend_t *)ctx;

    if (backend->sent_count < 16u) {
        (void)app_strlcpy(backend->sent[backend->sent_count].entity_id, entity_id,
                          sizeof(backend->sent[0].entity_id));
        (void)app_strlcpy(backend->sent[backend->sent_count].action, action,
                          sizeof(backend->sent[0].action));
        (void)app_strlcpy(backend->sent[backend->sent_count].value,
                          value != NULL ? value : "",
                          sizeof(backend->sent[0].value));
        backend->sent_count++;
    }
    return backend->result;
}

static app_control_backend_t backend_of(uint8_t id, fake_backend_t *fake,
                                        app_control_backend_ops_t *ops)
{
    app_control_backend_t backend;

    ops->backend_id = id;
    ops->name = "fake";
    ops->claims = fake_claims;
    ops->send = fake_send;
    backend.ops = ops;
    backend.ctx = fake;
    return backend;
}

/* ---------------- fixtures ---------------- */

static app_control_backend_ops_t g_ops_generic;
static app_control_backend_ops_t g_ops_second;

/*
 * Build one device with one writable entity, so a test can state exactly which of the
 * admission conditions it is exercising.
 *
 * A writable entity only exists when recognition produced a drivable recipe, so this
 * goes through the production path: ha_core_device_upsert + ha_core_entity_upsert,
 * then the application binding table is filled the way app_device.c fills it.
 */
static void make_writable_entity(const char *device_id, const char *entity_id,
                                 uint8_t backend, uint32_t mask,
                                 bool has_range, int32_t min_value, int32_t max_value)
{
    ha_device_t device;
    ha_entity_t entity;
    /*
     * The domain is taken from the entity id's own prefix, because ha_core requires
     * the two to agree: an entity id is "<domain>.<object>". Deriving it here means a
     * test cannot build an entity ha_core will reject, and the entity id is the thing
     * the test actually cares about.
     */
    char domain[HA_CORE_DOMAIN_LEN];
    const char *dot = strchr(entity_id, '.');

    if (dot == NULL || (size_t)(dot - entity_id) >= sizeof(domain)) {
        CHECK(false, "entity id '%s' has no usable domain prefix", entity_id);
        return;
    }
    memcpy(domain, entity_id, (size_t)(dot - entity_id));
    domain[dot - entity_id] = '\0';

    /*
     * ha_core refuses an entity whose device is not present, so the device is created
     * first. That check is why this helper takes a device id rather than inventing
     * one: an entity referencing a missing device is a state the application cannot
     * produce, and building tests on top of it would test the wrong thing.
     */
    memset(&device, 0, sizeof(device));
    (void)app_strlcpy(device.id, device_id, sizeof(device.id));
    (void)app_strlcpy(device.name, "Test Device", sizeof(device.name));
    CHECK(ha_core_device_upsert(&device) == HA_CORE_OK, "device insert");

    memset(&entity, 0, sizeof(entity));
    (void)app_strlcpy(entity.entity_id, entity_id, sizeof(entity.entity_id));
    (void)app_strlcpy(entity.unique_id, entity_id, sizeof(entity.unique_id));
    (void)app_strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)app_strlcpy(entity.domain, domain, sizeof(entity.domain));
    (void)app_strlcpy(entity.device_id, device_id, sizeof(entity.device_id));
    (void)app_strlcpy(entity.name, "Outlet", sizeof(entity.name));
    entity.has_entity_name = true;
    entity.enabled = true;
    entity.available = true;
    entity.supported_services = mask;
    CHECK(ha_core_entity_upsert(&entity) == HA_CORE_OK, "entity insert");

    /*
     * Place the application binding directly, through the hook that exists only in
     * host test builds. Whether it comes out writable is decided by the service mask
     * installed above, so a test cannot create a controllable entity that advertises
     * nothing.
     */
    app_device_test_bind_entity(entity_id, device_id, backend, has_range, min_value,
                                max_value);
}

static void reset_world(void)
{
    app_control_backend_t generic;

    memset(&g_generic, 0, sizeof(g_generic));
    memset(&g_second, 0, sizeof(g_second));

    /*
     * app_device_table_reset() zeroes ha_core as well, and app_control_reset() clears
     * the control bookkeeping. Both are needed between tests: resetting only the
     * control tables would leave ha_core full from the previous test, and the next
     * entity insert would fail with HA_CORE_CAPACITY instead of doing what the test
     * asked - which is exactly how this group failed the first time it ran.
     */
    app_device_table_reset();
    app_control_reset();

    g_generic.result = APP_CONTROL_BACKEND_SENT;
    g_second.result = APP_CONTROL_BACKEND_SENT;
    g_generic.claim_everything = true;
    g_generic.has_claims = true;
    g_second.has_claims = false;

    generic = backend_of(BACKEND_ID_GENERIC, &g_generic, &g_ops_generic);
    CHECK(app_control_register_backend(&generic) == ESP_OK,
          "the generic backend registers");
}

/* Register the second backend, which is what makes routing ambiguous. */
static void register_second_backend(void)
{
    app_control_backend_t second = backend_of(BACKEND_ID_SECOND, &g_second,
                                              &g_ops_second);

    CHECK(app_control_register_backend(&second) == ESP_OK,
          "the second backend registers");
}

/* A single controllable switch, online, with both services and a range. */
static void make_default_world(void)
{
    reset_world();
    make_writable_entity("ble_000000000001", "switch.dev00_outlet",
                         BACKEND_ID_GENERIC,
                         HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF |
                             HA_SERVICE_MASK_SET_VALUE,
                         true, 0, 100);
    /*
     * Online is what a generation that saw it produces. Driving the availability
     * directly keeps this test about the control loop rather than about the sweep,
     * which has its own group (app_device).
     */
    app_device_test_mark_availability("ble_000000000001", APP_AVAILABILITY_ONLINE);
}

/* ---------------- admission ---------------- */

static void test_unknown_entity_is_refused(void)
{
    make_default_world();

    CHECK(app_control_submit("switch.nonexistent_x", HA_SERVICE_TURN_ON, NULL, 1u,
                             0u, 1000u, NULL) == APP_CONTROL_ERR_UNKNOWN_ENTITY,
          "an unknown entity is refused");
    CHECK(g_generic.sent_count == 0u, "and nothing was sent");
}

static void test_read_only_entity_is_refused(void)
{
    make_default_world();
    /*
     * A read-only entity: it exists in ha_core, its device exists, and it advertises
     * no service - so the binding helper leaves it with no drivable backend. That is
     * what a recognised but undrivable profile produces, and what every entity in the
     * current corpus is, because no control backend is wired up yet.
     */
    make_writable_entity("ble_000000000002", "sensor.dev01_signal", BACKEND_ID_GENERIC,
                         0u, false, 0, 0);
    {
        const app_entity_binding_t *binding = app_entity_find("sensor.dev01_signal");

        CHECK(binding != NULL, "the binding exists");
        CHECK(binding != NULL && !binding->writable,
              "and is read-only, because the entity advertises no service");
    }
    CHECK(app_control_submit("sensor.dev01_signal", HA_SERVICE_TURN_ON, NULL, 2u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_NOT_WRITABLE,
          "a read-only entity is refused as not writable");
    CHECK(g_generic.sent_count == 0u, "and nothing was sent");
}

static void test_unavailable_device_is_not_controlled(void)
{
    make_default_world();
    /* Make the device stale, which is what a missed scan round produces. */
    app_device_test_mark_availability("ble_000000000001",
                                      APP_AVAILABILITY_UNAVAILABLE);

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 3u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_DEVICE_UNAVAILABLE,
          "a device that is not online is not controlled");
    CHECK(g_generic.sent_count == 0u,
          "and no command was sent into a device that is not there");
}

static void test_unadvertised_action_is_refused(void)
{
    make_default_world();
    /* The entity advertises turn_on/turn_off only. */
    make_writable_entity("ble_000000000003", "switch.dev02_outlet", BACKEND_ID_GENERIC,
                         HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, false, 0,
                         0);
    app_device_test_mark_availability("ble_000000000003", APP_AVAILABILITY_ONLINE);

    CHECK(app_control_submit("switch.dev02_outlet", HA_SERVICE_SET_TEMPERATURE, "21",
                             4u, 0u, 1000u, NULL) == APP_CONTROL_ERR_UNSUPPORTED,
          "an action the entity does not advertise is refused");
    CHECK(app_control_submit("switch.dev02_outlet", "no_such_action", NULL, 5u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_UNSUPPORTED,
          "and an action that does not exist at all");
    CHECK(g_generic.sent_count == 0u, "nothing was sent");
}

static void test_value_bounds_are_enforced(void)
{
    make_default_world();

    /* Within range. */
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "50", 6u, 0u,
                             1000u, NULL) == APP_CONTROL_OK,
          "an in-range value is accepted");

    /*
     * A second control for the same entity is refused while the first is pending. The
     * value here is deliberately IN RANGE: an out-of-range one would be refused by the
     * range check first, and the test would pass without ever reaching the pending
     * check it exists for. (That is not hypothetical - it is how this test first ran.)
     */
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "60", 7u,
                             0u, 1000u, NULL) == APP_CONTROL_ERR_BUSY,
          "a second control for the same entity is refused while one is pending");
    (void)app_control_cancel(6u, "test");

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "101", 8u,
                             0u, 1000u, NULL) == APP_CONTROL_ERR_OUT_OF_RANGE,
          "a value above the range is refused");
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "-1", 9u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_OUT_OF_RANGE,
          "and below it");
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "0", 10u, 0u,
                             1000u, NULL) == APP_CONTROL_OK,
          "the lower bound itself is inside the range");
    (void)app_control_cancel(10u, "test");
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "100", 11u,
                             0u, 1000u, NULL) == APP_CONTROL_OK,
          "and so is the upper bound");
    (void)app_control_cancel(11u, "test");

    /*
     * An unparseable value must NEVER become zero. atoi() maps "abc" to 0, which on a
     * light is "off" and on a thermostat is a real temperature, so the refusal is the
     * only safe answer.
     */
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "abc", 12u,
                             0u, 1000u, NULL) == APP_CONTROL_ERR_OUT_OF_RANGE,
          "an unparseable value is refused rather than coerced to zero");
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "12abc", 13u,
                             0u, 1000u, NULL) == APP_CONTROL_ERR_OUT_OF_RANGE,
          "and so is one with trailing junk");
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_SET_VALUE, "", 14u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_INVALID_ARGUMENT,
          "a required value that is missing is refused");

    /* A value on an action that takes none: refused, not silently dropped. */
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, "50", 15u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_INVALID_ARGUMENT,
          "a value on turn_on is refused");
}

static void test_backend_routing_refuses_ambiguity(void)
{
    make_default_world();

    /* A second backend that claims everything makes every entity ambiguous. */
    g_second.has_claims = true;
    g_second.claim_everything = true;
    register_second_backend();
    CHECK(app_control_backend_count() == 2u, "two backends are registered");

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 20u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_AMBIGUOUS_BACKEND,
          "two claimants is ambiguous, and ambiguity is never permission to guess");
    CHECK(g_generic.sent_count == 0u && g_second.sent_count == 0u,
          "so neither backend was asked to send");

    /* With the second backend no longer claiming anything, routing is unique again. */
    g_second.claim_everything = false;
    g_second.has_claims = false;
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 21u, 0u,
                             1000u, NULL) == APP_CONTROL_OK,
          "one claimant routes");
}

static void test_no_backend_is_refused(void)
{
    reset_world();
    g_generic.has_claims = false;
    make_writable_entity("ble_000000000004", "switch.dev03_outlet", BACKEND_ID_GENERIC,
                         HA_SERVICE_MASK_TURN_ON, false, 0, 0);
    app_device_test_mark_availability("ble_000000000004", APP_AVAILABILITY_ONLINE);

    CHECK(app_control_submit("switch.dev03_outlet", HA_SERVICE_TURN_ON, NULL, 22u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_NO_BACKEND,
          "no claimant is reported rather than guessed at");
}

/* ---------------- the core property ---------------- */

static void test_send_does_not_change_the_state(void)
{
    app_control_slot_t *slot = NULL;

    make_default_world();
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 30u, 0u,
                             5000u, &slot) == APP_CONTROL_OK,
          "the control was accepted");
    CHECK(slot != NULL && slot->state == APP_CONTROL_STATE_PENDING,
          "and is PENDING, not done");
    CHECK(g_generic.sent_count == 1u, "the backend was asked to send");

    /*
     * The property the module exists for. The command left the firmware; nothing is
     * known about the device yet, so the observed state must NOT be "on".
     */
    CHECK(app_control_observed_state("switch.dev00_outlet") == NULL ||
              strcmp(app_control_observed_state("switch.dev00_outlet"), HA_STATE_ON) !=
                  0,
          "a sent command does not make the entity report the requested state");

    CHECK(app_control_confirm(30u), "the confirmation is accepted");
    CHECK(app_control_observed_state("switch.dev00_outlet") != NULL &&
              strcmp(app_control_observed_state("switch.dev00_outlet"), HA_STATE_ON) ==
                  0,
          "and only then does the entity report on");
    {
        const ha_state_t *state = ha_core_state_get("switch.dev00_outlet");

        CHECK(state != NULL && strcmp(state->state, HA_STATE_ON) == 0,
              "ha_core carries the confirmed state too");
    }
    CHECK(app_control_pending_count() == 0u, "nothing is pending any more");
}

static void test_late_confirmation_cannot_resurrect_a_finished_request(void)
{
    make_default_world();

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 40u, 0u,
                             1000u, NULL) == APP_CONTROL_OK,
          "accepted");
    /* The device never answered in time. */
    CHECK(app_control_tick(1000u), "the deadline expired");
    CHECK(app_control_pending_count() == 0u, "the control is finished");
    CHECK(app_control_observed_state("switch.dev00_outlet") == NULL ||
              strcmp(app_control_observed_state("switch.dev00_outlet"), HA_STATE_ON) !=
                  0,
          "and the requested state was NOT published");

    /*
     * The device answers after the deadline. The caller has already been told the
     * control timed out, so a confirmation now must not move the state: doing so would
     * contradict the answer already given and could overwrite a newer request's
     * result.
     */
    CHECK(!app_control_confirm(40u),
          "a late confirmation for a timed-out request is rejected");
    CHECK(app_control_observed_state("switch.dev00_outlet") == NULL ||
              strcmp(app_control_observed_state("switch.dev00_outlet"), HA_STATE_ON) !=
                  0,
          "and the state is unchanged");
}

static void test_failure_restores_the_previous_confirmed_state(void)
{
    make_default_world();

    /*
     * Establish a confirmed state first. It comes from the device reporting its own
     * state - the only thing that publishes one - so the test starts from "the device
     * says it is on", which is what a failure has to restore.
     */
    CHECK(app_control_report("switch.dev00_outlet", HA_STATE_ON),
          "the device reports it is on");
    CHECK(strcmp(app_control_observed_state("switch.dev00_outlet"), HA_STATE_ON) == 0,
          "the observed state is on");

    /* A control that the device refuses after it was sent. */
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_OFF, NULL, 51u, 0u,
                             1000u, NULL) == APP_CONTROL_OK,
          "a control is accepted and pending");
    CHECK(app_control_fail(51u, "device_nak"), "the backend reported a failure");
    CHECK(strcmp(app_control_observed_state("switch.dev00_outlet"), HA_STATE_ON) == 0,
          "the previously confirmed state is restored, not left showing off");
}

static void test_backend_failure_before_send_is_reported(void)
{
    make_default_world();
    g_generic.result = APP_CONTROL_BACKEND_FAILED;

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 60u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_BACKEND_FAILED,
          "a backend that cannot send is reported as such");
    CHECK(app_control_pending_count() == 0u, "and nothing is left pending");

    g_generic.result = APP_CONTROL_BACKEND_UNSUPPORTED;
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 61u, 0u,
                             1000u, NULL) == APP_CONTROL_ERR_UNSUPPORTED,
          "an unsupported backend is reported as such");
    CHECK(app_control_pending_count() == 0u, "and nothing is left pending");
}

static void test_a_report_supersedes_a_pending_request(void)
{
    make_default_world();

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 70u, 0u,
                             10000u, NULL) == APP_CONTROL_OK,
          "accepted and pending");
    /*
     * The device reports its own state - it is off. The device is the authority on
     * itself, so waiting for a confirmation it has already contradicted would be
     * pointless, and the request is failed.
     */
    CHECK(app_control_report("switch.dev00_outlet", HA_STATE_OFF),
          "the report is accepted");
    CHECK(app_control_pending_count() == 0u, "the pending control is finished");
    CHECK(strcmp(app_control_observed_state("switch.dev00_outlet"), HA_STATE_OFF) == 0,
          "and the device's own report is what the entity shows");
    CHECK(!app_control_confirm(70u),
          "a confirmation arriving after the report is rejected");
}

static void test_one_control_per_entity_at_a_time(void)
{
    make_default_world();

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 80u, 0u,
                             10000u, NULL) == APP_CONTROL_OK,
          "the first control is accepted");
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_OFF, NULL, 81u, 0u,
                             10000u, NULL) == APP_CONTROL_ERR_BUSY,
          "a second control for the same entity is refused");
    CHECK(g_generic.sent_count == 1u,
          "so only one command was sent, got %u", (unsigned)g_generic.sent_count);

    /* After the first finishes, a new one is accepted. */
    CHECK(app_control_confirm(80u), "the first is confirmed");
    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_OFF, NULL, 82u, 0u,
                             10000u, NULL) == APP_CONTROL_OK,
          "and then a new control is accepted");
}

static void test_capacity_is_bounded_and_reported(void)
{
    make_default_world();

    /* Fill the pending table with distinct entities. */
    for (uint32_t i = 0u; i < APP_CONTROL_MAX_PENDING; ++i) {
        char device_id[HA_CORE_ID_LEN];
        char entity_id[HA_CORE_ENTITY_ID_LEN];

        (void)snprintf(device_id, sizeof(device_id), "ble_00000000%04u", (unsigned)i);
        (void)snprintf(entity_id, sizeof(entity_id), "switch.dev%02u_outlet",
                       (unsigned)i);
        make_writable_entity(device_id, entity_id, BACKEND_ID_GENERIC,
                             HA_SERVICE_MASK_TURN_ON, false, 0, 0);
        app_device_test_mark_availability(device_id, APP_AVAILABILITY_ONLINE);
        CHECK(app_control_submit(entity_id, HA_SERVICE_TURN_ON, NULL, 100u + i, 0u,
                                 10000u, NULL) == APP_CONTROL_OK,
              "control %u accepted", (unsigned)i);
    }
    CHECK(app_control_pending_count() == APP_CONTROL_MAX_PENDING,
          "the table is full, got %u", (unsigned)app_control_pending_count());

    /* One more is refused rather than silently dropped or overflowing. */
    make_writable_entity("ble_000000000099", "switch.dev99_outlet", BACKEND_ID_GENERIC,
                         HA_SERVICE_MASK_TURN_ON, false, 0, 0);
    app_device_test_mark_availability("ble_000000000099", APP_AVAILABILITY_ONLINE);
    CHECK(app_control_submit("switch.dev99_outlet", HA_SERVICE_TURN_ON, NULL, 999u, 0u,
                             10000u, NULL) == APP_CONTROL_ERR_NO_CAPACITY,
          "a control beyond capacity is refused with a reason");
}

/* ---------------- disappearing devices ---------------- */

static void test_a_device_that_disappears_mid_control_is_failed(void)
{
    make_default_world();

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 90u, 0u,
                             100000u, NULL) == APP_CONTROL_OK,
          "control accepted and pending");

    /* The device goes away: a scan generation finished without seeing it. */
    app_device_test_mark_availability("ble_000000000001",
                                      APP_AVAILABILITY_UNAVAILABLE);

    CHECK(app_control_reconcile() == 1u, "reconciliation failed the control");
    CHECK(app_control_pending_count() == 0u,
          "so no spinner is left waiting for a device that is gone");
    CHECK(!app_control_confirm(90u),
          "and a confirmation from it later is rejected");
}

static void test_an_entity_removed_mid_control_is_failed(void)
{
    make_default_world();

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 91u, 0u,
                             100000u, NULL) == APP_CONTROL_OK,
          "control accepted and pending");

    /* The whole device is removed, taking its entities with it. */
    CHECK(ha_core_device_remove("ble_000000000001") == HA_CORE_OK,
          "the device is removed");
    app_device_test_forget_entity("switch.dev00_outlet");

    CHECK(app_control_reconcile() == 1u, "reconciliation failed the control");
    CHECK(app_control_pending_count() == 0u, "nothing is pending");
    CHECK(!app_control_confirm(91u),
          "and a confirmation for a removed entity is rejected");
}

static void test_reconcile_leaves_healthy_controls_alone(void)
{
    make_default_world();

    CHECK(app_control_submit("switch.dev00_outlet", HA_SERVICE_TURN_ON, NULL, 92u, 0u,
                             100000u, NULL) == APP_CONTROL_OK,
          "control accepted and pending");
    CHECK(app_control_reconcile() == 0u,
          "a control whose device is online is left pending");
    CHECK(app_control_pending_count() == 1u, "still pending");
    CHECK(app_control_confirm(92u), "and it can still be confirmed");
}

int main(void)
{
    test_unknown_entity_is_refused();
    test_read_only_entity_is_refused();
    test_unavailable_device_is_not_controlled();
    test_unadvertised_action_is_refused();
    test_value_bounds_are_enforced();
    test_backend_routing_refuses_ambiguity();
    test_no_backend_is_refused();

    test_send_does_not_change_the_state();
    test_late_confirmation_cannot_resurrect_a_finished_request();
    test_failure_restores_the_previous_confirmed_state();
    test_backend_failure_before_send_is_reported();
    test_a_report_supersedes_a_pending_request();
    test_one_control_per_entity_at_a_time();
    test_capacity_is_bounded_and_reported();

    test_a_device_that_disappears_mid_control_is_failed();
    test_an_entity_removed_mid_control_is_failed();
    test_reconcile_leaves_healthy_controls_alone();

    printf("app_control: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
