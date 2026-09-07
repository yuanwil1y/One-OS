#include "kismet_ble.h"
#include "kismet_internal.h"

#include <stdlib.h>
#include <string.h>

#define KISMET_BLE_ADDR_LEN 6u

typedef struct {
    bool used;
    kismet_ble_device_t value;
} ble_device_slot_t;

struct kismet_ble_tracker {
    kismet_ble_tracker_config_t config;
    ble_device_slot_t *devices;
    kismet_ble_tracker_stats_t stats;
};

static bool address_equal(const uint8_t a[KISMET_BLE_ADDR_LEN],
                          const uint8_t b[KISMET_BLE_ADDR_LEN])
{
    return memcmp(a, b, KISMET_BLE_ADDR_LEN) == 0;
}

static bool address_is_zero(const uint8_t address[KISMET_BLE_ADDR_LEN])
{
    static const uint8_t zero[KISMET_BLE_ADDR_LEN] = {0};
    return address_equal(address, zero);
}

static bool is_expired(uint64_t now_ms, uint64_t last_seen_ms, uint32_t idle_ms)
{
    return idle_ms != 0u && now_ms >= last_seen_ms &&
           (now_ms - last_seen_ms) >= idle_ms;
}

static kismet_ble_tracker_config_t tracker_defaults(void)
{
    return (kismet_ble_tracker_config_t){
        .max_devices = KISMET_BLE_DEFAULT_MAX_DEVICES,
        .device_idle_ms = KISMET_BLE_DEFAULT_DEVICE_IDLE_MS,
    };
}

static bool config_valid(const kismet_ble_tracker_config_t *cfg)
{
    return cfg != NULL && cfg->max_devices > 0u &&
           cfg->max_devices <= KISMET_BLE_HARD_MAX_DEVICES;
}

static int find_device(const kismet_ble_tracker_t *tracker,
                       const uint8_t address[KISMET_BLE_ADDR_LEN],
                       uint8_t address_type)
{
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        const ble_device_slot_t *slot = &tracker->devices[i];
        if (slot->used && slot->value.address_type == address_type &&
            address_equal(slot->value.address, address)) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_device(kismet_ble_tracker_t *tracker)
{
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        if (!tracker->devices[i].used) {
            return (int)i;
        }
    }

    uint16_t oldest = 0u;
    for (uint16_t i = 1; i < tracker->config.max_devices; ++i) {
        if (tracker->devices[i].value.last_seen_ms <
            tracker->devices[oldest].value.last_seen_ms) {
            oldest = i;
        }
    }

    tracker->stats.device_evictions++;
    tracker->stats.partial_flags |= KISMET_BLE_PARTIAL_DEVICE_EVICTION;
    memset(&tracker->devices[oldest], 0, sizeof(tracker->devices[oldest]));
    return (int)oldest;
}

