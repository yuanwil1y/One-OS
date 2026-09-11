/*
 * Host tests for the Device DB (.nbdb) validating reader.
 *
 * This is the check that the format contract actually holds: the firmware reader
 * is run against the exact fixtures produced by tools/device_db, so the C reader
 * and the Python generator/validator must agree byte for byte or these tests
 * fail.
 *
 * Two directions are asserted:
 *   1. the valid fixture is accepted AND its records decode to the expected
 *      values, so the reader is not merely permissive;
 *   2. every deliberately damaged variant is REJECTED, so the reader is not
 *      merely strict about nothing.
 *
 * The fixture list is read from the generated manifest rather than hard-coded,
 * so a missing or renamed variant fails the test instead of being skipped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_db_format.h"

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

/* Read a whole file. Returns NULL on failure. */
static uint8_t *read_file(const char *path, uint32_t *out_size)
{
    FILE *handle = fopen(path, "rb");
    long size;
    uint8_t *buffer;

    if (handle == NULL) {
        return NULL;
    }
    if (fseek(handle, 0L, SEEK_END) != 0) {
        fclose(handle);
        return NULL;
    }
    size = ftell(handle);
    if (size <= 0) {
        fclose(handle);
        return NULL;
    }
    if (fseek(handle, 0L, SEEK_SET) != 0) {
        fclose(handle);
        return NULL;
    }
    buffer = (uint8_t *)malloc((size_t)size);
    if (buffer == NULL) {
        fclose(handle);
        return NULL;
    }
    if (fread(buffer, 1u, (size_t)size, handle) != (size_t)size) {
        free(buffer);
        fclose(handle);
        return NULL;
    }
    fclose(handle);
    *out_size = (uint32_t)size;
    return buffer;
}

static bool string_equals(device_db_string_t s, const char *want)
{
    size_t length = strlen(want);

    if (s.length != length) {
        return false;
    }
    return memcmp(s.data, want, length) == 0;
}

static char *join(const char *dir, const char *name)
{
    size_t need = strlen(dir) + strlen(name) + 2u;
    char *path = (char *)malloc(need);

    if (path == NULL) {
        return NULL;
    }
    snprintf(path, need, "%s/%s", dir, name);
    return path;
}

/*
 * Where the corpus lives.
 *
 * The runner passes -DDEVICE_DB_FIXTURE_DIR. It is optional here so the group can
 * also run on a host whose shell cannot quote a Windows path through a -D flag:
 * without it, the directory is derived from this file's own __FILE__, which is
 * correct both in-tree and out-of-tree. A directory that cannot be found fails the
 * run rather than skipping, so the reader is never silently untested.
 */
#ifdef DEVICE_DB_FIXTURE_DIR
#define FIXTURE_DIR_CANDIDATE DEVICE_DB_FIXTURE_DIR
#else
#define FIXTURE_DIR_CANDIDATE ""
#endif

static char g_fixture_dir[1024];

static bool fixture_dir_usable(const char *dir)
{
    char *path;
    FILE *f;

    if (dir == NULL || dir[0] == '\0') {
        return false;
    }
    path = join(dir, "devices_fixture.nbdb");
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
    static const char *const suffixes[] = {
        "/../fixtures/device_db",
        "/tests/fixtures/device_db",
        "/../../tests/fixtures/device_db",
    };
    static const char separators[] = { '/', '\\' };

    if (fixture_dir_usable(FIXTURE_DIR_CANDIDATE)) {
        snprintf(g_fixture_dir, sizeof(g_fixture_dir), "%s", FIXTURE_DIR_CANDIDATE);
        return;
    }

    for (size_t s = 0u; s < sizeof(suffixes) / sizeof(suffixes[0]); ++s) {
        for (size_t k = 0u; k < sizeof(separators); ++k) {
            const char *slash = strrchr(__FILE__, separators[k]);
            size_t dir_len;
            size_t total;

            if (slash == NULL) {
                continue;
            }
            dir_len = (size_t)(slash - __FILE__);
            total = dir_len + strlen(suffixes[s]) + 1u;
            if (total >= sizeof(g_fixture_dir)) {
                continue;
            }
            memcpy(g_fixture_dir, __FILE__, dir_len);
            snprintf(g_fixture_dir + dir_len, sizeof(g_fixture_dir) - dir_len, "%s",
                     suffixes[s]);
            if (fixture_dir_usable(g_fixture_dir)) {
                return;
            }
        }
    }
    g_fixture_dir[0] = '\0';
}

