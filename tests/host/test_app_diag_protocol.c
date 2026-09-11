/*
 * Host regression tests for the headless diagnostic command surface.
 *
 * These tests exercise the exact parser and formatter that the target firmware
 * compiles, so a passing run proves request correlation, error classification
 * and response shape without requiring hardware.
 */

#include <stdio.h>
#include <string.h>

#include "app_diag_protocol.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) {                                                        \
            ++failures;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* ---------------- parsing ---------------- */

static void test_parse_valid_commands(void)
{
    app_diag_request_t req;
    app_diag_error_t err;

    err = app_diag_parse("request 1 ping", &req);
    CHECK(err == APP_DIAG_OK, "ping should parse, got %s", app_diag_error_name(err));
    CHECK(req.command == APP_DIAG_CMD_PING, "ping command id wrong");
    CHECK(req.request_id == 1u, "request id not captured");

    err = app_diag_parse("request 42 status", &req);
    CHECK(err == APP_DIAG_OK, "status should parse");
    CHECK(req.command == APP_DIAG_CMD_STATUS, "status command id wrong");
    CHECK(req.request_id == 42u, "request id 42 not captured");

    err = app_diag_parse("request 7 version", &req);
    CHECK(err == APP_DIAG_OK && req.command == APP_DIAG_CMD_VERSION, "version parse");

    err = app_diag_parse("request 8 resources", &req);
    CHECK(err == APP_DIAG_OK && req.command == APP_DIAG_CMD_RESOURCES, "resources parse");

    err = app_diag_parse("request 9 devices", &req);
    CHECK(err == APP_DIAG_OK && req.command == APP_DIAG_CMD_DEVICES, "devices parse");

    err = app_diag_parse("request 10 help", &req);
    CHECK(err == APP_DIAG_OK && req.command == APP_DIAG_CMD_HELP, "help parse");

    /* Keywords are case-insensitive; ids are decimal. */
    err = app_diag_parse("REQUEST 11 STATUS", &req);
    CHECK(err == APP_DIAG_OK && req.command == APP_DIAG_CMD_STATUS,
          "keywords should be case-insensitive");
}

static void test_parse_scan_variants(void)
{
    app_diag_request_t req;

    CHECK(app_diag_parse("request 20 scan", &req) == APP_DIAG_OK, "bare scan");
    CHECK(req.scan_kind == APP_DIAG_SCAN_KIND_FULL, "bare scan means full");
    CHECK(req.timeout_ms == 0u, "bare scan has default timeout");

    CHECK(app_diag_parse("request 21 scan wifi", &req) == APP_DIAG_OK, "scan wifi");
    CHECK(req.scan_kind == APP_DIAG_SCAN_KIND_WIFI, "scan wifi kind");

    CHECK(app_diag_parse("request 22 scan ble", &req) == APP_DIAG_OK, "scan ble");
    CHECK(req.scan_kind == APP_DIAG_SCAN_KIND_BLE, "scan ble kind");

    CHECK(app_diag_parse("request 23 scan full 1500", &req) == APP_DIAG_OK, "scan with timeout");
    CHECK(req.scan_kind == APP_DIAG_SCAN_KIND_FULL, "scan full kind");
    CHECK(req.timeout_ms == 1500u, "scan timeout captured");

    /* Invalid scan argument must be rejected, not silently treated as full. */
    CHECK(app_diag_parse("request 24 scan bogus", &req) == APP_DIAG_ERR_INVALID_ARGUMENT,
          "unknown scan kind must be rejected");

    /* Timeout is a decimal count; overflow must not wrap. */
    CHECK(app_diag_parse("request 25 scan full 4294967296", &req) == APP_DIAG_ERR_INVALID_ARGUMENT,
          "overflowing timeout must be rejected");

    CHECK(app_diag_parse("request 26 scan full 100 extra", &req) == APP_DIAG_ERR_BAD_REQUEST,
          "trailing token must be rejected");
}

