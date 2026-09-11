/*
 * Host regression tests for the Device DB import state machine.
 *
 * The real app_db_import.c runs here against a FAKE FAT FILESYSTEM that models the
 * three things that make this code hard to get right:
 *
 *   - rename() is not atomic across a power loss, so the fake can be "killed" after
 *     any operation and the files it left behind are then inspected;
 *   - the card can run out of space, stop responding, or report a short write;
 *   - a leftover .part or .bak from an earlier attempt is present at start-up.
 *
 * No real card is involved, so every interruption point in the replacement
 * sequence can be exercised - which is the only way to claim anything about
 * power-loss behaviour without a board.
 *
 * The corpus under test is the committed .nbdb fixture, so the header gates the
 * import applies are the same ones the format group already validates.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_db_import.h"
#include "app_device_db.h"
#include "app_str.h"
#include "device_db_format.h"

/*
 * The runner passes -DDEVICE_DB_FIXTURE_DIR. It is optional for the same reason as
 * in the other fixture-driven groups: a Windows path cannot be quoted portably
 * through a -D flag, so the directory is derived from __FILE__ when it is absent.
 * A directory that cannot be found fails the run rather than skipping it.
 */
#ifdef DEVICE_DB_FIXTURE_DIR
#define FIXTURE_DIR_CANDIDATE DEVICE_DB_FIXTURE_DIR
#else
#define FIXTURE_DIR_CANDIDATE ""
#endif

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

/* ---------------- fake FAT filesystem ---------------- */

#define FAKE_FILES 6u
#define FAKE_PATH_MAX 96u
#define FAKE_DATA_MAX (64u * 1024u)

typedef struct {
    char path[FAKE_PATH_MAX];
    uint8_t data[FAKE_DATA_MAX];
    uint32_t size;
    bool used;
} fake_file_t;

typedef struct {
    fake_file_t files[FAKE_FILES];

    bool medium;
    uint64_t free_bytes;
    bool report_free;

    /* Fault injection. */
    bool write_fails;
    bool sync_fails;
    bool rename_fails;
    bool unlink_fails;
    bool read_fails;

    /*
     * Fail the Nth write() call (1-based). 0 disables. A chunk boundary is the
     * realistic place for a card to stop accepting data, and it is the seam where
     * "fewer bytes than given" has to become an error rather than a success.
     *
     * Declared here rather than among the flags above, and deliberately not inside
     * the log array below: an earlier version of this struct had the field placed
     * between the log arrays, so the test wrote into the log buffer instead of the
     * flag and the fault never fired.
     */
    uint32_t write_fail_on_call;

    /* Power loss: after this many *mutating* operations the fake stops accepting
     * any further ones, exactly as a dead device would. */
    uint32_t kill_after_mutations;
    uint32_t mutations;
    bool killed;

    /* Call log, for asserting the order of the replacement sequence. */
    char log[24][64];
    size_t log_count;

    uint32_t write_calls;
    uint32_t rename_calls;
    uint32_t unlink_calls;
    uint32_t open_calls;
} fake_fs_t;

static void fake_log(fake_fs_t *fs, const char *fmt, const char *a, const char *b)
{
    if (fs->log_count >= 24u) {
        return;
    }
    (void)snprintf(fs->log[fs->log_count], sizeof(fs->log[0]), fmt, a, b);
    fs->log_count++;
}

static bool fake_killed(fake_fs_t *fs)
{
    if (fs->killed) {
        return true;
    }
    if (fs->kill_after_mutations != 0u &&
        fs->mutations >= fs->kill_after_mutations) {
        fs->killed = true;
    }
    return fs->killed;
}

static void fake_note_mutation(fake_fs_t *fs)
{
    fs->mutations++;
    (void)fake_killed(fs);
}

