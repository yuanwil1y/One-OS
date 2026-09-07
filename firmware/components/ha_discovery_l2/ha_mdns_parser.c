#include "ha_discovery_internal.h"

#include <stdio.h>
#include <string.h>

#define DNS_HEADER_LEN 12u
#define DNS_TYPE_A 1u
#define DNS_TYPE_PTR 12u
#define DNS_TYPE_TXT 16u
#define DNS_TYPE_AAAA 28u
#define DNS_TYPE_SRV 33u
#define DNS_CLASS_IN 1u
#define DNS_MAX_NAME 256u
#define DNS_MAX_RR 40u
#define DNS_MAX_POINTER_HOPS 16u

static const char *const MDNS_META_SERVICE = "_services._dns-sd._udp.local.";

typedef struct {
    char owner[DNS_MAX_NAME];
    uint16_t type;
    uint16_t klass;
    uint32_t ttl;
    size_t rdata_offset;
    uint16_t rdlength;
} rr_view_t;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool copy_bounded(char *dst, size_t capacity, const char *src, bool *truncated)
{
    if (dst == NULL || capacity == 0 || src == NULL) return false;
    const size_t len = strlen(src);
    const size_t copy_len = len < capacity - 1 ? len : capacity - 1;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
    if (copy_len != len && truncated != NULL) *truncated = true;
    return copy_len == len;
}

static bool dns_read_name(
    const uint8_t *packet,
    size_t packet_len,
    size_t *offset,
    char *out,
    size_t out_capacity)
{
    if (packet == NULL || offset == NULL || out == NULL || out_capacity < 2 ||
        *offset >= packet_len) {
        return false;
    }

    size_t pos = *offset;
    size_t consumed_end = pos;
    size_t out_len = 0;
    size_t pointer_hops = 0;
    bool jumped = false;

    for (;;) {
        if (pos >= packet_len) return false;
        const uint8_t len = packet[pos];
        if ((len & 0xC0u) == 0xC0u) {
            if (pos + 1 >= packet_len || ++pointer_hops > DNS_MAX_POINTER_HOPS) return false;
            const uint16_t pointer = (uint16_t)(((len & 0x3Fu) << 8) | packet[pos + 1]);
            if (pointer >= packet_len) return false;
            if (!jumped) consumed_end = pos + 2;
            pos = pointer;
            jumped = true;
            continue;
        }
        if ((len & 0xC0u) != 0 || len > 63u) return false;
        ++pos;
        if (len == 0) {
            if (!jumped) consumed_end = pos;
            break;
        }
        if (pos + len > packet_len) return false;
        if (out_len != 0) {
            if (out_len + 1 >= out_capacity) return false;
            out[out_len++] = '.';
        }
        if (out_len + len + 1 >= out_capacity) return false;
        for (uint8_t i = 0; i < len; ++i) {
            const uint8_t c = packet[pos + i];
            out[out_len++] = c == '\0' ? '?' : (char)c;
        }
        pos += len;
        if (!jumped) consumed_end = pos;
    }

    if (out_len + 2 > out_capacity) return false;
    if (out_len != 0) out[out_len++] = '.';
    out[out_len] = '\0';
    *offset = consumed_end;
    return true;
}

static bool skip_question(const uint8_t *packet, size_t packet_len, size_t *offset)
{
    char ignored[DNS_MAX_NAME];
    if (!dns_read_name(packet, packet_len, offset, ignored, sizeof(ignored))) return false;
    if (*offset + 4 > packet_len) return false;
    *offset += 4;
    return true;
}

static bool parse_rr(const uint8_t *packet, size_t packet_len, size_t *offset, rr_view_t *rr)
{
    if (!dns_read_name(packet, packet_len, offset, rr->owner, sizeof(rr->owner))) return false;
    if (*offset + 10 > packet_len) return false;
    rr->type = be16(packet + *offset);
    rr->klass = (uint16_t)(be16(packet + *offset + 2) & 0x7FFFu);
    rr->ttl = be32(packet + *offset + 4);
    rr->rdlength = be16(packet + *offset + 8);
    *offset += 10;
    if (*offset + rr->rdlength > packet_len) return false;
    rr->rdata_offset = *offset;
    *offset += rr->rdlength;
    return true;
}

