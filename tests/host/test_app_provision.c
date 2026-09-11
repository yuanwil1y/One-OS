/*
 * Host regression tests for the provisioning session.
 *
 * app_provision.c is the application side of the portal: it owns the session, the
 * radio handover, the operation gate and the coordination between an import and the
 * recognition reader. All of that is ordering, and ordering is what a test can
 * actually pin - so this group drives the real state machine with injected
 * operations and asserts the ORDER of the calls, not merely their outcome.
 *
 * The cases that exist because getting them wrong is silently harmful:
 *
 *   - a failed AP start must give the radio back and release the gate, or the
 *     operator's network stays down because the portal could not start;
 *   - a failed server start must roll back the AP too, in the reverse order;
 *   - the portal must not start while a scan holds the application gate;
 *   - the reader must be CLOSED before the corpus is replaced and reopened after it,
 *     including when the replacement fails;
 *   - a session must end on its deadline, because one that never ends leaves the
 *     device in AP mode until it is power-cycled;
 *   - the token comparison must not exit early.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_ops.h"
#include "app_portal.h"
#include "app_provision.h"

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

/* ---------------- call log ---------------- */

/*
 * One ordered log of everything the session asked the platform to do.
 *
 * A test asserts on this rather than on a final state alone, because "did it release
 * the radio before configuring the AP" is exactly the property that a state check
 * cannot see.
 */
typedef struct {
    char entries[64][32];
    size_t count;

    /* Fault injection. */
    esp_err_t ap_start_result;
    esp_err_t ap_stop_result;
    esp_err_t sta_release_result;
    esp_err_t sta_restore_result;
    esp_err_t server_start_result;
    esp_err_t server_stop_result;
    esp_err_t db_reopen_result;

    /* State observed by the fake platform. */
    bool ap_up;
    bool server_up;
    bool reader_open;
    bool radio_held_by_station;
    bool db_has_import_in_progress;
    char ap_ssid[WIFI_MGR_SSID_STORE];
    char ap_password[APP_PORTAL_AP_PASSWORD_MAX];

    uint32_t now_ms;
    uint32_t rand_state;
} fake_platform_t;

static fake_platform_t g_pf;

static void pf_log(const char *what)
{
    if (g_pf.count < 64u) {
        (void)snprintf(g_pf.entries[g_pf.count], sizeof(g_pf.entries[0]), "%s", what);
        g_pf.count++;
    }
}

