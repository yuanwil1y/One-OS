#include "kismet_wifi.h"
#include "kismet_internal.h"

#include <stdlib.h>
#include <string.h>

#define MAC_LEN 6u

typedef struct {
    bool used;
    kismet_wifi_device_t value;
} device_slot_t;

typedef struct {
    bool used;
    kismet_wifi_ssid_t value;
    uint8_t hidden_owner[MAC_LEN];
} ssid_slot_t;

typedef struct {
    bool used;
    kismet_wifi_relation_t value;
} relation_slot_t;

typedef struct {
    bool used;
    uint8_t device[MAC_LEN];
    uint8_t ssid[KISMET_WIFI_MAX_SSID_LEN];
    uint8_t ssid_len;
    uint8_t hidden_owner[MAC_LEN];
    kismet_wifi_ssid_observation_kind_t kind;
    uint64_t last_seen_ms;
} ssid_link_slot_t;

struct kismet_wifi_tracker {
    kismet_wifi_tracker_config_t config;
    device_slot_t *devices;
    ssid_slot_t *ssids;
    relation_slot_t *relations;
    ssid_link_slot_t *links;
    kismet_wifi_tracker_stats_t stats;
};

static bool mac_equal(const uint8_t a[MAC_LEN], const uint8_t b[MAC_LEN])
{
    return memcmp(a, b, MAC_LEN) == 0;
}

static bool mac_is_zero(const uint8_t mac[MAC_LEN])
{
    static const uint8_t zero[MAC_LEN] = {0};
    return mac_equal(mac, zero);
}

static bool is_expired(uint64_t now_ms, uint64_t last_seen_ms, uint32_t idle_ms)
{
    return idle_ms != 0u && now_ms >= last_seen_ms &&
           (now_ms - last_seen_ms) >= idle_ms;
}

static uint32_t role_flag(kismet_wifi_role_t role)
{
    switch (role) {
    case KISMET_WIFI_ROLE_AP: return KISMET_WIFI_ROLE_FLAG_AP;
    case KISMET_WIFI_ROLE_STA: return KISMET_WIFI_ROLE_FLAG_STA;
    case KISMET_WIFI_ROLE_PEER: return KISMET_WIFI_ROLE_FLAG_PEER;
    default: return 0u;
    }
}

static kismet_wifi_role_t merge_role(kismet_wifi_role_t old_role,
                                     kismet_wifi_role_t new_role)
{
    if (new_role == KISMET_WIFI_ROLE_UNKNOWN) return old_role;
    if (old_role == KISMET_WIFI_ROLE_UNKNOWN || old_role == new_role) return new_role;
    return KISMET_WIFI_ROLE_PEER;
}

static void remove_links_for_device(kismet_wifi_tracker_t *tracker,
                                    const uint8_t mac[MAC_LEN])
{
    for (uint16_t i = 0; i < tracker->config.max_ssid_links; ++i) {
        if (tracker->links[i].used && mac_equal(tracker->links[i].device, mac)) {
            tracker->links[i].used = false;
        }
    }
}

static void remove_relations_for_device(kismet_wifi_tracker_t *tracker,
                                        const uint8_t mac[MAC_LEN])
{
    for (uint16_t i = 0; i < tracker->config.max_relations; ++i) {
        relation_slot_t *slot = &tracker->relations[i];
        if (slot->used &&
            (mac_equal(slot->value.client, mac) || mac_equal(slot->value.bssid, mac))) {
            slot->used = false;
        }
    }
}

static bool ssid_identity_equal(const ssid_slot_t *slot,
                                const uint8_t *ssid,
                                uint8_t ssid_len,
                                const uint8_t owner[MAC_LEN])
{
    if (!slot->used || slot->value.ssid_len != ssid_len) return false;
    if (ssid_len != 0u) {
        return memcmp(slot->value.ssid, ssid, ssid_len) == 0;
    }
    return mac_equal(slot->hidden_owner, owner);
}