static fake_file_t *fake_find(fake_fs_t *fs, const char *path)
{
    for (size_t i = 0u; i < FAKE_FILES; ++i) {
        if (fs->files[i].used && strcmp(fs->files[i].path, path) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

static fake_file_t *fake_create(fake_fs_t *fs, const char *path)
{
    fake_file_t *file = fake_find(fs, path);

    if (file != NULL) {
        file->size = 0u;
        return file;
    }
    for (size_t i = 0u; i < FAKE_FILES; ++i) {
        if (!fs->files[i].used) {
            memset(&fs->files[i], 0, sizeof(fs->files[i]));
            fs->files[i].used = true;
            (void)app_strlcpy(fs->files[i].path, path, FAKE_PATH_MAX);
            return &fs->files[i];
        }
    }
    return NULL;
}

static void fake_remove(fake_fs_t *fs, const char *path)
{
    fake_file_t *file = fake_find(fs, path);

    if (file != NULL) {
        memset(file, 0, sizeof(*file));
    }
}

static fake_file_t *g_open_file;

/* ---------------- io vtable ---------------- */

static bool io_medium_present(void *ctx)
{
    return ((fake_fs_t *)ctx)->medium;
}

static bool io_free_bytes(void *ctx, uint64_t *out_free)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;

    if (!fs->report_free) {
        return false;
    }
    *out_free = fs->free_bytes;
    return true;
}

static esp_err_t io_write_open(void *ctx, const char *path)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;

    fs->open_calls++;
    fake_log(fs, "open %s", path, "");
    if (fake_killed(fs)) {
        return ESP_FAIL;
    }
    g_open_file = fake_create(fs, path);
    if (g_open_file == NULL) {
        return ESP_ERR_NO_MEM;
    }
    fake_note_mutation(fs);
    return ESP_OK;
}

static esp_err_t io_write(void *ctx, const void *data, size_t length)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;

    fs->write_calls++;
    if (fake_killed(fs)) {
        return ESP_FAIL;
    }
    if (fs->write_fails) {
        return ESP_FAIL;
    }
    if (g_open_file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fs->write_fail_on_call != 0u && fs->write_calls == fs->write_fail_on_call) {
        /* Model a short write: the card stops accepting data part way through the
         * upload. The contract says the implementation must report this as an
         * error, never as success with fewer bytes. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (g_open_file->size + length > FAKE_DATA_MAX) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(g_open_file->data + g_open_file->size, data, length);
    g_open_file->size += (uint32_t)length;
    return ESP_OK;
}

static esp_err_t io_write_sync(void *ctx)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;

    if (fake_killed(fs)) {
        return ESP_FAIL;
    }
    return fs->sync_fails ? ESP_FAIL : ESP_OK;
}

static void io_write_close(void *ctx)
{
    (void)ctx;
    g_open_file = NULL;
}

static esp_err_t io_read_all(void *ctx, const char *path, uint8_t *dst,
                             uint32_t capacity, uint32_t *out_size)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;
    fake_file_t *file = fake_find(fs, path);

    if (fs->read_fails) {
        return ESP_FAIL;
    }
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (file->size < capacity) {
        /* The caller asked for a whole header; a shorter file is its own failure
         * and must not look like a successful short read. */
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, file->data, capacity);
    *out_size = file->size;
    return ESP_OK;
}

static esp_err_t io_read_range(void *ctx, const char *path, uint32_t offset,
                               void *dst, uint32_t length)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;
    fake_file_t *file = fake_find(fs, path);

    if (fs->read_fails) {
        return ESP_FAIL;
    }
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset >= file->size || length > file->size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, file->data + offset, length);
    return ESP_OK;
}

static bool io_exists(void *ctx, const char *path)
{
    return fake_find((fake_fs_t *)ctx, path) != NULL;
}

static esp_err_t io_rename(void *ctx, const char *from, const char *to)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;
    fake_file_t *source;
    fake_file_t *target;

    fs->rename_calls++;
    fake_log(fs, "rename %s -> %s", from, to);
    if (fake_killed(fs)) {
        return ESP_FAIL;
    }
    if (fs->rename_fails) {
        return ESP_FAIL;
    }
    source = fake_find(fs, from);
    if (source == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    /*
     * FAT rename onto an existing name fails unless the target is removed first.
     * Modelling that is the point: it forces the implementation to clear a stale
     * sibling rather than assume the filesystem will overwrite.
     */
    target = fake_find(fs, to);
    if (target != NULL && target != source) {
        return ESP_FAIL;
    }
    {
        char moved[FAKE_DATA_MAX];
        uint32_t moved_size = source->size;

        memcpy(moved, source->data, moved_size);
        fake_remove(fs, from);
        target = fake_create(fs, to);
        if (target == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(target->data, moved, moved_size);
        target->size = moved_size;
    }
    fake_note_mutation(fs);
    return ESP_OK;
}

static esp_err_t io_unlink(void *ctx, const char *path)
{
    fake_fs_t *fs = (fake_fs_t *)ctx;

    fs->unlink_calls++;
    fake_log(fs, "unlink %s", path, "");
    if (fake_killed(fs)) {
        return ESP_FAIL;
    }
    if (fs->unlink_fails) {
        return ESP_FAIL;
    }
    fake_remove(fs, path);
    fake_note_mutation(fs);
    return ESP_OK;
}

static app_db_import_io_t io_ops(void)
{
    app_db_import_io_t io;

    memset(&io, 0, sizeof(io));
    io.medium_present = io_medium_present;
    io.free_bytes = io_free_bytes;
    io.write_open = io_write_open;
    io.write = io_write;
    io.write_sync = io_write_sync;
    io.write_close = io_write_close;
    io.read_all = io_read_all;
    io.read_range = io_read_range;
    io.exists = io_exists;
    io.rename = io_rename;
    io.unlink = io_unlink;
    return io;
}

/* ---------------- fixture ---------------- */

#define DB_PATH "/sdcard/nearby/db/devices.nbdb"
#define PART_PATH DB_PATH ".part"
#define BAK_PATH DB_PATH ".bak"

static uint8_t *g_fixture;
static uint32_t g_fixture_size;
static char g_fixture_dir[1024];

static char *join_path(const char *dir, const char *name)
{
    size_t len = strlen(dir) + strlen(name) + 2u;
    char *out = (char *)malloc(len);

    if (out != NULL) {
        (void)snprintf(out, len, "%s/%s", dir, name);
    }
    return out;
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

static bool fixture_dir_usable(const char *dir)
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
    static const char *const suffixes[] = {
        "/../fixtures/device_db",
        "/tests/fixtures/device_db",
        "/../../tests/fixtures/device_db",
    };
    static const char separators[] = { '/', '\\' };

    if (fixture_dir_usable(FIXTURE_DIR_CANDIDATE)) {
        (void)snprintf(g_fixture_dir, sizeof(g_fixture_dir), "%s",
                       FIXTURE_DIR_CANDIDATE);
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
            (void)snprintf(g_fixture_dir + dir_len,
                           sizeof(g_fixture_dir) - dir_len, "%s", suffixes[s]);
            if (fixture_dir_usable(g_fixture_dir)) {
                return;
            }
        }
    }
    g_fixture_dir[0] = '\0';
}

