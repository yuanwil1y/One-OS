#include "ha_core.h"
#include "ha_discovery_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return false; \
    } \
} while (0)

typedef struct {
    uint8_t data[1024];
    size_t len;
} dns_buf_t;

static void dns_put_u8(dns_buf_t *b, uint8_t v)
{
    b->data[b->len++] = v;
}

static void dns_put_u16(dns_buf_t *b, uint16_t v)
{
    dns_put_u8(b, (uint8_t)(v >> 8));
    dns_put_u8(b, (uint8_t)v);
}

static void dns_put_u32(dns_buf_t *b, uint32_t v)
{
    dns_put_u8(b, (uint8_t)(v >> 24));
    dns_put_u8(b, (uint8_t)(v >> 16));
    dns_put_u8(b, (uint8_t)(v >> 8));
    dns_put_u8(b, (uint8_t)v);
}

static bool dns_put_name(dns_buf_t *b, const char *name)
{
    const char *p = name;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        const size_t part = dot == NULL ? strlen(p) : (size_t)(dot - p);
        if (part == 0) {
            if (dot != NULL && dot[1] == '\0') break;
            return false;
        }
        if (part > 63 || b->len + 1 + part >= sizeof(b->data)) return false;
        dns_put_u8(b, (uint8_t)part);
        memcpy(b->data + b->len, p, part);
        b->len += part;
        if (dot == NULL) break;
        p = dot + 1;
        if (*p == '\0') break;
    }
    if (b->len >= sizeof(b->data)) return false;
    dns_put_u8(b, 0);
    return true;
}

static size_t dns_rr_begin(dns_buf_t *b, const char *owner, uint16_t type)
{
    if (!dns_put_name(b, owner)) return SIZE_MAX;
    dns_put_u16(b, type);
    dns_put_u16(b, 1);
    dns_put_u32(b, 120);
    const size_t length_pos = b->len;
    dns_put_u16(b, 0);
    return length_pos;
}

static bool dns_rr_end(dns_buf_t *b, size_t length_pos)
{
    if (length_pos == SIZE_MAX || length_pos + 2 > b->len) return false;
    const size_t rdata_len = b->len - length_pos - 2;
    if (rdata_len > UINT16_MAX) return false;
    b->data[length_pos] = (uint8_t)(rdata_len >> 8);
    b->data[length_pos + 1] = (uint8_t)rdata_len;
    return true;
}

static bool build_mdns_fixture(dns_buf_t *b)
{
    memset(b, 0, sizeof(*b));
    b->len = 12;
    b->data[2] = 0x84;
    b->data[7] = 4;

    size_t lp = dns_rr_begin(b, "_http._tcp.local.", 12);
    CHECK(lp != SIZE_MAX);
    CHECK(dns_put_name(b, "Device._http._tcp.local."));
    CHECK(dns_rr_end(b, lp));

    lp = dns_rr_begin(b, "Device._http._tcp.local.", 33);
    CHECK(lp != SIZE_MAX);
    dns_put_u16(b, 0);
    dns_put_u16(b, 0);
    dns_put_u16(b, 80);
    CHECK(dns_put_name(b, "device.local."));
    CHECK(dns_rr_end(b, lp));

    lp = dns_rr_begin(b, "Device._http._tcp.local.", 16);
    CHECK(lp != SIZE_MAX);
    static const char txt[] = "id=abc";
    dns_put_u8(b, (uint8_t)(sizeof(txt) - 1));
    memcpy(b->data + b->len, txt, sizeof(txt) - 1);
    b->len += sizeof(txt) - 1;
    CHECK(dns_rr_end(b, lp));

    lp = dns_rr_begin(b, "device.local.", 1);
    CHECK(lp != SIZE_MAX);
    dns_put_u8(b, 192);
    dns_put_u8(b, 168);
    dns_put_u8(b, 1);
    dns_put_u8(b, 42);
    CHECK(dns_rr_end(b, lp));
    return true;
}