static bool link_ssid_identity_equal(const ssid_link_slot_t *slot,
                                     const ssid_slot_t *ssid)
{
    if (!slot->used || slot->ssid_len != ssid->value.ssid_len) return false;
    if (slot->ssid_len != 0u) {
        return memcmp(slot->ssid, ssid->value.ssid, slot->ssid_len) == 0;
    }
    return mac_equal(slot->hidden_owner, ssid->hidden_owner);
}

static void remove_links_for_ssid(kismet_wifi_tracker_t *tracker,
                                  const ssid_slot_t *ssid)
{
    for (uint16_t i = 0; i < tracker->config.max_ssid_links; ++i) {
        if (link_ssid_identity_equal(&tracker->links[i], ssid)) {
            tracker->links[i].used = false;
        }
    }
}

static int find_device(const kismet_wifi_tracker_t *tracker, const uint8_t mac[MAC_LEN])
{
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        if (tracker->devices[i].used && mac_equal(tracker->devices[i].value.mac, mac)) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_device(kismet_wifi_tracker_t *tracker)
{
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        if (!tracker->devices[i].used) return (int)i;
    }

    uint16_t oldest = 0;
    for (uint16_t i = 1; i < tracker->config.max_devices; ++i) {
        if (tracker->devices[i].value.last_seen_ms < tracker->devices[oldest].value.last_seen_ms) {
            oldest = i;
        }
    }
    remove_links_for_device(tracker, tracker->devices[oldest].value.mac);
    remove_relations_for_device(tracker, tracker->devices[oldest].value.mac);
    tracker->stats.device_evictions++;
    tracker->stats.partial_flags |= KISMET_WIFI_PARTIAL_DEVICE_EVICTION;
    tracker->devices[oldest].used = false;
    return (int)oldest;
}

static int find_ssid(const kismet_wifi_tracker_t *tracker,
                     const uint8_t *ssid,
                     uint8_t ssid_len,
                     const uint8_t owner[MAC_LEN])
{
    for (uint16_t i = 0; i < tracker->config.max_ssids; ++i) {
        if (ssid_identity_equal(&tracker->ssids[i], ssid, ssid_len, owner)) return (int)i;
    }
    return -1;
}

static int alloc_ssid(kismet_wifi_tracker_t *tracker)
{
    for (uint16_t i = 0; i < tracker->config.max_ssids; ++i) {
        if (!tracker->ssids[i].used) return (int)i;
    }
    uint16_t oldest = 0;
    for (uint16_t i = 1; i < tracker->config.max_ssids; ++i) {
        if (tracker->ssids[i].value.last_seen_ms < tracker->ssids[oldest].value.last_seen_ms) {
            oldest = i;
        }
    }
    remove_links_for_ssid(tracker, &tracker->ssids[oldest]);
    tracker->stats.ssid_evictions++;
    tracker->stats.partial_flags |= KISMET_WIFI_PARTIAL_SSID_EVICTION;
    tracker->ssids[oldest].used = false;
    return (int)oldest;
}

