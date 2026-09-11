/*
 * Platform-independent implementation of the diagnostic command surface.
 *
 * Compiled for both the ESP32-C6 firmware and the host regression tests. No
 * ESP-IDF, FreeRTOS or LVGL headers may be included here.
 */

#include "app_diag_protocol.h"

#include <stdio.h>
#include <string.h>

/* ---------------- small helpers ---------------- */

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool text_equal_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Parse an unsigned decimal integer. Rejects empty input, non-digits, and
 * overflow instead of silently wrapping. */
static bool parse_u32(const char *text, uint32_t *out)
{
    uint32_t value = 0;
    size_t digits = 0;

    if (text == NULL || *text == '\0') {
        return false;
    }

    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false; /* overflow */
        }
        value = value * 10u + digit;
        ++digits;
    }

    if (digits == 0u || digits > 10u) {
        return false;
    }

    *out = value;
    return true;
}

/* Split `line` into at most APP_DIAG_MAX_TOKENS tokens, in place.
 * Returns the number of tokens actually stored. Extra tokens are counted in
 * `*extra` so callers can reject over-long commands. */
static uint8_t tokenize(char *line, char **tokens, uint8_t max_tokens,
                        uint8_t *extra)
{
    uint8_t count = 0;
    uint8_t overflow = 0;
    char *p = line;

    *extra = 0;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            *p = '\0';
            ++p;
        }
        if (*p == '\0') {
            break;
        }

        char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            ++p;
        }

        if (count < max_tokens) {
            tokens[count] = start;
            ++count;
        } else {
            ++overflow;
        }
    }

    *extra = overflow;
    return count;
}

/* ---------------- name tables ---------------- */

const char *app_diag_error_name(app_diag_error_t error)
{
    switch (error) {
    case APP_DIAG_OK:                  return "ok";
    case APP_DIAG_ERR_BAD_REQUEST:     return "bad_request";
    case APP_DIAG_ERR_UNKNOWN_COMMAND: return "unknown_command";
    case APP_DIAG_ERR_INVALID_ARGUMENT:return "invalid_argument";
    case APP_DIAG_ERR_BUSY:            return "busy";
    case APP_DIAG_ERR_NOT_FOUND:       return "not_found";
    case APP_DIAG_ERR_NOT_ACTIVE:      return "not_active";
    case APP_DIAG_ERR_UNSUPPORTED:     return "unsupported";
    case APP_DIAG_ERR_NOT_IMPLEMENTED: return "not_implemented";
    case APP_DIAG_ERR_CAPACITY:        return "capacity";
    case APP_DIAG_ERR_TIMEOUT:         return "timeout";
    case APP_DIAG_ERR_CANCELED:        return "canceled";
    case APP_DIAG_ERR_INTERNAL:        return "internal";
    default:                           return "invalid";
    }
}

const char *app_diag_command_name(app_diag_command_t command)
{
    switch (command) {
    case APP_DIAG_CMD_NONE:      return "none";
    case APP_DIAG_CMD_HELP:      return "help";
    case APP_DIAG_CMD_PING:      return "ping";
    case APP_DIAG_CMD_VERSION:   return "version";
    case APP_DIAG_CMD_STATUS:    return "status";
    case APP_DIAG_CMD_RESOURCES: return "resources";
    case APP_DIAG_CMD_SCAN:      return "scan";
    case APP_DIAG_CMD_CANCEL:    return "cancel";
    case APP_DIAG_CMD_DEVICES:   return "devices";
    case APP_DIAG_CMD_ENTITIES:  return "entities";
    case APP_DIAG_CMD_CONTROL:   return "control";
    case APP_DIAG_CMD_UNKNOWN:   return "unknown";
    default:                     return "invalid";
    }
}

const char *app_diag_stage_name(app_diag_stage_t stage)
{
    switch (stage) {
    case APP_DIAG_STAGE_NONE:         return "none";
    case APP_DIAG_STAGE_WIFI_RF:      return "wifi_rf";
    case APP_DIAG_STAGE_BLE_RF:       return "ble_rf";
    case APP_DIAG_STAGE_THREAD:       return "thread";
    case APP_DIAG_STAGE_ZIGBEE:       return "zigbee";
    case APP_DIAG_STAGE_MDNS:         return "mdns";
    case APP_DIAG_STAGE_SSDP:         return "ssdp";
    case APP_DIAG_STAGE_LAN_HOSTS:    return "lan_hosts";
    case APP_DIAG_STAGE_LAN_SERVICES: return "lan_services";
    case APP_DIAG_STAGE_ENRICHMENT:   return "enrichment";
    case APP_DIAG_STAGE_MATERIALIZE:  return "materialize";
    case APP_DIAG_STAGE_DONE:         return "done";
    default:                          return "invalid";
    }
}

