/*
 * Host regression tests for the application scan policy and bounded evidence.
 *
 * The same app_scan.c source is compiled into the firmware, so these tests pin
 * the decisions that must not regress: an unavailable protocol is skipped rather
 * than reported done, LAN stages require an IP, and evidence tables merge
 * instead of duplicating.
 */

#include <stdio.h>
#include <string.h>

#include "app_scan.h"
#include "app_str.h"

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

static app_scan_inputs_t inputs_ready(void)
{
    app_scan_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.wifi_configured = true;
    in.wifi_connected = true;
    in.wifi_driver_acquired = true;
    in.ble_available = true;
    in.has_ip = true;
    return in;
}

static void test_wifi_stage_requires_driver(void)
{
    app_scan_inputs_t in = inputs_ready();
    app_scan_stage_plan_t plan;

    plan = app_scan_plan_stage(&in, APP_STAGE_WIFI_RF);
    CHECK(plan.action == APP_STAGE_ACTION_RUN, "wifi runs when driver acquired");
    CHECK(plan.terminal_state == APP_STAGE_STATE_PENDING, "wifi pending when running");

    /* The Kismet session refuses to start if it cannot take the driver. That
     * must surface as a failed stage, never as a successful empty scan. */
    in.wifi_driver_acquired = false;
    plan = app_scan_plan_stage(&in, APP_STAGE_WIFI_RF);
    CHECK(plan.action == APP_STAGE_ACTION_REJECTED, "wifi rejected without driver");
    CHECK(plan.terminal_state == APP_STAGE_STATE_FAILED, "wifi failed without driver");
    CHECK(plan.reason != NULL && strcmp(plan.reason, "wifi_driver_unavailable") == 0,
          "wifi reason recorded");
}

static void test_ble_stage_requires_backend(void)
{
    app_scan_inputs_t in = inputs_ready();
    app_scan_stage_plan_t plan;

    plan = app_scan_plan_stage(&in, APP_STAGE_BLE_RF);
    CHECK(plan.action == APP_STAGE_ACTION_RUN, "ble runs when available");

    in.ble_available = false;
    plan = app_scan_plan_stage(&in, APP_STAGE_BLE_RF);
    CHECK(plan.action == APP_STAGE_ACTION_UNAVAILABLE, "ble unavailable without backend");
    CHECK(plan.terminal_state == APP_STAGE_STATE_SKIPPED, "ble skipped without backend");
}

static void test_lan_stages_require_ip(void)
{
    const app_scan_stage_t lan[] = {
        APP_STAGE_MDNS, APP_STAGE_SSDP, APP_STAGE_LAN_HOSTS, APP_STAGE_LAN_SERVICES,
    };
    app_scan_inputs_t in = inputs_ready();
    app_scan_stage_plan_t plan;

    for (size_t i = 0; i < sizeof(lan) / sizeof(lan[0]); ++i) {
        plan = app_scan_plan_stage(&in, lan[i]);
        CHECK(plan.action == APP_STAGE_ACTION_RUN,
              "stage %s runs with an IP", app_scan_stage_name(lan[i]));
    }

    /* No IP: skipped, and the reason distinguishes "unconfigured" from
     * "configured but not connected". */
    in.has_ip = false;
    in.wifi_connected = false;
    plan = app_scan_plan_stage(&in, APP_STAGE_MDNS);
    CHECK(plan.action == APP_STAGE_ACTION_SKIP, "mdns skipped without IP");
    CHECK(plan.terminal_state == APP_STAGE_STATE_SKIPPED, "mdns marked skipped");
    CHECK(plan.reason != NULL && strcmp(plan.reason, "no_ip") == 0,
          "configured-but-no-ip reason, got %s", plan.reason ? plan.reason : "(null)");

    in.wifi_configured = false;
    plan = app_scan_plan_stage(&in, APP_STAGE_SSDP);
    CHECK(plan.reason != NULL && strcmp(plan.reason, "wifi_unconfigured") == 0,
          "unconfigured reason, got %s", plan.reason ? plan.reason : "(null)");
}

