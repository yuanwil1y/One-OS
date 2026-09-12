/*
 * Host regression tests for the SD-backed recognition database reader.
 *
 * The real app_device_db.c, the real recognition policy and the real device
 * table are compiled here. What is supplied by the test is the STORAGE: a small
 * in-memory vtable that can be told to fail in each of the ways a real card can.
 * That is the whole point of the vtable - no card, no compiler, no filesystem
 * needed to prove that a short read, a shrinking file, a same-length rewrite, a
 * replacement or an I/O error cannot produce a match.
 *
 * Asserted here, and nowhere else:
 *   - a hit, a miss and an ambiguity are three different outcomes;
 *   - a profile marked writable in the database still produces NO write target
 *     while this firmware cannot drive its backend;
 *   - an unknown domain id, and a profile whose selected decoder is absent, are
 *     refused rather than half-applied;
 *   - every degradation state (no card, no file, corrupt, incompatible, I/O
 *     error, index too large, oversized file) is distinct;
 *   - a corpus that changes underneath the reader stops recognition instead of
 *     answering from a mixture of two files;
 *   - a corrupt database never yields a partial match;
 *   - the enrichment stage fills a per-observation table, and materialisation
 *     applies exactly that table, so recognition cannot silently be skipped.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_device.h"
#include "app_device_db.h"
#include "app_scan.h"
#include "app_str.h"
#include "ha_core.h"
#include "theengs_l2.h"
#include "zha_l2.h"

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

/* ---------------- in-memory storage double ---------------- */

/*
 * The store models a medium, not a filesystem: `medium` is the card, `image` is
 * the database file on it. Every fault the adapter contract can report is a
 * field here, so a fault is injected without touching the reader.
 */
typedef struct {
    bool medium_present;
    bool file_present;
    uint8_t *image;
    uint32_t size;
    uint32_t capacity;

    /* Faults. */
    bool open_fails;            /* open() reports an error */
    uint32_t open_error;
    bool short_reads;           /* read() returns success but fewer bytes */
    uint32_t short_read_at;     /* only at/after this offset */
    esp_err_t read_error;       /* non-OK makes every read fail */
    uint32_t read_error_at;     /* only at/after this offset */
    bool report_larger_size;    /* open() claims a size the image does not have */

    /* Observations. */
    uint32_t opens;
    uint32_t close_calls;
    uint32_t reads;
} test_store_t;

static esp_err_t store_open(void *ctx, uint32_t *out_size)
{
    test_store_t *store = (test_store_t *)ctx;

    store->opens++;
    if (store->open_fails) {
        return (esp_err_t)store->open_error;
    }
    if (!store->file_present) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_size = store->report_larger_size ? store->size + 64u : store->size;
    return ESP_OK;
}

static esp_err_t store_read(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    test_store_t *store = (test_store_t *)ctx;

    store->reads++;
    /*
     * Injected faults come FIRST, so they model a failing medium rather than a
     * decode failure inside the test store. Judging bounds first would turn
     * "the card stopped responding" into "the file is short", which is a
     * different state and would hide the fault being tested.
     */
    if (store->read_error != ESP_OK && offset >= store->read_error_at) {
        return store->read_error;
    }
    if (store->short_reads && offset >= store->short_read_at) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (offset >= store->size || length > store->size - offset) {
        /* Past the end: the adapter contract says report the size problem, never
         * succeed with fewer bytes. */
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, store->image + offset, length);
    return ESP_OK;
}

static void store_close(void *ctx)
{
    test_store_t *store = (test_store_t *)ctx;

    store->close_calls++;
}

static bool store_medium_present(void *ctx)
{
    test_store_t *store = (test_store_t *)ctx;

    return store->medium_present;
}

static app_db_storage_ops_t store_ops(void)
{
    app_db_storage_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.open = store_open;
    ops.read = store_read;
    ops.close = store_close;
    ops.medium_present = store_medium_present;
    return ops;
}

/* ---------------- fixture loading ---------------- */

static char *join_path(const char *dir, const char *name)
{
    size_t len = strlen(dir) + strlen(name) + 2u;
    char *out = (char *)malloc(len);

    if (out == NULL) {
        return NULL;
    }
    (void)snprintf(out, len, "%s/%s", dir, name);
    return out;
}

/*
 * Where the corpus lives.
 *
 * The runner passes -DDEVICE_DB_FIXTURE_DIR (as the format group does). Because a
 * Windows path cannot be quoted portably through every shell, DEVICE_DB_FIXTURE_DIR
 * is optional here: when it is absent the group falls back to a path derived from
 * this file's own __FILE__, which is correct for both an in-tree and an
 * out-of-tree build. A missing directory fails the run outright rather than
 * quietly skipping the reader tests.
 */
#ifdef DEVICE_DB_FIXTURE_DIR
#define FIXTURE_DIR_CANDIDATE DEVICE_DB_FIXTURE_DIR
#else
#define FIXTURE_DIR_CANDIDATE ""
#endif

static char g_fixture_dir[1024];

