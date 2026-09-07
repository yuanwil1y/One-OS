#include "ha_discovery_internal.h"

#include <ctype.h>
#include <string.h>

static bool copy_span(char *dst, size_t capacity, const char *src, size_t len, bool *truncated)
{
    if (dst == NULL || capacity == 0 || src == NULL) return false;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) --len;
    while (len > 0 && (*src == ' ' || *src == '\t')) {
        ++src;
        --len;
    }
    const size_t copy_len = len < capacity - 1 ? len : capacity - 1;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
    if (copy_len != len && truncated != NULL) *truncated = true;
    return copy_len == len;
}

static bool name_equals(const char *name, size_t len, const char *expected)
{
    const size_t expected_len = strlen(expected);
    if (len != expected_len) return false;
    for (size_t i = 0; i < len; ++i) {
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)expected[i])) return false;
    }
    return true;
}

static bool value_equals_ci(const char *value, const char *expected)
{
    if (value == NULL || expected == NULL) return false;
    while (*value != '\0' && *expected != '\0') {
        if (tolower((unsigned char)*value) != tolower((unsigned char)*expected)) return false;
        ++value;
        ++expected;
    }
    return *value == '\0' && *expected == '\0';
}

ha_discovery_status_t ha_ssdp_parse_datagram(
    const char *data,
    size_t data_len,
    ha_ssdp_service_t *out)
{
    if (data == NULL || data_len == 0 || out == NULL) return HA_DISCOVERY_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->source = HA_SSDP_SOURCE_UNKNOWN;

    size_t pos = 0;
    size_t line_end = 0;
    while (line_end < data_len && data[line_end] != '\n') ++line_end;
    if (line_end == data_len) return HA_DISCOVERY_MALFORMED;
    size_t first_len = line_end;
    if (first_len > 0 && data[first_len - 1] == '\r') --first_len;

    if (first_len >= 8 && strncmp(data, "HTTP/1.1", 8) == 0) {
        out->source = HA_SSDP_SOURCE_RESPONSE;
    } else if (first_len >= 6 && strncmp(data, "NOTIFY", 6) == 0) {
        out->source = HA_SSDP_SOURCE_ALIVE;
    } else {
        return HA_DISCOVERY_MALFORMED;
    }
    pos = line_end + 1;

    char nts[32] = {0};
    while (pos < data_len) {
        line_end = pos;
        while (line_end < data_len && data[line_end] != '\n') ++line_end;
        size_t len = line_end - pos;
        if (len > 0 && data[pos + len - 1] == '\r') --len;
        if (len == 0) break;

        const char *line = data + pos;
        const char *colon = memchr(line, ':', len);
        if (colon == NULL) return HA_DISCOVERY_MALFORMED;
        const size_t name_len = (size_t)(colon - line);
        const char *value = colon + 1;
        const size_t value_len = len - name_len - 1;

        if (name_equals(line, name_len, "st")) {
            copy_span(out->st, sizeof(out->st), value, value_len, &out->truncated);
        } else if (name_equals(line, name_len, "nt")) {
            copy_span(out->nt, sizeof(out->nt), value, value_len, &out->truncated);
        } else if (name_equals(line, name_len, "nts")) {
            copy_span(nts, sizeof(nts), value, value_len, &out->truncated);
        } else if (name_equals(line, name_len, "usn")) {
            copy_span(out->usn, sizeof(out->usn), value, value_len, &out->truncated);
        } else if (name_equals(line, name_len, "server")) {
            copy_span(out->server, sizeof(out->server), value, value_len, &out->truncated);
        } else if (name_equals(line, name_len, "location")) {
            copy_span(out->location, sizeof(out->location), value, value_len, &out->truncated);
        } else if (name_equals(line, name_len, "cache-control")) {
            copy_span(out->cache_control, sizeof(out->cache_control), value, value_len, &out->truncated);
        }

        pos = line_end < data_len ? line_end + 1 : data_len;
    }

    if (out->source != HA_SSDP_SOURCE_RESPONSE) {
        if (value_equals_ci(nts, "ssdp:byebye")) out->source = HA_SSDP_SOURCE_BYEBYE;
        else if (value_equals_ci(nts, "ssdp:update")) out->source = HA_SSDP_SOURCE_UPDATE;
        else if (value_equals_ci(nts, "ssdp:alive")) out->source = HA_SSDP_SOURCE_ALIVE;
    }

    if (out->st[0] == '\0' && out->nt[0] == '\0' && out->usn[0] == '\0' && out->location[0] == '\0') {
        return HA_DISCOVERY_MALFORMED;
    }
    return out->truncated ? HA_DISCOVERY_TRUNCATED : HA_DISCOVERY_OK;
}
