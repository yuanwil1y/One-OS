#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "openthread_l2_internal.h"

static openthread_network_t network(uint8_t channel, uint16_t pan, uint8_t xpan0,
                                    const char *name, int8_t rssi, uint8_t lqi)
{
    openthread_network_t n = {0};
    n.channel = channel;
    n.pan_id = pan;
    n.extended_pan_id[0] = xpan0;
    n.extended_address[0] = (uint8_t)(rssi & 0xff);
    (void)snprintf(n.network_name, sizeof(n.network_name), "%s", name);
    n.rssi_dbm = rssi;
    n.lqi = lqi;
    return n;
}

static void test_dataset_bounds(void)
{
    assert(!openthread_l2_dataset_length_valid(0));
    assert(openthread_l2_dataset_length_valid(1));
    assert(openthread_l2_dataset_length_valid(OPENTHREAD_DATASET_TLVS_MAX));
    assert(!openthread_l2_dataset_length_valid(OPENTHREAD_DATASET_TLVS_MAX + 1u));
}

static void test_merge_deduplicates_and_keeps_strongest(void)
{
    openthread_network_t entries[2] = {0};
    size_t count = 0;
    bool truncated = false;
    openthread_network_t weak = network(15, 0x1234, 0xaa, "mesh", -80, 100);
    openthread_network_t strong = network(15, 0x1234, 0xaa, "mesh", -50, 90);
    strong.joinable = true;

    openthread_l2_merge_network(entries, &count, 2, &truncated, &weak);
    openthread_l2_merge_network(entries, &count, 2, &truncated, &strong);

    assert(count == 1);
    assert(!truncated);
    assert(entries[0].rssi_dbm == -50);
    assert(entries[0].joinable);
    assert(entries[0].extended_address[0] == (uint8_t)(-50 & 0xff));
}

static void test_merge_bounds_and_key(void)
{
    openthread_network_t entries[1] = {0};
    size_t count = 0;
    bool truncated = false;
    openthread_network_t a = network(15, 0x1234, 0xaa, "mesh-a", -60, 100);
    openthread_network_t b = network(16, 0x1234, 0xaa, "mesh-a", -55, 100);

    openthread_l2_merge_network(entries, &count, 1, &truncated, &a);
    openthread_l2_merge_network(entries, &count, 1, &truncated, &b);

    assert(count == 1);
    assert(truncated);
    assert(entries[0].channel == 15);
    assert(!openthread_l2_network_same(&a, &b));
}

int main(void)
{
    test_dataset_bounds();
    test_merge_deduplicates_and_keeps_strongest();
    test_merge_bounds_and_key();
    puts("openthread_l2 host tests: PASS");
    return 0;
}
