#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HA_MDNS_SERVICE_TYPE_LEN 72
#define HA_MDNS_INSTANCE_LEN 128
#define HA_MDNS_HOST_LEN 96
#define HA_MDNS_ADDRESS_LEN 48
#define HA_MDNS_TXT_KEY_LEN 32
#define HA_MDNS_TXT_VALUE_LEN 80
#define HA_MDNS_MAX_ADDRESSES 4
#define HA_MDNS_MAX_TXT 8
#define HA_MDNS_MAX_SERVICE_TYPES 16

#define HA_SSDP_ST_LEN 112
#define HA_SSDP_NT_LEN 112
#define HA_SSDP_USN_LEN 160
#define HA_SSDP_SERVER_LEN 112
#define HA_SSDP_LOCATION_LEN 192
#define HA_SSDP_CACHE_CONTROL_LEN 64

typedef enum {
    HA_DISCOVERY_OK = 0,
    HA_DISCOVERY_INVALID_ARGUMENT,
    HA_DISCOVERY_CANCELLED,
    HA_DISCOVERY_NETWORK_ERROR,
    HA_DISCOVERY_MALFORMED,
    HA_DISCOVERY_TRUNCATED,
} ha_discovery_status_t;

typedef bool (*ha_discovery_cancel_fn)(void *ctx);

typedef struct {
    char key[HA_MDNS_TXT_KEY_LEN];
    char value[HA_MDNS_TXT_VALUE_LEN];
    bool has_value;
} ha_mdns_txt_t;

typedef struct {
    char service_type[HA_MDNS_SERVICE_TYPE_LEN];
    char instance[HA_MDNS_INSTANCE_LEN];
    char hostname[HA_MDNS_HOST_LEN];
    uint16_t port;
    char addresses[HA_MDNS_MAX_ADDRESSES][HA_MDNS_ADDRESS_LEN];
    uint8_t address_count;
    ha_mdns_txt_t txt[HA_MDNS_MAX_TXT];
    uint8_t txt_count;
    bool truncated;
} ha_mdns_service_t;

typedef struct {
    uint32_t timeout_ms;
    const char *const *service_types;
    size_t service_type_count;
    ha_discovery_cancel_fn cancelled;
    void *cancel_context;
} ha_mdns_discover_options_t;

/*
 * Finite mDNS/DNS-SD browse. When service_type_count is zero the workflow first
 * queries _services._dns-sd._udp.local. and then browses the bounded set of
 * service types learned during the same window. Results are caller-owned.
 */
ha_discovery_status_t ha_mdns_discover_once(
    const ha_mdns_discover_options_t *options,
    ha_mdns_service_t *services,
    size_t service_capacity,
    size_t *service_count);

typedef enum {
    HA_SSDP_SOURCE_RESPONSE = 0,
    HA_SSDP_SOURCE_ALIVE,
    HA_SSDP_SOURCE_BYEBYE,
    HA_SSDP_SOURCE_UPDATE,
    HA_SSDP_SOURCE_UNKNOWN,
} ha_ssdp_source_t;

typedef struct {
    ha_ssdp_source_t source;
    char st[HA_SSDP_ST_LEN];
    char nt[HA_SSDP_NT_LEN];
    char usn[HA_SSDP_USN_LEN];
    char server[HA_SSDP_SERVER_LEN];
    char location[HA_SSDP_LOCATION_LEN];
    char cache_control[HA_SSDP_CACHE_CONTROL_LEN];
    bool truncated;
} ha_ssdp_service_t;

typedef struct {
    uint32_t timeout_ms;
    ha_discovery_cancel_fn cancelled;
    void *cancel_context;
} ha_ssdp_discover_options_t;

/* Finite HA-style SSDP M-SEARCH using multicast plus IPv4 broadcast fallback. */
ha_discovery_status_t ha_ssdp_discover_once(
    const ha_ssdp_discover_options_t *options,
    ha_ssdp_service_t *services,
    size_t service_capacity,
    size_t *service_count);

#ifdef __cplusplus
}
#endif
