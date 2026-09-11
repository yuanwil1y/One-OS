/*
 * Application Device/Entity binding table.
 *
 * Platform independent: compiled into the ESP32-C6 firmware and the host tests.
 * No ESP-IDF/FreeRTOS/LVGL headers may be included here.
 *
 * ha_core is not thread safe and is owned by one task; every function here
 * therefore has the same single-owner contract.
 */

#include "app_device.h"

#include <stdio.h>
#include <string.h>

/* ---------------- table storage ---------------- */

typedef struct {
    app_device_binding_t value;
    bool in_use;
    bool seen_this_generation;
} device_slot_t;

typedef struct {
    app_entity_binding_t value;
    bool in_use;
} entity_slot_t;

static device_slot_t s_devices[APP_DEVICE_MAX];
static entity_slot_t s_entities[APP_ENTITY_MAX];
static uint32_t s_generation;
static uint32_t s_swept;

/* Defined below; needed by the eviction path to avoid orphaned Entities. */
static void entity_slot_free_for_device(const char *device_id);

const char *app_recognition_name(app_recognition_state_t state)
{
    switch (state) {
    case APP_RECOGNITION_UNKNOWN:        return "unknown";
    case APP_RECOGNITION_AMBIGUOUS:      return "ambiguous";
    case APP_RECOGNITION_DB_UNAVAILABLE: return "db_unavailable";
    case APP_RECOGNITION_MATCHED:        return "matched";
    default:                             return "invalid";
    }
}

const char *app_availability_name(app_availability_t availability)
{
    switch (availability) {
    case APP_AVAILABILITY_UNKNOWN:     return "unknown";
    case APP_AVAILABILITY_ONLINE:      return "online";
    case APP_AVAILABILITY_STALE:       return "stale";
    case APP_AVAILABILITY_UNAVAILABLE: return "unavailable";
    default:                           return "invalid";
    }
}

void app_device_table_reset(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_entities, 0, sizeof(s_entities));
    s_generation = 0u;
    s_swept = 0u;
    ha_core_reset();
}

/* ---------------- device slots ---------------- */

static device_slot_t *device_slot_find(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        if (s_devices[i].in_use && strcmp(s_devices[i].value.device_id, device_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static device_slot_t *device_slot_alloc(void)
{
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        if (!s_devices[i].in_use) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
            s_devices[i].in_use = true;
            return &s_devices[i];
        }
    }
    return NULL;
}

/*
 * Eviction priority: only ephemeral bindings are ever evicted, and the oldest
 * one goes first. A persistent (authorized) identity is never displaced by an
 * ephemeral observation.
 *
 * Returns the victim with its previous id still readable in `*out_prev_id`, and
 * leaves the slot cleared and claimed for the caller to fill. The caller is
 * responsible for removing the victim's ha_core Device/Entities.
 */
static device_slot_t *device_slot_evict_ephemeral(char *out_prev_id,
                                                  size_t out_prev_id_size)
{
    device_slot_t *victim = NULL;

    if (out_prev_id != NULL && out_prev_id_size > 0u) {
        out_prev_id[0] = '\0';
    }

    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        if (!s_devices[i].in_use || !s_devices[i].value.ephemeral) {
            continue;
        }
        if (victim == NULL ||
            s_devices[i].value.last_seen_ms < victim->value.last_seen_ms) {
            victim = &s_devices[i];
        }
    }

    if (victim == NULL) {
        return NULL;
    }

    if (out_prev_id != NULL && out_prev_id_size > 0u) {
        (void)strlcpy(out_prev_id, victim->value.device_id, out_prev_id_size);
    }
    memset(victim, 0, sizeof(*victim));
    victim->in_use = true;
    s_swept++;
    return victim;
}

/* ---------------- entity slots ---------------- */

static entity_slot_t *entity_slot_find(const char *entity_id)
{
    if (entity_id == NULL || entity_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0u; i < APP_ENTITY_MAX; ++i) {
        if (s_entities[i].in_use && strcmp(s_entities[i].value.entity_id, entity_id) == 0) {
            return &s_entities[i];
        }
    }
    return NULL;
}