static void test_unwired_protocols_are_skipped(void)
{
    app_scan_inputs_t in = inputs_ready();
    app_scan_stage_plan_t plan;

    /* Thread and Zigbee have no native backend wired yet. They must be reported
     * as skipped so a scan never implies those protocols were covered. */
    plan = app_scan_plan_stage(&in, APP_STAGE_THREAD);
    CHECK(plan.action == APP_STAGE_ACTION_UNAVAILABLE, "thread unavailable");
    CHECK(plan.terminal_state == APP_STAGE_STATE_SKIPPED, "thread skipped");

    plan = app_scan_plan_stage(&in, APP_STAGE_ZIGBEE);
    CHECK(plan.action == APP_STAGE_ACTION_UNAVAILABLE, "zigbee unavailable");
    CHECK(plan.terminal_state == APP_STAGE_STATE_SKIPPED, "zigbee skipped");
}

static void test_cancel_dominates_planning(void)
{
    app_scan_inputs_t in = inputs_ready();
    app_scan_stage_plan_t plan;

    in.canceled = true;
    plan = app_scan_plan_stage(&in, APP_STAGE_WIFI_RF);
    CHECK(plan.action == APP_STAGE_ACTION_SKIP, "canceled scan does not run wifi");
    CHECK(plan.terminal_state == APP_STAGE_STATE_CANCELED, "canceled stage state");

    /* Even a stage that would otherwise be rejected is reported as canceled,
     * because cancellation is the reason nothing happened. */
    in.wifi_driver_acquired = false;
    plan = app_scan_plan_stage(&in, APP_STAGE_WIFI_RF);
    CHECK(plan.terminal_state == APP_STAGE_STATE_CANCELED, "cancel wins over rejection");
}

static void test_materialize_and_enrichment_always_run(void)
{
    app_scan_inputs_t in = inputs_ready();
    app_scan_stage_plan_t plan;

    in.has_ip = false;
    in.wifi_configured = false;

    plan = app_scan_plan_stage(&in, APP_STAGE_MATERIALIZE);
    CHECK(plan.action == APP_STAGE_ACTION_RUN,
          "materialize runs even with nothing discovered");
    plan = app_scan_plan_stage(&in, APP_STAGE_ENRICHMENT);
    CHECK(plan.action == APP_STAGE_ACTION_RUN, "enrichment always applicable");
}

static void test_null_inputs_rejected(void)
{
    app_scan_stage_plan_t plan = app_scan_plan_stage(NULL, APP_STAGE_WIFI_RF);
    CHECK(plan.action == APP_STAGE_ACTION_REJECTED, "null inputs rejected");
    CHECK(plan.terminal_state == APP_STAGE_STATE_FAILED, "null inputs failed");
}

/* ---------------- evidence store ---------------- */

static app_scan_wifi_t make_wifi(const uint8_t bssid[6], int8_t rssi, uint64_t seen)
{
    app_scan_wifi_t obs;
    memset(&obs, 0, sizeof(obs));
    memcpy(obs.bssid, bssid, 6);
    obs.rssi_last = rssi;
    obs.channel = 6u;
    obs.last_seen_ms = seen;
    obs.first_seen_ms = seen;
    obs.seen_count = 1u;
    obs.role = 1u;
    return obs;
}

