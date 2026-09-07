#include "matter_l2.h"
#include "matter_l2_bounds.h"

#include <assert.h>
#include <stdio.h>

static void test_endpoint_bound(void)
{
    matter_node_info_t node;
    matter_bounds_reset_node(&node, 0x1234);
    for (uint16_t i = 0; i < CHIP_L2_MAX_ENDPOINTS; ++i) {
        assert(matter_bounds_get_endpoint(&node, i) != NULL);
    }
    assert(node.endpoint_count == CHIP_L2_MAX_ENDPOINTS);
    assert(matter_bounds_get_endpoint(&node, 99) == NULL);
    assert(node.truncated);
}

static void test_cluster_bound_and_dedup(void)
{
    matter_node_info_t node;
    matter_bounds_reset_node(&node, 1);
    for (uint32_t i = 0; i < CHIP_L2_MAX_SERVER_CLUSTERS_PER_ENDPOINT; ++i) {
        assert(matter_bounds_add_server_cluster(&node, 1, 0x1000 + i));
    }
    assert(matter_bounds_add_server_cluster(&node, 1, 0x1000));
    assert(!matter_bounds_add_server_cluster(&node, 1, 0xFFFF));
    assert(node.endpoints[0].server_cluster_count == CHIP_L2_MAX_SERVER_CLUSTERS_PER_ENDPOINT);
    assert(node.endpoints[0].truncated);
    assert(node.truncated);
}

static void test_device_type_and_parts_bounds(void)
{
    matter_node_info_t node;
    matter_bounds_reset_node(&node, 2);
    for (uint32_t i = 0; i < CHIP_L2_MAX_DEVICE_TYPES_PER_ENDPOINT; ++i) {
        assert(matter_bounds_add_device_type(&node, 2, 0x200 + i, 1));
    }
    assert(!matter_bounds_add_device_type(&node, 2, 0x999, 1));

    matter_bounds_reset_node(&node, 3);
    for (uint16_t i = 0; i < CHIP_L2_MAX_PARTS_PER_ENDPOINT; ++i) {
        assert(matter_bounds_add_part(&node, 0, i + 1));
    }
    assert(matter_bounds_add_part(&node, 0, 1));
    assert(!matter_bounds_add_part(&node, 0, 100));
}

int main(void)
{
    test_endpoint_bound();
    test_cluster_bound_and_dedup();
    test_device_type_and_parts_bounds();
    puts("matter_l2 bounds tests: PASS");
    return 0;
}
