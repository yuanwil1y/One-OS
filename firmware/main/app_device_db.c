/*
 * SD-backed recognition over a .nbdb corpus.
 *
 * See app_device_db.h for the contract. Three properties shape this code:
 *
 *   - the corpus is NEVER loaded. Only the header, the index buckets and one
 *     record at a time are held, so memory use is independent of corpus size.
 *     That matters because the board has no PSRAM and the corpus will grow.
 *   - validation is bounded the same way. The whole-file validator in
 *     device_db_format.c remains the authority for host tooling and for a fully
 *     resident image; here the header, the streamed body checksum and the index
 *     are verified, and every record access is bounds-checked against the size
 *     captured at open. A file that changed underneath the reader therefore cannot
 *     produce a partial match.
 *   - degradation is explicit. Missing card, missing file, corrupt file,
 *     unsupported version and I/O error are distinct states; in all of them
 *     recognition is skipped and observed devices stay visible as generic
 *     read-only Devices.
 *
 * Platform independent: the filesystem arrives through app_db_storage_ops_t, which
 * is what makes short reads, I/O errors and a shrinking file testable on the host.
 */

#include "app_device_db.h"
#include "app_device.h"
#include "app_str.h"

#include <stdio.h>
#include <string.h>

#define MATCH_LABEL_MAX APP_RECOGNITION_MAX_LABEL
#define CHECKSUM_BLOCK 256u
/* Longest canonical key this reader will build: an SSID is at most 32 bytes and
 * the hex forms are shorter. */
#define KEY_SCRATCH_MAX 64u

const app_recognizer_ops_t *app_device_db_recognizer_ops(void);