static entity_slot_t *entity_slot_alloc(void)
{
    for (size_t i = 0u; i < APP_ENTITY_MAX; ++i) {
        if (!s_entities[i].in_use) {
            memset(&s_entities[i], 0, sizeof(s_entities[i]));
            s_entities[i].in_use = true;
            return &s_entities[i];
        }
    }
    return NULL;
}

static void entity_slot_free_for_device(const char *device_id)
{
    for (size_t i = 0u; i < APP_ENTITY_MAX; ++i) {
        if (s_entities[i].in_use &&
            strcmp(s_entities[i].value.device_id, device_id) == 0) {
            memset(&s_entities[i], 0, sizeof(s_entities[i]));
        }
    }
}

/* ---------------- enumeration ---------------- */

size_t app_device_count(void)
{
    size_t count = 0u;
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        if (s_devices[i].in_use) {
            ++count;
        }
    }
    return count;
}

const app_device_binding_t *app_device_at(size_t index)
{
    size_t seen = 0u;
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        if (!s_devices[i].in_use) {
            continue;
        }
        if (seen++ == index) {
            return &s_devices[i].value;
        }
    }
    return NULL;
}

const app_device_binding_t *app_device_find(const char *device_id)
{
    const device_slot_t *slot = device_slot_find(device_id);
    return slot == NULL ? NULL : &slot->value;
}

size_t app_entity_count(void)
{
    size_t count = 0u;
    for (size_t i = 0u; i < APP_ENTITY_MAX; ++i) {
        if (s_entities[i].in_use) {
            ++count;
        }
    }
    return count;
}

const app_entity_binding_t *app_entity_at(size_t index)
{
    size_t seen = 0u;
    for (size_t i = 0u; i < APP_ENTITY_MAX; ++i) {
        if (!s_entities[i].in_use) {
            continue;
        }
        if (seen++ == index) {
            return &s_entities[i].value;
        }
    }
    return NULL;
}

const app_entity_binding_t *app_entity_find(const char *entity_id)
{
    const entity_slot_t *slot = entity_slot_find(entity_id);
    return slot == NULL ? NULL : &slot->value;
}

size_t app_entity_count_for_device(const char *device_id)
{
    size_t count = 0u;

    if (device_id == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < APP_ENTITY_MAX; ++i) {
        if (s_entities[i].in_use &&
            strcmp(s_entities[i].value.device_id, device_id) == 0) {
            ++count;
        }
    }
    return count;
}

uint32_t app_device_generation(void)
{
    return s_generation;
}

/* ---------------- generation lifecycle ---------------- */

void app_device_generation_begin(uint32_t generation)
{
    s_generation = generation;
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        if (s_devices[i].in_use) {
            s_devices[i].seen_this_generation = false;
        }
    }
}

void app_device_generation_finish(void)
{
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        device_slot_t *slot = &s_devices[i];

        if (!slot->in_use) {
            continue;
        }
        if (slot->seen_this_generation) {
            slot->value.availability = APP_AVAILABILITY_ONLINE;
            slot->value.last_generation = s_generation;
            continue;
        }

        /* Not observed in this generation. */
        if (slot->value.ephemeral) {
            /* Ephemeral observations belong to the RF environment "now": once a
             * later generation completes without seeing them, they are gone
             * rather than accumulating forever. */
            if (slot->value.last_generation < s_generation) {
                entity_slot_free_for_device(slot->value.device_id);
                (void)ha_core_device_remove(slot->value.ha_device_id);
                memset(slot, 0, sizeof(*slot));
                s_swept++;
            }
        } else {
            /* Authorized/persistent identity: keep it, mark it stale so the user
             * still sees the device instead of losing controller identity. */
            slot->value.availability = APP_AVAILABILITY_STALE;
        }
    }
}

/* ---------------- identity ---------------- */

static void format_device_id(char *out, size_t out_size, const char *prefix,
                             const char *key)
{
    (void)snprintf(out, out_size, "%s%s", prefix, key);
}