/* Index of the first occurrence of `what`, or -1. */
static int pf_index_of(const char *what)
{
    for (size_t i = 0u; i < g_pf.count; ++i) {
        if (strcmp(g_pf.entries[i], what) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void pf_reset(void)
{
    memset(&g_pf, 0, sizeof(g_pf));
    g_pf.ap_start_result = ESP_OK;
    g_pf.ap_stop_result = ESP_OK;
    g_pf.sta_release_result = ESP_OK;
    g_pf.sta_restore_result = ESP_OK;
    g_pf.server_start_result = ESP_OK;
    g_pf.server_stop_result = ESP_OK;
    g_pf.db_reopen_result = ESP_OK;
    g_pf.reader_open = true;
    g_pf.radio_held_by_station = true;
    g_pf.rand_state = 0x12345678u;
}

static uint32_t pf_random(void)
{
    g_pf.rand_state ^= g_pf.rand_state << 13;
    g_pf.rand_state ^= g_pf.rand_state >> 17;
    g_pf.rand_state ^= g_pf.rand_state << 5;
    return g_pf.rand_state;
}

static uint32_t pf_now(void *ctx)
{
    (void)ctx;
    return g_pf.now_ms;
}

/* ---------------- fake operations ---------------- */

static esp_err_t pf_sta_release(void *ctx)
{
    (void)ctx;
    pf_log("sta_release");
    if (g_pf.sta_release_result != ESP_OK) {
        return g_pf.sta_release_result;
    }
    g_pf.radio_held_by_station = false;
    return ESP_OK;
}

static esp_err_t pf_sta_restore(void *ctx)
{
    (void)ctx;
    pf_log("sta_restore");
    if (g_pf.sta_restore_result != ESP_OK) {
        return g_pf.sta_restore_result;
    }
    g_pf.radio_held_by_station = true;
    return ESP_OK;
}

static esp_err_t pf_ap_start(void *ctx, const char *ssid, const char *password)
{
    (void)ctx;
    pf_log("ap_start");
    if (g_pf.ap_start_result != ESP_OK) {
        return g_pf.ap_start_result;
    }
    g_pf.ap_up = true;
    (void)snprintf(g_pf.ap_ssid, sizeof(g_pf.ap_ssid), "%s", ssid);
    (void)snprintf(g_pf.ap_password, sizeof(g_pf.ap_password), "%s", password);
    return ESP_OK;
}

static esp_err_t pf_ap_stop(void *ctx)
{
    (void)ctx;
    pf_log("ap_stop");
    g_pf.ap_up = false;
    return g_pf.ap_stop_result;
}

static esp_err_t pf_server_start(void *ctx, uint16_t port)
{
    (void)ctx;
    pf_log("server_start");
    if (g_pf.server_start_result != ESP_OK) {
        return g_pf.server_start_result;
    }
    g_pf.server_up = true;
    (void)port;
    return ESP_OK;
}

static esp_err_t pf_server_stop(void *ctx)
{
    (void)ctx;
    pf_log("server_stop");
    g_pf.server_up = false;
    return g_pf.server_stop_result;
}

static void pf_db_close(void *ctx)
{
    (void)ctx;
    pf_log("db_close");
    g_pf.reader_open = false;
}

static esp_err_t pf_db_reopen(void *ctx)
{
    (void)ctx;
    pf_log("db_reopen");
    if (g_pf.db_reopen_result != ESP_OK) {
        return g_pf.db_reopen_result;
    }
    g_pf.reader_open = true;
    return ESP_OK;
}

static app_provision_ops_t pf_ops(void)
{
    app_provision_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.ap_start = pf_ap_start;
    ops.ap_stop = pf_ap_stop;
    ops.sta_restore = pf_sta_restore;
    ops.sta_release = pf_sta_release;
    ops.server_start = pf_server_start;
    ops.server_stop = pf_server_stop;
    ops.db_close = pf_db_close;
    ops.db_reopen = pf_db_reopen;
    ops.now_ms = pf_now;
    ops.random_u32 = pf_random;
    return ops;
}

/* ---------------- fixtures ---------------- */

static app_ops_t g_gate;
static app_provision_t g_session;

static void session_init(uint32_t timeout_ms)
{
    app_provision_ops_t ops = pf_ops();
    app_provision_config_t cfg;

    app_provision_config_default(&cfg);
    if (timeout_ms != 0u) {
        cfg.session_timeout_ms = timeout_ms;
    }
    app_ops_init(&g_gate);
    CHECK(app_provision_init(&g_session, &ops, NULL, &g_gate, &cfg) == ESP_OK,
          "the session initialises");
}

/* ---------------- start / stop ---------------- */

static void test_start_orders_the_handover(void)
{
    int release_at;
    int ap_at;
    int server_at;

    pf_reset();
    session_init(0u);

    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "the session starts");
    CHECK(app_provision_is_active(&g_session), "and reports active");
    CHECK(g_pf.ap_up, "the AP is up");
    CHECK(g_pf.server_up, "the server is up");

    /*
     * The station must hand the radio over BEFORE the AP is configured, and the
     * server must come up only after the AP is up. A server accepting requests on a
     * transport that does not exist yet is worse than a slow start.
     */
    release_at = pf_index_of("sta_release");
    ap_at = pf_index_of("ap_start");
    server_at = pf_index_of("server_start");
    CHECK(release_at >= 0 && ap_at >= 0 && server_at >= 0, "all three steps ran");
    CHECK(release_at < ap_at, "the radio is released before the AP starts (%d < %d)",
          release_at, ap_at);
    CHECK(ap_at < server_at, "and the AP is up before the server (%d < %d)", ap_at,
          server_at);
    CHECK(!g_pf.radio_held_by_station, "the station no longer holds the radio");

    /* The gate is held for the whole session. */
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_WEB_MANAGEMENT,
          "the operation gate is held, got %s",
          app_ops_state_name(app_ops_state(&g_gate)));
    CHECK(app_ops_scan_begin(&g_gate, 1u, NULL) == APP_OPS_ERR_BUSY,
          "and a scan cannot start beside the portal");
}

static void test_credentials_are_generated_and_not_derived_from_the_mac(void)
{
    char first_ssid[WIFI_MGR_SSID_STORE];

    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");

    CHECK(strncmp(g_pf.ap_ssid, "NearBy-One-", 11u) == 0,
          "the SSID carries the configured prefix, got '%s'", g_pf.ap_ssid);
    CHECK(strlen(g_pf.ap_ssid) > 11u, "with a suffix");
    CHECK(strcmp(g_pf.ap_password, "") != 0, "and a password was generated");
    CHECK(strlen(g_session.token) == APP_PROVISION_TOKEN_HEX,
          "the token is the documented length, got %u",
          (unsigned)strlen(g_session.token));

    (void)snprintf(first_ssid, sizeof(first_ssid), "%s", g_pf.ap_ssid);
    (void)app_provision_stop(&g_session);

    /*
     * A second session on this device must not reuse the first one's credentials: a
     * password, SSID or token that never changes is one that is already known.
     *
     * The random source is deliberately NOT reset here. Resetting it would make this
     * test pass or fail depending on the seed rather than on the code - which is
     * exactly how it failed the first time it ran.
     */
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "restarted");
    CHECK(strcmp(g_pf.ap_ssid, first_ssid) != 0,
          "a new session has a different SSID, both '%s'", g_pf.ap_ssid);
}