/*
 * Truncate a corpus to `new_size` and repair its header, so the result is a
 * DIFFERENT but fully VALID corpus.
 *
 * The tests need two distinguishable records that are both acceptable: changing
 * any header byte breaks the header checksum, so a marked copy is not usable.
 * Cutting the index region short is a real difference in the bytes on the card,
 * and repairing the declared length, the body checksum and the header checksum
 * keeps it something the reader and the importer both accept.
 *
 * The result is used only where the question is "which of the two files ended up at
 * the path", never as an input to matching.
 */
static bool fs_truncate_corpus(const char *path, uint32_t new_size);

/* Size of the shortened old corpus the rename-window tests use. */
#define OLD_CORPUS_SIZE 2048u

/* ---------------- harness ---------------- */

static fake_fs_t g_fs;
static app_db_import_t g_imp;

static void fs_reset(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    g_fs.medium = true;
    g_fs.report_free = true;
    g_fs.free_bytes = 8u * 1024u * 1024u;
    g_open_file = NULL;
}

/*
 * Put a byte-perfect copy of the fixture at `path`.
 *
 * The slot is cleared first: the fake reuses slots, so without that a shorter file
 * written after a longer one keeps the longer one's tail and every size assertion
 * ends up being about leftover bytes rather than about the file.
 */
static void fs_put_fixture(const char *path)
{
    fake_file_t *file = fake_create(&g_fs, path);

    if (file == NULL || g_fixture_size > FAKE_DATA_MAX) {
        printf("FATAL: fake filesystem cannot hold the fixture\n");
        exit(2);
    }
    memset(file->data, 0, sizeof(file->data));
    memcpy(file->data, g_fixture, g_fixture_size);
    file->size = g_fixture_size;
}

/*
 * Put a damaged copy at `path`: one flipped byte in the index region, so the header
 * stays intact and only the body checksum no longer matches. This is the case a
 * header-only probe cannot see, which is what makes it useful for testing that the
 * full verification is actually used.
 */
static void fs_put_damaged(const char *path)
{
    fs_put_fixture(path);
    fake_find(&g_fs, path)->data[g_fixture_size - 1u] ^= 0x01u;
}

/*
 * Put a copy with a broken header at `path`: byte 100 is inside the region the
 * header checksum covers, so `probe()` - which checks only the header - rejects it
 * too. Use this when a test needs something that no check can accept.
 */
static void fs_put_header_damaged(const char *path)
{
    fs_put_fixture(path);
    fake_find(&g_fs, path)->data[100] ^= 0x01u;
}

/* Defined after the harness; see the comment on the declaration above. */
static bool fs_truncate_corpus(const char *path, uint32_t new_size)
{
    fake_file_t *file = fake_find(&g_fs, path);
    uint32_t body_crc;
    uint32_t header_crc;

    if (file == NULL || new_size < DEVICE_DB_HEADER_SIZE || new_size > file->size) {
        return false;
    }
    file->size = new_size;
    memset(file->data + new_size, 0, sizeof(file->data) - new_size);

    file->data[16] = (uint8_t)(new_size & 0xFFu);
    file->data[17] = (uint8_t)((new_size >> 8) & 0xFFu);
    file->data[18] = (uint8_t)((new_size >> 16) & 0xFFu);
    file->data[19] = (uint8_t)((new_size >> 24) & 0xFFu);

    body_crc = device_db_crc32(file->data + DEVICE_DB_HEADER_SIZE,
                               new_size - DEVICE_DB_HEADER_SIZE);
    file->data[116] = (uint8_t)(body_crc & 0xFFu);
    file->data[117] = (uint8_t)((body_crc >> 8) & 0xFFu);
    file->data[118] = (uint8_t)((body_crc >> 16) & 0xFFu);
    file->data[119] = (uint8_t)((body_crc >> 24) & 0xFFu);

    header_crc = device_db_crc32(file->data, 120u);
    file->data[120] = (uint8_t)(header_crc & 0xFFu);
    file->data[121] = (uint8_t)((header_crc >> 8) & 0xFFu);
    file->data[122] = (uint8_t)((header_crc >> 16) & 0xFFu);
    file->data[123] = (uint8_t)((header_crc >> 24) & 0xFFu);
    return true;
}