static void format_mac(char *out, size_t out_size, const uint8_t mac[6])
{
    (void)snprintf(out, out_size, "%02x%02x%02x%02x%02x%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void format_ip_key(char *out, size_t out_size, const char *ipv4)
{
    size_t j = 0u;

    for (size_t i = 0u; ipv4[i] != '\0' && j + 1u < out_size; ++i) {
        char c = ipv4[i];
        out[j++] = (c == '.') ? '_' : c;
    }
    out[j] = '\0';
}

/* ---------------- device upsert ---------------- */

static device_slot_t *device_upsert(const char *device_id,
                                    const char *ha_device_id,
                                    const char *name,
                                    const char *manufacturer,
                                    const char *model,
                                    const char *protocol_label,
                                    uint32_t source_bit,
                                    bool ephemeral,
                                    const ha_identifier_t *identifier,
                                    const ha_connection_t *connection,
                                    uint64_t seen_ms,
                                    bool *out_created,
                                    bool *out_truncated)
{
    device_slot_t *slot = device_slot_find(device_id);
    ha_device_t device;
    ha_core_status_t status;

    if (out_created != NULL) {
        *out_created = false;
    }

    if (slot == NULL) {
        char evicted_id[HA_CORE_ID_LEN];

        slot = device_slot_alloc();
        if (slot == NULL) {
            /* Table full. Evict the oldest ephemeral binding, but take its
             * ha_core Device and Entities with it: leaving them behind would
             * leave orphaned state that the UI could still address. */
            device_slot_t *victim =
                device_slot_evict_ephemeral(evicted_id, sizeof(evicted_id));
            if (victim == NULL) {
                if (out_truncated != NULL) {
                    *out_truncated = true;
                }
                return NULL;
            }
            entity_slot_free_for_device(evicted_id);
            (void)ha_core_device_remove(evicted_id);
            slot = victim;
            if (out_truncated != NULL) {
                *out_truncated = true;
            }
        }
        (void)strlcpy(slot->value.device_id, device_id, sizeof(slot->value.device_id));
        (void)strlcpy(slot->value.ha_device_id, ha_device_id,
                      sizeof(slot->value.ha_device_id));
        (void)strlcpy(slot->value.protocol_label, protocol_label,
                      sizeof(slot->value.protocol_label));
        slot->value.ephemeral = ephemeral;
        /* Until the SD Device DB is wired there is no recognition source at all,
         * so every device is reported as such and stays read-only. */
        slot->value.recognition = APP_RECOGNITION_DB_UNAVAILABLE;
        slot->value.read_only = true;
        slot->value.first_generation = s_generation;
        slot->value.first_seen_ms = seen_ms;
        if (out_created != NULL) {
            *out_created = true;
        }
    }

    slot->value.sources |= source_bit;
    slot->value.last_generation = s_generation;
    slot->seen_this_generation = true;
    slot->value.availability = APP_AVAILABILITY_ONLINE;
    if (seen_ms >= slot->value.last_seen_ms) {
        slot->value.last_seen_ms = seen_ms;
    }
    if (slot->value.first_seen_ms == 0u || (seen_ms != 0u && seen_ms < slot->value.first_seen_ms)) {
        slot->value.first_seen_ms = seen_ms;
    }

    memset(&device, 0, sizeof(device));
    (void)strlcpy(device.id, ha_device_id, sizeof(device.id));
    (void)strlcpy(device.name, name, sizeof(device.name));
    (void)strlcpy(device.manufacturer, manufacturer, sizeof(device.manufacturer));
    (void)strlcpy(device.model, model, sizeof(device.model));
    (void)strlcpy(device.model_id, protocol_label, sizeof(device.model_id));

    if (identifier != NULL) {
        device.identifiers[0] = *identifier;
        device.identifier_count = 1u;
    }
    if (connection != NULL) {
        device.connections[0] = *connection;
        device.connection_count = 1u;
    }

    status = ha_core_device_upsert(&device);
    if (status == HA_CORE_CAPACITY || status == HA_CORE_CONFLICT) {
        if (out_truncated != NULL) {
            *out_truncated = true;
        }
    }
    return slot;
}

/* ---------------- generic read-only entities ---------------- */

static void entity_upsert(device_slot_t *device,
                          const char *domain,
                          const char *object_id,
                          const char *name,
                          const char *device_class,
                          const char *unit,
                          const char *state_value,
                          bool *out_truncated)
{
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char unique_id[HA_CORE_UNIQUE_ID_LEN];
    entity_slot_t *slot;
    ha_entity_t entity;
    ha_core_status_t status;

    (void)snprintf(entity_id, sizeof(entity_id), "%s.%s_%s", domain,
                   device->value.device_id, object_id);
    (void)snprintf(unique_id, sizeof(unique_id), "%s_%s", device->value.device_id,
                   object_id);

    memset(&entity, 0, sizeof(entity));
    (void)strlcpy(entity.entity_id, entity_id, sizeof(entity.entity_id));
    (void)strlcpy(entity.unique_id, unique_id, sizeof(entity.unique_id));
    (void)strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)strlcpy(entity.domain, domain, sizeof(entity.domain));
    (void)strlcpy(entity.device_id, device->value.ha_device_id, sizeof(entity.device_id));
    (void)strlcpy(entity.name, name, sizeof(entity.name));
    (void)strlcpy(entity.device_class, device_class, sizeof(entity.device_class));
    (void)strlcpy(entity.unit_of_measurement, unit, sizeof(entity.unit_of_measurement));
    entity.has_entity_name = true;
    entity.enabled = true;
    entity.available = true;
    entity.supported_services = 0u; /* read-only: no service may be dispatched */
    entity.service_handler = NULL;
    entity.service_context = NULL;

    status = ha_core_entity_upsert(&entity);
    if (status != HA_CORE_OK) {
        if (out_truncated != NULL) {
            *out_truncated = true;
        }
        return;
    }

    slot = entity_slot_find(entity_id);
    if (slot == NULL) {
        slot = entity_slot_alloc();
        if (slot == NULL) {
            if (out_truncated != NULL) {
                *out_truncated = true;
            }
            return;
        }
        (void)strlcpy(slot->value.entity_id, entity_id, sizeof(slot->value.entity_id));
        (void)strlcpy(slot->value.device_id, device->value.device_id,
                      sizeof(slot->value.device_id));
        (void)strlcpy(slot->value.domain, domain, sizeof(slot->value.domain));
        (void)strlcpy(slot->value.name, name, sizeof(slot->value.name));
        (void)strlcpy(slot->value.unit, unit, sizeof(slot->value.unit));
        /* No writable entity exists yet: every binding is read-only until a
         * protocol profile with a verified control path is available. */
        slot->value.writable = false;
    }

    if (state_value != NULL) {
        (void)ha_core_state_set(entity_id, state_value, NULL, 0u);
    }
}

static void entity_upsert_signal(device_slot_t *device, bool *out_truncated)
{
    char value[HA_CORE_STATE_LEN];

    if (!device->value.has_signal) {
        return;
    }
    (void)snprintf(value, sizeof(value), "%d", (int)device->value.signal_dbm);
    entity_upsert(device, HA_DOMAIN_SENSOR, "signal_strength", "Signal strength",
                  "signal_strength", "dBm", value, out_truncated);
}

static void entity_upsert_last_seen(device_slot_t *device, bool *out_truncated)
{
    char value[HA_CORE_STATE_LEN];

    /* Seconds since boot, not a wall-clock timestamp: the board has no RTC and
     * inventing a date would be worse than an honest monotonic counter. */
    (void)snprintf(value, sizeof(value), "%llu",
                   (unsigned long long)(device->value.last_seen_ms / 1000u));
    entity_upsert(device, HA_DOMAIN_SENSOR, "last_seen", "Last seen",
                  "timestamp", "s", value, out_truncated);
}

/* ---------------- materialisation ---------------- */

static device_slot_t *materialize_wifi(const app_scan_evidence_t *ev,
                                       size_t index,
                                       bool *out_truncated)
{
    const app_scan_wifi_t *obs = &ev->wifi[index];
    char key[16];
    char device_id[HA_CORE_ID_LEN];
    char ha_device_id[HA_CORE_ID_LEN];
    char name[HA_CORE_NAME_LEN];
    ha_identifier_t identifier;
    ha_connection_t connection;
    device_slot_t *slot;
    bool created = false;

    format_mac(key, sizeof(key), obs->bssid);
    format_device_id(device_id, sizeof(device_id), "wifi_", key);
    format_device_id(ha_device_id, sizeof(ha_device_id), "wifi_", key);

    if (obs->has_ssid && obs->ssid_len > 0u) {
        /* The SSID is untrusted bytes from the air. Copy it as a bounded C
         * string and refuse anything that is not printable, so a hostile SSID
         * cannot inject control characters into the console or the future UI. */
        size_t len = obs->ssid_len < sizeof(name) - 1u ? obs->ssid_len : sizeof(name) - 1u;
        bool printable = true;

        for (size_t i = 0u; i < len; ++i) {
            if (obs->ssid[i] < 0x20u || obs->ssid[i] > 0x7Eu) {
                printable = false;
                break;
            }
        }
        if (printable) {
            memcpy(name, obs->ssid, len);
            name[len] = '\0';
        } else {
            (void)strlcpy(name, obs->ssid_hidden ? "Hidden Wi-Fi AP" : "Unknown Wi-Fi Device",
                          sizeof(name));
        }
    } else {
        (void)strlcpy(name, obs->ssid_hidden ? "Hidden Wi-Fi AP" : "Unknown Wi-Fi Device",
                      sizeof(name));
    }

    memset(&identifier, 0, sizeof(identifier));
    (void)strlcpy(identifier.domain, "wifi_bssid", sizeof(identifier.domain));
    (void)strlcpy(identifier.value, key, sizeof(identifier.value));

    memset(&connection, 0, sizeof(connection));
    (void)strlcpy(connection.type, "mac", sizeof(connection.type));
    (void)strlcpy(connection.value, key, sizeof(connection.value));

    slot = device_upsert(device_id, ha_device_id, name, "", "", "wifi",
                         APP_SOURCE_WIFI, true, &identifier, &connection,
                         obs->last_seen_ms, &created, out_truncated);
    if (slot == NULL) {
        return NULL;
    }

    slot->value.has_signal = true;
    slot->value.signal_dbm = obs->rssi_last;

    entity_upsert_signal(slot, out_truncated);
    entity_upsert_last_seen(slot, out_truncated);

    if (obs->channel != 0u) {
        char value[HA_CORE_STATE_LEN];
        (void)snprintf(value, sizeof(value), "%u", (unsigned)obs->channel);
        entity_upsert(slot, HA_DOMAIN_SENSOR, "channel", "Channel", "",
                      "", value, out_truncated);
    }

    return slot;
}

static device_slot_t *materialize_ble(const app_scan_evidence_t *ev,
                                      size_t index,
                                      bool *out_truncated)
{
    const app_scan_ble_t *obs = &ev->ble[index];
    char key[16];
    char type_key[24];
    char device_id[HA_CORE_ID_LEN];
    char ha_device_id[HA_CORE_ID_LEN];
    char name[HA_CORE_NAME_LEN];
    ha_identifier_t identifier;
    ha_connection_t connection;
    device_slot_t *slot;
    bool created = false;

    format_mac(key, sizeof(key), obs->address);
    /* Address type is part of the identity: the same bytes with a different
     * type are a different peer, so it is encoded into the key. */
    (void)snprintf(type_key, sizeof(type_key), "%02x%s", obs->address_type, key);
    format_device_id(device_id, sizeof(device_id), "ble_", type_key);
    format_device_id(ha_device_id, sizeof(ha_device_id), "ble_", type_key);

    if (obs->has_parsed_adv && obs->adv.name_present && obs->adv.name[0] != '\0') {
        size_t len = strlen(obs->adv.name);
        bool printable = true;

        for (size_t i = 0u; i < len; ++i) {
            if (obs->adv.name[i] < 0x20u || obs->adv.name[i] > 0x7Eu) {
                printable = false;
                break;
            }
        }
        if (printable && len < sizeof(name)) {
            memcpy(name, obs->adv.name, len + 1u);
        } else {
            (void)strlcpy(name, "Unknown BLE Device", sizeof(name));
        }
    } else {
        (void)strlcpy(name, "Unknown BLE Device", sizeof(name));
    }

    memset(&identifier, 0, sizeof(identifier));
    (void)strlcpy(identifier.domain, "ble_addr", sizeof(identifier.domain));
    (void)strlcpy(identifier.value, type_key, sizeof(identifier.value));

    memset(&connection, 0, sizeof(connection));
    (void)strlcpy(connection.type, "bluetooth", sizeof(connection.type));
    (void)strlcpy(connection.value, type_key, sizeof(connection.value));

    slot = device_upsert(device_id, ha_device_id, name, "", "", "ble",
                         APP_SOURCE_BLE, true, &identifier, &connection,
                         obs->last_seen_ms, &created, out_truncated);
    if (slot == NULL) {
        return NULL;
    }

    slot->value.has_signal = true;
    slot->value.signal_dbm = obs->rssi_last;

    entity_upsert_signal(slot, out_truncated);
    entity_upsert_last_seen(slot, out_truncated);

    /* Only exact protocol facts become entities. TX power is a real advertised
     * field, so it is exposed; a device type is not invented from the address. */
    if (obs->has_parsed_adv && obs->adv.tx_power_present) {
        char value[HA_CORE_STATE_LEN];
        (void)snprintf(value, sizeof(value), "%d", (int)obs->adv.tx_power_dbm);
        entity_upsert(slot, HA_DOMAIN_SENSOR, "tx_power", "TX power", "",
                      "dBm", value, out_truncated);
    }

    return slot;
}

static device_slot_t *materialize_lan(const app_scan_evidence_t *ev,
                                      size_t index,
                                      bool *out_truncated)
{
    const app_scan_lan_t *obs = &ev->lan[index];
    char key[24];
    char device_id[HA_CORE_ID_LEN];
    char ha_device_id[HA_CORE_ID_LEN];
    ha_identifier_t identifier;
    ha_connection_t connection;
    device_slot_t *slot;
    bool created = false;
    const char *name;

    format_ip_key(key, sizeof(key), obs->ipv4);
    format_device_id(device_id, sizeof(device_id), "lan_", key);
    format_device_id(ha_device_id, sizeof(ha_device_id), "lan_", key);

    name = obs->hostname[0] != '\0' ? obs->hostname : obs->ipv4;

    memset(&identifier, 0, sizeof(identifier));
    (void)strlcpy(identifier.domain, "lan_ip", sizeof(identifier.domain));
    (void)strlcpy(identifier.value, key, sizeof(identifier.value));

    memset(&connection, 0, sizeof(connection));
    (void)strlcpy(connection.type, "ip", sizeof(connection.type));
    (void)strlcpy(connection.value, obs->ipv4, sizeof(connection.value));

    slot = device_upsert(device_id, ha_device_id, name, "", "", "lan",
                         APP_SOURCE_LAN, true, &identifier, &connection,
                         obs->last_seen_ms, &created, out_truncated);
    if (slot == NULL) {
        return NULL;
    }

    entity_upsert_last_seen(slot, out_truncated);

    if (obs->service_count > 0u) {
        char value[HA_CORE_STATE_LEN];
        (void)snprintf(value, sizeof(value), "%u", (unsigned)obs->service_count);
        entity_upsert(slot, HA_DOMAIN_SENSOR, "service_count", "Services",
                      "", "", value, out_truncated);
    }

    return slot;
}

size_t app_device_materialize(const app_scan_evidence_t *ev, bool *truncated)
{
    size_t materialized = 0u;
    bool local_truncated = false;

    if (truncated != NULL) {
        *truncated = false;
    }
    if (ev == NULL) {
        return 0u;
    }

    for (size_t i = 0u; i < ev->wifi_count; ++i) {
        if (materialize_wifi(ev, i, &local_truncated) != NULL) {
            materialized++;
        }
    }
    for (size_t i = 0u; i < ev->ble_count; ++i) {
        if (materialize_ble(ev, i, &local_truncated) != NULL) {
            materialized++;
        }
    }
    for (size_t i = 0u; i < ev->lan_count; ++i) {
        if (materialize_lan(ev, i, &local_truncated) != NULL) {
            materialized++;
        }
    }

    if (app_scan_evidence_truncated(ev)) {
        local_truncated = true;
    }

    if (truncated != NULL) {
        *truncated = local_truncated;
    }
    return materialized;
}
