#include "ha_discovery_internal.h"

#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define MDNS_PACKET_BYTES 768u
#define MDNS_RECV_SLICE_MS 150u
#define MDNS_DEFAULT_TIMEOUT_MS 1800u
#define MDNS_MAX_TIMEOUT_MS 10000u

static const char *const MDNS_META_SERVICE = "_services._dns-sd._udp.local.";

static bool cancelled(const ha_mdns_discover_options_t *options)
{
    return options != NULL && options->cancelled != NULL && options->cancelled(options->cancel_context);
}

static uint32_t elapsed_ms(TickType_t start)
{
    return (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
}

static bool send_ptr_query(int sock, const struct sockaddr_in *target, const char *name)
{
    uint8_t query[256];
    const size_t query_len = ha_mdns_build_ptr_query(name, query, sizeof(query));
    if (query_len == 0) return false;
    return sendto(sock, query, query_len, 0, (const struct sockaddr *)target, sizeof(*target)) >= 0;
}

ha_discovery_status_t ha_mdns_discover_once(
    const ha_mdns_discover_options_t *options,
    ha_mdns_service_t *services,
    size_t service_capacity,
    size_t *service_count)
{
    if (options == NULL || service_count == NULL || (service_capacity > 0 && services == NULL) ||
        options->service_type_count > HA_MDNS_MAX_SERVICE_TYPES ||
        (options->service_type_count > 0 && options->service_types == NULL)) {
        return HA_DISCOVERY_INVALID_ARGUMENT;
    }
    *service_count = 0;
    if (service_capacity > 0) memset(services, 0, service_capacity * sizeof(services[0]));

    const uint32_t timeout_ms = options->timeout_ms == 0 ? MDNS_DEFAULT_TIMEOUT_MS : options->timeout_ms;
    if (timeout_ms > MDNS_MAX_TIMEOUT_MS) return HA_DISCOVERY_INVALID_ARGUMENT;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return HA_DISCOVERY_NETWORK_ERROR;

    int reuse = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = (int)MDNS_RECV_SLICE_MS * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(sock);
        return HA_DISCOVERY_NETWORK_ERROR;
    }

    struct ip_mreq membership = {
        .imr_multiaddr.s_addr = inet_addr(MDNS_GROUP),
        .imr_interface.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
        close(sock);
        return HA_DISCOVERY_NETWORK_ERROR;
    }

    const uint8_t ttl = 255;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = inet_addr(MDNS_GROUP),
    };

    char service_types[HA_MDNS_MAX_SERVICE_TYPES][HA_MDNS_SERVICE_TYPE_LEN] = {{0}};
    bool queried[HA_MDNS_MAX_SERVICE_TYPES] = {0};
    size_t service_type_count = 0;
    bool any_truncated = false;

    for (size_t i = 0; i < options->service_type_count; ++i) {
        const char *type = options->service_types[i];
        if (type == NULL || type[0] == '\0') {
            close(sock);
            return HA_DISCOVERY_INVALID_ARGUMENT;
        }
        bool duplicate = false;
        for (size_t j = 0; j < service_type_count; ++j) {
            if (strcmp(service_types[j], type) == 0) duplicate = true;
        }
        if (duplicate) continue;
        const size_t len = strlen(type);
        if (len >= HA_MDNS_SERVICE_TYPE_LEN) {
            close(sock);
            return HA_DISCOVERY_INVALID_ARGUMENT;
        }
        memcpy(service_types[service_type_count], type, len + 1);
        ++service_type_count;
    }

    if (service_type_count == 0) {
        if (!send_ptr_query(sock, &target, MDNS_META_SERVICE)) {
            close(sock);
            return HA_DISCOVERY_NETWORK_ERROR;
        }
    } else {
        for (size_t i = 0; i < service_type_count; ++i) {
            if (!send_ptr_query(sock, &target, service_types[i])) {
                close(sock);
                return HA_DISCOVERY_NETWORK_ERROR;
            }
            queried[i] = true;
        }
    }

    const TickType_t start = xTaskGetTickCount();
    uint8_t packet[MDNS_PACKET_BYTES];
    while (elapsed_ms(start) < timeout_ms) {
        if (cancelled(options)) {
            close(sock);
            return HA_DISCOVERY_CANCELLED;
        }

        const ssize_t received = recvfrom(sock, packet, sizeof(packet), 0, NULL, NULL);
        if (received > 0) {
            if ((size_t)received == sizeof(packet)) any_truncated = true;
            const size_t old_type_count = service_type_count;
            const ha_discovery_status_t parsed = ha_mdns_parse_packet(
                packet,
                (size_t)received,
                services,
                service_capacity,
                service_count,
                service_types,
                HA_MDNS_MAX_SERVICE_TYPES,
                &service_type_count,
                &any_truncated);
            if (parsed == HA_DISCOVERY_MALFORMED) {
                continue;
            }
            if (parsed == HA_DISCOVERY_INVALID_ARGUMENT) {
                close(sock);
                return parsed;
            }
            for (size_t i = old_type_count; i < service_type_count; ++i) queried[i] = false;
            for (size_t i = 0; i < service_type_count; ++i) {
                if (!queried[i] && service_types[i][0] != '\0') {
                    if (send_ptr_query(sock, &target, service_types[i])) queried[i] = true;
                }
            }
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(sock);
            return HA_DISCOVERY_NETWORK_ERROR;
        }
    }

    close(sock);

    size_t write = 0;
    for (size_t read = 0; read < *service_count; ++read) {
        if (services[read].service_type[0] == '\0' || services[read].instance[0] == '\0') continue;
        if (write != read) services[write] = services[read];
        ++write;
    }
    *service_count = write;
    return any_truncated ? HA_DISCOVERY_TRUNCATED : HA_DISCOVERY_OK;
}