static int find_relation(const kismet_wifi_tracker_t *tracker,
                         const uint8_t client[MAC_LEN],
                         const uint8_t bssid[MAC_LEN])
{
    for (uint16_t i = 0; i < tracker->config.max_relations; ++i) {
        const relation_slot_t *slot = &tracker->relations[i];
        if (slot->used && mac_equal(slot->value.client, client) &&
            mac_equal(slot->value.bssid, bssid)) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_relation(kismet_wifi_tracker_t *tracker)
{
    for (uint16_t i = 0; i < tracker->config.max_relations; ++i) {
        if (!tracker->relations[i].used) return (int)i;
    }
    uint16_t oldest = 0;
    for (uint16_t i = 1; i < tracker->config.max_relations; ++i) {
        if (tracker->relations[i].value.last_seen_ms < tracker->relations[oldest].value.last_seen_ms) {
            oldest = i;
        }
    }
    tracker->stats.relation_evictions++;
    tracker->stats.partial_flags |= KISMET_WIFI_PARTIAL_RELATION_EVICTION;
    tracker->relations[oldest].used = false;
    return (int)oldest;
}

static bool link_equal(const ssid_link_slot_t *slot,
                       const uint8_t device[MAC_LEN],
                       const uint8_t *ssid,
                       uint8_t ssid_len,
                       const uint8_t owner[MAC_LEN],
                       kismet_wifi_ssid_observation_kind_t kind)
{
    if (!slot->used || slot->kind != kind || slot->ssid_len != ssid_len ||
        !mac_equal(slot->device, device)) {
        return false;
    }
    if (ssid_len != 0u) return memcmp(slot->ssid, ssid, ssid_len) == 0;
    return mac_equal(slot->hidden_owner, owner);
}

static void upsert_link(kismet_wifi_tracker_t *tracker,
                        const uint8_t device[MAC_LEN],
                        const uint8_t *ssid,
                        uint8_t ssid_len,
                        const uint8_t owner[MAC_LEN],
                        kismet_wifi_ssid_observation_kind_t kind,
                        uint64_t seen_ms)
{
    for (uint16_t i = 0; i < tracker->config.max_ssid_links; ++i) {
        if (link_equal(&tracker->links[i], device, ssid, ssid_len, owner, kind)) {
            tracker->links[i].last_seen_ms = seen_ms;
            return;
        }
    }

    int free_idx = -1;
    uint16_t oldest = 0;
    bool have_oldest = false;
    for (uint16_t i = 0; i < tracker->config.max_ssid_links; ++i) {
        if (!tracker->links[i].used) {
            free_idx = (int)i;
            break;
        }
        if (!have_oldest || tracker->links[i].last_seen_ms < tracker->links[oldest].last_seen_ms) {
            oldest = i;
            have_oldest = true;
        }
    }
    if (free_idx < 0) {
        free_idx = (int)oldest;
        tracker->stats.ssid_link_evictions++;
        tracker->stats.partial_flags |= KISMET_WIFI_PARTIAL_LINK_EVICTION;
    }

    ssid_link_slot_t *slot = &tracker->links[free_idx];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    memcpy(slot->device, device, MAC_LEN);
    if (ssid_len != 0u) memcpy(slot->ssid, ssid, ssid_len);
    else memcpy(slot->hidden_owner, owner, MAC_LEN);
    slot->ssid_len = ssid_len;
    slot->kind = kind;
    slot->last_seen_ms = seen_ms;
}

static kismet_wifi_tracker_config_t defaults(void)
{
    return (kismet_wifi_tracker_config_t){
        .max_devices = KISMET_WIFI_DEFAULT_MAX_DEVICES,
        .max_ssids = KISMET_WIFI_DEFAULT_MAX_SSIDS,
        .max_relations = KISMET_WIFI_DEFAULT_MAX_RELATIONS,
        .max_ssid_links = KISMET_WIFI_DEFAULT_MAX_SSID_LINKS,
        .device_idle_ms = KISMET_WIFI_DEFAULT_DEVICE_IDLE_MS,
        .ssid_idle_ms = KISMET_WIFI_DEFAULT_SSID_IDLE_MS,
        .relation_idle_ms = KISMET_WIFI_DEFAULT_RELATION_IDLE_MS,
    };
}

static bool config_valid(const kismet_wifi_tracker_config_t *cfg)
{
    return cfg->max_devices > 0u && cfg->max_devices <= KISMET_WIFI_HARD_MAX_DEVICES &&
           cfg->max_ssids > 0u && cfg->max_ssids <= KISMET_WIFI_HARD_MAX_SSIDS &&
           cfg->max_relations > 0u && cfg->max_relations <= KISMET_WIFI_HARD_MAX_RELATIONS &&
           cfg->max_ssid_links > 0u && cfg->max_ssid_links <= KISMET_WIFI_HARD_MAX_SSID_LINKS;
}

esp_err_t kismet_wifi_tracker_create(const kismet_wifi_tracker_config_t *config,
                                     kismet_wifi_tracker_t **out_tracker)
{
    if (out_tracker == NULL) return ESP_ERR_INVALID_ARG;
    *out_tracker = NULL;
    kismet_wifi_tracker_config_t cfg = config != NULL ? *config : defaults();
    if (!config_valid(&cfg)) return ESP_ERR_INVALID_ARG;

    kismet_wifi_tracker_t *tracker = calloc(1, sizeof(*tracker));
    if (tracker == NULL) return ESP_ERR_NO_MEM;
    tracker->config = cfg;
    tracker->devices = calloc(cfg.max_devices, sizeof(*tracker->devices));
    tracker->ssids = calloc(cfg.max_ssids, sizeof(*tracker->ssids));
    tracker->relations = calloc(cfg.max_relations, sizeof(*tracker->relations));
    tracker->links = calloc(cfg.max_ssid_links, sizeof(*tracker->links));
    if (tracker->devices == NULL || tracker->ssids == NULL ||
        tracker->relations == NULL || tracker->links == NULL) {
        kismet_wifi_tracker_destroy(tracker);
        return ESP_ERR_NO_MEM;
    }
    *out_tracker = tracker;
    return ESP_OK;
}

void kismet_wifi_tracker_destroy(kismet_wifi_tracker_t *tracker)
{
    if (tracker == NULL) return;
    free(tracker->devices);
    free(tracker->ssids);
    free(tracker->relations);
    free(tracker->links);
    free(tracker);
}

void kismet_wifi_tracker_reset(kismet_wifi_tracker_t *tracker)
{
    if (tracker == NULL) return;
    memset(tracker->devices, 0, tracker->config.max_devices * sizeof(*tracker->devices));
    memset(tracker->ssids, 0, tracker->config.max_ssids * sizeof(*tracker->ssids));
    memset(tracker->relations, 0, tracker->config.max_relations * sizeof(*tracker->relations));
    memset(tracker->links, 0, tracker->config.max_ssid_links * sizeof(*tracker->links));
    memset(&tracker->stats, 0, sizeof(tracker->stats));
}

void kismet_wifi_tracker_expire(kismet_wifi_tracker_t *tracker, uint64_t now_ms)
{
    if (tracker == NULL) return;
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        device_slot_t *slot = &tracker->devices[i];
        if (slot->used && is_expired(now_ms, slot->value.last_seen_ms, tracker->config.device_idle_ms)) {
            uint8_t mac[MAC_LEN];
            memcpy(mac, slot->value.mac, MAC_LEN);
            slot->used = false;
            remove_links_for_device(tracker, mac);
            remove_relations_for_device(tracker, mac);
        }
    }
    for (uint16_t i = 0; i < tracker->config.max_ssids; ++i) {
        ssid_slot_t *slot = &tracker->ssids[i];
        if (slot->used && is_expired(now_ms, slot->value.last_seen_ms, tracker->config.ssid_idle_ms)) {
            remove_links_for_ssid(tracker, slot);
            slot->used = false;
        }
    }
    for (uint16_t i = 0; i < tracker->config.max_relations; ++i) {
        relation_slot_t *slot = &tracker->relations[i];
        if (slot->used && is_expired(now_ms, slot->value.last_seen_ms, tracker->config.relation_idle_ms)) {
            slot->used = false;
        }
    }
}

static void upsert_device(kismet_wifi_tracker_t *tracker,
                          const kismet_wifi_observation_t *obs)
{
    if (mac_is_zero(obs->device)) return;
    int idx = find_device(tracker, obs->device);
    if (idx < 0) idx = alloc_device(tracker);
    device_slot_t *slot = &tracker->devices[idx];
    if (!slot->used) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        memcpy(slot->value.mac, obs->device, MAC_LEN);
        slot->value.role = obs->role;
        slot->value.role_flags = role_flag(obs->role);
        slot->value.first_seen_ms = obs->seen_ms;
        slot->value.rssi_min = obs->rssi;
        slot->value.rssi_max = obs->rssi;
    } else {
        slot->value.role = merge_role(slot->value.role, obs->role);
        slot->value.role_flags |= role_flag(obs->role);
        if (obs->seen_ms < slot->value.first_seen_ms) slot->value.first_seen_ms = obs->seen_ms;
        if (obs->rssi < slot->value.rssi_min) slot->value.rssi_min = obs->rssi;
        if (obs->rssi > slot->value.rssi_max) slot->value.rssi_max = obs->rssi;
    }
    if (obs->seen_ms > slot->value.last_seen_ms) slot->value.last_seen_ms = obs->seen_ms;
    slot->value.rssi_last = obs->rssi;
    slot->value.last_channel = obs->channel;
    if (obs->channel >= 1u && obs->channel <= 14u) {
        slot->value.channel_bitmap |= (uint16_t)(1u << (obs->channel - 1u));
    }
    slot->value.seen_count++;
    if (obs->frame_class == KISMET_WIFI_FRAME_MANAGEMENT) slot->value.management_count++;
    else if (obs->frame_class == KISMET_WIFI_FRAME_DATA) slot->value.data_count++;
}