/* ---------------- the valid fixture ---------------- */

static void test_valid_fixture(void)
{
    char *path = join(g_fixture_dir, "devices_fixture.nbdb");
    uint32_t size = 0u;
    uint8_t *bytes;
    device_db_t db;
    device_db_status_t status;
    device_db_info_t info;

    CHECK(path != NULL, "path allocation");
    if (path == NULL) {
        return;
    }
    bytes = read_file(path, &size);
    CHECK(bytes != NULL, "valid fixture must be readable at %s", path);
    if (bytes == NULL) {
        free(path);
        return;
    }

    status = device_db_open(bytes, size, &db);
    CHECK(status == DEVICE_DB_OK, "valid fixture rejected: %s (%s)",
          device_db_status_name(status), device_db_status_detail());
    if (status != DEVICE_DB_OK) {
        free(bytes);
        free(path);
        return;
    }

    CHECK(device_db_get_info(&db, &info), "info available");
    CHECK(info.content_version == 20260911u, "content version, got %u",
          info.content_version);
    /* A reproducible build records timestamp 0 rather than a wall clock. */
    CHECK(info.build_timestamp == 0u, "build timestamp must be 0 for reproducibility");
    CHECK(info.profile_count == 5u, "profile count, got %u", info.profile_count);
    CHECK(info.recipe_count == 7u, "recipe count, got %u", info.recipe_count);
    CHECK(info.provenance_count == 5u, "provenance count, got %u",
          info.provenance_count);
    CHECK(info.index_bucket_count == 16u, "bucket count, got %u",
          info.index_bucket_count);

    /* Profile ids ascend and are addressable by id. */
    for (uint32_t i = 0u; i < info.profile_count; ++i) {
        device_db_profile_t profile;
        CHECK(device_db_get_profile(&db, i, &profile), "profile %u readable", i);
        if (i > 0u) {
            device_db_profile_t previous;
            CHECK(device_db_get_profile(&db, i - 1u, &previous), "previous profile");
            CHECK(profile.profile_id > previous.profile_id,
                  "profile ids must ascend");
        }
    }

    /* The known BLE beacon: identity, display name, decoder and two sensors. */
    {
        device_db_profile_t profile;
        CHECK(device_db_find_profile(&db, 1001u, &profile),
              "profile 1001 must exist");
        CHECK(string_equals(profile.display_name, "Example Temp/Humidity Beacon"),
              "profile 1001 display name");
        CHECK(string_equals(profile.model, "TH-BEACON-1"), "profile 1001 model");
        CHECK(profile.theengs_decoder_id == 1u, "profile 1001 decoder id, got %u",
              profile.theengs_decoder_id);
        CHECK(profile.writable == false, "profile 1001 must be read-only");
        CHECK(profile.recipe_count == 2u, "profile 1001 recipe count, got %u",
              profile.recipe_count);

        for (uint32_t r = 0u; r < profile.recipe_count; ++r) {
            device_db_recipe_t recipe;
            CHECK(device_db_get_recipe(&db, profile.first_recipe_index + r, &recipe),
                  "recipe %u readable", r);
            CHECK(recipe.profile_id == 1001u, "recipe belongs to profile 1001");
            CHECK(recipe.backend == DEVICE_DB_BACKEND_PASSIVE_VALUE,
                  "beacon recipes are passive values");
            CHECK(recipe.write_target_id == DEVICE_DB_NO_INDEX,
                  "a passive recipe must not be writable");
            CHECK(recipe.domain_id == DEVICE_DB_DOMAIN_SENSOR,
                  "beacon recipes are sensors");
        }
    }

    /* The Zigbee plug: writable, with a command recipe carrying endpoint/cluster. */
    {
        device_db_profile_t profile;
        bool found_command = false;

        CHECK(device_db_find_profile(&db, 1002u, &profile), "profile 1002 must exist");
        CHECK(profile.writable, "profile 1002 must be writable");
        CHECK(profile.zha_quirk_id == 1u, "profile 1002 quirk id");
        for (uint32_t r = 0u; r < profile.recipe_count; ++r) {
            device_db_recipe_t recipe;
            CHECK(device_db_get_recipe(&db, profile.first_recipe_index + r, &recipe),
                  "zigbee recipe %u readable", r);
            if (recipe.backend == DEVICE_DB_BACKEND_ZIGBEE_COMMAND) {
                found_command = true;
                CHECK(recipe.endpoint == 1u, "zigbee command endpoint, got %u",
                      recipe.endpoint);
                CHECK(recipe.cluster == 6u, "zigbee command cluster (OnOff), got %u",
                      recipe.cluster);
                CHECK(recipe.command == 1u, "zigbee command id (On), got %u",
                      recipe.command);
            }
        }
        CHECK(found_command, "profile 1002 must have a zigbee command recipe");
    }

    /* The ambiguous pair really does share a key, which is the point of it. */
    {
        device_db_profile_t a;
        device_db_profile_t b;
        device_db_fingerprint_t fa;

        CHECK(device_db_find_profile(&db, 1001u, &a), "1001");
        CHECK(device_db_find_profile(&db, 1003u, &b), "1003");
        CHECK(device_db_get_fingerprint(&db, a.fingerprint_first, &fa), "1001 fp");
        CHECK(fa.protocol == DEVICE_DB_PROTO_BLE, "1001 fingerprint is BLE");
        CHECK(string_equals(fa.key, "181c"), "1001 key, got '%.*s'",
              (int)fa.key.length, fa.key.data);
        CHECK(fa.key_hash == device_db_canonical_key_hash(fa.key.data, fa.key.length),
              "fingerprint key_hash must match the canonical key it names");
    }

    /* The distinct model has a different key so it can never merge with 1001. */
    {
        device_db_profile_t same;
        device_db_profile_t distinct;
        device_db_fingerprint_t fa;
        device_db_fingerprint_t fb;

        CHECK(device_db_find_profile(&db, 1001u, &same), "1001");
        CHECK(device_db_find_profile(&db, 1004u, &distinct), "1004");
        CHECK(device_db_get_fingerprint(&db, same.fingerprint_first, &fa), "1001 fp");
        CHECK(device_db_get_fingerprint(&db, distinct.fingerprint_first, &fb),
              "1004 fp");
        CHECK(fa.key_hash != fb.key_hash,
              "1004 must not share 1001's fingerprint key");
    }

    /* Identity rules: the Zigbee one is STRONG and requires authorization; the
     * BLE one is deliberately WEAK and must never be treated as a cross-protocol
     * merge key. */
    {
        device_db_profile_t zigbee;
        device_db_identity_t rule;
        bool saw_strong = false;
        bool saw_weak = false;

        CHECK(device_db_find_profile(&db, 1002u, &zigbee), "1002");
        for (uint32_t i = 0u; i < zigbee.identity_count; ++i) {
            CHECK(device_db_get_identity(&db, zigbee.identity_first + i, &rule),
                  "zigbee identity %u", i);
            if (rule.kind == DEVICE_DB_IDENTITY_ZIGBEE_IEEE) {
                saw_strong = rule.strength == DEVICE_DB_STRENGTH_STRONG;
                CHECK((rule.flags & 2u) != 0u,
                      "zigbee identity must require authorization");
            }
        }
        CHECK(saw_strong, "profile 1002 must carry a STRONG zigbee identity rule");

        {
            device_db_profile_t beacon;
            CHECK(device_db_find_profile(&db, 1001u, &beacon), "1001");
            for (uint32_t i = 0u; i < beacon.identity_count; ++i) {
                CHECK(device_db_get_identity(&db, beacon.identity_first + i, &rule),
                      "beacon identity %u", i);
                if (rule.kind == DEVICE_DB_IDENTITY_VENDOR_PROTOCOL_UUID) {
                    saw_weak = rule.strength == DEVICE_DB_STRENGTH_WEAK;
                }
            }
        }
        CHECK(saw_weak, "profile 1001 identity must be WEAK, not STRONG");
    }

    /* Provenance: nothing in the fixture may cite a reference-only source. */
    for (uint32_t i = 0u; i < info.provenance_count; ++i) {
        device_db_provenance_t prov;
        CHECK(device_db_get_provenance(&db, i, &prov), "provenance %u readable", i);
        CHECK(prov.reuse != DEVICE_DB_REUSE_REFERENCE_ONLY,
              "provenance %u must not be reference-only", i);
        CHECK(prov.source_id != 0u, "provenance %u must cite a source", i);
    }

    /* Index: a lookup for the beacon key must find a candidate, and the caller
     * must confirm it against the record. */
    {
        const uint8_t key[] = "181c";
        uint32_t indices[8];
        uint32_t count = 0u;
        bool incomplete = false;
        uint32_t hash = device_db_canonical_key_hash(key, sizeof(key) - 1u);
        bool found = false;

        CHECK(device_db_index_lookup(&db, DEVICE_DB_PROTO_BLE, hash, indices, 8u,
                                     &count, &incomplete),
              "index lookup runs");
        for (uint32_t i = 0u; i < count; ++i) {
            device_db_fingerprint_t fp;
            CHECK(device_db_get_fingerprint(&db, indices[i], &fp), "index candidate");
            if (fp.key_hash == hash && fp.protocol == DEVICE_DB_PROTO_BLE &&
                string_equals(fp.key, "181c")) {
                found = true;
            }
        }
        CHECK(found, "index must surface the beacon fingerprint");
        /* This key is shared by 1001 and 1003, so the caller must treat the
         * result as ambiguous rather than picking the first hit. */
        CHECK(count >= 2u,
              "the shared key must yield both candidate profiles, got %u", count);
    }

    free(bytes);
    free(path);
}