static void test_parse_cancel_devices_entities(void)
{
    app_diag_request_t req;

    CHECK(app_diag_parse("request 30 cancel", &req) == APP_DIAG_OK, "bare cancel");
    CHECK(req.request_id == 30u, "cancel without target keeps own id");

    CHECK(app_diag_parse("request 31 cancel 29", &req) == APP_DIAG_OK, "cancel target");
    CHECK(req.request_id == 29u, "cancel should adopt the target request id");

    CHECK(app_diag_parse("request 32 entities", &req) == APP_DIAG_OK, "entities all");
    CHECK(req.target[0] == '\0', "entities without device lists all");

    CHECK(app_diag_parse("request 33 entities dev-1", &req) == APP_DIAG_OK, "entities device");
    CHECK(strcmp(req.target, "dev-1") == 0, "entities device target captured, got '%s'", req.target);

    CHECK(app_diag_parse("request 34 devices extra", &req) == APP_DIAG_ERR_BAD_REQUEST,
          "devices takes no operand");
}

static void test_parse_control(void)
{
    app_diag_request_t req;

    CHECK(app_diag_parse("request 40 control switch.kitchen turn_on", &req) == APP_DIAG_OK,
          "control two-operand form");
    CHECK(strcmp(req.target, "switch.kitchen") == 0, "control target, got '%s'", req.target);
    CHECK(strcmp(req.action, "turn_on") == 0, "control action, got '%s'", req.action);
    CHECK(req.value[0] == '\0', "control without value leaves value empty");

    CHECK(app_diag_parse("request 41 control light.desk turn_on 128", &req) == APP_DIAG_OK,
          "control with value");
    CHECK(strcmp(req.value, "128") == 0, "control value, got '%s'", req.value);

    CHECK(app_diag_parse("request 42 control switch.kitchen", &req) == APP_DIAG_ERR_BAD_REQUEST,
          "control requires action");

    /* Over-long operands must be rejected rather than truncated into a
     * different Entity name. */
    {
        char line[APP_DIAG_MAX_LINE];
        char operand[APP_DIAG_TOKEN_LEN + 16];
        memset(operand, 'x', sizeof(operand) - 1u);
        operand[sizeof(operand) - 1u] = '\0';
        snprintf(line, sizeof(line), "request 43 control %s turn_on", operand);
        CHECK(app_diag_parse(line, &req) == APP_DIAG_ERR_INVALID_ARGUMENT,
              "over-long entity id must be rejected");
    }
}

static void test_parse_rejects_malformed(void)
{
    app_diag_request_t req;

    CHECK(app_diag_parse("", &req) == APP_DIAG_ERR_BAD_REQUEST, "empty line");
    CHECK(app_diag_parse("   ", &req) == APP_DIAG_ERR_BAD_REQUEST, "whitespace only");
    CHECK(app_diag_parse("request", &req) == APP_DIAG_ERR_BAD_REQUEST, "missing id and command");
    CHECK(app_diag_parse("request 1", &req) == APP_DIAG_ERR_BAD_REQUEST, "missing command");
    CHECK(app_diag_parse("status 1", &req) == APP_DIAG_ERR_BAD_REQUEST, "missing request prefix");
    CHECK(app_diag_parse("request abc ping", &req) == APP_DIAG_ERR_BAD_REQUEST, "non-numeric id");
    CHECK(app_diag_parse("request -1 ping", &req) == APP_DIAG_ERR_BAD_REQUEST, "negative id");
    CHECK(app_diag_parse("request 4294967296 ping", &req) == APP_DIAG_ERR_BAD_REQUEST,
          "overflowing id must be rejected");
    CHECK(app_diag_parse("request 1 frobnicate", &req) == APP_DIAG_ERR_UNKNOWN_COMMAND,
          "unknown command classified separately");
    CHECK(app_diag_parse(NULL, &req) == APP_DIAG_ERR_BAD_REQUEST, "null line");

    /* A request id that was readable must survive for correlation even when the
     * command itself is unknown, so the console can answer with that id. */
    CHECK(app_diag_parse("request 77 frobnicate", &req) == APP_DIAG_ERR_UNKNOWN_COMMAND,
          "unknown command parse status");
    CHECK(req.request_id == 77u, "request id must survive an unknown command");

    /* Over-long input is rejected, not truncated. */
    {
        char line[APP_DIAG_MAX_LINE + 32];
        memset(line, 'a', sizeof(line) - 1u);
        line[sizeof(line) - 1u] = '\0';
        CHECK(app_diag_parse(line, &req) == APP_DIAG_ERR_BAD_REQUEST,
              "over-long line must be rejected");
    }

    CHECK(app_diag_parse("request 1 ping", NULL) == APP_DIAG_ERR_BAD_REQUEST, "null out");
}