static ha_core_status_t service_handler(
    const char *entity_id,
    const char *service,
    const ha_attribute_t *data,
    size_t data_count,
    void *ctx)
{
    (void)data;
    (void)data_count;
    int *calls = (int *)ctx;
    if (strcmp(entity_id, "switch.plug") != 0 || strcmp(service, HA_SERVICE_TURN_ON) != 0) {
        return HA_CORE_UNSUPPORTED;
    }
    ++*calls;
    return HA_CORE_OK;
}

static bool test_ha_core_semantics(void)
{
    ha_core_reset();
    const uint32_t rev0 = ha_core_revision();
    CHECK(rev0 != 0);

    ha_device_t unknown = {0};
    snprintf(unknown.id, sizeof(unknown.id), "unknown-1");
    snprintf(unknown.name, sizeof(unknown.name), "Unknown LAN Device");
    CHECK(ha_core_device_upsert(&unknown) == HA_CORE_OK);
    CHECK(ha_core_device_count() == 1);
    CHECK(ha_core_device_get("unknown-1") != NULL);
    CHECK(ha_core_device_at(0) != NULL);
    CHECK(ha_core_revision() != rev0);

    ha_device_t dev = {0};
    snprintf(dev.id, sizeof(dev.id), "dev-1");
    dev.identifier_count = 1;
    snprintf(dev.identifiers[0].domain, sizeof(dev.identifiers[0].domain), "fixture");
    snprintf(dev.identifiers[0].value, sizeof(dev.identifiers[0].value), "stable-1");
    snprintf(dev.name, sizeof(dev.name), "Fixture Plug");
    CHECK(ha_core_device_upsert(&dev) == HA_CORE_OK);

    ha_device_t conflict = dev;
    snprintf(conflict.id, sizeof(conflict.id), "dev-2");
    CHECK(ha_core_device_upsert(&conflict) == HA_CORE_CONFLICT);

    int calls = 0;
    ha_entity_t entity = {0};
    snprintf(entity.entity_id, sizeof(entity.entity_id), "switch.plug");
    snprintf(entity.unique_id, sizeof(entity.unique_id), "plug-1");
    snprintf(entity.platform, sizeof(entity.platform), "fixture");
    snprintf(entity.domain, sizeof(entity.domain), HA_DOMAIN_SWITCH);
    snprintf(entity.device_id, sizeof(entity.device_id), "dev-1");
    snprintf(entity.name, sizeof(entity.name), "Outlet");
    entity.enabled = true;
    entity.available = true;
    entity.supported_services = HA_SERVICE_MASK_TURN_ON | HA_SERVICE_MASK_TURN_OFF;
    entity.service_handler = service_handler;
    entity.service_context = &calls;
    CHECK(ha_core_entity_upsert(&entity) == HA_CORE_OK);
    CHECK(ha_core_entity_count_for_device("dev-1") == 1);
    CHECK(ha_core_entity_at_for_device("dev-1", 0) != NULL);

    ha_entity_t duplicate = entity;
    snprintf(duplicate.entity_id, sizeof(duplicate.entity_id), "switch.other");
    CHECK(ha_core_entity_upsert(&duplicate) == HA_CORE_CONFLICT);

    ha_entity_t missing_device = entity;
    snprintf(missing_device.entity_id, sizeof(missing_device.entity_id), "switch.missing");
    snprintf(missing_device.unique_id, sizeof(missing_device.unique_id), "missing-1");
    snprintf(missing_device.device_id, sizeof(missing_device.device_id), "does-not-exist");
    CHECK(ha_core_entity_upsert(&missing_device) == HA_CORE_NOT_FOUND);

    ha_attribute_t attr = {0};
    snprintf(attr.key, sizeof(attr.key), "source");
    snprintf(attr.value, sizeof(attr.value), "fixture");
    CHECK(ha_core_state_set("switch.plug", HA_STATE_OFF, &attr, 1) == HA_CORE_OK);
    const ha_state_t *state = ha_core_state_get("switch.plug");
    CHECK(state != NULL);
    CHECK(strcmp(state->state, HA_STATE_OFF) == 0);
    CHECK(state->attribute_count == 1);
    CHECK(strcmp(state->attributes[0].key, "source") == 0);
    CHECK(ha_core_state_set("switch.none", HA_STATE_ON, NULL, 0) == HA_CORE_NOT_FOUND);
    CHECK(ha_core_state_set("switch.plug", HA_STATE_ON, &attr, HA_CORE_MAX_ATTRIBUTES + 1) == HA_CORE_INVALID_ARGUMENT);

    CHECK(ha_core_service_supports("switch.plug", HA_SERVICE_TURN_ON));
    CHECK(!ha_core_service_supports("switch.plug", HA_SERVICE_PRESS));
    CHECK(ha_core_service_call("switch.plug", HA_SERVICE_TURN_ON, NULL, 0) == HA_CORE_OK);
    CHECK(calls == 1);
    CHECK(ha_core_service_call("switch.plug", HA_SERVICE_PRESS, NULL, 0) == HA_CORE_UNSUPPORTED);

    ha_entity_t bad_id = entity;
    snprintf(bad_id.entity_id, sizeof(bad_id.entity_id), "switch.Bad");
    snprintf(bad_id.unique_id, sizeof(bad_id.unique_id), "bad-id");
    CHECK(ha_core_entity_upsert(&bad_id) == HA_CORE_INVALID_ARGUMENT);
    return true;
}

