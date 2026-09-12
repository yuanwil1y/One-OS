/*
 * The headless acceptance loop, driven end to end through the real diagnostic
 * surface.
 *
 * What this adds over tests/host/test_app_diag_protocol.c: that group checks the
 * parser and formatter as units. This one walks a full session - the lines an
 * operator actually types at the serial console while working through
 * docs/hardware-acceptance.md - through the same parse -> decide -> render path the
 * firmware uses, and asserts on the rendered text. The failure it is built to
 * catch is drift: a command that parses but renders a response an operator cannot
 * act on, a refusal whose reason changed, or a stage verdict that stopped being
 * reported.
 *
 * The decisions here are made with the real application modules (app_control for
 * the control refusals), not by a second copy of the rules: two implementations
 * of "is this control allowed" would eventually disagree, and the operator would
 * have no way to tell which one was right.
 *
 * What it cannot prove: the platform half of every decision. app_runtime.c,
 * app_scan_native.c and the radio paths are ESP-IDF-only, so the lines below are
 * driven through this test's own dispatch against the platform-independent
 * modules. A green run here means the protocol and the reasons are right, not that
 * a board behaves.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_control.h"
#include "app_diag_protocol.h"
#include "app_scan.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/* the session harness                                                 */
/* ------------------------------------------------------------------ */

/* One typed line, its parsed request, and the response the console would print. */
typedef struct {
    app_diag_request_t request;
    app_diag_response_t response;
    char rendered[APP_DIAG_MAX_OUTPUT];
} session_line_t;

/* Parse one line. Returns the parser's own status so a test can assert refusals. */
static app_diag_error_t parse_line(const char *line, session_line_t *out)
{
    memset(out, 0, sizeof(*out));
    return app_diag_parse(line, &out->request);
}

/* Render the response and return the rendered text. */
static const char *render(session_line_t *line, const char *payload)
{
    size_t written;

    if (payload != NULL) {
        written = app_diag_format_payload(&line->response, payload, line->rendered,
                                          sizeof(line->rendered));
    } else {
        written = app_diag_format_response(&line->response, line->rendered,
                                           sizeof(line->rendered));
    }
    if (written == 0u) {
        line->rendered[0] = '\0';
    }
    return line->rendered;
}

/*
 * Every documented command must parse.
 *
 * The grammar is `request <id> <command> [operands]`. These are the lines from the
 * hardware checklist, so a command that stops parsing is a checklist item that can
 * no longer be performed at all.
 */
static void test_every_checklist_command_parses(void)
{
    static const struct {
        const char *line;
        app_diag_command_t command;
    } cases[] = {
        {"request 1 version", APP_DIAG_CMD_VERSION},
        {"request 2 status", APP_DIAG_CMD_STATUS},
        {"request 3 resources", APP_DIAG_CMD_RESOURCES},
        {"request 4 help", APP_DIAG_CMD_HELP},
        {"request 5 ping", APP_DIAG_CMD_PING},
        {"request 10 scan full", APP_DIAG_CMD_SCAN},
        {"request 11 scan wifi 5000", APP_DIAG_CMD_SCAN},
        {"request 12 scan ble", APP_DIAG_CMD_SCAN},
        {"request 13 cancel", APP_DIAG_CMD_CANCEL},
        {"request 14 devices", APP_DIAG_CMD_DEVICES},
        {"request 15 entities", APP_DIAG_CMD_ENTITIES},
        {"request 20 control switch.nothing turn_on", APP_DIAG_CMD_CONTROL},
        {"request 30 portal start", APP_DIAG_CMD_PORTAL},
        {"request 31 portal status", APP_DIAG_CMD_PORTAL},
        {"request 32 portal stop", APP_DIAG_CMD_PORTAL},
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        session_line_t line;

        CHECK(parse_line(cases[i].line, &line) == APP_DIAG_OK, "'%s' did not parse",
              cases[i].line);
        CHECK(line.request.command == cases[i].command, "'%s' parsed as %s", cases[i].line,
              app_diag_command_name(line.request.command));
    }

    /* Case-insensitive keywords and extra whitespace are part of the grammar, and
     * an operator typing on a serial console will produce both. */
    {
        session_line_t line;

        CHECK(parse_line("  REQUEST   7   STATUS  ", &line) == APP_DIAG_OK,
              "whitespace and case are tolerated");
        CHECK(line.request.request_id == 7u, "request id is %lu",
              (unsigned long)line.request.request_id);
        CHECK(line.request.command == APP_DIAG_CMD_STATUS, "and the command is status");
    }
}

/* ------------------------------------------------------------------ */
/* request correlation                                                 */
/* ------------------------------------------------------------------ */

/*
 * The id an operator typed is the id that comes back.
 *
 * This is the whole reason the console is usable for acceptance: a session is a
 * stream of interleaved commands, and a response that carries the wrong id is
 * worse than no response, because it attributes one command's result to another.
 */