static bool fixture_dir_is_usable(const char *dir)
{
    char *path;
    FILE *f;

    if (dir == NULL || dir[0] == '\0') {
        return false;
    }
    path = join_path(dir, "devices_fixture.nbdb");
    if (path == NULL) {
        return false;
    }
    f = fopen(path, "rb");
    free(path);
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static void resolve_fixture_dir(void)
{
    /*
     * This file is tests/host/test_app_device_db.c, so the corpus is one
     * directory up and then down into fixtures:
     *   <dir of this file>/../fixtures/device_db
     * Both several build layouts are covered by trying the relative forms rather
     * than assuming one, and a missing directory fails the run.
     */
    static const char *const suffixes[] = {
        "/../fixtures/device_db",
        "/tests/fixtures/device_db",
        "/../../tests/fixtures/device_db",
    };

    if (fixture_dir_is_usable(FIXTURE_DIR_CANDIDATE)) {
        (void)app_strlcpy(g_fixture_dir, FIXTURE_DIR_CANDIDATE,
                          sizeof(g_fixture_dir));
        return;
    }

    for (size_t s = 0u; s < sizeof(suffixes) / sizeof(suffixes[0]); ++s) {
        const char *slash = strrchr(__FILE__, '/');

        if (slash != NULL) {
            size_t dir_len = (size_t)(slash - __FILE__);
            size_t total = dir_len + strlen(suffixes[s]) + 1u;

            if (total < sizeof(g_fixture_dir)) {
                memcpy(g_fixture_dir, __FILE__, dir_len);
                (void)app_strlcpy(g_fixture_dir + dir_len, suffixes[s],
                                  sizeof(g_fixture_dir) - dir_len);
                if (fixture_dir_is_usable(g_fixture_dir)) {
                    return;
                }
            }
        }
        /* A backslash-separated __FILE__, as MSVC-style paths come through. */
        slash = strrchr(__FILE__, '\\');
        if (slash != NULL) {
            size_t dir_len = (size_t)(slash - __FILE__);
            size_t total = dir_len + strlen(suffixes[s]) + 1u;

            if (total < sizeof(g_fixture_dir)) {
                memcpy(g_fixture_dir, __FILE__, dir_len);
                (void)app_strlcpy(g_fixture_dir + dir_len, suffixes[s],
                                  sizeof(g_fixture_dir) - dir_len);
                if (fixture_dir_is_usable(g_fixture_dir)) {
                    return;
                }
            }
        }
    }
    g_fixture_dir[0] = '\0';
}

static bool load_file(const char *dir, const char *name, uint8_t **out,
                      uint32_t *out_size)
{
    char *path = join_path(dir, name);
    FILE *f;
    long size;
    uint8_t *buffer;

    if (path == NULL) {
        return false;
    }
    f = fopen(path, "rb");
    free(path);
    if (f == NULL) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    buffer = (uint8_t *)malloc((size_t)size);
    if (buffer == NULL) {
        fclose(f);
        return false;
    }
    if (fread(buffer, 1u, (size_t)size, f) != (size_t)size) {
        free(buffer);
        fclose(f);
        return false;
    }
    fclose(f);
    *out = buffer;
    *out_size = (uint32_t)size;
    return true;
}

static test_store_t g_store;
static uint8_t *g_fixture;
static uint32_t g_fixture_size;
static uint8_t g_index[APP_DB_MAX_INDEX_BYTES];
static app_device_db_t g_db;

/* Point the store at a fresh copy of the fixture. */
static void store_use_fixture(void)
{
    memset(&g_store, 0, sizeof(g_store));
    g_store.medium_present = true;
    g_store.file_present = true;
    g_store.read_error = ESP_OK;
    /* Headroom beyond the file, because a test may grow or rewrite the image to
     * model a corpus being replaced while the reader holds it. */
    g_store.capacity = g_fixture_size + 65536u;
    g_store.image = (uint8_t *)calloc(1u, g_store.capacity);
    if (g_store.image == NULL) {
        printf("FATAL: out of memory\n");
        exit(2);
    }
    memcpy(g_store.image, g_fixture, g_fixture_size);
    g_store.size = g_fixture_size;
}

static void store_release(void)
{
    free(g_store.image);
    g_store.image = NULL;
    g_store.size = 0u;
    g_store.capacity = 0u;
}

static void db_open_with_store(app_db_storage_ops_t ops)
{
    app_device_db_open(&g_db, &ops, &g_store, g_index, (uint32_t)sizeof(g_index));
}

static void db_open_fixture(void)
{
    app_db_storage_ops_t ops = store_ops();

    db_open_with_store(ops);
}

/* ---------------- observation builders ---------------- */

static app_scan_wifi_t make_wifi(const char *ssid)
{
    app_scan_wifi_t obs;

    memset(&obs, 0, sizeof(obs));
    obs.bssid[0] = 0x02u;
    obs.bssid[5] = 0x11u;
    obs.rssi_last = -40;
    obs.channel = 1u;
    obs.last_seen_ms = 1000u;
    if (ssid != NULL) {
        size_t len = strlen(ssid);

        if (len > APP_SCAN_MAX_SSID) {
            len = APP_SCAN_MAX_SSID;
        }
        obs.has_ssid = true;
        obs.ssid_len = (uint8_t)len;
        memcpy(obs.ssid, ssid, len);
    }
    return obs;
}

static app_scan_ble_t make_ble(uint16_t uuid16, uint16_t company_id,
                               uint8_t service_data_count,
                               uint8_t manufacturer_count)
{
    app_scan_ble_t obs;

    memset(&obs, 0, sizeof(obs));
    obs.address[0] = 0xC4u;
    obs.address[5] = 0x99u;
    obs.address_type = 1u;
    obs.rssi_last = -60;
    obs.last_seen_ms = 1000u;
    obs.has_parsed_adv = true;
    obs.adv.valid = true;
    if (uuid16 != 0u) {
        obs.adv.uuid16_count = 1u;
        obs.adv.uuid16[0] = uuid16;
    }
    obs.adv.service_data_count = service_data_count;
    obs.adv.manufacturer_data_count = manufacturer_count;
    obs.adv.first_company_id = company_id;
    return obs;
}

static app_scan_lan_t make_lan(const char *service)
{
    app_scan_lan_t obs;

    memset(&obs, 0, sizeof(obs));
    (void)app_strlcpy(obs.ipv4, "192.168.1.50", sizeof(obs.ipv4));
    (void)app_strlcpy(obs.hostname, "printer", sizeof(obs.hostname));
    obs.from_mdns = true;
    obs.up = true;
    obs.last_seen_ms = 1000u;
    if (service != NULL) {
        (void)app_strlcpy(obs.service, service, sizeof(obs.service));
    }
    return obs;
}

static bool match_ble(uint16_t uuid16, app_recognition_result_t *out)
{
    app_scan_ble_t obs = make_ble(uuid16, 0u, 0u, 0u);

    return app_device_db_match(&g_db, APP_SOURCE_BLE, DEVICE_DB_PROTO_BLE, NULL, 0u,
                               NULL, &obs, NULL, out);
}

/* ---------------- open and state ---------------- */

static void test_open_fixture_is_ready(void)
{
    store_use_fixture();
    db_open_fixture();

    CHECK(g_db.state == APP_DB_STATE_READY, "expected ready, got %s",
          app_db_state_name(g_db.state));
    CHECK(g_db.open, "handle reports open");
    CHECK(app_db_state_is_usable(g_db.state), "ready is usable");
    CHECK(g_db.profile_count == 6u, "fixture has 6 profiles, got %lu",
          (unsigned long)g_db.profile_count);
    CHECK(g_db.content_version == 20260911u, "fixture content version, got %lu",
          (unsigned long)g_db.content_version);
    CHECK(g_db.file_size == g_fixture_size, "recorded size matches the file");
    CHECK(g_db.read_errors == 0u, "no read errors, got %lu",
          (unsigned long)g_db.read_errors);
    /* The corpus is never resident: the reader holds only the header and the
     * index, and its struct size must not track the corpus size. */
    CHECK(sizeof(g_db) < 4096u, "handle stays small: %lu bytes",
          (unsigned long)sizeof(g_db));

    app_device_db_close(&g_db);
    CHECK(g_db.state == APP_DB_STATE_CLOSED, "closed state after close");
    CHECK(g_store.close_calls == 1u, "storage closed exactly once, got %lu",
          (unsigned long)g_store.close_calls);
    store_release();
}

static void test_no_card_and_no_file_are_distinct(void)
{
    store_use_fixture();
    g_store.medium_present = false;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_SD_MISSING, "no card reported as sd_missing, got %s",
          app_db_state_name(g_db.state));
    CHECK(!app_db_state_is_usable(g_db.state), "sd_missing is not usable");
    CHECK(g_store.opens == 0u, "the file is not even opened without a card");
    store_release();

    store_use_fixture();
    g_store.file_present = false;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_FILE_MISSING,
          "missing file reported as file_missing, got %s",
          app_db_state_name(g_db.state));
    CHECK(g_store.opens == 1u, "the file open was attempted");
    store_release();
}