static ha_mdns_service_t *find_service(
    ha_mdns_service_t *services, size_t service_count, const char *instance)
{
    for (size_t i = 0; i < service_count; ++i) {
        if (strcmp(services[i].instance, instance) == 0) return &services[i];
    }
    return NULL;
}

static void infer_service_type(const char *instance, char *out, size_t out_capacity, bool *truncated)
{
    const char *suffix = strstr(instance, "._");
    if (suffix != NULL) copy_bounded(out, out_capacity, suffix + 1, truncated);
}

static ha_mdns_service_t *ensure_service(
    ha_mdns_service_t *services,
    size_t service_capacity,
    size_t *service_count,
    const char *instance,
    bool *truncated)
{
    if (strlen(instance) >= HA_MDNS_INSTANCE_LEN) {
        if (truncated != NULL) *truncated = true;
        return NULL;
    }
    ha_mdns_service_t *service = find_service(services, *service_count, instance);
    if (service != NULL) return service;
    if (*service_count >= service_capacity) {
        if (truncated != NULL) *truncated = true;
        return NULL;
    }
    service = &services[(*service_count)++];
    memset(service, 0, sizeof(*service));
    copy_bounded(service->instance, sizeof(service->instance), instance, &service->truncated);
    infer_service_type(instance, service->service_type, sizeof(service->service_type), &service->truncated);
    if (service->truncated && truncated != NULL) *truncated = true;
    return service;
}

static void add_service_type(
    char service_types[][HA_MDNS_SERVICE_TYPE_LEN],
    size_t capacity,
    size_t *count,
    const char *service_type,
    bool *truncated)
{
    if (service_types == NULL || count == NULL || capacity == 0) return;
    if (strlen(service_type) >= HA_MDNS_SERVICE_TYPE_LEN) {
        if (truncated != NULL) *truncated = true;
        return;
    }
    for (size_t i = 0; i < *count; ++i) {
        if (strcmp(service_types[i], service_type) == 0) return;
    }
    if (*count >= capacity) {
        if (truncated != NULL) *truncated = true;
        return;
    }
    copy_bounded(service_types[*count], HA_MDNS_SERVICE_TYPE_LEN, service_type, truncated);
    ++*count;
}

static void add_txt(
    ha_mdns_service_t *service,
    const uint8_t *data,
    size_t len,
    bool *truncated)
{
    if (service->txt_count >= HA_MDNS_MAX_TXT) {
        service->truncated = true;
        if (truncated != NULL) *truncated = true;
        return;
    }

    ha_mdns_txt_t *txt = &service->txt[service->txt_count++];
    memset(txt, 0, sizeof(*txt));
    size_t eq = len;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '=') {
            eq = i;
            break;
        }
    }
    txt->has_value = eq < len;
    const size_t key_len = eq;
    const size_t value_start = txt->has_value ? eq + 1 : len;
    const size_t value_len = len - value_start;

    const size_t key_copy = key_len < sizeof(txt->key) - 1 ? key_len : sizeof(txt->key) - 1;
    const size_t val_copy = value_len < sizeof(txt->value) - 1 ? value_len : sizeof(txt->value) - 1;
    for (size_t i = 0; i < key_copy; ++i) {
        const uint8_t c = data[i];
        txt->key[i] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '?';
    }
    for (size_t i = 0; i < val_copy; ++i) {
        const uint8_t c = data[value_start + i];
        txt->value[i] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '?';
    }
    txt->key[key_copy] = '\0';
    txt->value[val_copy] = '\0';
    if (key_copy != key_len || val_copy != value_len) {
        service->truncated = true;
        if (truncated != NULL) *truncated = true;
    }
}

