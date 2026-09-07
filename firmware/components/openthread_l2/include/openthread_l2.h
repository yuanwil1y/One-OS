#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENTHREAD_NETWORK_NAME_MAX 16u
#define OPENTHREAD_STEERING_DATA_MAX 16u
#define OPENTHREAD_DATASET_TLVS_MAX 254u
#define OPENTHREAD_JOINER_PSKD_MAX 32u

#define OPENTHREAD_DISCOVERY_TIMEOUT_DEFAULT_MS 6000u
#define OPENTHREAD_ATTACH_TIMEOUT_DEFAULT_MS 30000u
#define OPENTHREAD_JOINER_TIMEOUT_DEFAULT_MS 30000u
#define OPENTHREAD_OPERATION_TIMEOUT_MAX_MS 120000u

typedef enum {
    OPENTHREAD_STATUS_OK = 0,
    OPENTHREAD_STATUS_INVALID_ARGUMENT,
    OPENTHREAD_STATUS_INVALID_STATE,
    OPENTHREAD_STATUS_BUSY,
    OPENTHREAD_STATUS_TIMEOUT,
    OPENTHREAD_STATUS_NO_MEMORY,
    OPENTHREAD_STATUS_NOT_FOUND,
    OPENTHREAD_STATUS_SECURITY,
    OPENTHREAD_STATUS_IO,
    OPENTHREAD_STATUS_INTERNAL,
} openthread_status_t;

typedef enum {
    OPENTHREAD_ROLE_DISABLED = 0,
    OPENTHREAD_ROLE_DETACHED,
    OPENTHREAD_ROLE_CHILD,
    OPENTHREAD_ROLE_ROUTER,
    OPENTHREAD_ROLE_LEADER,
} openthread_role_t;

typedef struct {
    uint32_t channel_mask;
    uint16_t pan_id;
    bool joiner_flag;
    bool enable_eui64_filtering;
    uint32_t timeout_ms;
} openthread_discovery_options_t;

typedef struct {
    uint8_t channel;
    uint16_t pan_id;
    uint8_t extended_pan_id[8];
    uint8_t extended_address[8];
    char network_name[OPENTHREAD_NETWORK_NAME_MAX + 1u];
    int8_t rssi_dbm;
    uint8_t lqi;
    uint8_t thread_version;
    bool joinable;
    bool native_commissioner;
    uint16_t joiner_udp_port;
    uint8_t steering_data[OPENTHREAD_STEERING_DATA_MAX];
    uint8_t steering_data_len;
} openthread_network_t;

typedef struct {
    size_t count;
    bool truncated;
} openthread_discovery_result_t;

typedef struct {
    bool commissioned;
    bool ip6_enabled;
    openthread_role_t role;
    uint32_t attach_duration_s;

    uint8_t channel;
    uint16_t pan_id;
    uint8_t extended_pan_id[8];
    char network_name[OPENTHREAD_NETWORK_NAME_MAX + 1u];

    uint16_t rloc16;
    uint32_t partition_id;
    bool has_leader_data;
    uint8_t leader_router_id;
    uint8_t leader_weight;
    uint8_t data_version;
    uint8_t stable_data_version;

    bool has_parent;
    uint8_t parent_ext_address[8];
    uint16_t parent_rloc16;
    bool has_parent_average_rssi;
    int8_t parent_average_rssi_dbm;
    bool has_parent_last_rssi;
    int8_t parent_last_rssi_dbm;

    uint32_t ip_tx_success;
    uint32_t ip_rx_success;
    uint32_t ip_tx_failure;
    uint32_t ip_rx_failure;
    uint16_t attach_attempts;
    uint16_t parent_changes;
} openthread_state_snapshot_t;

typedef enum {
    OPENTHREAD_PEER_NEIGHBOR = 0,
    OPENTHREAD_PEER_PARENT,
    OPENTHREAD_PEER_CHILD,
    OPENTHREAD_PEER_ROUTER,
} openthread_peer_kind_t;

typedef struct {
    openthread_peer_kind_t kind;
    uint8_t ext_address[8];
    uint16_t rloc16;
    uint16_t thread_version;
    uint32_t age_s;
    bool has_average_rssi;
    int8_t average_rssi_dbm;
    bool has_last_rssi;
    int8_t last_rssi_dbm;
    bool has_link_quality_in;
    uint8_t link_quality_in;
    bool has_link_quality_out;
    uint8_t link_quality_out;
    bool has_path_cost;
    uint8_t path_cost;
    bool rx_on_when_idle;
    bool full_thread_device;
    bool full_network_data;
} openthread_peer_t;

typedef struct {
    size_t count;
    bool truncated;
    bool ftd_details_available;
} openthread_topology_result_t;

typedef struct {
    uint8_t bytes[OPENTHREAD_DATASET_TLVS_MAX];
    uint16_t length;
} openthread_dataset_tlvs_t;

typedef struct {
    uint32_t timeout_ms;
    bool require_complete_active_dataset;
} openthread_attach_options_t;

typedef struct {
    openthread_role_t final_role;
    uint32_t attach_duration_s;
    bool dataset_persisted;
} openthread_attach_result_t;

typedef struct {
    const char *pskd;
    const char *provisioning_url;
    const char *vendor_name;
    const char *vendor_model;
    const char *vendor_sw_version;
    const char *vendor_data;
    uint32_t join_timeout_ms;
    uint32_t attach_timeout_ms;
} openthread_joiner_options_t;

openthread_status_t openthread_discover_networks(
    const openthread_discovery_options_t *options,
    openthread_network_t *networks,
    size_t capacity,
    openthread_discovery_result_t *result);

openthread_status_t openthread_get_state_snapshot(openthread_state_snapshot_t *snapshot);

openthread_status_t openthread_get_local_topology(
    openthread_peer_t *peers,
    size_t capacity,
    openthread_topology_result_t *result);

openthread_status_t openthread_attach_dataset(
    const openthread_dataset_tlvs_t *dataset,
    const openthread_attach_options_t *options,
    openthread_attach_result_t *result);

openthread_status_t openthread_joiner_join(
    const openthread_joiner_options_t *options,
    openthread_attach_result_t *result);

#ifdef __cplusplus
}
#endif