static void test_corrupt_and_incompatible_are_distinct(void)
{
    /* Corrupt: body checksum no longer matches after a one-byte payload edit. */
    store_use_fixture();
    g_store.image[g_fixture_size - 1u] ^= 0xFFu;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_CORRUPT, "body edit reported as corrupt, got %s",
          app_db_state_name(g_db.state));
    store_release();

    /* Corrupt: magic destroyed. */
    store_use_fixture();
    g_store.image[0] = 'X';
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_CORRUPT, "bad magic reported as corrupt, got %s",
          app_db_state_name(g_db.state));
    store_release();

    /* Incompatible: a format version this reader does not implement. */
    store_use_fixture();
    g_store.image[4] = 0x7Fu;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_INCOMPATIBLE,
          "unknown format version reported as incompatible, got %s",
          app_db_state_name(g_db.state));
    store_release();

    /* Incompatible: a reader ABI from the future. */
    store_use_fixture();
    g_store.image[8] = 0x02u;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_INCOMPATIBLE,
          "future reader ABI reported as incompatible, got %s",
          app_db_state_name(g_db.state));
    store_release();
}

static void test_oversized_and_truncated(void)
{
    /* Declared size above the hard cap: incompatible, not corrupt, and without
     * any attempt to allocate. */
    store_use_fixture();
    g_store.size = DEVICE_DB_MAX_FILE_BYTES + 1u;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_INCOMPATIBLE,
          "file above the cap reported as incompatible, got %s",
          app_db_state_name(g_db.state));
    store_release();

    /* Shorter than a header. */
    store_use_fixture();
    g_store.size = 16u;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_CORRUPT, "under-header file is corrupt, got %s",
          app_db_state_name(g_db.state));
    store_release();
}

static void test_io_error_at_open(void)
{
    store_use_fixture();
    g_store.read_error = ESP_ERR_TIMEOUT;
    g_store.read_error_at = 0u;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_IO_ERROR, "I/O error reported as io_error, got %s",
          app_db_state_name(g_db.state));
    CHECK(g_db.last_error == ESP_ERR_TIMEOUT, "the error is preserved");
    CHECK(!app_db_state_is_usable(g_db.state), "io_error is not usable");
    store_release();
}

static void test_short_read_during_open_is_detected(void)
{
    store_use_fixture();
    /* Header reads, then every later read reports size trouble: the streaming
     * checksum pass must not accept a partial file. */
    g_store.short_reads = true;
    g_store.short_read_at = DEVICE_DB_HEADER_SIZE;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_CORRUPT,
          "short read during validation is corrupt, got %s",
          app_db_state_name(g_db.state));
    CHECK(g_db.read_errors > 0u, "the short read was counted");
    store_release();
}

static void test_open_reports_a_larger_file_than_exists(void)
{
    store_use_fixture();
    g_store.report_larger_size = true;
    db_open_fixture();
    /* The declared size is used for bounds, so reading the header succeeds, but
     * the body checksum cannot be satisfied and the header's own length field
     * disagrees. Either way the file must not become READY. */
    CHECK(g_db.state == APP_DB_STATE_CORRUPT || g_db.state == APP_DB_STATE_IO_ERROR,
          "a length that lies about the medium is refused, got %s",
          app_db_state_name(g_db.state));
    CHECK(g_db.state != APP_DB_STATE_READY, "never ready on a lying length");
    store_release();
}

static void test_index_too_large_for_the_ram_budget(void)
{
    app_db_storage_ops_t empty = store_ops();
    app_db_storage_ops_t ops;
    uint8_t tiny[8];

    store_use_fixture();

    /* Storage that cannot read at all is refused rather than half-used. */
    empty.read = NULL;
    app_device_db_open(&g_db, &empty, &g_store, g_index, (uint32_t)sizeof(g_index));
    CHECK(g_db.state == APP_DB_STATE_CLOSED, "no reader is refused");

    /* A tiny index buffer is what a corpus with too many buckets looks like. */
    ops = store_ops();
    app_device_db_open(&g_db, &ops, &g_store, tiny, (uint32_t)sizeof(tiny));
    CHECK(g_db.state == APP_DB_STATE_CORRUPT,
          "an index that does not fit is corrupt, got %s",
          app_db_state_name(g_db.state));
    CHECK(g_db.last_error == ESP_ERR_NO_MEM, "reported as a memory refusal");

    /* A NULL index buffer with a non-empty index is the same refusal. */
    ops = store_ops();
    app_device_db_open(&g_db, &ops, &g_store, NULL, 0u);
    CHECK(g_db.state == APP_DB_STATE_CORRUPT,
          "a NULL index buffer is refused, got %s",
          app_db_state_name(g_db.state));

    store_release();
}

static int g_progress_calls;

static bool progress_stop(void *ctx)
{
    (void)ctx;
    g_progress_calls++;
    return true;
}

static bool progress_continue(void *ctx)
{
    (void)ctx;
    g_progress_calls++;
    return false;
}

static void test_cancellation_during_validation(void)
{
    /* A progress hook that asks to stop must prevent the database from ever
     * being published. */
    store_use_fixture();
    g_progress_calls = 0;
    app_device_db_set_progress(&g_db, progress_stop, NULL, 64u);
    db_open_fixture();
    CHECK(g_progress_calls > 0, "the progress hook was polled");
    CHECK(g_db.state == APP_DB_STATE_IO_ERROR,
          "a cancelled validation does not publish the database, got %s",
          app_db_state_name(g_db.state));
    CHECK(!g_db.open, "handle is not open after cancellation");
    CHECK(g_db.progress_calls > 0u, "the poll count is recorded in diagnostics");
    store_release();

    /* A hook that never asks to stop must not change the outcome, so the
     * cancellation path cannot mask a real corruption. */
    store_use_fixture();
    g_progress_calls = 0;
    app_device_db_set_progress(&g_db, progress_continue, NULL, 64u);
    db_open_fixture();
    CHECK(g_progress_calls > 0, "the hook was polled on the success path too");
    CHECK(g_db.state == APP_DB_STATE_READY,
          "a hook that continues leaves the database ready, got %s",
          app_db_state_name(g_db.state));
    app_device_db_close(&g_db);
    store_release();
}

/* ---------------- matching ---------------- */