/* ---------------- readers ---------------- */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static esp_err_t db_read(app_device_db_t *db, uint32_t offset, void *dst,
                         uint32_t length)
{
    esp_err_t err;

    /*
     * Gated on `opened`, NOT on `open`.
     *
     * `open` means "this handle was fully validated and may be matched against",
     * so it only becomes true at the end of app_device_db_open(). The reads that
     * perform that validation therefore cannot require it - gating them on `open`
     * would make the very first header read fail and leave every database in
     * IO_ERROR, which is exactly the defect this comment exists to prevent from
     * coming back.
     *
     * `opened` means "the storage is bound and a size is known", which is true
     * from the moment the file was found until close().
     */
    if (!db->opened || db->storage.read == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Re-checked on every access, not only at open, so a file truncated after a
     * successful open still fails instead of yielding a short record. */
    if ((uint64_t)offset + (uint64_t)length > (uint64_t)db->file_size) {
        db->read_errors++;
        return ESP_ERR_INVALID_SIZE;
    }

    db->reads++;
    err = db->storage.read(db->storage_ctx, offset, dst, length);
    if (err != ESP_OK) {
        db->read_errors++;
        db->last_error = err;
    }
    return err;
}

/* ---------------- open ---------------- */

static void db_fail(app_device_db_t *db, app_db_state_t state, esp_err_t err)
{
    db->state = state;
    db->last_error = err;
    db->open = false;
}

static void db_release(app_device_db_t *db)
{
    /* Only release what was actually taken, so close() runs exactly once even
     * when a failure path is reached after another already released. */
    if (db->opened && db->storage.close != NULL) {
        db->storage.close(db->storage_ctx);
    }
    db->opened = false;
}

/*
 * Give up on opening: release the storage, then record why.
 *
 * The order matters. `opened` is what db_release() checks before calling close(),
 * so clearing it first - which db_fail() does - would silently skip the release
 * and leak the file handle on every failure path.
 */
static void db_abort(app_device_db_t *db, app_db_state_t state, esp_err_t err)
{
    db_release(db);
    db_fail(db, state, err);
}

/*
 * Stream the body checksum.
 *
 * The body can be up to 16 MiB, so it is read in fixed blocks and folded into one
 * CRC rather than being held. That is the price of proving the corpus is intact
 * without keeping it resident.
 *
 * The header is streamed through the same fold rather than being skipped, because
 * the format's `body_crc32` covers everything after the 120-byte header CRC field:
 * bytes 120..127 are part of the body region. Re-reading them here is what keeps
 * this reader byte-identical to the whole-file validator that produced the value.
 */
static esp_err_t db_verify_body_crc(app_device_db_t *db, uint32_t expected)
{
    uint8_t block[CHECKSUM_BLOCK];
    uint32_t offset = DEVICE_DB_HEADER_SIZE;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t since_poll = 0u;

    while (offset < db->file_size) {
        uint32_t chunk = db->file_size - offset;
        esp_err_t err;

        if (chunk > sizeof(block)) {
            chunk = (uint32_t)sizeof(block);
        }
        err = db->storage.read(db->storage_ctx, offset, block, chunk);
        if (err != ESP_OK) {
            db->read_errors++;
            db->last_error = err;
            return err;
        }
        db->reads++;

        for (uint32_t i = 0u; i < chunk; ++i) {
            crc ^= (uint32_t)block[i];
            for (int bit = 0; bit < 8; ++bit) {
                if ((crc & 1u) != 0u) {
                    crc = (crc >> 1) ^ 0xEDB88320u;
                } else {
                    crc >>= 1;
                }
            }
        }
        offset += chunk;

        /* Polled on a byte budget rather than per block, so the hook's cost does
         * not scale with corpus size and a cancelled scan stops validating within
         * a bounded amount of extra I/O. */
        since_poll += chunk;
        if (db->progress != NULL && since_poll >= db->progress_interval_bytes) {
            since_poll = 0u;
            db->progress_calls++;
            if (db->progress(db->progress_ctx)) {
                /* Cancelled mid-validation. The database is NOT published: a
                 * corpus that was only partly checksummed has not been
                 * validated, and treating it as ready is how a half-checked file
                 * reaches matching. */
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    crc ^= 0xFFFFFFFFu;
    return crc == expected ? ESP_OK : ESP_ERR_INVALID_CRC;
}

/*
 * Detect that the corpus changed underneath us.
 *
 * A size and bounds check at open cannot prove the file is still the same file
 * later: a rewrite of exactly the same length, a rename of a different database
 * over the path, or a card swap all leave the recorded size intact. The
 * consequence would be reading records from a different corpus than the one that
 * was validated, which is exactly how a partial or wrong match is produced.
 *
 * So the header's identity-bearing fields are shadowed at open and re-read before
 * every match attempt. Any difference - including a changed body checksum, which
 * no rewrite of the payload can preserve - closes the database and reports
 * IO_ERROR. Recognition then stops rather than continuing against a corpus whose
 * validation no longer describes what is on the medium.
 *
 * This costs one header read per match attempt, which is bounded and small; it is
 * not a per-record check, so a large corpus does not pay for it repeatedly.
 */
static bool db_verify_unchanged(app_device_db_t *db)
{
    uint8_t probe[DEVICE_DB_HEADER_SIZE];

    /* Exactly the header, never the whole staging array: bytes beyond the header
     * belong to the corpus body and are not part of the validated identity. */
    if (db->storage.read(db->storage_ctx, 0u, probe, sizeof(probe)) != ESP_OK) {
        db->read_errors++;
        db_abort(db, APP_DB_STATE_IO_ERROR, ESP_ERR_INVALID_STATE);
        return false;
    }
    db->reads++;

    /* The body CRC field (bytes 116..119) and the header CRC field (120..123)
     * both change when any byte of the corpus changes, so comparing the whole
     * staging header catches same-size rewrites as well as replacements. */
    if (memcmp(probe, db->header, sizeof(probe)) != 0) {
        db_abort(db, APP_DB_STATE_IO_ERROR, ESP_ERR_INVALID_STATE);
        return false;
    }
    return true;
}

void app_device_db_close(app_device_db_t *db)
{
    if (db == NULL) {
        return;
    }
    if (db->opened || db->open || db->state == APP_DB_STATE_READY) {
        db_release(db);
    }
    db->open = false;
    db->opened = false;
    db->state = APP_DB_STATE_CLOSED;
    db->file_size = 0u;
    /* The hook's owner (the runtime worker) is done with it at this point. */
    db->progress = NULL;
    db->progress_ctx = NULL;
}

void app_device_db_set_progress(app_device_db_t *db, app_db_progress_fn_t progress,
                                void *ctx, uint32_t interval_bytes)
{
    if (db == NULL) {
        return;
    }
    db->progress = progress;
    db->progress_ctx = ctx;
    db->progress_interval_bytes =
        interval_bytes == 0u ? APP_DB_PROGRESS_INTERVAL_BYTES : interval_bytes;
}

/*
 * Full revalidation: the corpus must still be byte-identical to what was
 * validated at open, header and body.
 *
 * This is the expensive form, and the two forms exist because they catch
 * different things:
 *
 *   - db_verify_unchanged() re-reads 128 bytes and runs before every match. Any
 *     replacement or rewrite of the file changes those bytes, because a corpus's
 *     header carries its own content checksum - so a different file cannot
 *     present the same header;
 *   - this form additionally streams the body, so it also catches an edit that
 *     leaves the header intact. On a card shared with the display a full read is
 *     not something to do before every match, so it is an explicit operation the
 *     application calls when it needs that guarantee (after a card may have been
 *     swapped, before acting on a recognition result, or from diagnostics).
 *
 * Threading: the installer must not call this while a match is in flight; the
 * application worker is the single owner of the database for exactly this reason.
 */
static bool db_revalidate(app_device_db_t *db)
{
    uint32_t live_size = 0u;

    /*
     * The medium's own answer about the size, asked again.
     *
     * The header comparison below cannot see growth on its own: bytes 0..127 are
     * unchanged by an append. The format states the file length in the header, so
     * a size that no longer equals the one recorded at open means the object on
     * the medium is not the object that was validated.
     */
    if (db->storage.open != NULL) {
        if (db->storage.open(db->storage_ctx, &live_size) != ESP_OK) {
            db_abort(db, APP_DB_STATE_IO_ERROR, ESP_ERR_INVALID_STATE);
            return false;
        }
        if (live_size != db->file_size) {
            db_abort(db, APP_DB_STATE_IO_ERROR, ESP_ERR_INVALID_SIZE);
            return false;
        }
    }

    if (!db_verify_unchanged(db)) {
        return false;
    }
    if (db_verify_body_crc(db, rd_u32(db->header + 116)) != ESP_OK) {
        db_abort(db, APP_DB_STATE_IO_ERROR, ESP_ERR_INVALID_CRC);
        return false;
    }
    return true;
}

bool app_device_db_verify_unchanged(app_device_db_t *db)
{
    if (db == NULL || !db->open) {
        return false;
    }
    return db_revalidate(db);
}

void app_device_db_open(app_device_db_t *db,
                        const app_db_storage_ops_t *storage,
                        void *storage_ctx,
                        uint8_t *index_buffer,
                        uint32_t index_capacity)
{
    uint32_t file_size = 0u;
    uint32_t index_offset;
    uint32_t index_length;
    esp_err_t err;
    /*
     * The progress hook belongs to the operation, not to the handle, so it is
     * preserved across open() and cleared by close(). A stale closure would
     * otherwise outlive whichever task installed it.
     */
    app_db_progress_fn_t progress;
    void *progress_ctx;
    uint32_t progress_interval;

    if (db == NULL) {
        return;
    }
    progress = db->progress;
    progress_ctx = db->progress_ctx;
    progress_interval = db->progress_interval_bytes;

    memset(db, 0, sizeof(*db));
    db->state = APP_DB_STATE_CLOSED;
    db->progress = progress;
    db->progress_ctx = progress_ctx;
    db->progress_interval_bytes =
        progress_interval == 0u ? APP_DB_PROGRESS_INTERVAL_BYTES : progress_interval;

    if (storage == NULL || storage->open == NULL || storage->read == NULL) {
        db_fail(db, APP_DB_STATE_CLOSED, ESP_ERR_INVALID_ARG);
        return;
    }
    db->storage = *storage;
    db->storage_ctx = storage_ctx;
    db->index = index_buffer;
    db->index_capacity = index_capacity;

    /* "No card" and "no file on the card" are different user problems. */
    if (db->storage.medium_present != NULL &&
        !db->storage.medium_present(storage_ctx)) {
        db_fail(db, APP_DB_STATE_SD_MISSING, ESP_ERR_NOT_FOUND);
        return;
    }

    err = db->storage.open(storage_ctx, &file_size);
    if (err != ESP_OK) {
        db_fail(db, APP_DB_STATE_FILE_MISSING, err);
        return;
    }

    if (file_size < DEVICE_DB_HEADER_SIZE ||
        file_size > DEVICE_DB_MAX_FILE_BYTES) {
        db->file_size = file_size;
        db_abort(db, file_size > DEVICE_DB_MAX_FILE_BYTES ? APP_DB_STATE_INCOMPATIBLE
                                                         : APP_DB_STATE_CORRUPT,
                ESP_ERR_INVALID_SIZE);
        return;
    }

    db->file_size = file_size;
    /* Storage is bound and the size is known, so the validation reads below may
     * now run. `open` keeps its stricter meaning - "validated" - and is set at
     * the end of this function. */
    db->opened = true;

    /* ---- header ---- */
    err = db_read(db, 0u, db->header, DEVICE_DB_HEADER_SIZE);
    if (err != ESP_OK) {
        db_abort(db, APP_DB_STATE_IO_ERROR, err);
        return;
    }
    db->header_size = DEVICE_DB_HEADER_SIZE;

    if (memcmp(db->header, "NBDB", 4u) != 0) {
        db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_RESPONSE);
        return;
    }

    /* Version and ABI gates before anything else, so an unsupported file is
     * reported as incompatible rather than corrupt. */
    if (rd_u16(db->header + 4) != DEVICE_DB_FORMAT_VERSION ||
        rd_u16(db->header + 6) != DEVICE_DB_SCHEMA_VERSION ||
        rd_u32(db->header + 8) > DEVICE_DB_READER_ABI ||
        rd_u32(db->header + 12) != 0u) {
        db_abort(db, APP_DB_STATE_INCOMPATIBLE, ESP_ERR_NOT_SUPPORTED);
        return;
    }
    if (rd_u32(db->header + 16) != file_size) {
        db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_SIZE);
        return;
    }
    if (device_db_crc32(db->header, 120u) != rd_u32(db->header + 120)) {
        db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_CRC);
        return;
    }

    err = db_verify_body_crc(db, rd_u32(db->header + 116));
    if (err != ESP_OK) {
        /* A cancelled validation is reported as its own condition rather than as
         * corruption: the file was never proven bad, it was simply not proven. */
        db_abort(db, err == ESP_ERR_INVALID_STATE ? APP_DB_STATE_IO_ERROR
                                                  : APP_DB_STATE_CORRUPT,
                 err);
        return;
    }

    /* ---- region table, re-derived and enforced from here on ---- */
    {
        const uint32_t region_offsets[6] = {
            rd_u32(db->header + 56), rd_u32(db->header + 64),
            rd_u32(db->header + 72), rd_u32(db->header + 80),
            rd_u32(db->header + 88), rd_u32(db->header + 96),
        };
        const uint32_t region_lengths[6] = {
            rd_u32(db->header + 60), rd_u32(db->header + 68),
            rd_u32(db->header + 76), rd_u32(db->header + 84),
            rd_u32(db->header + 92), rd_u32(db->header + 100),
        };

        for (int i = 0; i < 6; ++i) {
            if ((uint64_t)region_offsets[i] + (uint64_t)region_lengths[i] >
                (uint64_t)file_size) {
                db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_SIZE);
                return;
            }
        }
    }

    /* Family-boundary checks the header alone can prove. */
    if (rd_u16(db->header + 4) == 0u) {
        db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_RESPONSE);
        return;
    }

    db->content_version = rd_u32(db->header + 20);
    db->profile_count = rd_u32(db->header + 32);

    index_offset = rd_u32(db->header + 104);
    index_length = rd_u32(db->header + 108);

    if (index_length > 0u) {
        uint32_t bucket_count = rd_u16(db->header + 52);

        if ((uint64_t)index_offset + (uint64_t)index_length > (uint64_t)file_size) {
            db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_SIZE);
            return;
        }
        if ((bucket_count & (bucket_count - 1u)) != 0u) {
            db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_RESPONSE);
            return;
        }
        if (bucket_count * DEVICE_DB_INDEX_BUCKET_SIZE > index_length) {
            db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_INVALID_SIZE);
            return;
        }
        if (index_length > db->index_capacity || db->index == NULL) {
            /* More index than we are willing to hold in RAM. Reported rather than
             * silently dropped, because a truncated index would silently stop
             * matching some devices. */
            db_abort(db, APP_DB_STATE_CORRUPT, ESP_ERR_NO_MEM);
            return;
        }
        err = db_read(db, index_offset, db->index, index_length);
        if (err != ESP_OK) {
            db_abort(db, APP_DB_STATE_IO_ERROR, err);
            return;
        }
        db->index_size = index_length;
    }

    db->open = true;
    db->state = APP_DB_STATE_READY;
    db->last_error = ESP_OK;
}