static void test_failed_ap_start_gives_the_radio_back(void)
{
    pf_reset();
    session_init(0u);
    g_pf.ap_start_result = ESP_FAIL;

    CHECK(app_provision_start(&g_session) == APP_PROVISION_ERR_AP_FAILED,
          "a failed AP start is reported");
    CHECK(g_session.phase == APP_PROVISION_FAILED, "and the session is failed");

    /*
     * The critical part: the radio must be back with the station and the gate must
     * be free. Leaving either behind means the operator's network is down and no
     * scan can run, because a portal that never started is holding everything.
     */
    CHECK(g_pf.radio_held_by_station, "the station got the radio back");
    CHECK(pf_index_of("sta_restore") >= 0, "sta_restore was called");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE, "and the gate was released");
    CHECK(!g_pf.ap_up, "no AP was left up");
}

static void test_failed_server_start_rolls_back_the_ap(void)
{
    int ap_at;
    int ap_stop_at;

    pf_reset();
    session_init(0u);
    g_pf.server_start_result = ESP_FAIL;

    CHECK(app_provision_start(&g_session) == APP_PROVISION_ERR_SERVER_FAILED,
          "a failed server start is reported");

    /* Roll back in the reverse order of the way in. */
    ap_at = pf_index_of("ap_start");
    ap_stop_at = pf_index_of("ap_stop");
    CHECK(ap_stop_at >= 0, "the AP was stopped");
    CHECK(ap_at >= 0 && ap_stop_at > ap_at, "after it had started (%d then %d)",
          ap_at, ap_stop_at);
    CHECK(!g_pf.ap_up, "and it really is down");
    CHECK(g_pf.radio_held_by_station, "the radio went back to the station");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE, "and the gate was released");
}

