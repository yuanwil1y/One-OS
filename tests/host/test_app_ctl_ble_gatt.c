/*
 * The firmware binding from the control backend to the GATT session.
 *
 * app_ctl_ble has its own group proving the control policy against a scripted
 * session. What it cannot prove is that the table the FIRMWARE supplies answers the
 * one question only the firmware can: which characteristic, of which device.
 *
 * That question is where a control path does its worst damage when it is wrong. A
 * refusal is visible - the control fails and the reason is recorded. A WRONG HANDLE
 * IS NOT: the write succeeds, the device acknowledges it, and the wrong attribute has
 * been changed. So the refusals are what this group tests hardest, each one reached
 * deliberately:
 *
 *   - no live session
 *   - the entity's device is not the connected peer
 *   - a device id that is not a BLE one, or is malformed hex
 *   - a characteristic index with no value handle
 *   - a characteristic that is not writable
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_ble_gatt.h"
#include "app_ctl_ble.h"
#include "app_ctl_ble_gatt.h"
#include "app_str.h"
#include "esp_err.h"

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
/* fixtures                                                            */
/* ------------------------------------------------------------------ */

#define VALUE_HANDLE 0x0021u
#define CCCD_HANDLE 0x0022u

/* A service holding one writable characteristic with a CCCD. */
static esphome_ble_gatt_service_t g_services[2];
static esphome_ble_gatt_characteristic_t g_characteristics[2];
static esphome_ble_gatt_descriptor_t g_descriptors[2];
static esphome_ble_gatt_db_t g_db;

static app_ble_gatt_session_t g_session;
static app_ctl_ble_gatt_t g_adapter;
static app_ctl_ble_t g_backend;

/*
 * The device id the materialiser would build for this address: "ble_" + the address
 * type as two hex digits + the twelve hex digits of the DISPLAY-ORDER address, which
 * is what app_device_identity_of_ble() produces.
 */
#define PEER_ADDRESS_TYPE 0u
#define PEER_DEVICE_ID "ble_00c4994c1a2b3d"

static void seed_database(uint8_t properties, uint16_t value_handle)
{
    memset(g_services, 0, sizeof(g_services));
    memset(g_characteristics, 0, sizeof(g_characteristics));
    memset(g_descriptors, 0, sizeof(g_descriptors));

    g_db.services = g_services;
    g_db.service_capacity = 2u;
    g_db.characteristics = g_characteristics;
    g_db.characteristic_capacity = 2u;
    g_db.descriptors = g_descriptors;
    g_db.descriptor_capacity = 2u;

    g_db.service_count = 1u;
    g_services[0].start_handle = 0x0001u;
    g_services[0].end_handle = 0x0004u;
    g_services[0].first_characteristic = 0u;
    g_services[0].characteristic_count = 1u;
    g_services[0].uuid.width = ESPHOME_BLE_UUID16;
    g_services[0].uuid.value[0] = 0x0fu;
    g_services[0].uuid.value[1] = 0x18u;

    g_db.characteristic_count = 1u;
    g_characteristics[0].definition_handle = 0x0002u;
    g_characteristics[0].value_handle = value_handle;
    g_characteristics[0].end_handle = 0x0003u;
    g_characteristics[0].properties = properties;
    g_characteristics[0].first_descriptor = 0u;
    g_characteristics[0].descriptor_count = 1u;
    g_characteristics[0].uuid.width = ESPHOME_BLE_UUID16;
    g_characteristics[0].uuid.value[0] = 0x19u;
    g_characteristics[0].uuid.value[1] = 0x2au;

    g_db.descriptor_count = 1u;
    g_descriptors[0].handle = CCCD_HANDLE;
    g_descriptors[0].uuid.width = ESPHOME_BLE_UUID16;
    g_descriptors[0].uuid.value[0] = 0x02u;
    g_descriptors[0].uuid.value[1] = 0x29u;
}

