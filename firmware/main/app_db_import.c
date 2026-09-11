/*
 * Device DB import state machine. See app_db_import.h for the contract, the
 * replacement sequence and why it is ordered that way.
 *
 * Platform independent: every filesystem operation arrives through
 * app_db_import_io_t. That is what lets the host tests drive a real power loss
 * between any two steps and assert what the card holds afterwards.
 */

#include "app_db_import.h"

#include "app_str.h"

#include <stdio.h>
#include <string.h>

#include "device_db_format.h"

/* The format's own header is the authority on what a corpus may look like, so the
 * import cannot accept something the reader would later refuse. */
#define IMPORT_HEADER_SIZE DEVICE_DB_HEADER_SIZE

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

const char *app_db_import_phase_name(app_db_import_phase_t phase)
{
    switch (phase) {
    case APP_DB_IMPORT_IDLE:       return "idle";
    case APP_DB_IMPORT_RECEIVING:  return "receiving";
    case APP_DB_IMPORT_VALIDATING: return "validating";
    case APP_DB_IMPORT_REPLACING:  return "replacing";
    case APP_DB_IMPORT_DONE:       return "done";
    case APP_DB_IMPORT_FAILED:     return "failed";
    case APP_DB_IMPORT_CANCELED:   return "canceled";
    default:                       return "invalid";
    }
}

const char *app_db_import_error_name(app_db_import_error_t error)
{
    switch (error) {
    case APP_DB_IMPORT_ERR_NONE:            return "none";
    case APP_DB_IMPORT_ERR_NO_MEDIUM:       return "no_medium";
    case APP_DB_IMPORT_ERR_NO_SPACE:        return "no_space";
    case APP_DB_IMPORT_ERR_TOO_LARGE:       return "too_large";
    case APP_DB_IMPORT_ERR_IO:              return "io_error";
    case APP_DB_IMPORT_ERR_SHORT_WRITE:     return "short_write";
    case APP_DB_IMPORT_ERR_LENGTH_MISMATCH: return "length_mismatch";
    case APP_DB_IMPORT_ERR_INVALID_DB:      return "invalid_database";
    case APP_DB_IMPORT_ERR_BUSY:            return "busy";
    case APP_DB_IMPORT_ERR_NOT_ACTIVE:      return "not_active";
    case APP_DB_IMPORT_ERR_CANCELED:        return "canceled";
    default:                                return "unknown";
    }
}

const char *app_db_recovery_action_name(app_db_recovery_action_t action)
{
    switch (action) {
    case APP_DB_RECOVERY_NONE:          return "none";
    case APP_DB_RECOVERY_RESTORED_OLD:  return "restored_old";
    case APP_DB_RECOVERY_PROMOTED_PART: return "promoted_part";
    case APP_DB_RECOVERY_DISCARDED:     return "discarded";
    case APP_DB_RECOVERY_NO_MEDIUM:     return "no_medium";
    default:                            return "invalid";
    }
}

/* Derive "<path>.part" and "<path>.bak". Returns false when either would not fit
 * rather than truncating, because a truncated sibling path would collide with a
 * different file. */