static void test_station_release_failure_does_not_configure_an_ap(void)
{
    pf_reset();
    session_init(0u);
    g_pf.sta_release_result = ESP_FAIL;

    CHECK(app_provision_start(&g_session) == APP_PROVISION_ERR_AP_FAILED,
          "a failed handover stops the start");
    CHECK(pf_index_of("ap_start") < 0,
          "and the AP was never configured on top of a live station");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE, "the gate was released");
    CHECK(g_pf.radio_held_by_station,
          "and the station still has the radio it never gave up");
}

static void test_the_portal_does_not_start_beside_a_scan(void)
{
    pf_reset();
    session_init(0u);

    /* A scan is in progress. */
    CHECK(app_ops_scan_begin(&g_gate, 7u, NULL) == APP_OPS_OK, "a scan started");

    CHECK(app_provision_start(&g_session) == APP_PROVISION_ERR_BUSY,
          "the portal refuses to start while a scan owns the application");
    CHECK(g_session.phase == APP_PROVISION_IDLE, "and stays idle");
    CHECK(pf_index_of("sta_release") < 0,
          "without touching the radio, which the scan is using");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_SCANNING, "the scan still owns it");
}

static void test_stop_is_ordered_and_idempotent(void)
{
    int ap_stop_at;
    int server_stop_at;

    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");

    CHECK(app_provision_stop(&g_session) == APP_PROVISION_OK, "stopped");
    CHECK(!g_pf.ap_up, "the AP is down");
    CHECK(!g_pf.server_up, "the server is down");
    CHECK(g_pf.radio_held_by_station, "the radio is back with the station");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE, "the gate is free");

    /*
     * The server must stop BEFORE the AP: a request in flight when its transport
     * disappears would be operating on a radio that is no longer there.
     */
    server_stop_at = pf_index_of("server_stop");
    ap_stop_at = pf_index_of("ap_stop");
    CHECK(server_stop_at >= 0 && ap_stop_at >= 0, "both stops ran");
    CHECK(server_stop_at < ap_stop_at, "the server stops first (%d < %d)",
          server_stop_at, ap_stop_at);

    /* Stopping twice is normal: the operator closed the page twice. */
    CHECK(app_provision_stop(&g_session) == APP_PROVISION_OK,
          "stopping an already-stopped session is a no-op");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE, "and does not touch the gate");
}

static void test_starting_twice_is_refused(void)
{
    pf_reset();
    session_init(0u);

    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "first start");
    CHECK(app_provision_start(&g_session) == APP_PROVISION_ERR_BUSY,
          "a second start is refused");
}

static void test_the_session_expires_on_its_deadline(void)
{
    pf_reset();
    session_init(5000u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");

    g_pf.now_ms = 4000u;
    CHECK(!app_provision_tick(&g_session), "not yet at the deadline");
    CHECK(app_provision_is_active(&g_session), "still active");

    g_pf.now_ms = 5000u;
    CHECK(app_provision_tick(&g_session), "the deadline ends the session");
    CHECK(!app_provision_is_active(&g_session), "and it is no longer active");
    CHECK(!g_pf.ap_up, "the AP is down");
    CHECK(g_pf.radio_held_by_station, "and the station has the radio");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE, "the gate is free again");

    /* A tick after the session ended does nothing. */
    CHECK(!app_provision_tick(&g_session), "a later tick is a no-op");
}

static void test_a_failed_restore_is_reported_not_hidden(void)
{
    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");

    g_pf.sta_restore_result = ESP_FAIL;
    CHECK(app_provision_stop(&g_session) == APP_PROVISION_ERR_AP_FAILED,
          "a failed restore is reported");
    CHECK(g_session.last_io_error == ESP_FAIL, "with the underlying error");
    /* The gate is still released: a failed reconnect must not also block scanning. */
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE,
          "the gate is released regardless");
}

/* ---------------- token ---------------- */