static void test_match_ambiguous(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();
    if (g_db.state != APP_DB_STATE_READY) {
        CHECK(false, "fixture must open: %s", app_db_state_name(g_db.state));
        store_release();
        return;
    }

    /* Profiles 1001 and 1003 both fingerprint BLE key 181c, so a bare service
     * UUID is ambiguous. */
    memset(&result, 0, sizeof(result));
    CHECK(match_ble(0x181Cu, &result), "match attempt ran");
    CHECK(result.ambiguous, "two profiles on one key is ambiguous");
    CHECK(!result.matched, "an ambiguous result is not a match");
    CHECK(result.recipe_count == 0u, "ambiguity carries no recipes, got %u",
          (unsigned)result.recipe_count);
    CHECK(result.theengs_decoder_id == DEVICE_DB_NO_INDEX,
          "ambiguity selects no decoder");
    app_device_db_close(&g_db);
    store_release();
}

static void test_match_unknown(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();

    /* A UUID no profile mentions: NOT_FOUND, which is a normal outcome. */
    memset(&result, 0, sizeof(result));
    CHECK(match_ble(0x1234u, &result), "match attempt ran");
    CHECK(!result.matched, "unknown key is not a match");
    CHECK(!result.ambiguous, "unknown key is not ambiguous");
    CHECK(result.recipe_count == 0u, "unknown key yields no recipes");

    /* A Wi-Fi observation whose SSID is not in the corpus. */
    {
        app_scan_wifi_t wifi = make_wifi("not-in-the-database");

        memset(&result, 0, sizeof(result));
        CHECK(app_device_db_match(&g_db, APP_SOURCE_WIFI, DEVICE_DB_PROTO_WIFI,
                                  NULL, 0u, &wifi, NULL, NULL, &result),
              "wifi match attempt ran");
        CHECK(!result.matched, "unknown SSID is not a match");
    }

    /* LAN evidence whose service type is not in the corpus. */
    {
        app_scan_lan_t lan = make_lan("_unknown._tcp.local");

        memset(&result, 0, sizeof(result));
        CHECK(app_device_db_match(&g_db, APP_SOURCE_LAN, DEVICE_DB_PROTO_MDNS, NULL,
                                  0u, NULL, NULL, &lan, &result),
              "lan match attempt ran");
        CHECK(!result.matched, "unknown service is not a match");
    }

    /*
     * An ESPHome node, matched on its mDNS INSTANCE name.
     *
     * An ESPHome node is only ever seen as a LAN device, and the name that identifies
     * one is the mDNS instance: the service type `_esphome._tcp` is identical for every
     * node in range, so keying on it would resolve one profile for all of them. Profile
     * 1006 in the fixture is keyed on `example-node-1` and carries an `ESPHOME_API`
     * recipe, which is also the only writable ESPHome recipe in the corpus.
     */
    {
        app_scan_lan_t lan = make_lan("_esphome._tcp.local");

        (void)app_strlcpy(lan.instance, "example-node-1", sizeof(lan.instance));


        memset(&result, 0, sizeof(result));
        CHECK(app_device_db_match(&g_db, APP_SOURCE_LAN, DEVICE_DB_PROTO_ESPHOME, NULL,
                                  0u, NULL, NULL, &lan, &result),
              "the ESPHome match attempt ran");
        CHECK(result.matched, "an ESPHome node was not matched on its instance name");
        CHECK(result.profile_id == 1006u, "matched profile %u, expected 1006",
              (unsigned)result.profile_id);

        /* And the recipe really carries a write target, so the device is controllable
         * once the runtime registers the backend. */
        {
            bool found_write = false;

            for (uint8_t i = 0u; i < result.recipe_count; ++i) {
                if (result.recipes[i].backend == DEVICE_DB_BACKEND_ESPHOME_API &&
                    result.recipes[i].write_target_id != DEVICE_DB_NO_INDEX) {
                    found_write = true;
                }
            }
            CHECK(found_write, "the ESPHome profile has no writable recipe");
        }

        /* A DIFFERENT instance name is a different node: the match must not be a
         * blanket "any ESPHome service type" hit. */
        (void)app_strlcpy(lan.instance, "some-other-node", sizeof(lan.instance));
        memset(&result, 0, sizeof(result));
        CHECK(app_device_db_match(&g_db, APP_SOURCE_LAN, DEVICE_DB_PROTO_ESPHOME, NULL,
                                  0u, NULL, NULL, &lan, &result),
              "the second ESPHome match attempt ran");
        CHECK(!result.matched,
              "a different node name matched the same profile as example-node-1");
    }

    /*
     * An ESPHome sighting with no mDNS instance name.
     *
     * An ESPHome node is only ever seen as a LAN device, and the name that identifies
     * one is the mDNS INSTANCE - the service type `_esphome._tcp` is the same for every
     * node in range. So a sighting from SSDP or Nmap, which carries no instance, must
     * yield nothing matchable and leave the device generic. Before the ESPHome key case
     * existed this was also the behaviour, for the wrong reason: nothing matched at all.
     */
    {
        app_scan_lan_t lan = make_lan("_esphome._tcp.local");

        memset(&result, 0, sizeof(result));
        CHECK(app_device_db_match(&g_db, APP_SOURCE_LAN, DEVICE_DB_PROTO_ESPHOME, NULL,
                                  0u, NULL, NULL, &lan, &result),
              "an espHome match attempt ran");
        CHECK(!result.matched,
              "a sighting with no instance name must not match an ESPHome profile");
        /* And the service TYPE alone is never the key, whatever it is. */
        CHECK(strcmp(lan.service, "_esphome._tcp.local") == 0,
              "the fixture carries the service type: %s", lan.service);
    }

    /* A BLE observation with nothing matchable at all: also not an error. */
    {
        app_scan_ble_t bare = make_ble(0u, 0u, 0u, 0u);

        memset(&result, 0, sizeof(result));
        CHECK(app_device_db_match(&g_db, APP_SOURCE_BLE, DEVICE_DB_PROTO_BLE, NULL,
                                  0u, NULL, &bare, NULL, &result),
              "an unkeyable observation is not an error");
        CHECK(!result.matched, "an unkeyable observation does not match");
    }

    app_device_db_close(&g_db);
    store_release();
}

static void test_match_exact_key_does_not_merge_prefixed_profiles(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();

    /* Profile 1004's key is "181cff": a longer key that starts with 1001's. An
     * exact match on 181c must not select it, and 181cff must not select 1001. */
    {
        app_scan_ble_t obs = make_ble(0u, 0u, 0u, 0u);

        obs.adv.uuid16_count = 0u;
        obs.adv.service_data_count = 1u;
        memset(&result, 0, sizeof(result));
        CHECK(app_device_db_match(&g_db, APP_SOURCE_BLE, DEVICE_DB_PROTO_BLE, NULL,
                                  0u, NULL, &obs, NULL, &result),
              "service-data match ran");
        /* The reader's BLE key for service data is the AD type and UUID length;
         * no profile fingerprints that, so this is NOT_FOUND rather than a
         * prefix match on anything. */
        CHECK(!result.matched, "service-data key does not prefix-match 181c");
    }

    app_device_db_close(&g_db);
    store_release();
}

