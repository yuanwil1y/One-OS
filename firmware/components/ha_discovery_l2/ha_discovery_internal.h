#pragma once

#include "ha_discovery.h"

#include <stddef.h>
#include <stdint.h>

ha_discovery_status_t ha_mdns_parse_packet(
    const uint8_t *packet,
    size_t packet_len,
    ha_mdns_service_t *services,
    size_t service_capacity,
    size_t *service_count,
    char service_types[][HA_MDNS_SERVICE_TYPE_LEN],
    size_t service_type_capacity,
    size_t *service_type_count,
    bool *truncated);

size_t ha_mdns_build_ptr_query(const char *name, uint8_t *out, size_t out_capacity);

ha_discovery_status_t ha_ssdp_parse_datagram(
    const char *data,
    size_t data_len,
    ha_ssdp_service_t *out);