static void imp_init(uint32_t max_bytes)
{
    app_db_import_io_t io = io_ops();

    CHECK(app_db_import_init(&g_imp, &io, &g_fs, DB_PATH, max_bytes) == ESP_OK,
          "import handle initialises");
}

/* Stream the whole fixture through write() in the configured chunk size. */
static app_db_import_error_t imp_upload(const uint8_t *data, uint32_t size)
{
    uint32_t sent = 0u;

    while (sent < size) {
        uint32_t chunk = size - sent;

        if (chunk > APP_DB_IMPORT_CHUNK) {
            chunk = APP_DB_IMPORT_CHUNK;
        }
        {
            app_db_import_error_t err =
                app_db_import_write(&g_imp, data + sent, chunk);

            if (err != APP_DB_IMPORT_ERR_NONE) {
                return err;
            }
        }
        sent += chunk;
    }
    return APP_DB_IMPORT_ERR_NONE;
}

/* ---------------- tests ---------------- */

static void test_begin_requires_a_medium(void)
{
    uint32_t version = 0u;

    fs_reset();
    fs_put_fixture(DB_PATH);
    g_fs.medium = false;
    imp_init(0u);

    CHECK(app_db_import_begin(&g_imp, g_fixture_size) ==
              APP_DB_IMPORT_ERR_NO_MEDIUM,
          "an upload without a card is refused");
    CHECK(g_imp.phase == APP_DB_IMPORT_FAILED, "phase is failed");
    CHECK(!g_fs.files[0].used || fake_find(&g_fs, PART_PATH) == NULL,
          "and no .part file was created");
    CHECK(app_db_import_finish(&g_imp, &version) == APP_DB_IMPORT_ERR_NOT_ACTIVE,
          "and finishing it is refused");
}

static void test_declared_length_is_enforced_both_ways(void)
{
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);

    /* Longer than declared: refused before the byte is written. */
    CHECK(app_db_import_begin(&g_imp, 100u) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    CHECK(app_db_import_write(&g_imp, g_fixture, 101u) ==
              APP_DB_IMPORT_ERR_LENGTH_MISMATCH,
          "a body longer than Content-Length is refused");
    CHECK(g_imp.phase == APP_DB_IMPORT_FAILED, "and the import fails");
    app_db_import_cancel(&g_imp);
    CHECK(fake_find(&g_fs, DB_PATH) != NULL, "the live corpus is untouched");

    /* Shorter than declared: finishing fails, because a truncated body that ends
     * on a valid boundary is still truncated. */
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);
    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "second begin accepted");
    CHECK(imp_upload(g_fixture, g_fixture_size - 1u) == APP_DB_IMPORT_ERR_NONE,
          "short body streamed");
    {
        uint32_t version = 0u;

        CHECK(app_db_import_finish(&g_imp, &version) ==
                  APP_DB_IMPORT_ERR_LENGTH_MISMATCH,
              "fewer bytes than Content-Length is a failure");
        CHECK(version == 0u, "and no version is reported");
    }
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "the .part file was removed");
    CHECK(fake_find(&g_fs, DB_PATH) != NULL, "the live corpus is untouched");
}

static void test_policy_cap_and_free_space_are_checked_before_writing(void)
{
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(2048u);

    CHECK(app_db_import_begin(&g_imp, 4096u) == APP_DB_IMPORT_ERR_TOO_LARGE,
          "a declared size above the policy cap is refused");
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "and nothing was written");

    /* Free space is consulted before the first byte, so a full card cannot be
     * filled by an upload that was never going to fit. */
    fs_reset();
    fs_put_fixture(DB_PATH);
    g_fs.free_bytes = 1024u;
    imp_init(0u);
    CHECK(app_db_import_begin(&g_imp, g_fixture_size) ==
              APP_DB_IMPORT_ERR_NO_SPACE,
          "an upload larger than the free space is refused");
    CHECK(g_fs.write_calls == 0u, "without writing anything, got %u calls",
          (unsigned)g_fs.write_calls);

    /* Unknown free space must not be treated as zero. */
    fs_reset();
    fs_put_fixture(DB_PATH);
    g_fs.report_free = false;
    imp_init(0u);
    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "an unknown free-space figure does not block the upload");
    app_db_import_cancel(&g_imp);
}

