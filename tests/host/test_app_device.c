/*
 * Host regression tests for the application Device/Entity binding table.
 *
 * The same app_device.c source is compiled into the firmware together with the
 * real ha_core, so these tests pin the identity, dedup, generation,
 * availability and sweep rules that the product depends on.
 *
 * Deliberately asserted here:
 *   - the same bytes in two different protocols are NOT the same device;
 *   - an unknown device is kept and remains read-only;
 *   - no sensor or control capability is invented;
 *   - a device missing from a later generation is swept, not accumulated.
 */

#include <stdio.h>
#include <string.h>

#include "app_device.h"
#include "app_str.h"
#include "app_scan.h"
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

static void feed_wifi(app_scan_evidence_t *ev, const uint8_t bssid[6],
                      const char *ssid, int8_t rssi, uint8_t channel,
                      uint64_t seen_ms)
{
    app_scan_wifi_t obs;

    memset(&obs, 0, sizeof(obs));
    memcpy(obs.bssid, bssid, 6);
    obs.rssi_last = rssi;
    obs.channel = channel;
    obs.last_seen_ms = seen_ms;
    obs.first_seen_ms = seen_ms;
    obs.seen_count = 1u;
    if (ssid != NULL) {
        size_t len = strlen(ssid);
        if (len > APP_SCAN_MAX_SSID) {
            len = APP_SCAN_MAX_SSID;
        }
        obs.has_ssid = true;
        obs.ssid_len = (uint8_t)len;
        memcpy(obs.ssid, ssid, len);
    }
    (void)app_scan_ingest_wifi(ev, &obs);
}

static void feed_ble(app_scan_evidence_t *ev, const uint8_t addr[6],
                     uint8_t addr_type, const char *name, int8_t rssi,
                     bool with_tx_power, int8_t tx_power, uint64_t seen_ms)
{
    app_scan_ble_t obs;

    memset(&obs, 0, sizeof(obs));
    memcpy(obs.address, addr, 6);
    obs.address_type = addr_type;
    obs.rssi_last = rssi;
    obs.last_seen_ms = seen_ms;
    obs.first_seen_ms = seen_ms;
    obs.seen_count = 1u;
    obs.captured_len = 20u;
    if (name != NULL) {
        obs.has_parsed_adv = true;
        obs.adv.valid = true;
        obs.adv.name_present = true;
        (void)app_strlcpy(obs.adv.name, name, sizeof(obs.adv.name));
        if (with_tx_power) {
            obs.adv.tx_power_present = true;
            obs.adv.tx_power_dbm = tx_power;
        }
    }
    (void)app_scan_ingest_ble(ev, &obs);
}

static void feed_lan(app_scan_evidence_t *ev, const char *ip, const char *hostname,
                     uint64_t seen_ms)
{
    app_scan_lan_t obs;

    memset(&obs, 0, sizeof(obs));
    (void)app_strlcpy(obs.ipv4, ip, sizeof(obs.ipv4));
    (void)app_strlcpy(obs.hostname, hostname, sizeof(obs.hostname));
    obs.from_mdns = true;
    obs.up = true;
    obs.service_count = 1u;
    obs.last_seen_ms = seen_ms;
    obs.first_seen_ms = seen_ms;
    (void)app_scan_ingest_lan(ev, &obs);
}

