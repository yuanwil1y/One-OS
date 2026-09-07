#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kismet_ble.h"
#include "kismet_internal.h"
#include "kismet_wifi.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return false; \
    } \
} while (0)

static void make_mac(uint8_t mac[6], uint8_t id)
{
    memset(mac, 0, 6);
    mac[0] = 0x02u;
    mac[5] = id;
}

static kismet_wifi_observation_t wifi_obs(uint8_t id,
                                          uint64_t seen_ms,
                                          kismet_wifi_role_t role,
                                          int8_t rssi,
                                          uint8_t channel,
                                          kismet_wifi_frame_class_t frame_class)
{
    kismet_wifi_observation_t obs = {0};
    make_mac(obs.device, id);
    obs.seen_ms = seen_ms;
    obs.role = role;
    obs.rssi = rssi;
    obs.channel = channel;
    obs.frame_class = frame_class;
    return obs;
}

static bool wifi_find_device(const kismet_wifi_tracker_t *tracker,
                             const uint8_t mac[6],
                             kismet_wifi_device_t *out)
{
    const size_t count = kismet_wifi_tracker_device_count(tracker);
    for (size_t i = 0; i < count; ++i) {
        kismet_wifi_device_t current = {0};
        if (kismet_wifi_tracker_get_device(tracker, i, &current) == ESP_OK &&
            memcmp(current.mac, mac, 6) == 0) {
            if (out != NULL) {
                *out = current;
            }
            return true;
        }
    }
    return false;
}

static bool wifi_find_ssid(const kismet_wifi_tracker_t *tracker,
                           const uint8_t *ssid,
                           uint8_t ssid_len,
                           kismet_wifi_ssid_t *out)
{
    const size_t count = kismet_wifi_tracker_ssid_count(tracker);
    for (size_t i = 0; i < count; ++i) {
        kismet_wifi_ssid_t current = {0};
        if (kismet_wifi_tracker_get_ssid(tracker, i, &current) != ESP_OK) {
            continue;
        }
        if (current.ssid_len == ssid_len &&
            (ssid_len == 0u || memcmp(current.ssid, ssid, ssid_len) == 0)) {
            if (out != NULL) {
                *out = current;
            }
            return true;
        }
    }
    return false;
}

static bool ble_find_device(const kismet_ble_tracker_t *tracker,
                            const uint8_t address[6],
                            uint8_t address_type,
                            kismet_ble_device_t *out)
{
    const size_t count = kismet_ble_tracker_count(tracker);
    for (size_t i = 0; i < count; ++i) {
        kismet_ble_device_t current = {0};
        if (kismet_ble_tracker_get(tracker, i, &current) == ESP_OK &&
            current.address_type == address_type &&
            memcmp(current.address, address, 6) == 0) {
            if (out != NULL) {
                *out = current;
            }
            return true;
        }
    }
    return false;
}