static void test_successful_import_replaces_and_reports_version(void)
{
    uint32_t version = 0u;

    fs_reset();
    /* An older corpus at the path, so the replacement is observable. */
    fs_put_fixture(DB_PATH);
    fake_find(&g_fs, DB_PATH)->data[20] = 7u; /* content_version low byte */
    imp_init(0u);

    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    CHECK(imp_upload(g_fixture, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "the whole body streamed");
    CHECK(g_imp.received == g_fixture_size, "byte count is exact");

    CHECK(app_db_import_finish(&g_imp, &version) == APP_DB_IMPORT_ERR_NONE,
          "finish succeeded");
    CHECK(g_imp.phase == APP_DB_IMPORT_DONE, "phase is done");
    CHECK(version == 20260911u, "the fixture's content version was read, got %lu",
          (unsigned long)version);
    CHECK(g_imp.profile_count == 5u, "and its profile count, got %lu",
          (unsigned long)g_imp.profile_count);

    /* The path now holds the uploaded bytes, byte for byte. */
    {
        fake_file_t *file = fake_find(&g_fs, DB_PATH);

        CHECK(file != NULL, "the corpus is at the path");
        CHECK(file != NULL && file->size == g_fixture_size, "with the right size");
        CHECK(file != NULL &&
                  memcmp(file->data, g_fixture, g_fixture_size) == 0,
              "and the right content");
    }
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "no .part left behind");
    CHECK(fake_find(&g_fs, BAK_PATH) == NULL,
          "and the previous copy was retired once the path was confirmed good");

    /* The replacement order is what the recovery logic depends on: the backup is
     * only ever taken after the upload is written, and removed only after the new
     * corpus is at the path. */
    CHECK(g_fs.log_count >= 3u, "the sequence was logged");
    CHECK(strncmp(g_fs.log[0], "open ", 5) == 0, "step 1 is the .part open, got '%s'",
          g_fs.log[0]);
}

static void test_import_over_an_absent_corpus_is_not_a_rename_of_nothing(void)
{
    uint32_t version = 0u;

    fs_reset(); /* no corpus at the path at all: a first provisioning */
    imp_init(0u);

    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    CHECK(imp_upload(g_fixture, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "body streamed");
    CHECK(app_db_import_finish(&g_imp, &version) == APP_DB_IMPORT_ERR_NONE,
          "finish succeeded");
    CHECK(fake_find(&g_fs, DB_PATH) != NULL, "the corpus is now at the path");
    CHECK(fake_find(&g_fs, BAK_PATH) == NULL, "and no empty backup was created");
    CHECK(version == 20260911u, "version reported");
}

static void test_invalid_upload_leaves_the_old_corpus(void)
{
    uint32_t version = 0u;
    uint8_t *junk = (uint8_t *)malloc(g_fixture_size);

    CHECK(junk != NULL, "scratch allocation");
    if (junk == NULL) {
        return;
    }
    memset(junk, 0x5A, g_fixture_size);

    /* Wholly invalid bytes. */
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);
    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    CHECK(imp_upload(junk, g_fixture_size) == APP_DB_IMPORT_ERR_NONE, "streamed");
    CHECK(app_db_import_finish(&g_imp, &version) ==
              APP_DB_IMPORT_ERR_INVALID_DB,
          "junk is refused as an invalid database");
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "the .part file was removed");
    {
        fake_file_t *file = fake_find(&g_fs, DB_PATH);

        CHECK(file != NULL && file->size == g_fixture_size,
              "the old corpus is still at the path");
    }

    /* A damaged corpus: header intact, body checksum broken. The import's header
     * gates accept it, which is correct - the reader reports CORRUPT when it opens
     * the promoted file, and the import has not touched the live corpus yet. What
     * this test pins is that a *structurally* invalid file never gets that far. */
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);
    memcpy(junk, g_fixture, g_fixture_size);
    junk[4] = 0x7Fu; /* unknown format version */
    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    CHECK(imp_upload(junk, g_fixture_size) == APP_DB_IMPORT_ERR_NONE, "streamed");
    CHECK(app_db_import_finish(&g_imp, &version) ==
              APP_DB_IMPORT_ERR_INVALID_DB,
          "an incompatible format version is refused");
    CHECK(fake_find(&g_fs, DB_PATH) != NULL, "old corpus untouched");

    /* Truncated header only. */
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);
    CHECK(app_db_import_begin(&g_imp, 64u) == APP_DB_IMPORT_ERR_NONE, "begin");
    CHECK(imp_upload(g_fixture, 64u) == APP_DB_IMPORT_ERR_NONE, "64 bytes streamed");
    CHECK(app_db_import_finish(&g_imp, &version) ==
              APP_DB_IMPORT_ERR_INVALID_DB,
          "a file shorter than a header is refused");

    free(junk);
}

