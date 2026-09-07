#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nmap_l2.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nmap_internal_timing_validate(const nmap_timing_policy_t *policy);
void nmap_internal_classify_connect_error(int socket_errno, nmap_port_state_t *state, nmap_reason_t *reason);
bool nmap_internal_ipv4_is_unicast(uint32_t addr_be);
int nmap_internal_netmask_prefix(uint32_t netmask_be);
nmap_service_probe_profile_t nmap_internal_service_profile_for_port(nmap_service_probe_profile_t requested, uint16_t port);
void nmap_internal_service_match(const uint8_t *data, size_t len, bool truncated, bool active_http_probe, nmap_service_result_t *out);

#ifdef __cplusplus
}
#endif