static void test_unknown_wifi_device_kept_and_read_only(void)
{
    app_scan_evidence_t ev;
    const uint8_t bssid[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    const app_device_binding_t *binding;
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    feed_wifi(&ev, bssid, "LivingRoomAP", -55, 6u, 1000u);
    CHECK(app_device_materialize(&ev, &truncated) == 1u, "one device materialized");
    CHECK(!truncated, "nothing truncated");

    CHECK(app_device_count() == 1u, "one binding, got %u", (unsigned)app_device_count());
    binding = app_device_at(0u);
    CHECK(binding != NULL, "binding available");
    CHECK(binding->ephemeral, "RF observation is ephemeral");
    CHECK(binding->read_only, "unknown device is read-only");
    /* No Device DB is wired, so recognition must say so rather than pretending
     * a profile matched. */
    CHECK(binding->recognition == APP_RECOGNITION_DB_UNAVAILABLE,
          "recognition honestly reported as db_unavailable");
    CHECK(binding->sources == APP_SOURCE_WIFI, "wifi source recorded");
    CHECK(binding->has_signal && binding->signal_dbm == -55, "signal recorded");

    /* ha_core must carry the same device with the SSID as its name. */
    {
        const ha_device_t *device = ha_core_device_get(binding->ha_device_id);
        CHECK(device != NULL, "device present in ha_core");
        CHECK(strcmp(device->name, "LivingRoomAP") == 0, "ssid used as name, got %s",
              device ? device->name : "(null)");
    }
}

static void test_entities_are_only_exact_protocol_facts(void)
{
    app_scan_evidence_t ev;
    const uint8_t bssid[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const app_device_binding_t *binding;
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    feed_wifi(&ev, bssid, "AP", -60, 11u, 500u);
    (void)app_device_materialize(&ev, &truncated);
    binding = app_device_at(0u);
    CHECK(binding != NULL, "binding exists");

    /* Exactly the entities with a real protocol meaning: signal, last_seen,
     * channel. Nothing is invented beyond the observed values. */
    CHECK(app_entity_count() == 3u, "expected 3 entities, got %u",
          (unsigned)app_entity_count());

    {
        const char *expected[] = {
            "sensor.wifi_010203040506_signal_strength",
            "sensor.wifi_010203040506_last_seen",
            "sensor.wifi_010203040506_channel",
        };
        for (size_t i = 0u; i < sizeof(expected) / sizeof(expected[0]); ++i) {
            const app_entity_binding_t *e = app_entity_find(expected[i]);
            CHECK(e != NULL, "entity %s exists", expected[i]);
            if (e != NULL) {
                CHECK(!e->writable, "entity %s is read-only", expected[i]);
            }
            CHECK(ha_core_entity_get(expected[i]) != NULL,
                  "entity %s present in ha_core", expected[i]);
        }
    }

    /* Signal strength entity must carry the observed dBm value. */
    {
        const ha_state_t *state =
            ha_core_state_get("sensor.wifi_010203040506_signal_strength");
        CHECK(state != NULL, "signal state present");
        CHECK(state != NULL && strcmp(state->state, "-60") == 0,
              "signal state is the observed value, got %s",
              state ? state->state : "(null)");
    }
}

static void test_no_entity_when_no_meaningful_value(void)
{
    app_scan_evidence_t ev;
    const uint8_t addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    /* An advertisement we could not parse gives a device identity but no
     * protocol fact beyond RSSI. TX power must not be fabricated. */
    feed_ble(&ev, addr, 0u, NULL, -70, false, 0, 100u);
    (void)app_device_materialize(&ev, &truncated);

    CHECK(app_entity_find("sensor.ble_00112233445566_tx_power") == NULL,
          "tx_power must not exist when it was not advertised");
    CHECK(app_entity_find("sensor.ble_00112233445566_signal_strength") != NULL,
          "signal_strength does exist because RSSI was observed");
}

static void test_tx_power_entity_only_when_advertised(void)
{
    app_scan_evidence_t ev;
    const uint8_t addr[6] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    bool truncated = false;
    const char *id = "sensor.ble_000a0b0c0d0e0f_tx_power";

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    feed_ble(&ev, addr, 0u, "Beacon", -40, true, 4, 100u);
    (void)app_device_materialize(&ev, &truncated);

    CHECK(app_entity_find(id) != NULL, "tx_power entity exists when advertised");
    {
        const ha_state_t *state = ha_core_state_get(id);
        CHECK(state != NULL && strcmp(state->state, "4") == 0,
              "tx_power state is the advertised value");
    }
}

static void test_same_bytes_different_protocol_are_different_devices(void)
{
    app_scan_evidence_t ev;
    const uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    feed_wifi(&ev, mac, "SomeAP", -50, 1u, 100u);
    feed_ble(&ev, mac, 0u, "SomeBLE", -60, false, 0, 100u);
    (void)app_device_materialize(&ev, &truncated);

    /* Identical bytes in Wi-Fi and BLE must not be merged into one physical
     * device: there is no safe cross-protocol identity rule for that. */
    CHECK(app_device_count() == 2u,
          "same bytes in two protocols must stay separate, got %u",
          (unsigned)app_device_count());
    CHECK(app_device_find("wifi_deadbeef0001") != NULL, "wifi device exists");
    CHECK(app_device_find("ble_00deadbeef0001") != NULL, "ble device exists");
    CHECK(app_device_find("wifi_deadbeef0001") !=
              app_device_find("ble_00deadbeef0001"),
          "the two bindings are distinct");
}

static void test_ble_address_type_is_part_of_identity(void)
{
    app_scan_evidence_t ev;
    const uint8_t addr[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    feed_ble(&ev, addr, 0u, "Type0", -50, false, 0, 100u);
    feed_ble(&ev, addr, 1u, "Type1", -50, false, 0, 100u);
    (void)app_device_materialize(&ev, &truncated);

    CHECK(app_device_count() == 2u,
          "public and random address with same bytes are different devices, got %u",
          (unsigned)app_device_count());
    CHECK(app_device_find("ble_00010203040506") != NULL, "type 0 device");
    CHECK(app_device_find("ble_01010203040506") != NULL, "type 1 device");
}

static void test_repeat_materialize_does_not_grow(void)
{
    app_scan_evidence_t ev;
    const uint8_t bssid[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
    bool truncated = false;

    app_device_table_reset();

    /* Same generation, materialized repeatedly: no unbounded growth. */
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, bssid, "AP", -50, 1u, 100u);
    for (int i = 0; i < 5; ++i) {
        (void)app_device_materialize(&ev, &truncated);
        CHECK(app_device_count() == 1u, "repeat %d kept one device, got %u", i,
              (unsigned)app_device_count());
        CHECK(app_entity_count() == 3u, "repeat %d kept three entities, got %u", i,
              (unsigned)app_entity_count());
    }
}

/*
 * Build a scan report in which the given stages completed. The source-aware
 * sweep only treats absence as evidence for protocols whose stage actually ran,
 * so tests must state which stages observed the environment.
 */
static app_scan_status_t scan_report_with(app_scan_stage_t a, app_scan_stage_t b)
{
    app_scan_status_t scan;

    memset(&scan, 0, sizeof(scan));
    scan.generation = 1u;
    for (int i = 0; i < (int)APP_STAGE_COUNT; ++i) {
        scan.states[i] = APP_STAGE_STATE_SKIPPED;
    }
    scan.states[a] = APP_STAGE_STATE_DONE;
    if (b >= 0) {
        scan.states[b] = APP_STAGE_STATE_DONE;
    }
    return scan;
}

static void test_generation_sweeps_unseen_ephemeral(void)
{
    app_scan_evidence_t ev;
    const uint8_t a[6] = {0x10, 0, 0, 0, 0, 0x01};
    const uint8_t b[6] = {0x10, 0, 0, 0, 0, 0x02};
    bool truncated = false;
    /* The Wi-Fi stage really ran in both generations. */
    app_scan_status_t scan = scan_report_with(APP_STAGE_WIFI_RF,
                                              (app_scan_stage_t)-1);

    app_device_table_reset();

    /* Generation 1 sees A and B. */
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, a, "AP-A", -50, 1u, 100u);
    feed_wifi(&ev, b, "AP-B", -60, 6u, 100u);
    (void)app_device_materialize(&ev, &truncated);
    CHECK(app_device_count() == 2u, "two devices in generation 1");
    scan.generation = 1u;
    app_device_generation_finish(&scan);
    {
        const app_device_binding_t *a_binding = app_device_find("wifi_100000000001");
        CHECK(a_binding != NULL &&
                  a_binding->availability == APP_AVAILABILITY_ONLINE,
              "seen device is online");
    }

    /* Generation 2 sees only A: B must be swept, not accumulated. */
    app_scan_evidence_reset(&ev, 2u);
    app_device_generation_begin(2u);
    feed_wifi(&ev, a, "AP-A", -52, 1u, 200u);
    (void)app_device_materialize(&ev, &truncated);
    scan.generation = 2u;
    app_device_generation_finish(&scan);

    CHECK(app_device_count() == 1u,
          "stale ephemeral device must be swept, got %u", (unsigned)app_device_count());
    CHECK(app_device_find("wifi_100000000001") != NULL, "still-seen device kept");
    CHECK(app_device_find("wifi_100000000002") == NULL, "unseen device removed");
    /* ha_core must agree: no orphan Device or Entity left behind. */
    CHECK(ha_core_device_count() == 1u, "ha_core swept too, got %u",
          (unsigned)ha_core_device_count());
    CHECK(ha_core_entity_get("sensor.wifi_100000000002_signal_strength") == NULL,
          "no orphaned entity for the removed device");
}

/*
 * A protocol that never got to run must not have its devices declared gone.
 * This is the difference between "we looked and it was not there" and "we never
 * had a chance to look".
 */
static void test_unrun_protocol_does_not_sweep_its_devices(void)
{
    app_scan_evidence_t ev;
    const uint8_t wifi_mac[6] = {0x70, 0, 0, 0, 0, 0x01};
    const uint8_t ble_addr[6] = {0x70, 0, 0, 0, 0, 0x02};
    bool truncated = false;
    app_scan_status_t scan;

    app_device_table_reset();

    /* Generation 1: both Wi-Fi and BLE ran and found something. */
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, wifi_mac, "AP", -50, 1u, 100u);
    feed_ble(&ev, ble_addr, 0u, "Sensor", -60, false, 0, 100u);
    (void)app_device_materialize(&ev, &truncated);
    CHECK(app_device_count() == 2u, "wifi and ble devices materialized");
    scan = scan_report_with(APP_STAGE_WIFI_RF, APP_STAGE_BLE_RF);
    scan.generation = 1u;
    app_device_generation_finish(&scan);
    CHECK(app_device_count() == 2u, "both kept after a successful generation");

    /* Generation 2: only BLE ran (for example the Wi-Fi handover failed). The
     * Wi-Fi device must survive as stale, not be deleted. */
    app_scan_evidence_reset(&ev, 2u);
    app_device_generation_begin(2u);
    feed_ble(&ev, ble_addr, 0u, "Sensor", -62, false, 0, 200u);
    (void)app_device_materialize(&ev, &truncated);
    scan = scan_report_with(APP_STAGE_BLE_RF, (app_scan_stage_t)-1);
    scan.generation = 2u;
    app_device_generation_finish(&scan);

    CHECK(app_device_count() == 2u,
          "an unrun protocol must not remove its devices, got %u",
          (unsigned)app_device_count());
    {
        const app_device_binding_t *wifi_dev = app_device_find("wifi_700000000001");
        CHECK(wifi_dev != NULL, "wifi device still present");
        CHECK(wifi_dev != NULL && wifi_dev->availability == APP_AVAILABILITY_STALE,
              "wifi device marked stale because Wi-Fi never ran");
        CHECK(ha_core_device_get("wifi_700000000001") != NULL,
              "ha_core still holds the wifi device");
    }
    {
        const app_device_binding_t *ble_dev = app_device_find("ble_007000000002");
        CHECK(ble_dev != NULL &&
                  ble_dev->availability == APP_AVAILABILITY_ONLINE,
              "ble device refreshed and online");
    }

    /* Generation 3: BLE ran and no longer sees the sensor, so it is genuinely
     * gone and must be swept even though Wi-Fi still has not run. */
    app_scan_evidence_reset(&ev, 3u);
    app_device_generation_begin(3u);
    (void)app_device_materialize(&ev, &truncated);
    scan = scan_report_with(APP_STAGE_BLE_RF, (app_scan_stage_t)-1);
    scan.generation = 3u;
    app_device_generation_finish(&scan);

    CHECK(app_device_find("ble_007000000002") == NULL,
          "device from a protocol that ran and missed it is swept");
    CHECK(app_device_find("wifi_700000000001") != NULL,
          "device from the unrun protocol is still kept");
}

/* A canceled or failed stage is not evidence of absence either. */
static void test_canceled_and_failed_stages_do_not_sweep(void)
{
    app_scan_evidence_t ev;
    const uint8_t mac[6] = {0x71, 0, 0, 0, 0, 0x01};
    bool truncated = false;
    app_scan_status_t scan;

    app_device_table_reset();

    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, mac, "AP", -50, 1u, 100u);
    (void)app_device_materialize(&ev, &truncated);
    scan = scan_report_with(APP_STAGE_WIFI_RF, (app_scan_stage_t)-1);
    scan.generation = 1u;
    app_device_generation_finish(&scan);
    CHECK(app_device_count() == 1u, "device present");

    /* Generation 2: the Wi-Fi stage was canceled. */
    app_scan_evidence_reset(&ev, 2u);
    app_device_generation_begin(2u);
    (void)app_device_materialize(&ev, &truncated);
    scan = scan_report_with((app_scan_stage_t)-1, (app_scan_stage_t)-1);
    scan.states[APP_STAGE_WIFI_RF] = APP_STAGE_STATE_CANCELED;
    scan.generation = 2u;
    app_device_generation_finish(&scan);
    CHECK(app_device_count() == 1u, "canceled stage must not sweep");

    /* Generation 3: the Wi-Fi stage failed. */
    app_scan_evidence_reset(&ev, 3u);
    app_device_generation_begin(3u);
    (void)app_device_materialize(&ev, &truncated);
    scan.states[APP_STAGE_WIFI_RF] = APP_STAGE_STATE_FAILED;
    scan.generation = 3u;
    app_device_generation_finish(&scan);
    CHECK(app_device_count() == 1u, "failed stage must not sweep");

    /* A NULL report is the conservative case: nothing is swept. */
    app_scan_evidence_reset(&ev, 4u);
    app_device_generation_begin(4u);
    (void)app_device_materialize(&ev, &truncated);
    app_device_generation_finish(NULL);
    CHECK(app_device_count() == 1u, "null report must not sweep");
    CHECK(app_device_find("wifi_710000000001")->availability ==
              APP_AVAILABILITY_STALE,
          "kept device is marked stale");
}

