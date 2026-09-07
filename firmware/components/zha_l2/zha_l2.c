#include "zha_l2.h"

#include <limits.h>
#include <string.h>

#define ZHA_ON_OFF_CLUSTER_ID 0x0006u
#define ZHA_IKEA_CONFIG_CLUSTER_ID 0xFC81u
#define ZHA_IKEA_ATTR_DARK_ONLY 0x0000u
#define ZHA_IKEA_ATTR_ON_TIME 0x0002u

static const zha_quirk_info_t QUIRK_INFO[] = {
    {
        .quirk_id = ZHA_QUIRK_ID_IKEA_VALLHORN,
        .label = "IKEA VALLHORN selected quirk",
        .provenance = {
            .project = "zigpy/zha-device-handlers",
            .revision = "6a3822c1348f9e40ad243eb881084a1c075da277",
            .path = "zhaquirks/ikea/vallhorn.py",
            .license = "Apache-2.0",
            .canonical_key = "zhaquirks/ikea/vallhorn.py:IKEA of Sweden:VALLHORN Wireless Motion Sensor",
        },
    },
    {
        .quirk_id = ZHA_QUIRK_ID_SONOFF_BUTTON,
        .label = "Sonoff button action semantics",
        .provenance = {
            .project = "zigpy/zha-device-handlers",
            .revision = "6a3822c1348f9e40ad243eb881084a1c075da277",
            .path = "zhaquirks/sonoff/button.py",
            .license = "Apache-2.0",
            .canonical_key = "zhaquirks/sonoff/button.py:eWeLink:WB01",
        },
    },
};

static const zha_patch_t VALLHORN_PATCHES[] = {
    {
        .kind = ZHA_PATCH_REPLACE_CLUSTER,
        .endpoint_id = 1u,
        .side = ZHA_CLUSTER_SERVER,
        .cluster_id = ZHA_IKEA_CONFIG_CLUSTER_ID,
    },
};

static const zha_capability_t VALLHORN_CAPABILITIES[] = {
    {
        .kind = ZHA_CAP_SWITCH,
        .name = "on_only_when_dark",
        .endpoint_id = 1u,
        .side = ZHA_CLUSTER_SERVER,
        .cluster_id = ZHA_IKEA_CONFIG_CLUSTER_ID,
        .attribute_id = ZHA_IKEA_ATTR_DARK_ONLY,
        .writable = true,
        .min_value = 0,
        .max_value = 1,
        .step = 1,
        .unit = "",
        .transform = {.kind = ZHA_TRANSFORM_BOOL, .multiplier = 1, .divisor = 1},
    },
    {
        .kind = ZHA_CAP_NUMBER,
        .name = "on_time",
        .endpoint_id = 1u,
        .side = ZHA_CLUSTER_SERVER,
        .cluster_id = ZHA_IKEA_CONFIG_CLUSTER_ID,
        .attribute_id = ZHA_IKEA_ATTR_ON_TIME,
        .writable = true,
        .min_value = 10,
        .max_value = 65534,
        .step = 1,
        .unit = "s",
        .transform = {.kind = ZHA_TRANSFORM_IDENTITY, .multiplier = 1, .divisor = 1},
    },
};

static const zha_capability_t SONOFF_BUTTON_CAPABILITIES[] = {
    {
        .kind = ZHA_CAP_ACTION,
        .name = "button",
        .endpoint_id = 1u,
        .side = ZHA_CLUSTER_SERVER,
        .cluster_id = ZHA_ON_OFF_CLUSTER_ID,
        .attribute_id = 0u,
        .writable = false,
        .min_value = 0,
        .max_value = 0,
        .step = 0,
        .unit = "",
        .transform = {.kind = ZHA_TRANSFORM_IDENTITY, .multiplier = 1, .divisor = 1},
    },
};

static const zha_quirk_info_t *find_quirk_info(uint32_t quirk_id)
{
    size_t i;
    for (i = 0; i < sizeof(QUIRK_INFO) / sizeof(QUIRK_INFO[0]); ++i) {
        if (QUIRK_INFO[i].quirk_id == quirk_id) {
            return &QUIRK_INFO[i];
        }
    }
    return NULL;
}

static zha_endpoint_view_t *find_endpoint(zha_device_view_t *view, uint8_t endpoint_id)
{
    size_t i;
    for (i = 0; i < view->endpoint_count; ++i) {
        if (view->endpoints[i].endpoint_id == endpoint_id) {
            return &view->endpoints[i];
        }
    }
    return NULL;
}