/*
 * Put the session in the state a completed open leaves it in. Done directly rather
 * than by opening: this group is about the resolver's decision, and driving a whole
 * GATT session through a scripted transport to reach PHASE_READY is the other group's
 * job. `connected` and `phase` are the two fields the resolver reads.
 */
static void fixture_up(bool ready, uint8_t properties, uint16_t value_handle)
{
    memset(&g_session, 0, sizeof(g_session));
    seed_database(properties, value_handle);
    g_session.db = g_db;
    if (ready) {
        g_session.phase = APP_BLE_PHASE_READY;
        g_session.connected = true;
    } else {
        g_session.phase = APP_BLE_PHASE_IDLE;
        g_session.connected = false;
    }
    /* The connected peer, in display order, matching PEER_DEVICE_ID. */
    g_session.peer.address[0] = 0xc4u;
    g_session.peer.address[1] = 0x99u;
    g_session.peer.address[2] = 0x4cu;
    g_session.peer.address[3] = 0x1au;
    g_session.peer.address[4] = 0x2bu;
    g_session.peer.address[5] = 0x3du;
    g_session.peer.address_type = PEER_ADDRESS_TYPE;

    CHECK(app_ctl_ble_gatt_install(&g_adapter, &g_session, &g_backend) == ESP_OK,
          "install failed");
}

static void entity_of(app_entity_binding_t *entity, uint32_t write_target_id)
{
    memset(entity, 0, sizeof(*entity));
    (void)app_strlcpy(entity->entity_id, "switch.test_bulb", sizeof(entity->entity_id));
    (void)app_strlcpy(entity->device_id, PEER_DEVICE_ID, sizeof(entity->device_id));
    entity->writable = true;
    entity->backend = DEVICE_DB_BACKEND_BLE_GATT;
    entity->write_target_id = write_target_id;
}

static void device_of(app_device_binding_t *device, const char *device_id)
{
    memset(device, 0, sizeof(*device));
    (void)app_strlcpy(device->device_id, device_id, sizeof(device->device_id));
    device->availability = APP_AVAILABILITY_ONLINE;
}

/* ------------------------------------------------------------------ */
/* the writable-property rule                                          */
/* ------------------------------------------------------------------ */

/*
 * The property bits are the Bluetooth specification's, and the transport copies
 * NimBLE's ble_gatt_chr::properties verbatim - so this is a check on the spec's bit
 * assignment, not on a convention this project invented.
 */
static void test_writable_properties(void)
{
    /* Read only (0x02): not writable. This is the case that must be refused. */
    CHECK(!app_ctl_ble_gatt_property_is_writable(0x02u), "read-only was treated as writable");
    /* Read + Notify (0x12): the common sensor shape, also not writable. */
    CHECK(!app_ctl_ble_gatt_property_is_writable(0x12u),
          "read+notify was treated as writable");
    /* Write (0x08). */
    CHECK(app_ctl_ble_gatt_property_is_writable(0x08u), "write was refused");
    /* Write Without Response (0x04) alone: still a command the device accepts. */
    CHECK(app_ctl_ble_gatt_property_is_writable(0x04u), "write-without-response was refused");
    /* Read + Write (0x0a), and the full house. */
    CHECK(app_ctl_ble_gatt_property_is_writable(0x0au), "read+write was refused");
    CHECK(app_ctl_ble_gatt_property_is_writable(0xffu), "everything was refused");
    /* Nothing at all. */
    CHECK(!app_ctl_ble_gatt_property_is_writable(0x00u), "an empty property byte was writable");
    /* Notify or Indicate alone are not write permission. */
    CHECK(!app_ctl_ble_gatt_property_is_writable(0x10u), "notify was treated as writable");
    CHECK(!app_ctl_ble_gatt_property_is_writable(0x20u), "indicate was treated as writable");
}

/* ------------------------------------------------------------------ */
/* the refusals                                                        */
/* ------------------------------------------------------------------ */