static void test_stale_devices_do_not_accumulate_over_many_generations(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    app_scan_status_t scan;

    app_device_table_reset();

    /* Ten generations, each seeing one different AP and each running the Wi-Fi
     * stage. Without sweeping this would grow without bound; with sweeping it
     * stays at one. */
    for (uint32_t gen = 1u; gen <= 10u; ++gen) {
        uint8_t bssid[6] = {0x20, 0, 0, 0, 0, (uint8_t)gen};

        app_scan_evidence_reset(&ev, gen);
        app_device_generation_begin(gen);
        feed_wifi(&ev, bssid, "AP", -50, (uint8_t)gen, (uint64_t)gen * 100u);
        (void)app_device_materialize(&ev, &truncated);
        scan = scan_report_with(APP_STAGE_WIFI_RF, (app_scan_stage_t)-1);
        scan.generation = gen;
        app_device_generation_finish(&scan);
        CHECK(app_device_count() == 1u, "generation %u kept one device, got %u",
              (unsigned)gen, (unsigned)app_device_count());
    }
}

static void test_lan_device_uses_hostname_and_ip(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    const app_device_binding_t *binding;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    feed_lan(&ev, "192.168.1.50", "printer.local", 300u);
    (void)app_device_materialize(&ev, &truncated);

    CHECK(app_device_count() == 1u, "one LAN device");
    binding = app_device_find("lan_192_168_1_50");
    CHECK(binding != NULL, "LAN device id derived from the IP");
    CHECK(binding->sources == APP_SOURCE_LAN, "lan source recorded");
    if (binding != NULL) {
        const ha_device_t *device = ha_core_device_get(binding->ha_device_id);
        CHECK(device != NULL && strcmp(device->name, "printer.local") == 0,
              "hostname preferred as the device name");
    }
}