const char *app_diag_stage_state_name(app_diag_stage_state_t state)
{
    switch (state) {
    case APP_DIAG_STAGE_STATE_IDLE:    return "idle";
    case APP_DIAG_STAGE_STATE_PENDING: return "pending";
    case APP_DIAG_STAGE_STATE_RUNNING: return "running";
    case APP_DIAG_STAGE_STATE_DONE:    return "done";
    case APP_DIAG_STAGE_STATE_PARTIAL: return "partial";
    case APP_DIAG_STAGE_STATE_FAILED:  return "failed";
    case APP_DIAG_STAGE_STATE_SKIPPED: return "skipped";
    default:                           return "invalid";
    }
}

/* ---------------- parsing ---------------- */

static app_diag_scan_kind_t parse_scan_kind(const char *text)
{
    if (text == NULL) {
        return APP_DIAG_SCAN_KIND_NONE;
    }
    if (text_equal_ci(text, "full")) {
        return APP_DIAG_SCAN_KIND_FULL;
    }
    if (text_equal_ci(text, "wifi") || text_equal_ci(text, "wi-fi")) {
        return APP_DIAG_SCAN_KIND_WIFI;
    }
    if (text_equal_ci(text, "ble")) {
        return APP_DIAG_SCAN_KIND_BLE;
    }
    return APP_DIAG_SCAN_KIND_NONE;
}

static app_diag_command_t parse_command(const char *text)
{
    if (text_equal_ci(text, "help") || text_equal_ci(text, "?")) {
        return APP_DIAG_CMD_HELP;
    }
    if (text_equal_ci(text, "ping")) {
        return APP_DIAG_CMD_PING;
    }
    if (text_equal_ci(text, "version")) {
        return APP_DIAG_CMD_VERSION;
    }
    if (text_equal_ci(text, "status")) {
        return APP_DIAG_CMD_STATUS;
    }
    if (text_equal_ci(text, "resources")) {
        return APP_DIAG_CMD_RESOURCES;
    }
    if (text_equal_ci(text, "scan")) {
        return APP_DIAG_CMD_SCAN;
    }
    if (text_equal_ci(text, "cancel")) {
        return APP_DIAG_CMD_CANCEL;
    }
    if (text_equal_ci(text, "devices")) {
        return APP_DIAG_CMD_DEVICES;
    }
    if (text_equal_ci(text, "entities")) {
        return APP_DIAG_CMD_ENTITIES;
    }
    if (text_equal_ci(text, "control")) {
        return APP_DIAG_CMD_CONTROL;
    }
    return APP_DIAG_CMD_UNKNOWN;
}