static bool cluster_present(const uint16_t *clusters, uint8_t count, uint16_t cluster_id)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (clusters[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static zha_status_t apply_patch(zha_device_view_t *view, const zha_patch_t *patch)
{
    zha_endpoint_view_t *ep;
    uint16_t *clusters;
    uint8_t *count;
    size_t i;

    ep = find_endpoint(view, patch->endpoint_id);
    if (ep == NULL) {
        return ZHA_STATUS_NOT_FOUND;
    }
    if (patch->side == ZHA_CLUSTER_SERVER) {
        clusters = ep->server_clusters;
        count = &ep->server_cluster_count;
    } else {
        clusters = ep->client_clusters;
        count = &ep->client_cluster_count;
    }

    switch (patch->kind) {
    case ZHA_PATCH_REPLACE_CLUSTER:
        return cluster_present(clusters, *count, patch->cluster_id)
                   ? ZHA_STATUS_OK
                   : ZHA_STATUS_NOT_FOUND;
    case ZHA_PATCH_ADD_CLUSTER:
        if (cluster_present(clusters, *count, patch->cluster_id)) {
            return ZHA_STATUS_OK;
        }
        if (*count >= ZHA_MAX_CLUSTERS_PER_ENDPOINT) {
            return ZHA_STATUS_NO_SPACE;
        }
        clusters[*count] = patch->cluster_id;
        (*count)++;
        return ZHA_STATUS_OK;
    case ZHA_PATCH_REMOVE_CLUSTER:
        for (i = 0; i < *count; ++i) {
            if (clusters[i] == patch->cluster_id) {
                memmove(&clusters[i], &clusters[i + 1u],
                        ((size_t)(*count) - i - 1u) * sizeof(clusters[0]));
                (*count)--;
                return ZHA_STATUS_OK;
            }
        }
        return ZHA_STATUS_NOT_FOUND;
    default:
        return ZHA_STATUS_INVALID_ARG;
    }
}

zha_status_t zha_quirk_get_info(uint32_t quirk_id, zha_quirk_info_t *out)
{
    const zha_quirk_info_t *info;
    if (out == NULL) {
        return ZHA_STATUS_INVALID_ARG;
    }
    info = find_quirk_info(quirk_id);
    if (info == NULL) {
        return ZHA_STATUS_NOT_FOUND;
    }
    *out = *info;
    return ZHA_STATUS_OK;
}

zha_status_t zha_quirk_apply_by_id(uint32_t quirk_id,
                                   const zha_device_view_t *input,
                                   zha_quirked_device_t *out)
{
    const zha_patch_t *patches = NULL;
    size_t patch_count = 0u;
    size_t i;
    zha_status_t status;

    if (input == NULL || out == NULL || input->endpoint_count > ZHA_MAX_ENDPOINTS) {
        return ZHA_STATUS_INVALID_ARG;
    }
    if (find_quirk_info(quirk_id) == NULL) {
        return ZHA_STATUS_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    out->quirk_id = quirk_id;
    out->view = *input;

    if (quirk_id == ZHA_QUIRK_ID_IKEA_VALLHORN) {
        patches = VALLHORN_PATCHES;
        patch_count = sizeof(VALLHORN_PATCHES) / sizeof(VALLHORN_PATCHES[0]);
    }

    if (patch_count > ZHA_MAX_PATCHES) {
        return ZHA_STATUS_NO_SPACE;
    }
    for (i = 0; i < patch_count; ++i) {
        status = apply_patch(&out->view, &patches[i]);
        if (status != ZHA_STATUS_OK) {
            return status;
        }
        out->applied_patches[out->patch_count++] = patches[i];
    }
    return ZHA_STATUS_OK;
}

zha_status_t zha_capability_enumerate(uint32_t quirk_id, size_t index,
                                      zha_capability_t *out)
{
    const zha_capability_t *caps = NULL;
    size_t count = 0u;

    if (out == NULL) {
        return ZHA_STATUS_INVALID_ARG;
    }
    if (quirk_id == ZHA_QUIRK_ID_IKEA_VALLHORN) {
        caps = VALLHORN_CAPABILITIES;
        count = sizeof(VALLHORN_CAPABILITIES) / sizeof(VALLHORN_CAPABILITIES[0]);
    } else if (quirk_id == ZHA_QUIRK_ID_SONOFF_BUTTON) {
        caps = SONOFF_BUTTON_CAPABILITIES;
        count = sizeof(SONOFF_BUTTON_CAPABILITIES) /
                sizeof(SONOFF_BUTTON_CAPABILITIES[0]);
    } else {
        return ZHA_STATUS_NOT_FOUND;
    }
    if (index >= count) {
        return ZHA_STATUS_NOT_FOUND;
    }
    *out = caps[index];
    return ZHA_STATUS_OK;
}

static bool multiplication_overflows(int64_t a, int64_t b)
{
    if (a == 0 || b == 0) {
        return false;
    }
    if (a == -1 && b == INT64_MIN) {
        return true;
    }
    if (b == -1 && a == INT64_MIN) {
        return true;
    }
    if (a > 0) {
        if (b > 0) {
            return a > INT64_MAX / b;
        }
        return b < INT64_MIN / a;
    }
    if (b > 0) {
        return a < INT64_MIN / b;
    }
    return a != 0 && b < INT64_MAX / a;
}

zha_status_t zha_transform_decode(const zha_transform_t *transform,
                                  int64_t raw_value, int64_t *out_value)
{
    int64_t scaled;
    if (transform == NULL || out_value == NULL) {
        return ZHA_STATUS_INVALID_ARG;
    }

    switch (transform->kind) {
    case ZHA_TRANSFORM_IDENTITY:
        *out_value = raw_value;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_BOOL:
        *out_value = raw_value != 0 ? 1 : 0;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_INVERT_BOOL:
        *out_value = raw_value == 0 ? 1 : 0;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_SCALE_OFFSET:
        if (transform->divisor == 0 ||
            multiplication_overflows(raw_value, transform->multiplier)) {
            return ZHA_STATUS_RANGE;
        }
        scaled = (raw_value * transform->multiplier) / transform->divisor;
        if ((transform->offset > 0 && scaled > INT64_MAX - transform->offset) ||
            (transform->offset < 0 && scaled < INT64_MIN - transform->offset)) {
            return ZHA_STATUS_RANGE;
        }
        *out_value = scaled + transform->offset;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_BITMASK_BOOL:
        *out_value = (raw_value & transform->mask) != 0 ? 1 : 0;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_CLAMP:
        if (transform->min_value > transform->max_value) {
            return ZHA_STATUS_INVALID_ARG;
        }
        *out_value = raw_value < transform->min_value
                         ? transform->min_value
                         : (raw_value > transform->max_value ? transform->max_value
                                                              : raw_value);
        return ZHA_STATUS_OK;
    default:
        return ZHA_STATUS_UNSUPPORTED;
    }
}

zha_status_t zha_transform_encode(const zha_transform_t *transform,
                                  int64_t normalized_value, int64_t *out_value)
{
    int64_t adjusted;
    if (transform == NULL || out_value == NULL) {
        return ZHA_STATUS_INVALID_ARG;
    }

    switch (transform->kind) {
    case ZHA_TRANSFORM_IDENTITY:
        *out_value = normalized_value;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_BOOL:
        *out_value = normalized_value != 0 ? 1 : 0;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_INVERT_BOOL:
        *out_value = normalized_value == 0 ? 1 : 0;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_SCALE_OFFSET:
        if (transform->multiplier == 0) {
            return ZHA_STATUS_RANGE;
        }
        if ((transform->offset > 0 && normalized_value < INT64_MIN + transform->offset) ||
            (transform->offset < 0 && normalized_value > INT64_MAX + transform->offset)) {
            return ZHA_STATUS_RANGE;
        }
        adjusted = normalized_value - transform->offset;
        if (multiplication_overflows(adjusted, transform->divisor)) {
            return ZHA_STATUS_RANGE;
        }
        *out_value = (adjusted * transform->divisor) / transform->multiplier;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_BITMASK_BOOL:
        *out_value = normalized_value != 0 ? transform->mask : 0;
        return ZHA_STATUS_OK;
    case ZHA_TRANSFORM_CLAMP:
        if (normalized_value < transform->min_value ||
            normalized_value > transform->max_value) {
            return ZHA_STATUS_RANGE;
        }
        *out_value = normalized_value;
        return ZHA_STATUS_OK;
    default:
        return ZHA_STATUS_UNSUPPORTED;
    }
}

zha_status_t zha_action_decode(uint32_t quirk_id,
                               const zha_action_event_t *event,
                               zha_action_t *out)
{
    if (event == NULL || out == NULL) {
        return ZHA_STATUS_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (quirk_id != ZHA_QUIRK_ID_SONOFF_BUTTON) {
        return ZHA_STATUS_NOT_FOUND;
    }
    if (event->endpoint_id != 1u || event->cluster_id != ZHA_ON_OFF_CLUSTER_ID) {
        return ZHA_STATUS_NOT_FOUND;
    }

    out->button = 1u;
    switch (event->command_id) {
    case 0x02u:
        out->kind = ZHA_ACTION_SHORT_PRESS;
        return ZHA_STATUS_OK;
    case 0x01u:
        out->kind = ZHA_ACTION_DOUBLE_PRESS;
        return ZHA_STATUS_OK;
    case 0x00u:
        out->kind = ZHA_ACTION_LONG_PRESS;
        return ZHA_STATUS_OK;
    default:
        return ZHA_STATUS_NOT_FOUND;
    }
}