static void test_capacity_overflow_is_reported(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    /* More distinct devices than the binding table can hold. Overflow must be
     * reported rather than silently discarded. */
    for (uint32_t i = 0u; i < APP_DEVICE_MAX + 4u; ++i) {
        uint8_t bssid[6] = {0x30, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i};
        feed_wifi(&ev, bssid, "AP", -50, 1u, (uint64_t)(i + 1u) * 10u);
    }
    (void)app_device_materialize(&ev, &truncated);

    CHECK(app_device_count() <= APP_DEVICE_MAX, "binding table stayed bounded");
    CHECK(truncated, "capacity overflow reported to the caller");
}

static void test_entity_capacity_is_bounded(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    size_t device_count;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    /* More devices than ha_core itself can hold. Their Entities must not grow
     * without bound, and the overflow must be reported. */
    for (uint32_t i = 0u; i < HA_CORE_MAX_DEVICES + 2u; ++i) {
        uint8_t bssid[6] = {0x40, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i};
        feed_wifi(&ev, bssid, "AP", -50, 1u, (uint64_t)(i + 1u) * 10u);
    }
    (void)app_device_materialize(&ev, &truncated);

    CHECK(app_entity_count() <= APP_ENTITY_MAX, "app entity table stayed bounded");
    CHECK(truncated, "entity/device capacity overflow reported");

    /* Every Entity that does exist must still belong to a real ha_core Device:
     * no orphan may be left behind by a failed insert. */
    device_count = app_device_count();
    for (size_t i = 0u; i < device_count; ++i) {
        const app_device_binding_t *b = app_device_at(i);
        CHECK(b != NULL && ha_core_device_get(b->ha_device_id) != NULL,
              "binding %u has a matching ha_core device", (unsigned)i);
    }
    for (size_t i = 0u; i < app_entity_count(); ++i) {
        const app_entity_binding_t *e = app_entity_at(i);
        CHECK(e != NULL && app_device_find(e->device_id) != NULL,
              "entity %u belongs to a known device", (unsigned)i);
    }
}

