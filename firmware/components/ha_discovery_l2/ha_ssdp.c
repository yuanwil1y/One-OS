#include "ha_discovery_internal.h"

#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define SSDP_PORT 1900
#define SSDP_GROUP "239.255.255.250"
#define SSDP_PACKET_BYTES 1024u
#define SSDP_RECV_SLICE_MS 150u
#define SSDP_DEFAULT_TIMEOUT_MS 1600u
#define SSDP_MAX_TIMEOUT_MS 10000u

static const char SSDP_SEARCH[] =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 1\r\n"
    "ST: ssdp:all\r\n"
    "\r\n";

static bool cancelled(const ha_ssdp_discover_options_t *options)
{
    return options != NULL && options->cancelled != NULL && options->cancelled(options->cancel_context);
}

static uint32_t elapsed_ms(TickType_t start)
{
    return (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
}

static bool same_service(const ha_ssdp_service_t *a, const ha_ssdp_service_t *b)
{
    if (a->usn[0] != '\0' && b->usn[0] != '\0') {
        if (strcmp(a->usn, b->usn) != 0) return false;
        const char *a_type = a->st[0] != '\0' ? a->st : a->nt;
        const char *b_type = b->st[0] != '\0' ? b->st : b->nt;
        return strcmp(a_type, b_type) == 0;
    }
    return a->location[0] != '\0' && b->location[0] != '\0' &&
           strcmp(a->location, b->location) == 0;
}

ha_discovery_status_t ha_ssdp_discover_once(
    const ha_ssdp_discover_options_t *options,
    ha_ssdp_service_t *services,
    size_t service_capacity,
    size_t *service_count)
{
    if (options == NULL || service_count == NULL || (service_capacity > 0 && services == NULL)) {
        return HA_DISCOVERY_INVALID_ARGUMENT;
    }
    *service_count = 0;
    if (service_capacity > 0) memset(services, 0, service_capacity * sizeof(services[0]));

    const uint32_t timeout_ms = options->timeout_ms == 0 ? SSDP_DEFAULT_TIMEOUT_MS : options->timeout_ms;
    if (timeout_ms > SSDP_MAX_TIMEOUT_MS) return HA_DISCOVERY_INVALID_ARGUMENT;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return HA_DISCOVERY_NETWORK_ERROR;

    int broadcast = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = (int)SSDP_RECV_SLICE_MS * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(sock);
        return HA_DISCOVERY_NETWORK_ERROR;
    }

    struct sockaddr_in multicast = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
        .sin_addr.s_addr = inet_addr(SSDP_GROUP),
    };
    if (sendto(sock, SSDP_SEARCH, sizeof(SSDP_SEARCH) - 1, 0,
               (const struct sockaddr *)&multicast, sizeof(multicast)) < 0) {
        close(sock);
        return HA_DISCOVERY_NETWORK_ERROR;
    }

    struct sockaddr_in broadcast_target = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    (void)sendto(sock, SSDP_SEARCH, sizeof(SSDP_SEARCH) - 1, 0,
                 (const struct sockaddr *)&broadcast_target, sizeof(broadcast_target));

    const TickType_t start = xTaskGetTickCount();
    bool any_truncated = false;
    char packet[SSDP_PACKET_BYTES];
    while (elapsed_ms(start) < timeout_ms) {
        if (cancelled(options)) {
            close(sock);
            return HA_DISCOVERY_CANCELLED;
        }
        const ssize_t received = recvfrom(sock, packet, sizeof(packet), 0, NULL, NULL);
        if (received > 0) {
            if ((size_t)received == sizeof(packet)) any_truncated = true;
            ha_ssdp_service_t parsed;
            const ha_discovery_status_t status = ha_ssdp_parse_datagram(packet, (size_t)received, &parsed);
            if (status == HA_DISCOVERY_MALFORMED) continue;
            if (status == HA_DISCOVERY_TRUNCATED) any_truncated = true;
            if (status != HA_DISCOVERY_OK && status != HA_DISCOVERY_TRUNCATED) continue;

            size_t existing = *service_count;
            for (size_t i = 0; i < *service_count; ++i) {
                if (same_service(&services[i], &parsed)) {
                    existing = i;
                    break;
                }
            }
            if (existing < *service_count) {
                services[existing] = parsed;
            } else if (*service_count < service_capacity) {
                services[(*service_count)++] = parsed;
            } else {
                any_truncated = true;
            }
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(sock);
            return HA_DISCOVERY_NETWORK_ERROR;
        }
    }

    close(sock);
    return any_truncated ? HA_DISCOVERY_TRUNCATED : HA_DISCOVERY_OK;
}