static bool test_ha_core_capacity(void)
{
    ha_core_reset();
    for (size_t i = 0; i < HA_CORE_MAX_DEVICES; ++i) {
        ha_device_t dev = {0};
        snprintf(dev.id, sizeof(dev.id), "generic-%u", (unsigned)i);
        CHECK(ha_core_device_upsert(&dev) == HA_CORE_OK);
    }
    CHECK(ha_core_device_count() == HA_CORE_MAX_DEVICES);
    CHECK(ha_core_device_at(HA_CORE_MAX_DEVICES) == NULL);
    ha_device_t extra = {0};
    snprintf(extra.id, sizeof(extra.id), "overflow");
    CHECK(ha_core_device_upsert(&extra) == HA_CORE_CAPACITY);
    return true;
}

static bool test_mdns_parser(void)
{
    dns_buf_t packet;
    CHECK(build_mdns_fixture(&packet));

    ha_mdns_service_t services[4] = {0};
    char service_types[8][HA_MDNS_SERVICE_TYPE_LEN] = {{0}};
    size_t service_count = 0;
    size_t service_type_count = 0;
    bool truncated = false;
    const ha_discovery_status_t status = ha_mdns_parse_packet(
        packet.data, packet.len,
        services, 4, &service_count,
        service_types, 8, &service_type_count,
        &truncated);
    CHECK(status == HA_DISCOVERY_OK);
    CHECK(!truncated);
    CHECK(service_count == 1);
    CHECK(service_type_count == 1);
    CHECK(strcmp(service_types[0], "_http._tcp.local.") == 0);
    CHECK(strcmp(services[0].service_type, "_http._tcp.local.") == 0);
    CHECK(strcmp(services[0].instance, "Device._http._tcp.local.") == 0);
    CHECK(strcmp(services[0].hostname, "device.local.") == 0);
    CHECK(services[0].port == 80);
    CHECK(services[0].address_count == 1);
    CHECK(strcmp(services[0].addresses[0], "192.168.1.42") == 0);
    CHECK(services[0].txt_count == 1);
    CHECK(strcmp(services[0].txt[0].key, "id") == 0);
    CHECK(strcmp(services[0].txt[0].value, "abc") == 0);
    CHECK(services[0].txt[0].has_value);

    uint8_t query[256] = {0};
    const size_t query_len = ha_mdns_build_ptr_query("_http._tcp.local.", query, sizeof(query));
    CHECK(query_len > 16);
    CHECK(query[4] == 0 && query[5] == 1);
    CHECK(query[query_len - 4] == 0 && query[query_len - 3] == 12);
    CHECK(query[query_len - 2] == 0 && query[query_len - 1] == 1);

    uint8_t bad[14] = {0};
    bad[7] = 1;
    bad[12] = 0xC0;
    bad[13] = 0x0C;
    service_count = 0;
    service_type_count = 0;
    truncated = false;
    CHECK(ha_mdns_parse_packet(
              bad, sizeof(bad), services, 4, &service_count,
              service_types, 8, &service_type_count, &truncated) == HA_DISCOVERY_MALFORMED);

    service_count = 0;
    service_type_count = 0;
    truncated = false;
    CHECK(ha_mdns_parse_packet(
              packet.data, packet.len, NULL, 0, &service_count,
              service_types, 8, &service_type_count, &truncated) == HA_DISCOVERY_TRUNCATED);
    CHECK(truncated);
    return true;
}