/* ---------------- accessor bounds ---------------- */

static void test_accessor_bounds(void)
{
    char *path = join(g_fixture_dir, "devices_fixture.nbdb");
    uint32_t size = 0u;
    uint8_t *bytes;
    device_db_t db;
    device_db_profile_t profile;
    device_db_recipe_t recipe;
    device_db_info_t info;

    if (path == NULL) {
        return;
    }
    bytes = read_file(path, &size);
    if (bytes == NULL) {
        CHECK(false, "fixture unreadable");
        free(path);
        return;
    }
    if (device_db_open(bytes, size, &db) != DEVICE_DB_OK) {
        CHECK(false, "fixture invalid");
        free(bytes);
        free(path);
        return;
    }

    CHECK(device_db_get_info(&db, &info), "info");
    CHECK(!device_db_get_profile(&db, info.profile_count, &profile),
          "profile index past the end must fail");
    CHECK(!device_db_get_recipe(&db, info.recipe_count, &recipe),
          "recipe index past the end must fail");
    CHECK(!device_db_find_profile(&db, 0u, &profile),
          "profile id 0 must never be found");
    CHECK(!device_db_find_profile(&db, 999999u, &profile),
          "an unknown profile id must not be found");

    CHECK(!device_db_get_info(NULL, &info), "null db");
    CHECK(!device_db_get_info(&db, NULL), "null out");

    free(bytes);
    free(path);
}