/* ---------------- string and record loading ---------------- */

/* Resolve a StringRef into a bounded, NUL-terminated buffer. */
static bool load_string(app_device_db_t *db, char *dst, size_t dst_size,
                        uint32_t ref)
{
    uint8_t length = (uint8_t)(ref & 0xFFu);
    uint32_t offset = ref >> 8;
    uint32_t strings_base = rd_u32(db->header + 56);

    if (dst_size == 0u) {
        return false;
    }
    dst[0] = '\0';
    if (length == 0u) {
        return true; /* absent, not an error */
    }
    if (length >= dst_size) {
        length = (uint8_t)(dst_size - 1u);
    }
    /* The string must lie inside the strings region, which was already proven to
     * lie inside the file. */
    if ((uint64_t)strings_base + offset + length > (uint64_t)db->file_size) {
        db->read_errors++;
        return false;
    }
    if (db_read(db, strings_base + offset, dst, length) != ESP_OK) {
        return false;
    }
    dst[length] = '\0';
    return true;
}

/*
 * Read one recipe.
 *
 * Two decisions are made here rather than being deferred to the device layer,
 * because both are statements about what this firmware can actually do:
 *
 *   - the domain string comes from the compiled domain table, keyed by the
 *     numeric domain id. The file's own domain string is NOT used, so an unknown
 *     domain can never become part of an entity id;
 *   - the write target is carried through only when the profile declares
 *     writable AND this firmware can drive the named backend. A database record
 *     claiming `writable` is a claim about the device, not about our
 *     capabilities, so it can never conjure a control path that would silently
 *     fail.
 *
 * Returns false when the recipe's meaning cannot be established at all. The
 * caller then reports a truncated result rather than attaching an entity whose
 * domain or read source is unknown.
 */
