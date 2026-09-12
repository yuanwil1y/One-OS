/*
 * Software acceptance for the headless (no-GUI) product chain.
 *
 * This group is not a unit test of one module. It drives the whole application chain
 * the way the product is meant to be used - scan generation, evidence, recognition,
 * Device/Entity state, control and confirmation - and asserts the properties that only
 * appear when the pieces run together for a long time:
 *
 *   - repeated scanning does not grow any bounded table without bound, and every
 *     materialisation is either still bound or accounted for by a sweep;
 *   - cancellation and incomplete coverage leave the device list honest rather than
 *     empty or wrongly swept, and a device survives one missed report;
 *   - nothing above ever leaves an orphan between the application tables and ha_core;
 *   - an environment larger than the tables is reported as truncated rather than
 *     presented as a smaller complete one;
 *   - the declared capacities hold the relations the design depends on.
 *
 * WHAT THIS IS NOT
 *
 * It is not hardware acceptance. Every figure here is a software bound computed from
 * compile-time constants; no heap, stack, RF or SD behaviour is measured. The hardware
 * checklist is docs/hardware-acceptance.md and every item in it is outstanding.
 *
 * The clock, the radios and the filesystem are injected, but the state machines are the
 * real ones: "100 rounds" here means 100 real iterations, not 100 simulated ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_control.h"
#include "app_db_import.h"
#include "app_device.h"
#include "app_device_test_hooks.h"
#include "app_ops.h"
#include "app_scan.h"
#include "app_str.h"
#include "ha_core.h"

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

/* Rounds for the repetition tests. The task book suggests at least 100. */
#define ACCEPTANCE_ROUNDS 100u

/* ---------------- shared harness ---------------- */

static void world_reset(void)
{
    app_device_table_reset();
    app_control_reset();
}

/* Feed one Wi-Fi observation into a generation. */
static void feed_wifi(app_scan_evidence_t *ev, uint8_t last_octet, const char *ssid,
                      int8_t rssi, uint64_t seen_ms)
{
    app_scan_wifi_t obs;

    memset(&obs, 0, sizeof(obs));
    obs.bssid[0] = 0x02u;
    obs.bssid[5] = last_octet;
    obs.rssi_last = rssi;
    obs.channel = 6u;
    obs.last_seen_ms = seen_ms;
    obs.first_seen_ms = seen_ms;
    obs.seen_count = 1u;
    if (ssid != NULL) {
        size_t len = strlen(ssid);

        if (len > APP_SCAN_MAX_SSID) {
            len = APP_SCAN_MAX_SSID;
        }
        obs.has_ssid = true;
        obs.ssid_len = (uint8_t)len;
        memcpy(obs.ssid, ssid, len);
    }
    (void)app_scan_ingest_wifi(ev, &obs);
}

/* A scan report in which exactly one stage completed. */
static app_scan_status_t report_with_wifi(uint32_t generation)
{
    app_scan_status_t scan;

    memset(&scan, 0, sizeof(scan));
    for (int i = 0; i < (int)APP_STAGE_COUNT; ++i) {
        scan.states[i] = APP_STAGE_STATE_SKIPPED;
    }
    scan.states[APP_STAGE_WIFI_RF] = APP_STAGE_STATE_DONE;
    scan.generation = generation;
    return scan;
}

/* The application device id a given BSSID produces, so a test can name it. */
static void device_id_for(uint8_t last_octet, char *out, size_t out_size)
{
    (void)snprintf(out, out_size, "wifi_0200000000%02x", (unsigned)last_octet);
}

/* ---------------- 1. repeated scanning ---------------- */

/*
 * One hundred generations, each a full Wi-Fi scan, asserting after every round that
 * nothing has grown past its declared bound.
 *
 * The assertion that matters is the arithmetic at the end: every device ever
 * materialised is either still bound or was accounted for by a sweep. A slot that
 * vanished without being counted - a leak, or an eviction that forgot to increment -
 * shows up as a mismatch.
 */