/* ---------------- empty and boundary inputs ---------------- */

static void test_open_rejects_degenerate_inputs(void)
{
    device_db_t db;
    uint8_t tiny[8] = {0};

    CHECK(device_db_open(NULL, 0u, &db) == DEVICE_DB_ERR_CORRUPT,
          "null buffer rejected");
    CHECK(device_db_open(tiny, 0u, &db) == DEVICE_DB_ERR_TRUNCATED,
          "zero length is truncated");
    CHECK(device_db_open(tiny, sizeof(tiny), &db) == DEVICE_DB_ERR_TRUNCATED,
          "8 bytes is truncated");
    /* A full-size header that is all zeroes must be rejected on magic. */
    {
        uint8_t zeros[DEVICE_DB_HEADER_SIZE];
        memset(zeros, 0, sizeof(zeros));
        CHECK(device_db_open(zeros, sizeof(zeros), &db) == DEVICE_DB_ERR_NOT_A_DB,
              "zeroed header is not a database");
    }
}

/* ---------------- every damaged variant must be rejected ---------------- */

static void test_invalid_variants_rejected(void)
{
    char *manifest_path = join(g_fixture_dir, "invalid/manifest.txt");
    FILE *manifest;
    char line[256];
    unsigned count = 0u;

    CHECK(manifest_path != NULL, "manifest path");    if (manifest_path == NULL) {
        return;
    }
    manifest = fopen(manifest_path, "r");
    CHECK(manifest != NULL,
          "invalid-fixture manifest must exist at %s; run "
          "tools/device_db/make_invalid_fixtures.py", manifest_path);
    if (manifest == NULL) {
        free(manifest_path);
        return;
    }

    while (fgets(line, sizeof(line), manifest) != NULL) {
        char *newline = strchr(line, '\n');
        char *path;
        uint32_t size = 0u;
        uint8_t *bytes;
        device_db_t db;
        device_db_status_t status;

        if (newline != NULL) {
            *newline = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }
        ++count;

        {
            char *invalid_dir = join(g_fixture_dir, "invalid");

            path = invalid_dir == NULL ? NULL : join(invalid_dir, line);
            free(invalid_dir);
        }
        CHECK(path != NULL, "variant path");
        if (path == NULL) {
            continue;
        }
        bytes = read_file(path, &size);
        CHECK(bytes != NULL, "variant %s must be readable", line);
        if (bytes == NULL) {
            free(path);
            continue;
        }

        status = device_db_open(bytes, size, &db);
        /* The exact rejection class is not asserted: what matters is that the
         * reader refuses the file. Requiring a specific code here would make the
         * test brittle while proving nothing extra. */
        CHECK(status != DEVICE_DB_OK, "variant %s must be REJECTED, got %s",
              line, device_db_status_name(status));
        if (status != DEVICE_DB_OK && device_db_status_detail()[0] == '\0') {
            CHECK(false, "variant %s rejected without a reason", line);
        }

        free(bytes);
        free(path);
    }

    fclose(manifest);
    CHECK(count >= 25u, "expected at least 25 invalid variants, loaded %u", count);
    printf("  rejected %u invalid variants\n", count);

    free(manifest_path);
}