static void test_hostile_ssid_is_not_injected(void)
{
    app_scan_evidence_t ev;
    const uint8_t bssid[6] = {0x50, 0, 0, 0, 0, 1};
    bool truncated = false;
    const app_device_binding_t *binding;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);

    /* Non-printable bytes must not reach the device name. */
    {
        app_scan_wifi_t obs;
        memset(&obs, 0, sizeof(obs));
        memcpy(obs.bssid, bssid, 6);
        obs.rssi_last = -50;
        obs.channel = 1u;
        obs.last_seen_ms = 10u;
        obs.first_seen_ms = 10u;
        obs.seen_count = 1u;
        obs.has_ssid = true;
        obs.ssid_len = 5u;
        obs.ssid[0] = 'A';
        obs.ssid[1] = 0x1Bu; /* ESC */
        obs.ssid[2] = '[';
        obs.ssid[3] = '2';
        obs.ssid[4] = 'J';
        (void)app_scan_ingest_wifi(&ev, &obs);
    }
    (void)app_device_materialize(&ev, &truncated);

    binding = app_device_at(0u);
    CHECK(binding != NULL, "device created");
    if (binding != NULL) {
        const ha_device_t *device = ha_core_device_get(binding->ha_device_id);
        CHECK(device != NULL, "device in ha_core");
        CHECK(device != NULL && strchr(device->name, 0x1B) == NULL,
              "control characters must not reach the device name");
        CHECK(device != NULL && strcmp(device->name, "Unknown Wi-Fi Device") == 0,
              "non-printable SSID falls back to the generic label, got %s",
              device ? device->name : "(null)");
    }
}