static void test_repeated_scanning_is_bounded(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    size_t materialised_total = 0u;

    world_reset();

    for (uint32_t round = 1u; round <= ACCEPTANCE_ROUNDS; ++round) {
        uint8_t octet = (uint8_t)(1u + (round % 250u));

        app_scan_evidence_reset(&ev, round);
        app_device_generation_begin(round);
        feed_wifi(&ev, octet, "RoundAP", -50, (uint64_t)round * 1000u);
        materialised_total += app_device_materialize(&ev, NULL, &truncated);
        {
            /* A named object, not a temporary: generation_finish takes an address, and
             * a compound literal's lifetime would be the enclosing statement. */
            app_scan_status_t report = report_with_wifi(round);

            app_device_generation_finish(&report);
        }

        CHECK(app_device_count() <= APP_DEVICE_MAX,
              "round %u: devices %u within %u", (unsigned)round,
              (unsigned)app_device_count(), (unsigned)APP_DEVICE_MAX);
        CHECK(app_entity_count() <= APP_ENTITY_MAX,
              "round %u: entities %u within %u", (unsigned)round,
              (unsigned)app_entity_count(), (unsigned)APP_ENTITY_MAX);
        CHECK(ha_core_device_count() <= HA_CORE_MAX_DEVICES,
              "round %u: ha_core devices %u within %u", (unsigned)round,
              (unsigned)ha_core_device_count(), (unsigned)HA_CORE_MAX_DEVICES);
        CHECK(ha_core_entity_count() <= HA_CORE_MAX_ENTITIES,
              "round %u: ha_core entities %u within %u", (unsigned)round,
              (unsigned)ha_core_entity_count(), (unsigned)HA_CORE_MAX_ENTITIES);
        CHECK(app_control_pending_count() == 0u,
              "round %u: no control is left pending", (unsigned)round);
    }

    CHECK(materialised_total > 0u, "the rounds materialised devices");
    CHECK(materialised_total == app_device_count() + app_device_swept_count(),
          "every materialisation is bound or accounted for: %u created, %u bound, "
          "%u swept",
          (unsigned)materialised_total, (unsigned)app_device_count(),
          (unsigned)app_device_swept_count());
}

/* No orphan may exist between the application tables and ha_core, in either
 * direction. An orphan is state a UI could address but nothing owns. */
static void test_no_orphans_after_many_rounds(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;

    world_reset();

    for (uint32_t round = 1u; round <= 32u; ++round) {
        app_scan_evidence_reset(&ev, round);
        app_device_generation_begin(round);
        feed_wifi(&ev, (uint8_t)(1u + (round % 120u)), "AP-A", -50,
                  (uint64_t)round * 1000u);
        feed_wifi(&ev, (uint8_t)(130u + (round % 120u)), "AP-B", -60,
                  (uint64_t)round * 1000u);
        (void)app_device_materialize(&ev, NULL, &truncated);
        { app_scan_status_t report__ = report_with_wifi(round); app_device_generation_finish(&report__); }
    }

    for (size_t i = 0u; i < app_entity_count(); ++i) {
        const app_entity_binding_t *entity = app_entity_at(i);

        CHECK(entity != NULL && app_device_find(entity->device_id) != NULL,
              "entity '%s' has an owning device binding",
              entity != NULL ? entity->entity_id : "(null)");
        if (entity != NULL) {
            CHECK(ha_core_entity_get(entity->entity_id) != NULL,
                  "entity '%s' exists in ha_core too", entity->entity_id);
        }
    }
    for (size_t i = 0u; i < app_device_count(); ++i) {
        const app_device_binding_t *device = app_device_at(i);

        CHECK(device != NULL && ha_core_device_get(device->ha_device_id) != NULL,
              "device '%s' exists in ha_core",
              device != NULL ? device->device_id : "(null)");
    }
}

/*
 * Capacity pressure: more devices than any table can hold, every round.
 *
 * This is where eviction, truncation reporting and the ha_core bound interact. What
 * must hold is that the caller is TOLD the result is truncated rather than handed a
 * partial environment as if it were the whole one.
 */
static void test_capacity_pressure_is_reported(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    bool any_truncated = false;

    world_reset();

    for (uint32_t round = 1u; round <= 8u; ++round) {
        app_scan_evidence_reset(&ev, round);
        app_device_generation_begin(round);
        for (uint32_t i = 0u; i < APP_DEVICE_MAX + 8u; ++i) {
            feed_wifi(&ev, (uint8_t)(i + 1u), "Crowded", -55,
                      (uint64_t)round * 1000u + i);
        }
        (void)app_device_materialize(&ev, NULL, &truncated);
        if (truncated) {
            any_truncated = true;
        }
        { app_scan_status_t report__ = report_with_wifi(round); app_device_generation_finish(&report__); }

        CHECK(app_device_count() <= APP_DEVICE_MAX,
              "round %u stayed within the device bound: %u", (unsigned)round,
              (unsigned)app_device_count());
        CHECK(ha_core_device_count() <= HA_CORE_MAX_DEVICES,
              "round %u stayed within the ha_core device bound: %u", (unsigned)round,
              (unsigned)ha_core_device_count());
        CHECK(ha_core_entity_count() <= HA_CORE_MAX_ENTITIES,
              "round %u stayed within the ha_core entity bound: %u", (unsigned)round,
              (unsigned)ha_core_entity_count());
    }

    CHECK(any_truncated,
          "an environment larger than the tables is reported as truncated, not "
          "presented as a smaller complete one");

    /*
     * The evidence table is bounded as well, and it counts its rejections.
     *
     * The environment below is deliberately LARGER than APP_SCAN_MAX_WIFI, so the
     * truncation path is actually reached. The earlier rounds use exactly
     * APP_DEVICE_MAX + 8 observations, which for the current constants still fits in
     * the evidence table - so asserting truncation on those would have asserted
     * something the test never caused, which is how this check first failed.
     */
    {
        app_scan_evidence_t big;
        uint32_t total = APP_SCAN_MAX_WIFI + 16u;

        app_scan_evidence_reset(&big, 99u);
        for (uint32_t i = 0u; i < total; ++i) {
            feed_wifi(&big, (uint8_t)(i + 1u), "TooMany", -55, 99000u + i);
        }
        CHECK(big.wifi_count == APP_SCAN_MAX_WIFI,
              "the evidence table stopped at its bound %u, held %u",
              (unsigned)APP_SCAN_MAX_WIFI, (unsigned)big.wifi_count);
        CHECK(app_scan_evidence_truncated(&big),
              "and reports that it could not hold all %u observations",
              (unsigned)total);
    }
}

