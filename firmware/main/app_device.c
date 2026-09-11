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
#include "app_device_db.h"
#include "app_recognizer.h"
#include "app_str.h"

#ifdef APP_DEVICE_TEST_HOOKS
#include "app_device_test_hooks.h"
#endif

#include <stdio.h>
#include <string.h>

/* ---------------- table storage ---------------- */

typedef struct {
    app_device_binding_t value;
    bool in_use;
    /*
     * Per-source sighting flags for the current generation.
     *
     * Deliberately one flag per source rather than a single "seen" bit. A device
     * can be observed by Wi-Fi and BLE; if BLE reports it and Wi-Fi does not, the
     * device is still online and only the BLE side is fresh. Collapsing this into
     * one flag would let one source's silence hide another source's evidence.
     */
    bool seen_wifi;
    bool seen_ble;
    bool seen_lan;

    /*
     * Recognition bookkeeping.
     *
     * `profile_id` is remembered so a later scan can tell whether the matched
     * profile changed, and `recognition_applied` so the first successful match can
     * be distinguished from a repeat. Without them a Device would either have its
     * entities rebuilt on every scan (churning the entity table) or never adopt a
     * later, better match at all.
     */
    uint32_t profile_id;
    bool recognition_applied;
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
/* Defined below; needed by the generation sweep to keep Entity availability in
 * step with the binding's. */
static void sync_device_availability(device_slot_t *slot);
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
        (void)app_strlcpy(out_prev_id, victim->value.device_id, out_prev_id_size);
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

/*
 * Push a device's availability into ha_core, where the Entity and its State live.
 *
 * Without this the three views disagree: the binding table says STALE, the Entity
 * still says available, and a GUI reading either would show a different truth
 * depending on which one it happened to read. The rule is stated once here:
 *
 *   - ONLINE      -> entity available
 *   - STALE       -> entity UNAVAILABLE. The entity keeps its identity, its name,
 *                    its unit and its last state; only its availability changes.
 *                    Deleting it would lose exactly what a returning device needs
 *                    and would renumber the entity table on every RF hiccup.
 *   - UNAVAILABLE -> entity UNAVAILABLE, for the same reason.
 *   - UNKNOWN     -> left alone; nothing has been observed yet.
 *
 * `available == false` is also what a future control path must check before
 * dispatching, so this is the single place that makes "the device is not there"
 * visible to every consumer.
 */
static void entity_set_available(const char *entity_id, bool available)
{
    const ha_entity_t *current = ha_core_entity_get(entity_id);

    if (current == NULL) {
        return;
    }
    if (current->available == available) {
        return;
    }
    {
        ha_entity_t updated = *current;

        updated.available = available;
        (void)ha_core_entity_upsert(&updated);
    }
}

static void sync_device_availability(device_slot_t *slot)
{
    const bool available = slot->value.availability == APP_AVAILABILITY_ONLINE;

    if (slot->value.availability == APP_AVAILABILITY_UNKNOWN) {
        return;
    }
    for (size_t i = 0u; i < APP_ENTITY_MAX; ++i) {
        if (!s_entities[i].in_use) {
            continue;
        }
        if (strcmp(s_entities[i].value.device_id, slot->value.device_id) != 0) {
            continue;
        }
        entity_set_available(s_entities[i].value.entity_id, available);
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

uint32_t app_device_swept_count(void)
{
    return s_swept;
}

/* ---------------- generation lifecycle ---------------- */

void app_device_generation_begin(uint32_t generation)
{
    s_generation = generation;
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        if (s_devices[i].in_use) {
            s_devices[i].seen_wifi = false;
            s_devices[i].seen_ble = false;
            s_devices[i].seen_lan = false;
        }
    }
}

bool app_scan_stage_was_observed(const app_scan_status_t *scan,
                                 app_scan_stage_t stage)
{
    if (scan == NULL) {
        return false;
    }
    if ((int)stage < 0 || (int)stage >= (int)APP_STAGE_COUNT) {
        return false;
    }
    /* "Observed" means the stage covered its protocol well enough that a missing
     * device is evidence. That is stricter than "the stage reported success":
     * PARTIAL does not qualify, because a partially covered protocol may simply
     * have missed the device. See app_scan_state_covers_protocol(). */
    return app_scan_state_covers_protocol(scan->states[stage]);
}

/*
 * Was every protocol that has observed this device covered well enough this
 * generation that its absence is meaningful evidence?
 *
 * Both conditions are required:
 *
 *   1. NO source may have been observed without reporting the device. A source
 *      that ran and did not see it is direct evidence that it was not there.
 *   2. EVERY source must have fully covered its protocol. A source whose stage
 *      was skipped, cancelled, failed or merely PARTIAL may simply have missed
 *      the device, so its silence proves nothing. PARTIAL is deliberately not
 *      treated as coverage: "the stage finished" is not "we would have seen it".
 *
 * A device with no recorded sources is never swept.
 */
static bool device_all_sources_fully_covered(const device_slot_t *slot,
                                             const app_scan_status_t *scan)
{
    bool any = false;

    if (scan == NULL || slot->value.sources == 0u) {
        return false;
    }

    if ((slot->value.sources & APP_SOURCE_WIFI) != 0u) {
        any = true;
        if (slot->seen_wifi ||
            !app_scan_state_covers_protocol(scan->states[APP_STAGE_WIFI_RF])) {
            return false;
        }
    }
    if ((slot->value.sources & APP_SOURCE_BLE) != 0u) {
        any = true;
        if (slot->seen_ble ||
            !app_scan_state_covers_protocol(scan->states[APP_STAGE_BLE_RF])) {
            return false;
        }
    }
    if ((slot->value.sources & APP_SOURCE_LAN) != 0u) {
        /* LAN evidence can arrive from any of three stages, so the device is only
         * accounted for when none of them saw it and at least one of them fully
         * covered its protocol. */
        any = true;
        if (slot->seen_lan ||
            !(app_scan_state_covers_protocol(scan->states[APP_STAGE_MDNS]) ||
              app_scan_state_covers_protocol(scan->states[APP_STAGE_SSDP]) ||
              app_scan_state_covers_protocol(scan->states[APP_STAGE_LAN_HOSTS]))) {
            return false;
        }
    }

    return any;
}

void app_device_generation_finish(const app_scan_status_t *scan)
{
    for (size_t i = 0u; i < APP_DEVICE_MAX; ++i) {
        device_slot_t *slot = &s_devices[i];
        bool seen_by_any;

        if (!slot->in_use) {
            continue;
        }

        /* Availability is per device and derived from per-source freshness: if
         * any source still reports it, the device is online even when another
         * source has gone quiet. */
        seen_by_any = slot->seen_wifi || slot->seen_ble || slot->seen_lan;
        if (seen_by_any) {
            slot->value.availability = APP_AVAILABILITY_ONLINE;
            slot->value.last_generation = s_generation;
            slot->value.miss_rounds = 0u;
            continue;
        }

        /*
         * Not observed by any source this generation.
         *
         * `fully_covered` is the whole question: it is true only when EVERY
         * protocol that has ever observed this device covered its protocol
         * completely this generation. A skipped, failed, cancelled or merely
         * partial stage may simply have missed the device, so its silence proves
         * nothing and must not be read as a departure.
         */
        if (!device_all_sources_fully_covered(slot, scan)) {
            /* "We could not look" - weaker than "we looked and it was gone". The
             * device keeps its identity, its recognition and its entities, and is
             * reported as unavailable. A persistent authorized identity lives here
             * permanently when it is out of range, which is what stops an outage
             * from erasing a commissioning it once held. */
            slot->value.availability = APP_AVAILABILITY_UNAVAILABLE;
            slot->value.miss_rounds = 0u;
            sync_device_availability(slot);
            continue;
        }

        /*
         * Absence is real evidence now. It is still not proof: a single RF report
         * can be lost for reasons that have nothing to do with the device leaving,
         * so an ephemeral device is kept for APP_DEVICE_MISS_ROUNDS_BEFORE_EVICT
         * consecutive fully covered misses before it is removed.
         *
         * The wait is deliberately spent on a visible state rather than on
         * silence: the device is shown as stale, so an operator sees "we are no
         * longer hearing it" one round before it disappears, instead of seeing it
         * vanish with no warning.
         */
        if (slot->value.miss_rounds < 0xFFu) {
            slot->value.miss_rounds++;
        }
        slot->value.availability = APP_AVAILABILITY_STALE;

        if (slot->value.ephemeral &&
            slot->value.miss_rounds >= APP_DEVICE_MISS_ROUNDS_BEFORE_EVICT) {
            entity_slot_free_for_device(slot->value.device_id);
            (void)ha_core_device_remove(slot->value.ha_device_id);
            memset(slot, 0, sizeof(*slot));
            s_swept++;
        } else {
            /* Kept. The entity set is retained too: it is the device's identity,
             * and a returning device must not have to be rediscovered from
             * scratch. */
            sync_device_availability(slot);
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

/*
 * Identity keys, exposed so enrichment and materialisation agree by construction.
 *
 * The key is namespaced per protocol and includes everything that makes two
 * observations different devices: a BLE address type is part of the identity, and
 * the same bytes seen in Wi-Fi, BLE and LAN are three keys, never one. Both the
 * device table and the recognition table are keyed by exactly these functions, so
 * a recognition result can only ever be applied to the observation it came from.
 */
size_t app_device_identity_of_wifi(const app_scan_wifi_t *obs, char *out,
                                   size_t out_size)
{
    char mac[16];

    if (obs == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }
    format_mac(mac, sizeof(mac), obs->bssid);
    format_device_id(out, out_size, "wifi_", mac);
    return strlen(out);
}

size_t app_device_identity_of_ble(const app_scan_ble_t *obs, char *out,
                                  size_t out_size)
{
    char mac[16];
    char type_key[24];

    if (obs == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }
    format_mac(mac, sizeof(mac), obs->address);
    (void)snprintf(type_key, sizeof(type_key), "%02x%s", obs->address_type, mac);
    format_device_id(out, out_size, "ble_", type_key);
    return strlen(out);
}

size_t app_device_identity_of_lan(const app_scan_lan_t *obs, char *out,
                                  size_t out_size)
{
    char key[24];

    if (obs == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }
    format_ip_key(key, sizeof(key), obs->ipv4);
    format_device_id(out, out_size, "lan_", key);
    return strlen(out);
}

/* ---------------- recognition table ---------------- */

/* Identity of whichever observation `sources` names. Exactly one source bit is
 * expected; a multi-source observation takes the first present source, matching
 * the recognizer's own precedence. Returns false when nothing identifies it. */
static bool identity_of_observation(uint32_t sources,
                                    const app_scan_wifi_t *wifi,
                                    const app_scan_ble_t *ble,
                                    const app_scan_lan_t *lan,
                                    char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return false;
    }
    out[0] = '\0';

    if ((sources & APP_SOURCE_BLE) != 0u && ble != NULL) {
        return app_device_identity_of_ble(ble, out, out_size) > 0u;
    }
    if ((sources & APP_SOURCE_WIFI) != 0u && wifi != NULL) {
        return app_device_identity_of_wifi(wifi, out, out_size) > 0u;
    }
    if ((sources & APP_SOURCE_LAN) != 0u && lan != NULL) {
        return app_device_identity_of_lan(lan, out, out_size) > 0u;
    }
    if (ble != NULL) {
        return app_device_identity_of_ble(ble, out, out_size) > 0u;
    }
    if (wifi != NULL) {
        return app_device_identity_of_wifi(wifi, out, out_size) > 0u;
    }
    if (lan != NULL) {
        return app_device_identity_of_lan(lan, out, out_size) > 0u;
    }
    return false;
}

void app_recognition_table_reset(app_recognition_table_t *table)
{
    if (table == NULL) {
        return;
    }
    memset(table, 0, sizeof(*table));
}

size_t app_recognition_table_count(const app_recognition_table_t *table)
{
    return table == NULL ? 0u : table->count;
}

bool app_recognition_table_truncated(const app_recognition_table_t *table)
{
    return table != NULL && table->truncated;
}

const app_recognition_entry_t *app_recognition_table_find(
    const app_recognition_table_t *table, const char *identity)
{
    if (table == NULL || identity == NULL || identity[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0u; i < table->count && i < APP_RECOGNITION_TABLE_MAX; ++i) {
        if (table->entries[i].state == APP_RECOGNITION_ENTRY_PRESENT &&
            strcmp(table->entries[i].identity, identity) == 0) {
            return &table->entries[i];
        }
    }
    return NULL;
}

static app_recognition_entry_t *recognition_entry_get(
    app_recognition_table_t *table, const char *identity, uint32_t sources)
{
    app_recognition_entry_t *entry;

    if (table == NULL || identity == NULL || identity[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0u; i < table->count && i < APP_RECOGNITION_TABLE_MAX; ++i) {
        if (table->entries[i].state == APP_RECOGNITION_ENTRY_PRESENT &&
            strcmp(table->entries[i].identity, identity) == 0) {
            return &table->entries[i];
        }
    }
    if (table->count >= APP_RECOGNITION_TABLE_MAX) {
        /*
         * The table is full, so this observation gets no entry at all - and the
         * caller must report a partial scan, because a device with no entry stays
         * generic even when the corpus would have recognised it.
         *
         * The existing entries are deliberately NOT blanked. Blanking them would
         * turn a capacity problem into "recognition unavailable" for devices that
         * were in fact recognised, which is a false statement about the database;
         * leaving them is a false statement about coverage, which the truncation
         * flag discloses. Understating requires no correction, misreporting does.
         */
        table->truncated = true;
        return NULL;
    }
    entry = &table->entries[table->count++];
    memset(entry, 0, sizeof(*entry));
    entry->state = APP_RECOGNITION_ENTRY_PRESENT;
    entry->sources = sources;
    (void)app_strlcpy(entry->identity, identity, sizeof(entry->identity));
    return entry;
}

/*
 * True when an entry was just refused for capacity.
 *
 * Tracked as a public flag on the table plus this helper so the enrich loop can
 * stop asking, while the flag itself stays the single statement of "the result is
 * incomplete" that the caller reports.
 */
static bool recognition_table_saturated(const app_recognition_table_t *table)
{
    return table != NULL && table->truncated;
}

/*
 * Record one observation's recognition outcome.
 *
 * The result is normalised here into the three states the device layer acts on,
 * so the device layer never has to re-derive them:
 *
 *   - the recognizer could not run (no recognizer, no usable database) ->
 *     `attempted == false`. The device is reported as "recognition unavailable",
 *     which is a different statement from "nothing matched";
 *   - ambiguous -> recorded as ambiguous and left without recipes, so nothing
 *     writable can be built from it;
 *   - matched -> the recipes the reader accepted, already filtered for backend
 *     drivability at the database boundary.
 */
static bool recognition_record(app_recognition_table_t *table,
                               const app_recognizer_ref_t *recognizer,
                               uint32_t sources, const char *identity,
                               const app_scan_wifi_t *wifi,
                               const app_scan_ble_t *ble,
                               const app_scan_lan_t *lan)
{
    app_recognition_entry_t *entry = recognition_entry_get(table, identity, sources);
    app_recognition_result_t result;

    if (entry == NULL) {
        return false;
    }
    if (recognizer == NULL || recognizer->ops == NULL ||
        recognizer->ops->recognize == NULL) {
        entry->attempted = false;
        entry->db_state = APP_DB_STATE_CLOSED;
        return true;
    }

    memset(&result, 0, sizeof(result));
    entry->db_state = recognizer->ops->state != NULL
                          ? recognizer->ops->state(recognizer->ctx)
                          : APP_DB_STATE_CLOSED;
    if (!recognizer->ops->recognize(recognizer->ctx, sources, wifi, ble, lan,
                                    &result)) {
        entry->attempted = false;
        return true;
    }

    entry->attempted = true;
    if (result.ambiguous) {
        /*
         * Ambiguity is carried through but never as a match: no recipe, no
         * display metadata, nothing writable. The ids are still recorded for
         * diagnostics - they say which families the corpus named - but nothing
         * acts on them.
         */
        entry->ambiguous = true;
        entry->theengs_decoder_id = DEVICE_DB_NO_INDEX;
        entry->zha_quirk_id = DEVICE_DB_NO_INDEX;
        entry->backend_name = "none";
        return true;
    }

    entry->matched = result.matched;
    entry->profile_id = result.profile_id;
    entry->theengs_decoder_id = result.theengs_decoder_id;
    entry->zha_quirk_id = result.zha_quirk_id;
    entry->backend_supported = result.backend_supported;
    entry->backend_name = result.backend_name;
    entry->recipe_count = result.recipe_count;
    (void)app_strlcpy(entry->display_name, result.display_name,
                      sizeof(entry->display_name));
    (void)app_strlcpy(entry->vendor, result.vendor, sizeof(entry->vendor));
    (void)app_strlcpy(entry->model, result.model, sizeof(entry->model));
    if (result.recipe_count > APP_RECOGNITION_MAX_RECIPES) {
        entry->recipe_count = APP_RECOGNITION_MAX_RECIPES;
    }
    for (uint8_t i = 0u; i < entry->recipe_count; ++i) {
        entry->recipes[i] = result.recipes[i];
    }
    return true;
}

size_t app_recognition_enrich(const app_scan_evidence_t *ev,
                              const app_recognizer_ref_t *recognizer,
                              app_recognition_table_t *table)
{
    size_t recorded = 0u;

    if (table == NULL) {
        return 0u;
    }
    app_recognition_table_reset(table);
    if (ev == NULL) {
        return 0u;
    }

    /*
     * Every observation is attempted, and `recorded` counts attempts rather than
     * stored entries: an observation past the table's capacity was still tried,
     * and the difference between "we tried and could not store it" and "we never
     * looked" is what app_recognition_table_truncated() reports. A caller that
     * conflated the two would either hide a capacity problem or claim recognition
     * failed when the database was never consulted.
     *
     * The loops stop early once the table is saturated, because nothing further
     * can be stored and continuing would only burn read I/O on the card.
     */
    for (size_t i = 0u; i < ev->wifi_count; ++i) {
        char identity[HA_CORE_ID_LEN];

        if (app_device_identity_of_wifi(&ev->wifi[i], identity,
                                        sizeof(identity)) == 0u) {
            continue;
        }
        recorded++;
        if (!recognition_record(table, recognizer, APP_SOURCE_WIFI, identity,
                                &ev->wifi[i], NULL, NULL)) {
            break;
        }
    }
    if (!recognition_table_saturated(table)) {
        for (size_t i = 0u; i < ev->ble_count; ++i) {
            char identity[HA_CORE_ID_LEN];

            if (app_device_identity_of_ble(&ev->ble[i], identity,
                                           sizeof(identity)) == 0u) {
                continue;
            }
            recorded++;
            if (!recognition_record(table, recognizer, APP_SOURCE_BLE, identity,
                                    NULL, &ev->ble[i], NULL)) {
                break;
            }
        }
    }
    if (!recognition_table_saturated(table)) {
        for (size_t i = 0u; i < ev->lan_count; ++i) {
            char identity[HA_CORE_ID_LEN];

            if (app_device_identity_of_lan(&ev->lan[i], identity,
                                           sizeof(identity)) == 0u) {
                continue;
            }
            recorded++;
            if (!recognition_record(table, recognizer, APP_SOURCE_LAN, identity,
                                    NULL, NULL, &ev->lan[i])) {
                break;
            }
        }
    }
    return recorded;
}

/*
 * Recognizer view over an enriched table.
 *
 * The kernel the device table talks to. It never touches the database: it copies
 * the stored result for the identity it is asked about, which is what makes
 * "recognition happened in the enrichment stage" true by construction rather than
 * by convention, and keeps the device table independent of SD.
 */
static bool table_recognizer_recognize(void *ctx, uint32_t sources,
                                       const app_scan_wifi_t *wifi,
                                       const app_scan_ble_t *ble,
                                       const app_scan_lan_t *lan,
                                       app_recognition_result_t *out)
{
    app_recognition_table_t *table = (app_recognition_table_t *)ctx;
    const app_recognition_entry_t *entry = NULL;
    char identity[HA_CORE_ID_LEN];

    if (out == NULL) {
        return false;
    }

    /* One source bit identifies the observation, so the same helper the device
     * table uses builds the key here too. */
    if (identity_of_observation(sources, wifi, ble, lan, identity,
                                sizeof(identity))) {
        entry = app_recognition_table_find(table, identity);
    }

    if (entry == NULL || !entry->attempted) {
        /*
         * No outcome was recorded for this observation, so recognition could not
         * run. Reported as such rather than as "not matched": the device stays
         * generic and the report says recognition was unavailable, which is a
         * different statement from "we looked and found nothing".
         */
        memset(out, 0, sizeof(*out));
        out->theengs_decoder_id = DEVICE_DB_NO_INDEX;
        out->zha_quirk_id = DEVICE_DB_NO_INDEX;
        out->backend_name = "none";
        return false;
    }

    /* Rebuild the result the matcher produced from the stored entry. */
    memset(out, 0, sizeof(*out));
    out->matched = entry->matched;
    out->ambiguous = entry->ambiguous;
    out->profile_id = entry->profile_id;
    out->theengs_decoder_id = entry->theengs_decoder_id;
    out->zha_quirk_id = entry->zha_quirk_id;
    out->backend_supported = entry->backend_supported;
    out->backend_name = entry->backend_name != NULL ? entry->backend_name : "none";
    out->recipe_count = entry->recipe_count;
    (void)app_strlcpy(out->display_name, entry->display_name,
                      sizeof(out->display_name));
    (void)app_strlcpy(out->vendor, entry->vendor, sizeof(out->vendor));
    (void)app_strlcpy(out->model, entry->model, sizeof(out->model));
    for (uint8_t i = 0u; i < entry->recipe_count && i < APP_RECOGNITION_MAX_RECIPES;
         ++i) {
        out->recipes[i] = entry->recipes[i];
    }
    return true;
}

static app_db_state_t table_recognizer_state(void *ctx)
{
    const app_recognition_table_t *table = (const app_recognition_table_t *)ctx;

    if (table == NULL) {
        return APP_DB_STATE_CLOSED;
    }
    for (size_t i = 0u; i < table->count && i < APP_RECOGNITION_TABLE_MAX; ++i) {
        if (table->entries[i].state == APP_RECOGNITION_ENTRY_PRESENT) {
            /* The state the database itself reported when this entry was decided.
             * Returning READY/CLOSED from the `attempted` flag would lose the
             * reason: "the card is missing" and "the corpus is corrupt" are
             * different problems and Settings has to show which one it is. */
            return table->entries[i].attempted ? APP_DB_STATE_READY
                                               : table->entries[i].db_state;
        }
    }
    return APP_DB_STATE_CLOSED;
}

static void table_recognizer_describe(void *ctx, char *out, size_t out_size)
{
    const app_recognition_table_t *table = (const app_recognition_table_t *)ctx;

    if (out == NULL || out_size == 0u) {
        return;
    }
    if (table == NULL) {
        (void)snprintf(out, out_size, "closed");
        return;
    }
    (void)snprintf(out, out_size, "enriched entries=%lu%s",
                   (unsigned long)app_recognition_table_count(table),
                   table->truncated ? " truncated" : "");
}

static const app_recognizer_ops_t s_table_ops = {
    .recognize = table_recognizer_recognize,
    .state = table_recognizer_state,
    .describe = table_recognizer_describe,
};

app_recognizer_ref_t app_recognition_table_recognizer(app_recognition_table_t *table)
{
    app_recognizer_ref_t ref;

    ref.ops = &s_table_ops;
    ref.ctx = table;
    return ref;
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

    /* ha_core is authoritative. Build and insert there FIRST: claiming an
     * application binding before a successful insert would leave a binding with
     * no matching ha_core Device whenever the insert is rejected (capacity or an
     * identity conflict), which the enumeration would then report as a device
     * that does not exist. */
    memset(&device, 0, sizeof(device));
    (void)app_strlcpy(device.id, ha_device_id, sizeof(device.id));
    (void)app_strlcpy(device.name, name, sizeof(device.name));
    (void)app_strlcpy(device.manufacturer, manufacturer, sizeof(device.manufacturer));
    (void)app_strlcpy(device.model, model, sizeof(device.model));
    (void)app_strlcpy(device.model_id, protocol_label, sizeof(device.model_id));

    if (identifier != NULL) {
        device.identifiers[0] = *identifier;
        device.identifier_count = 1u;
    }
    if (connection != NULL) {
        device.connections[0] = *connection;
        device.connection_count = 1u;
    }

    status = ha_core_device_upsert(&device);
    if (status != HA_CORE_OK) {
        /* Capacity or conflict: report truncation and do not create a binding. */
        if (out_truncated != NULL) {
            *out_truncated = true;
        }
        return NULL;
    }

    if (slot == NULL) {
        char evicted_app_id[HA_CORE_ID_LEN];
        char evicted_ha_id[HA_CORE_ID_LEN];

        slot = device_slot_alloc();
        if (slot == NULL) {
            /* Table full. Evict the oldest ephemeral binding, but take its
             * ha_core Device and Entities with it: leaving them behind would
             * leave orphaned state that the UI could still address. */
            device_slot_t *victim =
                device_slot_evict_ephemeral(evicted_app_id, sizeof(evicted_app_id));
            if (victim == NULL) {
                if (out_truncated != NULL) {
                    *out_truncated = true;
                }
                return NULL;
            }
            /* The victim slot was cleared, so recover its ha_core id from the
             * application id. They are currently identical by construction, but
             * this stays correct if the two ever diverge. */
            (void)app_strlcpy(evicted_ha_id, evicted_app_id, sizeof(evicted_ha_id));
            entity_slot_free_for_device(evicted_app_id);
            (void)ha_core_device_remove(evicted_ha_id);
            slot = victim;
            if (out_truncated != NULL) {
                *out_truncated = true;
            }
        }
        (void)app_strlcpy(slot->value.device_id, device_id, sizeof(slot->value.device_id));
        (void)app_strlcpy(slot->value.ha_device_id, ha_device_id,
                      sizeof(slot->value.ha_device_id));
        (void)app_strlcpy(slot->value.protocol_label, protocol_label,
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
    /* Record freshness for the source that actually reported it, so one source's
     * later silence cannot erase another source's evidence. */
    if ((source_bit & APP_SOURCE_WIFI) != 0u) {
        slot->seen_wifi = true;
    }
    if ((source_bit & APP_SOURCE_BLE) != 0u) {
        slot->seen_ble = true;
    }
    if ((source_bit & APP_SOURCE_LAN) != 0u) {
        slot->seen_lan = true;
    }
    slot->value.availability = APP_AVAILABILITY_ONLINE;
    /* Seen by any source cancels the eviction countdown. Without this a device
     * that flickers in and out would accumulate misses across rounds and be
     * removed while it is still present. */
    slot->value.miss_rounds = 0u;
    if (seen_ms >= slot->value.last_seen_ms) {
        slot->value.last_seen_ms = seen_ms;
    }
    if (slot->value.first_seen_ms == 0u || (seen_ms != 0u && seen_ms < slot->value.first_seen_ms)) {
        slot->value.first_seen_ms = seen_ms;
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
    (void)app_strlcpy(entity.entity_id, entity_id, sizeof(entity.entity_id));
    (void)app_strlcpy(entity.unique_id, unique_id, sizeof(entity.unique_id));
    (void)app_strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)app_strlcpy(entity.domain, domain, sizeof(entity.domain));
    (void)app_strlcpy(entity.device_id, device->value.ha_device_id, sizeof(entity.device_id));
    (void)app_strlcpy(entity.name, name, sizeof(entity.name));
    (void)app_strlcpy(entity.device_class, device_class, sizeof(entity.device_class));
    (void)app_strlcpy(entity.unit_of_measurement, unit, sizeof(entity.unit_of_measurement));
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
        (void)app_strlcpy(slot->value.entity_id, entity_id, sizeof(slot->value.entity_id));
        (void)app_strlcpy(slot->value.device_id, device->value.device_id,
                      sizeof(slot->value.device_id));
        (void)app_strlcpy(slot->value.domain, domain, sizeof(slot->value.domain));
        (void)app_strlcpy(slot->value.name, name, sizeof(slot->value.name));
        (void)app_strlcpy(slot->value.unit, unit, sizeof(slot->value.unit));
        /* No writable entity exists yet: every binding is read-only until a
         * protocol profile with a verified control path is available. */
        slot->value.writable = false;
        slot->value.backend = APP_ENTITY_BACKEND_NONE;
        slot->value.backend_name = "none";
        slot->value.write_target_id = DEVICE_DB_NO_INDEX;
        slot->value.has_range = false;
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

/*
 * Create one Entity from a recognition recipe.
 *
 * `writable` is decided by the caller after checking that the recipe names a
 * backend this firmware can actually drive, so an entity is only ever writable
 * when a real control path exists. A read-only recipe still produces the entity:
 * the device publishes a value we can read.
 */
static void entity_upsert_recipe(device_slot_t *device,
                                 const app_entity_recipe_t *recipe,
                                 bool writable,
                                 bool *out_truncated)
{
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char unique_id[HA_CORE_UNIQUE_ID_LEN];
    char object_id[HA_CORE_NAME_LEN];
    entity_slot_t *slot;
    ha_entity_t entity;
    ha_core_status_t status;

    /* Stable, slug-safe entity id: the recipe name if usable, else the domain and
     * read source. Names come from the database, so they may contain anything. */
    (void)snprintf(object_id, sizeof(object_id), "%s", recipe->name[0] != '\0'
                                                         ? recipe->name
                                                         : "value");
    for (size_t i = 0u; object_id[i] != '\0'; ++i) {
        char c = object_id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (c >= 'A' && c <= 'Z') {
            object_id[i] = (char)(c - 'A' + 'a');
            continue;
        }
        if (!ok) {
            object_id[i] = '_';
        }
    }
    if (object_id[0] == '\0') {
        (void)app_strlcpy(object_id, "value", sizeof(object_id));
    }

    /*
     * Assembled explicitly rather than with one snprintf.
     *
     * The entity id is "<domain>.<device>_<object>"; every part is already bounded,
     * so appending them in order and checking each step proves the result fits
     * instead of hoping a single format call truncates safely. A truncated id
     * would be a bug that silently aliases two entities.
     */
    {
        size_t used;

        (void)app_strlcpy(entity_id, recipe->domain, sizeof(entity_id));
        used = strlen(entity_id);
        if (used + 1u >= sizeof(entity_id)) {
            if (out_truncated != NULL) {
                *out_truncated = true;
            }
            return;
        }
        entity_id[used++] = '.';
        (void)app_strlcpy(entity_id + used, device->value.device_id,
                          sizeof(entity_id) - used);
        used = strlen(entity_id);
        if (used + 1u >= sizeof(entity_id)) {
            if (out_truncated != NULL) {
                *out_truncated = true;
            }
            return;
        }
        entity_id[used++] = '_';
        (void)app_strlcpy(entity_id + used, object_id, sizeof(entity_id) - used);

        (void)app_strlcpy(unique_id, device->value.device_id, sizeof(unique_id));
        used = strlen(unique_id);
        if (used + 1u >= sizeof(unique_id)) {
            if (out_truncated != NULL) {
                *out_truncated = true;
            }
            return;
        }
        unique_id[used++] = '_';
        (void)app_strlcpy(unique_id + used, object_id, sizeof(unique_id) - used);
    }

    memset(&entity, 0, sizeof(entity));
    (void)app_strlcpy(entity.entity_id, entity_id, sizeof(entity.entity_id));
    (void)app_strlcpy(entity.unique_id, unique_id, sizeof(entity.unique_id));
    (void)app_strlcpy(entity.platform, "nearby", sizeof(entity.platform));
    (void)app_strlcpy(entity.domain, recipe->domain, sizeof(entity.domain));
    (void)app_strlcpy(entity.device_id, device->value.ha_device_id,
                      sizeof(entity.device_id));
    (void)app_strlcpy(entity.name, recipe->name, sizeof(entity.name));
    (void)app_strlcpy(entity.device_class, recipe->device_class,
                      sizeof(entity.device_class));
    (void)app_strlcpy(entity.unit_of_measurement, recipe->unit,
                      sizeof(entity.unit_of_measurement));
    entity.has_entity_name = true;
    entity.enabled = true;
    entity.available = true;

    if (writable) {
        /* A drivable write path exists. The service mask still has to be one HA
         * actually defines, so the dispatcher cannot be handed an unknown name. */
        entity.supported_services = HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF;
        entity.service_handler = NULL; /* bound by the control task in B10 */
        entity.service_context = NULL;
    } else {
        entity.supported_services = 0u;
        entity.service_handler = NULL;
        entity.service_context = NULL;
    }

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
        (void)app_strlcpy(slot->value.entity_id, entity_id, sizeof(slot->value.entity_id));
        (void)app_strlcpy(slot->value.device_id, device->value.device_id,
                          sizeof(slot->value.device_id));
        (void)app_strlcpy(slot->value.domain, recipe->domain, sizeof(slot->value.domain));
        (void)app_strlcpy(slot->value.name, recipe->name, sizeof(slot->value.name));
        (void)app_strlcpy(slot->value.unit, recipe->unit, sizeof(slot->value.unit));
    }
    slot->value.writable = writable;
    /*
     * The control binding travels with the entity, resolved once here rather than
     * re-derived by the control loop.
     *
     * A read-only entity reports no backend even when its recipe names one: `writable`
     * is already the decision that the recipe, the profile's declaration and this
     * firmware's drivable set all agreed, and the control loop must not be able to
     * arrive at a different answer by re-reading the same facts.
     */
    if (writable) {
        slot->value.backend = recipe->backend;
        slot->value.backend_name = app_backend_name(recipe->backend);
        slot->value.write_target_id = recipe->write_target_id;
        slot->value.has_range = true;
        slot->value.min_value = recipe->min_value;
        slot->value.max_value = recipe->max_value;
        slot->value.scale = recipe->scale;
    } else {
        slot->value.backend = APP_ENTITY_BACKEND_NONE;
        slot->value.backend_name = "none";
        slot->value.write_target_id = DEVICE_DB_NO_INDEX;
        slot->value.has_range = false;
    }
}

/* ---------------- recognition ---------------- */

/*
 * Recognise one observation and apply the result to its Device.
 *
 * The rules, in one place:
 *
 *   - a NULL recognizer, or a database that cannot be used, means recognition is
 *     UNAVAILABLE: the Device stays generic and says why, rather than vanishing
 *     or claiming to be unknown;
 *   - unmatched or ambiguous keeps the Device generic and read-only. Ambiguity is
 *     never permission to guess;
 *   - only a deterministic match attaches profile entities, and a writable binding
 *     is attached only when the recipe names a backend this firmware can drive. A
 *     database record saying `writable` is a claim about the device, not about our
 *     capabilities;
 *   - generic entities (signal, last seen, channel, tx power) are applied either
 *     way, so a recognised Device does not lose its observed facts.
 *
 * Entities are upserted by id, so re-materialising with the same profile refreshes
 * values instead of churning the entity table.
 */
static void apply_recognition(device_slot_t *slot,
                              const app_recognizer_ref_t *recognizer,
                              uint32_t sources,
                              const app_scan_wifi_t *wifi,
                              const app_scan_ble_t *ble,
                              const app_scan_lan_t *lan,
                              bool *out_truncated)
{
    app_recognition_result_t result;

    if (recognizer == NULL || recognizer->ops == NULL ||
        recognizer->ops->recognize == NULL) {
        slot->value.recognition = APP_RECOGNITION_DB_UNAVAILABLE;
        slot->value.read_only = true;
        slot->profile_id = 0u;
        return;
    }

    memset(&result, 0, sizeof(result));
    if (!recognizer->ops->recognize(recognizer->ctx, sources, wifi, ble, lan,
                                    &result)) {
        /* The call could not run at all: no usable database. */
        slot->value.recognition = APP_RECOGNITION_DB_UNAVAILABLE;
        slot->value.read_only = true;
        slot->profile_id = 0u;
        return;
    }

    if (result.ambiguous) {
        slot->value.recognition = APP_RECOGNITION_AMBIGUOUS;
        slot->value.read_only = true;
        slot->profile_id = 0u;
        return;
    }
    if (!result.matched) {
        slot->value.recognition = APP_RECOGNITION_UNKNOWN;
        slot->value.read_only = true;
        slot->profile_id = 0u;
        return;
    }

    slot->value.recognition = APP_RECOGNITION_MATCHED;
    slot->profile_id = result.profile_id;
    slot->recognition_applied = true;

    /*
     * Adopt recognised display metadata only when the Device has no better name of
     * its own: a name the device itself advertised beats a database label, and a
     * later stronger match may still replace a generic fallback.
     */
    if (result.display_name[0] != '\0') {
        const ha_device_t *current = ha_core_device_get(slot->value.ha_device_id);
        const bool generic_name =
            current == NULL || strcmp(current->name, "Unknown BLE Device") == 0 ||
            strcmp(current->name, "Unknown Wi-Fi Device") == 0 ||
            strcmp(current->name, "Hidden Wi-Fi AP") == 0;

        if (generic_name) {
            ha_device_t device;

            memset(&device, 0, sizeof(device));
            if (current != NULL) {
                device = *current;
            }
            (void)app_strlcpy(device.id, slot->value.ha_device_id, sizeof(device.id));
            (void)app_strlcpy(device.name, result.display_name, sizeof(device.name));
            (void)app_strlcpy(device.manufacturer, result.vendor, sizeof(device.manufacturer));
            (void)app_strlcpy(device.model, result.model, sizeof(device.model));
            (void)app_strlcpy(device.model_id, slot->value.protocol_label,
                              sizeof(device.model_id));
            if (ha_core_device_upsert(&device) != HA_CORE_OK && out_truncated != NULL) {
                *out_truncated = true;
            }
        }
    }

    slot->value.read_only = true;
    for (uint8_t i = 0u; i < result.recipe_count; ++i) {
        const app_entity_recipe_t *recipe = &result.recipes[i];
        const bool writable = recipe->write_target_id != DEVICE_DB_NO_INDEX &&
                              app_backend_is_drivable(recipe->backend);

        entity_upsert_recipe(slot, recipe, writable, out_truncated);
        if (writable) {
            slot->value.read_only = false;
        }
    }
}

/* ---------------- materialisation ---------------- */


static device_slot_t *materialize_wifi(const app_scan_evidence_t *ev,
                                       size_t index,
                                       const app_recognizer_ref_t *recognizer,
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

    /* The identity is built by the shared helper, so the device table and the
     * recognition table cannot disagree about which observation this is. */
    (void)app_device_identity_of_wifi(obs, device_id, sizeof(device_id));
    (void)app_strlcpy(ha_device_id, device_id, sizeof(ha_device_id));
    format_mac(key, sizeof(key), obs->bssid);

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
            (void)app_strlcpy(name, obs->ssid_hidden ? "Hidden Wi-Fi AP" : "Unknown Wi-Fi Device",
                          sizeof(name));
        }
    } else {
        (void)app_strlcpy(name, obs->ssid_hidden ? "Hidden Wi-Fi AP" : "Unknown Wi-Fi Device",
                      sizeof(name));
    }

    memset(&identifier, 0, sizeof(identifier));
    (void)app_strlcpy(identifier.domain, "wifi_bssid", sizeof(identifier.domain));
    (void)app_strlcpy(identifier.value, key, sizeof(identifier.value));

    memset(&connection, 0, sizeof(connection));
    (void)app_strlcpy(connection.type, "mac", sizeof(connection.type));
    (void)app_strlcpy(connection.value, key, sizeof(connection.value));

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

    apply_recognition(slot, recognizer, APP_SOURCE_WIFI, obs, NULL, NULL,
                      out_truncated);
    return slot;
}

static device_slot_t *materialize_ble(const app_scan_evidence_t *ev,
                                       size_t index,
                                       const app_recognizer_ref_t *recognizer,
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
     * type are a different peer. The shared helper encodes that rule, so the
     * recognition table keys BLE observations identically. */
    (void)app_device_identity_of_ble(obs, device_id, sizeof(device_id));
    (void)app_strlcpy(ha_device_id, device_id, sizeof(ha_device_id));
    (void)snprintf(type_key, sizeof(type_key), "%02x%s", obs->address_type, key);

    if (obs->has_parsed_adv && obs->adv.name_present && obs->adv.name[0] != '\0') {
        size_t len = strlen(obs->adv.name);
        bool printable = true;

        for (size_t i = 0u; i < len; ++i) {
            /* Cast to unsigned char: a plain char is signed on some targets and
             * would promote to a negative int for bytes >= 0x80. */
            unsigned char c = (unsigned char)obs->adv.name[i];
            if (c < 0x20u || c > 0x7Eu) {
                printable = false;
                break;
            }
        }
        if (printable && len < sizeof(name)) {
            memcpy(name, obs->adv.name, len + 1u);
        } else {
            (void)app_strlcpy(name, "Unknown BLE Device", sizeof(name));
        }
    } else {
        (void)app_strlcpy(name, "Unknown BLE Device", sizeof(name));
    }

    memset(&identifier, 0, sizeof(identifier));
    (void)app_strlcpy(identifier.domain, "ble_addr", sizeof(identifier.domain));
    (void)app_strlcpy(identifier.value, type_key, sizeof(identifier.value));

    memset(&connection, 0, sizeof(connection));
    (void)app_strlcpy(connection.type, "bluetooth", sizeof(connection.type));
    (void)app_strlcpy(connection.value, type_key, sizeof(connection.value));

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

    apply_recognition(slot, recognizer, APP_SOURCE_BLE, NULL, obs, NULL,
                      out_truncated);
    return slot;
}

static device_slot_t *materialize_lan(const app_scan_evidence_t *ev,
                                       size_t index,
                                       const app_recognizer_ref_t *recognizer,
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
    (void)app_device_identity_of_lan(obs, device_id, sizeof(device_id));
    (void)app_strlcpy(ha_device_id, device_id, sizeof(ha_device_id));

    name = obs->hostname[0] != '\0' ? obs->hostname : obs->ipv4;

    memset(&identifier, 0, sizeof(identifier));
    (void)app_strlcpy(identifier.domain, "lan_ip", sizeof(identifier.domain));
    (void)app_strlcpy(identifier.value, key, sizeof(identifier.value));

    memset(&connection, 0, sizeof(connection));
    (void)app_strlcpy(connection.type, "ip", sizeof(connection.type));
    (void)app_strlcpy(connection.value, obs->ipv4, sizeof(connection.value));

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

    apply_recognition(slot, recognizer, APP_SOURCE_LAN, NULL, NULL, obs,
                      out_truncated);
    return slot;
}

size_t app_device_materialize(const app_scan_evidence_t *ev,
                              const app_recognizer_ref_t *recognizer,
                              bool *truncated)
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
        if (materialize_wifi(ev, i, recognizer, &local_truncated) != NULL) {
            materialized++;
        }
    }
    for (size_t i = 0u; i < ev->ble_count; ++i) {
        if (materialize_ble(ev, i, recognizer, &local_truncated) != NULL) {
            materialized++;
        }
    }
    for (size_t i = 0u; i < ev->lan_count; ++i) {
        if (materialize_lan(ev, i, recognizer, &local_truncated) != NULL) {
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

/*
 * ============================================================================
 * Test hooks - compiled only when APP_DEVICE_TEST_HOOKS is defined.
 *
 * Not present in the firmware image: the macro is set by the host test runners and
 * by nothing else, so these three functions do not exist on the target. See
 * app_device_test_hooks.h for why they exist at all.
 * ============================================================================
 */
#ifdef APP_DEVICE_TEST_HOOKS

void app_device_test_bind_entity(const char *entity_id, const char *device_id,
                                 uint8_t backend, bool has_range, int32_t min_value,
                                 int32_t max_value)
{
    entity_slot_t *slot;
    device_slot_t *device;
    const ha_entity_t *ha;

    if (entity_id == NULL || device_id == NULL) {
        return;
    }

    /*
     * The device binding as well as the entity binding.
     *
     * A writable entity is only controllable when its Device is known and ONLINE, and
     * the control loop checks that before it reaches any backend. A hook that placed
     * only the entity would therefore produce a binding the loop correctly refuses, and
     * every test written on top of it would be testing the refusal rather than the
     * thing it meant to test - which is exactly how this hook failed when it was first
     * used.
     *
     * Availability is set to ONLINE here because a test that wants the device offline
     * marks it so explicitly; leaving it at UNKNOWN would make the default state an
     * unusable one.
     */
    device = device_slot_find(device_id);
    if (device == NULL) {
        device = device_slot_alloc();
        if (device == NULL) {
            return;
        }
        (void)app_strlcpy(device->value.device_id, device_id,
                          sizeof(device->value.device_id));
        (void)app_strlcpy(device->value.ha_device_id, device_id,
                          sizeof(device->value.ha_device_id));
        device->value.ephemeral = true;
        device->value.read_only = true;
        device->value.recognition = APP_RECOGNITION_UNKNOWN;
        device->value.availability = APP_AVAILABILITY_ONLINE;
    }

    ha = ha_core_entity_get(entity_id);
    slot = entity_slot_find(entity_id);
    if (slot == NULL) {
        slot = entity_slot_alloc();
        if (slot == NULL) {
            return;
        }
        (void)app_strlcpy(slot->value.entity_id, entity_id,
                          sizeof(slot->value.entity_id));
        (void)app_strlcpy(slot->value.device_id, device_id,
                          sizeof(slot->value.device_id));
        (void)app_strlcpy(slot->value.domain,
                          ha != NULL ? ha->domain : "switch",
                          sizeof(slot->value.domain));
        (void)app_strlcpy(slot->value.name, ha != NULL ? ha->name : "Test",
                          sizeof(slot->value.name));
        (void)app_strlcpy(slot->value.unit,
                          ha != NULL ? ha->unit_of_measurement : "",
                          sizeof(slot->value.unit));
    }

    /*
     * Writability is derived from what the entity actually advertises, so a test
     * cannot place a binding that claims to be controllable while offering no service.
     * That inconsistency is exactly what the control loop's admission checks catch, and
     * a helper that could create it would make those checks untestable.
     */
    slot->value.writable = ha != NULL && ha->supported_services != 0u;
    if (slot->value.writable) {
        slot->value.backend = backend;
        slot->value.backend_name = app_backend_name(backend);
        slot->value.write_target_id = 0u;
        slot->value.has_range = has_range;
        slot->value.min_value = min_value;
        slot->value.max_value = max_value;
    } else {
        slot->value.backend = APP_ENTITY_BACKEND_NONE;
        slot->value.backend_name = "none";
        slot->value.write_target_id = DEVICE_DB_NO_INDEX;
        slot->value.has_range = false;
    }
}

void app_device_test_mark_availability(const char *device_id,
                                       app_availability_t availability)
{
    device_slot_t *slot = device_slot_find(device_id);

    if (slot == NULL) {
        return;
    }
    slot->value.availability = availability;
    sync_device_availability(slot);
}

void app_device_test_forget_entity(const char *entity_id)
{
    entity_slot_t *slot = entity_slot_find(entity_id);

    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

#endif /* APP_DEVICE_TEST_HOOKS */