static void test_a_live_session_resolves(void)
{
    app_entity_binding_t entity;
    app_device_binding_t device;
    uint16_t handle = 0u;

    fixture_up(true, 0x08u, VALUE_HANDLE); /* write */
    entity_of(&entity, 0u);                /* characteristic index 0 */
    device_of(&device, PEER_DEVICE_ID);

    CHECK(app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a writable characteristic of the connected peer did not resolve");
    CHECK(handle == VALUE_HANDLE, "resolved handle is 0x%04x, expected 0x%04x",
          (unsigned)handle, (unsigned)VALUE_HANDLE);
    CHECK(g_adapter.resolve_refusals_wrong_peer == 0u, "a valid resolve counted a refusal");
}

static void test_no_live_session_is_refused(void)
{
    app_entity_binding_t entity;
    app_device_binding_t device;
    uint16_t handle = 0xFFFFu;

    fixture_up(false, 0x08u, VALUE_HANDLE);
    entity_of(&entity, 0u);
    device_of(&device, PEER_DEVICE_ID);

    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a control resolved with no live session");
    CHECK(handle == 0u, "a refused resolve wrote a handle: 0x%04x", (unsigned)handle);
    CHECK(g_adapter.resolve_refusals_not_open == 1u, "the refusal was not recorded");

    /* A session that is connecting has a peer but no link yet. */
    fixture_up(true, 0x08u, VALUE_HANDLE);
    g_session.phase = APP_BLE_PHASE_CONNECTING;
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a connecting session resolved a control");
    /* And a session whose link dropped under it: the phase may still say READY. */
    fixture_up(true, 0x08u, VALUE_HANDLE);
    g_session.connected = false;
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a disconnected session resolved a control");
}

/*
 * The control must be for the peer that is connected.
 *
 * This is the refusal that matters most: a handle resolved against the wrong device's
 * database - or the right index against the wrong device - writes successfully to an
 * attribute nobody meant to change, and no error is reported anywhere.
 */
static void test_a_different_peer_is_refused(void)
{
    app_entity_binding_t entity;
    app_device_binding_t device;
    uint16_t handle = 0u;

    fixture_up(true, 0x08u, VALUE_HANDLE);
    entity_of(&entity, 0u);

    /* A different BLE device: one byte apart in the address. */
    device_of(&device, "ble_00c4994c1a2b3e");
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a control for another BLE device resolved against the connected peer");
    CHECK(handle == 0u, "a refused resolve wrote a handle");
    CHECK(g_adapter.resolve_refusals_wrong_peer == 1u, "the refusal was not recorded");

    /* A device of another family entirely. */
    device_of(&device, "wifi_020000000001");
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a Wi-Fi device resolved as a BLE peer");

    /* A malformed BLE id: hex characters that are not hex. */
    device_of(&device, "ble_00c4994c1a2bzz");
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a malformed device id resolved");

    /* An id too short to hold an address. */
    device_of(&device, "ble_00");
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a truncated device id resolved");

    /* The reverse byte order is a DIFFERENT device, not the same one: the convention
     * is display order on both sides, and comparing order-insensitively here would
     * accept a peer whose address is the connected one backwards. */
    device_of(&device, "ble_003d2b1a4c99c4");
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "the reversed address resolved as the connected peer");
}

static void test_an_unusable_index_is_refused(void)
{
    app_entity_binding_t entity;
    app_device_binding_t device;
    uint16_t handle = 0u;

    fixture_up(true, 0x08u, VALUE_HANDLE);
    device_of(&device, PEER_DEVICE_ID);

    /* The recipe declared no write target. */
    entity_of(&entity, DEVICE_DB_NO_INDEX);
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "DEVICE_DB_NO_INDEX resolved to a handle");
    CHECK(g_adapter.resolve_refusals_bad_index == 1u, "the refusal was not recorded");

    /* An index past the end of the discovered table. */
    entity_of(&entity, 7u);
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "an out-of-range characteristic index resolved");
    CHECK(handle == 0u, "a refused resolve wrote a handle");

    /* A characteristic whose value handle is zero: the database has the entry but
     * discovery did not fill it in, so there is nothing to write to. */
    seed_database(0x08u, 0u);
    g_session.db = g_db;
    entity_of(&entity, 0u);
    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a zero value handle resolved");
}

