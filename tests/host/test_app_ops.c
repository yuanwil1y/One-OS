/*
 * Host regression tests for the application operation gate and scan lifecycle.
 *
 * The same app_ops.c source is compiled into the target firmware, so these
 * tests verify BUSY handling, cancellation and terminal-state reporting without
 * hardware.
 */

#include <stdio.h>
#include <string.h>

#include "app_ops.h"

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

static void test_initial_state(void)
{
    app_ops_t ops;

    app_ops_init(&ops);
    CHECK(app_ops_state(&ops) == APP_OP_STATE_IDLE, "must start idle");

    /* No scan has run yet, so no snapshot exists. */
    app_scan_status_t snap;
    CHECK(!app_ops_scan_snapshot(&ops, &snap), "no snapshot before first scan");
    CHECK(ops.scan.generation == 0u, "generation starts at zero");
}

static void test_generation_advances(void)
{
    app_ops_t ops;
    app_scan_status_t scan;

    app_ops_init(&ops);

    CHECK(app_ops_scan_begin(&ops, 10u, &scan) == APP_OPS_OK, "first scan begins");
    CHECK(scan.generation == 1u, "first generation is 1, got %u", scan.generation);
    CHECK(scan.request_id == 10u, "request id recorded");
    CHECK(app_ops_state(&ops) == APP_OP_STATE_SCANNING, "state is scanning");

    /* Finish and confirm generation only ever moves forward. */
    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "first scan finishes");
    CHECK(app_ops_state(&ops) == APP_OP_STATE_IDLE, "returns to idle");

    CHECK(app_ops_scan_begin(&ops, 11u, &scan) == APP_OPS_OK, "second scan begins");
    CHECK(scan.generation == 2u, "second generation is 2, got %u", scan.generation);
    CHECK(scan.canceled == false, "fresh generation is not canceled");
    CHECK(scan.partial == false, "fresh generation is not partial");
    CHECK(scan.truncated == false, "fresh generation is not truncated");
    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "second scan finishes");
}

static void test_busy_while_scanning(void)
{
    app_ops_t ops;
    app_scan_status_t scan;
    app_scan_status_t second;

    app_ops_init(&ops);
    CHECK(app_ops_scan_begin(&ops, 1u, &scan) == APP_OPS_OK, "scan begins");

    /* A second scan must be refused, not silently replace the running one. */
    CHECK(app_ops_scan_begin(&ops, 2u, &second) == APP_OPS_ERR_BUSY,
          "concurrent scan must report BUSY");
    CHECK(ops.scan.generation == 1u, "busy attempt must not advance generation");
    CHECK(ops.scan.request_id == 1u, "busy attempt must not steal the request id");

    /* Competing operations are refused too. */
    CHECK(app_ops_begin_exclusive(&ops, APP_OP_STATE_WEB_MANAGEMENT) ==
              APP_OPS_ERR_BUSY,
          "web management must be refused during a scan");
    CHECK(app_ops_begin_exclusive(&ops, APP_OP_STATE_COMMISSIONING) ==
              APP_OPS_ERR_BUSY,
          "commissioning must be refused during a scan");

    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "scan finishes");
}

