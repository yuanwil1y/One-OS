#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bounded RAM-only Home Assistant semantic core for One-OS.
 *
 * Ownership contract: one application owner task performs all ha_core calls.
 * The component is intentionally not thread-safe. Returned pointers are borrowed
 * read-only views into fixed pools and must not be cached across successful
 * mutations. No packet buffers, protocol handles, or cross-project L2 types are
 * stored here.
 */

#ifndef HA_CORE_MAX_DEVICES
#define HA_CORE_MAX_DEVICES 8
#endif
#ifndef HA_CORE_MAX_ENTITIES
#define HA_CORE_MAX_ENTITIES 32
#endif
#ifndef HA_CORE_MAX_STATES
#define HA_CORE_MAX_STATES HA_CORE_MAX_ENTITIES
#endif
#ifndef HA_CORE_MAX_ATTRIBUTES
#define HA_CORE_MAX_ATTRIBUTES 4
#endif
#ifndef HA_CORE_MAX_IDENTIFIERS
#define HA_CORE_MAX_IDENTIFIERS 2
#endif
#ifndef HA_CORE_MAX_CONNECTIONS
#define HA_CORE_MAX_CONNECTIONS 2
#endif

#define HA_CORE_ID_LEN 33
#define HA_CORE_ENTITY_ID_LEN 64
#define HA_CORE_UNIQUE_ID_LEN 64
#define HA_CORE_DOMAIN_LEN 24
#define HA_CORE_PLATFORM_LEN 32
#define HA_CORE_NAME_LEN 48
#define HA_CORE_STATE_LEN 40
#define HA_CORE_ATTRIBUTE_KEY_LEN 24
#define HA_CORE_ATTRIBUTE_VALUE_LEN 48
#define HA_CORE_DEVICE_CLASS_LEN 32
#define HA_CORE_UNIT_LEN 20
#define HA_CORE_ICON_LEN 32
#define HA_CORE_CONNECTION_TYPE_LEN 16
#define HA_CORE_IDENTIFIER_VALUE_LEN 48
#define HA_CORE_CONNECTION_VALUE_LEN 48
#define HA_CORE_SERVICE_LEN 24

#define HA_DOMAIN_SENSOR "sensor"
#define HA_DOMAIN_BINARY_SENSOR "binary_sensor"
#define HA_DOMAIN_SWITCH "switch"
#define HA_DOMAIN_LIGHT "light"
#define HA_DOMAIN_BUTTON "button"
#define HA_DOMAIN_NUMBER "number"
#define HA_DOMAIN_SELECT "select"
#define HA_DOMAIN_CLIMATE "climate"

#define HA_SERVICE_TURN_ON "turn_on"
#define HA_SERVICE_TURN_OFF "turn_off"
#define HA_SERVICE_PRESS "press"
#define HA_SERVICE_SET_VALUE "set_value"
#define HA_SERVICE_SELECT_OPTION "select_option"
#define HA_SERVICE_SET_TEMPERATURE "set_temperature"

#define HA_SERVICE_MASK_TURN_ON (1u << 0)
#define HA_SERVICE_MASK_TURN_OFF (1u << 1)
#define HA_SERVICE_MASK_PRESS (1u << 2)
#define HA_SERVICE_MASK_SET_VALUE (1u << 3)
#define HA_SERVICE_MASK_SELECT_OPTION (1u << 4)
#define HA_SERVICE_MASK_SET_TEMPERATURE (1u << 5)

#define HA_STATE_ON "on"
#define HA_STATE_OFF "off"
#define HA_STATE_UNAVAILABLE "unavailable"
#define HA_STATE_UNKNOWN "unknown"

typedef enum {
    HA_CORE_OK = 0,
    HA_CORE_INVALID_ARGUMENT,
    HA_CORE_NOT_FOUND,
    HA_CORE_CAPACITY,
    HA_CORE_CONFLICT,
    HA_CORE_UNSUPPORTED,
    HA_CORE_HANDLER_REJECTED,
} ha_core_status_t;