static void test_the_request_id_survives(void)
{
    static const uint32_t ids[] = {0u, 1u, 7u, 4294967295u};

    for (size_t i = 0u; i < sizeof(ids) / sizeof(ids[0]); i++) {
        session_line_t line;
        char typed[64];

        (void)snprintf(typed, sizeof(typed), "request %lu status", (unsigned long)ids[i]);
        CHECK(parse_line(typed, &line) == APP_DIAG_OK, "'%s' did not parse", typed);
        CHECK(line.request.request_id == ids[i], "id parsed as %lu, expected %lu",
              (unsigned long)line.request.request_id, (unsigned long)ids[i]);

        line.response.request_id = line.request.request_id;
        line.response.command = line.request.command;
        CHECK(strstr(render(&line, NULL), "request=") == line.rendered,
              "the response does not start with the request id: %s", line.rendered);
    }

    /* An unparseable id is a bad request, and the console must say so rather than
     * guess which command was meant. */
    {
        session_line_t line;

        CHECK(parse_line("request notanumber status", &line) == APP_DIAG_ERR_BAD_REQUEST,
              "a non-numeric id is refused");
        CHECK(parse_line("request", &line) == APP_DIAG_ERR_BAD_REQUEST,
              "a bare 'request' is refused");
        CHECK(parse_line("status 3", &line) == APP_DIAG_ERR_BAD_REQUEST,
              "a line that is not a request is refused");
    }
}

/*
 * An unknown command is refused by name, not executed.
 *
 * The distinction matters on a console that is also a security boundary: an
 * unrecognised word must not fall through to some default action.
 */
static void test_unknown_and_malformed_lines_are_refused(void)
{
    session_line_t line;

    CHECK(parse_line("request 9 frobnicate", &line) == APP_DIAG_ERR_UNKNOWN_COMMAND,
          "an unknown command is refused as unknown");
    CHECK(parse_line("request 9 control", &line) == APP_DIAG_ERR_BAD_REQUEST ||
              parse_line("request 9 control", &line) == APP_DIAG_ERR_INVALID_ARGUMENT,
          "control without operands is refused");
    CHECK(parse_line("request 9 scan sideways", &line) == APP_DIAG_ERR_INVALID_ARGUMENT,
          "an unknown scan kind is refused");
    CHECK(parse_line("", &line) == APP_DIAG_ERR_BAD_REQUEST, "an empty line is refused");
}

/* ------------------------------------------------------------------ */
/* scans: the stage verdict is what an operator reads                  */
/* ------------------------------------------------------------------ */

/*
 * A scan response must report the furthest stage and its state, and the refused
 * stages must carry a reason.
 *
 * The two stages with no native backend behind them are the ones that matter most:
 * they must come back SKIPPED with a name that says why, because a scan that
 * silently implies it covered Zigbee is how an operator concludes a device is
 * absent when it was never looked for.
 */
