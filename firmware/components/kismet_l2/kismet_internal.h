#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kismet_ble.h"
#include "kismet_wifi.h"

void kismet_wifi_tracker_note_capture(kismet_wifi_tracker_t *tracker,
                                      uint32_t rx_drops,
                                      uint32_t malformed,
                                      uint32_t truncated);
void kismet_ble_tracker_note_capture(kismet_ble_tracker_t *tracker,
                                     uint32_t report_drops,
                                     uint32_t truncated);

typedef enum {
    KISMET_WIFI_SHALLOW_IGNORED = 0,
    KISMET_WIFI_SHALLOW_OK,
    KISMET_WIFI_SHALLOW_MALFORMED,
} kismet_wifi_shallow_result_t;

kismet_wifi_shallow_result_t kismet_wifi_shallow_observe(
    const uint8_t *frame,
    size_t frame_len,
    int8_t rssi,
    uint8_t channel,
    uint64_t seen_ms,
    kismet_wifi_observation_t *out_primary,
    kismet_wifi_observation_t *out_secondary);