static void test_control_is_refused_while_the_backend_cannot_be_driven(void)
{
    app_recognition_result_t result;
    app_scan_lan_t lan = make_lan("_ipp._tcp.local");

    store_use_fixture();
    db_open_fixture();

    memset(&result, 0, sizeof(result));
    CHECK(app_device_db_match(&g_db, APP_SOURCE_LAN, DEVICE_DB_PROTO_MDNS, NULL, 0u,
                              NULL, NULL, &lan, &result),
          "mdns match ran");
    CHECK(result.matched, "the printer profile matches its service type");
    CHECK(result.profile_id == 1005u, "matched profile 1005, got %lu",
          (unsigned long)result.profile_id);
    CHECK(strcmp(result.display_name, "Example Network Printer") == 0,
          "display name from the database, got '%s'", result.display_name);
    CHECK(result.recipe_count == 1u, "one recipe, got %u",
          (unsigned)result.recipe_count);
    if (result.recipe_count == 1u) {
        CHECK(strcmp(result.recipes[0].domain, "sensor") == 0,
              "domain resolved from the compiled domain table, got '%s'",
              result.recipes[0].domain);
        CHECK(result.recipes[0].read_source_id == APP_DB_NO_READ_SOURCE,
              "a presentation recipe has no read source");
    }
    CHECK(!result.backend_supported, "no drivable control backend is claimed");
    for (uint8_t i = 0u; i < result.recipe_count; ++i) {
        CHECK(result.recipes[i].write_target_id == DEVICE_DB_NO_INDEX,
              "recipe %u carries no write target", (unsigned)i);
    }

    app_device_db_close(&g_db);
    store_release();
}

static void test_drivability_gates_a_write_target_per_backend(void)
{
    /*
     * Profile 1002 is `writable: true` with zigbee_command/zigbee_attribute
     * recipes and a valid ZHA quirk. It is keyed on the Zigbee protocol, which
     * this reader does not key from RF evidence yet, so the profile is exercised
     * through the policy instead: app_backend_is_drivable() is the single switch
     * that decides whether the write target may survive, and it must be false
     * for every control backend this firmware does not implement.
     *
     * ble_gatt is no longer in that set: B7 landed a controller for it
     * (app_ctl_ble + app_ctl_ble_gatt), so a BLE recipe's write target now
     * survives - which is the point of the switch. test_app_device.c pins the
     * whole table in both directions.
     */
    CHECK(!app_backend_is_drivable(DEVICE_DB_BACKEND_ZIGBEE_COMMAND),
          "zigbee_command must not be drivable yet");
    CHECK(!app_backend_is_drivable(DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE),
          "zigbee_attribute must not be drivable yet");
    CHECK(!app_backend_is_drivable(DEVICE_DB_BACKEND_MATTER_COMMAND),
          "matter_command must not be drivable yet");
    CHECK(app_backend_is_drivable(DEVICE_DB_BACKEND_BLE_GATT),
          "ble_gatt is drivable now that app_ctl_ble exists");
    CHECK(app_backend_is_drivable(DEVICE_DB_BACKEND_ESPHOME_API),
          "esphome_api is drivable now that app_ctl_esphome exists");
    CHECK(app_backend_is_drivable(DEVICE_DB_BACKEND_PASSIVE_VALUE),
          "a passive read is drivable");
    CHECK(app_backend_is_drivable(DEVICE_DB_BACKEND_NONE), "no backend is drivable");
}

static void test_decoder_and_quirk_selection_is_reported(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();

    /* The BLE key 181c belongs to profiles 1001/1003 and is ambiguous, so the
     * decoder selection is checked through the profile lookup the reader exposes
     * via a direct match on the exact longer key of 1004 instead. */
    memset(&result, 0, sizeof(result));
    CHECK(match_ble(0x181Cu, &result), "match ran");
    CHECK(result.ambiguous, "181c is ambiguous, as the corpus intends");
    CHECK(result.theengs_decoder_id == DEVICE_DB_NO_INDEX &&
              result.zha_quirk_id == DEVICE_DB_NO_INDEX,
          "an ambiguous result selects neither decoder nor quirk");

    app_device_db_close(&g_db);
    store_release();
}

static void test_decoder_availability_is_real(void)
{
    /* The policy asks the family, not a copied list. BTHome v2 and Ruuvi RAWv2
     * are carried; the fixture's own ids must be among the carried set, or the
     * corpus could never be applied. */
    CHECK(app_decoder_is_available(THEENGS_DECODER_BTHOME_V2),
          "BTHome v2 decoder is carried");
    CHECK(app_decoder_is_available(THEENGS_DECODER_RUUVI_RAW_V2),
          "Ruuvi RAWv2 decoder is carried");
    CHECK(!app_decoder_is_available(0x7FFFu), "an unknown decoder is refused");
    CHECK(!app_decoder_is_available(DEVICE_DB_NO_INDEX), "no decoder is not a decoder");

    CHECK(app_quirk_is_available(ZHA_QUIRK_ID_IKEA_VALLHORN), "ikea quirk is carried");
    CHECK(!app_quirk_is_available(0xDEADBEEFu), "an unknown quirk is refused");
    CHECK(!app_quirk_is_available(DEVICE_DB_NO_INDEX), "no quirk is not a quirk");

    CHECK(app_domain_is_known(DEVICE_DB_DOMAIN_SENSOR), "sensor domain is known");
    CHECK(app_domain_is_known(DEVICE_DB_DOMAIN_SWITCH), "switch domain is known");
    CHECK(!app_domain_is_known(200u), "an unknown domain id is refused");
    CHECK(strcmp(app_domain_name(DEVICE_DB_DOMAIN_BINARY_SENSOR), "binary_sensor") == 0,
          "domain names are the HA domain strings");
}

/* ---------------- the corpus changing underneath the reader ---------------- */

static void test_same_length_rewrite_is_detected(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_READY, "fixture open before the rewrite");

    /*
     * Rewrite one byte IN THE HEADER, in place, so the length is unchanged and
     * every bounds check still passes. This is what a different corpus written
     * over the same path looks like: the reader would otherwise answer from a
     * mixture of the validated image and the new one. Only comparing the bytes
     * against what was read at open can catch it.
     */
    g_store.image[100] ^= 0x01u;

    memset(&result, 0, sizeof(result));
    CHECK(!match_ble(0x181Cu, &result),
          "recognition refuses to run against a rewritten corpus");
    CHECK(g_db.state == APP_DB_STATE_IO_ERROR,
          "the change is reported as io_error, got %s",
          app_db_state_name(g_db.state));
    CHECK(!g_db.open, "the changed corpus is closed");
    CHECK(g_store.close_calls == 1u, "storage released once, got %lu",
          (unsigned long)g_store.close_calls);
    CHECK(!result.matched && !result.ambiguous,
          "a changed corpus never produces a partial match result");
    store_release();
}