static void test_a_read_only_characteristic_is_refused(void)
{
    app_entity_binding_t entity;
    app_device_binding_t device;
    uint16_t handle = 0u;

    /* Read + Notify: the shape a sensor reports on, and not a command surface. */
    fixture_up(true, 0x12u, VALUE_HANDLE);
    entity_of(&entity, 0u);
    device_of(&device, PEER_DEVICE_ID);

    CHECK(!app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "a read-only characteristic resolved as a write target");
    CHECK(handle == 0u, "a refused resolve wrote a handle");
    CHECK(g_adapter.resolve_refusals_not_writable == 1u, "the refusal was not recorded");

    /* Write Without Response alone is accepted, so the refusal above is about the
     * property and not about requiring the response form. */
    fixture_up(true, 0x04u, VALUE_HANDLE);
    CHECK(app_ctl_ble_gatt_resolve(&g_adapter, &entity, &device, &handle),
          "write-without-response did not resolve");
    CHECK(handle == VALUE_HANDLE, "the wrong handle resolved");
}

/* ------------------------------------------------------------------ */
/* the ops table                                                       */
/* ------------------------------------------------------------------ */

static void test_the_installed_table(void)
{
    app_ctl_ble_gatt_t adapter;
    app_ble_gatt_session_t session;
    app_ctl_ble_t backend;

    memset(&session, 0, sizeof(session));
    CHECK(app_ctl_ble_gatt_install(NULL, &session, &backend) == ESP_ERR_INVALID_ARG,
          "a NULL adapter was accepted");
    CHECK(app_ctl_ble_gatt_install(&adapter, NULL, &backend) == ESP_ERR_INVALID_ARG,
          "a NULL session was accepted");
    CHECK(app_ctl_ble_gatt_install(&adapter, &session, NULL) == ESP_ERR_INVALID_ARG,
          "a NULL output was accepted");

    CHECK(app_ctl_ble_gatt_install(&adapter, &session, &backend) == ESP_OK, "install failed");
    CHECK(backend.gatt.write != NULL && backend.gatt.is_ready != NULL &&
              backend.gatt.resolve != NULL,
          "the ops table is missing an entry the backend requires");
    /* The context must be the adapter this call was given: a mismatch here would
     * make every control fail with no indication why. */
    CHECK(backend.gatt_ctx == &adapter, "gatt_ctx is not the adapter that was installed");
    /* now_ms belongs to the caller - the runtime's clock or a test's - so the adapter
     * must not install one of its own. */
    CHECK(backend.now_ms == NULL, "the adapter installed its own clock");

    /* is_ready answers for an uninitialised session rather than crashing. */
    CHECK(!backend.gatt.is_ready(backend.gatt_ctx), "an idle session reported ready");
    session.phase = APP_BLE_PHASE_READY;
    session.connected = true;
    CHECK(backend.gatt.is_ready(backend.gatt_ctx), "a ready session did not report ready");
    session.connected = false;
    CHECK(!backend.gatt.is_ready(backend.gatt_ctx), "a disconnected session reported ready");

    /* A NULL argument is never a resolve. */
    CHECK(!app_ctl_ble_gatt_resolve(NULL, NULL, NULL, NULL), "NULL arguments resolved");
}

int main(void)
{
    printf("ble control adapter tests\n");

    test_writable_properties();
    test_a_live_session_resolves();
    test_no_live_session_is_refused();
    test_a_different_peer_is_refused();
    test_an_unusable_index_is_refused();
    test_a_read_only_characteristic_is_refused();
    test_the_installed_table();

    printf("app_ctl_ble_gatt: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