static void test_control_is_not_wired(void)
{
    /* Nothing in this layer may expose a writable entity: no protocol controller
     * binding exists yet, so a control request must not be able to find a
     * target here. */
    app_scan_evidence_t ev;
    const uint8_t bssid[6] = {0x60, 0, 0, 0, 0, 1};
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, bssid, "AP", -50, 1u, 10u);
    (void)app_device_materialize(&ev, &truncated);

    for (size_t i = 0u; i < app_entity_count(); ++i) {
        const app_entity_binding_t *e = app_entity_at(i);
        CHECK(e != NULL && !e->writable, "every entity is read-only");
        if (e != NULL) {
            const ha_entity_t *entity = ha_core_entity_get(e->entity_id);
            CHECK(entity != NULL && entity->supported_services == 0u,
                  "entity %s advertises no service", e->entity_id);
            CHECK(entity != NULL && entity->service_handler == NULL,
                  "entity %s has no service handler", e->entity_id);
            CHECK(entity != NULL && !ha_core_service_supports(e->entity_id, HA_SERVICE_TURN_ON),
                  "turn_on is not supported by %s", e->entity_id);
        }
    }
}

int main(void)
{
    test_unknown_wifi_device_kept_and_read_only();
    test_entities_are_only_exact_protocol_facts();
    test_no_entity_when_no_meaningful_value();
    test_tx_power_entity_only_when_advertised();
    test_same_bytes_different_protocol_are_different_devices();
    test_ble_address_type_is_part_of_identity();
    test_repeat_materialize_does_not_grow();
    test_generation_sweeps_unseen_ephemeral();
    test_unrun_protocol_does_not_sweep_its_devices();
    test_canceled_and_failed_stages_do_not_sweep();
    test_stale_devices_do_not_accumulate_over_many_generations();
    test_lan_device_uses_hostname_and_ip();
    test_capacity_overflow_is_reported();
    test_entity_capacity_is_bounded();
    test_hostile_ssid_is_not_injected();
    test_control_is_not_wired();

    printf("app_device: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