static bool test_ssdp_parser(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "sT: upnp:rootdevice\r\n"
        "UsN: uuid:fixture::upnp:rootdevice\r\n"
        "LoCaTiOn: http://192.168.1.2/device.xml\r\n"
        "SERVER: Fixture/1.0 UPnP/1.1\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "\r\n";
    ha_ssdp_service_t service;
    CHECK(ha_ssdp_parse_datagram(response, sizeof(response) - 1, &service) == HA_DISCOVERY_OK);
    CHECK(service.source == HA_SSDP_SOURCE_RESPONSE);
    CHECK(strcmp(service.st, "upnp:rootdevice") == 0);
    CHECK(strcmp(service.usn, "uuid:fixture::upnp:rootdevice") == 0);
    CHECK(strcmp(service.location, "http://192.168.1.2/device.xml") == 0);
    CHECK(strcmp(service.cache_control, "max-age=1800") == 0);

    static const char byebye[] =
        "NOTIFY * HTTP/1.1\r\n"
        "NT: upnp:rootdevice\r\n"
        "NTS: ssdp:byebye\r\n"
        "USN: uuid:fixture::upnp:rootdevice\r\n"
        "\r\n";
    CHECK(ha_ssdp_parse_datagram(byebye, sizeof(byebye) - 1, &service) == HA_DISCOVERY_OK);
    CHECK(service.source == HA_SSDP_SOURCE_BYEBYE);

    char long_location[260];
    memset(long_location, 'a', sizeof(long_location) - 1);
    long_location[sizeof(long_location) - 1] = '\0';
    char oversized[640];
    const int written = snprintf(
        oversized, sizeof(oversized),
        "HTTP/1.1 200 OK\r\nST: upnp:rootdevice\r\nUSN: uuid:fixture\r\nLOCATION: http://%s\r\n\r\n",
        long_location);
    CHECK(written > 0 && (size_t)written < sizeof(oversized));
    CHECK(ha_ssdp_parse_datagram(oversized, (size_t)written, &service) == HA_DISCOVERY_TRUNCATED);
    CHECK(service.truncated);
    CHECK(service.location[sizeof(service.location) - 1] == '\0');

    static const char malformed[] = "HTTP/1.1 200 OK\r\nBROKEN\r\n\r\n";
    CHECK(ha_ssdp_parse_datagram(malformed, sizeof(malformed) - 1, &service) == HA_DISCOVERY_MALFORMED);
    return true;
}

int main(void)
{
    CHECK(test_ha_core_semantics());
    CHECK(test_ha_core_capacity());
    CHECK(test_mdns_parser());
    CHECK(test_ssdp_parser());
    puts("ha_l2_host_tests: PASS");
    return 0;
}