static void upsert_ssid(kismet_wifi_tracker_t *tracker,
                        const kismet_wifi_observation_t *obs)
{
    if (!obs->has_ssid || obs->ssid_kind == KISMET_WIFI_SSID_NONE ||
        obs->ssid_len > KISMET_WIFI_MAX_SSID_LEN || mac_is_zero(obs->device)) {
        return;
    }
    const uint8_t *owner = obs->device;
    int idx = find_ssid(tracker, obs->ssid, obs->ssid_len, owner);
    if (idx < 0) idx = alloc_ssid(tracker);
    ssid_slot_t *slot = &tracker->ssids[idx];
    if (!slot->used) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->value.ssid_len = obs->ssid_len;
        if (obs->ssid_len != 0u) memcpy(slot->value.ssid, obs->ssid, obs->ssid_len);
        else {
            slot->value.flags |= KISMET_WIFI_SSID_FLAG_HIDDEN;
            memcpy(slot->hidden_owner, owner, MAC_LEN);
        }
        slot->value.first_seen_ms = obs->seen_ms;
    } else if (obs->seen_ms < slot->value.first_seen_ms) {
        slot->value.first_seen_ms = obs->seen_ms;
    }
    if (obs->seen_ms > slot->value.last_seen_ms) slot->value.last_seen_ms = obs->seen_ms;
    slot->value.seen_count++;
    upsert_link(tracker, obs->device, obs->ssid, obs->ssid_len, owner,
                obs->ssid_kind, obs->seen_ms);
}