static void test_wifi_evidence_merges_by_bssid(void)
{
    app_scan_evidence_t ev;
    const uint8_t a[6] = {0xAA, 0, 0, 0, 0, 1};
    const uint8_t b[6] = {0xBB, 0, 0, 0, 0, 2};
    app_scan_wifi_t obs;
    app_scan_wifi_t *found;

    app_scan_evidence_reset(&ev, 7u);
    CHECK(ev.generation == 7u, "generation recorded");

    obs = make_wifi(a, -50, 100u);
    obs.has_ssid = true;
    obs.ssid_len = 4u;
    memcpy(obs.ssid, "Home", 4u);
    CHECK(app_scan_ingest_wifi(&ev, &obs), "first observation accepted");
    CHECK(ev.wifi_count == 1u, "one device");

    /* Same BSSID again: merged, not duplicated. */
    obs = make_wifi(a, -60, 200u);
    CHECK(app_scan_ingest_wifi(&ev, &obs), "second observation accepted");
    CHECK(ev.wifi_count == 1u, "same BSSID must merge, got %u", (unsigned)ev.wifi_count);
    found = app_scan_find_wifi(&ev, a);
    CHECK(found != NULL, "device findable");
    CHECK(found->rssi_last == -60, "rssi updated to latest");
    CHECK(found->last_seen_ms == 200u, "last seen updated");
    CHECK(found->first_seen_ms == 100u, "first seen preserved");
    CHECK(found->has_ssid && found->ssid_len == 4u, "ssid preserved across merge");

    /* Different BSSID: separate device. */
    obs = make_wifi(b, -70, 300u);
    CHECK(app_scan_ingest_wifi(&ev, &obs), "second device accepted");
    CHECK(ev.wifi_count == 2u, "two devices now");
    CHECK(!app_scan_evidence_truncated(&ev), "no truncation yet");
}

static void test_wifi_capacity_evicts_oldest(void)
{
    app_scan_evidence_t ev;
    uint8_t mac[6] = {0x01, 0, 0, 0, 0, 0};

    app_scan_evidence_reset(&ev, 1u);
    for (uint32_t i = 0u; i < APP_SCAN_MAX_WIFI; ++i) {
        app_scan_wifi_t obs;
        mac[5] = (uint8_t)i;
        obs = make_wifi(mac, -50, (uint64_t)(i + 1u) * 10u);
        CHECK(app_scan_ingest_wifi(&ev, &obs), "fill accepted");
    }
    CHECK(ev.wifi_count == APP_SCAN_MAX_WIFI, "table full");
    CHECK(!app_scan_evidence_truncated(&ev), "filling exactly is not truncation");

    /* One more: oldest (last_seen 10) is evicted, and the drop is counted. */
    mac[5] = 0xFEu;
    {
        app_scan_wifi_t obs = make_wifi(mac, -40, 9999u);
        CHECK(app_scan_ingest_wifi(&ev, &obs), "overflow accepted via eviction");
    }
    CHECK(ev.wifi_count == APP_SCAN_MAX_WIFI, "still bounded");
    CHECK(ev.drops.device_evictions == 1u, "eviction counted, got %u",
          (unsigned)ev.drops.device_evictions);
    CHECK(app_scan_evidence_truncated(&ev), "eviction means truncated evidence");
}

static void test_ble_identity_includes_address_type(void)
{
    app_scan_evidence_t ev;
    app_scan_ble_t obs;

    app_scan_evidence_reset(&ev, 1u);

    memset(&obs, 0, sizeof(obs));
    obs.address[0] = 0x11;
    obs.address_type = 0u;
    obs.last_seen_ms = 10u;
    obs.seen_count = 1u;
    CHECK(app_scan_ingest_ble(&ev, &obs), "ble accepted");
    CHECK(ev.ble_count == 1u, "one ble device");

    /* Same bytes, different address type: a different peer. */
    obs.address_type = 1u;
    CHECK(app_scan_ingest_ble(&ev, &obs), "ble with other type accepted");
    CHECK(ev.ble_count == 2u,
          "address type must be part of BLE identity, got %u", (unsigned)ev.ble_count);

    /* Same address AND type: merged. */
    obs.address_type = 0u;
    obs.last_seen_ms = 50u;
    CHECK(app_scan_ingest_ble(&ev, &obs), "repeat accepted");
    CHECK(ev.ble_count == 2u, "repeat must merge");
}