static void test_a_short_write_is_a_failure_not_a_success(void)
{
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);

    /* The card stops accepting data on the second chunk. */
    g_fs.write_fail_on_call = 2u;
    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    {
        app_db_import_error_t err = imp_upload(g_fixture, g_fixture_size);

        CHECK(err == APP_DB_IMPORT_ERR_SHORT_WRITE,
              "a card that takes fewer bytes than given is a failure, got %s",
              app_db_import_error_name(err));
    }
    CHECK(g_imp.phase == APP_DB_IMPORT_FAILED, "phase is failed");
    app_db_import_cancel(&g_imp);
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "the partial .part was removed");
    {
        fake_file_t *file = fake_find(&g_fs, DB_PATH);

        CHECK(file != NULL && file->size == g_fixture_size,
              "the live corpus is untouched");
    }
}

static void test_sync_failure_aborts_before_replacing(void)
{
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);
    g_fs.sync_fails = true;

    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    CHECK(imp_upload(g_fixture, g_fixture_size) == APP_DB_IMPORT_ERR_NONE, "streamed");
    {
        uint32_t version = 0u;

        CHECK(app_db_import_finish(&g_imp, &version) == APP_DB_IMPORT_ERR_IO,
              "a failed flush stops the import before the rename");
    }
    CHECK(g_fs.rename_calls == 0u, "nothing was renamed, got %u calls",
          (unsigned)g_fs.rename_calls);
    CHECK(fake_find(&g_fs, DB_PATH) != NULL, "old corpus untouched");
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, ".part cleaned up");
}

static void test_promotion_failure_restores_the_previous_corpus(void)
{
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);

    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted");
    CHECK(imp_upload(g_fixture, g_fixture_size) == APP_DB_IMPORT_ERR_NONE, "streamed");

    /* The second rename fails: db -> .bak succeeded, .part -> db did not. */
    g_fs.rename_fails = true;
    {
        uint32_t version = 0u;

        CHECK(app_db_import_finish(&g_imp, &version) == APP_DB_IMPORT_ERR_IO,
              "a failed promotion is reported as an I/O failure");
    }
    CHECK(fake_find(&g_fs, DB_PATH) != NULL,
          "the previous corpus was put back at the path");
    CHECK(fake_find(&g_fs, BAK_PATH) == NULL, "and the backup was consumed");
}

static void test_begin_clears_a_leftover_part_file(void)
{
    fs_reset();
    fs_put_fixture(DB_PATH);
    /* Debris from an earlier interrupted upload. */
    fs_put_damaged(PART_PATH);
    imp_init(0u);

    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "begin accepted despite the leftover");
    {
        fake_file_t *part = fake_find(&g_fs, PART_PATH);

        CHECK(part == NULL || part->size == 0u,
              "the leftover .part was removed before the new one was opened");
    }
    CHECK(imp_upload(g_fixture, g_fixture_size) == APP_DB_IMPORT_ERR_NONE, "streamed");
    {
        uint32_t version = 0u;

        CHECK(app_db_import_finish(&g_imp, &version) == APP_DB_IMPORT_ERR_NONE,
              "and the import completed");
    }
}

static void test_a_second_import_is_refused(void)
{
    fs_reset();
    fs_put_fixture(DB_PATH);
    imp_init(0u);

    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_NONE,
          "first begin accepted");
    CHECK(app_db_import_begin(&g_imp, g_fixture_size) == APP_DB_IMPORT_ERR_BUSY,
          "a second concurrent begin is refused");
    app_db_import_cancel(&g_imp);
}

/* ---------------- power loss ---------------- */

/*
 * Every interruption point of the sequence, in order. The fake stops accepting
 * mutating operations once `kill_after_mutations` is reached, which is what power
 * loss looks like from the code's point of view: the call fails and nothing after
 * it happens.
 *
 * After the "death", recovery runs on the files that were left behind, and the
 * assertion is always the same: exactly one usable corpus exists at the path.
 */
static void test_power_loss_at_every_step_still_leaves_one_valid_corpus(void)
{
    for (uint32_t kill = 1u; kill <= 6u; ++kill) {
        uint32_t version = 0u;
        app_db_recovery_action_t action;
        app_db_state_t state = APP_DB_STATE_CLOSED;

        fs_reset();
        fs_put_fixture(DB_PATH);
        imp_init(0u);
        g_fs.kill_after_mutations = kill;

        (void)app_db_import_begin(&g_imp, g_fixture_size);
        if (g_imp.phase == APP_DB_IMPORT_RECEIVING) {
            (void)imp_upload(g_fixture, g_fixture_size);
        }
        if (g_imp.phase == APP_DB_IMPORT_RECEIVING) {
            (void)app_db_import_finish(&g_imp, &version);
        }

        /* Reboot: the fake is alive again, the files are whatever the interruption
         * left behind. */
        g_fs.killed = false;
        g_fs.kill_after_mutations = 0u;
        g_open_file = NULL;

        action = app_db_import_recover(&g_imp, &state);

        {
            uint32_t recovered_version = 0u;
            uint32_t recovered_profiles = 0u;
            bool usable = app_db_import_probe(&g_imp, DB_PATH, &recovered_version,
                                              &recovered_profiles);

            CHECK(usable,
                  "kill after %u mutations: a usable corpus is at the path "
                  "(action=%s)", (unsigned)kill, app_db_recovery_action_name(action));
            CHECK(state == APP_DB_STATE_READY,
                  "kill after %u mutations: recovery reports READY, got %s",
                  (unsigned)kill, app_db_state_name(state));
        }
        CHECK(fake_find(&g_fs, PART_PATH) == NULL ||
                  !app_db_import_probe(&g_imp, PART_PATH, NULL, NULL),
              "kill after %u mutations: no valid .part is left as debris",
              (unsigned)kill);
    }
}