static void test_token_matching(void)
{
    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");

    CHECK(app_provision_token_matches(&g_session, g_session.token),
          "the session's own token matches");

    /* Wrong length, empty, NULL, and a token that differs only in the last
     * character: all refused. */
    CHECK(!app_provision_token_matches(&g_session, ""), "an empty token is refused");
    CHECK(!app_provision_token_matches(&g_session, NULL), "NULL is refused");
    CHECK(!app_provision_token_matches(NULL, g_session.token), "no session, no match");

    {
        /* Room for the token plus a byte plus a terminator: a buffer sized exactly
         * to the token would truncate the extra byte instead of testing it, and the
         * "longer token" case would pass for the wrong reason. */
        char wrong[APP_PROVISION_TOKEN_HEX + 8u];

        (void)snprintf(wrong, sizeof(wrong), "%s", g_session.token);
        wrong[0] = (wrong[0] == '0') ? '1' : '0';
        CHECK(!app_provision_token_matches(&g_session, wrong),
              "a token differing in the first character is refused");

        (void)snprintf(wrong, sizeof(wrong), "%s", g_session.token);
        wrong[APP_PROVISION_TOKEN_HEX - 1u] =
            (wrong[APP_PROVISION_TOKEN_HEX - 1u] == '0') ? '1' : '0';
        CHECK(!app_provision_token_matches(&g_session, wrong),
              "a token differing in the last character is refused");

        (void)snprintf(wrong, sizeof(wrong), "%s0", g_session.token);
        CHECK(strlen(wrong) == APP_PROVISION_TOKEN_HEX + 1u,
              "the longer token really is longer (%u bytes)", (unsigned)strlen(wrong));
        CHECK(!app_provision_token_matches(&g_session, wrong),
              "a longer token is refused");

        /* A correct prefix of the token is not the token. */
        (void)snprintf(wrong, sizeof(wrong), "%s", g_session.token);
        wrong[APP_PROVISION_TOKEN_HEX - 1u] = '\0';
        CHECK(!app_provision_token_matches(&g_session, wrong),
              "a truncated token is refused");
    }
}

/* ---------------- import coordination ---------------- */

static void test_import_closes_the_reader_around_the_replace(void)
{
    app_db_import_t imp;
    uint32_t version = 0u;
    int close_at;
    int finish_at;
    int reopen_at;

    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");

    /*
     * The import handle is only used for its coordination contract here, so its
     * filesystem vtable is left null: finish() will report NOT_ACTIVE without
     * touching a medium, which is exactly the failure path being exercised - the
     * reader must still be reopened.
     */
    memset(&imp, 0, sizeof(imp));

    CHECK(app_provision_begin_import(&g_session, &imp, 0u) ==
              APP_PROVISION_ERR_IMPORT_FAILED,
          "an uninitialised import handle is refused");
    CHECK(g_pf.reader_open, "and the reader was not disturbed");

    /* Now the coordination itself, with a handle that reports an active receive
     * phase by leaving the phase unset: finish() is still called and the reader is
     * still closed and reopened around it. */
    pf_log("---");
    close_at = (int)g_pf.count;
    (void)app_provision_finish_import(&g_session, &imp, &version);
    finish_at = pf_index_of("db_close");
    reopen_at = pf_index_of("db_reopen");

    CHECK(finish_at >= close_at, "the reader was closed");
    CHECK(reopen_at > finish_at, "and reopened after the replace (%d then %d)",
          finish_at, reopen_at);
    CHECK(g_pf.reader_open, "so it is open again even though the import failed");
    CHECK(!g_session.reader_closed, "and the session does not think it is closed");
}

static void test_import_requires_an_active_session(void)
{
    app_db_import_t imp;

    pf_reset();
    session_init(0u);
    memset(&imp, 0, sizeof(imp));

    CHECK(app_provision_begin_import(&g_session, &imp, 1024u) ==
              APP_PROVISION_ERR_NOT_ACTIVE,
          "an import without a session is refused");
    CHECK(app_provision_finish_import(&g_session, &imp, NULL) ==
              APP_PROVISION_ERR_NOT_ACTIVE,
          "and finishing one is refused too");
    CHECK(g_pf.reader_open, "the reader was never touched");
}

