#include "ha_core.h"

#include <string.h>

#if HA_CORE_MAX_ATTRIBUTES > UINT8_MAX
#error "HA_CORE_MAX_ATTRIBUTES must fit in uint8_t"
#endif
#if HA_CORE_MAX_IDENTIFIERS > UINT8_MAX
#error "HA_CORE_MAX_IDENTIFIERS must fit in uint8_t"
#endif
#if HA_CORE_MAX_CONNECTIONS > UINT8_MAX
#error "HA_CORE_MAX_CONNECTIONS must fit in uint8_t"
#endif

typedef struct {
    bool in_use;
    ha_device_t value;
} device_slot_t;

typedef struct {
    bool in_use;
    ha_entity_t value;
} entity_slot_t;

typedef struct {
    bool in_use;
    ha_state_t value;
} state_slot_t;

static device_slot_t s_devices[HA_CORE_MAX_DEVICES];
static entity_slot_t s_entities[HA_CORE_MAX_ENTITIES];
static state_slot_t s_states[HA_CORE_MAX_STATES];
static uint32_t s_revision;

static bool cstr_fits(const char *value, size_t capacity, bool required)
{
    if (value == NULL || capacity == 0) {
        return false;
    }
    for (size_t i = 0; i < capacity; ++i) {
        if (value[i] == '\0') {
            return !required || i != 0;
        }
    }
    return false;
}

static bool slug_valid(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const char *p = value; *p != '\0'; ++p) {
        const bool lower = *p >= 'a' && *p <= 'z';
        const bool digit = *p >= '0' && *p <= '9';
        if (!lower && !digit && *p != '_') {
            return false;
        }
    }
    return true;
}

static bool entity_id_matches_domain(const char *entity_id, const char *domain)
{
    if (!cstr_fits(entity_id, HA_CORE_ENTITY_ID_LEN, true) ||
        !cstr_fits(domain, HA_CORE_DOMAIN_LEN, true) || !slug_valid(domain)) {
        return false;
    }
    const size_t domain_len = strlen(domain);
    if (domain_len + 2 > HA_CORE_ENTITY_ID_LEN ||
        strncmp(entity_id, domain, domain_len) != 0 || entity_id[domain_len] != '.') {
        return false;
    }
    return slug_valid(entity_id + domain_len + 1);
}

static void revision_bump(void)
{
    ++s_revision;
    if (s_revision == 0) {
        ++s_revision;
    }
}