static void test_scan_stages_are_reported_honestly(void)
{
    app_scan_inputs_t inputs;
    session_line_t line;

    memset(&inputs, 0, sizeof(inputs));
    inputs.wifi_configured = true;
    inputs.wifi_connected = true;
    inputs.wifi_driver_acquired = true;
    inputs.ble_available = true;
    inputs.has_ip = true;

    {
        app_scan_stage_plan_t plan = app_scan_plan_stage(&inputs, APP_STAGE_ZIGBEE);

        CHECK(plan.action == APP_STAGE_ACTION_UNAVAILABLE,
              "zigbee is reported unavailable, not run");
        CHECK(plan.terminal_state == APP_STAGE_STATE_SKIPPED, "zigbee is skipped");
        CHECK(plan.reason != NULL && plan.reason[0] != '\0',
              "zigbee carries a reason an operator can read");
        CHECK(strstr(plan.reason, "unavailable") != NULL, "the reason names the cause: %s",
              plan.reason);
    }
    {
        app_scan_stage_plan_t plan = app_scan_plan_stage(&inputs, APP_STAGE_THREAD);

        CHECK(plan.action == APP_STAGE_ACTION_UNAVAILABLE, "thread is reported unavailable");
        CHECK(plan.reason != NULL && strstr(plan.reason, "unavailable") != NULL,
              "thread carries a reason: %s", plan.reason);
    }

    /* A Wi-Fi stage whose driver was never acquired is FAILED, not skipped and not
     * done: nothing was listened to, so the scan must not look like it covered
     * Wi-Fi. This is the difference between "there were no networks" and "we never
     * looked", which is the whole point of the per-stage verdict. */
    memset(&inputs, 0, sizeof(inputs));
    {
        app_scan_stage_plan_t plan = app_scan_plan_stage(&inputs, APP_STAGE_WIFI_RF);

        CHECK(plan.action != APP_STAGE_ACTION_RUN, "a Wi-Fi scan without a driver does not run");
        CHECK(plan.terminal_state == APP_STAGE_STATE_FAILED,
              "and is reported FAILED rather than done or skipped");
        CHECK(plan.reason != NULL && plan.reason[0] != '\0', "carrying a reason: %s",
              plan.reason);
        CHECK(strstr(plan.reason, "unavailable") != NULL,
              "that names the missing driver: %s", plan.reason);
    }
    /* With no IP the LAN stages are skipped, which is the same rule expressed for
     * a later stage: a scan must never claim to have covered what it could not. */
    memset(&inputs, 0, sizeof(inputs));
    inputs.wifi_configured = true;
    inputs.wifi_connected = false;
    inputs.has_ip = false;
    {
        app_scan_stage_plan_t plan = app_scan_plan_stage(&inputs, APP_STAGE_LAN_HOSTS);

        CHECK(plan.action != APP_STAGE_ACTION_RUN, "a LAN stage without an IP does not run");
        CHECK(plan.reason != NULL && strstr(plan.reason, "no_ip") != NULL,
              "and names the missing IP: %s", plan.reason);
    }

    /* The response itself: a completed scan is `done` with no error, and the
     * truncation flag is separate from completion. */
    CHECK(parse_line("request 10 scan full", &line) == APP_DIAG_OK, "scan parses");
    line.response.request_id = line.request.request_id;
    line.response.command = APP_DIAG_CMD_SCAN;
    line.response.stage = APP_DIAG_STAGE_DONE;
    line.response.stage_state = APP_DIAG_STAGE_STATE_DONE;
    line.response.partial = false;
    line.response.truncated = true;
    CHECK(strstr(render(&line, NULL), "stage=done state=done partial=0 truncated=1") != NULL,
          "a complete but truncated scan says both: %s", line.rendered);

    /* A refused scan names the error and the stage it reached. */
    line.response.error = APP_DIAG_ERR_BUSY;
    line.response.stage = APP_DIAG_STAGE_BLE_RF;
    line.response.stage_state = APP_DIAG_STAGE_STATE_RUNNING;
    line.response.truncated = false;
    CHECK(strstr(render(&line, NULL), "error=busy stage=ble_rf state=running") != NULL,
          "a refused scan reports where it was: %s", line.rendered);
}

/* ------------------------------------------------------------------ */
/* control: the refusal reasons from the real loop                     */
/* ------------------------------------------------------------------ */

/*
 * A control against an entity the application does not know must be refused with a
 * reason, and the response must not say `ok`.
 *
 * app_control decides; this checks that the decision renders into something an
 * operator can act on. Every app_control_status_t value must have a name, because a
 * refusal whose reason is missing is a refusal nobody can debug.
 */
static void test_control_refusals_render(void)
{
    static const app_control_status_t statuses[] = {
        APP_CONTROL_OK,
        APP_CONTROL_ERR_INVALID_ARGUMENT,
        APP_CONTROL_ERR_UNKNOWN_ENTITY,
        APP_CONTROL_ERR_NOT_WRITABLE,
        APP_CONTROL_ERR_UNSUPPORTED,
        APP_CONTROL_ERR_OUT_OF_RANGE,
        APP_CONTROL_ERR_NO_BACKEND,
        APP_CONTROL_ERR_AMBIGUOUS_BACKEND,
        APP_CONTROL_ERR_DEVICE_UNAVAILABLE,
        APP_CONTROL_ERR_BUSY,
        APP_CONTROL_ERR_NO_CAPACITY,
        APP_CONTROL_ERR_BACKEND_FAILED,
    };

    for (size_t i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        const char *name = app_control_status_name(statuses[i]);

        CHECK(name != NULL && name[0] != '\0' && strcmp(name, "unknown") != 0,
              "status %d has no name", (int)statuses[i]);
    }

    /* The real loop, with no backend registered: the entity is unknown, and the
     * answer must be a refusal rather than a queue entry. */
    {
        app_control_slot_t *slot = NULL;
        app_control_status_t status;

        app_control_reset();
        status = app_control_submit("switch.nothing", "turn_on", NULL, 20u, 1000u, 5000u, &slot);
        CHECK(status == APP_CONTROL_ERR_UNKNOWN_ENTITY,
              "an unknown entity is refused as unknown, got %s", app_control_status_name(status));
        CHECK(slot == NULL, "and no slot was created for it");
        CHECK(app_control_pending_count() == 0u, "and nothing is pending");
    }

    /* And the rendered response for that refusal. */
    {
        session_line_t line;

        CHECK(parse_line("request 20 control switch.nothing turn_on", &line) == APP_DIAG_OK,
              "the control line parses");
        CHECK(strcmp(line.request.target, "switch.nothing") == 0, "target is %s",
              line.request.target);
        CHECK(strcmp(line.request.action, "turn_on") == 0, "action is %s", line.request.action);

        line.response.request_id = line.request.request_id;
        line.response.command = APP_DIAG_CMD_CONTROL;
        line.response.error = APP_DIAG_ERR_NOT_FOUND;
        line.response.detail = "unknown_entity";
        CHECK(strstr(render(&line, NULL), "command=control error=not_found") != NULL,
              "the refusal renders: %s", line.rendered);
        CHECK(strstr(line.rendered, "detail=unknown_entity") != NULL,
              "with the reason attached: %s", line.rendered);
        CHECK(strstr(line.rendered, "ok") == NULL,
              "and never says ok: %s", line.rendered);
    }
}