static bool derive_sibling(const char *path, const char *suffix, char *out,
                           size_t out_size)
{
    int written = snprintf(out, out_size, "%s%s", path, suffix);

    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t app_db_import_init(app_db_import_t *imp, const app_db_import_io_t *io,
                             void *io_ctx, const char *db_path,
                             uint32_t max_file_bytes)
{
    if (imp == NULL || io == NULL || db_path == NULL || db_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (io->medium_present == NULL || io->write_open == NULL || io->write == NULL ||
        io->write_close == NULL || io->read_all == NULL || io->exists == NULL ||
        io->rename == NULL || io->unlink == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(imp, 0, sizeof(*imp));
    imp->io = *io;
    imp->io_ctx = io_ctx;
    imp->phase = APP_DB_IMPORT_IDLE;
    imp->error = APP_DB_IMPORT_ERR_NONE;
    imp->max_file_bytes = max_file_bytes == 0u ? DEVICE_DB_MAX_FILE_BYTES
                                               : max_file_bytes;

    if (app_strlcpy(imp->db_path, db_path, sizeof(imp->db_path)) >=
        sizeof(imp->db_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!derive_sibling(imp->db_path, ".part", imp->part_path,
                        sizeof(imp->part_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!derive_sibling(imp->db_path, ".bak", imp->backup_path,
                        sizeof(imp->backup_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ---------------- validation ---------------- */

/*
 * Read a path and decide whether it is a corpus this firmware can open.
 *
 * Only the header is read and interpreted here. The body checksum is the reader's
 * job when it opens the promoted file: doing it twice would double the import time
 * on a card shared with the display, and a corpus that passes the header gates and
 * then fails its body checksum is reported by the reader as CORRUPT rather than
 * silently accepted.
 *
 * The header gates are the reader's own gates, in the reader's own order, so the
 * import cannot accept something the reader will refuse:
 *   magic, format version, schema version, reader ABI, reserved flags, the
 *   declared file length, and the header checksum.
 */
bool app_db_import_probe(app_db_import_t *imp, const char *path,
                         uint32_t *out_version, uint32_t *out_profiles)
{
    uint8_t header[IMPORT_HEADER_SIZE];
    uint32_t size = 0u;

    if (out_version != NULL) {
        *out_version = 0u;
    }
    if (out_profiles != NULL) {
        *out_profiles = 0u;
    }
    if (imp == NULL || path == NULL) {
        return false;
    }
    if (imp->io.read_all(imp->io_ctx, path, header, (uint32_t)sizeof(header),
                         &size) != ESP_OK) {
        return false;
    }
    if (size < IMPORT_HEADER_SIZE) {
        return false;
    }
    if (memcmp(header, "NBDB", 4u) != 0) {
        return false;
    }
    if (rd_u16(header + 4) != DEVICE_DB_FORMAT_VERSION ||
        rd_u16(header + 6) != DEVICE_DB_SCHEMA_VERSION ||
        rd_u32(header + 8) > DEVICE_DB_READER_ABI ||
        rd_u32(header + 12) != 0u) {
        return false;
    }
    if (rd_u32(header + 16) != size) {
        return false;
    }
    if (device_db_crc32(header, 120u) != rd_u32(header + 120)) {
        return false;
    }
    if (out_version != NULL) {
        *out_version = rd_u32(header + 20);
    }
    if (out_profiles != NULL) {
        *out_profiles = rd_u32(header + 32);
    }
    return true;
}

/*
 * Stream the body checksum of a file.
 *
 * `expected` is the header's own body_crc32, which covers everything from
 * DEVICE_DB_HEADER_SIZE to the end of the file - the same region the whole-file
 * validator in device_db_format.c covers, so the two agree by construction.
 *
 * The file is walked in fixed blocks, so a corpus far larger than any buffer this
 * component holds is still verified without being resident.
 */
static bool verify_body_crc(app_db_import_t *imp, const char *path,
                            uint32_t file_size, uint32_t expected)
{
    uint8_t block[APP_DB_IMPORT_CHUNK];
    uint32_t offset = IMPORT_HEADER_SIZE;
    uint32_t crc = 0xFFFFFFFFu;

    while (offset < file_size) {
        uint32_t chunk = file_size - offset;

        if (chunk > (uint32_t)sizeof(block)) {
            chunk = (uint32_t)sizeof(block);
        }
        if (imp->io.read_range(imp->io_ctx, path, offset, block, chunk) != ESP_OK) {
            return false;
        }
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
    }

    crc ^= 0xFFFFFFFFu;
    return crc == expected;
}

bool app_db_import_verify(app_db_import_t *imp, const char *path,
                          uint32_t *out_version, uint32_t *out_profiles)
{
    uint8_t header[IMPORT_HEADER_SIZE];
    uint32_t size = 0u;

    if (out_version != NULL) {
        *out_version = 0u;
    }
    if (out_profiles != NULL) {
        *out_profiles = 0u;
    }
    if (imp == NULL || path == NULL || imp->io.read_range == NULL) {
        return false;
    }
    if (!app_db_import_probe(imp, path, out_version, out_profiles)) {
        return false;
    }
    /* The header gates passed, so re-reading it cannot fail for a new reason: the
     * file was readable a moment ago. The declared length is the header's own. */
    if (imp->io.read_all(imp->io_ctx, path, header, (uint32_t)sizeof(header),
                         &size) != ESP_OK) {
        return false;
    }
    if (!verify_body_crc(imp, path, rd_u32(header + 16), rd_u32(header + 116))) {
        /* The payload does not match the checksum its own header declares. This is
         * the case a header-only probe cannot see, and the one that must never be
         * promoted: the reader would reject it as CORRUPT afterwards, leaving the
         * device without a corpus it could have kept. */
        if (out_version != NULL) {
            *out_version = 0u;
        }
        if (out_profiles != NULL) {
            *out_profiles = 0u;
        }
        return false;
    }
    return true;
}

/* ---------------- begin / write / finish ---------------- */

app_db_import_error_t app_db_import_begin(app_db_import_t *imp,
                                          uint64_t declared_length)
{
    if (imp == NULL) {
        return APP_DB_IMPORT_ERR_IO;
    }
    if (imp->phase == APP_DB_IMPORT_RECEIVING ||
        imp->phase == APP_DB_IMPORT_VALIDATING ||
        imp->phase == APP_DB_IMPORT_REPLACING) {
        /* One import at a time. A second upload would otherwise write into the
         * same .part file as the first. */
        imp->error = APP_DB_IMPORT_ERR_BUSY;
        return imp->error;
    }

    imp->error = APP_DB_IMPORT_ERR_NONE;
    imp->last_io_error = ESP_OK;
    imp->declared_length = declared_length;
    imp->received = 0u;
    imp->wrote_any = false;
    imp->backup_taken = false;
    imp->replace_attempts = 0u;

    if (!imp->io.medium_present(imp->io_ctx)) {
        imp->phase = APP_DB_IMPORT_FAILED;
        imp->error = APP_DB_IMPORT_ERR_NO_MEDIUM;
        return imp->error;
    }

    if (declared_length > (uint64_t)imp->max_file_bytes) {
        imp->phase = APP_DB_IMPORT_FAILED;
        imp->error = APP_DB_IMPORT_ERR_TOO_LARGE;
        return imp->error;
    }
    if (declared_length != 0u && imp->io.free_bytes != NULL) {
        uint64_t free_bytes = 0u;

        if (imp->io.free_bytes(imp->io_ctx, &free_bytes) &&
            declared_length > free_bytes) {
            /* Refused before a single byte is written, so a full card cannot be
             * filled by an upload that was never going to fit. */
            imp->phase = APP_DB_IMPORT_FAILED;
            imp->error = APP_DB_IMPORT_ERR_NO_SPACE;
            return imp->error;
        }
    }

    /*
     * A .part file from an earlier attempt is garbage, not resumable state.
     * Removing it first means "begin" always starts from a known state, and a
     * partial file can never be validated by a later finish().
     */
    if (imp->io.exists(imp->io_ctx, imp->part_path)) {
        (void)imp->io.unlink(imp->io_ctx, imp->part_path);
    }

    {
        esp_err_t err = imp->io.write_open(imp->io_ctx, imp->part_path);

        if (err != ESP_OK) {
            imp->phase = APP_DB_IMPORT_FAILED;
            imp->error = APP_DB_IMPORT_ERR_IO;
            imp->last_io_error = err;
            return imp->error;
        }
    }

    imp->phase = APP_DB_IMPORT_RECEIVING;
    return APP_DB_IMPORT_ERR_NONE;
}

app_db_import_error_t app_db_import_write(app_db_import_t *imp, const void *data,
                                          size_t length)
{
    esp_err_t err;

    if (imp == NULL || (data == NULL && length > 0u)) {
        return APP_DB_IMPORT_ERR_IO;
    }
    if (imp->phase != APP_DB_IMPORT_RECEIVING) {
        imp->error = APP_DB_IMPORT_ERR_NOT_ACTIVE;
        return imp->error;
    }
    if (length == 0u) {
        return APP_DB_IMPORT_ERR_NONE;
    }

    /* Both bounds are checked before writing, so an over-long body cannot leave a
     * partially written oversized file behind. */
    if (imp->received + (uint64_t)length > (uint64_t)imp->max_file_bytes) {
        imp->error = APP_DB_IMPORT_ERR_TOO_LARGE;
        imp->phase = APP_DB_IMPORT_FAILED;
        return imp->error;
    }
    if (imp->declared_length != 0u &&
        imp->received + (uint64_t)length > imp->declared_length) {
        imp->error = APP_DB_IMPORT_ERR_LENGTH_MISMATCH;
        imp->phase = APP_DB_IMPORT_FAILED;
        return imp->error;
    }

    err = imp->io.write(imp->io_ctx, data, length);
    if (err != ESP_OK) {
        imp->error = APP_DB_IMPORT_ERR_SHORT_WRITE;
        imp->phase = APP_DB_IMPORT_FAILED;
        imp->last_io_error = err;
        return imp->error;
    }

    imp->received += (uint64_t)length;
    imp->wrote_any = true;
    return APP_DB_IMPORT_ERR_NONE;
}

/* Remove the .part file. Safe when it does not exist. */
static void discard_part(app_db_import_t *imp)
{
    if (imp->io.exists(imp->io_ctx, imp->part_path)) {
        (void)imp->io.unlink(imp->io_ctx, imp->part_path);
    }
}

app_db_import_error_t app_db_import_finish(app_db_import_t *imp,
                                           uint32_t *out_version)
{
    if (out_version != NULL) {
        *out_version = 0u;
    }
    if (imp == NULL || imp->phase != APP_DB_IMPORT_RECEIVING) {
        if (imp != NULL) {
            imp->error = APP_DB_IMPORT_ERR_NOT_ACTIVE;
        }
        return APP_DB_IMPORT_ERR_NOT_ACTIVE;
    }

    /* The declared length must be met exactly. A truncated upload that happens to
     * end on a valid boundary is still a truncated upload. */
    if (imp->declared_length != 0u && imp->received != imp->declared_length) {
        imp->error = APP_DB_IMPORT_ERR_LENGTH_MISMATCH;
        imp->phase = APP_DB_IMPORT_FAILED;
        discard_part(imp);
        imp->io.write_close(imp->io_ctx);
        return imp->error;
    }
    if (imp->received < (uint64_t)IMPORT_HEADER_SIZE) {
        /* Nothing that small can be a corpus. */
        imp->error = APP_DB_IMPORT_ERR_INVALID_DB;
        imp->phase = APP_DB_IMPORT_FAILED;
        discard_part(imp);
        imp->io.write_close(imp->io_ctx);
        return imp->error;
    }

    imp->phase = APP_DB_IMPORT_VALIDATING;

    /* Flush before closing: the validation read and the later rename must see the
     * bytes, not the cache. */
    if (imp->io.write_sync != NULL) {
        esp_err_t err = imp->io.write_sync(imp->io_ctx);

        if (err != ESP_OK) {
            imp->error = APP_DB_IMPORT_ERR_IO;
            imp->phase = APP_DB_IMPORT_FAILED;
            imp->last_io_error = err;
            imp->io.write_close(imp->io_ctx);
            discard_part(imp);
            return imp->error;
        }
    }
    imp->io.write_close(imp->io_ctx);

    if (!app_db_import_verify(imp, imp->part_path, &imp->content_version,
                              &imp->profile_count)) {
        /* Full validation, not just the header: a file whose payload does not
         * match the checksum its own header declares is refused here, while the
         * live corpus is still untouched. Promoting it would leave the reader to
         * reject it afterwards, and the device with no corpus at all. */
        imp->error = APP_DB_IMPORT_ERR_INVALID_DB;
        imp->phase = APP_DB_IMPORT_FAILED;
        discard_part(imp);
        return imp->error;
    }

    /*
     * Replace. The sequence and its recovery are documented in app_db_import.h;
     * the short version is that the old database is always recoverable and the
     * only moment it is absent from the path is between the two renames.
     */
    imp->phase = APP_DB_IMPORT_REPLACING;

    if (imp->io.exists(imp->io_ctx, imp->db_path)) {
        /* A leftover .bak from an earlier interrupted import would make the
         * rename below fail on some filesystems, so it is cleared first. */
        if (imp->io.exists(imp->io_ctx, imp->backup_path)) {
            (void)imp->io.unlink(imp->io_ctx, imp->backup_path);
        }
        imp->replace_attempts++;
        if (imp->io.rename(imp->io_ctx, imp->db_path, imp->backup_path) != ESP_OK) {
            /* The old database is still at the path, untouched. */
            imp->error = APP_DB_IMPORT_ERR_IO;
            imp->phase = APP_DB_IMPORT_FAILED;
            discard_part(imp);
            return imp->error;
        }
        imp->backup_taken = true;
    }

    if (imp->io.rename(imp->io_ctx, imp->part_path, imp->db_path) != ESP_OK) {
        /*
         * The new corpus could not take the path. Put the old one back before
         * reporting, so a failed import never leaves the device without a corpus.
         */
        if (imp->backup_taken) {
            (void)imp->io.rename(imp->io_ctx, imp->backup_path, imp->db_path);
            imp->backup_taken = false;
        }
        imp->error = APP_DB_IMPORT_ERR_IO;
        imp->phase = APP_DB_IMPORT_FAILED;
        discard_part(imp);
        return imp->error;
    }

    /*
     * Promote succeeded. The previous copy is retired only now, and only after the
     * path is confirmed to hold something usable: if it does not, the backup is
     * what recovery will restore from, so it must survive.
     */
    if (imp->backup_taken) {
        uint32_t ignored_version = 0u;

        if (app_db_import_verify(imp, imp->db_path, &ignored_version, NULL)) {
            (void)imp->io.unlink(imp->io_ctx, imp->backup_path);
            imp->backup_taken = false;
        }
    }

    imp->phase = APP_DB_IMPORT_DONE;
    imp->error = APP_DB_IMPORT_ERR_NONE;
    if (out_version != NULL) {
        *out_version = imp->content_version;
    }
    return APP_DB_IMPORT_ERR_NONE;
}

void app_db_import_cancel(app_db_import_t *imp)
{
    if (imp == NULL) {
        return;
    }
    if (imp->phase == APP_DB_IMPORT_RECEIVING) {
        imp->io.write_close(imp->io_ctx);
    }
    discard_part(imp);
    imp->phase = APP_DB_IMPORT_CANCELED;
    imp->error = APP_DB_IMPORT_ERR_CANCELED;
    imp->received = 0u;
}

/* ---------------- recovery ---------------- */

app_db_recovery_action_t app_db_import_recover(app_db_import_t *imp,
                                               app_db_state_t *out_state)
{
    bool path_valid;
    bool backup_valid;
    bool part_valid;

    if (out_state != NULL) {
        *out_state = APP_DB_STATE_CLOSED;
    }
    if (imp == NULL) {
        return APP_DB_RECOVERY_NO_MEDIUM;
    }
    if (!imp->io.medium_present(imp->io_ctx)) {
        /* No card: nothing was examined and nothing is claimed about the corpus. */
        if (out_state != NULL) {
            *out_state = APP_DB_STATE_SD_MISSING;
        }
        return APP_DB_RECOVERY_NO_MEDIUM;
    }

    path_valid = app_db_import_probe(imp, imp->db_path, &imp->content_version,
                                     &imp->profile_count);

    if (path_valid) {
        /*
         * The path holds a usable corpus, so any sibling is debris from an
         * interruption that happened after the path became valid. Removing them is
         * safe and is what stops a stale .bak from accumulating across imports.
         */
        bool discarded = false;

        if (imp->io.exists(imp->io_ctx, imp->part_path)) {
            (void)imp->io.unlink(imp->io_ctx, imp->part_path);
            discarded = true;
        }
        if (imp->io.exists(imp->io_ctx, imp->backup_path)) {
            (void)imp->io.unlink(imp->io_ctx, imp->backup_path);
            discarded = true;
        }
        if (out_state != NULL) {
            *out_state = APP_DB_STATE_READY;
        }
        /* `discarded` means only that debris was cleaned; the corpus was already
         * fine, so the reported action is NONE. Saying otherwise would make an
         * ordinary start-up look like a recovery. */
        (void)discarded;
        return APP_DB_RECOVERY_NONE;
    }

    /*
     * The path is not usable. Before touching anything, ask whether a valid corpus
     * exists elsewhere: that is the difference between "recover" and "there is no
     * corpus", and reporting the first as the second would throw away a good
     * database.
     */
    backup_valid = app_db_import_probe(imp, imp->backup_path, &imp->content_version,
                                       &imp->profile_count);
    if (backup_valid) {
        /* Died between the two renames: the old corpus is in .bak. */
        if (imp->io.rename(imp->io_ctx, imp->backup_path, imp->db_path) == ESP_OK) {
            if (imp->io.exists(imp->io_ctx, imp->part_path)) {
                (void)imp->io.unlink(imp->io_ctx, imp->part_path);
            }
            if (out_state != NULL) {
                *out_state = APP_DB_STATE_READY;
            }
            return APP_DB_RECOVERY_RESTORED_OLD;
        }
        /* Restoring failed; leave both files alone so a later attempt can retry
         * rather than destroying the only good copy. */
        if (out_state != NULL) {
            *out_state = APP_DB_STATE_IO_ERROR;
        }
        return APP_DB_RECOVERY_DISCARDED;
    }

    part_valid = app_db_import_verify(imp, imp->part_path, &imp->content_version,
                                      &imp->profile_count);
    if (part_valid) {
        /* Died after validating the upload but before the first rename. Promoting
         * it is correct: the file passed the same gates the reader applies. */
        if (imp->io.rename(imp->io_ctx, imp->part_path, imp->db_path) == ESP_OK) {
            if (imp->io.exists(imp->io_ctx, imp->backup_path)) {
                (void)imp->io.unlink(imp->io_ctx, imp->backup_path);
            }
            if (out_state != NULL) {
                *out_state = APP_DB_STATE_READY;
            }
            return APP_DB_RECOVERY_PROMOTED_PART;
        }
    }

    /* Nothing usable anywhere. Clear the debris so the next import starts clean,
     * and report "no corpus" - which is a normal deployment state, not corruption. */
    if (imp->io.exists(imp->io_ctx, imp->part_path)) {
        (void)imp->io.unlink(imp->io_ctx, imp->part_path);
    }
    if (imp->io.exists(imp->io_ctx, imp->backup_path)) {
        (void)imp->io.unlink(imp->io_ctx, imp->backup_path);
    }
    if (out_state != NULL) {
        *out_state = APP_DB_STATE_FILE_MISSING;
    }
    return APP_DB_RECOVERY_DISCARDED;
}
