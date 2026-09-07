#pragma once

#include "matter_l2.h"

#ifdef __cplusplus
extern "C" {
#endif

void matter_bounds_reset_node(matter_node_info_t *node, uint64_t node_id);
matter_endpoint_info_t *matter_bounds_get_endpoint(matter_node_info_t *node, uint16_t endpoint_id);
bool matter_bounds_add_device_type(matter_node_info_t *node, uint16_t endpoint_id, uint32_t device_type_id,
                                   uint16_t revision);
bool matter_bounds_add_server_cluster(matter_node_info_t *node, uint16_t endpoint_id, uint32_t cluster_id);
bool matter_bounds_add_part(matter_node_info_t *node, uint16_t endpoint_id, uint16_t part_endpoint_id);

#ifdef __cplusplus
}
#endif