/* ------------------------------------------------------------------ */
/* payloads                                                            */
/* ------------------------------------------------------------------ */

/*
 * `devices` and `entities` carry a payload on its own lines after the header.
 *
 * The header and the payload are one response: the operator reads the header to
 * know whether to believe the list. A payload without its header is unattributable,
 * and a header that claimed success with no payload is an empty result nobody can
 * distinguish from a failure.
 */
static void test_payload_responses(void)
{
    session_line_t line;
    const char *rendered;
    const char *payload = "device 1 ble aa:bb:cc:dd:ee:ff name=Bulb\n"
                         "device 2 wifi 192.168.1.9 name=Printer";

    CHECK(parse_line("request 14 devices", &line) == APP_DIAG_OK, "devices parses");
    line.response.request_id = line.request.request_id;
    line.response.command = APP_DIAG_CMD_DEVICES;
    line.response.stage = APP_DIAG_STAGE_DONE;
    line.response.stage_state = APP_DIAG_STAGE_STATE_DONE;
    rendered = render(&line, payload);

    CHECK(strstr(rendered, "request=14 command=devices error=ok") != NULL,
          "the header comes first: %s", rendered);
    CHECK(strstr(rendered, "name=Bulb") != NULL, "and the payload follows: %s", rendered);

    /* The header line ends before the payload: a reader (and a test) can split them
     * without guessing. */
    {
        const char *newline = strchr(rendered, '\n');

        CHECK(newline != NULL, "the header is terminated");
        if (newline != NULL) {
            CHECK(strncmp(rendered, "request=14 command=devices", 25) == 0,
                  "the first line is the header");
            CHECK(strstr(newline, "name=Bulb") != NULL, "the payload is on later lines");
        }
    }

    /* An empty result renders as a header with no payload lines, which is the
     * honest "none found" and not a missing response. */
    rendered = render(&line, "");
    CHECK(strstr(rendered, "command=devices") != NULL, "an empty result still has a header");
    CHECK(strchr(rendered, '\n') != NULL, "and is terminated");
}

/*
 * The response buffer bound is enforced rather than truncated.
 *
 * A response cut in half is one an operator (or a test harness) will parse as a
 * complete but wrong answer, so refusing is the only safe outcome. The formatter
 * clears the caller's first byte on entry; what matters is that it reports 0, leaves
 * no partial response for a caller that ignores the return value to send, and
 * touches nothing beyond that byte. (test_app_portal.c separately pins the stronger
 * "buffer completely untouched" property for the portal's document builder, where
 * it does hold.)
 */
static void test_response_bound_is_enforced(void)
{
    session_line_t line;
    char tiny[8];
    app_diag_response_t response;

    CHECK(parse_line("request 1 status", &line) == APP_DIAG_OK, "status parses");

    memset(&response, 0, sizeof(response));
    response.request_id = 1u;
    response.command = APP_DIAG_CMD_STATUS;
    response.error = APP_DIAG_OK;
    response.stage = APP_DIAG_STAGE_DONE;
    response.stage_state = APP_DIAG_STAGE_STATE_DONE;

    memset(tiny, 0x5A, sizeof(tiny));
    CHECK(app_diag_format_response(&response, tiny, sizeof(tiny)) == 0u,
          "a buffer too small for the header is refused");
    CHECK(tiny[0] == '\0', "and no partial response is left for a caller to send");
    for (size_t i = 1u; i < sizeof(tiny); i++) {
        CHECK(tiny[i] == 0x5A, "byte %u beyond the first was written anyway", (unsigned)i);
        break; /* one report is enough; the loop exists to name the byte */
    }
}

int main(void)
{
    printf("headless acceptance session tests\n");

    test_every_checklist_command_parses();
    test_the_request_id_survives();
    test_unknown_and_malformed_lines_are_refused();
    test_scan_stages_are_reported_honestly();
    test_control_refusals_render();
    test_payload_responses();
    test_response_bound_is_enforced();

    printf("app_cli_session: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