/* ---------------- structural constants ---------------- */

static void test_format_constants(void)
{
    /* These mirror docs/device-db-format.md; a mismatch means the document and
     * the code have drifted apart. */
    CHECK(DEVICE_DB_HEADER_SIZE == 128u, "header size");
    CHECK(DEVICE_DB_PROVENANCE_SIZE == 24u, "provenance size");
    CHECK(DEVICE_DB_IDENTITY_SIZE == 24u, "identity size");
    CHECK(DEVICE_DB_PROFILE_SIZE == 96u, "profile size");
    CHECK(DEVICE_DB_FINGERPRINT_SIZE == 40u, "fingerprint size");
    CHECK(DEVICE_DB_RECIPE_SIZE == 72u, "recipe size");
    CHECK(DEVICE_DB_INDEX_BUCKET_SIZE == 32u, "index bucket size");

    /* CRC-32 of "123456789" is the standard check value 0xCBF43926. */
    {
        const uint8_t check[] = "123456789";
        CHECK(device_db_crc32(check, sizeof(check) - 1u) == 0xCBF43926u,
              "CRC-32 must match the standard check value, got 0x%08X",
              device_db_crc32(check, sizeof(check) - 1u));
    }
    /* FNV-1a 32 of the empty string is the offset basis. */
    CHECK(device_db_key_hash((const uint8_t *)"", 0u) == 2166136261u,
          "FNV-1a offset basis");

    /*
     * Canonical key normalisation is part of the format contract: the generator,
     * this reader and the host validator must fold keys identically, or a valid
     * file looks corrupt. These cases pin the rule.
     */
    {
        /* Vector cross-checked against tools/device_db/nbdb.py normalize_key():
         * '  Example|Plug-ZB-2  ' folds to 'example|plugzb2'. */
        const uint8_t raw[] = "  Example|Plug-ZB-2  ";
        const uint8_t folded[] = "example|plugzb2";

        CHECK(device_db_canonical_key_hash(raw, sizeof(raw) - 1u) ==
                  device_db_key_hash(folded, sizeof(folded) - 1u),
              "canonical hash must trim, lowercase and drop ':' and '-'");
        /* Already-canonical input must be unchanged. */
        CHECK(device_db_canonical_key_hash(folded, sizeof(folded) - 1u) ==
                  device_db_key_hash(folded, sizeof(folded) - 1u),
              "canonical hash of canonical input is idempotent");
        /* A MAC-style key folds identically however it is punctuated. */
        {
            const uint8_t a[] = "AA:BB:CC:DD:EE:FF";
            const uint8_t b[] = "aabbccddeeff";
            CHECK(device_db_canonical_key_hash(a, sizeof(a) - 1u) ==
                      device_db_key_hash(b, sizeof(b) - 1u),
                  "punctuated and plain addresses must fold to the same hash");
        }
        CHECK(device_db_canonical_key_hash(NULL, 0u) == 0u, "null key is rejected");
    }
}

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

    test_format_constants();
    test_valid_fixture();
    test_accessor_bounds();
    test_open_rejects_degenerate_inputs();
    test_invalid_variants_rejected();

    printf("device_db_format: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
