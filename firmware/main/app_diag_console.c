/*
 * Serial diagnostic console.
 *
 * Transport only: reads one request line, hands it to the application runtime
 * and prints the returned report. It performs no scanning, recognition or
 * control of its own, so anything reachable here is also reachable by the
 * future GUI through the same runtime entry points.
 *
 * Output never contains Wi-Fi credentials, protocol keys or SoftAP passwords.
 * Resource and device reports are produced by the runtime, not by this file.
 */

#include "app_diag_console.h"

#include <stdio.h>
#include <string.h>

#include "app_diag_protocol.h"
#include "app_runtime.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_console";

#define APP_CONSOLE_UART UART_NUM_0
#define APP_CONSOLE_BAUD 115200
#define APP_CONSOLE_STACK 6144u
#define APP_CONSOLE_PRIO 4u
#define APP_CONSOLE_RX_BUFFER 1024u

/* Reports are bounded; anything larger is reported as truncated rather than
 * silently dropped or heap-allocated per request. */
static char s_payload[APP_DIAG_MAX_OUTPUT];
static char s_line[APP_DIAG_MAX_LINE];
/* One formatted response: header plus the bounded payload plus separators. */
static char s_output[APP_DIAG_MAX_OUTPUT + 512u];

static const char *const k_help_text =
    "commands:\n"
    "  request <id> ping\n"
    "  request <id> version\n"
    "  request <id> status\n"
    "  request <id> resources\n"
    "  request <id> scan [full|wifi|ble] [timeout_ms]\n"
    "  request <id> cancel [target_request_id]\n"
    "  request <id> devices\n"
    "  request <id> entities [device_id]\n"
    "  request <id> control <entity_id> <action> [value]\n"
    "  request <id> portal <start|stop|status>\n";

static void console_write(const char *text)
{
    if (text == NULL) {
        return;
    }
    (void)uart_write_bytes(APP_CONSOLE_UART, text, strlen(text));
}

/*
 * Build the text body for commands whose answer is data rather than a single
 * status line. Returns NULL when the command needs no payload.
 */
static const char *build_payload(const app_diag_response_t *response,
                                 const app_diag_request_t *request,
                                 bool *out_truncated)
{
    size_t written = 0u;
    bool truncated = false;

    *out_truncated = false;

    switch (response->command) {
    case APP_DIAG_CMD_HELP:
        (void)snprintf(s_payload, sizeof(s_payload), "%s", k_help_text);
        return s_payload;

    case APP_DIAG_CMD_VERSION: {
        char version[96];
        if (app_runtime_version(version, sizeof(version)) != ESP_OK) {
            (void)snprintf(version, sizeof(version), "unknown");
        }
        (void)snprintf(s_payload, sizeof(s_payload), "firmware=%s", version);
        return s_payload;
    }

    case APP_DIAG_CMD_RESOURCES: {
        app_runtime_resources_t res;
        char db_description[96];

        if (app_runtime_get_resources(&res) != ESP_OK) {
            return NULL;
        }
        if (app_runtime_db_describe(db_description, sizeof(db_description)) != ESP_OK) {
            (void)snprintf(db_description, sizeof(db_description), "unavailable");
        }
        (void)snprintf(s_payload, sizeof(s_payload),
                       "op=%s generation=%lu stage_completed=%lu stage_total=%lu\n"
                       "free_heap=%lu min_free_heap=%lu largest_block=%lu\n"
                       "worker_stack_high_water=%lu console_stack_high_water=%lu\n"
                       "queue_drops=%lu\n"
                       "db_state=%s db=%s db_path=%s",
                       res.op_state,
                       (unsigned long)res.generation,
                       (unsigned long)res.stage_completed,
                       (unsigned long)res.stage_total,
                       (unsigned long)res.free_heap_bytes,
                       (unsigned long)res.min_free_heap_bytes,
                       (unsigned long)res.largest_free_block_bytes,
                       (unsigned long)res.worker_stack_high_water_bytes,
                       (unsigned long)res.console_stack_high_water_bytes,
                       (unsigned long)res.queue_drops,
                       res.db_state != NULL ? res.db_state : "invalid",
                       db_description,
                       res.db_path != NULL ? res.db_path : "");
        return s_payload;
    }

    case APP_DIAG_CMD_DEVICES:
        if (app_runtime_write_devices(s_payload, sizeof(s_payload), &written,
                                      &truncated) != ESP_OK) {
            return NULL;
        }
        *out_truncated = truncated;
        return s_payload;

    case APP_DIAG_CMD_ENTITIES:
        if (app_runtime_write_entities(request->target[0] != '\0' ? request->target
                                                                  : NULL,
                                       s_payload, sizeof(s_payload), &written,
                                       &truncated) != ESP_OK) {
            return NULL;
        }
        *out_truncated = truncated;
        return s_payload;

    case APP_DIAG_CMD_PORTAL:
        /*
         * The portal's answer is produced here rather than by the runtime, because
         * presenting a one-time password is a property of THIS transport: it is the local
         * console, and that is the only place the product rules allow the credentials to
         * appear. The runtime owns the session and shares it; it does not print it.
         *
         * `start` is the only verb that reveals anything, and only once. `status` reports
         * that they were presented, never what they are.
         */
        if (strcmp(request->action, "start") == 0) {
            if (!app_runtime_portal_present(s_payload, sizeof(s_payload))) {
                (void)snprintf(s_payload, sizeof(s_payload),
                               "portal=not_active");
            }
            return s_payload;
        }
        if (app_runtime_portal_status(s_payload, sizeof(s_payload)) != ESP_OK) {
            return NULL;
        }
        return s_payload;

    default:
        return NULL;
    }
}