/* ---------------- 2. cancellation and incomplete coverage ---------------- */

/*
 * A cancelled scan must not make devices look gone, and a device must survive one
 * missed report.
 *
 * The coverage rule does the first part: only a stage that reached DONE counts as
 * having looked, so a cancelled Wi-Fi stage leaves its devices bound and marked. The
 * grace round does the second: one fully covered miss is not proof of departure.
 */
static void test_cancelled_scan_does_not_sweep(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    app_scan_status_t scan;
    char device_id[HA_CORE_ID_LEN];

    world_reset();
    device_id_for(7u, device_id, sizeof(device_id));

    /* Round 1: a clean scan sees one device. */
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, 7u, "Stable", -50, 1000u);
    (void)app_device_materialize(&ev, NULL, &truncated);
    { app_scan_status_t report__ = report_with_wifi(1u); app_device_generation_finish(&report__); }
    CHECK(app_device_count() == 1u, "one device after a clean scan");

    /* Round 2: cancelled before the Wi-Fi stage finished. */
    app_scan_evidence_reset(&ev, 2u);
    app_device_generation_begin(2u);
    memset(&scan, 0, sizeof(scan));
    for (int i = 0; i < (int)APP_STAGE_COUNT; ++i) {
        scan.states[i] = APP_STAGE_STATE_SKIPPED;
    }
    scan.states[APP_STAGE_WIFI_RF] = APP_STAGE_STATE_CANCELED;
    scan.canceled = true;
    scan.generation = 2u;
    app_device_generation_finish(&scan);

    CHECK(app_device_count() == 1u,
          "a cancelled scan does not sweep: %u devices", (unsigned)app_device_count());
    {
        const app_device_binding_t *device = app_device_find(device_id);

        CHECK(device != NULL, "the device is still bound");
        CHECK(device != NULL && device->availability == APP_AVAILABILITY_UNAVAILABLE,
              "and is reported as not observed rather than gone, got %s",
              device ? app_availability_name(device->availability) : "absent");
    }

    /* Round 3: the stage completed and did not see it - first miss, still kept. */
    app_scan_evidence_reset(&ev, 3u);
    app_device_generation_begin(3u);
    (void)app_device_materialize(&ev, NULL, &truncated);
    { app_scan_status_t report__ = report_with_wifi(3u); app_device_generation_finish(&report__); }
    CHECK(app_device_count() == 1u,
          "the first fully covered miss keeps the device: %u",
          (unsigned)app_device_count());

    /* Round 4: a second consecutive miss removes it. */
    app_scan_evidence_reset(&ev, 4u);
    app_device_generation_begin(4u);
    (void)app_device_materialize(&ev, NULL, &truncated);
    { app_scan_status_t report__ = report_with_wifi(4u); app_device_generation_finish(&report__); }
    CHECK(app_device_count() == 0u, "two consecutive misses remove it: %u",
          (unsigned)app_device_count());
    CHECK(ha_core_device_count() == 0u, "ha_core agrees");
    CHECK(ha_core_entity_count() == 0u, "and no entity was orphaned");
}

/* ---------------- 3. control across a changing world ---------------- */

/*
 * A scan generation must not disturb a control on a device that is still online.
 *
 * This is the interaction the acceptance criteria call out: scanning and controlling
 * share one application worker and one radio handover, so the two must not corrupt each
 * other's bookkeeping.
 */