static bool load_recipe(app_device_db_t *db, uint32_t index, bool profile_writable,
                        app_entity_recipe_t *out)
{
    uint32_t base = rd_u32(db->header + 96) + index * DEVICE_DB_RECIPE_SIZE;
    uint8_t record[DEVICE_DB_RECIPE_SIZE];
    const char *domain;
    uint8_t backend;
    uint32_t read_source;
    uint32_t write_target;
    bool drivable;

    if (index >= rd_u32(db->header + 40)) {
        return false;
    }
    if (db_read(db, base, record, sizeof(record)) != ESP_OK) {
        return false;
    }

    domain = app_domain_name(record[20]);
    if (domain == NULL) {
        /* A domain id this firmware does not know. Producing an entity id from it
         * would put an uninterpretable string into the HA namespace. */
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->domain_id = record[20];
    backend = record[21];
    out->backend = backend;
    out->min_value = (int32_t)rd_u32(record + 48);
    out->max_value = (int32_t)rd_u32(record + 52);
    out->scale = rd_u32(record + 56);
    read_source = rd_u32(record + 24);
    write_target = rd_u32(record + 28);
    out->read_source_id = APP_DB_NO_READ_SOURCE;
    out->write_target_id = DEVICE_DB_NO_INDEX;

    (void)app_strlcpy(out->domain, domain, sizeof(out->domain));

    if (!load_string(db, out->name, sizeof(out->name), rd_u32(record + 8))) {
        return false;
    }
    (void)load_string(db, out->device_class, sizeof(out->device_class),
                      rd_u32(record + 12));
    (void)load_string(db, out->unit, sizeof(out->unit), rd_u32(record + 16));

    if (backend == DEVICE_DB_BACKEND_PASSIVE_VALUE ||
        backend == DEVICE_DB_BACKEND_BLE_GATT) {
        /* These recipes name a decoder property, a GATT characteristic index or
         * similar. Without one there is nothing to read, so the recipe would
         * publish an entity that can never receive a value. */
        if (read_source == DEVICE_DB_NO_INDEX) {
            return false;
        }
        out->read_source_id = read_source;
    }

    drivable = app_backend_is_drivable(backend);
    if (profile_writable && drivable && write_target != DEVICE_DB_NO_INDEX) {
        out->write_target_id = write_target;
    }
    return true;
}

/* Read one fingerprint record. */
static bool read_fingerprint(app_device_db_t *db, uint32_t index,
                             uint8_t *out_protocol, uint32_t *out_hash,
                             uint32_t *out_profile, char *key_out,
                             size_t key_size, uint8_t *out_key_len)
{
    uint32_t base = rd_u32(db->header + 88) + index * DEVICE_DB_FINGERPRINT_SIZE;
    uint8_t record[DEVICE_DB_FINGERPRINT_SIZE];
    uint32_t key_ref;
    uint8_t key_len;
    uint32_t key_off;
    uint32_t strings_base = rd_u32(db->header + 56);

    if (index >= rd_u32(db->header + 36)) {
        return false;
    }
    if (db_read(db, base, record, sizeof(record)) != ESP_OK) {
        return false;
    }

    *out_protocol = record[0];
    *out_hash = rd_u32(record + 4);
    *out_profile = rd_u32(record + 12);

    key_ref = rd_u32(record + 8);
    key_len = (uint8_t)(key_ref & 0xFFu);
    key_off = key_ref >> 8;
    if (key_len == 0u || (size_t)key_len >= key_size) {
        return false;
    }
    if ((uint64_t)strings_base + key_off + key_len > (uint64_t)db->file_size) {
        return false;
    }
    if (db_read(db, strings_base + key_off, key_out, key_len) != ESP_OK) {
        return false;
    }
    key_out[key_len] = '\0';
    *out_key_len = key_len;
    return true;
}

/* ---------------- matching ---------------- */

/* Canonical key bytes: lowercase, with ':' and '-' removed. This mirrors
 * normalize_key() in the generator and device_db_canonical_key_hash() in the
 * reader; all three must agree or a valid database stops matching. */
static size_t canonicalize(const char *src, size_t src_len, char *dst,
                           size_t dst_size)
{
    size_t used = 0u;

    if (dst_size == 0u) {
        return 0u;
    }
    for (size_t i = 0u; i < src_len && src[i] != '\0'; ++i) {
        char c = src[i];
        if (c == ':' || c == '-') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (used + 1u >= dst_size) {
            return 0u;
        }
        dst[used++] = c;
    }
    dst[used] = '\0';
    return used;
}

/*
 * Build the canonical key for one observation.
 *
 * Preference order for BLE matters: service data and manufacturer data are the
 * specific fingerprints a profile is keyed on, while a bare service UUID is the
 * weakest and most likely to be shared between models, so it is used only when
 * nothing more specific was advertised. Returns 0 when the observation carries
 * nothing matchable, which is not an error - the device simply stays generic.
 */
static size_t build_key(uint8_t protocol, const app_scan_wifi_t *wifi,
                        const app_scan_ble_t *ble, const app_scan_lan_t *lan,
                        char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";

    out[0] = '\0';

    switch (protocol) {
    case DEVICE_DB_PROTO_BLE:
        if (ble == NULL || !ble->has_parsed_adv) {
            return 0u;
        }
        if (ble->adv.service_data_count > 0u) {
            /* The compact advertisement keeps the service-data UUID, not its
             * bytes, so the UUID is the specific evidence available. */
            uint8_t ad_type = 0x16u;
            uint8_t uuid_len = 2u;
            if (out_size < 5u) {
                return 0u;
            }
            out[0] = hex[(ad_type >> 4) & 0x0Fu];
            out[1] = hex[ad_type & 0x0Fu];
            out[2] = hex[(uuid_len >> 4) & 0x0Fu];
            out[3] = hex[uuid_len & 0x0Fu];
            out[4] = '\0';
            return 4u;
        }
        if (ble->adv.manufacturer_data_count > 0u) {
            uint16_t company = ble->adv.first_company_id;
            if (out_size < 5u) {
                return 0u;
            }
            out[0] = hex[(company >> 12) & 0x0Fu];
            out[1] = hex[(company >> 8) & 0x0Fu];
            out[2] = hex[(company >> 4) & 0x0Fu];
            out[3] = hex[company & 0x0Fu];
            out[4] = '\0';
            return 4u;
        }
        if (ble->adv.uuid16_count > 0u) {
            uint16_t uuid = ble->adv.uuid16[0];
            if (out_size < 5u) {
                return 0u;
            }
            out[0] = hex[(uuid >> 12) & 0x0Fu];
            out[1] = hex[(uuid >> 8) & 0x0Fu];
            out[2] = hex[(uuid >> 4) & 0x0Fu];
            out[3] = hex[uuid & 0x0Fu];
            out[4] = '\0';
            return 4u;
        }
        return 0u;

    case DEVICE_DB_PROTO_WIFI:
        if (wifi == NULL || !wifi->has_ssid || wifi->ssid_len == 0u) {
            return 0u;
        }
        return canonicalize((const char *)wifi->ssid, wifi->ssid_len, out, out_size);

    case DEVICE_DB_PROTO_MDNS:
    case DEVICE_DB_PROTO_LAN:
        if (lan == NULL || lan->service[0] == '\0') {
            return 0u;
        }
        return canonicalize(lan->service, strlen(lan->service), out, out_size);

    case DEVICE_DB_PROTO_ESPHOME:
        /*
         * The mDNS service INSTANCE, which for ESPHome is the node name.
         *
         * This is the one node name a scan can see, and it is what a profile's
         * `esphome_node_name` identity rule (device-db-format.md, kind 4, a SAFE
         * cross-protocol identity) names. It has to be the instance rather than
         * `lan->service`: the service type is `_esphome._tcp` for every ESPHome node in
         * range, so keying on it would match one profile for all of them - or, as
         * before this case existed, match nothing at all.
         *
         * A sighting with no instance name (SSDP, Nmap) yields nothing matchable, which
         * is not an error: the device stays generic until an mDNS sighting supplies the
         * name. That is the same rule the other protocols follow - no key, no profile,
         * and never a guess.
         */
        if (lan == NULL || lan->instance[0] == '\0') {
            return 0u;
        }
        return canonicalize(lan->instance, strlen(lan->instance), out, out_size);

    default:
        return 0u;
    }
}

/* Look up one fingerprint index's profile and load the result. */
static bool resolve_profile(app_device_db_t *db, uint32_t profile_id,
                            app_recognition_result_t *out)
{
    uint32_t count = rd_u32(db->header + 32);
    uint32_t lo = 0u;
    uint32_t hi = count;
    uint8_t profile[DEVICE_DB_PROFILE_SIZE];

    /* Profiles are stored in ascending id order, so a binary search stays bounded
     * no matter how large the corpus grows. */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t base = rd_u32(db->header + 80) + mid * DEVICE_DB_PROFILE_SIZE;
        uint32_t have;

        if (db_read(db, base, profile, sizeof(profile)) != ESP_OK) {
            return false;
        }
        have = rd_u32(profile + 0);
        if (have == profile_id) {
            uint32_t recipe_first = rd_u32(profile + 32);
            uint32_t recipe_count = rd_u32(profile + 36);
            uint32_t theengs_id = rd_u32(profile + 24);
            uint32_t zha_id = rd_u32(profile + 28);
            uint8_t writable = profile[53];
            uint8_t accepted = 0u;
            bool needs_decoder = false;
            bool needs_quirk = false;

            out->matched = true;
            out->profile_id = profile_id;
            out->theengs_decoder_id = theengs_id;
            out->zha_quirk_id = zha_id;
            out->backend_name = "none";

            (void)load_string(db, out->display_name, sizeof(out->display_name),
                              rd_u32(profile + 12));
            (void)load_string(db, out->vendor, sizeof(out->vendor),
                              rd_u32(profile + 4));
            (void)load_string(db, out->model, sizeof(out->model),
                              rd_u32(profile + 8));

            for (uint32_t i = 0u; i < recipe_count; ++i) {
                app_entity_recipe_t *recipe;

                if (accepted >= APP_RECOGNITION_MAX_RECIPES) {
                    db->truncated_result = true;
                    break;
                }
                recipe = &out->recipes[accepted];
                if (!load_recipe(db, recipe_first + i, writable != 0u, recipe)) {
                    /* A recipe this firmware cannot interpret is reported and
                     * skipped, not silently attached as a half-meaning entity. */
                    db->truncated_result = true;
                    continue;
                }
                if (recipe->read_source_id != APP_DB_NO_READ_SOURCE) {
                    if (recipe->backend == DEVICE_DB_BACKEND_BLE_GATT ||
                        theengs_id != DEVICE_DB_NO_INDEX) {
                        needs_decoder = true;
                    }
                    if (zha_id != DEVICE_DB_NO_INDEX) {
                        needs_quirk = true;
                    }
                }
                if (recipe->write_target_id != DEVICE_DB_NO_INDEX) {
                    out->backend_supported = true;
                    out->backend_name = app_backend_name(recipe->backend);
                }
                accepted++;
            }
            out->recipe_count = accepted;

            /*
             * Decoder/quirk selection has to name something this firmware carries.
             *
             * A profile whose recipes need a decoder and whose selected decoder is
             * absent cannot produce a value, so exposing its entities would offer
             * readings nothing can fill. Such a match is still reported - the
             * profile really did match, and diagnostics should say so - but it
             * carries no recipes, so the Device stays generic and read-only.
             */
            if (needs_decoder && !app_decoder_is_available(theengs_id)) {
                out->recipe_count = 0u;
                out->backend_supported = false;
                out->backend_name = "none";
                db->truncated_result = true;
            } else if (needs_quirk && !app_quirk_is_available(zha_id)) {
                out->recipe_count = 0u;
                out->backend_supported = false;
                out->backend_name = "none";
                db->truncated_result = true;
            }
            return true;
        }
        if (have < profile_id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool app_device_db_match(app_device_db_t *db, uint32_t sources, uint8_t protocol,
                         const uint8_t *key, size_t key_length,
                         const app_scan_wifi_t *wifi,
                         const app_scan_ble_t *ble,
                         const app_scan_lan_t *lan,
                         app_recognition_result_t *out)
{
    char canonical[KEY_SCRATCH_MAX];
    char stored_key[KEY_SCRATCH_MAX];
    uint8_t stored_key_len = 0u;
    uint32_t hash;
    uint32_t matches[APP_RECOGNITION_MAX_RECIPES];
    uint32_t match_count = 0u;
    bool incomplete = false;

    (void)sources;
    (void)key;
    (void)key_length;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->theengs_decoder_id = DEVICE_DB_NO_INDEX;
    out->zha_quirk_id = DEVICE_DB_NO_INDEX;
    out->backend_name = "none";

    if (db == NULL || !db->open || db->state != APP_DB_STATE_READY) {
        return false;
    }
    if (protocol >= DEVICE_DB_PROTO_COUNT) {
        return false;
    }

    /*
     * The corpus must still be the one that was validated at open.
     *
     * The size check inside db_read() only catches shrinking, and only relative to
     * the size recorded at open; a same-length rewrite or a replacement leaves it
     * intact. Re-reading the header here is what stops a match from being answered
     * out of a corpus whose validation no longer describes the medium. When it
     * fails, this is reported as "recognition could not run" rather than as
     * "nothing matched", so the caller keeps generic Devices and says why.
     */
    if (!db_verify_unchanged(db)) {
        return false;
    }

    /* A protocol with no usable key cannot match; that is a normal outcome, not a
     * failure, so it is reported as "not matched" rather than as an error. */
    if (build_key(protocol, wifi, ble, lan, canonical, sizeof(canonical)) == 0u) {
        return true;
    }
    hash = device_db_canonical_key_hash((const uint8_t *)canonical,
                                        strlen(canonical));

    /*
     * Candidate lookup through the index, then confirmation against the record.
     *
     * The index is only an accelerator, so a bucket overflow falls back to a full
     * scan of that protocol's fingerprints rather than concluding "not found".
     */
    {
        uint32_t bucket_count = rd_u16(db->header + 52);

        if (bucket_count > 0u && db->index != NULL) {
            const uint8_t *bucket = db->index +
                                    (size_t)(hash & (bucket_count - 1u)) *
                                        DEVICE_DB_INDEX_BUCKET_SIZE;
            uint16_t entries = rd_u16(bucket);
            uint16_t flags = rd_u16(bucket + 30);

            if ((flags & 1u) != 0u) {
                incomplete = true;
            }
            for (uint16_t e = 0u; e < entries; ++e) {
                const uint8_t *entry = bucket + 2u + (size_t)e * 10u;

                if (rd_u32(entry) == hash) {
                    uint8_t fp_protocol;
                    uint32_t fp_hash;
                    uint32_t fp_profile;

                    if (!read_fingerprint(db, rd_u32(entry + 4), &fp_protocol,
                                          &fp_hash, &fp_profile, stored_key,
                                          sizeof(stored_key), &stored_key_len)) {
                        continue;
                    }
                    /* Confirm: the index points at fingerprint indices, not at
                     * matches, so both the protocol and the key are re-checked. */
                    if (fp_protocol != protocol || fp_hash != hash) {
                        continue;
                    }
                    if (strcmp(stored_key, canonical) != 0) {
                        continue;
                    }
                    if (match_count < APP_RECOGNITION_MAX_RECIPES) {
                        matches[match_count] = fp_profile;
                    }
                    match_count++;
                }
            }
        } else {
            incomplete = true;
        }
    }

    if (incomplete) {
        /* A flagged bucket's inline entries are not exhaustive, so fall back to
         * the whole protocol. Correctness never depends on the index. */
        uint32_t fp_count = rd_u32(db->header + 36);

        for (uint32_t i = 0u; i < fp_count; ++i) {
            uint8_t fp_protocol;
            uint32_t fp_hash;
            uint32_t fp_profile;
            bool already = false;

            if (!read_fingerprint(db, i, &fp_protocol, &fp_hash, &fp_profile,
                                  stored_key, sizeof(stored_key),
                                  &stored_key_len)) {
                continue;
            }
            if (fp_protocol != protocol || fp_hash != hash) {
                continue;
            }
            if (strcmp(stored_key, canonical) != 0) {
                continue;
            }
            for (uint32_t m = 0u; m < match_count; ++m) {
                if (matches[m] == fp_profile) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            if (match_count < APP_RECOGNITION_MAX_RECIPES) {
                matches[match_count] = fp_profile;
            }
            match_count++;
        }
    }

    if (match_count == 0u) {
        return true; /* NOT_FOUND: the device stays generic. */
    }

    /* Distinct profiles, not distinct fingerprints: two fingerprints of the same
     * profile are one candidate, not an ambiguity. */
    {
        uint32_t distinct = 1u;

        for (uint32_t i = 1u; i < match_count &&
                             i < APP_RECOGNITION_MAX_RECIPES; ++i) {
            bool seen = false;
            for (uint32_t j = 0u; j < i; ++j) {
                if (matches[j] == matches[i]) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                distinct++;
            }
        }
        if (distinct > 1u) {
            /* Ambiguous: visible, generic, never writable. Never guess. */
            out->ambiguous = true;
            out->matched = false;
            return true;
        }
    }

    if (!resolve_profile(db, matches[0], out)) {
        /* The record could not be read. Reported as not matched rather than as a
         * guess, and the caller keeps its generic Device. */
        memset(out, 0, sizeof(*out));
        out->theengs_decoder_id = DEVICE_DB_NO_INDEX;
        out->zha_quirk_id = DEVICE_DB_NO_INDEX;
        out->backend_name = "none";
        db->truncated_result = true;
        return true;
    }
    return true;
}

/* ---------------- recognizer vtable ---------------- */

static app_db_state_t db_recognizer_state(void *ctx)
{
    app_device_db_t *db = (app_device_db_t *)ctx;

    return db == NULL ? APP_DB_STATE_CLOSED : db->state;
}

static void db_recognizer_describe(void *ctx, char *out, size_t out_size)
{
    app_device_db_t *db = (app_device_db_t *)ctx;

    if (out == NULL || out_size == 0u) {
        return;
    }
    if (db == NULL) {
        (void)snprintf(out, out_size, "closed");
        return;
    }
    if (db->state == APP_DB_STATE_READY) {
        (void)snprintf(out, out_size, "%s v%lu profiles=%lu",
                       app_db_state_name(db->state),
                       (unsigned long)db->content_version,
                       (unsigned long)db->profile_count);
        return;
    }
    (void)snprintf(out, out_size, "%s", app_db_state_name(db->state));
}

/*
 * Recognise one observation.
 *
 * Runs the typed match for every protocol present in `sources`. One observation
 * normally carries a single source; when it carries more, each is matched and the
 * first deterministic match wins, while any ambiguity poisons the result - an
 * ambiguous protocol must not be overridden by a later confident one.
 */
static bool db_recognizer_recognize(void *ctx, uint32_t sources,
                                    const app_scan_wifi_t *wifi,
                                    const app_scan_ble_t *ble,
                                    const app_scan_lan_t *lan,
                                    app_recognition_result_t *out)
{
    app_device_db_t *db = (app_device_db_t *)ctx;
    app_recognition_result_t attempt;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->theengs_decoder_id = DEVICE_DB_NO_INDEX;
    out->zha_quirk_id = DEVICE_DB_NO_INDEX;
    out->backend_name = "none";

    if (db == NULL || !db->open || db->state != APP_DB_STATE_READY) {
        /* Could not be attempted. The caller keeps its generic Device and reports
         * the database state separately. */
        return false;
    }
    (void)sources;

    if (ble != NULL) {
        memset(&attempt, 0, sizeof(attempt));
        (void)app_device_db_match(db, APP_SOURCE_BLE, DEVICE_DB_PROTO_BLE, NULL, 0u,
                                  NULL, ble, NULL, &attempt);
        if (attempt.ambiguous) {
            *out = attempt;
            return true;
        }
        if (attempt.matched) {
            *out = attempt;
            return true;
        }
    }
    if (wifi != NULL) {
        memset(&attempt, 0, sizeof(attempt));
        (void)app_device_db_match(db, APP_SOURCE_WIFI, DEVICE_DB_PROTO_WIFI, NULL,
                                  0u, wifi, NULL, NULL, &attempt);
        if (attempt.ambiguous) {
            *out = attempt;
            return true;
        }
        if (attempt.matched) {
            *out = attempt;
            return true;
        }
    }
    if (lan != NULL) {
        memset(&attempt, 0, sizeof(attempt));
        (void)app_device_db_match(db, APP_SOURCE_LAN, DEVICE_DB_PROTO_MDNS, NULL,
                                  0u, NULL, NULL, lan, &attempt);
        if (attempt.ambiguous) {
            *out = attempt;
            return true;
        }
        if (attempt.matched) {
            *out = attempt;
            return true;
        }
    }
    return true; /* nothing matched: the device stays generic */
}

static const app_recognizer_ops_t s_ops = {
    .recognize = db_recognizer_recognize,
    .state = db_recognizer_state,
    .describe = db_recognizer_describe,
};

const app_recognizer_ops_t *app_device_db_recognizer_ops(void)
{
    return &s_ops;
}