static void upsert_relation(kismet_wifi_tracker_t *tracker,
                            const kismet_wifi_observation_t *obs)
{
    if (!obs->has_relation || mac_is_zero(obs->relation_client) || mac_is_zero(obs->relation_bssid)) return;
    if ((obs->relation_client[0] & 0x01u) != 0u || (obs->relation_bssid[0] & 0x01u) != 0u) return;
    int idx = find_relation(tracker, obs->relation_client, obs->relation_bssid);
    if (idx < 0) idx = alloc_relation(tracker);
    relation_slot_t *slot = &tracker->relations[idx];
    if (!slot->used) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        memcpy(slot->value.client, obs->relation_client, MAC_LEN);
        memcpy(slot->value.bssid, obs->relation_bssid, MAC_LEN);
        slot->value.first_seen_ms = obs->seen_ms;
    } else if (obs->seen_ms < slot->value.first_seen_ms) {
        slot->value.first_seen_ms = obs->seen_ms;
    }
    if (obs->seen_ms > slot->value.last_seen_ms) slot->value.last_seen_ms = obs->seen_ms;
    slot->value.seen_count++;
    slot->value.evidence_flags |= obs->relation_evidence_flags;
}

esp_err_t kismet_wifi_tracker_ingest(kismet_wifi_tracker_t *tracker,
                                     const kismet_wifi_observation_t *observation)
{
    if (tracker == NULL || observation == NULL || observation->ssid_len > KISMET_WIFI_MAX_SSID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    kismet_wifi_tracker_expire(tracker, observation->seen_ms);
    upsert_device(tracker, observation);
    upsert_ssid(tracker, observation);
    upsert_relation(tracker, observation);
    return ESP_OK;
}

static size_t used_devices(const kismet_wifi_tracker_t *tracker)
{
    size_t n = 0;
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) n += tracker->devices[i].used ? 1u : 0u;
    return n;
}