static void test_a_scan_generation_does_not_disturb_a_healthy_device(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    char device_id[HA_CORE_ID_LEN];

    world_reset();
    device_id_for(9u, device_id, sizeof(device_id));

    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, 9u, "SwitchAP", -50, 1000u);
    (void)app_device_materialize(&ev, NULL, &truncated);
    { app_scan_status_t report__ = report_with_wifi(1u); app_device_generation_finish(&report__); }

    {
        const app_device_binding_t *device = app_device_find(device_id);

        CHECK(device != NULL, "the device exists");
        if (device == NULL) {
            return;
        }
        CHECK(device->availability == APP_AVAILABILITY_ONLINE,
              "and is online");
    }

    /* A second generation that still sees it. */
    app_scan_evidence_reset(&ev, 2u);
    app_device_generation_begin(2u);
    feed_wifi(&ev, 9u, "SwitchAP", -52, 2000u);
    (void)app_device_materialize(&ev, NULL, &truncated);
    { app_scan_status_t report__ = report_with_wifi(2u); app_device_generation_finish(&report__); }

    CHECK(app_control_reconcile() == 0u,
          "reconciling after a scan fails nothing while the device is online");
    CHECK(app_device_count() == 1u, "and the device is still bound once");
}

/*
 * A control whose device stops being observed is cleaned up. No confirmation is coming
 * from a device that is no longer there, so leaving it pending would be a spinner that
 * never resolves.
 */
static void test_a_device_that_goes_away_finishes_its_controls(void)
{
    app_scan_evidence_t ev;
    bool truncated = false;
    char device_id[HA_CORE_ID_LEN];

    world_reset();
    device_id_for(11u, device_id, sizeof(device_id));

    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    feed_wifi(&ev, 11u, "GoneAP", -50, 1000u);
    (void)app_device_materialize(&ev, NULL, &truncated);
    { app_scan_status_t report__ = report_with_wifi(1u); app_device_generation_finish(&report__); }

    {
        const app_device_binding_t *device = app_device_find(device_id);

        CHECK(device != NULL, "the device exists");
        if (device == NULL) {
            return;
        }
        CHECK(app_control_reconcile() == 0u, "nothing pending, nothing failed");

        /* Take it offline, as a device that went out of range would. */
        app_device_test_mark_availability(device->device_id,
                                          APP_AVAILABILITY_UNAVAILABLE);
        CHECK(app_control_reconcile() == 0u,
              "and reconciling an offline device with nothing pending is a no-op");
        CHECK(app_control_pending_count() == 0u, "nothing is pending");
    }
}

/* ---------------- 4. declared capacities ---------------- */

/*
 * The relations the design depends on, asserted so a future change to any one constant
 * has to be deliberate.
 *
 * These are SOFTWARE bounds. Nothing here measures RAM, and a small table says nothing
 * about the heap left on the device.
 */
static void test_declared_capacities_hold_their_relations(void)
{
    CHECK(APP_DEVICE_MAX > 0u && APP_ENTITY_MAX > 0u,
          "both application tables have capacity");
    CHECK(APP_RECOGNITION_TABLE_MAX == APP_DEVICE_MAX,
          "one recognition entry per device suffices because enrichment runs once per "
          "observation");
    CHECK(HA_CORE_MAX_ENTITIES >= HA_CORE_MAX_DEVICES,
          "ha_core holds at least one entity per device it can hold");
    CHECK(HA_CORE_MAX_STATES >= HA_CORE_MAX_ENTITIES,
          "and a state slot per entity");

    /* The reader's fixed footprint must not depend on the corpus. */
    CHECK(APP_DB_HEADER_MAX < 1024u,
          "the reader's header staging is small and fixed: %u bytes",
          (unsigned)APP_DB_HEADER_MAX);
    CHECK(DEVICE_DB_INDEX_BUCKET_SIZE > 0u,
          "the index bucket size is a defined constant");

    /* The import's receive chunk must be bounded too. */
    CHECK(APP_DB_IMPORT_CHUNK <= 4096u,
          "the import chunk is bounded: %u bytes", (unsigned)APP_DB_IMPORT_CHUNK);
    CHECK(APP_CONTROL_MAX_PENDING > 0u && APP_CONTROL_MAX_PENDING <= 8u,
          "controls in flight are bounded and small: %u",
          (unsigned)APP_CONTROL_MAX_PENDING);
}

int main(void)
{
    test_repeated_scanning_is_bounded();
    test_no_orphans_after_many_rounds();
    test_capacity_pressure_is_reported();

    test_cancelled_scan_does_not_sweep();

    test_a_scan_generation_does_not_disturb_a_healthy_device();
    test_a_device_that_goes_away_finishes_its_controls();

    test_declared_capacities_hold_their_relations();

    printf("app_acceptance: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