static bool test_wifi_dedup_ssid_relation(void)
{
    const kismet_wifi_tracker_config_t cfg = {
        .max_devices = 8,
        .max_ssids = 4,
        .max_relations = 4,
        .max_ssid_links = 8,
        .device_idle_ms = 1000,
        .ssid_idle_ms = 1000,
        .relation_idle_ms = 1000,
    };
    kismet_wifi_tracker_t *tracker = NULL;
    CHECK(kismet_wifi_tracker_create(&cfg, &tracker) == ESP_OK);

    kismet_wifi_observation_t obs =
        wifi_obs(1, 100, KISMET_WIFI_ROLE_AP, -70, 1,
                 KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    obs.seen_ms = 110;
    obs.rssi = -50;
    obs.channel = 6;
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    CHECK(kismet_wifi_tracker_device_count(tracker) == 1u);

    uint8_t ap1[6];
    make_mac(ap1, 1);
    kismet_wifi_device_t device = {0};
    CHECK(wifi_find_device(tracker, ap1, &device));
    CHECK(device.first_seen_ms == 100u);
    CHECK(device.last_seen_ms == 110u);
    CHECK(device.seen_count == 2u);
    CHECK(device.management_count == 2u);
    CHECK(device.rssi_min == -70);
    CHECK(device.rssi_max == -50);
    CHECK(device.rssi_last == -50);
    CHECK(device.last_channel == 6u);
    CHECK((device.channel_bitmap & (1u << 0)) != 0u);
    CHECK((device.channel_bitmap & (1u << 5)) != 0u);

    static const uint8_t home[] = {'H', 'o', 'm', 'e'};
    obs = wifi_obs(1, 120, KISMET_WIFI_ROLE_AP, -48, 6,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    obs.has_ssid = true;
    obs.ssid_kind = KISMET_WIFI_SSID_ADVERTISED;
    memcpy(obs.ssid, home, sizeof(home));
    obs.ssid_len = sizeof(home);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    obs = wifi_obs(2, 130, KISMET_WIFI_ROLE_AP, -60, 11,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    obs.has_ssid = true;
    obs.ssid_kind = KISMET_WIFI_SSID_ADVERTISED;
    memcpy(obs.ssid, home, sizeof(home));
    obs.ssid_len = sizeof(home);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    obs = wifi_obs(3, 140, KISMET_WIFI_ROLE_STA, -65, 1,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    obs.has_ssid = true;
    obs.ssid_kind = KISMET_WIFI_SSID_PROBED;
    memcpy(obs.ssid, home, sizeof(home));
    obs.ssid_len = sizeof(home);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    CHECK(kismet_wifi_tracker_ssid_count(tracker) == 1u);
    kismet_wifi_ssid_t ssid = {0};
    CHECK(wifi_find_ssid(tracker, home, sizeof(home), &ssid));
    CHECK(ssid.seen_count == 3u);
    CHECK(ssid.advertiser_count == 2u);
    CHECK(ssid.prober_count == 1u);

    obs = wifi_obs(3, 150, KISMET_WIFI_ROLE_STA, -63, 1,
                   KISMET_WIFI_FRAME_DATA);
    obs.has_relation = true;
    make_mac(obs.relation_client, 3);
    make_mac(obs.relation_bssid, 1);
    obs.relation_evidence_flags = KISMET_WIFI_RELATION_EVIDENCE_TO_DS;
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    obs.seen_ms = 160;
    obs.relation_evidence_flags = KISMET_WIFI_RELATION_EVIDENCE_ASSOC_REQUEST;
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    CHECK(kismet_wifi_tracker_relation_count(tracker) == 1u);
    kismet_wifi_relation_t relation = {0};
    CHECK(kismet_wifi_tracker_get_relation(tracker, 0, &relation) == ESP_OK);
    CHECK(relation.first_seen_ms == 150u);
    CHECK(relation.last_seen_ms == 160u);
    CHECK(relation.seen_count == 2u);
    CHECK((relation.evidence_flags & KISMET_WIFI_RELATION_EVIDENCE_TO_DS) != 0u);
    CHECK((relation.evidence_flags & KISMET_WIFI_RELATION_EVIDENCE_ASSOC_REQUEST) != 0u);

    kismet_wifi_tracker_note_capture(tracker, 2, 3, 4);
    kismet_wifi_tracker_stats_t stats = {0};
    CHECK(kismet_wifi_tracker_get_stats(tracker, &stats) == ESP_OK);
    CHECK(stats.rx_queue_drops == 2u);
    CHECK(stats.malformed_frames == 3u);
    CHECK(stats.truncated_frames == 4u);
    CHECK((stats.partial_flags & KISMET_WIFI_PARTIAL_RX_DROP) != 0u);
    CHECK((stats.partial_flags & KISMET_WIFI_PARTIAL_MALFORMED_FRAME) != 0u);
    CHECK((stats.partial_flags & KISMET_WIFI_PARTIAL_TRUNCATED_FRAME) != 0u);

    kismet_wifi_tracker_reset(tracker);
    CHECK(kismet_wifi_tracker_device_count(tracker) == 0u);
    CHECK(kismet_wifi_tracker_ssid_count(tracker) == 0u);
    CHECK(kismet_wifi_tracker_relation_count(tracker) == 0u);
    CHECK(kismet_wifi_tracker_get_stats(tracker, &stats) == ESP_OK);
    CHECK(stats.partial_flags == 0u);

    kismet_wifi_tracker_destroy(tracker);
    return true;
}

static bool test_wifi_capacity_and_expiry(void)
{
    kismet_wifi_tracker_config_t cfg = {
        .max_devices = 2,
        .max_ssids = 1,
        .max_relations = 1,
        .max_ssid_links = 1,
        .device_idle_ms = 1000,
        .ssid_idle_ms = 1000,
        .relation_idle_ms = 1000,
    };
    kismet_wifi_tracker_t *tracker = NULL;
    CHECK(kismet_wifi_tracker_create(&cfg, &tracker) == ESP_OK);

    kismet_wifi_observation_t obs =
        wifi_obs(1, 100, KISMET_WIFI_ROLE_AP, -60, 1,
                 KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    obs = wifi_obs(2, 110, KISMET_WIFI_ROLE_AP, -61, 6,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    obs = wifi_obs(3, 120, KISMET_WIFI_ROLE_AP, -62, 11,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    CHECK(kismet_wifi_tracker_device_count(tracker) == 2u);

    uint8_t mac1[6];
    make_mac(mac1, 1);
    CHECK(!wifi_find_device(tracker, mac1, NULL));
    kismet_wifi_tracker_stats_t stats = {0};
    CHECK(kismet_wifi_tracker_get_stats(tracker, &stats) == ESP_OK);
    CHECK(stats.device_evictions == 1u);
    CHECK((stats.partial_flags & KISMET_WIFI_PARTIAL_DEVICE_EVICTION) != 0u);
    kismet_wifi_tracker_destroy(tracker);

    cfg.max_devices = 4;
    cfg.max_ssids = 1;
    cfg.max_relations = 1;
    cfg.max_ssid_links = 1;
    CHECK(kismet_wifi_tracker_create(&cfg, &tracker) == ESP_OK);
    static const uint8_t home[] = {'H', 'o', 'm', 'e'};
    static const uint8_t work[] = {'W', 'o', 'r', 'k'};

    obs = wifi_obs(1, 100, KISMET_WIFI_ROLE_AP, -50, 1,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    obs.has_ssid = true;
    obs.ssid_kind = KISMET_WIFI_SSID_ADVERTISED;
    memcpy(obs.ssid, home, sizeof(home));
    obs.ssid_len = sizeof(home);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    obs = wifi_obs(2, 110, KISMET_WIFI_ROLE_AP, -55, 6,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    obs.has_ssid = true;
    obs.ssid_kind = KISMET_WIFI_SSID_ADVERTISED;
    memcpy(obs.ssid, home, sizeof(home));
    obs.ssid_len = sizeof(home);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    obs = wifi_obs(2, 120, KISMET_WIFI_ROLE_AP, -54, 6,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    obs.has_ssid = true;
    obs.ssid_kind = KISMET_WIFI_SSID_ADVERTISED;
    memcpy(obs.ssid, work, sizeof(work));
    obs.ssid_len = sizeof(work);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    obs = wifi_obs(3, 130, KISMET_WIFI_ROLE_STA, -65, 1,
                   KISMET_WIFI_FRAME_DATA);
    obs.has_relation = true;
    make_mac(obs.relation_client, 3);
    make_mac(obs.relation_bssid, 1);
    obs.relation_evidence_flags = KISMET_WIFI_RELATION_EVIDENCE_TO_DS;
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    obs = wifi_obs(4, 140, KISMET_WIFI_ROLE_STA, -66, 1,
                   KISMET_WIFI_FRAME_DATA);
    obs.has_relation = true;
    make_mac(obs.relation_client, 4);
    make_mac(obs.relation_bssid, 1);
    obs.relation_evidence_flags = KISMET_WIFI_RELATION_EVIDENCE_TO_DS;
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);

    CHECK(kismet_wifi_tracker_get_stats(tracker, &stats) == ESP_OK);
    CHECK(stats.ssid_link_evictions == 1u);
    CHECK(stats.ssid_evictions == 1u);
    CHECK(stats.relation_evictions == 1u);
    CHECK((stats.partial_flags & KISMET_WIFI_PARTIAL_LINK_EVICTION) != 0u);
    CHECK((stats.partial_flags & KISMET_WIFI_PARTIAL_SSID_EVICTION) != 0u);
    CHECK((stats.partial_flags & KISMET_WIFI_PARTIAL_RELATION_EVICTION) != 0u);
    kismet_wifi_tracker_destroy(tracker);

    cfg.max_devices = 2;
    cfg.max_ssids = 1;
    cfg.max_relations = 1;
    cfg.max_ssid_links = 1;
    cfg.device_idle_ms = 10;
    cfg.ssid_idle_ms = 10;
    cfg.relation_idle_ms = 10;
    CHECK(kismet_wifi_tracker_create(&cfg, &tracker) == ESP_OK);
    obs = wifi_obs(1, 100, KISMET_WIFI_ROLE_AP, -60, 1,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    kismet_wifi_tracker_expire(tracker, 109);
    CHECK(kismet_wifi_tracker_device_count(tracker) == 1u);
    kismet_wifi_tracker_expire(tracker, 110);
    CHECK(kismet_wifi_tracker_device_count(tracker) == 0u);

    kismet_wifi_tracker_reset(tracker);
    obs = wifi_obs(1, 100, KISMET_WIFI_ROLE_AP, -60, 1,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    obs = wifi_obs(2, 101, KISMET_WIFI_ROLE_AP, -61, 6,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    obs = wifi_obs(3, 111, KISMET_WIFI_ROLE_AP, -62, 11,
                   KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(kismet_wifi_tracker_ingest(tracker, &obs) == ESP_OK);
    CHECK(kismet_wifi_tracker_device_count(tracker) == 1u);
    CHECK(kismet_wifi_tracker_get_stats(tracker, &stats) == ESP_OK);
    CHECK(stats.device_evictions == 0u);
    kismet_wifi_tracker_destroy(tracker);
    return true;
}

static void set_fc(uint8_t *frame, uint16_t fc)
{
    frame[0] = (uint8_t)(fc & 0xffu);
    frame[1] = (uint8_t)(fc >> 8u);
}

static void set_addr(uint8_t *frame, size_t offset, uint8_t id)
{
    uint8_t mac[6];
    make_mac(mac, id);
    memcpy(frame + offset, mac, 6);
}

static bool test_wifi_shallow_decode(void)
{
    uint8_t frame[30] = {0};
    kismet_wifi_observation_t primary = {0};
    kismet_wifi_observation_t secondary = {0};

    CHECK(kismet_wifi_shallow_observe(frame, 1, -50, 1, 10,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_MALFORMED);

    memset(frame, 0, sizeof(frame));
    set_fc(frame, 0x0004u);
    CHECK(kismet_wifi_shallow_observe(frame, 2, -50, 1, 10,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_IGNORED);

    memset(frame, 0, sizeof(frame));
    set_fc(frame, 0x0080u);
    set_addr(frame, 10, 1);
    CHECK(kismet_wifi_shallow_observe(frame, 24, -42, 6, 100,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_OK);
    CHECK(primary.role == KISMET_WIFI_ROLE_AP);
    CHECK(primary.frame_class == KISMET_WIFI_FRAME_MANAGEMENT);
    CHECK(primary.rssi == -42);
    CHECK(primary.channel == 6u);
    CHECK(!primary.has_ssid);

    memset(frame, 0, sizeof(frame));
    set_fc(frame, 0x0040u);
    set_addr(frame, 10, 2);
    CHECK(kismet_wifi_shallow_observe(frame, 24, -50, 1, 110,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_OK);
    CHECK(primary.role == KISMET_WIFI_ROLE_STA);

    memset(frame, 0, sizeof(frame));
    set_fc(frame, 0x0000u);
    set_addr(frame, 4, 1);
    set_addr(frame, 10, 2);
    CHECK(kismet_wifi_shallow_observe(frame, 24, -51, 1, 120,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_OK);
    CHECK(primary.role == KISMET_WIFI_ROLE_STA);
    CHECK(secondary.role == KISMET_WIFI_ROLE_AP);
    CHECK(primary.has_relation);
    CHECK(primary.relation_evidence_flags ==
          KISMET_WIFI_RELATION_EVIDENCE_ASSOC_REQUEST);

    memset(frame, 0, sizeof(frame));
    set_fc(frame, 0x0108u);
    set_addr(frame, 4, 1);
    set_addr(frame, 10, 2);
    CHECK(kismet_wifi_shallow_observe(frame, 24, -52, 6, 130,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_OK);
    CHECK(primary.role == KISMET_WIFI_ROLE_STA);
    CHECK(secondary.role == KISMET_WIFI_ROLE_AP);
    CHECK(primary.has_relation);
    CHECK(primary.relation_evidence_flags ==
          KISMET_WIFI_RELATION_EVIDENCE_TO_DS);

    memset(frame, 0, sizeof(frame));
    set_fc(frame, 0x0208u);
    set_addr(frame, 4, 2);
    set_addr(frame, 10, 1);
    CHECK(kismet_wifi_shallow_observe(frame, 24, -53, 11, 140,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_OK);
    CHECK(primary.role == KISMET_WIFI_ROLE_AP);
    CHECK(secondary.role == KISMET_WIFI_ROLE_STA);
    CHECK(primary.has_relation);
    CHECK(primary.relation_evidence_flags ==
          KISMET_WIFI_RELATION_EVIDENCE_FROM_DS);

    memset(frame, 0, sizeof(frame));
    set_fc(frame, 0x0308u);
    set_addr(frame, 10, 4);
    CHECK(kismet_wifi_shallow_observe(frame, 24, -54, 1, 150,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_MALFORMED);
    CHECK(kismet_wifi_shallow_observe(frame, 30, -54, 1, 150,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_OK);
    CHECK(primary.role == KISMET_WIFI_ROLE_PEER);
    CHECK(!primary.has_relation);

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x01u;
    CHECK(kismet_wifi_shallow_observe(frame, 24, -50, 1, 160,
                                      &primary, &secondary) ==
          KISMET_WIFI_SHALLOW_MALFORMED);
    return true;
}

static bool test_ble_tracker(void)
{
    const kismet_ble_tracker_config_t cfg = {
        .max_devices = 2,
        .device_idle_ms = 1000,
    };
    kismet_ble_tracker_t *tracker = NULL;
    CHECK(kismet_ble_tracker_create(&cfg, &tracker) == ESP_OK);

    kismet_ble_observation_t obs = {0};
    make_mac(obs.address, 1);
    obs.address_type = 0;
    obs.seen_ms = 100;
    obs.rssi = -70;
    obs.connectable = false;
    CHECK(kismet_ble_tracker_ingest(tracker, &obs) == ESP_OK);
    obs.seen_ms = 105;
    obs.rssi = -40;
    obs.connectable = true;
    CHECK(kismet_ble_tracker_ingest(tracker, &obs) == ESP_OK);
    CHECK(kismet_ble_tracker_count(tracker) == 1u);

    uint8_t address[6];
    make_mac(address, 1);
    kismet_ble_device_t device = {0};
    CHECK(ble_find_device(tracker, address, 0, &device));
    CHECK(device.first_seen_ms == 100u);
    CHECK(device.last_seen_ms == 105u);
    CHECK(device.seen_count == 2u);
    CHECK(device.rssi_min == -70);
    CHECK(device.rssi_max == -40);
    CHECK(device.rssi_last == -40);
    CHECK(device.connectable);

    obs.address_type = 1;
    obs.seen_ms = 106;
    CHECK(kismet_ble_tracker_ingest(tracker, &obs) == ESP_OK);
    CHECK(kismet_ble_tracker_count(tracker) == 2u);

    make_mac(obs.address, 2);
    obs.address_type = 0;
    obs.seen_ms = 120;
    obs.rssi = -60;
    CHECK(kismet_ble_tracker_ingest(tracker, &obs) == ESP_OK);
    CHECK(kismet_ble_tracker_count(tracker) == 2u);
    CHECK(!ble_find_device(tracker, address, 0, NULL));

    kismet_ble_tracker_stats_t stats = {0};
    CHECK(kismet_ble_tracker_get_stats(tracker, &stats) == ESP_OK);
    CHECK(stats.device_evictions == 1u);
    CHECK((stats.partial_flags & KISMET_BLE_PARTIAL_DEVICE_EVICTION) != 0u);

    kismet_ble_tracker_note_capture(tracker, 2, 3);
    CHECK(kismet_ble_tracker_get_stats(tracker, &stats) == ESP_OK);
    CHECK(stats.report_queue_drops == 2u);
    CHECK(stats.truncated_reports == 3u);
    CHECK((stats.partial_flags & KISMET_BLE_PARTIAL_REPORT_DROP) != 0u);
    CHECK((stats.partial_flags & KISMET_BLE_PARTIAL_TRUNCATED_REPORT) != 0u);
    kismet_ble_tracker_destroy(tracker);

    const kismet_ble_tracker_config_t expire_cfg = {
        .max_devices = 2,
        .device_idle_ms = 10,
    };
    CHECK(kismet_ble_tracker_create(&expire_cfg, &tracker) == ESP_OK);
    memset(&obs, 0, sizeof(obs));
    make_mac(obs.address, 1);
    obs.seen_ms = 100;
    obs.rssi = -70;
    CHECK(kismet_ble_tracker_ingest(tracker, &obs) == ESP_OK);
    kismet_ble_tracker_expire(tracker, 109);
    CHECK(kismet_ble_tracker_count(tracker) == 1u);
    kismet_ble_tracker_expire(tracker, 110);
    CHECK(kismet_ble_tracker_count(tracker) == 0u);

    memset(&obs, 0, sizeof(obs));
    CHECK(kismet_ble_tracker_ingest(tracker, &obs) == ESP_OK);
    CHECK(kismet_ble_tracker_count(tracker) == 0u);
    kismet_ble_tracker_destroy(tracker);
    return true;
}

static bool test_lifecycle_100x(void)
{
    for (unsigned i = 0; i < 100u; ++i) {
        kismet_wifi_tracker_t *wifi = NULL;
        kismet_ble_tracker_t *ble = NULL;
        CHECK(kismet_wifi_tracker_create(NULL, &wifi) == ESP_OK);
        CHECK(kismet_ble_tracker_create(NULL, &ble) == ESP_OK);

        kismet_wifi_observation_t wobs =
            wifi_obs((uint8_t)((i % 250u) + 1u), i,
                     KISMET_WIFI_ROLE_UNKNOWN, -70, 1,
                     KISMET_WIFI_FRAME_OTHER);
        CHECK(kismet_wifi_tracker_ingest(wifi, &wobs) == ESP_OK);

        kismet_ble_observation_t bobs = {0};
        make_mac(bobs.address, (uint8_t)((i % 250u) + 1u));
        bobs.seen_ms = i;
        bobs.rssi = -70;
        CHECK(kismet_ble_tracker_ingest(ble, &bobs) == ESP_OK);

        kismet_wifi_tracker_reset(wifi);
        kismet_ble_tracker_reset(ble);
        CHECK(kismet_wifi_tracker_device_count(wifi) == 0u);
        CHECK(kismet_ble_tracker_count(ble) == 0u);
        kismet_wifi_tracker_destroy(wifi);
        kismet_ble_tracker_destroy(ble);
    }
    return true;
}

int main(void)
{
    struct {
        const char *name;
        bool (*fn)(void);
    } tests[] = {
        {"wifi dedup/ssid/relation", test_wifi_dedup_ssid_relation},
        {"wifi capacity/expiry", test_wifi_capacity_and_expiry},
        {"wifi shallow decode", test_wifi_shallow_decode},
        {"ble tracker", test_ble_tracker},
        {"100x lifecycle", test_lifecycle_100x},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (!tests[i].fn()) {
            fprintf(stderr, "kismet_l2 host test failed: %s\n", tests[i].name);
            return 1;
        }
        printf("PASS: %s\n", tests[i].name);
    }

    puts("kismet_l2 host tests: PASS");
    return 0;
}
