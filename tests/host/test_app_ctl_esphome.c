/*
 * The ESPHome control backend, driven through the REAL app_control loop.
 *
 * The same property as the BLE backend, and on ESPHome it is easier to get wrong: a
 * state report arrives on the same subscription for an entity the device already had a
 * value for, so confirming on "a report arrived" would mark a refused command as
 * confirmed. Every assertion below is therefore about ORDER and about WHICH state
 * arrived, not merely that something did.
 *
 * The API client is scripted, so each moment is reached deliberately. The wire is not
 * exercised here: esphome_l2 has its own groups for the framing, the Noise handshake
 * and the protobuf codec, and the interaction with a real ESPHome node is item 5c of
 * docs/hardware-acceptance.md.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_control.h"
#include "app_ctl_esphome.h"
#include "app_device.h"
#include "app_device_test_hooks.h"
#include "app_str.h"
#include "esp_err.h"
#include "esphome_api.h"
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
/* a scripted API client                                               */
/* ------------------------------------------------------------------ */

#define TEST_KEY 0x0000A1B2u
#define TEST_DEVICE_ID 0x1234u

typedef struct {
    bool ready;
    int send_calls;
    esphome_api_command_t last_command;
    esp_err_t next_send;
    /* The discovered entity list, so find_entity has something to answer with. */
    esphome_api_entity_t entities[2];
    size_t entity_count;
} fake_api_t;

static fake_api_t g_api;

static esp_err_t fa_send(void *ctx, const esphome_api_command_t *command)
{
    fake_api_t *a = (fake_api_t *)ctx;

    a->send_calls++;
    if (command != NULL) {
        a->last_command = *command;
    }
    return a->next_send;
}

static bool fa_is_ready(void *ctx)
{
    return ((fake_api_t *)ctx)->ready;
}