/*
 * The specific window the sequence exists for: the old corpus has been renamed to
 * .bak and the new one has not yet taken the path. Recovery must put the old one
 * back, and it must be the OLD one - a recovery that silently promoted the upload
 * would be replacing a validated corpus with one the operator never confirmed.
 */
static void test_recovery_restores_the_old_corpus_in_the_rename_window(void)
{
    app_db_state_t state = APP_DB_STATE_CLOSED;
    app_db_recovery_action_t action;

    fs_reset();
    /*
     * Model the crash state directly: the path is empty, .bak holds the OLD corpus
     * (shorter than the upload, and still a valid corpus), and .part holds a
     * validated upload that was never promoted.
     */
    fs_put_fixture(BAK_PATH);
    CHECK(fs_truncate_corpus(BAK_PATH, OLD_CORPUS_SIZE),
          "the old corpus was shortened into a distinct valid corpus");
    fs_put_fixture(PART_PATH);

    imp_init(0u);
    CHECK(app_db_import_verify(&g_imp, BAK_PATH, NULL, NULL),
          "the shortened old corpus is genuinely valid");
    action = app_db_import_recover(&g_imp, &state);
    CHECK(action == APP_DB_RECOVERY_RESTORED_OLD,
          "the old corpus was restored, got %s",
          app_db_recovery_action_name(action));
    CHECK(state == APP_DB_STATE_READY, "and it is ready");
    {
        fake_file_t *file = fake_find(&g_fs, DB_PATH);

        CHECK(file != NULL && file->size == OLD_CORPUS_SIZE,
              "and it is the OLD corpus, not the unconfirmed upload (size %lu)",
              (unsigned long)(file != NULL ? file->size : 0u));
    }
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "the unconfirmed .part was removed");
}

/*
 * The other window: the upload was validated but the first rename had not happened.
 * Here promoting the .part IS correct - it passed the same gates the reader applies
 * - and leaving it as debris would lose a completed upload.
 */
static void test_recovery_promotes_a_validated_part_when_there_is_no_old_corpus(void)
{
    app_db_state_t state = APP_DB_STATE_CLOSED;
    app_db_recovery_action_t action;

    fs_reset();
    fs_put_fixture(PART_PATH); /* no corpus at the path: a first provisioning */
    imp_init(0u);

    action = app_db_import_recover(&g_imp, &state);
    CHECK(action == APP_DB_RECOVERY_PROMOTED_PART,
          "the validated upload was promoted, got %s",
          app_db_recovery_action_name(action));
    CHECK(state == APP_DB_STATE_READY, "and it is ready");
    CHECK(fake_find(&g_fs, DB_PATH) != NULL, "the corpus is at the path");
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "and the .part is gone");
}

static void test_recovery_discards_debris_and_reports_no_corpus(void)
{
    app_db_state_t state = APP_DB_STATE_CLOSED;
    app_db_recovery_action_t action;

    fs_reset();
    /*
     * Nothing usable anywhere: the path is empty, and both siblings hold a payload
     * that does not match the checksum its own header declares. Both need a broken
     * HEADER to be rejected by the header probe as well - a payload-only fault is
     * exactly what the header probe cannot see, and that case has its own test.
     */
    fs_put_header_damaged(PART_PATH);
    fs_put_header_damaged(BAK_PATH);
    imp_init(0u);
    action = app_db_import_recover(&g_imp, &state);
    CHECK(action == APP_DB_RECOVERY_DISCARDED, "damaged debris was discarded, got %s",
          app_db_recovery_action_name(action));
    CHECK(state == APP_DB_STATE_FILE_MISSING,
          "and the corpus is reported as absent, not corrupt, got %s",
          app_db_state_name(state));
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, ".part removed");
    CHECK(fake_find(&g_fs, BAK_PATH) == NULL, ".bak removed");
}

/*
 * A damaged sibling must not be promoted just because its header is intact. This
 * is the specific case a header-only check gets wrong, so it is asserted directly.
 */