static void test_parse_tolerates_crlf(void)
{
    app_diag_request_t req;

    CHECK(app_diag_parse("request 5 status\r\n", &req) == APP_DIAG_OK,
          "CRLF terminated line should parse");
    CHECK(req.command == APP_DIAG_CMD_STATUS, "CRLF command id");
    CHECK(req.request_id == 5u, "CRLF request id");

    CHECK(app_diag_parse("  request   6\tping  ", &req) == APP_DIAG_OK,
          "extra whitespace and tabs should parse");
    CHECK(req.command == APP_DIAG_CMD_PING, "whitespace command id");
}

/* ---------------- formatting ---------------- */

static void test_format_response(void)
{
    char out[APP_DIAG_MAX_OUTPUT];
    app_diag_response_t resp;
    size_t written;

    memset(&resp, 0, sizeof(resp));
    resp.request_id = 12u;
    resp.command = APP_DIAG_CMD_PING;
    resp.error = APP_DIAG_OK;
    resp.stage = APP_DIAG_STAGE_NONE;
    resp.stage_state = APP_DIAG_STAGE_STATE_IDLE;

    written = app_diag_format_response(&resp, out, sizeof(out));
    CHECK(written > 0u, "response should format");
    CHECK(strstr(out, "request=12") != NULL, "response must carry request id: %s", out);
    CHECK(strstr(out, "command=ping") != NULL, "response must carry command: %s", out);
    CHECK(strstr(out, "error=ok") != NULL, "response must carry error: %s", out);
    CHECK(strstr(out, "partial=0") != NULL, "response must carry partial flag: %s", out);
    CHECK(strstr(out, "truncated=0") != NULL, "response must carry truncated flag: %s", out);
    CHECK(out[written - 1u] == '\n', "response must be newline terminated");
}

static void test_format_flags_are_independent(void)
{
    char out[APP_DIAG_MAX_OUTPUT];
    app_diag_response_t resp;

    memset(&resp, 0, sizeof(resp));
    resp.request_id = 1u;
    resp.command = APP_DIAG_CMD_SCAN;
    resp.error = APP_DIAG_OK;
    resp.stage = APP_DIAG_STAGE_BLE_RF;
    resp.stage_state = APP_DIAG_STAGE_STATE_PARTIAL;
    resp.partial = true;
    resp.truncated = false;

    CHECK(app_diag_format_response(&resp, out, sizeof(out)) > 0u, "format partial");
    CHECK(strstr(out, "partial=1") != NULL, "partial set: %s", out);
    CHECK(strstr(out, "truncated=0") != NULL, "truncated clear: %s", out);
    CHECK(strstr(out, "stage=ble_rf") != NULL, "stage name: %s", out);
    CHECK(strstr(out, "state=partial") != NULL, "stage state name: %s", out);

    resp.truncated = true;
    resp.partial = false;
    CHECK(app_diag_format_response(&resp, out, sizeof(out)) > 0u, "format truncated");
    CHECK(strstr(out, "partial=0") != NULL, "partial clear: %s", out);
    CHECK(strstr(out, "truncated=1") != NULL, "truncated set: %s", out);
}