esp_err_t kismet_ble_tracker_create(const kismet_ble_tracker_config_t *config,
                                    kismet_ble_tracker_t **out_tracker)
{
    if (out_tracker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_tracker = NULL;

    const kismet_ble_tracker_config_t cfg =
        config != NULL ? *config : tracker_defaults();
    if (!config_valid(&cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    kismet_ble_tracker_t *tracker = calloc(1, sizeof(*tracker));
    if (tracker == NULL) {
        return ESP_ERR_NO_MEM;
    }
    tracker->config = cfg;
    tracker->devices = calloc(cfg.max_devices, sizeof(*tracker->devices));
    if (tracker->devices == NULL) {
        free(tracker);
        return ESP_ERR_NO_MEM;
    }

    *out_tracker = tracker;
    return ESP_OK;
}

void kismet_ble_tracker_destroy(kismet_ble_tracker_t *tracker)
{
    if (tracker == NULL) {
        return;
    }
    free(tracker->devices);
    free(tracker);
}

void kismet_ble_tracker_reset(kismet_ble_tracker_t *tracker)
{
    if (tracker == NULL) {
        return;
    }
    memset(tracker->devices, 0,
           tracker->config.max_devices * sizeof(*tracker->devices));
    memset(&tracker->stats, 0, sizeof(tracker->stats));
}

void kismet_ble_tracker_expire(kismet_ble_tracker_t *tracker, uint64_t now_ms)
{
    if (tracker == NULL) {
        return;
    }
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        ble_device_slot_t *slot = &tracker->devices[i];
        if (slot->used &&
            is_expired(now_ms, slot->value.last_seen_ms,
                       tracker->config.device_idle_ms)) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

esp_err_t kismet_ble_tracker_ingest(kismet_ble_tracker_t *tracker,
                                    const kismet_ble_observation_t *observation)
{
    if (tracker == NULL || observation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (address_is_zero(observation->address)) {
        return ESP_OK;
    }

    kismet_ble_tracker_expire(tracker, observation->seen_ms);

    int idx = find_device(tracker, observation->address,
                          observation->address_type);
    if (idx < 0) {
        idx = alloc_device(tracker);
    }

    ble_device_slot_t *slot = &tracker->devices[idx];
    if (!slot->used) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        memcpy(slot->value.address, observation->address,
               KISMET_BLE_ADDR_LEN);
        slot->value.address_type = observation->address_type;
        slot->value.first_seen_ms = observation->seen_ms;
        slot->value.last_seen_ms = observation->seen_ms;
        slot->value.rssi_min = observation->rssi;
        slot->value.rssi_max = observation->rssi;
    } else {
        if (observation->seen_ms < slot->value.first_seen_ms) {
            slot->value.first_seen_ms = observation->seen_ms;
        }
        if (observation->seen_ms > slot->value.last_seen_ms) {
            slot->value.last_seen_ms = observation->seen_ms;
        }
        if (observation->rssi < slot->value.rssi_min) {
            slot->value.rssi_min = observation->rssi;
        }
        if (observation->rssi > slot->value.rssi_max) {
            slot->value.rssi_max = observation->rssi;
        }
    }

    slot->value.rssi_last = observation->rssi;
    slot->value.connectable = observation->connectable;
    slot->value.seen_count++;
    return ESP_OK;
}

size_t kismet_ble_tracker_count(const kismet_ble_tracker_t *tracker)
{
    if (tracker == NULL) {
        return 0u;
    }
    size_t count = 0u;
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        if (tracker->devices[i].used) {
            ++count;
        }
    }
    return count;
}

static int nth_device(const kismet_ble_tracker_t *tracker, size_t index)
{
    size_t current = 0u;
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        if (tracker->devices[i].used) {
            if (current == index) {
                return (int)i;
            }
            ++current;
        }
    }
    return -1;
}

esp_err_t kismet_ble_tracker_get(const kismet_ble_tracker_t *tracker,
                                 size_t index,
                                 kismet_ble_device_t *out_device)
{
    if (tracker == NULL || out_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int idx = nth_device(tracker, index);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_device = tracker->devices[idx].value;
    return ESP_OK;
}

esp_err_t kismet_ble_tracker_get_stats(const kismet_ble_tracker_t *tracker,
                                       kismet_ble_tracker_stats_t *out_stats)
{
    if (tracker == NULL || out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_stats = tracker->stats;
    return ESP_OK;
}

void kismet_ble_tracker_note_capture(kismet_ble_tracker_t *tracker,
                                     uint32_t report_drops,
                                     uint32_t truncated)
{
    if (tracker == NULL) {
        return;
    }

    tracker->stats.report_queue_drops += report_drops;
    tracker->stats.truncated_reports += truncated;
    if (report_drops != 0u) {
        tracker->stats.partial_flags |= KISMET_BLE_PARTIAL_REPORT_DROP;
    }
    if (truncated != 0u) {
        tracker->stats.partial_flags |= KISMET_BLE_PARTIAL_TRUNCATED_REPORT;
    }
}