static void test_a_damaged_part_is_not_promoted(void)
{
    uint32_t version = 0u;

    fs_reset();
    fs_put_damaged(PART_PATH);
    imp_init(0u);

    CHECK(app_db_import_probe(&g_imp, PART_PATH, NULL, NULL),
          "the header alone looks fine, which is the trap");
    CHECK(!app_db_import_verify(&g_imp, PART_PATH, &version, NULL),
          "but full verification rejects it");
    CHECK(version == 0u, "and reports no version");

    (void)app_db_import_recover(&g_imp, &version);
    CHECK(fake_find(&g_fs, DB_PATH) == NULL,
          "so the damaged upload was NOT promoted to the corpus path");
}

static void test_recovery_keeps_the_good_copy_when_restoring_fails(void)
{
    app_db_state_t state = APP_DB_STATE_CLOSED;

    fs_reset();
    /* The only good copy is in .bak, and the restore cannot complete. */
    fs_put_fixture(BAK_PATH);
    CHECK(fs_truncate_corpus(BAK_PATH, OLD_CORPUS_SIZE), "old corpus prepared");

    imp_init(0u);
    g_fs.rename_fails = true; /* the restore cannot complete */
    (void)app_db_import_recover(&g_imp, &state);

    CHECK(state == APP_DB_STATE_IO_ERROR,
          "a failed restore is reported as an I/O problem, got %s",
          app_db_state_name(state));
    CHECK(fake_find(&g_fs, BAK_PATH) != NULL,
          "and the only good copy was NOT destroyed, because a later attempt "
          "can still restore it");
}

static void test_recovery_reports_no_medium_without_touching_anything(void)
{
    app_db_state_t state = APP_DB_STATE_CLOSED;

    fs_reset();
    fs_put_fixture(DB_PATH);
    fs_put_fixture(PART_PATH);
    g_fs.medium = false;
    imp_init(0u);

    CHECK(app_db_import_recover(&g_imp, &state) == APP_DB_RECOVERY_NO_MEDIUM,
          "no card is reported as such");
    CHECK(state == APP_DB_STATE_SD_MISSING, "state is sd_missing, got %s",
          app_db_state_name(state));
    CHECK(g_fs.unlink_calls == 0u, "nothing was removed, got %u unlink calls",
          (unsigned)g_fs.unlink_calls);
}

static void test_a_valid_corpus_with_debris_reports_no_recovery(void)
{
    app_db_state_t state = APP_DB_STATE_CLOSED;

    fs_reset();
    fs_put_fixture(DB_PATH);
    fs_put_damaged(PART_PATH);
    fs_put_fixture(BAK_PATH);
    imp_init(0u);

    CHECK(app_db_import_recover(&g_imp, &state) == APP_DB_RECOVERY_NONE,
          "an intact corpus is not a recovery event");
    CHECK(state == APP_DB_STATE_READY, "state is ready");
    CHECK(fake_find(&g_fs, DB_PATH) != NULL, "corpus kept");
    CHECK(fake_find(&g_fs, PART_PATH) == NULL, "stale .part cleaned");
    CHECK(fake_find(&g_fs, BAK_PATH) == NULL, "stale .bak cleaned");
}

int main(void)
{
    resolve_fixture_dir();
    if (g_fixture_dir[0] == '\0') {
        printf("FATAL: cannot locate the device_db fixture corpus\n");
        return 2;
    }
    if (!load_file(g_fixture_dir, "devices_fixture.nbdb", &g_fixture,
                   &g_fixture_size)) {
        printf("FATAL: cannot read the fixture\n");
        return 2;
    }
    printf("--- fixture corpus: %s (%lu bytes) ---\n", g_fixture_dir,
           (unsigned long)g_fixture_size);

    test_begin_requires_a_medium();
    test_declared_length_is_enforced_both_ways();
    test_policy_cap_and_free_space_are_checked_before_writing();
    test_successful_import_replaces_and_reports_version();
    test_import_over_an_absent_corpus_is_not_a_rename_of_nothing();
    test_invalid_upload_leaves_the_old_corpus();
    test_a_short_write_is_a_failure_not_a_success();
    test_sync_failure_aborts_before_replacing();
    test_promotion_failure_restores_the_previous_corpus();
    test_begin_clears_a_leftover_part_file();
    test_a_second_import_is_refused();

    test_power_loss_at_every_step_still_leaves_one_valid_corpus();
    test_recovery_restores_the_old_corpus_in_the_rename_window();
    test_recovery_promotes_a_validated_part_when_there_is_no_old_corpus();
    test_recovery_discards_debris_and_reports_no_corpus();
    test_a_damaged_part_is_not_promoted();
    test_recovery_keeps_the_good_copy_when_restoring_fails();
    test_recovery_reports_no_medium_without_touching_anything();
    test_a_valid_corpus_with_debris_reports_no_recovery();

    free(g_fixture);
    printf("app_db_import: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