static void test_format_payload(void)
{
    char out[APP_DIAG_MAX_OUTPUT];
    app_diag_response_t resp;

    memset(&resp, 0, sizeof(resp));
    resp.request_id = 3u;
    resp.command = APP_DIAG_CMD_DEVICES;
    resp.error = APP_DIAG_OK;
    resp.stage = APP_DIAG_STAGE_DONE;
    resp.stage_state = APP_DIAG_STAGE_STATE_DONE;

    CHECK(app_diag_format_payload(&resp, "device id=1 name=Bulb", out, sizeof(out)) > 0u,
          "payload should format");
    CHECK(strstr(out, "device id=1 name=Bulb") != NULL, "payload body present: %s", out);
    CHECK(strstr(out, "stage=done") != NULL, "payload header present: %s", out);

    /* Empty payload is allowed and yields a header-only response. */
    CHECK(app_diag_format_payload(&resp, NULL, out, sizeof(out)) > 0u, "null payload ok");
    CHECK(strstr(out, "request=3") != NULL, "null payload header: %s", out);
}

static void test_format_rejects_small_buffer(void)
{
    char small[16];
    app_diag_response_t resp;

    memset(&resp, 0, sizeof(resp));
    resp.request_id = 1u;
    resp.command = APP_DIAG_CMD_PING;
    resp.error = APP_DIAG_OK;

    CHECK(app_diag_format_response(&resp, small, sizeof(small)) == 0u,
          "undersized buffer must be reported, not truncated");
    CHECK(app_diag_format_response(&resp, small, 0u) == 0u, "zero size must be reported");
    CHECK(app_diag_format_response(NULL, small, sizeof(small)) == 0u, "null response");
    CHECK(app_diag_format_response(&resp, NULL, sizeof(small)) == 0u, "null buffer");
}

static void test_detail_never_shifts_header(void)
{
    char out[APP_DIAG_MAX_OUTPUT];
    app_diag_response_t resp;

    memset(&resp, 0, sizeof(resp));
    resp.request_id = 900u;
    resp.command = APP_DIAG_CMD_CONTROL;
    resp.error = APP_DIAG_ERR_UNSUPPORTED;
    resp.detail = "read_only_entity";

    CHECK(app_diag_format_response(&resp, out, sizeof(out)) > 0u, "detail format");
    CHECK(strstr(out, "request=900") != NULL, "detail keeps request id: %s", out);
    CHECK(strstr(out, "error=unsupported") != NULL, "detail keeps error: %s", out);
    CHECK(strstr(out, "detail=read_only_entity") != NULL, "detail present: %s", out);
}

/* ---------------- round trip ---------------- */

static void test_parse_then_format_round_trip(void)
{
    static const char *lines[] = {
        "request 1 ping",
        "request 2 status",
        "request 3 scan full",
        "request 4 cancel 3",
        "request 5 devices",
        "request 6 entities dev-1",
        "request 7 control switch.a turn_on",
    };
    char out[APP_DIAG_MAX_OUTPUT];

    for (size_t i = 0u; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        app_diag_request_t req;
        app_diag_response_t resp;

        CHECK(app_diag_parse(lines[i], &req) == APP_DIAG_OK, "round trip parse: %s", lines[i]);

        memset(&resp, 0, sizeof(resp));
        resp.request_id = req.request_id;
        resp.command = req.command;
        resp.error = APP_DIAG_OK;

        CHECK(app_diag_format_response(&resp, out, sizeof(out)) > 0u,
              "round trip format: %s", lines[i]);
        CHECK(strstr(out, app_diag_command_name(req.command)) != NULL,
              "round trip command echo for %s: %s", lines[i], out);
    }
}

int main(void)
{
    test_parse_valid_commands();
    test_parse_scan_variants();
    test_parse_cancel_devices_entities();
    test_parse_control();
    test_parse_rejects_malformed();
    test_parse_tolerates_crlf();
    test_format_response();
    test_format_flags_are_independent();
    test_format_payload();
    test_format_rejects_small_buffer();
    test_detail_never_shifts_header();
    test_parse_then_format_round_trip();

    printf("app_diag_protocol: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