static void test_ble_later_parsed_adv_wins(void)
{
    app_scan_evidence_t ev;
    app_scan_ble_t obs;
    app_scan_ble_t *found;

    app_scan_evidence_reset(&ev, 1u);

    /* First advertisement has only a short copy (scan response pending). */
    memset(&obs, 0, sizeof(obs));
    obs.address[0] = 0x22;
    obs.address_type = 0u;
    obs.last_seen_ms = 10u;
    obs.seen_count = 1u;
    obs.captured_len = 8u;
    obs.has_parsed_adv = false;
    CHECK(app_scan_ingest_ble(&ev, &obs), "short adv accepted");

    /* Later advertisement carries the parsed structure. */
    obs.last_seen_ms = 20u;
    obs.captured_len = 31u;
    obs.has_parsed_adv = true;
    obs.adv.valid = true;
    obs.adv.name_present = true;
    (void)app_strlcpy(obs.adv.name, "TH Sensor", sizeof(obs.adv.name));
    CHECK(app_scan_ingest_ble(&ev, &obs), "parsed adv accepted");

    found = app_scan_find_ble(&ev, obs.address, obs.address_type);
    CHECK(found != NULL, "ble findable");
    CHECK(found->has_parsed_adv, "parsed advertisement retained");
    CHECK(strcmp(found->adv.name, "TH Sensor") == 0, "parsed name retained");
    CHECK(found->captured_len == 31u, "largest capture retained");
    CHECK(found->seen_count == 2u, "sightings counted, got %u", (unsigned)found->seen_count);
}

static void test_ble_adv_store_is_bounded(void)
{
    app_scan_ble_adv_t slots[2];
    size_t used = 0u;
    uint32_t evictions = 0u;
    app_scan_ble_adv_t adv;

    memset(&adv, 0, sizeof(adv));
    CHECK(app_scan_store_ble_adv(slots, 2u, &used, &adv, &evictions), "first stored");
    CHECK(app_scan_store_ble_adv(slots, 2u, &used, &adv, &evictions), "second stored");
    CHECK(!app_scan_store_ble_adv(slots, 2u, &used, &adv, &evictions),
          "third refused at capacity");
    CHECK(used == 2u, "store stayed bounded");
    CHECK(evictions == 1u, "eviction counted, got %u", (unsigned)evictions);
}

static void test_lan_requires_address(void)
{
    app_scan_evidence_t ev;
    app_scan_lan_t obs;

    app_scan_evidence_reset(&ev, 1u);

    /* An mDNS result with no address cannot be keyed to a device. It must be
     * rejected and counted, not attached to an arbitrary host. */
    memset(&obs, 0, sizeof(obs));
    CHECK(!app_scan_ingest_lan(&ev, &obs), "addressless LAN evidence rejected");
    CHECK(ev.lan_count == 0u, "no device invented");
    CHECK(ev.drops.rej_malformed == 1u, "rejection counted, got %u",
          (unsigned)ev.drops.rej_malformed);
}

static void test_lan_merges_sources_by_ip(void)
{
    app_scan_evidence_t ev;
    app_scan_lan_t obs;
    app_scan_lan_t *found;

    app_scan_evidence_reset(&ev, 1u);

    memset(&obs, 0, sizeof(obs));
    (void)app_strlcpy(obs.ipv4, "192.168.1.10", sizeof(obs.ipv4));
    obs.from_mdns = true;
    obs.up = true;
    obs.service_count = 1u;
    (void)app_strlcpy(obs.hostname, "printer.local", sizeof(obs.hostname));
    (void)app_strlcpy(obs.service, "_ipp._tcp.local", sizeof(obs.service));
    obs.first_seen_ms = 5u;
    obs.last_seen_ms = 5u;
    CHECK(app_scan_ingest_lan(&ev, &obs), "mdns evidence accepted");

    /* Nmap finds the same host: merged, sources combined. */
    memset(&obs, 0, sizeof(obs));
    (void)app_strlcpy(obs.ipv4, "192.168.1.10", sizeof(obs.ipv4));
    obs.from_nmap = true;
    obs.up = true;
    obs.last_seen_ms = 40u;
    CHECK(app_scan_ingest_lan(&ev, &obs), "nmap evidence accepted");
    CHECK(ev.lan_count == 1u, "same IP must merge, got %u", (unsigned)ev.lan_count);

    found = app_scan_find_lan(&ev, "192.168.1.10");
    CHECK(found != NULL, "lan device findable");
    CHECK(found->from_mdns && found->from_nmap, "both sources recorded");
    CHECK(strcmp(found->hostname, "printer.local") == 0, "hostname preserved");
    CHECK(found->last_seen_ms == 40u, "last seen updated");
    CHECK(found->first_seen_ms == 5u, "first seen preserved");
}