static device_slot_t *device_find_slot(const char *device_id)
{
    if (!cstr_fits(device_id, HA_CORE_ID_LEN, true)) {
        return NULL;
    }
    for (size_t i = 0; i < HA_CORE_MAX_DEVICES; ++i) {
        if (s_devices[i].in_use && strcmp(s_devices[i].value.id, device_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static entity_slot_t *entity_find_slot(const char *entity_id)
{
    if (!cstr_fits(entity_id, HA_CORE_ENTITY_ID_LEN, true)) {
        return NULL;
    }
    for (size_t i = 0; i < HA_CORE_MAX_ENTITIES; ++i) {
        if (s_entities[i].in_use && strcmp(s_entities[i].value.entity_id, entity_id) == 0) {
            return &s_entities[i];
        }
    }
    return NULL;
}

static state_slot_t *state_find_slot(const char *entity_id)
{
    if (!cstr_fits(entity_id, HA_CORE_ENTITY_ID_LEN, true)) {
        return NULL;
    }
    for (size_t i = 0; i < HA_CORE_MAX_STATES; ++i) {
        if (s_states[i].in_use && strcmp(s_states[i].value.entity_id, entity_id) == 0) {
            return &s_states[i];
        }
    }
    return NULL;
}

static bool attributes_valid(const ha_attribute_t *attributes, size_t count)
{
    if (count > HA_CORE_MAX_ATTRIBUTES || (count > 0 && attributes == NULL)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!cstr_fits(attributes[i].key, sizeof(attributes[i].key), true) ||
            !cstr_fits(attributes[i].value, sizeof(attributes[i].value), false)) {
            return false;
        }
    }
    return true;
}

static bool device_valid(const ha_device_t *device)
{
    if (device == NULL || !cstr_fits(device->id, sizeof(device->id), true) ||
        device->identifier_count > HA_CORE_MAX_IDENTIFIERS ||
        device->connection_count > HA_CORE_MAX_CONNECTIONS ||
        !cstr_fits(device->manufacturer, sizeof(device->manufacturer), false) ||
        !cstr_fits(device->model, sizeof(device->model), false) ||
        !cstr_fits(device->model_id, sizeof(device->model_id), false) ||
        !cstr_fits(device->name, sizeof(device->name), false)) {
        return false;
    }
    for (size_t i = 0; i < device->identifier_count; ++i) {
        if (!cstr_fits(device->identifiers[i].domain, sizeof(device->identifiers[i].domain), true) ||
            !cstr_fits(device->identifiers[i].value, sizeof(device->identifiers[i].value), true)) {
            return false;
        }
    }
    for (size_t i = 0; i < device->connection_count; ++i) {
        if (!cstr_fits(device->connections[i].type, sizeof(device->connections[i].type), true) ||
            !cstr_fits(device->connections[i].value, sizeof(device->connections[i].value), true)) {
            return false;
        }
    }
    return true;
}

static bool entity_valid(const ha_entity_t *entity)
{
    return entity != NULL &&
           cstr_fits(entity->entity_id, sizeof(entity->entity_id), true) &&
           cstr_fits(entity->unique_id, sizeof(entity->unique_id), true) &&
           cstr_fits(entity->platform, sizeof(entity->platform), true) &&
           cstr_fits(entity->domain, sizeof(entity->domain), true) &&
           cstr_fits(entity->device_id, sizeof(entity->device_id), false) &&
           cstr_fits(entity->device_class, sizeof(entity->device_class), false) &&
           cstr_fits(entity->name, sizeof(entity->name), false) &&
           cstr_fits(entity->icon, sizeof(entity->icon), false) &&
           cstr_fits(entity->unit_of_measurement, sizeof(entity->unit_of_measurement), false) &&
           entity_id_matches_domain(entity->entity_id, entity->domain);
}

static bool device_identity_conflicts(const ha_device_t *device, const device_slot_t *self)
{
    for (size_t i = 0; i < HA_CORE_MAX_DEVICES; ++i) {
        const device_slot_t *slot = &s_devices[i];
        if (!slot->in_use || slot == self) {
            continue;
        }
        for (size_t a = 0; a < device->identifier_count; ++a) {
            for (size_t b = 0; b < slot->value.identifier_count; ++b) {
                if (strcmp(device->identifiers[a].domain, slot->value.identifiers[b].domain) == 0 &&
                    strcmp(device->identifiers[a].value, slot->value.identifiers[b].value) == 0) {
                    return true;
                }
            }
        }
        for (size_t a = 0; a < device->connection_count; ++a) {
            for (size_t b = 0; b < slot->value.connection_count; ++b) {
                if (strcmp(device->connections[a].type, slot->value.connections[b].type) == 0 &&
                    strcmp(device->connections[a].value, slot->value.connections[b].value) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

static const ha_entity_t *entity_find_unique(
    const char *domain, const char *platform, const char *unique_id)
{
    for (size_t i = 0; i < HA_CORE_MAX_ENTITIES; ++i) {
        if (s_entities[i].in_use && strcmp(s_entities[i].value.domain, domain) == 0 &&
            strcmp(s_entities[i].value.platform, platform) == 0 &&
            strcmp(s_entities[i].value.unique_id, unique_id) == 0) {
            return &s_entities[i].value;
        }
    }
    return NULL;
}

static uint32_t service_mask(const char *service)
{
    if (!cstr_fits(service, HA_CORE_SERVICE_LEN, true)) {
        return 0;
    }
    if (strcmp(service, HA_SERVICE_TURN_ON) == 0) return HA_SERVICE_MASK_TURN_ON;
    if (strcmp(service, HA_SERVICE_TURN_OFF) == 0) return HA_SERVICE_MASK_TURN_OFF;
    if (strcmp(service, HA_SERVICE_PRESS) == 0) return HA_SERVICE_MASK_PRESS;
    if (strcmp(service, HA_SERVICE_SET_VALUE) == 0) return HA_SERVICE_MASK_SET_VALUE;
    if (strcmp(service, HA_SERVICE_SELECT_OPTION) == 0) return HA_SERVICE_MASK_SELECT_OPTION;
    if (strcmp(service, HA_SERVICE_SET_TEMPERATURE) == 0) return HA_SERVICE_MASK_SET_TEMPERATURE;
    return 0;
}

void ha_core_reset(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_entities, 0, sizeof(s_entities));
    memset(s_states, 0, sizeof(s_states));
    revision_bump();
}

uint32_t ha_core_revision(void)
{
    return s_revision;
}

ha_core_status_t ha_core_device_upsert(const ha_device_t *device)
{
    if (!device_valid(device)) {
        return HA_CORE_INVALID_ARGUMENT;
    }
    device_slot_t *slot = device_find_slot(device->id);
    if (device_identity_conflicts(device, slot)) {
        return HA_CORE_CONFLICT;
    }
    if (slot == NULL) {
        for (size_t i = 0; i < HA_CORE_MAX_DEVICES; ++i) {
            if (!s_devices[i].in_use) {
                slot = &s_devices[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        return HA_CORE_CAPACITY;
    }
    slot->value = *device;
    slot->in_use = true;
    revision_bump();
    return HA_CORE_OK;
}

const ha_device_t *ha_core_device_get(const char *device_id)
{
    const device_slot_t *slot = device_find_slot(device_id);
    return slot == NULL ? NULL : &slot->value;
}

size_t ha_core_device_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < HA_CORE_MAX_DEVICES; ++i) {
        if (s_devices[i].in_use) ++count;
    }
    return count;
}

const ha_device_t *ha_core_device_at(size_t index)
{
    size_t seen = 0;
    for (size_t i = 0; i < HA_CORE_MAX_DEVICES; ++i) {
        if (!s_devices[i].in_use) continue;
        if (seen++ == index) return &s_devices[i].value;
    }
    return NULL;
}

ha_core_status_t ha_core_device_remove(const char *device_id)
{
    device_slot_t *slot = device_find_slot(device_id);
    if (slot == NULL) {
        return HA_CORE_NOT_FOUND;
    }

    /*
     * Remove the dependent Entities and States first so no Entity or State can
     * outlive the Device it belongs to. A dangling Entity would let the UI or a
     * control request address a device that no longer exists.
     */
    for (size_t i = 0; i < HA_CORE_MAX_ENTITIES; ++i) {
        if (!s_entities[i].in_use) continue;
        if (strcmp(s_entities[i].value.device_id, device_id) != 0) continue;

        const char *entity_id = s_entities[i].value.entity_id;
        for (size_t s = 0; s < HA_CORE_MAX_STATES; ++s) {
            if (s_states[s].in_use &&
                strcmp(s_states[s].value.entity_id, entity_id) == 0) {
                memset(&s_states[s], 0, sizeof(s_states[s]));
            }
        }
        memset(&s_entities[i], 0, sizeof(s_entities[i]));
    }

    memset(slot, 0, sizeof(*slot));
    revision_bump();
    return HA_CORE_OK;
}

ha_core_status_t ha_core_entity_upsert(const ha_entity_t *entity)
{
    if (!entity_valid(entity)) {
        return HA_CORE_INVALID_ARGUMENT;
    }
    if (entity->device_id[0] != '\0' && ha_core_device_get(entity->device_id) == NULL) {
        return HA_CORE_NOT_FOUND;
    }

    entity_slot_t *slot = entity_find_slot(entity->entity_id);
    const ha_entity_t *same_unique = entity_find_unique(entity->domain, entity->platform, entity->unique_id);
    if (same_unique != NULL && same_unique != (slot == NULL ? NULL : &slot->value)) {
        return HA_CORE_CONFLICT;
    }
    if (slot != NULL &&
        (strcmp(slot->value.domain, entity->domain) != 0 ||
         strcmp(slot->value.platform, entity->platform) != 0 ||
         strcmp(slot->value.unique_id, entity->unique_id) != 0)) {
        return HA_CORE_CONFLICT;
    }

    if (slot == NULL) {
        for (size_t i = 0; i < HA_CORE_MAX_ENTITIES; ++i) {
            if (!s_entities[i].in_use) {
                slot = &s_entities[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        return HA_CORE_CAPACITY;
    }
    slot->value = *entity;
    slot->in_use = true;
    revision_bump();
    return HA_CORE_OK;
}

const ha_entity_t *ha_core_entity_get(const char *entity_id)
{
    const entity_slot_t *slot = entity_find_slot(entity_id);
    return slot == NULL ? NULL : &slot->value;
}

size_t ha_core_entity_count_for_device(const char *device_id)
{
    if (!cstr_fits(device_id, HA_CORE_ID_LEN, true)) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < HA_CORE_MAX_ENTITIES; ++i) {
        if (s_entities[i].in_use && strcmp(s_entities[i].value.device_id, device_id) == 0) {
            ++count;
        }
    }
    return count;
}

const ha_entity_t *ha_core_entity_at_for_device(const char *device_id, size_t index)
{
    if (!cstr_fits(device_id, HA_CORE_ID_LEN, true)) {
        return NULL;
    }
    size_t seen = 0;
    for (size_t i = 0; i < HA_CORE_MAX_ENTITIES; ++i) {
        if (!s_entities[i].in_use || strcmp(s_entities[i].value.device_id, device_id) != 0) {
            continue;
        }
        if (seen++ == index) return &s_entities[i].value;
    }
    return NULL;
}

ha_core_status_t ha_core_state_set(
    const char *entity_id,
    const char *state,
    const ha_attribute_t *attributes,
    size_t attribute_count)
{
    if (!cstr_fits(entity_id, HA_CORE_ENTITY_ID_LEN, true) ||
        !cstr_fits(state, HA_CORE_STATE_LEN, true) ||
        !attributes_valid(attributes, attribute_count)) {
        return HA_CORE_INVALID_ARGUMENT;
    }
    if (ha_core_entity_get(entity_id) == NULL) {
        return HA_CORE_NOT_FOUND;
    }

    state_slot_t *slot = state_find_slot(entity_id);
    if (slot == NULL) {
        for (size_t i = 0; i < HA_CORE_MAX_STATES; ++i) {
            if (!s_states[i].in_use) {
                slot = &s_states[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        return HA_CORE_CAPACITY;
    }

    memset(&slot->value, 0, sizeof(slot->value));
    memcpy(slot->value.entity_id, entity_id, strlen(entity_id) + 1);
    memcpy(slot->value.state, state, strlen(state) + 1);
    if (attribute_count > 0) {
        memcpy(slot->value.attributes, attributes, attribute_count * sizeof(attributes[0]));
    }
    slot->value.attribute_count = (uint8_t)attribute_count;
    slot->in_use = true;
    revision_bump();
    return HA_CORE_OK;
}

const ha_state_t *ha_core_state_get(const char *entity_id)
{
    const state_slot_t *slot = state_find_slot(entity_id);
    return slot == NULL ? NULL : &slot->value;
}

bool ha_core_service_supports(const char *entity_id, const char *service)
{
    const ha_entity_t *entity = ha_core_entity_get(entity_id);
    const uint32_t mask = service_mask(service);
    return entity != NULL && mask != 0 && entity->enabled && entity->available &&
           entity->service_handler != NULL && (entity->supported_services & mask) != 0;
}

ha_core_status_t ha_core_service_call(
    const char *entity_id,
    const char *service,
    const ha_attribute_t *data,
    size_t data_count)
{
    if (!attributes_valid(data, data_count) || !cstr_fits(service, HA_CORE_SERVICE_LEN, true)) {
        return HA_CORE_INVALID_ARGUMENT;
    }
    const ha_entity_t *entity = ha_core_entity_get(entity_id);
    if (entity == NULL) {
        return HA_CORE_NOT_FOUND;
    }
    if (!ha_core_service_supports(entity_id, service)) {
        return HA_CORE_UNSUPPORTED;
    }
    const ha_core_status_t status = entity->service_handler(
        entity->entity_id, service, data, data_count, entity->service_context);
    return status == HA_CORE_OK ? HA_CORE_OK : HA_CORE_HANDLER_REJECTED;
}