static size_t used_ssids(const kismet_wifi_tracker_t *tracker)
{
    size_t n = 0;
    for (uint16_t i = 0; i < tracker->config.max_ssids; ++i) n += tracker->ssids[i].used ? 1u : 0u;
    return n;
}

static size_t used_relations(const kismet_wifi_tracker_t *tracker)
{
    size_t n = 0;
    for (uint16_t i = 0; i < tracker->config.max_relations; ++i) n += tracker->relations[i].used ? 1u : 0u;
    return n;
}

size_t kismet_wifi_tracker_device_count(const kismet_wifi_tracker_t *tracker)
{
    return tracker == NULL ? 0u : used_devices(tracker);
}

size_t kismet_wifi_tracker_ssid_count(const kismet_wifi_tracker_t *tracker)
{
    return tracker == NULL ? 0u : used_ssids(tracker);
}

size_t kismet_wifi_tracker_relation_count(const kismet_wifi_tracker_t *tracker)
{
    return tracker == NULL ? 0u : used_relations(tracker);
}

static int nth_device(const kismet_wifi_tracker_t *tracker, size_t index)
{
    size_t n = 0;
    for (uint16_t i = 0; i < tracker->config.max_devices; ++i) {
        if (tracker->devices[i].used && n++ == index) return (int)i;
    }
    return -1;
}

static int nth_ssid(const kismet_wifi_tracker_t *tracker, size_t index)
{
    size_t n = 0;
    for (uint16_t i = 0; i < tracker->config.max_ssids; ++i) {
        if (tracker->ssids[i].used && n++ == index) return (int)i;
    }
    return -1;
}

static int nth_relation(const kismet_wifi_tracker_t *tracker, size_t index)
{
    size_t n = 0;
    for (uint16_t i = 0; i < tracker->config.max_relations; ++i) {
        if (tracker->relations[i].used && n++ == index) return (int)i;
    }
    return -1;
}

esp_err_t kismet_wifi_tracker_get_device(const kismet_wifi_tracker_t *tracker,
                                         size_t index,
                                         kismet_wifi_device_t *out_device)
{
    if (tracker == NULL || out_device == NULL) return ESP_ERR_INVALID_ARG;
    int idx = nth_device(tracker, index);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    *out_device = tracker->devices[idx].value;
    return ESP_OK;
}

esp_err_t kismet_wifi_tracker_get_ssid(const kismet_wifi_tracker_t *tracker,
                                       size_t index,
                                       kismet_wifi_ssid_t *out_ssid)
{
    if (tracker == NULL || out_ssid == NULL) return ESP_ERR_INVALID_ARG;
    int idx = nth_ssid(tracker, index);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    *out_ssid = tracker->ssids[idx].value;
    out_ssid->advertiser_count = 0;
    out_ssid->responder_count = 0;
    out_ssid->prober_count = 0;
    for (uint16_t i = 0; i < tracker->config.max_ssid_links; ++i) {
        const ssid_link_slot_t *link = &tracker->links[i];
        if (!link_ssid_identity_equal(link, &tracker->ssids[idx])) continue;
        if (link->kind == KISMET_WIFI_SSID_ADVERTISED) out_ssid->advertiser_count++;
        else if (link->kind == KISMET_WIFI_SSID_RESPONDED) out_ssid->responder_count++;
        else if (link->kind == KISMET_WIFI_SSID_PROBED) out_ssid->prober_count++;
    }
    return ESP_OK;
}

