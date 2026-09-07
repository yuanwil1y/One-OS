#include "matter_l2_bounds.h"

#include <string.h>

void matter_bounds_reset_node(matter_node_info_t *node, uint64_t node_id)
{
    if (!node) {
        return;
    }
    memset(node, 0, sizeof(*node));
    node->node_id = node_id;
}

matter_endpoint_info_t *matter_bounds_get_endpoint(matter_node_info_t *node, uint16_t endpoint_id)
{
    if (!node) {
        return NULL;
    }
    for (uint8_t i = 0; i < node->endpoint_count; ++i) {
        if (node->endpoints[i].endpoint_id == endpoint_id) {
            return &node->endpoints[i];
        }
    }
    if (node->endpoint_count >= CHIP_L2_MAX_ENDPOINTS) {
        node->truncated = true;
        return NULL;
    }
    matter_endpoint_info_t *endpoint = &node->endpoints[node->endpoint_count++];
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->endpoint_id = endpoint_id;
    return endpoint;
}

bool matter_bounds_add_device_type(matter_node_info_t *node, uint16_t endpoint_id, uint32_t device_type_id,
                                   uint16_t revision)
{
    matter_endpoint_info_t *endpoint = matter_bounds_get_endpoint(node, endpoint_id);
    if (!endpoint) {
        return false;
    }
    for (uint8_t i = 0; i < endpoint->device_type_count; ++i) {
        if (endpoint->device_types[i].device_type_id == device_type_id && endpoint->device_types[i].revision == revision) {
            return true;
        }
    }
    if (endpoint->device_type_count >= CHIP_L2_MAX_DEVICE_TYPES_PER_ENDPOINT) {
        endpoint->truncated = true;
        node->truncated = true;
        return false;
    }
    matter_device_type_t *entry = &endpoint->device_types[endpoint->device_type_count++];
    entry->device_type_id = device_type_id;
    entry->revision = revision;
    return true;
}

bool matter_bounds_add_server_cluster(matter_node_info_t *node, uint16_t endpoint_id, uint32_t cluster_id)
{
    matter_endpoint_info_t *endpoint = matter_bounds_get_endpoint(node, endpoint_id);
    if (!endpoint) {
        return false;
    }
    for (uint8_t i = 0; i < endpoint->server_cluster_count; ++i) {
        if (endpoint->server_clusters[i] == cluster_id) {
            return true;
        }
    }
    if (endpoint->server_cluster_count >= CHIP_L2_MAX_SERVER_CLUSTERS_PER_ENDPOINT) {
        endpoint->truncated = true;
        node->truncated = true;
        return false;
    }
    endpoint->server_clusters[endpoint->server_cluster_count++] = cluster_id;
    return true;
}

bool matter_bounds_add_part(matter_node_info_t *node, uint16_t endpoint_id, uint16_t part_endpoint_id)
{
    matter_endpoint_info_t *endpoint = matter_bounds_get_endpoint(node, endpoint_id);
    if (!endpoint) {
        return false;
    }
    for (uint8_t i = 0; i < endpoint->parts_count; ++i) {
        if (endpoint->parts[i] == part_endpoint_id) {
            return true;
        }
    }
    if (endpoint->parts_count >= CHIP_L2_MAX_PARTS_PER_ENDPOINT) {
        endpoint->truncated = true;
        node->truncated = true;
        return false;
    }
    endpoint->parts[endpoint->parts_count++] = part_endpoint_id;
    return true;
}