typedef struct {
    char key[HA_CORE_ATTRIBUTE_KEY_LEN];
    char value[HA_CORE_ATTRIBUTE_VALUE_LEN];
} ha_attribute_t;

typedef struct {
    char domain[HA_CORE_DOMAIN_LEN];
    char value[HA_CORE_IDENTIFIER_VALUE_LEN];
} ha_identifier_t;

typedef struct {
    char type[HA_CORE_CONNECTION_TYPE_LEN];
    char value[HA_CORE_CONNECTION_VALUE_LEN];
} ha_connection_t;

typedef struct {
    char id[HA_CORE_ID_LEN];
    ha_identifier_t identifiers[HA_CORE_MAX_IDENTIFIERS];
    uint8_t identifier_count;
    ha_connection_t connections[HA_CORE_MAX_CONNECTIONS];
    uint8_t connection_count;
    char manufacturer[HA_CORE_NAME_LEN];
    char model[HA_CORE_NAME_LEN];
    char model_id[HA_CORE_NAME_LEN];
    char name[HA_CORE_NAME_LEN];
} ha_device_t;

struct ha_entity;

typedef ha_core_status_t (*ha_service_handler_t)(
    const char *entity_id,
    const char *service,
    const ha_attribute_t *data,
    size_t data_count,
    void *ctx);

typedef struct ha_entity {
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char unique_id[HA_CORE_UNIQUE_ID_LEN];
    char platform[HA_CORE_PLATFORM_LEN];
    char domain[HA_CORE_DOMAIN_LEN];
    char device_id[HA_CORE_ID_LEN];
    char device_class[HA_CORE_DEVICE_CLASS_LEN];
    char name[HA_CORE_NAME_LEN];
    char icon[HA_CORE_ICON_LEN];
    char unit_of_measurement[HA_CORE_UNIT_LEN];
    uint32_t supported_features;
    uint32_t supported_services;
    bool has_entity_name;
    bool enabled;
    bool available;
    ha_service_handler_t service_handler;
    void *service_context;
} ha_entity_t;

typedef struct {
    char entity_id[HA_CORE_ENTITY_ID_LEN];
    char state[HA_CORE_STATE_LEN];
    ha_attribute_t attributes[HA_CORE_MAX_ATTRIBUTES];
    uint8_t attribute_count;
} ha_state_t;

void ha_core_reset(void);
uint32_t ha_core_revision(void);

ha_core_status_t ha_core_device_upsert(const ha_device_t *device);
const ha_device_t *ha_core_device_get(const char *device_id);
size_t ha_core_device_count(void);
const ha_device_t *ha_core_device_at(size_t index);

/*
 * Remove a Device together with its Entities and their States.
 *
 * Needed by the application to sweep ephemeral observations once a later scan
 * generation completes without seeing them. Returns HA_CORE_NOT_FOUND when the
 * Device is unknown, so a caller cannot mistake "already gone" for success on
 * the wrong id. Persistent, authorized identities must not be removed this way;
 * that policy lives in the application.
 */
ha_core_status_t ha_core_device_remove(const char *device_id);

ha_core_status_t ha_core_entity_upsert(const ha_entity_t *entity);
const ha_entity_t *ha_core_entity_get(const char *entity_id);
size_t ha_core_entity_count_for_device(const char *device_id);
const ha_entity_t *ha_core_entity_at_for_device(const char *device_id, size_t index);

ha_core_status_t ha_core_state_set(
    const char *entity_id,
    const char *state,
    const ha_attribute_t *attributes,
    size_t attribute_count);
const ha_state_t *ha_core_state_get(const char *entity_id);

bool ha_core_service_supports(const char *entity_id, const char *service);
ha_core_status_t ha_core_service_call(
    const char *entity_id,
    const char *service,
    const ha_attribute_t *data,
    size_t data_count);

#ifdef __cplusplus
}
#endif
