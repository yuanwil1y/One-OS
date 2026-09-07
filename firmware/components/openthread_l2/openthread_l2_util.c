#include "openthread_l2_internal.h"

#include <string.h>

bool openthread_l2_dataset_length_valid(uint16_t length)
{
    return length > 0u && length <= OPENTHREAD_DATASET_TLVS_MAX;
}

bool openthread_l2_network_same(const openthread_network_t *a, const openthread_network_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    return a->channel == b->channel &&
           a->pan_id == b->pan_id &&
           memcmp(a->extended_pan_id, b->extended_pan_id, sizeof(a->extended_pan_id)) == 0 &&
           strncmp(a->network_name, b->network_name, OPENTHREAD_NETWORK_NAME_MAX + 1u) == 0;
}

void openthread_l2_merge_network(openthread_network_t *entries,
                                 size_t *count,
                                 size_t capacity,
                                 bool *truncated,
                                 const openthread_network_t *candidate)
{
    if (entries == NULL || count == NULL || truncated == NULL || candidate == NULL) {
        return;
    }

    for (size_t i = 0; i < *count; ++i) {
        if (!openthread_l2_network_same(&entries[i], candidate)) {
            continue;
        }
        if (candidate->rssi_dbm > entries[i].rssi_dbm ||
            (candidate->rssi_dbm == entries[i].rssi_dbm && candidate->lqi > entries[i].lqi)) {
            entries[i] = *candidate;
        } else {
            entries[i].joinable = entries[i].joinable || candidate->joinable;
            entries[i].native_commissioner = entries[i].native_commissioner || candidate->native_commissioner;
        }
        return;
    }

    if (*count >= capacity) {
        *truncated = true;
        return;
    }

    entries[*count] = *candidate;
    ++(*count);
}
