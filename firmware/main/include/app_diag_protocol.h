#pragma once

/*
 * Line-oriented diagnostic command surface for the headless Nearby Devices
 * runtime.
 *
 * This layer is deliberately platform independent: it parses one request line
 * and formats one response. It contains no FreeRTOS, ESP-IDF or LVGL
 * dependency so the exact same parser and formatter used by the UART console is
 * compiled and exercised by the host tests and by the target firmware.
 *
 * The diagnostic entry point is a thin transport only. It never re-implements
 * scanning, recognition or control: the application worker executes every
 * request through the same runtime entry points that the future GUI will use.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_DIAG_MAX_LINE 192u
#define APP_DIAG_MAX_TOKENS 6u
#define APP_DIAG_TOKEN_LEN 48u
#define APP_DIAG_MAX_OUTPUT 2048u

/* Command identifiers. Values are stable: they appear in diagnostics and tests. */
typedef enum {
    APP_DIAG_CMD_NONE = 0,
    APP_DIAG_CMD_HELP,
    APP_DIAG_CMD_PING,
    APP_DIAG_CMD_VERSION,
    APP_DIAG_CMD_STATUS,
    APP_DIAG_CMD_RESOURCES,
    APP_DIAG_CMD_SCAN,
    APP_DIAG_CMD_CANCEL,
    APP_DIAG_CMD_DEVICES,
    APP_DIAG_CMD_ENTITIES,
    APP_DIAG_CMD_CONTROL,
    /*
     * Provisioning portal control, from the local serial console only.
     *
     * This is the exit that starts a provisioning session and prints the temporary AP
     * password. The product rules put that password on the device's own presentation and
     * a local serial output and nowhere else: not in a log, not in /api/status, and not
     * in any HTTP response. A serial console is local by construction, which is what
     * makes it the right place for it and the HTTP transport the wrong one.
     */
    APP_DIAG_CMD_PORTAL,
    APP_DIAG_CMD_UNKNOWN,
} app_diag_command_t;

/* Scan stages mirror the application workflow order. */
typedef enum {
    APP_DIAG_STAGE_NONE = 0,
    APP_DIAG_STAGE_WIFI_RF,
    APP_DIAG_STAGE_BLE_RF,
    APP_DIAG_STAGE_THREAD,
    APP_DIAG_STAGE_ZIGBEE,
    APP_DIAG_STAGE_MDNS,
    APP_DIAG_STAGE_SSDP,
    APP_DIAG_STAGE_LAN_HOSTS,
    APP_DIAG_STAGE_LAN_SERVICES,
    APP_DIAG_STAGE_ENRICHMENT,
    APP_DIAG_STAGE_MATERIALIZE,
    APP_DIAG_STAGE_DONE,
} app_diag_stage_t;

/* Terminal and in-flight stage states. */
typedef enum {
    APP_DIAG_STAGE_STATE_IDLE = 0,
    APP_DIAG_STAGE_STATE_PENDING,
    APP_DIAG_STAGE_STATE_RUNNING,
    APP_DIAG_STAGE_STATE_DONE,
    APP_DIAG_STAGE_STATE_PARTIAL,
    APP_DIAG_STAGE_STATE_FAILED,
    APP_DIAG_STAGE_STATE_SKIPPED,
} app_diag_stage_state_t;

/* Stable machine-readable error codes. */
typedef enum {
    APP_DIAG_OK = 0,
    APP_DIAG_ERR_BAD_REQUEST,
    APP_DIAG_ERR_UNKNOWN_COMMAND,
    APP_DIAG_ERR_INVALID_ARGUMENT,
    APP_DIAG_ERR_BUSY,
    APP_DIAG_ERR_NOT_FOUND,
    APP_DIAG_ERR_NOT_ACTIVE,
    APP_DIAG_ERR_UNSUPPORTED,
    APP_DIAG_ERR_NOT_IMPLEMENTED,
    APP_DIAG_ERR_CAPACITY,
    APP_DIAG_ERR_TIMEOUT,
    APP_DIAG_ERR_CANCELED,
    APP_DIAG_ERR_INTERNAL,
} app_diag_error_t;

typedef enum {
    APP_DIAG_SCAN_KIND_NONE = 0,
    APP_DIAG_SCAN_KIND_FULL,
    APP_DIAG_SCAN_KIND_WIFI,
    APP_DIAG_SCAN_KIND_BLE,
} app_diag_scan_kind_t;

typedef struct {
    app_diag_command_t command;
    app_diag_scan_kind_t scan_kind;
    uint32_t request_id;   /* 0 = caller did not supply one. */
    uint32_t timeout_ms;   /* 0 = use the runtime default. */
    /* `control` operands. */
    char target[APP_DIAG_TOKEN_LEN];
    char action[APP_DIAG_TOKEN_LEN];
    char value[APP_DIAG_TOKEN_LEN];
    uint8_t token_count;
} app_diag_request_t;

/*
 * Parse one request line.
 *
 * Grammar (whitespace separated, case-insensitive keywords):
 *   request <id> <command> [operands...]
 *
 * Returns APP_DIAG_OK, APP_DIAG_ERR_BAD_REQUEST or APP_DIAG_ERR_UNKNOWN_COMMAND.
 * A malformed line still fills request_id when a numeric id was readable so the
 * caller can correlate the error response.
 */
app_diag_error_t app_diag_parse(const char *line, app_diag_request_t *out);

/*
 * Resolve a completed request into one response line.
 *
 * `stage`/`stage_state` describe the furthest stage reached so that progress is
 * derived from real stage completion and never from elapsed time. `partial` and
 * `truncated` are independent flags: a scan can be complete but truncated by
 * fixed table capacity.
 */
typedef struct {
    uint32_t request_id;
    app_diag_command_t command;
    app_diag_error_t error;
    app_diag_stage_t stage;
    app_diag_stage_state_t stage_state;
    bool partial;
    bool truncated;
    const char *detail; /* Optional short literal; never contains secrets. */
} app_diag_response_t;

/* Format one response into `out`. Returns the written length (excluding the
 * terminator) or 0 when the buffer cannot hold the result. */
size_t app_diag_format_response(const app_diag_response_t *response,
                                char *out,
                                size_t out_size);

/* Format a successful payload response (devices/entities/resources/etc). The
 * payload is emitted verbatim after the response header and must already be
 * bounded and free of secrets. Returns written length or 0. */
size_t app_diag_format_payload(const app_diag_response_t *response,
                               const char *payload,
                               char *out,
                               size_t out_size);

const char *app_diag_error_name(app_diag_error_t error);
const char *app_diag_command_name(app_diag_command_t command);
const char *app_diag_stage_name(app_diag_stage_t stage);
const char *app_diag_stage_state_name(app_diag_stage_state_t state);

#ifdef __cplusplus
}
#endif
