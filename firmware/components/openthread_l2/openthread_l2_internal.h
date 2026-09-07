#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openthread_l2.h"

bool openthread_l2_dataset_length_valid(uint16_t length);
bool openthread_l2_network_same(const openthread_network_t *a, const openthread_network_t *b);
void openthread_l2_merge_network(openthread_network_t *entries,
                                 size_t *count,
                                 size_t capacity,
                                 bool *truncated,
                                 const openthread_network_t *candidate);