static const esphome_api_entity_t *fa_find_entity(void *ctx, uint32_t key)
{
    fake_api_t *a = (fake_api_t *)ctx;

    for (size_t i = 0u; i < a->entity_count; ++i) {
        if (a->entities[i].key == key) {
            return &a->entities[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* fixture                                                             */
/* ------------------------------------------------------------------ */

static uint32_t g_now_ms;

static uint32_t now_ms(void)
{
    return g_now_ms;
}

static app_ctl_esphome_t g_ctl;

#define TEST_SWITCH "switch.esph_node_relay"
#define TEST_NUMBER "number.esph_node_level"
#define TEST_SELECT "select.esph_node_mode"
#define TEST_DEVICE "esphome_esph_node"

/* A discovered entity as the API client would report it for this key. */
static void add_entity(esphome_api_entity_kind_t kind, uint32_t key, const char *object_id)
{
    esphome_api_entity_t *e;

    if (g_api.entity_count >= sizeof(g_api.entities) / sizeof(g_api.entities[0])) {
        return;
    }
    e = &g_api.entities[g_api.entity_count++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->key = key;
    e->device_id = TEST_DEVICE_ID;
    (void)app_strlcpy(e->object_id, object_id, sizeof(e->object_id));
}

/*
 * Install the backend into a freshly reset loop, with one entity of the wanted kind.
 *
 * A fresh binding table matters as much as a fresh loop: an entity left from a previous
 * case would make the routing assertions describe the wrong world.
 */
/*
 * The binding a recipe would produce. `has_range` is only set for a domain whose action
 * takes a NUMBER: the loop applies the declared range to a value-carrying action, so a
 * range on a text-valued entity (a select) would make the loop refuse the option as
 * out-of-range before any backend saw it - which is how this fixture failed its first
 * run, and it is the loop being right.
 */
static void bind_entity(const char *entity_id, const char *domain, uint32_t key)
{
    const bool numeric = strcmp(domain, "number") == 0;

    app_device_test_bind_entity(entity_id, TEST_DEVICE, DEVICE_DB_BACKEND_ESPHOME_API, numeric,
                                0, numeric ? 1000 : 0);
    {
        app_entity_binding_t *binding = (app_entity_binding_t *)app_control_lookup(entity_id);

        REQUIRE(binding != NULL && binding->writable,
                "the binding is not writable, so the loop will never route to a backend");
        /* The corpus names the entity portably; the device id comes from discovery. */
        binding->write_target_id = key;
    }
}

static void fixture_up(bool ready, esphome_api_entity_kind_t kind, const char *domain,
                       const char *entity_id, uint32_t mask, uint32_t key)
{
    app_control_backend_t backend;
    ha_device_t device;
    ha_entity_t entity;

    memset(&g_api, 0, sizeof(g_api));
    g_api.ready = ready;
    g_api.next_send = ESP_OK;
    g_api.entity_count = 0u;
    if (kind != ESPHOME_API_ENTITY_UNKNOWN) {
        add_entity(kind, key, entity_id);
    }
    memset(&g_ctl, 0, sizeof(g_ctl));
    g_now_ms = 1000u;

    g_ctl.api.send = fa_send;
    g_ctl.api.is_ready = fa_is_ready;
    g_ctl.api.find_entity = fa_find_entity;
    g_ctl.api_ctx = &g_api;
    g_ctl.now_ms = now_ms;

    app_device_table_reset();
    app_control_reset();

    memset(&device, 0, sizeof(device));
    (void)app_strlcpy(device.id, TEST_DEVICE, sizeof(device.id));
    (void)app_strlcpy(device.name, "ESPHome Node", sizeof(device.name));
    CHECK(ha_core_device_upsert(&device) == HA_CORE_OK, "the test device was not inserted");

    memset(&entity, 0, sizeof(entity));
    (void)app_strlcpy(entity.entity_id, entity_id, sizeof(entity.entity_id));
    (void)app_strlcpy(entity.unique_id, entity_id, sizeof(entity.unique_id));
    (void)app_strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)app_strlcpy(entity.domain, domain, sizeof(entity.domain));
    (void)app_strlcpy(entity.device_id, TEST_DEVICE, sizeof(entity.device_id));
    (void)app_strlcpy(entity.name, "Test", sizeof(entity.name));
    entity.has_entity_name = true;
    entity.enabled = true;
    entity.available = true;
    entity.supported_services = mask;
    CHECK(ha_core_entity_upsert(&entity) == HA_CORE_OK, "the test entity was not inserted");

    bind_entity(entity_id, domain, key);

    backend.ops = app_ctl_esphome_backend_ops();
    backend.ctx = &g_ctl;
    CHECK(app_control_register_backend(&backend) == ESP_OK, "the backend did not register");
}

/* ------------------------------------------------------------------ */
/* claims and the vtable                                               */
/* ------------------------------------------------------------------ */

static void test_claims_and_vtable(void)
{
    app_ctl_esphome_t ctl;
    app_entity_binding_t entity;

    memset(&ctl, 0, sizeof(ctl));
    memset(&entity, 0, sizeof(entity));

    entity.writable = true;
    entity.backend = DEVICE_DB_BACKEND_ESPHOME_API;
    CHECK(app_ctl_esphome_backend_ops()->claims(&ctl, &entity),
          "a writable ESPHome entity was refused");

    entity.writable = false;
    CHECK(!app_ctl_esphome_backend_ops()->claims(&ctl, &entity), "a read-only entity was claimed");

    entity.writable = true;
    entity.backend = DEVICE_DB_BACKEND_BLE_GATT;
    CHECK(!app_ctl_esphome_backend_ops()->claims(&ctl, &entity),
          "a BLE entity was claimed by the ESPHome backend - two backends would make the "
          "loop report AMBIGUOUS_BACKEND");
    entity.backend = DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE;
    CHECK(!app_ctl_esphome_backend_ops()->claims(&ctl, &entity), "a Zigbee entity was claimed");
    entity.backend = APP_ENTITY_BACKEND_NONE;
    CHECK(!app_ctl_esphome_backend_ops()->claims(&ctl, &entity), "a backend-less entity was claimed");
    CHECK(!app_ctl_esphome_backend_ops()->claims(&ctl, NULL), "NULL was claimed");

    CHECK(app_ctl_esphome_backend_ops()->name != NULL, "the backend has no name");
    CHECK(strcmp(app_ctl_esphome_backend_ops()->name, "esphome_api") == 0,
          "the backend is named '%s'", app_ctl_esphome_backend_ops()->name);
    CHECK(app_ctl_esphome_backend_ops()->backend_id != 0u, "the backend id is zero");
    /* The ids must differ, or app_control cannot tell the backends apart in a report. */
    CHECK(app_ctl_esphome_backend_ops()->backend_id != 0x02u,
          "the ESPHome backend id collides with the BLE one");
}

/* ------------------------------------------------------------------ */
/* the rule                                                            */
/* ------------------------------------------------------------------ */

static void test_a_switch_command_is_not_a_state_change(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true, ESPHOME_API_ENTITY_SWITCH, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, TEST_KEY);

    status = app_control_submit(TEST_SWITCH, "turn_on", NULL, 61u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    CHECK(g_api.send_calls == 0, "the command was sent inside submit, on the caller's task");
    CHECK(app_control_observed_state(TEST_SWITCH) == NULL,
          "a state was published before the device said anything");

    /* The worker sends. Still nothing has moved. */
    REQUIRE(app_ctl_esphome_tick(&g_ctl), "the tick did not send the command");
    CHECK(g_api.send_calls == 1, "the command was sent %d times", g_api.send_calls);
    CHECK(g_api.last_command.kind == ESPHOME_API_COMMAND_SWITCH, "the wrong command kind");
    CHECK(g_api.last_command.key == TEST_KEY, "the command carried key 0x%x",
          (unsigned)g_api.last_command.key);
    CHECK(g_api.last_command.device_id == TEST_DEVICE_ID,
          "the command carried device id 0x%x, expected the discovered one",
          (unsigned)g_api.last_command.device_id);
    CHECK(g_api.last_command.value.switch_.state == true, "turn_on did not send state=true");
    CHECK(app_control_pending_count() == 1u, "the control left PENDING on the strength of its send");
    CHECK(app_control_observed_state(TEST_SWITCH) == NULL,
          "a state was published after the send but before any report");

    /* The device reports ON. This is the first moment the state may move. */
    {
        esphome_api_state_t state;

        memset(&state, 0, sizeof(state));
        state.kind = ESPHOME_API_ENTITY_SWITCH;
        state.key = TEST_KEY;
        state.value.boolean = true;
        app_ctl_esphome_on_state(&g_ctl, &state);
    }
    CHECK(app_control_observed_state(TEST_SWITCH) == NULL,
          "the report published a state from the client's task instead of the worker");
    CHECK(app_ctl_esphome_tick(&g_ctl), "the tick did not act on the device's report");
    CHECK(g_ctl.confirmations == 1u, "confirmations counted %u", (unsigned)g_ctl.confirmations);
    CHECK(app_control_observed_state(TEST_SWITCH) != NULL &&
              strcmp(app_control_observed_state(TEST_SWITCH), HA_STATE_ON) == 0,
          "the observed state is not the one the device reported");
    CHECK(app_ctl_esphome_inflight_count(&g_ctl) == 0u, "the control is still in flight");
}

/*
 * A report that disagrees does not confirm.
 *
 * The case ESPHome makes easy to get wrong: the entity already had a value, so a report
 * arrives whatever the command did. Confirming on its arrival would mark a refused
 * command as confirmed and the UI would show a state the device never entered.
 */
static void test_a_disagreeing_report_does_not_confirm(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true, ESPHOME_API_ENTITY_SWITCH, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, TEST_KEY);
    status = app_control_submit(TEST_SWITCH, "turn_on", NULL, 62u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    REQUIRE(app_ctl_esphome_tick(&g_ctl), "the command was not sent");

    {
        esphome_api_state_t state;

        memset(&state, 0, sizeof(state));
        state.kind = ESPHOME_API_ENTITY_SWITCH;
        state.key = TEST_KEY;
        state.value.boolean = false; /* the device says OFF */
        app_ctl_esphome_on_state(&g_ctl, &state);
        (void)app_ctl_esphome_tick(&g_ctl);
    }
    CHECK(g_ctl.confirmations == 0u, "a disagreeing report confirmed the control");
    CHECK(app_ctl_esphome_inflight_count(&g_ctl) == 1u, "the control ended on a disagreement");
    CHECK(app_control_observed_state(TEST_SWITCH) == NULL, "a disagreement published a state");

    /* The deadline is what ends it, and the state stays unpublished. */
    g_now_ms += APP_CTL_ESPHOME_CONFIRM_TIMEOUT_MS + 1u;
    CHECK(app_ctl_esphome_tick(&g_ctl), "the deadline did not end the control");
    CHECK(g_ctl.deadlines_expired == 1u, "the expiry was not counted");
    CHECK(app_ctl_esphome_inflight_count(&g_ctl) == 0u, "the expired control is still in flight");
    CHECK(app_control_observed_state(TEST_SWITCH) == NULL,
          "an expired control left the requested state showing");
}

/*
 * `missing` is not a state.
 *
 * The API reports `missing` when the device has no value for an entity. Treating that as
 * a value would confirm a control from an absence; treating the last value as current
 * would confirm from stale data armed with a `missing` flag.
 */
static void test_missing_is_not_a_state(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true, ESPHOME_API_ENTITY_SWITCH, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, TEST_KEY);
    status = app_control_submit(TEST_SWITCH, "turn_on", NULL, 63u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    REQUIRE(app_ctl_esphome_tick(&g_ctl), "the command was not sent");

    {
        esphome_api_state_t state;

        memset(&state, 0, sizeof(state));
        state.kind = ESPHOME_API_ENTITY_SWITCH;
        state.key = TEST_KEY;
        state.missing = true;
        state.value.boolean = true; /* a value alongside `missing`, which must not count */
        app_ctl_esphome_on_state(&g_ctl, &state);
        (void)app_ctl_esphome_tick(&g_ctl);
    }
    CHECK(g_ctl.states_seen == 0u, "a `missing` report was recorded as a state");
    CHECK(g_ctl.confirmations == 0u, "a `missing` report confirmed the control");
}

/* ------------------------------------------------------------------ */
/* number and select                                                   */
/* ------------------------------------------------------------------ */

static void test_a_number_value_is_confirmed_by_its_own_value(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true, ESPHOME_API_ENTITY_NUMBER, "number", TEST_NUMBER,
              HA_SERVICE_MASK_SET_VALUE, TEST_KEY);
    status = app_control_submit(TEST_NUMBER, "set_value", "42", 64u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    REQUIRE(app_ctl_esphome_tick(&g_ctl), "the command was not sent");
    CHECK(g_api.last_command.kind == ESPHOME_API_COMMAND_NUMBER, "the wrong command kind");
    CHECK(g_api.last_command.value.number.state == 42.0f, "the value did not reach the command");

    {
        esphome_api_state_t state;

        memset(&state, 0, sizeof(state));
        state.kind = ESPHOME_API_ENTITY_NUMBER;
        state.key = TEST_KEY;
        state.value.number = 42.0f;
        app_ctl_esphome_on_state(&g_ctl, &state);
        CHECK(app_ctl_esphome_tick(&g_ctl), "the tick did not act on the device's report");
    }
    CHECK(g_ctl.confirmations == 1u,
          "a device that reported the value it was given was not confirmed (counted %u)",
          (unsigned)g_ctl.confirmations);
}

static void test_a_select_option_is_confirmed_by_its_own_text(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true, ESPHOME_API_ENTITY_SELECT, "select", TEST_SELECT,
              HA_SERVICE_MASK_SELECT_OPTION, TEST_KEY);
    status = app_control_submit(TEST_SELECT, "select_option", "eco", 65u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    REQUIRE(app_ctl_esphome_tick(&g_ctl), "the command was not sent");
    CHECK(g_api.last_command.kind == ESPHOME_API_COMMAND_SELECT, "the wrong command kind");
    CHECK(g_api.last_command.value.select.state != NULL &&
              strcmp(g_api.last_command.value.select.state, "eco") == 0,
          "the option did not reach the command");

    {
        esphome_api_state_t state;

        memset(&state, 0, sizeof(state));
        state.kind = ESPHOME_API_ENTITY_SELECT;
        state.key = TEST_KEY;
        (void)app_strlcpy(state.value.text, "eco", sizeof(state.value.text));
        app_ctl_esphome_on_state(&g_ctl, &state);
        CHECK(app_ctl_esphome_tick(&g_ctl), "the tick did not act on the device's report");
    }
    CHECK(g_ctl.confirmations == 1u, "the selected option did not confirm (counted %u)",
          (unsigned)g_ctl.confirmations);
}

/* ------------------------------------------------------------------ */
/* refusals                                                            */
/* ------------------------------------------------------------------ */

static void test_no_live_session_is_failed_not_sent(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(false, ESPHOME_API_ENTITY_SWITCH, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, TEST_KEY);
    status = app_control_submit(TEST_SWITCH, "turn_on", NULL, 66u, g_now_ms, 5000u, &slot);
    CHECK(status == APP_CONTROL_ERR_BACKEND_FAILED,
          "a control with no session returned %s, expected backend_failed",
          app_control_status_name(status));
    CHECK(g_api.send_calls == 0, "a command was sent without a session");
    CHECK(app_control_pending_count() == 0u, "a refused control is still pending");
    CHECK(g_ctl.refused_not_ready == 1u, "the refusal was not recorded");
}

/*
 * A key the device does not expose cannot be commanded.
 *
 * This is the entity that was in the corpus but has since been removed from the node, or
 * a key that was never discovered. Either way there is nothing to command, and the
 * failure must be reported rather than sent as a command for an unknown entity.
 */
static void test_an_undiscovered_key_is_refused(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    /* No discovered entity at all. */
    fixture_up(true, ESPHOME_API_ENTITY_UNKNOWN, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, TEST_KEY);
    status = app_control_submit(TEST_SWITCH, "turn_on", NULL, 67u, g_now_ms, 5000u, &slot);
    CHECK(status == APP_CONTROL_ERR_BACKEND_FAILED,
          "an undiscovered key returned %s, expected backend_failed",
          app_control_status_name(status));
    CHECK(g_api.send_calls == 0, "a command was sent for an entity that was not discovered");
    CHECK(g_ctl.refused_unknown_key == 1u, "the refusal was not recorded");
}

/*
 * A command the API client refuses is reported as a failed control, not as sent.
 */
static void test_a_refused_send_is_a_failed_control(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    fixture_up(true, ESPHOME_API_ENTITY_SWITCH, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, TEST_KEY);
    g_api.next_send = ESP_FAIL;
    status = app_control_submit(TEST_SWITCH, "turn_on", NULL, 68u, g_now_ms, 5000u, &slot);
    REQUIRE(status == APP_CONTROL_OK && slot != NULL, "submit returned %s",
            app_control_status_name(status));
    (void)app_ctl_esphome_tick(&g_ctl);
    CHECK(g_ctl.commands_failed == 1u, "the failed send was not counted");
    CHECK(g_ctl.confirmations == 0u, "a failed send was confirmed");
    CHECK(app_ctl_esphome_inflight_count(&g_ctl) == 0u, "the failed control is still in flight");
    CHECK(app_control_pending_count() == 0u, "the failed control is still pending");
    CHECK(app_control_observed_state(TEST_SWITCH) == NULL, "a failed send published a state");
}

/*
 * An action with no ESPHome equivalent is refused, without guessing a command.
 */
static void test_an_action_without_a_command_is_refused(void)
{
    app_control_slot_t *slot = NULL;
    app_control_status_t status;

    /* A switch entity, which has no `set_value` command. */
    fixture_up(true, ESPHOME_API_ENTITY_SWITCH, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_SET_VALUE, TEST_KEY);
    status = app_control_submit(TEST_SWITCH, "set_value", "5", 69u, g_now_ms, 5000u, &slot);
    CHECK(status == APP_CONTROL_ERR_UNSUPPORTED,
          "set_value on a switch returned %s, expected unsupported",
          app_control_status_name(status));
    CHECK(g_api.send_calls == 0, "a command was guessed for an action with no equivalent");
    CHECK(g_ctl.refused_unsupported_action == 1u, "the refusal was not recorded");
}

/* A report for an entity nobody is controlling is ignored, not counted. */
static void test_a_state_with_no_control_in_flight_is_ignored(void)
{
    esphome_api_state_t state;

    fixture_up(true, ESPHOME_API_ENTITY_SWITCH, "switch", TEST_SWITCH,
              HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF, TEST_KEY);
    memset(&state, 0, sizeof(state));
    state.kind = ESPHOME_API_ENTITY_SWITCH;
    state.key = TEST_KEY;
    state.value.boolean = true;
    app_ctl_esphome_on_state(&g_ctl, &state);
    CHECK(g_ctl.states_seen == 0u, "a report with no control in flight was recorded as %u",
          (unsigned)g_ctl.states_seen);
    CHECK(!app_ctl_esphome_tick(&g_ctl), "the tick claimed a change with nothing in flight");
}

int main(void)
{
    printf("esphome control backend tests\n");

    test_claims_and_vtable();
    test_a_switch_command_is_not_a_state_change();
    test_a_disagreeing_report_does_not_confirm();
    test_missing_is_not_a_state();
    test_a_number_value_is_confirmed_by_its_own_value();
    test_a_select_option_is_confirmed_by_its_own_text();
    test_no_live_session_is_failed_not_sent();
    test_an_undiscovered_key_is_refused();
    test_a_refused_send_is_a_failed_control();
    test_an_action_without_a_command_is_refused();
    test_a_state_with_no_control_in_flight_is_ignored();

    printf("app_ctl_esphome: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