static void test_exclusive_operations(void)
{
    app_ops_t ops;
    app_scan_status_t scan;

    app_ops_init(&ops);

    CHECK(app_ops_begin_exclusive(&ops, APP_OP_STATE_WEB_MANAGEMENT) == APP_OPS_OK,
          "web management begins when idle");
    CHECK(app_ops_state(&ops) == APP_OP_STATE_WEB_MANAGEMENT, "state updated");

    /* A scan must not start while the portal owns the radio. */
    CHECK(app_ops_scan_begin(&ops, 1u, &scan) == APP_OPS_ERR_BUSY,
          "scan refused during web management");

    /* A second exclusive operation must not stack. */
    CHECK(app_ops_begin_exclusive(&ops, APP_OP_STATE_COMMISSIONING) ==
              APP_OPS_ERR_BUSY,
          "second exclusive operation refused");

    CHECK(app_ops_end_exclusive(&ops, APP_OP_STATE_WEB_MANAGEMENT) == APP_OPS_OK,
          "web management ends");
    CHECK(app_ops_state(&ops) == APP_OP_STATE_IDLE, "idle after portal");

    /* Ending the wrong operation must not release the gate. */
    CHECK(app_ops_begin_exclusive(&ops, APP_OP_STATE_COMMISSIONING) == APP_OPS_OK,
          "commissioning begins");
    CHECK(app_ops_end_exclusive(&ops, APP_OP_STATE_WEB_MANAGEMENT) ==
              APP_OPS_ERR_NOT_ACTIVE,
          "cannot end an operation that is not active");
    CHECK(app_ops_state(&ops) == APP_OP_STATE_COMMISSIONING,
          "gate still held by commissioning");
    CHECK(app_ops_end_exclusive(&ops, APP_OP_STATE_COMMISSIONING) == APP_OPS_OK,
          "commissioning ends");

    /* Idle is not a valid exclusive target. */
    CHECK(app_ops_begin_exclusive(&ops, APP_OP_STATE_IDLE) ==
              APP_OPS_ERR_INVALID_ARGUMENT,
          "IDLE is not a valid exclusive state");
    CHECK(app_ops_begin_exclusive(&ops, APP_OP_STATE_SCANNING) ==
              APP_OPS_ERR_INVALID_ARGUMENT,
          "SCANNING is not a valid exclusive state");
}

static void test_stage_lifecycle(void)
{
    app_ops_t ops;
    app_scan_status_t scan;

    app_ops_init(&ops);
    CHECK(app_ops_scan_begin(&ops, 1u, &scan) == APP_OPS_OK, "scan begins");

    CHECK(app_ops_stage_begin(&ops, APP_STAGE_WIFI_RF) == APP_OPS_OK,
          "wifi stage begins");
    CHECK(ops.scan.current == APP_STAGE_WIFI_RF, "current stage is wifi");
    CHECK(app_ops_stage_end(&ops, APP_STAGE_WIFI_RF, APP_STAGE_STATE_DONE) ==
              APP_OPS_OK,
          "wifi stage ends");
    CHECK(ops.scan.states[APP_STAGE_WIFI_RF] == APP_STAGE_STATE_DONE,
          "wifi stage recorded done");

    /* A stage cannot be started twice or ended without starting. */
    CHECK(app_ops_stage_begin(&ops, APP_STAGE_WIFI_RF) == APP_OPS_ERR_INVALID_STATE,
          "stage cannot restart");
    CHECK(app_ops_stage_end(&ops, APP_STAGE_BLE_RF, APP_STAGE_STATE_DONE) ==
              APP_OPS_ERR_INVALID_STATE,
          "stage cannot end without starting");

    /* A partial stage marks the whole scan partial. */
    CHECK(app_ops_stage_begin(&ops, APP_STAGE_BLE_RF) == APP_OPS_OK,
          "ble stage begins");
    CHECK(app_ops_stage_end(&ops, APP_STAGE_BLE_RF, APP_STAGE_STATE_PARTIAL) ==
              APP_OPS_OK,
          "ble stage partial");
    CHECK(ops.scan.partial, "partial flag set by partial stage");

    /* Out-of-range and invalid arguments are rejected. */
    CHECK(app_ops_stage_begin(&ops, (app_scan_stage_t)999) ==
              APP_OPS_ERR_INVALID_ARGUMENT,
          "invalid stage index rejected");
    CHECK(app_ops_stage_end(&ops, APP_STAGE_WIFI_RF, APP_STAGE_STATE_RUNNING) ==
              APP_OPS_ERR_INVALID_STATE,
          "non-terminal stage state rejected");

    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "scan finishes");
}