static void add_address(ha_mdns_service_t *service, const char *address, bool *truncated)
{
    for (size_t i = 0; i < service->address_count; ++i) {
        if (strcmp(service->addresses[i], address) == 0) return;
    }
    if (service->address_count >= HA_MDNS_MAX_ADDRESSES) {
        service->truncated = true;
        if (truncated != NULL) *truncated = true;
        return;
    }
    copy_bounded(
        service->addresses[service->address_count],
        HA_MDNS_ADDRESS_LEN,
        address,
        &service->truncated);
    ++service->address_count;
    if (service->truncated && truncated != NULL) *truncated = true;
}

static void format_ipv4(const uint8_t *p, char *out, size_t out_capacity)
{
    (void)snprintf(out, out_capacity, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

static void format_ipv6(const uint8_t *p, char *out, size_t out_capacity)
{
    (void)snprintf(
        out,
        out_capacity,
        "%x:%x:%x:%x:%x:%x:%x:%x",
        be16(p), be16(p + 2), be16(p + 4), be16(p + 6),
        be16(p + 8), be16(p + 10), be16(p + 12), be16(p + 14));
}

ha_discovery_status_t ha_mdns_parse_packet(
    const uint8_t *packet,
    size_t packet_len,
    ha_mdns_service_t *services,
    size_t service_capacity,
    size_t *service_count,
    char service_types[][HA_MDNS_SERVICE_TYPE_LEN],
    size_t service_type_capacity,
    size_t *service_type_count,
    bool *truncated)
{
    if (packet == NULL || packet_len < DNS_HEADER_LEN || service_count == NULL ||
        service_type_count == NULL || truncated == NULL ||
        (service_capacity > 0 && services == NULL) ||
        (service_type_capacity > 0 && service_types == NULL) ||
        *service_count > service_capacity || *service_type_count > service_type_capacity) {
        return HA_DISCOVERY_INVALID_ARGUMENT;
    }

    const uint16_t qdcount = be16(packet + 4);
    const uint32_t rr_total = (uint32_t)be16(packet + 6) + be16(packet + 8) + be16(packet + 10);
    size_t offset = DNS_HEADER_LEN;
    for (uint16_t i = 0; i < qdcount; ++i) {
        if (!skip_question(packet, packet_len, &offset)) return HA_DISCOVERY_MALFORMED;
    }

    rr_view_t records[DNS_MAX_RR];
    size_t record_count = 0;
    for (uint32_t i = 0; i < rr_total; ++i) {
        rr_view_t rr;
        if (!parse_rr(packet, packet_len, &offset, &rr)) return HA_DISCOVERY_MALFORMED;
        if (record_count < DNS_MAX_RR) {
            records[record_count++] = rr;
        } else {
            *truncated = true;
        }
    }

    for (size_t i = 0; i < record_count; ++i) {
        const rr_view_t *rr = &records[i];
        if (rr->klass != DNS_CLASS_IN || rr->type != DNS_TYPE_PTR || rr->ttl == 0) continue;
        size_t target_offset = rr->rdata_offset;
        char target[DNS_MAX_NAME];
        if (!dns_read_name(packet, packet_len, &target_offset, target, sizeof(target)) ||
            target_offset > rr->rdata_offset + rr->rdlength) {
            return HA_DISCOVERY_MALFORMED;
        }
        if (strcmp(rr->owner, MDNS_META_SERVICE) == 0) {
            add_service_type(service_types, service_type_capacity, service_type_count, target, truncated);
            continue;
        }
        add_service_type(service_types, service_type_capacity, service_type_count, rr->owner, truncated);
        ha_mdns_service_t *service = ensure_service(
            services, service_capacity, service_count, target, truncated);
        if (service != NULL) {
            copy_bounded(service->service_type, sizeof(service->service_type), rr->owner, &service->truncated);
            if (service->truncated) *truncated = true;
        }
    }

    for (size_t i = 0; i < record_count; ++i) {
        const rr_view_t *rr = &records[i];
        if (rr->klass != DNS_CLASS_IN || rr->ttl == 0) continue;
        if (rr->type == DNS_TYPE_SRV) {
            if (rr->rdlength < 6) return HA_DISCOVERY_MALFORMED;
            ha_mdns_service_t *service = ensure_service(
                services, service_capacity, service_count, rr->owner, truncated);
            if (service == NULL) continue;
            service->port = be16(packet + rr->rdata_offset + 4);
            size_t target_offset = rr->rdata_offset + 6;
            char host[DNS_MAX_NAME];
            if (!dns_read_name(packet, packet_len, &target_offset, host, sizeof(host)) ||
                target_offset > rr->rdata_offset + rr->rdlength) {
                return HA_DISCOVERY_MALFORMED;
            }
            copy_bounded(service->hostname, sizeof(service->hostname), host, &service->truncated);
            if (service->truncated) *truncated = true;
        } else if (rr->type == DNS_TYPE_TXT) {
            ha_mdns_service_t *service = ensure_service(
                services, service_capacity, service_count, rr->owner, truncated);
            if (service == NULL) continue;
            size_t pos = rr->rdata_offset;
            const size_t end = rr->rdata_offset + rr->rdlength;
            while (pos < end) {
                const uint8_t item_len = packet[pos++];
                if (pos + item_len > end) return HA_DISCOVERY_MALFORMED;
                add_txt(service, packet + pos, item_len, truncated);
                pos += item_len;
            }
        }
    }

    for (size_t i = 0; i < record_count; ++i) {
        const rr_view_t *rr = &records[i];
        if (rr->klass != DNS_CLASS_IN || rr->ttl == 0) continue;
        char address[HA_MDNS_ADDRESS_LEN];
        if (rr->type == DNS_TYPE_A && rr->rdlength == 4) {
            format_ipv4(packet + rr->rdata_offset, address, sizeof(address));
        } else if (rr->type == DNS_TYPE_AAAA && rr->rdlength == 16) {
            format_ipv6(packet + rr->rdata_offset, address, sizeof(address));
        } else {
            continue;
        }
        for (size_t j = 0; j < *service_count; ++j) {
            if (services[j].hostname[0] != '\0' && strcmp(services[j].hostname, rr->owner) == 0) {
                add_address(&services[j], address, truncated);
            }
        }
    }

    return *truncated ? HA_DISCOVERY_TRUNCATED : HA_DISCOVERY_OK;
}

static bool write_name(const char *name, uint8_t *out, size_t capacity, size_t *offset)
{
    if (name == NULL || out == NULL || offset == NULL || name[0] == '\0') return false;
    const char *p = name;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        const size_t len = dot == NULL ? strlen(p) : (size_t)(dot - p);
        if (len == 0) {
            if (dot != NULL && dot[1] == '\0') break;
            return false;
        }
        if (len > 63 || *offset + 1 + len >= capacity) return false;
        out[(*offset)++] = (uint8_t)len;
        memcpy(out + *offset, p, len);
        *offset += len;
        if (dot == NULL) break;
        p = dot + 1;
        if (*p == '\0') break;
    }
    if (*offset + 1 > capacity) return false;
    out[(*offset)++] = 0;
    return true;
}

size_t ha_mdns_build_ptr_query(const char *name, uint8_t *out, size_t out_capacity)
{
    if (name == NULL || out == NULL || out_capacity < DNS_HEADER_LEN + 6) return 0;
    memset(out, 0, out_capacity);
    out[5] = 1;
    size_t offset = DNS_HEADER_LEN;
    if (!write_name(name, out, out_capacity, &offset) || offset + 4 > out_capacity) return 0;
    out[offset++] = 0;
    out[offset++] = DNS_TYPE_PTR;
    out[offset++] = 0;
    out[offset++] = DNS_CLASS_IN;
    return offset;
}
