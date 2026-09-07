#include "nmap_internal.h"

#include <errno.h>

#include "lwip/def.h"

nmap_timing_policy_t nmap_timing_policy_default(void)
{
    const nmap_timing_policy_t policy = {
        .max_inflight = 4,
        .max_retries = 1,
        .max_probes_per_second = 20,
        .probe_timeout_ms = 400,
        .scan_timeout_ms = 30000,
    };
    return policy;
}

esp_err_t nmap_internal_timing_validate(const nmap_timing_policy_t *policy)
{
    if (policy == NULL) return ESP_ERR_INVALID_ARG;
    if (policy->max_inflight == 0 || policy->max_inflight > NMAP_MAX_CONCURRENCY) return ESP_ERR_INVALID_ARG;
    if (policy->max_retries > NMAP_MAX_RETRIES) return ESP_ERR_INVALID_ARG;
    if (policy->max_probes_per_second == 0 || policy->max_probes_per_second > 50) return ESP_ERR_INVALID_ARG;
    if (policy->probe_timeout_ms < 50 || policy->probe_timeout_ms > 3000) return ESP_ERR_INVALID_ARG;
    if (policy->scan_timeout_ms < 100 || policy->scan_timeout_ms > 120000) return ESP_ERR_INVALID_ARG;
    if (policy->scan_timeout_ms < policy->probe_timeout_ms) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

void nmap_internal_classify_connect_error(int socket_errno, nmap_port_state_t *state, nmap_reason_t *reason)
{
    if (state == NULL || reason == NULL) return;
    switch (socket_errno) {
    case 0:
        *state = NMAP_PORT_OPEN;
        *reason = NMAP_REASON_CONNECTION_ACCEPTED;
        break;
    case ECONNREFUSED:
        *state = NMAP_PORT_CLOSED;
        *reason = NMAP_REASON_CONNECTION_REFUSED;
        break;
    case ETIMEDOUT:
#if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
#endif
    case EAGAIN:
        *state = NMAP_PORT_FILTERED;
        *reason = NMAP_REASON_TIMEOUT;
        break;
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
    case EHOSTUNREACH:
        *state = NMAP_PORT_UNREACHABLE;
        *reason = NMAP_REASON_HOST_UNREACHABLE;
        break;
#ifdef ENETDOWN
    case ENETDOWN:
#endif
    case ENETUNREACH:
        *state = NMAP_PORT_UNREACHABLE;
        *reason = NMAP_REASON_NETWORK_UNREACHABLE;
        break;
#ifdef ENOBUFS
    case ENOBUFS:
#endif
    case ENOMEM:
#ifdef EMFILE
    case EMFILE:
#endif
#ifdef ENFILE
    case ENFILE:
#endif
        *state = NMAP_PORT_ERROR;
        *reason = NMAP_REASON_LOCAL_RESOURCE;
        break;
    default:
        *state = NMAP_PORT_ERROR;
        *reason = NMAP_REASON_IO_ERROR;
        break;
    }
}

bool nmap_internal_ipv4_is_unicast(uint32_t addr_be)
{
    const uint32_t host = lwip_ntohl(addr_be);
    const uint8_t first = (uint8_t)(host >> 24);
    if (host == 0 || host == 0xffffffffu) return false;
    if (first == 0 || first >= 224) return false;
    return true;
}

int nmap_internal_netmask_prefix(uint32_t netmask_be)
{
    uint32_t mask = lwip_ntohl(netmask_be);
    int prefix = 0;
    bool saw_zero = false;
    for (int bit = 31; bit >= 0; --bit) {
        const bool one = (mask & (1u << bit)) != 0;
        if (one) {
            if (saw_zero) return -1;
            ++prefix;
        } else {
            saw_zero = true;
        }
    }
    return prefix;
}