static void test_unreached_stages_are_skipped(void)
{
    app_ops_t ops;
    app_scan_status_t scan;

    app_ops_init(&ops);
    CHECK(app_ops_scan_begin(&ops, 1u, &scan) == APP_OPS_OK, "scan begins");
    CHECK(app_ops_stage_begin(&ops, APP_STAGE_WIFI_RF) == APP_OPS_OK, "begin");
    CHECK(app_ops_stage_end(&ops, APP_STAGE_WIFI_RF, APP_STAGE_STATE_DONE) ==
              APP_OPS_OK,
          "end");
    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "scan finishes");

    /* Stages never reached must be explicitly SKIPPED, not left PENDING, so a
     * partial report is never mistaken for a complete one. */
    CHECK(scan.states[APP_STAGE_WIFI_RF] == APP_STAGE_STATE_DONE, "wifi done");
    for (int i = (int)APP_STAGE_BLE_RF; i < (int)APP_STAGE_COUNT; ++i) {
        CHECK(scan.states[i] == APP_STAGE_STATE_SKIPPED,
              "stage %s should be skipped, got %s",
              app_scan_stage_name((app_scan_stage_t)i),
              app_stage_state_name(scan.states[i]));
    }
    CHECK(app_ops_stage_completed_count(&scan) == (uint32_t)APP_STAGE_COUNT,
          "every stage is terminal after finish");
}

static void test_cancel_flow(void)
{
    app_ops_t ops;
    app_scan_status_t scan;

    app_ops_init(&ops);

    /* Cancelling nothing is an explicit error, not a silent success. */
    CHECK(app_ops_scan_cancel(&ops) == APP_OPS_ERR_NOT_ACTIVE,
          "cancel without a scan reports NOT_ACTIVE");
    CHECK(!app_ops_scan_is_canceled(&ops), "no scan, no cancel flag");

    CHECK(app_ops_scan_begin(&ops, 1u, &scan) == APP_OPS_OK, "scan begins");
    CHECK(app_ops_stage_begin(&ops, APP_STAGE_WIFI_RF) == APP_OPS_OK, "wifi begins");
    CHECK(app_ops_scan_cancel(&ops) == APP_OPS_OK, "cancel accepted");
    CHECK(app_ops_scan_is_canceled(&ops), "cancel flag set");

    /* A canceled scan must not start further stages. */
    CHECK(app_ops_stage_begin(&ops, APP_STAGE_BLE_RF) == APP_OPS_ERR_INVALID_STATE,
          "canceled scan must not start a new stage");

    /* A stage that finishes after cancellation is recorded as canceled, never
     * as cleanly done. */
    CHECK(app_ops_stage_end(&ops, APP_STAGE_WIFI_RF, APP_STAGE_STATE_DONE) ==
              APP_OPS_OK,
          "stage end accepted");
    CHECK(ops.scan.states[APP_STAGE_WIFI_RF] == APP_STAGE_STATE_CANCELED,
          "canceled scan records the stage as canceled, got %s",
          app_stage_state_name(ops.scan.states[APP_STAGE_WIFI_RF]));

    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "scan finishes");
    CHECK(scan.canceled, "snapshot carries cancel flag");
    CHECK(app_ops_state(&ops) == APP_OP_STATE_IDLE, "idle after canceled scan");

    /* Cancelling again is no longer active. */
    CHECK(app_ops_scan_cancel(&ops) == APP_OPS_ERR_NOT_ACTIVE,
          "cancel after finish reports NOT_ACTIVE");

    /* The next scan must start clean. */
    CHECK(app_ops_scan_begin(&ops, 2u, &scan) == APP_OPS_OK, "next scan begins");
    CHECK(!scan.canceled, "cancel flag cleared for the new generation");
    CHECK(scan.generation == 2u, "generation advanced");
    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "next scan finishes");
}

static void test_truncation_flag(void)
{
    app_ops_t ops;
    app_scan_status_t scan;

    app_ops_init(&ops);
    CHECK(app_ops_scan_begin(&ops, 1u, &scan) == APP_OPS_OK, "scan begins");
    CHECK(!ops.scan.truncated, "not truncated initially");

    app_ops_scan_mark_truncated(&ops);
    CHECK(ops.scan.truncated, "truncation recorded");
    CHECK(ops.scan.partial, "truncation implies a partial scan");

    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "scan finishes");
    CHECK(scan.truncated, "snapshot carries truncation flag");
}