static void test_payload_only_edits_are_caught_by_full_revalidation(void)
{
    app_recognition_result_t result;

    /*
     * The two checks catch different things, and the split is deliberate:
     *
     *   - the header probe before every match is cheap and catches a file that was
     *     replaced or rewritten, because any change to a corpus's bytes leaves the
     *     header's own checksum fields describing the old content;
     *   - the streamed body checksum catches an edit that leaves the header
     *     untouched, and it costs a full read of the corpus, so it is not run
     *     before every match on a card shared with the display.
     *
     * This test pins the second one: after an in-place edit far from the header,
     * recognition stops only once the corpus is revalidated, and the state says
     * the medium changed rather than that nothing matched.
     */
    store_use_fixture();
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_READY, "open before the edit");

    /* A byte in the index region: not part of the header, so the cheap probe is
     * blind to it by construction. */
    g_store.image[g_store.size - 1u] ^= 0x01u;

    memset(&result, 0, sizeof(result));
    CHECK(match_ble(0x181Cu, &result),
          "the cheap probe alone does not catch a payload-only edit");
    CHECK(g_db.state == APP_DB_STATE_READY, "and the handle is still ready");

    /* Full revalidation is the explicit operation for this case. */
    CHECK(!app_device_db_verify_unchanged(&g_db),
          "full revalidation rejects a corpus whose payload changed");
    CHECK(g_db.state == APP_DB_STATE_IO_ERROR, "reported as io_error, got %s",
          app_db_state_name(g_db.state));
    CHECK(!g_db.open, "and the handle is closed");
    CHECK(g_store.close_calls == 1u, "storage released once, got %lu",
          (unsigned long)g_store.close_calls);
    store_release();
}

static void test_header_rewrite_with_valid_crc_is_detected(void)
{
    app_recognition_result_t result;

    /*
     * The strongest case: a different but perfectly valid corpus is written over
     * the file. Every internal checksum agrees with itself, so only comparing the
     * bytes against what was read at open can tell that the validated image is
     * gone. Profile count is a header field the reader recorded, so changing it
     * is exactly the kind of edit the shadow copy exists for.
     */
    store_use_fixture();
    db_open_fixture();

    g_store.image[32] = 0x09u; /* profile_count low byte */

    memset(&result, 0, sizeof(result));
    CHECK(!match_ble(0x181Cu, &result), "recognition refuses the replaced corpus");
    CHECK(g_db.state == APP_DB_STATE_IO_ERROR, "reported as io_error, got %s",
          app_db_state_name(g_db.state));
    store_release();
}

static void test_replaced_file_with_identical_bytes_is_a_new_medium_state(void)
{
    app_recognition_result_t result;

    /*
     * Replacing the path with a byte-identical copy is indistinguishable from no
     * change by content, and that is correct: recognition may continue, because
     * the corpus really is the one that was validated. What must NOT happen is a
     * match answered out of a mixture of two files.
     */
    store_use_fixture();
    db_open_fixture();

    memcpy(g_store.image, g_fixture, g_fixture_size);

    memset(&result, 0, sizeof(result));
    CHECK(match_ble(0x181Cu, &result), "an identical replacement keeps recognition");
    CHECK(g_db.state == APP_DB_STATE_READY, "handle stays ready");
    CHECK(result.ambiguous, "and the answer is the same as before");
    store_release();
}

static void test_growing_file_is_detected(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();

    /*
     * The corpus grew: extra bytes were appended, so the header's own length
     * field no longer describes the medium and the size recorded at open is stale.
     * The bytes are really written, not just declared, so this models a corpus
     * replaced by a larger one - and the append starts after the header, so what
     * is being tested is the size disagreement rather than a header edit.
     */
    memset(g_store.image + g_fixture_size, 0xAB, 64u);
    g_store.size = g_fixture_size + 64u;

    memset(&result, 0, sizeof(result));
    CHECK(match_ble(0x181Cu, &result),
          "a grown corpus is still answered while the header is intact");
    CHECK(g_db.state == APP_DB_STATE_READY,
          "the cheap probe cannot see growth alone, got %s",
          app_db_state_name(g_db.state));

    /*
     * The header's own length field is the format's own statement about the size,
     * so it is the check that has to catch this. Full revalidation streams the
     * body and therefore hits the appended region.
     */
    CHECK(!app_device_db_verify_unchanged(&g_db),
          "full revalidation rejects a corpus that grew");
    CHECK(g_db.state == APP_DB_STATE_IO_ERROR || g_db.state == APP_DB_STATE_CORRUPT,
          "reported as a medium change, got %s", app_db_state_name(g_db.state));
    CHECK(!g_db.open, "and the handle is closed");
    store_release();
}

static void test_io_error_during_match_is_detected(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_READY, "open succeeded");

    /* The card starts failing after a successful open, as a pulled card does. */
    g_store.read_error = ESP_ERR_TIMEOUT;
    g_store.read_error_at = 0u;

    memset(&result, 0, sizeof(result));
    CHECK(!match_ble(0x181Cu, &result), "I/O failure stops recognition");
    CHECK(g_db.state == APP_DB_STATE_IO_ERROR, "reported as io_error, got %s",
          app_db_state_name(g_db.state));
    CHECK(g_db.read_errors > 0u, "the failure was counted");
    store_release();
}

static void test_closed_database_cannot_match(void)
{
    app_recognition_result_t result;

    store_use_fixture();
    db_open_fixture();
    app_device_db_close(&g_db);

    memset(&result, 0, sizeof(result));
    CHECK(!app_device_db_match(&g_db, APP_SOURCE_BLE, DEVICE_DB_PROTO_BLE, NULL, 0u,
                               NULL, NULL, NULL, &result),
          "a closed database reports recognition unavailable");
    CHECK(!result.matched && !result.ambiguous, "and produces no result");

    /* The recognizer vtable over the same handle behaves the same way. */
    {
        const app_recognizer_ops_t *ops = app_device_db_recognizer_ops();
        char describe[64];

        CHECK(ops != NULL && ops->recognize != NULL, "vtable is available");
        CHECK(ops->state(&g_db) == APP_DB_STATE_CLOSED, "vtable reports closed");
        ops->describe(&g_db, describe, sizeof(describe));
        CHECK(strcmp(describe, "closed") == 0, "describe says closed, got '%s'",
              describe);
    }
    store_release();
}

/* ---------------- enrichment and materialisation ---------------- */