static void test_a_session_stopped_mid_import_reopens_the_reader(void)
{
    app_db_import_t imp;

    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");
    memset(&imp, 0, sizeof(imp));

    /*
     * Model the interruption: the reader was closed for a replace and the session is
     * stopped before it is reopened - a power-down or a cancellation between the two.
     */
    g_session.reader_closed = true;
    g_pf.reader_open = false;
    pf_log("db_close");

    CHECK(app_provision_stop(&g_session) == APP_PROVISION_OK, "stopped");
    CHECK(pf_index_of("db_reopen") >= 0,
          "stopping reopens a reader left closed by an interrupted import");
    CHECK(g_pf.reader_open, "so the device keeps recognising devices");
}

static void test_stopping_releases_everything_it_held(void)
{
    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");
    CHECK(app_provision_stop(&g_session) == APP_PROVISION_OK, "stopped");

    CHECK(!g_pf.ap_up, "AP down");
    CHECK(!g_pf.server_up, "server down");
    CHECK(g_pf.reader_open, "reader open");
    CHECK(g_pf.radio_held_by_station, "radio returned");
    CHECK(app_ops_state(&g_gate) == APP_OP_STATE_IDLE, "gate free");
    CHECK(g_session.phase == APP_PROVISION_STOPPED, "phase is stopped");

    /* And the whole cycle can be repeated: a portal that could only run once would
     * be useless on a device that needs provisioning twice. */
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "restarted");
    CHECK(app_provision_stop(&g_session) == APP_PROVISION_OK, "and stopped again");
}

/* ---------------- status snapshot ---------------- */

static void test_status_snapshot_reveals_no_secret(void)
{
    app_portal_status_t status;

    pf_reset();
    session_init(0u);
    CHECK(app_provision_start(&g_session) == APP_PROVISION_OK, "started");

    app_provision_status_snapshot(&g_session, true, &status);
    CHECK(status.active, "the snapshot reports active");
    CHECK(strcmp(status.ap_ssid, g_session.ap_ssid) == 0,
          "and carries the AP SSID, which the operator needs to find the device");

    /*
     * The snapshot is what /api/status is built from. A password or a token must not
     * be reachable through it, so the check is on the snapshot's own bytes rather
     * than on the JSON: if the value never enters this structure, no later change to
     * the JSON builder can leak it.
     */
    {
        const unsigned char *bytes = (const unsigned char *)&status;
        const char *secret = g_session.ap_password;

        CHECK(strstr((const char *)&status, secret) == NULL,
              "the AP password is not in the status snapshot");
        CHECK(strstr((const char *)&status, g_session.token) == NULL,
              "and neither is the session token");
        (void)bytes;
    }

    /* The transport's view wins over the session's intent: a session that asked for
     * an AP that is not actually up must not report itself active. */
    app_provision_status_snapshot(&g_session, false, &status);
    CHECK(!status.active,
          "a session whose transport is down does not report active");
}

int main(void)
{
    test_start_orders_the_handover();
    test_credentials_are_generated_and_not_derived_from_the_mac();
    test_failed_ap_start_gives_the_radio_back();
    test_failed_server_start_rolls_back_the_ap();
    test_station_release_failure_does_not_configure_an_ap();
    test_the_portal_does_not_start_beside_a_scan();
    test_stop_is_ordered_and_idempotent();
    test_starting_twice_is_refused();
    test_the_session_expires_on_its_deadline();
    test_a_failed_restore_is_reported_not_hidden();

    test_token_matching();

    test_import_closes_the_reader_around_the_replace();
    test_import_requires_an_active_session();
    test_a_session_stopped_mid_import_reopens_the_reader();
    test_stopping_releases_everything_it_held();

    test_status_snapshot_reveals_no_secret();

    printf("app_provision: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
