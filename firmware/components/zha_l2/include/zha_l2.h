#ifndef ONE_OS_ZHA_L2_H
#define ONE_OS_ZHA_L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZHA_MAX_ENDPOINTS 16u
#define ZHA_MAX_CLUSTERS_PER_ENDPOINT 24u
#define ZHA_MAX_PATCHES 8u
#define ZHA_MAX_NAME_LEN 32u

#define ZHA_QUIRK_ID_IKEA_VALLHORN 0xCE5181FFu
#define ZHA_QUIRK_ID_SONOFF_BUTTON 0x82538874u

typedef enum {
    ZHA_STATUS_OK = 0,
    ZHA_STATUS_INVALID_ARG,
    ZHA_STATUS_NOT_FOUND,
    ZHA_STATUS_NO_SPACE,
    ZHA_STATUS_TRUNCATED,
    ZHA_STATUS_UNSUPPORTED,
    ZHA_STATUS_RANGE,
} zha_status_t;

typedef enum {
    ZHA_CLUSTER_SERVER = 0,
    ZHA_CLUSTER_CLIENT = 1,
} zha_cluster_side_t;

typedef struct {
    uint8_t endpoint_id;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t server_cluster_count;
    uint8_t client_cluster_count;
    uint16_t server_clusters[ZHA_MAX_CLUSTERS_PER_ENDPOINT];
    uint16_t client_clusters[ZHA_MAX_CLUSTERS_PER_ENDPOINT];
} zha_endpoint_view_t;

typedef struct {
    uint8_t endpoint_count;
    zha_endpoint_view_t endpoints[ZHA_MAX_ENDPOINTS];
} zha_device_view_t;

typedef enum {
    ZHA_PATCH_REPLACE_CLUSTER = 0,
    ZHA_PATCH_ADD_CLUSTER,
    ZHA_PATCH_REMOVE_CLUSTER,
} zha_patch_kind_t;

typedef struct {
    zha_patch_kind_t kind;
    uint8_t endpoint_id;
    zha_cluster_side_t side;
    uint16_t cluster_id;
} zha_patch_t;

typedef struct {
    uint32_t quirk_id;
    zha_device_view_t view;
    uint8_t patch_count;
    zha_patch_t applied_patches[ZHA_MAX_PATCHES];
} zha_quirked_device_t;

typedef enum {
    ZHA_CAP_SENSOR = 0,
    ZHA_CAP_BINARY_SENSOR,
    ZHA_CAP_SWITCH,
    ZHA_CAP_NUMBER,
    ZHA_CAP_BUTTON,
    ZHA_CAP_ACTION,
} zha_capability_kind_t;

typedef enum {
    ZHA_TRANSFORM_IDENTITY = 0,
    ZHA_TRANSFORM_BOOL,
    ZHA_TRANSFORM_INVERT_BOOL,
    ZHA_TRANSFORM_SCALE_OFFSET,
    ZHA_TRANSFORM_BITMASK_BOOL,
    ZHA_TRANSFORM_CLAMP,
} zha_transform_kind_t;

typedef struct {
    zha_transform_kind_t kind;
    int64_t multiplier;
    int64_t divisor;
    int64_t offset;
    int64_t mask;
    int64_t min_value;
    int64_t max_value;
} zha_transform_t;

typedef struct {
    zha_capability_kind_t kind;
    char name[ZHA_MAX_NAME_LEN + 1u];
    uint8_t endpoint_id;
    zha_cluster_side_t side;
    uint16_t cluster_id;
    uint16_t attribute_id;
    bool writable;
    int64_t min_value;
    int64_t max_value;
    int64_t step;
    char unit[12];
    zha_transform_t transform;
} zha_capability_t;

typedef struct {
    const char *project;
    const char *revision;
    const char *path;
    const char *license;
    const char *canonical_key;
} zha_quirk_provenance_t;

typedef struct {
    uint32_t quirk_id;
    const char *label;
    zha_quirk_provenance_t provenance;
} zha_quirk_info_t;

typedef struct {
    uint8_t endpoint_id;
    uint16_t cluster_id;
    uint8_t command_id;
} zha_action_event_t;

typedef enum {
    ZHA_ACTION_NONE = 0,
    ZHA_ACTION_SHORT_PRESS,
    ZHA_ACTION_DOUBLE_PRESS,
    ZHA_ACTION_LONG_PRESS,
} zha_action_kind_t;

typedef struct {
    zha_action_kind_t kind;
    uint8_t button;
} zha_action_t;

zha_status_t zha_quirk_get_info(uint32_t quirk_id, zha_quirk_info_t *out);
zha_status_t zha_quirk_apply_by_id(uint32_t quirk_id,
                                   const zha_device_view_t *input,
                                   zha_quirked_device_t *out);
zha_status_t zha_capability_enumerate(uint32_t quirk_id, size_t index,
                                      zha_capability_t *out);
zha_status_t zha_transform_decode(const zha_transform_t *transform,
                                  int64_t raw_value, int64_t *out_value);
zha_status_t zha_transform_encode(const zha_transform_t *transform,
                                  int64_t normalized_value, int64_t *out_value);
zha_status_t zha_action_decode(uint32_t quirk_id,
                               const zha_action_event_t *event,
                               zha_action_t *out);

#ifdef __cplusplus
}
#endif

#endif