static void test_enrichment_fills_the_table_and_materialisation_applies_it(void)
{
    app_scan_evidence_t ev;
    app_recognition_table_t table;
    app_recognizer_ref_t ref;
    app_scan_lan_t lan = make_lan("_ipp._tcp.local");
    app_scan_wifi_t wifi = make_wifi("not-in-the-database");
    const app_recognition_entry_t *entry;
    char identity[HA_CORE_ID_LEN];
    bool truncated = false;
    size_t recorded;

    store_use_fixture();
    db_open_fixture();

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    (void)app_scan_ingest_wifi(&ev, &wifi);
    (void)app_scan_ingest_lan(&ev, &lan);

    ref.ops = app_device_db_recognizer_ops();
    ref.ctx = &g_db;

    recorded = app_recognition_enrich(&ev, &ref, &table);
    CHECK(recorded == 2u, "two observations recognised, got %lu",
          (unsigned long)recorded);
    CHECK(app_recognition_table_count(&table) == 2u, "two table entries");
    CHECK(!table.truncated, "table did not overflow");

    (void)app_device_identity_of_lan(&lan, identity, sizeof(identity));
    entry = app_recognition_table_find(&table, identity);
    CHECK(entry != NULL, "the LAN observation has an entry");
    if (entry != NULL) {
        CHECK(entry->attempted, "recognition was attempted for it");
        CHECK(entry->matched, "the printer profile matched");
        CHECK(entry->profile_id == 1005u, "profile 1005, got %lu",
              (unsigned long)entry->profile_id);
    }

    (void)app_device_identity_of_wifi(&wifi, identity, sizeof(identity));
    entry = app_recognition_table_find(&table, identity);
    CHECK(entry != NULL, "the Wi-Fi observation has an entry");
    if (entry != NULL) {
        CHECK(entry->attempted, "recognition was attempted for the AP too");
        CHECK(!entry->matched && !entry->ambiguous,
              "an unknown AP is NOT_FOUND");
    }

    /* Materialisation consumes the table, not the database. */
    {
        app_recognizer_ref_t table_ref = app_recognition_table_recognizer(&table);
        const app_device_binding_t *binding;
        size_t materialized;

        materialized = app_device_materialize(&ev, &table_ref, &truncated);
        CHECK(materialized == 2u, "two devices materialised, got %lu",
              (unsigned long)materialized);
        CHECK(app_device_count() == 2u, "two bindings, got %lu",
              (unsigned long)app_device_count());

        (void)app_device_identity_of_lan(&lan, identity, sizeof(identity));
        binding = app_device_find(identity);
        CHECK(binding != NULL, "the LAN device exists by its identity key");
        if (binding != NULL) {
            CHECK(binding->recognition == APP_RECOGNITION_MATCHED,
                  "recognition state is matched, got %s",
                  app_recognition_name(binding->recognition));
            CHECK(binding->read_only,
                  "a matched but undrivable profile stays read-only");
        }
        (void)app_device_identity_of_wifi(&wifi, identity, sizeof(identity));
        binding = app_device_find(identity);
        CHECK(binding != NULL, "the Wi-Fi device exists by its identity key");
        if (binding != NULL) {
            CHECK(binding->recognition == APP_RECOGNITION_UNKNOWN,
                  "recognition state is unknown, got %s",
                  app_recognition_name(binding->recognition));
            CHECK(binding->read_only, "an unknown device is read-only");
        }
    }

    /* Repeating enrichment and materialisation must not churn the tables. */
    {
        app_recognizer_ref_t table_ref;
        size_t entities_before;
        size_t devices_before;

        recorded = app_recognition_enrich(&ev, &ref, &table);
        CHECK(recorded == 2u, "a second enrichment records the same two");
        CHECK(app_recognition_table_count(&table) == 2u,
              "the table was reset, not appended to");
        table_ref = app_recognition_table_recognizer(&table);
        entities_before = app_entity_count();
        devices_before = app_device_count();
        (void)app_device_materialize(&ev, &table_ref, &truncated);
        CHECK(app_entity_count() == entities_before,
              "entities: %lu before, %lu after",
              (unsigned long)entities_before, (unsigned long)app_entity_count());
        CHECK(app_device_count() == devices_before, "devices do not accumulate");
    }

    app_device_db_close(&g_db);
    store_release();
}

static void test_enrichment_without_a_database_reports_unavailable(void)
{
    app_scan_evidence_t ev;
    app_recognition_table_t table;
    app_recognizer_ref_t ref;
    app_scan_wifi_t wifi = make_wifi("SomeAP");
    const app_recognition_entry_t *entry;
    char identity[HA_CORE_ID_LEN];
    app_recognizer_ref_t table_ref;
    const app_device_binding_t *binding;
    bool truncated = false;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    (void)app_scan_ingest_wifi(&ev, &wifi);

    /* A recognizer whose database never opened. */
    store_use_fixture();
    g_store.medium_present = false;
    db_open_fixture();
    ref.ops = app_device_db_recognizer_ops();
    ref.ctx = &g_db;

    CHECK(app_recognition_enrich(&ev, &ref, &table) == 1u, "one observation recorded");
    (void)app_device_identity_of_wifi(&wifi, identity, sizeof(identity));
    entry = app_recognition_table_find(&table, identity);
    CHECK(entry != NULL, "the observation has an entry");
    if (entry != NULL) {
        CHECK(!entry->attempted,
              "the entry records that recognition could NOT run");
    }

    /* The device still appears, as a generic read-only Device. */
    table_ref = app_recognition_table_recognizer(&table);
    CHECK(app_device_materialize(&ev, &table_ref, &truncated) == 1u,
          "the device is still materialised");
    binding = app_device_find(identity);
    CHECK(binding != NULL, "the device exists");
    if (binding != NULL) {
        CHECK(binding->recognition == APP_RECOGNITION_DB_UNAVAILABLE,
              "state is db_unavailable, got %s",
              app_recognition_name(binding->recognition));
        CHECK(binding->read_only, "and it is read-only");
    }
    store_release();
}