static void test_snapshot_after_finish(void)
{
    app_ops_t ops;
    app_scan_status_t scan;
    app_scan_status_t snap;

    app_ops_init(&ops);
    CHECK(app_ops_scan_begin(&ops, 77u, &scan) == APP_OPS_OK, "scan begins");
    CHECK(app_ops_stage_begin(&ops, APP_STAGE_WIFI_RF) == APP_OPS_OK, "begin");
    CHECK(app_ops_stage_end(&ops, APP_STAGE_WIFI_RF, APP_STAGE_STATE_DONE) ==
              APP_OPS_OK,
          "end");
    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_OK, "scan finishes");

    /* The report stays readable after the scan returns to IDLE. */
    CHECK(app_ops_scan_snapshot(&ops, &snap), "snapshot available after finish");
    CHECK(snap.generation == 1u, "snapshot generation");
    CHECK(snap.request_id == 77u, "snapshot request id");
    CHECK(snap.states[APP_STAGE_WIFI_RF] == APP_STAGE_STATE_DONE, "snapshot stage");
}

static void test_invalid_arguments(void)
{
    app_ops_t ops;
    app_scan_status_t scan;

    app_ops_init(&ops);

    CHECK(app_ops_scan_begin(NULL, 1u, &scan) == APP_OPS_ERR_INVALID_ARGUMENT,
          "null ops rejected");
    CHECK(app_ops_scan_finish(NULL, &scan) == APP_OPS_ERR_INVALID_ARGUMENT,
          "null ops finish rejected");
    CHECK(app_ops_stage_begin(NULL, APP_STAGE_WIFI_RF) ==
              APP_OPS_ERR_INVALID_ARGUMENT,
          "null ops stage begin rejected");

    /* Finishing without an active scan is an explicit error. */
    CHECK(app_ops_scan_finish(&ops, &scan) == APP_OPS_ERR_NOT_ACTIVE,
          "finish without scan reports NOT_ACTIVE");

    /* Stages cannot be touched outside a scan. */
    CHECK(app_ops_stage_begin(&ops, APP_STAGE_WIFI_RF) == APP_OPS_ERR_INVALID_STATE,
          "stage begin outside scan rejected");
    CHECK(app_ops_stage_end(&ops, APP_STAGE_WIFI_RF, APP_STAGE_STATE_DONE) ==
              APP_OPS_ERR_INVALID_STATE,
          "stage end outside scan rejected");

    /* A NULL output pointer is allowed (the report is simply discarded). */
    CHECK(app_ops_scan_begin(&ops, 1u, NULL) == APP_OPS_OK,
          "null out accepted on begin");
    CHECK(app_ops_scan_finish(&ops, NULL) == APP_OPS_OK,
          "null out accepted on finish");
    CHECK(!app_ops_scan_snapshot(&ops, NULL), "null snapshot out returns false");
    CHECK(!app_ops_scan_snapshot(NULL, &scan), "null ops snapshot returns false");
}

static void test_names_are_stable(void)
{
    CHECK(strcmp(app_ops_state_name(APP_OP_STATE_IDLE), "idle") == 0, "idle name");
    CHECK(strcmp(app_ops_state_name(APP_OP_STATE_SCANNING), "scanning") == 0,
          "scanning name");
    CHECK(strcmp(app_scan_stage_name(APP_STAGE_WIFI_RF), "wifi_rf") == 0,
          "wifi_rf name");
    CHECK(strcmp(app_scan_stage_name(APP_STAGE_MATERIALIZE), "materialize") == 0,
          "materialize name");
    CHECK(strcmp(app_stage_state_name(APP_STAGE_STATE_SKIPPED), "skipped") == 0,
          "skipped name");
    CHECK(strcmp(app_stage_state_name(APP_STAGE_STATE_CANCELED), "canceled") == 0,
          "canceled name");
}

int main(void)
{
    test_initial_state();
    test_generation_advances();
    test_busy_while_scanning();
    test_exclusive_operations();
    test_stage_lifecycle();
    test_unreached_stages_are_skipped();
    test_cancel_flow();
    test_truncation_flag();
    test_snapshot_after_finish();
    test_invalid_arguments();
    test_names_are_stable();

    printf("app_ops: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