app_diag_error_t app_diag_parse(const char *line, app_diag_request_t *out)
{
    char buffer[APP_DIAG_MAX_LINE];
    char *tokens[APP_DIAG_MAX_TOKENS];
    uint8_t extra = 0;
    uint8_t count;
    size_t length;

    if (out == NULL) {
        return APP_DIAG_ERR_BAD_REQUEST;
    }

    memset(out, 0, sizeof(*out));
    out->command = APP_DIAG_CMD_NONE;

    if (line == NULL) {
        return APP_DIAG_ERR_BAD_REQUEST;
    }

    length = strlen(line);
    if (length == 0u) {
        return APP_DIAG_ERR_BAD_REQUEST;
    }
    if (length >= sizeof(buffer)) {
        /* Reject over-long lines rather than silently truncating a request. */
        return APP_DIAG_ERR_BAD_REQUEST;
    }

    memcpy(buffer, line, length + 1u);

    count = tokenize(buffer, tokens, APP_DIAG_MAX_TOKENS, &extra);
    if (count < 3u) {
        /* Minimum meaningful request is: request <id> <command> */
        return APP_DIAG_ERR_BAD_REQUEST;
    }
    if (!text_equal_ci(tokens[0], "request")) {
        return APP_DIAG_ERR_BAD_REQUEST;
    }
    if (!parse_u32(tokens[1], &out->request_id)) {
        return APP_DIAG_ERR_BAD_REQUEST;
    }

    out->token_count = (uint8_t)(count - 3u);
    out->command = parse_command(tokens[2]);

    if (out->command == APP_DIAG_CMD_UNKNOWN) {
        return APP_DIAG_ERR_UNKNOWN_COMMAND;
    }

    switch (out->command) {
    case APP_DIAG_CMD_SCAN: {
        if (count >= 4u) {
            out->scan_kind = parse_scan_kind(tokens[3]);
            if (out->scan_kind == APP_DIAG_SCAN_KIND_NONE) {
                return APP_DIAG_ERR_INVALID_ARGUMENT;
            }
            out->token_count = (uint8_t)(count - 4u);
            if (count == 5u && !parse_u32(tokens[4], &out->timeout_ms)) {
                return APP_DIAG_ERR_INVALID_ARGUMENT;
            }
            if (count > 5u) {
                return APP_DIAG_ERR_BAD_REQUEST;
            }
        } else {
            out->scan_kind = APP_DIAG_SCAN_KIND_FULL;
        }
        break;
    }

    case APP_DIAG_CMD_CONTROL: {
        if (count < 5u || extra > 0u) {
            return APP_DIAG_ERR_BAD_REQUEST;
        }
        if (strlen(tokens[3]) >= APP_DIAG_TOKEN_LEN ||
            strlen(tokens[4]) >= APP_DIAG_TOKEN_LEN) {
            return APP_DIAG_ERR_INVALID_ARGUMENT;
        }
        memcpy(out->target, tokens[3], strlen(tokens[3]) + 1u);
        memcpy(out->action, tokens[4], strlen(tokens[4]) + 1u);
        if (count >= 6u) {
            if (strlen(tokens[5]) >= APP_DIAG_TOKEN_LEN) {
                return APP_DIAG_ERR_INVALID_ARGUMENT;
            }
            memcpy(out->value, tokens[5], strlen(tokens[5]) + 1u);
        }
        out->token_count = (uint8_t)(count - 5u);
        break;
    }

    case APP_DIAG_CMD_ENTITIES: {
        if (count >= 4u) {
            if (strlen(tokens[3]) >= APP_DIAG_TOKEN_LEN) {
                return APP_DIAG_ERR_INVALID_ARGUMENT;
            }
            memcpy(out->target, tokens[3], strlen(tokens[3]) + 1u);
            out->token_count = (uint8_t)(count - 4u);
        }
        if (count > 4u) {
            return APP_DIAG_ERR_BAD_REQUEST;
        }
        break;
    }

    case APP_DIAG_CMD_CANCEL: {
        if (count >= 4u) {
            if (!parse_u32(tokens[3], &out->request_id)) {
                return APP_DIAG_ERR_INVALID_ARGUMENT;
            }
            out->token_count = (uint8_t)(count - 4u);
        }
        if (count > 4u) {
            return APP_DIAG_ERR_BAD_REQUEST;
        }
        break;
    }

    case APP_DIAG_CMD_HELP:
    case APP_DIAG_CMD_PING:
    case APP_DIAG_CMD_VERSION:
    case APP_DIAG_CMD_STATUS:
    case APP_DIAG_CMD_RESOURCES:
    case APP_DIAG_CMD_DEVICES: {
        if (count != 3u) {
            return APP_DIAG_ERR_BAD_REQUEST;
        }
        break;
    }

    default:
        return APP_DIAG_ERR_UNKNOWN_COMMAND;
    }

    return APP_DIAG_OK;
}

/* ---------------- formatting ---------------- */

/* Append `text` to `out` at `*used`. Returns false when it would not fit, in
 * which case nothing is written. */
static bool append(char *out, size_t out_size, size_t *used, const char *text)
{
    size_t length = strlen(text);
    if (*used + length + 1u > out_size) {
        return false;
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

static size_t format_header(const app_diag_response_t *response,
                            char *out,
                            size_t out_size)
{
    char scratch[192];
    size_t used = 0;

    if (out_size > 0u) {
        out[0] = '\0';
    }

    int written = snprintf(scratch, sizeof(scratch),
                           "request=%lu command=%s error=%s stage=%s state=%s partial=%u truncated=%u",
                           (unsigned long)response->request_id,
                           app_diag_command_name(response->command),
                           app_diag_error_name(response->error),
                           app_diag_stage_name(response->stage),
                           app_diag_stage_state_name(response->stage_state),
                           response->partial ? 1u : 0u,
                           response->truncated ? 1u : 0u);
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        return 0u;
    }
    if (!append(out, out_size, &used, scratch)) {
        return 0u;
    }
    return used;
}

size_t app_diag_format_response(const app_diag_response_t *response,
                                char *out,
                                size_t out_size)
{
    size_t used;

    if (response == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }

    used = format_header(response, out, out_size);
    if (used == 0u) {
        return 0u;
    }

    if (response->detail != NULL && response->detail[0] != '\0') {
        if (!append(out, out_size, &used, " detail=") ||
            !append(out, out_size, &used, response->detail)) {
            return 0u;
        }
    }

    if (!append(out, out_size, &used, "\n")) {
        return 0u;
    }

    return used;
}

size_t app_diag_format_payload(const app_diag_response_t *response,
                               const char *payload,
                               char *out,
                               size_t out_size)
{
    size_t used;

    if (response == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }

    used = format_header(response, out, out_size);
    if (used == 0u) {
        return 0u;
    }

    if (payload != NULL && payload[0] != '\0') {
        if (!append(out, out_size, &used, "\n") ||
            !append(out, out_size, &used, payload)) {
            return 0u;
        }
    }

    if (!append(out, out_size, &used, "\n")) {
        return 0u;
    }

    return used;
}