static void handle_line(const char *line)
{
    app_diag_request_t request;
    app_diag_response_t response;
    app_diag_error_t parse_error;
    const char *payload;
    bool payload_truncated = false;
    esp_err_t submit_error;

    if (line == NULL || line[0] == '\0') {
        return;
    }

    parse_error = app_diag_parse(line, &request);
    if (parse_error != APP_DIAG_OK) {
        app_diag_response_t reject;

        memset(&reject, 0, sizeof(reject));
        reject.request_id = request.request_id;
        reject.command = request.command;
        reject.error = parse_error;
        if (app_diag_format_response(&reject, s_output, sizeof(s_output)) > 0u) {
            console_write(s_output);
        }
        return;
    }

    submit_error = app_runtime_submit(&request, &response, request.timeout_ms);
    if (submit_error != ESP_OK) {
        app_diag_response_t failed;

        memset(&failed, 0, sizeof(failed));
        failed.request_id = request.request_id;
        failed.command = request.command;
        failed.error = (submit_error == ESP_ERR_TIMEOUT) ? APP_DIAG_ERR_BUSY
                                                         : APP_DIAG_ERR_INTERNAL;
        if (app_diag_format_response(&failed, s_output, sizeof(s_output)) > 0u) {
            console_write(s_output);
        }
        return;
    }

    payload = build_payload(&response, &request, &payload_truncated);
    if (payload_truncated) {
        response.truncated = true;
    }

    if (payload != NULL) {
        if (app_diag_format_payload(&response, payload, s_output,
                                    sizeof(s_output)) > 0u) {
            console_write(s_output);
        } else {
            app_diag_response_t too_big = response;
            too_big.error = APP_DIAG_ERR_CAPACITY;
            too_big.truncated = true;
            too_big.detail = "report_too_large";
            if (app_diag_format_response(&too_big, s_output, sizeof(s_output)) > 0u) {
                console_write(s_output);
            }
        }
    } else if (app_diag_format_response(&response, s_output, sizeof(s_output)) > 0u) {
        console_write(s_output);
    }
}

static void app_console_task(void *arg)
{
    size_t used = 0u;

    (void)arg;

    app_runtime_set_console_task(xTaskGetCurrentTaskHandle());

    console_write("\nOne-OS headless diagnostics ready. Type: request 1 help\n");

    for (;;) {
        uint8_t byte = 0;
        int read = uart_read_bytes(APP_CONSOLE_UART, &byte, 1, pdMS_TO_TICKS(100));

        if (read <= 0) {
            continue;
        }

        if (byte == '\r' || byte == '\n') {
            if (used > 0u) {
                s_line[used] = '\0';
                handle_line(s_line);
                used = 0u;
            }
            continue;
        }

        /* Backspace handling keeps an over-typed line recoverable. */
        if (byte == 0x08u || byte == 0x7Fu) {
            if (used > 0u) {
                used--;
            }
            continue;
        }

        if (used + 1u < sizeof(s_line)) {
            s_line[used++] = (char)byte;
        } else {
            /* Over-long line: discard it and tell the operator rather than
             * executing a silently truncated request. */
            used = 0u;
            console_write("error=bad_request detail=line_too_long\n");
        }
    }
}

esp_err_t app_diag_console_start(void)
{
    const uart_config_t config = {
        .baud_rate = APP_CONSOLE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(APP_CONSOLE_UART, APP_CONSOLE_RX_BUFFER, 0,
                                        0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(APP_CONSOLE_UART, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart param config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(app_console_task, "app_console", APP_CONSOLE_STACK, NULL,
                    APP_CONSOLE_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