static void test_recognition_table_capacity_is_reported(void)
{
    app_scan_evidence_t ev;
    app_recognition_table_t table;
    app_recognizer_ref_t ref;
    size_t recorded;

    store_use_fixture();
    db_open_fixture();
    ref.ops = app_device_db_recognizer_ops();
    ref.ctx = &g_db;

    app_scan_evidence_reset(&ev, 1u);
    /* One more observation than the table can hold. The evidence tables are
     * larger than the recognition table, so this is reachable in production. */
    for (size_t i = 0u; i < APP_RECOGNITION_TABLE_MAX + 1u; ++i) {
        app_scan_wifi_t obs;

        memset(&obs, 0, sizeof(obs));
        obs.bssid[0] = 0x10u;
        obs.bssid[1] = (uint8_t)i;
        obs.has_ssid = true;
        obs.ssid_len = 4u;
        memcpy(obs.ssid, "cap!", 4u);
        (void)app_scan_ingest_wifi(&ev, &obs);
    }

    recorded = app_recognition_enrich(&ev, &ref, &table);
    CHECK(ev.wifi_count == APP_RECOGNITION_TABLE_MAX + 1u,
          "the evidence table holds one more than the recognition table: %lu",
          (unsigned long)ev.wifi_count);
    CHECK(recorded == APP_RECOGNITION_TABLE_MAX + 1u,
          "every observation is attempted, got %lu", (unsigned long)recorded);
    CHECK(app_recognition_table_truncated(&table),
          "the overflow is reported, not hidden");
    CHECK(app_recognition_table_count(&table) == APP_RECOGNITION_TABLE_MAX,
          "the table is exactly full, got %lu",
          (unsigned long)app_recognition_table_count(&table));

    /* The observation that did not fit has no entry, and the ones that did are
     * untouched: capacity must not be reported as "recognition unavailable". */
    {
        char overflow_identity[HA_CORE_ID_LEN];

        (void)app_device_identity_of_wifi(&ev.wifi[APP_RECOGNITION_TABLE_MAX],
                                          overflow_identity,
                                          sizeof(overflow_identity));
        CHECK(app_recognition_table_find(&table, overflow_identity) == NULL,
              "the observation past capacity has no entry");
        (void)app_device_identity_of_wifi(&ev.wifi[0], overflow_identity,
                                          sizeof(overflow_identity));
        {
            const app_recognition_entry_t *first =
                app_recognition_table_find(&table, overflow_identity);

            CHECK(first != NULL && first->attempted,
                  "an entry that fit still records a real attempt");
        }
    }

    app_device_db_close(&g_db);
    store_release();
}

/* ---------------- a database switch ---------------- */

static void test_switching_the_database_replaces_recognition_without_orphans(void)
{
    app_scan_evidence_t ev;
    app_recognition_table_t table;
    app_recognizer_ref_t ref;
    app_scan_lan_t lan = make_lan("_ipp._tcp.local");
    app_recognizer_ref_t table_ref;
    const app_device_binding_t *binding;
    const app_recognition_entry_t *entry;
    char identity[HA_CORE_ID_LEN];
    bool truncated = false;
    size_t entities_after_match;

    app_device_table_reset();
    app_scan_evidence_reset(&ev, 1u);
    app_device_generation_begin(1u);
    (void)app_scan_ingest_lan(&ev, &lan);
    (void)app_device_identity_of_lan(&lan, identity, sizeof(identity));

    /* Generation 1: the corpus recognises the printer. */
    store_use_fixture();
    db_open_fixture();
    ref.ops = app_device_db_recognizer_ops();
    ref.ctx = &g_db;
    (void)app_recognition_enrich(&ev, &ref, &table);
    table_ref = app_recognition_table_recognizer(&table);
    (void)app_device_materialize(&ev, &table_ref, &truncated);
    entities_after_match = app_entity_count();
    binding = app_device_find(identity);
    CHECK(binding != NULL && binding->recognition == APP_RECOGNITION_MATCHED,
          "generation 1 matched");

    /* The card is swapped for one whose corpus does not know this device. */
    app_device_db_close(&g_db);
    store_release();
    store_use_fixture();
    g_store.file_present = false;
    db_open_fixture();
    CHECK(g_db.state == APP_DB_STATE_FILE_MISSING, "the replacement has no corpus");

    app_scan_evidence_reset(&ev, 2u);
    app_device_generation_begin(2u);
    (void)app_scan_ingest_lan(&ev, &lan);
    (void)app_recognition_enrich(&ev, &ref, &table);
    entry = app_recognition_table_find(&table, identity);
    CHECK(entry != NULL && !entry->attempted,
          "generation 2 records that recognition could not run");
    table_ref = app_recognition_table_recognizer(&table);
    (void)app_device_materialize(&ev, &table_ref, &truncated);

    binding = app_device_find(identity);
    CHECK(binding != NULL, "the device survives the database switch");
    if (binding != NULL) {
        CHECK(binding->recognition == APP_RECOGNITION_DB_UNAVAILABLE,
              "and is reported as unrecognised rather than matched, got %s",
              app_recognition_name(binding->recognition));
        CHECK(binding->read_only, "with no writable capability left over");
    }
    /* No orphaned entities: the same device must not accumulate state, and the
     * entity table must stay consistent with the binding table. */
    CHECK(app_entity_count() == entities_after_match,
          "entity count stable across the switch: %lu vs %lu",
          (unsigned long)entities_after_match, (unsigned long)app_entity_count());
    for (size_t i = 0u; i < app_entity_count(); ++i) {
        const app_entity_binding_t *e = app_entity_at(i);

        CHECK(e != NULL && app_device_find(e->device_id) != NULL,
              "every entity still has an owning device binding");
    }

    store_release();
}

/* ---------------- entry point ---------------- */

int main(void)
{
    resolve_fixture_dir();
    if (g_fixture_dir[0] == '\0') {
        printf("FATAL: cannot locate the device_db fixture corpus.\n");
        printf("       looked for -DDEVICE_DB_FIXTURE_DIR and a path relative to %s\n",
               __FILE__);
        return 2;
    }
    printf("--- fixture corpus: %s ---\n", g_fixture_dir);

    if (!load_file(g_fixture_dir, "devices_fixture.nbdb", &g_fixture,
                   &g_fixture_size)) {
        printf("FATAL: cannot read %s/devices_fixture.nbdb\n", g_fixture_dir);
        return 2;
    }

    test_open_fixture_is_ready();
    test_no_card_and_no_file_are_distinct();
    test_corrupt_and_incompatible_are_distinct();
    test_oversized_and_truncated();
    test_io_error_at_open();
    test_short_read_during_open_is_detected();
    test_open_reports_a_larger_file_than_exists();
    test_index_too_large_for_the_ram_budget();
    test_cancellation_during_validation();

    test_match_ambiguous();
    test_match_unknown();
    test_match_exact_key_does_not_merge_prefixed_profiles();
    test_control_is_refused_while_the_backend_cannot_be_driven();
    test_drivability_gates_a_write_target_per_backend();
    test_decoder_and_quirk_selection_is_reported();
    test_decoder_availability_is_real();

    test_same_length_rewrite_is_detected();
    test_payload_only_edits_are_caught_by_full_revalidation();
    test_header_rewrite_with_valid_crc_is_detected();
    test_replaced_file_with_identical_bytes_is_a_new_medium_state();
    test_growing_file_is_detected();
    test_io_error_during_match_is_detected();
    test_closed_database_cannot_match();

    test_enrichment_fills_the_table_and_materialisation_applies_it();
    test_enrichment_without_a_database_reports_unavailable();
    test_recognition_table_capacity_is_reported();
    test_switching_the_database_replaces_recognition_without_orphans();

    free(g_fixture);
    printf("app_device_db: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