static void test_reset_clears_everything(void)
{
    app_scan_evidence_t ev;
    app_scan_wifi_t obs;

    app_scan_evidence_reset(&ev, 1u);
    memset(&obs, 0, sizeof(obs));
    obs.bssid[0] = 1u;
    CHECK(app_scan_ingest_wifi(&ev, &obs), "ingest before reset");
    CHECK(ev.wifi_count == 1u, "one before reset");

    app_scan_evidence_reset(&ev, 2u);
    CHECK(ev.wifi_count == 0u, "wifi cleared");
    CHECK(ev.ble_count == 0u, "ble cleared");
    CHECK(ev.lan_count == 0u, "lan cleared");
    CHECK(ev.drops.device_evictions == 0u, "drops cleared");
    CHECK(ev.generation == 2u, "generation advanced");
}

/* ---------------- RF stage verdict (fault injection) ---------------- */

/*
 * Fault injection for session teardown.
 *
 * A stage that cannot shut its session down must not publish evidence, and a
 * stage that ended on its own deadline must not be recorded as DONE even though
 * the call returns a value a caller might read as success.
 */
static void test_rf_verdict_clean_run_is_done(void)
{
    app_scan_rf_outcome_t outcome;
    app_scan_rf_verdict_t verdict;

    memset(&outcome, 0, sizeof(outcome));
    outcome.stop_confirmed = true;
    outcome.native_error = ESP_OK;
    outcome.collected = 7u;
    verdict = app_scan_evaluate_rf_stage(&outcome);

    CHECK(verdict.terminal_state == APP_STAGE_STATE_DONE, "a clean run is DONE");
    CHECK(verdict.evidence_usable, "a clean run's evidence is usable");
}

static void test_rf_verdict_unconfirmed_stop_is_failed_and_unusable(void)
{
    app_scan_rf_outcome_t outcome;
    app_scan_rf_verdict_t verdict;

    /* The session could not be shut down: its task may still be writing. */
    memset(&outcome, 0, sizeof(outcome));
    outcome.stop_confirmed = false;
    outcome.collected = 42u; /* plenty collected, and still unusable */
    verdict = app_scan_evaluate_rf_stage(&outcome);

    CHECK(verdict.terminal_state == APP_STAGE_STATE_FAILED,
          "an unconfirmed stop is FAILED, got %s",
          app_stage_state_name(verdict.terminal_state));
    CHECK(!verdict.evidence_usable,
          "evidence from a session we could not stop must NOT be published");

    /* Cancellation does not make a still-running session safe either. */
    outcome.canceled = true;
    verdict = app_scan_evaluate_rf_stage(&outcome);
    CHECK(!verdict.evidence_usable, "cancel does not make a live session safe");
    CHECK(verdict.terminal_state == APP_STAGE_STATE_FAILED,
          "unconfirmed stop outranks cancel");
}

static void test_rf_verdict_timeout_is_never_done(void)
{
    app_scan_rf_outcome_t outcome;
    app_scan_rf_verdict_t verdict;

    /* Stopped cleanly but on its own deadline, with results collected. */
    memset(&outcome, 0, sizeof(outcome));
    outcome.stop_confirmed = true;
    outcome.timed_out = true;
    outcome.collected = 5u;
    verdict = app_scan_evaluate_rf_stage(&outcome);
    CHECK(verdict.terminal_state == APP_STAGE_STATE_PARTIAL,
          "a timed-out stage with results is PARTIAL, got %s",
          app_stage_state_name(verdict.terminal_state));
    CHECK(verdict.evidence_usable, "a cleanly stopped session's results are usable");

    /* Timed out with nothing at all: a failure, not a thin success. */
    outcome.collected = 0u;
    verdict = app_scan_evaluate_rf_stage(&outcome);
    CHECK(verdict.terminal_state == APP_STAGE_STATE_FAILED,
          "a timed-out stage with no results is FAILED");
}