esp_err_t kismet_wifi_tracker_get_relation(const kismet_wifi_tracker_t *tracker,
                                           size_t index,
                                           kismet_wifi_relation_t *out_relation)
{
    if (tracker == NULL || out_relation == NULL) return ESP_ERR_INVALID_ARG;
    int idx = nth_relation(tracker, index);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    *out_relation = tracker->relations[idx].value;
    return ESP_OK;
}

esp_err_t kismet_wifi_tracker_get_device_ssid(const kismet_wifi_tracker_t *tracker,
                                              const uint8_t mac[6],
                                              uint8_t *out_ssid,
                                              size_t out_capacity,
                                              uint8_t *out_len,
                                              bool *out_hidden)
{
    uint16_t i;
    int best_rank = -1;

    if (tracker == NULL || mac == NULL || out_ssid == NULL || out_capacity == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len != NULL) *out_len = 0u;
    if (out_hidden != NULL) *out_hidden = false;

    if (find_device(tracker, mac) < 0) return ESP_ERR_NOT_FOUND;

    /*
     * Prefer the strongest evidence for what this device calls itself:
     * ADVERTISED (it is the AP) over RESPONDED (probe response) over PROBED
     * (client looking for that name).
     */
    for (i = 0; i < tracker->config.max_ssid_links; ++i) {
        const ssid_link_slot_t *link = &tracker->links[i];
        int rank;
        if (!link->used || !mac_equal(link->device, mac)) continue;
        switch (link->kind) {
        case KISMET_WIFI_SSID_ADVERTISED: rank = 3; break;
        case KISMET_WIFI_SSID_RESPONDED:  rank = 2; break;
        case KISMET_WIFI_SSID_PROBED:     rank = 1; break;
        default:                          rank = 0; break;
        }
        if (rank <= best_rank) continue;

        if (link->ssid_len == 0u) {
            /* Hidden SSID: report it as such with no bytes. */
            best_rank = rank;
            if (out_len != NULL) *out_len = 0u;
            if (out_hidden != NULL) *out_hidden = true;
            continue;
        }
        if (link->ssid_len > out_capacity) continue;

        memcpy(out_ssid, link->ssid, link->ssid_len);
        if (out_len != NULL) *out_len = link->ssid_len;
        if (out_hidden != NULL) *out_hidden = false;
        best_rank = rank;
    }

    return best_rank < 0 ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t kismet_wifi_tracker_get_stats(const kismet_wifi_tracker_t *tracker,
                                        kismet_wifi_tracker_stats_t *out_stats)
{
    if (tracker == NULL || out_stats == NULL) return ESP_ERR_INVALID_ARG;
    *out_stats = tracker->stats;
    return ESP_OK;
}

void kismet_wifi_tracker_note_capture(kismet_wifi_tracker_t *tracker,
                                      uint32_t rx_drops,
                                      uint32_t malformed,
                                      uint32_t truncated)
{
    if (tracker == NULL) return;
    tracker->stats.rx_queue_drops += rx_drops;
    tracker->stats.malformed_frames += malformed;
    tracker->stats.truncated_frames += truncated;
    if (rx_drops != 0u) tracker->stats.partial_flags |= KISMET_WIFI_PARTIAL_RX_DROP;
    if (malformed != 0u) tracker->stats.partial_flags |= KISMET_WIFI_PARTIAL_MALFORMED_FRAME;
    if (truncated != 0u) tracker->stats.partial_flags |= KISMET_WIFI_PARTIAL_TRUNCATED_FRAME;
}
