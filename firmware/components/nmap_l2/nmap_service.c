#include "nmap_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static size_t bounded_strlen(const char *s, size_t cap)
{
    size_t n = 0;
    if (s == NULL) return 0;
    while (n < cap && s[n] != '\0') ++n;
    return n;
}

static void copy_text(char *dst, size_t cap, const char *src, size_t len)
{
    if (dst == NULL || cap == 0) return;
    size_t n = len;
    if (n >= cap) n = cap - 1;
    if (src != NULL && n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

static void copy_cstr(char *dst, size_t cap, const char *src)
{
    copy_text(dst, cap, src, bounded_strlen(src, cap));
}

static bool starts_with(const uint8_t *data, size_t len, const char *prefix)
{
    const size_t n = strlen(prefix);
    return data != NULL && len >= n && memcmp(data, prefix, n) == 0;
}

static int ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

static bool contains_ascii_ci(const uint8_t *data, size_t len, const char *needle)
{
    const size_t n = strlen(needle);
    if (data == NULL || n == 0 || len < n) return false;
    for (size_t i = 0; i + n <= len; ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            if (ascii_tolower(data[i + j]) != ascii_tolower((unsigned char)needle[j])) break;
        }
        if (j == n) return true;
    }
    return false;
}

static bool is_line_boundary_before(const uint8_t *data, size_t pos)
{
    return pos == 0 || data[pos - 1] == '\n';
}

static bool extract_http_header(const uint8_t *data, size_t len, const char *name, char *out, size_t out_cap)
{
    const size_t name_len = strlen(name);
    for (size_t i = 0; i + name_len + 1 < len; ++i) {
        if (!is_line_boundary_before(data, i)) continue;
        size_t j = 0;
        for (; j < name_len; ++j) {
            if (ascii_tolower(data[i + j]) != ascii_tolower((unsigned char)name[j])) break;
        }
        if (j != name_len || data[i + name_len] != ':') continue;
        size_t start = i + name_len + 1;
        while (start < len && (data[start] == ' ' || data[start] == '\t')) ++start;
        size_t end = start;
        while (end < len && data[end] != '\r' && data[end] != '\n') ++end;
        while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t')) --end;
        copy_text(out, out_cap, (const char *)&data[start], end - start);
        return end > start;
    }
    return false;
}

static void sanitize_banner(const uint8_t *data, size_t len, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    size_t n = len;
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = data[i];
        if (c == '\r' || c == '\n' || c == '\t') out[i] = ' ';
        else if (c >= 0x20 && c <= 0x7e) out[i] = (char)c;
        else out[i] = '.';
    }
    out[n] = '\0';
}

static void set_service(nmap_service_result_t *out, nmap_service_kind_t kind,
                        nmap_service_confidence_t confidence, const char *name, const char *probe_id)
{
    out->service = kind;
    out->confidence = confidence;
    out->evidence_flags |= NMAP_SERVICE_EVIDENCE_PROTOCOL_MATCH;
    copy_cstr(out->service_name, sizeof(out->service_name), name);
    copy_cstr(out->probe_id, sizeof(out->probe_id), probe_id);
}

static void extract_ssh_product(const uint8_t *data, size_t len, nmap_service_result_t *out)
{
    static const char prefix[] = "SSH-";
    size_t i = sizeof(prefix) - 1;
    while (i < len && data[i] != '-') ++i;
    if (i >= len || data[i] != '-') return;
    ++i;
    const size_t start = i;
    while (i < len && data[i] != ' ' && data[i] != '\r' && data[i] != '\n') ++i;
    if (i > start) copy_text(out->product, sizeof(out->product), (const char *)&data[start], i - start);
}

nmap_service_probe_profile_t nmap_internal_service_profile_for_port(nmap_service_probe_profile_t requested,
                                                                    uint16_t port)
{
    if (requested == NMAP_SERVICE_PROBE_HTTP_HEAD || requested == NMAP_SERVICE_PROBE_PASSIVE) return requested;
    switch (port) {
    case 80:
    case 8000:
    case 8080:
    case 8888:
        return NMAP_SERVICE_PROBE_HTTP_HEAD;
    default:
        return NMAP_SERVICE_PROBE_PASSIVE;
    }
}

void nmap_internal_service_match(const uint8_t *data, size_t len, bool truncated,
                                 bool active_http_probe, nmap_service_result_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->service = NMAP_SERVICE_UNKNOWN;
    out->confidence = NMAP_SERVICE_CONFIDENCE_NONE;
    out->reason = NMAP_REASON_NONE;
    out->response_bytes = len > UINT16_MAX ? UINT16_MAX : (uint16_t)len;
    if (truncated) out->result_flags |= NMAP_RESULT_TRUNCATED;
    if (active_http_probe) {
        out->evidence_flags |= NMAP_SERVICE_EVIDENCE_ACTIVE_PROBE;
        copy_cstr(out->probe_id, sizeof(out->probe_id), "http-head-rfc9110");
    } else {
        copy_cstr(out->probe_id, sizeof(out->probe_id), "passive-banner");
    }

    if (data == NULL || len == 0) return;
    if (!active_http_probe) out->evidence_flags |= NMAP_SERVICE_EVIDENCE_PASSIVE_BANNER;
    sanitize_banner(data, len, out->banner, sizeof(out->banner));

    if (starts_with(data, len, "SSH-")) {
        set_service(out, NMAP_SERVICE_SSH, NMAP_SERVICE_CONFIDENCE_STRONG,
                    "ssh", "passive-ssh-rfc4253");
        extract_ssh_product(data, len, out);
        return;
    }

    if (starts_with(data, len, "HTTP/1.0 ") || starts_with(data, len, "HTTP/1.1 ")) {
        set_service(out, NMAP_SERVICE_HTTP, NMAP_SERVICE_CONFIDENCE_STRONG,
                    "http", active_http_probe ? "http-head-rfc9110" : "passive-http-rfc9112");
        (void)extract_http_header(data, len, "Server", out->product, sizeof(out->product));
        return;
    }

    const bool greeting_220 = len >= 4 && data[0] == '2' && data[1] == '2' && data[2] == '0' &&
                              (data[3] == ' ' || data[3] == '-');
    if (greeting_220 &&
        (contains_ascii_ci(data, len, "smtp") || contains_ascii_ci(data, len, "esmtp") ||
         contains_ascii_ci(data, len, "postfix") || contains_ascii_ci(data, len, "exim") ||
         contains_ascii_ci(data, len, "sendmail"))) {
        set_service(out, NMAP_SERVICE_SMTP, NMAP_SERVICE_CONFIDENCE_STRONG,
                    "smtp", "passive-smtp-rfc5321");
        return;
    }
    if (greeting_220 && contains_ascii_ci(data, len, "ftp")) {
        set_service(out, NMAP_SERVICE_FTP, NMAP_SERVICE_CONFIDENCE_STRONG,
                    "ftp", "passive-ftp-rfc959");
        return;
    }
}