static void test_rf_verdict_cancel_and_native_error(void)
{
    app_scan_rf_outcome_t outcome;
    app_scan_rf_verdict_t verdict;

    memset(&outcome, 0, sizeof(outcome));
    outcome.stop_confirmed = true;
    outcome.canceled = true;
    outcome.collected = 3u;
    verdict = app_scan_evaluate_rf_stage(&outcome);
    CHECK(verdict.terminal_state == APP_STAGE_STATE_CANCELED, "cancel is CANCELED");
    CHECK(verdict.evidence_usable, "canceled-but-stopped evidence is usable");
    CHECK(verdict.reason != NULL && strcmp(verdict.reason, "canceled") == 0,
          "cancel reason recorded");

    memset(&outcome, 0, sizeof(outcome));
    outcome.stop_confirmed = true;
    outcome.native_error = ESP_FAIL;
    outcome.collected = 2u;
    verdict = app_scan_evaluate_rf_stage(&outcome);
    CHECK(verdict.terminal_state == APP_STAGE_STATE_PARTIAL,
          "a native error with results is PARTIAL");

    outcome.collected = 0u;
    verdict = app_scan_evaluate_rf_stage(&outcome);
    CHECK(verdict.terminal_state == APP_STAGE_STATE_FAILED,
          "a native error with no results is FAILED");

    CHECK(app_scan_evaluate_rf_stage(NULL).evidence_usable == false,
          "a missing outcome is never usable");
}

/* ---------------- protocol coverage semantics ---------------- */

/*
 * Coverage is what decides whether a missing device means anything. Only DONE
 * qualifies: PARTIAL explicitly does not, because a partially covered protocol
 * may simply have missed the device.
 */
static void test_only_done_counts_as_protocol_coverage(void)
{
    CHECK(app_scan_state_covers_protocol(APP_STAGE_STATE_DONE),
          "DONE covers its protocol");
    CHECK(!app_scan_state_covers_protocol(APP_STAGE_STATE_PARTIAL),
          "PARTIAL must NOT count as coverage: it may have missed devices");
    CHECK(!app_scan_state_covers_protocol(APP_STAGE_STATE_SKIPPED),
          "SKIPPED does not cover");
    CHECK(!app_scan_state_covers_protocol(APP_STAGE_STATE_FAILED),
          "FAILED does not cover");
    CHECK(!app_scan_state_covers_protocol(APP_STAGE_STATE_CANCELED),
          "CANCELED does not cover");
    CHECK(!app_scan_state_covers_protocol(APP_STAGE_STATE_PENDING),
          "PENDING does not cover");
    CHECK(!app_scan_state_covers_protocol(APP_STAGE_STATE_RUNNING),
          "RUNNING does not cover");
}

int main(void)
{
    test_wifi_stage_requires_driver();
    test_ble_stage_requires_backend();
    test_lan_stages_require_ip();
    test_unwired_protocols_are_skipped();
    test_cancel_dominates_planning();
    test_materialize_and_enrichment_always_run();
    test_null_inputs_rejected();
    test_wifi_evidence_merges_by_bssid();
    test_wifi_capacity_evicts_oldest();
    test_ble_identity_includes_address_type();
    test_ble_later_parsed_adv_wins();
    test_ble_adv_store_is_bounded();
    test_lan_requires_address();
    test_lan_merges_sources_by_ip();
    test_reset_clears_everything();
    test_rf_verdict_clean_run_is_done();
    test_rf_verdict_unconfirmed_stop_is_failed_and_unusable();
    test_rf_verdict_timeout_is_never_done();
    test_rf_verdict_cancel_and_native_error();
    test_only_done_counts_as_protocol_coverage();

    printf("app_scan: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
